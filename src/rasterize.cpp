#include "rasterize.h"
#include "framebuffer.h"
#include "light.h"
#include "linalg.h"
#include "shadow.h"
#include "texture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// ─── internal helpers ─────────────────────────────────────────────────────────

namespace
{

    constexpr float DEGEN_AREA_EPS = 1e-6f; // minimum |denom| to treat a triangle as non-degenerate

    // Standard normal-incidence reflectance (F0) for dielectrics; metals lerp from
    // this toward their base colour by the metalness factor.
    constexpr vec3 DIELECTRIC_F0{ 0.04f, 0.04f, 0.04f };

    constexpr Color vec3_to_color(vec3 c) noexcept
    {
        return { static_cast<uint8_t>(clamp(c.x, 0.0f, 1.0f) * 255.0f),
                 static_cast<uint8_t>(clamp(c.y, 0.0f, 1.0f) * 255.0f),
                 static_cast<uint8_t>(clamp(c.z, 0.0f, 1.0f) * 255.0f) };
    }

    // Precomputed barycentric rasterization setup for one triangle.
    struct TriSetup
    {
        int x0, x1, y0, y1;           // pixel bounding box, clamped to band and screen
        float inv_wa, inv_wb, inv_wc; // reciprocal clip-space w (perspective correction)
        float ba_dx, ba_dy;           // ba gradient per pixel-column / pixel-row
        float bb_dx, bb_dy;
        float ba_row, bb_row; // ba/bb at pixel center (x0+0.5, y0+0.5)
    };

    // Fill s from the three screen-space vertices (x,y,ndc_z) and clip-space w values.
    // width is the framebuffer width; y_min/y_max are the thread's row band.
    // Returns false if the triangle is degenerate or misses the band entirely.
    bool
    setup_tri(vec3 sa, vec3 sb, vec3 sc, float wa, float wb, float wc, int width, int y_min, int y_max, TriSetup &s)
    {
        s.x0 = std::max(0, static_cast<int>(std::floor(std::min({ sa.x, sb.x, sc.x }))));
        s.x1 = std::min(width - 1, static_cast<int>(std::ceil(std::max({ sa.x, sb.x, sc.x }))));
        s.y0 = std::max(y_min, static_cast<int>(std::floor(std::min({ sa.y, sb.y, sc.y }))));
        s.y1 = std::min(y_max, static_cast<int>(std::ceil(std::max({ sa.y, sb.y, sc.y }))));
        if (s.y0 > s.y1 || s.x0 > s.x1)
        {
            return false;
        }

        // Barycentric denominator (proportional to 2× signed screen area).
        const float denom = ((sb.y - sc.y) * (sa.x - sc.x)) + ((sc.x - sb.x) * (sa.y - sc.y));
        if (std::abs(denom) < DEGEN_AREA_EPS)
        {
            return false;
        }
        const float inv_d = 1.0f / denom;

        s.inv_wa = 1.0f / wa;
        s.inv_wb = 1.0f / wb;
        s.inv_wc = 1.0f / wc;

        s.ba_dx = (sb.y - sc.y) * inv_d;
        s.ba_dy = (sc.x - sb.x) * inv_d;
        s.bb_dx = (sc.y - sa.y) * inv_d;
        s.bb_dy = (sa.x - sc.x) * inv_d;

        const float px0 = static_cast<float>(s.x0) + 0.5f;
        const float py0 = static_cast<float>(s.y0) + 0.5f;
        s.ba_row = (((sb.y - sc.y) * (px0 - sc.x)) + ((sc.x - sb.x) * (py0 - sc.y))) * inv_d;
        s.bb_row = (((sc.y - sa.y) * (px0 - sc.x)) + ((sa.x - sc.x) * (py0 - sc.y))) * inv_d;
        return true;
    }

} // namespace

// ─── clip_near ────────────────────────────────────────────────────────────────
// Clip triangle (a,b,c) against the near plane w = NEAR_W to prevent
// division-by-near-zero in the perspective divide and the rendering artefacts
// that occur when a triangle straddles the camera plane.
//
// near_w must match camera.near_plane: the clip-space w at the near plane.
// Too small a value produces off-screen NDC coordinates whose magnitude
// overwhelms float precision in the barycentric computation.

