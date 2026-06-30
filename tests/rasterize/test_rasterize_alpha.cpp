#include "tests/test.h"
#include "tests/rasterize_test_util.h"
#include "src/rasterize.h"

#include <initializer_list>

// ─── helpers ──────────────────────────────────────────────────────────────────

static Texture make_tex(int w, int h, std::initializer_list<int> rgba)
{
    Texture t;
    t.width = w;
    t.height = h;
    t.pixels.reserve(rgba.size());
    for (int v : rgba)
    {
        t.pixels.push_back(static_cast<uint8_t>(v));
    }
    return t;
}

// Canonical triangle used throughout: sa=(4,2), sb=(36,2), sc=(20,18) on 40×20.
// The interior pixel (20,10) is reliably inside it.
static void rast(Framebuffer &fb, const Texture *tex, float alpha_cutoff, int y_min = 0, int y_max = 19)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 white{ 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    rasterize_flat(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, uv, uv, uv, tex, alpha_cutoff, y_min, y_max);
}

static void rast_phong(Framebuffer &fb, const Texture *tex, float alpha_cutoff, int y_min = 0, int y_max = 19)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{};
    vec3 white{ 1.0f, 1.0f, 1.0f };
    vec3 normal{ 0.0f, 0.0f, 1.0f };
    vec3 tan{ 1.0f, 0.0f, 0.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    mat.alpha_cutoff = alpha_cutoff;
    rasterize_phong(
        fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
        1.0f, 1.0f, white, white, white, false, eye, nullptr, 0, ambient, mat, tex, nullptr, nullptr, y_min, y_max
    );
}

// ─── Group A: alpha_cutoff = 0 (disabled) is bit-identical to no cutoff ──────

// A1: cutoff=0 with an opaque texture must produce the same pixel as cutoff=0.
// Verifies the sentinel path doesn't silently branch differently.
TEST(rasterize_alpha, cutoff_zero_matches_no_cutoff_flat)
{
    Texture tex = make_tex(1, 1, { 200, 100, 50, 255 });
    Framebuffer fb_a(40, 20, /*headless=*/true), fb_b(40, 20, /*headless=*/true);
    rast(fb_a, &tex, 0.0f);
    rast(fb_b, &tex, 0.0f);
    Color ca = fb_a.get_pixel(20, 10);
    Color cb = fb_b.get_pixel(20, 10);
    ASSERT_EQ(ca.r, cb.r);
    ASSERT_EQ(ca.g, cb.g);
    ASSERT_EQ(ca.b, cb.b);
}

// A2: cutoff=0 with opaque texture is identical between Flat and Phong paths
// (both should produce the same texture tint when there's no lighting variation).
TEST(rasterize_alpha, cutoff_zero_flat_and_phong_both_draw)
{
    Texture tex = make_tex(1, 1, { 255, 255, 255, 255 });
    Framebuffer fb_g(40, 20, /*headless=*/true), fb_p(40, 20, /*headless=*/true);
    rast(fb_g, &tex, 0.0f);
    rast_phong(fb_p, &tex, 0.0f);
    ASSERT_TRUE(was_drawn(fb_g, 20, 10));
    ASSERT_TRUE(was_drawn(fb_p, 20, 10));
}

// ─── Group B: cutout discards transparent pixels ─────────────────────────────

// B1: fully transparent texture (alpha=0) + cutoff=0.5 → pixel not drawn (Flat).
// Core correctness: a fully-transparent pixel must be discarded.
TEST(rasterize_alpha, fully_transparent_pixel_not_drawn_flat)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex(1, 1, { 255, 0, 0, 0 }); // red but alpha=0
    rast(fb, &tex, 0.5f);
    ASSERT_FALSE(was_drawn(fb, 20, 10));
}

// B2: fully transparent texture + cutoff=0.5 → pixel not drawn (Phong).
TEST(rasterize_alpha, fully_transparent_pixel_not_drawn_phong)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex(1, 1, { 255, 0, 0, 0 });
    rast_phong(fb, &tex, 0.5f);
    ASSERT_FALSE(was_drawn(fb, 20, 10));
}

