#include "ktx2_decode.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "basisu_transcoder.h"

namespace
{
    // basisu_transcoder_init() builds global lookup tables and must run once before any
    // transcode. Texture decode runs on worker threads (decode_textures), so the call
    // must be one-time and thread-safe: std::call_once guarantees both.
    void ensure_basisu_init()
    {
        static std::once_flag init_flag;
        std::call_once(init_flag, []() { basist::basisu_transcoder_init(); });
    }
} // namespace

bool decode_ktx2_rgba(const void *data, size_t size, std::vector<uint8_t> &out_rgba, int &w, int &h)
{
    // The transcoder API takes a uint32_t size; reject anything that would not fit
    // before touching it (also covers null/empty).
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

    // ktx2_transcoder::init() already rejects any dimension above 16384
    // (BASISU_MAX_SUPPORTED_TEXTURE_DIMENSION), so pixel_count fits in 32 bits and the
    // uint32_t buffer-size argument below cannot truncate. Guard it explicitly anyway so
    // that safety does not silently depend on a vendored constant across upstream bumps.
    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixel_count > static_cast<size_t>(UINT32_MAX))
    {
        return false;
    }

    // Unlike the mesh loaders, the output size cannot be bounded against the file size:
    // a small (super)compressed KTX2 legitimately decodes to a large image (the ETC1S /
    // UASTC compressed-to-pixel ratio is unbounded). The transcoder's dimension cap
    // bounds the worst case (<=16384^2, i.e. <=1 GiB here); this guard only makes an
    // oversized allocation fail loud (returning false) instead of letting std::bad_alloc
    // propagate and terminate, since nothing at the load boundary catches it.
    std::vector<uint8_t> rgba;
    try
    {
        rgba.resize(pixel_count * 4);
    }
    catch (const std::bad_alloc &)
    {
        return false;
    }

    // cTFRGBA32 writes one RGBA pixel per output element. With pitch/rows left 0 the
    // transcoder defaults them to orig_width/orig_height and assumes an orig-sized
    // output buffer (it crops block-aligned codecs back to the original dimensions).
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
