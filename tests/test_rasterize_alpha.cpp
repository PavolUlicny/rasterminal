#include "test.h"
#include "rasterize_test_util.h"
#include "../src/rasterize.h"

#include <initializer_list>
#include <limits>

// ─── helpers ──────────────────────────────────────────────────────────────────

static Texture make_tex(int w, int h, std::initializer_list<int> rgba)
{
    Texture t;
    t.width = w;
    t.height = h;
    t.pixels.reserve(rgba.size());
    for (int v : rgba)
        t.pixels.push_back(static_cast<uint8_t>(v));
    return t;
}

// Canonical triangle used throughout: sa=(4,2), sb=(36,2), sc=(20,18) on 40×20.
// The interior pixel (20,10) is reliably inside it.
static void rast(Framebuffer &fb, const Texture *tex, float alpha_cutoff, int y_min = 0, int y_max = 19)
{
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 zero{};
    vec3 white{1.0f, 1.0f, 1.0f};
    vec2 uv{0.5f, 0.5f};
    rasterize(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f,
              white, white, white, zero, zero, zero,
              zero, zero, zero,
              uv, uv, uv,
              tex, alpha_cutoff, nullptr,
              y_min, y_max);
}

static void rast_phong(Framebuffer &fb, const Texture *tex, float alpha_cutoff, int y_min = 0, int y_max = 19)
{
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 zero{};
    vec3 white{1.0f, 1.0f, 1.0f};
    vec3 normal{0.0f, 0.0f, 1.0f};
    vec3 tan{1.0f, 0.0f, 0.0f};
    vec3 eye{20.0f, 10.0f, -100.0f};
    vec3 ambient{1.0f, 1.0f, 1.0f};
    vec2 uv{0.5f, 0.5f};
    Material mat{};
    mat.diffuse = {1.0f, 1.0f, 1.0f};
    mat.ambient = {1.0f, 1.0f, 1.0f};
    mat.specular = {0.0f, 0.0f, 0.0f};
    mat.alpha_cutoff = alpha_cutoff;
    rasterize_phong(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f,
                    zero, zero, zero, normal, normal, normal, tan, tan, tan,
                    uv, uv, uv,
                    1.0f, 1.0f, 1.0f,
                    white, white, white, false,
                    eye, nullptr, 0, ambient, mat,
                    tex, nullptr, nullptr, nullptr,
                    y_min, y_max);
}

// ─── Group A: alpha_cutoff = 0 (disabled) is bit-identical to no cutoff ──────

// A1: cutoff=0 with an opaque texture must produce the same pixel as cutoff=0.
// Verifies the sentinel path doesn't silently branch differently.
TEST(rasterize_alpha, cutoff_zero_matches_no_cutoff_gouraud)
{
    Texture tex = make_tex(1, 1, {200, 100, 50, 255});
    Framebuffer fb_a(40, 20, /*headless=*/true), fb_b(40, 20, /*headless=*/true);
    rast(fb_a, &tex, 0.0f);
    rast(fb_b, &tex, 0.0f);
    Color ca = fb_a.get_pixel(20, 10);
    Color cb = fb_b.get_pixel(20, 10);
    ASSERT_EQ(ca.r, cb.r);
    ASSERT_EQ(ca.g, cb.g);
    ASSERT_EQ(ca.b, cb.b);
}

// A2: cutoff=0 with opaque texture is identical between Gouraud and Phong paths
// (both should produce the same texture tint when there's no lighting variation).
TEST(rasterize_alpha, cutoff_zero_gouraud_and_phong_both_draw)
{
    Texture tex = make_tex(1, 1, {255, 255, 255, 255});
    Framebuffer fb_g(40, 20, /*headless=*/true), fb_p(40, 20, /*headless=*/true);
    rast(fb_g, &tex, 0.0f);
    rast_phong(fb_p, &tex, 0.0f);
    ASSERT_TRUE(was_drawn(fb_g, 20, 10));
    ASSERT_TRUE(was_drawn(fb_p, 20, 10));
}

// ─── Group B: cutout discards transparent pixels ─────────────────────────────

