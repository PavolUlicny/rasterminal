#include "src/loaders/ktx2_decode.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "basisu_transcoder.h"

namespace
{
    // The transcoder builds global tables once; texture decoding is multithreaded.
    void ensure_basisu_init()
    {
        static std::once_flag init_flag;
        std::call_once(init_flag, []() { basist::basisu_transcoder_init(); });
    }
} // namespace

bool decode_ktx2_rgba(const void *data, size_t size, std::vector<uint8_t> &out_rgba, int &w, int &h)
{
    // The transcoder accepts only a uint32_t input size.
    if (!data || size == 0 || size > static_cast<size_t>(UINT32_MAX))
    {
        return false;
    }

    ensure_basisu_init();

    basist::ktx2_transcoder transcoder;
    if (!transcoder.init(data, static_cast<uint32_t>(size)))
    {
        return false;
    }
    if (!transcoder.start_transcoding())
    {
        return false;
    }

    basist::ktx2_image_level_info info{};
    if (!transcoder.get_image_level_info(info, /*level=*/0, /*layer=*/0, /*face=*/0))
    {
        return false;
    }

    const uint32_t width = info.m_orig_width;
    const uint32_t height = info.m_orig_height;
    if (width == 0 || height == 0)
    {
        return false;
    }

    // Keep buffer-size safety independent of basisu's current 16384-pixel dimension cap.
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixel_count > static_cast<size_t>(UINT32_MAX))
    {
        return false;
    }

    // Compressed input does not bound decoded size. Convert OOM into a load failure.
    std::vector<uint8_t> rgba;
    try
    {
        rgba.resize(pixel_count * 4);
    }
    catch (const std::bad_alloc &)
    {
        return false;
    }

    // Zero pitch and rows make basisu crop block-aligned data to the original dimensions.
    const bool ok = transcoder.transcode_image_level(
        /*level=*/0, /*layer=*/0, /*face=*/0, rgba.data(), static_cast<uint32_t>(pixel_count),
        basist::transcoder_texture_format::cTFRGBA32
    );
    if (!ok)
    {
        return false;
    }

    out_rgba = std::move(rgba);
    w = static_cast<int>(width);
    h = static_cast<int>(height);
    return true;
}
