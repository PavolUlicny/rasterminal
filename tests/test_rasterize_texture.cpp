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
// Catches: Phong UV interpolation diverges from the rasterize() path (separate code).
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

// ── Group O: glTF occlusion texture overrides baked AO (Phong) ────────────────
//
// Isolate AO: no lights, ambient={1,0,0}, mat defaults (mat.ambient={1,1,1}), so the
// lit colour is exactly ambient * mat.ambient * ao → R = 255 * ao. uv={0.5,0.5} hits
// the single texel of any 1×1 occlusion map. octex R channel + strength drive ao via
// ao = 1 + strength*(R-1); when octex is null the interpolated vertex ao is used as-is.

namespace
{
    const vec3 g_occl_sa{ 4.0f, 2.0f, 0.5f }, g_occl_sb{ 36.0f, 2.0f, 0.5f }, g_occl_sc{ 20.0f, 18.0f, 0.5f };

    void rast_occl(Framebuffer &fb, const Texture *octex, float strength, float ao_vert)
    {
        vec3 zero{}, normal{ 0.0f, 0.0f, -1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
        vec3 eye{ 20.0f, 10.0f, -100.0f };
        vec3 ambient{ 1.0f, 0.0f, 0.0f };
        vec2 uv{ 0.5f, 0.5f };
        Material mat{};
        rasterize_phong(
            fb, g_occl_sa, g_occl_sb, g_occl_sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan,
            tan, uv, uv, uv, ao_vert, ao_vert, ao_vert, white, white, white, false, eye, nullptr, 0, ambient, mat,
            nullptr, nullptr, nullptr, nullptr, 0, 19, nullptr, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, false, octex,
            strength
        );
    }
} // namespace

// R=128 (≈0.502) at full strength darkens ambient: ao 1.0 → 0.502, R 255 → ≈128.
TEST(rasterize_phong, occlusion_darkens_ambient)
{
    Texture occ = make_tex_rgba(1, 1, { 128, 128, 128, 255 });
    Framebuffer fb_none(40, 20, /*headless=*/true), fb_occ(40, 20, /*headless=*/true);
    rast_occl(fb_none, nullptr, 1.0f, 1.0f);
    rast_occl(fb_occ, &occ, 1.0f, 1.0f);

    ASSERT_TRUE(was_drawn(fb_none, 20, 10));
    ASSERT_TRUE(was_drawn(fb_occ, 20, 10));
    assert_pixel_near(fb_none, 20, 10, Color{ 255, 0, 0 }, 3);
    assert_pixel_near(fb_occ, 20, 10, Color{ 128, 0, 0 }, 5);
}

// strength=0 collapses the override to a no-op (ao = 1 + 0*(R-1) = 1), matching the
// no-occlusion result regardless of the texel value.
TEST(rasterize_phong, occlusion_strength_zero_is_noop)
{
    Texture occ = make_tex_rgba(1, 1, { 0, 0, 0, 255 }); // R=0 would fully occlude at strength 1
    Framebuffer fb(40, 20, /*headless=*/true);
    rast_occl(fb, &occ, 0.0f, 1.0f);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    assert_pixel_near(fb, 20, 10, Color{ 255, 0, 0 }, 3);
}

// Override, not multiply: a low baked vertex AO (0.2) with an occlusion map of R=1.0 at
// full strength yields ao = 1 + 1*(1-1) = 1.0 → R≈255. Multiplying would give 0.2*1=0.2
// → R≈51. The high R proves the texture replaces the baked AO rather than stacking.
TEST(rasterize_phong, occlusion_replaces_not_multiplies_baked_ao)
{
    Texture occ_full = make_tex_rgba(1, 1, { 255, 255, 255, 255 }); // R=1.0 → ao override to 1.0
    Framebuffer fb_baked(40, 20, /*headless=*/true), fb_occ(40, 20, /*headless=*/true);
    rast_occl(fb_baked, nullptr, 1.0f, 0.2f); // baked-only: ao=0.2 → R≈51
    rast_occl(fb_occ, &occ_full, 1.0f, 0.2f); // occlusion replaces baked → ao=1.0 → R≈255

    ASSERT_TRUE(was_drawn(fb_baked, 20, 10));
    ASSERT_TRUE(was_drawn(fb_occ, 20, 10));
    assert_pixel_near(fb_baked, 20, 10, Color{ 51, 0, 0 }, 6);
    assert_pixel_near(fb_occ, 20, 10, Color{ 255, 0, 0 }, 3);
}

// ORM packing: when the occlusion and metallic-roughness textures are the SAME image,
// rasterize_phong samples it once and reuses the read for both AO (R) and metalness/
// roughness (B/G). The shared-pointer path (occ_is_mr) must match passing two distinct
// but identical textures (two separate samples) — a wrong-channel or wrong-variable
// reuse would diverge. Channels are distinct (R≠G≠B) so a misroute is visible.
TEST(rasterize_phong, orm_shared_occlusion_mr_sample_matches_separate)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f };
    vec3 ambient{ 0.6f, 0.6f, 0.6f }; // nonzero so the AO (R) channel affects the pixel
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };
    // R=64 (AO≈0.25), G=128 (roughness), B=255 (metallic 1.0): distinct so a misroute shows.
    Texture orm_a = make_tex_rgba(1, 1, { 64, 128, 255, 255 });
    Texture orm_b = make_tex_rgba(1, 1, { 64, 128, 255, 255 }); // identical content, different pointer

    auto rph = [&](Framebuffer &fb, const Texture *octex, const Texture *mrtex)
    {
        Material mat{};
        mat.diffuse = { 0.0f, 0.0f, 1.0f };
        mat.ambient = { 1.0f, 1.0f, 1.0f };
        mat.specular = { 0.0f, 0.0f, 0.0f };
        mat.metallic = 1.0f;
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, nullptr, nullptr, nullptr, nullptr, 0,
            19, mrtex, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, false, octex, 1.0f
        );
    };

    Framebuffer fb_shared(40, 20, /*headless=*/true), fb_separate(40, 20, /*headless=*/true);
    rph(fb_shared, &orm_a, &orm_a);   // same pointer → occ_is_mr → one shared sample
    rph(fb_separate, &orm_a, &orm_b); // distinct pointers → two separate samples

    ASSERT_TRUE(was_drawn(fb_shared, 20, 10));
    ASSERT_TRUE(was_drawn(fb_separate, 20, 10));
    assert_pixel_near(fb_separate, 20, 10, fb_shared.get_pixel(20, 10), 1);
}

