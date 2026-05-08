#include "test.h"
#include "rasterize_test_util.h"
#include "../src/framebuffer.h"

#include <limits>

// ─── test_and_set_depth bounds ────────────────────────────────────────────────

TEST(framebuffer, test_and_set_depth_rejects_negative_x)
{
    FdRedirect rd;
    Framebuffer fb(4, 4);
    ASSERT_FALSE(fb.test_and_set_depth(-1, 0, 0.5f));
}

TEST(framebuffer, test_and_set_depth_rejects_x_at_width)
{
    FdRedirect rd;
    Framebuffer fb(4, 4);
    ASSERT_FALSE(fb.test_and_set_depth(fb.width(), 0, 0.5f));
}

TEST(framebuffer, test_and_set_depth_rejects_negative_y)
{
    FdRedirect rd;
    Framebuffer fb(4, 4);
    ASSERT_FALSE(fb.test_and_set_depth(0, -1, 0.5f));
}

TEST(framebuffer, test_and_set_depth_rejects_y_at_height)
{
    FdRedirect rd;
    Framebuffer fb(4, 4);
    ASSERT_FALSE(fb.test_and_set_depth(0, fb.height(), 0.5f));
}

// ─── set_pixel / get_pixel bounds ────────────────────────────────────────────

TEST(framebuffer, set_pixel_oob_silently_ignored)
{
    FdRedirect rd;
    Framebuffer fb(4, 4);
    // OOB writes must not corrupt adjacent in-bounds pixels.
    fb.set_pixel(-1, 0, {1, 2, 3});
    fb.set_pixel(fb.width(), 0, {1, 2, 3});
    fb.set_pixel(0, -1, {1, 2, 3});
    fb.set_pixel(0, fb.height(), {1, 2, 3});

    // In-bounds pixels should still be default (black).
    Color c = fb.get_pixel(0, 0);
    ASSERT_EQ(static_cast<int>(c.r), 0);
    ASSERT_EQ(static_cast<int>(c.g), 0);
    ASSERT_EQ(static_cast<int>(c.b), 0);

    // get_pixel OOB returns default Color{}.
    Color oob = fb.get_pixel(-1, 0);
    ASSERT_EQ(static_cast<int>(oob.r), 0);
    ASSERT_EQ(static_cast<int>(oob.g), 0);
    ASSERT_EQ(static_cast<int>(oob.b), 0);
}

// ─── clear() ─────────────────────────────────────────────────────────────────

TEST(framebuffer, clear_fills_all_pixels_with_bg)
{
    FdRedirect rd;
    Framebuffer fb(4, 4);
    fb.clear({10, 20, 30});
    for (int y = 0; y < fb.height(); ++y)
    {
        for (int x = 0; x < fb.width(); ++x)
        {
            Color c = fb.get_pixel(x, y);
            ASSERT_EQ(static_cast<int>(c.r), 10);
            ASSERT_EQ(static_cast<int>(c.g), 20);
            ASSERT_EQ(static_cast<int>(c.b), 30);
        }
    }
}

TEST(framebuffer, clear_resets_depth_to_infinity)
{
    FdRedirect rd;
    Framebuffer fb(4, 4);
    // Write a finite depth so depth[2,2] < +inf.
    ASSERT_TRUE(fb.test_and_set_depth(2, 2, 0.5f));
    // After clear, depth should be +inf again — any finite value passes.
    fb.clear();
    ASSERT_TRUE(fb.test_and_set_depth(2, 2, 0.9f));
}

// ─── resize() ────────────────────────────────────────────────────────────────

TEST(framebuffer, resize_resets_depth_to_infinity)
{
    FdRedirect rd;
    Framebuffer fb(4, 4);
    ASSERT_TRUE(fb.test_and_set_depth(1, 1, 0.3f));
    fb.resize(8, 8);
    // After resize, depth must be +inf — large finite value must pass.
    ASSERT_TRUE(fb.test_and_set_depth(1, 1, 0.999f));
}

