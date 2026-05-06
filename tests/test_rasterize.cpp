#include "test.h"
#include "rasterize_test_util.h"
#include "../src/rasterize.h"

// Call rasterize() with white flat colour, no texture/shadow, on the given band.
static void rast(Framebuffer &fb, vec3 sa, vec3 sb, vec3 sc, int y_min, int y_max)
{
    vec3 white{1.0f, 1.0f, 1.0f};
    vec3 black{0.0f, 0.0f, 0.0f};
    vec3 zero{0.0f, 0.0f, 0.0f};
    rasterize(fb, sa, sb, sc,
              1.0f, 1.0f, 1.0f,
              white, white, white,
              black, black, black,
              zero, zero, zero,
              vec2{0.0f, 0.0f}, vec2{0.0f, 0.0f}, vec2{0.0f, 0.0f},
              nullptr, nullptr,
              y_min, y_max);
}

// ─── draw_line ────────────────────────────────────────────────────────────────

TEST(draw_line, horizontal)
{
    FdRedirect r;
    Framebuffer fb(20, 10);
    draw_line(fb, {2.0f, 5.0f, 0.5f}, {8.0f, 5.0f, 0.5f}, Color{255, 255, 255});

    for (int x = 2; x <= 8; ++x)
        ASSERT_TRUE(was_drawn(fb, x, 5));
    ASSERT_FALSE(was_drawn(fb, 1, 5));
    ASSERT_FALSE(was_drawn(fb, 9, 5));
    ASSERT_FALSE(was_drawn(fb, 5, 4));
    ASSERT_FALSE(was_drawn(fb, 5, 6));
}

TEST(draw_line, vertical)
{
    FdRedirect r;
    Framebuffer fb(20, 10);
    draw_line(fb, {5.0f, 2.0f, 0.5f}, {5.0f, 8.0f, 0.5f}, Color{255, 255, 255});

    for (int y = 2; y <= 8; ++y)
        ASSERT_TRUE(was_drawn(fb, 5, y));
    ASSERT_FALSE(was_drawn(fb, 5, 1));
    ASSERT_FALSE(was_drawn(fb, 5, 9));
    ASSERT_FALSE(was_drawn(fb, 4, 5));
    ASSERT_FALSE(was_drawn(fb, 6, 5));
}

TEST(draw_line, diagonal)
{
    FdRedirect r;
    Framebuffer fb(20, 10);
    draw_line(fb, {1.0f, 1.0f, 0.5f}, {7.0f, 7.0f, 0.5f}, Color{255, 255, 255});

    for (int i = 1; i <= 7; ++i)
        ASSERT_TRUE(was_drawn(fb, i, i));
}

TEST(draw_line, single_pixel)
{
    FdRedirect r;
    Framebuffer fb(20, 10);
    draw_line(fb, {5.0f, 5.0f, 0.3f}, {5.0f, 5.0f, 0.3f}, Color{255, 0, 0});

    ASSERT_TRUE(was_drawn(fb, 5, 5));
    ASSERT_FALSE(was_drawn(fb, 4, 5));
    ASSERT_FALSE(was_drawn(fb, 6, 5));
    ASSERT_FALSE(was_drawn(fb, 5, 4));
    ASSERT_FALSE(was_drawn(fb, 5, 6));
}

TEST(draw_line, depth_closer_wins)
{
    // Far line (z=0.7) drawn first; near line (z=0.3) second — near must win.
    {
        FdRedirect r;
        Framebuffer fb(20, 10);
        draw_line(fb, {2.0f, 5.0f, 0.7f}, {8.0f, 5.0f, 0.7f}, Color{255, 0, 0});
        draw_line(fb, {2.0f, 5.0f, 0.3f}, {8.0f, 5.0f, 0.3f}, Color{0, 0, 255});
        assert_depth_near(fb, 5, 5, 0.3f, 0.05f);
    }
    // Near line (z=0.3) drawn first; far (z=0.7) drawn second — near must still win.
    {
        FdRedirect r;
        Framebuffer fb(20, 10);
        draw_line(fb, {2.0f, 5.0f, 0.3f}, {8.0f, 5.0f, 0.3f}, Color{0, 0, 255});
        draw_line(fb, {2.0f, 5.0f, 0.7f}, {8.0f, 5.0f, 0.7f}, Color{255, 0, 0});
        assert_depth_near(fb, 5, 5, 0.3f, 0.05f);
    }
}

