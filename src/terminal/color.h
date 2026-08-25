#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Terminal RGB, output depth and xterm-256 quantization, kept independent of Framebuffer.

struct Color
{
    uint8_t r, g, b;
    constexpr Color() noexcept : r(0), g(0), b(0) {}
    constexpr Color(uint8_t r_, uint8_t g_, uint8_t b_) noexcept : r(r_), g(g_), b(b_) {}
};

constexpr bool operator==(Color a, Color b) noexcept
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
constexpr bool operator!=(Color a, Color b) noexcept
{
    return !(a == b);
}

// Text output depth used by present().
enum class ColorMode : uint8_t
{
    TrueColor,
    Palette256
};

// A 256 KB LUT maps six high bits per RGB channel to the nearest non-system xterm color
// by deltaE76. Palette colors round-trip exactly. The first 256-color or sixel frame builds it.
inline constexpr size_t QUANT256_LUT_SIZE = size_t{ 64 } * 64u * 64u;

// Defined in color.cpp; the first call builds the table (thread-safe magic static).
const std::array<uint8_t, QUANT256_LUT_SIZE> &quant256_lut() noexcept;

constexpr size_t quant256_idx(Color c) noexcept
{
    return (static_cast<size_t>(c.r >> 2u) << 12u) | (static_cast<size_t>(c.g >> 2u) << 6u) |
           static_cast<size_t>(c.b >> 2u);
}

// Packed-color form avoids GCC's slower partial-register shifts, worth 7% at 1080p sixel.
constexpr size_t quant256_idx_packed(uint32_t c) noexcept
{
    return (static_cast<size_t>((c >> 2u) & 0x3Fu) << 12u) | (static_cast<size_t>((c >> 10u) & 0x3Fu) << 6u) |
           static_cast<size_t>((c >> 18u) & 0x3Fu);
}

// Convenience form; hot loops hoist the LUT pointer to avoid the static-init guard.
inline uint8_t quantize_256(Color c) noexcept
{
    return quant256_lut()[quant256_idx(c)];
}

// The 240 non-system xterm colors: a 6x6x6 cube followed by the gray ramp.
constexpr Color quant256_palette_entry(int j) noexcept
{
    if (j < 216)
    {
        const auto val = [](int l) { return static_cast<uint8_t>(l == 0 ? 0 : 55 + (40 * l)); };
        return { val(j / 36), val((j / 6) % 6), val(j % 6) };
    }
    const auto v = static_cast<uint8_t>(8 + (10 * (j - 216)));
    return { v, v, v };
}

// Shared HUD colors keep framebuffer escapes and HUD styling in sync.
inline constexpr Color HUD_BAR_BG = { 18, 18, 18 };
inline constexpr Color HUD_BAR_FG = { 220, 220, 220 };

// Append a truecolor or palette foreground SGR.
void append_fg_sgr(std::string &out, Color c, ColorMode mode);
