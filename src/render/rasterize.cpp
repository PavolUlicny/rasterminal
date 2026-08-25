#include "src/render/rasterize.h"
#include "src/math/fastmath.h"
#include "src/math/light.h"
#include "src/math/linalg.h"
#include "src/render/texture.h"
#include "src/terminal/color.h"
#include "src/terminal/framebuffer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace
{

    constexpr float DEGEN_AREA_EPS = 1e-6f; // minimum |denom| to treat a triangle as non-degenerate

    // Standard dielectric normal-incidence reflectance.
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

    // Set up barycentrics inside an inclusive pixel window. clip_near bounds screen
    // coordinates before these float-to-int conversions.
    bool setup_tri(
        vec3 sa, vec3 sb, vec3 sc, float wa, float wb, float wc, int x_min, int x_max, int y_min, int y_max, TriSetup &s
    )
    {
        s.x0 = std::max(x_min, static_cast<int>(std::floor(std::min({ sa.x, sb.x, sc.x }))));
        s.x1 = std::min(x_max, static_cast<int>(std::ceil(std::max({ sa.x, sb.x, sc.x }))));
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

    // Conservatively narrow a bbox row by edge zero crossings. eps expands each
    // crossing by its floating-point uncertainty, especially for near-horizontal edges.
    inline bool row_span(
        float ba_row,
        float bb_row,
        float ba_dx,
        float bb_dx,
        const float inv_dx[3],
        const float eps[3],
        int x0,
        int x1,
        int &lo,
        int &hi
    )
    {
        const float bc_row = 1.0f - ba_row - bb_row;
        const float bc_dx = -(ba_dx + bb_dx);
        const float tmax = static_cast<float>(x1 - x0) + 2.0f;
        float flo = -2.0f;
        float fhi = tmax;
        // Solve row + t*dx = -e for t, the loosest crossing consistent with the error bound.
        const auto edge = [&](float row, float dx, float inv, float e) -> bool
        {
            const float cross = -(row + e) * inv;
            if (dx > 0.0f)
            {
                flo = std::max(flo, std::min(cross, tmax));
            }
            else if (dx < 0.0f)
            {
                fhi = std::min(fhi, std::max(cross, -2.0f));
            }
            else if (row < -e)
            {
                return false;
            }
            return true;
        };
        if (!edge(ba_row, ba_dx, inv_dx[0], eps[0]) || !edge(bb_row, bb_dx, inv_dx[1], eps[1]) ||
            !edge(bc_row, bc_dx, inv_dx[2], eps[2]))
        {
            return false;
        }
        lo = std::max(x0, x0 + static_cast<int>(std::floor(flo)) - 1);
        hi = std::min(x1, x0 + static_cast<int>(std::ceil(fhi)) + 1);
        return lo <= hi;
    }

    // Reciprocal edge gradients for row_span; 0 marks an x-parallel edge.
    inline void span_inv_dx(const TriSetup &s, float inv_dx[3])
    {
        const float bc_dx = -(s.ba_dx + s.bb_dx);
        inv_dx[0] = s.ba_dx != 0.0f ? 1.0f / s.ba_dx : 0.0f;
        inv_dx[1] = s.bb_dx != 0.0f ? 1.0f / s.bb_dx : 0.0f;
        inv_dx[2] = bc_dx != 0.0f ? 1.0f / bc_dx : 0.0f;
    }

    // Eight-ulp bounds for edge values across one bbox row.
    inline void span_eps(const TriSetup &s, float ba_row, float bb_row, float eps[3])
    {
        constexpr float ULPS = 8.0f * 1.1920929e-7f;
        const auto width = static_cast<float>(s.x1 - s.x0);
        eps[0] = ULPS * (std::fabs(ba_row) + (width * std::fabs(s.ba_dx)));
        eps[1] = ULPS * (std::fabs(bb_row) + (width * std::fabs(s.bb_dx)));
        eps[2] = eps[0] + eps[1] + ULPS;
    }

    constexpr int SPAN_MIN_WIDTH = 8; // bbox rows narrower than this scan every pixel

    // Evaluate barycentrics from pixel coordinates instead of accumulating them.
    // Accumulation drift moves near-horizontal edges and makes coverage start-dependent.

} // namespace

// Clip before perspective division. near_w must match Camera::near_plane.

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
        // a inside; b and c outside: one clipped triangle.
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
    // a and b inside; c outside: a clipped quad split into two triangles.
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

    // Atomically test depth and commit depth+color so workers may draw concurrently.
    // The rasterizer owns viewport checks; commit_pixel uses strict < and an
    // uncontended caller completes one CAS iteration.
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

// Rasterize Flat or unlit color with perspective-correct screen-space barycentrics.

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
    float cac,
    const TileVis *vis
)
{
    // Deferred rasterizes within its tile; the other sinks span the framebuffer width and the
    // caller's row band.
    const int x_min = (S == Sink::Deferred) ? vis->x_min : 0;
    const int x_max = (S == Sink::Deferred) ? vis->x_max : fb.width() - 1;
    if constexpr (S == Sink::Deferred)
    {
        y_min = vis->y_min;
        y_max = vis->y_max;
    }
    TriSetup s; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): setup_tri writes all fields before
                // they are read
    if (!setup_tri(sa, sb, sc, wa, wb, wc, x_min, x_max, y_min, y_max, s))
    {
        return;
    }

    if constexpr (S == Sink::Transparent)
    {
        // The clamped bounding box, once, before any of this triangle's fragments is published:
        // that ordering is what ABuffer::widen requires, and why it is not per fragment.
        abuf->widen(s.x0, s.x1, s.y0, s.y1);
    }

    const float inv_wa = s.inv_wa;
    const float inv_wb = s.inv_wb;
    const float inv_wc = s.inv_wc;
    const float ba_dx = s.ba_dx;
    const float ba_dy = s.ba_dy;
    const float bb_dx = s.bb_dx;
    const float bb_dy = s.bb_dy;
    const float ba_row0 = s.ba_row;
    const float bb_row0 = s.bb_row;

    // Deferred never cuts out here: the visibility pass already dropped the pixels below the
    // cutoff, so the shade pass samples the plain RGB (identical to sample_rgba's RGB).
    const bool has_cutout = (S != Sink::Deferred) && (alpha_cutoff > 0.0f && tex);
    // glTF: emissive = emissiveFactor * emissiveTexture.rgb. Factor {0,0,0} zeros the
    // contribution regardless of texture, so the texture sample is skippable too.
    const bool do_emissive = (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f);
    // Tonemap the lit output (Flat shading), but not the unlit path: unlit reuses this
    // rasterizer to emit baseColour*texture directly, which is bounded [0,1] and meant to be
    // faithful, so the rolloff would only dim it. Loop-invariant, hoisted above the y-loop.
    const bool do_tonemap = !(mat && mat->unlit);
    const auto stride = static_cast<size_t>(fb.width());

    // Interpolate UV1 only when a sampled flat-path slot requests it.
    const uint8_t diffuse_set = mat ? mat->diffuse_map.uv_set : uint8_t{ 0 };
    const uint8_t emissive_set = mat ? mat->emissive_map.uv_set : uint8_t{ 0 };
    const bool need_uv1 =
        ((tex != nullptr) && diffuse_set != 0) || (do_emissive && (etex != nullptr) && emissive_set != 0);
    // KHR_texture_transform is per slot; a null material has none. When no
    // slot carries one the residue is a hoisted bool + a predicted not-taken branch per sampler.
    const bool diffuse_xf = mat && mat->diffuse_map.has_transform;
    const bool emissive_xf = mat && mat->emissive_map.has_transform;

    // Approximate top-left ownership for transparent shared edges. Float rounding can
    // still leave a cosmetic one-pixel gap or double-blend seam.
    [[maybe_unused]] const float bc_dx = -(ba_dx + bb_dx);
    [[maybe_unused]] const float bc_dy = -(ba_dy + bb_dy);
    [[maybe_unused]] const bool tl_a = (ba_dy > 0.0f) || (ba_dy == 0.0f && ba_dx > 0.0f);
    [[maybe_unused]] const bool tl_b = (bb_dy > 0.0f) || (bb_dy == 0.0f && bb_dx > 0.0f);
    [[maybe_unused]] const bool tl_c = (bc_dy > 0.0f) || (bc_dy == 0.0f && bc_dx > 0.0f);
    [[maybe_unused]] constexpr float ALPHA_EPS = 1.0f / 512.0f; // skip fragments too sheer to matter

    // Deferred also uses row spans. Whole-pixel padding covers setup FMA drift between
    // visibility and shading; retain it if span tolerances change.
    const bool use_span = (s.x1 - s.x0) >= SPAN_MIN_WIDTH;
    float inv_dx[3] = { 0.0f, 0.0f, 0.0f };
    if (use_span)
    {
        span_inv_dx(s, inv_dx);
    }

    // Narrow immediate walks to pixel-centre vertex spans. Keep this outside setup_tri:
    // moving it changes FMA contraction and edge rounding across all instantiations.
    // Deferred coverage comes from visibility ids and must retain the full bounds.
    const bool narrow_walk = (S != Sink::Deferred);
    int walk_x0 = s.x0;
    int walk_x1 = s.x1;
    int walk_y0 = s.y0;
    int walk_y1 = s.y1;
    if (narrow_walk)
    {
        int sp0 = 0;
        int sp1 = 0;
        if (pixel_span(std::min({ sa.x, sb.x, sc.x }), std::max({ sa.x, sb.x, sc.x }), s.x1 + 1, sp0, sp1))
        {
            walk_x0 = std::max(walk_x0, sp0);
            walk_x1 = std::min(walk_x1, sp1);
        }
        if (pixel_span(std::min({ sa.y, sb.y, sc.y }), std::max({ sa.y, sb.y, sc.y }), s.y1 + 1, sp0, sp1))
        {
            walk_y0 = std::max(walk_y0, sp0);
            walk_y1 = std::min(walk_y1, sp1);
        }
    }

    // Evaluate edge values instead of accumulating them. Float ry is exact for row
    // indices and stays relative to barycentric anchor s.y0, so narrowing the walk
    // cannot change a pixel's value.
    auto ry = static_cast<float>(walk_y0 - s.y0);
    for (int y = walk_y0; y <= walk_y1; y++, ry += 1.0f)
    {
        const float ba_row = ba_row0 + (ry * ba_dy);
        const float bb_row = bb_row0 + (ry * bb_dy);
        int x_lo = walk_x0;
        int x_hi = walk_x1;
        if (use_span)
        {
            float eps[3];
            span_eps(s, ba_row, bb_row, eps);
            // row_span's t is measured from s.x0, where ba_row is anchored, so it keeps the box
            // bounds; its answer is then clamped into the walk.
            if (!row_span(ba_row, bb_row, ba_dx, bb_dx, inv_dx, eps, s.x0, s.x1, x_lo, x_hi))
            {
                continue;
            }
            x_lo = std::max(x_lo, walk_x0);
            x_hi = std::min(x_hi, walk_x1);
            if (x_lo > x_hi)
            {
                continue;
            }
        }
        size_t idx = (static_cast<size_t>(y) * stride) + static_cast<size_t>(x_lo);
        // Deferred: offset of this row in the tile id buffer, indexed by absolute x.
        [[maybe_unused]] const ptrdiff_t vis_row =
            (S == Sink::Deferred) ? (static_cast<ptrdiff_t>(y - vis->y_min) * vis->stride) - vis->x_min : 0;
        auto rx = static_cast<float>(x_lo - s.x0);
        for (int x = x_lo; x <= x_hi; ++x, ++idx, rx += 1.0f)
        {
            const float ba = ba_row + (rx * ba_dx);
            const float bb = bb_row + (rx * bb_dx);
            // Deferred takes coverage from the visibility pass's ids alone (see rasterize_phong).
            const float bc = 1.0f - ba - bb;
            if constexpr (S == Sink::Opaque)
            {
                // One branch for the three edge signs, as in rasterize_phong (see the note there).
                const unsigned outside = static_cast<unsigned>(ba < 0.0f) | static_cast<unsigned>(bb < 0.0f) |
                                         static_cast<unsigned>(bc < 0.0f);
                if (outside != 0u)
                {
                    continue;
                }
            }
            else if constexpr (S == Sink::Transparent)
            {
                const bool covered = (ba > 0.0f || (ba == 0.0f && tl_a)) && (bb > 0.0f || (bb == 0.0f && tl_b)) &&
                                     (bc > 0.0f || (bc == 0.0f && tl_c));
                if (!covered)
                {
                    continue;
                }
            }

            // z_ndc is linear in screen space (projection makes it A + B/z_view,
            // which is linear in NDC x/y), so plain barycentric is correct here;
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
                    continue;
                }
            }
            else if constexpr (S == Sink::Deferred)
            {
                if (vis->ids[vis_row + x] != vis->id)
                {
                    continue;
                }
            }
            else
            {
                if (depth > fb.depth_at(idx))
                {
                    continue;
                }
            }

            // Perspective-correct weights, computed once, reused for all attributes.
            if (!has_cutout)
            {
                pwa = ba * inv_wa;
                pwb = bb * inv_wb;
                pwc = bc * inv_wc;
                w_corr = 1.0f / (pwa + pwb + pwc);
            }

            // Compute UV only for a sampled diffuse or emissive texture, and reuse the
            // cutout pre-pass result. A zero emissive factor skips its bound texture.
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
                // Opacity combines material, texture, and perspective-correct vertex alpha.
                // Tonemap before the push so resolve composites display-space values and
                // only needs its final safety clamp.
                const float vca = ((caa * pwa) + (cab * pwb) + (cac * pwc)) * w_corr;
                const float a = base_alpha * tex_a * vca;
                if (a >= ALPHA_EPS)
                {
                    abuf->push(idx, depth, do_tonemap ? tonemap(col) : col, a);
                }
            }
            else
            {
                if (tex)
                {
                    vec2 d_uv = diffuse_set ? uv1v : uv;
                    // When has_cutout the colour reuses cutout_rgb (already transformed in the
                    // pre-pass), so an affine here would be computed and discarded: skip it.
                    if (diffuse_xf && !has_cutout)
                    {
                        d_uv = apply_tex_transform(mat->diffuse_map, d_uv);
                    }
                    col = col * (has_cutout ? cutout_rgb : tex->sample_rgb(d_uv.x, d_uv.y));
                }

                // Add emissive after lighting, then soft-knee tonemap the sum instead of
                // hard-clipping bright surfaces. Loading bakes emissive strength into the factor.
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

                if constexpr (S == Sink::Deferred)
                {
                    fb.set_pixel_at(idx, depth, vec3_to_color(do_tonemap ? tonemap(col) : col));
                }
                else
                {
                    fb.commit_pixel(idx, depth, vec3_to_color(do_tonemap ? tonemap(col) : col));
                }
            }
        }
    }
}

