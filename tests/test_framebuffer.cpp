#include "test.h"
#include "rasterize_test_util.h"
#include "../src/framebuffer.h"

#include <limits>

// ─── test_and_set_depth bounds ────────────────────────────────────────────────

TEST(framebuffer, test_and_set_depth_rejects_negative_x)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    ASSERT_FALSE(fb.test_and_set_depth(-1, 0, 0.5f));
}

TEST(framebuffer, test_and_set_depth_rejects_x_at_width)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    ASSERT_FALSE(fb.test_and_set_depth(fb.width(), 0, 0.5f));
}

TEST(framebuffer, test_and_set_depth_rejects_negative_y)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    ASSERT_FALSE(fb.test_and_set_depth(0, -1, 0.5f));
}

TEST(framebuffer, test_and_set_depth_rejects_y_at_height)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    ASSERT_FALSE(fb.test_and_set_depth(0, fb.height(), 0.5f));
}

// ─── set_pixel / get_pixel bounds ────────────────────────────────────────────

TEST(framebuffer, set_pixel_oob_silently_ignored)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    // OOB writes must not corrupt adjacent in-bounds pixels.
    fb.set_pixel(-1, 0, { 1, 2, 3 });
    fb.set_pixel(fb.width(), 0, { 1, 2, 3 });
    fb.set_pixel(0, -1, { 1, 2, 3 });
    fb.set_pixel(0, fb.height(), { 1, 2, 3 });

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
    Framebuffer fb(4, 4, /*headless=*/true);
    fb.clear({ 10, 20, 30 });
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
    Framebuffer fb(4, 4, /*headless=*/true);
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
    Framebuffer fb(4, 4, /*headless=*/true);
    ASSERT_TRUE(fb.test_and_set_depth(1, 1, 0.3f));
    fb.resize(8, 8);
    // After resize, depth must be +inf — large finite value must pass.
    ASSERT_TRUE(fb.test_and_set_depth(1, 1, 0.999f));
}

TEST(framebuffer, resize_to_same_size_is_idempotent)
{
    FdRedirect rd;
    Framebuffer fb(6, 6, /*headless=*/true);
    fb.resize(6, 6);
    // Basic operations must still work after a no-op resize.
    fb.set_pixel(3, 3, { 77, 88, 99 });
    Color c = fb.get_pixel(3, 3);
    ASSERT_EQ(static_cast<int>(c.r), 77);
    ASSERT_EQ(static_cast<int>(c.g), 88);
    ASSERT_EQ(static_cast<int>(c.b), 99);
    ASSERT_TRUE(fb.test_and_set_depth(3, 3, 0.5f));
}

TEST(framebuffer, resize_sequence_clears_each_time)
{
    FdRedirect rd;
    Framebuffer fb(10, 10, /*headless=*/true);

    // Write something into the initial 10×10.
    fb.set_pixel(5, 5, { 1, 2, 3 });
    ASSERT_TRUE(fb.test_and_set_depth(5, 5, 0.1f));

    // Grow to 20×20: pixels should be default, depth +inf.
    fb.resize(20, 20);
    Color c = fb.get_pixel(5, 5);
    ASSERT_EQ(static_cast<int>(c.r), 0);
    ASSERT_TRUE(fb.test_and_set_depth(5, 5, 0.8f));

    // Write again.
    fb.set_pixel(3, 3, { 9, 8, 7 });
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
    Framebuffer fb(0, 0, /*headless=*/true);
    ASSERT_EQ(fb.width(), 0);
    ASSERT_EQ(fb.height(), 0);
    fb.clear();
    fb.set_pixel(0, 0, { 1, 2, 3 }); // OOB — silently ignored
    fb.present();                    // must not crash on empty buffer
}

TEST(framebuffer, odd_height_present_does_not_crash)
{
    // height=3: term_rows=1; bot_base for row 0 uses prow+1=1 (< 3, valid);
    // the guard `prow + 1 < m_height ? prow + 1 : prow` exercises the
    // bottom-row fallback path when height is odd.
    FdRedirect rd;
    Framebuffer fb(2, 3, /*headless=*/true);
    ASSERT_EQ(fb.width(), 2);
    ASSERT_EQ(fb.height(), 3);
    fb.set_pixel(0, 2, { 100, 150, 200 }); // row 2 — the lone odd row
    fb.present();                          // must not crash
}

// ─── headless mode ───────────────────────────────────────────────────────────

TEST(framebuffer, headless_dimensions_correct)
{
    // No FdRedirect needed — headless suppresses all terminal I/O.
    Framebuffer fb(10, 8, /*headless=*/true);
    ASSERT_EQ(fb.width(), 10);
    ASSERT_EQ(fb.height(), 8);
}

