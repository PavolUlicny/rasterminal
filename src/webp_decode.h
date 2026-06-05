#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Decode a WebP image (glTF EXT_texture_webp) to 8-bit RGBA.
//
// This confines the libwebp headers to webp_decode.cpp, keeping the whole libwebp
// dependency surface inside one src/ TU — the same isolation draco_decode.cpp and
// ktx2_decode.cpp give the Draco / basis_universal headers. (libwebp's decode.h is a
// clean C API and resolves via -isystem, so this is for consistency and to keep the
// vendored include — and the cppcheck -I — confined to a single source file.)
//
// Decodes a still WebP image to RGBA. Animated WebP is rejected (returns false): the glTF
// EXT_texture_webp extension requires a still image, and libwebp's simple decode API cannot
// demux animation frames regardless. On success, out_rgba holds w*h*4 bytes (row-major,
// top-left first) and w/h are set; returns true. On any failure (malformed header, animation,
// corrupt body, or OOM) returns false and leaves out_rgba/w/h untouched — fail-loud, matching
// the rest of the loaders.
bool decode_webp_rgba(const void *data, size_t size, std::vector<uint8_t> &out_rgba, int &w, int &h);