// Gather covered pixels, then shade SoA batches so arithmetic vectorizes without
// keeping every interpolated attribute live in one scalar loop.

namespace
{

    constexpr int BATCH = Texture::BATCH_MAX; // pixels shaded per batch; the SoA arrays stay L1-resident

    struct PhongBatch // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): scratch arrays, each
                      // stage writes the entries [0, n) it needs before any reads them; zeroing 10 KB per batch would
                      // cost more than the shading of a small one
    {
        int n = 0;
        int x[BATCH];
        int y[BATCH];
        float ba[BATCH];
        float bb[BATCH];
        float bc[BATCH];
        float depth[BATCH];
        float pwa[BATCH];
        float pwb[BATCH];
        float pwc[BATCH];
        float wcorr[BATCH];
        float px[BATCH];
        float py[BATCH];
        float pz[BATCH];
        float nx[BATCH];
        float ny[BATCH];
        float nz[BATCH];
        float u[BATCH];
        float v[BATCH];
        float u1[BATCH];
        float v1[BATCH];
        float ao[BATCH];
        // Diffuse RGBA. Deferred cutout happened in visibility; transparent cutout
        // uses the immediate path, so batched shading always writes these together.
        float tr[BATCH];
        float tg[BATCH];
        float tb[BATCH];
        float ta[BATCH];
        // Working colour: material diffuse * texture * vertex colour, and the specular tint.
        float dr[BATCH];
        float dg[BATCH];
        float db[BATCH];
        float sr[BATCH];
        float sg[BATCH];
        float sb[BATCH];
        float shin[BATCH];
        // Result.
        float cr[BATCH];
        float cg[BATCH];
        float cb[BATCH];
        float alpha[BATCH]; // Transparent only
    };

