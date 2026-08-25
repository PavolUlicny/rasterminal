#include "tests/test.h"
#include "tests/rasterize_test_util.h"
#include "src/render/rasterize.h"

#include <initializer_list>

// helper

// Build an in-memory Texture without disk I/O.
static Texture make_vcol_tex(int w, int h, std::initializer_list<int> rgba)
{
    Texture t;
    t.width = w;
    t.height = h;
    t.pixels.reserve(rgba.size());
    for (int v : rgba)
    {
        t.pixels.push_back(static_cast<uint8_t>(v));
    }
    return t;
}

// Ambient-only Phong wrapper over the canonical triangle. Callers supply vertex
// colors, enable flag, clip w, material, and optional texture.
static void rast_phong_vcol(
    Framebuffer &fb,
    vec3 vcola,
    vec3 vcolb,
    vec3 vcolc,
    bool has_vcol,
    float wa,
    float wb,
    float wc,
    const Material &mat,
    const Texture *tex
)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    rasterize_phong(
        fb, sa, sb, sc, wa, wb, wc, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f, 1.0f,
        1.0f, vcola, vcolb, vcolc, has_vcol, eye, nullptr, 0, ambient, mat, tex, nullptr, nullptr, 0, 19
    );
}

// Canonical triangle: sa=(4,2), sb=(36,2), sc=(20,18) on 40×20 framebuffer.
// Key pixel centres and pre-computed screen-space barycentric weights:
//
//   Pixel (20,10): ba=0.21875, bb=0.25,  bc=0.53125
//   Pixel (12,10): ba=0.46875, bb=0,     bc=0.53125

TEST(rasterize_phong, has_vcol_false_ignores_vertex_colors)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Material mat{};
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    vec3 red{ 1.0f, 0.0f, 0.0f };
    rast_phong_vcol(fb, red, red, red, false, 1.0f, 1.0f, 1.0f, mat, nullptr);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    // White (1.0 per channel) rolls off through the soft-knee tonemap to ~226.
    if (c.r < 220 || c.g < 220 || c.b < 220)
    {
        ASSERT_FAIL(
            "has_vcol=false: expected white, got (" + std::to_string(static_cast<int>(c.r)) + "," +
            std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

TEST(rasterize_phong, has_vcol_true_tints_ambient)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Material mat{};
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    vec3 red{ 1.0f, 0.0f, 0.0f };
    rast_phong_vcol(fb, red, red, red, true, 1.0f, 1.0f, 1.0f, mat, nullptr);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    // Full red (1.0) rolls off through the soft-knee tonemap to ~226.
    if (c.r < 220)
    {
        ASSERT_FAIL("has_vcol=true red: R too low (" + std::to_string(static_cast<int>(c.r)) + "), expected ~226");
    }
    if (c.g > 5 || c.b > 5)
    {
        ASSERT_FAIL("has_vcol=true red: G/B too high, expected near-zero");
    }
}

TEST(rasterize_phong, white_vcol_matches_no_vcol)
{
    Material mat{};
    mat.ambient = { 0.7f, 0.5f, 0.3f };
    mat.diffuse = { 0.7f, 0.5f, 0.3f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    vec3 white{ 1.0f, 1.0f, 1.0f };

    Framebuffer fb_off(40, 20, /*headless=*/true), fb_on(40, 20, /*headless=*/true);
    rast_phong_vcol(fb_off, white, white, white, false, 1.0f, 1.0f, 1.0f, mat, nullptr);
    rast_phong_vcol(fb_on, white, white, white, true, 1.0f, 1.0f, 1.0f, mat, nullptr);

    ASSERT_TRUE(was_drawn(fb_off, 20, 10));
    ASSERT_TRUE(was_drawn(fb_on, 20, 10));
    assert_pixel_near(fb_on, 20, 10, fb_off.get_pixel(20, 10), 2);
}

// RGB vcol interpolated at centroid with equal w=1.
// At (20,10): ba=0.21875, bb=0.25, bc=0.53125.
// Expected: vcol ≈ (0.219, 0.250, 0.531) → pixel ≈ (56, 64, 135) ±5.
TEST(rasterize_phong, vcol_per_vertex_interpolation_equal_w)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Material mat{};
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    vec3 red{ 1.0f, 0.0f, 0.0f }, green{ 0.0f, 1.0f, 0.0f }, blue{ 0.0f, 0.0f, 1.0f };
    rast_phong_vcol(fb, red, green, blue, true, 1.0f, 1.0f, 1.0f, mat, nullptr);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color expected{ 56, 64, 135 };
    assert_pixel_near(fb, 20, 10, expected, 5);
}

// vcola=red, vcolb=red, vcolc=blue; wa=wb=10, wc=1.
// Pixel (12,10): pwa=0.046875, pwb=0, pwc=0.53125 → w_corr≈1.7297
// vcol_b = 0.53125*1.7297 ≈ 0.919 → B≥200; R≤60.
TEST(rasterize_phong, vcol_perspective_correct_unequal_w)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Material mat{};
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    vec3 red{ 1.0f, 0.0f, 0.0f }, blue{ 0.0f, 0.0f, 1.0f };
    rast_phong_vcol(fb, red, red, blue, true, 10.0f, 10.0f, 1.0f, mat, nullptr);

    ASSERT_TRUE(was_drawn(fb, 12, 10));
    Color c = fb.get_pixel(12, 10);
    if (c.b < 200)
    {
        ASSERT_FAIL(
            "B too low (" + std::to_string(static_cast<int>(c.b)) +
            "): perspective-correct vcol should bias hard toward blue"
        );
    }
    if (c.r > 60)
    {
        ASSERT_FAIL(
            "R too high (" + std::to_string(static_cast<int>(c.r)) + "): far red vertices should contribute little"
        );
    }
}

// Red vertex color times a green diffuse texture yields a drawn black pixel.
// This catches tinting a fresh material copy instead of the texture-modified one.
TEST(rasterize_phong, vcol_combined_with_diffuse_texture)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Material mat{};
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    Texture green_tex = make_vcol_tex(1, 1, { 0, 255, 0, 255 });
    vec3 red{ 1.0f, 0.0f, 0.0f };
    rast_phong_vcol(fb, red, red, red, true, 1.0f, 1.0f, 1.0f, mat, &green_tex);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 5 || c.g > 5 || c.b > 5)
    {
        ASSERT_FAIL(
            "expected black (red vcol * green tex = 0 per channel), got (" + std::to_string(static_cast<int>(c.r)) +
            "," + std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}