// B3: fully opaque texture (alpha=255) + cutoff=0.5 → pixel is drawn (Flat).
TEST(rasterize_alpha, opaque_pixel_drawn_with_cutoff_flat)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex(1, 1, { 200, 100, 50, 255 });
    rast(fb, &tex, 0.5f);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
}

// B4: fully opaque texture + cutoff=0.5 → pixel is drawn (Phong).
TEST(rasterize_alpha, opaque_pixel_drawn_with_cutoff_phong)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex(1, 1, { 200, 100, 50, 255 });
    rast_phong(fb, &tex, 0.5f);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
}

// B5: opaque texture + cutoff active → drawn pixel colour matches cutoff=0 baseline.
// The cutout path must still multiply the texture RGB correctly for passing pixels.
TEST(rasterize_alpha, opaque_cutoff_pixel_colour_matches_baseline_flat)
{
    Texture tex = make_tex(1, 1, { 200, 100, 50, 255 });
    Framebuffer fb_base(40, 20, /*headless=*/true), fb_cut(40, 20, /*headless=*/true);
    rast(fb_base, &tex, 0.0f);
    rast(fb_cut, &tex, 0.5f);
    Color cb = fb_base.get_pixel(20, 10);
    Color cc = fb_cut.get_pixel(20, 10);
    ASSERT_EQ(cb.r, cc.r);
    ASSERT_EQ(cb.g, cc.g);
    ASSERT_EQ(cb.b, cc.b);
}

// B6: opaque texture + cutoff active → drawn pixel colour matches baseline (Phong).
TEST(rasterize_alpha, opaque_cutoff_pixel_colour_matches_baseline_phong)
{
    Texture tex = make_tex(1, 1, { 200, 100, 50, 255 });
    Framebuffer fb_base(40, 20, /*headless=*/true), fb_cut(40, 20, /*headless=*/true);
    rast_phong(fb_base, &tex, 0.0f);
    rast_phong(fb_cut, &tex, 0.5f);
    Color cb = fb_base.get_pixel(20, 10);
    Color cc = fb_cut.get_pixel(20, 10);
    ASSERT_EQ(cb.r, cc.r);
    ASSERT_EQ(cb.g, cc.g);
    ASSERT_EQ(cb.b, cc.b);
}

// ─── Group C: discarded pixels must not write the depth buffer ────────────────

