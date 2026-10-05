#include "src/terminal/sixel_palette.h"

#include "src/args.h"
#include "src/terminal/cielab.h"
#include "src/terminal/color.h"
#include "src/terminal/sixel.h"
#include "src/viewer/scene_colors.h"
#include "tests/test.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

namespace
{
    using sixel::FittedPalette;

    constexpr sixel::ColorRange fixed(int size) noexcept
    {
        return { size, size };
    }

    constexpr uint32_t pack(Color c) noexcept
    {
        return static_cast<uint32_t>(c.r) | (static_cast<uint32_t>(c.g) << 8u) | (static_cast<uint32_t>(c.b) << 16u);
    }

    // Framebuffer-shaped pixels: colour in the low 24 bits, a depth-like value above it that
    // the palette must ignore.
    struct Frame
    {
        int w;
        int h;
        std::vector<std::atomic<uint64_t>> px;

        Frame(int width, int height, Color fill)
            : w(width), h(height), px(static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
        {
            fill_all(fill);
        }

        void fill_all(Color c)
        {
            for (auto &p : px)
            {
                p.store(0x3F80000000000000ull | pack(c), std::memory_order_relaxed);
            }
        }

        void set(int x, int y, Color c)
        {
            px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w)) + static_cast<std::size_t>(x)].store(
                0x3F80000000000000ull | pack(c), std::memory_order_relaxed
            );
        }

        [[nodiscard]] Color get(std::size_t i) const
        {
            const auto v = static_cast<uint32_t>(px[i].load(std::memory_order_relaxed));
            return { static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8u), static_cast<uint8_t>(v >> 16u) };
        }

        [[nodiscard]] std::size_t size() const { return px.size(); }
    };

    bool update(FittedPalette &p, const Frame &f, Color clear = {}, std::optional<Color> required = std::nullopt)
    {
        std::optional<uint32_t> req;
        if (required)
        {
            req = pack(*required);
        }
        return p.update(f.px.data(), f.w, f.h, pack(clear), req);
    }

    std::vector<unsigned char> map(const FittedPalette &p, const Frame &f)
    {
        std::vector<unsigned char> out(f.size());
        p.map_rows(f.px.data(), f.w, 0, f.h, out.data());
        return out;
    }

    // Map with one thread per row range, as the framebuffer's pool does.
    std::vector<unsigned char> map_threads(const FittedPalette &p, const Frame &f, int threads)
    {
        std::vector<unsigned char> out(f.size());
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; t++)
        {
            const int y0 = f.h * t / threads;
            const int y1 = f.h * (t + 1) / threads;
            workers.emplace_back([&p, &f, &out, y0, y1] { p.map_rows(f.px.data(), f.w, y0, y1, out.data()); });
        }
        for (auto &w : workers)
        {
            w.join();
        }
        return out;
    }

    float delta_e(Color x, Color y)
    {
        const Lab a = cielab_from_srgb8(x);
        const Lab b = cielab_from_srgb8(y);
        return std::sqrt(((a.L - b.L) * (a.L - b.L)) + ((a.a - b.a) * (a.a - b.a)) + ((a.b - b.b) * (a.b - b.b)));
    }

    bool has_entry(const FittedPalette &p, Color c)
    {
        const auto &e = p.entries();
        return std::find(e.begin(), e.end(), sixel::detail::percent_round_trip(c)) != e.end();
    }

    // The mapping rule written out per pixel without the table: the nearest entry and up to
    // 3 alternates from detail::table_entry, pinned cells holding their pin alone, and the
    // left neighbour's register kept when it is an alternate.
    std::vector<unsigned char> reference_map(const FittedPalette &p, const Frame &f, const std::vector<Color> &pins)
    {
        sixel::detail::LabEntries lab;
        lab.set(p.entries());
        std::vector<unsigned char> out(f.size());
        for (int y = 0; y < f.h; y++)
        {
            uint32_t prev = 256u;
            for (int x = 0; x < f.w; x++)
            {
                const std::size_t i =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(f.w)) + static_cast<std::size_t>(x);
                const Color c = f.get(i);
                const std::size_t cell = quant256_idx(c);
                uint32_t e = sixel::detail::table_entry(sixel::detail::cell_centre_lab(cell), lab);
                for (auto it = pins.rbegin(); it != pins.rend(); ++it)
                {
                    const auto &entries = p.entries();
                    const auto j = std::find(entries.begin(), entries.end(), sixel::detail::percent_round_trip(*it));
                    if (quant256_idx(*it) == cell && j != entries.end())
                    {
                        e = sixel::detail::pinned_entry(static_cast<int>(j - entries.begin()));
                    }
                }
                const bool keep = prev == ((e >> 8u) & 0xFFu) || prev == ((e >> 16u) & 0xFFu) || prev == (e >> 24u);
                prev = keep ? prev : (e & 0xFFu);
                out[i] = static_cast<unsigned char>(prev);
            }
        }
        return out;
    }

    // Deterministic pseudo-random colours, smooth enough to look like shading.
    Frame shaded_frame(int w, int h, unsigned seed)
    {
        Frame f(w, h, {});
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                seed = (seed * 1664525u) + 1013904223u;
                const int noise = static_cast<int>((seed >> 24u) & 15u);
                f.set(
                    x, y,
                    { static_cast<uint8_t>(((x * 255 / w) + noise) & 255), static_cast<uint8_t>((y * 255 / h) & 255),
                      static_cast<uint8_t>((((x + y) * 127 / (w + h)) + (noise * 2)) & 255) }
                );
            }
        }
        return f;
    }

    std::size_t sample_count(int w, int h)
    {
        std::vector<std::size_t> positions;
        (void)sixel::detail::sample_positions(w, h, positions);
        return positions.size();
    }

    // The colours the fit reads from a frame.
    std::vector<uint32_t> sample_of(const Frame &f)
    {
        std::vector<std::size_t> positions;
        (void)sixel::detail::sample_positions(f.w, f.h, positions);
        std::vector<uint32_t> out;
        sixel::detail::read_sample(f.px.data(), positions, out);
        return out;
    }
} // namespace

