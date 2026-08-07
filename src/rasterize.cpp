#include "rasterize.h"
#include "color.h"
#include "framebuffer.h"
#include "light.h"
#include "linalg.h"
#include "texture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// internal helpers

namespace
{

    constexpr float DEGEN_AREA_EPS = 1e-6f; // minimum |denom| to treat a triangle as non-degenerate

    // Standard normal-incidence reflectance (F0) for dielectrics; metals lerp from
    // this toward their base colour by the metalness factor.
    constexpr vec3 DIELECTRIC_F0{ 0.04f, 0.04f, 0.04f };

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
                 v0.color + (v1.color - v0.color) * t,
                 v0.color_a + ((v1.color_a - v0.color_a) * t),
                 v0.uv1 + (v1.uv1 - v0.uv1) * t };
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
    if (!ia)
    {
        const ClipVert t = aa;
        aa = bb;
        bb = cc;
        cc = t;
    }
    else if (!ib)
    {
        const ClipVert t = bb;
        bb = aa;
        aa = cc;
        cc = t;
    }
    // else: c outside, already last, nothing to do.
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

    // Atomic depth-test + (depth,color) commit via CAS, so the wireframe pass can run
    // concurrently across the worker pool (the rasterizer owns the viewport bounds check
    // that commit_pixel itself does not). Single-threaded callers are byte-identical: the
    // CAS loop runs once when uncontended, with a strict-< depth test (nearer wins).
    const int w = fb.width();
    const int h = fb.height();
    const auto plot = [&](int px, int py, float z)
    {
        if (px >= 0 && px < w && py >= 0 && py < h)
        {
            fb.commit_pixel(px, py, z, color);
        }
    };

    if (steps == 0)
    {
        plot(x0, y0, a.z);
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
        plot(static_cast<int>(std::round(x)), static_cast<int>(std::round(y)), z);
        x += sx;
        y += sy;
        z += sz;
    }
}

// Rasterize a triangle using screen-space barycentric coordinates.
// sa/sb/sc hold (screen_x, screen_y, ndc_z).
// wa/wb/wc are clip-space w values for perspective-correct interpolation.
// col_a/b/c are per-vertex base colours (uniform for Flat lighting; per-vertex for the
// unlit path).
// uva/uvb/uvc are per-vertex texture coordinates.
// tex may be nullptr if no diffuse texture is active.

