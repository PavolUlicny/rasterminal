#include "tests/test.h"
#include "tests/rasterize_test_util.h"
#include "src/framebuffer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace
{
    // Counts cursor advances of both forms, bare \033[C and counted \033[NC. Searching for the
    // literal "\033[C" would miss "\033[3C", so an "emitted no advance" assertion must count.
    size_t count_cursor_advances(const std::string &s)
    {
        size_t n = 0;
        for (size_t i = 0; i + 2 < s.size(); i++)
        {
            if (s[i] != '\033' || s[i + 1] != '[')
            {
                continue;
            }
            size_t j = i + 2;
            while (j < s.size() && s[j] >= '0' && s[j] <= '9')
            {
                j++;
            }
            if (j < s.size() && s[j] == 'C')
            {
                n++;
            }
        }
        return n;
    }
} // namespace

// ─── get_pixel / depth_at bounds ──────────────────────────────────────────────

TEST(framebuffer, get_pixel_oob_returns_default)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    // get_pixel OOB returns default Color{} (no crash, no read past the buffer).
    Color oob = fb.get_pixel(-1, 0);
    ASSERT_EQ(static_cast<int>(oob.r), 0);
    ASSERT_EQ(static_cast<int>(oob.g), 0);
    ASSERT_EQ(static_cast<int>(oob.b), 0);
    // In-bounds default is also black (sanity that the read maps correctly).
    Color in = fb.get_pixel(0, 0);
    ASSERT_EQ(static_cast<int>(in.r), 0);
}

TEST(framebuffer, depth_at_oob_returns_infinity)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    const float inf = std::numeric_limits<float>::infinity();
    ASSERT_TRUE(fb.depth_at(-1, 0) == inf);
    ASSERT_TRUE(fb.depth_at(fb.width(), 0) == inf);
    ASSERT_TRUE(fb.depth_at(0, -1) == inf);
    ASSERT_TRUE(fb.depth_at(0, fb.height()) == inf);
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
    (void)fb.commit_pixel(2, 2, 0.5f, { 0, 0, 0 });
    ASSERT_TRUE(fb.depth_at(2, 2) == 0.5f);
    // After clear, depth must be +inf again.
    fb.clear();
    ASSERT_TRUE(fb.depth_at(2, 2) == std::numeric_limits<float>::infinity());
}

// ─── resize() ────────────────────────────────────────────────────────────────

TEST(framebuffer, resize_resets_depth_to_infinity)
{
    FdRedirect rd;
    Framebuffer fb(4, 4, /*headless=*/true);
    (void)fb.commit_pixel(1, 1, 0.3f, { 0, 0, 0 });
    fb.resize(8, 8);
    // After resize, depth must be +inf.
    ASSERT_TRUE(fb.depth_at(1, 1) == std::numeric_limits<float>::infinity());
}

TEST(framebuffer, resize_to_same_size_is_idempotent)
{
    FdRedirect rd;
    Framebuffer fb(6, 6, /*headless=*/true);
    fb.resize(6, 6);
    // Basic operations must still work after a no-op resize.
    (void)fb.commit_pixel(3, 3, 0.5f, { 77, 88, 99 });
    Color c = fb.get_pixel(3, 3);
    ASSERT_EQ(static_cast<int>(c.r), 77);
    ASSERT_EQ(static_cast<int>(c.g), 88);
    ASSERT_EQ(static_cast<int>(c.b), 99);
    ASSERT_TRUE(fb.depth_at(3, 3) == 0.5f);
}

TEST(framebuffer, resize_sequence_clears_each_time)
{
    FdRedirect rd;
    Framebuffer fb(10, 10, /*headless=*/true);

    // Write something into the initial 10×10.
    (void)fb.commit_pixel(5, 5, 0.1f, { 1, 2, 3 });

    // Grow to 20×20: pixels should be default, depth +inf.
    fb.resize(20, 20);
    Color c = fb.get_pixel(5, 5);
    ASSERT_EQ(static_cast<int>(c.r), 0);
    ASSERT_TRUE(fb.depth_at(5, 5) == std::numeric_limits<float>::infinity());

    // Write again.
    (void)fb.commit_pixel(3, 3, 0.2f, { 9, 8, 7 });

    // Shrink to 5×5: pixels default, depth +inf.
    fb.resize(5, 5);
    Color c2 = fb.get_pixel(3, 3);
    ASSERT_EQ(static_cast<int>(c2.r), 0);
    ASSERT_TRUE(fb.depth_at(3, 3) == std::numeric_limits<float>::infinity());
}

