#include "tests/test.h"
#include "tests/rasterize_test_util.h"
#include "src/render/rasterize.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>

// Call rasterize_flat() with white flat colour, no texture, on the given band.
static void rast(Framebuffer &fb, vec3 sa, vec3 sb, vec3 sc, int y_min, int y_max)
{
    vec3 white{ 1.0f, 1.0f, 1.0f };
    rasterize_flat(
        fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, vec2{ 0.0f, 0.0f }, vec2{ 0.0f, 0.0f },
        vec2{ 0.0f, 0.0f }, nullptr, 0.0f, y_min, y_max
    );
}

// draw_line

TEST(draw_line, horizontal)
{
    Framebuffer fb(20, 10, /*headless=*/true);
    draw_line(fb, { 2.0f, 5.0f, 0.5f }, { 8.0f, 5.0f, 0.5f }, Color{ 255, 255, 255 });

    for (int x = 2; x <= 8; ++x)
    {
        ASSERT_TRUE(was_drawn(fb, x, 5));
    }
    ASSERT_FALSE(was_drawn(fb, 1, 5));
    ASSERT_FALSE(was_drawn(fb, 9, 5));
    ASSERT_FALSE(was_drawn(fb, 5, 4));
    ASSERT_FALSE(was_drawn(fb, 5, 6));
}

TEST(draw_line, vertical)
{
    Framebuffer fb(20, 10, /*headless=*/true);
    draw_line(fb, { 5.0f, 2.0f, 0.5f }, { 5.0f, 8.0f, 0.5f }, Color{ 255, 255, 255 });

    for (int y = 2; y <= 8; ++y)
    {
        ASSERT_TRUE(was_drawn(fb, 5, y));
    }
    ASSERT_FALSE(was_drawn(fb, 5, 1));
    ASSERT_FALSE(was_drawn(fb, 5, 9));
    ASSERT_FALSE(was_drawn(fb, 4, 5));
    ASSERT_FALSE(was_drawn(fb, 6, 5));
}

TEST(draw_line, diagonal)
{
    Framebuffer fb(20, 10, /*headless=*/true);
    draw_line(fb, { 1.0f, 1.0f, 0.5f }, { 7.0f, 7.0f, 0.5f }, Color{ 255, 255, 255 });

    for (int i = 1; i <= 7; ++i)
    {
        ASSERT_TRUE(was_drawn(fb, i, i));
    }
}

TEST(draw_line, single_pixel)
{
    Framebuffer fb(20, 10, /*headless=*/true);
    draw_line(fb, { 5.0f, 5.0f, 0.3f }, { 5.0f, 5.0f, 0.3f }, Color{ 255, 0, 0 });

    ASSERT_TRUE(was_drawn(fb, 5, 5));
    ASSERT_FALSE(was_drawn(fb, 4, 5));
    ASSERT_FALSE(was_drawn(fb, 6, 5));
    ASSERT_FALSE(was_drawn(fb, 5, 4));
    ASSERT_FALSE(was_drawn(fb, 5, 6));
}

TEST(draw_line, depth_closer_wins)
{
    // Far line (z=0.7) drawn first; near line (z=0.3) second; near must win.
    {
        Framebuffer fb(20, 10, /*headless=*/true);
        draw_line(fb, { 2.0f, 5.0f, 0.7f }, { 8.0f, 5.0f, 0.7f }, Color{ 255, 0, 0 });
        draw_line(fb, { 2.0f, 5.0f, 0.3f }, { 8.0f, 5.0f, 0.3f }, Color{ 0, 0, 255 });
        assert_depth_near(fb, 5, 5, 0.3f, 0.05f);
    }
    // Near line (z=0.3) drawn first; far (z=0.7) drawn second; near must still win.
    {
        Framebuffer fb(20, 10, /*headless=*/true);
        draw_line(fb, { 2.0f, 5.0f, 0.3f }, { 8.0f, 5.0f, 0.3f }, Color{ 0, 0, 255 });
        draw_line(fb, { 2.0f, 5.0f, 0.7f }, { 8.0f, 5.0f, 0.7f }, Color{ 255, 0, 0 });
        assert_depth_near(fb, 5, 5, 0.3f, 0.05f);
    }
}

TEST(draw_line, out_of_bounds_no_crash)
{
    Framebuffer fb(20, 10, /*headless=*/true);
    // Line starts off-screen at (-5,-5) and ends on-screen at (7,7).
    draw_line(fb, { -5.0f, -5.0f, 0.5f }, { 7.0f, 7.0f, 0.5f }, Color{ 255, 255, 255 });

    ASSERT_TRUE(was_drawn(fb, 3, 3));
    ASSERT_FALSE(was_drawn(fb, 8, 8)); // beyond endpoint
}

TEST(draw_line, reversed_endpoints_same_pixels)
{
    Framebuffer fb1(20, 10, /*headless=*/true), fb2(20, 10, /*headless=*/true);
    draw_line(fb1, { 2.0f, 5.0f, 0.5f }, { 8.0f, 5.0f, 0.5f }, Color{ 255, 255, 255 });
    draw_line(fb2, { 8.0f, 5.0f, 0.5f }, { 2.0f, 5.0f, 0.5f }, Color{ 255, 255, 255 });
    for (int x = 2; x <= 8; x++)
    {
        ASSERT_TRUE(was_drawn(fb1, x, 5));
        ASSERT_TRUE(was_drawn(fb2, x, 5));
    }
}

