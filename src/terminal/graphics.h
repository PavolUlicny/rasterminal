#pragma once

// Pure terminal-graphics reply parsing. platform.h owns query I/O.

#include <cstdint>

// Rendering backends available to present().
enum class GraphicsBackend : std::uint8_t
{
    Blocks,
    Kitty,
    Sixel
};

// Results of the startup query. Shared memory is verified with a real object.
// Zero dimensions mean unreported.
struct TermGraphics
{
    // Once a query stops for a control request, SIGCONT cannot make it complete.
    bool interrupted = false;
    // Setup or I/O failed; this is not a negative capability reply.
    bool failed = false;
    bool kitty = false;
    bool kitty_shm = false;
    bool sixel = false;
    int cell_w = 0;
    int cell_h = 0;
    // XTSMGRAPHICS size limit. xterm discards oversized images instead of clipping them.
    int sixel_max_w = 0;
    int sixel_max_h = 0;
};

// `done` means the DSR sentinel arrived. `consumed` excludes any partial sequence.
struct ReplyScan
{
    bool done = false;
    int consumed = 0;
};

// Parse kitty, cell-size, DA1, sixel-size and DSR replies. Skip unrelated complete
// sequences and loose input. The caller may compact the buffer by `consumed`.
ReplyScan parse_graphics_replies(const char *buf, int len, TermGraphics &out);
