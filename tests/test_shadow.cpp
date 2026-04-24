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
    m.triangles.push_back({0, 1, 2});
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
    ShadowMap smap;
    smap.clear();
    for (float d : smap.depth)
        ASSERT_NEAR(d, 1.0f, 1e-6f);
}

// ─── build_shadow_map + in_shadow ────────────────────────────────────────────

TEST(shadow, empty_mesh_nothing_in_shadow)
{
    Mesh m;
    ShadowMap smap = build_shadow_map(m, make_light_z());
    // No triangles rasterized → depth stays at 1.0 everywhere → nothing in shadow.
    ASSERT_FALSE(smap.in_shadow({0.0f, 0.0f, 0.0f}));
    ASSERT_FALSE(smap.in_shadow({0.0f, 0.0f, -5.0f}));
}

TEST(shadow, lit_point_not_in_shadow)
{
    // Triangle at z=0, light from +Z. A point at z=+5 sits between the light
    // and the triangle — it is closer to the light and cannot be occluded.
    ShadowMap smap = build_shadow_map(make_flat_triangle(), make_light_z());
    ASSERT_FALSE(smap.in_shadow({0.0f, 0.0f, 5.0f}));
}

TEST(shadow, occluded_point_is_in_shadow)
{
    // A point at z=−5 is on the far side of the triangle from the light, so
    // the triangle lies directly between it and the light source → in shadow.
    ShadowMap smap = build_shadow_map(make_flat_triangle(), make_light_z());
    ASSERT_TRUE(smap.in_shadow({0.0f, 0.0f, -5.0f}));
}

TEST(shadow, point_outside_frustum_is_lit)
{
    // in_shadow() returns false for points whose NDC coordinates land outside
    // [−1,1] in any axis — they are outside the light's shadow volume.
    ShadowMap smap = build_shadow_map(make_flat_triangle(), make_light_z());
    ASSERT_FALSE(smap.in_shadow({1000.0f, 0.0f, 0.0f}));
}
