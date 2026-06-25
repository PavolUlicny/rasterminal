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
    v.pos = { -10.0f, -10.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 10.0f, -10.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 0.0f, 10.0f, 0.0f };
    m.vertices.push_back(v);
    m.triangles.push_back({ { 0, 1, 2 } });
    m.materials.push_back({});
    return m;
}

// Light from +Z used for all shadow tests.
static Light make_light_z_shadow()
{
    Light l{};
    l.direction = { 0.0f, 0.0f, 1.0f };
    l.color = { 1.0f, 1.0f, 1.0f };
    return l;
}

// rasterize_flat() wrapper: canonical screen triangle, caller-supplied world positions,
// lit and shadowed colors, and shadow map.
// Screen triangle: sa=(4,2), sb=(36,2), sc=(20,18) on 40×20 fb.
static void rast_shadow(Framebuffer &fb, vec3 pa, vec3 pb, vec3 pc, vec3 col, vec3 shad, const ShadowMap *sm)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec2 uv{ 0.5f, 0.5f };
    rasterize_flat(
        fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, col, col, col, shad, pa, pb, pc, uv, uv, uv, nullptr, 0.0f, sm, 0, 19
    );
}

// rasterize_phong() wrapper: canonical triangle, caller-supplied world positions,
// lights, material, and shadow map. ambient=(0,0,0) so ambient terms don't mask
// whether the key light is included or excluded.
static void rast_phong_shadow(
    Framebuffer &fb,
    vec3 pa,
    vec3 pb,
    vec3 pc,
    const Light *lights,
    int n_lights,
    const Material &mat,
    const ShadowMap *sm
)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 normal{ 0.0f, 0.0f, 1.0f }, tan{ 1.0f, 0.0f, 0.0f };
    vec3 white{ 1.0f, 1.0f, 1.0f };
    vec3 eye{ 20.0f, 10.0f, -100.0f };
    vec3 ambient{ 0.0f, 0.0f, 0.0f };
    vec2 uv{ 0.5f, 0.5f };
    rasterize_phong(
        fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, pa, pb, pc, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f, 1.0f,
        1.0f, white, white, white, false, eye, lights, n_lights, ambient, mat, nullptr, nullptr, nullptr, sm, 0, 19
    );
}

// ─── rasterizer shadow integration ───────────────────────────────────────────
//
// Canonical triangle: sa=(4,2), sb=(36,2), sc=(20,18) on 40×20 framebuffer.
// Occluder: flat XY triangle at z=0. Light from +Z.
// "Above" = world z>0 (between light and occluder) → lit.
// "Below" = world z<0 (on far side of occluder from light) → in shadow.

// ── Group A: rasterize_flat() (Flat path) ─────────────────────────────────────────

// S2: pa/pb/pc at z=+10 (above occluder, lit). shadow_map=nullptr and a valid
// map must both produce the same result — lerp(col,shad,sf=0)=col in both cases.
// Catches: nullptr short-circuit broken, or a lit query still uses shad color.
TEST(rasterize, shadow_lit_position_matches_nullptr)
{
    Light light = make_light_z_shadow();
    ShadowMap sm = build_shadow_map(make_occluder_z0(), light);
    vec3 above{ 0.0f, 0.0f, 10.0f };
    vec3 red{ 1.0f, 0.0f, 0.0f }, blue{ 0.0f, 0.0f, 1.0f };

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
    vec3 below{ 0.0f, 0.0f, -5.0f };
    vec3 red{ 1.0f, 0.0f, 0.0f }, blue{ 0.0f, 0.0f, 1.0f };

    Framebuffer fb(40, 20, /*headless=*/true);
    rast_shadow(fb, below, below, below, red, blue, &sm);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.b < 250)
    {
        ASSERT_FAIL(
            "B too low (" + std::to_string(static_cast<int>(c.b)) +
            "): occluded position should use shadowed color (blue)"
        );
    }
    if (c.r > 5)
    {
        ASSERT_FAIL(
            "R too high (" + std::to_string(static_cast<int>(c.r)) + "): lit color (red) must not appear in shadow"
        );
    }
}

