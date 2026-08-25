#include "tests/test.h"
#include "tests/b64_test_util.h"
#include "tests/rasterize_test_util.h"
#include "tests/sixel_test_util.h"
#include "src/terminal/framebuffer.h"
#include "src/render/renderer.h"

#include "miniz.h" // independent inflate for the kitty deflated-frame round trip

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#ifndef _WIN32
// The kitty shm tests read the frame object back by name; there is no shm
// transport on Windows (platform.h's stubs), so they compile out entirely.
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

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

// get_pixel / depth_at bounds

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

// clear()

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

// resize()

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

// construction edge cases

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
    (void)fb.commit_pixel(0, 2, 0.5f, { 100, 150, 200 }); // row 2, the lone odd row
    fb.present();                                         // must not crash
}

// headless mode

TEST(framebuffer, headless_dimensions_correct)
{
    // No FdRedirect needed: headless suppresses all terminal I/O.
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

TEST(framebuffer, present_dirty_then_present_again_no_crash)
{
    // Follow a full redraw with a mostly clean incremental frame.
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

    fb.present(); // dirty-tracking path, must not crash
}

// HUD
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
    // HUD is empty by default, so the !m_hud.empty() block must be skipped,
    // so \033[?7l (disable-autowrap, unique to the HUD block) must not appear.
    Framebuffer fb(10, 4, /*headless=*/true);
    CaptureStdout cap;
    fb.present();
    ASSERT_TRUE(cap.read().find("\033[?7l") == std::string::npos);
}

TEST(framebuffer, hud_erase_precedes_text_never_follows)
{
    // Erase before drawing; trailing EL0 would remove a full-width line's last cell.
    // Byte order is the unit-testable proxy for the rendered tmux check in CLAUDE.md.
    Framebuffer fb(10, 4, /*headless=*/true);
    CaptureStdout cap;
    fb.set_hud("HUDMARK");
    fb.present();
    const std::string out = cap.read();
    const size_t erase_pos = out.find("\033[K");
    const size_t text_pos = out.find("HUDMARK");
    ASSERT_TRUE(erase_pos != std::string::npos);
    ASSERT_TRUE(text_pos != std::string::npos);
    ASSERT_TRUE(erase_pos < text_pos);
    ASSERT_TRUE(out.find("\033[K", text_pos) == std::string::npos);
}

TEST(framebuffer, hud_unchanged_is_skipped)
{
    // The HUD changes at the fps-latch rate, far below the frame rate; an unchanged line must
    // cost zero bytes. \033[?7l is unique to the HUD block, so its count is the emission count.
    Framebuffer fb(10, 4, /*headless=*/true);
    CaptureStdout cap;
    fb.set_hud("SAME");
    fb.present(); // frame 1: full redraw, emits
    fb.present(); // frame 2: unchanged, must skip
    std::string out = cap.read();
    size_t first = out.find("\033[?7l");
    ASSERT_TRUE(first != std::string::npos);
    ASSERT_TRUE(out.find("\033[?7l", first + 1) == std::string::npos);

    fb.set_hud("CHANGED");
    fb.present(); // frame 3: new text, must emit
    out = cap.read();
    first = out.find("\033[?7l");
    const size_t second = out.find("\033[?7l", first + 1);
    ASSERT_TRUE(second != std::string::npos);
    ASSERT_TRUE(out.find("\033[?7l", second + 1) == std::string::npos);
    ASSERT_TRUE(out.find("CHANGED") != std::string::npos);
}

TEST(framebuffer, hud_skip_latch_survives_an_empty_frame_on_a_full_redraw)
{
    // An empty HUD draws nothing, so the skip latch must not go on naming a line that the full
    // redraw's \033[2J wiped: restoring that same line afterwards would be skipped as unchanged
    // and the row would stay blank for the rest of the session.
    Framebuffer fb(10, 4, /*headless=*/true);
    CaptureStdout cap;

    fb.set_hud("LATCHED");
    fb.present(); // emits (first frame is a full redraw)
    fb.set_hud("");
    fb.resize(10, 4); // full redraw with an empty HUD: nothing drawn, row wiped
    fb.present();
    fb.set_hud("LATCHED"); // the same text as before, and it must be drawn again
    fb.present();

    const std::string out = cap.read();
    const size_t first = out.find("LATCHED");
    ASSERT_TRUE(first != std::string::npos);
    ASSERT_TRUE(out.find("LATCHED", first + 1) != std::string::npos);
}