TEST(draw_line, out_of_bounds_no_crash)
{
    FdRedirect r;
    Framebuffer fb(20, 10);
    // Line starts off-screen at (-5,-5) and ends on-screen at (7,7).
    draw_line(fb, {-5.0f, -5.0f, 0.5f}, {7.0f, 7.0f, 0.5f}, Color{255, 255, 255});

    ASSERT_TRUE(was_drawn(fb, 3, 3));
    ASSERT_FALSE(was_drawn(fb, 8, 8)); // beyond endpoint
}

TEST(draw_line, reversed_endpoints_same_pixels)
{
    FdRedirect r;
    Framebuffer fb1(20, 10), fb2(20, 10);
    draw_line(fb1, {2.0f, 5.0f, 0.5f}, {8.0f, 5.0f, 0.5f}, Color{255, 255, 255});
    draw_line(fb2, {8.0f, 5.0f, 0.5f}, {2.0f, 5.0f, 0.5f}, Color{255, 255, 255});
    for (int x = 2; x <= 8; x++)
    {
        ASSERT_TRUE(was_drawn(fb1, x, 5));
        ASSERT_TRUE(was_drawn(fb2, x, 5));
    }
}

TEST(draw_line, fully_offscreen_draws_nothing)
{
    FdRedirect r;
    Framebuffer fb(20, 10);
    draw_line(fb, {-10.0f, -10.0f, 0.5f}, {-5.0f, -5.0f, 0.5f}, Color{255, 255, 255});
    // No pixel inside the framebuffer should have been touched.
    for (int y = 0; y < fb.height(); y++)
        for (int x = 0; x < fb.width(); x++)
            ASSERT_FALSE(was_drawn(fb, x, y));
}

// ─── rasterize ────────────────────────────────────────────────────────────────
// Triangle: sa=(4,2), sb=(36,2), sc=(20,18) on a 40x20 framebuffer.
// Verified: pixel center (20.5,10.5) is inside; (0.5,10.5) and (39.5,10.5) outside.
// Pixel center (20.5,4.5) is inside geometrically — used for band-clipping test.

TEST(rasterize, triangle_fill_covers_interior)
{
    FdRedirect r;
    Framebuffer fb(40, 20);
    rast(fb, {4.0f, 2.0f, 0.5f}, {36.0f, 2.0f, 0.5f}, {20.0f, 18.0f, 0.5f}, 0, fb.height() - 1);

    ASSERT_TRUE(was_drawn(fb, 20, 10)); // clearly inside
    ASSERT_FALSE(was_drawn(fb, 0, 10)); // left of triangle
}

TEST(rasterize, respects_y_band)
{
    FdRedirect r;
    Framebuffer fb(40, 20);
    // y_min=6, y_max=12: pixel (20,4) is inside the triangle but below the band.
    rast(fb, {4.0f, 2.0f, 0.5f}, {36.0f, 2.0f, 0.5f}, {20.0f, 18.0f, 0.5f}, 6, 12);

    ASSERT_FALSE(was_drawn(fb, 20, 4)); // in triangle, below y_min — must not be drawn
    ASSERT_TRUE(was_drawn(fb, 20, 10)); // in triangle, in band — must be drawn
}

TEST(rasterize, depth_closer_wins)
{
    // Same screen-space triangle at two different depths.
    vec3 sa_far{4.0f, 2.0f, 0.7f}, sb_far{36.0f, 2.0f, 0.7f}, sc_far{20.0f, 18.0f, 0.7f};
    vec3 sa_near{4.0f, 2.0f, 0.2f}, sb_near{36.0f, 2.0f, 0.2f}, sc_near{20.0f, 18.0f, 0.2f};

    // Far drawn first, near second — near must win (depth test passes for second).
    {
        FdRedirect r;
        Framebuffer fb(40, 20);
        rast(fb, sa_far, sb_far, sc_far, 0, fb.height() - 1);
        rast(fb, sa_near, sb_near, sc_near, 0, fb.height() - 1);
        assert_depth_near(fb, 20, 10, 0.2f, 0.05f);
    }
    // Near drawn first, far second — near must still win (depth test rejects far).
    {
        FdRedirect r;
        Framebuffer fb(40, 20);
        rast(fb, sa_near, sb_near, sc_near, 0, fb.height() - 1);
        rast(fb, sa_far, sb_far, sc_far, 0, fb.height() - 1);
        assert_depth_near(fb, 20, 10, 0.2f, 0.05f);
    }
}

