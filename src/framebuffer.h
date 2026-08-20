#pragma once

#include "color.h"    // Color, ColorMode (re-exported: every includer of this header sees them)
#include "graphics.h" // GraphicsBackend (GraphicsConfig tags the present() backend with it)
#include "linalg.h"   // vec3 (for vec3_to_color)
#include "sixel.h"    // sixel::Scratch (the encoder's caller-owned staging)

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Soft-knee highlight rolloff mapping the renderer's HDR shading output into displayable
// range: identity below `knee`, above it values bend smoothly toward (never reaching) 1.0,
// slope-continuous at the knee. Lighting accumulates past 1.0, and a hard clamp would
// collapse every over-range value to flat white, erasing the shading gradient on bright or
// untextured surfaces. Applied exactly once per shaded contribution, at the opaque commit
// and the transparent A-buffer push (both in rasterize.cpp): the transparent resolve
// composites already display-referred values and must NOT tonemap again (that would
// double-darken the opaque base under glass). The unlit path (bounded [0,1], meant faithful)
// is excluded; UI chrome (wireframe/HUD/background, authored in display space) bypasses
// vec3_to_color entirely.
inline float tonemap_channel(float x) noexcept
{
    constexpr float knee = 0.7f;
    constexpr float range = 1.0f - knee; // headroom above the knee
    if (x <= knee)
    {
        return x; // common, well-exposed case: no transcendental
    }
    return knee + (range * (1.0f - std::exp(-(x - knee) / range)));
}

inline vec3 tonemap(vec3 c) noexcept
{
    return { tonemap_channel(c.x), tonemap_channel(c.y), tonemap_channel(c.z) };
}

// Clamp a float RGB colour to [0,1] and pack to 8-bit per channel. Shared by the
// rasterizer's opaque commit path and the transparent resolve so the two quantize
// identically (a divergence here would seam blended against unblended pixels).
constexpr Color vec3_to_color(vec3 c) noexcept
{
    return { static_cast<uint8_t>(clamp(c.x, 0.0f, 1.0f) * 255.0f),
             static_cast<uint8_t>(clamp(c.y, 0.0f, 1.0f) * 255.0f),
             static_cast<uint8_t>(clamp(c.z, 0.0f, 1.0f) * 255.0f) };
}

// Graphics backend configuration, fixed at construction like ColorMode.
// Blocks is the historical half-block backend, where a pixel is half a cell;
// the pixel backends (Kitty, Sixel) present the pixel buffer as one image
// spanning cols x rows cells (the HUD row, when shown, sits just below them).
// shm is kitty-only and selects the shared-memory medium; the caller sets it
// only when the startup query verified the transport end to end, and a
// transiently failing frame falls back to direct transmission for that frame
// alone (identical pixels, so no diagnostic: this is a transport capability,
// not correctness).
struct GraphicsConfig
{
    GraphicsBackend backend = GraphicsBackend::Blocks;
    bool shm = false;
    int cols = 0;
    int rows = 0;
    // 1-based cursor cell where the sixel frame is homed: (1, 1) when the
    // image spans the grid, the centered cell when the terminal's geometry
    // limit letterboxes it (main.cpp's pixel_fb_size computes it). Kitty
    // ignores these: its c=/r= placement always spans the full grid.
    int origin_col = 1;
    int origin_row = 1;
};

class Framebuffer
{
  public:
    // pixel_width  = terminal columns (blocks) or columns * cell width in px (pixel backends)
    // pixel_height = terminal rows * 2 via ▀ (blocks) or image rows * cell height (pixel backends)
    // headless     = true skips all terminal I/O (ANSI escapes, buffer reserve)
    // mode         = terminal colour depth for present() (default 24-bit truecolor)
    // gfx          = graphics backend selection; default is the half-block backend
    // adopt_alt_screen = the caller (the graphics query) already entered the
    //                alternate screen; skip re-entering, so the normal screen
    //                never flashes between the query and the first frame and the
    //                saved cursor keeps its pre-launch position. The dtor leaves
    //                the alternate screen either way.
    Framebuffer(
        int pixel_width,
        int pixel_height,
        bool headless = false,
        ColorMode mode = ColorMode::TrueColor,
        const GraphicsConfig &gfx = {},
        bool adopt_alt_screen = false
    );
    ~Framebuffer();

    Framebuffer(const Framebuffer &) = delete;
    Framebuffer &operator=(const Framebuffer &) = delete;
    Framebuffer(Framebuffer &&) = delete;
    Framebuffer &operator=(Framebuffer &&) = delete;

