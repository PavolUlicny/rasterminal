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

// ─── PCF kernel ───────────────────────────────────────────────────────────────
// Tests use a manually constructed ShadowMap with light_vp = mat4::identity().
// With identity VP, NDC == world_pos. Probe (0,0,0.5) maps to:
//   u = 0.5 → cx = int(0.5 * 2048) = 1024 (interior, fast path)
//   v = 0.5 → cy = 1024
//   ref = 0.5 − 0.001 = 0.499
// Stored 0.0 → ref > 0.0 → hit (occluded). Stored 1.0 → ref > 1.0 → miss (lit).

TEST(shadow, pcf_partial_occlusion_returns_fraction)
{
    ShadowMap sm;
    sm.clear();
    sm.light_vp = mat4::identity();
    // Set 4 of the 9 kernel texels to 0.0 (occluded); 5 remain at 1.0 (lit).
    int cnt = 0;
    for (int dy = -1; dy <= 1 && cnt < 4; dy++)
        for (int dx = -1; dx <= 1 && cnt < 4; dx++)
        {
            sm.depth[static_cast<size_t>((1024 + dy) * ShadowMap::SIZE + (1024 + dx))] = 0.0f;
            cnt++;
        }
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, 0.5f}), 4.0f / 9.0f, 1e-6f);
}

TEST(shadow, pcf_ref_equal_to_stored_depth_is_lit)
{
    // ref = ndc.z − fp_eps = 0.499. Stored exactly ref → ref > ref is false → lit.
    // Proves the comparison is strict > (not >=).
    ShadowMap sm;
    sm.clear();
    sm.light_vp = mat4::identity();
    constexpr float ref = 0.5f - 0.001f;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            sm.depth[static_cast<size_t>((1024 + dy) * ShadowMap::SIZE + (1024 + dx))] = ref;
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, 0.5f}), 0.0f, 1e-6f);
}

TEST(shadow, pcf_border_clamps_kernel_samples)
{
    // Probe (−1, 0, 0.5): cx=0 → else-branch (border clamping).
    // dx∈{−1,0,1} clamps to px∈{0,0,1}; column x=0 set to 0.0, x=1 stays 1.0.
    // Each of 3 rows contributes 2 hits (both x=0 samples share the same occluded texel)
    // → 6 hits total → 6/9.
    ShadowMap sm;
    sm.clear();
    sm.light_vp = mat4::identity();
    for (int py = 1023; py <= 1025; py++)
        sm.depth[static_cast<size_t>(py * ShadowMap::SIZE + 0)] = 0.0f;
    ASSERT_NEAR(sm.in_shadow({-1.0f, 0.0f, 0.5f}), 6.0f / 9.0f, 1e-6f);
}

TEST(shadow, pcf_negative_w_returns_lit)
{
    // m[3][3] = −1 → light_clip.w = −1 ≤ 0 → early-out returns 0 (lit).
    ShadowMap sm;
    sm.clear();
    sm.light_vp = mat4::identity();
    sm.light_vp.m[3][3] = -1.0f;
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, 0.0f}), 0.0f, 1e-6f);
}

// ─── Alpha cutout boundary ────────────────────────────────────────────────────
// build_shadow_map discards pixels with `alpha < cutoff` (strict <), so a pixel
// with alpha == cutoff must write depth and cast a shadow.

TEST(shadow, alpha_exactly_at_cutoff_casts_shadow)
{
    constexpr float cutoff = 128.0f * (1.0f / 255.0f);
    Mesh m = make_cutout_triangle(cutoff, 128);
    ShadowMap sm = build_shadow_map(m, make_light_z());
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, -5.0f}), 1.0f, 1e-6f);
}

// ─── in_shadow NDC z bounds ───────────────────────────────────────────────────

TEST(shadow, in_shadow_ndc_z_above_one_returns_lit)
{
    // With identity VP, world_pos.z becomes ndc.z directly.
    // ndc.z = 1.5 > 1.0 → NDC z bounds check → early-out returns 0 (lit).
    // Without the z check, ref = 1.499 > depths = 0.0 for all 9 kernel texels → sf = 1.
    ShadowMap sm;
    sm.clear();
    sm.light_vp = mat4::identity();
    std::fill(sm.depth.begin(), sm.depth.end(), 0.0f);
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, 1.5f}), 0.0f, 1e-6f);
}

// ─── Depth min-test ───────────────────────────────────────────────────────────

TEST(shadow, depth_min_test_closer_triangle_wins)
{
    // Two overlapping triangles: closer one (z=+5, to the +Z light) listed first,
    // farther one (z=−5) listed second. The depth min-test must prevent the farther
    // triangle from overwriting the closer depth entry. The query at (0,0,0) lies
    // between them — it is in shadow of the closer triangle but would be falsely lit
    // if the farther triangle's larger depth overwrote the closer one.
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.pos = {-10.0f, -10.0f, 5.0f};
    m.vertices.push_back(v);
    v.pos = {10.0f, -10.0f, 5.0f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 10.0f, 5.0f};
    m.vertices.push_back(v);
    v.pos = {-10.0f, -10.0f, -5.0f};
    m.vertices.push_back(v);
    v.pos = {10.0f, -10.0f, -5.0f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 10.0f, -5.0f};
    m.vertices.push_back(v);
    m.materials.push_back({});
    m.triangles.push_back({{0, 1, 2}}); // closer first
    m.triangles.push_back({{3, 4, 5}}); // farther second
    ShadowMap sm = build_shadow_map(m, make_light_z());
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, 0.0f}), 1.0f, 1e-6f);
}

