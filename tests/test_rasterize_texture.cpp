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
    {
        t.pixels.push_back(static_cast<uint8_t>(v));
    }
    return t;
}

// rasterize() with explicit w, UVs, optional texture; shad == col (no shadow needed).
static void rast_tex(
    Framebuffer &fb,
    vec3 sa,
    vec3 sb,
    vec3 sc,
    float wa,
    float wb,
    float wc,
    vec3 ca,
    vec3 cb,
    vec3 cc,
    vec2 uva,
    vec2 uvb,
    vec2 uvc,
    const Texture *tex,
    int y_min,
    int y_max
)
{
    vec3 zero{};
    rasterize(
        fb, sa, sb, sc, wa, wb, wc, ca, cb, cc, ca, cb, cc, zero, zero, zero, uva, uvb, uvc, tex, 0.0f, nullptr, y_min,
        y_max
    );
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
    Texture tex = make_tex_rgba(1, 1, { 255, 0, 0, 255 });
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 white{ 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    rast_tex(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, uv, uv, uv, &tex, 0, 19);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 250)
    {
        ASSERT_FAIL("R too low (" + std::to_string(static_cast<int>(c.r)) + "): expected red from 1x1 red texture");
    }
    if (c.g > 5 || c.b > 5)
    {
        ASSERT_FAIL("G/B too high: expected pure red pixel");
    }
}

// A2: 1×1 red texture × green vertex colour → black (R*G = 0 per component).
// Catches: texture applied to wrong channel or in wrong multiply order.
TEST(rasterize, texture_modulates_with_vertex_color)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex_rgba(1, 1, { 255, 0, 0, 255 });
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 green{ 0.0f, 1.0f, 0.0f };
    vec2 uv{ 0.5f, 0.5f };
    rast_tex(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, green, green, green, uv, uv, uv, &tex, 0, 19);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 5 || c.g > 5 || c.b > 5)
    {
        ASSERT_FAIL("Expected black: red*green per-component should zero all channels");
    }
}

// A3: 2×1 red/blue texture; wa=wb=10 (far), wc=1 (near).
// Pixel (12,10) midpoint of edge a→c. Perspective-correct UV.x≈0.835 → blue-biased
// (B≈213, R≈42). Plain barycentric would give UV.x≈0.525 → mixed.
// Catches: UV interpolation regressed to plain barycentric.
TEST(rasterize, texture_uv_perspective_correct_unequal_w)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex_rgba(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 white{ 1.0f, 1.0f, 1.0f };
    vec2 uva{ 0.1f, 0.5f }, uvb{ 0.1f, 0.5f }, uvc{ 0.9f, 0.5f };
    rast_tex(fb, sa, sb, sc, 10.0f, 10.0f, 1.0f, white, white, white, uva, uvb, uvc, &tex, 0, 19);

    ASSERT_TRUE(was_drawn(fb, 12, 10));
    Color c = fb.get_pixel(12, 10);
    if (c.b < 200)
    {
        ASSERT_FAIL(
            "B too low (" + std::to_string(static_cast<int>(c.b)) +
            "): perspective-correct UV should bias toward blue near vertex c"
        );
    }
    if (c.r > 60)
    {
        ASSERT_FAIL(
            "R too high (" + std::to_string(static_cast<int>(c.r)) + "): far red vertices should contribute little"
        );
    }
}

// A4: 1×2 texture (row 0=red, row 1=blue); UV v=0.
// V-flip maps UV v=0 → image bottom row (row 1) = blue.
// Catches: V-flip removed (produces red) or doubled (also produces red).
TEST(rasterize, texture_v_flip_convention)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex_rgba(1, 2, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 white{ 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.0f };
    rast_tex(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, uv, uv, uv, &tex, 0, 19);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.b < 200)
    {
        ASSERT_FAIL("B too low: UV v=0 should map to bottom image row (blue) via V-flip");
    }
    if (c.r > 5)
    {
        ASSERT_FAIL("R too high: UV v=0 must not map to top image row (red)");
    }
}