TEST(draw_line, fully_offscreen_draws_nothing)
{
    Framebuffer fb(20, 10, /*headless=*/true);
    draw_line(fb, { -10.0f, -10.0f, 0.5f }, { -5.0f, -5.0f, 0.5f }, Color{ 255, 255, 255 });
    // No pixel inside the framebuffer should have been touched.
    for (int y = 0; y < fb.height(); y++)
    {
        for (int x = 0; x < fb.width(); x++)
        {
            ASSERT_FALSE(was_drawn(fb, x, y));
        }
    }
}

// rasterize
// Triangle: sa=(4,2), sb=(36,2), sc=(20,18) on a 40x20 framebuffer.
// Verified: pixel center (20.5,10.5) is inside; (0.5,10.5) and (39.5,10.5) outside.
// Pixel center (20.5,4.5) is inside geometrically, used for band-clipping test.

TEST(rasterize, triangle_fill_covers_interior)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    rast(fb, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, 0, fb.height() - 1);

    ASSERT_TRUE(was_drawn(fb, 20, 10)); // clearly inside
    ASSERT_FALSE(was_drawn(fb, 0, 10)); // left of triangle
}

TEST(rasterize, respects_y_band)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    // y_min=6, y_max=12: pixel (20,4) is inside the triangle but below the band.
    rast(fb, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, 6, 12);

    ASSERT_FALSE(was_drawn(fb, 20, 4)); // in triangle, below y_min, must not be drawn
    ASSERT_TRUE(was_drawn(fb, 20, 10)); // in triangle, in band, must be drawn
}

TEST(rasterize, depth_closer_wins)
{
    // Same screen-space triangle at two different depths.
    vec3 sa_far{ 4.0f, 2.0f, 0.7f }, sb_far{ 36.0f, 2.0f, 0.7f }, sc_far{ 20.0f, 18.0f, 0.7f };
    vec3 sa_near{ 4.0f, 2.0f, 0.2f }, sb_near{ 36.0f, 2.0f, 0.2f }, sc_near{ 20.0f, 18.0f, 0.2f };

    // Far drawn first, near second; near must win (depth test passes for second).
    {
        Framebuffer fb(40, 20, /*headless=*/true);
        rast(fb, sa_far, sb_far, sc_far, 0, fb.height() - 1);
        rast(fb, sa_near, sb_near, sc_near, 0, fb.height() - 1);
        assert_depth_near(fb, 20, 10, 0.2f, 0.05f);
    }
    // Near drawn first, far second; near must still win (depth test rejects far).
    {
        Framebuffer fb(40, 20, /*headless=*/true);
        rast(fb, sa_near, sb_near, sc_near, 0, fb.height() - 1);
        rast(fb, sa_far, sb_far, sc_far, 0, fb.height() - 1);
        assert_depth_near(fb, 20, 10, 0.2f, 0.05f);
    }
}

TEST(rasterize, degenerate_collinear_skipped)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    // All three vertices on the same horizontal line: denom=0, setup_tri returns false.
    rast(fb, { 5.0f, 5.0f, 0.5f }, { 15.0f, 5.0f, 0.5f }, { 10.0f, 5.0f, 0.5f }, 0, fb.height() - 1);

    for (int x = 4; x <= 16; ++x)
    {
        ASSERT_FALSE(was_drawn(fb, x, 5));
    }
}

TEST(rasterize, entirely_off_screen_no_crash)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    // All vertices far off-screen in the negative direction.
    rast(fb, { -50.0f, -30.0f, 0.5f }, { -20.0f, -30.0f, 0.5f }, { -35.0f, -10.0f, 0.5f }, 0, fb.height() - 1);

    for (int x = 0; x < fb.width(); ++x)
    {
        for (int y = 0; y < fb.height(); ++y)
        {
            ASSERT_FALSE(was_drawn(fb, x, y));
        }
    }
}

TEST(rasterize, subpixel_degenerate_no_crash)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    // Vertices spaced much less than one pixel, must not crash.
    rast(fb, { 10.0f, 10.0f, 0.5f }, { 10.3f, 10.0f, 0.5f }, { 10.15f, 10.3f, 0.5f }, 0, fb.height() - 1);
}

// rasterize_phong

TEST(rasterize_phong, fills_interior)
{
    Framebuffer fb(40, 20, /*headless=*/true);

    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{ 0.0f, 0.0f, 0.0f };
    vec3 normal{ 0.0f, 0.0f, -1.0f };
    vec3 tan{ 1.0f, 0.0f, 0.0f };
    Light light{};
    Material mat{};

    rasterize_phong(
        fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, vec2{ 0.0f, 0.0f },
        vec2{ 1.0f, 0.0f }, vec2{ 0.0f, 1.0f }, 1.0f, 1.0f, 1.0f, vec3{ 1, 1, 1 }, vec3{ 1, 1, 1 }, vec3{ 1, 1, 1 },
        false, vec3{ 20.0f, 10.0f, -10.0f }, &light, 1, vec3{ 0.2f, 0.2f, 0.2f }, mat, nullptr, nullptr, nullptr, 0,
        fb.height() - 1
    );

    ASSERT_TRUE(was_drawn(fb, 20, 10)); // interior must be covered
    ASSERT_FALSE(was_drawn(fb, 0, 10)); // exterior must not be drawn
}

