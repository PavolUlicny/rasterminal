#include "tests/renderer_test_util.h"

#include <cstdint>

// ─── near-clip mesh helpers ───────────────────────────────────────────────────
//
// Camera eye=(0,0,5), near_plane=0.1.  Clip w = 5 − world_z.
// World z=0   → w=5   (comfortably in front).
// World z=4.95 → w=0.05 (behind near plane → clipped).

// Two vertices in front (z=0), one behind (z=4.95).
// clip_near: 2 inside, 1 outside → 2 output triangles.
// Face normal·(eye−v0) = 0.4 > 0 → front-facing.
static Mesh make_straddling_triangle_one_behind()
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = { 0.0f, 0.0f, 1.0f };
    v.uv = { 0.5f, 0.5f };

    v.pos = { -2.0f, -2.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 2.0f, -2.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 0.0f, 0.0f, 4.95f };
    m.vertices.push_back(v);

    m.tangents.resize(3, { 1.0f, 0.0f, 0.0f });

    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    m.triangles.push_back(tri);
    m.materials.push_back(Material{});
    return m;
}

// One vertex in front (z=0), two behind (z=4.95).
// clip_near: 1 inside, 2 outside → 1 output triangle.
// Winding chosen so face normal·(eye−v0) > 0 (front-facing).
// After clip, screen vertices ≈ (20,14)-(29,12)-(11,12) → ~18 px area.
static Mesh make_straddling_triangle_two_behind()
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = { 0.0f, 0.0f, 1.0f };
    v.uv = { 0.5f, 0.5f };

    v.pos = { 0.0f, -2.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 0.09f, 0.0f, 4.95f };
    m.vertices.push_back(v);
    v.pos = { -0.09f, 0.0f, 4.95f };
    m.vertices.push_back(v);

    m.tangents.resize(3, { 1.0f, 0.0f, 0.0f });

    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    m.triangles.push_back(tri);
    m.materials.push_back(Material{});
    return m;
}

// All three vertices behind the near plane (z=4.95, w=0.05 < near=0.1).
// clip_near returns 0 → nothing rasterized.
// Face normal = (0,0,4): front-facing so backface cull passes first.
static Mesh make_fully_behind_triangle()
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = { 0.0f, 0.0f, 1.0f };
    v.uv = { 0.5f, 0.5f };

    v.pos = { -1.0f, -1.0f, 4.95f };
    m.vertices.push_back(v);
    v.pos = { 1.0f, -1.0f, 4.95f };
    m.vertices.push_back(v);
    v.pos = { 0.0f, 1.0f, 4.95f };
    m.vertices.push_back(v);

    m.tangents.resize(3, { 1.0f, 0.0f, 0.0f });

    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    m.triangles.push_back(tri);
    m.materials.push_back(Material{});
    return m;
}

// ─── Group G — near-plane clip integration ────────────────────────────────────
//
// Camera: eye=(0,0,5), near_plane=0.1. Clip w = 5 − world_z.
// Vertices at z=4.95 have w=0.05 → behind near plane (w < 0.1).
// All tests verify that Renderer::render() forwards clip_near results correctly
// for both the MT (Phong/Flat) and wireframe code paths.

// G1: one vertex behind near plane, two in front.
// clip_near produces 2 output tris → pixels must be drawn (MT path).
TEST(renderer, near_clip_one_vertex_behind_renders)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_straddling_triangle_one_behind();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.1f, 0.1f, 0.1f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    if (count_drawn_pixels(fb) == 0)
    {
        ASSERT_FAIL("one-vertex-behind triangle: clip_near must produce visible output");
    }
}

// G2: two vertices behind near plane, one in front.
// clip_near produces 1 output tri → pixels must be drawn (MT path).
TEST(renderer, near_clip_two_vertices_behind_renders)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_straddling_triangle_two_behind();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.1f, 0.1f, 0.1f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    if (count_drawn_pixels(fb) == 0)
    {
        ASSERT_FAIL("two-vertices-behind triangle: clip_near must produce visible output");
    }
}