// ── Group B: rasterize_phong() ────────────────────────────────────────────────

// S4: pa/pb/pc at z=+10 (lit). One key light (dir=(0,0,1), color=(1,0,0)).
// shadow_map=nullptr and a valid map must produce matching pixels.
// Catches: Phong shadow query crashes, or sf<=0 branch alters the result.
TEST(rasterize_phong, shadow_lit_position_matches_nullptr)
{
    Light key{};
    key.direction = { 0.0f, 0.0f, 1.0f };
    key.color = { 1.0f, 0.0f, 0.0f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 0.0f, 0.0f, 0.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    ShadowMap sm = build_shadow_map(make_occluder_z0(), make_light_z_shadow());
    vec3 above{ 0.0f, 0.0f, 10.0f };

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
    lights[0].direction = { 0.0f, 0.0f, 1.0f };
    lights[0].color = { 1.0f, 0.0f, 0.0f };
    lights[1].direction = { 1.0f, 0.0f, 0.0f };
    lights[1].color = { 0.0f, 1.0f, 0.0f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 0.0f, 0.0f, 0.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    ShadowMap sm = build_shadow_map(make_occluder_z0(), make_light_z_shadow());
    vec3 below{ 0.0f, 0.0f, -5.0f };

    Framebuffer fb_nosm(40, 20, /*headless=*/true), fb_sm(40, 20, /*headless=*/true);
    rast_phong_shadow(fb_nosm, below, below, below, lights, 2, mat, nullptr);
    rast_phong_shadow(fb_sm, below, below, below, lights, 2, mat, &sm);

    ASSERT_TRUE(was_drawn(fb_nosm, 20, 10));
    ASSERT_TRUE(was_drawn(fb_sm, 20, 10));
    if (fb_nosm.get_pixel(20, 10).r < 200)
    {
        ASSERT_FAIL("without shadow: R too low, key light should give strong red diffuse");
    }
    Color c = fb_sm.get_pixel(20, 10);
    if (c.r > 30)
    {
        ASSERT_FAIL(
            "with shadow: R too high (" + std::to_string(static_cast<int>(c.r)) +
            "): key light must be excluded when fully occluded"
        );
    }
    if (c.g > 30)
    {
        ASSERT_FAIL(
            "with shadow: G too high (" + std::to_string(static_cast<int>(c.g)) +
            "): fill light dir is tangential to normal, should give no diffuse"
        );
    }
}

// ── Group C: rasterize_phong() manual shadow map ──────────────────────────────
//
// Build ShadowMap with identity light_vp and hand-crafted depth values so that
// in_shadow() returns a predictable factor.
//
// With identity light_vp and world_pos=(0,0,0.5):
//   light_clip = (0,0,0.5,1)  →  ndc = (0,0,0.5)
//   u=0.5, v=0.5  →  cx=cy=ShadowMap::SIZE/2=1024
//   ref = ndc.z - fp_eps = 0.499
//
// Each of the 9 PCF kernel entries set to 0.0 contributes one hit (0.0 < 0.499).
// Entries left at the default 1.0 contribute no hit (1.0 > 0.499).
// sf = hits / 9.
//
// Canonical screen triangle: sa=(4,2), sb=(36,2), sc=(20,18) on 40×20.
// All world vertices at (0,0,0.5)  →  every pixel interpolates to the same wpos.

namespace
{
    // Build a ShadowMap whose PCF for wpos=(0,0,0.5) returns exactly hits/9.
    // Caller owns the returned object.
    ShadowMap make_manual_sm(int hits)
    {
        constexpr int S = ShadowMap::SIZE;
        constexpr int cx = S / 2; // 1024
        constexpr int cy = S / 2; // 1024
        ShadowMap sm;
        sm.clear();
        sm.light_vp = mat4::identity();
        int count = 0;
        for (int dy = -1; dy <= 1 && count < hits; dy++)
        {
            for (int dx = -1; dx <= 1 && count < hits; dx++, ++count)
            {
                sm.depth[(static_cast<size_t>(cy + dy) * S) + static_cast<size_t>(cx + dx)] = 0.0f;
            }
        }
        return sm;
    }

    void
    draw_phong_manual_sm(Framebuffer &fb, const Light *lights, int n_lights, const Material &mat, const ShadowMap *sm)
    {
        vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
        vec3 wpos{ 0.0f, 0.0f, 0.5f }; // maps to cx=cy=1024 with identity light_vp
        vec3 normal{ 0.0f, 0.0f, 1.0f };
        vec3 tan{ 1.0f, 0.0f, 0.0f };
        vec3 white{ 1.0f, 1.0f, 1.0f };
        vec3 eye{ 20.0f, 10.0f, -100.0f };
        vec3 ambient{ 0.0f, 0.0f, 0.0f };
        vec2 uv{ 0.5f, 0.5f };
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, wpos, wpos, wpos, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, lights, n_lights, ambient, mat, nullptr, nullptr, nullptr, sm,
            0, 19
        );
    }
} // namespace

// S6: Phong partial shadow (0 < sf < 1) exercises the else-branch lerp.
// 5/9 kernel entries shadowed → sf=5/9 ≈ 0.556, strictly between 0 and 1.
// Expected: R is strictly between fully-lit and fully-shadowed values.
TEST(rasterize_phong, partial_shadow_lerps_between_lit_and_shadowed)
{
    Light key{};
    key.direction = { 0.0f, 0.0f, 1.0f }; // aligned with normal → maximum diffuse
    key.color = { 1.0f, 0.0f, 0.0f };     // red key — only R channel varies
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 0.0f, 0.0f, 0.0f }; // no ambient so shadow = black
    mat.specular = { 0.0f, 0.0f, 0.0f };

    ShadowMap sm0 = make_manual_sm(0); // sf=0  → sf<=0  branch (fully lit)
    ShadowMap sm9 = make_manual_sm(9); // sf=1  → sf>=1  branch (fully shadowed)
    ShadowMap sm5 = make_manual_sm(5); // sf=5/9 → else  branch (partial)

    Framebuffer fb_lit(40, 20, /*headless=*/true);
    Framebuffer fb_shd(40, 20, /*headless=*/true);
    Framebuffer fb_par(40, 20, /*headless=*/true);
    draw_phong_manual_sm(fb_lit, &key, 1, mat, &sm0);
    draw_phong_manual_sm(fb_shd, &key, 1, mat, &sm9);
    draw_phong_manual_sm(fb_par, &key, 1, mat, &sm5);

    ASSERT_TRUE(was_drawn(fb_lit, 20, 10));
    ASSERT_TRUE(was_drawn(fb_par, 20, 10));

    const int r_lit = static_cast<int>(fb_lit.get_pixel(20, 10).r);
    const int r_shd = static_cast<int>(fb_shd.get_pixel(20, 10).r);
    const int r_par = static_cast<int>(fb_par.get_pixel(20, 10).r);

    if (r_lit <= r_shd)
    {
        ASSERT_FAIL(
            "lit must be brighter than shadowed: lit=" + std::to_string(r_lit) + " shd=" + std::to_string(r_shd)
        );
    }
    if (r_par <= r_shd)
    {
        ASSERT_FAIL(
            "partial must be brighter than fully shadowed: par=" + std::to_string(r_par) +
            " shd=" + std::to_string(r_shd)
        );
    }
    if (r_par >= r_lit)
    {
        ASSERT_FAIL(
            "partial must be darker than fully lit: par=" + std::to_string(r_par) + " lit=" + std::to_string(r_lit)
        );
    }
}

