#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

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

class Framebuffer
{
  public:
    // pixel_width  = terminal columns
    // pixel_height = terminal rows * 2  (two pixels per cell via ▀)
    // headless     = true skips all terminal I/O (ANSI escapes, buffer reserve)
    Framebuffer(int pixel_width, int pixel_height, bool headless = false);
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

    // Returns true if depth test passes, and writes the new depth value.
    // Single-threaded callers (draw_line, shadow map): plain load+store, no CAS.
    [[nodiscard]] bool test_and_set_depth(int x, int y, float depth)
    {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height)
            return false;
        auto &d = m_depth[pixel_idx(x, y)];
        const float old = d.load(std::memory_order_relaxed);
        if (depth < old)
        {
            d.store(depth, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    void set_pixel(int x, int y, Color color)
    {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height)
            return;
        m_color[pixel_idx(x, y)].store(pack_color(color), std::memory_order_relaxed);
    }

    [[nodiscard]] Color get_pixel(int x, int y) const
    {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height)
            return {};
        const uint32_t v = m_color[pixel_idx(x, y)].load(std::memory_order_relaxed);
        return { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8u), static_cast<uint8_t>(v >> 16u) };
    }

    // Unchecked variant — caller guarantees 0 <= x < width, 0 <= y < height.
    // Used by the rasterizer inner loop where setup_tri already clamps bounds.
    // CAS loop: safe for concurrent writes from multiple worker threads.
    // Depth-wins invariant (min depth wins) is preserved atomically; the color
    // write that follows a successful CAS is a relaxed atomic store — no UB, but
    // two CAS winners on the same pixel produce non-deterministic colour ordering.
    [[nodiscard]] bool unchecked_test_and_set_depth(int x, int y, float depth)
    {
        auto &d = m_depth[pixel_idx(x, y)];
        float old = d.load(std::memory_order_relaxed);
        while (depth < old)
            if (d.compare_exchange_weak(old, depth, std::memory_order_relaxed, std::memory_order_relaxed))
                return true;
        return false;
    }

    void unchecked_set_pixel(int x, int y, Color color)
    {
        m_color[pixel_idx(x, y)].store(pack_color(color), std::memory_order_relaxed);
    }

    // Set a one-line status string rendered below the pixel rows each frame.
    // Call before present(). Pass an empty string to clear.
    void set_hud(const std::string &text) { m_hud = text; }

    // Flush the pixel buffer to the terminal as a single write.
    void present();

  private:
    static uint32_t pack_color(Color c) noexcept
    {
        return static_cast<uint32_t>(c.r) | (static_cast<uint32_t>(c.g) << 8u) | (static_cast<uint32_t>(c.b) << 16u);
    }

    // Fills every depth entry with infinity using relaxed atomic stores.
    // Must be a loop — std::fill doesn't work on non-copyable atomic<float>.
    void fill_depth_infinity() noexcept
    {
        const float inf = std::numeric_limits<float>::infinity();
        for (auto &d : m_depth)
            d.store(inf, std::memory_order_relaxed);
    }

    [[nodiscard]] size_t pixel_idx(int x, int y) const noexcept
    {
        return static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x);
    }

    int m_width, m_height;
    std::vector<std::atomic<uint32_t>> m_color;
    std::vector<std::atomic<uint32_t>> m_prev_color;
    std::vector<std::atomic<float>> m_depth;
    std::string m_buf; // reused output buffer, avoids per-frame allocation
    std::string m_hud; // status line written below pixel rows
    bool m_force_redraw = true;
    bool m_headless = false;
};
