#include "src/bench.h"
#include "tests/test.h"

#include <cstdint>
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