// S7: n_lights=0 with shadow_map != nullptr. The shadow factor may be non-zero but
// sl=lights (=nullptr) and n_shadow=0 in every branch → result is ambient-only in
// all cases. Matches the no-shadow reference (shadow_map=nullptr).
// Verifies that the lights+1 pointer-arithmetic is not reached when n_lights=0.
TEST(rasterize_phong, n_lights_zero_with_shadow_map_matches_no_shadow)
{
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };

    ShadowMap sm9 = make_manual_sm(9); // sf=1 for wpos=(0,0,0.5)

    Framebuffer fb_nosm(40, 20, /*headless=*/true);
    Framebuffer fb_sm(40, 20, /*headless=*/true);
    draw_phong_manual_sm(fb_nosm, nullptr, 0, mat, nullptr);
    draw_phong_manual_sm(fb_sm, nullptr, 0, mat, &sm9);

    ASSERT_TRUE(was_drawn(fb_nosm, 20, 10));
    ASSERT_TRUE(was_drawn(fb_sm, 20, 10));
    // Both paths produce ambient-only; colors must match.
    assert_pixel_near(fb_sm, 20, 10, fb_nosm.get_pixel(20, 10), 2);
}

// S8: n_lights=1, sf=1 (fully shadowed). sl=lights+1 (one past end of 1-element array),
// n_shadow=0. compute_lighting is called with 0 lights → ambient-only. With ambient=0,
// result is near-black. Verifies the sl pointer is never dereferenced when n_shadow=0.
TEST(rasterize_phong, n_lights_one_fully_shadowed_gives_ambient_only)
{
    Light key{};
    key.direction = { 0.0f, 0.0f, 1.0f };
    key.color = { 1.0f, 0.0f, 0.0f };
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 0.0f, 0.0f, 0.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };

    ShadowMap sm9 = make_manual_sm(9); // sf=1

    Framebuffer fb_lit(40, 20, /*headless=*/true);
    Framebuffer fb_shd(40, 20, /*headless=*/true);
    draw_phong_manual_sm(fb_lit, &key, 1, mat, nullptr); // sf=0, key light active
    draw_phong_manual_sm(fb_shd, &key, 1, mat, &sm9);    // sf=1, key excluded

    ASSERT_TRUE(was_drawn(fb_lit, 20, 10));
    ASSERT_TRUE(was_drawn(fb_shd, 20, 10));

    const int r_lit = static_cast<int>(fb_lit.get_pixel(20, 10).r);
    Color c_shd = fb_shd.get_pixel(20, 10);
    if (r_lit < 200)
    {
        ASSERT_FAIL("unoccluded with key light must be bright: R=" + std::to_string(r_lit));
    }
    if (c_shd.r > 5 || c_shd.g > 5 || c_shd.b > 5)
    {
        ASSERT_FAIL(
            "n_lights=1 fully shadowed: sl=key+1 n_shadow=0 → ambient-only → near-black, got (" +
            std::to_string(static_cast<int>(c_shd.r)) + "," + std::to_string(static_cast<int>(c_shd.g)) + "," +
            std::to_string(static_cast<int>(c_shd.b)) + ")"
        );
    }
}

