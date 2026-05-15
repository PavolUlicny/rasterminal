#include "test.h"
#include "rasterize_test_util.h"
#include "../src/rasterize.h"

#include <initializer_list>

// ─── helpers ──────────────────────────────────────────────────────────────────

// Build an in-memory Texture without disk I/O.
// Takes int values to avoid uint8_t narrowing-conversion issues at call sites.
static Texture make_tex_rgba(int w, int h, std::initializer_list<int> rgba)
{
    Texture t;
    t.width = w;
    t.height = h;
    t.pixels.reserve(rgba.size());
    for (int v : rgba)
        t.pixels.push_back(static_cast<uint8_t>(v));
    return t;
}

// rasterize() with explicit w, UVs, optional texture; shad == col (no shadow needed).
static void rast_tex(Framebuffer &fb,
                     vec3 sa, vec3 sb, vec3 sc,
                     float wa, float wb, float wc,
                     vec3 ca, vec3 cb, vec3 cc,
                     vec2 uva, vec2 uvb, vec2 uvc,
                     const Texture *tex, int y_min, int y_max)
{
    vec3 zero{};
    rasterize(fb, sa, sb, sc, wa, wb, wc,
              ca, cb, cc, ca, cb, cc,
              zero, zero, zero,
              uva, uvb, uvc,
              tex, 0.0f, nullptr, y_min, y_max);
}

// ─── texture-rasterizer integration ──────────────────────────────────────────
//
// Canonical triangle: sa=(4,2), sb=(36,2), sc=(20,18) on 40×20 framebuffer.
// Key pixel centres and pre-computed screen-space barycentric weights:
//
//   Pixel (12,10): ba=0.46875, bb=0,       bc=0.53125
//   Pixel (20,10): ba=0.21875, bb=0.25,    bc=0.53125

// ── Group A: diffuse texture in rasterize() ───────────────────────────────────

// A1: 1×1 red texture × white vertex colour → red pixel.
// Catches: texture multiply silently dropped.
TEST(rasterize, solid_diffuse_texture_replaces_color)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex_rgba(1, 1, {255, 0, 0, 255});
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 white{1.0f, 1.0f, 1.0f};
    vec2 uv{0.5f, 0.5f};
    rast_tex(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, uv, uv, uv, &tex, 0, 19);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 250)
        ASSERT_FAIL("R too low (" + std::to_string(static_cast<int>(c.r)) +
                    "): expected red from 1x1 red texture");
    if (c.g > 5 || c.b > 5)
        ASSERT_FAIL("G/B too high: expected pure red pixel");
}

// A2: 1×1 red texture × green vertex colour → black (R*G = 0 per component).
// Catches: texture applied to wrong channel or in wrong multiply order.
TEST(rasterize, texture_modulates_with_vertex_color)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex_rgba(1, 1, {255, 0, 0, 255});
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 green{0.0f, 1.0f, 0.0f};
    vec2 uv{0.5f, 0.5f};
    rast_tex(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, green, green, green, uv, uv, uv, &tex, 0, 19);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 5 || c.g > 5 || c.b > 5)
        ASSERT_FAIL("Expected black: red*green per-component should zero all channels");
}

// A3: 2×1 red/blue texture; wa=wb=10 (far), wc=1 (near).
// Pixel (12,10) midpoint of edge a→c. Perspective-correct UV.x≈0.835 → blue-biased
// (B≈213, R≈42). Plain barycentric would give UV.x≈0.525 → mixed.
// Catches: UV interpolation regressed to plain barycentric.
TEST(rasterize, texture_uv_perspective_correct_unequal_w)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex_rgba(2, 1, {255, 0, 0, 255, 0, 0, 255, 255});
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 white{1.0f, 1.0f, 1.0f};
    vec2 uva{0.1f, 0.5f}, uvb{0.1f, 0.5f}, uvc{0.9f, 0.5f};
    rast_tex(fb, sa, sb, sc, 10.0f, 10.0f, 1.0f, white, white, white, uva, uvb, uvc, &tex, 0, 19);

    ASSERT_TRUE(was_drawn(fb, 12, 10));
    Color c = fb.get_pixel(12, 10);
    if (c.b < 200)
        ASSERT_FAIL("B too low (" + std::to_string(static_cast<int>(c.b)) +
                    "): perspective-correct UV should bias toward blue near vertex c");
    if (c.r > 60)
        ASSERT_FAIL("R too high (" + std::to_string(static_cast<int>(c.r)) +
                    "): far red vertices should contribute little");
}