TEST(framebuffer, hud_reemitted_after_resize)
{
    // A resize wipes the HUD row (the \033[2J its next present carries), so that present must
    // re-emit the line even though the string itself did not change.
    Framebuffer fb(10, 4, /*headless=*/true);
    CaptureStdout cap;
    fb.set_hud("PERSIST");
    fb.present();
    fb.resize(10, 4);
    fb.present();
    const std::string out = cap.read();
    const size_t first = out.find("\033[?7l");
    ASSERT_TRUE(first != std::string::npos);
    ASSERT_TRUE(out.find("\033[?7l", first + 1) != std::string::npos);
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

// incremental skip path

namespace
{
    // Seed m_prev_color in a separate capture so full-redraw positioning cannot
    // satisfy assertions intended for the incremental frame.
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

// A clean row tail needs no cursor advance because all later output positions
// absolutely. This is mode-independent, so truecolor covers both.
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

// additional incremental / emit_cell paths

TEST(framebuffer, present_incremental_entirely_clean_row_emits_no_cursor_pos)
{
    // A clean incremental row emits no positioning or cells. CaptureStdout retains
    // both frames, so the home escape must occur only in the initial full redraw.
    Framebuffer fb(4, 2, /*headless=*/true);
    CaptureStdout cap;
    fb.present(); // frame 1: full-redraw, emits \033[1;1H once
    fb.present(); // frame 2: entirely clean row, must not emit another \033[1;1H

    const std::string out = cap.read();
    const size_t first = out.find("\033[1;1H");
    ASSERT_TRUE(first != std::string::npos);                            // frame 1 emitted it
    ASSERT_TRUE(out.find("\033[1;1H", first + 1) == std::string::npos); // frame 2 did not
}

TEST(framebuffer, present_incremental_dirty_at_last_column)
{
    // Dirty cell at col m_width-1: after emitting it col==m_width, the outer
    // while exits immediately: the skip branch is never entered.
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
    // Resize from one to two terminal rows. The next present must fully redraw and
    // emit row-two positioning absent from the first two frames.
    Framebuffer fb(4, 2, /*headless=*/true);
    CaptureStdout cap;
    fb.present(); // frame 1: full-redraw (1 term row, only \033[1;1H)
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

TEST(framebuffer, write_byte_lut_boundary_values)
{
    // Exercise both ends of write_byte's one-, two-, and three-digit ranges through
    // the red channel of an emitted foreground SGR.
    const uint8_t vals[] = { 0, 9, 10, 99, 100, 255 };
    for (uint8_t v : vals)
    {
        Framebuffer fb(1, 2, /*headless=*/true);
        CaptureStdout cap;
        (void)fb.commit_pixel(0, 0, 0.5f, { v, 1, 2 }); // top != bot (bot stays default black)
        fb.present();
        const std::string out = cap.read();
        // When fg and bg both change, the code emits a combined sequence
        // \033[38;2;R;G;B;48;2;...m: match the fg prefix up to the separator.
        char expected[32];
        std::snprintf(expected, sizeof(expected), "\033[38;2;%d;1;2;", static_cast<int>(v));
        ASSERT_TRUE(out.find(expected) != std::string::npos);
    }
}

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

TEST(framebuffer, adopt_alt_screen_skips_enter_but_still_exits)
{
    // An adopted alternate screen must not be entered twice, but the framebuffer
    // still owns the eventual exit.
    CaptureStdout cap;
    {
        Framebuffer fb(1, 2, /*headless=*/false, ColorMode::TrueColor, {}, /*adopt_alt_screen=*/true);
    }
    const std::string out = cap.read();
    ASSERT_TRUE(out.find("\033[?1049h") == std::string::npos);
    ASSERT_TRUE(out.find("\033[?1049l") != std::string::npos);
    ASSERT_TRUE(out.find("\033[?25l") != std::string::npos); // cursor hide is not skipped
}

// tonemap (soft-knee highlight rolloff)
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

// quantize_256: nearest CIELAB entry through the 64^3 LUT
// Results use cube/ramp indices 16..255, with exact palette colours fixed to themselves.

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
    // Exact ramp values map to 232+k; 160 instead maps to ramp 158. Indices 233 and
    // 253 also pin the framebuffer's hard-coded HUD background and foreground.
    ASSERT_EQ(static_cast<int>(quantize_256({ 8, 8, 8 })), 232);
    // Through the shared constants, not their literal values, so this fails if the bar's
    // colours move rather than only if the quantizer does.
    ASSERT_EQ(static_cast<int>(quantize_256(HUD_BAR_BG)), 233);
    ASSERT_EQ(static_cast<int>(quantize_256(HUD_BAR_FG)), 253);
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
    // CIELAB keeps dark green and brown chromatic where squared RGB chose grey.
    // Their runner-up margin is at least 0.47 deltaE, well above float noise.
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

TEST(framebuffer, quant256_idx_packed_matches_color_form)
{
    // The packed form must agree with the Color form, exhaustively over the
    // full RGB cube, and ignore the reserved top byte.
    for (uint32_t r = 0; r < 256; r++)
    {
        for (uint32_t g = 0; g < 256; g++)
        {
            for (uint32_t b = 0; b < 256; b++)
            {
                const uint32_t packed = r | (g << 8u) | (b << 16u);
                const Color c{ static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b) };
                ASSERT_EQ(quant256_idx_packed(packed), quant256_idx(c));
                ASSERT_EQ(quant256_idx_packed(packed | 0xFF000000u), quant256_idx(c));
            }
        }
    }
}

TEST(framebuffer, quant256_palette_entry_matches_reference)
{
    // The exported table (color.h) against this file's independent copy of the xterm formulas:
    // the sixel emitter derives its colour registers from the export, so a drift here would
    // repaint every sixel frame in wrong colours while the quantizer tests stay green.
    ASSERT_TRUE(quant256_palette_entry(0) == Color(0, 0, 0));
    ASSERT_TRUE(quant256_palette_entry(215) == Color(255, 255, 255));
    ASSERT_TRUE(quant256_palette_entry(216) == Color(8, 8, 8));
    ASSERT_TRUE(quant256_palette_entry(239) == Color(238, 238, 238));
    for (int j = 0; j < 240; ++j)
    {
        ASSERT_TRUE(quant256_palette_entry(j) == palette_entry(j));
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
    // Exhaustively compare all 64^3 cells with independent double-precision CIELAB.
    // Compare distances rather than indices so float-vs-double near-ties remain stable.
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

// 256-colour present() emission

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

// 256-colour index coalescing / skip path

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

// 256-colour HUD, plumbing, degenerate sizes

TEST(framebuffer, present_256_hud_uses_palette_colors)
{
    Framebuffer fb(10, 4, /*headless=*/true, ColorMode::Palette256);
    CaptureStdout cap;
    fb.set_hud("HUD256");
    fb.present();
    const std::string out = cap.read();
    // Palette bg for the bar (the fg SGRs are the composed HUD string's own business now),
    // erased before the text is drawn.
    ASSERT_TRUE(out.find("\033[48;5;233m\033[38;5;253m\033[K") != std::string::npos);
    ASSERT_TRUE(out.find("\033[48;2;18;18;18m") == std::string::npos);
    ASSERT_TRUE(out.find("38;2;") == std::string::npos);
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

// kitty backend

namespace
{
    GraphicsConfig direct_kitty_config(int cols, int rows)
    {
        GraphicsConfig kc;
        kc.backend = GraphicsBackend::Kitty;
        kc.shm = false; // the direct transport is capturable; shm needs a live reader
        kc.cols = cols;
        kc.rows = rows;
        return kc;
    }

#ifndef _WIN32
    GraphicsConfig shm_kitty_config(int cols, int rows)
    {
        GraphicsConfig kc = direct_kitty_config(cols, rows);
        kc.shm = true;
        return kc;
    }

    // The object the frame was written to, read back before the Framebuffer dtor
    // unlinks the ring. Empty when the transport was unavailable (no /dev/shm), in
    // which case present() fell back to the direct form and there is nothing to check.
    std::vector<unsigned char> read_shm_ring_slot(unsigned slot, size_t expect_bytes)
    {
        char name[64];
        std::snprintf(name, sizeof name, "/rasterminal-%lu-%u", static_cast<unsigned long>(getpid()), slot);
        const int fd = shm_open(name, O_RDONLY, 0600);
        if (fd < 0)
        {
            return {};
        }
        std::vector<unsigned char> got(expect_bytes);
        size_t at = 0;
        while (at < expect_bytes)
        {
            const ssize_t r = read(fd, got.data() + at, expect_bytes - at);
            if (r <= 0)
            {
                break;
            }
            at += static_cast<size_t>(r);
        }
        close(fd);
        got.resize(at);
        return got;
    }
#endif
} // namespace

TEST(framebuffer, kitty_present_deflated_pixel_roundtrip)
{
    // The full path from the pixel slots to the wire: write_rgb's byte order and
    // the deflate leg, decoded back with an independent inflate.
    Framebuffer fb(20, 8, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(10, 4));
    fb.clear({ 10, 20, 30 });
    (void)fb.commit_pixel(3, 2, 0.5f, { 200, 100, 50 });
    CaptureStdout cap;
    fb.present();
    const std::string out = cap.read();

    const size_t apc = out.find("\033_G");
    ASSERT_TRUE(apc != std::string::npos);
    ASSERT_TRUE(out.find("o=z") != std::string::npos);
    ASSERT_TRUE(out.find("s=20") != std::string::npos);
    ASSERT_TRUE(out.find("v=8") != std::string::npos);
    ASSERT_TRUE(out.find("c=10") != std::string::npos);
    ASSERT_TRUE(out.find("r=4") != std::string::npos);

    // Reassemble every APC payload (chunked or not) in order and decode.
    std::string b64;
    size_t i = apc;
    while ((i = out.find("\033_G", i)) != std::string::npos)
    {
        const size_t semi = out.find(';', i);
        const size_t st = out.find("\033\\", i);
        ASSERT_TRUE(st != std::string::npos);
        if (semi != std::string::npos && semi < st)
        {
            b64 += out.substr(semi + 1, st - semi - 1);
        }
        i = st + 2;
    }
    const auto z = b64_decode(b64);
    std::vector<unsigned char> rgb(static_cast<size_t>(20 * 8 * 3));
    // unsigned int intermediary for the same -Wuseless-cast/LLP64 reason as
    // framebuffer.cpp's transmit_direct.
    const auto rgb_len = static_cast<unsigned int>(rgb.size());
    mz_ulong len = rgb_len;
    ASSERT_EQ(mz_uncompress(rgb.data(), &len, z.data(), static_cast<unsigned int>(z.size())), MZ_OK);
    ASSERT_EQ(len, rgb_len);

    const size_t bg = 0;                                       // pixel (0,0)
    const size_t px = ((static_cast<size_t>(2) * 20) + 3) * 3; // pixel (3,2)
    ASSERT_EQ(rgb[bg], 10);
    ASSERT_EQ(rgb[bg + 1], 20);
    ASSERT_EQ(rgb[bg + 2], 30);
    ASSERT_EQ(rgb[px], 200);
    ASSERT_EQ(rgb[px + 1], 100);
    ASSERT_EQ(rgb[px + 2], 50);
}

#ifndef _WIN32
TEST(framebuffer, kitty_shm_frame_holds_the_exact_pixels)
{
    // The shm transport writes the frame to an object the terminal reads by name,
    // so nothing on the wire carries the pixels and the escape-level tests above
    // cannot see them. Read the object back and check every byte.
    Framebuffer fb(20, 8, /*headless=*/true, ColorMode::TrueColor, shm_kitty_config(10, 4));
    fb.clear({ 10, 20, 30 });
    (void)fb.commit_pixel(3, 2, 0.5f, { 200, 100, 50 });
    std::string out;
    {
        CaptureStdout cap;
        fb.present();
        out = cap.read();
    }

    const auto expect = static_cast<size_t>(20 * 8 * 3);
    const auto got = read_shm_ring_slot(0, expect);
    if (got.empty())
    {
        return; // no working shm here; present() took the direct fallback
    }
    ASSERT_TRUE(out.find("t=s") != std::string::npos);
    ASSERT_EQ(got.size(), expect);

    const size_t bg = 0;                                       // pixel (0,0)
    const size_t px = ((static_cast<size_t>(2) * 20) + 3) * 3; // pixel (3,2)
    ASSERT_EQ(got[bg], 10);
    ASSERT_EQ(got[bg + 1], 20);
    ASSERT_EQ(got[bg + 2], 30);
    ASSERT_EQ(got[px], 200);
    ASSERT_EQ(got[px + 1], 100);
    ASSERT_EQ(got[px + 2], 50);

    // Every other pixel is the clear colour: a chunked fill that dropped, doubled
    // or misaligned a chunk shows up here and nowhere else.
    for (size_t i = 0; i < expect; i += 3)
    {
        if (i == px)
        {
            continue;
        }
        ASSERT_EQ(got[i], 10);
        ASSERT_EQ(got[i + 1], 20);
        ASSERT_EQ(got[i + 2], 30);
    }
}

TEST(framebuffer, kitty_shm_frame_spans_several_chunks)
{
    // The fill hands the object one cache-sized chunk at a time, so a frame larger
    // than one chunk exercises the loop's offset arithmetic. 400x300 is 360000
    // bytes against a 256 KB chunk: two chunks, the second a partial one.
    constexpr int W = 400;
    constexpr int H = 300;
    Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, shm_kitty_config(50, 18));
    fb.clear({ 7, 9, 11 });
    // Landmarks either side of the first chunk boundary (256*1024/3 = 87381 px).
    (void)fb.commit_pixel(87380 % W, 87380 / W, 0.5f, { 1, 2, 3 });
    (void)fb.commit_pixel(87381 % W, 87381 / W, 0.5f, { 4, 5, 6 });
    (void)fb.commit_pixel(W - 1, H - 1, 0.5f, { 250, 251, 252 });
    {
        CaptureStdout cap;
        fb.present();
    }

    const size_t expect = static_cast<size_t>(W) * static_cast<size_t>(H) * 3u;
    const auto got = read_shm_ring_slot(0, expect);
    if (got.empty())
    {
        return;
    }
    ASSERT_EQ(got.size(), expect);
    const size_t last = size_t{ 87380 } * 3;  // last pixel of chunk 0
    const size_t first = size_t{ 87381 } * 3; // first pixel of chunk 1
    ASSERT_EQ(got[last], 1);
    ASSERT_EQ(got[last + 1], 2);
    ASSERT_EQ(got[last + 2], 3);
    ASSERT_EQ(got[first], 4);
    ASSERT_EQ(got[first + 1], 5);
    ASSERT_EQ(got[first + 2], 6);
    ASSERT_EQ(got[expect - 3], 250);
    ASSERT_EQ(got[expect - 2], 251);
    ASSERT_EQ(got[expect - 1], 252);
}
#endif

namespace
{
    // Reassembles the transmitted image payload from however many APC records it took.
    std::string collect_apc_payload(const std::string &out)
    {
        std::string b64;
        size_t i = out.find("\033_G");
        while (i != std::string::npos)
        {
            const size_t semi = out.find(';', i);
            const size_t st = out.find("\033\\", i);
            if (st == std::string::npos)
            {
                break;
            }
            if (semi != std::string::npos && semi < st)
            {
                b64 += out.substr(semi + 1, st - semi - 1);
            }
            i = out.find("\033_G", st + 2);
        }
        return b64;
    }

    // Run each worker id sequentially to test deterministic splitting and assembly.
    Framebuffer::ParallelRunner sequential_runner(int n)
    {
        return { [n](const std::function<void(int, int)> &fn)
                 {
                     for (int w = 0; w < n; w++)
                     {
                         fn(w, n);
                     }
                 },
                 n };
    }
} // namespace

TEST(framebuffer, kitty_parallel_deflate_stream_inflates_to_the_exact_frame)
{
    // A 2.16 MB frame forces chunked deflate. Inflate the assembled stream with an
    // ordinary decoder, matching terminal behavior.
    constexpr int W = 1200;
    constexpr int H = 600;
    Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(150, 37));
    fb.set_parallel_runner(sequential_runner(8));
    fb.clear({ 12, 34, 56 });
    (void)fb.commit_pixel(0, 0, 0.5f, { 1, 2, 3 });
    (void)fb.commit_pixel(W / 2, H / 2, 0.5f, { 90, 91, 92 });
    (void)fb.commit_pixel(W - 1, H - 1, 0.5f, { 250, 251, 252 });

    std::string out;
    {
        CaptureStdout cap;
        fb.present();
        out = cap.read();
    }
    ASSERT_TRUE(out.find("o=z") != std::string::npos);

    const auto z = b64_decode(collect_apc_payload(out));
    std::vector<unsigned char> rgb(static_cast<size_t>(W) * static_cast<size_t>(H) * 3u);
    const auto rgb_len = static_cast<unsigned int>(rgb.size());
    mz_ulong len = rgb_len;
    ASSERT_EQ(mz_uncompress(rgb.data(), &len, z.data(), static_cast<unsigned int>(z.size())), MZ_OK);
    ASSERT_EQ(len, rgb_len);

    ASSERT_EQ(rgb[0], 1);
    ASSERT_EQ(rgb[1], 2);
    ASSERT_EQ(rgb[2], 3);
    const size_t mid = ((static_cast<size_t>(H / 2) * W) + (W / 2)) * 3u;
    ASSERT_EQ(rgb[mid], 90);
    ASSERT_EQ(rgb[mid + 1], 91);
    ASSERT_EQ(rgb[mid + 2], 92);
    ASSERT_EQ(rgb[rgb.size() - 3], 250);
    ASSERT_EQ(rgb[rgb.size() - 2], 251);
    ASSERT_EQ(rgb[rgb.size() - 1], 252);

    // Everything else is the clear colour: a dropped, doubled or misordered chunk is
    // invisible in the landmarks above but not here.
    for (size_t i = 3; i < rgb.size() - 3; i += 3)
    {
        if (i == mid)
        {
            continue;
        }
        ASSERT_EQ(rgb[i], 12);
        ASSERT_EQ(rgb[i + 1], 34);
        ASSERT_EQ(rgb[i + 2], 56);
    }
}

TEST(framebuffer, kitty_parallel_and_serial_deflate_agree)
{
    // The split must not change what the terminal ends up displaying, so the two paths
    // are rendered identically and their inflated frames compared byte for byte.
    constexpr int W = 1200;
    constexpr int H = 600;
    const auto paint = [](Framebuffer &fb)
    {
        fb.clear({ 5, 6, 7 });
        for (int y = 0; y < H; y += 7)
        {
            for (int x = 0; x < W; x += 5)
            {
                (void)fb.commit_pixel(
                    x, y, 0.5f, { static_cast<uint8_t>(x), static_cast<uint8_t>(y), static_cast<uint8_t>(x ^ y) }
                );
            }
        }
    };
    const auto inflate_frame = [](const std::string &out)
    {
        const auto z = b64_decode(collect_apc_payload(out));
        std::vector<unsigned char> rgb(static_cast<size_t>(W) * static_cast<size_t>(H) * 3u);
        const auto rgb_len = static_cast<unsigned int>(rgb.size());
        mz_ulong len = rgb_len;
        const int rc = mz_uncompress(rgb.data(), &len, z.data(), static_cast<unsigned int>(z.size()));
        if (rc != MZ_OK || len != rgb_len)
        {
            rgb.clear();
        }
        return rgb;
    };

    std::vector<unsigned char> serial;
    std::vector<unsigned char> parallel;
    {
        Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(150, 37));
        paint(fb);
        CaptureStdout cap;
        fb.present();
        serial = inflate_frame(cap.read());
    }
    {
        Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(150, 37));
        fb.set_parallel_runner(sequential_runner(6));
        paint(fb);
        CaptureStdout cap;
        fb.present();
        parallel = inflate_frame(cap.read());
    }
    ASSERT_FALSE(serial.empty());
    ASSERT_EQ(parallel.size(), serial.size());
    ASSERT_TRUE(parallel == serial);
}

TEST(framebuffer, kitty_parallel_deflate_on_a_real_worker_pool)
{
    // Borrow a real Renderer pool so sanitizers exercise concurrent chunks, not only
    // the deterministic sequential runner above.
    constexpr int W = 1200;
    constexpr int H = 600;
    Renderer renderer(4);
    Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(150, 37));
    fb.set_parallel_runner({ [&renderer](const std::function<void(int, int)> &fn) { renderer.run_on_workers(fn); },
                             renderer.worker_count() });
    fb.clear({ 3, 4, 5 });
    (void)fb.commit_pixel(W - 1, H - 1, 0.5f, { 77, 78, 79 });

    std::string out;
    {
        CaptureStdout cap;
        fb.present();
        out = cap.read();
    }
    ASSERT_TRUE(out.find("o=z") != std::string::npos);

    const auto z = b64_decode(collect_apc_payload(out));
    std::vector<unsigned char> rgb(static_cast<size_t>(W) * static_cast<size_t>(H) * 3u);
    const auto rgb_len = static_cast<unsigned int>(rgb.size());
    mz_ulong len = rgb_len;
    ASSERT_EQ(mz_uncompress(rgb.data(), &len, z.data(), static_cast<unsigned int>(z.size())), MZ_OK);
    ASSERT_EQ(len, rgb_len);
    ASSERT_EQ(rgb[0], 3);
    ASSERT_EQ(rgb[rgb.size() - 3], 77);
    ASSERT_EQ(rgb[rgb.size() - 2], 78);
    ASSERT_EQ(rgb[rgb.size() - 1], 79);
}

