#include "renderer_test_util.h"

// ─── Group K: shadow map end-to-end through the renderer ─────────────────────
// These tests exercise the renderer's shadow_map wiring that is NOT covered by
// test_shadow.cpp (build/query in isolation) or test_rasterize_shadow.cpp
// (rasterizer-level blending with pre-built col/shad values).
//
// Light: direction = normalize(1,0,1) ≈ (0.707,0,0.707) — NOT along the
// camera's z-axis, so the occluder can sit outside the receiver's screen
// footprint while still shadowing the receiver in light space.
//
// Receiver: make_unit_triangle() at z=0, centre pixel (20,10) → world (0,0,0).
//   dot(n=(0,0,1), L) = 0.707 → diffuse ≈ 0.707.
// Occluder: small triangle near (1,0,1) — on the same light ray as (0,0,0)
//   but in camera space it projects to ~(25,10), not overlapping (20,10).
//   Its light-space footprint covers (0,0), so shadow_factor at (0,0,0) ≈ 1.
//
// With ambient=(0.1,0,0) and key red:
//   Lit R  ≈ 0.1 + 0.707 ≈ 0.807 → ~206  (threshold: R ≥ 150)
//   Shadowed R ≈ 0.1 → ~25               (threshold: R ≤ 60)
//   Fill green G ≈ 0.707 → ~180           (threshold: G ≥ 150)

static Light make_diagonal_key_light(vec3 color = {1.0f, 0.0f, 0.0f})
{
    constexpr float inv_sqrt2 = 0.70710678f;
    Light l{};
    l.direction = {inv_sqrt2, 0.0f, inv_sqrt2};
    l.color = color;
    return l;
}

// Receiver: copy of make_unit_triangle() — z=0, normal (0,0,1).
static Mesh make_receiver_only()
{
    return make_unit_triangle();
}

// Shadow scene: receiver (z=0) + occluder (z=1) in one mesh, used ONLY for
// build_shadow_map so the frustum encompasses both triangles.  We render only
// make_receiver_only() so the occluder never appears on screen.
// Occluder centroid (1,0,1) is on the same light ray as (0,0,0):
//   dot((1,0,1), L) = 1.414 > dot((0,0,0), L) = 0  → closer to the light.
// In_shadow(0,0,0) sees a stored depth from the occluder that is less than
// the receiver's depth → shadow_factor ≈ 1.
// In camera space the occluder projects to screen ~(25,10), not (20,10).
static Mesh make_shadow_scene()
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = {0.0f, 0.0f, 1.0f};
    v.uv = {0.5f, 0.5f};

    // receiver
    v.pos = {-1.0f, -1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {1.0f, -1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 1.0f, 0.0f};
    m.vertices.push_back(v);
    // occluder
    v.pos = {0.7f, -0.3f, 1.0f};
    m.vertices.push_back(v);
    v.pos = {1.3f, -0.3f, 1.0f};
    m.vertices.push_back(v);
    v.pos = {1.0f, 0.3f, 1.0f};
    m.vertices.push_back(v);

    m.tangents.resize(6, {1.0f, 0.0f, 0.0f});

    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    m.triangles.push_back(tri);
    tri.v[0] = 3;
    tri.v[1] = 4;
    tri.v[2] = 5;
    tri.material_idx = 0;
    m.triangles.push_back(tri);
    m.materials.push_back(Material{});
    return m;
}

// K1: Phong — shadow map causes occluded pixel to darken.
TEST(renderer, phong_shadow_map_darkens_occluded_pixel)
{
    Mesh receiver = make_receiver_only();
    Camera cam = make_test_camera();
    Light light = make_diagonal_key_light();
    vec3 ambient{0.1f, 0.0f, 0.0f};

    Renderer r;
    r.mode = ShadingMode::Phong;

    // Baseline: no shadow map.
    Framebuffer fb_lit(40, 20, /*headless=*/true);
    fb_lit.clear();
    r.render(receiver, cam, &light, 1, ambient, fb_lit);
    ASSERT_TRUE(was_drawn(fb_lit, 20, 10));
    Color c_lit = fb_lit.get_pixel(20, 10);
    if (c_lit.r < 150)
        ASSERT_FAIL("Phong lit baseline R too low (" + std::to_string(static_cast<int>(c_lit.r)) + ")");

    // Shadow map built from full scene (receiver+occluder) so frustum covers both.
    ShadowMap sm = build_shadow_map(make_shadow_scene(), light);
    Framebuffer fb_shadow(40, 20, /*headless=*/true);
    fb_shadow.clear();
    r.render(receiver, cam, &light, 1, ambient, fb_shadow, &sm);
    ASSERT_TRUE(was_drawn(fb_shadow, 20, 10));
    Color c_shadow = fb_shadow.get_pixel(20, 10);
    if (c_shadow.r > 60)
        ASSERT_FAIL("Phong shadowed R too high (" + std::to_string(static_cast<int>(c_shadow.r)) + ") — shadow_map not applied");
}