int clip_near(const ClipVert &a, const ClipVert &b, const ClipVert &c, ClipVert out[2][3], float near_w)
{
    const float NEAR_W = near_w;

    const bool ia = a.c.w > NEAR_W;
    const bool ib = b.c.w > NEAR_W;
    const bool ic = c.c.w > NEAR_W;
    const int n = static_cast<int>(ia) + static_cast<int>(ib) + static_cast<int>(ic);

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

    // Only cases with partial visibility need local permutation/crossing.
    ClipVert aa = a;
    ClipVert bb = b;
    ClipVert cc = c;

    // Interpolate all attributes from an inside vertex v0 toward an outside
    // vertex v1 to find the exact w = NEAR_W crossing.
    auto cross_edge = [&](const ClipVert &v0, const ClipVert &v1) -> ClipVert
    {
        const float t = (NEAR_W - v0.c.w) / (v1.c.w - v0.c.w);
        return { v0.c + (v1.c - v0.c) * t,
                 v0.pos + (v1.pos - v0.pos) * t,
                 v0.normal + (v1.normal - v0.normal) * t,
                 v0.tangent + (v1.tangent - v0.tangent) * t,
                 v0.uv + (v1.uv - v0.uv) * t,
                 v0.ao + ((v1.ao - v0.ao) * t),
                 v0.color + (v1.color - v0.color) * t };
    };

    if (n == 1)
    {
        // Rotate so the single inside vertex is first.
        if (ib)
        {
            const ClipVert t = aa;
            aa = bb;
            bb = cc;
            cc = t;
        }
        else if (ic)
        {
            const ClipVert t = aa;
            aa = cc;
            cc = bb;
            bb = t;
        }
        // a inside; b, c outside → one clipped triangle.
        out[0][0] = aa;
        out[0][1] = cross_edge(aa, bb);
        out[0][2] = cross_edge(aa, cc);
        return 1;
    }

    // n == 2: rotate so the single outside vertex is last.
    if (!ic)
    { /* a, b inside, c outside — already correct */
    }
    else if (!ia)
    {
        const ClipVert t = aa;
        aa = bb;
        bb = cc;
        cc = t;
    }
    else
    {
        const ClipVert t = bb;
        bb = aa;
        aa = cc;
        cc = t;
    }
    // a, b inside; c outside → clipped quad → two triangles.
    const ClipVert ac = cross_edge(aa, cc);
    const ClipVert bc = cross_edge(bb, cc);
    out[0][0] = aa;
    out[0][1] = bb;
    out[0][2] = bc;
    out[1][0] = aa;
    out[1][1] = bc;
    out[1][2] = ac;
    return 2;
}

// ─── draw_line ────────────────────────────────────────────────────────────────
// DDA line rasterizer with per-pixel depth testing.

void draw_line(Framebuffer &fb, vec3 a, vec3 b, Color color)
{
    const int x0 = static_cast<int>(std::round(a.x));
    const int y0 = static_cast<int>(std::round(a.y));
    const int x1 = static_cast<int>(std::round(b.x));
    const int y1 = static_cast<int>(std::round(b.y));

    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const int steps = std::max(dx, dy);

    if (steps == 0)
    {
        if (fb.test_and_set_depth(x0, y0, a.z))
        {
            fb.set_pixel(x0, y0, color);
        }
        return;
    }

    const float sx = static_cast<float>(x1 - x0) / static_cast<float>(steps);
    const float sy = static_cast<float>(y1 - y0) / static_cast<float>(steps);
    const float sz = (b.z - a.z) / static_cast<float>(steps);

    auto x = static_cast<float>(x0);
    auto y = static_cast<float>(y0);
    float z = a.z;
    for (int i = 0; i <= steps; i++)
    {
        const int px = static_cast<int>(std::round(x));
        const int py = static_cast<int>(std::round(y));
        if (fb.test_and_set_depth(px, py, z))
        {
            fb.set_pixel(px, py, color);
        }
        x += sx;
        y += sy;
        z += sz;
    }
}