// A5: 2×1 red/blue texture; UV u=1.1 wraps to 0.1 → mostly red (R≈230, B≈26).
// Without wrap: u=1.1 clamps to blue edge → R≈0.
// Catches: UV wrap removed (would clamp to blue instead of wrapping to red).
TEST(rasterize, texture_uv_wrap)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    Texture tex = make_tex_rgba(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 white{ 1.0f, 1.0f, 1.0f };
    vec2 uv{ 1.1f, 0.5f };
    rast_tex(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, uv, uv, uv, &tex, 0, 19);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 200)
    {
        ASSERT_FAIL(
            "R too low (" + std::to_string(static_cast<int>(c.r)) + "): u=1.1 should wrap to 0.1 giving mostly red"
        );
    }
    if (c.b > 50)
    {
        ASSERT_FAIL("B too high: blue texel should barely contribute after wrap");
    }
}

// ── Group B: diffuse texture in rasterize_phong() ────────────────────────────

// B1: 1×1 gray (128,128,128) texture halves both diffuse and ambient.
// Ambient-only setup (n_lights=0): without tex R≈255, with tex R≈128.
// Catches: texture not applied to ambient, or applied to diffuse only.
TEST(rasterize_phong, texture_modulates_diffuse_and_ambient)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 1.0f, 1.0f, 1.0f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    vec2 uv{ 0.5f, 0.5f };
    Texture gray_tex = make_tex_rgba(1, 1, { 128, 128, 128, 255 });

    auto rph = [&](Framebuffer &fb, const Texture *tex)
    {
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, nullptr, 0, ambient, mat, tex, nullptr, nullptr, nullptr, 0, 19
        );
    };

    Framebuffer fb_notex(40, 20, /*headless=*/true), fb_tex(40, 20, /*headless=*/true);
    rph(fb_notex, nullptr);
    rph(fb_tex, &gray_tex);

    ASSERT_TRUE(was_drawn(fb_notex, 20, 10));
    ASSERT_TRUE(was_drawn(fb_tex, 20, 10));
    if (fb_notex.get_pixel(20, 10).r < 250)
    {
        ASSERT_FAIL("no-texture: R too low, expected ~255 from ambient");
    }
    Color with_tex = fb_tex.get_pixel(20, 10);
    if (with_tex.r < 110 || with_tex.r > 145)
    {
        ASSERT_FAIL(
            "with gray tex: R=" + std::to_string(static_cast<int>(with_tex.r)) + " expected ~128 (gray halves ambient)"
        );
    }
}

// B2: Same 2×1 red/blue + unequal-w setup as A3, but in the Phong path.
// At pixel (12,10): UV.x≈0.835 → blue-biased result.
// Catches: Phong UV interpolation diverges from the Gouraud path (separate code).
TEST(rasterize_phong, texture_uv_perspective_correct_unequal_w)
{
    Framebuffer fb(40, 20, /*headless=*/true);
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 1.0f, 1.0f, 1.0f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    Texture tex = make_tex_rgba(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec2 uva{ 0.1f, 0.5f }, uvb{ 0.1f, 0.5f }, uvc{ 0.9f, 0.5f };

    rasterize_phong(
        fb, sa, sb, sc, 10.0f, 10.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uva, uvb, uvc,
        1.0f, 1.0f, 1.0f, white, white, white, false, eye, nullptr, 0, ambient, mat, &tex, nullptr, nullptr, nullptr, 0,
        19
    );

    ASSERT_TRUE(was_drawn(fb, 12, 10));
    Color c = fb.get_pixel(12, 10);
    if (c.b < 200)
    {
        ASSERT_FAIL(
            "B too low (" + std::to_string(static_cast<int>(c.b)) +
            "): Phong UV must use perspective-correct interpolation"
        );
    }
    if (c.r > 60)
    {
        ASSERT_FAIL("R too high: far red vertices should contribute little");
    }
}

// B3: 1×1 white texture must not change the rendered colour.
// Catches: tex multiply introduces precision loss or incorrect normalization.
TEST(rasterize_phong, white_texture_matches_no_texture)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 0.8f, 0.8f, 0.8f };
    Material mat{};
    mat.diffuse = { 0.6f, 0.4f, 0.8f };
    mat.ambient = { 0.3f, 0.5f, 0.7f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    vec2 uv{ 0.5f, 0.5f };
    Texture white_tex = make_tex_rgba(1, 1, { 255, 255, 255, 255 });

    auto rph = [&](Framebuffer &fb, const Texture *tex)
    {
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, nullptr, 0, ambient, mat, tex, nullptr, nullptr, nullptr, 0, 19
        );
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
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f };
    vec3 ambient{ 0.0f, 0.0f, 0.0f };
    Material mat{};
    mat.diffuse = { 0.0f, 0.0f, 0.0f };
    mat.ambient = { 0.0f, 0.0f, 0.0f };
    mat.specular = { 1.0f, 1.0f, 1.0f };
    mat.shininess = 32.0f;
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    Texture black_stex = make_tex_rgba(1, 1, { 0, 0, 0, 255 });

    auto rph = [&](Framebuffer &fb, const Texture *stex)
    {
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, nullptr, nullptr, stex, nullptr, 0, 19
        );
    };

    Framebuffer fb_nostex(40, 20, /*headless=*/true), fb_stex(40, 20, /*headless=*/true);
    rph(fb_nostex, nullptr);
    rph(fb_stex, &black_stex);

    ASSERT_TRUE(was_drawn(fb_nostex, 20, 10));
    ASSERT_TRUE(was_drawn(fb_stex, 20, 10));
    if (fb_nostex.get_pixel(20, 10).r < 240)
    {
        ASSERT_FAIL("without stex: R too low, expected peak specular ~255");
    }
    if (fb_stex.get_pixel(20, 10).r > 5)
    {
        ASSERT_FAIL("with black stex: R too high, specular should be zeroed");
    }
}

