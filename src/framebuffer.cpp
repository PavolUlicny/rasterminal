#include "framebuffer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>
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

    // sRGB EOTF (IEC 61966-2-1): non-linear [0,1] -> linear-light [0,1].
    float srgb_to_linear(float v)
    {
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    }

    struct Lab
    {
        float L, a, b;
    };

    // Linear sRGB -> CIELAB (D65 white), the deltaE76 space the quantizer measures in. CIELAB
    // rather than OKLab deliberately: OKLab's lightness term dominates its chroma terms so hard
    // in the darks that it still sends dark greens/browns to the grey ramp (measured on a real
    // model: 90.9% of pixels grey vs 93.4% for squared RGB, where CIELAB gives 69.8% and keeps
    // the model's hue), defeating the purpose of a perceptual metric here.
    Lab cielab_from_linear(float r, float g, float b)
    {
        // sRGB -> XYZ (D65); X and Z rows pre-divided by the white point (Y's is 1).
        const float x = ((0.4124564f / 0.95047f) * r) + ((0.3575761f / 0.95047f) * g) + ((0.1804375f / 0.95047f) * b);
        const float y = (0.2126729f * r) + (0.7151522f * g) + (0.0721750f * b);
        const float z = ((0.0193339f / 1.08883f) * r) + ((0.1191920f / 1.08883f) * g) + ((0.9503041f / 1.08883f) * b);
        const auto f = [](float t)
        { return t > 216.0f / 24389.0f ? std::cbrt(t) : (((24389.0f / 27.0f) * t) + 16.0f) / 116.0f; };
        const float fx = f(x);
        const float fy = f(y);
        const float fz = f(z);
        return { (116.0f * fy) - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz) };
    }

    // The 240 addressable xterm-256 palette RGB values by entry number 0..239 (palette index is
    // 16 + j): the 6x6x6 colour cube on levels {0,95,135,175,215,255}, then the 24-step grey ramp.
    Color quant256_palette_entry(int j)
    {
        if (j < 216)
        {
            const auto val = [](int l) { return static_cast<uint8_t>(l == 0 ? 0 : 55 + (40 * l)); };
            return { val(j / 36), val((j / 6) % 6), val(j % 6) };
        }
        const auto v = static_cast<uint8_t>(8 + (10 * (j - 216)));
        return { v, v, v };
    }

    // Builds the 64^3 quantization table documented at quant256_idx (framebuffer.h): per cell the
    // deltaE76-nearest of the 240 addressable palette entries for the cell centre, then an exact
    // overwrite so any cell containing a palette colour maps to it.
    std::array<uint8_t, QUANT256_LUT_SIZE> build_quant256_lut()
    {
        constexpr int n_pal = 240;

        // Palette CIELAB as structure-of-arrays so the argmin scan below vectorizes. (An L*-sorted
        // two-pointer prune was tried and measured slower: it visits fewer entries but its branchy
        // walk defeats the vectorization this straight 240-entry loop gets.)
        std::array<float, n_pal> pl{};
        std::array<float, n_pal> pa{};
        std::array<float, n_pal> pb{};
        for (int j = 0; j < n_pal; ++j)
        {
            const auto i = static_cast<size_t>(j);
            const Color p = quant256_palette_entry(j);
            const Lab o = cielab_from_linear(
                srgb_to_linear(static_cast<float>(p.r) / 255.0f), srgb_to_linear(static_cast<float>(p.g) / 255.0f),
                srgb_to_linear(static_cast<float>(p.b) / 255.0f)
            );
            pl[i] = o.L;
            pa[i] = o.a;
            pb[i] = o.b;
        }

        // The 64 per-channel cell-centre values, linearized once (the centre of [4i, 4i+3] is 4i+1.5).
        std::array<float, 64> centre_lin{};
        for (int i = 0; i < 64; ++i)
        {
            centre_lin[static_cast<size_t>(i)] = srgb_to_linear(((4.0f * static_cast<float>(i)) + 1.5f) / 255.0f);
        }

        std::array<uint8_t, QUANT256_LUT_SIZE> lut{};

        // Scan a range of r-slices; slices write disjoint lut ranges, so they parallelize with no
        // shared state. Each cell is an independent argmin over the 240 entries; strict < keeps
        // the first (lowest-index) entry on a tie, so the cube beats the ramp.
        const auto scan = [&](int r_begin, int r_end)
        {
            for (int r = r_begin; r < r_end; ++r)
            {
                size_t idx = static_cast<size_t>(r) << 12u;
                for (int g = 0; g < 64; ++g)
                {
                    for (int b = 0; b < 64; ++b)
                    {
                        const Lab c = cielab_from_linear(
                            centre_lin[static_cast<size_t>(r)], centre_lin[static_cast<size_t>(g)],
                            centre_lin[static_cast<size_t>(b)]
                        );
                        int best = 0;
                        float best_d = std::numeric_limits<float>::max();
                        for (int j = 0; j < n_pal; ++j)
                        {
                            const auto i = static_cast<size_t>(j);
                            const float dl = pl[i] - c.L;
                            const float da = pa[i] - c.a;
                            const float db = pb[i] - c.b;
                            const float d = (dl * dl) + (da * da) + (db * db);
                            if (d < best_d)
                            {
                                best_d = d;
                                best = j;
                            }
                        }
                        lut[idx++] = static_cast<uint8_t>(16 + best);
                    }
                }
            }
        };

        // The serial scan measures ~50 ms; slicing it across cores gets ~7 ms. Serial is the
        // fallback both for hardware_concurrency() == 0 and for a thread that fails to spawn;
        // the spawned threads are joined before the fallback runs, so its full re-scan (of
        // ranges they may already have covered) writes identical values race-free.
        const unsigned n_threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
        bool serial = n_threads <= 1;
        std::vector<std::thread> workers;
        if (!serial)
        {
            try
            {
                // reserve stays inside the try: its bad_alloc must take the same serial fallback
                // as a failed spawn (an escape would hit quant256_lut()'s noexcept and terminate).
                workers.reserve(n_threads);
                for (unsigned t = 0; t < n_threads; ++t)
                {
                    const int r_begin = static_cast<int>(64u * t / n_threads);
                    const int r_end = static_cast<int>(64u * (t + 1) / n_threads);
                    workers.emplace_back(scan, r_begin, r_end);
                }
            }
            catch (...)
            {
                serial = true;
            }
        }
        for (auto &w : workers)
        {
            w.join();
        }
        if (serial)
        {
            scan(0, 64);
        }

        // Exact-palette overwrite: a palette colour must round-trip to itself (distance zero) even
        // when its cell centre, up to ~2.6 RGB units away, is nearest a different entry: a pure
        // black background must never wash to the adjacent grey. With the current CIELAB constants
        // every such cell already picks its contained colour, so this pins the guarantee rather
        // than changing anything today (under OKLab it was load-bearing: black's cell centre sat
        // nearer grey 8 than black). At 64^3 no two palette entries share a cell (the closest
        // pairs, cube grey levels vs their ramp neighbours 3 units away such as 95/98, land in
        // adjacent cells), so overwrites cannot collide.
        for (int j = 0; j < n_pal; ++j)
        {
            lut[quant256_idx(quant256_palette_entry(j))] = static_cast<uint8_t>(16 + j);
        }
        return lut;
    }

} // namespace

const std::array<uint8_t, QUANT256_LUT_SIZE> &quant256_lut() noexcept
{
    static const std::array<uint8_t, QUANT256_LUT_SIZE> lut = build_quant256_lut();
    return lut;
}

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

    if (!m_hud.empty())
    {
        append_cursor_pos(term_rows + 1, 1);

        // Disable auto-wrap so a long HUD string clips at the terminal edge
        // instead of wrapping onto the next line and corrupting the display.
        m_buf += "\033[?7l";
        if constexpr (TC)
        {
            // bg {18,18,18} / fg {160,160,160}; keep in sync with the Palette256 branch + the test pin
            // (quantize_256_grey_ramp_exact ties the 233/247 below to the quantizer of these same RGBs).
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
