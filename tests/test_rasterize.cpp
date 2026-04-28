#include "test.h"
#include "../src/rasterize.h"

#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <unistd.h>

// ─── helpers ──────────────────────────────────────────────────────────────────

// Redirects stdout to /dev/null. Must outlive any Framebuffer in the same scope
// so that the Framebuffer ctor/dtor ANSI escape codes are silenced.
struct FdRedirect
{
    int saved_out;
    FdRedirect()
    {
        std::fflush(stdout);
        saved_out = dup(STDOUT_FILENO);
        int dn = open("/dev/null", O_WRONLY);
        dup2(dn, STDOUT_FILENO);
        close(dn);
    }
    ~FdRedirect()
    {
        std::fflush(stdout);
        dup2(saved_out, STDOUT_FILENO);
        close(saved_out);
    }
};

// Returns true iff pixel (x,y) was drawn (stored depth < +inf).
// One-shot: mutates the depth of undrawn pixels. Do not probe the same pixel twice.
static bool was_drawn(Framebuffer &fb, int x, int y)
{
    return !fb.test_and_set_depth(x, y, std::numeric_limits<float>::max());
}

// Assert stored depth at (x,y) is within eps of D.
// Two one-shot probes — call only after all drawing is done.
static void assert_depth_near(Framebuffer &fb, int x, int y, float D, float eps)
{
    // D+eps probe does not mutate (returns false → no write).
    if (fb.test_and_set_depth(x, y, D + eps))
        ASSERT_FAIL("depth > " + std::to_string(D + eps) + " at (" +
                    std::to_string(x) + "," + std::to_string(y) + ")");
    // D-eps probe mutates stored to D-eps if it passes (returns true).
    if (!fb.test_and_set_depth(x, y, D - eps))
        ASSERT_FAIL("depth <= " + std::to_string(D - eps) + " at (" +
                    std::to_string(x) + "," + std::to_string(y) + ")");
}

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
    vec3 nrm{0.0f, 0.0f, -1.0f};
    vec3 tan{1.0f, 0.0f, 0.0f};
    Light light{};
    Material mat{};

    rasterize_phong(fb, sa, sb, sc,
                    1.0f, 1.0f, 1.0f,
                    zero, zero, zero,
                    nrm, nrm, nrm,
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
    vec3 nrm{0.0f, 0.0f, -1.0f};
    vec3 tan{1.0f, 0.0f, 0.0f};
    Light light{};
    Material mat{};

    rasterize_phong(fb, sa, sb, sc,
                    1.0f, 1.0f, 1.0f,
                    zero, zero, zero,
                    nrm, nrm, nrm,
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
