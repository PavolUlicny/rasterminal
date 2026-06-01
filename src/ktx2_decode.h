#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Decode a KTX2 / Basis Universal texture (glTF KHR_texture_basisu) to 8-bit RGBA.
//
// This confines the basis_universal transcoder headers to ktx2_decode.cpp: the
// transcoder trips the project's strict -Werror set, so it must not be pulled into
// src/ TUs — the same isolation draco_decode.cpp gives the Draco headers.
//
// Decodes mip level 0, layer 0, face 0 only (the base image), which is all the CPU
// rasterizer samples. On success, out_rgba holds w*h*4 bytes (row-major, top-left
// first) and w/h are set; returns true. On any failure (malformed data, unsupported
// container subset, or transcode error) returns false and leaves out_rgba/w/h
// untouched — fail-loud, matching the rest of the loaders.
bool decode_ktx2_rgba(const void *data, size_t size, std::vector<uint8_t> &out_rgba, int &w, int &h);