// ── Group D: normal map ───────────────────────────────────────────────────────

// D1: nmap texel (255,128,128) unpacks to nm≈(1,0,0) via *2−1.
// Vertex normals=(0,0,1), tangents=(1,0,0) → TBN redirects normal to +x.
// Light dir=(1,0,0): without nmap dot=0→R≈0; with nmap dot=1→R≈255.
// Catches: nmap unpack removed, or TBN basis transposed/wrong handedness.
TEST(rasterize_phong, normal_map_redirects_lighting)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f };
    vec3 ambient{ 0.0f, 0.0f, 0.0f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 0.0f, 0.0f, 0.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    Light light{};
    light.direction = { 1.0f, 0.0f, 0.0f };
    light.color = { 1.0f, 0.0f, 0.0f };
    vec2 uv{ 0.5f, 0.5f };
    Texture nmap_tex = make_tex_rgba(1, 1, { 255, 128, 128, 255 });

    auto rph = [&](Framebuffer &fb, const Texture *nm)
    {
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, nullptr, nm, nullptr, nullptr, 0, 19
        );
    };

    Framebuffer fb_nonmap(40, 20, /*headless=*/true), fb_nmap(40, 20, /*headless=*/true);
    rph(fb_nonmap, nullptr);
    rph(fb_nmap, &nmap_tex);

    ASSERT_TRUE(was_drawn(fb_nonmap, 20, 10));
    ASSERT_TRUE(was_drawn(fb_nmap, 20, 10));
    if (fb_nonmap.get_pixel(20, 10).r > 10)
    {
        ASSERT_FAIL("without nmap: R too high -- dot((0,0,1),(1,0,0))=0, no diffuse expected");
    }
    if (fb_nmap.get_pixel(20, 10).r < 240)
    {
        ASSERT_FAIL("with nmap: R too low -- redirected normal ~(1,0,0) should give full red diffuse");
    }
}

// ── Group D+: glTF normalScale applied to the tangent-space normal ───────────

