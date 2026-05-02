#include "test.h"
#include "../src/shadow.h"
#include "../src/light.h"
#include "../src/mesh.h"
#include "../src/linalg.h"

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
