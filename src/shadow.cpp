#include "shadow.h"
#include "clip.h"
#include "light.h"
#include "linalg.h"
#include "mesh.h"
#include "texture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

// ─── internal helpers ─────────────────────────────────────────────────────────

namespace
{

    // Orthographic projection: maps axis-aligned box to NDC cube [-1,1]^3.
    // l/r/b/t are left/right/bottom/top extents in light view space.
    // n/f are near/far distances (positive).
    mat4 ortho(float l, float r, float b, float t, float n, float f)
    {
        mat4 m = mat4::identity();
        m.m[0][0] = 2.0f / (r - l);
        m.m[1][1] = 2.0f / (t - b);
        m.m[2][2] = -2.0f / (f - n);
        m.m[3][0] = -(r + l) / (r - l);
        m.m[3][1] = -(t + b) / (t - b);
        m.m[3][2] = -(f + n) / (f - n);
        return m;
    }

} // namespace

// ─── ShadowMap ────────────────────────────────────────────────────────────────

float ShadowMap::in_shadow(vec3 world_pos) const
{
    const vec4 light_clip = light_vp * vec4(world_pos, 1.0f);
    if (light_clip.w <= 0.0f)
        return 0.0f;
    const vec3 ndc = light_clip.perspective_divide();
    if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f ||
        ndc.z < -1.0f || ndc.z > 1.0f)
        return 0.0f;
    const float u = (ndc.x + 1.0f) * 0.5f;
    const float v = (ndc.y + 1.0f) * 0.5f;
    const int cx = std::clamp(static_cast<int>(u * static_cast<float>(SIZE)), 0, SIZE - 1);
    const int cy = std::clamp(static_cast<int>(v * static_cast<float>(SIZE)), 0, SIZE - 1);
    constexpr float fp_eps = 0.001f;
    const float ref = ndc.z - fp_eps;

    int hits = 0;
    if (cx > 0 && cx < SIZE - 1 && cy > 0 && cy < SIZE - 1)
    {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (ref > depth[static_cast<size_t>(cy + dy) * SIZE + static_cast<size_t>(cx + dx)])
                    hits++;
    }
    else
    {
        for (int dy = -1; dy <= 1; dy++)
        {
            const int py = std::clamp(cy + dy, 0, SIZE - 1);
            for (int dx = -1; dx <= 1; dx++)
            {
                const int px = std::clamp(cx + dx, 0, SIZE - 1);
                if (ref > depth[static_cast<size_t>(py) * SIZE + static_cast<size_t>(px)])
                    hits++;
            }
        }
    }
    return static_cast<float>(hits) * (1.0f / 9.0f);
}