TEST(sixel_palette, sampler_never_exceeds_the_bound)
{
    // Stride transitions and strips, where a square-root stride alone overshoots.
    const std::array<std::array<int, 2>, 11> sizes = { { { 512, 255 },
                                                         { 512, 256 },
                                                         { 513, 255 },
                                                         { 4097, 31 },
                                                         { 100000, 1 },
                                                         { 1, 100000 },
                                                         { 640, 600 },
                                                         { 1920, 1080 },
                                                         { 3840, 2160 },
                                                         { 1, 1 },
                                                         { 7, 7 } } };
    for (const auto &s : sizes)
    {
        const std::size_t n = sample_count(s[0], s[1]);
        ASSERT_TRUE(n >= 1u);
        ASSERT_TRUE(n <= static_cast<std::size_t>(sixel::detail::MAX_SAMPLES));
    }
    ASSERT_EQ(sample_count(640, 600), 24000u);
    ASSERT_EQ(sample_count(1920, 1080), 32400u);
    ASSERT_EQ(sample_count(512, 256), 32768u);
    ASSERT_EQ(sample_count(7, 7), 49u);
}

// The palette outlives framebuffer resizes. Each step keeps the width, the height or the pixel
// count, and the pixel count never drops, so stale positions from an incomplete cache key stay
// in bounds but read the wrong pixels. A fresh fit's error depends only on the sample, so it
// matches a new palette's whether or not the old one refits.
TEST(sixel_palette, resize_samples_the_new_size)
{
    FittedPalette p(fixed(24));
    const std::array<std::array<int, 2>, 4> sizes = { { { 320, 200 }, { 640, 200 }, { 640, 600 }, { 600, 640 } } };
    unsigned seed = 1;
    for (const auto &size : sizes)
    {
        const Frame f = shaded_frame(size[0], size[1], seed++);
        update(p, f);
        FittedPalette fresh(fixed(24));
        update(fresh, f);
        ASSERT_TRUE(p.fit_error() == fresh.fit_error());
    }
}

TEST(sixel_palette, wu_gives_one_entry_per_distinct_cell)
{
    const std::array<Color, 5> colors = {
        { { 10, 20, 30 }, { 200, 40, 40 }, { 40, 200, 40 }, { 250, 250, 250 }, { 128, 128, 128 } }
    };
    sixel::detail::Histogram hist;
    for (std::size_t i = 0; i < 1000; i++)
    {
        hist.add(pack(colors[i % colors.size()]));
    }
    sixel::detail::Wu wu;
    wu.build(hist);
    wu.split_to(24);
    std::vector<std::array<float, 3>> means;
    wu.box_means(means);
    ASSERT_EQ(means.size(), colors.size());
    for (const auto &m : means)
    {
        const Color c{ static_cast<uint8_t>(std::lround(m[0])), static_cast<uint8_t>(std::lround(m[1])),
                       static_cast<uint8_t>(std::lround(m[2])) };
        ASSERT_TRUE(std::find(colors.begin(), colors.end(), c) != colors.end());
    }

    // Through the whole fit: each entry is one of the colours as the terminal shows it.
    Frame f(50, 20, colors[0]);
    for (std::size_t i = 0; i < f.size(); i++)
    {
        f.set(static_cast<int>(i % 50u), static_cast<int>(i / 50u), colors[i % colors.size()]);
    }
    FittedPalette p(fixed(24));
    update(p, f);
    ASSERT_EQ(p.entry_count(), 5);
    for (const Color c : colors)
    {
        ASSERT_TRUE(has_entry(p, c));
    }
}

TEST(sixel_palette, two_colour_frame_gives_two_entries_at_any_budget)
{
    Frame f(64, 64, { 0, 0, 0 });
    for (int x = 0; x < 64; x++)
    {
        f.set(x, 10, { 200, 200, 200 });
    }
    FittedPalette p(fixed(256));
    update(p, f);
    ASSERT_EQ(p.entry_count(), 2);
    ASSERT_EQ(p.budget(), 256);
}

TEST(sixel_palette, colours_sharing_a_cell_merge)
{
    // A documented limit: the histogram cannot separate colours within one 5-bit cell.
    sixel::detail::Histogram hist;
    for (int i = 0; i < 100; i++)
    {
        hist.add(pack(i % 2 == 0 ? Color{ 80, 80, 80 } : Color{ 87, 87, 87 }));
    }
    sixel::detail::Wu wu;
    wu.build(hist);
    wu.split_to(24);
    std::vector<std::array<float, 3>> means;
    wu.box_means(means);
    ASSERT_EQ(means.size(), 1u);
    ASSERT_NEAR(means[0][0], 83.5f, 1e-4f);
}