// Same D1 geometry: nmap texel (255,128,128) → nm≈(1,0,0), tan=(1,0,0),
// light dir+color along +x. scale=0 must zero nm.x so the normal falls back to
// N=(0,0,1); dot(N, +x)=0 → no red. Run side-by-side with apply_normal_scale=false
// (scale ignored → full red) to isolate the gate AND prove the multiply hits
// nm.x/nm.y, not nm.z. Catches an inverted gate or a wrong-axis multiply.
TEST(rasterize_phong, normal_scale_zero_flattens_bump)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f };
    vec3 ambient{ 0.0f, 0.0f, 0.0f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 0.0f, 0.0f, 0.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    Light light{};
    light.direction = { 1.0f, 0.0f, 0.0f };
    light.color = { 1.0f, 0.0f, 0.0f };
    vec2 uv{ 0.5f, 0.5f };
    Texture nmap_tex = make_tex_rgba(1, 1, { 255, 128, 128, 255 });

    auto rph = [&](Framebuffer &fb, const Material &m, bool apply_scale)
    {
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, m, nullptr, &nmap_tex, nullptr, nullptr, 0,
            19, nullptr, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, apply_scale
        );
    };

    Material mat_zero = mat;
    mat_zero.normal_scale = 0.0f;
    Framebuffer fb_gateoff(40, 20, /*headless=*/true), fb_zero(40, 20, /*headless=*/true);
    rph(fb_gateoff, mat, /*apply_scale=*/false);  // scale ignored → bump intact
    rph(fb_zero, mat_zero, /*apply_scale=*/true); // nm.x *= 0 → bump flattened

    ASSERT_TRUE(was_drawn(fb_gateoff, 20, 10));
    ASSERT_TRUE(was_drawn(fb_zero, 20, 10));
    if (fb_gateoff.get_pixel(20, 10).r < 240)
    {
        ASSERT_FAIL("gate off: scale must be ignored, redirected normal should give full red");
    }
    if (fb_zero.get_pixel(20, 10).r > 10)
    {
        ASSERT_FAIL("scale=0: nm.x zeroed, normal falls back to N=(0,0,1), dot(+x)=0, no red expected");
    }
}

// Negative scale flips nm.x: with scale=-1 the redirected normal points -x, so
// dot(-x, light +x)=-1 → diffuse clamped to 0 → no red. scale=+1 under the gate
// is the no-op baseline (full red). Catches a dropped sign or a multiply replaced
// with abs/clamp.
TEST(rasterize_phong, normal_scale_negative_flips_bump)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f };
    vec3 ambient{ 0.0f, 0.0f, 0.0f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 0.0f, 0.0f, 0.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    Light light{};
    light.direction = { 1.0f, 0.0f, 0.0f };
    light.color = { 1.0f, 0.0f, 0.0f };
    vec2 uv{ 0.5f, 0.5f };
    Texture nmap_tex = make_tex_rgba(1, 1, { 255, 128, 128, 255 });

    auto rph = [&](Framebuffer &fb, const Material &m)
    {
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, m, nullptr, &nmap_tex, nullptr, nullptr, 0,
            19, nullptr, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, /*apply_normal_scale=*/true
        );
    };

    Material mat_pos = mat, mat_neg = mat;
    mat_pos.normal_scale = 1.0f; // no-op baseline under the gate
    mat_neg.normal_scale = -1.0f;
    Framebuffer fb_pos(40, 20, /*headless=*/true), fb_neg(40, 20, /*headless=*/true);
    rph(fb_pos, mat_pos);
    rph(fb_neg, mat_neg);

    ASSERT_TRUE(was_drawn(fb_pos, 20, 10));
    ASSERT_TRUE(was_drawn(fb_neg, 20, 10));
    if (fb_pos.get_pixel(20, 10).r < 240)
    {
        ASSERT_FAIL("scale=+1: no-op baseline, redirected normal +x should give full red");
    }
    if (fb_neg.get_pixel(20, 10).r > 10)
    {
        ASSERT_FAIL("scale=-1: nm.x flipped, normal points -x, dot(+x)=-1 clamps diffuse to 0, no red expected");
    }
}

// D2: degenerate tangent (tan parallel to normal) must not crash or produce NaN.
// When tan=(0,0,1) == N=(0,0,1): Gram-Schmidt gives T=normalize(0,0,0)=(0,0,0),
// B=cross(N,T)=(0,0,0). Mapped normal = T*nm.x + B*nm.y + N*nm.z = N*nm.z.
// With nmap texel (128,128,255): nm≈(0,0,1) → normal≈N → same as no nmap.
// The test verifies: (a) no crash, (b) pixel is drawn, (c) each channel in [0,255].
TEST(rasterize_phong, normal_map_degenerate_tangent_no_crash)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 tan_degenerate{ 0.0f, 0.0f, 1.0f }; // same direction as normal → T=0 after Gram-Schmidt
    vec3 eye{ 0.0f, 0.0f, 5.0f };
    vec3 ambient{ 0.2f, 0.2f, 0.2f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    Texture nmap_tex = make_tex_rgba(1, 1, { 128, 128, 255, 255 }); // nm≈(0,0,1) → near-identity

    Framebuffer fb(40, 20, /*headless=*/true);
    rasterize_phong(
        fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan_degenerate, tan_degenerate,
        tan_degenerate, uv, uv, uv, 1.0f, 1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, nullptr,
        &nmap_tex, nullptr, nullptr, 0, 19
    );

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    // nmap texel (128,128,255): nm≈(0,0,1) → mapped normal≈N*nm.z≈N.
    // Result should be close to normal-map-free lighting (bright, not black).
    Color c = fb.get_pixel(20, 10);
    if (c.r < 100)
    {
        ASSERT_FAIL(
            "degenerate tangent: T/B=0 so normal falls back to N*nm.z≈N; "
            "expected bright pixel, got R=" +
            std::to_string(static_cast<int>(c.r))
        );
    }
}