// ─── rasterize ────────────────────────────────────────────────────────────────
// Rasterize a triangle using screen-space barycentric coordinates.
// sa/sb/sc hold (screen_x, screen_y, ndc_z).
// wa/wb/wc are clip-space w values for perspective-correct interpolation.
// col_a/b/c are per-vertex Blinn-Phong colours when lit; shad_a/b/c when in shadow.
// pa/pb/pc are world-space positions used for the per-pixel shadow test.
// uva/uvb/uvc are per-vertex texture coordinates.
// tex may be nullptr if no diffuse texture is active.
// shadow_map may be nullptr if shadows are disabled.

void rasterize(
    Framebuffer &fb,
    vec3 sa,
    vec3 sb,
    vec3 sc,
    float wa,
    float wb,
    float wc,
    vec3 col_a,
    vec3 col_b,
    vec3 col_c,
    vec3 shad_a,
    vec3 shad_b,
    vec3 shad_c,
    vec3 pa,
    vec3 pb,
    vec3 pc,
    vec2 uva,
    vec2 uvb,
    vec2 uvc,
    const Texture *tex,
    float alpha_cutoff,
    const ShadowMap *shadow_map,
    int y_min,
    int y_max,
    const Texture *etex,
    vec3 emissive
)
{
    const int width = fb.width();
    TriSetup s; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — setup_tri writes all fields before
                // they are read
    if (!setup_tri(sa, sb, sc, wa, wb, wc, width, y_min, y_max, s))
    {
        return;
    }

    const float inv_wa = s.inv_wa;
    const float inv_wb = s.inv_wb;
    const float inv_wc = s.inv_wc;
    const float ba_dx = s.ba_dx;
    const float ba_dy = s.ba_dy;
    const float bb_dx = s.bb_dx;
    const float bb_dy = s.bb_dy;
    float ba_row = s.ba_row;
    float bb_row = s.bb_row;

    const bool has_cutout = (alpha_cutoff > 0.0f && tex);
    // glTF: emissive = emissiveFactor * emissiveTexture.rgb. Factor {0,0,0} zeros the
    // contribution regardless of texture, so the texture sample is skippable too.
    const bool do_emissive = (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f);
    const auto stride = static_cast<size_t>(fb.width());

    for (int y = s.y0; y <= s.y1; y++)
    {
        float ba = ba_row;
        float bb = bb_row;
        size_t idx = (static_cast<size_t>(y) * stride) + static_cast<size_t>(s.x0);
        for (int x = s.x0; x <= s.x1; ++x, ++idx)
        {
            if (ba < 0.0f || bb < 0.0f)
            {
                ba += ba_dx;
                bb += bb_dx;
                continue;
            }
            const float bc = 1.0f - ba - bb;
            if (bc < 0.0f)
            {
                ba += ba_dx;
                bb += bb_dx;
                continue;
            }

            // z_ndc is linear in screen space (projection makes it A + B/z_view,
            // which is linear in NDC x/y), so plain barycentric is correct here —
            // perspective correction would distort it and break depth ordering.
            const float depth = (ba * sa.z) + (bb * sb.z) + (bc * sc.z);

            // Alpha cutout: sample before depth write so discarded pixels don't
            // claim z-buffer entries (otherwise transparent holes occlude geometry).
            float pwa = 0.0f;
            float pwb = 0.0f;
            float pwc = 0.0f;
            float w_corr = 1.0f;
            vec3 cutout_rgb;
            vec2 uv{}; // shared by cutout pre-pass, diffuse sample, and emissive sample
            if (has_cutout)
            {
                pwa = ba * inv_wa;
                pwb = bb * inv_wb;
                pwc = bc * inv_wc;
                w_corr = 1.0f / (pwa + pwb + pwc);
                uv = (uva * pwa + uvb * pwb + uvc * pwc) * w_corr;
                const vec4 ta = tex->sample_rgba(uv.x, uv.y);
                if (ta.w < alpha_cutoff)
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
                cutout_rgb = { ta.x, ta.y, ta.z };
            }

            if (!fb.depth_test_relaxed(idx, depth))
            {
                ba += ba_dx;
                bb += bb_dx;
                continue;
            }

            // Perspective-correct weights — computed once, reused for all attributes.
            if (!has_cutout)
            {
                pwa = ba * inv_wa;
                pwb = bb * inv_wb;
                pwc = bc * inv_wc;
                w_corr = 1.0f / (pwa + pwb + pwc);
            }

            // UV needed when sampling either the diffuse or the emissive texture; skip
            // recomputation when the cutout pre-pass already produced it. The emissive
            // sample is gated on do_emissive (factor non-zero), so don't pay for it on
            // factor-zero materials that happen to carry a bound emissive texture.
            if (!has_cutout && ((tex != nullptr) || (do_emissive && (etex != nullptr))))
            {
                uv = (uva * pwa + uvb * pwb + uvc * pwc) * w_corr;
            }

            // Per-pixel shadow test using interpolated world position.
            float sf = 0.0f;
            if (shadow_map)
            {
                const vec3 pos = (pa * pwa + pb * pwb + pc * pwc) * w_corr;
                sf = shadow_map->in_shadow(pos);
            }
            vec3 ca = col_a;
            vec3 cb = col_b;
            vec3 cc = col_c;
            if (sf > 0.0f)
            {
                ca = lerp(col_a, shad_a, sf);
                cb = lerp(col_b, shad_b, sf);
                cc = lerp(col_c, shad_c, sf);
            }

            vec3 col = (ca * pwa + cb * pwb + cc * pwc) * w_corr;

            if (tex)
            {
                col = col * (has_cutout ? cutout_rgb : tex->sample_rgb(uv.x, uv.y));
            }

            // Emissive add bypasses the shadow lerp so shaded areas still glow. The sum is
            // clamped per channel in vec3_to_color, so on already-near-1 lit surfaces the
            // emissive contribution saturates invisibly — visible only where the lit colour
            // has headroom. Real HDR (KHR_materials_emissive_strength + tonemap) would fix it.
            if (do_emissive)
            {
                vec3 e = emissive;
                if (etex)
                {
                    e = e * etex->sample_rgb(uv.x, uv.y);
                }
                col = col + e;
            }

            fb.commit_pixel(idx, depth, vec3_to_color(col));

            ba += ba_dx;
            bb += bb_dx;
        }

        ba_row += ba_dy;
        bb_row += bb_dy;
    }
}

