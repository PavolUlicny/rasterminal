#pragma once

// Sixel protocol builders. They append to caller-owned buffers and perform no I/O.

#include <cstddef>
#include <memory>
#include <string>

namespace sixel
{
    // Grow-only per-band masks. Stamps prevent stale bytes from being read.
    struct Scratch
    {
        std::unique_ptr<unsigned char[]> mask;
        std::size_t cap = 0;
    };

    // Cached palette block, exposed so framebuffer reservation uses its exact size.
    const std::string &palette_block();

    // Compose one frame from row-major xterm palette indices in [16, 255].
    // Non-positive dimensions append nothing.
    void append_frame(std::string &out, const unsigned char *indices, int width, int height, Scratch &scratch);

    // Six-pixel band count without the overflow-prone `height + 5` ceiling form.
    [[nodiscard]] constexpr int band_count(int height) noexcept
    {
        return (height <= 0) ? 0 : (height / 6) + static_cast<int>(height % 6 != 0);
    }

    // Split encoding for worker-owned band ranges. Each concurrent caller needs its own
    // Scratch, and band1 is exclusive. Do not use the split form for degenerate dimensions,
    // because append_footer always emits ST.
    void append_header(std::string &out, int width, int height);
    void append_bands(
        std::string &out, const unsigned char *indices, int width, int height, int band0, int band1, Scratch &scratch
    );
    void append_footer(std::string &out);
} // namespace sixel
