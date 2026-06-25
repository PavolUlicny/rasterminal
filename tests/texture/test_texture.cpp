#include "tests/test.h"
#include "tests/loader_util.h"
#include "tests/inline_bmp.h"
#include "src/texture.h"

#include <climits>
#include <cstdio>
#include <cstdint>
#include <vector>

// Build a Texture from a raw RGBA pixel buffer, bypassing stb_image.
// Pixels are row-major, top-left first (same layout as Texture::pixels).
static Texture make_tex(int w, int h, std::vector<uint8_t> rgba)
{
    Texture t;
    t.width = w;
    t.height = h;
    t.pixels = std::move(rgba);
    return t;
}

// 1×1 texture of a single solid colour.
static Texture solid(uint8_t r, uint8_t g, uint8_t b)
{
    return make_tex(1, 1, { r, g, b, 255 });
}

// ─── valid() ──────────────────────────────────────────────────────────────────

TEST(texture, valid_true_when_populated)
{
    ASSERT_TRUE(solid(255, 0, 0).valid());
}

TEST(texture, valid_false_when_default_constructed)
{
    Texture t;
    ASSERT_FALSE(t.valid());
}

// ─── 1×1 solid colour ─────────────────────────────────────────────────────────

TEST(texture, solid_1x1_returns_exact_color)
{
    Texture t = solid(255, 128, 0);
    vec3 c = t.sample_rgb(0.5f, 0.5f);
    ASSERT_NEAR(c.x, 255.0f / 255.0f, 1e-4f);
    ASSERT_NEAR(c.y, 128.0f / 255.0f, 1e-4f);
    ASSERT_NEAR(c.z, 0.0f / 255.0f, 1e-4f);
}

TEST(texture, solid_1x1_is_invariant_to_uv_coords)
{
    // Any UV value on a 1×1 texture maps to the same pixel.
    Texture t = solid(100, 200, 50);
    vec3 a = t.sample_rgb(0.0f, 0.0f);
    vec3 b = t.sample_rgb(0.7f, 0.3f);
    vec3 c = t.sample_rgb(3.5f, 7.2f);
    ASSERT_NEAR(a.x, b.x, 1e-5f);
    ASSERT_NEAR(a.x, c.x, 1e-5f);
}

// ─── UV wrap ──────────────────────────────────────────────────────────────────

