#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

struct Color
{
    uint8_t r, g, b;
    Color() : r(0), g(0), b(0) {}
    Color(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}
};

class Framebuffer
{
public:
    // pixel_width  = terminal columns
    // pixel_height = terminal rows * 2  (two pixels per cell via ▀)
    Framebuffer(int pixel_width, int pixel_height);
    ~Framebuffer();

    int width() const { return m_width; }
    int height() const { return m_height; }

    // Resize pixel buffer to new dimensions and clear. Emits a one-shot
    // \033[2J so any leftover content from the old (larger) size is wiped.
    void resize(int pixel_width, int pixel_height);

    void clear(Color bg = {0, 0, 0});

    // Returns true if depth test passes, and writes the new depth value.
    bool test_and_set_depth(int x, int y, float depth);

    void set_pixel(int x, int y, Color color);

    // Set a one-line status string rendered below the pixel rows each frame.
    // Call before present(). Pass an empty string to clear.
    void set_hud(const std::string &text) { m_hud = text; }

    // Flush the pixel buffer to the terminal as a single write.
    void present();

private:
    int m_width, m_height;
    std::vector<Color> m_color;
    std::vector<float> m_depth;
    std::string m_buf; // reused output buffer, avoids per-frame allocation
    std::string m_hud; // status line written below pixel rows
};