// K2: Flat — shadow map darkens occluded pixel; exercises rt.fg.shad_* precomputation.
TEST(renderer, flat_shadow_map_darkens_occluded_pixel)
{
    Mesh receiver = make_receiver_only();
    Camera cam = make_test_camera();
    Light light = make_diagonal_key_light();
    vec3 ambient{0.1f, 0.0f, 0.0f};

    Renderer r;
    r.mode = ShadingMode::Flat;

    Framebuffer fb_lit(40, 20, /*headless=*/true);
    fb_lit.clear();
    r.render(receiver, cam, &light, 1, ambient, fb_lit);
    ASSERT_TRUE(was_drawn(fb_lit, 20, 10));
    Color c_lit = fb_lit.get_pixel(20, 10);
    if (c_lit.r < 150)
        ASSERT_FAIL("Flat lit baseline R too low (" + std::to_string(static_cast<int>(c_lit.r)) + ")");

    ShadowMap sm = build_shadow_map(make_shadow_scene(), light);
    Framebuffer fb_shadow(40, 20, /*headless=*/true);
    fb_shadow.clear();
    r.render(receiver, cam, &light, 1, ambient, fb_shadow, &sm);
    ASSERT_TRUE(was_drawn(fb_shadow, 20, 10));
    Color c_shadow = fb_shadow.get_pixel(20, 10);
    if (c_shadow.r > 60)
        ASSERT_FAIL("Flat shadowed R too high (" + std::to_string(static_cast<int>(c_shadow.r)) + ") — shadow_map not applied");
}

// K3: Gouraud — same check, per-vertex shad_* path.
TEST(renderer, gouraud_shadow_map_darkens_occluded_pixel)
{
    Mesh receiver = make_receiver_only();
    Camera cam = make_test_camera();
    Light light = make_diagonal_key_light();
    vec3 ambient{0.1f, 0.0f, 0.0f};

    Renderer r;
    r.mode = ShadingMode::Gouraud;

    Framebuffer fb_lit(40, 20, /*headless=*/true);
    fb_lit.clear();
    r.render(receiver, cam, &light, 1, ambient, fb_lit);
    ASSERT_TRUE(was_drawn(fb_lit, 20, 10));
    Color c_lit = fb_lit.get_pixel(20, 10);
    if (c_lit.r < 150)
        ASSERT_FAIL("Gouraud lit baseline R too low (" + std::to_string(static_cast<int>(c_lit.r)) + ")");

    ShadowMap sm = build_shadow_map(make_shadow_scene(), light);
    Framebuffer fb_shadow(40, 20, /*headless=*/true);
    fb_shadow.clear();
    r.render(receiver, cam, &light, 1, ambient, fb_shadow, &sm);
    ASSERT_TRUE(was_drawn(fb_shadow, 20, 10));
    Color c_shadow = fb_shadow.get_pixel(20, 10);
    if (c_shadow.r > 60)
        ASSERT_FAIL("Gouraud shadowed R too high (" + std::to_string(static_cast<int>(c_shadow.r)) + ") — shadow_map not applied");
}

// K4: Shadow map built from receiver alone — slope bias must prevent self-shadow.
TEST(renderer, shadow_map_no_occluder_no_false_darkening)
{
    Mesh receiver = make_receiver_only();
    Camera cam = make_test_camera();
    Light light = make_diagonal_key_light();
    vec3 ambient{0.1f, 0.0f, 0.0f};

    Renderer r;
    r.mode = ShadingMode::Phong;

    ShadowMap sm = build_shadow_map(receiver, light);
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(receiver, cam, &light, 1, ambient, fb, &sm);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
        ASSERT_FAIL("no-occluder self-shadow map darkened pixel (R=" + std::to_string(static_cast<int>(c.r)) + ") — false positive shadow");
}