TEST(framebuffer, kitty_parallel_deflate_across_threshold_sizes)
{
    // Straddle the pixel and byte thresholds: 512x512 reaches parallel staging,
    // 592x592 reaches chunked deflate, and tiny frames stay serial. Inflate every result.
    const int sizes[][2] = { { 1, 1 }, { 3, 7 }, { 64, 6 }, { 65, 5 }, { 511, 511 }, { 512, 512 }, { 592, 592 } };
    Renderer renderer(4);
    for (const auto &s : sizes)
    {
        const int W = s[0];
        const int H = s[1];
        Framebuffer fb(
            W, H, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(std::max(1, W / 8), std::max(1, H / 16))
        );
        fb.set_parallel_runner({ [&renderer](const std::function<void(int, int)> &fn) { renderer.run_on_workers(fn); },
                                 renderer.worker_count() });
        fb.clear({ 17, 34, 51 });
        (void)fb.commit_pixel(0, 0, 0.5f, { 1, 2, 3 });

        std::string out;
        {
            CaptureStdout cap;
            fb.present();
            out = cap.read();
        }
        const auto payload = b64_decode(collect_apc_payload(out));
        ASSERT_FALSE(payload.empty());

        std::vector<unsigned char> rgb(static_cast<size_t>(W) * static_cast<size_t>(H) * 3u);
        if (out.find("o=z") != std::string::npos)
        {
            const auto rgb_len = static_cast<unsigned int>(rgb.size());
            mz_ulong len = rgb_len;
            ASSERT_EQ(
                mz_uncompress(rgb.data(), &len, payload.data(), static_cast<unsigned int>(payload.size())), MZ_OK
            );
            ASSERT_EQ(len, rgb_len);
        }
        else
        {
            ASSERT_EQ(payload.size(), rgb.size()); // raw fallback
            std::copy(payload.begin(), payload.end(), rgb.begin());
        }
        ASSERT_EQ(rgb[0], 1);
        ASSERT_EQ(rgb[1], 2);
        ASSERT_EQ(rgb[2], 3);
        // Every remaining pixel is the clear colour: a dropped or doubled chunk at this
        // size is invisible in the landmark above but not here.
        for (size_t i = 3; i < rgb.size(); i += 3)
        {
            ASSERT_EQ(rgb[i], 17);
            ASSERT_EQ(rgb[i + 1], 34);
            ASSERT_EQ(rgb[i + 2], 51);
        }
    }
}

