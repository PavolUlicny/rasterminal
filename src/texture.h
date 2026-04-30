#pragma once

#include "linalg.h"

#include <cstdint>
#include <string>
#include <vector>

// A 2-D RGBA texture loaded from an image file.
// Supports JPEG, PNG, BMP, TGA, GIF, and more via stb_image.
struct Texture
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // RGBA, row-major, top-left first

    // Load a texture from disk.  Returns false and leaves the object unchanged on error.
    bool load(const std::string &path);

    // Load a texture from a memory buffer (e.g. embedded GLB image data).
    // Returns false and leaves the object unchanged on error.
    bool load_from_memory(const uint8_t *data, size_t size);

    bool valid() const { return width > 0 && height > 0; }

    // Sample the texture at normalised (u, v) with bilinear interpolation.
    // UV coordinates repeat (wrap mode).
    // V is flipped so that OBJ convention (v = 0 at bottom) maps correctly
    // to image storage order (row 0 at top).
    // Returns RGB in [0, 1].
    vec3 sample_rgb(float u, float v) const;
};
