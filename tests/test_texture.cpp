#include "test.h"
#include "../src/texture.h"

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
    return make_tex(1, 1, {r, g, b, 255});
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

TEST(texture, uv_wraps_at_integer_boundary)
{
    // 2×1: left pixel = red, right pixel = blue.
    // u=0.25 and u=1.25 must produce the same sample.
    Texture t = make_tex(2, 1, {255, 0, 0, 255, 0, 0, 255, 255});
    vec3 a = t.sample_rgb(0.25f, 0.5f);
    vec3 b = t.sample_rgb(1.25f, 0.5f);
    ASSERT_NEAR(a.x, b.x, 1e-5f);
    ASSERT_NEAR(a.z, b.z, 1e-5f);
}

TEST(texture, uv_wraps_for_negative_coords)
{
    // u=-0.75 wraps to 0.25 (−0.75 − floor(−0.75) = −0.75 − (−1) = 0.25).
    Texture t = make_tex(2, 1, {255, 0, 0, 255, 0, 0, 255, 255});
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
    Texture t = make_tex(2, 1, {255, 0, 0, 255, 0, 0, 255, 255});
    vec3 c = t.sample_rgb(0.5f, 0.5f);
    ASSERT_NEAR(c.x, 0.5f, 1e-4f);
    ASSERT_NEAR(c.y, 0.0f, 1e-4f);
    ASSERT_NEAR(c.z, 0.5f, 1e-4f);
}

TEST(texture, bilinear_midpoint_averages_vertical_neighbors)
{
    // 1×2: row0 (top image) = red, row1 (bottom image) = blue.
    // v=0.5 (OBJ mid) → after flip: 0.5 → fy=0.5 → lerp(red,blue,0.5) = (0.5,0,0.5).
    Texture t = make_tex(1, 2, {255, 0, 0, 255, 0, 0, 255, 255});
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
    Texture t = make_tex(1, 2, {255, 0, 0, 255, 0, 0, 255, 255});
    vec3 c = t.sample_rgb(0.0f, 0.0f);
    ASSERT_NEAR(c.x, 0.0f, 1e-4f); // low red
    ASSERT_NEAR(c.z, 1.0f, 1e-4f); // full blue
}

TEST(texture, v_near_one_maps_toward_top_image_row)
{
    // OBJ v→1 (texture top) → flip → v→0 → fy→0 → image row 0 = red.
    Texture t = make_tex(1, 2, {255, 0, 0, 255, 0, 0, 255, 255});
    vec3 c = t.sample_rgb(0.0f, 0.999f);
    ASSERT_TRUE(c.x > 0.9f); // mostly red
    ASSERT_TRUE(c.z < 0.1f); // little blue
}

// ─── Texture::load / load_from_memory ────────────────────────────────────────

TEST(texture_load, load_valid_file)
{
    Texture t;
    ASSERT_TRUE(t.load("models/gltf/DuckCM.png"));
    ASSERT_TRUE(t.valid());
    ASSERT_TRUE(t.width > 0);
    ASSERT_TRUE(t.height > 0);
}

TEST(texture_load, load_nonexistent_returns_false)
{
    Texture t;
    ASSERT_FALSE(t.load("/tmp/rasterminal_nonexistent_XXXXX.png"));
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
    const uint8_t data[] = {1, 2, 3};
    ASSERT_FALSE(t.load_from_memory(data, 0));
    ASSERT_FALSE(t.valid());
}

TEST(texture_load, load_from_memory_garbage_returns_false)
{
    Texture t;
    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_FALSE(t.load_from_memory(garbage, sizeof(garbage)));
    ASSERT_FALSE(t.valid());
}

TEST(texture_load, load_from_memory_valid_png)
{
    std::FILE *f = std::fopen("models/gltf/DuckCM.png", "rb");
    ASSERT_TRUE(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    const size_t sz = static_cast<size_t>(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz);
    const size_t nread = std::fread(buf.data(), 1, sz, f);
    ASSERT_EQ(nread, sz);
    std::fclose(f);

    Texture t;
    ASSERT_TRUE(t.load_from_memory(buf.data(), sz));
    ASSERT_TRUE(t.valid());
    ASSERT_TRUE(t.width > 0);
    ASSERT_TRUE(t.height > 0);
}