TEST(framebuffer, headless_pixel_ops_work)
{
    Framebuffer fb(6, 6, /*headless=*/true);
    fb.set_pixel(2, 3, { 11, 22, 33 });
    Color c = fb.get_pixel(2, 3);
    ASSERT_EQ(static_cast<int>(c.r), 11);
    ASSERT_EQ(static_cast<int>(c.g), 22);
    ASSERT_EQ(static_cast<int>(c.b), 33);
}

TEST(framebuffer, headless_clear_fills_pixels)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    fb.set_pixel(1, 1, { 99, 99, 99 });
    fb.clear({ 7, 8, 9 });
    Color c = fb.get_pixel(1, 1);
    ASSERT_EQ(static_cast<int>(c.r), 7);
    ASSERT_EQ(static_cast<int>(c.g), 8);
    ASSERT_EQ(static_cast<int>(c.b), 9);
}

TEST(framebuffer, headless_depth_ops_work)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    ASSERT_TRUE(fb.test_and_set_depth(1, 1, 0.5f));
    ASSERT_FALSE(fb.test_and_set_depth(1, 1, 0.9f));
    ASSERT_TRUE(fb.test_and_set_depth(1, 1, 0.1f));
}

TEST(framebuffer, headless_clear_resets_depth)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    ASSERT_TRUE(fb.test_and_set_depth(2, 2, 0.5f));
    fb.clear();
    ASSERT_TRUE(fb.test_and_set_depth(2, 2, 0.9f)); // depth reset to +inf
}

// ─── dirty-tracking path ──────────────────────────────────────────────────────

TEST(framebuffer, present_dirty_then_present_again_no_crash)
{
    // First present() takes the full-redraw path; second takes the dirty path
    // with most cells clean.  Verifies the dirty path doesn't crash or corrupt.
    FdRedirect rd;
    Framebuffer fb(4, 4, /*headless=*/true);
    fb.set_pixel(0, 0, { 255, 0, 0 });
    fb.present(); // full-redraw path; swaps m_color ↔ m_prev_color

    fb.clear();
    fb.set_pixel(2, 2, { 0, 255, 0 }); // only one cell dirty vs previous frame

    // Pixel is readable before present().
    Color c = fb.get_pixel(2, 2);
    ASSERT_EQ(static_cast<int>(c.r), 0);
    ASSERT_EQ(static_cast<int>(c.g), 255);
    ASSERT_EQ(static_cast<int>(c.b), 0);

    fb.present(); // dirty-tracking path — must not crash
}

// ─── HUD ─────────────────────────────────────────────────────────────────────
// CaptureStdout redirects stdout to a temp file for the duration of its lifetime,
// then reads back the captured bytes. Used to verify HUD presence/absence.

struct CaptureStdout
{
    std::FILE *tmp;
    int saved_out;

    CaptureStdout() : tmp(std::tmpfile()), saved_out(test_dup(TEST_STDOUT))
    {
        std::fflush(stdout);
#ifdef _WIN32
        test_dup2(_fileno(tmp), TEST_STDOUT);
#else
        test_dup2(fileno(tmp), TEST_STDOUT);
#endif
    }

    ~CaptureStdout()
    {
        std::fflush(stdout);
        test_dup2(saved_out, TEST_STDOUT);
        test_close(saved_out);
        std::fclose(tmp);
    }

    std::string read() const
    {
        std::fflush(stdout);
        std::rewind(tmp);
        std::string out;
        char buf[4096];
        for (;;)
        {
            const size_t n = std::fread(buf, 1, sizeof(buf), tmp);
            if (n == 0)
            {
                break;
            }
            out.append(buf, n);
        }
        return out;
    }
};

TEST(framebuffer, hud_text_appears_in_present_output)
{
    Framebuffer fb(10, 4, /*headless=*/true);
    CaptureStdout cap;
    fb.set_hud("RASTERMINAL_HUD");
    fb.present();
    ASSERT_TRUE(cap.read().find("RASTERMINAL_HUD") != std::string::npos);
}

TEST(framebuffer, hud_empty_emits_no_hud_escape)
{
    // HUD is empty by default — the !m_hud.empty() block must be skipped,
    // so \033[?7l (disable-autowrap, unique to the HUD block) must not appear.
    Framebuffer fb(10, 4, /*headless=*/true);
    CaptureStdout cap;
    fb.present();
    ASSERT_TRUE(cap.read().find("\033[?7l") == std::string::npos);
}

