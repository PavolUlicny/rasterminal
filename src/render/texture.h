#pragma once

#include "src/math/linalg.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Use SSE2 for bilinear blending where available; keep the same arithmetic order elsewhere.
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define RASTERMINAL_TEXTURE_SSE2
#include <emmintrin.h>
#endif

// GCC vectorizes blend_batch only after inlining. Forcing it improved Sponza by 1.30x.
// Do not use __forceinline on MSVC because an unfulfilled request fails the /WX build.
#if defined(__GNUC__) || defined(__clang__)
#define RASTERMINAL_FORCE_INLINE inline __attribute__((always_inline))
#else
#define RASTERMINAL_FORCE_INLINE inline
#endif

// Software prefetch hint; a no-op on unsupported compilers.
inline void prefetch_line([[maybe_unused]] const void *p) noexcept
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p);
#elif defined(RASTERMINAL_TEXTURE_SSE2)
    _mm_prefetch(static_cast<const char *>(p), _MM_HINT_T0);
#endif
}

// Supported UV wrapping. Loaders map their format-specific modes to this set.
enum class WrapMode : uint8_t
{
    Repeat,
    Clamp,
    Mirror
};

// Row-major RGBA texture decoded by stb_image or a format-specific decoder.
struct Texture
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // RGBA, row-major, top-left first
    WrapMode wrap_s = WrapMode::Repeat;
    WrapMode wrap_t = WrapMode::Repeat;

    // Failure leaves the object unchanged.
    [[nodiscard]] bool load(const std::string &path);

    // Load an stb_image format from memory. Failure leaves the object unchanged.
    [[nodiscard]] bool load_from_memory(const uint8_t *data, size_t size);

    // Load KTX2 through basisu. Failure leaves the object unchanged.
    [[nodiscard]] bool load_ktx2_from_memory(const uint8_t *data, size_t size);

    // Load WebP through libwebp. Failure leaves the object unchanged.
    [[nodiscard]] bool load_webp_from_memory(const uint8_t *data, size_t size);

    [[nodiscard]] bool valid() const { return width > 0 && height > 0; }

    // Fold a single normalised coordinate into [0, 1] per wrap mode.
    [[nodiscard]] static float wrap_coord(float c, WrapMode m) noexcept
    {
        switch (m)
        {
        case WrapMode::Clamp:
            return clamp(c, 0.0f, 1.0f);
        case WrapMode::Mirror:
        {
            const float t = c - (2.0f * std::floor(c * 0.5f)); // [0, 2): triangle-wave period
            return 1.0f - std::fabs(t - 1.0f);
        }
        case WrapMode::Repeat:
        default:
            return c - std::floor(c);
        }
    }

    // Keep the common Repeat/Repeat path branch-free per coordinate. Bilinear neighbors
    // remain clamped, preserving the existing seam behavior.
    void wrap_uv(float &u, float &v) const noexcept
    {
        if (wrap_s == WrapMode::Repeat && wrap_t == WrapMode::Repeat)
        {
            u = u - std::floor(u);
            v = v - std::floor(v);
        }
        else
        {
            u = wrap_coord(u, wrap_s);
            v = wrap_coord(v, wrap_t);
        }
    }

    // Blend the four texels of a bilinear footprint (p00 = top-left, p10 = right, p01 = below,
    // p11 = diagonal; tx/ty the fractional weights) into RGBA in [0, 1].
    static vec4 blend_texels(
        const uint8_t *p00, const uint8_t *p10, const uint8_t *p01, const uint8_t *p11, float tx, float ty
    ) noexcept
    {
        constexpr float inv255 = 1.0f / 255.0f;
#ifdef RASTERMINAL_TEXTURE_SSE2
        // NOLINTBEGIN(portability-simd-intrinsics): deliberately x86-only, behind the SSE2 guard
        // above, with the scalar fallback below for every other target
        const __m128i zero = _mm_setzero_si128();
        const auto texel = [&zero](const uint8_t *p) noexcept -> __m128
        {
            uint32_t v; // NOLINT(cppcoreguidelines-init-variables): memcpy fills it
            std::memcpy(&v, p, sizeof v);
            const __m128i b = _mm_cvtsi32_si128(static_cast<int>(v));
            return _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(b, zero), zero));
        };
        const __m128 vtx = _mm_set1_ps(tx);
        const __m128 vty = _mm_set1_ps(ty);
        const __m128 one = _mm_set1_ps(1.0f);
        const __m128 top = _mm_add_ps(_mm_mul_ps(texel(p00), _mm_sub_ps(one, vtx)), _mm_mul_ps(texel(p10), vtx));
        const __m128 bottom = _mm_add_ps(_mm_mul_ps(texel(p01), _mm_sub_ps(one, vtx)), _mm_mul_ps(texel(p11), vtx));
        const __m128 res =
            _mm_mul_ps(_mm_add_ps(_mm_mul_ps(top, _mm_sub_ps(one, vty)), _mm_mul_ps(bottom, vty)), _mm_set1_ps(inv255));
        alignas(16) float out[4];
        _mm_store_ps(out, res);
        return { out[0], out[1], out[2], out[3] };
        // NOLINTEND(portability-simd-intrinsics)
