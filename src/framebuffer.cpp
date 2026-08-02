#include "framebuffer.h"

#include "color.h" // Color, ColorMode

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{

    // UTF-8 encoding of ▀ (U+2580 UPPER HALF BLOCK)
    // Top pixel → foreground color, bottom pixel → background color.
    const char UPPER_HALF[] = "\xe2\x96\x80";

    struct ByteLut
    {
        char s[256][3];
        uint8_t len[256];
    };
    constexpr ByteLut make_byte_lut() noexcept
    {
        ByteLut t{};
        for (int i = 0; i < 256; ++i)
        {
            if (i < 10)
            {
                t.s[i][0] = static_cast<char>('0' + i);
                t.len[i] = 1;
            }
            else if (i < 100)
            {
                t.s[i][0] = static_cast<char>('0' + (i / 10));
                t.s[i][1] = static_cast<char>('0' + (i % 10));
                t.len[i] = 2;
            }
            else
            {
                t.s[i][0] = static_cast<char>('0' + (i / 100));
                t.s[i][1] = static_cast<char>('0' + ((i / 10) % 10));
                t.s[i][2] = static_cast<char>('0' + (i % 10));
                t.len[i] = 3;
            }
        }
        return t;
    }
    constexpr ByteLut byte_lut = make_byte_lut();

    // Always writes 3 bytes (single store); caller advances by the returned length.
    // 1-2 slop bytes past len are within tmp[48] and overwritten by the next write.
    int write_byte(char *buf, uint8_t v)
    {
        buf[0] = byte_lut.s[v][0];
        buf[1] = byte_lut.s[v][1];
        buf[2] = byte_lut.s[v][2];
        return byte_lut.len[v];
    }

    // Fast integer-to-string: writes decimal digits of v into buf, returns length.
    int write_int(char *buf, int v)
    {
        if (v == 0)
        {
            buf[0] = '0';
            return 1;
        }
        char tmp[12]; // 10 digits max for INT_MAX (2147483647) + headroom
        int len = 0;
        while (v > 0)
        {
            tmp[len++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
        for (int i = 0; i < len; i++)
        {
            buf[i] = tmp[len - 1 - i];
        }
        return len;
    }

} // namespace

Framebuffer::Framebuffer(int pixel_width, int pixel_height, bool headless, ColorMode mode)
    : m_width(pixel_width), m_height(pixel_height),
      m_pixel(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height)),
      m_prev_color(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height), 0u), m_headless(headless),
      m_mode(mode)
{
    fill_cleared(0u);
    if (!m_headless)
    {
        // Precondition: stdout is a terminal (main.cpp enforces the tty check
        // before constructing us); this path and present() write ANSI to it.
        // Preallocate a mode-dependent per-cell upper bound (see buf_reserve_bytes).
        m_buf.reserve(buf_reserve_bytes());
        std::fputs("\033[?1049h", stdout); // enter alternate screen buffer
        std::fputs("\033[?25l", stdout);   // hide cursor
        std::fflush(stdout);
    }
}

Framebuffer::~Framebuffer()
{
    if (!m_headless)
    {
        // Restore cursor, reset colours, then leave the alternate screen buffer —
        // this restores the terminal to exactly the state it was in before launch.
        std::fputs("\033[?25h\033[0m\033[?1049l", stdout);
        std::fflush(stdout);
    }
}

void Framebuffer::resize(int pixel_width, int pixel_height)
{
    m_width = pixel_width;
    m_height = pixel_height;
    const size_t npx = static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height);
    m_pixel = std::vector<std::atomic<uint64_t>>(npx);
    m_prev_color = std::vector<uint32_t>(npx, 0u);
    fill_cleared(0u);
    m_buf.clear();
    m_buf.reserve(buf_reserve_bytes());
    m_force_redraw = true;

    // Wipe any leftover content from the previous (possibly larger) terminal.
    std::fputs("\033[2J", stdout);
    std::fflush(stdout);
}

void Framebuffer::clear(Color bg)
{
    fill_cleared(pack_color(bg));
}

void Framebuffer::present()
{
    if (m_mode == ColorMode::TrueColor)
    {
        present_impl<true>();
    }
    else
    {
        present_impl<false>();
    }
}