TEST(framebuffer, kitty_parallel_deflate_falls_back_when_a_chunk_fails)
{
    // A runner that never calls the task stands in for every way a worker can fail to
    // produce its chunk (bad_alloc in the compressor, chiefly). The frame must still be
    // transmitted, and must still carry the right pixels.
    constexpr int W = 1200;
    constexpr int H = 600;
    Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(150, 37));
    fb.set_parallel_runner({ [](const std::function<void(int, int)> &) {}, 8 });
    fb.clear({ 21, 22, 23 });
    (void)fb.commit_pixel(4, 4, 0.5f, { 60, 61, 62 });

    std::string out;
    {
        CaptureStdout cap;
        fb.present();
        out = cap.read();
    }
    ASSERT_TRUE(out.find("\033_G") != std::string::npos);

    const auto payload = b64_decode(collect_apc_payload(out));
    std::vector<unsigned char> rgb(static_cast<size_t>(W) * static_cast<size_t>(H) * 3u);
    if (out.find("o=z") != std::string::npos)
    {
        const auto rgb_len = static_cast<unsigned int>(rgb.size());
        mz_ulong len = rgb_len;
        ASSERT_EQ(mz_uncompress(rgb.data(), &len, payload.data(), static_cast<unsigned int>(payload.size())), MZ_OK);
        ASSERT_EQ(len, rgb_len);
    }
    else
    {
        ASSERT_EQ(payload.size(), rgb.size()); // raw fallback
        std::copy(payload.begin(), payload.end(), rgb.begin());
    }
    const size_t px = ((static_cast<size_t>(4) * W) + 4) * 3u;
    ASSERT_EQ(rgb[px], 60);
    ASSERT_EQ(rgb[px + 1], 61);
    ASSERT_EQ(rgb[px + 2], 62);
    ASSERT_EQ(rgb[0], 21);
}