// G3: all three vertices behind near plane → clip_near returns 0 → no pixels (MT path).
TEST(renderer, near_clip_fully_behind_draws_nothing)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_fully_behind_triangle();
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.1f, 0.1f, 0.1f }, fb);
    if (count_drawn_pixels(fb) != 0)
    {
        ASSERT_FAIL("fully-behind triangle must not produce any pixels");
    }
}

// G4: wireframe path with a straddling triangle → clip_near fires → pixels drawn.
// Covers the separate clip_near call in the wireframe branch (renderer.cpp:430).
TEST(renderer, near_clip_wireframe_straddling_renders)
{
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    Mesh mesh = make_straddling_triangle_one_behind();
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    if (count_drawn_pixels(fb) == 0)
    {
        ASSERT_FAIL("wireframe: straddling triangle must draw clipped edges");
    }
}

// G5: wireframe path with all vertices behind → no pixels.
TEST(renderer, near_clip_wireframe_fully_behind_draws_nothing)
{
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    Mesh mesh = make_fully_behind_triangle();
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    if (count_drawn_pixels(fb) != 0)
    {
        ASSERT_FAIL("wireframe: fully-behind triangle must produce no pixels");
    }
}

// G6: raising camera.near_plane above all clip-w values rejects a front-facing
// triangle that would otherwise render. Verifies m_near_plane is forwarded from
// camera.near_plane in the MT dispatch path (renderer.cpp:480).
TEST(renderer, near_clip_uses_camera_near_plane_value)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_unit_triangle(); // all vertices at z=0, clip w=5
    Camera cam = make_test_camera();
    cam.near_plane = 10.0f; // all w=5 < 10 → clip_near returns 0 for every tri
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.1f, 0.1f, 0.1f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    if (count_drawn_pixels(fb) != 0)
    {
        ASSERT_FAIL("near_plane=10 must reject all vertices (clip w=5 < near_plane)");
    }
}

// ─── grid mesh helper ─────────────────────────────────────────────────────────

// Build a grid_w×grid_h cell mesh over x,y ∈ [-half, +half] at z=0.
// Each cell splits into 2 CCW front-facing triangles → 2*grid_w*grid_h tris.
// With grid_w=grid_h=16 and half=4, the test camera (fov=π/2, aspect 2,
// eye=(0,0,5)) projects the grid to screen rect (12..28)×(2..18) ≈ 256 px.
// 512 triangles with 4 workers → choose_phase1_chunk=64 → ~2 chunks/worker.
static Mesh make_grid_mesh(int grid_w, int grid_h, float half = 4.0f)
{
    Mesh m;
    const float dx = 2.0f * half / static_cast<float>(grid_w);
    const float dy = 2.0f * half / static_cast<float>(grid_h);

    Vertex v{};
    v.ao = 1.0f;
    v.normal = { 0.0f, 0.0f, 1.0f };
    v.uv = { 0.5f, 0.5f };
    for (int j = 0; j <= grid_h; j++)
    {
        for (int i = 0; i <= grid_w; i++)
        {
            v.pos = { -half + (static_cast<float>(i) * dx), -half + (static_cast<float>(j) * dy), 0.0f };
            m.vertices.push_back(v);
        }
    }

    m.tangents.resize(m.vertices.size(), { 1.0f, 0.0f, 0.0f });

    for (int j = 0; j < grid_h; j++)
    {
        for (int i = 0; i < grid_w; i++)
        {
            const auto v00 = static_cast<uint32_t>((j * (grid_w + 1)) + i);
            const auto v10 = static_cast<uint32_t>((j * (grid_w + 1)) + i + 1);
            const auto v01 = static_cast<uint32_t>(((j + 1) * (grid_w + 1)) + i);
            const auto v11 = static_cast<uint32_t>(((j + 1) * (grid_w + 1)) + i + 1);
            Triangle tri{};
            tri.material_idx = 0;
            tri.v[0] = v00;
            tri.v[1] = v10;
            tri.v[2] = v11;
            m.triangles.push_back(tri);
            tri.v[0] = v00;
            tri.v[1] = v11;
            tri.v[2] = v01;
            m.triangles.push_back(tri);
        }
    }

    m.materials.push_back(Material{});
    return m;
}

