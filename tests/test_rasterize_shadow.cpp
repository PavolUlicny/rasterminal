#include "test.h"
#include "rasterize_test_util.h"
#include "../src/rasterize.h"
#include "../src/shadow.h"

// ─── helpers ──────────────────────────────────────────────────────────────────

// Flat XY triangle spanning ±10 units at z=0 — used as the occluder mesh
// when building the shadow map.
static Mesh make_occluder_z0()
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.pos = {-10.0f, -10.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {10.0f, -10.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 10.0f, 0.0f};
    m.vertices.push_back(v);
    m.triangles.push_back({{0, 1, 2}});
    m.materials.push_back({});
    return m;
}

// Light from +Z used for all shadow tests.
static Light make_light_z_shadow()
{
    Light l{};
    l.direction = {0.0f, 0.0f, 1.0f};
    l.color = {1.0f, 1.0f, 1.0f};
    return l;
}

// rasterize() wrapper: canonical screen triangle, caller-supplied world positions,
// lit and shadowed colors, and shadow map.
// Screen triangle: sa=(4,2), sb=(36,2), sc=(20,18) on 40×20 fb.
static void rast_shadow(Framebuffer &fb,
                        vec3 pa, vec3 pb, vec3 pc,
                        vec3 col, vec3 shad,
                        const ShadowMap *sm)
{
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec2 uv{0.5f, 0.5f};
    rasterize(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f,
              col, col, col,
              shad, shad, shad,
              pa, pb, pc,
              uv, uv, uv,
              nullptr, 0.0f, sm, 0, 19);
}

// rasterize_phong() wrapper: canonical triangle, caller-supplied world positions,
// lights, material, and shadow map. ambient=(0,0,0) so ambient terms don't mask
// whether the key light is included or excluded.
static void rast_phong_shadow(Framebuffer &fb,
                              vec3 pa, vec3 pb, vec3 pc,
                              const Light *lights, int n_lights,
                              const Material &mat,
                              const ShadowMap *sm)
{
    vec3 sa{4.0f, 2.0f, 0.5f}, sb{36.0f, 2.0f, 0.5f}, sc{20.0f, 18.0f, 0.5f};
    vec3 normal{0.0f, 0.0f, 1.0f}, tan{1.0f, 0.0f, 0.0f};
    vec3 white{1.0f, 1.0f, 1.0f};
    vec3 eye{20.0f, 10.0f, -100.0f};
    vec3 ambient{0.0f, 0.0f, 0.0f};
    vec2 uv{0.5f, 0.5f};
    rasterize_phong(fb, sa, sb, sc, 1.0f, 1.0f, 1.0f,
                    pa, pb, pc,
                    normal, normal, normal,
                    tan, tan, tan,
                    uv, uv, uv,
                    1.0f, 1.0f, 1.0f,
                    white, white, white, false,
                    eye, lights, n_lights, ambient, mat,
                    nullptr, nullptr, nullptr, sm,
                    0, 19);
}

// ─── rasterizer shadow integration ───────────────────────────────────────────
//
// Canonical triangle: sa=(4,2), sb=(36,2), sc=(20,18) on 40×20 framebuffer.
// Occluder: flat XY triangle at z=0. Light from +Z.
// "Above" = world z>0 (between light and occluder) → lit.
// "Below" = world z<0 (on far side of occluder from light) → in shadow.

// ── Group A: rasterize() (Gouraud/Flat path) ─────────────────────────────────

// S2: pa/pb/pc at z=+10 (above occluder, lit). shadow_map=nullptr and a valid
// map must both produce the same result — lerp(col,shad,sf=0)=col in both cases.
// Catches: nullptr short-circuit broken, or a lit query still uses shad color.
TEST(rasterize, shadow_lit_position_matches_nullptr)
{
    Light light = make_light_z_shadow();
    ShadowMap sm = build_shadow_map(make_occluder_z0(), light);
    vec3 above{0.0f, 0.0f, 10.0f};
    vec3 red{1.0f, 0.0f, 0.0f}, blue{0.0f, 0.0f, 1.0f};

    Framebuffer fb_null(40, 20, /*headless=*/true), fb_sm(40, 20, /*headless=*/true);
    rast_shadow(fb_null, above, above, above, red, blue, nullptr);
    rast_shadow(fb_sm, above, above, above, red, blue, &sm);

    ASSERT_TRUE(was_drawn(fb_null, 20, 10));
    ASSERT_TRUE(was_drawn(fb_sm, 20, 10));
    assert_pixel_near(fb_sm, 20, 10, fb_null.get_pixel(20, 10), 2);
}