// S10: Phong partial shadow (0 < sf < 1) with n_lights=0 exercises the else-branch.
// When n_lights=0: sl=lights=nullptr, n_shadow=0.  Both lit and shd calls reduce to
// ambient-only, so lerp(lit,shd,sf)=lit.  Result must equal the no-shadow reference.
TEST(rasterize_phong, partial_shadow_n_lights_zero_produces_ambient_only)
{
    // sm5: 5/9 PCF hits → sf≈5/9, strictly 0<sf<1 → else branch fires.
    ShadowMap sm5 = make_manual_sm(5);
    Material mat{};
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };

    // Inline draw (draw_phong_manual_sm hard-codes ambient=0; we need non-zero ambient
    // so the pixel has a colour to compare against).
    auto draw_inline = [&](Framebuffer &fb, const ShadowMap *sm)
    {
        vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
        vec3 wpos{ 0.0f, 0.0f, 0.5f };
        vec3 normal{ 0.0f, 0.0f, 1.0f };
        vec3 tan{ 1.0f, 0.0f, 0.0f };
        vec3 white{ 1.0f, 1.0f, 1.0f };
        vec3 eye{ 20.0f, 10.0f, -100.0f };
        vec3 ambient{ 0.5f, 0.5f, 0.5f };
        vec2 uv{ 0.5f, 0.5f };
        rasterize_phong(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, wpos, wpos, wpos, normal, normal, normal, tan, tan, tan, uv, uv, uv, 1.0f,
            1.0f, 1.0f, white, white, white, false, eye, nullptr, 0, ambient, mat, nullptr, nullptr, nullptr, sm, 0, 19
        );
    };

    Framebuffer fb_nosm(40, 20, /*headless=*/true), fb_sm5(40, 20, /*headless=*/true);
    draw_inline(fb_nosm, nullptr); // sf=0 → sf<=0 branch → ambient only
    draw_inline(fb_sm5, &sm5);     // sf≈5/9 → else branch, n_lights=0 → also ambient only

    ASSERT_TRUE(was_drawn(fb_nosm, 20, 10));
    ASSERT_TRUE(was_drawn(fb_sm5, 20, 10));
    // n_lights=0 means lit==shd; lerp is a no-op → same colour as no shadow map.
    assert_pixel_near(fb_sm5, 20, 10, fb_nosm.get_pixel(20, 10), 2);
}

