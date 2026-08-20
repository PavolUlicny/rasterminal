#include "webp_decode.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

#include "src/webp/decode.h"

bool decode_webp_rgba(const void *data, size_t size, std::vector<uint8_t> &out_rgba, int &w, int &h)
{
    if (!data || size == 0)
    {
        return false;
    }

    const auto *bytes = static_cast<const uint8_t *>(data);

    // WebPGetFeatures validates the RIFF/VP8(L) header and reports the dimensions plus the
    // animation flag in a single parse; it returns non-OK on a malformed or truncated header.
    // libwebp's size param is size_t, so (unlike the basisu API) there is no uint32_t size to
    // bound here.
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(bytes, size, &features) != VP8_STATUS_OK)
    {
        return false;
    }

    // EXT_texture_webp requires a still image. The simple decode API cannot demux animation
    // frames anyway (WebPDecodeRGBAInto would just fail), so reject animated input explicitly
    // and loud rather than allocate a canvas-sized buffer for a decode that cannot succeed.
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

    // The stride (width*4) is passed to WebPDecodeRGBAInto as an int. Valid WebP dimensions are
    // far smaller than that limit (a VP8X canvas is at most 2^24 per axis, VP8/VP8L at most
    // 2^14), so width*4 stays well under INT_MAX and this never fires on a well-formed image;
    // guard the int multiply explicitly anyway so a corrupt oversized width can't overflow it.
    // Past this guard, out_size is computed in size_t and the resize below is bad_alloc-guarded,
    // so an absurd-but-in-range width still fails loud rather than overflowing or terminating.
    if (width > INT_MAX / 4)
    {
        return false;
    }
    const int stride = width * 4;
    const size_t out_size = static_cast<size_t>(stride) * static_cast<size_t>(height);

    // Decode runs on worker threads (decode_textures) with no exception boundary, so an unguarded
    // allocation would terminate the process on OOM. Fail loud instead, and decode straight into
    // our own buffer: there is no libwebp-owned allocation to free.
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