TEST(framebuffer, kitty_idle_present_emits_no_image)
{
    // Only a clear() (a rewritten pixel buffer) re-arms the transmission; a
    // present with nothing new emits no image bytes at all.
    Framebuffer fb(4, 4, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(2, 2));
    fb.clear({ 1, 2, 3 });
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().find("\033_G") != std::string::npos);
    }
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().find("\033_G") == std::string::npos);
    }
    {
        CaptureStdout cap;
        fb.clear({ 1, 2, 3 });
        fb.present();
        ASSERT_TRUE(cap.read().find("\033_G") != std::string::npos);
    }
}

TEST(framebuffer, kitty_zero_rows_deletes_resident_image)
{
    // Shrinking to zero image rows transmits nothing but must delete the resident
    // kitty image exactly once so it cannot cover the HUD.
    Framebuffer fb(4, 4, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(2, 2));
    fb.clear({ 1, 2, 3 });
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().find("a=T") != std::string::npos);
    }
    {
        CaptureStdout cap;
        fb.resize(4, 0, 2, 0, 1, 1);
        fb.present();
        const std::string out = cap.read();
        ASSERT_TRUE(out.find("a=d,d=I") != std::string::npos);
        ASSERT_TRUE(out.find("a=T") == std::string::npos);
    }
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().find("\033_G") == std::string::npos);
    }
}

// sixel backend

namespace
{
    GraphicsConfig sixel_config(int cols, int rows)
    {
        GraphicsConfig sc;
        sc.backend = GraphicsBackend::Sixel;
        sc.cols = cols;
        sc.rows = rows;
        return sc;
    }
} // namespace

