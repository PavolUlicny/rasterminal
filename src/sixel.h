#pragma once

// sixel escape composition: pure string builders, zero I/O (the kitty.h
// pattern: this file composes, framebuffer writes). Everything a frame needs
// is appended into a caller-owned string so the per-frame buffer is reused,
// never reallocated.

#include <cstddef>
#include <memory>
#include <string>

namespace sixel
{
    // Caller-owned encoder scratch (the per-band column masks, 240 x width
    // bytes), grown on demand and deliberately left dirty between frames: the
    // encoder's per-band stamp gates every first touch, so stale contents are
    // never read, and a per-frame allocation plus zero-fill would cost more
    // than the lazy clearing it exists to avoid.
    struct Scratch
    {
        std::unique_ptr<unsigned char[]> mask;
        std::size_t cap = 0;
    };

    // The frame-invariant block of all 240 register definitions, composed once
    // (magic static) and appended verbatim into every frame. Exposed so the
    // framebuffer's reserve consumes the same size the encoder appends instead
    // of restating a hand-counted bound that can drift.
    const std::string &palette_block();

    // Composes one full frame: DCS introducer (P2=1), raster attributes, all
    // 240 palette-register definitions, per-band per-colour passes, ST.
    // `indices` is a row-major width x height plane of xterm-256 palette
    // indices in [16, 255] (colour register = index - 16), the quantizer's
    // output range (quantize_256 never yields a 0..15 system colour, so the
    // subtraction cannot underflow; pinned by test). Non-positive dimensions
    // append nothing. Rows of the last band past `height` are left unset,
    // which under P2=1 paints nothing there.
    void append_frame(std::string &out, const unsigned char *indices, int width, int height, Scratch &scratch);
} // namespace sixel