// ─── Group H — multiple triangles / work-stealing at scale ───────────────────
//
// 16×16 grid = 512 triangles. With 4 workers, choose_phase1_chunk=64
// → 8 chunks → ~2 iterations per worker in the Phase 1 steal loop.
// Projected coverage: screen rect (12..28)×(2..18) ≈ 256 pixels.

// H1: 512-triangle grid renders with expected pixel coverage (single worker).
// Catches: chunk-loop early exit → only ~12% of grid processed.
TEST(renderer, many_triangles_grid_renders_expected_coverage)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_grid_mesh(16, 16);
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.1f, 0.1f, 0.1f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    int drawn = count_drawn_pixels(fb);
    if (drawn < 200)
    {
        ASSERT_FAIL("grid: only " + std::to_string(drawn) + " pixels drawn, expected ≥200");
    }
}

// H2: same grid with 1 vs 4 workers → identical pixel counts.
// Catches: worker data race, cursor double-claim, band ordering bug.
TEST(renderer, many_triangles_consistent_across_thread_counts)
{
    Mesh mesh = make_grid_mesh(16, 16);
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.1f, 0.1f, 0.1f };

    Framebuffer fb1(40, 20, /*headless=*/true), fb4(40, 20, /*headless=*/true);
    fb1.clear();
    fb4.clear();
    {
        Renderer r1(1);
        r1.mode = ShadingMode::Phong;
        r1.render(mesh, cam, &light, 1, ambient, fb1);
    }
    {
        Renderer r4(4);
        r4.mode = ShadingMode::Phong;
        r4.render(mesh, cam, &light, 1, ambient, fb4);
    }

    int n1 = count_drawn_pixels(fb1);
    int n4 = count_drawn_pixels(fb4);
    if (n1 != n4)
    {
        ASSERT_FAIL(
            "1-worker drew " + std::to_string(n1) + " px, 4-worker drew " + std::to_string(n4) + " px: must match"
        );
    }
}

// H3: rendering same mesh twice reuses same Renderer → second frame must match.
// Catches: m_tri_cursor not reset before dispatch → second render claims 0 work → empty fb2.
TEST(renderer, many_triangles_repeated_render_resets_cursor)
{
    Renderer r(2);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_grid_mesh(16, 16);
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.1f, 0.1f, 0.1f };

    Framebuffer fb1(40, 20, /*headless=*/true), fb2(40, 20, /*headless=*/true);
    fb1.clear();
    fb2.clear();
    r.render(mesh, cam, &light, 1, ambient, fb1);
    r.render(mesh, cam, &light, 1, ambient, fb2);

    int n1 = count_drawn_pixels(fb1);
    int n2 = count_drawn_pixels(fb2);
    if (n1 != n2)
    {
        ASSERT_FAIL(
            "second render drew " + std::to_string(n2) + " px vs first " + std::to_string(n1) +
            ": m_tri_cursor may not have been reset"
        );
    }
    assert_pixel_near(fb2, 20, 10, fb1.get_pixel(20, 10), 1);
}

// H4: after rendering a full grid, rendering an empty mesh must produce nothing.
// Catches: stale framebuffer pixels not cleared between renders (fb.clear() must
// be called by the test, and the empty mesh must not draw anything new).
TEST(renderer, many_triangles_then_empty_mesh_clears_bands)
{
    Renderer r(2);
    r.mode = ShadingMode::Phong;
    Mesh grid = make_grid_mesh(16, 16);
    Mesh empty;
    empty.materials.push_back(Material{});
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.1f, 0.1f, 0.1f };

    Framebuffer fb1(40, 20, /*headless=*/true), fb2(40, 20, /*headless=*/true);
    fb1.clear();
    fb2.clear();
    r.render(grid, cam, &light, 1, ambient, fb1);
    r.render(empty, cam, &light, 1, ambient, fb2);

    int n = count_drawn_pixels(fb2);
    if (n != 0)
    {
        ASSERT_FAIL(
            "empty mesh after grid: " + std::to_string(n) +
            " stale pixels remain — fb.clear() or tri-cursor reset may be broken"
        );
    }
}

