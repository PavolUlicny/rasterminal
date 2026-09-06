#pragma once

#include "src/math/linalg.h"       // vec3 (for vec3_to_color)
#include "src/terminal/color.h"    // Color, ColorMode (re-exported: every includer of this header sees them)
#include "src/terminal/graphics.h" // GraphicsBackend (GraphicsConfig tags the present() backend with it)
#include "src/terminal/sixel.h"    // sixel::Scratch (the encoder's caller-owned staging)

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

// Soft-knee HDR rolloff. Apply once to lit contributions, before transparency compositing.
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

// Clamp and pack RGB identically for opaque and transparent paths.
constexpr Color vec3_to_color(vec3 c) noexcept
{
    return { static_cast<uint8_t>(clamp(c.x, 0.0f, 1.0f) * 255.0f),
             static_cast<uint8_t>(clamp(c.y, 0.0f, 1.0f) * 255.0f),
             static_cast<uint8_t>(clamp(c.z, 0.0f, 1.0f) * 255.0f) };
}

// Fixed backend configuration. Kitty may retry direct transport when shm fails.
struct GraphicsConfig
{
    GraphicsBackend backend = GraphicsBackend::Blocks;
    bool shm = false;
    int cols = 0;
    int rows = 0;
    // 1-based sixel origin; ignored by kitty.
    int origin_col = 1;
    int origin_row = 1;
};

class Framebuffer
{
  public:
    // Inject writers to test short writes and cleanup failures.
    using WriteFrameFn = int64_t (*)(const char *, size_t);
    using WriteCleanupFn = bool (*)(const char *);

    // Pixel dimensions are literal for image backends; blocks use two pixels per cell.
    // adopt_alt_screen means startup detection already entered the alternate screen.
    // Call resume_terminal() to acquire output for a non-headless framebuffer.
    Framebuffer(
        int pixel_width,
        int pixel_height,
        bool headless = false,
        ColorMode mode = ColorMode::TrueColor,
        const GraphicsConfig &gfx = {},
        bool adopt_alt_screen = false,
        WriteFrameFn write_frame = nullptr
    );
    ~Framebuffer() noexcept;

    // Release the terminal while retaining render buffers; resume forces a redraw.
    // Clear a pending mouse reset only after the complete cleanup write succeeds.
    void suspend_terminal(WriteCleanupFn write_cleanup = nullptr, bool *mouse_cleanup_pending = nullptr) noexcept;
    bool resume_terminal(bool *canceled = nullptr) noexcept;

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

    // Resize blocks output. The next synchronized present clears stale terminal content.
    void resize(int pixel_width, int pixel_height);

    // Resize image output, including its cell rectangle and explicit sixel origin.
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