TEST(rasterize, degenerate_collinear_skipped)
{
    FdRedirect r;
    Framebuffer fb(40, 20);
    // All three vertices on the same horizontal line — denom=0, setup_tri returns false.
    rast(fb, {5.0f, 5.0f, 0.5f}, {15.0f, 5.0f, 0.5f}, {10.0f, 5.0f, 0.5f}, 0, fb.height() - 1);

    for (int x = 4; x <= 16; ++x)
        ASSERT_FALSE(was_drawn(fb, x, 5));
}

TEST(rasterize, entirely_off_screen_no_crash)
{
    FdRedirect r;
    Framebuffer fb(40, 20);
    // All vertices far off-screen in the negative direction.
    rast(fb, {-50.0f, -30.0f, 0.5f}, {-20.0f, -30.0f, 0.5f}, {-35.0f, -10.0f, 0.5f},
         0, fb.height() - 1);

    for (int x = 0; x < fb.width(); ++x)
        for (int y = 0; y < fb.height(); ++y)
            ASSERT_FALSE(was_drawn(fb, x, y));
}

TEST(rasterize, subpixel_degenerate_no_crash)
{
    FdRedirect r;
    Framebuffer fb(40, 20);
    // Vertices spaced much less than one pixel — must not crash.
    rast(fb, {10.0f, 10.0f, 0.5f}, {10.3f, 10.0f, 0.5f}, {10.15f, 10.3f, 0.5f},
         0, fb.height() - 1);
}

// ─── rasterize_phong ──────────────────────────────────────────────────────────

TEST(rasterize_phong, fills_interior)
{
    FdRedirect r;
    Framebuffer fb(40, 20);

    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 zero{0.0f, 0.0f, 0.0f};
    vec3 normal{0.0f, 0.0f, -1.0f};
    vec3 tan{1.0f, 0.0f, 0.0f};
    Light light{};
    Material mat{};

    rasterize_phong(fb, sa, sb, sc,
                    1.0f, 1.0f, 1.0f,
                    zero, zero, zero,
                    normal, normal, normal,
                    tan, tan, tan,
                    vec2{0.0f, 0.0f}, vec2{1.0f, 0.0f}, vec2{0.0f, 1.0f},
                    1.0f, 1.0f, 1.0f,
                    vec3{1, 1, 1}, vec3{1, 1, 1}, vec3{1, 1, 1},
                    false,
                    vec3{20.0f, 10.0f, -10.0f},
                    &light, 1,
                    vec3{0.2f, 0.2f, 0.2f},
                    mat,
                    nullptr, nullptr, nullptr, nullptr,
                    0, fb.height() - 1);

    ASSERT_TRUE(was_drawn(fb, 20, 10)); // interior must be covered
    ASSERT_FALSE(was_drawn(fb, 0, 10)); // exterior must not be drawn
}

TEST(rasterize_phong, respects_y_band)
{
    FdRedirect r;
    Framebuffer fb(40, 20);

    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 zero{0.0f, 0.0f, 0.0f};
    vec3 normal{0.0f, 0.0f, -1.0f};
    vec3 tan{1.0f, 0.0f, 0.0f};
    Light light{};
    Material mat{};

    rasterize_phong(fb, sa, sb, sc,
                    1.0f, 1.0f, 1.0f,
                    zero, zero, zero,
                    normal, normal, normal,
                    tan, tan, tan,
                    vec2{0.0f, 0.0f}, vec2{1.0f, 0.0f}, vec2{0.0f, 1.0f},
                    1.0f, 1.0f, 1.0f,
                    vec3{1, 1, 1}, vec3{1, 1, 1}, vec3{1, 1, 1},
                    false,
                    vec3{20.0f, 10.0f, -10.0f},
                    &light, 1,
                    vec3{0.2f, 0.2f, 0.2f},
                    mat,
                    nullptr, nullptr, nullptr, nullptr,
                    6, 12);

    ASSERT_FALSE(was_drawn(fb, 20, 4)); // in triangle, below y_min=6 — must not be drawn
    ASSERT_TRUE(was_drawn(fb, 20, 10)); // in triangle, in band — must be drawn
}