// A4: 1×2 texture (row 0=red, row 1=blue); UV v=0.
// V-flip maps UV v=0 → image bottom row (row 1) = blue.
// Catches: V-flip removed (produces red) or doubled (also produces red).
TEST(rasterize, texture_v_flip_convention)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex_rgba(1, 2, {255, 0, 0, 255, 0, 0, 255, 255});
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 white{1.0f, 1.0f, 1.0f};
    vec2 uv{0.5f, 0.0f};
    rast_tex(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, uv, uv, uv, &tex, 0, 19);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.b < 200)
        ASSERT_FAIL("B too low: UV v=0 should map to bottom image row (blue) via V-flip");
    if (c.r > 5)
        ASSERT_FAIL("R too high: UV v=0 must not map to top image row (red)");
}

// A5: 2×1 red/blue texture; UV u=1.1 wraps to 0.1 → mostly red (R≈230, B≈26).
// Without wrap: u=1.1 clamps to blue edge → R≈0.
// Catches: UV wrap removed (would clamp to blue instead of wrapping to red).
TEST(rasterize, texture_uv_wrap)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex_rgba(2, 1, {255, 0, 0, 255, 0, 0, 255, 255});
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 white{1.0f, 1.0f, 1.0f};
    vec2 uv{1.1f, 0.5f};
    rast_tex(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, uv, uv, uv, &tex, 0, 19);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 200)
        ASSERT_FAIL("R too low (" + std::to_string(static_cast<int>(c.r)) +
                    "): u=1.1 should wrap to 0.1 giving mostly red");
    if (c.b > 50)
        ASSERT_FAIL("B too high: blue texel should barely contribute after wrap");
}

// ── Group B: diffuse texture in rasterize_phong() ────────────────────────────

// B1: 1×1 gray (128,128,128) texture halves both diffuse and ambient.
// Ambient-only setup (n_lights=0): without tex R≈255, with tex R≈128.
// Catches: texture not applied to ambient, or applied to diffuse only.
TEST(rasterize_phong, texture_modulates_diffuse_and_ambient)
{
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 zero{}, normal{0.0f, 0.0f, 1.0f}, tan{1.0f, 0.0f, 0.0f}, white{1.0f, 1.0f, 1.0f};
    vec3 eye{20.0f, 10.0f, -100.0f};
    vec3 ambient{1.0f, 1.0f, 1.0f};
    Material mat{};
    mat.diffuse = {1.0f, 1.0f, 1.0f};
    mat.ambient = {1.0f, 1.0f, 1.0f};
    mat.specular = {0.0f, 0.0f, 0.0f};
    vec2 uv{0.5f, 0.5f};
    Texture gray_tex = make_tex_rgba(1, 1, {128, 128, 128, 255});

    auto rph = [&](Framebuffer &fb, const Texture *tex)
    {
        rasterize_phong(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f,
                        zero, zero, zero, normal, normal, normal, tan, tan, tan,
                        uv, uv, uv,
                        1.0f, 1.0f, 1.0f,
                        white, white, white, false,
                        eye, nullptr, 0, ambient, mat,
                        tex, nullptr, nullptr, nullptr,
                        0, 19);
    };

    Framebuffer fb_notex(40, 20, /*headless=*/true), fb_tex(40, 20, /*headless=*/true);
    rph(fb_notex, nullptr);
    rph(fb_tex, &gray_tex);

    ASSERT_TRUE(was_drawn(fb_notex, 20, 10));
    ASSERT_TRUE(was_drawn(fb_tex, 20, 10));
    if (fb_notex.get_pixel(20, 10).r < 250)
        ASSERT_FAIL("no-texture: R too low, expected ~255 from ambient");
    Color with_tex = fb_tex.get_pixel(20, 10);
    if (with_tex.r < 110 || with_tex.r > 145)
        ASSERT_FAIL("with gray tex: R=" + std::to_string(static_cast<int>(with_tex.r)) +
                    " expected ~128 (gray halves ambient)");
}