    [[nodiscard]] int width() const { return m_width; }
    [[nodiscard]] int height() const { return m_height; }
    // The sixel home cell (see GraphicsConfig); the resize path compares the
    // recomputed origin against these, like width()/height() for the dims.
    [[nodiscard]] int origin_col() const { return m_gfx.origin_col; }
    [[nodiscard]] int origin_row() const { return m_gfx.origin_row; }

    // Resize pixel buffer to new dimensions and clear. Owes the terminal a
    // one-shot \033[2J so leftover content from the old (larger) size is
    // wiped; the erase is deferred into the next present's synchronized
    // bracket (see m_pending_clear), so resize itself writes nothing.
    // On a pixel-backend framebuffer use the overload below: this form leaves
    // the image cell rectangle (m_gfx.cols/rows, kitty's c=/r= keys and the
    // HUD row) at its old values, desynchronizing it from the new pixel
    // dimensions.
    void resize(int pixel_width, int pixel_height);

    // Pixel-backend resize: the cell rectangle changes along with the pixel
    // size, and so does the sixel home cell. No origin defaults on purpose: a
    // sixel caller that forgot them would silently re-home a centered image
    // to 1;1, so every caller spells its origins (uncapped configs pass 1, 1).
    void resize(int pixel_width, int pixel_height, int image_cols, int image_rows, int origin_col, int origin_row);

    void clear(Color bg = { 0, 0, 0 });