TEST(rasterize_phong, respects_y_band)
{
    Framebuffer fb(40, 20, /*headless=*/true);

    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{ 0.0f, 0.0f, 0.0f };
    vec3 normal{ 0.0f, 0.0f, -1.0f };
    vec3 tan{ 1.0f, 0.0f, 0.0f };
    Light light{};
    Material mat{};

    rasterize_phong(
        fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, vec2{ 0.0f, 0.0f },
        vec2{ 1.0f, 0.0f }, vec2{ 0.0f, 1.0f }, 1.0f, 1.0f, 1.0f, vec3{ 1, 1, 1 }, vec3{ 1, 1, 1 }, vec3{ 1, 1, 1 },
        false, vec3{ 20.0f, 10.0f, -10.0f }, &light, 1, vec3{ 0.2f, 0.2f, 0.2f }, mat, nullptr, nullptr, nullptr, 6, 12
    );

    ASSERT_FALSE(was_drawn(fb, 20, 4)); // in triangle, below y_min=6, must not be drawn
    ASSERT_TRUE(was_drawn(fb, 20, 10)); // in triangle, in band, must be drawn
}

// Framebuffer

TEST(framebuffer, resize_resets_depth_and_dimensions)
{
    FdRedirect r;
    Framebuffer fb(20, 10, /*headless=*/true);
    draw_line(fb, { 2.0f, 5.0f, 0.4f }, { 8.0f, 5.0f, 0.4f }, Color{ 255, 0, 0 });

    fb.resize(30, 16); // the deferred resize erase reaches stdout on the next present; FdRedirect covers it

    ASSERT_EQ(fb.width(), 30);
    ASSERT_EQ(fb.height(), 16);
    ASSERT_FALSE(was_drawn(fb, 5, 5)); // depth reset: formerly drawn pixel reads as undrawn
}

// Two threads race writing different (depth, color) at the same pixel.
// The shallower fragment's color must always be the one observed: depth and
// color update atomically together, regardless of which thread's CAS landed last.
TEST(framebuffer, multithread_depth_color_race)
{
    constexpr int ITERS = 500;
    constexpr int N_PAIRS_PER_ITER = 256;

    Framebuffer fb(64, 64, /*headless=*/true);
    std::atomic<bool> go{ false };

    for (int it = 0; it < ITERS; ++it)
    {
        fb.clear({ 0, 0, 0 });
        go.store(false, std::memory_order_relaxed);

        // Thread A writes the shallower fragment (depth 0.2, red).
        std::thread ta(
            [&]
            {
                while (!go.load(std::memory_order_acquire))
                {
                }
                for (int i = 0; i < N_PAIRS_PER_ITER; ++i)
                {
                    (void)fb.commit_pixel(i % 64, (i / 64) % 64, 0.2f, Color{ 255, 0, 0 });
                }
            }
        );
        // Thread B writes the deeper fragment (depth 0.8, blue): must lose.
        std::thread tb(
            [&]
            {
                while (!go.load(std::memory_order_acquire))
                {
                }
                for (int i = 0; i < N_PAIRS_PER_ITER; ++i)
                {
                    (void)fb.commit_pixel(i % 64, (i / 64) % 64, 0.8f, Color{ 0, 0, 255 });
                }
            }
        );

        go.store(true, std::memory_order_release);
        ta.join();
        tb.join();

        // For every pixel both threads touched, the shallower (red) color must win.
        for (int i = 0; i < N_PAIRS_PER_ITER; ++i)
        {
            const Color c = fb.get_pixel(i % 64, (i / 64) % 64);
            ASSERT_EQ(c.r, 255);
            ASSERT_EQ(c.g, 0);
            ASSERT_EQ(c.b, 0);
        }
    }
}

// perspective-correct interpolation
//
// Canonical triangle: sa=(4,2), sb=(36,2), sc=(20,18) on a 40×20 framebuffer.
// Key pixel centres and their pre-computed screen-space barycentric weights:
//
//   Pixel (12,10), centre (12.5,10.5): ba=0.46875, bb=0,       bc=0.53125
//   Pixel (27,10), centre (27.5,10.5): ba=0,       bb=0.46875, bc=0.53125
//   Pixel (20,10), centre (20.5,10.5): ba=0.21875, bb=0.25,    bc=0.53125
//
// All values derived analytically from setup_tri's formulas and verified below.

// Call rasterize_flat() with per-vertex colours (no texture).
static void rast_colored(
    Framebuffer &fb,
    vec3 sa,
    vec3 sb,
    vec3 sc,
    float wa,
    float wb,
    float wc,
    vec3 ca,
    vec3 cb,
    vec3 cc,
    int y_min,
    int y_max
)
{
    rasterize_flat(
        fb, sa, sb, sc, wa, wb, wc, ca, cb, cc, vec2{ 0.0f, 0.0f }, vec2{ 0.0f, 0.0f }, vec2{ 0.0f, 0.0f }, nullptr,
        0.0f, y_min, y_max
    );
}

// Group A: equal-w invariance

// With wa=wb=wc (uniform), perspective-correct reduces to plain barycentric.
// Scaling all three w's by the same factor must not change pixel colours.
TEST(rasterize, equal_w_nontrivial_matches_w1)
{
    // Colours: a=red, b=green, c=blue.  At pixel (20,10): ba=0.21875, bb=0.25, bc=0.53125.
    // Expected: R≈55, G≈63, B≈135 for any uniform w.
    vec3 red{ 1.0f, 0.0f, 0.0f }, green{ 0.0f, 1.0f, 0.0f }, blue{ 0.0f, 0.0f, 1.0f };
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };

    Framebuffer fb1(40, 20, /*headless=*/true), fb2(40, 20, /*headless=*/true);
    rast_colored(fb1, sa, sb, sc, 1.0f, 1.0f, 1.0f, red, green, blue, 0, 19);
    rast_colored(fb2, sa, sb, sc, 5.0f, 5.0f, 5.0f, red, green, blue, 0, 19);

    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    ASSERT_TRUE(was_drawn(fb2, 20, 10));
    // Both runs must agree channel-wise
    assert_pixel_near(fb2, 20, 10, fb1.get_pixel(20, 10), 2);
    // And the colour must match the analytic expectation
    assert_pixel_near(fb1, 20, 10, Color{ 55, 63, 135 }, 4);
}

