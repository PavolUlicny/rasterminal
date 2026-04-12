#pragma once

#include "linalg.h"

#include <cstdint>
#include <string>
#include <vector>

// A 2-D RGBA texture loaded from a PNG file.
// Only 8-bit-per-channel PNG is supported (color types 0/2/4/6).
// Interlaced and indexed (palette) PNGs are not supported.
struct Texture
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // RGBA, row-major, top-left first (PNG storage order)

    // Load a texture from disk.  Supports JPEG, PNG, BMP, TGA, GIF, and more
    // (via stb_image).  Returns false and leaves the object unchanged on error.
    bool load_png(const std::string &path);

    bool valid() const { return width > 0 && height > 0; }

    // Sample the texture at normalised (u, v) with bilinear interpolation.
    // UV coordinates repeat (wrap mode).
    // V is flipped so that OBJ convention (v = 0 at bottom) maps correctly
    // to PNG storage order (row 0 at top).
    // Returns RGB in [0, 1].
    vec3 sample_rgb(float u, float v) const;
};
