#include "src/terminal/sixel.h"

#include "tests/sixel_test_util.h"
#include "tests/test.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

    std::vector<unsigned char> solid_plane(int w, int h, unsigned char index)
    {
        return std::vector<unsigned char>(static_cast<size_t>(w) * static_cast<size_t>(h), index);
    }

    // Definitions for registers 0..count-1, each a distinct colour.
    std::string block_of(int count)
    {
        std::string block;
        for (int j = 0; j < count; j++)
        {
            const auto v = static_cast<uint8_t>(j);
            sixel::append_register(block, j, { v, static_cast<uint8_t>(255 - j), static_cast<uint8_t>(j / 2) });
        }
        return block;
    }

    const std::string &full_block()
    {
        static const std::string block = block_of(sixel::MAX_REGISTERS);
        return block;
    }

    std::string encode(const std::vector<unsigned char> &plane, int w, int h)
    {
        std::string out;
        sixel::Scratch scratch;
        sixel::append_frame(out, plane.data(), w, h, full_block(), scratch);
        return out;
    }

} // namespace

TEST(sixel, non_positive_dimensions_append_nothing)
{
    std::string out;
    sixel::Scratch scratch;
    // volatile so constant propagation cannot mint an append_frame clone
    // specialized for a negative width: GCC's LTO alloc-size analysis then
    // flags the guarded (dead) allocation path inside that clone.
    volatile int zero = 0;
    volatile int neg = -1;
    sixel::append_frame(out, nullptr, zero, 6, full_block(), scratch);
    sixel::append_frame(out, nullptr, 4, zero, full_block(), scratch);
    sixel::append_frame(out, nullptr, neg, neg, full_block(), scratch);
    ASSERT_TRUE(out.empty());
}

TEST(sixel, dirty_scratch_reuse_encodes_cleanly)
{
    // The scratch masks are deliberately left dirty between frames (the stamp
    // gates every first touch); a reused scratch must round-trip a different
    // plane exactly, including one that shrinks the width.
    sixel::Scratch scratch;
    std::string first;
    const auto plane_a = solid_plane(9, 12, 40);
    sixel::append_frame(first, plane_a.data(), 9, 12, full_block(), scratch);

    std::vector<unsigned char> plane_b(static_cast<size_t>(5) * 6u);
    for (size_t i = 0; i < plane_b.size(); i++)
    {
        plane_b[i] = static_cast<unsigned char>(1u + (i % 200u));
    }
    std::string second;
    sixel::append_frame(second, plane_b.data(), 5, 6, full_block(), scratch);
    const SixelFrame f = sixel_decode(second);
    for (size_t i = 0; i < plane_b.size(); i++)
    {
        ASSERT_EQ(f.plane[i], plane_b[i]);
    }
}

TEST(sixel, header_declares_transparent_zeros_and_raster_size)
{
    const auto plane = solid_plane(4, 6, 40);
    const std::string out = encode(plane, 4, 6);
    ASSERT_TRUE(out.rfind("\033P0;1;0q\"1;1;4;6", 0) == 0);
    ASSERT_TRUE(out.size() >= 2 && out.compare(out.size() - 2, 2, "\033\\") == 0);
    const SixelFrame f = sixel_decode(out);
    ASSERT_EQ(f.p2, 1);
    ASSERT_EQ(f.w, 4);
    ASSERT_EQ(f.h, 6);
}

TEST(sixel, header_carries_the_register_block)
{
    // Only the registers the caller defines go out, at any count the encoder can address.
    for (const int count : { 1, 2, 24, 64, 255, 256 })
    {
        const std::string block = block_of(count);
        const auto plane = solid_plane(2, 6, static_cast<unsigned char>(count - 1));
        std::string out;
        sixel::Scratch scratch;
        sixel::append_frame(out, plane.data(), 2, 6, block, scratch);
        ASSERT_TRUE(out.find(block) != std::string::npos);
        const SixelFrame f = sixel_decode(out);
        for (int j = 0; j < sixel::MAX_REGISTERS; j++)
        {
            const auto i = static_cast<size_t>(j);
            ASSERT_EQ(f.defined[i], j < count);
            if (j < count)
            {
                const auto v = static_cast<uint8_t>(j);
                ASSERT_EQ(f.palette[i].r, static_cast<int>(sixel::channel_pct(v)));
                ASSERT_EQ(f.palette[i].g, static_cast<int>(sixel::channel_pct(static_cast<uint8_t>(255 - j))));
                ASSERT_EQ(f.palette[i].b, static_cast<int>(sixel::channel_pct(static_cast<uint8_t>(j / 2))));
            }
        }
        for (const int reg : f.plane)
        {
            ASSERT_EQ(reg, count - 1);
        }
    }
}