// K5: n_lights=0 must discard the shadow_map (renderer.cpp:406).
// If that guard is missing, shadow_lights = lights + 1 with n_shadow_lights=-1
// would crash or produce garbage.
TEST(renderer, n_lights_zero_discards_shadow_map)
{
    Mesh receiver = make_receiver_only();
    Camera cam = make_test_camera();
    Light light = make_diagonal_key_light();
    vec3 ambient{0.1f, 0.0f, 0.0f};

    ShadowMap sm = build_shadow_map(make_shadow_scene(), light);

    Renderer r;
    r.mode = ShadingMode::Phong;

    // n_lights=0 with non-null shadow_map — must not crash; result = ambient only.
    Framebuffer fb_sm(40, 20, /*headless=*/true);
    fb_sm.clear();
    r.render(receiver, cam, &light, 0, ambient, fb_sm, &sm);

    // n_lights=0 with nullptr — baseline.
    Framebuffer fb_null(40, 20, /*headless=*/true);
    fb_null.clear();
    r.render(receiver, cam, &light, 0, ambient, fb_null, nullptr);

    // Both must produce identical pixels at the centre.
    ASSERT_TRUE(was_drawn(fb_sm, 20, 10));
    ASSERT_TRUE(was_drawn(fb_null, 20, 10));
    Color csm = fb_sm.get_pixel(20, 10);
    Color cnull = fb_null.get_pixel(20, 10);
    if (csm.r != cnull.r || csm.g != cnull.g || csm.b != cnull.b)
        ASSERT_FAIL("n_lights=0 + shadow_map produced different pixel than null shadow_map");
}

// K6: Phong + two lights — only key (lights[0]) is shadowed; fill survives.
TEST(renderer, phong_multi_light_only_key_shadowed)
{
    Mesh receiver = make_receiver_only();
    Camera cam = make_test_camera();
    Light lights[2];
    lights[0] = make_diagonal_key_light({1.0f, 0.0f, 0.0f}); // key: red
    lights[1] = make_diagonal_key_light({0.0f, 1.0f, 0.0f}); // fill: green
    vec3 ambient{0.0f, 0.0f, 0.0f};

    ShadowMap sm = build_shadow_map(make_shadow_scene(), lights[0]);

    Renderer r;
    r.mode = ShadingMode::Phong;
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(receiver, cam, lights, 2, ambient, fb, &sm);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 60)
        ASSERT_FAIL("Phong multi-light: key (red) not shadowed, R=" + std::to_string(static_cast<int>(c.r)));
    if (c.g < 150)
        ASSERT_FAIL("Phong multi-light: fill (green) incorrectly shadowed, G=" + std::to_string(static_cast<int>(c.g)));
}

// K7: Gouraud + two lights — covers the Flat/Gouraud shadow_lights split
// (renderer.cpp:108-109), which is a separate code path from Phong's.
TEST(renderer, gouraud_multi_light_only_key_shadowed)
{
    Mesh receiver = make_receiver_only();
    Camera cam = make_test_camera();
    Light lights[2];
    lights[0] = make_diagonal_key_light({1.0f, 0.0f, 0.0f});
    lights[1] = make_diagonal_key_light({0.0f, 1.0f, 0.0f});
    vec3 ambient{0.0f, 0.0f, 0.0f};

    ShadowMap sm = build_shadow_map(make_shadow_scene(), lights[0]);

    Renderer r;
    r.mode = ShadingMode::Gouraud;
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(receiver, cam, lights, 2, ambient, fb, &sm);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 60)
        ASSERT_FAIL("Gouraud multi-light: key (red) not shadowed, R=" + std::to_string(static_cast<int>(c.r)));
    if (c.g < 150)
        ASSERT_FAIL("Gouraud multi-light: fill (green) incorrectly shadowed, G=" + std::to_string(static_cast<int>(c.g)));
}

// ─── Group L: depth-buffer ordering ──────────────────────────────────────────
// Two overlapping unit triangles at different z depths. The closer one
// (z=+1, camera at +5) must win every contested pixel regardless of which
// triangle appears first in the mesh.
//
// Analytical NDC z values with the test camera (near=0.1, far=100, fov=π/2):
//   Closer  (world z=+1 → view_z=-4): ndc_z ≈ 0.952
//   Farther (world z=-1 → view_z=-6): ndc_z ≈ 0.969
// The smaller value wins the depth test.
//
// Material colours: closer → red (diffuse+ambient=(1,0,0))
//                   farther → blue (diffuse+ambient=(0,0,1))
// A lit closer pixel has R≥200, B≤30. A lit farther pixel has the opposite.

