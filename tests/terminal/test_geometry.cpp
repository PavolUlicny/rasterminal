#include "src/terminal/geometry.h"
#include "tests/test.h"

using terminal_geometry::FbSize;
using terminal_geometry::image_rows_for;
using terminal_geometry::pixel_fb_size;
using terminal_geometry::Requests;
using terminal_geometry::TerminalGeometry;
using terminal_geometry::Update;

TEST(geometry, sixel_reserves_bottom_row_even_without_hud)
{
    ASSERT_EQ(image_rows_for(GraphicsBackend::Sixel, 24, 0), 23);
    ASSERT_EQ(image_rows_for(GraphicsBackend::Sixel, 24, 1), 23);
    ASSERT_EQ(image_rows_for(GraphicsBackend::Kitty, 24, 1), 23);
    ASSERT_EQ(image_rows_for(GraphicsBackend::Kitty, 24, 0), 24);
}

TEST(geometry, sixel_bounds_and_trusted_cell_center_image)
{
    const auto size = pixel_fb_size(GraphicsBackend::Sixel, 80, 23, 10, 20, { 0, 0, 400, 300, true });
    ASSERT_EQ(size.w, 400);
    ASSERT_EQ(size.h, 300);
    ASSERT_EQ(size.origin_col, 21);
    ASSERT_EQ(size.origin_row, 5);
}

TEST(geometry, guessed_sixel_cell_keeps_origin_at_one)
{
    const auto size = pixel_fb_size(GraphicsBackend::Sixel, 80, 23, 10, 20, { 0, 0, 400, 300, false });
    ASSERT_EQ(size.origin_col, 1);
    ASSERT_EQ(size.origin_row, 1);
}

TEST(geometry, sixel_height_uses_complete_bands)
{
    const auto size = pixel_fb_size(GraphicsBackend::Sixel, 80, 23, 8, 16, {});
    ASSERT_EQ(size.w, 640);
    ASSERT_EQ(size.h, 366);
}

TEST(geometry, kitty_scales_large_frame_without_losing_short_axis)
{
    const auto size = pixel_fb_size(GraphicsBackend::Kitty, 1, 20000, 1, 10, {});
    ASSERT_EQ(size.w, 1);
    ASSERT_EQ(size.h, terminal_geometry::MAX_FB_DIM_PX);
}

TEST(geometry, missing_exact_reply_uses_ioctl_cell_size)
{
    TerminalGeometry geometry(GraphicsBackend::Kitty, 0, 0, 0, 0, 0, { 80, 24, true, 9, 18 });
    const FbSize size = geometry.framebuffer_size();
    ASSERT_EQ(size.w, 720);
    ASSERT_EQ(size.h, 432);
}

TEST(geometry, missing_cell_sources_guess_and_disable_centering)
{
    TerminalGeometry geometry(GraphicsBackend::Sixel, 0, 0, 0, 400, 300, { 80, 24 });
    const FbSize size = geometry.framebuffer_size();
    ASSERT_EQ(size.w, 400);
    ASSERT_EQ(size.h, 300);
    ASSERT_EQ(size.origin_col, 1);
    ASSERT_EQ(size.origin_row, 1);
}

TEST(geometry, exact_reply_trusts_guessed_cell_size)
{
    TerminalGeometry geometry(GraphicsBackend::Sixel, 0, 0, 0, 400, 300, { 80, 24 });
    const FbSize guessed = geometry.framebuffer_size();
    geometry.accept_cell_size(10, 20);
    const Update trusted = geometry.observe({ 80, 24 }, guessed);
    ASSERT_TRUE(trusted.resize);
    ASSERT_EQ(trusted.size.origin_col, 21);
    ASSERT_EQ(trusted.size.origin_row, 5);
}

TEST(geometry, changed_ioctl_size_trusts_guessed_cell_size)
{
    TerminalGeometry geometry(GraphicsBackend::Sixel, 0, 0, 0, 400, 300, { 80, 24 });
    const Update trusted = geometry.observe({ 80, 24, true, 10, 20 }, geometry.framebuffer_size());
    ASSERT_TRUE(trusted.cell_size_before_resize);
    ASSERT_TRUE(trusted.resize);
    ASSERT_EQ(trusted.size.origin_col, 21);
    ASSERT_EQ(trusted.size.origin_row, 5);
}

TEST(geometry, unknown_sixel_limit_is_not_requested_on_grid_change)
{
    TerminalGeometry geometry(GraphicsBackend::Sixel, 0, 10, 20, 0, 0, { 80, 24 });
    const Update changed = geometry.observe({ 81, 24 }, geometry.framebuffer_size());
    ASSERT_TRUE(changed.resize);
    ASSERT_TRUE(changed.after_resize.cell_size);
    ASSERT_FALSE(changed.after_resize.sixel_geometry);
}

