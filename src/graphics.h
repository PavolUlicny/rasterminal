#pragma once

// Terminal graphics capability vocabulary and the pure half of detection.
// platform.h owns the impure half (writing the query batch, reading stdin);
// this scanner is a pure function over a byte span so the reply grammar is
// unit-testable with no fds on every platform, the same split as
// input.h/parse_input and classify_term_color/detect_term_color.

#include <cstdint>

// The three rendering backends present() can drive: the historical half-block
// cell renderer and the two pixel protocols.
enum class GraphicsBackend : std::uint8_t
{
    Blocks,
    Kitty,
    Sixel
};

// What the startup query learned. kitty_shm is the shared-memory transport
// verified END TO END (the query batch names a real shm object; a terminal
// that cannot open it, being remote or sandboxed, answers with an error).
// sixel is DA1 parameter 4 in the primary device attributes reply.
// cell_w/cell_h are the terminal's cell size in pixels; 0 means the terminal
// did not answer, and the caller falls back (TIOCGWINSZ pixel fields, then an
// 8x16 assumption).
struct TermGraphics
{
    bool kitty = false;
    bool kitty_shm = false;
    bool sixel = false;
    int cell_w = 0;
    int cell_h = 0;
    // Max sixel image size in px (XTSMGRAPHICS item-2 read; 0 = unreported).
    // xterm discards, not clips, an image past this (default min(window,
    // 1000x1000)), so the framebuffer must be capped by it.
    int sixel_max_w = 0;
    int sixel_max_h = 0;
};

// One scan over the reply buffer.
//   done     - the DSR sentinel arrived: every reply the terminal will send is in
//   consumed - bytes fully processed; the caller may drop them and append fresh
//              reads after the remainder (a partial sequence stays unconsumed)
struct ReplyScan
{
    bool done = false;
    int consumed = 0;
};

// Scans the bytes read since the query batch was written, recording into `out`
// whatever replies it finds: the kitty graphics OK ("\033_Gi=<id>;OK\033\\"),
// the cell-size report ("\033[6;<h>;<w>t"), the DA1 reply ("\033[?<params>c"),
// the sixel-geometry reply ("\033[?2;0;<w>;<h>S"), and the DSR sentinel
// (final 'n', the wire order the batch requests them in)
// that says the terminal has answered everything it is going to. Anything else
// (keystrokes typed during the window, unrelated replies) is located by its own
// terminator and skipped. Idempotent over a growing buffer: the caller may
// rescan from the start or compact by `consumed` between reads.
ReplyScan parse_graphics_replies(const char *buf, int len, TermGraphics &out);