TEST(framebuffer, sixel_parallel_across_threshold_sizes)
{
    // Same idea for sixel: the encode splits by BAND and bails to serial below two bands
    // per worker, and the quantize splits by pixel count, so these straddle both. A
    // height under 6 is less than one band; 512x512 is the quantize split point.
    const int sizes[][2] = { { 1, 1 }, { 4, 5 }, { 64, 6 }, { 64, 7 }, { 511, 511 }, { 512, 512 }, { 592, 592 } };
    Renderer renderer(4);
    for (const auto &s : sizes)
    {
        const int W = s[0];
        const int H = s[1];
        std::string serial;
        std::string parallel;
        for (int arm = 0; arm < 2; arm++)
        {
            Framebuffer fb(
                W, H, /*headless=*/true, ColorMode::TrueColor, sixel_config(std::max(1, W / 8), std::max(1, H / 16))
            );
            if (arm == 1)
            {
                fb.set_parallel_runner({ [&renderer](const std::function<void(int, int)> &fn)
                                         { renderer.run_on_workers(fn); }, renderer.worker_count() });
            }
            fb.clear({ 17, 34, 51 });
            (void)fb.commit_pixel(0, 0, 0.5f, { 200, 100, 50 });
            CaptureStdout cap;
            fb.present();
            (arm == 0 ? serial : parallel) = cap.read();
        }
        // The split is exact, so the frame must be byte-identical at every size.
        ASSERT_TRUE(parallel == serial);
    }
}

TEST(framebuffer, sixel_parallel_matches_serial_byte_for_byte)
{
    // A 900x600 frame engages parallel quantization and band encoding. Both splits
    // are deterministic, so output must match serial bytes exactly.
    constexpr int W = 900;
    constexpr int H = 600;
    const auto paint = [](Framebuffer &fb)
    {
        fb.clear({ 9, 40, 90 });
        for (int y = 0; y < H; y += 3)
        {
            for (int x = 0; x < W; x += 3)
            {
                (void)fb.commit_pixel(
                    x, y, 0.5f,
                    { static_cast<uint8_t>(x * 5), static_cast<uint8_t>(y * 3), static_cast<uint8_t>((x + y) * 7) }
                );
            }
        }
    };

    std::string serial;
    std::string parallel;
    {
        Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, sixel_config(112, 37));
        paint(fb);
        CaptureStdout cap;
        fb.present();
        serial = cap.read();
    }
    {
        Renderer renderer(4);
        Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, sixel_config(112, 37));
        fb.set_parallel_runner({ [&renderer](const std::function<void(int, int)> &fn) { renderer.run_on_workers(fn); },
                                 renderer.worker_count() });
        paint(fb);
        CaptureStdout cap;
        fb.present();
        parallel = cap.read();
    }
    ASSERT_FALSE(serial.empty());
    ASSERT_TRUE(parallel == serial);
}

TEST(framebuffer, sixel_recovers_when_the_pool_produces_nothing)
{
    // A runner that produces nothing models swallowed worker failures in both sixel
    // stages. Serial recovery must prevent partial bands, stale bytes, and invalid
    // palette indices, and reproduce the serial frame exactly.
    constexpr int W = 900;
    constexpr int H = 600;
    const auto paint = [](Framebuffer &fb)
    {
        fb.clear({ 30, 60, 90 });
        for (int y = 0; y < H; y += 5)
        {
            for (int x = 0; x < W; x += 4)
            {
                (void)fb.commit_pixel(
                    x, y, 0.5f, { static_cast<uint8_t>(x), static_cast<uint8_t>(y), static_cast<uint8_t>(x + y) }
                );
            }
        }
    };

    std::string serial;
    std::string recovered;
    {
        Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, sixel_config(112, 37));
        paint(fb);
        CaptureStdout cap;
        fb.present();
        serial = cap.read();
    }
    {
        Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, sixel_config(112, 37));
        fb.set_parallel_runner({ [](const std::function<void(int, int)> &) {}, 8 });
        // Two frames: the second proves a stale per-worker buffer from the first cannot
        // be spliced in, which is why the parts are cleared on the calling thread.
        paint(fb);
        {
            CaptureStdout warmup;
            fb.present();
        }
        paint(fb);
        CaptureStdout cap;
        fb.present();
        recovered = cap.read();
    }
    ASSERT_FALSE(serial.empty());
    ASSERT_TRUE(recovered == serial);
}

TEST(framebuffer, sixel_survives_a_pool_reporting_a_different_worker_count)
{
    // A worker-count mismatch changes band ownership. Mismatched workers must remain
    // uncovered so completeness forces a serial encode instead of a truncated frame.
    constexpr int W = 900;
    constexpr int H = 600;
    const auto paint = [](Framebuffer &fb)
    {
        fb.clear({ 30, 60, 90 });
        for (int y = 0; y < H; y += 5)
        {
            for (int x = 0; x < W; x += 4)
            {
                (void)fb.commit_pixel(
                    x, y, 0.5f, { static_cast<uint8_t>(x), static_cast<uint8_t>(y), static_cast<uint8_t>(x + y) }
                );
            }
        }
    };

    std::string serial;
    {
        Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, sixel_config(112, 37));
        paint(fb);
        CaptureStdout cap;
        fb.present();
        serial = cap.read();
    }
    // Both directions: a pool larger than declared (which would leave the tail unencoded) and a
    // smaller one (which would cover the frame, but in ranges the serial redo does not expect).
    for (const int actual : { 16, 3 })
    {
        Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, sixel_config(112, 37));
        fb.set_parallel_runner({ [actual](const std::function<void(int, int)> &fn)
                                 {
                                     for (int w = 0; w < actual; w++)
                                     {
                                         fn(w, actual);
                                     }
                                 },
                                 8 });
        paint(fb);
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read() == serial);
    }
}

TEST(framebuffer, sixel_present_pixel_roundtrip)
{
    // The full path from the pixel slots to the wire: quantization into the
    // index plane and the emitter, decoded back with the independent decoder.
    Framebuffer fb(8, 12, /*headless=*/true, ColorMode::TrueColor, sixel_config(4, 4));
    fb.clear({ 10, 20, 30 });
    (void)fb.commit_pixel(3, 2, 0.5f, { 200, 100, 50 });
    CaptureStdout cap;
    fb.present();
    const std::string out = cap.read();

    ASSERT_TRUE(out.rfind("\033[?2026h\033[1;1H", 0) == 0); // sync open, image painted from home
    const size_t dcs = out.find("\033P");
    ASSERT_TRUE(dcs != std::string::npos);
    const size_t st = out.find("\033\\", dcs);
    ASSERT_TRUE(st != std::string::npos);
    const SixelFrame f = sixel_decode(out.substr(dcs, (st + 2) - dcs));
    ASSERT_EQ(f.w, 8);
    ASSERT_EQ(f.h, 12);
    const int bg_reg = quantize_256({ 10, 20, 30 }) - 16;
    const int px_reg = quantize_256({ 200, 100, 50 }) - 16;
    for (int y = 0; y < 12; y++)
    {
        for (int x = 0; x < 8; x++)
        {
            const int want = (x == 3 && y == 2) ? px_reg : bg_reg;
            ASSERT_EQ(f.plane[(static_cast<size_t>(y) * 8u) + static_cast<size_t>(x)], want);
        }
    }
}

