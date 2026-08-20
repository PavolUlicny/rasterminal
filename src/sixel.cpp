#include "sixel.h"

#include "color.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

// The word-at-a-time run scan below assumes little-endian (ctz of a XOR picks
// the LOWEST-ADDRESSED differing byte only there); every supported target is
// LE, and an exotic BE port just keeps the byte-loop tail. The MSVC family
// (which never defines __BYTE_ORDER__) is decided first and on 64-bit only:
// _BitScanForward64 is not declared under _M_IX86 (the Win32 CI job keeps the
// byte loop). clang-cl defines _MSC_VER too but is excluded here and in the
// branch below (its 32-bit target has no _BitScanForward64 either; it always
// has __builtin_ctzll), so it gates through the __BYTE_ORDER__ arm, whose
// defined() guard keeps an undefined ORDER macro from reading as a 0 == 0
// match. A flag macro, not a constexpr bool: it guards preprocessor blocks.
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

    // Digits straight into the output; RLE counts make this hot on busy bands,
    // where a std::to_string temporary per number is measurable churn.
    // Deliberately local despite kitty.cpp's append_uint and framebuffer.cpp's
    // write_int: three call shapes (append hot, append cold via to_string,
    // caller-buffer with length), and a shared header for ten lines of digit
    // emission is not warranted.
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

    // n copies of the sixel data char ch. The counted form `!<n><ch>` costs at
    // least 3 bytes, so runs up to 3 are cheaper spelled out.
    void append_run(std::string &out, int n, char ch)
    {
        if (n == 1)
        {
            // The overwhelming case on detailed frames; += inlines to a
            // push_back where append(1, ch) is an out-of-line replace call
            // (measured -26..30% whole-encode on dithered 1080p planes).
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

    // All 240 register definitions, #<reg>;2;<r>;<g>;<b> in RGB percent. Redefined
    // in every frame on purpose: registers are shared terminal state anything else
    // could clobber, and ~4 KB is noise next to the pixel data.
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

        // Fixed-size prefix: DCS header + raster attributes (~32 B) + the
        // palette block's exact size. Guarded (P0966: pre-C++20 reserve may
        // shrink); the band data grows amortized on top.
        const size_t need = out.size() + palette_block().size() + 64u;
        if (need > out.capacity())
        {
            out.reserve(need);
        }

        // P1=0 (aspect comes from the raster attributes), P2=1 so zero-bits
        // paint nothing. The palette partitions RGB space, so every pixel is
        // painted by exactly one colour pass per frame and holes cannot occur;
        // P2=0/2 would make the terminal pre-fill the whole raster region with
        // a background register first, a wasted full-frame fill.
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

        // Per-band staging: a column bitmask per register, cleared lazily on the
        // register's first touch in the band (stamp) so a band pays for the
        // registers it uses, not all 240. Grow-only and uninitialized, like the
        // framebuffer's staging arrays; see Scratch.
        const auto w = static_cast<size_t>(width);
        const size_t mask_bytes = static_cast<size_t>(REGISTERS) * w;
        if (mask_bytes > scratch.cap)
        {
            // NOLINTNEXTLINE(modernize-make-unique,cppcoreguidelines-owning-memory): value-init defeats the point
            scratch.mask.reset(new unsigned char[mask_bytes]);
            scratch.cap = mask_bytes;
        }
        unsigned char *mask = scratch.mask.get();
        // Every min_x/max_x/colors read is stamp-gated, so their {} is dead
        // zeroing (~4 KB/frame, ~0.1 us); it stays because clang-tidy's
        // member-init check flags the uninitialized form on all four.
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
                append_run(out, x, '?'); // leading gap; the trailing one is simply omitted
                while (x <= hi)
                {
                    const unsigned char v = m[x];
                    int run = 1;
#ifdef SIXEL_LE_WORD_SCAN
                    // Eight columns per compare (byte-identical output; measured
                    // -20..40% whole-encode on the portable build, no plane
                    // regresses); the byte loop below finishes the partial word.
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