static Mesh make_two_z_triangles(bool closer_first)
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.uv = {0.5f, 0.5f};

    // Closer (z=+1): vertices share z=+1, normal +z.
    v.normal = {0.0f, 0.0f, 1.0f};
    v.pos = {-1.0f, -1.0f, 1.0f};
    m.vertices.push_back(v);
    v.pos = {1.0f, -1.0f, 1.0f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 1.0f, 1.0f};
    m.vertices.push_back(v);

    // Farther (z=-1): same XY, shifted back.
    v.pos = {-1.0f, -1.0f, -1.0f};
    m.vertices.push_back(v);
    v.pos = {1.0f, -1.0f, -1.0f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 1.0f, -1.0f};
    m.vertices.push_back(v);

    m.tangents.resize(6, {1.0f, 0.0f, 0.0f});

    Triangle tri_close{}, tri_far{};
    tri_close.v[0] = 0;
    tri_close.v[1] = 1;
    tri_close.v[2] = 2;
    tri_close.material_idx = 0;
    tri_far.v[0] = 3;
    tri_far.v[1] = 4;
    tri_far.v[2] = 5;
    tri_far.material_idx = 1;

    if (closer_first)
    {
        m.triangles.push_back(tri_close);
        m.triangles.push_back(tri_far);
    }
    else
    {
        m.triangles.push_back(tri_far);
        m.triangles.push_back(tri_close);
    }

    Material mat_close;
    mat_close.diffuse = {1.0f, 0.0f, 0.0f};
    mat_close.ambient = {1.0f, 0.0f, 0.0f};
    mat_close.specular = {0.0f, 0.0f, 0.0f};

    Material mat_far;
    mat_far.diffuse = {0.0f, 0.0f, 1.0f};
    mat_far.ambient = {0.0f, 0.0f, 1.0f};
    mat_far.specular = {0.0f, 0.0f, 0.0f};

    m.materials.push_back(mat_close);
    m.materials.push_back(mat_far);
    return m;
}

// L1: Phong — closer triangle wins regardless of submission order.
TEST(renderer, phong_depth_order_independent)
{
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 1.0f, 1.0f});
    vec3 ambient{0.05f, 0.05f, 0.05f};

    Renderer r(1);
    r.mode = ShadingMode::Phong;

    Framebuffer fb_cf(40, 20, /*headless=*/true), fb_ff(40, 20, /*headless=*/true);
    fb_cf.clear();
    fb_ff.clear();

    Mesh m_cf = make_two_z_triangles(true);
    Mesh m_ff = make_two_z_triangles(false);

    r.render(m_cf, cam, &light, 1, ambient, fb_cf);
    r.render(m_ff, cam, &light, 1, ambient, fb_ff);

    ASSERT_TRUE(was_drawn(fb_cf, 20, 10));
    ASSERT_TRUE(was_drawn(fb_ff, 20, 10));

    Color c_cf = fb_cf.get_pixel(20, 10);
    Color c_ff = fb_ff.get_pixel(20, 10);

    if (c_cf.r < 200)
        ASSERT_FAIL("Phong closer-first: red (closer) not dominant, R=" + std::to_string(static_cast<int>(c_cf.r)));
    if (c_cf.b > 30)
        ASSERT_FAIL("Phong closer-first: blue (farther) leaked through, B=" + std::to_string(static_cast<int>(c_cf.b)));
    if (c_ff.r < 200)
        ASSERT_FAIL("Phong farther-first: red (closer) not dominant, R=" + std::to_string(static_cast<int>(c_ff.r)));
    if (c_ff.b > 30)
        ASSERT_FAIL("Phong farther-first: blue (farther) leaked through, B=" + std::to_string(static_cast<int>(c_ff.b)));
    if (c_cf.r != c_ff.r || c_cf.g != c_ff.g || c_cf.b != c_ff.b)
        ASSERT_FAIL("Phong depth result differs between submission orders");
}

