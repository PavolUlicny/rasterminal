#include "framebuffer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>

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
                t.s[i][0] = static_cast<char>('0' + i / 10);
                t.s[i][1] = static_cast<char>('0' + i % 10);
                t.len[i] = 2;
            }
            else
            {
                t.s[i][0] = static_cast<char>('0' + i / 100);
                t.s[i][1] = static_cast<char>('0' + (i / 10) % 10);
                t.s[i][2] = static_cast<char>('0' + i % 10);
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
            buf[i] = tmp[len - 1 - i];
        return len;
    }

} // namespace

Framebuffer::Framebuffer(int pixel_width, int pixel_height, bool headless)
    : m_width(pixel_width),
      m_height(pixel_height),
      m_color(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height)),
      m_prev_color(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height)),
      m_depth(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height), std::numeric_limits<float>::infinity()),
      m_headless(headless)
{
    if (!m_headless)
    {
        // Preallocate: ~50 bytes per terminal cell is a safe upper bound.
        m_buf.reserve(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height / 2) * 50u);
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
    m_color.assign(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height), Color{});
    m_prev_color.assign(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height), Color{});
    m_depth.assign(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height), std::numeric_limits<float>::infinity());
    m_buf.clear();
    m_buf.reserve(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height / 2) * 50u);
    m_force_redraw = true;

    // Wipe any leftover content from the previous (possibly larger) terminal.
    std::fputs("\033[2J", stdout);
    std::fflush(stdout);
}

void Framebuffer::clear(Color bg)
{
    std::fill(m_color.begin(), m_color.end(), bg);
    std::fill(m_depth.begin(), m_depth.end(), std::numeric_limits<float>::infinity());
}

void Framebuffer::present()
{
    m_buf.clear();

    const int term_rows = m_height / 2;

    char tmp[48]; // 36 bytes worst case for combined fg+bg SGR sequence
    int n = 0;

    // fg_known / bg_known track whether the terminal's current fg/bg are
    // reflected by prev_fg / prev_bg.  Both start false because \033[0m at
    // the end of the previous frame reset SGR to an unknown terminal default.
    Color prev_fg{};
    Color prev_bg{};
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

    // When top == bot, a space with bg-only SGR is visually identical to ▀
    // with fg==bg, saving 2+ bytes and skipping the fg SGR entirely.
    auto emit_cell = [&](const Color &top, const Color &bot)
    {
        if (top == bot)
        {
            const bool bg_change = !bg_known || bot != prev_bg;
            if (bg_change)
            {
                tmp[0] = '\033';
                tmp[1] = '[';
                n = 2;
                tmp[n++] = '4';
                tmp[n++] = '8';
                tmp[n++] = ';';
                tmp[n++] = '2';
                tmp[n++] = ';';
                n += write_byte(tmp + n, bot.r);
                tmp[n++] = ';';
                n += write_byte(tmp + n, bot.g);
                tmp[n++] = ';';
                n += write_byte(tmp + n, bot.b);
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

            // One SGR sequence covering whichever of fg/bg changed; combining
            // when both change avoids a redundant ESC[ header and closing m.
            if (fg_change || bg_change)
            {
                tmp[0] = '\033';
                tmp[1] = '[';
                n = 2;
                if (fg_change)
                {
                    tmp[n++] = '3';
                    tmp[n++] = '8';
                    tmp[n++] = ';';
                    tmp[n++] = '2';
                    tmp[n++] = ';';
                    n += write_byte(tmp + n, top.r);
                    tmp[n++] = ';';
                    n += write_byte(tmp + n, top.g);
                    tmp[n++] = ';';
                    n += write_byte(tmp + n, top.b);
                    prev_fg = top;
                    fg_known = true;
                    if (bg_change)
                        tmp[n++] = ';';
                }
                if (bg_change)
                {
                    tmp[n++] = '4';
                    tmp[n++] = '8';
                    tmp[n++] = ';';
                    tmp[n++] = '2';
                    tmp[n++] = ';';
                    n += write_byte(tmp + n, bot.r);
                    tmp[n++] = ';';
                    n += write_byte(tmp + n, bot.g);
                    tmp[n++] = ';';
                    n += write_byte(tmp + n, bot.b);
                    prev_bg = bot;
                    bg_known = true;
                }
                tmp[n++] = 'm';
                m_buf.append(tmp, static_cast<size_t>(n));
            }
            m_buf.append(UPPER_HALF, 3);
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
                emit_cell(m_color[top_base + static_cast<size_t>(col)],
                          m_color[bot_base + static_cast<size_t>(col)]);
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
                if (m_color[top_base + static_cast<size_t>(col)] != m_prev_color[top_base + static_cast<size_t>(col)] ||
                    m_color[bot_base + static_cast<size_t>(col)] != m_prev_color[bot_base + static_cast<size_t>(col)])
                {
                    if (!row_started)
                    {
                        append_cursor_pos(row + 1, col + 1);
                        row_started = true;
                    }
                    emit_cell(m_color[top_base + static_cast<size_t>(col)],
                              m_color[bot_base + static_cast<size_t>(col)]);
                    col++;
                }
                else if (!row_started)
                {
                    col++;
                }
                else
                {
                    int skip = 0;
                    while (col + skip < m_width &&
                           m_color[top_base + static_cast<size_t>(col + skip)] == m_prev_color[top_base + static_cast<size_t>(col + skip)] &&
                           m_color[bot_base + static_cast<size_t>(col + skip)] == m_prev_color[bot_base + static_cast<size_t>(col + skip)])
                        skip++;
                    if (skip == 1)
                        m_buf.append("\033[C", 3);
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
        m_buf += "\033[48;2;18;18;18m\033[38;2;160;160;160m";
        m_buf += m_hud;
        m_buf += "\033[K"; // erase to end of line (clears leftover from wider text)
        m_buf += "\033[0m";
        m_buf += "\033[?7h"; // re-enable auto-wrap
    }

    // Return values intentionally ignored: a write failure to a terminal means
    // the session is already broken; there is no meaningful recovery path here.
    (void)std::fwrite(m_buf.data(), 1, m_buf.size(), stdout);
    (void)std::fflush(stdout);

    // Swap instead of copy: O(1) and leaves m_prev_color holding the frame
    // we just rendered — the correct reference for the next dirty comparison.
    std::swap(m_color, m_prev_color);
}
