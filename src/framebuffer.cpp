#include "framebuffer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>

// The HUD grey is authored as RGB {18,18,18} bg / {160,160,160} fg. present_impl emits it as a literal
// 24-bit SGR in TrueColor and as the hard-coded palette indices 233/247 in Palette256. These asserts pin
// the palette indices to the quantizer of those RGBs, so the 256 constants can't drift from the quantizer;
// the TrueColor literal must be kept matching the same RGBs by hand (a comment marks it at the emit site).
static_assert(quantize_256(Color{ 18, 18, 18 }) == 233, "HUD 256 bg constant out of sync with quantize_256");
static_assert(quantize_256(Color{ 160, 160, 160 }) == 247, "HUD 256 fg constant out of sync with quantize_256");

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

    const int term_rows = m_height / 2;

    char tmp[48]; // 36 bytes worst case for combined fg+bg SGR sequence
    int n = 0;

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

    // Append one SGR colour body (no leading ESC[ or trailing m) into tmp at n. The two colour modes
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
            return quantize_256(unpack_color(rgb));
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
                    if (skip == 1)
                    {
                        m_buf.append("\033[C", 3);
                    }
                    else
                    {
                        tmp[0] = '\033';
                        tmp[1] = '[';
                        n = 2;
                        n += write_int(tmp + n, skip);
                        tmp[n++] = 'C';
                        m_buf.append(tmp, static_cast<size_t>(n));
                    }
                    col += skip;
                }
            }
        }
    }

    // Reset SGR once at the end of the pixel section — keeps the terminal clean
    // when the HUD is empty and prevents bleed into HUD's own colour escapes.
    m_buf += "\033[0m";

    if (!m_hud.empty())
    {
        append_cursor_pos(term_rows + 1, 1);

        // Disable auto-wrap so a long HUD string clips at the terminal edge
        // instead of wrapping onto the next line and corrupting the display.
        m_buf += "\033[?7l";
        if constexpr (TC)
        {
            // bg {18,18,18} / fg {160,160,160}; keep in sync with the Palette256 branch + the file-scope
            // static_asserts (which pin the 233/247 below to the quantizer of these same RGBs).
            m_buf += "\033[48;2;18;18;18m\033[38;2;160;160;160m";
        }
        else
        {
            m_buf += "\033[48;5;233m\033[38;5;247m";
        }
        m_buf += m_hud;
        m_buf += "\033[K"; // erase to end of line (clears leftover from wider text)
        m_buf += "\033[0m";
        m_buf += "\033[?7h"; // re-enable auto-wrap
    }

    // Return values intentionally ignored: a write failure to a terminal means
    // the session is already broken; there is no meaningful recovery path here.
    (void)std::fwrite(m_buf.data(), 1, m_buf.size(), stdout);
    (void)std::fflush(stdout);
}