// S9: rasterize_flat() (Flat path) with 0 < sf < 1 exercises the sf>0 lerp branch.
// col=red, shad=blue; sf=5/9 ≈ 0.556. Result must be strictly between fully-lit (red)
// and fully-shadowed (blue).
TEST(rasterize, flat_partial_shadow_lerps_between_lit_and_shadowed)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 wpos{ 0.0f, 0.0f, 0.5f }; // maps to cx=cy=1024 with identity light_vp
    vec3 red{ 1.0f, 0.0f, 0.0f }, blue{ 0.0f, 0.0f, 1.0f };
    vec2 uv{ 0.5f, 0.5f };

    auto draw = [&](Framebuffer &fb, const ShadowMap *sm)
    {
        rasterize_flat(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, red, red, red, blue, wpos, wpos, wpos, uv, uv, uv, nullptr, 0.0f, sm, 0,
            19
        );
    };

    ShadowMap sm0 = make_manual_sm(0); // sf=0   → if(sf>0) false → col (red)
    ShadowMap sm9 = make_manual_sm(9); // sf=1   → lerp(red,blue,1) = blue
    ShadowMap sm5 = make_manual_sm(5); // sf=5/9 → lerp(red,blue,5/9) intermediate

    Framebuffer fb_lit(40, 20, /*headless=*/true);
    Framebuffer fb_shd(40, 20, /*headless=*/true);
    Framebuffer fb_par(40, 20, /*headless=*/true);
    draw(fb_lit, &sm0);
    draw(fb_shd, &sm9);
    draw(fb_par, &sm5);

    ASSERT_TRUE(was_drawn(fb_lit, 20, 10));
    ASSERT_TRUE(was_drawn(fb_par, 20, 10));

    const int r_lit = static_cast<int>(fb_lit.get_pixel(20, 10).r);
    const int r_shd = static_cast<int>(fb_shd.get_pixel(20, 10).r);
    const int r_par = static_cast<int>(fb_par.get_pixel(20, 10).r);

    if (r_lit <= r_shd)
    {
        ASSERT_FAIL("lit must be redder than shadowed: lit=" + std::to_string(r_lit) + " shd=" + std::to_string(r_shd));
    }
    if (r_par <= r_shd)
    {
        ASSERT_FAIL(
            "partial must be redder than fully shadowed: par=" + std::to_string(r_par) + " shd=" + std::to_string(r_shd)
        );
    }
    if (r_par >= r_lit)
    {
        ASSERT_FAIL(
            "partial must be less red than fully lit: par=" + std::to_string(r_par) + " lit=" + std::to_string(r_lit)
        );
    }
}