    // Bounds-checked depth probe; out-of-bounds is +inf.
    [[nodiscard]] float depth_at(int x, int y) const
    {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height)
        {
            return std::numeric_limits<float>::infinity();
        }
        return unpack_depth(m_pixel[pixel_idx(x, y)].load(std::memory_order_relaxed));
    }

    // Relaxed read for early rejection; callers reuse the precomputed index.
    [[nodiscard]] bool depth_test_relaxed(size_t idx, float depth) const noexcept
    {
        const uint64_t cur = m_pixel[idx].load(std::memory_order_relaxed);
        return depth < unpack_depth(cur);
    }

    // Transparent passes treat the opaque depth half as immutable.
    [[nodiscard]] float depth_at(size_t idx) const noexcept
    {
        return unpack_depth(m_pixel[idx].load(std::memory_order_relaxed));
    }

    // Resolve workers own disjoint indices; the color store preserves opaque depth.
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

    // Unconditional tiled-path store; never race it with commit_pixel on the same slot.
    void set_pixel_at(size_t idx, float depth, Color color) noexcept
    {
        m_pixel[idx].store(pack_pixel(depth, pack_color(color)), std::memory_order_relaxed);
    }

    // Atomically replace the slot only while this depth still wins.
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

    // Borrow the renderer's idle pool for presentation; unset means serial.
    struct ParallelRunner
    {
        std::function<void(const std::function<void(int, int)> &)> run;
        int n_workers = 1;

        [[nodiscard]] bool usable() const noexcept { return n_workers > 1 && static_cast<bool>(run); }
    };

    void set_parallel_runner(ParallelRunner runner) { m_par = std::move(runner); }

    // Set the one-line HUD. SGR is allowed; newlines and cursor movement are not.
    void set_hud(std::string text) { m_hud = std::move(text); }

    // Flush the pixel buffer, retrying short writes until complete or interrupted.
    void present();

  private:
    // Compile-time color mode keeps the blocks inner loop branch-free.
    template <bool TC> void present_impl();

    // present() body for the kitty backend: one image transmission (skipped when
    // the pixel buffer was not rewritten since the last present) plus the HUD row.
    void present_kitty();

    // present() body for the sixel backend: quantize the pixel buffer to the
    // xterm-240 palette (m_idx) and emit one full sixel frame, gated like kitty.
    void present_sixel();

    // Compose and flush inside synchronized-output markers.
    void begin_frame();
    void end_frame();
    bool recover_terminal_output() noexcept;

    // Shared HUD-row emission; erase before writing shorter replacement text.
    void append_hud_line(bool full_redraw);

    // \033[row;colH, 1-based cursor position (no reliance on newlines or
    // auto-wrap); shared by present_impl's row positioning and the HUD block.
    void append_cursor_pos(int row, int col);

    // Serialize packed colors as row-major RGB for kitty f=24.
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

    // Return one zlib stream, or zero to request raw transmission.
    size_t deflate_frame(size_t len);
    size_t deflate_frame_parallel(size_t len, int chunks);

    // Quantize the frame's colours into m_idx, the sixel encoder's input plane.
    void quantize_to_palette(size_t npx);
    [[nodiscard]] const unsigned char *idx_plane() const noexcept { return m_idx.get(); }

    // Append one sixel frame to m_buf, split across the borrowed pool when there is one.
    void encode_sixel_frame();

    // Slot: float depth bits in the high half, 0x00BBGGRR in the low half.
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

    // std::fill cannot assign non-copyable atomics.
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

    // Reserve worst-case cell output for blocks. Pixel transports grow their own
    // frame buffers and need only HUD and escape-sequence slack here.
    [[nodiscard]] size_t buf_reserve_bytes() const
    {
        if (m_gfx.backend == GraphicsBackend::Kitty)
        {
            return (static_cast<size_t>(m_gfx.cols) * 50u) + 4096u;
        }
        if (m_gfx.backend == GraphicsBackend::Sixel)
        {
            // Sixel has no useful worst-case bound. Let early frames grow the
            // buffer, whose capacity persists until resize.
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
    // Previous block colours: packed RGB or palette indices according to the
    // construction-time mode. Pixel backends do not use per-cell diffs.
    std::vector<uint32_t> m_prev_color;
    // Kitty direct-transport staging. Raw arrays avoid vector::resize zeroing
    // memory that write_rgb and mz_compress2 immediately overwrite.
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
    // Workers that completed the latest split. Shared by staging fill, palette
    // quantization, and sixel encoding, which never overlap.
    std::vector<uint8_t> m_par_covered;
    // Sixel staging: the frame quantized to xterm-256 palette indices, the
    // emitter's input plane. Same raw-array rationale as m_rgb/m_z above.
    std::unique_ptr<unsigned char[]> m_idx;
    size_t m_idx_cap = 0;
    // The sixel encoder's caller-owned band masks (grow-only, dirty between
    // frames by contract; see sixel::Scratch).
    sixel::Scratch m_sixel_scratch;
    // Parallel sixel encode: one output buffer and one Scratch per worker because
    // staging is not shareable. Allocate only for a split and retain capacity.
    std::vector<std::string> m_sixel_parts;
    std::vector<sixel::Scratch> m_sixel_par_scratch;
    std::string m_buf;      // reused output buffer, avoids per-frame allocation
    std::string m_hud;      // status line written below pixel rows
    std::string m_prev_hud; // last HUD actually emitted; present() skips an unchanged one
    bool m_force_redraw = true;
    // Defer resize erasure into the next synchronized frame to avoid a blank flash.
    bool m_pending_clear = false;
    bool m_output_recovery_pending = false;
    bool m_headless = false;
    bool m_terminal_active = false;
    bool m_adopted_alt_screen = false;
    // Suspension stops rendering even if terminal release must be retried at teardown.
    bool m_terminal_release_pending = false;
    bool m_mouse_cleanup_pending = false;
    ColorMode m_mode = ColorMode::TrueColor;
    GraphicsConfig m_gfx;
    WriteFrameFn m_write_frame;
    // clear() arms image emission; hot-path pixel writes deliberately do not.
    bool m_image_dirty = true;
    // Whether a transmitted image is resident in the terminal; lets a
    // shrink to zero image rows delete the now-orphaned frame exactly once.
    bool m_image_shown = false;
    // Rotate shm names so an unread frame cannot be overwritten.
    unsigned m_shm_seq = 0;
    ParallelRunner m_par;
};