TEST(framebuffer, sixel_idle_present_emits_no_frame)
{
    // Only a clear() (a rewritten pixel buffer) re-arms the emission; a present
    // with nothing new emits no image bytes at all.
    Framebuffer fb(4, 6, /*headless=*/true, ColorMode::TrueColor, sixel_config(2, 2));
    fb.clear({ 1, 2, 3 });
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().find("\033P") != std::string::npos);
    }
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().empty());
    }
    {
        CaptureStdout cap;
        fb.clear({ 1, 2, 3 });
        fb.present();
        ASSERT_TRUE(cap.read().find("\033P") != std::string::npos);
    }
}

TEST(framebuffer, sixel_zero_rows_emits_no_image)
{
    // A shrink to zero image rows: nothing to paint and, unlike kitty, nothing
    // resident terminal-side to delete (the erase the resize defers into the
    // next present wipes the cells).
    Framebuffer fb(4, 6, /*headless=*/true, ColorMode::TrueColor, sixel_config(2, 2));
    fb.clear({ 1, 2, 3 });
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().find("\033P") != std::string::npos);
    }
    {
        CaptureStdout cap;
        fb.resize(4, 0, 2, 0, 1, 1);
        fb.present();
        // The deferred erase is the frame's whole content, inside the bracket;
        // no image bytes follow it.
        ASSERT_TRUE(cap.read() == "\033[?2026h\033[2J\033[?2026l");
    }
}

TEST(framebuffer, sixel_frame_homes_at_the_configured_origin)
{
    // A geometry-capped image is centered by homing at the config's origin
    // cell instead of 1;1 (CUP is row;col); a resize carries new origins.
    GraphicsConfig cfg = sixel_config(4, 4);
    cfg.origin_col = 26;
    cfg.origin_row = 6;
    Framebuffer fb(4, 6, /*headless=*/true, ColorMode::TrueColor, cfg);
    fb.clear({ 1, 2, 3 });
    {
        // Exact prefix: the CUP must precede the image bytes.
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().rfind("\033[?2026h\033[6;26H", 0) == 0);
    }
    fb.resize(4, 6, 4, 4, 2, 3);
    fb.clear({ 1, 2, 3 });
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().rfind("\033[?2026h\033[2J\033[3;2H", 0) == 0);
    }
}

TEST(framebuffer, sixel_hud_row_sits_below_the_image_rows)
{
    Framebuffer fb(4, 6, /*headless=*/true, ColorMode::TrueColor, sixel_config(4, 4));
    fb.clear({ 1, 2, 3 });
    fb.set_hud("HUDMARK");
    CaptureStdout cap;
    fb.present();
    const std::string out = cap.read();
    ASSERT_TRUE(out.find("\033[5;1H") != std::string::npos); // cfg rows + 1
    ASSERT_TRUE(out.find("HUDMARK") != std::string::npos);
}

// synchronized output (mode 2026)

TEST(framebuffer, sync_output_wraps_every_backend_frame)
{
    const auto wrapped = [](const std::string &out)
    {
        ASSERT_TRUE(out.rfind("\033[?2026h", 0) == 0);
        ASSERT_TRUE(out.size() >= 8 && out.compare(out.size() - 8, 8, "\033[?2026l") == 0);
    };
    {
        Framebuffer blocks(4, 4, /*headless=*/true);
        blocks.clear({ 1, 2, 3 });
        CaptureStdout cap;
        blocks.present();
        wrapped(cap.read());
    }
    {
        Framebuffer kitty_fb(4, 4, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(2, 2));
        kitty_fb.clear({ 1, 2, 3 });
        CaptureStdout cap;
        kitty_fb.present();
        wrapped(cap.read());
    }
    {
        Framebuffer sixel_fb(4, 6, /*headless=*/true, ColorMode::TrueColor, sixel_config(2, 2));
        sixel_fb.clear({ 1, 2, 3 });
        CaptureStdout cap;
        sixel_fb.present();
        wrapped(cap.read());
    }
}

TEST(framebuffer, sync_output_idle_blocks_frame_stays_zero_bytes)
{
    // A fully clean incremental frame with an unchanged HUD composes nothing:
    // the trailing SGR reset is skipped along with the cells, so blocks idle
    // frames take end_frame's zero-byte path like the pixel backends.
    Framebuffer fb(4, 4, /*headless=*/true);
    fb.clear({ 1, 2, 3 });
    fb.set_hud("HUD");
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(!cap.read().empty());
    }
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().empty());
    }
}

TEST(framebuffer, resize_erase_is_deferred_into_the_next_frame_bracket)
{
    // resize() defers its erase into the next synchronized present, avoiding a blank frame.
    Framebuffer fb(4, 4, /*headless=*/true);
    fb.clear({ 1, 2, 3 });
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(!cap.read().empty());
    }
    {
        CaptureStdout cap;
        fb.resize(6, 4);
        ASSERT_TRUE(cap.read().empty());
    }
    {
        CaptureStdout cap;
        fb.present();
        const std::string out = cap.read();
        ASSERT_TRUE(out.rfind("\033[?2026h\033[2J", 0) == 0);
    }
    {
        // The deferred erase is consumed by exactly one frame: the next clean
        // present composes nothing (a leaked flag would re-erase every frame).
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().empty());
    }
}

TEST(framebuffer, resize_erase_precedes_the_image_in_the_same_bracket)
{
    // The deferred \033[2J must land BEFORE the frame's image bytes: erased
    // after the transmit it would wipe what the frame just painted (and on
    // kitty can drop the placement terminal-side).
    {
        Framebuffer fb(4, 6, /*headless=*/true, ColorMode::TrueColor, sixel_config(2, 2));
        fb.clear({ 1, 2, 3 });
        {
            CaptureStdout cap;
            fb.present();
        }
        CaptureStdout cap;
        fb.resize(4, 6, 2, 2, 1, 1);
        fb.present();
        ASSERT_TRUE(cap.read().rfind("\033[?2026h\033[2J\033[1;1H", 0) == 0);
    }
    {
        Framebuffer fb(4, 4, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(2, 2));
        fb.clear({ 1, 2, 3 });
        {
            CaptureStdout cap;
            fb.present();
        }
        CaptureStdout cap;
        fb.resize(4, 4, 2, 2, 1, 1);
        fb.present();
        ASSERT_TRUE(cap.read().rfind("\033[?2026h\033[2J\033[1;1H", 0) == 0);
    }
}