template <bool TC> void Framebuffer::present_impl()
{
    m_buf.clear();

    // Captured before the pixel section consumes the flag: the HUD must also be re-emitted on
    // any frame that repaints every cell (first frame, resize), since \033[2J wiped its row.
    const bool full_redraw = m_force_redraw;

    const int term_rows = m_height / 2;

    char tmp[48]; // 36 bytes worst case for combined fg+bg SGR sequence
    int n = 0;

    // Table pointer hoisted out of the pixel loops so quant256_lut()'s magic-static init guard is
    // paid once per frame, not per load_color call. Never read in the truecolor instantiation.
    const uint8_t *qlut = nullptr;
    if constexpr (!TC)
    {
        qlut = quant256_lut().data();
    }

    // fg_known / bg_known track whether the terminal's current fg/bg are
    // reflected by prev_fg / prev_bg.  Both start false because \033[0m at
    // the end of the previous frame reset SGR to an unknown terminal default.
    // prev_fg / prev_bg hold the raw cell value (packed RGB in truecolor, palette
    // index in 256; see load_color), compared directly against the incoming raw.
    uint32_t prev_fg = 0;
    uint32_t prev_bg = 0;
    bool fg_known = false;
    bool bg_known = false;

    // \033[row;colH — 1-based coordinates, no reliance on newlines or auto-wrap.
    auto append_cursor_pos = [&](int row, int col)
    {
        tmp[0] = '\033';
        tmp[1] = '[';
        n = 2;
        n += write_int(tmp + n, row);
        tmp[n++] = ';';
        n += write_int(tmp + n, col);
        tmp[n++] = 'H';
        m_buf.append(tmp, static_cast<size_t>(n));
    };

    // \033[NC — advance the cursor N columns; the bare form is 1 byte shorter for N == 1.
    auto append_cursor_advance = [&](int cols)
    {
        if (cols == 1)
        {
            m_buf.append("\033[C", 3);
            return;
        }
        tmp[0] = '\033';
        tmp[1] = '[';
        n = 2;
        n += write_int(tmp + n, cols);
        tmp[n++] = 'C';
        m_buf.append(tmp, static_cast<size_t>(n));
    };

    // Append one SGR colour body (no leading ESC[ or trailing m) into tmp at n. Deliberately not
    // color.h's append_fg_sgr, which is the shared spelling for styling a string: this one emits a
    // BODY so a changed fg and bg combine into a single escape, writes into a fixed buffer rather
    // than a std::string, and goes through write_byte's LUT to avoid a division. It runs per cell
    // per frame, where those three differences are the point; append_fg_sgr runs a dozen times a
    // frame, where clarity is. The two colour modes
    // differ only here: 38;2;r;g;b vs 38;5;idx for fg, 48;2;r;g;b vs 48;5;idx for bg. `if constexpr`
    // gives each present_impl instantiation only its own body, so the truecolor bytes are exactly the
    // historical output. `raw` is a raw cell value from load_color (packed RGB in truecolor, palette
    // index in the low byte in 256). `lead` is '3' (fg) or '4' (bg).
    auto write_color = [&](char lead, uint32_t raw)
    {
        tmp[n++] = lead;
        tmp[n++] = '8';
        tmp[n++] = ';';
        if constexpr (TC)
        {
            const Color c = unpack_color(raw);
            tmp[n++] = '2';
            tmp[n++] = ';';
            n += write_byte(tmp + n, c.r);
            tmp[n++] = ';';
            n += write_byte(tmp + n, c.g);
            tmp[n++] = ';';
            n += write_byte(tmp + n, c.b);
        }
        else
        {
            tmp[n++] = '5';
            tmp[n++] = ';';
            n += write_byte(tmp + n, static_cast<uint8_t>(raw));
        }
    };

    // Cell emission, shared across modes. When top == bot a space with bg-only SGR is visually
    // identical to ▀ with fg==bg, saving the fg SGR and 2+ bytes; otherwise emit ▀ with one combined
    // SGR covering whichever of fg/bg changed (combining avoids a redundant ESC[ header and closing m).
    // top/bot are raw cell values compared directly (raw-equal == colour/index-equal in both modes).
    auto emit_cell = [&](uint32_t top, uint32_t bot)
    {
        if (top == bot)
        {
            const bool bg_change = !bg_known || bot != prev_bg;
            if (bg_change)
            {
                tmp[0] = '\033';
                tmp[1] = '[';
                n = 2;
                write_color('4', bot);
                tmp[n++] = 'm';
                m_buf.append(tmp, static_cast<size_t>(n));
                prev_bg = bot;
                bg_known = true;
                // fg_known intentionally unchanged: terminal fg is unaffected.
            }
            m_buf += ' ';
        }
        else
        {
            const bool fg_change = !fg_known || top != prev_fg;
            const bool bg_change = !bg_known || bot != prev_bg;
            if (fg_change || bg_change)
            {
                tmp[0] = '\033';
                tmp[1] = '[';
                n = 2;
                if (fg_change)
                {
                    write_color('3', top);
                    prev_fg = top;
                    fg_known = true;
                    if (bg_change)
                    {
                        tmp[n++] = ';';
                    }
                }
                if (bg_change)
                {
                    write_color('4', bot);
                    prev_bg = bot;
                    bg_known = true;
                }
                tmp[n++] = 'm';
                m_buf.append(tmp, static_cast<size_t>(n));
            }
            m_buf.append(UPPER_HALF, 3);
        }
    };

    // The value stored per cell and compared against m_prev_color: packed RGB in truecolor (byte-for-byte
    // as before), or the xterm-256 index in 256 mode. Quantizing here means two distinct RGB values that
    // collapse to the same index read as unchanged, so the incremental skip path coalesces them.
    auto load_color = [&](size_t i) -> uint32_t
    {
        const uint32_t rgb = unpack_color_bits(m_pixel[i].load(std::memory_order_relaxed));
        if constexpr (TC)
        {
            return rgb;
        }
        else
        {
            return qlut[quant256_idx(unpack_color(rgb))];
        }
    };

    if (m_force_redraw)
    {
        for (int row = 0; row < term_rows; row++)
        {
            append_cursor_pos(row + 1, 1);

            const int prow = row * 2;
            const size_t top_base = pixel_idx(0, prow);
            // Guard against odd pixel height: reuse top pixel for bottom row.
            const size_t bot_base = pixel_idx(0, prow + 1 < m_height ? prow + 1 : prow);

            for (int col = 0; col < m_width; col++)
            {
                const size_t ti = top_base + static_cast<size_t>(col);
                const size_t bi = bot_base + static_cast<size_t>(col);
                const uint32_t tc = load_color(ti);
                const uint32_t bc = load_color(bi);
                emit_cell(tc, bc);
                m_prev_color[ti] = tc;
                m_prev_color[bi] = bc;
            }
        }
        m_force_redraw = false;
    }
    else
    {
        for (int row = 0; row < term_rows; row++)
        {
            const int prow = row * 2;
            const size_t top_base = pixel_idx(0, prow);
            const size_t bot_base = pixel_idx(0, prow + 1 < m_height ? prow + 1 : prow);

            bool row_started = false;
            int col = 0;
            while (col < m_width)
            {
                const size_t ti = top_base + static_cast<size_t>(col);
                const size_t bi = bot_base + static_cast<size_t>(col);
                const uint32_t top_cur = load_color(ti);
                const uint32_t bot_cur = load_color(bi);
                if (top_cur != m_prev_color[ti] || bot_cur != m_prev_color[bi])
                {
                    if (!row_started)
                    {
                        append_cursor_pos(row + 1, col + 1);
                        row_started = true;
                    }
                    emit_cell(top_cur, bot_cur);
                    m_prev_color[ti] = top_cur;
                    m_prev_color[bi] = bot_cur;
                    col++;
                }
                else if (!row_started)
                {
                    col++;
                }
                else
                {
                    // col+0 is already known unchanged (dirty check above); start at 1.
                    int skip = 1;
                    while (col + skip < m_width &&
                           load_color(top_base + static_cast<size_t>(col + skip)) ==
                               m_prev_color[top_base + static_cast<size_t>(col + skip)] &&
                           load_color(bot_base + static_cast<size_t>(col + skip)) ==
                               m_prev_color[bot_base + static_cast<size_t>(col + skip)])
                    {
                        skip++;
                    }
                    col += skip;
                    // A clean run reaching the row end needs no advance: the cursor is hidden
                    // and never read back, and anything written after it (a later dirty row,
                    // the HUD, or the next frame) positions absolutely. With --no-hud on the
                    // last row nothing follows at all. Emitting one would be dead bytes on
                    // every row that ends in background.
                    if (col < m_width)
                    {
                        append_cursor_advance(skip);
                    }
                }
            }
        }
    }

    // Reset SGR once at the end of the pixel section — keeps the terminal clean
    // when the HUD is empty and prevents bleed into HUD's own colour escapes.
    m_buf += "\033[0m";

    // The HUD changes at the fps latch rate or on a keypress, far below the frame rate, so an
    // unchanged line is skipped outright (most frames emit zero HUD bytes). A full redraw always
    // re-emits: \033[2J wiped the row.
    //
    // This does give up the old block's incidental self-healing, where re-writing the row every
    // frame repainted it after something else wrote over the terminal. Deliberate: the pixel
    // rows above have never had that property (they are diffed against m_prev_color the same
    // way), so unconditional HUD output could only ever restore one row of a screen that was
    // corrupted as a whole, and the recovery for both is the same full redraw, which any resize
    // already triggers.
    if (m_hud.empty())
    {
        // Nothing is drawn, so nothing is on the row that a later frame could match against.
        // Without this the pair (empty HUD, full redraw) would leave m_prev_hud naming a line
        // the \033[2J just wiped, and restoring that same line afterwards would be skipped as
        // unchanged, forever. Not reachable from main.cpp, which sets a non-empty line every
        // frame or none at all, but the skip must not depend on that.
        m_prev_hud.clear();
    }
    else if (full_redraw || m_hud != m_prev_hud)
    {
        append_cursor_pos(term_rows + 1, 1);

        // Disable auto-wrap so a long HUD string clips at the terminal edge
        // instead of wrapping onto the next line and corrupting the display.
        m_buf += "\033[?7l";
        if constexpr (TC)
        {
            // bg {18,18,18} and a default fg {220,220,220}; keep both in sync with the Palette256
            // branch and the test pins (quantize_256_grey_ramp_exact ties 233 and 253 below to
            // the quantizer of these same RGBs). A composed line overrides the fg immediately
            // with its own per-segment SGRs, so this exists for the other kind of caller: set_hud
            // takes an arbitrary string, and an unstyled one would otherwise draw in whatever
            // foreground the terminal happened to be left with. {220,220,220} is hud.cpp's
            // FG_VALUE, so an unstyled line matches the styled line's primary text.
            static_assert(
                HUD_BAR_BG.r == 18 && HUD_BAR_BG.g == 18 && HUD_BAR_BG.b == 18 && HUD_BAR_FG.r == 220 &&
                    HUD_BAR_FG.g == 220 && HUD_BAR_FG.b == 220,
                "the escape literal below spells these out; change both together"
            );
            m_buf += "\033[48;2;18;18;18m\033[38;2;220;220;220m";
        }
        else
        {
            m_buf += "\033[48;5;233m\033[38;5;253m";
        }
        // Erase BEFORE drawing, never after: the line runs the full width, and with auto-wrap
        // off the cursor finishes ON the last column, so a trailing EL0 would erase from that
        // column inclusive and eat the final character (invisible to byte-level tests and to
        // pyte; only a real terminal shows it, see the tmux recipe in CLAUDE.md). Erasing first
        // also clears residue from a previously wider line, and on BCE terminals paints the row
        // in the bar background, covering the few columns the composer can underfill when it
        // rounds a doubtful glyph width up; non-BCE terminals get the bar from its own padding.
        m_buf += "\033[K";
        m_buf += m_hud;
        m_buf += "\033[0m";
        m_buf += "\033[?7h"; // re-enable auto-wrap
        m_prev_hud = m_hud;
    }

    // Return values intentionally ignored: a write failure to a terminal means
    // the session is already broken; there is no meaningful recovery path here.
    (void)std::fwrite(m_buf.data(), 1, m_buf.size(), stdout);
    (void)std::fflush(stdout);
}
