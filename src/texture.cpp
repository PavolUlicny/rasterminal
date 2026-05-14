#include "texture.h"
#include "linalg.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ─── Texture::load ───────────────────────────────────────────────────────────

bool Texture::load(const std::string &path)
{
    int w = 0, h = 0, channels = 0;
    // Force 4 output channels (RGBA) regardless of source format.
    uint8_t *data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!data)
        return false;

    width = w;
    height = h;
    pixels.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(data);
    return true;
}

// ─── Texture::load_from_memory ───────────────────────────────────────────────

bool Texture::load_from_memory(const uint8_t *data, size_t size)
{
    if (!data || size == 0 || size > static_cast<size_t>(INT_MAX))
        return false;
    int w = 0, h = 0, channels = 0;
    uint8_t *img = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
    if (!img)
        return false;
    width = w;
    height = h;
    pixels.assign(img, img + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(img);
    return true;
}

// ─── Texture::sample_rgb ─────────────────────────────────────────────────────

vec3 Texture::sample_rgb(float u, float v) const
{
    // Wrap UV to [0, 1).
    u = u - std::floor(u);
    v = v - std::floor(v);

    // Flip V: OBJ UV v = 0 is the bottom of the image;
    // most image formats store row 0 at the top.
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
        const uint8_t *p = pixels.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
        constexpr float inv255 = 1.0f / 255.0f;
        return {static_cast<float>(p[0]) * inv255, static_cast<float>(p[1]) * inv255, static_cast<float>(p[2]) * inv255};
    };

    const vec3 top = get(x0, y0) * (1.0f - tx) + get(x1, y0) * tx;
    const vec3 bottom = get(x0, y1) * (1.0f - tx) + get(x1, y1) * tx;
    return top * (1.0f - ty) + bottom * ty;
}

// ─── Texture::sample_rgba ─────────────────────────────────────────────────────

vec4 Texture::sample_rgba(float u, float v) const
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

    const vec4 c00 = {static_cast<float>(p00[0]) * inv255, static_cast<float>(p00[1]) * inv255, static_cast<float>(p00[2]) * inv255, static_cast<float>(p00[3]) * inv255};
    const vec4 c10 = {static_cast<float>(p10[0]) * inv255, static_cast<float>(p10[1]) * inv255, static_cast<float>(p10[2]) * inv255, static_cast<float>(p10[3]) * inv255};
    const vec4 c01 = {static_cast<float>(p01[0]) * inv255, static_cast<float>(p01[1]) * inv255, static_cast<float>(p01[2]) * inv255, static_cast<float>(p01[3]) * inv255};
    const vec4 c11 = {static_cast<float>(p11[0]) * inv255, static_cast<float>(p11[1]) * inv255, static_cast<float>(p11[2]) * inv255, static_cast<float>(p11[3]) * inv255};

    const vec4 top = c00 * (1.0f - tx) + c10 * tx;
    const vec4 bottom = c01 * (1.0f - tx) + c11 * tx;
    return top * (1.0f - ty) + bottom * ty;
}