// Same invariance check for the Phong path using AO as the varying attribute.
// aoc=1, others=0; ambient=(1,0,0).  At pixel (20,10): ao=bc=0.53125 → R≈135.
TEST(rasterize_phong, equal_w_nontrivial_matches_w1)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, -1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 1.0f, 0.0f, 0.0f };
    Material mat{};

    auto rph = [&](Framebuffer &fb, float wa, float wb, float wc)
    {
        rasterize_phong(
            fb, sa, sb, sc, wa, wb, wc, zero, zero, zero, normal, normal, normal, tan, tan, tan, vec2{ 0.0f, 0.0f },
            vec2{ 0.0f, 0.0f }, vec2{ 0.0f, 0.0f }, 0.0f, 0.0f, 1.0f, white, white, white, false, eye, nullptr, 0,
            ambient, mat, nullptr, nullptr, nullptr, 0, 19
        );
    };

    Framebuffer fb1(40, 20, /*headless=*/true), fb2(40, 20, /*headless=*/true);
    rph(fb1, 1.0f, 1.0f, 1.0f);
    rph(fb2, 5.0f, 5.0f, 5.0f);

    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    ASSERT_TRUE(was_drawn(fb2, 20, 10));
    assert_pixel_near(fb2, 20, 10, fb1.get_pixel(20, 10), 2);
    // ao≈0.531 → R≈135
    assert_pixel_near(fb1, 20, 10, Color{ 135, 0, 0 }, 5);
}

// Depth must not be affected by w values (depth is linearly interpolated).
// Using uniform wa=wb=wc=10 with constant depth 0.5: depth must still be 0.5.
TEST(rasterize, equal_w_does_not_change_depth)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 white{ 1.0f, 1.0f, 1.0f };
    rast_colored(
        fb, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, 10.0f, 10.0f, 10.0f, white, white,
        white, 0, 19
    );

    assert_depth_near(fb, 20, 10, 0.5f, 0.01f);
}

// Group B: unequal w biases attributes toward the smaller-w (nearer) vertex

// a=(red), b=(red), c=(blue); wa=wb=10 (far), wc=1 (near).
// Pixel (12,10) is at the screen midpoint of edge a→c.
// Perspective-correct: R≈20, B≈234 (biased toward near blue vertex c).
TEST(rasterize, unequal_w_color_biased_to_near_vertex)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 red{ 1.0f, 0.0f, 0.0f }, blue{ 0.0f, 0.0f, 1.0f };
    rast_colored(
        fb, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, 10.0f, 10.0f, 1.0f, red, red, blue, 0,
        19
    );

    ASSERT_TRUE(was_drawn(fb, 12, 10));
    Color c = fb.get_pixel(12, 10);
    if (c.r > 50)
    {
        ASSERT_FAIL(
            "R too high (" + std::to_string(static_cast<int>(c.r)) + "): expected bias toward blue near vertex"
        );
    }
    if (c.b < 200)
    {
        ASSERT_FAIL("B too low (" + std::to_string(static_cast<int>(c.b)) + "): expected bias toward blue near vertex");
    }
}

// a=white(far), b=red(near), c=green(far); wb=1, wa=wc=10.
// Pixel (27,10) is at the screen midpoint of edge b→c.
// Perspective-correct: R≈229, G≈26 (biased toward near red vertex b).
// Linear interpolation would give R≈120, G≈135.
TEST(rasterize, unequal_w_screen_midpoint_not_attribute_midpoint)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 white{ 1.0f, 1.0f, 1.0f }, red{ 1.0f, 0.0f, 0.0f }, green{ 0.0f, 1.0f, 0.0f };
    rast_colored(
        fb, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, 10.0f, 1.0f, 10.0f, white, red, green,
        0, 19
    );

    ASSERT_TRUE(was_drawn(fb, 27, 10));
    Color c = fb.get_pixel(27, 10);
    if (c.r < 200)
    {
        ASSERT_FAIL(
            "R too low (" + std::to_string(static_cast<int>(c.r)) + "): expected bias toward near red vertex b"
        );
    }
    if (c.g > 50)
    {
        ASSERT_FAIL(
            "G too high (" + std::to_string(static_cast<int>(c.g)) + "): green (far) vertex should contribute little"
        );
    }
}

// Phong path: aoc=1 (only c has AO); wa=wb=10 (far), wc=1 (near).
// Pixel (12,10): perspective-correct ao≈0.919 → R≈234.
// Baseline with equal w: linear ao=bc≈0.531 → R≈135.
// The unequal-w run must be significantly brighter than the equal-w baseline.
TEST(rasterize_phong, unequal_w_ao_biased_to_near_vertex)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, -1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 1.0f, 0.0f, 0.0f };
    Material mat{};

    auto rph = [&](Framebuffer &fb, float wa, float wb, float wc)
    {
        rasterize_phong(
            fb, sa, sb, sc, wa, wb, wc, zero, zero, zero, normal, normal, normal, tan, tan, tan, vec2{ 0.0f, 0.0f },
            vec2{ 0.0f, 0.0f }, vec2{ 0.0f, 0.0f }, 0.0f, 0.0f, 1.0f, white, white, white, false, eye, nullptr, 0,
            ambient, mat, nullptr, nullptr, nullptr, 0, 19
        );
    };

    Framebuffer fb_persp(40, 20, /*headless=*/true), fb_linear(40, 20, /*headless=*/true);
    rph(fb_persp, 10.0f, 10.0f, 1.0f);
    rph(fb_linear, 1.0f, 1.0f, 1.0f);

    ASSERT_TRUE(was_drawn(fb_persp, 12, 10));
    ASSERT_TRUE(was_drawn(fb_linear, 12, 10));

    // Perspective-correct: ao≈0.919 → R≥200
    if (fb_persp.get_pixel(12, 10).r < 200)
    {
        ASSERT_FAIL("R too low for perspective-correct run: expected ao≈0.919 at (12,10)");
    }
    // Linear baseline: ao≈0.531 → R≤160
    if (fb_linear.get_pixel(12, 10).r > 160)
    {
        ASSERT_FAIL("R too high for linear baseline: expected ao≈0.531 at (12,10)");
    }
}