// ─── AO mesh helpers ──────────────────────────────────────────────────────────

// Same geometry as make_unit_triangle (front-facing, centre pixel (20,10))
// but with caller-specified per-vertex AO values.
static Mesh make_unit_triangle_ao(float ao_a, float ao_b, float ao_c)
{
    Mesh m;
    Vertex v{};
    v.normal = { 0.0f, 0.0f, 1.0f };
    v.uv = { 0.5f, 0.5f };

    v.ao = ao_a;
    v.pos = { -1.0f, -1.0f, 0.0f };
    m.vertices.push_back(v);
    v.ao = ao_b;
    v.pos = { 1.0f, -1.0f, 0.0f };
    m.vertices.push_back(v);
    v.ao = ao_c;
    v.pos = { 0.0f, 1.0f, 0.0f };
    m.vertices.push_back(v);

    m.tangents.resize(3, { 1.0f, 0.0f, 0.0f });

    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    m.triangles.push_back(tri);
    m.materials.push_back(Material{});
    return m;
}

// Same geometry as make_large_triangle (sa≈(12,18), sb≈(28,18), sc≈(20,2))
// but with caller-specified per-vertex AO values.
// Samples near a: pixel (13,17); near c: pixel (20,3).
static Mesh make_screen_triangle_ao(float ao_a, float ao_b, float ao_c)
{
    Mesh m;
    Vertex v{};
    v.normal = { 0.0f, 0.0f, 1.0f };
    v.uv = { 0.5f, 0.5f };

    v.ao = ao_a;
    v.pos = { -4.0f, -4.0f, 0.0f };
    m.vertices.push_back(v);
    v.ao = ao_b;
    v.pos = { 4.0f, -4.0f, 0.0f };
    m.vertices.push_back(v);
    v.ao = ao_c;
    v.pos = { 0.0f, 4.0f, 0.0f };
    m.vertices.push_back(v);

    m.tangents.resize(3, { 1.0f, 0.0f, 0.0f });

    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    m.triangles.push_back(tri);
    m.materials.push_back(Material{});
    return m;
}

// ─── Group I — AO end-to-end ──────────────────────────────────────────────────
//
// Strategy: n_lights=0 so compute_lighting output = ambient * mat.ambient * ao.
// With ambient=(0.8,0,0) and default mat.ambient=(1,1,1):
//   ao=1.0  → R ≈ 204
//   ao=0.0  → R = 0
//   ao=1/3  → R ≈ 68  (Flat average of 1+0+0)

// I1: Flat — all ao=0 → ambient term zero → pixel is essentially black.
// Catches: ao dropped from ClipVert construction or Flat face_ao path.
TEST(renderer, flat_ao_uniform_zero_darkens_pixel)
{
    Renderer r(1);
    r.mode = ShadingMode::Flat;
    Mesh mesh = make_unit_triangle_ao(0.0f, 0.0f, 0.0f);
    Camera cam = make_test_camera();
    vec3 ambient{ 0.8f, 0.0f, 0.0f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 5)
    {
        ASSERT_FAIL("flat ao=0: R=" + std::to_string(static_cast<int>(c.r)) + " expected ≤5");
    }
}

// I8: Flat — all ao=1 → full ambient → bright baseline.
// Counterpart to I1: ensures the n_lights=0 + ambient setup actually produces
// a bright pixel when ao=1, making the uniform-zero tests falsifiable.
TEST(renderer, flat_ao_uniform_one_full_brightness_baseline)
{
    Renderer r(1);
    r.mode = ShadingMode::Flat;
    Mesh mesh = make_unit_triangle_ao(1.0f, 1.0f, 1.0f);
    Camera cam = make_test_camera();
    vec3 ambient{ 0.8f, 0.0f, 0.0f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 180)
    {
        ASSERT_FAIL("flat ao=1: R=" + std::to_string(static_cast<int>(c.r)) + " expected ≥180");
    }
}

