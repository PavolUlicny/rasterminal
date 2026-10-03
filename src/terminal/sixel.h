#pragma once

// Sixel protocol builders. They append to caller-owned buffers and perform no I/O.

#include "src/terminal/color.h"

#include <cstddef>
#include <cstdint>
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

    // Registers the encoder can address. Its scratch is sized for all of them, so any byte in
    // the register plane stays in bounds.
    inline constexpr int MAX_REGISTERS = 256;

    // The longest register block: MAX_REGISTERS definitions of up to 18 bytes (`#255;2;100;100;100`).
    inline constexpr std::size_t MAX_REGISTER_BLOCK_BYTES = std::size_t{ MAX_REGISTERS } * 18u;

    // Sixel register channels are 0..100; round-to-nearest keeps 0 and 255 exact.
    [[nodiscard]] constexpr unsigned int channel_pct(uint8_t v) noexcept
    {
        return ((static_cast<unsigned int>(v) * 100u) + 127u) / 255u;
    }

    // The 8-bit value a register percentage stands for. Terminals convert with their own
    // rounding and may differ by one.
    [[nodiscard]] constexpr uint8_t channel_from_pct(unsigned int pct) noexcept
    {
        return static_cast<uint8_t>(((pct * 255u) + 50u) / 100u);
    }

    // Append one RGB register definition, `#reg;2;r;g;b` in percent.
    void append_register(std::string &out, int reg, Color c);

    // Compose one frame from row-major register numbers. The block holds the register
    // definitions and must define every register the plane uses.
    // Non-positive dimensions append nothing.
    void append_frame(
        std::string &out,
        const unsigned char *registers,
        int width,
        int height,
        const std::string &register_block,
        Scratch &scratch
    );

    // Six-pixel band count without the overflow-prone `height + 5` ceiling form.
    [[nodiscard]] constexpr int band_count(int height) noexcept
    {
        return (height <= 0) ? 0 : (height / 6) + static_cast<int>(height % 6 != 0);
    }

    // Split encoding for worker-owned band ranges. Each concurrent caller needs its own
    // Scratch, and band1 is exclusive. Do not use the split form for degenerate dimensions,
    // because append_footer always emits ST.
    void append_header(std::string &out, int width, int height, const std::string &register_block);
    void append_bands(
        std::string &out, const unsigned char *registers, int width, int height, int band0, int band1, Scratch &scratch
    );
    void append_footer(std::string &out);
} // namespace sixel