TEST(framebuffer, hud_cleared_after_set_is_omitted)
{
    Framebuffer fb(10, 4, /*headless=*/true);
    CaptureStdout cap;

    fb.set_hud("MAGIC_MARKER_A");
    fb.present(); // frame 1: HUD block fires
    fb.clear();
    fb.set_hud("");
    fb.present(); // frame 2: HUD is empty, block must not fire

    const std::string out = cap.read();

    // Frame 1's HUD content is present in the combined output.
    ASSERT_TRUE(out.find("MAGIC_MARKER_A") != std::string::npos);
    // HUD escape \033[?7l appears exactly once (frame 1 only; not frame 2).
    const size_t first_pos = out.find("\033[?7l");
    ASSERT_TRUE(first_pos != std::string::npos);
    ASSERT_TRUE(out.find("\033[?7l", first_pos + 1) == std::string::npos);
}

// ─── incremental skip path ────────────────────────────────────────────────────

TEST(framebuffer, incremental_single_skip_emits_cursor_advance)
{
    // After the first present() establishes m_prev_color, a second present() where
    // a dirty cell starts a row and exactly one unchanged cell follows must emit
    // \033[C (single-step cursor advance, not \033[1C) for the skip.
    Framebuffer fb(4, 2, /*headless=*/true);
    CaptureStdout cap;
    fb.present(); // full-redraw — establishes m_prev_color = all black

    fb.clear();
    fb.set_pixel(0, 0, { 100, 0, 0 }); // col 0 dirty
    fb.set_pixel(2, 0, { 0, 100, 0 }); // col 2 dirty; col 1 unchanged → skip=1
    fb.present();                      // incremental path fires skip=1 branch

    ASSERT_TRUE(cap.read().find("\033[C") != std::string::npos);
}

TEST(framebuffer, incremental_multi_skip_emits_counted_cursor_advance)
{
    // When a dirty cell starts a row and multiple unchanged columns follow, the
    // skip path must emit \033[NC with an explicit count, not the bare \033[C form.
    Framebuffer fb(5, 2, /*headless=*/true);
    CaptureStdout cap;
    fb.present(); // full-redraw

    fb.clear();
    fb.set_pixel(0, 0, { 100, 0, 0 }); // col 0 dirty; cols 1-4 unchanged → skip=4
    fb.present();                      // incremental path fires skip>1 branch

    ASSERT_TRUE(cap.read().find("\033[4C") != std::string::npos);
}

// ─── additional incremental / emit_cell paths ────────────────────────────────

TEST(framebuffer, present_incremental_entirely_clean_row_emits_no_cursor_pos)
{
    // Incremental path: when no cell in a row changed, row_started stays false
    // and no cursor-positioning or cell output is emitted for that row.
    // CaptureStdout always rewinds on read(), so we count occurrences across
    // both frames: \033[1;1H must appear exactly once (from the full-redraw),
    // not twice, proving the clean incremental frame added none.
    Framebuffer fb(4, 2, /*headless=*/true);
    CaptureStdout cap;
    fb.present(); // frame 1: full-redraw — emits \033[1;1H once
    fb.present(); // frame 2: entirely clean row — must not emit another \033[1;1H

    const std::string out = cap.read();
    const size_t first = out.find("\033[1;1H");
    ASSERT_TRUE(first != std::string::npos);                            // frame 1 emitted it
    ASSERT_TRUE(out.find("\033[1;1H", first + 1) == std::string::npos); // frame 2 did not
}

TEST(framebuffer, present_incremental_dirty_at_last_column)
{
    // Dirty cell at col m_width-1: after emitting it col==m_width, the outer
    // while exits immediately — the skip branch is never entered.
    Framebuffer fb(4, 2, /*headless=*/true);
    CaptureStdout cap;
    fb.present(); // full-redraw
    cap.read();   // drain

    fb.set_pixel(3, 0, { 200, 0, 0 }); // only last column dirty
    fb.present();                      // must not crash

    const std::string out = cap.read();
    ASSERT_TRUE(out.find("\033[1;4H") != std::string::npos); // cursor jumped to last col
    ASSERT_TRUE(out.find("\033[C") == std::string::npos);    // no skip advance emitted
}

TEST(framebuffer, present_height_1_does_not_crash)
{
    // height=1 → term_rows=0; the cell loop never runs.
    // The ternary guard `prow+1 < m_height ? prow+1 : prow` is unreachable dead
    // code for any height, but this degenerate size must still produce a valid call.
    FdRedirect rd;
    Framebuffer fb(4, 1, /*headless=*/true);
    fb.set_pixel(0, 0, { 50, 50, 50 });
    fb.present(); // must not crash
}

