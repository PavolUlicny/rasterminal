#pragma once

#include "color.h"  // Color, ColorMode (re-exported: every includer of this header sees them)
#include "linalg.h" // vec3 (for vec3_to_color)

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// Soft-knee highlight rolloff: the display transform that maps the renderer's HDR
// shading output into the displayable range. Identity below `knee`; above it values
// bend smoothly toward (but never reach) 1.0, slope-continuous at the knee so there is
// no visible crease. Lighting accumulates past 1.0, and a hard clamp would collapse
// every over-range value to flat white, erasing the shading gradient on bright/untextured
// surfaces; the rolloff keeps the gradient instead.
//
// Applied exactly once per shaded contribution: at the opaque commit and at the transparent
// A-buffer push (both in rasterize.cpp). The transparent resolve therefore composites values
// that are already display-referred and must NOT tonemap again (doing so would double-darken
// the opaque base under glass). The unlit path is excluded (its output is bounded [0,1] and is
// meant to be faithful), as is UI chrome (wireframe/HUD/background), which is authored in
// display space and bypasses vec3_to_color entirely.
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

class Framebuffer
{
  public:
    // pixel_width  = terminal columns
    // pixel_height = terminal rows * 2  (two pixels per cell via ▀)
    // headless     = true skips all terminal I/O (ANSI escapes, buffer reserve)
    // mode         = terminal colour depth for present() (default 24-bit truecolor)
    Framebuffer(int pixel_width, int pixel_height, bool headless = false, ColorMode mode = ColorMode::TrueColor);
    ~Framebuffer();

    Framebuffer(const Framebuffer &) = delete;
    Framebuffer &operator=(const Framebuffer &) = delete;
    Framebuffer(Framebuffer &&) = delete;
    Framebuffer &operator=(Framebuffer &&) = delete;

    [[nodiscard]] int width() const { return m_width; }
    [[nodiscard]] int height() const { return m_height; }

    // Resize pixel buffer to new dimensions and clear. Emits a one-shot
    // \033[2J so any leftover content from the old (larger) size is wiped.
    void resize(int pixel_width, int pixel_height);

    void clear(Color bg = { 0, 0, 0 });

    [[nodiscard]] Color get_pixel(int x, int y) const
    {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height)
        {
            return {};
        }
        return unpack_color(unpack_color_bits(m_pixel[pixel_idx(x, y)].load(std::memory_order_relaxed)));
    }

    // Bounds-checked, read-only (x,y) depth probe — the get_pixel analog for depth. Returns
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
    // y*width+x across the shading work — register pressure spills it otherwise.
    [[nodiscard]] bool depth_test_relaxed(size_t idx, float depth) const noexcept
    {
        const uint64_t cur = m_pixel[idx].load(std::memory_order_relaxed);
        return depth < unpack_depth(cur);
    }

    // Read the depth half of a slot without touching colour. Used by the transparent
    // pass to cull fragments behind opaque geometry with a <= test (it never writes
    // depth). INVARIANT: across the transparent accumulate + resolve passes the depth
    // half is immutable — the opaque pass set it and nothing after writes it — so this
    // read and commit_pixel/get_pixel stay consistent. Do not add a depth write to the
    // resolve path without revisiting this.
    [[nodiscard]] float depth_at(size_t idx) const noexcept
    {
        return unpack_depth(m_pixel[idx].load(std::memory_order_relaxed));
    }

    // idx-based colour read/write for the transparent resolve, peers to depth_at(idx):
    // the linear index is already in hand there, so these skip the (x,y) pixel_idx
    // recompute and bounds branch that get_pixel pays. Single-threaded contract — safe in
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

    // Atomically replaces (depth, color) iff our depth still wins against whatever's
    // in the slot now. Returns false if a concurrent thread became shallower between
    // our depth_test_relaxed() and this call — our fragment is then dropped.
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

    // Set a one-line status string rendered below the pixel rows each frame. Call before
    // present(). Pass an empty string to clear. By value so a composed line (the usual caller)
    // moves in rather than being copied every frame.
    //
    // The text may carry its own SGR colour escapes, as compose_hud's does; present() sets the
    // bar background and a default foreground first, so a plain unstyled string is drawn
    // legibly too. It must not contain a newline or cursor movement: the row is written with
    // auto-wrap off and nothing re-positions the cursor afterwards.
    void set_hud(std::string text) { m_hud = std::move(text); }

    // Flush the pixel buffer to the terminal as a single write.
    void present();

  private:
    // present() body, specialized per colour mode so the truecolor path carries no runtime
    // per-cell branch and stays byte-identical to the historical output. TC == true selects the
    // 24-bit 38;2/48;2 emission; TC == false selects the quantized 38;5/48;5 palette emission.
    template <bool TC> void present_impl();

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
    // Must be a loop — std::fill doesn't work on non-copyable atomic<uint64_t>.
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

    // Upper bound on present()'s output-buffer size, used to preallocate m_buf. The per-cell worst case
    // is one combined fg+bg SGR plus the glyph: ~39 B in TrueColor (38;2;r;g;b;48;2;r;g;bm) but only
    // ~23 B in Palette256 (38;5;i;48;5;jm), so 256 mode reserves less. Both constants keep headroom over
    // the worst case for per-row cursor moves and the HUD tail; a miss only costs a one-time realloc.
    [[nodiscard]] size_t buf_reserve_bytes() const noexcept
    {
        const size_t per_cell = (m_mode == ColorMode::TrueColor) ? 50u : 32u;
        return static_cast<size_t>(m_width) * static_cast<size_t>(m_height / 2) * per_cell;
    }

    int m_width, m_height;
    std::vector<std::atomic<uint64_t>> m_pixel;
    // plain: only read/written by single-threaded present(). Value domain is mode-dependent: packed
    // RGB in TrueColor, palette indices in Palette256 (safe because m_mode is fixed at construction; a
    // future runtime mode switch would need m_force_redraw to avoid stale index-vs-RGB comparisons).
    std::vector<uint32_t> m_prev_color;
    std::string m_buf;      // reused output buffer, avoids per-frame allocation
    std::string m_hud;      // status line written below pixel rows
    std::string m_prev_hud; // last HUD actually emitted; present() skips an unchanged one
    bool m_force_redraw = true;
    bool m_headless = false;
    ColorMode m_mode = ColorMode::TrueColor;
};