// L2: Gouraud — same assertions.
TEST(renderer, gouraud_depth_order_independent)
{
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 1.0f, 1.0f});
    vec3 ambient{0.05f, 0.05f, 0.05f};

    Renderer r(1);
    r.mode = ShadingMode::Gouraud;

    Framebuffer fb_cf(40, 20, /*headless=*/true), fb_ff(40, 20, /*headless=*/true);
    fb_cf.clear();
    fb_ff.clear();

    Mesh m_cf = make_two_z_triangles(true);
    Mesh m_ff = make_two_z_triangles(false);

    r.render(m_cf, cam, &light, 1, ambient, fb_cf);
    r.render(m_ff, cam, &light, 1, ambient, fb_ff);

    ASSERT_TRUE(was_drawn(fb_cf, 20, 10));
    ASSERT_TRUE(was_drawn(fb_ff, 20, 10));

    Color c_cf = fb_cf.get_pixel(20, 10);
    Color c_ff = fb_ff.get_pixel(20, 10);

    if (c_cf.r < 200)
        ASSERT_FAIL("Gouraud closer-first: red not dominant, R=" + std::to_string(static_cast<int>(c_cf.r)));
    if (c_cf.b > 30)
        ASSERT_FAIL("Gouraud closer-first: blue leaked through, B=" + std::to_string(static_cast<int>(c_cf.b)));
    if (c_ff.r < 200)
        ASSERT_FAIL("Gouraud farther-first: red not dominant, R=" + std::to_string(static_cast<int>(c_ff.r)));
    if (c_ff.b > 30)
        ASSERT_FAIL("Gouraud farther-first: blue leaked through, B=" + std::to_string(static_cast<int>(c_ff.b)));
    if (c_cf.r != c_ff.r || c_cf.g != c_ff.g || c_cf.b != c_ff.b)
        ASSERT_FAIL("Gouraud depth result differs between submission orders");
}

// L3: Flat — same assertions.
TEST(renderer, flat_depth_order_independent)
{
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 1.0f, 1.0f});
    vec3 ambient{0.05f, 0.05f, 0.05f};

    Renderer r(1);
    r.mode = ShadingMode::Flat;

    Framebuffer fb_cf(40, 20, /*headless=*/true), fb_ff(40, 20, /*headless=*/true);
    fb_cf.clear();
    fb_ff.clear();

    Mesh m_cf = make_two_z_triangles(true);
    Mesh m_ff = make_two_z_triangles(false);

    r.render(m_cf, cam, &light, 1, ambient, fb_cf);
    r.render(m_ff, cam, &light, 1, ambient, fb_ff);

    ASSERT_TRUE(was_drawn(fb_cf, 20, 10));
    ASSERT_TRUE(was_drawn(fb_ff, 20, 10));

    Color c_cf = fb_cf.get_pixel(20, 10);
    Color c_ff = fb_ff.get_pixel(20, 10);

    if (c_cf.r < 200)
        ASSERT_FAIL("Flat closer-first: red not dominant, R=" + std::to_string(static_cast<int>(c_cf.r)));
    if (c_cf.b > 30)
        ASSERT_FAIL("Flat closer-first: blue leaked through, B=" + std::to_string(static_cast<int>(c_cf.b)));
    if (c_ff.r < 200)
        ASSERT_FAIL("Flat farther-first: red not dominant, R=" + std::to_string(static_cast<int>(c_ff.r)));
    if (c_ff.b > 30)
        ASSERT_FAIL("Flat farther-first: blue leaked through, B=" + std::to_string(static_cast<int>(c_ff.b)));
    if (c_cf.r != c_ff.r || c_cf.g != c_ff.g || c_cf.b != c_ff.b)
        ASSERT_FAIL("Flat depth result differs between submission orders");
}

// L4: Stored depth at overlap matches the closer triangle's NDC z.
// Render farther-first so the depth value is overwritten when the closer
// triangle wins, verifying both the write and the comparison direction.
// Expected ndc_z for world z=+1 with the test camera:
//   view_z=-4, clip_z≈3.808, clip_w=4  →  ndc_z≈0.952
TEST(renderer, depth_value_matches_closer_triangle)
{
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 1.0f, 1.0f});
    vec3 ambient{0.05f, 0.05f, 0.05f};

    Renderer r(1);
    r.mode = ShadingMode::Phong;

    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    Mesh m = make_two_z_triangles(false); // farther first
    r.render(m, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    // ndc_z for world z=+1: (-(far+near)/(far-near))*(-4) + (-(2*far*near)/(far-near)) / 4
    //   = 3.808 / 4 ≈ 0.952; tolerance 0.005 comfortably separates from farther's 0.969.
    assert_depth_near(fb, 20, 10, 0.952f, 0.005f);
}