// ── Group T1: per-slot UV set selection (TEXCOORD_1) ──────────────────────────
//
// 2×1 texture (left red, right blue). uv0 samples the red half, uv1 the blue half.
// rasterize() reads the diffuse binding's uv_set from the Material*, so flipping
// mat.diffuse_map.uv_set must switch which set the diffuse sample uses — proving the
// second-set plumbing reaches the sampler.
TEST(rasterize, diffuse_uv_set_selects_texcoord1)
{
    Texture tex = make_tex_rgba(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 white{ 1.0f, 1.0f, 1.0f };
    vec3 zero{};
    vec2 uv0{ 0.25f, 0.5f }; // texel-0 (red) centre
    vec2 uv1{ 0.75f, 0.5f }; // texel-1 (blue) centre

    const auto run = [&](Framebuffer &fb, uint8_t set)
    {
        Material mat;
        mat.diffuse_map.uv_set = set;
        rasterize(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, white, white, white, zero, zero, zero, uv0, uv0, uv0,
            &tex, 0.0f, nullptr, 0, 19, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, uv1, uv1, uv1, &mat
        );
    };

    Framebuffer fb0(40, 20, /*headless=*/true), fb1(40, 20, /*headless=*/true);
    run(fb0, 0); // diffuse on TEXCOORD_0 → samples red half
    run(fb1, 1); // diffuse on TEXCOORD_1 → samples blue half

    ASSERT_TRUE(was_drawn(fb0, 20, 10));
    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    const Color c0 = fb0.get_pixel(20, 10);
    const Color c1 = fb1.get_pixel(20, 10);
    if (c0.r <= c0.b)
    {
        ASSERT_FAIL("uv_set 0 should sample the red half (R > B)");
    }
    if (c1.b <= c1.r)
    {
        ASSERT_FAIL("uv_set 1 should sample the blue half (B > R)");
    }
}

// KHR_texture_transform reaches the sampler: a 2×1 red|blue texture sampled at uv0 (red half)
// shifts to the blue half once the diffuse slot carries a +0.5 u-offset affine. Proves the
// post-select transform apply in rasterize().
TEST(rasterize, diffuse_texture_transform_shifts_sample)
{
    Texture tex = make_tex_rgba(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 white{ 1.0f, 1.0f, 1.0f };
    vec3 zero{};
    vec2 uv0{ 0.25f, 0.5f }; // red texel centre; +0.5 u-offset lands on the blue texel

    const auto run = [&](Framebuffer &fb, bool xf)
    {
        Material mat;
        if (xf)
        {
            mat.diffuse_map.has_transform = true;
            mat.diffuse_map.t[0] = 1.0f;
            mat.diffuse_map.t[1] = 0.0f;
            mat.diffuse_map.t[2] = 0.5f; // u += 0.5
            mat.diffuse_map.t[3] = 0.0f;
            mat.diffuse_map.t[4] = 1.0f;
            mat.diffuse_map.t[5] = 0.0f;
        }
        rasterize(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, white, white, white, zero, zero, zero, uv0, uv0, uv0,
            &tex, 0.0f, nullptr, 0, 19, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, uv0, uv0, uv0, &mat
        );
    };

    Framebuffer fb0(40, 20, /*headless=*/true), fb1(40, 20, /*headless=*/true);
    run(fb0, false); // no transform → red half
    run(fb1, true);  // +0.5 u-offset → blue half

    ASSERT_TRUE(was_drawn(fb0, 20, 10));
    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    const Color c0 = fb0.get_pixel(20, 10);
    const Color c1 = fb1.get_pixel(20, 10);
    if (c0.r <= c0.b)
    {
        ASSERT_FAIL("no transform should sample the red half (R > B)");
    }
    if (c1.b <= c1.r)
    {
        ASSERT_FAIL("u-offset transform should sample the blue half (B > R)");
    }
}

// Phong path: the diffuse binding's uv_set (read from mat) selects which set the diffuse
// sample uses, exactly as in rasterize(). 2×1 red|blue texture; uv0→red, uv1→blue.
TEST(rasterize_phong, diffuse_uv_set_selects_texcoord1)
{
    Texture tex = make_tex_rgba(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f }, ambient{ 0.4f, 0.4f, 0.4f };
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv0{ 0.25f, 0.5f }; // red texel
    vec2 uv1{ 0.75f, 0.5f }; // blue texel

    const auto run = [&](Framebuffer &fb, uint8_t set)
    {
        Material mat{};
        mat.specular = { 0.0f, 0.0f, 0.0f }; // no white specular wash → red/blue stays clean
        mat.diffuse_map.uv_set = set;
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv0, uv0, uv0,
            1.0f, 1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, &tex, nullptr, nullptr, nullptr,
            0, 19, nullptr, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, false, nullptr, 1.0f, uv1, uv1, uv1
        );
    };

    Framebuffer fb0(40, 20, /*headless=*/true), fb1(40, 20, /*headless=*/true);
    run(fb0, 0);
    run(fb1, 1);

    ASSERT_TRUE(was_drawn(fb0, 20, 10));
    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    const Color c0 = fb0.get_pixel(20, 10);
    const Color c1 = fb1.get_pixel(20, 10);
    if (c0.r <= c0.b)
    {
        ASSERT_FAIL("phong uv_set 0 should sample the red half (R > B)");
    }
    if (c1.b <= c1.r)
    {
        ASSERT_FAIL("phong uv_set 1 should sample the blue half (B > R)");
    }
}

// Phong path: KHR_texture_transform on the diffuse slot shifts the sampled texel, mirroring the
// rasterize() case. 2×1 red|blue texture; a +0.5 u-offset moves the red sample to blue.
TEST(rasterize_phong, diffuse_texture_transform_shifts_sample)
{
    Texture tex = make_tex_rgba(2, 1, { 255, 0, 0, 255, 0, 0, 255, 255 });
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f }, ambient{ 0.4f, 0.4f, 0.4f };
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv0{ 0.25f, 0.5f }; // red texel; +0.5 u-offset lands on blue

    const auto run = [&](Framebuffer &fb, bool xf)
    {
        Material mat{};
        mat.specular = { 0.0f, 0.0f, 0.0f }; // keep red/blue clean
        if (xf)
        {
            mat.diffuse_map.has_transform = true;
            mat.diffuse_map.t[0] = 1.0f;
            mat.diffuse_map.t[1] = 0.0f;
            mat.diffuse_map.t[2] = 0.5f;
            mat.diffuse_map.t[3] = 0.0f;
            mat.diffuse_map.t[4] = 1.0f;
            mat.diffuse_map.t[5] = 0.0f;
        }
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv0, uv0, uv0,
            1.0f, 1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, &tex, nullptr, nullptr, nullptr,
            0, 19, nullptr, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, false, nullptr, 1.0f, uv0, uv0, uv0
        );
    };

    Framebuffer fb0(40, 20, /*headless=*/true), fb1(40, 20, /*headless=*/true);
    run(fb0, false);
    run(fb1, true);

    ASSERT_TRUE(was_drawn(fb0, 20, 10));
    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    const Color c0 = fb0.get_pixel(20, 10);
    const Color c1 = fb1.get_pixel(20, 10);
    if (c0.r <= c0.b)
    {
        ASSERT_FAIL("phong no transform should sample the red half (R > B)");
    }
    if (c1.b <= c1.r)
    {
        ASSERT_FAIL("phong u-offset transform should sample the blue half (B > R)");
    }
}