TEST(sixel_palette, background_over_one_percent_is_exact)
{
    // 128 gray goes out as 50% and the palette models the terminal showing 128.
    const Color gray{ 128, 128, 128 };
    ASSERT_TRUE(sixel::detail::percent_round_trip(gray) == gray);
    Frame f = shaded_frame(100, 100, 7);
    for (int y = 0; y < 2; y++)
    {
        for (int x = 0; x < 100; x++)
        {
            f.set(x, y, gray);
        }
    }
    FittedPalette p(fixed(24));
    update(p, f, gray);
    ASSERT_TRUE(has_entry(p, gray));
    const auto plane = map(p, f);
    for (std::size_t i = 0; i < f.size(); i++)
    {
        if (f.get(i) == gray)
        {
            ASSERT_TRUE(p.entries()[plane[i]] == gray);
        }
    }

    // At 0.5% nothing forces it, and a smooth gradient leaves no entry at exactly 128.
    Frame half = shaded_frame(100, 100, 7);
    for (int x = 0; x < 50; x++)
    {
        half.set(x, 0, gray);
    }
    FittedPalette q(fixed(24));
    update(q, half, gray);
    ASSERT_FALSE(has_entry(q, gray));
}

TEST(sixel_palette, pins_never_displace_each_other)
{
    // Budget 2 with two pins and a dominant third colour: replacing entries independently
    // could hand both pins the same entry and lose black.
    Frame f(100, 100, { 200, 30, 30 });
    for (int x = 0; x < 100; x++)
    {
        f.set(x, 0, { 0, 0, 0 });
        f.set(x, 1, { 0, 0, 0 });
        f.set(x, 2, { 128, 128, 128 });
        f.set(x, 3, { 128, 128, 128 });
    }
    FittedPalette p(fixed(2));
    update(p, f, { 128, 128, 128 });
    ASSERT_EQ(p.entry_count(), 2);
    ASSERT_TRUE(has_entry(p, { 0, 0, 0 }));
    ASSERT_TRUE(has_entry(p, { 128, 128, 128 }));
}

TEST(sixel_palette, place_pins_appends_replaces_and_prioritises)
{
    // Fewer entries than pins: the pins are appended.
    std::vector<Color> entries = { { 200, 0, 0 } };
    std::vector<int> held = sixel::detail::place_pins(entries, { { 0, 0, 0 }, { 128, 128, 128 } }, 3);
    ASSERT_EQ(entries.size(), 3u);
    ASSERT_EQ(held[0], 1);
    ASSERT_EQ(held[1], 2);

    // An equal entry is reused rather than duplicated.
    entries = { { 0, 0, 0 }, { 200, 0, 0 } };
    held = sixel::detail::place_pins(entries, { { 0, 0, 0 } }, 2);
    ASSERT_EQ(entries.size(), 2u);
    ASSERT_EQ(held[0], 0);

    // At the budget, each pin takes the nearest entry no earlier pin holds.
    entries = { { 10, 10, 10 }, { 250, 250, 250 } };
    held = sixel::detail::place_pins(entries, { { 0, 0, 0 }, { 20, 20, 20 } }, 2);
    ASSERT_EQ(held[0], 0);
    ASSERT_EQ(held[1], 1);
    ASSERT_TRUE(entries[0] == Color(0, 0, 0));
    ASSERT_TRUE(entries[1] == Color(20, 20, 20));
}

TEST(sixel_palette, more_pins_than_budget_keeps_the_first)
{
    // Required, then the clear colour, then black: budget 2 drops black.
    Frame f(100, 100, { 0, 0, 0 });
    for (int y = 0; y < 50; y++)
    {
        for (int x = 0; x < 100; x++)
        {
            f.set(x, y, { 128, 128, 128 });
        }
    }
    FittedPalette p(fixed(2));
    update(p, f, { 128, 128, 128 }, Color{ 220, 80, 80 });
    ASSERT_EQ(p.entry_count(), 2);
    ASSERT_TRUE(has_entry(p, { 220, 80, 80 }));
    ASSERT_TRUE(has_entry(p, { 128, 128, 128 }));
}

TEST(sixel_palette, earlier_pin_owns_a_shared_cell)
{
    // {3,3,3} and black share a 6-bit cell but stay distinct after the percent round trip.
    // As the required colour, {3,3,3} outranks black, so black pixels show as {3,3,3}.
    const Color dark{ 3, 3, 3 };
    ASSERT_TRUE(quant256_idx(dark) == quant256_idx(Color{ 0, 0, 0 }));
    ASSERT_TRUE(sixel::detail::percent_round_trip(dark) == dark);
    Frame f(100, 100, { 0, 0, 0 });
    for (int y = 50; y < 100; y++)
    {
        for (int x = 0; x < 100; x++)
        {
            f.set(x, y, { 200, 200, 0 });
        }
    }
    FittedPalette p(fixed(24));
    update(p, f, { 0, 0, 0 }, dark);
    ASSERT_TRUE(has_entry(p, { 0, 0, 0 }));
    ASSERT_TRUE(has_entry(p, dark));
    const auto plane = map(p, f);
    ASSERT_TRUE(p.entries()[plane[0]] == dark);
}

TEST(sixel_palette, pinnable_colours_occupy_distinct_cells)
{
    // A new background or wireframe colour sharing a 6-bit cell with another pinnable colour
    // would lose exactness for one of them.
    std::vector<Color> colors = { { 0, 0, 0 } };
    for (int b = 0; b < BACKGROUND_COUNT; b++)
    {
        colors.push_back(viewer::background_color(static_cast<Background>(b)));
    }
    for (int w = 0; w < WIREFRAME_COLOR_COUNT; w++)
    {
        colors.push_back(viewer::wireframe_color_of(static_cast<WireframeColor>(w)));
    }
    std::vector<Color> distinct;
    for (const Color c : colors)
    {
        if (std::find(distinct.begin(), distinct.end(), c) == distinct.end())
        {
            distinct.push_back(c);
        }
    }
    ASSERT_EQ(distinct.size(), 9u);
    for (std::size_t i = 0; i < distinct.size(); i++)
    {
        for (std::size_t j = i + 1; j < distinct.size(); j++)
        {
            ASSERT_TRUE(quant256_idx(distinct[i]) != quant256_idx(distinct[j]));
        }
    }
}

