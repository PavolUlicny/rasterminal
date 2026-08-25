#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Decode a still WebP image to row-major RGBA8. Animation and malformed input fail,
// leaving all outputs unchanged. The implementation alone includes libwebp headers.
bool decode_webp_rgba(const void *data, size_t size, std::vector<uint8_t> &out_rgba, int &w, int &h);
