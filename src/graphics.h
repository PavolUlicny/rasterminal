#pragma once

// Terminal graphics capability vocabulary and the pure half of detection.
// platform.h owns the impure half (writing the query batch, reading stdin);
// this scanner is a pure function over a byte span so the reply grammar is
// unit-testable with no fds on every platform, the same split as
// input.h/parse_input and classify_term_color/detect_term_color.

// What the startup query learned. kitty_shm is the shared-memory transport
// verified END TO END (the query batch names a real shm object; a terminal
// that cannot open it, being remote or sandboxed, answers with an error).
// cell_w/cell_h are the terminal's cell size in pixels; 0 means the terminal
// did not answer, and the caller falls back (TIOCGWINSZ pixel fields, then an
// 8x16 assumption).
struct TermGraphics
{
    bool kitty = false;
    bool kitty_shm = false;
    int cell_w = 0;
    int cell_h = 0;
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
// the cell-size report ("\033[6;<h>;<w>t"), and the DSR sentinel (final 'n')
// that says the terminal has answered everything it is going to. Anything else
// (keystrokes typed during the window, unrelated replies) is located by its own
// terminator and skipped. Idempotent over a growing buffer: the caller may
// rescan from the start or compact by `consumed` between reads.
ReplyScan parse_graphics_replies(const char *buf, int len, TermGraphics &out);