// ── Group D3: degenerate tangent — colour value validation ───────────────────

// D3: Validates the pixel VALUE when tangent is degenerate (D2 checks no-crash only).
// tan=(0,0,1) ∥ N=(0,0,1) → Gram-Schmidt yields T=B={0,0,0}.
// nmap texel (128,128,255): nm≈(0,0,1) → normal_mapped = N*nm.z ≈ N.
// Baseline: valid tangent (1,0,0) + nmap=nullptr — vertex normal used directly.
// Both paths resolve to the same effective normal → pixel colours must match.
TEST(rasterize_phong, normal_map_degenerate_tangent_correct_value)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 tan_valid{ 1.0f, 0.0f, 0.0f };
    vec3 tan_degen{ 0.0f, 0.0f, 1.0f }; // parallel to normal → T=B=0 after Gram-Schmidt
    vec3 eye{ 0.0f, 0.0f, 5.0f };
    vec3 ambient{ 0.2f, 0.2f, 0.2f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    Texture nmap_tex = make_tex_rgba(1, 1, { 128, 128, 255, 255 }); // nm≈(0,0,1)

    auto rph = [&](Framebuffer &fb, const vec3 &tan, const Texture *nm)
    {
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, nullptr, nm, nullptr, nullptr, 0, 19
        );
    };

    Framebuffer fb_base(40, 20, /*headless=*/true);
    rph(fb_base, tan_valid, nullptr); // baseline: valid tan, no nmap → vertex normal N

    Framebuffer fb_degen(40, 20, /*headless=*/true);
    rph(fb_degen, tan_degen, &nmap_tex); // subject: degenerate tan + nmap(0,0,1) → normal ≈ N

    ASSERT_TRUE(was_drawn(fb_base, 20, 10));
    ASSERT_TRUE(was_drawn(fb_degen, 20, 10));
    // Degenerate TBN with nm≈(0,0,1) must produce the same colour as using N directly.
    assert_pixel_near(fb_degen, 20, 10, fb_base.get_pixel(20, 10), 5);
}

// ── Group E: diffuse + specular texture simultaneously ────────────────────────

// E1: tex and stex both non-null. The if(tex||stex) block must apply both:
//   mat_tex.diffuse  *= tex->sample_rgb()
//   mat_tex.specular *= stex->sample_rgb()
// Setup: mat.diffuse=(1,1,1), mat.specular=(1,1,1), mat.ambient=(1,1,1).
//   tex = red (255,0,0) → diffuse and ambient become (1,0,0).
//   stex = green (0,255,0) → specular becomes (0,1,0).
// Light dir=(0,0,1), normal=(0,0,1), eye=(0,0,5) → N·L=1, H=N → peak specular.
// With both textures: R comes from diffuse/ambient (red), G from specular (green).
// Without stex (tex only): no green specular → G should be much lower.
TEST(rasterize_phong, diffuse_and_specular_texture_both_applied)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f };
    vec3 ambient{ 1.0f, 1.0f, 1.0f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 1.0f, 1.0f, 1.0f };
    mat.shininess = 32.0f;
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    Texture red_tex = make_tex_rgba(1, 1, { 255, 0, 0, 255 });  // kills G+B diffuse/ambient
    Texture grn_stex = make_tex_rgba(1, 1, { 0, 255, 0, 255 }); // kills R+B specular

    auto rph = [&](Framebuffer &fb, const Texture *tex, const Texture *stex)
    {
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, tex, nullptr, stex, nullptr, 0, 19
        );
    };

    Framebuffer fb_both(40, 20, /*headless=*/true), fb_texonly(40, 20, /*headless=*/true);
    rph(fb_both, &red_tex, &grn_stex);
    rph(fb_texonly, &red_tex, nullptr);

    ASSERT_TRUE(was_drawn(fb_both, 20, 10));
    ASSERT_TRUE(was_drawn(fb_texonly, 20, 10));

    Color both = fb_both.get_pixel(20, 10);
    Color texonly = fb_texonly.get_pixel(20, 10);

    // tex-only (red diffuse, white specular): B_specular=1 → B≈255.
    // both (red diffuse, green specular): B_specular=0, B_diffuse=0 → B≈0.
    // Green stex zeroes the blue specular channel; this is the distinguishing observable.
    if (texonly.b < 200)
    {
        ASSERT_FAIL(
            "tex-only: white specular should give high B, got B=" + std::to_string(static_cast<int>(texonly.b))
        );
    }
    if (both.b > 20)
    {
        ASSERT_FAIL(
            "both textures: green stex zeroes B specular, expected B≈0, got B=" +
            std::to_string(static_cast<int>(both.b))
        );
    }
}

