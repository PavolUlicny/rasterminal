#pragma once

#include "src/terminal/graphics.h"

namespace terminal_geometry
{
    // Bound hostile grid-by-cell products and scale both axes to preserve aspect.
    constexpr int MAX_FB_DIM_PX = 8192;

    struct FbSize
    {
        int w = 0;
        int h = 0;
        // 1-based sixel origin; kitty and blocks stay at (1, 1).
        int origin_col = 1;
        int origin_row = 1;
    };

    // Sixel always reserves the last row because a bottom-touching image scrolls.
    [[nodiscard]] int image_rows_for(GraphicsBackend backend, int rows, int hud_rows) noexcept;

    // Terminal-reported sixel cell and image limits; zero means unknown.
    // xterm discards, rather than clips, an image beyond either axis limit.
    struct SixelBounds
    {
        int max_cell_w = 0;
        int max_cell_h = 0;
        int max_img_w = 0;
        int max_img_h = 0;
        bool cell_trusted = false;
    };

    // Size native-resolution image backends. Sixel caps axes independently and
    // letterboxes because it paints 1:1; kitty stretches to its cell rectangle.
    [[nodiscard]] FbSize pixel_fb_size(
        GraphicsBackend backend, int cols, int image_rows, int cell_w, int cell_h, const SixelBounds &lim
    ) noexcept;

    struct Observation
    {
        int cols = 0;
        int rows = 0;
        bool has_pixel_report = false;
        int pixel_cell_w = 0;
        int pixel_cell_h = 0;
    };

    struct Requests
    {
        bool cell_size = false;
        bool sixel_geometry = false;
    };

    struct Update
    {
        FbSize size;
        bool resize = false;
        bool cell_size_before_resize = false;
        Requests after_resize;
    };

    // Startup query replies; zero means unreported. The explicit constructors
    // reject bare braces, so a swapped cell size and sixel limit cannot compile.
    struct ExactCellSize
    {
        explicit ExactCellSize(int width, int height) noexcept : w(width), h(height) {}
        int w;
        int h;
    };

    struct SixelMaxSize
    {
        explicit SixelMaxSize(int width, int height) noexcept : w(width), h(height) {}
        int w;
        int h;
    };

    class TerminalGeometry
    {
      public:
        TerminalGeometry(
            GraphicsBackend backend,
            int hud_rows,
            ExactCellSize exact_cell,
            SixelMaxSize sixel_max,
            const Observation &initial
        ) noexcept;

        [[nodiscard]] bool pixel_backend() const noexcept { return backend_ != GraphicsBackend::Blocks; }
        [[nodiscard]] int cols() const noexcept { return cols_; }
        [[nodiscard]] int rows() const noexcept { return rows_; }
        [[nodiscard]] int image_rows() const noexcept { return image_rows_for(backend_, rows_, hud_rows_); }
        [[nodiscard]] FbSize framebuffer_size() const noexcept;

        void accept_cell_size(int width, int height) noexcept;
        void accept_sixel_geometry(int width, int height) noexcept;
        [[nodiscard]] Requests after_resume() noexcept;
        [[nodiscard]] Update observe(const Observation &observation, const FbSize &presented) noexcept;

      private:
        GraphicsBackend backend_;
        int hud_rows_;
        int cols_;
        int rows_;
        int cell_w_;
        int cell_h_;
        int ioctl_cell_w_ = 0;
        int ioctl_cell_h_ = 0;
        int sixel_geom_w_;
        int sixel_geom_h_;
        bool have_pixel_report_ = false;
        bool cell_guessed_ = false;
    };

} // namespace terminal_geometry
