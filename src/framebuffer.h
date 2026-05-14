#pragma once

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

constexpr bool operator==(Color a, Color b) noexcept { return a.r == b.r && a.g == b.g && a.b == b.b; }
constexpr bool operator!=(Color a, Color b) noexcept { return !(a == b); }

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

    void clear(Color bg = {0, 0, 0});

    // Returns true if depth test passes, and writes the new depth value.
    [[nodiscard]] inline bool test_and_set_depth(int x, int y, float depth)
    {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height)
            return false;
        float &d = m_depth[pixel_idx(x, y)];
        if (depth < d)
        {
            d = depth;
            return true;
        }
        return false;
    }

    inline void set_pixel(int x, int y, Color color)
    {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height)
            return;
        m_color[pixel_idx(x, y)] = color;
    }

    [[nodiscard]] inline Color get_pixel(int x, int y) const
    {
        if (x < 0 || x >= m_width || y < 0 || y >= m_height)
            return {};
        return m_color[pixel_idx(x, y)];
    }

    // Unchecked variants — caller guarantees 0 <= x < width, 0 <= y < height.
    // Used by the rasterizer inner loop where setup_tri already clamps bounds.
    [[nodiscard]] inline bool unchecked_test_and_set_depth(int x, int y, float depth)
    {
        float &d = m_depth[pixel_idx(x, y)];
        if (depth < d)
        {
            d = depth;
            return true;
        }
        return false;
    }

    inline void unchecked_set_pixel(int x, int y, Color color)
    {
        m_color[pixel_idx(x, y)] = color;
    }

    // Set a one-line status string rendered below the pixel rows each frame.
    // Call before present(). Pass an empty string to clear.
    void set_hud(const std::string &text) { m_hud = text; }

    // Flush the pixel buffer to the terminal as a single write.
    void present();

private:
    [[nodiscard]] size_t pixel_idx(int x, int y) const noexcept
    {
        return static_cast<size_t>(y) * static_cast<size_t>(m_width) + static_cast<size_t>(x);
    }

    int m_width, m_height;
    std::vector<Color> m_color;
    std::vector<Color> m_prev_color;
    std::vector<float> m_depth;
    std::string m_buf; // reused output buffer, avoids per-frame allocation
    std::string m_hud; // status line written below pixel rows
    bool m_force_redraw = true;
    bool m_headless = false;
};