// ─── rasterize_phong ──────────────────────────────────────────────────────────
// Rasterize a triangle with per-pixel Blinn-Phong lighting (Phong shading).
// Perspective-correct interpolates world-space position and normal to each
// pixel, then evaluates compute_lighting() there.
// uva/uvb/uvc are per-vertex texture coordinates.
// tex may be nullptr if no diffuse texture is active; when present its RGB is
// multiplied into mat.diffuse before the lighting calculation.

void rasterize_phong(
    Framebuffer &fb,
    vec3 sa,
    vec3 sb,
    vec3 sc,
    float wa,
    float wb,
    float wc,
    vec3 pa,
    vec3 pb,
    vec3 pc,
    vec3 na,
    vec3 nb,
    vec3 nc,
    vec3 tana,
    vec3 tanb,
    vec3 tanc,
    vec2 uva,
    vec2 uvb,
    vec2 uvc,
    float aoa,
    float aob,
    float aoc,
    vec3 vcola,
    vec3 vcolb,
    vec3 vcolc,
    bool has_vcol,
    const vec3 &eye,
    const Light *lights,
    int n_lights,
    const vec3 &ambient,
    const Material &mat,
    const Texture *tex,
    const Texture *nmap,
    const Texture *stex,
    const ShadowMap *shadow_map,
    int y_min,
    int y_max,
    const Texture *mrtex,
    const Texture *etex,
    vec3 emissive
)
{
    const int width = fb.width();
    TriSetup s; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — setup_tri writes all fields before
                // they are read
    if (!setup_tri(sa, sb, sc, wa, wb, wc, width, y_min, y_max, s))
    {
        return;
    }

    const float inv_wa = s.inv_wa;
    const float inv_wb = s.inv_wb;
    const float inv_wc = s.inv_wc;
    const float ba_dx = s.ba_dx;
    const float ba_dy = s.ba_dy;
    const float bb_dx = s.bb_dx;
    const float bb_dy = s.bb_dy;
    float ba_row = s.ba_row;
    float bb_row = s.bb_row;

    const bool has_cutout = (mat.alpha_cutoff > 0.0f && tex);
    // Off (false) for every dielectric and non-glTF material, so they run unchanged.
    const bool is_metallic = (mat.metallic > 0.0f);
    // glTF: emissive = emissiveFactor * emissiveTexture.rgb. Factor {0,0,0} zeros the
    // contribution regardless of texture, so the texture sample is skippable too.
    // The factor is passed in (not read from mat) so the caller can gate emissive entirely
    // on the texture-toggle UI flag — see show_emissive in renderer.cpp.
    const bool do_emissive = (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f);
    const auto stride = static_cast<size_t>(fb.width());

    for (int y = s.y0; y <= s.y1; y++)
    {
        float ba = ba_row;
        float bb = bb_row;
        size_t idx = (static_cast<size_t>(y) * stride) + static_cast<size_t>(s.x0);
        for (int x = s.x0; x <= s.x1; ++x, ++idx)
        {
            if (ba < 0.0f || bb < 0.0f)
            {
                ba += ba_dx;
                bb += bb_dx;
                continue;
            }
            const float bc = 1.0f - ba - bb;
            if (bc < 0.0f)
            {
                ba += ba_dx;
                bb += bb_dx;
                continue;
            }

            const float depth = (ba * sa.z) + (bb * sb.z) + (bc * sc.z);

            // Alpha cutout: sample before depth write so discarded pixels don't
            // claim z-buffer entries (otherwise transparent holes occlude geometry).
            float pwa = 0.0f;
            float pwb = 0.0f;
            float pwc = 0.0f;
            float w_corr = 1.0f;
            vec3 cutout_rgb;
            vec2 uv{}; // hoisted: shared by cutout pre-pass and nmap/stex below
            if (has_cutout)
            {
                pwa = ba * inv_wa;
                pwb = bb * inv_wb;
                pwc = bc * inv_wc;
                w_corr = 1.0f / (pwa + pwb + pwc);
                uv = (uva * pwa + uvb * pwb + uvc * pwc) * w_corr;
                const vec4 ta = tex->sample_rgba(uv.x, uv.y);
                if (ta.w < mat.alpha_cutoff)
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
                cutout_rgb = { ta.x, ta.y, ta.z };
            }

            if (!fb.depth_test_relaxed(idx, depth))
            {
                ba += ba_dx;
                bb += bb_dx;
                continue;
            }

            if (!has_cutout)
            {
                pwa = ba * inv_wa;
                pwb = bb * inv_wb;
                pwc = bc * inv_wc;
                w_corr = 1.0f / (pwa + pwb + pwc);
            }

            const vec3 pos = (pa * pwa + pb * pwb + pc * pwc) * w_corr;
            vec3 normal = (na * pwa + nb * pwb + nc * pwc) * w_corr;

            // Compute UV once — needed by diffuse, normal, specular, MR, and emissive samplers.
            // Skip when has_cutout: uv was already computed in the pre-pass above. Emissive
            // sample is gated on do_emissive, so a bound etex with a zero factor stays free.
            if (!has_cutout && (tex || nmap || stex || mrtex || (do_emissive && etex)))
            {
                uv = (uva * pwa + uvb * pwb + uvc * pwc) * w_corr;
            }

            // Normal mapping: sample tangent-space normal, rotate into world space via TBN.
            if (nmap)
            {
                const vec3 tan = (tana * pwa + tanb * pwb + tanc * pwc) * w_corr;

                // Unpack normal map texel from [0,1] to [-1,1].
                const vec3 nm = nmap->sample_rgb(uv.x, uv.y) * 2.0f - vec3{ 1.0f, 1.0f, 1.0f };

                // Re-orthogonalize T against the interpolated N (Gram-Schmidt),
                // then derive B so TBN is a proper orthonormal basis.
                const vec3 N = normalize(normal);
                const vec3 T = normalize(tan - N * dot(N, tan));
                const vec3 B = cross(N, T);

                // Transform tangent-space normal to world space.
                normal = T * nm.x + B * nm.y + N * nm.z;
                // normal will be normalized inside compute_lighting.
            }

            const float ao = ((aoa * pwa) + (aob * pwb) + (aoc * pwc)) * w_corr;

            // Precompute view vector once — reused by both shadow branches.
            const vec3 v = normalize(eye - pos);

            // Only copy Material when a texture modifies it; otherwise use mat directly.
            Material mat_tex;
            const Material *use_mat = &mat;
            if (tex || stex || is_metallic)
            {
                mat_tex = mat;
                if (tex)
                {
                    // Reuse the rgba sample from the cutout pre-pass when active.
                    const vec3 tc = has_cutout ? cutout_rgb : tex->sample_rgb(uv.x, uv.y);
                    mat_tex.diffuse = mat_tex.diffuse * tc;
                    mat_tex.ambient = mat_tex.ambient * tc;
                }
                if (stex)
                {
                    mat_tex.specular = mat_tex.specular * stex->sample_rgb(uv.x, uv.y);
                }
                use_mat = &mat_tex;
            }

            // Vertex color tint: skip entirely when all vertices are white (common case).
            if (has_vcol)
            {
                const vec3 vcol = (vcola * pwa + vcolb * pwb + vcolc * pwc) * w_corr;
                if (use_mat == &mat)
                {
                    mat_tex = mat;
                    use_mat = &mat_tex;
                }
                mat_tex.diffuse = mat_tex.diffuse * vcol;
                mat_tex.ambient = mat_tex.ambient * vcol;
            }

            // Metals tint specular reflectance (F0) toward their base colour (already
            // in mat_tex.diffuse); dielectrics keep the 4% baseline. Diffuse is
            // deliberately NOT zeroed as strict PBR would: with no environment map a
            // diffuse-less metal has nothing to reflect and renders near-black.
            if (is_metallic)
            {
                const vec3 base = mat_tex.diffuse;
                float metalness = mat.metallic;
                if (mrtex)
                {
                    const vec3 mr = mrtex->sample_rgb(uv.x, uv.y); // G=roughness, B=metallic
                    metalness *= mr.z;
                    mat_tex.shininess = roughness_to_shininess(mat.roughness * mr.y);
                }
                mat_tex.specular = lerp(DIELECTRIC_F0, base, metalness);
            }

            // Shadow test: PCF factor in [0,1]; lerp between lit and shadowed lighting.
            const float sf = shadow_map ? shadow_map->in_shadow(pos) : 0.0f;
            const Light *sl = (n_lights > 0) ? lights + 1 : lights;
            const int n_shadow = (n_lights > 0) ? n_lights - 1 : 0;
            vec3 color;
            if (sf <= 0.0f)
            {
                color = compute_lighting(normal, v, lights, n_lights, ambient, *use_mat, ao);
            }
            else if (sf >= 1.0f)
            {
                color = compute_lighting(normal, v, sl, n_shadow, ambient, *use_mat, ao);
            }
            else
            {
                const vec3 lit = compute_lighting(normal, v, lights, n_lights, ambient, *use_mat, ao);
                const vec3 shd = compute_lighting(normal, v, sl, n_shadow, ambient, *use_mat, ao);
                color = lerp(lit, shd, sf);
            }

            // Emissive add bypasses the shadow lerp so shaded areas still glow. The sum is
            // clamped per channel in vec3_to_color, so on already-near-1 lit surfaces the
            // emissive contribution saturates invisibly — visible only where the lit colour
            // has headroom. Real HDR (KHR_materials_emissive_strength + tonemap) would fix it.
            if (do_emissive)
            {
                vec3 e = emissive;
                if (etex)
                {
                    e = e * etex->sample_rgb(uv.x, uv.y);
                }
                color = color + e;
            }

            fb.commit_pixel(idx, depth, vec3_to_color(color));

            ba += ba_dx;
            bb += bb_dx;
        }

        ba_row += ba_dy;
        bb_row += bb_dy;
    }
}