// B2: Same 2×1 red/blue + unequal-w setup as A3, but in the Phong path.
// At pixel (12,10): UV.x≈0.835 → blue-biased result.
// Catches: Phong UV interpolation diverges from the Gouraud path (separate code).
TEST(rasterize_phong, texture_uv_perspective_correct_unequal_w)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 zero{}, normal{0.0f, 0.0f, 1.0f}, tan{1.0f, 0.0f, 0.0f}, white{1.0f, 1.0f, 1.0f};
    vec3 eye{20.0f, 10.0f, -100.0f};
    vec3 ambient{1.0f, 1.0f, 1.0f};
    Material mat{};
    mat.diffuse = {1.0f, 1.0f, 1.0f};
    mat.ambient = {1.0f, 1.0f, 1.0f};
    mat.specular = {0.0f, 0.0f, 0.0f};
    Texture tex = make_tex_rgba(2, 1, {255, 0, 0, 255, 0, 0, 255, 255});
    vec2 uva{0.1f, 0.5f}, uvb{0.1f, 0.5f}, uvc{0.9f, 0.5f};

    rasterize_phong(fb, sa, sb, sc, 10.0f, 10.0f, 1.0f,
                    zero, zero, zero, normal, normal, normal, tan, tan, tan,
                    uva, uvb, uvc,
                    1.0f, 1.0f, 1.0f,
                    white, white, white, false,
                    eye, nullptr, 0, ambient, mat,
                    &tex, nullptr, nullptr, nullptr,
                    0, 19);

    ASSERT_TRUE(was_drawn(fb, 12, 10));
    Color c = fb.get_pixel(12, 10);
    if (c.b < 200)
        ASSERT_FAIL("B too low (" + std::to_string(static_cast<int>(c.b)) +
                    "): Phong UV must use perspective-correct interpolation");
    if (c.r > 60)
        ASSERT_FAIL("R too high: far red vertices should contribute little");
}

// B3: 1×1 white texture must not change the rendered colour.
// Catches: tex multiply introduces precision loss or incorrect normalization.
TEST(rasterize_phong, white_texture_matches_no_texture)
{
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 zero{}, normal{0.0f, 0.0f, 1.0f}, tan{1.0f, 0.0f, 0.0f}, white{1.0f, 1.0f, 1.0f};
    vec3 eye{20.0f, 10.0f, -100.0f};
    vec3 ambient{0.8f, 0.8f, 0.8f};
    Material mat{};
    mat.diffuse = {0.6f, 0.4f, 0.8f};
    mat.ambient = {0.3f, 0.5f, 0.7f};
    mat.specular = {0.0f, 0.0f, 0.0f};
    vec2 uv{0.5f, 0.5f};
    Texture white_tex = make_tex_rgba(1, 1, {255, 255, 255, 255});

    auto rph = [&](Framebuffer &fb, const Texture *tex)
    {
        rasterize_phong(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f,
                        zero, zero, zero, normal, normal, normal, tan, tan, tan,
                        uv, uv, uv,
                        1.0f, 1.0f, 1.0f,
                        white, white, white, false,
                        eye, nullptr, 0, ambient, mat,
                        tex, nullptr, nullptr, nullptr,
                        0, 19);
    };

    Framebuffer fb_notex(40, 20, /*headless=*/true), fb_tex(40, 20, /*headless=*/true);
    rph(fb_notex, nullptr);
    rph(fb_tex, &white_tex);

    ASSERT_TRUE(was_drawn(fb_notex, 20, 10));
    ASSERT_TRUE(was_drawn(fb_tex, 20, 10));
    assert_pixel_near(fb_tex, 20, 10, fb_notex.get_pixel(20, 10), 2);
}

// ── Group C: specular texture ─────────────────────────────────────────────────