TEST(framebuffer, sync_output_idle_pixel_frame_stays_zero_bytes)
{
    // An idle pixel-backend present must not emit a bare 2026 pair at the idle
    // pacing rate: nothing composed means nothing written.
    Framebuffer fb(4, 4, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(2, 2));
    fb.clear({ 1, 2, 3 });
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(!cap.read().empty());
    }
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().empty());
    }
}

TEST(framebuffer, sixel_hud_only_change_emits_no_frame)
{
    // An unchanged scene with a changed HUD line repaints the HUD alone.
    Framebuffer fb(4, 6, /*headless=*/true, ColorMode::TrueColor, sixel_config(2, 2));
    fb.clear({ 1, 2, 3 });
    fb.set_hud("first");
    {
        CaptureStdout cap;
        fb.present();
        ASSERT_TRUE(cap.read().find("\033P") != std::string::npos);
    }
    {
        CaptureStdout cap;
        fb.set_hud("second");
        fb.present();
        const std::string out = cap.read();
        ASSERT_TRUE(out.find("\033P") == std::string::npos);
        ASSERT_TRUE(out.find("second") != std::string::npos);
    }
}

TEST(framebuffer, kitty_staging_fill_survives_a_pool_reporting_a_different_worker_count)
{
    // Seed staging through a matching pool, then use a mismatched worker count.
    // Mismatched workers must stay uncovered so serial redo replaces every stale range.
    constexpr int W = 640;
    constexpr int H = 480; // 307200 px, past write_rgb's 1<<18 split threshold
    const auto paint = [](Framebuffer &fb, int frame)
    {
        fb.clear(frame == 0 ? Color{ 11, 22, 33 } : Color{ 200, 190, 180 });
        for (int y = 0; y < H; y += 3)
        {
            for (int x = 0; x < W; x += 7)
            {
                (void)fb.commit_pixel(
                    x, y, 0.5f,
                    { static_cast<uint8_t>((x + frame) & 0xFF), static_cast<uint8_t>(y & 0xFF),
                      static_cast<uint8_t>((x + y) & 0xFF) }
                );
            }
        }
    };
    const auto runner = [](int report)
    {
        return Framebuffer::ParallelRunner{ [report](const std::function<void(int, int)> &fn)
                                            {
                                                for (int w = 0; w < report; w++)
                                                {
                                                    fn(w, report);
                                                }
                                            },
                                            8 };
    };
    const auto second_frame = [&](int actual)
    {
        Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(80, 30));
        fb.set_parallel_runner(runner(8)); // matching: frame 0 fills the staging buffer everywhere
        paint(fb, 0);
        {
            CaptureStdout warmup;
            fb.present();
        }
        fb.set_parallel_runner(runner(actual));
        paint(fb, 1);
        std::string out;
        {
            CaptureStdout cap;
            fb.present();
            out = cap.read();
        }
        const auto z = b64_decode(collect_apc_payload(out));
        std::vector<unsigned char> rgb(static_cast<size_t>(W) * static_cast<size_t>(H) * 3u);
        const auto rgb_len = static_cast<unsigned int>(rgb.size());
        mz_ulong len = rgb_len;
        ASSERT_EQ(mz_uncompress(rgb.data(), &len, z.data(), static_cast<unsigned int>(z.size())), MZ_OK);
        ASSERT_EQ(len, rgb_len);
        return rgb;
    };

    const std::vector<unsigned char> good = second_frame(8);
    ASSERT_TRUE(second_frame(16) == good); // a bigger pool would cover only the frame's front
    ASSERT_TRUE(second_frame(3) == good);  // a smaller one, ranges the redo does not expect
}

TEST(framebuffer, kitty_staging_fill_recovers_from_an_incomplete_pool)
{
    // Seed staging with a complete frame, then run only even worker ids. Serial redo
    // must overwrite every uncovered range rather than transmit stale pixels from frame 0.
    constexpr int W = 640;
    constexpr int H = 480; // 307200 px, past write_rgb's 1<<18 split threshold
    const auto paint = [](Framebuffer &fb, int frame)
    {
        fb.clear(frame == 0 ? Color{ 11, 22, 33 } : Color{ 200, 190, 180 });
        for (int y = 0; y < H; y += 3)
        {
            for (int x = 0; x < W; x += 7)
            {
                (void)fb.commit_pixel(
                    x, y, 0.5f,
                    { static_cast<uint8_t>((x + frame) & 0xFF), static_cast<uint8_t>(y & 0xFF),
                      static_cast<uint8_t>((x + y) & 0xFF) }
                );
            }
        }
    };
    const auto runner = [](int step)
    {
        return Framebuffer::ParallelRunner{ [step](const std::function<void(int, int)> &fn)
                                            {
                                                for (int w = 0; w < 8; w += step)
                                                {
                                                    fn(w, 8);
                                                }
                                            },
                                            8 };
    };
    const auto second_frame = [&](int step)
    {
        Framebuffer fb(W, H, /*headless=*/true, ColorMode::TrueColor, direct_kitty_config(80, 30));
        fb.set_parallel_runner(runner(1)); // every worker: frame 0 fills the buffer everywhere
        paint(fb, 0);
        {
            CaptureStdout warmup;
            fb.present();
        }
        fb.set_parallel_runner(runner(step));
        paint(fb, 1);
        std::string out;
        {
            CaptureStdout cap;
            fb.present();
            out = cap.read();
        }
        const auto z = b64_decode(collect_apc_payload(out));
        std::vector<unsigned char> rgb(static_cast<size_t>(W) * static_cast<size_t>(H) * 3u);
        const auto rgb_len = static_cast<unsigned int>(rgb.size());
        mz_ulong len = rgb_len;
        ASSERT_EQ(mz_uncompress(rgb.data(), &len, z.data(), static_cast<unsigned int>(z.size())), MZ_OK);
        ASSERT_EQ(len, rgb_len);
        return rgb;
    };

    ASSERT_TRUE(second_frame(2) == second_frame(1));
}