TEST(sixel_palette, required_colour_survives_pixels_the_sampler_misses)
{
    // A thin wireframe fragment between sampled positions never reaches the fit. Pinning the
    // wireframe colour covers it.
    const Color line{ 200, 200, 200 };
    Frame f(640, 600, { 0, 0, 0 });
    int x = 0;
    for (;; x++)
    {
        f.set(x, 100, line);
        const std::vector<uint32_t> sample = sample_of(f);
        if (std::find(sample.begin(), sample.end(), pack(line)) == sample.end())
        {
            break;
        }
        f.set(x, 100, { 0, 0, 0 });
    }
    FittedPalette pinned(fixed(24));
    update(pinned, f, { 0, 0, 0 }, line);
    const auto plane = map(pinned, f);
    ASSERT_TRUE(
        pinned.entries()[plane[(std::size_t{ 100 } * 640u) + static_cast<std::size_t>(x)]] ==
        sixel::detail::percent_round_trip(line)
    );

    FittedPalette unpinned(fixed(24));
    update(unpinned, f);
    ASSERT_FALSE(has_entry(unpinned, line));
}

// Patterns whose period divides the stride, at strides 2, 4 and 8. A grid with a fixed row
// phase saw only one colour of the horizontal ones.
TEST(sixel_palette, periodic_patterns_reach_the_fit)
{
    const Color a{ 0, 0, 0 };
    const Color b{ 200, 30, 30 };
    const std::array<std::array<int, 2>, 3> sizes = { { { 512, 256 }, { 640, 600 }, { 1920, 1080 } } };
    for (const auto &size : sizes)
    {
        const int w = size[0];
        const int h = size[1];
        Frame f(w, h, a);
        std::vector<std::size_t> positions;
        const int stride = sixel::detail::sample_positions(w, h, positions);
        const std::array<bool (*)(int, int, int), 4> patterns = {
            [](int, int y, int) { return y % 2 == 1; },
            [](int x, int, int) { return x % 2 == 1; },
            [](int x, int y, int) { return (x + y) % 2 == 1; },
            [](int, int y, int s) { return y % s != 0; },
        };
        for (const auto pattern : patterns)
        {
            for (int y = 0; y < h; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    f.set(x, y, pattern(x, y, stride) ? b : a);
                }
            }
            FittedPalette p(fixed(24));
            update(p, f);
            ASSERT_TRUE(has_entry(p, a));
            ASSERT_TRUE(has_entry(p, b));
        }
    }
}

TEST(sixel_palette, wireframe_colour_change_refits_on_its_frame)
{
    Frame f(320, 240, { 0, 0, 0 });
    const auto draw = [&f](Color c)
    {
        for (int i = 0; i < 300; i++)
        {
            f.set(10 + i, 50 + (i / 10), c);
        }
    };
    FittedPalette p(fixed(24));
    draw({ 200, 200, 200 });
    update(p, f, { 0, 0, 0 }, Color{ 200, 200, 200 });
    draw({ 220, 80, 80 });
    ASSERT_TRUE(update(p, f, { 0, 0, 0 }, Color{ 220, 80, 80 }));
    const auto plane = map(p, f);
    ASSERT_TRUE(p.entries()[plane[(50u * 320u) + 10u]] == sixel::detail::percent_round_trip({ 220, 80, 80 }));
}

TEST(sixel_palette, cielab_round_trips_and_clips)
{
    // Exact, not just within 1: a fitted entry for a flat colour must round to that colour.
    for (int r = 0; r < 256; r += 5)
    {
        for (int g = 0; g < 256; g += 5)
        {
            for (int b = 0; b < 256; b += 5)
            {
                const Color c{ static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b) };
                ASSERT_TRUE(srgb8_from_cielab(cielab_from_srgb8(c)) == c);
            }
        }
    }
    // Saturated Lab outside sRGB, as a centroid of saturated colours can be.
    for (const Lab lab : { Lab{ 50.0f, 120.0f, -120.0f }, Lab{ 100.0f, 0.0f, 0.0f }, Lab{ 0.0f, -80.0f, 90.0f },
                           Lab{ 110.0f, 0.0f, 0.0f }, Lab{ -5.0f, 0.0f, 0.0f } })
    {
        const Color c = srgb8_from_cielab(lab);
        (void)c; // any uint8_t is a valid result, so only the conversion itself is under test
        const float lin = linear_to_srgb(std::nanf(""));
        ASSERT_TRUE(lin == 0.0f);
    }
    ASSERT_TRUE(srgb8_from_cielab({ 100.0f, 0.0f, 0.0f }) == Color(255, 255, 255));
    ASSERT_TRUE(srgb8_from_cielab({ 0.0f, 0.0f, 0.0f }) == Color(0, 0, 0));
    ASSERT_TRUE(srgb8_from_cielab({ 150.0f, 0.0f, 0.0f }) == Color(255, 255, 255));
}

TEST(sixel_palette, same_frame_keeps_the_palette)
{
    const Frame f = shaded_frame(200, 150, 3);
    FittedPalette p(fixed(24));
    ASSERT_TRUE(update(p, f));
    const unsigned gen = p.generation();
    ASSERT_FALSE(update(p, f));
    ASSERT_EQ(p.generation(), gen);
}

TEST(sixel_palette, moved_colours_refit)
{
    Frame f(200, 150, { 200, 30, 30 });
    FittedPalette p(fixed(24));
    update(p, f);
    f.fill_all({ 30, 30, 200 });
    ASSERT_TRUE(update(p, f));
    ASSERT_TRUE(has_entry(p, { 30, 30, 200 }));
}

