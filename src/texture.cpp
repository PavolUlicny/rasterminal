#include "texture.h"

#include "ktx2_decode.h"
#include "webp_decode.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

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

    // Copy into a local buffer first: decode runs on worker threads with no exception
    // boundary at the load site, so an unguarded heap copy would terminate the process on
    // OOM. Fail loud instead, and only touch members on success (leave-unchanged contract).
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
    // See load(): guard the heap copy so a worker-thread OOM fails loud instead of
    // terminating, and only touch members on success.
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

// ─── Texture::load_ktx2_from_memory ──────────────────────────────────────────

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

// ─── Texture::load_webp_from_memory ──────────────────────────────────────────

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