// Per-slot routing of the transform beyond diffuse: the occlusion slot carries a +0.5 u-offset
// while diffuse is untransformed. The occlusion R channel scales ambient, so shifting the
// occlusion sample from the bright texel to the dark one darkens the result — proving each slot
// applies its own transform (and that the non-diffuse Phong sample sites are wired).
TEST(rasterize_phong, occlusion_texture_transform_independent_of_diffuse)
{
    Texture occ = make_tex_rgba(2, 1, { 255, 0, 0, 255, 0, 0, 0, 255 }); // texel0 bright, texel1 dark
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f }, ambient{ 1.0f, 1.0f, 1.0f };
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv0{ 0.25f, 0.5f }; // occlusion texel0 (bright); +0.5 u-offset → texel1 (dark)

    const auto run = [&](Framebuffer &fb, bool xf)
    {
        Material mat{};
        mat.diffuse = { 0.3f, 0.0f, 0.0f };
        mat.ambient = { 0.5f, 0.0f, 0.0f };
        mat.specular = { 0.0f, 0.0f, 0.0f };
        if (xf)
        {
            mat.occlusion_map.has_transform = true;
            mat.occlusion_map.t[0] = 1.0f;
            mat.occlusion_map.t[1] = 0.0f;
            mat.occlusion_map.t[2] = 0.5f;
            mat.occlusion_map.t[3] = 0.0f;
            mat.occlusion_map.t[4] = 1.0f;
            mat.occlusion_map.t[5] = 0.0f;
        }
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv0, uv0, uv0,
            1.0f, 1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, nullptr, nullptr, nullptr,
            nullptr, 0, 19, nullptr, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, false, &occ, 1.0f, uv0, uv0, uv0
        );
    };

    Framebuffer fb0(40, 20, /*headless=*/true), fb1(40, 20, /*headless=*/true);
    run(fb0, false); // bright occlusion texel → brighter
    run(fb1, true);  // transform → dark occlusion texel → darker

    ASSERT_TRUE(was_drawn(fb0, 20, 10));
    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    const Color c0 = fb0.get_pixel(20, 10);
    const Color c1 = fb1.get_pixel(20, 10);
    if (c0.r <= c1.r + 30)
    {
        ASSERT_FAIL(
            "occlusion transform should shift to the dark texel (clearly darker): got R0=" +
            std::to_string(static_cast<int>(c0.r)) + " R1=" + std::to_string(static_cast<int>(c1.r))
        );
    }
}

