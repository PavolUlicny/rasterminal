#include "src/bench.h"
#include "tests/test.h"

#include "src/args.h"
#include "src/terminal/framebuffer.h"
#include "src/terminal/graphics.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

TEST(bench, summary_of_unordered_frames)
{
    std::vector<int64_t> frames = { 5'000'000, 1'000'000, 3'000'000, 2'000'000, 4'000'000 };
    const BenchStats stats = summarize_frames(frames);
    ASSERT_EQ(stats.min_ns, int64_t{ 1'000'000 });
    ASSERT_EQ(stats.max_ns, int64_t{ 5'000'000 });
    ASSERT_EQ(stats.median_ns, int64_t{ 3'000'000 });
    // Index 5 * 95 / 100 = 4 is the largest of five samples.
    ASSERT_EQ(stats.p95_ns, int64_t{ 5'000'000 });
    // Population standard deviation of 1..5 ms is sqrt(2) ms.
    ASSERT_NEAR(stats.stddev_ms, 1.41421356, 1e-6);
}

// The p95 index is n * 95 / 100, so 100 samples report the 96th smallest.
TEST(bench, p95_of_one_hundred_frames)
{
    std::vector<int64_t> frames;
    for (int64_t i = 100; i >= 1; i--)
    {
        frames.push_back(i);
    }
    const BenchStats stats = summarize_frames(frames);
    ASSERT_EQ(stats.median_ns, int64_t{ 51 });
    ASSERT_EQ(stats.p95_ns, int64_t{ 96 });
}

TEST(bench, single_frame_has_zero_spread)
{
    std::vector<int64_t> frames = { 7 };
    const BenchStats stats = summarize_frames(frames);
    ASSERT_EQ(stats.min_ns, int64_t{ 7 });
    ASSERT_EQ(stats.median_ns, int64_t{ 7 });
    ASSERT_EQ(stats.p95_ns, int64_t{ 7 });
    ASSERT_EQ(stats.max_ns, int64_t{ 7 });
    ASSERT_NEAR(stats.stddev_ms, 0.0, 0.0);
}

TEST(bench, without_bench_graphics_measures_rendering_only)
{
    ParsedArgs args;
    ASSERT_FALSE(bench_graphics(args).has_value());
    // The viewer's backend choice does not switch the bench's presentation on.
    args.graphics = GraphicsChoice::Kitty;
    ASSERT_FALSE(bench_graphics(args).has_value());
}

TEST(bench, explicit_graphics_selects_its_backend)
{
    ParsedArgs args;
    args.bench_width = 400;
    args.bench_height = 240;
    const std::pair<GraphicsChoice, GraphicsBackend> cases[] = {
        { GraphicsChoice::Kitty, GraphicsBackend::Kitty },
        { GraphicsChoice::Sixel, GraphicsBackend::Sixel },
        { GraphicsChoice::Blocks, GraphicsBackend::Blocks },
    };
    for (const auto &[choice, backend] : cases)
    {
        args.bench_graphics = choice;
        const std::optional<GraphicsConfig> gfx = bench_graphics(args);
        ASSERT_TRUE(gfx.has_value());
        ASSERT_TRUE(gfx->backend == backend);
        // Remote sessions cannot use shared memory, so the bench never does.
        ASSERT_FALSE(gfx->shm);
        ASSERT_TRUE(gfx->cols >= 1);
        ASSERT_TRUE(gfx->rows >= 1);
    }
}

TEST(bench, tiny_framebuffer_still_has_one_cell)
{
    ParsedArgs args;
    args.bench_graphics = GraphicsChoice::Kitty;
    args.bench_width = 1;
    args.bench_height = 1;
    const std::optional<GraphicsConfig> gfx = bench_graphics(args);
    ASSERT_TRUE(gfx.has_value());
    ASSERT_EQ(gfx->cols, 1);
    ASSERT_EQ(gfx->rows, 1);
}

TEST(bench, sixel_colors_pass_through_within_register_limit)
{
    ParsedArgs args;
    args.bench_graphics = GraphicsChoice::Sixel;
    args.sixel_colors = { 16, 16 };
    const std::optional<GraphicsConfig> gfx = bench_graphics(args);
    ASSERT_TRUE(gfx.has_value());
    ASSERT_EQ(gfx->sixel_colors.min, 16);
    ASSERT_EQ(gfx->sixel_colors.max, 16);
}

TEST(bench, present_samples_keep_sent_frames)
{
    PresentSamples samples;
    Framebuffer::PresentStats sent;
    sent.encode_ns = 5'000'000;
    sent.bytes = 1'200'000;
    samples.add(sent);
    sent.encode_ns = 7'000'000;
    sent.bytes = 900'000;
    samples.add(sent);
    ASSERT_EQ(samples.encode_ns.size(), size_t{ 2 });
    ASSERT_EQ(samples.encode_ns[1], int64_t{ 7'000'000 });
    ASSERT_EQ(samples.bytes[0], int64_t{ 1'200'000 });
    ASSERT_EQ(samples.empty, 0);
}

TEST(bench, present_samples_count_empty_frames_apart)
{
    PresentSamples samples;
    samples.add(Framebuffer::PresentStats{}); // nothing composed
    Framebuffer::PresentStats sent;
    sent.encode_ns = 1;
    sent.bytes = 10;
    samples.add(sent);
    samples.add(Framebuffer::PresentStats{});
    // Empty frames never enter the medians.
    ASSERT_EQ(samples.encode_ns.size(), size_t{ 1 });
    ASSERT_EQ(samples.bytes.size(), size_t{ 1 });
    ASSERT_EQ(samples.empty, 2);
}
