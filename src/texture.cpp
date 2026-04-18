#include "texture.h"

#include <cmath>
#include <algorithm>

// stb_image: single-header image loader (public domain).
// Supports JPEG, PNG, BMP, TGA, GIF, PSD, HDR, PNM.
// The implementation is compiled here and nowhere else.
// Suppress warnings from vendored third-party code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wduplicated-branches"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma GCC diagnostic pop

// ─── Texture::load ───────────────────────────────────────────────────────────

bool Texture::load(const std::string &path)
{
    int w, h, channels;
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

// ─── Texture::sample_rgb ─────────────────────────────────────────────────────

vec3 Texture::sample_rgb(float u, float v) const
{
    // Wrap UV to [0, 1).
    u = u - std::floor(u);
    v = v - std::floor(v);

    // Flip V: OBJ UV v = 0 is the bottom of the image;
    // most image formats store row 0 at the top.
    v = 1.0f - v;

    float fx = u * static_cast<float>(width - 1);
    float fy = v * static_cast<float>(height - 1);

    int x0 = static_cast<int>(fx);
    int y0 = static_cast<int>(fy);
    int x1 = std::min(x0 + 1, width - 1);
    int y1 = std::min(y0 + 1, height - 1);

    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);

    auto get = [&](int x, int y) -> vec3
    {
        const uint8_t *p = pixels.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
        constexpr float inv255 = 1.0f / 255.0f;
        return {p[0] * inv255, p[1] * inv255, p[2] * inv255};
    };

    vec3 top = get(x0, y0) * (1.0f - tx) + get(x1, y0) * tx;
    vec3 bottom = get(x0, y1) * (1.0f - tx) + get(x1, y1) * tx;
    return top * (1.0f - ty) + bottom * ty;
}
