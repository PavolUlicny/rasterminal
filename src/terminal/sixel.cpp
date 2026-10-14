#include "src/terminal/sixel.h"

#include "src/terminal/color.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace
{

    // A painting run stops before this many columns without its own pixels and skips them.
    // Painting across saves a few bytes, but xterm's decode time follows pixel writes rather
    // than bytes; 16 decoded fastest across the measured terminals.
    constexpr int MIN_SKIP = 16;

    inline bool has_zero_byte(uint64_t v) noexcept
    {
        return ((v - 0x0101010101010101ull) & ~v & 0x8080808080808080ull) != 0;
    }

    // OR of the eight bytes of a word, which byte order cannot change.
    inline unsigned int or_bytes(uint64_t v) noexcept
    {
        v |= v >> 32u;
        v |= v >> 16u;
        v |= v >> 8u;
        return static_cast<unsigned int>(v & 0xFFu);
    }

    // Avoid std::to_string allocations in hot RLE output.
    void append_uint(std::string &out, unsigned int v)
    {
        if (v == 0)
        {
            out += '0';
            return;
        }
        char buf[10]; // UINT_MAX is 10 digits
        int n = 0;
        while (v != 0)
        {
            buf[n++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
        while (n > 0)
        {
            out += buf[--n];
        }
    }

    // Counted RLE costs at least three bytes, so spell out runs of three or fewer.
    void append_run(std::string &out, int n, char ch)
    {
        if (n == 1)
        {
            // += avoids an out-of-line append call in the common one-byte case.
            out += ch;
            return;
        }
        if (n <= 0)
        {
            return;
        }
        if (n <= 3)
        {
            out.append(static_cast<size_t>(n), ch);
            return;
        }
        out += '!';
        append_uint(out, static_cast<unsigned int>(n));
        out += ch;
    }

} // namespace

namespace sixel
{

    void append_register(std::string &out, int reg, Color c)
    {
        out += '#';
        append_uint(out, static_cast<unsigned int>(reg));
        out += ";2;";
        append_uint(out, channel_pct(c.r));
        out += ';';
        append_uint(out, channel_pct(c.g));
        out += ';';
        append_uint(out, channel_pct(c.b));
    }

    void append_header(std::string &out, int width, int height, const std::string &register_block)
    {
        if (width <= 0 || height <= 0)
        {
            return;
        }

        // Reserve the fixed header and palette. Guard because pre-C++20 reserve may shrink.
        const size_t need = out.size() + register_block.size() + 64u;
        if (need > out.capacity())
        {
            out.reserve(need);
        }

        // P2=1 leaves zero bits untouched. Every pixel is painted, last by its own register's pass.
        out += "\033P0;1;0q\"1;1;";
        append_uint(out, static_cast<unsigned int>(width));
        out += ';';
        append_uint(out, static_cast<unsigned int>(height));
        out += register_block;
    }

    void append_footer(std::string &out)
    {
        out += "\033\\";
    }

    void append_bands(
        std::string &out, const unsigned char *registers, int width, int height, int band0, int band1, Scratch &scratch
    )
    {
        if (width <= 0 || height <= 0)
        {
            return;
        }

        // Lazily clear masks only for palette registers used in this band. The row after the
        // last register's mask holds the bits earlier passes of the band painted.
        const auto w = static_cast<size_t>(width);
        const size_t mask_bytes = static_cast<size_t>(MAX_REGISTERS + 1) * w;
        if (mask_bytes > scratch.cap)
        {
            // NOLINTNEXTLINE(modernize-make-unique,cppcoreguidelines-owning-memory): value-init defeats the point
            scratch.mask.reset(new unsigned char[mask_bytes]);
            scratch.cap = mask_bytes;
        }
        unsigned char *mask = scratch.mask.get();
        unsigned char *painted = mask + (static_cast<size_t>(MAX_REGISTERS) * w);
        // Reads are stamp-gated; value initialization remains for clang-tidy.
        std::array<int, MAX_REGISTERS> stamp{};
        stamp.fill(-1);
        std::array<int, MAX_REGISTERS> min_x{};
        std::array<int, MAX_REGISTERS> max_x{};
        std::array<int, MAX_REGISTERS> colors{};

        const int bands = band_count(height);
        const int first_band = (band0 > 0) ? band0 : 0;
        const int last_band = (band1 < bands) ? band1 : bands;
        for (int band = first_band; band < last_band; band++)
        {
            int ncolors = 0;
            const int y0 = band * 6;
            const int band_rows = (height - y0 < 6) ? height - y0 : 6;
            for (int dy = 0; dy < band_rows; dy++)
            {
                const unsigned char *row = registers + ((static_cast<size_t>(y0) + static_cast<size_t>(dy)) * w);
                const auto bit = static_cast<unsigned char>(1u << static_cast<unsigned int>(dy));
                for (int x = 0; x < width; x++)
                {
                    const int reg = row[x];
                    unsigned char *m = mask + (static_cast<size_t>(reg) * w);
                    if (stamp[static_cast<size_t>(reg)] != band)
                    {
                        std::memset(m, 0, w);
                        stamp[static_cast<size_t>(reg)] = band;
                        colors[static_cast<size_t>(ncolors++)] = reg;
                        min_x[static_cast<size_t>(reg)] = x;
                        max_x[static_cast<size_t>(reg)] = x;
                    }
                    m[x] = static_cast<unsigned char>(m[x] | bit);
                    if (x < min_x[static_cast<size_t>(reg)])
                    {
                        min_x[static_cast<size_t>(reg)] = x;
                    }
                    else if (x > max_x[static_cast<size_t>(reg)])
                    {
                        max_x[static_cast<size_t>(reg)] = x;
                    }
                }
            }

            // A pass may also set bits that a later pass of the band repaints, since the last
            // write wins. Those free bits merge runs across colour edges and short gaps.
            std::memset(painted, 0, w);
            for (int ci = 0; ci < ncolors; ci++)
            {
                if (ci > 0)
                {
                    out += '$'; // return to the band's left edge for the next pass
                }
                const auto reg = static_cast<size_t>(colors[static_cast<size_t>(ci)]);
                out += '#';
                append_uint(out, static_cast<unsigned int>(reg));
                const unsigned char *m = mask + (reg * w);
                const int lo_x = min_x[reg];
                const int hi_x = max_x[reg];
                int x = lo_x;
                append_run(out, x, '?'); // Emit the leading gap; omit the trailing gap.
                while (x <= hi_x)
                {
                    // A run's value must hold each column's own bits and only bits free in
                    // all its columns. Taking the longest run at each step gives the fewest.
                    unsigned int need = m[x];
                    unsigned int allowed = ~static_cast<unsigned int>(painted[x]);
                    int run = 1;
                    // Eight columns at once: if their union fits, so does every prefix.
                    while (x + run + 8 <= hi_x + 1)
                    {
                        uint64_t own = 0;
                        uint64_t done = 0;
                        std::memcpy(&own, m + x + run, 8);
                        std::memcpy(&done, painted + x + run, 8);
                        // Columns without own pixels may end the run; the byte loop decides.
                        if (need == 0 ? own != 0 : has_zero_byte(own))
                        {
                            break;
                        }
                        const unsigned int next_need = need | or_bytes(own);
                        const unsigned int next_allowed = allowed & ~or_bytes(done);
                        if ((next_need & ~next_allowed) != 0)
                        {
                            break;
                        }
                        need = next_need;
                        allowed = next_allowed;
                        run += 8;
                    }
                    while (x + run <= hi_x)
                    {
                        if (need != 0 && m[x + run] == 0)
                        {
                            // m[hi_x] is nonzero, so the count stays inside the span.
                            int gap = 1;
                            while (gap < MIN_SKIP && m[x + run + gap] == 0)
                            {
                                gap++;
                            }
                            if (gap == MIN_SKIP)
                            {
                                break;
                            }
                            // A short gap joins the run while its painted bits allow.
                            int joined = 0;
                            while (joined < gap)
                            {
                                const unsigned int next_allowed =
                                    allowed & ~static_cast<unsigned int>(painted[x + run + joined]);
                                if ((need & ~next_allowed) != 0)
                                {
                                    break;
                                }
                                allowed = next_allowed;
                                joined++;
                            }
                            run += joined;
                            if (joined < gap)
                            {
                                break;
                            }
                            continue;
                        }
                        if (need == 0 && m[x + run] != 0 && run >= MIN_SKIP)
                        {
                            break; // a skip ends where own pixels start
                        }
                        const unsigned int next_need = need | m[x + run];
                        const unsigned int next_allowed = allowed & ~static_cast<unsigned int>(painted[x + run]);
                        if ((next_need & ~next_allowed) != 0)
                        {
                            break;
                        }
                        need = next_need;
                        allowed = next_allowed;
                        run++;
                    }
                    append_run(out, run, static_cast<char>(0x3F + need));
                    x += run;
                }
                if (ci + 1 < ncolors)
                {
                    for (int px = lo_x; px <= hi_x; px++)
                    {
                        painted[px] = static_cast<unsigned char>(painted[px] | m[px]);
                    }
                }
            }
            if (band + 1 < bands)
            {
                out += '-'; // next band; none after the last (avoids an extra line advance)
            }
        }
    }

    void append_frame(
        std::string &out,
        const unsigned char *registers,
        int width,
        int height,
        const std::string &register_block,
        Scratch &scratch
    )
    {
        if (width <= 0 || height <= 0)
        {
            return;
        }
        append_header(out, width, height, register_block);
        append_bands(out, registers, width, height, 0, band_count(height), scratch);
        append_footer(out);
    }

} // namespace sixel
