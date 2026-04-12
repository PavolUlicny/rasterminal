#include "renderer.h"

#include <algorithm>
#include <cmath>

// ─── internal helpers ─────────────────────────────────────────────────────────

static Color vec3_to_color(vec3 c)
{
    return {
        (uint8_t)(clamp(c.x, 0.0f, 1.0f) * 255.0f),
        (uint8_t)(clamp(c.y, 0.0f, 1.0f) * 255.0f),
        (uint8_t)(clamp(c.z, 0.0f, 1.0f) * 255.0f)};
}

// Conservative frustum rejection: returns true if all three clip-space vertices
// lie entirely outside any single frustum half-space. Does not clip — just
// avoids processing triangles that are obviously invisible.
static bool clip_reject(const vec4 &a, const vec4 &b, const vec4 &c)
{
    if (a.w <= 0.0f || b.w <= 0.0f || c.w <= 0.0f)
        return true;
    if (a.x > a.w && b.x > b.w && c.x > c.w)
        return true;
    if (a.x < -a.w && b.x < -b.w && c.x < -c.w)
        return true;
    if (a.y > a.w && b.y > b.w && c.y > c.w)
        return true;
    if (a.y < -a.w && b.y < -b.w && c.y < -c.w)
        return true;
    if (a.z > a.w && b.z > b.w && c.z > c.w)
        return true;
    if (a.z < -a.w && b.z < -b.w && c.z < -c.w)
        return true;
    return false;
}

// NDC → screen-space pixel coordinates.
// NDC x/y ∈ [-1,1]; y is flipped (NDC +1 = top, screen y=0 = top).
// z is kept as NDC depth for the z-buffer.
static vec3 ndc_to_screen(vec3 ndc, int W, int H)
{
    return {
        (ndc.x + 1.0f) * 0.5f * (float)W,
        (1.0f - ndc.y) * 0.5f * (float)H,
        ndc.z};
}

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

// ─── Shadow map ───────────────────────────────────────────────────────────────
// Off-screen depth buffer rendered from the key light's point of view.
// Used in all shading modes to determine which pixels receive direct light.

struct ShadowMap
{
    static constexpr int SIZE = 256;
    float depth[SIZE * SIZE]; // NDC z (slope-biased on write), initialised to 1.0
    mat4 light_vp;

    void clear()
    {
        for (auto &d : depth)
            d = 1.0f;
    }

    // Returns true if world_pos is occluded from the key light.
    // Slope-scale bias is baked into the stored depths at build time, so only a
    // tiny constant is needed here to guard against floating-point round-trip error.
    bool in_shadow(vec3 world_pos) const
    {
        vec4 lc = light_vp * vec4(world_pos, 1.0f);
        if (lc.w <= 0.0f)
            return false;
        vec3 ndc = lc.perspective_divide();
        // Outside the light frustum → treat as lit (no shadow cast here).
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f)
            return false;
        float u = (ndc.x + 1.0f) * 0.5f;
        float v = (ndc.y + 1.0f) * 0.5f;
        int px = clamp((int)(u * SIZE), 0, SIZE - 1);
        int py = clamp((int)(v * SIZE), 0, SIZE - 1);
        constexpr float fp_eps = 0.001f;
        return ndc.z > depth[py * SIZE + px] + fp_eps;
    }
};