TEST(sixel, percent_conversion_round_trips)
{
    // A percentage read back as 8 bits encodes to the same percentage, so a palette that
    // rounds its entries through both directions matches what the terminal shows.
    for (unsigned int p = 0; p <= 100; p++)
    {
        ASSERT_EQ(sixel::channel_pct(sixel::channel_from_pct(p)), p);
    }
    for (unsigned int v = 0; v <= 255; v++)
    {
        const uint8_t once = sixel::channel_from_pct(sixel::channel_pct(static_cast<uint8_t>(v)));
        ASSERT_TRUE((once > v ? once - v : v - once) <= 1u);
        ASSERT_EQ(sixel::channel_from_pct(sixel::channel_pct(once)), once);
    }
    ASSERT_EQ(sixel::channel_pct(128), 50u);
    ASSERT_EQ(sixel::channel_from_pct(50), 128u);
}

TEST(sixel, solid_frame_roundtrip)
{
    const auto plane = solid_plane(8, 12, 40);
    const SixelFrame f = sixel_decode(encode(plane, 8, 12));
    for (const int reg : f.plane)
    {
        ASSERT_EQ(reg, 40);
    }
}

TEST(sixel, two_color_split_uses_a_return_between_passes)
{
    auto plane = solid_plane(8, 6, 16);
    for (int y = 0; y < 6; y++)
    {
        for (int x = 4; x < 8; x++)
        {
            plane[(static_cast<size_t>(y) * 8u) + static_cast<size_t>(x)] = 231;
        }
    }
    const std::string out = encode(plane, 8, 6);
    // One band, two colour passes: exactly one `$` in the whole stream.
    size_t dollars = 0;
    for (const char c : out)
    {
        dollars += (c == '$') ? 1u : 0u;
    }
    ASSERT_EQ(dollars, 1u);
    const SixelFrame f = sixel_decode(out);
    for (int y = 0; y < 6; y++)
    {
        for (int x = 0; x < 8; x++)
        {
            const int want = (x < 4) ? 16 : 231;
            ASSERT_EQ(f.plane[(static_cast<size_t>(y) * 8u) + static_cast<size_t>(x)], want);
        }
    }
}

TEST(sixel, pseudo_random_plane_roundtrip)
{
    const int w = 31;
    const int h = 18;
    std::vector<unsigned char> plane(static_cast<size_t>(w) * static_cast<size_t>(h));
    unsigned int seed = 0x12345u;
    for (auto &v : plane)
    {
        seed = (seed * 1664525u) + 1013904223u;
        v = static_cast<unsigned char>((seed >> 16u) % 256u);
    }
    const SixelFrame f = sixel_decode(encode(plane, w, h));
    for (size_t i = 0; i < plane.size(); i++)
    {
        ASSERT_EQ(f.plane[i], plane[i]);
    }
}