// C1: 1×1 black specular texture zeroes the specular highlight.
// normal=(0,0,1), eye=(0,0,5), light dir=(0,0,1): H=N → ndh=1 → peak specular.
// mat.diffuse=mat.ambient=(0,0,0) so only specular contributes.
// Without stex: R≈255.  With black stex: R≈0.
// Catches: stex dropped or applied additively instead of multiplicatively.
TEST(rasterize_phong, specular_texture_zeroes_highlight)
{
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 zero{}, normal{0.0f, 0.0f, 1.0f}, tan{1.0f, 0.0f, 0.0f}, white{1.0f, 1.0f, 1.0f};
    vec3 eye{0.0f, 0.0f, 5.0f};
    vec3 ambient{0.0f, 0.0f, 0.0f};
    Material mat{};
    mat.diffuse = {0.0f, 0.0f, 0.0f};
    mat.ambient = {0.0f, 0.0f, 0.0f};
    mat.specular = {1.0f, 1.0f, 1.0f};
    mat.shininess = 32.0f;
    Light light{};
    light.direction = {0.0f, 0.0f, 1.0f};
    light.color = {1.0f, 1.0f, 1.0f};
    vec2 uv{0.5f, 0.5f};
    Texture black_stex = make_tex_rgba(1, 1, {0, 0, 0, 255});

    auto rph = [&](Framebuffer &fb, const Texture *stex)
    {
        rasterize_phong(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f,
                        zero, zero, zero, normal, normal, normal, tan, tan, tan,
                        uv, uv, uv,
                        1.0f, 1.0f, 1.0f,
                        white, white, white, false,
                        eye, &light, 1, ambient, mat,
                        nullptr, nullptr, stex, nullptr,
                        0, 19);
    };

    Framebuffer fb_nostex(40, 20, /*headless=*/true), fb_stex(40, 20, /*headless=*/true);
    rph(fb_nostex, nullptr);
    rph(fb_stex, &black_stex);

    ASSERT_TRUE(was_drawn(fb_nostex, 20, 10));
    ASSERT_TRUE(was_drawn(fb_stex, 20, 10));
    if (fb_nostex.get_pixel(20, 10).r < 240)
        ASSERT_FAIL("without stex: R too low, expected peak specular ~255");
    if (fb_stex.get_pixel(20, 10).r > 5)
        ASSERT_FAIL("with black stex: R too high, specular should be zeroed");
}

// ── Group D: normal map ───────────────────────────────────────────────────────

// D1: nmap texel (255,128,128) unpacks to nm≈(1,0,0) via *2−1.
// Vertex normals=(0,0,1), tangents=(1,0,0) → TBN redirects normal to +x.
// Light dir=(1,0,0): without nmap dot=0→R≈0; with nmap dot=1→R≈255.
// Catches: nmap unpack removed, or TBN basis transposed/wrong handedness.
TEST(rasterize_phong, normal_map_redirects_lighting)
{
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 zero{}, normal{0.0f, 0.0f, 1.0f}, tan{1.0f, 0.0f, 0.0f}, white{1.0f, 1.0f, 1.0f};
    vec3 eye{0.0f, 0.0f, 5.0f};
    vec3 ambient{0.0f, 0.0f, 0.0f};
    Material mat{};
    mat.diffuse = {1.0f, 1.0f, 1.0f};
    mat.ambient = {0.0f, 0.0f, 0.0f};
    mat.specular = {0.0f, 0.0f, 0.0f};
    Light light{};
    light.direction = {1.0f, 0.0f, 0.0f};
    light.color = {1.0f, 0.0f, 0.0f};
    vec2 uv{0.5f, 0.5f};
    Texture nmap_tex = make_tex_rgba(1, 1, {255, 128, 128, 255});

    auto rph = [&](Framebuffer &fb, const Texture *nm)
    {
        rasterize_phong(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f,
                        zero, zero, zero, normal, normal, normal, tan, tan, tan,
                        uv, uv, uv,
                        1.0f, 1.0f, 1.0f,
                        white, white, white, false,
                        eye, &light, 1, ambient, mat,
                        nullptr, nm, nullptr, nullptr,
                        0, 19);
    };

    Framebuffer fb_nonmap(40, 20, /*headless=*/true), fb_nmap(40, 20, /*headless=*/true);
    rph(fb_nonmap, nullptr);
    rph(fb_nmap, &nmap_tex);

    ASSERT_TRUE(was_drawn(fb_nonmap, 20, 10));
    ASSERT_TRUE(was_drawn(fb_nmap, 20, 10));
    if (fb_nonmap.get_pixel(20, 10).r > 10)
        ASSERT_FAIL("without nmap: R too high -- dot((0,0,1),(1,0,0))=0, no diffuse expected");
    if (fb_nmap.get_pixel(20, 10).r < 240)
        ASSERT_FAIL("with nmap: R too low -- redirected normal ~(1,0,0) should give full red diffuse");
}
