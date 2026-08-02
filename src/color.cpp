#include "color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace
{

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

    // Builds the 64^3 quantization table documented at quant256_idx (color.h): per cell the
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

void append_fg_sgr(std::string &out, Color c, ColorMode mode)
{
    char buf[24];
    int n = 0;
    if (mode == ColorMode::TrueColor)
    {
        n = std::snprintf(
            buf, sizeof(buf), "\033[38;2;%u;%u;%um", static_cast<unsigned>(c.r), static_cast<unsigned>(c.g),
            static_cast<unsigned>(c.b)
        );
    }
    else
    {
        n = std::snprintf(buf, sizeof(buf), "\033[38;5;%um", static_cast<unsigned>(quantize_256(c)));
    }
    out.append(buf, static_cast<size_t>(n));
}