// Extreme w ratio (wc=1000): the contribution of c must be negligible.
// a=red, b=red, c=blue; wa=wb=1, wc=1000. At centroid (20,10): R≈255, B≈0.
// Also verifies no NaN/inf crashes from near-zero gamma/wc contribution.
TEST(rasterize, extreme_w_ratio_numerical_stability)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 red{ 1.0f, 0.0f, 0.0f }, blue{ 0.0f, 0.0f, 1.0f };
    rast_colored(
        fb, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, 1.0f, 1.0f, 1000.0f, red, red, blue, 0,
        19
    );

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    // Full red (1.0) rolls off through the soft-knee tonemap to ~226, so the bright sentinel is < 255.
    if (c.r < 220)
    {
        ASSERT_FAIL(
            "R too low (" + std::to_string(static_cast<int>(c.r)) +
            "): far blue vertex (wc=1000) should contribute nearly zero"
        );
    }
    if (c.b > 5)
    {
        ASSERT_FAIL(
            "B too high (" + std::to_string(static_cast<int>(c.b)) +
            "): far blue vertex (wc=1000) should contribute nearly zero"
        );
    }
}

// Group C: depth interpolation is linear (not perspective-corrected)

// z_ndc varies: sa.z=0.2, sc.z=0.8; wa=wb=10, wc=1.
// At pixel (12,10): linear depth = 0.46875*0.2 + 0*0.2 + 0.53125*0.8 = 0.51875.
// Perspective-correct depth would be ≈0.751, far outside the tolerance window.
TEST(rasterize, unequal_w_depth_still_linear)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 white{ 1.0f, 1.0f, 1.0f };
    rast_colored(
        fb, { 4.0f, 2.0f, 0.2f }, { 36.0f, 2.0f, 0.2f }, { 20.0f, 18.0f, 0.8f }, 10.0f, 10.0f, 1.0f, white, white,
        white, 0, 19
    );

    assert_depth_near(fb, 12, 10, 0.519f, 0.015f);
}

// Same depth-linearity invariant in the Phong rasterizer path.
TEST(rasterize_phong, unequal_w_depth_still_linear)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 zero{}, normal{ 0.0f, 0.0f, -1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    Material mat{};
    vec3 ambient{ 0.5f, 0.5f, 0.5f };

    rasterize_phong(
        fb, { 4.0f, 2.0f, 0.2f }, { 36.0f, 2.0f, 0.2f }, { 20.0f, 18.0f, 0.8f }, 10.0f, 10.0f, 1.0f, zero, zero, zero,
        normal, normal, normal, tan, tan, tan, vec2{ 0.0f, 0.0f }, vec2{ 0.0f, 0.0f }, vec2{ 0.0f, 0.0f }, 1.0f, 1.0f,
        1.0f, white, white, white, false, eye, nullptr, 0, ambient, mat, nullptr, nullptr, nullptr, 0, 19
    );

    assert_depth_near(fb, 12, 10, 0.519f, 0.015f);
}

// Group D: y_band clipping is unaffected by w values

// Repeats the band-clipping test with wa=wb=10, wc=1 to confirm perspective
// correction does not bleed into the row-exclusion decision.
TEST(rasterize, unequal_w_y_band_unaffected)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 white{ 1.0f, 1.0f, 1.0f };
    rast_colored(
        fb, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, 10.0f, 10.0f, 1.0f, white, white,
        white, 6, 12
    );

    ASSERT_FALSE(was_drawn(fb, 20, 4)); // in triangle, below y_min=6, must not be drawn
    ASSERT_TRUE(was_drawn(fb, 20, 10)); // in triangle, in band, must be drawn
}

// setup_tri bounding-box & band clamping

TEST(rasterize, bbox_clamps_to_right_edge)
{
    // sb.x=100 is far off-screen; x1 clamps to width-1=19.
    // Pixels inside the framebuffer must still be drawn (no OOB write).
    Framebuffer fb(20, 20, /*headless=*/true);
    rast(fb, { 2.0f, 5.0f, 0.5f }, { 100.0f, 5.0f, 0.5f }, { 10.0f, 15.0f, 0.5f }, 0, fb.height() - 1);
    ASSERT_TRUE(was_drawn(fb, 8, 10)); // inside triangle and within framebuffer bounds
}

TEST(rasterize, bbox_clamps_to_bottom_edge)
{
    // sc.y=100 is far off-screen; y1 clamps to height-1=19.
    Framebuffer fb(20, 20, /*headless=*/true);
    rast(fb, { 2.0f, 2.0f, 0.5f }, { 18.0f, 2.0f, 0.5f }, { 10.0f, 100.0f, 0.5f }, 0, fb.height() - 1);
    ASSERT_TRUE(was_drawn(fb, 10, 10)); // inside triangle and within framebuffer bounds
}