template <Sink S>
void rasterize_flat(
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
    vec2 uva,
    vec2 uvb,
    vec2 uvc,
    const Texture *tex,
    float alpha_cutoff,
    int y_min,
    int y_max,
    const Texture *etex,
    vec3 emissive,
    vec2 uv1a,
    vec2 uv1b,
    vec2 uv1c,
    const Material *mat,
    const ABuffer *abuf,
    float base_alpha,
    float caa,
    float cab,
    float cac
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
    // Tonemap the lit output (Flat shading), but not the unlit path: unlit reuses this
    // rasterizer to emit baseColour*texture directly, which is bounded [0,1] and meant to be
    // faithful, so the rolloff would only dim it. Loop-invariant, hoisted above the y-loop.
    const bool do_tonemap = !(mat && mat->unlit);
    const auto stride = static_cast<size_t>(fb.width());

    // Per-slot UV set (glTF TEXCOORD_n). diffuse and emissive are the only textures this
    // (Flat/unlit) rasterizer samples; uv_set comes from mat (null ⇒ set 0, e.g. tests
    // and non-glTF). need_uv1 gates the second perspective-correct interpolation, so when false
    // the per-pixel uv1v compute is skipped entirely; the residue on the no-uv1 path is one
    // loop-invariant-conditioned select (`set ? uv1v : uv`) per sampled texture, which benches in
    // the noise (full-PBR Phong is ~0%). Loop-invariant flags — hoisted above the y-loop.
    const uint8_t diffuse_set = mat ? mat->diffuse_map.uv_set : uint8_t{ 0 };
    const uint8_t emissive_set = mat ? mat->emissive_map.uv_set : uint8_t{ 0 };
    const bool need_uv1 =
        ((tex != nullptr) && diffuse_set != 0) || (do_emissive && (etex != nullptr) && emissive_set != 0);
    // KHR_texture_transform (per-slot, gates the post-select affine; null mat ⇒ none). When no
    // slot carries one the residue is a hoisted bool + a predicted not-taken branch per sampler.
    const bool diffuse_xf = mat && mat->diffuse_map.has_transform;
    const bool emissive_xf = mat && mat->emissive_map.has_transform;

    // Transparent: per-edge top-left flags so a pixel center landing exactly on a shared edge
    // is owned by one triangle (no double-composited seam). The barycentric gradients are
    // winding-normalized (always point toward the interior), so the classification is
    // winding-independent. bc's gradient is -(grad ba + grad bb). Compiles out for Opaque.
    // NOTE: this is the float-barycentric approximation of a top-left rule, not an exact one.
    // Barycentrics are accumulated by repeated += per pixel, so an exact == 0.0f on an edge
    // almost never occurs; ownership effectively rests on the strict > 0 tests. Where rounding
    // puts a shared-edge pixel tiny-negative for both neighbours you get a 1px gap, tiny-positive
    // for both a faint double-blend seam. Cosmetic and blend-only; an exact rule would need
    // fixed-point edge functions, which this incremental-float rasterizer does not use.
    [[maybe_unused]] const float bc_dx = -(ba_dx + bb_dx);
    [[maybe_unused]] const float bc_dy = -(ba_dy + bb_dy);
    [[maybe_unused]] const bool tl_a = (ba_dy > 0.0f) || (ba_dy == 0.0f && ba_dx > 0.0f);
    [[maybe_unused]] const bool tl_b = (bb_dy > 0.0f) || (bb_dy == 0.0f && bb_dx > 0.0f);
    [[maybe_unused]] const bool tl_c = (bc_dy > 0.0f) || (bc_dy == 0.0f && bc_dx > 0.0f);
    [[maybe_unused]] constexpr float ALPHA_EPS = 1.0f / 512.0f; // skip fragments too sheer to matter

    for (int y = s.y0; y <= s.y1; y++)
    {
        float ba = ba_row;
        float bb = bb_row;
        size_t idx = (static_cast<size_t>(y) * stride) + static_cast<size_t>(s.x0);
        for (int x = s.x0; x <= s.x1; ++x, ++idx)
        {
            if constexpr (S == Sink::Opaque)
            {
                if (ba < 0.0f || bb < 0.0f)
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
            }
            const float bc = 1.0f - ba - bb;
            if constexpr (S == Sink::Opaque)
            {
                if (bc < 0.0f)
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
            }
            else
            {
                const bool covered = (ba > 0.0f || (ba == 0.0f && tl_a)) && (bb > 0.0f || (bb == 0.0f && tl_b)) &&
                                     (bc > 0.0f || (bc == 0.0f && tl_c));
                if (!covered)
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
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
            vec2 uv{};   // TEXCOORD_0, shared by cutout pre-pass, diffuse sample, and emissive sample
            vec2 uv1v{}; // TEXCOORD_1, computed only when need_uv1; selected per sampler below
            if (has_cutout)
            {
                pwa = ba * inv_wa;
                pwb = bb * inv_wb;
                pwc = bc * inv_wc;
                w_corr = 1.0f / (pwa + pwb + pwc);
                uv = (uva * pwa + uvb * pwb + uvc * pwc) * w_corr;
                if (need_uv1)
                {
                    uv1v = (uv1a * pwa + uv1b * pwb + uv1c * pwc) * w_corr;
                }
                vec2 d_uv = diffuse_set ? uv1v : uv;
                if (diffuse_xf)
                {
                    d_uv = apply_tex_transform(mat->diffuse_map, d_uv);
                }
                const vec4 ta = tex->sample_rgba(d_uv.x, d_uv.y);
                if (ta.w < alpha_cutoff)
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
                cutout_rgb = { ta.x, ta.y, ta.z };
            }

            // Opaque: strict depth-CAS gate. Transparent: keep fragments at or in front of
            // the (final) opaque depth (<= so coplanar decals show); never writes depth.
            if constexpr (S == Sink::Opaque)
            {
                if (!fb.depth_test_relaxed(idx, depth))
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
            }
            else
            {
                if (depth > fb.depth_at(idx))
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
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
                if (need_uv1)
                {
                    uv1v = (uv1a * pwa + uv1b * pwb + uv1c * pwc) * w_corr;
                }
            }

            // Perspective-correct base colour. Flat passes three identical vertex colours
            // (so this is a no-op average); the unlit path passes distinct per-vertex colours.
            vec3 col = (col_a * pwa + col_b * pwb + col_c * pwc) * w_corr;

            if constexpr (S == Sink::Transparent)
            {
                // Diffuse via sample_rgba: .rgb tints, .w is the texture opacity.
                float tex_a = 1.0f;
                if (tex)
                {
                    vec2 d_uv = diffuse_set ? uv1v : uv;
                    if (diffuse_xf)
                    {
                        d_uv = apply_tex_transform(mat->diffuse_map, d_uv);
                    }
                    const vec4 t = tex->sample_rgba(d_uv.x, d_uv.y);
                    col = col * vec3{ t.x, t.y, t.z };
                    tex_a = t.w;
                }
                if (do_emissive)
                {
                    vec3 e = emissive;
                    if (etex)
                    {
                        vec2 e_uv = emissive_set ? uv1v : uv;
                        if (emissive_xf)
                        {
                            e_uv = apply_tex_transform(mat->emissive_map, e_uv);
                        }
                        e = e * etex->sample_rgb(e_uv.x, e_uv.y);
                    }
                    col = col + e;
                }
                // Fragment opacity = material base * texture * (perspective-correct) vertex alpha.
                // Tonemap to display space before the push so the resolve composites display-referred
                // values and never tonemaps again (its final clamp then only guards the negative case,
                // which lit colours never reach).
                const float vca = ((caa * pwa) + (cab * pwb) + (cac * pwc)) * w_corr;
                const float a = base_alpha * tex_a * vca;
                if (a >= ALPHA_EPS)
                {
                    abuf->push(idx, x, y, depth, do_tonemap ? tonemap(col) : col, a);
                }
            }
            else
            {
                if (tex)
                {
                    vec2 d_uv = diffuse_set ? uv1v : uv;
                    // When has_cutout the colour reuses cutout_rgb (already transformed in the
                    // pre-pass), so an affine here would be computed and discarded — skip it.
                    if (diffuse_xf && !has_cutout)
                    {
                        d_uv = apply_tex_transform(mat->diffuse_map, d_uv);
                    }
                    col = col * (has_cutout ? cutout_rgb : tex->sample_rgb(d_uv.x, d_uv.y));
                }

                // Emissive add applies after lighting so shaded areas still glow. The sum feeds the
                // soft-knee tonemap below, so a strong emissive on an already-bright surface rolls off
                // toward white instead of hard-clipping. KHR_materials_emissive_strength is baked into
                // the factor at load.
                if (do_emissive)
                {
                    vec3 e = emissive;
                    if (etex)
                    {
                        vec2 e_uv = emissive_set ? uv1v : uv;
                        if (emissive_xf)
                        {
                            e_uv = apply_tex_transform(mat->emissive_map, e_uv);
                        }
                        e = e * etex->sample_rgb(e_uv.x, e_uv.y);
                    }
                    col = col + e;
                }

                fb.commit_pixel(idx, depth, vec3_to_color(do_tonemap ? tonemap(col) : col));
            }

            ba += ba_dx;
            bb += bb_dx;
        }

        ba_row += ba_dy;
        bb_row += bb_dy;
    }
}

// Rasterize a triangle with per-pixel Blinn-Phong lighting (Phong shading).
// Perspective-correct interpolates world-space position and normal to each
// pixel, then evaluates compute_lighting() there.
// uva/uvb/uvc are per-vertex texture coordinates.
// tex may be nullptr if no diffuse texture is active; when present its RGB is
// multiplied into mat.diffuse before the lighting calculation.

template <Sink S>
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
    int y_min,
    int y_max,
    const Texture *mrtex,
    const Texture *etex,
    vec3 emissive,
    bool apply_normal_scale,
    const Texture *octex,
    float occlusion_strength,
    vec2 uv1a,
    vec2 uv1b,
    vec2 uv1c,
    const ABuffer *abuf,
    float caa,
    float cab,
    float cac
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
    // ORM packing (glTF): when occlusion and metallic-roughness reference the same image,
    // load_tex dedups them to one Texture* — sample once and reuse the AO read for metalness.
    // The addressing must also match: the cache key is image-only (ignores texCoord and
    // KHR_texture_transform), so two bindings can dedup to one Texture* yet differ in UV set or
    // transform, in which case the samples land at different texels and can't be shared.
    const bool occ_is_mr = (octex != nullptr && octex == mrtex && same_uv_mapping(mat.occlusion_map, mat.mr_map));
    // glTF: emissive = emissiveFactor * emissiveTexture.rgb. Factor {0,0,0} zeros the
    // contribution regardless of texture, so the texture sample is skippable too.
    // The factor is passed in so callers can override it (e.g. tests); the UI texture
    // toggle only controls whether etex is sampled (see show_emissive in renderer.cpp).
    const bool do_emissive = (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f);
    const auto stride = static_cast<size_t>(fb.width());

    // Per-slot UV set (glTF TEXCOORD_n), read from mat. need_uv1 gates the second perspective-
    // correct interpolation, so when false the per-pixel uv1v compute is skipped entirely; the
    // residue on the no-uv1 path is one loop-invariant-conditioned select (`set ? uv1v : uv`) per
    // sampled texture (up to ~5 here), which benches in the noise — full-PBR Phong (the max-select
    // case) measured ~0%. All loop-invariant — hoisted above the y-loop. specular_map is MTL-only
    // (always set 0) but read uniformly. occ_is_mr shares one sample, so it shares mr's set.
    const uint8_t diffuse_set = mat.diffuse_map.uv_set;
    const uint8_t normal_set = mat.normal_map.uv_set;
    const uint8_t specular_set = mat.specular_map.uv_set;
    const uint8_t mr_set = mat.mr_map.uv_set;
    const uint8_t emissive_set = mat.emissive_map.uv_set;
    const uint8_t occ_set = mat.occlusion_map.uv_set;
    const bool need_uv1 = ((tex != nullptr) && diffuse_set != 0) || ((nmap != nullptr) && normal_set != 0) ||
                          ((stex != nullptr) && specular_set != 0) || ((mrtex != nullptr) && mr_set != 0) ||
                          ((octex != nullptr) && occ_set != 0) ||
                          (do_emissive && (etex != nullptr) && emissive_set != 0);
    // KHR_texture_transform per slot: gates the post-select affine. Loop-invariant; the no-transform
    // residue is a hoisted bool + a predicted not-taken branch per sampler (same class as the uv1
    // selects). occ_is_mr shares mr's sample, so it shares mr's transform.
    const bool diffuse_xf = mat.diffuse_map.has_transform;
    const bool normal_xf = mat.normal_map.has_transform;
    const bool specular_xf = mat.specular_map.has_transform;
    const bool mr_xf = mat.mr_map.has_transform;
    const bool occ_xf = mat.occlusion_map.has_transform;
    const bool emissive_xf = mat.emissive_map.has_transform;

    // Transparent fill-rule flags (see rasterize_flat() for the rationale); compile out for Opaque.
    [[maybe_unused]] const float bc_dx = -(ba_dx + bb_dx);
    [[maybe_unused]] const float bc_dy = -(ba_dy + bb_dy);
    [[maybe_unused]] const bool tl_a = (ba_dy > 0.0f) || (ba_dy == 0.0f && ba_dx > 0.0f);
    [[maybe_unused]] const bool tl_b = (bb_dy > 0.0f) || (bb_dy == 0.0f && bb_dx > 0.0f);
    [[maybe_unused]] const bool tl_c = (bc_dy > 0.0f) || (bc_dy == 0.0f && bc_dx > 0.0f);
    [[maybe_unused]] constexpr float ALPHA_EPS = 1.0f / 512.0f;

    for (int y = s.y0; y <= s.y1; y++)
    {
        float ba = ba_row;
        float bb = bb_row;
        size_t idx = (static_cast<size_t>(y) * stride) + static_cast<size_t>(s.x0);
        for (int x = s.x0; x <= s.x1; ++x, ++idx)
        {
            if constexpr (S == Sink::Opaque)
            {
                if (ba < 0.0f || bb < 0.0f)
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
            }
            const float bc = 1.0f - ba - bb;
            if constexpr (S == Sink::Opaque)
            {
                if (bc < 0.0f)
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
            }
            else
            {
                const bool covered = (ba > 0.0f || (ba == 0.0f && tl_a)) && (bb > 0.0f || (bb == 0.0f && tl_b)) &&
                                     (bc > 0.0f || (bc == 0.0f && tl_c));
                if (!covered)
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
            }

            const float depth = (ba * sa.z) + (bb * sb.z) + (bc * sc.z);

            // Alpha cutout: sample before depth write so discarded pixels don't
            // claim z-buffer entries (otherwise transparent holes occlude geometry).
            float pwa = 0.0f;
            float pwb = 0.0f;
            float pwc = 0.0f;
            float w_corr = 1.0f;
            vec3 cutout_rgb;
            vec2 uv{};   // hoisted: TEXCOORD_0, shared by cutout pre-pass and nmap/stex/etc below
            vec2 uv1v{}; // TEXCOORD_1, computed only when need_uv1; selected per sampler below
            if (has_cutout)
            {
                pwa = ba * inv_wa;
                pwb = bb * inv_wb;
                pwc = bc * inv_wc;
                w_corr = 1.0f / (pwa + pwb + pwc);
                uv = (uva * pwa + uvb * pwb + uvc * pwc) * w_corr;
                if (need_uv1)
                {
                    uv1v = (uv1a * pwa + uv1b * pwb + uv1c * pwc) * w_corr;
                }
                vec2 d_uv = diffuse_set ? uv1v : uv;
                if (diffuse_xf)
                {
                    d_uv = apply_tex_transform(mat.diffuse_map, d_uv);
                }
                const vec4 ta = tex->sample_rgba(d_uv.x, d_uv.y);
                if (ta.w < mat.alpha_cutoff)
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
                cutout_rgb = { ta.x, ta.y, ta.z };
            }

            // Opaque: strict depth-CAS gate. Transparent: keep at/in front of opaque (<=),
            // never writing depth (see rasterize_flat()).
            if constexpr (S == Sink::Opaque)
            {
                if (!fb.depth_test_relaxed(idx, depth))
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
            }
            else
            {
                if (depth > fb.depth_at(idx))
                {
                    ba += ba_dx;
                    bb += bb_dx;
                    continue;
                }
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
            if (!has_cutout && (tex || nmap || stex || mrtex || octex || (do_emissive && etex)))
            {
                uv = (uva * pwa + uvb * pwb + uvc * pwc) * w_corr;
                if (need_uv1)
                {
                    uv1v = (uv1a * pwa + uv1b * pwb + uv1c * pwc) * w_corr;
                }
            }

            // Normal mapping: sample tangent-space normal, rotate into world space via TBN.
            if (nmap)
            {
                const vec3 tan = (tana * pwa + tanb * pwb + tanc * pwc) * w_corr;

                // Unpack normal map texel from [0,1] to [-1,1].
                vec2 n_uv = normal_set ? uv1v : uv;
                if (normal_xf)
                {
                    n_uv = apply_tex_transform(mat.normal_map, n_uv);
                }
                vec3 nm = nmap->sample_rgb(n_uv.x, n_uv.y) * 2.0f - vec3{ 1.0f, 1.0f, 1.0f };
                // glTF normalScale: scales X/Y of the tangent-space normal (Z unchanged) before TBN.
                // apply_normal_scale is loop-invariant; mesh-level gate keeps default-scale paths free.
                if (apply_normal_scale)
                {
                    const float ns = mat.normal_scale;
                    nm.x *= ns;
                    nm.y *= ns;
                }

                // Re-orthogonalize T against the interpolated N (Gram-Schmidt),
                // then derive B so TBN is a proper orthonormal basis.
                const vec3 N = normalize(normal);
                const vec3 T = normalize(tan - N * dot(N, tan));
                const vec3 B = cross(N, T);

                // Transform tangent-space normal to world space.
                normal = T * nm.x + B * nm.y + N * nm.z;
                // normal will be normalized inside compute_lighting.
            }

            // Authored occlusion (glTF) overrides the baked vertex AO per-pixel — both target the
            // same scale, so multiplying would double-darken. octex is loop-invariant: free when null.
            float ao = ((aoa * pwa) + (aob * pwb) + (aoc * pwc)) * w_corr;
            vec3 occ_sample{}; // valid only when octex != nullptr; reused as the MR sample when occ_is_mr
            if (octex)
            {
                vec2 o_uv = occ_set ? uv1v : uv;
                if (occ_xf)
                {
                    o_uv = apply_tex_transform(mat.occlusion_map, o_uv);
                }
                occ_sample = octex->sample_rgb(o_uv.x, o_uv.y);
                ao = 1.0f + (occlusion_strength * (occ_sample.x - 1.0f));
            }

            // View vector for the specular term.
            const vec3 v = normalize(eye - pos);

            // Shading-params locals: avoid the ~92 B per-pixel Material copy by feeding
            // only the four fields lighting consumes into the compute_lighting overload.
            vec3 use_diffuse = mat.diffuse;
            vec3 use_ambient = mat.ambient;
            vec3 use_specular = mat.specular;
            float use_shin = mat.shininess;

            // Diffuse sample. Opaque keeps the original plain-vec3 path (structurally identical
            // codegen — no vec4, no round-trip). Transparent additionally carries the texture
            // alpha to the finalize push. NOLINT: tex_a is mutated only in the Transparent
            // instantiation, so const-correctness flags it in the Opaque one where that branch is
            // compiled out; [[maybe_unused]] covers the matching unused read there.
            [[maybe_unused]] float tex_a = 1.0f; // NOLINT(misc-const-correctness)
            if (tex)
            {
                vec2 d_uv = diffuse_set ? uv1v : uv;
                // Transparent always samples here; Opaque reuses cutout_rgb when has_cutout (already
                // transformed in the pre-pass), discarding any affine. has_cutout is false in the
                // Transparent sink (MASK and BLEND are mutually exclusive), so this still transforms there.
                if (diffuse_xf && !has_cutout)
                {
                    d_uv = apply_tex_transform(mat.diffuse_map, d_uv);
                }
                if constexpr (S == Sink::Transparent)
                {
                    const vec4 t = tex->sample_rgba(d_uv.x, d_uv.y);
                    const vec3 tc{ t.x, t.y, t.z };
                    use_diffuse = use_diffuse * tc;
                    use_ambient = use_ambient * tc;
                    tex_a = t.w;
                }
                else
                {
                    // Reuse the rgba sample from the cutout pre-pass when active.
                    const vec3 tc = has_cutout ? cutout_rgb : tex->sample_rgb(d_uv.x, d_uv.y);
                    use_diffuse = use_diffuse * tc;
                    use_ambient = use_ambient * tc;
                }
            }
            if (stex)
            {
                vec2 s_uv = specular_set ? uv1v : uv;
                if (specular_xf)
                {
                    s_uv = apply_tex_transform(mat.specular_map, s_uv);
                }
                use_specular = use_specular * stex->sample_rgb(s_uv.x, s_uv.y);
            }

            // Vertex color tint: skip entirely when all vertices are white (common case).
            if (has_vcol)
            {
                const vec3 vcol = (vcola * pwa + vcolb * pwb + vcolc * pwc) * w_corr;
                use_diffuse = use_diffuse * vcol;
                use_ambient = use_ambient * vcol;
            }

            // Metals tint specular reflectance (F0) toward their base colour (already in
            // use_diffuse); dielectrics keep the 4% baseline. Diffuse is deliberately NOT
            // zeroed as strict PBR would: with no environment map a diffuse-less metal has
            // nothing to reflect and renders near-black.
            if (is_metallic)
            {
                const vec3 base = use_diffuse;
                float metalness = mat.metallic;
                if (mrtex)
                {
                    // G=roughness, B=metallic. Reuse the occlusion sample when ORM-packed (same image);
                    // occ_is_mr implies an identical transform (same_uv_mapping), so skip m_uv's affine
                    // when reusing — it would be discarded.
                    vec2 m_uv = mr_set ? uv1v : uv;
                    if (mr_xf && !occ_is_mr)
                    {
                        m_uv = apply_tex_transform(mat.mr_map, m_uv);
                    }
                    const vec3 mr = occ_is_mr ? occ_sample : mrtex->sample_rgb(m_uv.x, m_uv.y);
                    metalness *= mr.z;
                    use_shin = roughness_to_shininess(mat.roughness * mr.y);
                }
                use_specular = lerp(DIELECTRIC_F0, base, metalness);
            }

            vec3 color = compute_lighting(
                normal, v, lights, n_lights, ambient, use_diffuse, use_ambient, use_specular, use_shin, ao
            );

            // Emissive add applies after lighting so shaded areas still glow. The sum feeds the
            // soft-knee tonemap below, so a strong emissive on an already-bright surface rolls off
            // toward white instead of hard-clipping. KHR_materials_emissive_strength is baked into
            // the factor at load.
            if (do_emissive)
            {
                vec3 e = emissive;
                if (etex)
                {
                    vec2 e_uv = emissive_set ? uv1v : uv;
                    if (emissive_xf)
                    {
                        e_uv = apply_tex_transform(mat.emissive_map, e_uv);
                    }
                    e = e * etex->sample_rgb(e_uv.x, e_uv.y);
                }
                color = color + e;
            }

            // Phong is always a lit path (unlit is dispatched to rasterize_flat before reaching here),
            // so the tonemap is unconditional.
            if constexpr (S == Sink::Transparent)
            {
                // Opacity = material base * texture * (perspective-correct) vertex alpha. Tonemap to
                // display space before the push so the resolve composites display-referred values and
                // never tonemaps again (its final clamp then only guards the negative case, which lit
                // colours never reach).
                const float vca = ((caa * pwa) + (cab * pwb) + (cac * pwc)) * w_corr;
                const float a = mat.alpha * tex_a * vca;
                if (a >= ALPHA_EPS)
                {
                    abuf->push(idx, x, y, depth, tonemap(color), a);
                }
            }
            else
            {
                fb.commit_pixel(idx, depth, vec3_to_color(tonemap(color)));
            }

            ba += ba_dx;
            bb += bb_dx;
        }

        ba_row += ba_dy;
        bb_row += bb_dy;
    }
}

// Explicit template instantiation
// The rasterizer definitions live in this TU; callers in renderer.cpp (and the
// tests) need both Sink instantiations at link time. Emit them explicitly here.

template void rasterize_flat<Sink::Opaque>(
    Framebuffer &,
    vec3,
    vec3,
    vec3,
    float,
    float,
    float,
    vec3,
    vec3,
    vec3,
    vec2,
    vec2,
    vec2,
    const Texture *,
    float,
    int,
    int,
    const Texture *,
    vec3,
    vec2,
    vec2,
    vec2,
    const Material *,
    const ABuffer *,
    float,
    float,
    float,
    float
);
template void rasterize_flat<Sink::Transparent>(
    Framebuffer &,
    vec3,
    vec3,
    vec3,
    float,
    float,
    float,
    vec3,
    vec3,
    vec3,
    vec2,
    vec2,
    vec2,
    const Texture *,
    float,
    int,
    int,
    const Texture *,
    vec3,
    vec2,
    vec2,
    vec2,
    const Material *,
    const ABuffer *,
    float,
    float,
    float,
    float
);

template void rasterize_phong<Sink::Opaque>(
    Framebuffer &,
    vec3,
    vec3,
    vec3,
    float,
    float,
    float,
    vec3,
    vec3,
    vec3,
    vec3,
    vec3,
    vec3,
    vec3,
    vec3,
    vec3,
    vec2,
    vec2,
    vec2,
    float,
    float,
    float,
    vec3,
    vec3,
    vec3,
    bool,
    const vec3 &,
    const Light *,
    int,
    const vec3 &,
    const Material &,
    const Texture *,
    const Texture *,
    const Texture *,
    int,
    int,
    const Texture *,
    const Texture *,
    vec3,
    bool,
    const Texture *,
    float,
    vec2,
    vec2,
    vec2,
    const ABuffer *,
    float,
    float,
    float
);
template void rasterize_phong<Sink::Transparent>(
    Framebuffer &,
    vec3,
    vec3,
    vec3,
    float,
    float,
    float,
    vec3,
    vec3,
    vec3,
    vec3,
    vec3,
    vec3,
    vec3,
    vec3,
    vec3,
    vec2,
    vec2,
    vec2,
    float,
    float,
    float,
    vec3,
    vec3,
    vec3,
    bool,
    const vec3 &,
    const Light *,
    int,
    const vec3 &,
    const Material &,
    const Texture *,
    const Texture *,
    const Texture *,
    int,
    int,
    const Texture *,
    const Texture *,
    vec3,
    bool,
    const Texture *,
    float,
    vec2,
    vec2,
    vec2,
    const ABuffer *,
    float,
    float,
    float
);
