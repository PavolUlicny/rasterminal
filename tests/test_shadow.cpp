#include "test.h"
#include "../src/shadow.h"
#include "../src/light.h"
#include "../src/mesh.h"
#include "../src/linalg.h"
#include "../src/texture.h"

// Builds a mesh with one large flat triangle in the XY plane (z=0).
// Vertices span ±10 units so the shadow frustum comfortably covers (0,0,±5).
static Mesh make_flat_triangle()
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

// Light coming from the +Z direction (shines down the −Z axis onto the triangle).
static Light make_light_z()
{
    Light l{};
    l.direction = {0.0f, 0.0f, 1.0f};
    l.color = {1.0f, 1.0f, 1.0f};
    return l;
}

// ─── ShadowMap::clear() ───────────────────────────────────────────────────────

TEST(shadow, clear_initializes_depth_to_one)
{
    ShadowMap shadow_map;
    shadow_map.clear();
    for (float d : shadow_map.depth)
        ASSERT_NEAR(d, 1.0f, 1e-6f);
}

// ─── build_shadow_map + in_shadow ────────────────────────────────────────────

TEST(shadow, empty_mesh_nothing_in_shadow)
{
    Mesh m;
    ShadowMap shadow_map = build_shadow_map(m, make_light_z());
    // No triangles rasterized → depth stays at 1.0 everywhere → nothing in shadow.
    ASSERT_NEAR(shadow_map.in_shadow({0.0f, 0.0f, 0.0f}), 0.0f, 1e-6f);
    ASSERT_NEAR(shadow_map.in_shadow({0.0f, 0.0f, -5.0f}), 0.0f, 1e-6f);
}

TEST(shadow, lit_point_not_in_shadow)
{
    // Triangle at z=0, light from +Z. A point at z=+5 sits between the light
    // and the triangle — it is closer to the light and cannot be occluded.
    ShadowMap shadow_map = build_shadow_map(make_flat_triangle(), make_light_z());
    ASSERT_NEAR(shadow_map.in_shadow({0.0f, 0.0f, 5.0f}), 0.0f, 1e-6f);
}

TEST(shadow, occluded_point_is_in_shadow)
{
    // A point at z=−5 is on the far side of the triangle from the light, so
    // the triangle lies directly between it and the light source → in shadow.
    ShadowMap shadow_map = build_shadow_map(make_flat_triangle(), make_light_z());
    ASSERT_NEAR(shadow_map.in_shadow({0.0f, 0.0f, -5.0f}), 1.0f, 1e-6f);
}

TEST(shadow, point_outside_frustum_is_lit)
{
    // in_shadow() returns false for points whose NDC coordinates land outside
    // [−1,1] in any axis — they are outside the light's shadow volume.
    ShadowMap shadow_map = build_shadow_map(make_flat_triangle(), make_light_z());
    ASSERT_NEAR(shadow_map.in_shadow({1000.0f, 0.0f, 0.0f}), 0.0f, 1e-6f);
}

TEST(shadow, coincident_vertices_radius_clamped)
{
    // All vertices at the same point → radius = 0, clamped to 1.0 in shadow.cpp.
    // Shadow map must still build without crash and return a finite value.
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.pos = {1.0f, 1.0f, 1.0f};
    m.vertices.push_back(v);
    m.vertices.push_back(v);
    m.vertices.push_back(v);
    m.triangles.push_back({{0, 1, 2}});
    m.materials.push_back({});
    ShadowMap shadow_map = build_shadow_map(m, make_light_z());
    float sf = shadow_map.in_shadow({1.0f, 1.0f, 0.0f});
    ASSERT_TRUE(sf >= 0.0f && sf <= 1.0f);
}

TEST(shadow, light_pointing_up_uses_x_axis_fallback)
{
    // |dir.y| = 1.0 >= 0.9 → world_up falls back to {1,0,0}.
    // look_at must remain valid (forward not parallel to up in this fallback).
    Light light{};
    light.direction = {0.0f, 1.0f, 0.0f};
    light.color = {1.0f, 1.0f, 1.0f};
    ShadowMap shadow_map = build_shadow_map(make_flat_triangle(), light);
    float sf = shadow_map.in_shadow({0.0f, 0.0f, 0.0f});
    ASSERT_TRUE(sf >= 0.0f && sf <= 1.0f);
}

TEST(shadow, surface_point_not_self_shadowed_by_slope_bias)
{
    // A point ON the triangle surface (z=0) must not be in shadow under its own
    // depth write. The slope-scale bias adds a positive offset to stored depth
    // so the surface cannot occlude itself.
    // Catches: slope bias removed or too small → acne on every surface.
    ShadowMap shadow_map = build_shadow_map(make_flat_triangle(), make_light_z());
    ASSERT_NEAR(shadow_map.in_shadow({0.0f, 0.0f, 0.0f}), 0.0f, 1e-6f);
}

// ─── Alpha cutout in shadow map ───────────────────────────────────────────────

