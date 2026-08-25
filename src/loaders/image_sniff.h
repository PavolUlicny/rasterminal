#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

inline bool is_ktx2(const uint8_t *data, size_t size)
{
    static constexpr uint8_t magic[12] = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    return data && size >= sizeof(magic) && std::memcmp(data, magic, sizeof(magic)) == 0;
}

inline bool is_webp(const uint8_t *data, size_t size)
{
    return data && size >= 12 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBP", 4) == 0;
}

// AMF stores raw RGBA bytes in pcData; other importers use aiTexel fields.
enum class TexelLayout : std::uint8_t
{
    ArgbTexels,      // aiTexel field order: memory holds b,g,r,a per pixel
    RgbaInterleaved, // plain bytes hold r,g,b,a per pixel, absent channels omitted
};

// AMF may leave the terminator byte uninitialized.
inline TexelLayout embedded_texel_layout(std::string_view hint, bool amf_origin)
{
    const std::string_view head = hint.substr(0, 8);
    const bool well_formed_rgba = head.size() == 8 && head.compare(0, 4, "rgba") == 0 &&
                                  head.find_first_not_of("80", 4) == std::string_view::npos &&
                                  head.find('8', 4) != std::string_view::npos;
    return amf_origin && well_formed_rgba ? TexelLayout::RgbaInterleaved : TexelLayout::ArgbTexels;
}
