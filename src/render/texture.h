#pragma once

#include "src/math/linalg.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Bilinear texel blend in SSE2 where the target has it (every x86-64, and x86-32 built with
// -msse2 / /arch:SSE2); the scalar fallback covers everything else. Same arithmetic order in
// both, only the byte-to-float conversion moves before or after the blend.
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define RASTERMINAL_TEXTURE_SSE2
#include <emmintrin.h>
#endif

// Forces blend_batch into its callers. Not a style preference: GCC declines to inline it at some
// of shade_batch's six call sites, and the out-of-line copy LTO then keeps is compiled SCALAR (46
// scalar FP ops and not one packed instruction) even though the identical source vectorises to
// 64-byte vectors wherever it is inlined. That one copy measured 36% of a Sponza frame; forcing
// the inline is worth 1.30x there and 1.07x on a glass scene. Deliberately GCC/Clang only: MSVC's
// __forceinline warns (C4714) when it cannot honour the request, which /WX would make fatal, and
// the codegen decision this works around is GCC's.
#if defined(__GNUC__) || defined(__clang__)
#define RASTERMINAL_FORCE_INLINE inline __attribute__((always_inline))
#else
#define RASTERMINAL_FORCE_INLINE inline
#endif

// Software prefetch for the batch shader's texel fetches (a hint; a no-op where unknown).
inline void prefetch_line([[maybe_unused]] const void *p) noexcept
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p);
#elif defined(RASTERMINAL_TEXTURE_SSE2)
    _mm_prefetch(static_cast<const char *>(p), _MM_HINT_T0);
#endif
}

// How out-of-[0,1] UV coordinates fold back into the texture. Closed set: glTF defines
// exactly these three, MTL only Repeat/Clamp, and Assimp maps its common modes here.
// Default Repeat keeps every loader that does not set it (and all historical output) unchanged.
enum class WrapMode : uint8_t
{
    Repeat,
    Clamp,
    Mirror
};

// A 2-D RGBA texture loaded from an image file.
// Supports JPEG, PNG, BMP, TGA, GIF, and more via stb_image.
struct Texture
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // RGBA, row-major, top-left first
    WrapMode wrap_s = WrapMode::Repeat;
    WrapMode wrap_t = WrapMode::Repeat;

    // Load a texture from disk.  Returns false and leaves the object unchanged on error.
    [[nodiscard]] bool load(const std::string &path);

    // Load a texture from a memory buffer (e.g. embedded GLB image data).
    // Returns false and leaves the object unchanged on error.
    [[nodiscard]] bool load_from_memory(const uint8_t *data, size_t size);

    // Load a KTX2 / Basis Universal texture from a memory buffer (glTF
    // KHR_texture_basisu). stb_image cannot decode KTX2, so this is a separate entry
    // point that transcodes via the basis_universal transcoder (see ktx2_decode.h).
    // Returns false and leaves the object unchanged on error.
    [[nodiscard]] bool load_ktx2_from_memory(const uint8_t *data, size_t size);

    // Load a WebP texture from a memory buffer (glTF EXT_texture_webp). stb_image cannot
    // decode WebP, so this is a separate entry point that decodes via libwebp (see
    // webp_decode.h). Returns false and leaves the object unchanged on error.
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

    // Fold (u, v) into [0, 1] per the texture's wrap modes. The all-Repeat case (the
    // overwhelming majority) runs the historical fract() verbatim behind one
    // perfectly-predicted branch (the mode is constant per texture), so it stays
    // byte-identical at the cost of that single branch; non-repeat textures take the
    // cold general path. The bilinear tap downstream stays
    // min(x0+1, width-1): that clamped tap is exactly today's Repeat behaviour (the
    // seam-column blend is left unchanged) and is correct for Clamp and at the Mirror fold.
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
        // Scaled to [0,1] after the blend, matching the SSE2 path and sample_a: same operations
        // in the same order, so this fallback and the SSE2 path agree bit for bit. sample_a does
        // NOT: it blends one scalar channel where the SSE2 path blends four lanes, which lands an
        // ulp apart on ~30% of samples (measured). Only the alpha cutout reads both, and only a
        // sample landing within that ulp of the cutoff could mask differently between the tiled
        // and immediate paths: zero flips in 100M measured comparisons, accepted as one pixel of
        // a cutout edge at worst.
        const auto texel = [](const uint8_t *p) noexcept -> vec4 {
            return { static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]),
                     static_cast<float>(p[3]) };
        };
        const vec4 top = texel(p00) * (1.0f - tx) + texel(p10) * tx;
        const vec4 bottom = texel(p01) * (1.0f - tx) + texel(p11) * tx;
        return (top * (1.0f - ty) + bottom * ty) * inv255;