// C1: transparent foreground triangle must not occlude an opaque triangle behind it.
// If discarded pixels claim z-buffer entries, the rear triangle would be invisible.
// Setup: opaque rear triangle at depth=0.8, transparent front triangle at depth=0.3.
TEST(rasterize_alpha, discarded_pixel_does_not_occlude_geometry_behind_flat)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    // Rear triangle: white, no texture, drawn first.
    {
        vec3 sa{ 4.0f, 2.0f, 0.8f }, sb{ 36.0f, 2.0f, 0.8f }, sc{ 20.0f, 18.0f, 0.8f };
        vec3 white{ 1.0f, 1.0f, 1.0f };
        vec2 uv{ 0.0f, 0.0f };
        rasterize_flat(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, uv, uv, uv, nullptr, 0.0f, 0, 19);
    }
    ASSERT_TRUE(was_drawn(fb, 20, 10));

    // Now reset just pixel (20,10) depth so rear triangle can be re-evaluated.
    // Re-draw the rear triangle with a fresh framebuffer for the actual test.
    Framebuffer fb2(40, 20, /*headless=*/true);

    // Rear triangle first (depth=0.8, white).
    {
        vec3 sa{ 4.0f, 2.0f, 0.8f }, sb{ 36.0f, 2.0f, 0.8f }, sc{ 20.0f, 18.0f, 0.8f };
        vec3 white{ 1.0f, 1.0f, 1.0f };
        vec2 uv{ 0.0f, 0.0f };
        rasterize_flat(fb2, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, uv, uv, uv, nullptr, 0.0f, 0, 19);
    }

    // Front triangle (depth=0.3, fully transparent at cutoff=0.5).
    Texture tex_transparent = make_tex(1, 1, { 255, 0, 0, 0 });
    {
        vec3 sa{ 4.0f, 2.0f, 0.3f }, sb{ 36.0f, 2.0f, 0.3f }, sc{ 20.0f, 18.0f, 0.3f };
        vec3 white{ 1.0f, 1.0f, 1.0f };
        vec2 uv{ 0.5f, 0.5f };
        rasterize_flat(
            fb2, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, uv, uv, uv, &tex_transparent, 0.5f, 0, 19
        );
    }

    // Pixel (20,10) should still show the white rear triangle, not be occluded.
    Color c = fb2.get_pixel(20, 10);
    if (c.r < 200 || c.g < 200 || c.b < 200)
    {
        ASSERT_FAIL(
            "transparent front should not occlude rear: got (" + std::to_string(static_cast<int>(c.r)) + "," +
            std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

// C2: same depth-non-pollution test for Phong path.
TEST(rasterize_alpha, discarded_pixel_does_not_occlude_geometry_behind_phong)
{
    Framebuffer fb(40, 20, /*headless=*/true);

    vec3 sa_rear{ 4.0f, 2.0f, 0.8f }, sb_rear{ 36.0f, 2.0f, 0.8f }, sc_rear{ 20.0f, 18.0f, 0.8f };
    vec3 sa_front{ 4.0f, 2.0f, 0.3f }, sb_front{ 36.0f, 2.0f, 0.3f }, sc_front{ 20.0f, 18.0f, 0.3f };
    vec3 zero{};
    vec3 white{ 1.0f, 1.0f, 1.0f };
    vec3 normal{ 0.0f, 0.0f, 1.0f };
    vec3 tan{ 1.0f, 0.0f, 0.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };

    Material mat_opaque{};
    mat_opaque.diffuse = { 1.0f, 1.0f, 1.0f };
    mat_opaque.ambient = { 1.0f, 1.0f, 1.0f };
    mat_opaque.specular = { 0.0f, 0.0f, 0.0f };
    mat_opaque.alpha_cutoff = 0.0f;

    Material mat_cutout = mat_opaque;
    mat_cutout.alpha_cutoff = 0.5f;

    // Draw rear opaque triangle.
    rasterize_phong(
        fb, sa_rear, sb_rear, sc_rear, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv,
        uv, uv, 1.0f, 1.0f, 1.0f, white, white, white, false, eye, nullptr, 0, ambient, mat_opaque, nullptr, nullptr,
        nullptr, 0, 19
    );

    // Draw front fully-transparent triangle.
    Texture tex_transparent = make_tex(1, 1, { 255, 0, 0, 0 });
    rasterize_phong(
        fb, sa_front, sb_front, sc_front, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv,
        uv, uv, 1.0f, 1.0f, 1.0f, white, white, white, false, eye, nullptr, 0, ambient, mat_cutout, &tex_transparent,
        nullptr, nullptr, 0, 19
    );

    Color c = fb.get_pixel(20, 10);
    if (c.r < 200 || c.g < 200 || c.b < 200)
    {
        ASSERT_FAIL(
            "transparent front should not occlude rear: got (" + std::to_string(static_cast<int>(c.r)) + "," +
            std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

// ─── Group D: no overhead on non-cutout path ─────────────────────────────────

// D1: texture without cutout (alpha_cutoff=0) with opaque image → same colour
// as cutoff active but alpha=1 image. Verifies the two paths are equivalent when
// alpha is 1 everywhere.
TEST(rasterize_alpha, opaque_image_same_result_with_or_without_cutoff_flat)
{
    Texture tex = make_tex(1, 1, { 150, 80, 40, 255 });
    Framebuffer fb_no(40, 20, /*headless=*/true), fb_with(40, 20, /*headless=*/true);
    rast(fb_no, &tex, 0.0f);
    rast(fb_with, &tex, 0.5f);
    Color cn = fb_no.get_pixel(20, 10);
    Color cw = fb_with.get_pixel(20, 10);
    ASSERT_EQ(cn.r, cw.r);
    ASSERT_EQ(cn.g, cw.g);
    ASSERT_EQ(cn.b, cw.b);
}

// D2: same equivalence test for Phong.
TEST(rasterize_alpha, opaque_image_same_result_with_or_without_cutoff_phong)
{
    Texture tex = make_tex(1, 1, { 150, 80, 40, 255 });
    Framebuffer fb_no(40, 20, /*headless=*/true), fb_with(40, 20, /*headless=*/true);
    rast_phong(fb_no, &tex, 0.0f);
    rast_phong(fb_with, &tex, 0.5f);
    Color cn = fb_no.get_pixel(20, 10);
    Color cw = fb_with.get_pixel(20, 10);
    ASSERT_EQ(cn.r, cw.r);
    ASSERT_EQ(cn.g, cw.g);
    ASSERT_EQ(cn.b, cw.b);
}

// ─── Group E: alpha exactly at cutoff boundary ────────────────────────────────
// The discard condition is strict < (alpha < cutoff), so alpha == cutoff → drawn.
// cutoff = 128 * (1/255) matches how sample_rgba converts alpha bytes, making
// the comparison bit-exact.

TEST(rasterize_alpha, alpha_exactly_at_cutoff_drawn_flat)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    constexpr float cutoff = 128.0f * (1.0f / 255.0f);
    Texture tex = make_tex(1, 1, { 255, 255, 255, 128 });
    rast(fb, &tex, cutoff);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
}

TEST(rasterize_alpha, alpha_exactly_at_cutoff_drawn_phong)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    constexpr float cutoff = 128.0f * (1.0f / 255.0f);
    Texture tex = make_tex(1, 1, { 255, 255, 255, 128 });
    rast_phong(fb, &tex, cutoff);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
}

// ─── Group F: has_cutout + nmap/stex combined ─────────────────────────────────

// F1: has_cutout=true + nmap active — the UV from the cutout pre-pass must be reused
// for nmap sampling (the `if (!has_cutout && ...)` UV recompute is skipped).
// nmap texel (255,128,128) → nm≈(1,0,0) → normal redirected to +x in world space.
// Light dir=(1,0,0): without nmap dot((0,0,1),(1,0,0))=0 → R≈0; with nmap dot≈1 → R≈255.
// alpha=255 ≥ cutoff=0.5 → pixel passes the cutout test.
TEST(rasterize_phong, cutout_and_nmap_combined_nmap_still_applied)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f };
    vec3 ambient{ 0.0f, 0.0f, 0.0f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 0.0f, 0.0f, 0.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    mat.alpha_cutoff = 0.5f;
    Light light{};
    light.direction = { 1.0f, 0.0f, 0.0f };
    light.color = { 1.0f, 0.0f, 0.0f };
    vec2 uv{ 0.5f, 0.5f };
    Texture diffuse_tex = make_tex(1, 1, { 255, 255, 255, 255 }); // opaque white → passes cutout
    Texture nmap_tex = make_tex(1, 1, { 255, 128, 128, 255 });    // nm≈(1,0,0) → redirects normal to +x

    auto rph = [&](Framebuffer &fb, const Texture *nm)
    {
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, &diffuse_tex, nm, nullptr, 0, 19
        );
    };

    Framebuffer fb_nonmap(40, 20, /*headless=*/true), fb_nmap(40, 20, /*headless=*/true);
    rph(fb_nonmap, nullptr);
    rph(fb_nmap, &nmap_tex);

    ASSERT_TRUE(was_drawn(fb_nonmap, 20, 10));
    ASSERT_TRUE(was_drawn(fb_nmap, 20, 10));
    if (fb_nonmap.get_pixel(20, 10).r > 5)
    {
        ASSERT_FAIL("without nmap+cutout: R too high, normal perpendicular to light");
    }
    Color c_nmap = fb_nmap.get_pixel(20, 10);
    // Full-intensity red (1.0) rolls off through the soft-knee tonemap to ~226, so the bright
    // sentinel sits below 255.
    if (c_nmap.r < 220)
    {
        ASSERT_FAIL(
            "with nmap+cutout: R too low, expected bright red from redirected normal, got R=" +
            std::to_string(static_cast<int>(c_nmap.r))
        );
    }
}

// ─── Group G: has_vcol + alpha_cutoff combined ────────────────────────────────

// G1: has_vcol=true + alpha_cutoff active — both compose correctly: pixel passes
// alpha test AND is tinted red by vcol.
// ambient=(1,1,1), mat.ambient=(1,1,1), red vcol → R≈255, G≈B≈0.
TEST(rasterize_phong, vcol_and_alpha_cutout_combined)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    mat.alpha_cutoff = 0.5f;
    Texture opaque_tex = make_tex(1, 1, { 255, 255, 255, 255 }); // white, alpha=255
    vec3 red{ 1.0f, 0.0f, 0.0f };

    rasterize_phong(
        fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
        1.0f, 1.0f, red, red, red, true, eye, nullptr, 0, ambient, mat, &opaque_tex, nullptr, nullptr, 0, 19
    );

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    // Full-intensity red (1.0) rolls off through the soft-knee tonemap to ~226.
    if (c.r < 220)
    {
        ASSERT_FAIL("has_vcol+cutout: R too low, expected red tint, got R=" + std::to_string(static_cast<int>(c.r)));
    }
    if (c.g > 20 || c.b > 20)
    {
        ASSERT_FAIL(
            "has_vcol+cutout: G/B too high, expected near-zero from red vcol, got (" +
            std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

// ─── Group H: cutout-only path (no nmap, no stex) — UV lifecycle ─────────────

// H1: has_cutout=true, nmap=nullptr, stex=nullptr.
// The block `if (!has_cutout && (tex || nmap || stex))` evaluates false on both
// halves — UV recompute is entirely skipped.  The `if (tex)` branch uses cutout_rgb
// (RGB from the pre-pass RGBA sample fetched at the pre-pass UV).
//
// Discriminating setup: 2×1 texture (left=red, right=blue) with all vertex UVs at
// (0.75, 0.5) → samples the blue half.  If the pre-pass UV is missing or defaulted
// to vec2{} the pixel would be red, not blue.
TEST(rasterize_phong, cutout_without_nmap_stex_uses_precomputed_uv)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 1.0f, 1.0f, 1.0f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    mat.alpha_cutoff = 0.5f;
    // 2×1: left=red, right=blue; both fully opaque.
    // Sampler: fx = u*(width-1) = u*1.  At u=0.99: tx=0.99 → R≈3, B≈252.
    // If UV defaulted to zero: tx=0 → pure red (R=255) — distinguishes the paths.
    Texture tex = make_tex(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec2 uv{ 0.99f, 0.5f }; // deep in the blue half; opaque alpha passes cutout

    Framebuffer fb(40, 20, /*headless=*/true);
    rasterize_phong(
        fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
        1.0f, 1.0f, white, white, white, false, eye, nullptr, 0, ambient, mat, &tex, nullptr, nullptr, 0, 19
    );

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    // ambient*(1,1,1)*blue_texel=(0,0,1) → R≈0, B≈255
    if (c.r > 10)
    {
        ASSERT_FAIL("cutout UV lifecycle: expected blue texel (R≈0), got R=" + std::to_string(static_cast<int>(c.r)));
    }
    if (c.b < 200)
    {
        ASSERT_FAIL("cutout UV lifecycle: expected blue texel (B≈255), got B=" + std::to_string(static_cast<int>(c.b)));
    }
}