// S10: distinct per-vertex base colours combined with an active shadow lerp.
// The refactor collapsed three per-vertex shad colours into one uniform shad applied
// after interpolation (col = lerp(interp_col, shad, sf)). Production never mixes the two
// regimes — Flat passes identical vertex colours, and the only distinct-colour caller
// (unlit) passes a null shadow map — but the function now permits it, so guard the order:
//   sf=0 (no shadow): distinct red/green/blue still interpolate to a gradient, so the
//                     centroid pixel must carry all three channels (not a flat colour).
//   sf=1 (full shadow): the interpolated colour is fully replaced by the single shad, so
//                     the centroid must equal shad regardless of the distinct vertex colours.
// A regression that lerped per-vertex toward shad before interpolating would still pass
// every other test (all use identical col_a/b/c); only this one distinguishes the order.
TEST(rasterize, flat_distinct_vertex_colours_with_shadow_lerp_applies_shad_after_interp)
{
    vec3 sa{ 4.0f, 2.0f, 0.5f }, sb{ 36.0f, 2.0f, 0.5f }, sc{ 20.0f, 18.0f, 0.5f };
    vec3 wpos{ 0.0f, 0.0f, 0.5f }; // maps to cx=cy=1024 with identity light_vp
    vec3 red{ 1.0f, 0.0f, 0.0f }, green{ 0.0f, 1.0f, 0.0f }, blue{ 0.0f, 0.0f, 1.0f };
    vec3 shad{ 1.0f, 1.0f, 1.0f }; // white: distinct from every vertex colour and from their blend
    vec2 uv{ 0.5f, 0.5f };

    auto draw = [&](Framebuffer &fb, const ShadowMap *sm)
    {
        rasterize_flat(
            fb, sa, sb, sc, 1.0f, 1.0f, 1.0f, red, green, blue, shad, wpos, wpos, wpos, uv, uv, uv, nullptr, 0.0f, sm,
            0, 19
        );
    };

    ShadowMap sm0 = make_manual_sm(0); // sf=0 → if(sf>0) false → interpolated gradient
    ShadowMap sm9 = make_manual_sm(9); // sf=1 → lerp(interp, shad, 1) = shad (white)

    Framebuffer fb_lit(40, 20, /*headless=*/true), fb_shd(40, 20, /*headless=*/true);
    draw(fb_lit, &sm0);
    draw(fb_shd, &sm9);

    ASSERT_TRUE(was_drawn(fb_lit, 20, 10));
    ASSERT_TRUE(was_drawn(fb_shd, 20, 10));

    // Unshadowed: the centroid blends all three vertices, so every channel is partially lit
    // (none saturated, none zero). A pre-interpolation lerp toward white would also push the
    // off-vertex channels up, so the discriminating case is the fully-shadowed run below.
    Color lit = fb_lit.get_pixel(20, 10);
    if (lit.r < 10 || lit.g < 10 || lit.b < 10)
    {
        ASSERT_FAIL("unshadowed centroid must blend all three distinct vertex colours");
    }
    if (lit.r > 200 || lit.g > 200 || lit.b > 200)
    {
        ASSERT_FAIL("unshadowed centroid must be a partial blend, no channel fully saturated");
    }

    // Fully shadowed: shad replaces the interpolated colour entirely → white.
    assert_pixel_near(fb_shd, 20, 10, Color{ 255, 255, 255 }, 2);
}