    // Everything shade_batch needs beyond the batch: the per-triangle constants of rasterize_phong.
    struct PhongCtx // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): rasterize_phong assigns every
                    // field before the first shade_batch call
    {
        Framebuffer *fb;
        size_t stride;
        vec3 pa, pb, pc, na, nb, nc, tana, tanb, tanc;
        vec2 uva, uvb, uvc, uv1a, uv1b, uv1c;
        float aoa, aob, aoc;
        vec3 vcola, vcolb, vcolc;
        float caa, cab, cac;
        float inv_wa, inv_wb, inv_wc;
        // Deferred: barycentric plane (value at pixel (x0, y0) and gradients) and vertex depths, so
        // shade_batch rebuilds ba/bb/bc/depth from (x, y) instead of the gather storing them.
        float ba0, bb0, ba_dx, ba_dy, bb_dx, bb_dy;
        int x0, y0;
        float za, zb, zc;
        vec3 eye;
        const Light *lights;
        int n_lights;
        vec3 ambient;
        const Material *mat;
        const Texture *tex, *nmap, *stex, *mrtex, *etex, *octex;
        vec3 emissive;
        float occlusion_strength;
        const ABuffer *abuf;
        // Loop-invariant feature flags (see rasterize_phong for their derivations).
        bool has_vcol, has_cutout, is_metallic, occ_is_mr, do_emissive, need_uv1, apply_normal_scale;
        uint8_t diffuse_set, normal_set, specular_set, mr_set, emissive_set, occ_set;
        bool diffuse_xf, normal_xf, specular_xf, mr_xf, occ_xf, emissive_xf;
    };

    // Per-slot UV selection + KHR_texture_transform for one sampled texture: writes the feed UV
    // of every batch pixel into ou/ov.
    inline void select_uv(const PhongBatch &b, uint8_t set, bool xf, const TexSlot &slot, float *ou, float *ov)
    {
        const float *su = set ? b.u1 : b.u;
        const float *sv = set ? b.v1 : b.v;
        if (xf)
        {
            for (int i = 0; i < b.n; i++)
            {
                const vec2 t = apply_tex_transform(slot, { su[i], sv[i] });
                ou[i] = t.x;
                ov[i] = t.y;
            }
        }
        else
        {
            for (int i = 0; i < b.n; i++)
            {
                ou[i] = su[i];
                ov[i] = sv[i];
            }
        }
    }

    // Branch-free form of tonemap_channel (framebuffer.h) with the polynomial exp, for the batch
    // shader's vectorized rolloff loop; same knee and range, so the curve is the same.
    constexpr float TONEMAP_KNEE = 0.7f;
    inline float tonemap_soft(float x) noexcept
    {
        constexpr float range = 1.0f - TONEMAP_KNEE;
        const float soft = TONEMAP_KNEE + (range * (1.0f - fast_exp(-(x - TONEMAP_KNEE) / range)));
        return x <= TONEMAP_KNEE ? x : soft;
    }

    // Branch-free unit-length normalize over SoA components (the select replaces normalize()'s
    // zero-length early return, so the loops it sits in still vectorize).
    inline void normalize_soa(float &x, float &y, float &z) noexcept
    {
        const float len_sq = (x * x) + (y * y) + (z * z);
        // Zero-length in -> zero out, as normalize(): a 0/1 factor rather than a conditional,
        // which GCC would not if-convert here (see fast_exp2's clamp).
        const float inv = static_cast<float>(len_sq >= 1e-16f) / std::sqrt(std::max(len_sq, 1e-16f));
        x *= inv;
        y *= inv;
        z *= inv;
    }

    // Vectorizable one-light Blinn-Phong. SPEC chooses fixed squaring chains,
    // uniform polynomial power, or per-pixel metallic-roughness power.
    enum class SpecPow : uint8_t
    {
        Chain32,
        Chain16,
        Chain8,
        Uniform,
        PerPixel
    };

    template <SpecPow SPEC> void apply_light_batch(PhongBatch &b, const Light &light, float shin_uniform)
    {
        const vec3 l = light.direction;
        const vec3 lc = light.color;
        const int n = b.n;
        // (px,py,pz) hold the unit view vector at this stage (see shade_batch), so h = l + v.
        for (int i = 0; i < n; i++)
        {
            const float diff = (b.nx[i] * l.x) + (b.ny[i] * l.y) + (b.nz[i] * l.z);
            const float hx = l.x + b.px[i];
            const float hy = l.y + b.py[i];
            const float hz = l.z + b.pz[i];
            const float ndh = (b.nx[i] * hx) + (b.ny[i] * hy) + (b.nz[i] * hz);
            const float hh = (hx * hx) + (hy * hy) + (hz * hz);
            const float q = (ndh * ndh) / hh;
            float spec; // NOLINT(cppcoreguidelines-init-variables): every constexpr branch assigns
            if constexpr (SPEC == SpecPow::Chain32)
            {
                const float x4 = q * q;
                const float x8 = x4 * x4;
                const float x16 = x8 * x8;
                spec = x16 * x16;
            }
            else if constexpr (SPEC == SpecPow::Chain16)
            {
                const float x4 = q * q;
                const float x8 = x4 * x4;
                spec = x8 * x8;
            }
            else if constexpr (SPEC == SpecPow::Chain8)
            {
                const float x4 = q * q;
                spec = x4 * x4;
            }
            else
            {
                // q <= 0 must not reach the log (the select below zeroes the term anyway).
                const float sh = (SPEC == SpecPow::Uniform) ? shin_uniform : b.shin[i];
                spec = fast_exp2(sh * 0.5f * fast_log2(q > 1e-30f ? q : 1e-30f));
            }
            // Selects, not mask multiplies: when the half vector vanishes (light exactly opposite
            // the view) q is 0/0, so the squaring chains give NaN, and NaN * 0 is NaN.
            const float dw = diff >= 0.0f ? diff : 0.0f;
            const float sw = (diff >= 0.0f && ndh > 0.0f) ? spec : 0.0f;
            b.cr[i] += lc.x * ((b.dr[i] * dw) + (b.sr[i] * sw));
            b.cg[i] += lc.y * ((b.dg[i] * dw) + (b.sg[i] * sw));
            b.cb[i] += lc.z * ((b.db[i] * dw) + (b.sb[i] * sw));
        }
    }

    // Use a separate per_pixel flag instead of a shininess sentinel. MTL permits
    // negative, unclamped Ns values, while only the metallic-roughness path fills shin[].
    inline void apply_light_batch(PhongBatch &b, const Light &light, float shin_uniform, bool per_pixel)
    {
        if (per_pixel)
        {
            apply_light_batch<SpecPow::PerPixel>(b, light, shin_uniform);
        }
        else if (shin_uniform == 32.0f)
        {
            apply_light_batch<SpecPow::Chain32>(b, light, shin_uniform);
        }
        else if (shin_uniform == 16.0f)
        {
            apply_light_batch<SpecPow::Chain16>(b, light, shin_uniform);
        }
        else if (shin_uniform == 8.0f)
        {
            apply_light_batch<SpecPow::Chain8>(b, light, shin_uniform);
        }
        else
        {
            apply_light_batch<SpecPow::Uniform>(b, light, shin_uniform);
        }
    }

