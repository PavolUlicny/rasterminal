#include "framebuffer.h"
#include "platform.h"

int main()
{
    platform::enable_raw_mode();

    int cols, rows;
    platform::get_terminal_size(cols, rows);

    // pixel height = terminal rows * 2 (two pixels per cell)
    Framebuffer fb(cols, rows * 2);

    // Smoke test: fill with a gradient and hold until 'q' or ESC.
    const int w = fb.width();
    const int h = fb.height();

    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            float u = static_cast<float>(x) / (w - 1);
            float v = static_cast<float>(y) / (h - 1);

            Color c(
                static_cast<uint8_t>(255 * u),
                static_cast<uint8_t>(255 * v),
                static_cast<uint8_t>(180));
            fb.set_pixel(x, y, c);
        }
    }

    fb.present();

    // Wait for quit key.
    while (true)
    {
        platform::Key k = platform::poll_key();
        if (k == platform::KEY_Q || k == platform::KEY_ESCAPE)
            break;
    }

    platform::disable_raw_mode();
    return 0;
}
