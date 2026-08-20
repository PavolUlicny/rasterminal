#include "src/sixel.h"

#include "src/color.h"
#include "tests/sixel_test_util.h"
#include "tests/test.h"

#include <cstddef>
#include <string>
#include <vector>

namespace
{

    std::vector<unsigned char> solid_plane(int w, int h, unsigned char index)
    {
        return std::vector<unsigned char>(static_cast<size_t>(w) * static_cast<size_t>(h), index);
    }

    std::string encode(const std::vector<unsigned char> &plane, int w, int h)
    {
        std::string out;
        sixel::Scratch scratch;
        sixel::append_frame(out, plane.data(), w, h, scratch);
        return out;
    }

    // The register each decoded pixel should carry for a source index.
    int reg_of(unsigned char index)
    {
        return index - 16;
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
    sixel::append_frame(out, nullptr, zero, 6, scratch);
    sixel::append_frame(out, nullptr, 4, zero, scratch);
    sixel::append_frame(out, nullptr, neg, neg, scratch);
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
    sixel::append_frame(first, plane_a.data(), 9, 12, scratch);

    std::vector<unsigned char> plane_b(static_cast<size_t>(5) * 6u);
    for (size_t i = 0; i < plane_b.size(); i++)
    {
        plane_b[i] = static_cast<unsigned char>(17u + (i % 200u));
    }
    std::string second;
    sixel::append_frame(second, plane_b.data(), 5, 6, scratch);
    const SixelFrame f = sixel_decode(second);
    for (size_t i = 0; i < plane_b.size(); i++)
    {
        ASSERT_EQ(f.plane[i], plane_b[i] - 16);
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

TEST(sixel, all_240_registers_defined_with_percent_channels)
{
    const auto plane = solid_plane(2, 6, 16);
    const SixelFrame f = sixel_decode(encode(plane, 2, 6));
    for (int j = 0; j < 240; j++)
    {
        ASSERT_TRUE(f.defined[static_cast<size_t>(j)]);
        const Color c = quant256_palette_entry(j);
        const auto pct = [](uint8_t v)
        { return static_cast<int>(((static_cast<unsigned int>(v) * 100u) + 127u) / 255u); };
        ASSERT_EQ(f.palette[static_cast<size_t>(j)].r, pct(c.r));
        ASSERT_EQ(f.palette[static_cast<size_t>(j)].g, pct(c.g));
        ASSERT_EQ(f.palette[static_cast<size_t>(j)].b, pct(c.b));
    }
    ASSERT_FALSE(f.defined[240]);
}

TEST(sixel, solid_frame_roundtrip)
{
    const auto plane = solid_plane(8, 12, 40);
    const SixelFrame f = sixel_decode(encode(plane, 8, 12));
    for (const int reg : f.plane)
    {
        ASSERT_EQ(reg, reg_of(40));
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
            const int want = (x < 4) ? reg_of(16) : reg_of(231);
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
        v = static_cast<unsigned char>(16u + ((seed >> 16u) % 240u));
    }
    const SixelFrame f = sixel_decode(encode(plane, w, h));
    for (size_t i = 0; i < plane.size(); i++)
    {
        ASSERT_EQ(f.plane[i], reg_of(plane[i]));
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
        plane[i] = static_cast<unsigned char>(16u + (i % 240u));
    }
    const SixelFrame f = sixel_decode(encode(plane, w, h));
    for (size_t i = 0; i < plane.size(); i++)
    {
        ASSERT_EQ(f.plane[i], reg_of(plane[i]));
    }
}

TEST(sixel, partial_band_reuses_a_register_from_the_full_band)
{
    // The same register live in band 0 AND the partial band: mask bits left
    // over from band 0 would paint rows past the declared height, which the
    // decoder asserts on. The distinct-register test above cannot see this
    // (no register there spans both bands); found by mutating the first-touch
    // clear to wipe only the band's own bit positions.
    const auto plane = solid_plane(5, 8, 40);
    const SixelFrame f = sixel_decode(encode(plane, 5, 8));
    for (size_t i = 0; i < plane.size(); i++)
    {
        ASSERT_EQ(f.plane[i], reg_of(40));
    }
}

TEST(sixel, all_240_registers_in_one_band)
{
    // The colors/stamp/min_x/max_x arrays at their exact capacity boundary:
    // one band whose row cycles through every register.
    const int w = 480;
    std::vector<unsigned char> plane(static_cast<size_t>(w) * 6u);
    for (size_t i = 0; i < plane.size(); i++)
    {
        plane[i] = static_cast<unsigned char>(16u + (i % 240u));
    }
    const SixelFrame f = sixel_decode(encode(plane, w, 6));
    for (size_t i = 0; i < plane.size(); i++)
    {
        ASSERT_EQ(f.plane[i], reg_of(plane[i]));
    }
}

TEST(sixel, appends_after_existing_bytes)
{
    // The production call shape: append_frame composes into a buffer that
    // already holds frame bytes (the cursor home); the prefix must survive.
    const auto plane = solid_plane(4, 6, 40);
    std::string out = "PFX";
    sixel::Scratch scratch;
    sixel::append_frame(out, plane.data(), 4, 6, scratch);
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
    sixel::append_frame(out, big.data(), 64, 12, scratch);
    out.clear();
    sixel::append_frame(out, small.data(), 3, 6, scratch);
    out.clear();
    sixel::append_frame(out, big.data(), 64, 12, scratch);
    const SixelFrame f = sixel_decode(out);
    for (size_t i = 0; i < big.size(); i++)
    {
        ASSERT_EQ(f.plane[i], reg_of(40));
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

TEST(sixel, register_is_index_minus_16)
{
    const SixelFrame lo = sixel_decode(encode(solid_plane(1, 6, 16), 1, 6));
    ASSERT_EQ(lo.plane[0], 0);
    const SixelFrame hi = sixel_decode(encode(solid_plane(1, 6, 255), 1, 6));
    ASSERT_EQ(hi.plane[0], 239);
}

TEST(sixel, width_one_multi_band_roundtrip)
{
    std::vector<unsigned char> plane(7);
    for (size_t y = 0; y < 7; y++)
    {
        plane[y] = static_cast<unsigned char>(16u + y);
    }
    const SixelFrame f = sixel_decode(encode(plane, 1, 7));
    for (size_t y = 0; y < 7; y++)
    {
        ASSERT_EQ(f.plane[y], reg_of(plane[y]));
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
        ASSERT_EQ(
            f.plane[(static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(w) - 1u], reg_of(231)
        );
    }
}