TEST(rasterize, triangle_entirely_left_of_screen_no_draw)
{
    // All vertices off-screen to the left: x1=min(19,-10)=-10 < x0=max(0,-20)=0.
    // setup_tri returns false on the s.x0 > s.x1 early-return path.
    Framebuffer fb(20, 20, /*headless=*/true);
    rast(fb, { -20.0f, 5.0f, 0.5f }, { -10.0f, 5.0f, 0.5f }, { -15.0f, 15.0f, 0.5f }, 0, fb.height() - 1);
    for (int y = 0; y < fb.height(); ++y)
    {
        for (int x = 0; x < fb.width(); ++x)
        {
            ASSERT_FALSE(was_drawn(fb, x, y));
        }
    }
}

TEST(rasterize, single_row_band)
{
    // y_min == y_max == 10: only row 10 is rasterized; adjacent rows untouched.
    Framebuffer fb(40, 20, /*headless=*/true);
    rast(fb, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, 10, 10);
    ASSERT_TRUE(was_drawn(fb, 20, 10));  // in triangle, in band
    ASSERT_FALSE(was_drawn(fb, 20, 9));  // in triangle, outside band above
    ASSERT_FALSE(was_drawn(fb, 20, 11)); // in triangle, outside band below
}

TEST(rasterize, band_disjoint_from_triangle_draws_nothing)
{
    // Triangle spans y=2..18; band y_min=y_max=19 lies below it.
    // y0 = max(19,2)=19, y1 = min(19,18)=18 → s.y0 > s.y1 early return.
    Framebuffer fb(40, 20, /*headless=*/true);
    rast(fb, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, 19, 19);
    for (int y = 0; y < fb.height(); ++y)
    {
        for (int x = 0; x < fb.width(); ++x)
        {
            ASSERT_FALSE(was_drawn(fb, x, y));
        }
    }
}

// degenerate triangle edge cases

TEST(rasterize, three_identical_vertices_no_draw)
{
    // All three vertices coincide: single-point zero-area; denom=0 < DEGEN_AREA_EPS.
    // Distinct from degenerate_collinear_skipped (three different points on a line).
    Framebuffer fb(20, 20, /*headless=*/true);
    rast(fb, { 10.0f, 10.0f, 0.5f }, { 10.0f, 10.0f, 0.5f }, { 10.0f, 10.0f, 0.5f }, 0, fb.height() - 1);
    for (int y = 9; y <= 11; ++y)
    {
        for (int x = 9; x <= 11; ++x)
        {
            ASSERT_FALSE(was_drawn(fb, x, y));
        }
    }
}

