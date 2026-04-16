#include "shadow.h"
#include "rasterize.h"

#include <algorithm>
#include <cmath>

// ─── internal helpers ─────────────────────────────────────────────────────────

// Orthographic projection: maps axis-aligned box to NDC cube [-1,1]^3.
// l/r/b/t are left/right/bottom/top extents in light view space.
// n/f are near/far distances (positive).
static mat4 ortho(float l, float r, float b, float t, float n, float f)
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

// ─── ShadowMap ────────────────────────────────────────────────────────────────

bool ShadowMap::in_shadow(vec3 world_pos) const
{
    vec4 lc = light_vp * vec4(world_pos, 1.0f);
    if (lc.w <= 0.0f)
        return false;
    vec3 ndc = lc.perspective_divide();
    // Outside the light frustum → treat as lit (no shadow cast here).
    if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f ||
        ndc.z < -1.0f || ndc.z > 1.0f)
        return false;
    float u = (ndc.x + 1.0f) * 0.5f;
    float v = (ndc.y + 1.0f) * 0.5f;
    int px = std::clamp(static_cast<int>(u * static_cast<float>(SIZE)), 0, SIZE - 1);
    int py = std::clamp(static_cast<int>(v * static_cast<float>(SIZE)), 0, SIZE - 1);
    constexpr float fp_eps = 0.001f;
    return ndc.z > depth[py * SIZE + px] + fp_eps;
}

ShadowMap build_shadow_map(const Mesh &mesh, const Light &light)
{
    ShadowMap smap;
    smap.clear();

    if (mesh.vertices.empty())
        return smap;

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
    vec3 dir = normalize(light.direction);
    vec3 eye_pos = center + dir * (radius * 3.0f);
    vec3 world_up = (std::abs(dir.y) < 0.9f) ? vec3{0.0f, 1.0f, 0.0f} : vec3{1.0f, 0.0f, 0.0f};
    mat4 light_view = look_at(eye_pos, center, world_up);

    float ext = radius * 1.2f;
    mat4 light_proj = ortho(-ext, ext, -ext, ext, radius * 0.5f, radius * 6.0f);
    smap.light_vp = light_proj * light_view;

    const int S = ShadowMap::SIZE;

    // Depth-only rasterization into the shadow map.
    for (const Triangle &tri : mesh.triangles)
    {
        const vec3 &pa = mesh.vertices[tri.v[0]].pos;
        const vec3 &pb = mesh.vertices[tri.v[1]].pos;
        const vec3 &pc = mesh.vertices[tri.v[2]].pos;

        // Slope-scale bias: surfaces nearly tangent to the light direction need a
        // larger depth offset to avoid self-shadowing acne.  We bake it into the
        // stored depth so in_shadow() needs only a tiny epsilon.
        // bias = base / max(n·l, min_clamp)  →  large bias for glancing angles.
        vec3 face_n = normalize(cross(pb - pa, pc - pa));
        float n_dot_l = dot(face_n, dir);
        float slope_bias = (n_dot_l > 0.01f) ? 0.007f / n_dot_l : 0.5f;

        vec4 ca = smap.light_vp * vec4(pa, 1.0f);
        vec4 cb = smap.light_vp * vec4(pb, 1.0f);
        vec4 cc = smap.light_vp * vec4(pc, 1.0f);

        if (clip_reject(ca, cb, cc))
            continue;

        // Ortho projection: w is always 1, perspective divide is safe.
        vec3 ndc_a = ca.perspective_divide();
        vec3 ndc_b = cb.perspective_divide();
        vec3 ndc_c = cc.perspective_divide();

        // Map NDC [-1,1] → shadow map pixels [0, S-1].
        // Use (ndc + 1) / 2 * S consistently for both write and read.
        auto to_spx = [&](vec3 ndc) -> vec3
        {
            return {(ndc.x + 1.0f) * 0.5f * S,
                    (ndc.y + 1.0f) * 0.5f * S,
                    ndc.z};
        };
        vec3 sa = to_spx(ndc_a);
        vec3 sb = to_spx(ndc_b);
        vec3 sc = to_spx(ndc_c);

        int x0 = std::max(0, static_cast<int>(std::floor(std::min({sa.x, sb.x, sc.x}))));
        int x1 = std::min(S - 1, static_cast<int>(std::ceil(std::max({sa.x, sb.x, sc.x}))));
        int y0 = std::max(0, static_cast<int>(std::floor(std::min({sa.y, sb.y, sc.y}))));
        int y1 = std::min(S - 1, static_cast<int>(std::ceil(std::max({sa.y, sb.y, sc.y}))));

        float denom = (sb.y - sc.y) * (sa.x - sc.x) + (sc.x - sb.x) * (sa.y - sc.y);
        if (std::abs(denom) < 1e-6f)
            continue;
        float inv_d = 1.0f / denom;

        for (int y = y0; y <= y1; y++)
        {
            for (int x = x0; x <= x1; x++)
            {
                float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                float ba = ((sb.y - sc.y) * (px - sc.x) + (sc.x - sb.x) * (py - sc.y)) * inv_d;
                float bb = ((sc.y - sa.y) * (px - sc.x) + (sa.x - sc.x) * (py - sc.y)) * inv_d;
                float bc = 1.0f - ba - bb;
                if (ba < 0.0f || bb < 0.0f || bc < 0.0f)
                    continue;
                float d = ba * sa.z + bb * sb.z + bc * sc.z + slope_bias;
                float &stored = smap.depth[y * S + x];
                if (d < stored)
                    stored = d;
            }
        }
    }

    return smap;
}