// A detail too small to move the mean error past the keep test's slack, on a frame small
// enough that every pixel is sampled.
TEST(sixel_palette, small_colour_change_refits)
{
    Frame f(160, 120, {});
    const auto patch = [&f](Color c)
    {
        for (int y = 60; y < 62; y++)
        {
            for (int x = 80; x < 82; x++)
            {
                f.set(x, y, c);
            }
        }
    };
    FittedPalette p(fixed(24));
    patch({ 200, 30, 30 });
    update(p, f);
    patch({ 30, 30, 200 });
    ASSERT_TRUE(update(p, f));
    ASSERT_TRUE(has_entry(p, { 30, 30, 200 }));
}

// Deliberate: a colour on one sample keeps its old entry until the next refit. Refitting on
// single samples made tiny models refit almost every frame.
TEST(sixel_palette, one_changed_sample_keeps_the_palette)
{
    Frame f(160, 120, {});
    FittedPalette p(fixed(24));
    f.set(80, 60, { 200, 30, 30 });
    update(p, f);
    f.set(80, 60, { 30, 30, 200 });
    ASSERT_FALSE(update(p, f));
}

TEST(sixel_palette, clear_colour_change_refits_in_the_same_call)
{
    // A background toggle: the new clear colour covers most of the frame.
    Frame f = shaded_frame(200, 150, 5);
    for (int y = 0; y < 150; y++)
    {
        for (int x = 0; x < 100; x++)
        {
            f.set(x, y, { 0, 0, 0 });
        }
    }
    FittedPalette p(fixed(24));
    update(p, f, { 0, 0, 0 });
    for (int y = 0; y < 150; y++)
    {
        for (int x = 0; x < 100; x++)
        {
            f.set(x, y, { 240, 240, 240 });
        }
    }
    ASSERT_TRUE(update(p, f, { 240, 240, 240 }));
    const auto plane = map(p, f);
    ASSERT_TRUE(p.entries()[plane[0]] == sixel::detail::percent_round_trip({ 240, 240, 240 }));
}

TEST(sixel_palette, new_pin_with_an_exact_entry_keeps_the_palette)
{
    Frame f(200, 150, { 0, 0, 0 });
    for (int y = 0; y < 75; y++)
    {
        for (int x = 0; x < 200; x++)
        {
            f.set(x, y, { 255, 255, 255 });
        }
    }
    FittedPalette p(fixed(24));
    update(p, f);
    const unsigned gen = p.generation();
    ASSERT_FALSE(update(p, f, { 0, 0, 0 }, Color{ 255, 255, 255 }));
    ASSERT_EQ(p.generation(), gen);

    // A pin without an entry refits at once and becomes exact.
    ASSERT_TRUE(update(p, f, { 0, 0, 0 }, Color{ 220, 80, 80 }));
    ASSERT_EQ(p.generation(), gen + 1u);
    ASSERT_TRUE(has_entry(p, { 220, 80, 80 }));
}

TEST(sixel_palette, background_hovering_at_one_percent_refits_once)
{
    // A gray strip on sampled pixels covering 0.9% or 1.1% of the sample, alternately, so the
    // gray pin forms and lapses every frame.
    const Color gray{ 128, 128, 128 };
    const int w = 300;
    const int h = 200;
    std::vector<std::size_t> sampled;
    (void)sixel::detail::sample_positions(w, h, sampled);
    ASSERT_EQ(sampled.size(), 15000u);

    FittedPalette p(fixed(24));
    update(p, shaded_frame(w, h, 11), gray);
    const unsigned gen = p.generation();
    int refits = 0;
    for (int frame = 0; frame < 20; frame++)
    {
        Frame g = shaded_frame(w, h, 11);
        const std::size_t strip = frame % 2 == 0 ? 135 : 165;
        for (std::size_t i = 0; i < strip; i++)
        {
            g.px[sampled[i]].store(0x3F80000000000000ull | pack(gray), std::memory_order_relaxed);
        }
        const std::vector<uint32_t> sample = sample_of(g);
        ASSERT_EQ(static_cast<std::size_t>(std::count(sample.begin(), sample.end(), pack(gray))), strip);
        refits += update(p, g, gray) ? 1 : 0;
    }
    ASSERT_TRUE(refits <= 1);
    ASSERT_TRUE(p.generation() <= gen + 1u);
}

TEST(sixel_palette, budget_and_entry_count_stay_separate)
{
    // Frame 1 has two colours, so two entries at budget 24. Frame 2 adds a 2% gray region.
    // Fitting at the entry count would keep the 2-entry palette (0.93 dE against 1.74).
    Frame f(100, 100, { 0, 0, 0 });
    for (int y = 0; y < 50; y++)
    {
        for (int x = 0; x < 100; x++)
        {
            f.set(x, y, { 255, 255, 255 });
        }
    }
    FittedPalette p(fixed(24));
    update(p, f);
    ASSERT_EQ(p.entry_count(), 2);
    ASSERT_EQ(p.budget(), 24);
    for (int y = 60; y < 62; y++)
    {
        for (int x = 0; x < 100; x++)
        {
            f.set(x, y, { 128, 128, 128 });
        }
    }
    ASSERT_TRUE(update(p, f));
    ASSERT_EQ(p.entry_count(), 3);
}

