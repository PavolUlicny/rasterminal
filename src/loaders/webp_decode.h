#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Decode a still WebP image (glTF EXT_texture_webp) to 8-bit RGBA. Animated WebP is rejected
// (returns false): the extension requires a still image, and libwebp's simple decode API
// cannot demux frames regardless. Confines the libwebp headers to webp_decode.cpp, the same
// isolation draco_decode.cpp / ktx2_decode.cpp give their vendors (libwebp's decode.h is
// clean C and resolves via -isystem, so this is for consistency and to keep the vendored
// include, and the cppcheck -I, in one TU). On success out_rgba holds w*h*4 bytes (row-major,
// top-left first) and w/h are set; on any failure (malformed header, animation, corrupt
// body, OOM) returns false and leaves the outputs untouched: fail-loud, matching the rest
// of the loaders.
bool decode_webp_rgba(const void *data, size_t size, std::vector<uint8_t> &out_rgba, int &w, int &h);