TEST(sixel, partial_last_band_paints_nothing_past_height)
{
    // h=8: the second band has two live rows; the decoder asserts any bit past
    // the declared height, so a clean decode is the proof.
    const int w = 5;
    const int h = 8;
    std::vector<unsigned char> plane(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (size_t i = 0; i < plane.size(); i++)
    {
        plane[i] = static_cast<unsigned char>(i % 256u);
    }
    const SixelFrame f = sixel_decode(encode(plane, w, h));
    for (size_t i = 0; i < plane.size(); i++)
    {
        ASSERT_EQ(f.plane[i], plane[i]);
    }
}

TEST(sixel, partial_band_reuses_a_register_from_the_full_band)
{
    // Reuse one register across a full and partial band. Stale mask bits would
    // paint beyond the declared height, unlike the distinct-register case above.
    const auto plane = solid_plane(5, 8, 40);
    const SixelFrame f = sixel_decode(encode(plane, 5, 8));
    for (size_t i = 0; i < plane.size(); i++)
    {
        ASSERT_EQ(f.plane[i], 40);
    }
}

TEST(sixel, all_256_registers_in_one_band)
{
    // The colors/stamp/min_x/max_x arrays at their exact capacity boundary:
    // one band whose row cycles through every register.
    const int w = 512;
    std::vector<unsigned char> plane(static_cast<size_t>(w) * 6u);
    for (size_t i = 0; i < plane.size(); i++)
    {
        plane[i] = static_cast<unsigned char>(i % 256u);
    }
    const SixelFrame f = sixel_decode(encode(plane, w, 6));
    for (size_t i = 0; i < plane.size(); i++)
    {
        ASSERT_EQ(f.plane[i], plane[i]);
    }
}

TEST(sixel, appends_after_existing_bytes)
{
    // The production call shape: append_frame composes into a buffer that
    // already holds frame bytes (the cursor home); the prefix must survive.
    const auto plane = solid_plane(4, 6, 40);
    std::string out = "PFX";
    sixel::Scratch scratch;
    sixel::append_frame(out, plane.data(), 4, 6, full_block(), scratch);
    ASSERT_TRUE(out.rfind("PFX", 0) == 0);
    const SixelFrame f = sixel_decode(out.substr(3));
    ASSERT_EQ(f.w, 4);
    ASSERT_EQ(f.h, 6);
}

TEST(sixel, scratch_grows_back_after_shrinking)
{
    // One Scratch across big -> small -> big: the grow-only capacity must
    // re-derive its stride from the current width every call.
    sixel::Scratch scratch;
    std::string out;
    const auto big = solid_plane(64, 12, 40);
    const auto small = solid_plane(3, 6, 41);
    sixel::append_frame(out, big.data(), 64, 12, full_block(), scratch);
    out.clear();
    sixel::append_frame(out, small.data(), 3, 6, full_block(), scratch);
    out.clear();
    sixel::append_frame(out, big.data(), 64, 12, full_block(), scratch);
    const SixelFrame f = sixel_decode(out);
    for (size_t i = 0; i < big.size(); i++)
    {
        ASSERT_EQ(f.plane[i], 40);
    }
}

TEST(sixel, no_band_separator_after_the_last_band)
{
    // Single band: no `-` at all (the only place the encoder emits one).
    const std::string one = encode(solid_plane(4, 6, 40), 4, 6);
    ASSERT_TRUE(one.find('-') == std::string::npos);
    // Two bands: exactly one, and not immediately before the terminator.
    const std::string two = encode(solid_plane(4, 12, 40), 4, 12);
    size_t seps = 0;
    for (const char c : two)
    {
        seps += (c == '-') ? 1u : 0u;
    }
    ASSERT_EQ(seps, 1u);
    ASSERT_TRUE(two[two.size() - 3] != '-');
}

TEST(sixel, runs_of_three_or_fewer_stay_literal)
{
    // 3-wide solid: a run of 3 full-mask chars, spelled out; `!` appears nowhere.
    const std::string out = encode(solid_plane(3, 6, 40), 3, 6);
    ASSERT_TRUE(out.find('!') == std::string::npos);
    ASSERT_TRUE(out.find("~~~") != std::string::npos);
    // 4-wide solid: the counted form takes over.
    const std::string counted = encode(solid_plane(4, 6, 40), 4, 6);
    ASSERT_TRUE(counted.find("!4~") != std::string::npos);
}

TEST(sixel, plane_holds_register_numbers)
{
    const SixelFrame lo = sixel_decode(encode(solid_plane(1, 6, 0), 1, 6));
    ASSERT_EQ(lo.plane[0], 0);
    const SixelFrame hi = sixel_decode(encode(solid_plane(1, 6, 255), 1, 6));
    ASSERT_EQ(hi.plane[0], 255);
}

TEST(sixel, width_one_multi_band_roundtrip)
{
    std::vector<unsigned char> plane(7);
    for (size_t y = 0; y < 7; y++)
    {
        plane[y] = static_cast<unsigned char>(y);
    }
    const SixelFrame f = sixel_decode(encode(plane, 1, 7));
    for (size_t y = 0; y < 7; y++)
    {
        ASSERT_EQ(f.plane[y], plane[y]);
    }
}

TEST(sixel, leading_gap_is_skipped_with_empty_sixels)
{
    // Colour B occupies only the right edge of a wide band; its pass must skip
    // the gap with `!<n>?` rather than walking it, and the round trip must hold.
    const int w = 40;
    auto plane = solid_plane(w, 6, 16);
    for (int y = 0; y < 6; y++)
    {
        plane[(static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(w) - 1u] = 231;
    }
    const std::string out = encode(plane, w, 6);
    ASSERT_TRUE(out.find("!39?") != std::string::npos);
    const SixelFrame f = sixel_decode(out);
    for (int y = 0; y < 6; y++)
    {
        ASSERT_EQ(f.plane[(static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(w) - 1u], 231);
    }
}

TEST(sixel, decoder_rejects_a_second_paint)
{
    // The roundtrip tests rely on the decoder catching a pixel that two passes paint.
    bool rejected = false;
    try
    {
        (void)sixel_decode("\033P0;1;0q\"1;1;1;1#0;2;0;0;0#1;2;100;0;0#0@$#1@\033\\");
    }
    catch (const testing::AssertionError &)
    {
        rejected = true;
    }
    ASSERT_TRUE(rejected);
    (void)sixel_decode("\033P0;1;0q\"1;1;1;1#0;2;0;0;0#0@\033\\");
}
