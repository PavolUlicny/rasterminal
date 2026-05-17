#include "test.h"
#include "loader_util.h"
#include "inline_bmp.h"
#include "../src/texture.h"

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
        ASSERT_FAIL("load() should leave pixels unchanged on failure");
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
        ASSERT_FAIL("load_from_memory() should leave pixels unchanged on failure");
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