TEST(sixel_palette, slow_drift_at_low_error_keeps_the_palette)
{
    // Ten flat colours, so a fresh fit is exact but for the percent rounding. One tenth of the
    // frame moves by one level each frame, a few hundredths of a dE on the mean. The 5% test
    // alone would refit on such frames; the 0.05 dE slack keeps the palette.
    FittedPalette p(fixed(24));
    Frame f(200, 80, {});
    int refits = 0;
    for (int frame = 0; frame < 12; frame++)
    {
        for (int y = 0; y < 80; y++)
        {
            for (int x = 0; x < 200; x++)
            {
                const int band = x / 20;
                const auto shade = static_cast<uint8_t>(30 + (band * 22) + (band == 0 ? frame % 2 : 0));
                f.set(x, y, { shade, static_cast<uint8_t>(shade / 2), static_cast<uint8_t>(200 - shade) });
            }
        }
        refits += update(p, f) ? 1 : 0;
    }
    ASSERT_EQ(refits, 1);
}

TEST(sixel_palette, toggle_to_a_wireframe_maps_it_on_the_first_frame)
{
    FittedPalette p(fixed(24));
    Frame f(200, 150, {});
    for (int y = 0; y < 150; y++)
    {
        for (int x = 0; x < 200; x++)
        {
            f.set(x, y, { static_cast<uint8_t>(90 + (x / 4)), static_cast<uint8_t>(50 + (y / 6)), 20 });
        }
    }
    update(p, f);
    Frame grid(200, 150, { 0, 0, 0 });
    for (int y = 0; y < 150; y++)
    {
        for (int x = 0; x < 200; x++)
        {
            if (x % 8 == 0 || y % 8 == 0)
            {
                grid.set(x, y, { 255, 255, 255 });
            }
        }
    }
    update(p, grid);
    const auto plane = map(p, grid);
    for (std::size_t i = 0; i < grid.size(); i++)
    {
        if (grid.get(i) == Color(255, 255, 255))
        {
            ASSERT_TRUE(delta_e(p.entries()[plane[i]], { 255, 255, 255 }) < 2.0f);
        }
    }
}

TEST(sixel_palette, table_entries_never_collide_with_the_sentinel)
{
    // Entry 255 alone: the value is 0xFFFFFFFF, a valid entry distinct from EMPTY_ENTRY.
    sixel::detail::LabEntries lab;
    std::vector<Color> entries(256);
    for (std::size_t j = 0; j < 256; j++)
    {
        entries[j] = { static_cast<uint8_t>(j), static_cast<uint8_t>((j * 7u) & 255u),
                       static_cast<uint8_t>((j * 13u) & 255u) };
    }
    lab.set(entries);
    const Color last = entries[255];
    const uint32_t v = sixel::detail::table_entry(cielab_from_srgb8(last), lab);
    ASSERT_EQ(v & 0xFFu, 255u);
    for (std::size_t count : { 1u, 2u, 24u, 64u, 255u, 256u })
    {
        sixel::detail::LabEntries part;
        part.set(std::vector<Color>(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(count)));
        for (std::size_t cell = 0; cell < QUANT256_LUT_SIZE; cell += 97)
        {
            const uint32_t e = sixel::detail::table_entry(sixel::detail::cell_centre_lab(cell), part);
            ASSERT_TRUE(e != sixel::detail::EMPTY_ENTRY);
            // A missing alternate repeats the nearest entry; a present one is a valid index.
            for (const uint32_t alt : { (e >> 8u) & 0xFFu, (e >> 16u) & 0xFFu, e >> 24u })
            {
                ASSERT_TRUE(alt < count);
            }
        }
    }
    ASSERT_EQ(sixel::detail::pinned_entry(255), 0xFFFFFFFFu);
}

TEST(sixel_palette, mapping_matches_the_reference_rule)
{
    // Whole frames, through the lazy table, against the rule evaluated per pixel. Several
    // frames in a row on one palette, so stale table cells from earlier palettes would show.
    FittedPalette p(fixed(24));
    for (unsigned seed = 1; seed <= 4; seed++)
    {
        Frame f = shaded_frame(160, 90, seed * 977u);
        for (int x = 0; x < 160; x++)
        {
            f.set(x, 3, { 0, 0, 0 });
            f.set(x, 4, { 0, 0, 0 });
        }
        update(p, f);
        ASSERT_TRUE(map(p, f) == reference_map(p, f, { { 0, 0, 0 } }));
    }
}

TEST(sixel_palette, lazy_fill_matches_a_full_table)
{
    // Map every cell centre first, so the second mapping reads a fully filled table, against
    // a palette with the same history whose table fills during the mapping.
    const Frame f = shaded_frame(160, 90, 42);
    Frame all(512, 512, {});
    for (std::size_t cell = 0; cell < QUANT256_LUT_SIZE; cell++)
    {
        all.set(
            static_cast<int>(cell % 512u), static_cast<int>(cell / 512u),
            { static_cast<uint8_t>(((cell >> 12u) & 63u) * 4u), static_cast<uint8_t>(((cell >> 6u) & 63u) * 4u),
              static_cast<uint8_t>((cell & 63u) * 4u) }
        );
    }
    FittedPalette eager(fixed(24));
    FittedPalette lazy(fixed(24));
    update(eager, f);
    update(lazy, f);
    (void)map(eager, all);
    ASSERT_TRUE(map(eager, f) == map(lazy, f));
}