// ─── Framebuffer ──────────────────────────────────────────────────────────────

TEST(framebuffer, clear_resets_depth)
{
    FdRedirect r;
    Framebuffer fb(20, 10);
    draw_line(fb, {2.0f, 5.0f, 0.4f}, {8.0f, 5.0f, 0.4f}, Color{255, 0, 0});
    ASSERT_TRUE(was_drawn(fb, 5, 5)); // confirm it was drawn

    fb.clear();

    for (int x = 2; x <= 8; ++x)
        ASSERT_FALSE(was_drawn(fb, x, 5)); // all depths reset to infinity
}

TEST(framebuffer, resize_resets_depth_and_dimensions)
{
    FdRedirect r;
    Framebuffer fb(20, 10);
    draw_line(fb, {2.0f, 5.0f, 0.4f}, {8.0f, 5.0f, 0.4f}, Color{255, 0, 0});

    fb.resize(30, 16); // also emits \033[2J — still safe, stdout is redirected

    ASSERT_EQ(fb.width(), 30);
    ASSERT_EQ(fb.height(), 16);
    ASSERT_FALSE(was_drawn(fb, 5, 5)); // depth reset: formerly drawn pixel reads as undrawn
}

TEST(framebuffer, test_and_set_depth_semantics)
{
    FdRedirect r;
    Framebuffer fb(10, 10);

    ASSERT_TRUE(fb.test_and_set_depth(5, 5, 0.5f));  // fresh: always succeeds
    ASSERT_FALSE(fb.test_and_set_depth(5, 5, 0.6f)); // deeper: fails
    ASSERT_FALSE(fb.test_and_set_depth(5, 5, 0.5f)); // equal: not strictly less, fails
    ASSERT_TRUE(fb.test_and_set_depth(5, 5, 0.4f));  // shallower: succeeds

    // Out-of-bounds must return false and not crash.
    ASSERT_FALSE(fb.test_and_set_depth(-1, 5, 0.1f));
    ASSERT_FALSE(fb.test_and_set_depth(5, -1, 0.1f));
    ASSERT_FALSE(fb.test_and_set_depth(10, 5, 0.1f));
    ASSERT_FALSE(fb.test_and_set_depth(5, 10, 0.1f));
}

TEST(framebuffer, set_pixel_out_of_bounds_no_crash)
{
    FdRedirect r;
    Framebuffer fb(10, 10);
    fb.set_pixel(-1, 0, Color{255, 0, 0});
    fb.set_pixel(0, -1, Color{255, 0, 0});
    fb.set_pixel(10, 0, Color{255, 0, 0});
    fb.set_pixel(0, 10, Color{255, 0, 0});
    fb.set_pixel(-100, -100, Color{0, 0, 0});
}

// ─── perspective-correct interpolation ───────────────────────────────────────
//
// Canonical triangle: sa=(4,2), sb=(36,2), sc=(20,18) on a 40×20 framebuffer.
// Key pixel centres and their pre-computed screen-space barycentric weights:
//
//   Pixel (12,10) — centre (12.5,10.5): ba=0.46875, bb=0,       bc=0.53125
//   Pixel (27,10) — centre (27.5,10.5): ba=0,       bb=0.46875, bc=0.53125
//   Pixel (20,10) — centre (20.5,10.5): ba=0.21875, bb=0.25,    bc=0.53125
//
// All values derived analytically from setup_tri's formulas and verified below.

// Call rasterize() with per-vertex colours (no texture, no shadow).
static void rast_colored(Framebuffer &fb,
                         vec3 sa, vec3 sb, vec3 sc,
                         float wa, float wb, float wc,
                         vec3 ca, vec3 cb, vec3 cc,
                         int y_min, int y_max)
{
    vec3 zero{};
    rasterize(fb, sa, sb, sc,
              wa, wb, wc,
              ca, cb, cc,
              ca, cb, cc,
              zero, zero, zero,
              vec2{0.0f, 0.0f}, vec2{0.0f, 0.0f}, vec2{0.0f, 0.0f},
              nullptr, nullptr,
              y_min, y_max);
}

// ── Group A: equal-w invariance ───────────────────────────────────────────────