// Build a 1×1 white RGBA texture with the given alpha for inline test use.
static Texture make_shadow_tex(uint8_t a)
{
    Texture t;
    t.width = 1;
    t.height = 1;
    t.pixels = {255, 255, 255, a};
    return t;
}

// Large flat triangle at z=0 with UV [0,1] on each vertex and a 1×1 texture.
// material_idx 1 carries the cutout material; the triangle is big enough that
// the shadow frustum always covers the probe point (0,0,−5).
static Mesh make_cutout_triangle(float alpha_cutoff, uint8_t tex_alpha)
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;

    v.pos = {-10.0f, -10.0f, 0.0f};
    v.uv = {0.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {10.0f, -10.0f, 0.0f};
    v.uv = {1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 10.0f, 0.0f};
    v.uv = {0.5f, 1.0f};
    m.vertices.push_back(v);

    m.materials.push_back({});

    Material mat{};
    mat.alpha_cutoff = alpha_cutoff;
    m.textures.push_back(make_shadow_tex(tex_alpha));
    mat.diffuse_tex = 0;
    m.materials.push_back(mat);

    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 1;
    m.triangles.push_back(tri);

    return m;
}

// alpha_cutoff=0 disables the cutout path entirely even if the texture is
// transparent — the full opaque shadow still forms.
TEST(shadow, alpha_cutoff_zero_casts_full_shadow)
{
    Mesh m = make_cutout_triangle(0.0f, 0); // transparent texture, cutoff disabled
    ShadowMap sm = build_shadow_map(m, make_light_z());
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, -5.0f}), 1.0f, 1e-6f);
}

// alpha_cutoff=0.5 with an all-opaque texture must behave exactly like no cutout
// (no spurious discards for non-transparent pixels).
TEST(shadow, opaque_texture_with_cutoff_casts_shadow)
{
    Mesh m = make_cutout_triangle(0.5f, 255); // fully opaque texture
    ShadowMap sm = build_shadow_map(m, make_light_z());
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, -5.0f}), 1.0f, 1e-6f);
}

// alpha_cutoff=0.5 with an all-transparent texture: no depth should be written,
// so the point behind the triangle must NOT be in shadow.
TEST(shadow, transparent_texture_with_cutoff_casts_no_shadow)
{
    Mesh m = make_cutout_triangle(0.5f, 0); // fully transparent (alpha=0)
    ShadowMap sm = build_shadow_map(m, make_light_z());
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, -5.0f}), 0.0f, 1e-6f);
}

// diffuse_tex=-1 with alpha_cutoff>0: no texture means has_cutout=false;
// the triangle must still cast its shadow.
TEST(shadow, no_texture_with_cutoff_casts_shadow)
{
    Mesh m = make_cutout_triangle(0.5f, 0); // alpha=0 texture set up...
    // ...but now clear diffuse_tex to simulate "cutoff set but no texture"
    m.materials[1].diffuse_tex = -1;
    ShadowMap sm = build_shadow_map(m, make_light_z());
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, -5.0f}), 1.0f, 1e-6f);
}

// Mixed-material mesh: one opaque triangle (no cutout) and one fully-transparent
// cutout triangle covering separate parts of the shadow map.
// Verifies the per-triangle material lookup is correct — a bug that computed
// has_cutout once per mesh rather than per triangle would fail here.
TEST(shadow, mixed_material_opaque_casts_shadow_transparent_does_not)
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;

    // Triangle A (x < 0): opaque, material 0 (default, no cutout).
    v.pos = {-10.0f, -5.0f, 0.0f};
    v.uv = {0.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {-0.5f, -5.0f, 0.0f};
    v.uv = {1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {-5.0f, 5.0f, 0.0f};
    v.uv = {0.5f, 1.0f};
    m.vertices.push_back(v);

    // Triangle B (x > 0): cutout, transparent texture.
    v.pos = {0.5f, -5.0f, 0.0f};
    v.uv = {0.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {10.0f, -5.0f, 0.0f};
    v.uv = {1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {5.0f, 5.0f, 0.0f};
    v.uv = {0.5f, 1.0f};
    m.vertices.push_back(v);

    m.materials.push_back({});

    Material cut{};
    cut.alpha_cutoff = 0.5f;
    m.textures.push_back(make_shadow_tex(0));
    cut.diffuse_tex = 0;
    m.materials.push_back(cut);

    Triangle ta{};
    ta.v[0] = 0;
    ta.v[1] = 1;
    ta.v[2] = 2;
    ta.material_idx = 0;
    Triangle tb{};
    tb.v[0] = 3;
    tb.v[1] = 4;
    tb.v[2] = 5;
    tb.material_idx = 1;
    m.triangles.push_back(ta);
    m.triangles.push_back(tb);

    ShadowMap sm = build_shadow_map(m, make_light_z());

    // Point behind the opaque triangle → in shadow.
    ASSERT_NEAR(sm.in_shadow({-5.0f, 0.0f, -5.0f}), 1.0f, 1e-6f);
    // Point behind the transparent cutout triangle → NOT in shadow.
    ASSERT_NEAR(sm.in_shadow({5.0f, 0.0f, -5.0f}), 0.0f, 1e-6f);
}
