#include "src/terminal/geometry.h"
#include "src/terminal/graphics.h"

#include <algorithm>

namespace terminal_geometry
{

    int image_rows_for(GraphicsBackend backend, int rows, int hud_rows) noexcept
    {
        const int reserved = (backend == GraphicsBackend::Sixel) ? 1 : hud_rows;
        return rows - reserved;
    }

    FbSize pixel_fb_size(
        GraphicsBackend backend, int cols, int image_rows, int cell_w, int cell_h, const SixelBounds &lim
    ) noexcept
    {
        if (backend == GraphicsBackend::Sixel)
        {
            cell_w = (lim.max_cell_w > 0) ? std::min(cell_w, lim.max_cell_w) : cell_w;
            cell_h = (lim.max_cell_h > 0) ? std::min(cell_h, lim.max_cell_h) : cell_h;
        }
        int w = cols * cell_w;
        int h = image_rows * cell_h;
        if (backend == GraphicsBackend::Sixel)
        {
            w = (lim.max_img_w > 0) ? std::min(w, lim.max_img_w) : w;
            h = (lim.max_img_h > 0) ? std::min(h, lim.max_img_h) : h;
        }
        const int longest = std::max(w, h);
        if (longest > MAX_FB_DIM_PX)
        {
            const int sw = static_cast<int>(static_cast<long long>(w) * MAX_FB_DIM_PX / longest);
            const int sh = static_cast<int>(static_cast<long long>(h) * MAX_FB_DIM_PX / longest);
            // A nonzero axis stays nonzero: rounding the short axis to zero would
            // blank the image where a 1 px sliver still renders. A legitimately
            // zero axis (one-row terminal, HUD shown) stays zero.
            w = (w > 0) ? std::max(1, sw) : 0;
            h = (h > 0) ? std::max(1, sh) : 0;
        }
        if (backend == GraphicsBackend::Sixel)
        {
            // Whole sixel bands avoid terminals rounding a partial band into a scroll.
            h -= h % 6;
            if (lim.cell_trusted && w > 0 && h > 0)
            {
                // Center only with a trusted cell size; a guessed size could move the
                // image into the reserved row or past the right edge.
                const int used_cols = (w + cell_w - 1) / cell_w;
                const int used_rows = (h + cell_h - 1) / cell_h;
                return { w, h, 1 + ((cols - used_cols) / 2), 1 + ((image_rows - used_rows) / 2) };
            }
        }
        return { w, h };
    }

    TerminalGeometry::TerminalGeometry(
        GraphicsBackend backend,
        int hud_rows,
        ExactCellSize exact_cell,
        SixelMaxSize sixel_max,
        const Observation &initial
    ) noexcept
        : backend_(backend), hud_rows_(hud_rows), cols_(initial.cols), rows_(initial.rows), cell_w_(exact_cell.w),
          cell_h_(exact_cell.h), sixel_geom_w_(sixel_max.w), sixel_geom_h_(sixel_max.h)
    {
        if (pixel_backend())
        {
            // Exact query, ioctl-derived size, then a guess. A guess cannot
            // establish a safe sixel centering origin.
            have_pixel_report_ = initial.has_pixel_report;
            if (have_pixel_report_)
            {
                ioctl_cell_w_ = initial.pixel_cell_w;
                ioctl_cell_h_ = initial.pixel_cell_h;
            }
            if (cell_w_ <= 0 || cell_h_ <= 0)
            {
                cell_w_ = ioctl_cell_w_;
                cell_h_ = ioctl_cell_h_;
            }
            if (cell_w_ <= 0 || cell_h_ <= 0)
            {
                cell_w_ = 8;
                cell_h_ = 16;
                cell_guessed_ = true;
            }
        }
    }

    FbSize TerminalGeometry::framebuffer_size() const noexcept
    {
        if (!pixel_backend())
        {
            return { cols_, (rows_ - hud_rows_) * 2 };
        }
        // Only a live pixel report can bound an image. Retained ioctl values
        // track adoption and may describe an earlier window size.
        return pixel_fb_size(
            backend_, cols_, image_rows(), cell_w_, cell_h_,
            { have_pixel_report_ ? ioctl_cell_w_ : 0, have_pixel_report_ ? ioctl_cell_h_ : 0, sixel_geom_w_,
              sixel_geom_h_, !cell_guessed_ }
        );
    }

    void TerminalGeometry::accept_cell_size(int width, int height) noexcept
    {
        if (pixel_backend())
        {
            cell_w_ = width;
            cell_h_ = height;
            cell_guessed_ = false;
        }
    }

    void TerminalGeometry::accept_sixel_geometry(int width, int height) noexcept
    {
        if (backend_ == GraphicsBackend::Sixel)
        {
            sixel_geom_w_ = width;
            sixel_geom_h_ = height;
        }
    }

    Requests TerminalGeometry::after_resume() noexcept
    {
        cols_ = 0;
        rows_ = 0;
        return { pixel_backend(), backend_ == GraphicsBackend::Sixel };
    }

    Update TerminalGeometry::observe(const Observation &observation, const FbSize &presented) noexcept
    {
        int next_cell_w = cell_w_;
        int next_cell_h = cell_h_;
        have_pixel_report_ = pixel_backend() && observation.has_pixel_report;
        Update update;
        if (have_pixel_report_ &&
            (observation.pixel_cell_w != ioctl_cell_w_ || observation.pixel_cell_h != ioctl_cell_h_))
        {
            // A stable disagreement with an exact reply is not a resize.
            // Padding can inflate the quotient, so re-query after a change.
            ioctl_cell_w_ = observation.pixel_cell_w;
            ioctl_cell_h_ = observation.pixel_cell_h;
            next_cell_w = ioctl_cell_w_;
            next_cell_h = ioctl_cell_h_;
            cell_guessed_ = false;
            update.cell_size_before_resize = true;
        }
        const bool grid_changed = observation.cols != cols_ || observation.rows != rows_;
        cols_ = observation.cols;
        rows_ = observation.rows;
        cell_w_ = next_cell_w;
        cell_h_ = next_cell_h;
        update.size = framebuffer_size();
        // Font zoom can change sixel placement without changing image size.
        const bool size_changed = pixel_backend() && (update.size.w != presented.w || update.size.h != presented.h ||
                                                      update.size.origin_col != presented.origin_col ||
                                                      update.size.origin_row != presented.origin_row);
        update.resize = grid_changed || size_changed;
        if (update.resize && pixel_backend())
        {
            // Without ioctl pixels, refresh the cell size after a grid change.
            // Terminals that never answer keep the startup value.
            update.after_resize.cell_size = grid_changed && !have_pixel_report_;
            // xterm and foot report sixel limits tied to the window size.
            update.after_resize.sixel_geometry =
                grid_changed && backend_ == GraphicsBackend::Sixel && sixel_geom_w_ > 0;
        }
        return update;
    }

} // namespace terminal_geometry