    // Shade and emit one gathered batch. Sink-independent through the lighting; only the last
    // stage differs (Opaque CAS commit, Deferred plain store, Transparent A-buffer push).
    template <Sink S> void shade_batch(const PhongCtx &c, PhongBatch &b)
    {
        const int n = b.n;
        const Material &mat = *c.mat;

        if constexpr (S == Sink::Deferred)
        {
            // Invariants copied to locals: a float read through the ctx reference inside a loop
            // that stores floats into the batch may alias as far as the compiler knows, and that
            // stops the loop vectorizing (same for every batch loop below).
            const int x0 = c.x0;
            const int y0 = c.y0;
            const float ba0 = c.ba0;
            const float bb0 = c.bb0;
            const float ba_dx = c.ba_dx;
            const float ba_dy = c.ba_dy;
            const float bb_dx = c.bb_dx;
            const float bb_dy = c.bb_dy;
            const float za = c.za;
            const float zb = c.zb;
            const float zc = c.zc;
            for (int i = 0; i < n; i++)
            {
                const auto fx = static_cast<float>(b.x[i] - x0);
                const auto fy = static_cast<float>(b.y[i] - y0);
                const float ba = ba0 + (fy * ba_dy) + (fx * ba_dx);
                const float bb = bb0 + (fy * bb_dy) + (fx * bb_dx);
                const float bc = 1.0f - ba - bb;
                b.ba[i] = ba;
                b.bb[i] = bb;
                b.bc[i] = bc;
                b.depth[i] = (ba * za) + (bb * zb) + (bc * zc);
            }
        }
        // Perspective-correct weights (once per pixel, reused by every attribute).
        const float inv_wa = c.inv_wa;
        const float inv_wb = c.inv_wb;
        const float inv_wc = c.inv_wc;
        for (int i = 0; i < n; i++)
        {
            const float pwa = b.ba[i] * inv_wa;
            const float pwb = b.bb[i] * inv_wb;
            const float pwc = b.bc[i] * inv_wc;
            b.pwa[i] = pwa;
            b.pwb[i] = pwb;
            b.pwc[i] = pwc;
            b.wcorr[i] = 1.0f / (pwa + pwb + pwc);
        }
        // Interpolate a scalar attribute over the batch (the (a*pwa + b*pwb + c*pwc) * w_corr form).
        const auto interp = [&](float av, float bv, float cv, float *out)
        {
            for (int i = 0; i < n; i++)
            {
                out[i] = ((av * b.pwa[i]) + (bv * b.pwb[i]) + (cv * b.pwc[i])) * b.wcorr[i];
            }
        };
        // UVs first: the texture footprints are located and prefetched right after, and the
        // remaining interpolations then run while those fetches are in flight.
        const bool any_tex = c.tex || c.nmap || c.stex || c.mrtex || c.octex || (c.do_emissive && c.etex);
        if (any_tex)
        {
            interp(c.uva.x, c.uvb.x, c.uvc.x, b.u);
            interp(c.uva.y, c.uvb.y, c.uvc.y, b.v);
            if (c.need_uv1)
            {
                interp(c.uv1a.x, c.uv1b.x, c.uv1c.x, b.u1);
                interp(c.uv1a.y, c.uv1b.y, c.uv1c.y, b.v1);
            }
        }

        // Locate and prefetch each active texture for the whole batch before sampling so
        // cache misses overlap. Leave footprint blocks uninitialized; locate_batch fills
        // every used [0, n) range, and zeroing six 2.5 KB blocks would dominate small batches.
        float fu[BATCH];
        float fv[BATCH];
        Texture::FootprintBatch fp_d; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
        Texture::FootprintBatch fp_n; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
        Texture::FootprintBatch fp_s; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
        Texture::FootprintBatch fp_m; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
        Texture::FootprintBatch fp_o; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
        Texture::FootprintBatch fp_e; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
        const auto locate =
            [&](const Texture *t, uint8_t set, bool xf, const TexSlot &slot, Texture::FootprintBatch &fp)
        {
            select_uv(b, set, xf, slot, fu, fv);
            t->locate_batch(fu, fv, n, fp);
            t->prefetch_batch(fp, n);
        };
        const bool sample_diffuse = c.tex && !c.has_cutout;
        const bool sample_mr = c.is_metallic && c.mrtex && !c.occ_is_mr;
        if (c.nmap)
        {
            locate(c.nmap, c.normal_set, c.normal_xf, mat.normal_map, fp_n);
        }
        if (c.octex)
        {
            locate(c.octex, c.occ_set, c.occ_xf, mat.occlusion_map, fp_o);
        }
        if (sample_diffuse)
        {
            locate(c.tex, c.diffuse_set, c.diffuse_xf, mat.diffuse_map, fp_d);
        }
        if (c.stex)
        {
            locate(c.stex, c.specular_set, c.specular_xf, mat.specular_map, fp_s);
        }
        if (sample_mr)
        {
            locate(c.mrtex, c.mr_set, c.mr_xf, mat.mr_map, fp_m);
        }
        if (c.do_emissive && c.etex)
        {
            locate(c.etex, c.emissive_set, c.emissive_xf, mat.emissive_map, fp_e);
        }

        interp(c.pa.x, c.pb.x, c.pc.x, b.px);
        interp(c.pa.y, c.pb.y, c.pc.y, b.py);
        interp(c.pa.z, c.pb.z, c.pc.z, b.pz);
        interp(c.na.x, c.nb.x, c.nc.x, b.nx);
        interp(c.na.y, c.nb.y, c.nc.y, b.ny);
        interp(c.na.z, c.nb.z, c.nc.z, b.nz);
        interp(c.aoa, c.aob, c.aoc, b.ao);

        // Normal mapping: sample the tangent-space normal, rotate into world space via TBN
        // (Gram-Schmidt T against the interpolated N, B = N x T).
        if (c.nmap)
        {
            float tx[BATCH];
            float ty[BATCH];
            float tz[BATCH];
            interp(c.tana.x, c.tanb.x, c.tanc.x, tx);
            interp(c.tana.y, c.tanb.y, c.tanc.y, ty);
            interp(c.tana.z, c.tanb.z, c.tanc.z, tz);
            float mx[BATCH];
            float my[BATCH];
            float mz[BATCH];
            float m_unused[BATCH];
            c.nmap->blend_batch(fp_n, n, mx, my, mz, m_unused);
            const float ns = c.apply_normal_scale ? mat.normal_scale : 1.0f;
            for (int i = 0; i < n; i++)
            {
                // Unpack [0,1] -> [-1,1]; glTF normalScale scales X/Y before TBN.
                const float nmx = ((mx[i] * 2.0f) - 1.0f) * ns;
                const float nmy = ((my[i] * 2.0f) - 1.0f) * ns;
                const float nmz = (mz[i] * 2.0f) - 1.0f;
                float Nx = b.nx[i];
                float Ny = b.ny[i];
                float Nz = b.nz[i];
                normalize_soa(Nx, Ny, Nz);
                const float nt = (Nx * tx[i]) + (Ny * ty[i]) + (Nz * tz[i]);
                float Tx = tx[i] - (Nx * nt);
                float Ty = ty[i] - (Ny * nt);
                float Tz = tz[i] - (Nz * nt);
                normalize_soa(Tx, Ty, Tz);
                const float Bx = (Ny * Tz) - (Nz * Ty);
                const float By = (Nz * Tx) - (Nx * Tz);
                const float Bz = (Nx * Ty) - (Ny * Tx);
                b.nx[i] = (Tx * nmx) + (Bx * nmy) + (Nx * nmz);
                b.ny[i] = (Ty * nmx) + (By * nmy) + (Ny * nmz);
                b.nz[i] = (Tz * nmx) + (Bz * nmy) + (Nz * nmz);
            }
        }

        // Authored occlusion (glTF, R channel) overrides the baked vertex AO per-pixel. G and B are
        // kept for the metallic stage, which reuses this sample when it doubles as the MR texture
        // (ORM packing).
        float occ_g[BATCH];
        float occ_b[BATCH];
        // Set with the arrays below; the metallic stage reads them back under occ_is_mr, which
        // cannot be true unless octex is, so they are always filled by the time it does.
        const float *occ_mr_g = nullptr;
        const float *occ_mr_b = nullptr;
        if (c.octex)
        {
            const float strength = c.occlusion_strength;
            float occ_r[BATCH];
            float occ_a[BATCH];
            c.octex->blend_batch(fp_o, n, occ_r, occ_g, occ_b, occ_a);
            for (int i = 0; i < n; i++)
            {
                b.ao[i] = 1.0f + (strength * (occ_r[i] - 1.0f));
            }
            occ_mr_g = occ_g;
            occ_mr_b = occ_b;
        }

        // Diffuse and ambient tint: material * diffuse texture * vertex colour. Ambient shares
        // the diffuse tint (mat.ambient * same factors), so only the tint product is kept.
        for (int i = 0; i < n; i++)
        {
            b.dr[i] = 1.0f;
            b.dg[i] = 1.0f;
            b.db[i] = 1.0f;
        }
        // sample_diffuse, not c.tex: the two differ only in the cutout case that batch_shade sends
        // down the immediate path instead, and gating the multiply on the same flag as the locate
        // and the blend keeps b.tr/tg/tb from being read before anything filled them.
        if (sample_diffuse)
        {
            c.tex->blend_batch(fp_d, n, b.tr, b.tg, b.tb, b.ta);
            for (int i = 0; i < n; i++)
            {
                b.dr[i] *= b.tr[i];
                b.dg[i] *= b.tg[i];
                b.db[i] *= b.tb[i];
            }
        }
        if (c.has_vcol)
        {
            float vr[BATCH];
            float vg[BATCH];
            float vb[BATCH];
            interp(c.vcola.x, c.vcolb.x, c.vcolc.x, vr);
            interp(c.vcola.y, c.vcolb.y, c.vcolc.y, vg);
            interp(c.vcola.z, c.vcolb.z, c.vcolc.z, vb);
            for (int i = 0; i < n; i++)
            {
                b.dr[i] *= vr[i];
                b.dg[i] *= vg[i];
                b.db[i] *= vb[i];
            }
        }

        // Specular tint and shininess. Metals tint F0 toward the base colour (already tinted
        // diffuse); dielectrics keep mat.specular (times the MTL specular map).
        const float shin_uniform = mat.shininess; // shared by the batch unless shin_per_pixel below
        bool shin_per_pixel = false;              // set when an MR texture fills b.shin[]
        const vec3 mspec = mat.specular;
        for (int i = 0; i < n; i++)
        {
            b.sr[i] = mspec.x;
            b.sg[i] = mspec.y;
            b.sb[i] = mspec.z;
        }
        if (c.stex)
        {
            float sp_r[BATCH];
            float sp_g[BATCH];
            float sp_b[BATCH];
            float sp_a[BATCH];
            c.stex->blend_batch(fp_s, n, sp_r, sp_g, sp_b, sp_a);
            for (int i = 0; i < n; i++)
            {
                b.sr[i] *= sp_r[i];
                b.sg[i] *= sp_g[i];
                b.sb[i] *= sp_b[i];
            }
        }
        if (c.is_metallic)
        {
            float metal[BATCH];
            const float mmetal = mat.metallic;
            for (int i = 0; i < n; i++)
            {
                metal[i] = mmetal;
            }
            if (c.mrtex)
            {
                // G = roughness, B = metallic; the ORM-packed occlusion sample is reused when it
                // addresses the same texels (occ_is_mr).
                float mg[BATCH];
                float mb[BATCH];
                // Exactly c.occ_is_mr, with no second test on the pointers: occ_is_mr implies
                // octex, which is what filled them. A wider condition here would let the else
                // blend fp_m, which sample_mr only locates when occ_is_mr is false.
                if (c.occ_is_mr)
                {
                    for (int i = 0; i < n; i++)
                    {
                        mg[i] = occ_mr_g[i];
                        mb[i] = occ_mr_b[i];
                    }
                }
                else
                {
                    float mr_r[BATCH];
                    float mr_a[BATCH];
                    c.mrtex->blend_batch(fp_m, n, mr_r, mg, mb, mr_a);
                }
                const float rough = mat.roughness;
                for (int i = 0; i < n; i++)
                {
                    metal[i] *= mb[i];
                    b.shin[i] = roughness_to_shininess(rough * mg[i]);
                }
                shin_per_pixel = true;
            }
            const vec3 mdiff = mat.diffuse;
            for (int i = 0; i < n; i++)
            {
                // lerp(DIELECTRIC_F0, base, metalness), base = the tinted diffuse.
                const float dr = mdiff.x * b.dr[i];
                const float dg = mdiff.y * b.dg[i];
                const float db = mdiff.z * b.db[i];
                b.sr[i] = DIELECTRIC_F0.x + ((dr - DIELECTRIC_F0.x) * metal[i]);
                b.sg[i] = DIELECTRIC_F0.y + ((dg - DIELECTRIC_F0.y) * metal[i]);
                b.sb[i] = DIELECTRIC_F0.z + ((db - DIELECTRIC_F0.z) * metal[i]);
            }
        }

        // Lighting: ambient term, then each light. The normal is normalized once here (the scalar
        // compute_lighting did it per call) and the view vector replaces the position arrays,
        // which nothing reads afterwards.
        const vec3 eye = c.eye;
        const vec3 amb = c.ambient * mat.ambient;
        const vec3 mdiff = mat.diffuse;
        for (int i = 0; i < n; i++)
        {
            normalize_soa(b.nx[i], b.ny[i], b.nz[i]);
            b.px[i] = eye.x - b.px[i];
            b.py[i] = eye.y - b.py[i];
            b.pz[i] = eye.z - b.pz[i];
            normalize_soa(b.px[i], b.py[i], b.pz[i]);
            b.cr[i] = amb.x * b.dr[i] * b.ao[i];
            b.cg[i] = amb.y * b.dg[i] * b.ao[i];
            b.cb[i] = amb.z * b.db[i] * b.ao[i];
            // From here dr/dg/db carry the full diffuse (material colour folded in).
            b.dr[i] *= mdiff.x;
            b.dg[i] *= mdiff.y;
            b.db[i] *= mdiff.z;
        }
        for (int li = 0; li < c.n_lights; li++)
        {
            apply_light_batch(b, c.lights[li], shin_uniform, shin_per_pixel);
        }

        // Emissive add after lighting so shaded areas still glow; the sum feeds the tonemap.
        if (c.do_emissive)
        {
            if (c.etex)
            {
                const vec3 em = c.emissive;
                float em_r[BATCH];
                float em_g[BATCH];
                float em_b[BATCH];
                float em_a[BATCH];
                c.etex->blend_batch(fp_e, n, em_r, em_g, em_b, em_a);
                for (int i = 0; i < n; i++)
                {
                    b.cr[i] += em.x * em_r[i];
                    b.cg[i] += em.y * em_g[i];
                    b.cb[i] += em.z * em_b[i];
                }
            }
            else
            {
                const vec3 em = c.emissive;
                for (int i = 0; i < n; i++)
                {
                    b.cr[i] += em.x;
                    b.cg[i] += em.y;
                    b.cb[i] += em.z;
                }
            }
        }

        // Apply the display-space soft knee only when any channel exceeds it. OR-reduced
        // comparisons and a branch-free select keep both the gate and rolloff vectorizable.
        {
            unsigned over = 0;
            for (int i = 0; i < n; i++)
            {
                over |= static_cast<unsigned>(b.cr[i] > TONEMAP_KNEE) | static_cast<unsigned>(b.cg[i] > TONEMAP_KNEE) |
                        static_cast<unsigned>(b.cb[i] > TONEMAP_KNEE);
            }
            if (over != 0u)
            {
                for (int i = 0; i < n; i++)
                {
                    b.cr[i] = tonemap_soft(b.cr[i]);
                    b.cg[i] = tonemap_soft(b.cg[i]);
                    b.cb[i] = tonemap_soft(b.cb[i]);
                }
            }
        }

        // Emit.
        if constexpr (S == Sink::Transparent)
        {
            // Opacity = material base * texture * (perspective-correct) vertex alpha.
            const float caa = c.caa;
            const float cab = c.cab;
            const float cac = c.cac;
            const float base_alpha = mat.alpha;
            const bool tex_alpha = c.tex != nullptr;
            for (int i = 0; i < n; i++)
            {
                const float vca = ((caa * b.pwa[i]) + (cab * b.pwb[i]) + (cac * b.pwc[i])) * b.wcorr[i];
                b.alpha[i] = base_alpha * (tex_alpha ? b.ta[i] : 1.0f) * vca;
            }
            constexpr float ALPHA_EPS = 1.0f / 512.0f; // skip fragments too sheer to matter
            for (int i = 0; i < n; i++)
            {
                if (b.alpha[i] >= ALPHA_EPS)
                {
                    const size_t idx = (static_cast<size_t>(b.y[i]) * c.stride) + static_cast<size_t>(b.x[i]);
                    c.abuf->push(idx, b.depth[i], vec3{ b.cr[i], b.cg[i], b.cb[i] }, b.alpha[i]);
                }
            }
        }
        else
        {
            for (int i = 0; i < n; i++)
            {
                const size_t idx = (static_cast<size_t>(b.y[i]) * c.stride) + static_cast<size_t>(b.x[i]);
                const Color col = vec3_to_color(vec3{ b.cr[i], b.cg[i], b.cb[i] });
                if constexpr (S == Sink::Deferred)
                {
                    c.fb->set_pixel_at(idx, b.depth[i], col);
                }
                else
                {
                    c.fb->commit_pixel(idx, b.depth[i], col);
                }
            }
        }
        b.n = 0;
    }

} // namespace

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
    float cac,
    const TileVis *vis
)
{
    // Deferred rasterizes within its tile; the other sinks span the framebuffer width and the
    // caller's row band.
    const int x_min = (S == Sink::Deferred) ? vis->x_min : 0;
    const int x_max = (S == Sink::Deferred) ? vis->x_max : fb.width() - 1;
    if constexpr (S == Sink::Deferred)
    {
        y_min = vis->y_min;
        y_max = vis->y_max;
    }
    TriSetup s; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): setup_tri writes all fields before
                // they are read
    if (!setup_tri(sa, sb, sc, wa, wb, wc, x_min, x_max, y_min, y_max, s))
    {
        return;
    }

    if constexpr (S == Sink::Transparent)
    {
        // The clamped bounding box, once, before any of this triangle's fragments is published:
        // that ordering is what ABuffer::widen requires, and why it is not per fragment.
        abuf->widen(s.x0, s.x1, s.y0, s.y1);
    }

    const float inv_wa = s.inv_wa;
    const float inv_wb = s.inv_wb;
    const float inv_wc = s.inv_wc;
    const float ba_dx = s.ba_dx;
    const float ba_dy = s.ba_dy;
    const float bb_dx = s.bb_dx;
    const float bb_dy = s.bb_dy;
    const float ba_row0 = s.ba_row;
    const float bb_row0 = s.bb_row;

    // Deferred never cuts out here: the visibility pass already dropped the pixels below the
    // cutoff, so the shade pass samples the plain RGB (identical to sample_rgba's RGB).
    const bool has_cutout = (S != Sink::Deferred) && (mat.alpha_cutoff > 0.0f && tex);
    // glTF emissive is factor * texture.rgb, so a zero factor skips sampling.
    // Callers may override the factor; the UI texture toggle controls only etex.
    const bool do_emissive = (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f);
    const auto stride = static_cast<size_t>(fb.width());

    // Per-slot UV set (glTF TEXCOORD_n) and KHR_texture_transform, read from mat. need_uv1 gates
    // the second perspective-correct interpolation. specular_map is MTL-only (always set 0) but
    // read uniformly. occ_is_mr shares one sample, so it shares mr's set and transform.
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
    const bool diffuse_xf = mat.diffuse_map.has_transform;
    const bool normal_xf = mat.normal_map.has_transform;
    const bool specular_xf = mat.specular_map.has_transform;
    const bool mr_xf = mat.mr_map.has_transform;
    const bool occ_xf = mat.occlusion_map.has_transform;
    const bool emissive_xf = mat.emissive_map.has_transform;
    // Off (false) for every dielectric and non-glTF material, so they run unchanged.
    const bool is_metallic = (mat.metallic > 0.0f);
    // Reuse a packed ORM sample only when image and UV mapping both match.
    const bool occ_is_mr = (octex != nullptr && octex == mrtex && same_uv_mapping(mat.occlusion_map, mat.mr_map));

    const bool use_span = (s.x1 - s.x0) >= SPAN_MIN_WIDTH;
    float inv_dx[3] = { 0.0f, 0.0f, 0.0f };
    if (use_span)
    {
        span_inv_dx(s, inv_dx);
    }

    // Batch triangles likely to fill a batch; tiny blend triangles are 14% faster on
    // the immediate path, while typical large transparent surfaces gain 1.7-2.5x.
    if constexpr (S != Sink::Opaque)
    {
        // Keep any future transparent cutout on the immediate path, which owns its alpha test.
        const bool batch_shade =
            (S == Sink::Deferred) || (!has_cutout && static_cast<int64_t>(s.x1 - s.x0 + 1) * (s.y1 - s.y0 + 1) >=
                                                         2 * static_cast<int64_t>(BATCH));
        if (batch_shade)
        {
            // Deferred gathers visibility-owned pixels; Transparent gathers after its own
            // coverage and depth tests. Both supply long runs that justify batching.
            PhongBatch batch; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): filled before use
            batch.n = 0;

            // Filled on the first flush, not here: nothing reads it until a pixel is gathered.
            PhongCtx c; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): build_ctx sets every field
            bool ctx_ready = false;
            const auto build_ctx = [&]
            {
                c.fb = &fb;
                c.stride = stride;
                c.pa = pa;
                c.pb = pb;
                c.pc = pc;
                c.na = na;
                c.nb = nb;
                c.nc = nc;
                c.tana = tana;
                c.tanb = tanb;
                c.tanc = tanc;
                c.uva = uva;
                c.uvb = uvb;
                c.uvc = uvc;
                c.uv1a = uv1a;
                c.uv1b = uv1b;
                c.uv1c = uv1c;
                c.aoa = aoa;
                c.aob = aob;
                c.aoc = aoc;
                c.vcola = vcola;
                c.vcolb = vcolb;
                c.vcolc = vcolc;
                c.caa = caa;
                c.cab = cab;
                c.cac = cac;
                c.inv_wa = inv_wa;
                c.inv_wb = inv_wb;
                c.inv_wc = inv_wc;
                c.ba0 = ba_row0;
                c.bb0 = bb_row0;
                c.ba_dx = ba_dx;
                c.ba_dy = ba_dy;
                c.bb_dx = bb_dx;
                c.bb_dy = bb_dy;
                c.x0 = s.x0;
                c.y0 = s.y0;
                c.za = sa.z;
                c.zb = sb.z;
                c.zc = sc.z;
                c.eye = eye;
                c.lights = lights;
                c.n_lights = n_lights;
                c.ambient = ambient;
                c.mat = &mat;
                c.tex = tex;
                c.nmap = nmap;
                c.stex = stex;
                c.mrtex = mrtex;
                c.etex = do_emissive ? etex : nullptr;
                c.octex = octex;
                c.emissive = emissive;
                c.occlusion_strength = occlusion_strength;
                c.abuf = abuf;
                c.has_vcol = has_vcol;
                c.has_cutout = has_cutout;
                c.is_metallic = is_metallic;
                c.occ_is_mr = occ_is_mr;
                c.do_emissive = do_emissive;
                c.apply_normal_scale = apply_normal_scale;
                c.diffuse_set = diffuse_set;
                c.normal_set = normal_set;
                c.specular_set = specular_set;
                c.mr_set = mr_set;
                c.emissive_set = emissive_set;
                c.occ_set = occ_set;
                c.need_uv1 = need_uv1;
                c.diffuse_xf = diffuse_xf;
                c.normal_xf = normal_xf;
                c.specular_xf = specular_xf;
                c.mr_xf = mr_xf;
                c.occ_xf = occ_xf;
                c.emissive_xf = emissive_xf;
            };
            const auto flush = [&]
            {
                if (!ctx_ready)
                {
                    build_ctx();
                    ctx_ready = true;
                }
                shade_batch<S>(c, batch);
            };

            // Transparent coverage: the same winding-aware top-left fill rule the immediate path uses,
            // so a pixel centre landing on a shared edge is claimed by exactly one of the two
            // triangles and cannot composite twice.
            [[maybe_unused]] const float bc_dx = -(ba_dx + bb_dx);
            [[maybe_unused]] const float bc_dy = -(ba_dy + bb_dy);
            [[maybe_unused]] const bool tl_a = (ba_dy > 0.0f) || (ba_dy == 0.0f && ba_dx > 0.0f);
            [[maybe_unused]] const bool tl_b = (bb_dy > 0.0f) || (bb_dy == 0.0f && bb_dx > 0.0f);
            [[maybe_unused]] const bool tl_c = (bc_dy > 0.0f) || (bc_dy == 0.0f && bc_dx > 0.0f);

            // ry counts rows as a float (exact for any row index) so the row values cost one
            // multiply-add each, with no integer conversion in front of them.
            float ry = 0.0f;
            for (int y = s.y0; y <= s.y1; y++, ry += 1.0f)
            {
                const float ba_row = ba_row0 + (ry * ba_dy);
                const float bb_row = bb_row0 + (ry * bb_dy);
                int x_lo = s.x0;
                int x_hi = s.x1;
                if (use_span)
                {
                    float eps[3];
                    span_eps(s, ba_row, bb_row, eps);
                    if (!row_span(ba_row, bb_row, ba_dx, bb_dx, inv_dx, eps, s.x0, s.x1, x_lo, x_hi))
                    {
                        continue;
                    }
                }
                if constexpr (S == Sink::Deferred)
                {
                    // Trust visibility IDs because its vectorized edge math may round differently
                    // from this walk. Rebuild barycentrics from (x, y) without another edge test.
                    const ptrdiff_t vis_row = (static_cast<ptrdiff_t>(y - vis->y_min) * vis->stride) - vis->x_min;
                    for (int x = x_lo; x <= x_hi; ++x)
                    {
                        if (vis->ids[vis_row + x] != vis->id)
                        {
                            continue;
                        }
                        const int k = batch.n++;
                        batch.x[k] = x;
                        batch.y[k] = y;
                        if (batch.n == BATCH)
                        {
                            flush();
                        }
                    }
                }
                else
                {
                    // The barycentrics and depth are already in registers here, so the walk stores
                    // them rather than making the batch rebuild them from (x, y) as Deferred does.
                    size_t idx = (static_cast<size_t>(y) * stride) + static_cast<size_t>(x_lo);
                    auto rx = static_cast<float>(x_lo - s.x0);
                    for (int x = x_lo; x <= x_hi; ++x, ++idx, rx += 1.0f)
                    {
                        const float ba = ba_row + (rx * ba_dx);
                        const float bb = bb_row + (rx * bb_dx);
                        const float bc = 1.0f - ba - bb;
                        const bool covered = (ba > 0.0f || (ba == 0.0f && tl_a)) &&
                                             (bb > 0.0f || (bb == 0.0f && tl_b)) && (bc > 0.0f || (bc == 0.0f && tl_c));
                        if (!covered)
                        {
                            continue;
                        }
                        // Keep fragments at or in front of the final opaque depth, never writing depth
                        // (see rasterize_flat()).
                        const float depth = (ba * sa.z) + (bb * sb.z) + (bc * sc.z);
                        if (depth > fb.depth_at(idx))
                        {
                            continue;
                        }
                        const int k = batch.n++;
                        batch.x[k] = x;
                        batch.y[k] = y;
                        batch.ba[k] = ba;
                        batch.bb[k] = bb;
                        batch.bc[k] = bc;
                        batch.depth[k] = depth;
                        if (batch.n == BATCH)
                        {
                            flush();
                        }
                    }
                }
            }
            if (batch.n > 0)
            {
                flush();
            }
            return;
        }
    }
    if constexpr (S != Sink::Deferred)
    {
        // Small triangles shade in place because SoA setup outweighs vectorization.
        // Renderer tiled tests pin agreement with the batched path.
        [[maybe_unused]] const float bc_dx = -(ba_dx + bb_dx);
        [[maybe_unused]] const float bc_dy = -(ba_dy + bb_dy);
        [[maybe_unused]] const bool tl_a = (ba_dy > 0.0f) || (ba_dy == 0.0f && ba_dx > 0.0f);
        [[maybe_unused]] const bool tl_b = (bb_dy > 0.0f) || (bb_dy == 0.0f && bb_dx > 0.0f);
        [[maybe_unused]] const bool tl_c = (bc_dy > 0.0f) || (bc_dy == 0.0f && bc_dx > 0.0f);
        [[maybe_unused]] constexpr float ALPHA_EPS = 1.0f / 512.0f;

        // Walk the pixel-centre span, not setup_tri's box; see the same block in rasterize_flat for
        // why, and for why it is spelled out here rather than shared or moved into setup_tri.
        // Deferred never reaches this block, so the span always narrows.
        int walk_x0 = s.x0;
        int walk_x1 = s.x1;
        int walk_y0 = s.y0;
        int walk_y1 = s.y1;
        {
            int sp0 = 0;
            int sp1 = 0;
            if (pixel_span(std::min({ sa.x, sb.x, sc.x }), std::max({ sa.x, sb.x, sc.x }), s.x1 + 1, sp0, sp1))
            {
                walk_x0 = std::max(walk_x0, sp0);
                walk_x1 = std::min(walk_x1, sp1);
            }
            if (pixel_span(std::min({ sa.y, sb.y, sc.y }), std::max({ sa.y, sb.y, sc.y }), s.y1 + 1, sp0, sp1))
            {
                walk_y0 = std::max(walk_y0, sp0);
                walk_y1 = std::min(walk_y1, sp1);
            }
        }

        // ry counts rows as a float (exact for any row index) so the row values cost one
        // multiply-add each, with no integer conversion in front of them. It stays measured from
        // s.y0, the barycentric anchor, so narrowing the walk cannot change a pixel's value.
        auto ry = static_cast<float>(walk_y0 - s.y0);
        for (int y = walk_y0; y <= walk_y1; y++, ry += 1.0f)
        {
            const float ba_row = ba_row0 + (ry * ba_dy);
            const float bb_row = bb_row0 + (ry * bb_dy);
            int x_lo = walk_x0;
            int x_hi = walk_x1;
            if (use_span)
            {
                float eps[3];
                span_eps(s, ba_row, bb_row, eps);
                // row_span's t is measured from s.x0, where ba_row is anchored, so it keeps the box
                // bounds; its answer is then clamped into the walk.
                if (!row_span(ba_row, bb_row, ba_dx, bb_dx, inv_dx, eps, s.x0, s.x1, x_lo, x_hi))
                {
                    continue;
                }
                x_lo = std::max(x_lo, walk_x0);
                x_hi = std::min(x_hi, walk_x1);
                if (x_lo > x_hi)
                {
                    continue;
                }
            }
            size_t idx = (static_cast<size_t>(y) * stride) + static_cast<size_t>(x_lo);
            auto rx = static_cast<float>(x_lo - s.x0);
            for (int x = x_lo; x <= x_hi; ++x, ++idx, rx += 1.0f)
            {
                const float ba = ba_row + (rx * ba_dx);
                const float bb = bb_row + (rx * bb_dx);
                const float bc = 1.0f - ba - bb;
                if constexpr (S == Sink::Opaque)
                {
                    // Reduce three edge tests to one branch; computing bc first is
                    // cheaper than short-circuiting each edge.
                    const unsigned outside = static_cast<unsigned>(ba < 0.0f) | static_cast<unsigned>(bb < 0.0f) |
                                             static_cast<unsigned>(bc < 0.0f);
                    if (outside != 0u)
                    {
                        continue;
                    }
                }
                else
                {
                    const bool covered = (ba > 0.0f || (ba == 0.0f && tl_a)) && (bb > 0.0f || (bb == 0.0f && tl_b)) &&
                                         (bc > 0.0f || (bc == 0.0f && tl_c));
                    if (!covered)
                    {
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
                        continue;
                    }
                }
                else
                {
                    if (depth > fb.depth_at(idx))
                    {
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

                // Compute UV once: needed by diffuse, normal, specular, MR, and emissive samplers.
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

                // Authored occlusion (glTF) overrides the baked vertex AO per-pixel: both target the
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

                // Copy only the four mutable lighting fields, not the whole Material.
                vec3 use_diffuse = mat.diffuse;
                vec3 use_ambient = mat.ambient;
                vec3 use_specular = mat.specular;
                float use_shin = mat.shininess;

                // Opaque samples RGB; Transparent also carries alpha. The Opaque
                // instantiation leaves tex_a constant, hence the suppression.
                [[maybe_unused]] float tex_a = 1.0f; // NOLINT(misc-const-correctness)
                if (tex)
                {
                    vec2 d_uv = diffuse_set ? uv1v : uv;
                    // Opaque cutout reuses a pre-transformed sample. Transparent samples
                    // here, so it must still transform even if a future material combines modes.
                    if (diffuse_xf && (S != Sink::Opaque || !has_cutout))
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

                // Metals tint F0 toward base color; dielectrics retain 4%. Keep diffuse
                // for metals because without an environment map, strict PBR renders them black.
                if (is_metallic)
                {
                    const vec3 base = use_diffuse;
                    float metalness = mat.metallic;
                    if (mrtex)
                    {
                        // G=roughness, B=metallic. Reuse the occlusion sample when ORM-packed (same image);
                        // occ_is_mr implies an identical transform (same_uv_mapping), so skip m_uv's affine
                        // when reusing: it would be discarded.
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

                // Add emissive after lighting, then soft-knee tonemap the sum instead of
                // hard-clipping bright surfaces. Loading bakes emissive strength into the factor.
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

                // Phong is always a lit path (unlit is dispatched to rasterize_flat before reaching
                // here), so the tonemap is unconditional.
                if constexpr (S == Sink::Transparent)
                {
                    // Opacity combines material, texture, and perspective-correct vertex alpha.
                    // Tonemap before the push so resolve composites display-space values and
                    // only needs its final safety clamp.
                    const float vca = ((caa * pwa) + (cab * pwb) + (cac * pwc)) * w_corr;
                    const float a = mat.alpha * tex_a * vca;
                    if (a >= ALPHA_EPS)
                    {
                        abuf->push(idx, depth, tonemap(color), a);
                    }
                }
                else
                {
                    fb.commit_pixel(idx, depth, vec3_to_color(tonemap(color)));
                }
            }
        }
    }
}

// Resolve tile-local coverage and depth before deferred shading. Apply alpha cutout
// here so discarded texels never claim visibility.

void raster_visibility(
    vec3 sa,
    vec3 sb,
    vec3 sc,
    float wa,
    float wb,
    float wc,
    const TileVis &vis,
    float *depth,
    uint32_t *ids,
    const Texture *tex,
    float alpha_cutoff,
    const TexSlot *slot,
    vec2 uva,
    vec2 uvb,
    vec2 uvc,
    vec2 uv1a,
    vec2 uv1b,
    vec2 uv1c
)
{
    TriSetup s; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): setup_tri writes all fields before
                // they are read
    if (!setup_tri(sa, sb, sc, wa, wb, wc, vis.x_min, vis.x_max, vis.y_min, vis.y_max, s))
    {
        return;
    }
    const float ba_dx = s.ba_dx;
    const float ba_dy = s.ba_dy;
    const float bb_dx = s.bb_dx;
    const float bb_dy = s.bb_dy;
    const float ba_row0 = s.ba_row;
    const float bb_row0 = s.bb_row;

    // Reject a clamped box when one linear edge is negative at all four corners.
    // This removes bbox-only tile touches; exact coverage remains per pixel.
    {
        const auto bw = static_cast<float>(s.x1 - s.x0);
        const auto bh = static_cast<float>(s.y1 - s.y0);
        const auto all_negative = [&](float v0, float dx, float dy)
        {
            const float v1 = v0 + (bw * dx);
            const float v2 = v0 + (bh * dy);
            const float v3 = v2 + (bw * dx);
            return v0 < 0.0f && v1 < 0.0f && v2 < 0.0f && v3 < 0.0f;
        };
        if (all_negative(ba_row0, ba_dx, ba_dy) || all_negative(bb_row0, bb_dx, bb_dy) ||
            all_negative(1.0f - ba_row0 - bb_row0, -(ba_dx + bb_dx), -(ba_dy + bb_dy)))
        {
            return;
        }
    }

    const bool has_cutout = (alpha_cutoff > 0.0f && tex);
    const bool cutout_uv1 = has_cutout && slot && slot->uv_set != 0;
    const bool cutout_xf = has_cutout && slot && slot->has_transform;

    const bool use_span = (s.x1 - s.x0) >= SPAN_MIN_WIDTH;
    float inv_dx[3] = { 0.0f, 0.0f, 0.0f };
    if (use_span)
    {
        span_inv_dx(s, inv_dx);
    }

    // Evaluate edge values instead of accumulating them. Float ry is exact for row
    // indices, so each row value needs one multiply-add and no integer conversion.
    float ry = 0.0f;
    for (int y = s.y0; y <= s.y1; y++, ry += 1.0f)
    {
        const float ba_row = ba_row0 + (ry * ba_dy);
        const float bb_row = bb_row0 + (ry * bb_dy);
        int x_lo = s.x0;
        int x_hi = s.x1;
        if (use_span)
        {
            float eps[3];
            span_eps(s, ba_row, bb_row, eps);
            if (!row_span(ba_row, bb_row, ba_dx, bb_dx, inv_dx, eps, s.x0, s.x1, x_lo, x_hi))
            {
                continue;
            }
        }
        const ptrdiff_t row = (static_cast<ptrdiff_t>(y - vis.y_min) * vis.stride) - vis.x_min;
        auto rx = static_cast<float>(x_lo - s.x0);
        if (!has_cutout)
        {
            // Branch-free form (selects into unconditional stores) so the row vectorizes.
            const uint32_t id = vis.id;
            for (int x = x_lo; x <= x_hi; ++x, rx += 1.0f)
            {
                const float ba = ba_row + (rx * ba_dx);
                const float bb = bb_row + (rx * bb_dx);
                const float bc = 1.0f - ba - bb;
                const float d = (ba * sa.z) + (bb * sb.z) + (bc * sc.z);
                const float cur = depth[row + x];
                const bool in = ba >= 0.0f && bb >= 0.0f && bc >= 0.0f && d < cur;
                depth[row + x] = in ? d : cur;
                ids[row + x] = in ? id : ids[row + x];
            }
            continue;
        }
        for (int x = x_lo; x <= x_hi; ++x, rx += 1.0f)
        {
            const float ba = ba_row + (rx * ba_dx);
            const float bb = bb_row + (rx * bb_dx);
            if (ba < 0.0f || bb < 0.0f)
            {
                continue;
            }
            const float bc = 1.0f - ba - bb;
            if (bc < 0.0f)
            {
                continue;
            }
            const float d = (ba * sa.z) + (bb * sb.z) + (bc * sc.z);
            if (!(d < depth[row + x]))
            {
                continue;
            }
            const float pwa = ba * s.inv_wa;
            const float pwb = bb * s.inv_wb;
            const float pwc = bc * s.inv_wc;
            const float w_corr = 1.0f / (pwa + pwb + pwc);
            vec2 uv = cutout_uv1 ? (uv1a * pwa + uv1b * pwb + uv1c * pwc) * w_corr
                                 : (uva * pwa + uvb * pwb + uvc * pwc) * w_corr;
            if (cutout_xf)
            {
                uv = apply_tex_transform(*slot, uv);
            }
            if (tex->sample_a(uv.x, uv.y) < alpha_cutoff)
            {
                continue;
            }
            depth[row + x] = d;
            ids[row + x] = vis.id;
        }
    }
}

// Explicit template instantiation
// The rasterizer definitions live in this TU; callers in renderer.cpp (and the
// tests) need every Sink instantiation at link time. Emit them explicitly here.

template void rasterize_flat<
    Sink::
        Opaque>(Framebuffer &, vec3, vec3, vec3, float, float, float, vec3, vec3, vec3, vec2, vec2, vec2, const Texture *, float, int, int, const Texture *, vec3, vec2, vec2, vec2, const Material *, const ABuffer *, float, float, float, float, const TileVis *);
template void rasterize_flat<
    Sink::
        Transparent>(Framebuffer &, vec3, vec3, vec3, float, float, float, vec3, vec3, vec3, vec2, vec2, vec2, const Texture *, float, int, int, const Texture *, vec3, vec2, vec2, vec2, const Material *, const ABuffer *, float, float, float, float, const TileVis *);
template void rasterize_flat<
    Sink::
        Deferred>(Framebuffer &, vec3, vec3, vec3, float, float, float, vec3, vec3, vec3, vec2, vec2, vec2, const Texture *, float, int, int, const Texture *, vec3, vec2, vec2, vec2, const Material *, const ABuffer *, float, float, float, float, const TileVis *);

template void
rasterize_phong<Sink::Opaque>(Framebuffer &, vec3, vec3, vec3, float, float, float, vec3, vec3, vec3, vec3, vec3, vec3, vec3, vec3, vec3, vec2, vec2, vec2, float, float, float, vec3, vec3, vec3, bool, const vec3 &, const Light *, int, const vec3 &, const Material &, const Texture *, const Texture *, const Texture *, int, int, const Texture *, const Texture *, vec3, bool, const Texture *, float, vec2, vec2, vec2, const ABuffer *, float, float, float, const TileVis *);
template void rasterize_phong<
    Sink::Transparent>(Framebuffer &, vec3, vec3, vec3, float, float, float, vec3, vec3, vec3, vec3, vec3, vec3, vec3, vec3, vec3, vec2, vec2, vec2, float, float, float, vec3, vec3, vec3, bool, const vec3 &, const Light *, int, const vec3 &, const Material &, const Texture *, const Texture *, const Texture *, int, int, const Texture *, const Texture *, vec3, bool, const Texture *, float, vec2, vec2, vec2, const ABuffer *, float, float, float, const TileVis *);
template void
rasterize_phong<Sink::
                    Deferred>(Framebuffer &, vec3, vec3, vec3, float, float, float, vec3, vec3, vec3, vec3, vec3, vec3, vec3, vec3, vec3, vec2, vec2, vec2, float, float, float, vec3, vec3, vec3, bool, const vec3 &, const Light *, int, const vec3 &, const Material &, const Texture *, const Texture *, const Texture *, int, int, const Texture *, const Texture *, vec3, bool, const Texture *, float, vec2, vec2, vec2, const ABuffer *, float, float, float, const TileVis *);