// With wa=wb=wc (uniform), perspective-correct reduces to plain barycentric.
// Scaling all three w's by the same factor must not change pixel colours.
TEST(rasterize, equal_w_nontrivial_matches_w1)
{
    // Colours: a=red, b=green, c=blue.  At pixel (20,10): ba=0.21875, bb=0.25, bc=0.53125.
    // Expected: R≈55, G≈63, B≈135 for any uniform w.
    FdRedirect r;
    vec3 red{1.0f, 0.0f, 0.0f}, green{0.0f, 1.0f, 0.0f}, blue{0.0f, 0.0f, 1.0f};
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};

    Framebuffer fb1(40, 20), fb2(40, 20);
    rast_colored(fb1, sa, sb, sc, 1.0f, 1.0f, 1.0f, red, green, blue, 0, 19);
    rast_colored(fb2, sa, sb, sc, 5.0f, 5.0f, 5.0f, red, green, blue, 0, 19);

    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    ASSERT_TRUE(was_drawn(fb2, 20, 10));
    // Both runs must agree channel-wise
    assert_pixel_near(fb2, 20, 10, fb1.get_pixel(20, 10), 2);
    // And the colour must match the analytic expectation
    assert_pixel_near(fb1, 20, 10, Color{55, 63, 135}, 4);
}

// Same invariance check for the Phong path using AO as the varying attribute.
// aoc=1, others=0; ambient=(1,0,0).  At pixel (20,10): ao=bc=0.53125 → R≈135.
TEST(rasterize_phong, equal_w_nontrivial_matches_w1)
{
    FdRedirect r;
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 zero{}, normal{0.0f, 0.0f, -1.0f}, tan{1.0f, 0.0f, 0.0f}, white{1.0f, 1.0f, 1.0f};
    vec3 eye{20.0f, 10.0f, -100.0f};
    vec3 ambient{1.0f, 0.0f, 0.0f};
    Material mat{};

    auto rph = [&](Framebuffer &fb, float wa, float wb, float wc)
    {
        rasterize_phong(fb, sa, sb, sc, wa, wb, wc,
                        zero, zero, zero, normal, normal, normal, tan, tan, tan,
                        vec2{0.0f, 0.0f}, vec2{0.0f, 0.0f}, vec2{0.0f, 0.0f},
                        0.0f, 0.0f, 1.0f,
                        white, white, white, false,
                        eye, nullptr, 0, ambient, mat,
                        nullptr, nullptr, nullptr, nullptr,
                        0, 19);
    };

    Framebuffer fb1(40, 20), fb2(40, 20);
    rph(fb1, 1.0f, 1.0f, 1.0f);
    rph(fb2, 5.0f, 5.0f, 5.0f);

    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    ASSERT_TRUE(was_drawn(fb2, 20, 10));
    assert_pixel_near(fb2, 20, 10, fb1.get_pixel(20, 10), 2);
    // ao≈0.531 → R≈135
    assert_pixel_near(fb1, 20, 10, Color{135, 0, 0}, 5);
}

// Depth must not be affected by w values (depth is linearly interpolated).
// Using uniform wa=wb=wc=10 with constant depth 0.5 — depth must still be 0.5.
TEST(rasterize, equal_w_does_not_change_depth)
{
    FdRedirect r;
    Framebuffer fb(40, 20);
    vec3 white{1.0f, 1.0f, 1.0f};
    rast_colored(fb,
                 {4.0f, 2.0f, 0.5f}, {36.0f, 2.0f, 0.5f}, {20.0f, 18.0f, 0.5f},
                 10.0f, 10.0f, 10.0f, white, white, white, 0, 19);

    assert_depth_near(fb, 20, 10, 0.5f, 0.01f);
}

// ── Group B: unequal w biases attributes toward the smaller-w (nearer) vertex ─