    [[nodiscard]] Color get_pixel(int x, int y) const
    {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height)
        {
            return {};
        }
        return unpack_color(unpack_color_bits(m_pixel[pixel_idx(x, y)].load(std::memory_order_relaxed)));
    }

    // Bounds-checked, read-only (x,y) depth probe, the get_pixel analog for depth. Returns
    // +inf for out-of-bounds (mirroring get_pixel's default Color{}). Writes go through
    // commit_pixel(); this never mutates.
    [[nodiscard]] float depth_at(int x, int y) const
    {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height)
        {
            return std::numeric_limits<float>::infinity();
        }
        return unpack_depth(m_pixel[pixel_idx(x, y)].load(std::memory_order_relaxed));
    }

    // Keeps the rejection path free of writes so reject-heavy meshes don't generate
    // coherency traffic. Inner-loop callers pass a precomputed `idx` to avoid recomputing
    // y*width+x across the shading work; register pressure spills it otherwise.
    [[nodiscard]] bool depth_test_relaxed(size_t idx, float depth) const noexcept
    {
        const uint64_t cur = m_pixel[idx].load(std::memory_order_relaxed);
        return depth < unpack_depth(cur);
    }

    // Read the depth half of a slot without touching colour. Used by the transparent
    // pass to cull fragments behind opaque geometry with a <= test (it never writes
    // depth). INVARIANT: across the transparent accumulate + resolve passes the depth
    // half is immutable (the opaque pass set it and nothing after writes it), so this
    // read and commit_pixel/get_pixel stay consistent. Do not add a depth write to the
    // resolve path without revisiting this.
    [[nodiscard]] float depth_at(size_t idx) const noexcept
    {
        return unpack_depth(m_pixel[idx].load(std::memory_order_relaxed));
    }

    // idx-based colour read/write for the transparent resolve, peers to depth_at(idx):
    // the linear index is already in hand there, so these skip the (x,y) pixel_idx
    // recompute and bounds branch that get_pixel pays. Single-threaded contract: safe in
    // resolve because workers own disjoint pixels and run post-barrier, and the colour-only
    // store preserves the (immutable, see depth_at) depth.
    [[nodiscard]] Color color_at(size_t idx) const noexcept
    {
        return unpack_color(unpack_color_bits(m_pixel[idx].load(std::memory_order_relaxed)));
    }

    void set_color_at(size_t idx, Color color) noexcept
    {
        auto &slot = m_pixel[idx];
        const uint64_t cur = slot.load(std::memory_order_relaxed);
        slot.store(pack_pixel(unpack_depth(cur), pack_color(color)), std::memory_order_relaxed);
    }

    // Unconditional (depth, colour) store for the tiled opaque path, whose visibility pass has
    // already resolved the nearest fragment per pixel and whose tiles are each owned by one
    // worker, so no CAS is needed. Never mix with concurrent commit_pixel on the same slot.
    void set_pixel_at(size_t idx, float depth, Color color) noexcept
    {
        m_pixel[idx].store(pack_pixel(depth, pack_color(color)), std::memory_order_relaxed);
    }

    // Atomically replaces (depth, color) iff our depth still wins against whatever's
    // in the slot now. Returns false if a concurrent thread became shallower between
    // our depth_test_relaxed() and this call: our fragment is then dropped.
    bool commit_pixel(size_t idx, float depth, Color color) noexcept
    {
        const uint64_t want = pack_pixel(depth, pack_color(color));
        auto &slot = m_pixel[idx];
        uint64_t old = slot.load(std::memory_order_relaxed);
        while (depth < unpack_depth(old))
        {
            if (slot.compare_exchange_weak(old, want, std::memory_order_relaxed, std::memory_order_relaxed))
            {
                return true;
            }
        }
        return false;
    }

    // (x, y) convenience overload for callers outside the rasterizer hot path.
    bool commit_pixel(int x, int y, float depth, Color color) noexcept
    {
        return commit_pixel(pixel_idx(x, y), depth, color);
    }

    // A worker pool the present path may borrow. The framebuffer owns no threads and
    // must not: this is the renderer's pool, idle between frames, wired in by main.cpp.
    // `run` invokes its argument once per worker as (worker_id, n_workers) and returns
    // when all have finished. Left unset (n_workers 1) everything stays serial, which is
    // what the tests and --bench get.
    struct ParallelRunner
    {
        std::function<void(const std::function<void(int, int)> &)> run;
        int n_workers = 1;

        [[nodiscard]] bool usable() const noexcept { return n_workers > 1 && static_cast<bool>(run); }
    };

    void set_parallel_runner(ParallelRunner runner) { m_par = std::move(runner); }

    // Set a one-line status string rendered below the pixel rows each frame; call before
    // present(), empty clears, by value so the composed line moves in. The text may carry its
    // own SGR colour escapes (present() sets the bar background and a default foreground
    // first, so a plain unstyled string draws legibly too) but must not contain a newline or
    // cursor movement: the row is written with auto-wrap off and nothing re-positions the
    // cursor afterwards.
    void set_hud(std::string text) { m_hud = std::move(text); }

    // Flush the pixel buffer to the terminal as a single write.
    void present();

  private:
    // present() body, specialized per colour mode so the truecolor path carries no runtime
    // per-cell branch and stays byte-identical to the historical output. TC == true selects the
    // 24-bit 38;2/48;2 emission; TC == false selects the quantized 38;5/48;5 palette emission.
    template <bool TC> void present_impl();

    // present() body for the kitty backend: one image transmission (skipped when
    // the pixel buffer was not rewritten since the last present) plus the HUD row.
    void present_kitty();

    // present() body for the sixel backend: quantize the pixel buffer to the
    // xterm-240 palette (m_idx) and emit one full sixel frame, gated like kitty.
    void present_sixel();

    // Frame composition brackets shared by the three present bodies: begin
    // clears m_buf and opens synchronized output (mode 2026), so capable
    // terminals paint the whole frame atomically and the rest ignore the pair;
    // end closes it and performs the single fwrite, or writes nothing at all
    // when the frame composed no content (every backend's idle contract).
    void begin_frame();
    void end_frame();

    // The HUD row emission shared by all three backends (cursor to the row below the
    // image/pixel rows, wrap off, bg+default fg, erase BEFORE the text, line,
    // reset). One implementation so the erase-order rule cannot fork.
    void append_hud_line(bool full_redraw);

    // \033[row;colH, 1-based cursor position (no reliance on newlines or
    // auto-wrap); shared by present_impl's row positioning and the HUD block.
    void append_cursor_pos(int row, int col);

    // Serializes the pixel buffer's colour halves as packed RGB bytes into out
    // (3 bytes per pixel, row-major), the wire layout of kitty f=24. Non-const because
    // the parallel split records which workers covered their range (see the definition).
    void write_rgb(unsigned char *out);

    // write_rgb over the pixel range [first, first + count), so the shm transport
    // can convert and hand over one cache-sized chunk at a time instead of
    // materializing a frame-sized buffer.
    void write_rgb_range(unsigned char *out, size_t first, size_t count) const;

    // The two kitty transports. transmit_shm returns false when the shm object
    // cannot be created (present_kitty then falls back to direct for that frame
    // only; shm is retried next frame).
    bool transmit_shm();
    void transmit_direct();

    // Deflate m_rgb[0, len) into m_z as one zlib stream, returning its length, or 0 to
    // mean "compression did not happen" (the caller then sends the frame raw). Splits
    // the work across the borrowed pool when one is set and the frame is worth
    // splitting; otherwise it is the plain one-shot deflate.
    size_t deflate_frame(size_t len);
    size_t deflate_frame_parallel(size_t len, int chunks);

    // Quantize the frame's colours into m_idx, the sixel encoder's input plane.
    void quantize_to_palette(size_t npx);
    [[nodiscard]] const unsigned char *idx_plane() const noexcept { return m_idx.get(); }

    // Append one sixel frame to m_buf, split across the borrowed pool when there is one.
    void encode_sixel_frame();

    // Packed slot layout: high 32 bits = float depth bit pattern,
    // low 24 bits = packed RGB (0x00BBGGRR), top byte of low half reserved (zero).
    static constexpr uint32_t COLOR_MASK = 0x00FFFFFFu;

    static uint32_t pack_color(Color c) noexcept
    {
        return static_cast<uint32_t>(c.r) | (static_cast<uint32_t>(c.g) << 8u) | (static_cast<uint32_t>(c.b) << 16u);
    }

    static Color unpack_color(uint32_t v) noexcept
    {
        return { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8u), static_cast<uint8_t>(v >> 16u) };
    }

    static uint64_t pack_pixel(float depth, uint32_t color_bits) noexcept
    {
        uint32_t db = 0;
        std::memcpy(&db, &depth, sizeof(db));
        return (static_cast<uint64_t>(db) << 32u) | (color_bits & COLOR_MASK);
    }

    static float unpack_depth(uint64_t p) noexcept
    {
        const auto db = static_cast<uint32_t>(p >> 32u);
        float d = 0.0f;
        std::memcpy(&d, &db, sizeof(d));
        return d;
    }

    static uint32_t unpack_color_bits(uint64_t p) noexcept { return static_cast<uint32_t>(p) & COLOR_MASK; }

    // Fills every slot with (+inf depth, bg) using relaxed atomic stores.
    // Must be a loop: std::fill doesn't work on non-copyable atomic<uint64_t>.
    void fill_cleared(uint32_t bg_bits) noexcept
    {
        const uint64_t v = pack_pixel(std::numeric_limits<float>::infinity(), bg_bits);
        for (auto &p : m_pixel)
        {
            p.store(v, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] size_t pixel_idx(int x, int y) const noexcept
    {
        return (static_cast<size_t>(y) * static_cast<size_t>(m_width)) + static_cast<size_t>(x);
    }

    // m_buf's preallocation: a worst-case bound for blocks, a deliberately small
    // starting size for the pixel backends (each branch says why). The per-cell worst case
    // is one combined fg+bg SGR plus the glyph: ~39 B in TrueColor (38;2;r;g;b;48;2;r;g;bm) but only
    // ~23 B in Palette256 (38;5;i;48;5;jm), so 256 mode reserves less. Both constants keep headroom over
    // the worst case for per-row cursor moves and the HUD tail; a miss only costs a one-time realloc.
    // Kitty reserves only the HUD row plus escape slack for either transport: shm frames are a
    // ~60-byte escape, and the direct transport sizes m_buf itself on first transmit
    // (append_transmit_direct reserves the whole encoded frame, keys included, before its chunk loop,
    // guarded so it never shrinks), after which the capacity persists until the next resize shrinks
    // it back. Duplicating that full-frame bound here would be redundant address space and a second
    // allocation for every direct-mode session.
    [[nodiscard]] size_t buf_reserve_bytes() const
    {
        if (m_gfx.backend == GraphicsBackend::Kitty)
        {
            return (static_cast<size_t>(m_gfx.cols) * 50u) + 4096u;
        }
        if (m_gfx.backend == GraphicsBackend::Sixel)
        {
            // The encoder's own palette block + a band of slack + the HUD row.
            // A worst-case bound is unusable here (a noise frame can approach a
            // byte per pixel per touched register), and a typical frame is a
            // few hundred KB: the first frames grow m_buf amortized and the
            // capacity then persists until the next resize shrinks it, the
            // kitty-direct precedent.
            return sixel::palette_block().size() + (static_cast<size_t>(m_width) * 4u) + 4096u;
        }
        const size_t per_cell = (m_mode == ColorMode::TrueColor) ? 50u : 32u;
        return static_cast<size_t>(m_width) * static_cast<size_t>(m_height / 2) * per_cell;
    }

    // The pixel protocols share the image-spanning present model (cols x rows
    // cells above the HUD row); Blocks alone renders per cell.
    [[nodiscard]] bool is_pixel_backend() const noexcept { return m_gfx.backend != GraphicsBackend::Blocks; }

    int m_width, m_height;
    std::vector<std::atomic<uint64_t>> m_pixel;
    // plain: only read/written by single-threaded present(). Value domain is mode-dependent: packed
    // RGB in TrueColor, palette indices in Palette256 (safe because m_mode is fixed at construction; a
    // future runtime mode switch would need m_force_redraw to avoid stale index-vs-RGB comparisons).
    // Left empty on the pixel backends, which have no per-cell diff (8 MB at 1080p it would never read).
    std::vector<uint32_t> m_prev_color;
    // Kitty direct-transport staging (pixel unpack and deflate output), empty
    // until first used. Raw arrays rather than vectors on purpose: both are
    // fully overwritten every use (write_rgb / mz_compress2), and vector::resize
    // would zero-fill them first, a ~12 MB memset at 1080p on the first direct
    // frame after every resize. Capacities tracked beside the pointers.
    std::unique_ptr<unsigned char[]> m_rgb;
    size_t m_rgb_cap = 0;
    std::unique_ptr<unsigned char[]> m_z;
    size_t m_z_cap = 0;
    // Parallel-deflate scratch: one output block per chunk, and the byte count each
    // chunk produced. Capacity persists across frames like the buffers above.
    std::unique_ptr<unsigned char[]> m_zchunk;
    size_t m_zchunk_cap = 0; // total bytes across all chunk blocks
    std::vector<size_t> m_zchunk_len;
    // Per-chunk adler32, folded into the stream's. uint32_t rather than miniz's mz_ulong
    // so the vendored header stays out of this one: the value is two 16-bit sums.
    std::vector<uint32_t> m_zchunk_adler;
    // Which workers actually ran their share of the split just dispatched. Shared by the three
    // splits (staging fill, palette quantize, sixel encode), which never overlap.
    std::vector<uint8_t> m_par_covered;
    // Sixel staging: the frame quantized to xterm-256 palette indices, the
    // emitter's input plane. Same raw-array rationale as m_rgb/m_z above.
    std::unique_ptr<unsigned char[]> m_idx;
    size_t m_idx_cap = 0;
    // The sixel encoder's caller-owned band masks (grow-only, dirty between
    // frames by contract; see sixel::Scratch).
    sixel::Scratch m_sixel_scratch;
    // Parallel sixel encode: one output buffer and one Scratch per worker, since the
    // staging is not shareable. Allocated only when the split actually runs, and the
    // capacity persists like every other staging buffer here.
    std::vector<std::string> m_sixel_parts;
    std::vector<sixel::Scratch> m_sixel_par_scratch;
    std::string m_buf;      // reused output buffer, avoids per-frame allocation
    std::string m_hud;      // status line written below pixel rows
    std::string m_prev_hud; // last HUD actually emitted; present() skips an unchanged one
    bool m_force_redraw = true;
    // A resize owes the terminal one whole-screen erase (\033[2J), emitted by
    // the NEXT present inside its synchronized-output bracket rather than by
    // resize() itself: flushed immediately it blanks the window for however
    // long the first re-render takes, exactly the flash mode 2026 exists to
    // prevent; deferred, the terminal swaps atomically from the old content to
    // the erased-and-repainted frame.
    bool m_pending_clear = false;
    bool m_headless = false;
    ColorMode m_mode = ColorMode::TrueColor;
    GraphicsConfig m_gfx;
    // Whether the pixel buffer was rewritten (clear() ran) since the last
    // pixel-backend emission; an unchanged frame emits no image bytes at all. This is why
    // every rendered frame must begin with clear() (main.cpp gates render and
    // clear together): commit_pixel and the transparent resolve deliberately do
    // not arm this flag, since a per-pixel store on the hot path would cost more
    // than the transmit it saves.
    bool m_image_dirty = true;
    // Whether a transmitted image is currently resident in the terminal; lets a
    // shrink to zero image rows delete the now-orphaned frame exactly once.
    bool m_image_shown = false;
    // Rotates the shm object name through a small ring: the terminal unlinks an
    // object only after reading it, so reusing one name could overwrite a frame
    // it has not read yet.
    unsigned m_shm_seq = 0;
    ParallelRunner m_par;
};