TEST(rasterize, winding_agnostic_cw_also_draws)
{
    // rasterize_flat() is winding-agnostic: CW triangles fill the same interior pixels
    // as their CCW mirror. Backface culling is the renderer's responsibility, not
    // rasterize_flat()'s; it must work correctly for both winding orders.
    Framebuffer fb1(40, 20, /*headless=*/true), fb2(40, 20, /*headless=*/true);
    rast(fb1, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, 0, 19); // CCW
    rast(fb2, { 4.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, 0, 19); // CW (b,c swapped)
    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    ASSERT_TRUE(was_drawn(fb2, 20, 10));
}

// HDR highlight rolloff

TEST(rasterize, color_above_one_rolls_off_below_255)
{
    // Soft-knee tonemapping maps 2.0 to 0.99606, or 253, instead of clipping to 255.
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 hot{ 2.0f, 2.0f, 2.0f };
    rast_colored(
        fb, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, 1.0f, 1.0f, 1.0f, hot, hot, hot, 0, 19
    );
    assert_pixel_near(fb, 20, 10, Color{ 253, 253, 253 }, 1);
}

TEST(rasterize, color_below_zero_clamps_to_0)
{
    // Colour (-1,-1,-1), underflow, must yield (0,0,0).
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 dark{ -1.0f, -1.0f, -1.0f };
    rast_colored(
        fb, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, 1.0f, 1.0f, 1.0f, dark, dark, dark, 0,
        19
    );
    assert_pixel_near(fb, 20, 10, Color{ 0, 0, 0 }, 0);
}

// draw_line additional paths

TEST(draw_line, depth_interpolates_along_line)
{
    // Endpoints z=0.2 and z=0.8; 8 steps → sz=0.075; midpoint (x=6) has z≈0.5.
    // Existing tests use constant depth; this verifies the sz accumulation.
    Framebuffer fb(20, 10, /*headless=*/true);
    draw_line(fb, { 2.0f, 5.0f, 0.2f }, { 10.0f, 5.0f, 0.8f }, Color{ 255, 255, 255 });
    assert_depth_near(fb, 6, 5, 0.5f, 0.02f);
}

TEST(draw_line, steep_slope_dy_dominant)
{
    // dy=10, dx=2 → steps=10 (y axis dominates); sx=0.2, sy=1.0.
    // Analytically: step 0 → (3,2), step 5 → (4,7), step 10 → (5,12).
    Framebuffer fb(20, 20, /*headless=*/true);
    draw_line(fb, { 3.0f, 2.0f, 0.5f }, { 5.0f, 12.0f, 0.5f }, Color{ 255, 255, 255 });
    ASSERT_TRUE(was_drawn(fb, 3, 2));  // step 0
    ASSERT_TRUE(was_drawn(fb, 4, 7));  // step 5: x=round(3+5*0.2)=round(4.0)=4
    ASSERT_TRUE(was_drawn(fb, 5, 12)); // step 10
}

TEST(draw_line, negative_slope)
{
    // From (1,7) to (7,1): dx=+6, dy=−6, steps=6, sx=+1.0, sy=−1.0.
    // Anti-diagonal: (1,7),(2,6),(3,5),(4,4),(5,3),(6,2),(7,1).
    Framebuffer fb(20, 10, /*headless=*/true);
    draw_line(fb, { 1.0f, 7.0f, 0.5f }, { 7.0f, 1.0f, 0.5f }, Color{ 255, 255, 255 });
    for (int i = 0; i <= 6; ++i)
    {
        ASSERT_TRUE(was_drawn(fb, 1 + i, 7 - i));
    }
    ASSERT_FALSE(was_drawn(fb, 0, 8)); // before start
}

// rasterize_phong additional edge cases

// Minimal rasterize_phong helper: no lights, no textures, AO=1.
static void rast_phong_minimal(
    Framebuffer &fb, vec3 sa, vec3 sb, vec3 sc, const vec3 &ambient, const Material &mat, int y_min, int y_max
)
{
    vec3 zero{};
    vec3 normal{ 0.0f, 0.0f, -1.0f };
    vec3 tan{ 1.0f, 0.0f, 0.0f };
    vec3 white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    rasterize_phong(
        fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, vec2{ 0.0f, 0.0f },
        vec2{ 0.0f, 0.0f }, vec2{ 0.0f, 0.0f }, 1.0f, 1.0f, 1.0f, white, white, white, false, eye, nullptr, 0, ambient,
        mat, nullptr, nullptr, nullptr, y_min, y_max
    );
}

TEST(rasterize_phong, degenerate_collinear_no_crash)
{
    // Three collinear vertices → denom=0 → setup_tri returns false; no pixels drawn.
    Framebuffer fb(40, 20, /*headless=*/true);
    Material mat{};
    rast_phong_minimal(
        fb, { 5.0f, 5.0f, 0.5f }, { 15.0f, 5.0f, 0.5f }, { 10.0f, 5.0f, 0.5f }, { 0.2f, 0.2f, 0.2f }, mat, 0,
        fb.height() - 1
    );
    for (int x = 4; x <= 16; ++x)
    {
        ASSERT_FALSE(was_drawn(fb, x, 5));
    }
}

TEST(rasterize_phong, entirely_off_screen_no_crash)
{
    // All vertices far off-screen; setup_tri returns false on bbox check.
    Framebuffer fb(40, 20, /*headless=*/true);
    Material mat{};
    rast_phong_minimal(
        fb, { -50.0f, -30.0f, 0.5f }, { -20.0f, -30.0f, 0.5f }, { -35.0f, -10.0f, 0.5f }, { 0.2f, 0.2f, 0.2f }, mat, 0,
        fb.height() - 1
    );
    for (int x = 0; x < fb.width(); ++x)
    {
        for (int y = 0; y < fb.height(); ++y)
        {
            ASSERT_FALSE(was_drawn(fb, x, y));
        }
    }
}

TEST(rasterize, bbox_clamps_all_four_edges)
{
    // Clamp a triangle crossing all four edges to [0,19] on both axes. Interior
    // pixel (10,5) must draw without an out-of-bounds write.
    Framebuffer fb(20, 20, /*headless=*/true);
    rast(fb, { -5.0f, -5.0f, 0.5f }, { 25.0f, -5.0f, 0.5f }, { 10.0f, 25.0f, 0.5f }, 0, fb.height() - 1);
    ASSERT_TRUE(was_drawn(fb, 10, 5));
}

TEST(rasterize_phong, no_lights_uses_ambient_only)
{
    // n_lights=0, lights=nullptr → only ambient term: ambient * mat.ambient * ao.
    // ambient=(0.4,0.4,0.4), mat.ambient=(1,1,1), ao=1 → pixel≈(102,102,102).
    Framebuffer fb(40, 20, /*headless=*/true);
    Material mat{};
    rast_phong_minimal(
        fb, { 4.0f, 2.0f, 0.5f }, { 36.0f, 2.0f, 0.5f }, { 20.0f, 18.0f, 0.5f }, { 0.4f, 0.4f, 0.4f }, mat, 0,
        fb.height() - 1
    );
    ASSERT_TRUE(was_drawn(fb, 20, 10));
    assert_pixel_near(fb, 20, 10, Color{ 102, 102, 102 }, 5);
}

// pixel_span / tri_covers_no_pixel: the shared pixel-centre span both geometry front-ends
// and bin_triangles' tile rectangle are built on. Its epsilon is load-bearing in BOTH
// directions, so the tests below bracket it rather than only checking the ordinary cases.

TEST(pixel_span, selects_the_centres_inside_the_interval)
{
    int p0 = -1;
    int p1 = -1;
    // Centres 2.5, 3.5, 4.5 lie in [2, 5]; 5.5 does not.
    ASSERT_TRUE(pixel_span(2.0f, 5.0f, 10, p0, p1));
    ASSERT_EQ(p0, 2);
    ASSERT_EQ(p1, 4);
}

TEST(pixel_span, clamps_to_the_frame)
{
    int p0 = -1;
    int p1 = -1;
    ASSERT_TRUE(pixel_span(-500.0f, 3.0f, 10, p0, p1));
    ASSERT_EQ(p0, 0);
    ASSERT_EQ(p1, 2);
    ASSERT_TRUE(pixel_span(2.0f, 500.0f, 10, p0, p1));
    ASSERT_EQ(p0, 2);
    ASSERT_EQ(p1, 9);
}

TEST(pixel_span, rejects_an_interval_that_holds_no_centre)
{
    int p0 = -1;
    int p1 = -1;
    ASSERT_FALSE(pixel_span(2.6f, 2.9f, 10, p0, p1));    // between the 2.5 and 3.5 centres
    ASSERT_FALSE(pixel_span(-40.0f, -5.0f, 10, p0, p1)); // wholly left of the frame
    ASSERT_FALSE(pixel_span(20.0f, 30.0f, 10, p0, p1));  // wholly right of it
    ASSERT_FALSE(pixel_span(0.0f, 10.0f, 0, p0, p1));    // no frame at all
}

TEST(pixel_span, the_epsilon_is_small_but_not_zero)
{
    int p0 = -1;
    int p1 = -1;
    // Just under a centre by less than EPS: still that pixel. Raising EPS to a
    // quarter pixel was measured to cost 85% of the rejection's win, and dropping it
    // to zero lets a triangle the barycentric walk would cover fall out as a hole, so
    // both bounds matter. 0.002 px is inside 1/256; 0.1 px is far outside it.
    ASSERT_TRUE(pixel_span(2.498f, 2.498f, 10, p0, p1));
    ASSERT_EQ(p0, 2);
    ASSERT_EQ(p1, 2);
    ASSERT_FALSE(pixel_span(2.6f, 2.6f, 10, p0, p1));
}

TEST(tri_covers_no_pixel, separates_a_covering_triangle_from_a_sub_pixel_one)
{
    const vec3 a{ 4.0f, 2.0f, 0.5f };
    const vec3 b{ 36.0f, 2.0f, 0.5f };
    const vec3 c{ 20.0f, 18.0f, 0.5f };
    ASSERT_FALSE(tri_covers_no_pixel(a, b, c, 40, 20));

    // Wholly inside one pixel's cell but missing its centre on x.
    const vec3 sa{ 2.6f, 2.2f, 0.5f };
    const vec3 sb{ 2.9f, 2.2f, 0.5f };
    const vec3 sc{ 2.7f, 2.9f, 0.5f };
    ASSERT_TRUE(tri_covers_no_pixel(sa, sb, sc, 40, 20));

    // Missing it on y instead, so that a one-axis test could not pass this.
    const vec3 ya{ 2.0f, 2.6f, 0.5f };
    const vec3 yb{ 5.0f, 2.6f, 0.5f };
    const vec3 yc{ 3.0f, 2.9f, 0.5f };
    ASSERT_TRUE(tri_covers_no_pixel(ya, yb, yc, 40, 20));

    // Entirely off screen on either side.
    const vec3 oa{ -40.0f, -40.0f, 0.5f };
    const vec3 ob{ -30.0f, -40.0f, 0.5f };
    const vec3 oc{ -35.0f, -30.0f, 0.5f };
    ASSERT_TRUE(tri_covers_no_pixel(oa, ob, oc, 40, 20));
}

// Compare pixel-center coverage with independent double-precision edge functions.
// The long, shallow triangle exposes repeated-addition drift that can move a nearly
// horizontal edge by several pixels; direct barycentric evaluation must remain stable.
TEST(rasterize, coverage_follows_the_pixel_centre_on_a_near_horizontal_edge)
{
    constexpr int W = 320;
    constexpr int H = 32;
    Framebuffer fb(W, H, /*headless=*/true);
    fb.clear(Color{ 0, 0, 0 });

    const vec3 sa{ 2.0f, 10.2f, 0.5f };
    const vec3 sb{ 310.0f, 10.6f, 0.5f }; // top edge: 0.4 px of rise over 308 px of run
    const vec3 sc{ 150.0f, 24.4f, 0.5f };
    const vec3 white{ 1.0f, 1.0f, 1.0f };
    const vec2 uv{ 0.0f, 0.0f };
    rasterize_flat(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, uv, uv, uv, nullptr, 0.0f, 0, H - 1);

    const auto edge = [](const vec3 &p, const vec3 &q, double x, double y)
    {
        const auto px = static_cast<double>(p.x);
        const auto py = static_cast<double>(p.y);
        return ((static_cast<double>(q.x) - px) * (y - py)) - ((static_cast<double>(q.y) - py) * (x - px));
    };
    const double area = edge(sa, sb, static_cast<double>(sc.x), static_cast<double>(sc.y));
    ASSERT_TRUE(area != 0.0);

    // Pixels within TOL of an edge are the rasterizer's own rounding to make; everything
    // outside that band has one right answer. TOL is ~1/1000 of the triangle's height here,
    // four orders of magnitude below the drift this guards against.
    constexpr double TOL = 1e-3;
    int inside_lit = 0;
    for (int y = 0; y < H; y++)
    {
        for (int x = 0; x < W; x++)
        {
            const double px = x + 0.5;
            const double py = y + 0.5;
            const double w0 = edge(sb, sc, px, py) / area;
            const double w1 = edge(sc, sa, px, py) / area;
            const double w2 = edge(sa, sb, px, py) / area;
            const double m = std::min({ w0, w1, w2 });
            const bool lit = was_drawn(fb, x, y);
            if (m > TOL)
            {
                inside_lit++;
                if (!lit)
                {
                    ASSERT_FAIL("hole at (" + std::to_string(x) + "," + std::to_string(y) + ")");
                }
            }
            else if (m < -TOL && lit)
            {
                ASSERT_FAIL("bleed at (" + std::to_string(x) + "," + std::to_string(y) + ")");
            }
        }
    }
    ASSERT_TRUE(inside_lit > 1000); // the triangle really is being drawn
}