// ─── construction edge cases ──────────────────────────────────────────────────

TEST(framebuffer, zero_size_construction_present_does_not_crash)
{
    FdRedirect rd;
    Framebuffer fb(0, 0, /*headless=*/true);
    ASSERT_EQ(fb.width(), 0);
    ASSERT_EQ(fb.height(), 0);
    fb.clear();
    fb.present(); // must not crash on empty buffer
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
    (void)fb.commit_pixel(0, 2, 0.5f, { 100, 150, 200 }); // row 2 — the lone odd row
    fb.present();                                         // must not crash
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
    (void)fb.commit_pixel(2, 3, 0.5f, { 11, 22, 33 });
    Color c = fb.get_pixel(2, 3);
    ASSERT_EQ(static_cast<int>(c.r), 11);
    ASSERT_EQ(static_cast<int>(c.g), 22);
    ASSERT_EQ(static_cast<int>(c.b), 33);
}

TEST(framebuffer, headless_clear_fills_pixels)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    (void)fb.commit_pixel(1, 1, 0.5f, { 99, 99, 99 });
    fb.clear({ 7, 8, 9 });
    Color c = fb.get_pixel(1, 1);
    ASSERT_EQ(static_cast<int>(c.r), 7);
    ASSERT_EQ(static_cast<int>(c.g), 8);
    ASSERT_EQ(static_cast<int>(c.b), 9);
}

// commit_pixel's strict-< depth gate, single-threaded (multithread_depth_color_race in
// test_rasterize.cpp covers it under contention).
TEST(framebuffer, commit_pixel_depth_gate)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    ASSERT_TRUE(fb.commit_pixel(1, 1, 0.5f, { 10, 0, 0 }));  // fresh (+inf): wins
    ASSERT_FALSE(fb.commit_pixel(1, 1, 0.9f, { 0, 10, 0 })); // deeper: loses
    ASSERT_FALSE(fb.commit_pixel(1, 1, 0.5f, { 0, 0, 10 })); // equal: not strictly less, loses
    ASSERT_TRUE(fb.commit_pixel(1, 1, 0.1f, { 0, 10, 0 }));  // shallower: wins
    ASSERT_TRUE(fb.depth_at(1, 1) == 0.1f);
    Color c = fb.get_pixel(1, 1); // last winner's colour
    ASSERT_EQ(static_cast<int>(c.g), 10);
}

TEST(framebuffer, headless_clear_resets_depth)
{
    Framebuffer fb(4, 4, /*headless=*/true);
    (void)fb.commit_pixel(2, 2, 0.5f, { 0, 0, 0 });
    fb.clear();
    ASSERT_TRUE(fb.depth_at(2, 2) == std::numeric_limits<float>::infinity()); // reset to +inf
}

// ─── dirty-tracking path ──────────────────────────────────────────────────────