#endif
    }

    // The four texels of a bilinear footprint as byte offsets into pixels (o00 top-left, o10
    // right, o01 below, o11 diagonal) plus the fractional weights: the sample split into locate
    // and blend. FootprintBatch below is the batch shader's SoA form of the same thing.
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

    // The batch shader's form of Footprint: one SoA block for up to BATCH_MAX samples, filled
    // by locate_batch in loops the compiler vectorizes (locate's per-sample struct store did
    // not), prefetched together, then blended one at a time. Locating and prefetching a whole
    // batch before reading any texel is what makes the fetches overlap: they are the shader's
    // dominant memory stalls (three textures per pixel on a PBR model, and minified regions
    // miss cache on nearly every texel).
    //
    // locate_batch is NOT bit-exact with locate, and cannot be relied on to be. It spells the
    // v-flip as `(1 - v) * sy` where locate flips then multiplies, and GCC contracts the former
    // into an FMA and not the latter, so `ty` differs by an ulp on ~13% of samples under
    // `-march=native` (measured; zero under the portable build and under clang, and zero on both
    // for `tx`, which has no such subtraction). Almost always that is a rounding difference in
    // the blend weight, but where `ty` sits on a bin boundary the two pick texel rows one apart
    // (2 samples in 320000): the one place the tiled and immediate paths sample DIFFERENT texels
    // rather than blending the same ones differently. Harmless, because the blend is continuous
    // across that boundary, so the colours differ by ~1e-5 of a channel. It is why
    // tests/renderer/test_renderer_tiled.cpp compares the two paths with a tolerance instead of
    // exactly; do not tighten that to zero expecting it to hold on GCC.
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
        // Repeat/Repeat is the common case and a plain fract; the other modes take the per-sample
        // wrap_coord switch (rare, per texture).
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

    // Touch each footprint's two rows (its right texels are on the same line as the left ones
    // except at a line boundary, not worth a third hint).
    void prefetch_batch(const FootprintBatch &f, int n) const noexcept
    {
        const uint8_t *base = pixels.data();
        for (int i = 0; i < n; i++)
        {
            prefetch_line(base + f.o00[i]);
            prefetch_line(base + f.o01[i]);
        }
    }

    // Blend a whole batch into SoA channel arrays. Every texel is loaded before any is blended:
    // blending a sample at a time interleaves ~15 flops between each group of four loads, which
    // caps how many misses the core can have in flight, and this shader is memory-stalled (the same
    // scene at 4K textures instead of 1K runs at IPC 0.78 against 1.07 on identical geometry and
    // fragment counts). The 1 KB of staging stays L1-resident; chunking it into groups of 16 to
    // shrink it further measured worse at both ends (4K city 1.14x against 1.18x, Duck 0.94x
    // against 0.97x).
    RASTERMINAL_FORCE_INLINE void
    blend_batch(const FootprintBatch &f, int n, float *out_r, float *out_g, float *out_b, float *out_a) const noexcept
    {
        const uint8_t *base = pixels.data();
        // Bytes, not a uint32 whose channels are shifted out: RGBA order in memory is fixed, a
        // word's channel order is not, and this keeps the load width at four bytes either way.
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
        // Spelled out per channel rather than calling blend_texels on the staged bytes, even
        // though that would reuse one blend implementation: this form lets the compiler vectorise
        // ACROSS SAMPLES, where blend_texels' SSE2 body is four channels of ONE sample and stays
        // four lanes wide however much vector width the target has. Measured 5% faster on Sponza
        // and 3% on the 4K city. The arithmetic and its order are blend_texels' exactly, so the
        // two still agree bar the last bit of any reassociation the vectoriser applies.
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

// True if every texel is achromatic (R==G==B). Early-outs on the first chromatic texel,
// so an RGB normal map mislabeled into map_Bump costs ~O(1); a real grayscale height map
// pays one full scan (then gets converted, which dwarfs the scan). An empty texture → false.
[[nodiscard]] bool is_grayscale(const Texture &t);

// Convert an MTL bump/height map to the tangent-space normal map the TBN rasterizer path
// consumes. `imfchan` (-imfchan, default 'l' = luminance) selects the scalar height channel;
// `bm` (-bm, default 1.0) scales the gradient. Honours the source texture's wrap modes at
// the central-difference edges. Used for OBJ map_Bump/bump; see mesh_obj.cpp.
[[nodiscard]] Texture height_to_normal_map(const Texture &src, char imfchan, float bm);
