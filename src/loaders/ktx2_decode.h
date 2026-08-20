#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Decode a KTX2 / Basis Universal texture (glTF KHR_texture_basisu) to 8-bit RGBA: mip 0,
// layer 0, face 0 only (all the CPU rasterizer samples). Confines the basis_universal
// transcoder headers to ktx2_decode.cpp; they trip the strict -Werror set and must not be
// pulled into src/ TUs (the same isolation draco_decode.cpp gives Draco). On success
// out_rgba holds w*h*4 bytes (row-major,
// top-left first) and w/h are set; on any failure returns false and leaves the outputs
// untouched: fail-loud, matching the rest of the loaders.
bool decode_ktx2_rgba(const void *data, size_t size, std::vector<uint8_t> &out_rgba, int &w, int &h);