TEST(texture, uv_wraps_very_large_positive)
{
    // u=1000.5 should wrap identically to u=0.5 via floor().
    Texture t = make_tex(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 a = t.sample_rgb(0.25f, 0.5f);
    vec3 b = t.sample_rgb(1000.25f, 0.5f);
    ASSERT_NEAR(a.x, b.x, 1e-4f);
    ASSERT_NEAR(a.z, b.z, 1e-4f);
}

TEST(texture, uv_wraps_at_integer_boundary)
{
    // 2×1: left pixel = red, right pixel = blue.
    // u=0.25 and u=1.25 must produce the same sample.
    Texture t = make_tex(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 a = t.sample_rgb(0.25f, 0.5f);
    vec3 b = t.sample_rgb(1.25f, 0.5f);
    ASSERT_NEAR(a.x, b.x, 1e-5f);
    ASSERT_NEAR(a.z, b.z, 1e-5f);
}

TEST(texture, uv_wraps_for_negative_coords)
{
    // u=-0.75 wraps to 0.25 (−0.75 − floor(−0.75) = −0.75 − (−1) = 0.25).
    Texture t = make_tex(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 a = t.sample_rgb(0.25f, 0.5f);
    vec3 b = t.sample_rgb(-0.75f, 0.5f);
    ASSERT_NEAR(a.x, b.x, 1e-5f);
    ASSERT_NEAR(a.z, b.z, 1e-5f);
}

// ─── bilinear interpolation ───────────────────────────────────────────────────

TEST(texture, bilinear_midpoint_averages_horizontal_neighbors)
{
    // 2×1: left=red (1,0,0), right=blue (0,0,1).
    // u=0.5 → fx=0.5, tx=0.5 → result = (0.5, 0, 0.5).
    Texture t = make_tex(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 c = t.sample_rgb(0.5f, 0.5f);
    ASSERT_NEAR(c.x, 0.5f, 1e-4f);
    ASSERT_NEAR(c.y, 0.0f, 1e-4f);
    ASSERT_NEAR(c.z, 0.5f, 1e-4f);
}

TEST(texture, bilinear_midpoint_averages_vertical_neighbors)
{
    // 1×2: row0 (top image) = red, row1 (bottom image) = blue.
    // v=0.5 (OBJ mid) → after flip: 0.5 → fy=0.5 → lerp(red,blue,0.5) = (0.5,0,0.5).
    Texture t = make_tex(1, 2, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 c = t.sample_rgb(0.0f, 0.5f);
    ASSERT_NEAR(c.x, 0.5f, 1e-4f);
    ASSERT_NEAR(c.z, 0.5f, 1e-4f);
}

// ─── V-axis flip ──────────────────────────────────────────────────────────────
// OBJ v=0 means "bottom of texture"; image row 0 is the top. sample_rgb flips V.

TEST(texture, v_zero_maps_to_bottom_image_row)
{
    // 1×2: image row0=red (top), row1=blue (bottom).
    // OBJ v=0 (texture bottom) → flip → v=1.0 → fy=1.0 → image row 1 = blue.
    Texture t = make_tex(1, 2, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 c = t.sample_rgb(0.0f, 0.0f);
    ASSERT_NEAR(c.x, 0.0f, 1e-4f); // low red
    ASSERT_NEAR(c.z, 1.0f, 1e-4f); // full blue
}

TEST(texture, v_near_one_maps_toward_top_image_row)
{
    // OBJ v→1 (texture top) → flip → v→0 → fy→0 → image row 0 = red.
    Texture t = make_tex(1, 2, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 c = t.sample_rgb(0.0f, 0.999f);
    ASSERT_TRUE(c.x > 0.9f); // mostly red
    ASSERT_TRUE(c.z < 0.1f); // little blue
}

// ─── Texture::load / load_from_memory ────────────────────────────────────────

TEST(texture_load, load_nonexistent_returns_false)
{
    Texture t;
    ASSERT_FALSE(t.load(tmp_path("rasterminal_nonexistent_XXXXX.png")));
    ASSERT_FALSE(t.valid());
}

TEST(texture_load, load_from_memory_null_returns_false)
{
    Texture t;
    ASSERT_FALSE(t.load_from_memory(nullptr, 8));
    ASSERT_FALSE(t.valid());
}

TEST(texture_load, load_from_memory_zero_size_returns_false)
{
    Texture t;
    const uint8_t data[] = { 1, 2, 3 };
    ASSERT_FALSE(t.load_from_memory(data, 0));
    ASSERT_FALSE(t.valid());
}

TEST(texture_load, load_from_memory_garbage_returns_false)
{
    Texture t;
    const uint8_t garbage[] = { 0x00, 0x01, 0x02, 0x03, 0xDE, 0xAD, 0xBE, 0xEF };
    ASSERT_FALSE(t.load_from_memory(garbage, sizeof(garbage)));
    ASSERT_FALSE(t.valid());
}

// ─── bilinear 2×2 full interpolation ─────────────────────────────────────────

TEST(texture, bilinear_2x2_center_averages_all_four_corners)
{
    // 2×2 texture (row-major, top-left first):
    //   [0,0] = red    (255,  0,  0)
    //   [1,0] = green  (  0,255,  0)
    //   [0,1] = blue   (  0,  0,255)
    //   [1,1] = white  (255,255,255)
    // sample_rgb(u=0.5, v=0.5):
    //   wrap: u=0.5, v=0.5  →  v_flipped = 0.5
    //   fx = 0.5*(2-1) = 0.5  →  x0=0, x1=1, tx=0.5
    //   fy = 0.5*(2-1) = 0.5  →  y0=0, y1=1, ty=0.5
    //   top    = lerp(red, green, 0.5) = (0.5, 0.5, 0)
    //   bottom = lerp(blue, white,0.5) = (0.5, 0.5, 1)
    //   result = lerp(top, bottom, 0.5) = (0.5, 0.5, 0.5)
    Texture t = make_tex(
        2, 2,
        {
            255, 0, 0,
            255, // [0,0] red
            0, 255, 0,
            255, // [1,0] green
            0, 0, 255,
            255, // [0,1] blue
            255, 255, 255,
            255, // [1,1] white
        }
    );
    vec3 c = t.sample_rgb(0.5f, 0.5f);
    ASSERT_NEAR(c.x, 0.5f, 1e-4f);
    ASSERT_NEAR(c.y, 0.5f, 1e-4f);
    ASSERT_NEAR(c.z, 0.5f, 1e-4f);
}

TEST(texture, sample_rgba_2x2_center_averages_all_four_corners)
{
    Texture t = make_tex(
        2, 2,
        {
            255,
            0,
            0,
            0,
            0,
            255,
            0,
            64,
            0,
            0,
            255,
            128,
            255,
            255,
            255,
            255,
        }
    );
    vec4 c = t.sample_rgba(0.5f, 0.5f);
    ASSERT_NEAR(c.x, 0.5f, 1e-4f);
    ASSERT_NEAR(c.y, 0.5f, 1e-4f);
    ASSERT_NEAR(c.z, 0.5f, 1e-4f);
    ASSERT_NEAR(c.w, (0.0f + 64.0f + 128.0f + 255.0f) / 4.0f / 255.0f, 1e-4f);
}

// ─── sample_rgba UV wrap ──────────────────────────────────────────────────────
// sample_rgba has the same wrap + V-flip code as sample_rgb; these tests confirm
// all four channels including alpha.

TEST(texture, sample_rgba_wraps_positive_coords)
{
    // 2×1: left=(255,0,0,200), right=(0,0,255,100). u=0.25 and u=1.25 must agree.
    Texture t = make_tex(2, 1, { 255, 0, 0, 200, 0, 0, 255, 100 });
    vec4 a = t.sample_rgba(0.25f, 0.5f);
    vec4 b = t.sample_rgba(1.25f, 0.5f);
    vec4 c = t.sample_rgba(1000.25f, 0.5f);
    ASSERT_NEAR(a.x, b.x, 1e-4f);
    ASSERT_NEAR(a.y, b.y, 1e-4f);
    ASSERT_NEAR(a.z, b.z, 1e-4f);
    ASSERT_NEAR(a.w, b.w, 1e-4f);
    ASSERT_NEAR(a.x, c.x, 1e-4f);
    ASSERT_NEAR(a.w, c.w, 1e-4f);
}

TEST(texture, sample_rgba_wraps_negative_coords)
{
    // u=−0.75 → floor(−0.75)=−1 → −0.75−(−1)=0.25 — same as u=0.25.
    Texture t = make_tex(2, 1, { 255, 0, 0, 200, 0, 0, 255, 100 });
    vec4 a = t.sample_rgba(0.25f, 0.5f);
    vec4 b = t.sample_rgba(-0.75f, 0.5f);
    ASSERT_NEAR(a.x, b.x, 1e-5f);
    ASSERT_NEAR(a.y, b.y, 1e-5f);
    ASSERT_NEAR(a.z, b.z, 1e-5f);
    ASSERT_NEAR(a.w, b.w, 1e-5f);
}

TEST(texture, sample_rgba_flips_v_axis)
{
    // 1×2: image row0 (top)=(255,0,0,200), row1 (bottom)=(0,0,255,100).
    // OBJ v=0 (texture bottom) → flip → samples image row1 → alpha=100/255.
    Texture t = make_tex(1, 2, { 255, 0, 0, 200, 0, 0, 255, 100 });
    vec4 c = t.sample_rgba(0.0f, 0.0f);
    ASSERT_NEAR(c.w, 100.0f / 255.0f, 1e-4f);
    ASSERT_NEAR(c.z, 1.0f, 1e-4f); // blue channel confirms row1 was sampled
}

// ─── wrap modes ───────────────────────────────────────────────────────────────
// 2×1 reference texture: texel0 = red (u→0), texel1 = blue (u→1). With width 2 the
// bilinear fx = u*(width-1) = u, so u=0 is pure red, u=1 pure blue, u=0.5 the midpoint.
static Texture wrap_ref()
{
    return make_tex(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
}

static void expect_rgb_near(const vec3 &a, const vec3 &b, float eps = 1e-4f)
{
    ASSERT_NEAR(a.x, b.x, eps);
    ASSERT_NEAR(a.y, b.y, eps);
    ASSERT_NEAR(a.z, b.z, eps);
}

TEST(texture_wrap, default_is_repeat)
{
    Texture t;
    ASSERT_TRUE(t.wrap_s == WrapMode::Repeat);
    ASSERT_TRUE(t.wrap_t == WrapMode::Repeat);
}

TEST(texture_wrap, explicit_repeat_matches_default_for_out_of_range)
{
    // Setting Repeat explicitly must reproduce the historical fract() exactly.
    Texture t = wrap_ref();
    t.wrap_s = WrapMode::Repeat;
    t.wrap_t = WrapMode::Repeat;
    expect_rgb_near(t.sample_rgb(1.25f, 0.5f), t.sample_rgb(0.25f, 0.5f));
    expect_rgb_near(t.sample_rgb(-0.75f, 0.5f), t.sample_rgb(0.25f, 0.5f));
}

TEST(texture_wrap, clamp_holds_high_edge_texel)
{
    // u=2.0 clamps to 1.0 → pure blue (edge held), NOT red as Repeat would wrap it to.
    Texture t = wrap_ref();
    t.wrap_s = WrapMode::Clamp;
    expect_rgb_near(t.sample_rgb(2.0f, 0.5f), t.sample_rgb(1.0f, 0.5f));
    ASSERT_NEAR(t.sample_rgb(2.0f, 0.5f).z, 1.0f, 1e-4f); // blue, no opposite-edge bleed
    ASSERT_NEAR(t.sample_rgb(2.0f, 0.5f).x, 0.0f, 1e-4f);
}

TEST(texture_wrap, clamp_holds_low_edge_texel)
{
    // u=-0.5 clamps to 0.0 → pure red.
    Texture t = wrap_ref();
    t.wrap_s = WrapMode::Clamp;
    expect_rgb_near(t.sample_rgb(-0.5f, 0.5f), t.sample_rgb(0.0f, 0.5f));
    ASSERT_NEAR(t.sample_rgb(-0.5f, 0.5f).x, 1.0f, 1e-4f);
}

TEST(texture_wrap, clamp_extreme_coords_stay_in_bounds)
{
    // Huge magnitudes must fold to the edge texel with no OOB read / NaN.
    Texture t = wrap_ref();
    t.wrap_s = WrapMode::Clamp;
    expect_rgb_near(t.sample_rgb(1e6f, 0.5f), t.sample_rgb(1.0f, 0.5f));
    expect_rgb_near(t.sample_rgb(-1e6f, 0.5f), t.sample_rgb(0.0f, 0.5f));
}

TEST(texture_wrap, clamp_interior_unchanged)
{
    // In-range coords are identical to Repeat.
    Texture t = wrap_ref();
    Texture r = wrap_ref();
    t.wrap_s = WrapMode::Clamp;
    expect_rgb_near(t.sample_rgb(0.5f, 0.5f), r.sample_rgb(0.5f, 0.5f));
}

TEST(texture_wrap, mirror_reflects_in_first_repeat)
{
    // [1,2] mirrors [0,1]: u=1.25→0.75, u=1.75→0.25.
    Texture t = wrap_ref();
    t.wrap_s = WrapMode::Mirror;
    expect_rgb_near(t.sample_rgb(1.25f, 0.5f), t.sample_rgb(0.75f, 0.5f));
    expect_rgb_near(t.sample_rgb(1.75f, 0.5f), t.sample_rgb(0.25f, 0.5f));
}

TEST(texture_wrap, mirror_identity_in_second_period)
{
    // [2,3] is identity again; u=2.25→0.25, u=2.0→0.0.
    Texture t = wrap_ref();
    t.wrap_s = WrapMode::Mirror;
    expect_rgb_near(t.sample_rgb(2.25f, 0.5f), t.sample_rgb(0.25f, 0.5f));
    expect_rgb_near(t.sample_rgb(2.0f, 0.5f), t.sample_rgb(0.0f, 0.5f));
}

TEST(texture_wrap, mirror_reflects_negative)
{
    // u=-0.25 mirrors to 0.25.
    Texture t = wrap_ref();
    t.wrap_s = WrapMode::Mirror;
    expect_rgb_near(t.sample_rgb(-0.25f, 0.5f), t.sample_rgb(0.25f, 0.5f));
}

TEST(texture_wrap, per_axis_independent)
{
    // wrap_s=Clamp, wrap_t=Repeat: u must clamp while v wraps, on a texture that depends
    // on both axes. sample(2.0, 1.3) == sample(1.0, 0.3): u 2→clamp 1, v 1.3→wrap 0.3.
    Texture t = make_tex(2, 2, { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 });
    t.wrap_s = WrapMode::Clamp;
    t.wrap_t = WrapMode::Repeat;
    expect_rgb_near(t.sample_rgb(2.0f, 1.3f), t.sample_rgb(1.0f, 0.3f));
}

TEST(texture_wrap, sample_rgba_clamp_holds_edge_alpha)
{
    // The rgba fold is a separate copy; confirm it clamps identically incl. alpha.
    Texture t = make_tex(2, 1, { 255, 0, 0, 200, 0, 0, 255, 100 });
    t.wrap_s = WrapMode::Clamp;
    vec4 a = t.sample_rgba(2.0f, 0.5f);
    vec4 b = t.sample_rgba(1.0f, 0.5f);
    ASSERT_NEAR(a.w, b.w, 1e-4f);
    ASSERT_NEAR(a.w, 100.0f / 255.0f, 1e-4f);
}

TEST(texture_wrap, sample_rgba_mirror_matches_reflection)
{
    Texture t = make_tex(2, 1, { 255, 0, 0, 200, 0, 0, 255, 100 });
    t.wrap_s = WrapMode::Mirror;
    vec4 a = t.sample_rgba(1.25f, 0.5f);
    vec4 b = t.sample_rgba(0.75f, 0.5f);
    ASSERT_NEAR(a.x, b.x, 1e-4f);
    ASSERT_NEAR(a.z, b.z, 1e-4f);
    ASSERT_NEAR(a.w, b.w, 1e-4f);
}

TEST(texture_wrap, clamp_on_v_folds_before_flip)
{
    // 1×2: image row0 (top)=red, row1 (bottom)=blue. wrap_t=Clamp must fold v into [0,1]
    // and THEN v-flip (same order as Repeat). v=2.0 → clamp 1.0 → flip 0 → row0 (red);
    // v=-0.5 → clamp 0.0 → flip 1 → row1 (blue).
    Texture t = make_tex(1, 2, { 255, 0, 0, 255, 0, 0, 255, 255 });
    t.wrap_t = WrapMode::Clamp;
    ASSERT_NEAR(t.sample_rgb(0.0f, 2.0f).x, 1.0f, 1e-4f);
    ASSERT_NEAR(t.sample_rgb(0.0f, -0.5f).z, 1.0f, 1e-4f);
}

TEST(texture_wrap, degenerate_1x1_safe_under_all_modes)
{
    // A 1×1 texture must return its single texel for any coordinate under any mode.
    for (WrapMode m : { WrapMode::Repeat, WrapMode::Clamp, WrapMode::Mirror })
    {
        Texture t = solid(255, 128, 0);
        t.wrap_s = m;
        t.wrap_t = m;
        for (float u : { -1e6f, -0.3f, 0.5f, 1.7f, 1e6f })
        {
            vec3 c = t.sample_rgb(u, u);
            ASSERT_NEAR(c.x, 1.0f, 1e-4f);
            ASSERT_NEAR(c.y, 128.0f / 255.0f, 1e-4f);
            ASSERT_NEAR(c.z, 0.0f, 1e-4f);
        }
    }
}

TEST(texture_load, load_failure_preserves_previous_data)
{
    Texture t = solid(255, 0, 0);
    const int old_w = t.width;
    const int old_h = t.height;
    const std::vector<uint8_t> old_pixels = t.pixels;

    ASSERT_FALSE(t.load(tmp_path("rasterminal_missing_texture_XXXXX.png")));
    ASSERT_EQ(t.width, old_w);
    ASSERT_EQ(t.height, old_h);
    if (t.pixels != old_pixels)
    {
        ASSERT_FAIL("load() should leave pixels unchanged on failure");
    }
}

TEST(texture_load, load_from_memory_failure_preserves_previous_data)
{
    Texture t = solid(0, 255, 0);
    const int old_w = t.width;
    const int old_h = t.height;
    const std::vector<uint8_t> old_pixels = t.pixels;

    const uint8_t garbage[] = { 0x00, 0x01, 0x02, 0x03, 0xDE, 0xAD, 0xBE, 0xEF };
    ASSERT_FALSE(t.load_from_memory(garbage, sizeof(garbage)));
    ASSERT_EQ(t.width, old_w);
    ASSERT_EQ(t.height, old_h);
    if (t.pixels != old_pixels)
    {
        ASSERT_FAIL("load_from_memory() should leave pixels unchanged on failure");
    }
}

// ─── success paths for load() and load_from_memory() ─────────────────────────

TEST(texture_load, load_from_memory_valid_image_populates_texture)
{
    // Exercises the success branch of load_from_memory (lines 44-49 in texture.cpp).
    Texture t;
    ASSERT_TRUE(t.load_from_memory(k1x1_red_bmp, sizeof(k1x1_red_bmp)));
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(t.width, 1);
    ASSERT_EQ(t.height, 1);
    ASSERT_EQ(t.pixels.size(), size_t{ 4 });
    ASSERT_EQ(t.pixels[0], uint8_t{ 255 }); // R
    ASSERT_EQ(t.pixels[1], uint8_t{ 0 });   // G
    ASSERT_EQ(t.pixels[2], uint8_t{ 0 });   // B
}

TEST(texture_load, load_valid_file_populates_texture)
{
    // Exercises the success branch of load() (lines 26-30 in texture.cpp).
    TmpFile f(tmp_path("rasterminal_test_tex.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    Texture t;
    ASSERT_TRUE(t.load(f.path));
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(t.width, 1);
    ASSERT_EQ(t.height, 1);
    ASSERT_EQ(t.pixels.size(), size_t{ 4 });
    ASSERT_EQ(t.pixels[0], uint8_t{ 255 }); // R
    // Verify sampling works on the loaded data.
    vec3 c = t.sample_rgb(0.5f, 0.5f);
    ASSERT_NEAR(c.x, 1.0f, 1e-4f);
    ASSERT_NEAR(c.y, 0.0f, 1e-4f);
    ASSERT_NEAR(c.z, 0.0f, 1e-4f);
}

TEST(texture_load, load_from_memory_success_overwrites_previous)
{
    // Complements load_from_memory_failure_preserves_previous_data: a SUCCESSFUL
    // load must replace the existing texture, not leave the old data in place.
    Texture t = solid(0, 255, 0); // green
    ASSERT_TRUE(t.load_from_memory(k1x1_red_bmp, sizeof(k1x1_red_bmp)));
    ASSERT_EQ(t.pixels[0], uint8_t{ 255 }); // R now 255 (was 0)
    ASSERT_EQ(t.pixels[1], uint8_t{ 0 });   // G now 0 (was 255)
}

TEST(texture_load, load_from_memory_oversized_returns_false)
{
    // size > INT_MAX triggers the early-return before stbi is called.
    // The actual buffer need not be that large; only the size argument matters.
    Texture t;
    const uint8_t dummy = 0;
    ASSERT_FALSE(t.load_from_memory(&dummy, static_cast<size_t>(INT_MAX) + 1));
    ASSERT_FALSE(t.valid());
}

TEST(texture, sample_rgba_v_near_one_maps_to_top_image_row)
{
    // 1×2: image row0 (top) = red/alpha=200, row1 (bottom) = blue/alpha=100.
    // v=0.999 → wrap→0.999 → flip→0.001 → fy≈0 → mostly image row0.
    Texture t = make_tex(1, 2, { 255, 0, 0, 200, 0, 0, 255, 100 });
    vec4 c = t.sample_rgba(0.0f, 0.999f);
    ASSERT_TRUE(c.x > 0.9f);                   // mostly red (row0)
    ASSERT_TRUE(c.z < 0.1f);                   // little blue
    ASSERT_TRUE(c.w > 200.0f / 255.0f * 0.9f); // alpha mostly from row0
}

TEST(texture_load, load_from_memory_success_updates_dimensions)
{
    // Previous overwrite test uses same-size textures; here we verify width/height
    // are actually reassigned when the new image has different dimensions.
    Texture t = make_tex(2, 2, { 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255 });
    ASSERT_TRUE(t.load_from_memory(k1x1_red_bmp, sizeof(k1x1_red_bmp)));
    ASSERT_EQ(t.width, 1);
    ASSERT_EQ(t.height, 1);
    ASSERT_EQ(t.pixels.size(), size_t{ 4 });
}

TEST(texture_load, load_empty_path_returns_false)
{
    Texture t;
    ASSERT_FALSE(t.load(""));
    ASSERT_FALSE(t.valid());
}

// ─── very large negative UV precision ────────────────────────────────────────
// floor(-1000.75f) = -1001.0f exactly in float32; frac = 0.25f — wrapping must
// produce the same result as u = 0.25f.

TEST(texture, uv_wraps_very_large_negative_rgb)
{
    Texture t = make_tex(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 a = t.sample_rgb(0.25f, 0.5f);
    vec3 b = t.sample_rgb(-1000.75f, 0.5f);
    ASSERT_NEAR(a.x, b.x, 1e-4f);
    ASSERT_NEAR(a.z, b.z, 1e-4f);
}

TEST(texture, uv_wraps_very_large_negative_rgba)
{
    Texture t = make_tex(2, 1, { 255, 0, 0, 200, 0, 0, 255, 100 });
    vec4 a = t.sample_rgba(0.25f, 0.5f);
    vec4 b = t.sample_rgba(-1000.75f, 0.5f);
    ASSERT_NEAR(a.x, b.x, 1e-4f);
    ASSERT_NEAR(a.z, b.z, 1e-4f);
    ASSERT_NEAR(a.w, b.w, 1e-4f);
}

// ─── is_grayscale (bump/normal classification) ───────────────────────────────

TEST(texture_bump, is_grayscale_true_for_achromatic)
{
    Texture t = make_tex(2, 2, { 10, 10, 10, 255, 80, 80, 80, 255, 200, 200, 200, 255, 255, 255, 255, 255 });
    ASSERT_TRUE(is_grayscale(t));
}

TEST(texture_bump, is_grayscale_false_for_chromatic)
{
    // One texel with R != G ⇒ not grayscale (this is the early-out texel).
    Texture t = make_tex(2, 1, { 50, 50, 50, 255, 10, 20, 30, 255 });
    ASSERT_FALSE(is_grayscale(t));
}

TEST(texture_bump, is_grayscale_false_for_empty)
{
    Texture t;
    ASSERT_FALSE(is_grayscale(t));
}

// ─── height_to_normal_map ────────────────────────────────────────────────────

TEST(texture_bump, height_to_normal_flat_is_plus_z)
{
    // Constant height ⇒ zero gradient ⇒ tangent-space normal (0,0,1) ⇒ encoded (128,128,255).
    Texture src = make_tex(2, 2, { 128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255 });
    Texture n = height_to_normal_map(src, 'l', 1.0f);
    ASSERT_EQ(n.width, 2);
    ASSERT_EQ(n.height, 2);
    for (size_t i = 0; i + 4 <= n.pixels.size(); i += 4)
    {
        ASSERT_EQ(n.pixels[i + 0], uint8_t{ 128 });
        ASSERT_EQ(n.pixels[i + 1], uint8_t{ 128 });
        ASSERT_EQ(n.pixels[i + 2], uint8_t{ 255 });
        ASSERT_EQ(n.pixels[i + 3], uint8_t{ 255 });
    }
}

TEST(texture_bump, height_to_normal_u_ramp_tilts_negative_x)
{
    // Height rises along +u; the heightfield normal tilts toward -u (n.x<0 ⇒ R<128).
    // No v variation (height 1) ⇒ n.y==0 ⇒ G==128; z stays dominant ⇒ B>128.
    Texture src = make_tex(3, 1, { 0, 0, 0, 255, 128, 128, 128, 255, 255, 255, 255, 255 });
    Texture n = height_to_normal_map(src, 'l', 1.0f);
    ASSERT_TRUE(n.pixels[4 + 0] < 128); // center texel (x=1): tilted toward -x
    ASSERT_EQ(n.pixels[4 + 1], uint8_t{ 128 });
    ASSERT_TRUE(n.pixels[4 + 2] > 128);
}

TEST(texture_bump, height_to_normal_bm_scales_tilt)
{
    // A larger bump multiplier steepens the gradient ⇒ R pushed further below 128.
    Texture src = make_tex(3, 1, { 0, 0, 0, 255, 128, 128, 128, 255, 255, 255, 255, 255 });
    Texture n1 = height_to_normal_map(src, 'l', 1.0f);
    Texture n4 = height_to_normal_map(src, 'l', 4.0f);
    ASSERT_TRUE(n4.pixels[4] < n1.pixels[4]);
}

TEST(texture_bump, height_to_normal_imfchan_selects_channel)
{
    // R ramps, G constant. -imfchan r reads the ramp (tilt); -imfchan g reads the flat (no tilt).
    Texture src = make_tex(3, 1, { 0, 100, 0, 255, 128, 100, 0, 255, 255, 100, 0, 255 });
    Texture rr = height_to_normal_map(src, 'r', 1.0f);
    Texture gg = height_to_normal_map(src, 'g', 1.0f);
    ASSERT_TRUE(rr.pixels[4] < 128);
    ASSERT_EQ(gg.pixels[4], uint8_t{ 128 });
}

TEST(texture_bump, height_to_normal_preserves_wrap_modes)
{
    Texture src = make_tex(2, 2, { 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255 });
    src.wrap_s = WrapMode::Clamp;
    src.wrap_t = WrapMode::Mirror;
    Texture n = height_to_normal_map(src, 'l', 1.0f);
    ASSERT_TRUE(n.wrap_s == WrapMode::Clamp);
    ASSERT_TRUE(n.wrap_t == WrapMode::Mirror);
}

// ─── is_grayscale edge cases ──────────────────────────────────────────────────

TEST(texture_bump, is_grayscale_ignores_alpha)
{
    // Classification is on RGB only; varying alpha over equal RGB is still grayscale (a height
    // map with an alpha channel must not be misread as a chromatic normal map).
    Texture t = make_tex(2, 1, { 60, 60, 60, 10, 60, 60, 60, 250 });
    ASSERT_TRUE(is_grayscale(t));
}

TEST(texture_bump, is_grayscale_detects_chromatic_last_texel)
{
    // The early-out must not false-negate: a chromatic texel at the very end still returns false.
    Texture t = make_tex(3, 1, { 40, 40, 40, 255, 90, 90, 90, 255, 90, 90, 10, 255 });
    ASSERT_FALSE(is_grayscale(t));
}

TEST(texture_bump, is_grayscale_true_1x1)
{
    Texture t = make_tex(1, 1, { 77, 77, 77, 255 });
    ASSERT_TRUE(is_grayscale(t));
}

TEST(texture_bump, is_grayscale_tolerates_small_chroma)
{
    // A grayscale height map saved lossily (JPEG) nudges channels apart by a few levels; within
    // the tolerance it is still recognised as grayscale.
    Texture t = make_tex(2, 1, { 100, 103, 98, 255, 50, 50, 54, 255 });
    ASSERT_TRUE(is_grayscale(t));
}

TEST(texture_bump, is_grayscale_rejects_beyond_tolerance)
{
    // A channel spread well above the tolerance is chromatic (a real normal map's spread is far
    // larger still).
    Texture t = make_tex(1, 1, { 100, 100, 120, 255 });
    ASSERT_FALSE(is_grayscale(t));
}

// ─── height_to_normal_map edge cases ─────────────────────────────────────────

TEST(texture_bump, height_to_normal_v_ramp_tilts_positive_y)
{
    // Height rises along +v (down the image rows) ⇒ the heightfield normal's y-component is
    // positive (green > 128). Locks the dy_sign / green-channel axis. No u variation ⇒ R==128.
    Texture src = make_tex(1, 3, { 0, 0, 0, 255, 128, 128, 128, 255, 255, 255, 255, 255 });
    Texture n = height_to_normal_map(src, 'l', 1.0f);
    ASSERT_EQ(n.pixels[4 + 0], uint8_t{ 128 }); // center texel (y=1)
    ASSERT_TRUE(n.pixels[4 + 1] > 128);
    ASSERT_TRUE(n.pixels[4 + 2] > 128);
}

TEST(texture_bump, height_to_normal_z_channel_always_ge_128)
{
    // n.z = 1 before normalization ⇒ always positive ⇒ encoded blue ≥ 128 everywhere, even
    // across a steep full-range step at a high bump multiplier.
    Texture src = make_tex(4, 1, { 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255 });
    Texture n = height_to_normal_map(src, 'l', 4.0f);
    for (size_t i = 0; i + 4 <= n.pixels.size(); i += 4)
    {
        ASSERT_TRUE(n.pixels[i + 2] >= 128);
    }
}

TEST(texture_bump, height_to_normal_clamps_huge_bm)
{
    // A hostile finite-but-absurd -bm must be bounded so the gradient can't overflow to inf and
    // collapse normalize() to NaN: it yields the same result as the clamp ceiling, not garbage.
    Texture src = make_tex(3, 1, { 0, 0, 0, 255, 128, 128, 128, 255, 255, 255, 255, 255 });
    Texture huge = height_to_normal_map(src, 'l', 1e38f);
    Texture cap = height_to_normal_map(src, 'l', 1e6f);
    ASSERT_EQ(huge.pixels[4 + 0], cap.pixels[4 + 0]);
    ASSERT_EQ(huge.pixels[4 + 1], cap.pixels[4 + 1]);
    ASSERT_TRUE(huge.pixels[4 + 2] >= 128); // blue stays valid (no NaN collapse)
}

TEST(texture_bump, height_to_normal_bm_zero_is_flat)
{
    // bm scales the gradient; bm=0 ⇒ no relief ⇒ flat (128,128,255) regardless of height.
    Texture src = make_tex(3, 1, { 0, 0, 0, 255, 200, 200, 200, 255, 255, 255, 255, 255 });
    Texture n = height_to_normal_map(src, 'l', 0.0f);
    ASSERT_EQ(n.pixels[4 + 0], uint8_t{ 128 });
    ASSERT_EQ(n.pixels[4 + 1], uint8_t{ 128 });
    ASSERT_EQ(n.pixels[4 + 2], uint8_t{ 255 });
}

TEST(texture_bump, height_to_normal_preserves_dimensions)
{
    constexpr int dw = 4;
    constexpr int dh = 2;
    const size_t bytes = static_cast<size_t>(dw) * static_cast<size_t>(dh) * 4;
    Texture src = make_tex(dw, dh, std::vector<uint8_t>(bytes, 100));
    Texture n = height_to_normal_map(src, 'l', 1.0f);
    ASSERT_EQ(n.width, dw);
    ASSERT_EQ(n.height, dh);
    ASSERT_EQ(n.pixels.size(), bytes);
}

TEST(texture_bump, height_to_normal_1x1_is_flat_no_crash)
{
    // Degenerate single texel: wrap_index folds every neighbour to the one texel ⇒ zero
    // gradient ⇒ flat normal, no out-of-bounds read.
    Texture src = make_tex(1, 1, { 200, 200, 200, 255 });
    Texture n = height_to_normal_map(src, 'l', 1.0f);
    ASSERT_EQ(n.width, 1);
    ASSERT_EQ(n.height, 1);
    ASSERT_EQ(n.pixels[0], uint8_t{ 128 });
    ASSERT_EQ(n.pixels[1], uint8_t{ 128 });
    ASSERT_EQ(n.pixels[2], uint8_t{ 255 });
}

TEST(texture_bump, height_to_normal_imfchan_alpha)
{
    // -imfchan m reads the alpha channel; with constant RGB, 'l' (luminance) would be flat.
    Texture src = make_tex(3, 1, { 100, 100, 100, 0, 100, 100, 100, 128, 100, 100, 100, 255 });
    Texture mm = height_to_normal_map(src, 'm', 1.0f);
    Texture ll = height_to_normal_map(src, 'l', 1.0f);
    ASSERT_TRUE(mm.pixels[4] < 128);         // alpha ramp ⇒ tilt
    ASSERT_EQ(ll.pixels[4], uint8_t{ 128 }); // constant luminance ⇒ flat
}

TEST(texture_bump, height_to_normal_imfchan_blue_and_z_alias)
{
    // -imfchan b reads blue; z (depth) aliases the blue channel per the MTL convention.
    Texture src = make_tex(3, 1, { 50, 50, 0, 255, 50, 50, 128, 255, 50, 50, 255, 255 });
    Texture b = height_to_normal_map(src, 'b', 1.0f);
    Texture z = height_to_normal_map(src, 'z', 1.0f);
    ASSERT_TRUE(b.pixels[4] < 128);
    ASSERT_EQ(b.pixels[4], z.pixels[4]);
}

TEST(texture_bump, height_to_normal_unknown_imfchan_defaults_to_luminance)
{
    Texture src = make_tex(3, 1, { 0, 0, 0, 255, 128, 128, 128, 255, 255, 255, 255, 255 });
    Texture x = height_to_normal_map(src, 'x', 1.0f);
    Texture l = height_to_normal_map(src, 'l', 1.0f);
    ASSERT_EQ(x.pixels[4], l.pixels[4]); // unknown char falls through to the luminance branch
}
