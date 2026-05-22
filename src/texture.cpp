#include "texture.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// ─── Texture::load ───────────────────────────────────────────────────────────

bool Texture::load(const std::string &path)
{
    int w = 0;
    int h = 0;
    int channels = 0;
    // Force 4 output channels (RGBA) regardless of source format.
    uint8_t *data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!data)
    {
        return false;
    }

    width = w;
    height = h;
    pixels.assign(data, data + (static_cast<size_t>(w) * static_cast<size_t>(h) * 4));
    stbi_image_free(data);
    return true;
}

// ─── Texture::load_from_memory ───────────────────────────────────────────────

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
    width = w;
    height = h;
    pixels.assign(img, img + (static_cast<size_t>(w) * static_cast<size_t>(h) * 4));
    stbi_image_free(img);
    return true;
}