// ── Group F: specular texture combined with alpha cutout ─────────────────────

// F1: has_cutout=true (alpha_cutoff=0.5, opaque white diffuse tex) AND stex != nullptr.
// When has_cutout=true the UV is computed in the cutout pre-pass; the post-depth
// UV recompute block (`if (!has_cutout && ...)`) is skipped.  stex sampling uses
// that pre-pass UV.  This is the only test exercising the stex+cutout code path.
//
// Setup: mat.diffuse=ambient=(0,0,0), specular=(1,1,1), shininess=32.
//   eye=(0,0,5), light=(0,0,1), normal=(0,0,1) → H=N → peak specular on the no-stex run.
//   diff_tex: white+opaque (alpha=255 ≥ 0.5*255) → cutout passes; cutout_rgb=(1,1,1).
//   black_stex: (0,0,0) → mat_tex.specular*=(0,0,0)=(0,0,0) → R≈0.
// Without stex: full specular → R≈255.  With black stex: R≈0.
TEST(rasterize_phong, specular_tex_and_cutout_active)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f };
    vec3 ambient{ 0.0f, 0.0f, 0.0f };
    Material mat{};
    mat.diffuse = { 0.0f, 0.0f, 0.0f };
    mat.ambient = { 0.0f, 0.0f, 0.0f };
    mat.specular = { 1.0f, 1.0f, 1.0f };
    mat.shininess = 32.0f;
    mat.alpha_cutoff = 0.5f;
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    Texture diff_tex = make_tex_rgba(1, 1, { 255, 255, 255, 255 }); // opaque white → passes cutout
    Texture black_stex = make_tex_rgba(1, 1, { 0, 0, 0, 255 });     // zeroes specular

    auto rph = [&](Framebuffer &fb, const Texture *stex)
    {
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, &diff_tex, nullptr, stex, nullptr, 0,
            19
        );
    };

    Framebuffer fb_nostex(40, 20, /*headless=*/true), fb_stex(40, 20, /*headless=*/true);
    rph(fb_nostex, nullptr);
    rph(fb_stex, &black_stex);

    ASSERT_TRUE(was_drawn(fb_nostex, 20, 10));
    ASSERT_TRUE(was_drawn(fb_stex, 20, 10));
    if (fb_nostex.get_pixel(20, 10).r < 240)
    {
        ASSERT_FAIL(
            "cutout+no-stex: peak specular expected, got R=" +
            std::to_string(static_cast<int>(fb_nostex.get_pixel(20, 10).r))
        );
    }
    if (fb_stex.get_pixel(20, 10).r > 5)
    {
        ASSERT_FAIL(
            "cutout+black-stex: specular should be zeroed, got R=" +
            std::to_string(static_cast<int>(fb_stex.get_pixel(20, 10).r))
        );
    }
}

// ── Group F: glTF metallic-roughness remap ────────────────────────────────────
// rasterize_phong tints specular reflectance toward the base colour when
// mat.metallic > 0:  specular(F0) = lerp(0.04, base, m),  m = mat.metallic * MR_tex.b.
// Diffuse is intentionally NOT zeroed (no IBL → diffuse-less metals go near-black).
// Dielectrics (metallic == 0) are untouched.

