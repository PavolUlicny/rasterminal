#include "src/render/texture.h"

#include "src/loaders/ktx2_decode.h"
#include "src/loaders/webp_decode.h"
#include "src/math/linalg.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

bool Texture::load(const std::string &path)
{
    int w = 0;
    int h = 0;
    int channels = 0;
    // Normalize all sources to RGBA.
    uint8_t *data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!data)
    {
        return false;
    }

    // Worker decode has no exception boundary. Preserve the object and report OOM as failure.
    std::vector<uint8_t> buf;
    try
    {
        buf.assign(data, data + (static_cast<size_t>(w) * static_cast<size_t>(h) * 4));
    }
    catch (const std::bad_alloc &)
    {
        stbi_image_free(data);
        return false;
    }
    stbi_image_free(data);
    width = w;
    height = h;
    pixels = std::move(buf);
    return true;
}

bool Texture::load_from_memory(const uint8_t *data, size_t size)
{
    if (!data || size == 0 || size > static_cast<size_t>(INT_MAX))
    {
        return false;
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    uint8_t *img = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
    if (!img)
    {
        return false;
    }
    // Preserve the object and report worker-thread OOM as failure.
    std::vector<uint8_t> buf;
    try
    {
        buf.assign(img, img + (static_cast<size_t>(w) * static_cast<size_t>(h) * 4));
    }
    catch (const std::bad_alloc &)
    {
        stbi_image_free(img);
        return false;
    }
    stbi_image_free(img);
    width = w;
    height = h;
    pixels = std::move(buf);
    return true;
}

bool Texture::load_ktx2_from_memory(const uint8_t *data, size_t size)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    if (!decode_ktx2_rgba(data, size, rgba, w, h))
    {
        return false;
    }
    width = w;
    height = h;
    pixels = std::move(rgba);
    return true;
}

bool is_grayscale(const Texture &t)
{
    if (t.pixels.empty())
    {
        return false;
    }
    // Lossy grayscale images may differ by a few channel levels. Normal maps differ far more.
    constexpr int kChromaTol = 8;
    for (size_t i = 0; i + 4 <= t.pixels.size(); i += 4)
    {
        const int r = t.pixels[i];
        const int g = t.pixels[i + 1];
        const int b = t.pixels[i + 2];
        const int lo = std::min({ r, g, b });
        const int hi = std::max({ r, g, b });
        if (hi - lo > kChromaTol)
        {
            return false;
        }
    }
    return true;
}

namespace
{
    // Fold an edge tap according to the texture's wrap mode.
    int wrap_index(int c, int n, WrapMode m) noexcept
    {
        if (n <= 1)
        {
            return 0;
        }
        switch (m)
        {
        case WrapMode::Clamp:
            return c < 0 ? 0 : (c >= n ? n - 1 : c);
        case WrapMode::Mirror:
        {
            const int period = 2 * n;
            int p = c % period;
            if (p < 0)
            {
                p += period;
            }
            return p < n ? p : (period - 1 - p);
        }
        case WrapMode::Repeat:
        default:
        {
            int p = c % n;
            if (p < 0)
            {
                p += n;
            }
            return p;
        }
        }
    }

    // Scalar height in [0,1] from a texel per MTL -imfchan ('l' luminance default;
    // r/g/b channel; m = matte/alpha; z = depth, conventionally the blue channel).
    float height_channel(const uint8_t *p, char imfchan) noexcept
    {
        constexpr float inv255 = 1.0f / 255.0f;
        switch (imfchan)
        {
        case 'r':
            return static_cast<float>(p[0]) * inv255;
        case 'g':
            return static_cast<float>(p[1]) * inv255;
        case 'b':
        case 'z':
            return static_cast<float>(p[2]) * inv255;
        case 'm':
            return static_cast<float>(p[3]) * inv255;
        case 'l':
        default:
            return ((0.299f * static_cast<float>(p[0])) + (0.587f * static_cast<float>(p[1])) +
                    (0.114f * static_cast<float>(p[2]))) *
                   inv255;
        }
    }
} // namespace

Texture height_to_normal_map(const Texture &src, char imfchan, float bm)
{
    // The flipped-v sampling path uses the OpenGL +Y normal-map convention.
    constexpr float dy_sign = 1.0f;

    // MTL defines -bm but no height unit, so this renderer supplies a fixed per-texel scale.
    constexpr float kHeightScale = 16.0f;

    // Bound finite but hostile -bm values before fast-math can turn overflow into undefined behavior.
    const float bm_s = clamp(bm, -1e6f, 1e6f);

    Texture out;
    out.width = src.width;
    out.height = src.height;
    out.wrap_s = src.wrap_s;
    out.wrap_t = src.wrap_t;
    if (src.width <= 0 || src.height <= 0 || src.pixels.empty())
    {
        return out;
    }

    const int w = src.width;
    const int h = src.height;
    const size_t n_texels = static_cast<size_t>(w) * static_cast<size_t>(h);
    out.pixels.resize(n_texels * 4);

    // Cache scalar heights because neighboring Sobel kernels reuse each texel.
    std::vector<float> height(n_texels);
    for (size_t i = 0; i < n_texels; i++)
    {
        height[i] = height_channel(src.pixels.data() + (i * 4), imfchan);
    }

    const auto height_at = [&](int x, int y) -> float
    {
        // One unsigned comparison covers both bounds; only edge taps pay for wrapping.
        const int xx = (static_cast<unsigned>(x) < static_cast<unsigned>(w)) ? x : wrap_index(x, w, src.wrap_s);
        const int yy = (static_cast<unsigned>(y) < static_cast<unsigned>(h)) ? y : wrap_index(y, h, src.wrap_t);
        return height[(static_cast<size_t>(yy) * static_cast<size_t>(w)) + static_cast<size_t>(xx)];
    };

    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            // Sobel derivatives suppress spikes from 8-bit quantization. Fixed per-texel
            // strength avoids resolution-dependent relief.
            const float tl = height_at(x - 1, y - 1);
            const float tc = height_at(x, y - 1);
            const float tr = height_at(x + 1, y - 1);
            const float ml = height_at(x - 1, y);
            const float mr = height_at(x + 1, y);
            const float bl = height_at(x - 1, y + 1);
            const float bc = height_at(x, y + 1);
            const float br = height_at(x + 1, y + 1);
            const float dx = ((tr + (2.0f * mr) + br) - (tl + (2.0f * ml) + bl)) * 0.125f;
            const float dy = ((bl + (2.0f * bc) + br) - (tl + (2.0f * tc) + tr)) * 0.125f;

            const vec3 n = normalize(vec3{ -bm_s * kHeightScale * dx, dy_sign * bm_s * kHeightScale * dy, 1.0f });

            const auto enc = [](float c) -> uint8_t
            {
                const float u = clamp((c * 0.5f) + 0.5f, 0.0f, 1.0f) * 255.0f;
                return static_cast<uint8_t>(std::lround(u));
            };

            uint8_t *o =
                out.pixels.data() + (((static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(x)) * 4);
            o[0] = enc(n.x);
            o[1] = enc(n.y);
            o[2] = enc(n.z);
            o[3] = 255;
        }
    }
    return out;
}

bool Texture::load_webp_from_memory(const uint8_t *data, size_t size)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    if (!decode_webp_rgba(data, size, rgba, w, h))
    {
        return false;
    }
    width = w;
    height = h;
    pixels = std::move(rgba);
    return true;
}
