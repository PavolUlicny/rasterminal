#include "src/loaders/webp_decode.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

// This must resolve from vendor/libwebp; a project src/webp/ directory would shadow it.
#include "src/webp/decode.h"

bool decode_webp_rgba(const void *data, size_t size, std::vector<uint8_t> &out_rgba, int &w, int &h)
{
    if (!data || size == 0)
    {
        return false;
    }

    const auto *bytes = static_cast<const uint8_t *>(data);

    // Parse and validate the header before allocating the output.
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(bytes, size, &features) != VP8_STATUS_OK)
    {
        return false;
    }

    // EXT_texture_webp permits only still images.
    if (features.has_animation)
    {
        return false;
    }

    const int width = features.width;
    const int height = features.height;
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    // libwebp takes stride as int, so guard the multiply even though valid WebP is smaller.
    if (width > INT_MAX / 4)
    {
        return false;
    }
    const int stride = width * 4;
    const size_t out_size = static_cast<size_t>(stride) * static_cast<size_t>(height);

    // Worker decode has no exception boundary, so turn OOM into a load failure.
    std::vector<uint8_t> rgba;
    try
    {
        rgba.resize(out_size);
    }
    catch (const std::bad_alloc &)
    {
        return false;
    }

    if (!WebPDecodeRGBAInto(bytes, size, rgba.data(), rgba.size(), stride))
    {
        return false;
    }

    out_rgba = std::move(rgba);
    w = width;
    h = height;
    return true;
}