// S3: pa/pb/pc at z=−5 (below occluder, fully shadowed).
// Expected: lerp(red, blue, 1) = blue at pixel (20,10).
// Catches: shadow lerp inverted, col/shad swapped, or sf never reaches 1.
TEST(rasterize, shadow_occluded_position_uses_shad_color)
{
    Light light = make_light_z_shadow();
    ShadowMap sm = build_shadow_map(make_occluder_z0(), light);
    vec3 below{0.0f, 0.0f, -5.0f};
    vec3 red{1.0f, 0.0f, 0.0f}, blue{0.0f, 0.0f, 1.0f};

    Framebuffer fb(40, 20, /*headless=*/true);
    rast_shadow(fb, below, below, below, red, blue, &sm);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.b < 250)
        ASSERT_FAIL("B too low (" + std::to_string(static_cast<int>(c.b)) + "): occluded position should use shadowed color (blue)");
    if (c.r > 5)
        ASSERT_FAIL("R too high (" + std::to_string(static_cast<int>(c.r)) + "): lit color (red) must not appear in shadow");
}

// ── Group B: rasterize_phong() ────────────────────────────────────────────────

// S4: pa/pb/pc at z=+10 (lit). One key light (dir=(0,0,1), color=(1,0,0)).
// shadow_map=nullptr and a valid map must produce matching pixels.
// Catches: Phong shadow query crashes, or sf<=0 branch alters the result.
TEST(rasterize_phong, shadow_lit_position_matches_nullptr)
{
    Light key{};
    key.direction = {0.0f, 0.0f, 1.0f};
    key.color = {1.0f, 0.0f, 0.0f};
    Material mat{};
    mat.diffuse = {1.0f, 1.0f, 1.0f};
    mat.ambient = {0.0f, 0.0f, 0.0f};
    mat.specular = {0.0f, 0.0f, 0.0f};
    ShadowMap sm = build_shadow_map(make_occluder_z0(), make_light_z_shadow());
    vec3 above{0.0f, 0.0f, 10.0f};

    Framebuffer fb_null(40, 20, /*headless=*/true), fb_sm(40, 20, /*headless=*/true);
    rast_phong_shadow(fb_null, above, above, above, &key, 1, mat, nullptr);
    rast_phong_shadow(fb_sm, above, above, above, &key, 1, mat, &sm);

    ASSERT_TRUE(was_drawn(fb_null, 20, 10));
    ASSERT_TRUE(was_drawn(fb_sm, 20, 10));
    assert_pixel_near(fb_sm, 20, 10, fb_null.get_pixel(20, 10), 2);
}

// S5: pa/pb/pc at z=−5 (fully occluded). Two lights:
//   lights[0]=key (dir=(0,0,1), color=(1,0,0))  — contributes red diffuse,
//   lights[1]=fill (dir=(1,0,0), color=(0,1,0)) — dir⊥normal → zero diffuse.
// Without shadow: key + fill → red dominant.
// With shadow (sf=1): lights+1, n_lights-1 → fill only; fill dir⊥normal → 0 diffuse.
// Expected: R≤30, G≤30 (no ambient, fill contributes nothing to normal (0,0,1)).
// Catches: key-light-exclusion branch dropped → key light leaks into shadow.
TEST(rasterize_phong, shadow_occluded_position_excludes_key_light)
{
    Light lights[2];
    lights[0].direction = {0.0f, 0.0f, 1.0f};
    lights[0].color = {1.0f, 0.0f, 0.0f};
    lights[1].direction = {1.0f, 0.0f, 0.0f};
    lights[1].color = {0.0f, 1.0f, 0.0f};
    Material mat{};
    mat.diffuse = {1.0f, 1.0f, 1.0f};
    mat.ambient = {0.0f, 0.0f, 0.0f};
    mat.specular = {0.0f, 0.0f, 0.0f};
    ShadowMap sm = build_shadow_map(make_occluder_z0(), make_light_z_shadow());
    vec3 below{0.0f, 0.0f, -5.0f};

    Framebuffer fb_nosm(40, 20, /*headless=*/true), fb_sm(40, 20, /*headless=*/true);
    rast_phong_shadow(fb_nosm, below, below, below, lights, 2, mat, nullptr);
    rast_phong_shadow(fb_sm, below, below, below, lights, 2, mat, &sm);

    ASSERT_TRUE(was_drawn(fb_nosm, 20, 10));
    ASSERT_TRUE(was_drawn(fb_sm, 20, 10));
    if (fb_nosm.get_pixel(20, 10).r < 200)
        ASSERT_FAIL("without shadow: R too low, key light should give strong red diffuse");
    Color c = fb_sm.get_pixel(20, 10);
    if (c.r > 30)
        ASSERT_FAIL("with shadow: R too high (" + std::to_string(static_cast<int>(c.r)) + "): key light must be excluded when fully occluded");
    if (c.g > 30)
        ASSERT_FAIL("with shadow: G too high (" + std::to_string(static_cast<int>(c.g)) + "): fill light dir is tangential to normal, should give no diffuse");
}