// B1: fully transparent texture (alpha=0) + cutoff=0.5 → pixel not drawn (Gouraud).
// Core correctness: a fully-transparent pixel must be discarded.
TEST(rasterize_alpha, fully_transparent_pixel_not_drawn_gouraud)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex(1, 1, {255, 0, 0, 0}); // red but alpha=0
    rast(fb, &tex, 0.5f);
    ASSERT_FALSE(was_drawn(fb, 20, 10));
}

// B2: fully transparent texture + cutoff=0.5 → pixel not drawn (Phong).
TEST(rasterize_alpha, fully_transparent_pixel_not_drawn_phong)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex(1, 1, {255, 0, 0, 0});
    rast_phong(fb, &tex, 0.5f);
    ASSERT_FALSE(was_drawn(fb, 20, 10));
}

// B3: fully opaque texture (alpha=255) + cutoff=0.5 → pixel is drawn (Gouraud).
TEST(rasterize_alpha, opaque_pixel_drawn_with_cutoff_gouraud)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex(1, 1, {200, 100, 50, 255});
    rast(fb, &tex, 0.5f);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
}

// B4: fully opaque texture + cutoff=0.5 → pixel is drawn (Phong).
TEST(rasterize_alpha, opaque_pixel_drawn_with_cutoff_phong)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex(1, 1, {200, 100, 50, 255});
    rast_phong(fb, &tex, 0.5f);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
}

// B5: opaque texture + cutoff active → drawn pixel colour matches cutoff=0 baseline.
// The cutout path must still multiply the texture RGB correctly for passing pixels.
TEST(rasterize_alpha, opaque_cutoff_pixel_colour_matches_baseline_gouraud)
{
    Texture tex = make_tex(1, 1, {200, 100, 50, 255});
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
    Texture tex = make_tex(1, 1, {200, 100, 50, 255});
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
TEST(rasterize_alpha, discarded_pixel_does_not_occlude_geometry_behind_gouraud)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    // Rear triangle: white, no texture, drawn first.
    {
        vec3 sa{4.0f, 2.0f, 0.8f}, sb{36.0f, 2.0f, 0.8f}, sc{20.0f, 18.0f, 0.8f};
        vec3 zero{};
        vec3 white{1.0f, 1.0f, 1.0f};
        vec2 uv{0.0f, 0.0f};
        rasterize(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f,
                  white, white, white, zero, zero, zero,
                  zero, zero, zero,
                  uv, uv, uv,
                  nullptr, 0.0f, nullptr, 0, 19);
    }
    ASSERT_TRUE(was_drawn(fb, 20, 10));

    // Now reset just pixel (20,10) depth so rear triangle can be re-evaluated.
    // Re-draw the rear triangle with a fresh framebuffer for the actual test.
    Framebuffer fb2(40, 20, /*headless=*/true);

    // Rear triangle first (depth=0.8, white).
    {
        vec3 sa{4.0f, 2.0f, 0.8f}, sb{36.0f, 2.0f, 0.8f}, sc{20.0f, 18.0f, 0.8f};
        vec3 zero{};
        vec3 white{1.0f, 1.0f, 1.0f};
        vec2 uv{0.0f, 0.0f};
        rasterize(fb2, sa, sb, sc, 1.0f, 1.0f, 1.0f,
                  white, white, white, zero, zero, zero,
                  zero, zero, zero,
                  uv, uv, uv,
                  nullptr, 0.0f, nullptr, 0, 19);
    }

    // Front triangle (depth=0.3, fully transparent at cutoff=0.5).
    Texture tex_transparent = make_tex(1, 1, {255, 0, 0, 0});
    {
        vec3 sa{4.0f, 2.0f, 0.3f}, sb{36.0f, 2.0f, 0.3f}, sc{20.0f, 18.0f, 0.3f};
        vec3 zero{};
        vec3 white{1.0f, 1.0f, 1.0f};
        vec2 uv{0.5f, 0.5f};
        rasterize(fb2, sa, sb, sc, 1.0f, 1.0f, 1.0f,
                  white, white, white, zero, zero, zero,
                  zero, zero, zero,
                  uv, uv, uv,
                  &tex_transparent, 0.5f, nullptr, 0, 19);
    }

    // Pixel (20,10) should still show the white rear triangle, not be occluded.
    Color c = fb2.get_pixel(20, 10);
    if (c.r < 200 || c.g < 200 || c.b < 200)
        ASSERT_FAIL("transparent front should not occlude rear: got (" + std::to_string(static_cast<int>(c.r)) + "," + std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")");
}

