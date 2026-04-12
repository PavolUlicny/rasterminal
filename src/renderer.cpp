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
// col_a/b/c are per-vertex Blinn-Phong colours in [0,1].
// uva/uvb/uvc are per-vertex texture coordinates.
// tex may be nullptr if no diffuse texture is active.
static void rasterize(Framebuffer &fb,
                      vec3 sa, vec3 sb, vec3 sc,
                      float wa, float wb, float wc,
                      vec3 col_a, vec3 col_b, vec3 col_c,
                      vec2 uva, vec2 uvb, vec2 uvc,
                      const Texture *tex)
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

            vec3 col = (col_a * (ba * inv_wa) + col_b * (bb * inv_wb) + col_c * (bc * inv_wc)) * w_corr;

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
                            vec2 uva, vec2 uvb, vec2 uvc,
                            const vec3 &eye,
                            const Light *lights, int n_lights,
                            const vec3 &ambient,
                            const Material &mat,
                            const Texture *tex)
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
            // nrm is interpolated linearly; normalize before lighting so length
            // variations across the triangle don't affect the result.

            Material px_mat = mat;
            if (tex)
            {
                vec2 uv = (uva * (ba * inv_wa) + uvb * (bb * inv_wb) + uvc * (bc * inv_wc)) * w_corr;
                px_mat.diffuse = px_mat.diffuse * tex->sample_rgb(uv.x, uv.y);
            }

            fb.set_pixel(x, y, vec3_to_color(compute_lighting(pos, nrm, eye, lights, n_lights, ambient, px_mat)));
        }
    }
}

// ─── Near-plane clipping ──────────────────────────────────────────────────────
// A clip-space vertex bundled with the world-space attributes needed for lighting.
struct ClipVert
{
    vec4 c;      // clip-space position (w = -z_view; > 0 means in front of camera)
    vec3 pos;    // world-space position
    vec3 normal; // world-space normal
    vec2 uv;     // texture coordinates
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

        // ── Transform to clip space ───────────────────────────────────
        ClipVert cva = {vp * vec4(va.pos, 1.0f), va.pos, va.normal, va.uv};
        ClipVert cvb = {vp * vec4(vb.pos, 1.0f), vb.pos, vb.normal, vb.uv};
        ClipVert cvc = {vp * vec4(vc.pos, 1.0f), vc.pos, vc.normal, vc.uv};

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
                                a.uv, b.uv, c.uv,
                                eye, lights, n_lights, ambient, mat, tex);
                continue;
            }

            vec3 col_a, col_b, col_c;

            if (mode == ShadingMode::Flat)
            {
                vec3 fn = normalize(cross(b.pos - a.pos, c.pos - a.pos));
                vec3 fc = (a.pos + b.pos + c.pos) * (1.0f / 3.0f);
                col_a = col_b = col_c = compute_lighting(fc, fn, eye, lights, n_lights, ambient, mat);
            }
            else // Gouraud
            {
                col_a = compute_lighting(a.pos, a.normal, eye, lights, n_lights, ambient, mat);
                col_b = compute_lighting(b.pos, b.normal, eye, lights, n_lights, ambient, mat);
                col_c = compute_lighting(c.pos, c.normal, eye, lights, n_lights, ambient, mat);
            }

            rasterize(fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, col_a, col_b, col_c,
                      a.uv, b.uv, c.uv, tex);
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