// Build a shadow map for the given directional light by depth-rasterizing the
// mesh with an orthographic projection fitted to the mesh bounding sphere.
// Only the key light (lights[0]) casts shadows; this function receives that light.
static ShadowMap build_shadow_map(const Mesh &mesh, const Light &light)
{
    ShadowMap smap;
    smap.clear();

    if (mesh.vertices.empty())
        return smap;

    // Bounding sphere: centroid + max radius.
    vec3 center{};
    for (const auto &v : mesh.vertices)
        center += v.pos;
    center = center * (1.0f / (float)mesh.vertices.size());
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

        int x0 = std::max(0, (int)std::floor(std::min({sa.x, sb.x, sc.x})));
        int x1 = std::min(S - 1, (int)std::ceil(std::max({sa.x, sb.x, sc.x})));
        int y0 = std::max(0, (int)std::floor(std::min({sa.y, sb.y, sc.y})));
        int y1 = std::min(S - 1, (int)std::ceil(std::max({sa.y, sb.y, sc.y})));

        float denom = (sb.y - sc.y) * (sa.x - sc.x) + (sc.x - sb.x) * (sa.y - sc.y);
        if (std::abs(denom) < 1e-6f)
            continue;
        float inv_d = 1.0f / denom;

        for (int y = y0; y <= y1; y++)
        {
            for (int x = x0; x <= x1; x++)
            {
                float px = x + 0.5f, py = y + 0.5f;
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

// DDA line rasterizer with per-pixel depth testing.
static void draw_line(Framebuffer &fb, vec3 a, vec3 b, Color color)
{
    int x0 = (int)std::round(a.x), y0 = (int)std::round(a.y);
    int x1 = (int)std::round(b.x), y1 = (int)std::round(b.y);

    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int steps = std::max(dx, dy);

    if (steps == 0)
    {
        if (fb.test_and_set_depth(x0, y0, a.z))
            fb.set_pixel(x0, y0, color);
        return;
    }

    float sx = (float)(x1 - x0) / (float)steps;
    float sy = (float)(y1 - y0) / (float)steps;
    float sz = (b.z - a.z) / (float)steps;

    float x = (float)x0, y = (float)y0, z = a.z;
    for (int i = 0; i <= steps; i++)
    {
        int px = (int)std::round(x), py = (int)std::round(y);
        if (fb.test_and_set_depth(px, py, z))
            fb.set_pixel(px, py, color);
        x += sx;
        y += sy;
        z += sz;
    }
}

// Rasterize a triangle using screen-space barycentric coordinates.
// sa/sb/sc hold (screen_x, screen_y, ndc_z).
// wa/wb/wc are clip-space w values for perspective-correct interpolation.
// col_a/b/c are per-vertex Blinn-Phong colours when lit; shad_a/b/c when in shadow.
// pa/pb/pc are world-space positions used for the per-pixel shadow test.
// uva/uvb/uvc are per-vertex texture coordinates.
// tex may be nullptr if no diffuse texture is active.
// smap may be nullptr if shadows are disabled.
static void rasterize(Framebuffer &fb,
                      vec3 sa, vec3 sb, vec3 sc,
                      float wa, float wb, float wc,
                      vec3 col_a, vec3 col_b, vec3 col_c,
                      vec3 shad_a, vec3 shad_b, vec3 shad_c,
                      vec3 pa, vec3 pb, vec3 pc,
                      vec2 uva, vec2 uvb, vec2 uvc,
                      const Texture *tex,
                      const ShadowMap *smap)
{
    const int W = fb.width();
    const int H = fb.height();

    // Bounding box, clamped to framebuffer
    int x0 = std::max(0, (int)std::floor(std::min({sa.x, sb.x, sc.x})));
    int x1 = std::min(W - 1, (int)std::ceil(std::max({sa.x, sb.x, sc.x})));
    int y0 = std::max(0, (int)std::floor(std::min({sa.y, sb.y, sc.y})));
    int y1 = std::min(H - 1, (int)std::ceil(std::max({sa.y, sb.y, sc.y})));

    // Barycentric denominator (proportional to 2× signed screen area)
    float denom = (sb.y - sc.y) * (sa.x - sc.x) + (sc.x - sb.x) * (sa.y - sc.y);
    if (std::abs(denom) < 1e-6f)
        return;
    float inv_d = 1.0f / denom;

    // Reciprocal clip-space w for perspective-correct interpolation
    float inv_wa = 1.0f / wa;
    float inv_wb = 1.0f / wb;
    float inv_wc = 1.0f / wc;

    for (int y = y0; y <= y1; y++)
    {
        for (int x = x0; x <= x1; x++)
        {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;

            // Screen-space barycentric weights
            float ba = ((sb.y - sc.y) * (px - sc.x) + (sc.x - sb.x) * (py - sc.y)) * inv_d;
            float bb = ((sc.y - sa.y) * (px - sc.x) + (sa.x - sc.x) * (py - sc.y)) * inv_d;
            float bc = 1.0f - ba - bb;

            if (ba < 0.0f || bb < 0.0f || bc < 0.0f)
                continue;

            // z_ndc is linear in screen space (projection makes it A + B/z_view,
            // which is linear in NDC x/y), so plain barycentric is correct here —
            // perspective correction would distort it and break depth ordering.
            float depth = ba * sa.z + bb * sb.z + bc * sc.z;

            // Perspective-correct weight for colour and UV interpolation.
            float inv_w = ba * inv_wa + bb * inv_wb + bc * inv_wc;
            float w_corr = 1.0f / inv_w;

            if (!fb.test_and_set_depth(x, y, depth))
                continue;

            // Per-pixel shadow test using interpolated world position.
            bool shadowed = false;
            if (smap)
            {
                vec3 pos = (pa * (ba * inv_wa) + pb * (bb * inv_wb) + pc * (bc * inv_wc)) * w_corr;
                shadowed = smap->in_shadow(pos);
            }
            const vec3 &ca = shadowed ? shad_a : col_a;
            const vec3 &cb = shadowed ? shad_b : col_b;
            const vec3 &cc = shadowed ? shad_c : col_c;

            vec3 col = (ca * (ba * inv_wa) + cb * (bb * inv_wb) + cc * (bc * inv_wc)) * w_corr;

            if (tex)
            {
                vec2 uv = (uva * (ba * inv_wa) + uvb * (bb * inv_wb) + uvc * (bc * inv_wc)) * w_corr;
                col = col * tex->sample_rgb(uv.x, uv.y);
            }

            fb.set_pixel(x, y, vec3_to_color(col));
        }
    }
}

// Rasterize a triangle with per-pixel Blinn-Phong lighting (Phong shading).
// Perspective-correct interpolates world-space position and normal to each
// pixel, then evaluates compute_lighting() there.
// uva/uvb/uvc are per-vertex texture coordinates.
// tex may be nullptr if no diffuse texture is active; when present its RGB is
// multiplied into mat.diffuse before the lighting calculation.
static void rasterize_phong(Framebuffer &fb,
                            vec3 sa, vec3 sb, vec3 sc,
                            float wa, float wb, float wc,
                            vec3 pa, vec3 pb, vec3 pc,
                            vec3 na, vec3 nb, vec3 nc,
                            vec3 tana, vec3 tanb, vec3 tanc,
                            vec2 uva, vec2 uvb, vec2 uvc,
                            const vec3 &eye,
                            const Light *lights, int n_lights,
                            const vec3 &ambient,
                            const Material &mat,
                            const Texture *tex,
                            const Texture *nmap,
                            const ShadowMap *smap)
{
    const int W = fb.width();
    const int H = fb.height();

    int x0 = std::max(0, (int)std::floor(std::min({sa.x, sb.x, sc.x})));
    int x1 = std::min(W - 1, (int)std::ceil(std::max({sa.x, sb.x, sc.x})));
    int y0 = std::max(0, (int)std::floor(std::min({sa.y, sb.y, sc.y})));
    int y1 = std::min(H - 1, (int)std::ceil(std::max({sa.y, sb.y, sc.y})));

    float denom = (sb.y - sc.y) * (sa.x - sc.x) + (sc.x - sb.x) * (sa.y - sc.y);
    if (std::abs(denom) < 1e-6f)
        return;
    float inv_d = 1.0f / denom;

    float inv_wa = 1.0f / wa;
    float inv_wb = 1.0f / wb;
    float inv_wc = 1.0f / wc;

    for (int y = y0; y <= y1; y++)
    {
        for (int x = x0; x <= x1; x++)
        {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;

            float ba = ((sb.y - sc.y) * (px - sc.x) + (sc.x - sb.x) * (py - sc.y)) * inv_d;
            float bb = ((sc.y - sa.y) * (px - sc.x) + (sa.x - sc.x) * (py - sc.y)) * inv_d;
            float bc = 1.0f - ba - bb;

            if (ba < 0.0f || bb < 0.0f || bc < 0.0f)
                continue;

            float depth = ba * sa.z + bb * sb.z + bc * sc.z;
            if (!fb.test_and_set_depth(x, y, depth))
                continue;

            // Perspective-correct interpolation of world-space position and normal.
            float inv_w = ba * inv_wa + bb * inv_wb + bc * inv_wc;
            float w_corr = 1.0f / inv_w;

            vec3 pos = (pa * (ba * inv_wa) + pb * (bb * inv_wb) + pc * (bc * inv_wc)) * w_corr;
            vec3 nrm = (na * (ba * inv_wa) + nb * (bb * inv_wb) + nc * (bc * inv_wc)) * w_corr;

            // Compute UV once — needed by both diffuse and normal map.
            vec2 uv{};
            if (tex || nmap)
                uv = (uva * (ba * inv_wa) + uvb * (bb * inv_wb) + uvc * (bc * inv_wc)) * w_corr;

            // Normal mapping: sample tangent-space normal, rotate into world space via TBN.
            if (nmap)
            {
                vec3 tan = (tana * (ba * inv_wa) + tanb * (bb * inv_wb) + tanc * (bc * inv_wc)) * w_corr;

                // Unpack normal map texel from [0,1] to [-1,1].
                vec3 nm = nmap->sample_rgb(uv.x, uv.y) * 2.0f - vec3{1.0f, 1.0f, 1.0f};

                // Re-orthogonalize T against the interpolated N (Gram-Schmidt),
                // then derive B so TBN is a proper orthonormal basis.
                vec3 N = normalize(nrm);
                vec3 T = normalize(tan - N * dot(N, tan));
                vec3 B = cross(N, T);

                // Transform tangent-space normal to world space.
                nrm = T * nm.x + B * nm.y + N * nm.z;
                // nrm will be normalized inside compute_lighting.
            }

            Material px_mat = mat;
            if (tex)
                px_mat.diffuse = px_mat.diffuse * tex->sample_rgb(uv.x, uv.y);

            // Shadow test: if the key light (index 0) is blocked, skip it.
            bool shadowed = smap && smap->in_shadow(pos);
            vec3 color = shadowed
                             ? compute_lighting(pos, nrm, eye, lights + 1, n_lights - 1, ambient, px_mat)
                             : compute_lighting(pos, nrm, eye, lights, n_lights, ambient, px_mat);
            fb.set_pixel(x, y, vec3_to_color(color));
        }
    }
}

// ─── Near-plane clipping ──────────────────────────────────────────────────────
// A clip-space vertex bundled with the world-space attributes needed for lighting.
struct ClipVert
{
    vec4 c;       // clip-space position (w = -z_view; > 0 means in front of camera)
    vec3 pos;     // world-space position
    vec3 normal;  // world-space normal
    vec3 tangent; // world-space tangent (for TBN normal mapping)
    vec2 uv;      // texture coordinates
};

// Clip triangle (a,b,c) against the near plane w = NEAR_W to prevent
// division-by-near-zero in the perspective divide and the rendering artefacts
// that occur when a triangle straddles the camera plane.
//
// Produces 0 (fully behind), 1, or 2 output triangles written into out[0..n-1].
// Returns the count.  Winding order is preserved for all outputs.
static int clip_near(ClipVert a, ClipVert b, ClipVert c, ClipVert out[2][3])
{
    // Must match the camera near plane (camera.cpp: perspective(..., 0.1f, ...)).
    // Too small a value produces off-screen NDC coordinates whose magnitude
    // overwhelms float precision in the barycentric computation.
    constexpr float NEAR_W = 0.1f;

    const bool ia = a.c.w > NEAR_W;
    const bool ib = b.c.w > NEAR_W;
    const bool ic = c.c.w > NEAR_W;
    const int n = (int)ia + (int)ib + (int)ic;

    if (n == 3)
    {
        out[0][0] = a;
        out[0][1] = b;
        out[0][2] = c;
        return 1;
    }
    if (n == 0)
    {
        return 0;
    }

    // Interpolate all attributes from an inside vertex v0 toward an outside
    // vertex v1 to find the exact w = NEAR_W crossing.
    auto cross_edge = [&](const ClipVert &v0, const ClipVert &v1) -> ClipVert
    {
        float t = (NEAR_W - v0.c.w) / (v1.c.w - v0.c.w);
        return {v0.c + (v1.c - v0.c) * t,
                v0.pos + (v1.pos - v0.pos) * t,
                v0.normal + (v1.normal - v0.normal) * t,
                v0.tangent + (v1.tangent - v0.tangent) * t,
                v0.uv + (v1.uv - v0.uv) * t};
    };

    if (n == 1)
    {
        // Rotate so the single inside vertex is first.
        if (ib)
        {
            ClipVert t = a;
            a = b;
            b = c;
            c = t;
        }
        else if (ic)
        {
            ClipVert t = a;
            a = c;
            c = b;
            b = t;
        }
        // a inside; b, c outside → one clipped triangle.
        out[0][0] = a;
        out[0][1] = cross_edge(a, b);
        out[0][2] = cross_edge(a, c);
        return 1;
    }

    // n == 2: rotate so the single outside vertex is last.
    if (!ic)
    { /* a, b inside, c outside — already correct */
    }
    else if (!ia)
    {
        ClipVert t = a;
        a = b;
        b = c;
        c = t;
    }
    else
    {
        ClipVert t = b;
        b = a;
        a = c;
        c = t;
    }
    // a, b inside; c outside → clipped quad → two triangles.
    ClipVert ac = cross_edge(a, c);
    ClipVert bc = cross_edge(b, c);
    out[0][0] = a;
    out[0][1] = b;
    out[0][2] = bc;
    out[1][0] = a;
    out[1][1] = bc;
    out[1][2] = ac;
    return 2;
}

// ─── Renderer::render ─────────────────────────────────────────────────────────

void Renderer::render(const Mesh &mesh, const Camera &camera,
                      const Light *lights, int n_lights, const vec3 &ambient,
                      Framebuffer &fb) const
{
    const mat4 view = camera.view();
    const mat4 proj = camera.projection(fb.width(), fb.height());
    const mat4 vp = proj * view;
    const vec3 eye = camera.eye();
    const int W = fb.width();
    const int H = fb.height();

    // Build shadow map from key light (lights[0]) before the main render pass.
    ShadowMap smap;
    const ShadowMap *psmap = nullptr;
    if (n_lights > 0)
    {
        smap = build_shadow_map(mesh, lights[0]);
        psmap = &smap;
    }

    for (const Triangle &tri : mesh.triangles)
    {
        const Vertex &va = mesh.vertices[tri.v[0]];
        const Vertex &vb = mesh.vertices[tri.v[1]];
        const Vertex &vc = mesh.vertices[tri.v[2]];

        // Material lookup (index 0 is always the default).
        const Material &mat = (tri.material_idx < mesh.materials.size())
                                  ? mesh.materials[tri.material_idx]
                                  : mesh.materials[0];

        // ── Texture lookup for this triangle ─────────────────────────
        const Texture *tex = nullptr;
        if (mat.diffuse_tex >= 0 && mat.diffuse_tex < (int)mesh.textures.size())
            tex = &mesh.textures[(size_t)mat.diffuse_tex];

        const Texture *nmap = nullptr;
        if (mat.normal_tex >= 0 && mat.normal_tex < (int)mesh.textures.size())
            nmap = &mesh.textures[(size_t)mat.normal_tex];

        // ── Transform to clip space ───────────────────────────────────
        ClipVert cva = {vp * vec4(va.pos, 1.0f), va.pos, va.normal, va.tangent, va.uv};
        ClipVert cvb = {vp * vec4(vb.pos, 1.0f), vb.pos, vb.normal, vb.tangent, vb.uv};
        ClipVert cvc = {vp * vec4(vc.pos, 1.0f), vc.pos, vc.normal, vc.tangent, vc.uv};

        // ── Near-plane clip → 0, 1, or 2 triangles ───────────────────
        ClipVert clipped[2][3];
        int n_tris = clip_near(cva, cvb, cvc, clipped);

        for (int ti = 0; ti < n_tris; ti++)
        {
            const ClipVert &a = clipped[ti][0];
            const ClipVert &b = clipped[ti][1];
            const ClipVert &c = clipped[ti][2];

            // Conservative frustum rejection against the remaining 5 planes.
            if (clip_reject(a.c, b.c, c.c))
                continue;

            // ── Perspective divide → NDC → screen ─────────────────────
            vec3 sa = ndc_to_screen(a.c.perspective_divide(), W, H);
            vec3 sb = ndc_to_screen(b.c.perspective_divide(), W, H);
            vec3 sc = ndc_to_screen(c.c.perspective_divide(), W, H);

            // ── Backface culling (screen-space signed area) ────────────
            float area = (sb.x - sa.x) * (sc.y - sa.y) - (sc.x - sa.x) * (sb.y - sa.y);
            if (area >= 0.0f)
                continue;

            // ── Wireframe ─────────────────────────────────────────────
            if (mode == ShadingMode::Wireframe)
            {
                const Color wf = {200, 200, 200};
                draw_line(fb, sa, sb, wf);
                draw_line(fb, sb, sc, wf);
                draw_line(fb, sc, sa, wf);
                continue;
            }

            // ── Shading ───────────────────────────────────────────────
            if (mode == ShadingMode::Phong)
            {
                rasterize_phong(fb, sa, sb, sc, a.c.w, b.c.w, c.c.w,
                                a.pos, b.pos, c.pos,
                                a.normal, b.normal, c.normal,
                                a.tangent, b.tangent, c.tangent,
                                a.uv, b.uv, c.uv,
                                eye, lights, n_lights, ambient, mat, tex, nmap, psmap);
                continue;
            }

            vec3 col_a, col_b, col_c;    // lit colors
            vec3 shad_a, shad_b, shad_c; // shadow colors (key light removed)

            if (mode == ShadingMode::Flat)
            {
                vec3 fn = normalize(cross(b.pos - a.pos, c.pos - a.pos));
                vec3 fc = (a.pos + b.pos + c.pos) * (1.0f / 3.0f);
                col_a = col_b = col_c = compute_lighting(fc, fn, eye, lights, n_lights, ambient, mat);
                shad_a = shad_b = shad_c = compute_lighting(fc, fn, eye, lights + 1, n_lights - 1, ambient, mat);
            }
            else // Gouraud
            {
                col_a = compute_lighting(a.pos, a.normal, eye, lights, n_lights, ambient, mat);
                col_b = compute_lighting(b.pos, b.normal, eye, lights, n_lights, ambient, mat);
                col_c = compute_lighting(c.pos, c.normal, eye, lights, n_lights, ambient, mat);
                shad_a = compute_lighting(a.pos, a.normal, eye, lights + 1, n_lights - 1, ambient, mat);
                shad_b = compute_lighting(b.pos, b.normal, eye, lights + 1, n_lights - 1, ambient, mat);
                shad_c = compute_lighting(c.pos, c.normal, eye, lights + 1, n_lights - 1, ambient, mat);
            }

            rasterize(fb, sa, sb, sc, a.c.w, b.c.w, c.c.w,
                      col_a, col_b, col_c, shad_a, shad_b, shad_c,
                      a.pos, b.pos, c.pos,
                      a.uv, b.uv, c.uv, tex, psmap);
        }
    }
}

// ─── Renderer::cycle_shading ──────────────────────────────────────────────────

void Renderer::cycle_shading()
{
    switch (mode)
    {
    case ShadingMode::Wireframe:
        mode = ShadingMode::Flat;
        break;
    case ShadingMode::Flat:
        mode = ShadingMode::Gouraud;
        break;
    case ShadingMode::Gouraud:
        mode = ShadingMode::Phong;
        break;
    case ShadingMode::Phong:
        mode = ShadingMode::Wireframe;
        break;
    }
}
