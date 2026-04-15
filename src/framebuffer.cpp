#include "framebuffer.h"

#include <algorithm>
#include <cstdio>

// UTF-8 encoding of ▀ (U+2580 UPPER HALF BLOCK)
// Top pixel → foreground color, bottom pixel → background color.
static const char UPPER_HALF[] = "\xe2\x96\x80";

// Fast integer-to-string: writes decimal digits of v into buf, returns length.
static int write_int(char *buf, int v)
{
    if (v == 0)
    {
        buf[0] = '0';
        return 1;
    }
    char tmp[4];
    int len = 0;
    while (v > 0)
    {
        tmp[len++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int i = 0; i < len; i++)
        buf[i] = tmp[len - 1 - i];
    return len;
}

Framebuffer::Framebuffer(int pixel_width, int pixel_height)
    : m_width(pixel_width),
      m_height(pixel_height),
      m_color((size_t)(pixel_width * pixel_height)),
      m_depth((size_t)(pixel_width * pixel_height), std::numeric_limits<float>::infinity())
{
    // Preallocate: ~50 bytes per terminal cell is a safe upper bound.
    m_buf.reserve((size_t)(pixel_width * (pixel_height / 2) * 50));

    std::fputs("\033[?1049h", stdout); // enter alternate screen buffer
    std::fputs("\033[?25l", stdout);   // hide cursor
    std::fflush(stdout);
}

Framebuffer::~Framebuffer()
{
    // Restore cursor, reset colours, then leave the alternate screen buffer —
    // this restores the terminal to exactly the state it was in before launch.
    std::fputs("\033[?25h\033[0m\033[?1049l", stdout);
    std::fflush(stdout);
}

void Framebuffer::resize(int pixel_width, int pixel_height)
{
    m_width = pixel_width;
    m_height = pixel_height;
    m_color.assign((size_t)(pixel_width * pixel_height), Color{});
    m_depth.assign((size_t)(pixel_width * pixel_height), std::numeric_limits<float>::infinity());
    m_buf.clear();
    m_buf.reserve((size_t)(pixel_width * (pixel_height / 2) * 50));

    // Wipe any leftover content from the previous (possibly larger) terminal.
    std::fputs("\033[2J", stdout);
    std::fflush(stdout);
}

void Framebuffer::clear(Color bg)
{
    std::fill(m_color.begin(), m_color.end(), bg);
    std::fill(m_depth.begin(), m_depth.end(), std::numeric_limits<float>::infinity());
}

bool Framebuffer::test_and_set_depth(int x, int y, float depth)
{
    if (x < 0 || x >= m_width || y < 0 || y >= m_height)
        return false;
    float &d = m_depth[(size_t)(y * m_width + x)];
    if (depth < d)
    {
        d = depth;
        return true;
    }
    return false;
}

void Framebuffer::set_pixel(int x, int y, Color color)
{
    if (x < 0 || x >= m_width || y < 0 || y >= m_height)
        return;
    m_color[(size_t)(y * m_width + x)] = color;
}

void Framebuffer::present()
{
    m_buf.clear();

    // Move cursor to (1,1) before each frame — no erase needed because every
    // cell is overwritten unconditionally, so old content never shows through.
    m_buf += "\033[H";

    const int term_rows = m_height / 2;

    char tmp[32];
    int n;

    for (int row = 0; row < term_rows; row++)
    {
        // Explicit cursor positioning: no reliance on newlines or auto-wrap.
        // \033[row;colH uses 1-based indices.
        tmp[0] = '\033';
        tmp[1] = '[';
        n = 2;
        n += write_int(tmp + n, row + 1);
        tmp[n++] = ';';
        tmp[n++] = '1';
        tmp[n++] = 'H';
        m_buf.append(tmp, (size_t)n);

        for (int col = 0; col < m_width; col++)
        {
            const Color &top = m_color[(size_t)((row * 2) * m_width + col)];
            // Guard against odd pixel height: reuse top pixel for bottom row.
            const int bot_row = (row * 2 + 1 < m_height) ? row * 2 + 1 : row * 2;
            const Color &bot = m_color[(size_t)(bot_row * m_width + col)];

            // Foreground (top pixel): ESC[38;2;R;G;Bm
            tmp[0] = '\033';
            tmp[1] = '[';
            tmp[2] = '3';
            tmp[3] = '8';
            tmp[4] = ';';
            tmp[5] = '2';
            tmp[6] = ';';
            n = 7;
            n += write_int(tmp + n, top.r);
            tmp[n++] = ';';
            n += write_int(tmp + n, top.g);
            tmp[n++] = ';';
            n += write_int(tmp + n, top.b);
            tmp[n++] = 'm';
            m_buf.append(tmp, (size_t)n);

            // Background (bottom pixel): ESC[48;2;R;G;Bm
            tmp[0] = '\033';
            tmp[1] = '[';
            tmp[2] = '4';
            tmp[3] = '8';
            tmp[4] = ';';
            tmp[5] = '2';
            tmp[6] = ';';
            n = 7;
            n += write_int(tmp + n, bot.r);
            tmp[n++] = ';';
            n += write_int(tmp + n, bot.g);
            tmp[n++] = ';';
            n += write_int(tmp + n, bot.b);
            tmp[n++] = 'm';
            m_buf.append(tmp, (size_t)n);

            m_buf.append(UPPER_HALF, 3);
        }

        m_buf += "\033[0m";
    }

    // HUD: one status line immediately below the pixel rows.
    if (!m_hud.empty())
    {
        // Position cursor at the row after the last pixel row (1-based).
        tmp[0] = '\033';
        tmp[1] = '[';
        n = 2;
        n += write_int(tmp + n, term_rows + 1);
        tmp[n++] = ';';
        tmp[n++] = '1';
        tmp[n++] = 'H';
        m_buf.append(tmp, (size_t)n);

        // Dark background, muted text.
        // Disable auto-wrap so a long HUD string clips at the terminal edge
        // instead of wrapping onto the next line and corrupting the display.
        m_buf += "\033[?7l";
        m_buf += "\033[48;2;18;18;18m\033[38;2;160;160;160m";
        m_buf += m_hud;
        m_buf += "\033[K"; // erase to end of line (clears leftover from wider text)
        m_buf += "\033[0m";
        m_buf += "\033[?7h"; // re-enable auto-wrap
    }

    std::fwrite(m_buf.data(), 1, m_buf.size(), stdout);
    std::fflush(stdout);
}