// a=(red), b=(red), c=(blue); wa=wb=10 (far), wc=1 (near).
// Pixel (12,10) is at the screen midpoint of edge a→c.
// Perspective-correct: R≈20, B≈234 (biased toward near blue vertex c).
TEST(rasterize, unequal_w_color_biased_to_near_vertex)
{
    FdRedirect r;
    Framebuffer fb(40, 20);
    vec3 red{1.0f, 0.0f, 0.0f}, blue{0.0f, 0.0f, 1.0f};
    rast_colored(fb,
                 {4.0f, 2.0f, 0.5f}, {36.0f, 2.0f, 0.5f}, {20.0f, 18.0f, 0.5f},
                 10.0f, 10.0f, 1.0f,
                 red, red, blue,
                 0, 19);

    ASSERT_TRUE(was_drawn(fb, 12, 10));
    Color c = fb.get_pixel(12, 10);
    if (c.r > 50)
        ASSERT_FAIL("R too high (" + std::to_string(static_cast<int>(c.r)) +
                    "): expected bias toward blue near vertex");
    if (c.b < 200)
        ASSERT_FAIL("B too low (" + std::to_string(static_cast<int>(c.b)) +
                    "): expected bias toward blue near vertex");
}

// a=white(far), b=red(near), c=green(far); wb=1, wa=wc=10.
// Pixel (27,10) is at the screen midpoint of edge b→c.
// Perspective-correct: R≈229, G≈26 (biased toward near red vertex b).
// Linear interpolation would give R≈120, G≈135.
TEST(rasterize, unequal_w_screen_midpoint_not_attribute_midpoint)
{
    FdRedirect r;
    Framebuffer fb(40, 20);
    vec3 white{1.0f, 1.0f, 1.0f}, red{1.0f, 0.0f, 0.0f}, green{0.0f, 1.0f, 0.0f};
    rast_colored(fb,
                 {4.0f, 2.0f, 0.5f}, {36.0f, 2.0f, 0.5f}, {20.0f, 18.0f, 0.5f},
                 10.0f, 1.0f, 10.0f,
                 white, red, green,
                 0, 19);

    ASSERT_TRUE(was_drawn(fb, 27, 10));
    Color c = fb.get_pixel(27, 10);
    if (c.r < 200)
        ASSERT_FAIL("R too low (" + std::to_string(static_cast<int>(c.r)) +
                    "): expected bias toward near red vertex b");
    if (c.g > 50)
        ASSERT_FAIL("G too high (" + std::to_string(static_cast<int>(c.g)) +
                    "): green (far) vertex should contribute little");
}

// Phong path: aoc=1 (only c has AO); wa=wb=10 (far), wc=1 (near).
// Pixel (12,10): perspective-correct ao≈0.919 → R≈234.
// Baseline with equal w: linear ao=bc≈0.531 → R≈135.
// The unequal-w run must be significantly brighter than the equal-w baseline.
TEST(rasterize_phong, unequal_w_ao_biased_to_near_vertex)
{
    FdRedirect r;
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 zero{}, normal{0.0f, 0.0f, -1.0f}, tan{1.0f, 0.0f, 0.0f}, white{1.0f, 1.0f, 1.0f};
    vec3 eye{20.0f, 10.0f, -100.0f};
    vec3 ambient{1.0f, 0.0f, 0.0f};
    Material mat{};

    auto rph = [&](Framebuffer &fb, float wa, float wb, float wc)
    {
        rasterize_phong(fb, sa, sb, sc, wa, wb, wc,
                        zero, zero, zero, normal, normal, normal, tan, tan, tan,
                        vec2{0.0f, 0.0f}, vec2{0.0f, 0.0f}, vec2{0.0f, 0.0f},
                        0.0f, 0.0f, 1.0f,
                        white, white, white, false,
                        eye, nullptr, 0, ambient, mat,
                        nullptr, nullptr, nullptr, nullptr,
                        0, 19);
    };

    Framebuffer fb_persp(40, 20), fb_linear(40, 20);
    rph(fb_persp, 10.0f, 10.0f, 1.0f);
    rph(fb_linear, 1.0f, 1.0f, 1.0f);

    ASSERT_TRUE(was_drawn(fb_persp, 12, 10));
    ASSERT_TRUE(was_drawn(fb_linear, 12, 10));

    // Perspective-correct: ao≈0.919 → R≥200
    if (fb_persp.get_pixel(12, 10).r < 200)
        ASSERT_FAIL("R too low for perspective-correct run: expected ao≈0.919 at (12,10)");
    // Linear baseline: ao≈0.531 → R≤160
    if (fb_linear.get_pixel(12, 10).r > 160)
        ASSERT_FAIL("R too high for linear baseline: expected ao≈0.531 at (12,10)");
}