// Cutout pre-pass honours the diffuse transform: a 2×1 texture with an opaque texel0 and a fully
// transparent texel1, alpha_cutoff active. uv0 samples opaque (fragment kept) but a +0.5 u-offset
// shifts the alpha sample to the transparent texel → the fragment is discarded (not drawn). This
// exercises the transform application in the cutout pre-pass, distinct from the colour sample.
TEST(rasterize, cutout_pre_pass_honours_diffuse_transform)
{
    Texture tex = make_tex_rgba(2, 1, { 255, 255, 255, 255, 255, 255, 255, 0 }); // texel0 opaque, texel1 transparent
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 white{ 1.0f, 1.0f, 1.0f };
    vec3 zero{};
    vec2 uv0{ 0.25f, 0.5f }; // opaque texel; +0.5 u-offset → transparent texel

    const auto run = [&](Framebuffer &fb, bool xf)
    {
        Material mat;
        if (xf)
        {
            mat.diffuse_map.has_transform = true;
            mat.diffuse_map.t[0] = 1.0f;
            mat.diffuse_map.t[1] = 0.0f;
            mat.diffuse_map.t[2] = 0.5f;
            mat.diffuse_map.t[3] = 0.0f;
            mat.diffuse_map.t[4] = 1.0f;
            mat.diffuse_map.t[5] = 0.0f;
        }
        rasterize(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, white, white, white, white, white, white, zero, zero, zero, uv0, uv0, uv0,
            &tex, 0.5f, nullptr, 0, 19, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, uv0, uv0, uv0, &mat
        );
    };

    Framebuffer fb0(40, 20, /*headless=*/true), fb1(40, 20, /*headless=*/true);
    run(fb0, false); // opaque texel → fragment kept
    run(fb1, true);  // transform → transparent texel → fragment discarded

    ASSERT_TRUE(was_drawn(fb0, 20, 10));
    if (was_drawn(fb1, 20, 10))
    {
        ASSERT_FAIL("cutout pre-pass should sample the transformed (transparent) texel and discard");
    }
}