TEST(sixel_palette, refits_leave_no_stale_table_cells_under_concurrency)
{
    // Several refits in a row, each mapped across threads, against fresh palettes given the
    // same history and mapped serially.
    std::vector<Frame> frames;
    for (unsigned seed = 0; seed < 5; seed++)
    {
        frames.push_back(shaded_frame(256, 96, (seed + 1u) * 7919u));
        frames.back().set(0, 0, { static_cast<uint8_t>(seed * 50u), 0, 0 });
    }
    // Shift whole frames so each one refits.
    for (std::size_t k = 0; k < frames.size(); k++)
    {
        for (std::size_t i = 0; i < frames[k].size(); i++)
        {
            const Color c = frames[k].get(i);
            frames[k].set(
                static_cast<int>(i % 256u), static_cast<int>(i / 256u),
                { static_cast<uint8_t>((c.r / 2u) + (k * 25u)), c.g, static_cast<uint8_t>(255u - c.b) }
            );
        }
    }
    FittedPalette threaded(fixed(24));
    for (std::size_t k = 0; k < frames.size(); k++)
    {
        update(threaded, frames[k]);
        const auto got = map_threads(threaded, frames[k], 7);
        FittedPalette fresh(fixed(24));
        for (std::size_t j = 0; j <= k; j++)
        {
            update(fresh, frames[j]);
        }
        ASSERT_EQ(fresh.generation(), threaded.generation());
        ASSERT_TRUE(got == map(fresh, frames[k]));
    }
}

TEST(sixel_palette, output_is_independent_of_the_row_split)
{
    const Frame f = shaded_frame(300, 120, 99);
    FittedPalette p(fixed(24));
    update(p, f);
    const auto serial = map(p, f);
    for (const int threads : { 2, 3, 7, 16 })
    {
        FittedPalette q(fixed(24));
        update(q, f);
        ASSERT_TRUE(map_threads(q, f, threads) == serial);
    }
}

TEST(sixel_palette, register_block_defines_each_entry)
{
    const Frame f = shaded_frame(64, 48, 17);
    FittedPalette p(fixed(24));
    update(p, f);
    std::string want;
    for (int j = 0; j < p.entry_count(); j++)
    {
        sixel::append_register(want, j, p.entries()[static_cast<std::size_t>(j)]);
    }
    ASSERT_TRUE(p.register_block() == want);
    ASSERT_TRUE(p.entry_count() >= 1 && p.entry_count() <= 24);
    // Entries are already percent-rounded, so the block reproduces them exactly.
    for (const Color c : p.entries())
    {
        ASSERT_TRUE(sixel::detail::percent_round_trip(c) == c);
    }
}

namespace
{
    Color hsv(float hue, float sat, float val)
    {
        hue = std::fmod(hue, 360.0f);
        const float c = val * sat;
        const float x = c * (1.0f - std::fabs(std::fmod(hue / 60.0f, 2.0f) - 1.0f));
        const float m = val - c;
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        if (hue < 60.0f)
        {
            r = c;
            g = x;
        }
        else if (hue < 120.0f)
        {
            r = x;
            g = c;
        }
        else if (hue < 180.0f)
        {
            g = c;
            b = x;
        }
        else if (hue < 240.0f)
        {
            g = x;
            b = c;
        }
        else if (hue < 300.0f)
        {
            r = x;
            b = c;
        }
        else
        {
            r = c;
            b = x;
        }
        const auto to8 = [m](float v) { return static_cast<uint8_t>(std::lround((v + m) * 255.0f)); };
        return { to8(r), to8(g), to8(b) };
    }

    // Hue runs across [hue0, hue0 + span) and value across [low, 1] down the frame. The fit
    // error at a given size grows with the span and with the value range.
    Frame gradient(float hue0, float span, float low)
    {
        Frame f(160, 120, {});
        for (int y = 0; y < f.h; y++)
        {
            for (int x = 0; x < f.w; x++)
            {
                f.set(
                    x, y,
                    hsv(hue0 + (span * static_cast<float>(x) / static_cast<float>(f.w)), 0.8f,
                        low + ((1.0f - low) * static_cast<float>(y) / static_cast<float>(f.h - 1)))
                );
            }
        }
        return f;
    }

    // Fit error at one fixed size, so the tests can check their fixtures sit where intended.
    double error_at(const Frame &f, int size)
    {
        FittedPalette p(fixed(size));
        update(p, f);
        return p.fit_error();
    }
} // namespace

TEST(sixel_palette, ladder_steps)
{
    using sixel::detail::ladder;
    ASSERT_TRUE(ladder({ 24, 64 }) == std::vector<int>({ 24, 32, 48, 64 }));
    ASSERT_TRUE(ladder({ 16, 64 }) == std::vector<int>({ 16, 24, 32, 48, 64 }));
    ASSERT_TRUE(ladder({ 8, 24 }) == std::vector<int>({ 8, 12, 16, 24 }));
    ASSERT_TRUE(ladder({ 40, 40 }) == std::vector<int>({ 40 }));
    for (const sixel::ColorRange r :
         { sixel::ColorRange{ 2, 8 }, sixel::ColorRange{ 255, 256 }, sixel::ColorRange{ 2, 256 } })
    {
        const std::vector<int> steps = ladder(r);
        ASSERT_EQ(steps.front(), r.min);
        ASSERT_EQ(steps.back(), r.max);
        for (std::size_t i = 1; i < steps.size(); i++)
        {
            ASSERT_TRUE(steps[i] > steps[i - 1]);
        }
    }
}

TEST(sixel_palette, register_count_caps_the_range)
{
    using sixel::cap_to_registers;
    const sixel::ColorRange capped = cap_to_registers({ 24, 64 }, 16);
    ASSERT_EQ(capped.min, 16);
    ASSERT_EQ(capped.max, 16);
    ASSERT_EQ(cap_to_registers({ 24, 64 }, 40).max, 40);
    ASSERT_EQ(cap_to_registers({ 24, 64 }, 40).min, 24);
    ASSERT_EQ(cap_to_registers({ 24, 64 }, 1024).max, 64);
    // No reply, or one register, which cannot show an image: no cap.
    ASSERT_EQ(cap_to_registers({ 24, 64 }, 0).max, 64);
    ASSERT_EQ(cap_to_registers({ 24, 64 }, 1).max, 64);

    // A rich frame under the capped range gets at most 16 registers.
    FittedPalette p(capped);
    update(p, gradient(0.0f, 359.0f, 0.2f));
    ASSERT_TRUE(p.entry_count() <= 16);
}