TEST(geometry, stable_ioctl_disagreement_does_not_replace_exact_reply)
{
    TerminalGeometry geometry(GraphicsBackend::Kitty, 1, 10, 20, 0, 0, { 80, 24, true, 11, 20 });
    const FbSize initial = geometry.framebuffer_size();
    ASSERT_EQ(initial.w, 800);
    const Update unchanged = geometry.observe({ 80, 24, true, 11, 20 }, initial);
    ASSERT_FALSE(unchanged.resize);
    ASSERT_FALSE(unchanged.cell_size_before_resize);

    geometry.accept_cell_size(9, 20);
    const Update exact = geometry.observe({ 80, 24, true, 11, 20 }, initial);
    ASSERT_TRUE(exact.resize);
    ASSERT_EQ(exact.size.w, 720);
    ASSERT_FALSE(exact.cell_size_before_resize);
}

TEST(geometry, missing_pixel_report_drops_live_sixel_containment)
{
    TerminalGeometry geometry(GraphicsBackend::Sixel, 0, 10, 20, 0, 0, { 80, 24, true, 7, 14 });
    const FbSize bounded = geometry.framebuffer_size();
    ASSERT_EQ(bounded.w, 560);
    const Update missing = geometry.observe({ 80, 24, false, 0, 0 }, bounded);
    ASSERT_TRUE(missing.resize);
    ASSERT_EQ(missing.size.w, 800);
    ASSERT_EQ(missing.size.h, 456);
    ASSERT_FALSE(missing.cell_size_before_resize);
}

TEST(geometry, sixel_origin_change_resizes_with_same_dimensions)
{
    TerminalGeometry geometry(GraphicsBackend::Sixel, 0, 10, 20, 400, 300, { 80, 24 });
    const FbSize initial = geometry.framebuffer_size();
    geometry.accept_cell_size(8, 20);
    const Update moved = geometry.observe({ 80, 24 }, initial);
    ASSERT_TRUE(moved.resize);
    ASSERT_EQ(moved.size.w, initial.w);
    ASSERT_EQ(moved.size.h, initial.h);
    ASSERT_TRUE(moved.size.origin_col != initial.origin_col);
}

TEST(geometry, changed_ioctl_size_requests_exact_reply)
{
    TerminalGeometry geometry(GraphicsBackend::Kitty, 0, 10, 20, 0, 0, { 80, 24, true, 10, 20 });
    const Update changed = geometry.observe({ 80, 24, true, 12, 20 }, geometry.framebuffer_size());
    ASSERT_TRUE(changed.cell_size_before_resize);
    ASSERT_TRUE(changed.resize);
    ASSERT_EQ(changed.size.w, 960);
}

TEST(geometry, resume_requests_reports_and_forces_grid_reapplication)
{
    TerminalGeometry geometry(GraphicsBackend::Sixel, 0, 10, 20, 400, 300, { 80, 24 });
    const FbSize presented = geometry.framebuffer_size();
    const Requests resume = geometry.after_resume();
    ASSERT_TRUE(resume.cell_size);
    ASSERT_TRUE(resume.sixel_geometry);
    const Update restored = geometry.observe({ 80, 24 }, presented);
    ASSERT_TRUE(restored.resize);
    ASSERT_TRUE(restored.after_resize.cell_size);
    ASSERT_TRUE(restored.after_resize.sixel_geometry);
}

TEST(geometry, blocks_resize_only_on_grid_change)
{
    TerminalGeometry geometry(GraphicsBackend::Blocks, 1, 0, 0, 0, 0, { 80, 24 });
    const FbSize initial = geometry.framebuffer_size();
    ASSERT_FALSE(geometry.observe({ 80, 24 }, initial).resize);
    const Update changed = geometry.observe({ 81, 24 }, initial);
    ASSERT_TRUE(changed.resize);
    ASSERT_EQ(changed.size.w, 81);
    ASSERT_EQ(changed.size.h, 46);
    ASSERT_FALSE(changed.cell_size_before_resize);
    ASSERT_FALSE(changed.after_resize.cell_size);
    ASSERT_FALSE(geometry.observe({ 81, 24 }, changed.size).resize);
}

TEST(geometry, live_pixel_report_avoids_redundant_grid_query)
{
    TerminalGeometry geometry(GraphicsBackend::Kitty, 0, 10, 20, 0, 0, { 80, 24, true, 10, 20 });
    const Update changed = geometry.observe({ 81, 24, true, 10, 20 }, geometry.framebuffer_size());
    ASSERT_TRUE(changed.resize);
    ASSERT_FALSE(changed.cell_size_before_resize);
    ASSERT_FALSE(changed.after_resize.cell_size);
}

TEST(geometry, sixel_geometry_reply_changes_framebuffer_size)
{
    TerminalGeometry geometry(GraphicsBackend::Sixel, 0, 10, 20, 400, 300, { 80, 24 });
    const FbSize initial = geometry.framebuffer_size();
    geometry.accept_sixel_geometry(600, 360);
    const Update grown = geometry.observe({ 80, 24 }, initial);
    ASSERT_TRUE(grown.resize);
    ASSERT_EQ(grown.size.w, 600);
    ASSERT_EQ(grown.size.h, 360);
}