#else
        // Match the SSE2 operation order. sample_a may differ by one ulp, but 100 million
        // comparisons produced no alpha-cutout decision changes.
        const auto texel = [](const uint8_t *p) noexcept -> vec4 {
            return { static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]),
                     static_cast<float>(p[3]) };
        };
        const vec4 top = texel(p00) * (1.0f - tx) + texel(p10) * tx;
        const vec4 bottom = texel(p01) * (1.0f - tx) + texel(p11) * tx;
        return (top * (1.0f - ty) + bottom * ty) * inv255;
#endif
    }

    // Byte offsets and weights for one bilinear sample.
    struct Footprint
    {
        size_t o00, o10, o01, o11;
        float tx, ty;
    };

    // Footprint of a bilinear sample at normalised (u, v). Out-of-range UVs fold per
    // wrap_s/wrap_t (default Repeat). V is flipped so that OBJ convention (v = 0 at bottom)
    // maps correctly to image storage order (row 0 at top).
    [[nodiscard]] Footprint locate(float u, float v) const noexcept
    {
        wrap_uv(u, v);
        v = 1.0f - v;

        const float fx = u * static_cast<float>(width - 1);
        const float fy = v * static_cast<float>(height - 1);

        const auto x0 = static_cast<int>(fx);
        const auto y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, width - 1);
        const int y1 = std::min(y0 + 1, height - 1);

        const auto w = static_cast<size_t>(width);
        const size_t row0 = static_cast<size_t>(y0) * w * 4;
        const size_t row1 = static_cast<size_t>(y1) * w * 4;
        return { row0 + (static_cast<size_t>(x0) * 4), row0 + (static_cast<size_t>(x1) * 4),
                 row1 + (static_cast<size_t>(x0) * 4), row1 + (static_cast<size_t>(x1) * 4),
                 fx - static_cast<float>(x0),          fy - static_cast<float>(y0) };
    }

    [[nodiscard]] vec4 blend(const Footprint &f) const noexcept
    {
        const uint8_t *base = pixels.data();
        return blend_texels(base + f.o00, base + f.o10, base + f.o01, base + f.o11, f.tx, f.ty);
    }

    // Bilinear sample at normalised (u, v), RGBA in [0, 1] (locate + blend).
    [[nodiscard]] vec4 sample_rgba(float u, float v) const { return blend(locate(u, v)); }

    // SoA footprints let the compiler vectorize location and overlap texture misses.
    // GCC may contract the batch v-flip into an FMA, so batch and scalar sampling can differ
    // by about 1e-5 at row boundaries. Tiled-path tests therefore require a tolerance.
    static constexpr int BATCH_MAX = 64;
    struct FootprintBatch
    {
        size_t o00[BATCH_MAX], o10[BATCH_MAX], o01[BATCH_MAX], o11[BATCH_MAX];
        float tx[BATCH_MAX], ty[BATCH_MAX];
    };

    void locate_batch(const float *u, const float *v, int n, FootprintBatch &f) const noexcept
    {
        const auto sx = static_cast<float>(width - 1);
        const auto sy = static_cast<float>(height - 1);
        const int xmax = width - 1;
        const int ymax = height - 1;
        const size_t w4 = static_cast<size_t>(width) * 4;
        // Avoid a per-sample wrap-mode switch for the common Repeat case.
        const bool plain = wrap_s == WrapMode::Repeat && wrap_t == WrapMode::Repeat;
        for (int i = 0; i < n; i++)
        {
            float uu = u[i];
            float vv = v[i];
            if (plain)
            {
                uu = uu - std::floor(uu);
                vv = vv - std::floor(vv);
            }
            else
            {
                uu = wrap_coord(uu, wrap_s);
                vv = wrap_coord(vv, wrap_t);
            }
            const float fx = uu * sx;
            const float fy = (1.0f - vv) * sy;
            const auto x0 = static_cast<int>(fx);
            const auto y0 = static_cast<int>(fy);
            const int x1 = std::min(x0 + 1, xmax);
            const int y1 = std::min(y0 + 1, ymax);
            const size_t row0 = static_cast<size_t>(y0) * w4;
            const size_t row1 = static_cast<size_t>(y1) * w4;
            f.o00[i] = row0 + (static_cast<size_t>(x0) * 4);
            f.o10[i] = row0 + (static_cast<size_t>(x1) * 4);
            f.o01[i] = row1 + (static_cast<size_t>(x0) * 4);
            f.o11[i] = row1 + (static_cast<size_t>(x1) * 4);
            f.tx[i] = fx - static_cast<float>(x0);
            f.ty[i] = fy - static_cast<float>(y0);
        }
    }

    // Prefetch the two rows; right texels normally share their cache lines.
    void prefetch_batch(const FootprintBatch &f, int n) const noexcept
    {
        const uint8_t *base = pixels.data();
        for (int i = 0; i < n; i++)
        {
            prefetch_line(base + f.o00[i]);
            prefetch_line(base + f.o01[i]);
        }
    }

    // Load every texel before blending so the CPU can overlap cache misses. The 1 KB staging
    // block outperformed smaller chunks on both large and small scenes.
    RASTERMINAL_FORCE_INLINE void
    blend_batch(const FootprintBatch &f, int n, float *out_r, float *out_g, float *out_b, float *out_a) const noexcept
    {
        const uint8_t *base = pixels.data();
        // Bytes preserve RGBA order across endianness while retaining four-byte loads.
        uint8_t t00[BATCH_MAX][4];
        uint8_t t10[BATCH_MAX][4];
        uint8_t t01[BATCH_MAX][4];
        uint8_t t11[BATCH_MAX][4];
        for (int i = 0; i < n; i++)
        {
            std::memcpy(t00[i], base + f.o00[i], 4);
            std::memcpy(t10[i], base + f.o10[i], 4);
            std::memcpy(t01[i], base + f.o01[i], 4);
            std::memcpy(t11[i], base + f.o11[i], 4);
        }
        // Blend per channel so the compiler vectorizes across samples, not RGBA lanes.
        constexpr float inv255 = 1.0f / 255.0f;
        for (int i = 0; i < n; i++)
        {
            const float tx = f.tx[i];
            const float ty = f.ty[i];
            float out[4];
            for (int ch = 0; ch < 4; ch++)
            {
                const float top =
                    (static_cast<float>(t00[i][ch]) * (1.0f - tx)) + (static_cast<float>(t10[i][ch]) * tx);
                const float bot =
                    (static_cast<float>(t01[i][ch]) * (1.0f - tx)) + (static_cast<float>(t11[i][ch]) * tx);
                out[ch] = ((top * (1.0f - ty)) + (bot * ty)) * inv255;
            }
            out_r[i] = out[0];
            out_g[i] = out[1];
            out_b[i] = out[2];
            out_a[i] = out[3];
        }
    }

    // RGB of sample_rgba (same texels, same blend), for the samplers that never read alpha.
    [[nodiscard]] vec3 sample_rgb(float u, float v) const
    {
        const vec4 c = sample_rgba(u, v);
        return { c.x, c.y, c.z };
    }

    // Alpha channel only, bilinear like sample_rgba: the tiled visibility pass tests the alpha
    // cutout without paying for the three colour channels it does not need there.
    [[nodiscard]] float sample_a(float u, float v) const
    {
        wrap_uv(u, v);
        v = 1.0f - v;

        const float fx = u * static_cast<float>(width - 1);
        const float fy = v * static_cast<float>(height - 1);

        const auto x0 = static_cast<int>(fx);
        const auto y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, width - 1);
        const int y1 = std::min(y0 + 1, height - 1);

        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);

        constexpr float inv255 = 1.0f / 255.0f;
        const uint8_t *base = pixels.data();
        const auto w = static_cast<size_t>(width);
        const auto a00 = static_cast<float>(base[(((static_cast<size_t>(y0) * w) + static_cast<size_t>(x0)) * 4) + 3]);
        const auto a10 = static_cast<float>(base[(((static_cast<size_t>(y0) * w) + static_cast<size_t>(x1)) * 4) + 3]);
        const auto a01 = static_cast<float>(base[(((static_cast<size_t>(y1) * w) + static_cast<size_t>(x0)) * 4) + 3]);
        const auto a11 = static_cast<float>(base[(((static_cast<size_t>(y1) * w) + static_cast<size_t>(x1)) * 4) + 3]);

        const float top = (a00 * (1.0f - tx)) + (a10 * tx);
        const float bottom = (a01 * (1.0f - tx)) + (a11 * tx);
        return ((top * (1.0f - ty)) + (bottom * ty)) * inv255;
    }
};

// True when every texel is achromatic within the decoder's tolerance. Empty is false.
[[nodiscard]] bool is_grayscale(const Texture &t);

// Convert an MTL height map to a tangent-space normal map. `imfchan` selects the height
// channel, `bm` scales the gradient, and edge taps honor the texture's wrap modes.
[[nodiscard]] Texture height_to_normal_map(const Texture &src, char imfchan, float bm);