TEST(framebuffer, present_after_resize_forces_full_redraw)
{
    // resize() sets m_force_redraw=true; the next present() must take the
    // full-redraw path and emit cursor positioning for every term row.
    // Start with a 2-pixel-high FB (1 term row); frames 1-2 only emit \033[1;1H.
    // Resize to 4-pixel-high (2 term rows); frame 3's full-redraw must emit
    // \033[2;1H — something frames 1-2 never produced.
    Framebuffer fb(4, 2, /*headless=*/true);
    CaptureStdout cap;
    fb.present(); // frame 1: full-redraw (1 term row — only \033[1;1H)
    fb.present(); // frame 2: incremental, clean (no cursor pos)

    fb.resize(4, 4); // sets m_force_redraw=true; now 2 term rows
    fb.present();    // frame 3: full-redraw must emit \033[2;1H

    ASSERT_TRUE(cap.read().find("\033[2;1H") != std::string::npos);
}

TEST(framebuffer, emit_cell_top_eq_bot_known_bg_suppresses_sgr)
{
    // Two adjacent cells with top==bot at the same color: the first emits the
    // bg SGR; the second suppresses it (bg_change=false) and emits only a space.
    Framebuffer fb(2, 2, /*headless=*/true);
    CaptureStdout cap;

    fb.clear({ 100, 150, 200 });
    fb.present(); // full-redraw; both cells have top==bot=={100,150,200}

    const std::string out = cap.read();
    const std::string sgr = "\033[48;2;100;150;200m";
    const size_t first = out.find(sgr);
    ASSERT_TRUE(first != std::string::npos);                    // emitted at least once
    ASSERT_TRUE(out.find(sgr, first + 1) == std::string::npos); // suppressed for second cell
}

// ─── write_byte() LUT boundary values ────────────────────────────────────────

TEST(framebuffer, write_byte_lut_boundary_values)
{
    // write_byte() has three ranges: [0-9] (1 digit), [10-99] (2 digits),
    // [100-255] (3 digits).  Exercise the start and end of each range by using
    // the value as the R channel of a pixel whose top != bot so that the fg SGR
    // \033[38;2;R;G;Bm is emitted on the full-redraw present().
    const uint8_t vals[] = { 0, 9, 10, 99, 100, 255 };
    for (uint8_t v : vals)
    {
        Framebuffer fb(1, 2, /*headless=*/true);
        CaptureStdout cap;
        fb.set_pixel(0, 0, { v, 1, 2 }); // top != bot (bot stays default black)
        fb.present();
        const std::string out = cap.read();
        // When fg and bg both change, the code emits a combined sequence
        // \033[38;2;R;G;B;48;2;...m — match the fg prefix up to the separator.
        char expected[32];
        std::snprintf(expected, sizeof(expected), "\033[38;2;%d;1;2;", static_cast<int>(v));
        ASSERT_TRUE(out.find(expected) != std::string::npos);
    }
}

// ─── all-cells-dirty incremental frame ───────────────────────────────────────

TEST(framebuffer, present_all_cells_dirty_skip_never_fires)
{
    // After the first full-redraw (m_prev_color = all-black), clearing to a
    // non-black color makes every cell dirty on the second present().
    // The incremental path runs but the skip branch is never entered.
    Framebuffer fb(4, 4, /*headless=*/true);
    CaptureStdout cap;
    fb.present(); // full-redraw; establishes m_prev_color = all-black

    fb.clear({ 200, 100, 50 }); // every cell now differs from m_prev_color
    fb.present();               // incremental path; all cells dirty; skip never fires

    ASSERT_TRUE(cap.read().find("\033[48;2;200;100;50m") != std::string::npos);
}

// ─── non-headless ctor/dtor ───────────────────────────────────────────────────

TEST(framebuffer, non_headless_ctor_dtor_emits_alternate_screen_escapes)
{
    // headless=false triggers the alternate-screen enter (ctor) and exit (dtor)
    // ANSI sequences, which are normally suppressed by headless=true in all other
    // tests.  CaptureStdout captures both in one buffer.
    CaptureStdout cap;
    {
        Framebuffer fb(1, 2, /*headless=*/false);
    } // destructor runs here, emitting the exit sequence
    const std::string out = cap.read();
    ASSERT_TRUE(out.find("\033[?1049h") != std::string::npos); // enter alternate screen
    ASSERT_TRUE(out.find("\033[?1049l") != std::string::npos); // exit alternate screen
}
