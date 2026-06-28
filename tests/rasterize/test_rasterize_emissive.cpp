#include "tests/test.h"
#include "tests/rasterize_test_util.h"
#include "src/rasterize.h"

#include <initializer_list>

// ─── helpers ──────────────────────────────────────────────────────────────────

static Texture make_em_tex(int w, int h, std::initializer_list<int> rgba)
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

// rasterize_flat() (Flat path) with optional emissive factor + texture.
// Lighting is zero (col_a/b/c == {0,0,0}) so any non-zero output comes from emissive.
static void rast_emissive(Framebuffer &fb, const Texture *etex, vec3 emissive, vec2 uva, vec2 uvb, vec2 uvc)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{ 0.0f, 0.0f, 0.0f };
    rasterize_flat(
        fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, zero, zero, zero, zero, uva, uvb, uvc, nullptr, 0.0f,
        nullptr, 0, 19, etex, emissive
    );
}

// rasterize_phong() with no lights so any output comes from ambient*0 + emissive.
static void rast_phong_emissive(Framebuffer &fb, const Material &mat, const Texture *etex)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 0.0f, 0.0f, 0.0f }; // no ambient so emissive is the only contribution
    vec2 uv{ 0.5f, 0.5f };
    vec3 white{ 1.0f, 1.0f, 1.0f };
    rasterize_phong(
        fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
        1.0f, 1.0f, white, white, white, false, eye, nullptr, 0, ambient, mat, nullptr, nullptr, nullptr, nullptr, 0,
        19, nullptr, etex, mat.emissive
    );
}

// ─── Flat emissive in rasterize_flat() ────────────────────────────────────────────

// Pure factor with no lit color, no texture: pixel should be the emissive colour.
TEST(rasterize_emissive, factor_only_adds_pure_colour)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec2 uv{ 0.5f, 0.5f };
    rast_emissive(fb, nullptr, vec3{ 1.0f, 0.0f, 0.0f }, uv, uv, uv);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    // Full-intensity emissive (1.0) rolls off through the soft-knee tonemap: 1.0 -> ~0.890 -> 226.
    assert_pixel_near(fb, 20, 10, Color{ 226, 0, 0 }, 2);
}

// Emissive texture modulates the factor.
TEST(rasterize_emissive, texture_modulates_factor)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture etex = make_em_tex(1, 1, { 0, 255, 0, 255 }); // pure green
    vec2 uv{ 0.5f, 0.5f };
    // factor = white; texture = green; expected output = green.
    rast_emissive(fb, &etex, vec3{ 1.0f, 1.0f, 1.0f }, uv, uv, uv);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    // Full-intensity green (1.0) rolls off through the soft-knee tonemap: 1.0 -> ~0.890 -> 226.
    assert_pixel_near(fb, 20, 10, Color{ 0, 226, 0 }, 2);
}

// Zero factor + no texture: nothing added — pixel stays as the lit colour (zero here).
TEST(rasterize_emissive, zero_factor_no_texture_is_noop)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec2 uv{ 0.5f, 0.5f };
    rast_emissive(fb, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, uv, uv, uv);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    assert_pixel_near(fb, 20, 10, Color{ 0, 0, 0 }, 2);
}

// ─── Phong emissive in rasterize_phong() ─────────────────────────────────────

TEST(rasterize_phong_emissive, factor_only_bypasses_lighting)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Material mat{};
    mat.ambient = { 0.0f, 0.0f, 0.0f };
    mat.diffuse = { 0.0f, 0.0f, 0.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    mat.emissive = { 1.0f, 0.0f, 0.0f }; // pure red emissive, no lights
    rast_phong_emissive(fb, mat, nullptr);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    // Full-intensity emissive (1.0) rolls off through the soft-knee tonemap: 1.0 -> ~0.890 -> 226.
    assert_pixel_near(fb, 20, 10, Color{ 226, 0, 0 }, 2);
}

TEST(rasterize_phong_emissive, texture_modulates_factor)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Material mat{};
    mat.ambient = { 0.0f, 0.0f, 0.0f };
    mat.diffuse = { 0.0f, 0.0f, 0.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    mat.emissive = { 1.0f, 1.0f, 1.0f };
    Texture etex = make_em_tex(1, 1, { 0, 0, 255, 255 }); // pure blue
    rast_phong_emissive(fb, mat, &etex);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    // Full-intensity blue (1.0) rolls off through the soft-knee tonemap: 1.0 -> ~0.890 -> 226.
    assert_pixel_near(fb, 20, 10, Color{ 0, 0, 226 }, 2);
}