TEST(framebuffer, resize_to_same_size_is_idempotent)
{
    FdRedirect rd;
    Framebuffer fb(6, 6);
    fb.resize(6, 6);
    // Basic operations must still work after a no-op resize.
    fb.set_pixel(3, 3, {77, 88, 99});
    Color c = fb.get_pixel(3, 3);
    ASSERT_EQ(static_cast<int>(c.r), 77);
    ASSERT_EQ(static_cast<int>(c.g), 88);
    ASSERT_EQ(static_cast<int>(c.b), 99);
    ASSERT_TRUE(fb.test_and_set_depth(3, 3, 0.5f));
}

TEST(framebuffer, resize_sequence_clears_each_time)
{
    FdRedirect rd;
    Framebuffer fb(10, 10);

    // Write something into the initial 10×10.
    fb.set_pixel(5, 5, {1, 2, 3});
    ASSERT_TRUE(fb.test_and_set_depth(5, 5, 0.1f));

    // Grow to 20×20: pixels should be default, depth +inf.
    fb.resize(20, 20);
    Color c = fb.get_pixel(5, 5);
    ASSERT_EQ(static_cast<int>(c.r), 0);
    ASSERT_TRUE(fb.test_and_set_depth(5, 5, 0.8f));

    // Write again.
    fb.set_pixel(3, 3, {9, 8, 7});
    ASSERT_TRUE(fb.test_and_set_depth(3, 3, 0.2f));

    // Shrink to 5×5: pixels default, depth +inf.
    fb.resize(5, 5);
    Color c2 = fb.get_pixel(3, 3);
    ASSERT_EQ(static_cast<int>(c2.r), 0);
    ASSERT_TRUE(fb.test_and_set_depth(3, 3, 0.7f));
}

// ─── construction edge cases ──────────────────────────────────────────────────

TEST(framebuffer, zero_size_construction_present_does_not_crash)
{
    FdRedirect rd;
    Framebuffer fb(0, 0);
    ASSERT_EQ(fb.width(), 0);
    ASSERT_EQ(fb.height(), 0);
    fb.clear();
    fb.set_pixel(0, 0, {1, 2, 3}); // OOB — silently ignored
    fb.present();                  // must not crash on empty buffer
}

TEST(framebuffer, odd_height_present_does_not_crash)
{
    // height=3: term_rows=1; bot_base for row 0 uses prow+1=1 (< 3, valid);
    // the guard `prow + 1 < m_height ? prow + 1 : prow` exercises the
    // bottom-row fallback path when height is odd.
    FdRedirect rd;
    Framebuffer fb(2, 3);
    ASSERT_EQ(fb.width(), 2);
    ASSERT_EQ(fb.height(), 3);
    fb.set_pixel(0, 2, {100, 150, 200}); // row 2 — the lone odd row
    fb.present();                        // must not crash
}

// ─── dirty-tracking path ──────────────────────────────────────────────────────

TEST(framebuffer, present_dirty_then_present_again_no_crash)
{
    // First present() takes the full-redraw path; second takes the dirty path
    // with most cells clean.  Verifies the dirty path doesn't crash or corrupt.
    FdRedirect rd;
    Framebuffer fb(4, 4);
    fb.set_pixel(0, 0, {255, 0, 0});
    fb.present(); // full-redraw path; swaps m_color ↔ m_prev_color

    fb.clear();
    fb.set_pixel(2, 2, {0, 255, 0}); // only one cell dirty vs previous frame

    // Pixel is readable before present().
    Color c = fb.get_pixel(2, 2);
    ASSERT_EQ(static_cast<int>(c.r), 0);
    ASSERT_EQ(static_cast<int>(c.g), 255);
    ASSERT_EQ(static_cast<int>(c.b), 0);

    fb.present(); // dirty-tracking path — must not crash
}