// I2: Flat — ao=(1,0,0) → face_ao=1/3 → R≈68, uniform across triangle.
// Catches: Flat picking a single vertex's ao instead of averaging.
TEST(renderer, flat_ao_averaged_across_vertices)
{
    Renderer r(1);
    r.mode = ShadingMode::Flat;
    Mesh mesh = make_unit_triangle_ao(1.0f, 0.0f, 0.0f);
    Camera cam = make_test_camera();
    vec3 ambient{ 0.8f, 0.0f, 0.0f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    // face_ao = (1+0+0)/3 ≈ 0.333 → R = 0.8*0.333*255 ≈ 68; allow [50,90].
    if (c.r < 50 || c.r > 90)
    {
        ASSERT_FAIL("flat ao avg: R=" + std::to_string(static_cast<int>(c.r)) + " expected 50-90 (face_ao≈1/3)");
    }
}

// I5: Phong — all ao=0 → rasterize_phong produces 0 ambient → black.
// Catches: ao not copied into rt.ph.aoa/b/c or not forwarded to rasterize_phong.
TEST(renderer, phong_ao_uniform_zero_darkens_pixel)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_unit_triangle_ao(0.0f, 0.0f, 0.0f);
    Camera cam = make_test_camera();
    vec3 ambient{ 0.8f, 0.0f, 0.0f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 5)
    {
        ASSERT_FAIL("phong ao=0: R=" + std::to_string(static_cast<int>(c.r)) + " expected ≤5");
    }
}

// I6: Phong — ao=(1,0,0) → per-pixel AO gradient across triangle.
// Catches: Phong using a uniform ao or hardcoding ao=1 per pixel.
TEST(renderer, phong_ao_interpolates_per_pixel)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_screen_triangle_ao(1.0f, 0.0f, 0.0f);
    Camera cam = make_test_camera();
    vec3 ambient{ 0.8f, 0.0f, 0.0f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 13, 17));
    ASSERT_TRUE(was_drawn(fb, 20, 3));
    int r_near_a = static_cast<int>(fb.get_pixel(13, 17).r);
    int r_near_c = static_cast<int>(fb.get_pixel(20, 3).r);
    if (r_near_a < 120)
    {
        ASSERT_FAIL("phong ao interp: near-a R=" + std::to_string(r_near_a) + " expected ≥120");
    }
    if (r_near_c > 30)
    {
        ASSERT_FAIL("phong ao interp: near-c R=" + std::to_string(r_near_c) + " expected ≤30");
    }
    if (r_near_a - r_near_c < 80)
    {
        ASSERT_FAIL("phong ao interp: gradient=" + std::to_string(r_near_a - r_near_c) + " expected ≥80");
    }
}

// I7: AO must not affect direct diffuse (only ambient).
// Render once with ao=0 and once with ao=1; ambient=0 so the ao×ambient
// term is zero in both cases.  Both centre pixels must match within ±1.
// Catches: a regression multiplying AO into the diffuse term.
TEST(renderer, ao_does_not_affect_direct_diffuse)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 0.5f, 0.0f, 0.0f });
    vec3 ambient{ 0.0f, 0.0f, 0.0f };

    Framebuffer fb0(40, 20, /*headless=*/true), fb1(40, 20, /*headless=*/true);
    fb0.clear();
    fb1.clear();

    Mesh mesh_ao0 = make_unit_triangle_ao(0.0f, 0.0f, 0.0f);
    Mesh mesh_ao1 = make_unit_triangle_ao(1.0f, 1.0f, 1.0f);
    r.render(mesh_ao0, cam, &light, 1, ambient, fb0);
    r.render(mesh_ao1, cam, &light, 1, ambient, fb1);

    ASSERT_TRUE(was_drawn(fb0, 20, 10));
    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    // Diffuse is the same regardless of AO when ambient=0.
    assert_pixel_near(fb0, 20, 10, fb1.get_pixel(20, 10), 1);
}