// Per-slot independence in the Phong path: the occlusion texture sits on TEXCOORD_1 while the
// (untextured) diffuse stays on set 0. The occlusion R channel scales the ambient term, so
// flipping ONLY occlusion_map.uv_set changes brightness — proving each slot resolves its own
// set rather than one global choice. 2×1 occlusion: texel0 bright (ao≈1), texel1 dark (ao≈0).
TEST(rasterize_phong, occlusion_uv_set_independent_of_diffuse)
{
    Texture occ = make_tex_rgba(2, 1, { 255, 0, 0, 255, 0, 0, 0, 255 });
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f }, ambient{ 1.0f, 1.0f, 1.0f };
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv0{ 0.25f, 0.5f }; // occlusion texel0 (bright → ao 1)
    vec2 uv1{ 0.75f, 0.5f }; // occlusion texel1 (dark → ao 0)

    const auto run = [&](Framebuffer &fb, uint8_t occ_set)
    {
        Material mat{};
        mat.diffuse = { 0.3f, 0.0f, 0.0f };  // headroom so ao change is visible (not clamped)
        mat.ambient = { 0.5f, 0.0f, 0.0f };  // ambient term is what occlusion scales
        mat.specular = { 0.0f, 0.0f, 0.0f }; // no specular wash
        mat.occlusion_map.uv_set = occ_set;  // diffuse stays default set 0
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv0, uv0, uv0,
            1.0f, 1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, nullptr, nullptr, nullptr,
            nullptr, 0, 19, nullptr, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, false, &occ, 1.0f, uv1, uv1, uv1
        );
    };

    Framebuffer fb0(40, 20, /*headless=*/true), fb1(40, 20, /*headless=*/true);
    run(fb0, 0); // occlusion on set 0 → bright texel → ao 1 → brighter
    run(fb1, 1); // occlusion on set 1 → dark texel  → ao 0 → darker

    ASSERT_TRUE(was_drawn(fb0, 20, 10));
    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    const Color c0 = fb0.get_pixel(20, 10);
    const Color c1 = fb1.get_pixel(20, 10);
    if (c0.r <= c1.r + 30)
    {
        ASSERT_FAIL(
            "occlusion on set 0 (bright) should be clearly brighter than on set 1 (dark): got R0=" +
            std::to_string(static_cast<int>(c0.r)) + " R1=" + std::to_string(static_cast<int>(c1.r))
        );
    }
}