// F1: a metal keeps its diffuse-lit surface. Grazing view (eye on +x, light/normal
// on +z) gives full N·L (diffuse) but tiny N·H (no specular peak), so brightness can
// only come from diffuse. Both metal and dielectric stay lit — guards against
// re-introducing the diffuse-kill, which would render the metal near-black here.
TEST(rasterize_phong, metallic_keeps_diffuse)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 5.0f, 0.0f, 0.0f }; // grazing: V≈(1,0,0), so N·H≈0.707 → 0.707^32 ≈ 0
    vec3 ambient{ 0.0f, 0.0f, 0.0f };
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };

    auto rph = [&](Framebuffer &fb, float metallic)
    {
        Material mat{};
        mat.diffuse = { 1.0f, 1.0f, 1.0f };
        mat.ambient = { 0.0f, 0.0f, 0.0f };
        mat.specular = { 0.0f, 0.0f, 0.0f };
        mat.shininess = 32.0f;
        mat.metallic = metallic;
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, nullptr, nullptr, nullptr, nullptr, 0,
            19
        );
    };

    Framebuffer fb_diel(40, 20, /*headless=*/true), fb_metal(40, 20, /*headless=*/true);
    rph(fb_diel, 0.0f);
    rph(fb_metal, 1.0f);

    ASSERT_TRUE(was_drawn(fb_diel, 20, 10));
    ASSERT_TRUE(was_drawn(fb_metal, 20, 10));
    if (fb_diel.get_pixel(20, 10).r < 240)
    {
        ASSERT_FAIL(
            "dielectric: full diffuse expected, got R=" + std::to_string(static_cast<int>(fb_diel.get_pixel(20, 10).r))
        );
    }
    if (fb_metal.get_pixel(20, 10).r < 240)
    {
        ASSERT_FAIL(
            "metal: diffuse must be kept (not zeroed), got R=" +
            std::to_string(static_cast<int>(fb_metal.get_pixel(20, 10).r))
        );
    }
}

// F2: metallic=1 tints F0 by the base colour. Peak specular (H=N) with a blue base
// must stay blue — a regression to the old white specular={metallic} would light up
// R/G. An MR texture with B=0 (dielectric texel) overrides metallic back to 0.
TEST(rasterize_phong, metallic_tints_specular_and_mr_texture_modulates)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f }; // V=(0,0,1), light=(0,0,1) → H=N → peak specular
    vec3 ambient{ 0.0f, 0.0f, 0.0f };
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    Texture mr_dielectric = make_tex_rgba(1, 1, { 0, 255, 0, 255 }); // B=0 metallic, G=1 roughness

    auto rph = [&](Framebuffer &fb, const Texture *mrtex)
    {
        Material mat{};
        mat.diffuse = { 0.0f, 0.0f, 1.0f }; // blue base colour
        mat.ambient = { 0.0f, 0.0f, 0.0f };
        mat.specular = { 0.0f, 0.0f, 0.0f };
        mat.shininess = 32.0f;
        mat.metallic = 1.0f;
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, nullptr, nullptr, nullptr, nullptr, 0,
            19, mrtex
        );
    };

    Framebuffer fb_metal(40, 20, /*headless=*/true), fb_mr(40, 20, /*headless=*/true);
    rph(fb_metal, nullptr);
    rph(fb_mr, &mr_dielectric);

    ASSERT_TRUE(was_drawn(fb_metal, 20, 10));
    Color metal = fb_metal.get_pixel(20, 10);
    if (metal.b < 240)
    {
        ASSERT_FAIL("metal: blue F0 highlight expected, got B=" + std::to_string(static_cast<int>(metal.b)));
    }
    if (metal.r > 20)
    {
        ASSERT_FAIL("metal: highlight should be blue not white, got R=" + std::to_string(static_cast<int>(metal.r)));
    }

    // MR texel B=0 forces m=0 → dielectric: F0 = lerp(0.04, base, 0) ≈ 0.04 (a faint
    // highlight, R≈10), with the blue base diffuse dominating — not a blue metal F0.
    Color mr = fb_mr.get_pixel(20, 10);
    if (mr.b < 240)
    {
        ASSERT_FAIL("mr-dielectric: full blue diffuse expected, got B=" + std::to_string(static_cast<int>(mr.b)));
    }
    if (mr.r > 20)
    {
        ASSERT_FAIL("mr-dielectric: red should stay zero, got R=" + std::to_string(static_cast<int>(mr.r)));
    }
}
