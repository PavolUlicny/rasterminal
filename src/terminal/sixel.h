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

    // Number of 6-pixel-row bands a frame of this height is encoded as. Spelled without the
    // `height + 5` of the usual ceiling form, which overflows for a height near INT_MAX; no
    // caller can reach that (main.cpp caps a framebuffer side at 8192), but this is public and
    // constexpr, so it should not carry UB a caller has to know to avoid.
    [[nodiscard]] constexpr int band_count(int height) noexcept
    {
        return (height <= 0) ? 0 : (height / 6) + static_cast<int>(height % 6 != 0);
    }

    // append_frame split into its three pieces, so a caller with a worker pool can encode
    // disjoint band ranges into separate buffers and concatenate them in order. Bands are
    // independent (each covers its own 6 pixel rows, the per-band staging is reset by the stamp,
    // and the band separator belongs to the band before it), so the concatenation is exactly
    // append_frame's bytes. Each concurrent caller needs its OWN Scratch; `band1` is exclusive
    // and both bounds are clamped, so a range past the end is fine.
    //
    // The caller owes one check the pieces do not make for it: append_header and append_bands
    // both decline a non-positive width or height, but append_footer takes no dimensions and
    // always emits the string terminator, so a degenerate frame must not be sent through the
    // three-piece form at all or it emits a bare ST with no DCS to end. append_frame handles
    // this itself.
    void append_header(std::string &out, int width, int height);
    void append_bands(
        std::string &out, const unsigned char *indices, int width, int height, int band0, int band1, Scratch &scratch
    );
    void append_footer(std::string &out);
} // namespace sixel