TEST(sixel_palette, few_colours_stay_at_the_bottom_of_the_ladder)
{
    Frame f(120, 90, { 0, 0, 0 });
    for (int y = 0; y < 90; y++)
    {
        for (int x = 0; x < 40; x++)
        {
            f.set(x, y, { 200, 40, 40 });
            f.set(x + 40, y, { 40, 40, 200 });
        }
    }
    FittedPalette p({ 24, 64 });
    update(p, f);
    ASSERT_EQ(p.budget(), 24);
    ASSERT_EQ(p.entry_count(), 3);
}

TEST(sixel_palette, many_colours_step_up_to_the_first_passing_size)
{
    // Over 2.5 dE at 24 and 32, under it at 48.
    const Frame f = gradient(0.0f, 20.0f, 0.2f);
    ASSERT_TRUE(error_at(f, 32) > 2.6);
    ASSERT_TRUE(error_at(f, 48) < 2.4);
    FittedPalette p({ 24, 64 });
    update(p, f);
    ASSERT_EQ(p.budget(), 48);
}

TEST(sixel_palette, background_coverage_does_not_change_the_size)
{
    // The same model filling 11% of a black frame. The background is pinned and exact, so a
    // mean over every sample would stay under the threshold and keep the model at 24.
    const Frame model = gradient(0.0f, 20.0f, 0.2f);
    Frame framed(model.w * 3, model.h * 3, { 0, 0, 0 });
    for (int y = 0; y < model.h; y++)
    {
        for (int x = 0; x < model.w; x++)
        {
            const std::size_t i =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(model.w)) + static_cast<std::size_t>(x);
            framed.set(x + model.w, y + model.h, model.get(i));
        }
    }
    FittedPalette alone({ 24, 64 });
    update(alone, model);
    ASSERT_EQ(alone.budget(), 48);
    FittedPalette on_background({ 24, 64 });
    update(on_background, framed);
    ASSERT_EQ(on_background.budget(), 48);
}

TEST(sixel_palette, step_up_while_the_palette_still_fits)
{
    // A scene that gains colours slowly: the current palette tracks a fresh fit at its size
    // closely enough for the keep test, but the fresh fit has crossed the threshold.
    const Frame before = gradient(0.0f, 20.0f, 0.56f);
    const Frame after = gradient(0.0f, 21.0f, 0.558f);
    FittedPalette at24(fixed(24));
    update(at24, before);
    ASSERT_TRUE(at24.fit_error() < 2.5);
    ASSERT_FALSE(update(at24, after));
    ASSERT_TRUE(at24.fit_error() > 2.5);

    FittedPalette p({ 24, 64 });
    update(p, before);
    ASSERT_EQ(p.budget(), 24);
    ASSERT_TRUE(update(p, after));
    // The refit never installs the size whose fit triggered it.
    ASSERT_TRUE(p.budget() > 24);
}

TEST(sixel_palette, size_band_stops_flapping)
{
    // A sits between 2.5 / 1.1 and 2.5 at 24, B above 2.5. Opposite hues, so every switch
    // refits. Without the band, A would take 24 at each refit and the size would alternate.
    const Frame a = gradient(180.0f, 20.0f, 0.725f);
    const Frame b = gradient(0.0f, 20.0f, 0.5f);
    const Frame c = gradient(180.0f, 20.0f, 0.8f);
    ASSERT_TRUE(error_at(a, 24) > 2.3 && error_at(a, 24) < 2.45);
    ASSERT_TRUE(error_at(b, 24) > 2.55);
    ASSERT_TRUE(error_at(c, 24) < 2.2);

    FittedPalette p({ 24, 64 });
    update(p, a);
    ASSERT_EQ(p.budget(), 24);
    int changes = 0;
    int last = p.budget();
    for (int frame = 0; frame < 8; frame++)
    {
        ASSERT_TRUE(update(p, frame % 2 == 0 ? b : a));
        changes += p.budget() != last ? 1 : 0;
        last = p.budget();
    }
    ASSERT_EQ(changes, 1);
    ASSERT_EQ(p.budget(), 32);

    // An error well under the band brings it back down at the next refit.
    ASSERT_TRUE(update(p, c));
    ASSERT_EQ(p.budget(), 24);
}

TEST(sixel_palette, size_steps_down_only_at_a_refit)
{
    FittedPalette p({ 24, 64 });
    const Frame rich = gradient(0.0f, 20.0f, 0.2f);
    update(p, rich);
    ASSERT_EQ(p.budget(), 48);

    // The same frame again fits the 48-entry palette exactly as well, so nothing refits and
    // the larger budget stays, by design.
    ASSERT_FALSE(update(p, rich));
    ASSERT_EQ(p.budget(), 48);

    // A frame the old palette no longer fits refits, and the ladder starts from the bottom.
    Frame flat(160, 120, { 30, 30, 30 });
    for (int x = 0; x < 80; x++)
    {
        for (int y = 0; y < 120; y++)
        {
            flat.set(x, y, { 230, 210, 40 });
        }
    }
    ASSERT_TRUE(update(p, flat));
    ASSERT_EQ(p.budget(), 24);
}