ShadowMap build_shadow_map(const Mesh &mesh, const Light &light)
{
    ShadowMap shadow_map;
    shadow_map.clear();

    if (mesh.vertices.empty())
        return shadow_map;

    // Bounding sphere: centroid + max radius.
    vec3 center{};
    for (const auto &v : mesh.vertices)
        center += v.pos;
    center = center * (1.0f / static_cast<float>(mesh.vertices.size()));
    float radius = 0.0f;
    for (const auto &v : mesh.vertices)
        radius = std::max(radius, (v.pos - center).length());
    if (radius < 0.001f)
        radius = 1.0f;

    // Place a virtual camera at the light source, looking toward scene centre.
    // light.direction is "toward the light", so eye is in that direction.
    const vec3 dir = normalize(light.direction);
    const vec3 eye_pos = center + dir * (radius * 3.0f);
    const vec3 world_up = (std::abs(dir.y) < 0.9f) ? vec3{0.0f, 1.0f, 0.0f} : vec3{1.0f, 0.0f, 0.0f};
    const mat4 light_view = look_at(eye_pos, center, world_up);

    const float ext = radius * 1.2f;
    const mat4 light_proj = ortho(-ext, ext, -ext, ext, radius * 0.5f, radius * 6.0f);
    shadow_map.light_vp = light_proj * light_view;

    const int S = ShadowMap::SIZE;

    // Depth-only rasterization into the shadow map.
    for (const Triangle &tri : mesh.triangles)
    {
        const vec3 &pa = mesh.vertices[tri.v[0]].pos;
        const vec3 &pb = mesh.vertices[tri.v[1]].pos;
        const vec3 &pc = mesh.vertices[tri.v[2]].pos;

        const Material &mat = mesh.mat_at(tri.material_idx);
        const Texture *atex = (mat.alpha_cutoff > 0.0f && mat.diffuse_tex >= 0)
                                  ? mesh.tex_at(mat.diffuse_tex)
                                  : nullptr;
        vec2 uva{}, uvb{}, uvc{};
        if (atex)
        {
            uva = mesh.vertices[tri.v[0]].uv;
            uvb = mesh.vertices[tri.v[1]].uv;
            uvc = mesh.vertices[tri.v[2]].uv;
        }

        // Slope-scale bias: surfaces nearly tangent to the light direction need a
        // larger depth offset to avoid self-shadowing acne.  We bake it into the
        // stored depth so in_shadow() needs only a tiny epsilon.
        // bias = base / max(n·l, min_clamp)  →  large bias for glancing angles.
        const vec3 face_n = normalize(cross(pb - pa, pc - pa));
        const float n_dot_l = dot(face_n, dir);
        const float slope_bias = (n_dot_l > 0.01f) ? 0.007f / n_dot_l : 0.5f;

        const vec4 ca = shadow_map.light_vp * vec4(pa, 1.0f);
        const vec4 cb = shadow_map.light_vp * vec4(pb, 1.0f);
        const vec4 cc = shadow_map.light_vp * vec4(pc, 1.0f);

        if (clip_reject(ca, cb, cc))
            continue;

        // Ortho projection: w is always 1, perspective divide is safe.
        const vec3 ndc_a = ca.perspective_divide();
        const vec3 ndc_b = cb.perspective_divide();
        const vec3 ndc_c = cc.perspective_divide();

        // Map NDC [-1,1] → shadow map pixels [0, S-1].
        // Use (ndc + 1) / 2 * S consistently for both write and read.
        auto to_spx = [&](vec3 ndc) -> vec3
        {
            return {(ndc.x + 1.0f) * 0.5f * S,
                    (ndc.y + 1.0f) * 0.5f * S,
                    ndc.z};
        };
        const vec3 sa = to_spx(ndc_a);
        const vec3 sb = to_spx(ndc_b);
        const vec3 sc = to_spx(ndc_c);

        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({sa.x, sb.x, sc.x}))));
        const int x1 = std::min(S - 1, static_cast<int>(std::ceil(std::max({sa.x, sb.x, sc.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({sa.y, sb.y, sc.y}))));
        const int y1 = std::min(S - 1, static_cast<int>(std::ceil(std::max({sa.y, sb.y, sc.y}))));

        const float denom = (sb.y - sc.y) * (sa.x - sc.x) + (sc.x - sb.x) * (sa.y - sc.y);
        if (std::abs(denom) < 1e-6f)
            continue;
        const float inv_d = 1.0f / denom;

        const float ba_dx = (sb.y - sc.y) * inv_d;
        const float ba_dy = (sc.x - sb.x) * inv_d;
        const float bb_dx = (sc.y - sa.y) * inv_d;
        const float bb_dy = (sa.x - sc.x) * inv_d;

        const float px0 = static_cast<float>(x0) + 0.5f;
        const float py0 = static_cast<float>(y0) + 0.5f;
        float ba_row = ((sb.y - sc.y) * (px0 - sc.x) + (sc.x - sb.x) * (py0 - sc.y)) * inv_d;
        float bb_row = ((sc.y - sa.y) * (px0 - sc.x) + (sa.x - sc.x) * (py0 - sc.y)) * inv_d;

        for (int y = y0; y <= y1; y++)
        {
            float ba = ba_row, bb = bb_row;
            const size_t row_base = static_cast<size_t>(y) * S;
            for (int x = x0; x <= x1; x++)
            {
                const float bc = 1.0f - ba - bb;
                if (ba >= 0.0f && bb >= 0.0f && bc >= 0.0f)
                {
                    if (atex)
                    {
                        const vec2 uv = uva * ba + uvb * bb + uvc * bc;
                        if (atex->sample_rgba(uv.x, uv.y).w < mat.alpha_cutoff)
                        {
                            ba += ba_dx;
                            bb += bb_dx;
                            continue;
                        }
                    }
                    const float d = ba * sa.z + bb * sb.z + bc * sc.z + slope_bias;
                    float &stored = shadow_map.depth[row_base + static_cast<size_t>(x)];
                    if (d < stored)
                        stored = d;
                }
                ba += ba_dx;
                bb += bb_dx;
            }
            ba_row += ba_dy;
            bb_row += bb_dy;
        }
    }

    return shadow_map;
}