// ─── Slope-bias clamp path ────────────────────────────────────────────────────

TEST(shadow, back_facing_triangle_not_self_shadowed)
{
    // Reversed-winding flat triangle: face_n = (0,0,−1), n_dot_l = −1 ≤ 0.01
    // → slope_bias = 0.5 (clamped path). The stored depth is ndc_z(surface) + 0.5,
    // which is far larger than ref = ndc_z(surface) − fp_eps, so a point on the
    // surface is not falsely in shadow.
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    // Reversed winding vs make_flat_triangle() → normal points −Z (away from +Z light).
    v.pos = {-10.0f, -10.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 10.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {10.0f, -10.0f, 0.0f};
    m.vertices.push_back(v);
    m.triangles.push_back({{0, 1, 2}});
    m.materials.push_back({});
    ShadowMap sm = build_shadow_map(m, make_light_z());
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, 0.0f}), 0.0f, 1e-6f);
}

// ─── PCF right-edge and corner border clamping ────────────────────────────────

TEST(shadow, pcf_right_border_clamps_kernel_samples)
{
    // ndc.x = 1.0 → u = 1.0 → cx = int(1.0 × 2048) = 2048, clamped to SIZE−1 = 2047.
    // cx < SIZE−1 is false → else-branch fires.
    // dx = {−1,0,+1} clamp to px = {2046, 2047, 2047}: dx=0 and dx=+1 share the same
    // column. Setting only column SIZE−1 to 0.0 → 2 hits per row × 3 rows = 6 → sf=6/9.
    ShadowMap sm;
    sm.clear();
    sm.light_vp = mat4::identity();
    constexpr int S = ShadowMap::SIZE;
    for (int py = 1023; py <= 1025; py++)
        sm.depth[static_cast<size_t>(py) * S + (S - 1)] = 0.0f;
    ASSERT_NEAR(sm.in_shadow({1.0f, 0.0f, 0.5f}), 6.0f / 9.0f, 1e-6f);
}

TEST(shadow, pcf_bottom_left_corner_clamps_kernel_samples)
{
    // ndc = (−1,−1,0.5) → cx = cy = 0 → else-branch on both axes.
    // dy,dx ∈ {−1,0,+1}: py clamps to {0,0,1}, px clamps to {0,0,1}.
    // Setting the 2×2 block (rows 0..1, cols 0..1) to 0.0 → all 9 kernel samples hit
    // (each of the 9 (py,px) pairs maps into that block) → sf = 1.
    ShadowMap sm;
    sm.clear();
    sm.light_vp = mat4::identity();
    constexpr int S = ShadowMap::SIZE;
    for (int py = 0; py <= 1; py++)
        for (int px = 0; px <= 1; px++)
            sm.depth[static_cast<size_t>(py) * S + static_cast<size_t>(px)] = 0.0f;
    ASSERT_NEAR(sm.in_shadow({-1.0f, -1.0f, 0.5f}), 1.0f, 1e-6f);
}

TEST(shadow, pcf_top_border_clamps_y_kernel_samples)
{
    // ndc = (0, −1, 0.5) → cx = 1024 (interior), cy = 0 (top border).
    // else-branch fires because cy = 0, not > 0.
    // dy ∈ {−1,0,+1} clamp to py ∈ {0,0,1}: dy=−1 and dy=0 share row 0.
    // Setting only row 0 of the 3 center columns to 0.0 → 6 of 9 samples hit → 6/9.
    // Fails if py is not clamped (dy=−1 would produce invalid access).
    ShadowMap sm;
    sm.clear();
    sm.light_vp = mat4::identity();
    for (int px = 1023; px <= 1025; px++)
        sm.depth[static_cast<size_t>(px)] = 0.0f; // row 0
    ASSERT_NEAR(sm.in_shadow({0.0f, -1.0f, 0.5f}), 6.0f / 9.0f, 1e-6f);
}

// ─── in_shadow NDC z below −1 ─────────────────────────────────────────────────

TEST(shadow, in_shadow_ndc_z_below_neg_one_returns_lit)
{
    // ndc.z = −1.5 < −1.0 → NDC z bounds check → early-out returns 0 (lit).
    // With depths set to −2.0, ref = −1.501 > −2.0 would be true without the
    // check, producing sf = 1.0.  The check must fire to return 0.0.
    ShadowMap sm;
    sm.clear();
    sm.light_vp = mat4::identity();
    std::fill(sm.depth.begin(), sm.depth.end(), -2.0f);
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, -1.5f}), 0.0f, 1e-6f);
}

// ─── Degenerate triangle in build ─────────────────────────────────────────────

TEST(shadow, degenerate_triangle_skipped_no_depth_written)
{
    // Collinear vertices → face_n = (0,0,0) → denom = 0 → skipped via the
    // |denom| < 1e-6 guard.  No depth is written so the depth buffer stays at
    // 1.0 everywhere and any probe inside the frustum reports lit.
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.pos = {0.0f, 0.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {1.0f, 0.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {2.0f, 0.0f, 0.0f}; // collinear
    m.vertices.push_back(v);
    m.triangles.push_back({{0, 1, 2}});
    m.materials.push_back({});
    ShadowMap sm = build_shadow_map(m, make_light_z());
    // No depth written → stored = 1.0; ref for any in-frustum point ≤ 0.999 < 1.0 → lit.
    ASSERT_NEAR(sm.in_shadow({0.0f, 0.0f, -5.0f}), 0.0f, 1e-6f);
}