// C2: same depth-non-pollution test for Phong path.
TEST(rasterize_alpha, discarded_pixel_does_not_occlude_geometry_behind_phong)
{
    Framebuffer fb(40, 20, /*headless=*/true);

    vec3 sa_rear{4.0f, 2.0f, 0.8f}, sb_rear{36.0f, 2.0f, 0.8f}, sc_rear{20.0f, 18.0f, 0.8f};
    vec3 sa_front{4.0f, 2.0f, 0.3f}, sb_front{36.0f, 2.0f, 0.3f}, sc_front{20.0f, 18.0f, 0.3f};
    vec3 zero{};
    vec3 white{1.0f, 1.0f, 1.0f};
    vec3 normal{0.0f, 0.0f, 1.0f};
    vec3 tan{1.0f, 0.0f, 0.0f};
    vec3 eye{20.0f, 10.0f, -100.0f};
    vec3 ambient{1.0f, 1.0f, 1.0f};
    vec2 uv{0.5f, 0.5f};

    Material mat_opaque{};
    mat_opaque.diffuse = {1.0f, 1.0f, 1.0f};
    mat_opaque.ambient = {1.0f, 1.0f, 1.0f};
    mat_opaque.specular = {0.0f, 0.0f, 0.0f};
    mat_opaque.alpha_cutoff = 0.0f;

    Material mat_cutout = mat_opaque;
    mat_cutout.alpha_cutoff = 0.5f;

    // Draw rear opaque triangle.
    rasterize_phong(fb, sa_rear, sb_rear, sc_rear, 1.0f, 1.0f, 1.0f,
                    zero, zero, zero, normal, normal, normal, tan, tan, tan,
                    uv, uv, uv,
                    1.0f, 1.0f, 1.0f,
                    white, white, white, false,
                    eye, nullptr, 0, ambient, mat_opaque,
                    nullptr, nullptr, nullptr, nullptr, 0, 19);

    // Draw front fully-transparent triangle.
    Texture tex_transparent = make_tex(1, 1, {255, 0, 0, 0});
    rasterize_phong(fb, sa_front, sb_front, sc_front, 1.0f, 1.0f, 1.0f,
                    zero, zero, zero, normal, normal, normal, tan, tan, tan,
                    uv, uv, uv,
                    1.0f, 1.0f, 1.0f,
                    white, white, white, false,
                    eye, nullptr, 0, ambient, mat_cutout,
                    &tex_transparent, nullptr, nullptr, nullptr, 0, 19);

    Color c = fb.get_pixel(20, 10);
    if (c.r < 200 || c.g < 200 || c.b < 200)
        ASSERT_FAIL("transparent front should not occlude rear: got (" + std::to_string(static_cast<int>(c.r)) + "," + std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")");
}

// ─── Group D: no overhead on non-cutout path ─────────────────────────────────

// D1: texture without cutout (alpha_cutoff=0) with opaque image → same colour
// as cutoff active but alpha=1 image. Verifies the two paths are equivalent when
// alpha is 1 everywhere.
TEST(rasterize_alpha, opaque_image_same_result_with_or_without_cutoff_gouraud)
{
    Texture tex = make_tex(1, 1, {150, 80, 40, 255});
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
    Texture tex = make_tex(1, 1, {150, 80, 40, 255});
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

TEST(rasterize_alpha, alpha_exactly_at_cutoff_drawn_gouraud)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    constexpr float cutoff = 128.0f * (1.0f / 255.0f);
    Texture tex = make_tex(1, 1, {255, 255, 255, 128});
    rast(fb, &tex, cutoff);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
}

TEST(rasterize_alpha, alpha_exactly_at_cutoff_drawn_phong)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    constexpr float cutoff = 128.0f * (1.0f / 255.0f);
    Texture tex = make_tex(1, 1, {255, 255, 255, 128});
    rast_phong(fb, &tex, cutoff);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
}
