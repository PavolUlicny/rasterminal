#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Decode mip 0, layer 0, face 0 of a KTX2 texture to row-major RGBA8. Keeps basisu
// headers out of other source files because they fail the project's warning set.
// Failure leaves all outputs unchanged.
bool decode_ktx2_rgba(const void *data, size_t size, std::vector<uint8_t> &out_rgba, int &w, int &h);
