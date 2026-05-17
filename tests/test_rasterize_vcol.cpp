#include "test.h"
#include "rasterize_test_util.h"
#include "../src/rasterize.h"

#include <initializer_list>

// ─── helper ───────────────────────────────────────────────────────────────────

// Build an in-memory Texture without disk I/O.
static Texture make_vcol_tex(int w, int h, std::initializer_list<int> rgba)
{
    Texture t;
    t.width = w;
    t.height = h;
    t.pixels.reserve(rgba.size());
    for (int v : rgba)
        t.pixels.push_back(static_cast<uint8_t>(v));
    return t;
}

// rasterize_phong() wrapper with ambient-only defaults so each test is concise.
//   ambient=(1,1,1), n_lights=0, ao=1 at all vertices
//   normal=(0,0,1), tangent=(1,0,0), eye=(20,10,-100)
//   Canonical triangle: sa=(4,2), sb=(36,2), sc=(20,18) on 40×20 framebuffer
//
// Caller supplies: vcol per vertex, has_vcol flag, w per vertex, mat, optional tex.
static void rast_phong_vcol(
    Framebuffer &fb, vec3 vcola, vec3 vcolb, vec3 vcolc, bool has_vcol, float wa, float wb, float wc,
    const Material &mat, const Texture *tex
)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    rasterize_phong(
        fb, sa, sb, sc, wa, wb, wc, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f, 1.0f,
        1.0f, vcola, vcolb, vcolc, has_vcol, eye, nullptr, 0, ambient, mat, tex, nullptr, nullptr, nullptr, 0, 19
    );
}

// ─── vertex-color in rasterize_phong() ───────────────────────────────────────
//
// Canonical triangle: sa=(4,2), sb=(36,2), sc=(20,18) on 40×20 framebuffer.
// Key pixel centres and pre-computed screen-space barycentric weights:
//
//   Pixel (20,10): ba=0.21875, bb=0.25,  bc=0.53125
//   Pixel (12,10): ba=0.46875, bb=0,     bc=0.53125

// V1: has_vcol=false: red vcol on all vertices must NOT tint a white result.
// Catches: has_vcol flag silently ignored (always-on tinting).
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
    if (c.r < 250 || c.g < 250 || c.b < 250)
        ASSERT_FAIL(
            "has_vcol=false: expected white, got (" + std::to_string(static_cast<int>(c.r)) + "," +
            std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
}

// V2: has_vcol=true: red vcol on all vertices must produce a red pixel.
// Catches: the has_vcol branch never executes.
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
    if (c.r < 250)
        ASSERT_FAIL("has_vcol=true red: R too low (" + std::to_string(static_cast<int>(c.r)) + "), expected ~255");
    if (c.g > 5 || c.b > 5)
        ASSERT_FAIL("has_vcol=true red: G/B too high, expected near-zero");
}

// V3: white vcol with has_vcol=true must match the has_vcol=false result within ±2.
// Catches: the has_vcol=true path introduces drift with the identity colour
// (wrong material copy, lost specular, double-multiply, etc.).
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

// V4: RGB vcol interpolated at centroid with equal w=1.
// At (20,10): ba=0.21875, bb=0.25, bc=0.53125.
// Expected: vcol ≈ (0.219, 0.250, 0.531) → pixel ≈ (56, 64, 135) ±5.
// Catches: vcol interpolation broken (constant, swapped indices, wrong weights).
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

// V5: vcola=red, vcolb=red, vcolc=blue; wa=wb=10, wc=1.
// Pixel (12,10): pwa=0.046875, pwb=0, pwc=0.53125 → w_corr≈1.7297
// vcol_b = 0.53125*1.7297 ≈ 0.919 → B≥200; R≤60.
// Catches: vcol interpolation regressed to plain barycentric (much redder result).
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
        ASSERT_FAIL(
            "B too low (" + std::to_string(static_cast<int>(c.b)) +
            "): perspective-correct vcol should bias hard toward blue"
        );
    if (c.r > 60)
        ASSERT_FAIL(
            "R too high (" + std::to_string(static_cast<int>(c.r)) + "): far red vertices should contribute little"
        );
}

// V6: red vcol × green diffuse texture → black.
// ambient*(mat.ambient*tex)*vcol = (1)*(0,1,0)*(1,0,0) = (0,0,0).
// Also asserts was_drawn (pixel rendered, not skipped).
// Catches: vcol applied to a fresh mat copy ignoring the tex-modified copy
//          (the bug the `if (use_mat == &mat)` guard prevents).
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
        ASSERT_FAIL(
            "expected black (red vcol * green tex = 0 per channel), got (" + std::to_string(static_cast<int>(c.r)) +
            "," + std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
}