// Extreme w ratio (wc=1000): the contribution of c must be negligible.
// a=red, b=red, c=blue; wa=wb=1, wc=1000. At centroid (20,10): R≈255, B≈0.
// Also verifies no NaN/inf crashes from near-zero gamma/wc contribution.
TEST(rasterize, extreme_w_ratio_numerical_stability)
{
    FdRedirect r;
    Framebuffer fb(40, 20);
    vec3 red{1.0f, 0.0f, 0.0f}, blue{0.0f, 0.0f, 1.0f};
    rast_colored(fb,
                 {4.0f, 2.0f, 0.5f}, {36.0f, 2.0f, 0.5f}, {20.0f, 18.0f, 0.5f},
                 1.0f, 1.0f, 1000.0f,
                 red, red, blue,
                 0, 19);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 250)
        ASSERT_FAIL("R too low (" + std::to_string(static_cast<int>(c.r)) +
                    "): far blue vertex (wc=1000) should contribute nearly zero");
    if (c.b > 5)
        ASSERT_FAIL("B too high (" + std::to_string(static_cast<int>(c.b)) +
                    "): far blue vertex (wc=1000) should contribute nearly zero");
}

// ── Group C: depth interpolation is linear (not perspective-corrected) ────────

// z_ndc varies: sa.z=0.2, sc.z=0.8; wa=wb=10, wc=1.
// At pixel (12,10): linear depth = 0.46875*0.2 + 0*0.2 + 0.53125*0.8 = 0.51875.
// Perspective-correct depth would be ≈0.751 — far outside the tolerance window.
TEST(rasterize, unequal_w_depth_still_linear)
{
    FdRedirect r;
    Framebuffer fb(40, 20);
    vec3 white{1.0f, 1.0f, 1.0f};
    rast_colored(fb,
                 {4.0f, 2.0f, 0.2f}, {36.0f, 2.0f, 0.2f}, {20.0f, 18.0f, 0.8f},
                 10.0f, 10.0f, 1.0f,
                 white, white, white,
                 0, 19);

    assert_depth_near(fb, 12, 10, 0.519f, 0.015f);
}

// Same depth-linearity invariant in the Phong rasterizer path.
TEST(rasterize_phong, unequal_w_depth_still_linear)
{
    FdRedirect r;
    Framebuffer fb(40, 20);
    vec3 zero{}, normal{0.0f, 0.0f, -1.0f}, tan{1.0f, 0.0f, 0.0f}, white{1.0f, 1.0f, 1.0f};
    vec3 eye{20.0f, 10.0f, -100.0f};
    Material mat{};
    vec3 ambient{0.5f, 0.5f, 0.5f};

    rasterize_phong(fb,
                    {4.0f, 2.0f, 0.2f}, {36.0f, 2.0f, 0.2f}, {20.0f, 18.0f, 0.8f},
                    10.0f, 10.0f, 1.0f,
                    zero, zero, zero, normal, normal, normal, tan, tan, tan,
                    vec2{0.0f, 0.0f}, vec2{0.0f, 0.0f}, vec2{0.0f, 0.0f},
                    1.0f, 1.0f, 1.0f,
                    white, white, white, false,
                    eye, nullptr, 0, ambient, mat,
                    nullptr, nullptr, nullptr, nullptr,
                    0, 19);

    assert_depth_near(fb, 12, 10, 0.519f, 0.015f);
}

// ── Group D: y_band clipping is unaffected by w values ───────────────────────

// Repeats the band-clipping test with wa=wb=10, wc=1 to confirm perspective
// correction does not bleed into the row-exclusion decision.
TEST(rasterize, unequal_w_y_band_unaffected)
{
    FdRedirect r;
    Framebuffer fb(40, 20);
    vec3 white{1.0f, 1.0f, 1.0f};
    rast_colored(fb,
                 {4.0f, 2.0f, 0.5f}, {36.0f, 2.0f, 0.5f}, {20.0f, 18.0f, 0.5f},
                 10.0f, 10.0f, 1.0f,
                 white, white, white,
                 6, 12);

    ASSERT_FALSE(was_drawn(fb, 20, 4)); // in triangle, below y_min=6 — must not be drawn
    ASSERT_TRUE(was_drawn(fb, 20, 10)); // in triangle, in band — must be drawn
}