// ORM dedup + different UV sets: two glTF bindings can collapse to one Texture* (the dedup key
// ignores texCoord) yet select different sets. The occ_is_mr fast path (sample once, reuse the
// AO read for metalness) is then INVALID — it must be gated on the sets matching, not just the
// pointer. Here occlusion is on set 0 and metallic-roughness on set 1 of the SAME texture; the
// MR sample must come from set 1, not reuse the set-0 occlusion read.
//
// Clamp wrap + uv {0,1} read the two texels purely (no bilinear blend). texel0/texel1 share AO
// (R) and roughness (G) but differ in metallic (B): 0 vs 255. Run A (mr on set 1) → metal 1 →
// red-tinted specular → R saturates. Control B (mr on set 0 → occ_is_mr reuse) → metal 0 → dim
// R. If the guard ignored sets, A would wrongly reuse the set-0 sample and match B.
TEST(rasterize_phong, orm_dedup_different_uv_sets_samples_mr_independently)
{
    Texture tex = make_tex_rgba(2, 1, { 64, 128, 0, 255, 64, 128, 255, 255 }); // B: texel0=0, texel1=255
    tex.wrap_s = WrapMode::Clamp;                                              // pure texel reads at u=0 / u=1
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f }, ambient{ 0.05f, 0.05f, 0.05f };
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f }; // L=V=N → max specular (n·h = 1)
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv0{ 0.0f, 0.5f }; // → texel0 (metal 0)
    vec2 uv1{ 1.0f, 0.5f }; // → texel1 (metal 1)

    const auto run = [&](Framebuffer &fb, uint8_t mr_set)
    {
        Material mat{};
        // Red base: metalness lerps specular toward it. 0.5 keeps the metal-1 case saturating R
        // while the metal-0 case (specular F0 0.04) stays mid-range → a large, clamp-separated gap.
        mat.diffuse = { 0.5f, 0.0f, 0.0f };
        mat.ambient = { 0.5f, 0.0f, 0.0f };
        mat.metallic = 1.0f;
        mat.occlusion_map.uv_set = 0; // occlusion always set 0 → texel0
        mat.mr_map.uv_set = mr_set;   // varies
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv0, uv0, uv0,
            1.0f, 1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, nullptr, nullptr, nullptr,
            nullptr, 0, 19, &tex, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, false, &tex, 1.0f, uv1, uv1, uv1
        );
    };

    Framebuffer fbA(40, 20, /*headless=*/true), fbB(40, 20, /*headless=*/true);
    run(fbA, 1); // mr on set 1 → texel1 → metal 1 → red specular → bright R
    run(fbB, 0); // mr on set 0 → occ_is_mr reuse → texel0 → metal 0 → dim R

    ASSERT_TRUE(was_drawn(fbA, 20, 10));
    ASSERT_TRUE(was_drawn(fbB, 20, 10));
    const Color cA = fbA.get_pixel(20, 10);
    const Color cB = fbB.get_pixel(20, 10);
    if (cA.r <= cB.r + 100)
    {
        ASSERT_FAIL(
            "mr on set 1 (metal 1) must sample independently of the set-0 occlusion read: got RA=" +
            std::to_string(static_cast<int>(cA.r)) + " RB=" + std::to_string(static_cast<int>(cB.r))
        );
    }
}

