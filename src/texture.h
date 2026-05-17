#pragma once

#include "linalg.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A 2-D RGBA texture loaded from an image file.
// Supports JPEG, PNG, BMP, TGA, GIF, and more via stb_image.
struct Texture
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // RGBA, row-major, top-left first

    // Load a texture from disk.  Returns false and leaves the object unchanged on error.
    [[nodiscard]] bool load(const std::string &path);

    // Load a texture from a memory buffer (e.g. embedded GLB image data).
    // Returns false and leaves the object unchanged on error.
    [[nodiscard]] bool load_from_memory(const uint8_t *data, size_t size);

    [[nodiscard]] bool valid() const { return width > 0 && height > 0; }

    // Sample the texture at normalised (u, v) with bilinear interpolation.
    // UV coordinates repeat (wrap mode).
    // V is flipped so that OBJ convention (v = 0 at bottom) maps correctly
    // to image storage order (row 0 at top).
    // Returns RGB in [0, 1].
    [[nodiscard]] vec3 sample_rgb(float u, float v) const
    {
        u = u - std::floor(u);
        v = v - std::floor(v);
        v = 1.0f - v;

        const float fx = u * static_cast<float>(width - 1);
        const float fy = v * static_cast<float>(height - 1);

        const int x0 = static_cast<int>(fx);
        const int y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, width - 1);
        const int y1 = std::min(y0 + 1, height - 1);

        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);

        auto get = [&](int x, int y) -> vec3
        {
            const uint8_t *p =
                pixels.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
            constexpr float inv255 = 1.0f / 255.0f;
            return { static_cast<float>(p[0]) * inv255, static_cast<float>(p[1]) * inv255,
                     static_cast<float>(p[2]) * inv255 };
        };

        const vec3 top = get(x0, y0) * (1.0f - tx) + get(x1, y0) * tx;
        const vec3 bottom = get(x0, y1) * (1.0f - tx) + get(x1, y1) * tx;
        return top * (1.0f - ty) + bottom * ty;
    }

    // Same as sample_rgb but also returns the alpha channel in .w.
    // Used by the alpha-cutout path; separate from sample_rgb so non-cutout
    // callers (nmap, stex) pay no alpha computation cost.
    [[nodiscard]] vec4 sample_rgba(float u, float v) const
    {
        u = u - std::floor(u);
        v = v - std::floor(v);
        v = 1.0f - v;

        const float fx = u * static_cast<float>(width - 1);
        const float fy = v * static_cast<float>(height - 1);

        const int x0 = static_cast<int>(fx);
        const int y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, width - 1);
        const int y1 = std::min(y0 + 1, height - 1);

        const float tx = fx - static_cast<float>(x0);
        const float ty = fy - static_cast<float>(y0);

        constexpr float inv255 = 1.0f / 255.0f;
        const uint8_t *base = pixels.data();
        const auto w = static_cast<size_t>(width);
        const uint8_t *p00 = base + (static_cast<size_t>(y0) * w + static_cast<size_t>(x0)) * 4;
        const uint8_t *p10 = base + (static_cast<size_t>(y0) * w + static_cast<size_t>(x1)) * 4;
        const uint8_t *p01 = base + (static_cast<size_t>(y1) * w + static_cast<size_t>(x0)) * 4;
        const uint8_t *p11 = base + (static_cast<size_t>(y1) * w + static_cast<size_t>(x1)) * 4;

        const vec4 c00 = { static_cast<float>(p00[0]) * inv255, static_cast<float>(p00[1]) * inv255,
                           static_cast<float>(p00[2]) * inv255, static_cast<float>(p00[3]) * inv255 };
        const vec4 c10 = { static_cast<float>(p10[0]) * inv255, static_cast<float>(p10[1]) * inv255,
                           static_cast<float>(p10[2]) * inv255, static_cast<float>(p10[3]) * inv255 };
        const vec4 c01 = { static_cast<float>(p01[0]) * inv255, static_cast<float>(p01[1]) * inv255,
                           static_cast<float>(p01[2]) * inv255, static_cast<float>(p01[3]) * inv255 };
        const vec4 c11 = { static_cast<float>(p11[0]) * inv255, static_cast<float>(p11[1]) * inv255,
                           static_cast<float>(p11[2]) * inv255, static_cast<float>(p11[3]) * inv255 };

        const vec4 top = c00 * (1.0f - tx) + c10 * tx;
        const vec4 bottom = c01 * (1.0f - tx) + c11 * tx;
        return top * (1.0f - ty) + bottom * ty;
    }
};
