#include "src/terminal/sixel.h"

#include "src/terminal/color.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

// Enable word-at-a-time run scans only on little-endian targets with a 64-bit ctz.
// Other targets keep the byte loop. clang-cl uses its builtin path.
#if defined(_MSC_VER) && !defined(__clang__)
#if defined(_M_X64) || defined(_M_ARM64)
#define SIXEL_LE_WORD_SCAN
#endif
#elif defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define SIXEL_LE_WORD_SCAN
#endif

#if defined(SIXEL_LE_WORD_SCAN) && defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h> // _BitScanForward64 for the run scan
#endif

namespace
{

    constexpr int REGISTERS = 240;

#ifdef SIXEL_LE_WORD_SCAN
    // Byte offset of the lowest-addressed differing byte in a nonzero XOR word.
    inline int first_diff_byte(uint64_t d) noexcept
    {
#if defined(_MSC_VER) && !defined(__clang__)
        unsigned long i = 0;
        _BitScanForward64(&i, d);
        return static_cast<int>(i >> 3u);
#else
        return static_cast<int>(static_cast<unsigned int>(__builtin_ctzll(d)) >> 3u);
#endif
    }
#endif

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

    // Sixel register channels are 0..100; round-to-nearest keeps the palette's
    // exact endpoints (0 and 255) exact.
    constexpr unsigned int channel_pct(uint8_t v)
    {
        return ((static_cast<unsigned int>(v) * 100u) + 127u) / 255u;
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

    // Redefine all registers each frame because the palette is shared terminal state.
    const std::string &palette_block()
    {
        static const std::string block = []
        {
            std::string s;
            for (int j = 0; j < REGISTERS; j++)
            {
                const Color c = quant256_palette_entry(j);
                s += '#';
                append_uint(s, static_cast<unsigned int>(j));
                s += ";2;";
                append_uint(s, channel_pct(c.r));
                s += ';';
                append_uint(s, channel_pct(c.g));
                s += ';';
                append_uint(s, channel_pct(c.b));
            }
            return s;
        }();
        return block;
    }

    void append_header(std::string &out, int width, int height)
    {
        if (width <= 0 || height <= 0)
        {
            return;
        }

        // Reserve the fixed header and palette. Guard because pre-C++20 reserve may shrink.
        const size_t need = out.size() + palette_block().size() + 64u;
        if (need > out.capacity())
        {
            out.reserve(need);
        }

        // P2=1 leaves zero bits untouched. Every pixel is painted by one palette pass.
        out += "\033P0;1;0q\"1;1;";
        append_uint(out, static_cast<unsigned int>(width));
        out += ';';
        append_uint(out, static_cast<unsigned int>(height));
        out += palette_block();
    }

    void append_footer(std::string &out)
    {
        out += "\033\\";
    }

    void append_bands(
        std::string &out, const unsigned char *indices, int width, int height, int band0, int band1, Scratch &scratch
    )
    {
        if (width <= 0 || height <= 0)
        {
            return;
        }

        // Lazily clear masks only for palette registers used in this band.
        const auto w = static_cast<size_t>(width);
        const size_t mask_bytes = static_cast<size_t>(REGISTERS) * w;
        if (mask_bytes > scratch.cap)
        {
            // NOLINTNEXTLINE(modernize-make-unique,cppcoreguidelines-owning-memory): value-init defeats the point
            scratch.mask.reset(new unsigned char[mask_bytes]);
            scratch.cap = mask_bytes;
        }
        unsigned char *mask = scratch.mask.get();
        // Reads are stamp-gated; value initialization remains for clang-tidy.
        std::array<int, REGISTERS> stamp{};
        stamp.fill(-1);
        std::array<int, REGISTERS> min_x{};
        std::array<int, REGISTERS> max_x{};
        std::array<int, REGISTERS> colors{};

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
                const unsigned char *row = indices + ((static_cast<size_t>(y0) + static_cast<size_t>(dy)) * w);
                const auto bit = static_cast<unsigned char>(1u << static_cast<unsigned int>(dy));
                for (int x = 0; x < width; x++)
                {
                    const int reg = row[x] - 16;
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
                const int hi = max_x[reg];
                int x = min_x[reg];
                append_run(out, x, '?'); // Emit the leading gap; omit the trailing gap.
                while (x <= hi)
                {
                    const unsigned char v = m[x];
                    int run = 1;
#ifdef SIXEL_LE_WORD_SCAN
                    // Compare eight columns at once; the byte loop handles the tail.
                    const uint64_t bcast = 0x0101010101010101ull * v;
                    while (x + run + 8 <= hi + 1)
                    {
                        uint64_t chunk = 0;
                        std::memcpy(&chunk, m + x + run, 8);
                        const uint64_t d = chunk ^ bcast;
                        if (d != 0)
                        {
                            run += first_diff_byte(d);
                            break;
                        }
                        run += 8;
                    }
#endif
                    while (x + run <= hi && m[x + run] == v)
                    {
                        run++;
                    }
                    append_run(out, run, static_cast<char>(0x3F + v));
                    x += run;
                }
            }
            if (band + 1 < bands)
            {
                out += '-'; // next band; none after the last (avoids an extra line advance)
            }
        }
    }

    void append_frame(std::string &out, const unsigned char *indices, int width, int height, Scratch &scratch)
    {
        if (width <= 0 || height <= 0)
        {
            return;
        }
        append_header(out, width, height);
        append_bands(out, indices, width, height, 0, band_count(height), scratch);
        append_footer(out);
    }

} // namespace sixel