TEST(framebuffer, present_dirty_then_present_again_no_crash)
{
    // First present() takes the full-redraw path; second takes the dirty path
    // with most cells clean.  Verifies the dirty path doesn't crash or corrupt.
    FdRedirect rd;
    Framebuffer fb(4, 4, /*headless=*/true);
    (void)fb.commit_pixel(0, 0, 0.5f, { 255, 0, 0 });
    fb.present(); // full-redraw path; swaps m_color ↔ m_prev_color

    fb.clear();
    (void)fb.commit_pixel(2, 2, 0.5f, { 0, 255, 0 }); // only one cell dirty vs previous frame

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

namespace
{
    struct CaptureStdout
    {
        std::FILE *tmp;
        int saved_out;

        CaptureStdout() : tmp(std::tmpfile()), saved_out(test_dup(TEST_STDOUT))
        {
            ASSERT_TRUE(tmp != nullptr);
            std::fflush(stdout);
            test_dup2(test_fileno(tmp), TEST_STDOUT);
        }

        ~CaptureStdout()
        {
            std::fflush(stdout);
            if (saved_out >= 0)
            {
                test_dup2(saved_out, TEST_STDOUT);
                test_close(saved_out);
            }
            std::fclose(tmp);
        }

        CaptureStdout(const CaptureStdout &) = delete;
        CaptureStdout &operator=(const CaptureStdout &) = delete;
        CaptureStdout(CaptureStdout &&) = delete;
        CaptureStdout &operator=(CaptureStdout &&) = delete;

        [[nodiscard]] std::string read() const
        {
            std::fflush(stdout);
            // clearerr + fseek instead of rewind: writes via the dup'd fd bypass the FILE*
            // buffer so the stream's EOF/error flags may be stale; clearerr resets them, and
            // fseek (unlike rewind) reports failure if the file is genuinely broken.
            std::clearerr(tmp);
            if (std::fseek(tmp, 0, SEEK_SET) != 0)
            {
                return {};
            }
            std::string out;
            char buf[4096];
            for (;;)
            {
                // clearerr+fseek above resets state, but the analyzer can't model that
                // across the fd-write/FILE*-read mismatch.
                const size_t n = std::fread(buf, 1, sizeof(buf), tmp); // NOLINT(clang-analyzer-unix.Stream)
                if (n == 0)
                {
                    break;
                }
                out.append(buf, n);
            }
            return out;
        }
    };
} // namespace

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

namespace
{
    // Runs the full-redraw frame that seeds m_prev_color, discarding its bytes in a capture
    // scope of its own. The incremental frame under test is then the ONLY thing in the caller's
    // capture: asserting across both frames is a trap, since the full redraw positions every row
    // and emits no advances, so it can satisfy an assertion the incremental frame never earns.
    void seed_prev_color(Framebuffer &fb)
    {
        CaptureStdout warm;
        fb.present();
        (void)warm.read();
    }
} // namespace

TEST(framebuffer, incremental_single_skip_emits_cursor_advance)
{
    // After the first present() establishes m_prev_color, a second present() where
    // a dirty cell starts a row and exactly one unchanged cell follows must emit
    // \033[C (single-step cursor advance, not \033[1C) for the skip.
    Framebuffer fb(4, 2, /*headless=*/true);
    seed_prev_color(fb);

    CaptureStdout cap;
    fb.clear();
    (void)fb.commit_pixel(0, 0, 0.5f, { 100, 0, 0 }); // col 0 dirty
    (void)fb.commit_pixel(2, 0, 0.5f, { 0, 100, 0 }); // col 2 dirty; col 1 unchanged → skip=1
    fb.present();                                     // incremental path fires skip=1 branch

    const std::string inc = cap.read();
    ASSERT_EQ(count_cursor_advances(inc), static_cast<size_t>(1)); // col 1 only; col 3's tail is dropped
    ASSERT_TRUE(inc.find("\033[C") != std::string::npos);          // bare form, not \033[1C
}

TEST(framebuffer, incremental_multi_skip_emits_counted_cursor_advance)
{
    // Multiple unchanged columns between two dirty ones must emit \033[NC with an explicit
    // count, not the bare \033[C. The clean run has to end at a dirty cell rather than the
    // row edge: a run reaching the edge emits nothing, so a 5-wide framebuffer tests nothing.
    Framebuffer fb(6, 2, /*headless=*/true);
    seed_prev_color(fb);

    CaptureStdout cap;
    fb.clear();
    (void)fb.commit_pixel(0, 0, 0.5f, { 100, 0, 0 }); // col 0 dirty
    (void)fb.commit_pixel(5, 0, 0.5f, { 0, 100, 0 }); // col 5 dirty; cols 1-4 unchanged → skip=4
    fb.present();                                     // incremental path fires skip>1 branch

    const std::string inc = cap.read();
    ASSERT_EQ(count_cursor_advances(inc), static_cast<size_t>(1));
    ASSERT_TRUE(inc.find("\033[4C") != std::string::npos);
}

// A clean run reaching the row end must emit no cursor advance of either form: the cursor is
// hidden and never read back, and whatever writes next (a later dirty row, the HUD, or the next
// frame) positions absolutely. Without this the escape is dead bytes on every row ending in
// background. Mode-independent, so truecolor covers both.
TEST(framebuffer, present_no_cursor_advance_for_clean_run_to_row_end)
{
    Framebuffer fb(4, 2, /*headless=*/true);
    seed_prev_color(fb);

    CaptureStdout cap;
    fb.clear();
    (void)fb.commit_pixel(0, 0, 0.5f, { 200, 0, 0 }); // col 0 dirty; cols 1..3 clean to the row end
    fb.present();

    ASSERT_EQ(count_cursor_advances(cap.read()), static_cast<size_t>(0));
}

// Dropping the end-of-row advance is only safe because a later dirty row still positions
// absolutely. Every other incremental test is a single terminal row, so nothing else pins that:
// here row 0 ends in a dropped clean run and row 1 must still emit its own \033[2;1H.
TEST(framebuffer, present_next_dirty_row_repositions_after_dropped_advance)
{
    Framebuffer fb(4, 4, /*headless=*/true); // 4 px tall = 2 terminal rows
    seed_prev_color(fb);

    CaptureStdout cap;
    fb.clear();
    (void)fb.commit_pixel(0, 0, 0.5f, { 200, 0, 0 }); // row 0, col 0; cols 1..3 clean to the row end
    (void)fb.commit_pixel(0, 2, 0.5f, { 0, 200, 0 }); // row 1, col 0 (pixel y=2 → terminal row 1)
    fb.present();

    const std::string inc = cap.read();
    ASSERT_EQ(count_cursor_advances(inc), static_cast<size_t>(0)); // both rows' tails dropped
    ASSERT_TRUE(inc.find("\033[1;1H") != std::string::npos);
    ASSERT_TRUE(inc.find("\033[2;1H") != std::string::npos); // row 1 still positioned absolutely
}

// The HUD is the most common thing that follows a dropped advance in real use: it must still
// position itself absolutely on its own row rather than continuing from the pixel cursor.
TEST(framebuffer, present_hud_positions_absolutely_after_dropped_advance)
{
    Framebuffer fb(4, 2, /*headless=*/true); // 2 px tall = 1 pixel row; HUD lands on row 2
    seed_prev_color(fb);

    CaptureStdout cap;
    fb.clear();
    (void)fb.commit_pixel(0, 0, 0.5f, { 200, 0, 0 }); // col 0 dirty; cols 1..3 clean to the row end
    fb.set_hud("HUD");
    fb.present();

    const std::string inc = cap.read();
    ASSERT_EQ(count_cursor_advances(inc), static_cast<size_t>(0));
    ASSERT_TRUE(inc.find("\033[2;1H") != std::string::npos); // HUD row, absolute
    ASSERT_TRUE(inc.find("HUD") != std::string::npos);
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
    fb.present();     // full-redraw
    (void)cap.read(); // drain

    (void)fb.commit_pixel(3, 0, 0.5f, { 200, 0, 0 }); // only last column dirty
    fb.present();                                     // must not crash

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
    (void)fb.commit_pixel(0, 0, 0.5f, { 50, 50, 50 });
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
        (void)fb.commit_pixel(0, 0, 0.5f, { v, 1, 2 }); // top != bot (bot stays default black)
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

// ─── tonemap (soft-knee highlight rolloff) ────────────────────────────────────
// The display transform applied once per shaded contribution before quantization. Knee 0.7.

TEST(tonemap, identity_below_and_at_knee)
{
    // Everything up to the knee passes through untouched, so well-exposed and textured
    // surfaces are byte-identical to the pre-tonemap pipeline.
    ASSERT_NEAR(tonemap_channel(0.0f), 0.0f, 1e-6f);
    ASSERT_NEAR(tonemap_channel(0.3f), 0.3f, 1e-6f);
    ASSERT_NEAR(tonemap_channel(0.5f), 0.5f, 1e-6f);
    ASSERT_NEAR(tonemap_channel(0.7f), 0.7f, 1e-6f); // exactly the knee
}

TEST(tonemap, locks_agreed_curve_above_knee)
{
    // Pin the chosen rolloff so a future curve change is caught. Values: knee 0.7, range 0.3,
    // out = 0.7 + 0.3*(1 - exp(-(x-0.7)/0.3)).
    ASSERT_NEAR(tonemap_channel(1.0f), 0.889636f, 1e-4f);
    ASSERT_NEAR(tonemap_channel(1.17f), 0.937406f, 1e-4f);
    ASSERT_NEAR(tonemap_channel(1.5f), 0.979155f, 1e-4f);
    ASSERT_NEAR(tonemap_channel(2.0f), 0.996060f, 1e-4f);
}

TEST(tonemap, monotonic_increasing)
{
    float prev = -1.0f;
    for (int i = 0; i <= 400; ++i)
    {
        const float x = static_cast<float>(i) * 0.01f; // 0.0 .. 4.0
        const float y = tonemap_channel(x);
        ASSERT_TRUE(y > prev); // strictly increasing across the whole domain
        prev = y;
    }
}

TEST(tonemap, bounded_by_one)
{
    // Output never exceeds 1.0, so the downstream vec3_to_color clamp is a no-op (no overflow in the
    // uint8 cast). Across the realistic lit range (up to ~4x) it stays strictly below 1.0, so distinct
    // bright values map to distinct displayed values: the HDR gradient the hard clamp destroyed.
    ASSERT_TRUE(tonemap_channel(2.0f) < 1.0f);
    ASSERT_TRUE(tonemap_channel(4.0f) < 1.0f);
    // Very large inputs saturate to exactly 1.0 in float (the math asymptote rounds up); still bounded.
    ASSERT_TRUE(tonemap_channel(1.0e6f) <= 1.0f);
    ASSERT_TRUE(tonemap_channel(1.0e6f) > 0.7f);
}

TEST(tonemap, slope_continuous_at_knee)
{
    // Numeric derivative just above the knee must be ~1 (matching the identity slope below it),
    // i.e. no crease where the rolloff begins.
    const float h = 1.0e-3f;
    const float slope = (tonemap_channel(0.7f + h) - tonemap_channel(0.7f)) / h;
    ASSERT_NEAR(slope, 1.0f, 2.0e-3f);
}

TEST(tonemap, vec3_applies_per_channel)
{
    const vec3 out = tonemap(vec3{ 0.5f, 1.0f, 2.0f });
    ASSERT_NEAR(out.x, 0.5f, 1e-6f);      // below knee: unchanged
    ASSERT_NEAR(out.y, 0.889636f, 1e-4f); // 1.0 rolls off
    ASSERT_NEAR(out.z, 0.996060f, 1e-4f); // 2.0 rolls off further
}

// ─── quantize_256 (perceptual CIELAB nearest via the 64^3 LUT) ────────────────
// The RGB->palette-index map used by present() in ColorMode::Palette256. Cube 16..231,
// grey ramp 232..255; never returns a 0..15 system colour. Each colour takes the
// deltaE76-nearest entry for its 64^3 cell's centre, except that a cell containing an
// exact palette colour maps to that colour.

namespace
{
    // The 240 addressable xterm-256 palette RGB values, indexed 0..239 (palette index 16 + j).
    Color palette_entry(int j)
    {
        if (j < 216)
        {
            auto val = [](int l) { return static_cast<uint8_t>(l == 0 ? 0 : 55 + (40 * l)); };
            return { val(j / 36), val((j / 6) % 6), val(j % 6) };
        }
        const auto v = static_cast<uint8_t>(8 + (10 * (j - 216)));
        return { v, v, v };
    }

    // Independent double-precision CIELAB (D65) reference for validating the float LUT builder.
    void cielab_ref(double r8, double g8, double b8, double out[3])
    {
        auto lin = [](double v)
        {
            v /= 255.0;
            return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        const double r = lin(r8);
        const double g = lin(g8);
        const double b = lin(b8);
        const double x = ((0.4124564 * r) + (0.3575761 * g) + (0.1804375 * b)) / 0.95047;
        const double y = (0.2126729 * r) + (0.7151522 * g) + (0.0721750 * b);
        const double z = ((0.0193339 * r) + (0.1191920 * g) + (0.9503041 * b)) / 1.08883;
        auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (((24389.0 / 27.0) * t) + 16.0) / 116.0; };
        const double fx = f(x);
        const double fy = f(y);
        const double fz = f(z);
        out[0] = (116.0 * fy) - 16.0;
        out[1] = 500.0 * (fx - fy);
        out[2] = 200.0 * (fy - fz);
    }
} // namespace

TEST(framebuffer, quantize_256_corners)
{
    // Pure black/white live in the cube (16 / 231), not on the grey ramp (which is 8..238).
    ASSERT_EQ(static_cast<int>(quantize_256({ 0, 0, 0 })), 16);
    ASSERT_EQ(static_cast<int>(quantize_256({ 255, 255, 255 })), 231);
}

TEST(framebuffer, quantize_256_cube_exact)
{
    // Colours that land exactly on cube levels {0,95,135,175,215,255} pick their cube cell.
    ASSERT_EQ(static_cast<int>(quantize_256({ 95, 135, 175 })), 67); // 16 + 36*1 + 6*2 + 3
    ASSERT_EQ(static_cast<int>(quantize_256({ 255, 0, 0 })), 196);   // 16 + 36*5
    ASSERT_EQ(static_cast<int>(quantize_256({ 0, 255, 0 })), 46);    // 16 + 6*5
    ASSERT_EQ(static_cast<int>(quantize_256({ 0, 0, 255 })), 21);    // 16 + 5
}

TEST(framebuffer, quantize_256_grey_ramp_exact)
{
    // Exact ramp values (8 + 10*k) pick index 232 + k; {160,160,160} is not one (its cell centre
    // resolves to ramp 158 = 247 via the scan). 233 and 247 also pin the hard-coded HUD bg/fg
    // palette constants (RGB {18,18,18} / {160,160,160}) at the emit site in framebuffer.cpp,
    // which a static_assert can no longer reach now that the table is runtime-built.
    ASSERT_EQ(static_cast<int>(quantize_256({ 8, 8, 8 })), 232);
    ASSERT_EQ(static_cast<int>(quantize_256({ 18, 18, 18 })), 233);
    ASSERT_EQ(static_cast<int>(quantize_256({ 238, 238, 238 })), 255);
    ASSERT_EQ(static_cast<int>(quantize_256({ 160, 160, 160 })), 247);
}

TEST(framebuffer, quantize_256_grey_beats_cube)
{
    // A near-grey colour is perceptually nearest the ramp (grey 128 here, via the centre-nearest
    // scan: this colour's cell (32,32,31) holds no palette entry, so the exact overwrite is not
    // what decides it).
    ASSERT_EQ(static_cast<int>(quantize_256({ 130, 128, 126 })), 244);
}

TEST(framebuffer, quantize_256_cube_beats_grey)
{
    // A saturated colour is perceptually nearest a cube cell, (175,0,0) here.
    ASSERT_EQ(static_cast<int>(quantize_256({ 200, 50, 50 })), 124);
}

TEST(framebuffer, quantize_256_dark_colors_keep_hue)
{
    // The regression the CIELAB metric exists to fix: dark foliage green and terracotta brown
    // used to collapse to the grey ramp under squared-RGB distance (the palette cube has no
    // chromatic entry below 95 per channel, so nearly all dark colours were nearer a grey).
    // deltaE76 keeps their hue. Margins to the runner-up entry are >= 0.47 deltaE (~4 orders
    // above float noise), so the pins are stable across libm implementations.
    ASSERT_EQ(static_cast<int>(quantize_256({ 60, 90, 60 })), 65); // cube (95,135,95), green
    ASSERT_EQ(static_cast<int>(quantize_256({ 90, 55, 40 })), 95); // cube (135,95,95), rust
}

TEST(framebuffer, quantize_256_palette_exact_idempotent)
{
    // Every addressable palette colour must map to itself (the builder's exact-cell override):
    // palette-exact pixels, backgrounds above all, must never wash to a neighbouring entry.
    for (int j = 0; j < 240; ++j)
    {
        ASSERT_EQ(static_cast<int>(quantize_256(palette_entry(j))), 16 + j);
    }
}

TEST(framebuffer, quantize_256_never_system_color)
{
    // The result must always be an addressable, theme-independent palette entry (16..255), never
    // a 0..15 system colour. The table is finite, so sweep every one of its 64^3 cells.
    const auto &lut = quant256_lut();
    for (const uint8_t entry : lut)
    {
        ASSERT_TRUE(entry >= 16);
    }
}

TEST(framebuffer, quantize_256_matches_cielab_reference)
{
    // Ground truth for the whole table against an independent double-precision CIELAB
    // implementation. For every 64^3 cell: a cell containing an exact palette colour must map to
    // it; any other cell's entry must achieve the true minimum squared deltaE76 to the cell
    // centre within a small epsilon (distances are compared, not indices, so a float-vs-double
    // near-tie between two entries cannot flake the test). Deliberately exhaustive, not sampled:
    // ~64 ms plain and ~1-2 s under the sanitizer CI jobs, accepted for a whole-table guarantee.
    double pal_lab[240][3];
    for (int j = 0; j < 240; ++j)
    {
        const Color p = palette_entry(j);
        cielab_ref(p.r, p.g, p.b, pal_lab[j]);
    }
    std::vector<int> owner(QUANT256_LUT_SIZE, -1); // cell -> contained palette entry (0..239)
    for (int j = 0; j < 240; ++j)
    {
        owner[quant256_idx(palette_entry(j))] = j;
    }

    const auto &lut = quant256_lut();
    for (int r = 0; r < 64; ++r)
    {
        for (int g = 0; g < 64; ++g)
        {
            for (int b = 0; b < 64; ++b)
            {
                const size_t idx =
                    (static_cast<size_t>(r) << 12u) | (static_cast<size_t>(g) << 6u) | static_cast<size_t>(b);
                const int chosen = static_cast<int>(lut[idx]) - 16;
                ASSERT_TRUE(chosen >= 0 && chosen < 240);
                if (owner[idx] >= 0)
                {
                    ASSERT_EQ(chosen, owner[idx]);
                    continue;
                }
                double c[3];
                cielab_ref((4.0 * r) + 1.5, (4.0 * g) + 1.5, (4.0 * b) + 1.5, c);
                auto sq = [&c](const double p[3])
                {
                    const double dl = p[0] - c[0];
                    const double da = p[1] - c[1];
                    const double db = p[2] - c[2];
                    return (dl * dl) + (da * da) + (db * db);
                };
                double best = std::numeric_limits<double>::max();
                for (const auto &p : pal_lab)
                {
                    best = std::min(best, sq(p));
                }
                ASSERT_TRUE(sq(pal_lab[chosen]) <= best + 1e-2);
            }
        }
    }
}

// ─── 256-colour present() emission ────────────────────────────────────────────

TEST(framebuffer, present_256_emits_palette_fg_and_bg)
{
    // top != bot in 256 mode emits a combined 38;5;fg;48;5;bg sequence and never any 24-bit SGR.
    Framebuffer fb(1, 2, /*headless=*/true, ColorMode::Palette256);
    CaptureStdout cap;
    (void)fb.commit_pixel(0, 0, 0.5f, { 200, 40, 40 }); // top; bottom stays black
    fb.present();
    const std::string out = cap.read();
    ASSERT_TRUE(out.find("38;5;") != std::string::npos);
    ASSERT_TRUE(out.find("48;5;") != std::string::npos);
    ASSERT_TRUE(out.find("38;2") == std::string::npos);
    ASSERT_TRUE(out.find("48;2") == std::string::npos);
}

TEST(framebuffer, present_256_top_eq_bot_suppresses_fg)
{
    // top == bot collapses to a bg-only SGR + space; no fg SGR and no half-block are emitted.
    Framebuffer fb(2, 2, /*headless=*/true, ColorMode::Palette256);
    CaptureStdout cap;
    fb.clear({ 100, 150, 200 });
    fb.present();
    const std::string out = cap.read();
    ASSERT_TRUE(out.find("48;5;") != std::string::npos);        // bg emitted
    ASSERT_TRUE(out.find("38;5;") == std::string::npos);        // fg suppressed
    ASSERT_TRUE(out.find("\xe2\x96\x80") == std::string::npos); // no ▀ half-block: cells are spaces
}

TEST(framebuffer, present_256_second_identical_cell_suppresses_bg)
{
    // Two adjacent top==bot cells with the same index: the bg SGR for that index is emitted once.
    Framebuffer fb(2, 2, /*headless=*/true, ColorMode::Palette256);
    CaptureStdout cap;
    fb.clear({ 100, 150, 200 });
    fb.present();
    const std::string out = cap.read();
    char sgr[16];
    std::snprintf(sgr, sizeof(sgr), "\033[48;5;%dm", static_cast<int>(quantize_256({ 100, 150, 200 })));
    const size_t first = out.find(sgr);
    ASSERT_TRUE(first != std::string::npos);                    // emitted once
    ASSERT_TRUE(out.find(sgr, first + 1) == std::string::npos); // suppressed for the second cell
}

TEST(framebuffer, present_truecolor_default_still_24bit)
{
    // The default ctor mode is TrueColor: 24-bit SGR, never a palette index. Locks the default.
    Framebuffer fb(2, 2, /*headless=*/true);
    CaptureStdout cap;
    fb.clear({ 100, 150, 200 });
    fb.present();
    const std::string out = cap.read();
    ASSERT_TRUE(out.find("48;2;") != std::string::npos);
    ASSERT_TRUE(out.find("38;5;") == std::string::npos);
    ASSERT_TRUE(out.find("48;5;") == std::string::npos);
}

// ─── 256-colour index coalescing / skip path ──────────────────────────────────

TEST(framebuffer, present_256_distinct_rgb_same_index_is_clean)
{
    // {16,16,16} and {20,20,20} both quantize to 233, so m_prev_color (which stores indices in
    // 256 mode) reads the second frame as entirely clean: no new cursor positioning is emitted.
    ASSERT_EQ(static_cast<int>(quantize_256({ 16, 16, 16 })), 233);
    ASSERT_EQ(static_cast<int>(quantize_256({ 20, 20, 20 })), 233);
    Framebuffer fb(4, 2, /*headless=*/true, ColorMode::Palette256);
    CaptureStdout cap;
    fb.clear({ 16, 16, 16 });
    fb.present(); // full redraw emits \033[1;1H once
    fb.clear({ 20, 20, 20 });
    fb.present(); // entirely clean: no cursor pos
    const std::string out = cap.read();
    const size_t first = out.find("\033[1;1H");
    ASSERT_TRUE(first != std::string::npos);
    ASSERT_TRUE(out.find("\033[1;1H", first + 1) == std::string::npos);
}

TEST(framebuffer, present_256_single_skip_emits_cursor_advance)
{
    // The incremental skip scan must re-quantize/compare correctly in 256 mode: a clean cell
    // between two dirty ones yields a single-step \033[C advance.
    Framebuffer fb(4, 2, /*headless=*/true, ColorMode::Palette256);
    seed_prev_color(fb); // full redraw establishes prev (all black -> 16)

    CaptureStdout cap;
    fb.clear();
    (void)fb.commit_pixel(0, 0, 0.5f, { 200, 0, 0 }); // col 0 dirty
    (void)fb.commit_pixel(2, 0, 0.5f, { 0, 200, 0 }); // col 2 dirty; col 1 clean -> skip=1
    fb.present();
    const std::string inc = cap.read();
    ASSERT_EQ(count_cursor_advances(inc), static_cast<size_t>(1)); // col 1 only; col 3's tail is dropped
    ASSERT_TRUE(inc.find("\033[C") != std::string::npos);
}

// ─── 256-colour HUD, plumbing, degenerate sizes ───────────────────────────────

TEST(framebuffer, present_256_hud_uses_palette_colors)
{
    Framebuffer fb(10, 4, /*headless=*/true, ColorMode::Palette256);
    CaptureStdout cap;
    fb.set_hud("HUD256");
    fb.present();
    const std::string out = cap.read();
    ASSERT_TRUE(out.find("\033[48;5;233m\033[38;5;247m") != std::string::npos);
    ASSERT_TRUE(out.find("\033[48;2;18;18;18m") == std::string::npos);
    ASSERT_TRUE(out.find("HUD256") != std::string::npos);
}

TEST(framebuffer, present_256_degenerate_sizes_no_crash)
{
    FdRedirect rd;
    {
        Framebuffer fb(0, 0, /*headless=*/true, ColorMode::Palette256);
        fb.clear();
        fb.present();
    }
    {
        Framebuffer fb(2, 3, /*headless=*/true, ColorMode::Palette256); // odd height
        (void)fb.commit_pixel(0, 2, 0.5f, { 100, 150, 200 });
        fb.present();
    }
}

TEST(framebuffer, construct_explicit_truecolor_matches_default)
{
    // Passing ColorMode::TrueColor explicitly behaves like the default: 24-bit SGR only.
    Framebuffer fb(2, 2, /*headless=*/true, ColorMode::TrueColor);
    CaptureStdout cap;
    fb.clear({ 10, 20, 30 });
    fb.present();
    const std::string out = cap.read();
    ASSERT_TRUE(out.find("48;2;") != std::string::npos);
    ASSERT_TRUE(out.find("48;5;") == std::string::npos);
}
