#include "tests/test.h"
#include "tests/rasterize_test_util.h"
#include "src/render/rasterize.h"

// Verify the soft-knee rolloff reaches flat and Phong output, but not the unlit path.
// The triangle covers pixel (20,10) in a 40x20 buffer.

namespace
{
    const vec3 g_sa{ 4.0f, 2.0f, 0.5f }, g_sb{ 36.0f, 2.0f, 0.5f }, g_sc{ 20.0f, 18.0f, 0.5f };
    const vec3 g_zero{ 0.0f, 0.0f, 0.0f };
    const vec2 g_uv{ 0.5f, 0.5f };

    // Flat path with a uniform per-vertex colour `col` and no texture/emissive. mat == null
    // selects the lit (tonemapped) Flat path.
    void rast_flat_color(Framebuffer &fb, vec3 col, const Material *mat = nullptr)
    {
        rasterize_flat(
            fb, g_sa, g_sb, g_sc, 1.0f, 1.0f, 1.0f, col, col, col, g_uv, g_uv, g_uv, nullptr, 0.0f, 0, 19, nullptr,
            g_zero, vec2{}, vec2{}, vec2{}, mat
        );
    }
} // namespace

// A lit value above the knee rolls off below 255 instead of hard-clipping. tonemap(1.2) =
// 0.7 + 0.3*(1 - exp(-0.5/0.3)) = 0.94334 -> 240.
TEST(rasterize_tonemap, opaque_above_knee_rolls_off)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    rast_flat_color(fb, vec3{ 1.2f, 1.2f, 1.2f });

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    assert_pixel_near(fb, 20, 10, Color{ 240, 240, 240 }, 1);
}

// A value at or below the knee is byte-identical to the pre-tonemap pipeline, so the
// well-exposed / textured look is untouched. 0.5 * 255 = 127.5 -> 127, exactly.
TEST(rasterize_tonemap, opaque_below_knee_byte_identical)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    rast_flat_color(fb, vec3{ 0.5f, 0.5f, 0.5f });

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    assert_pixel_near(fb, 20, 10, Color{ 127, 127, 127 }, 0);
}

// Above-range values remain distinct instead of both clamping to 255:
// tonemap(1.0) -> 226; tonemap(1.6) -> 251.
TEST(rasterize_tonemap, distinct_overrange_values_stay_distinct)
{
    Framebuffer fb_a(40, 20, /*headless=*/true), fb_b(40, 20, /*headless=*/true);
    rast_flat_color(fb_a, vec3{ 1.0f, 1.0f, 1.0f });
    rast_flat_color(fb_b, vec3{ 1.6f, 1.6f, 1.6f });

    const Color a = fb_a.get_pixel(20, 10);
    const Color b = fb_b.get_pixel(20, 10);
    ASSERT_TRUE(a.r < 255 && b.r < 255); // neither hard-clips to white
    ASSERT_TRUE(b.r > a.r + 10);         // the brighter input stays distinguishably brighter
}

// The unlit path reuses rasterize_flat but must NOT be tonemapped: its output is bounded [0,1]
// and meant to be faithful. White (1.0) therefore stays 255, not 226.
TEST(rasterize_tonemap, unlit_path_is_not_tonemapped)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Material unlit{};
    unlit.unlit = true;
    rast_flat_color(fb, vec3{ 1.0f, 1.0f, 1.0f }, &unlit);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    assert_pixel_near(fb, 20, 10, Color{ 255, 255, 255 }, 0);
}

// The Phong commit site tonemaps too (Phong is always a lit path). Ambient 1.0 with no lights
// and no texture yields a full-white lit colour that rolls off to ~226, distinctly below 255.
TEST(rasterize_tonemap, phong_commit_tonemaps)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 1.0f, 1.0f, 1.0f };
    Material mat{};
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };

    rasterize_phong(
        fb, g_sa, g_sb, g_sc, 1.0f, 1.0f, 1.0f, g_zero, g_zero, g_zero, normal, normal, normal, tan, tan, tan, g_uv,
        g_uv, g_uv, 1.0f, 1.0f, 1.0f, white, white, white, false, eye, nullptr, 0, ambient, mat, nullptr, nullptr,
        nullptr, 0, 19
    );

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    assert_pixel_near(fb, 20, 10, Color{ 226, 226, 226 }, 2);
}
