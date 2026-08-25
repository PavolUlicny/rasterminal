#include "src/terminal/color.h"

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

    // CIELAB preserves dark model hues better than OKLab here: 69.8% of measured pixels
    // mapped to gray, compared with 90.9% under OKLab.
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

    // Map each 64^3 cell center to the nearest addressable xterm color by deltaE76.
    std::array<uint8_t, QUANT256_LUT_SIZE> build_quant256_lut()
    {
        constexpr int n_pal = 240;

        // Structure of arrays lets the palette scan vectorize.
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

        // Linear-light values at each four-level cell center.
        std::array<float, 64> centre_lin{};
        for (int i = 0; i < 64; ++i)
        {
            centre_lin[static_cast<size_t>(i)] = srgb_to_linear(((4.0f * static_cast<float>(i)) + 1.5f) / 255.0f);
        }

        std::array<uint8_t, QUANT256_LUT_SIZE> lut{};

        // Red slices write disjoint ranges. Strict comparison favors the lower index on ties.
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

        // Parallel construction measures about 7 ms versus 50 ms serial. On spawn failure,
        // join workers before the deterministic serial rescan.
        const unsigned n_threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
        bool serial = n_threads <= 1;
        std::vector<std::thread> workers;
        if (!serial)
        {
            try
            {
                // Allocation failure must use the serial path because the caller is noexcept.
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

        // Palette colors must round-trip exactly. No two entries share a 64^3 cell.
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
