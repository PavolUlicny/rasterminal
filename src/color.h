#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// The terminal colour vocabulary: the 8-bit RGB type, the output depth, and the RGB -> xterm-256
// quantizer (table built in color.cpp). Split out of framebuffer.h so a consumer that only speaks
// in colours (hud.h, which styles the status bar) need not pull in the whole Framebuffer class,
// its atomics and its vector storage. Same rationale as shading.h holding ShadingMode for args.h.
// framebuffer.h includes this and re-exports it, so every existing `#include "framebuffer.h"`
// still sees these names.

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

// Terminal colour depth used by present(). TrueColor emits 24-bit 38;2/48;2 SGR (the historical,
// byte-for-byte path); Palette256 quantizes each cell to an xterm-256 index and emits 38;5/48;5.
enum class ColorMode : uint8_t
{
    TrueColor,
    Palette256
};

// The RGB -> xterm-256 quantization table behind present()'s Palette256 mode and the HUD styling:
// 64x64x64 cells (6 high bits per channel, 256 KB), each holding the palette index (16..231 cube,
// 232..255 grey ramp; never a theme-dependent 0..15 system colour) nearest the cell centre by
// squared CIELAB (deltaE76), ties toward the lower index so the cube beats the ramp. CIELAB rather
// than squared RGB because the cube has no chromatic entry below 95 per channel, so an RGB metric
// visibly desaturates dark models onto the grey ramp (the choice over OKLab is argued at
// cielab_from_linear in color.cpp). A cell that contains a palette colour maps exactly to it
// (at 64^3 no cell holds two), so palette-exact pixels (the black and grey backgrounds and the
// HUD bar bg {18,18,18}) round-trip unchanged;
// white {240,240,240} is not a palette colour and takes ramp 238; everything else errs by far
// less than the palette's own spacing (>= 10 grey, >= 40 cube). Built at runtime (CIELAB needs
// cbrt, not constexpr in C++17), paid on the first quantize only: 256-colour presents, sixel
// frames (whose pixels are always palette-quantized), and tests; never a truecolor blocks/kitty
// session or --bench.
inline constexpr size_t QUANT256_LUT_SIZE = size_t{ 64 } * 64u * 64u;

// Defined in color.cpp; the first call builds the table (thread-safe magic static).
const std::array<uint8_t, QUANT256_LUT_SIZE> &quant256_lut() noexcept;

constexpr size_t quant256_idx(Color c) noexcept
{
    return (static_cast<size_t>(c.r >> 2u) << 12u) | (static_cast<size_t>(c.g >> 2u) << 6u) |
           static_cast<size_t>(c.b >> 2u);
}

// quant256_idx over a packed r | g<<8 | b<<16 word (the framebuffer's colour
// layout), ignoring bits 24 and up: the identical index (pinned by test over
// the packed range), but full 32-bit shifts where the Color form makes GCC
// issue 8-bit partial-register shifts per channel (measured -7% on a whole
// 1080p sixel present).
constexpr size_t quant256_idx_packed(uint32_t c) noexcept
{
    return (static_cast<size_t>((c >> 2u) & 0x3Fu) << 12u) | (static_cast<size_t>((c >> 10u) & 0x3Fu) << 6u) |
           static_cast<size_t>((c >> 18u) & 0x3Fu);
}

// Convenience form paying the magic-static init guard per call; present()'s pixel loops instead
// hoist quant256_lut().data() once per frame and index it with quant256_idx directly.
inline uint8_t quantize_256(Color c) noexcept
{
    return quant256_lut()[quant256_idx(c)];
}

// The 240 addressable xterm-256 palette RGB values by entry number 0..239 (palette index is
// 16 + j): the 6x6x6 colour cube on levels {0,95,135,175,215,255}, then the 24-step grey ramp.
// The single source of the palette (one color table, no drift): the LUT build in color.cpp and
// the sixel emitter's colour-register definitions both consume it.
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

// The status bar's two fixed colours, shared so the escape literals framebuffer.cpp emits for
// the row cannot drift from the styling hud.cpp composes into it. framebuffer static_asserts the
// truecolor literals against these, and a test pins their palette indices.
inline constexpr Color HUD_BAR_BG = { 18, 18, 18 };
inline constexpr Color HUD_BAR_FG = { 220, 220, 220 };

// Append the SGR sequence setting the foreground to `c` at `mode`'s depth (38;2;r;g;b or
// 38;5;index, quantizing for the latter). The one place the escape grammar is written for
// string output, so a caller that styles text never spells it out again. Framebuffer's per-cell
// emitter keeps its own specialised form on purpose; the reason is documented there.
void append_fg_sgr(std::string &out, Color c, ColorMode mode);