// Symmetric to the uv_set case above, for KHR_texture_transform: occlusion and MR dedup to one
// Texture* on the SAME uv set, but MR carries a +1.0 u-offset transform while occlusion has none.
// The transforms differ, so occ_is_mr must be false and MR must sample its own (transformed)
// texel — not reuse the occlusion sample. Guards the same_uv_mapping transform check.
TEST(rasterize_phong, orm_dedup_different_transforms_samples_mr_independently)
{
    Texture tex = make_tex_rgba(2, 1, { 64, 128, 0, 255, 64, 128, 255, 255 }); // B: texel0=0, texel1=255
    tex.wrap_s = WrapMode::Clamp;                                              // pure texel reads at u=0 / u=1
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 zero{}, normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f }, white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 0.0f, 0.0f, 5.0f }, ambient{ 0.05f, 0.05f, 0.05f };
    Light light{};
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    vec2 uv0{ 0.0f, 0.5f }; // → texel0 (metal 0); occlusion (no transform) reads this

    const auto run = [&](Framebuffer &fb, bool mr_xf)
    {
        Material mat{};
        mat.diffuse = { 0.5f, 0.0f, 0.0f };
        mat.ambient = { 0.5f, 0.0f, 0.0f };
        mat.metallic = 1.0f;
        // Both bindings on set 0; only MR carries a +1.0 u-offset → reads texel1 (metal 1).
        if (mr_xf)
        {
            mat.mr_map.has_transform = true;
            mat.mr_map.t[0] = 1.0f;
            mat.mr_map.t[1] = 0.0f;
            mat.mr_map.t[2] = 1.0f; // u += 1.0 → texel1 under Clamp
            mat.mr_map.t[3] = 0.0f;
            mat.mr_map.t[4] = 1.0f;
            mat.mr_map.t[5] = 0.0f;
        }
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, zero, zero, zero, normal, normal, normal, tan, tan, tan, uv0, uv0, uv0,
            1.0f, 1.0f, 1.0f, white, white, white, false, eye, &light, 1, ambient, mat, nullptr, nullptr, nullptr,
            nullptr, 0, 19, &tex, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, false, &tex, 1.0f, uv0, uv0, uv0
        );
    };

    Framebuffer fbA(40, 20, /*headless=*/true), fbB(40, 20, /*headless=*/true);
    run(fbA, true);  // MR transform → texel1 → metal 1 → red specular → bright R
    run(fbB, false); // no MR transform → occ_is_mr reuse → texel0 → metal 0 → dim R

    ASSERT_TRUE(was_drawn(fbA, 20, 10));
    ASSERT_TRUE(was_drawn(fbB, 20, 10));
    const Color cA = fbA.get_pixel(20, 10);
    const Color cB = fbB.get_pixel(20, 10);
    if (cA.r <= cB.r + 100)
    {
        ASSERT_FAIL(
            "MR with its own transform must not reuse the occlusion sample: got RA=" +
            std::to_string(static_cast<int>(cA.r)) + " RB=" + std::to_string(static_cast<int>(cB.r))
        );
    }
}
