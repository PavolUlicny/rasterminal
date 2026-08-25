#include "tests/renderer_test_util.h"

#include <algorithm>
#include <climits>

// Renderer edge cases

// Four workers share four rows while a large triangle exercises concurrent depth writes.

TEST(renderer, n_active_capped_to_half_framebuffer_height)
{
    Renderer r(4);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_large_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.2f, 0.2f, 0.2f };
    // The single-pass renderer uses all four workers.
    Framebuffer fb(40, 4, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    if (count_drawn_pixels(fb) == 0)
    {
        ASSERT_FAIL("n_active cap: no pixels drawn with height=4 and 4 workers");
    }
}

// Rendering after a height change must reset the work cursor.

TEST(renderer, band_tris_resize_across_frames)
{
    Renderer r(4);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_large_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.2f, 0.2f, 0.2f };

    Framebuffer fb1(40, 20, /*headless=*/true);
    fb1.clear();
    r.render(mesh, cam, &light, 1, ambient, fb1);
    if (count_drawn_pixels(fb1) == 0)
    {
        ASSERT_FAIL("band_tris resize: frame 1 (40x20) drew nothing");
    }

    Framebuffer fb2(40, 4, /*headless=*/true);
    fb2.clear();
    r.render(mesh, cam, &light, 1, ambient, fb2);
    if (count_drawn_pixels(fb2) == 0)
    {
        ASSERT_FAIL("band_tris resize: frame 2 (40x4) drew nothing after resize");
    }
}

// With textures enabled, alpha 0 falls below the 0.5 cutoff and draws nothing.

TEST(renderer, alpha_cutoff_show_tex_on_transparent_not_drawn)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_unit_triangle();

    Texture t;
    t.width = 2;
    t.height = 2;
    t.pixels.resize(16, 0); // r=g=b=a=0
    mesh.textures.push_back(std::move(t));
    mesh.materials[0].diffuse_map.tex = 0;
    mesh.materials[0].alpha_cutoff = 0.5f;

    r.show_texture = true;
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.2f, 0.2f, 0.2f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    if (count_drawn_pixels(fb) != 0)
    {
        ASSERT_FAIL("alpha_cutoff: transparent texture pixels must be culled when show_texture=true");
    }
}

// Disabling textures also disables their alpha cutoff, so the material draws.

TEST(renderer, alpha_cutoff_zeroed_when_show_tex_false)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_unit_triangle();

    Texture t;
    t.width = 2;
    t.height = 2;
    t.pixels.resize(16, 0);
    mesh.textures.push_back(std::move(t));
    mesh.materials[0].diffuse_map.tex = 0;
    mesh.materials[0].alpha_cutoff = 0.5f;

    r.show_texture = false; // disables both tex and alpha_cutoff
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.2f, 0.2f, 0.2f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    if (!was_drawn(fb, 20, 10))
    {
        ASSERT_FAIL("alpha_cutoff zeroed: triangle must be drawn when show_texture=false");
    }
}

// A triangle at world x=50 passes near clipping but has clip_x=25 > clip_w=5,
// so conservative frustum rejection removes it before rasterization.

TEST(renderer, clip_reject_removes_off_screen_triangle_mt)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;

    Mesh mesh;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = { 0.0f, 0.0f, 1.0f };
    v.uv = { 0.5f, 0.5f };
    v.pos = { 50.0f, -1.0f, 0.0f };
    mesh.vertices.push_back(v);
    v.pos = { 51.0f, -1.0f, 0.0f };
    mesh.vertices.push_back(v);
    v.pos = { 50.5f, 1.0f, 0.0f };
    mesh.vertices.push_back(v);
    mesh.tangents.resize(3, { 1.0f, 0.0f, 0.0f });
    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    mesh.triangles.push_back(tri);
    mesh.materials.push_back(Material{});

    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.2f, 0.2f, 0.2f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    if (count_drawn_pixels(fb) != 0)
    {
        ASSERT_FAIL("clip_reject MT: off-screen triangle must produce no pixels");
    }
}

TEST(renderer, clip_reject_removes_off_screen_triangle_wireframe)
{
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;

    Mesh mesh;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = { 0.0f, 0.0f, 1.0f };
    v.uv = { 0.5f, 0.5f };
    v.pos = { 50.0f, -1.0f, 0.0f };
    mesh.vertices.push_back(v);
    v.pos = { 51.0f, -1.0f, 0.0f };
    mesh.vertices.push_back(v);
    v.pos = { 50.5f, 1.0f, 0.0f };
    mesh.vertices.push_back(v);
    mesh.tangents.resize(3, { 1.0f, 0.0f, 0.0f });
    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    mesh.triangles.push_back(tri);
    mesh.materials.push_back(Material{});

    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    if (count_drawn_pixels(fb) != 0)
    {
        ASSERT_FAIL("clip_reject wireframe: off-screen triangle must produce no pixels");
    }
}

// Zero tangents disable TBN perturbation, so Phong falls back to interpolated normals.

TEST(renderer, phong_zero_tangents_uses_vertex_normals)
{
    Mesh mesh;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = { 0.0f, 0.0f, 1.0f };
    v.uv = { 0.5f, 0.5f };
    v.pos = { -1.0f, -1.0f, 0.0f };
    mesh.vertices.push_back(v);
    v.pos = { 1.0f, -1.0f, 0.0f };
    mesh.vertices.push_back(v);
    v.pos = { 0.0f, 1.0f, 0.0f };
    mesh.vertices.push_back(v);
    // zero tangents → no TBN perturbation; rasterize_phong falls back to vertex normals
    mesh.tangents.resize(3, { 0.0f, 0.0f, 0.0f });
    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    mesh.triangles.push_back(tri);
    Material mat;
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mesh.materials.push_back(mat);

    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Camera cam = make_test_camera();
    vec3 ambient{ 0.5f, 0.5f, 0.5f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);
    if (!was_drawn(fb, 20, 10))
    {
        ASSERT_FAIL("phong zero tangents: centre pixel not drawn");
    }
}

TEST(renderer, choose_phase1_chunk_zero_tris_no_crash)
{
    // Back-facing triangle (flipped winding) → all tris rejected by backface cull
    // → total_tris = 0 at Phase 1 setup → choose_phase1_chunk(0, n) returns MIN_CHUNK=64.
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = true;
    Mesh mesh = make_unit_triangle(/*flip_winding=*/true);
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    ASSERT_EQ(count_drawn_pixels(fb), 0);
}

// Pin the shared resolver contract: -1 = default, 0 = all cores, N clamped to hardware.
// main.cpp uses this function for both benchmark reporting and load-time threading.
TEST(renderer, resolve_thread_count_contract)
{
    const int all = Renderer::resolve_thread_count(0);
    ASSERT_TRUE(all >= 1);
    // Explicit N above hw clamps to hw (== the all-cores resolution).
    ASSERT_EQ(Renderer::resolve_thread_count(INT_MAX), all);
    ASSERT_EQ(Renderer::resolve_thread_count(all), all);
    // Auto = min(hw, 4) for a half-block frame, every core for a pixel-backend one. Only the
    // default moves: an explicit request resolves the same either way.
    ASSERT_EQ(Renderer::resolve_thread_count(-1), std::min(all, 4));
    ASSERT_EQ(Renderer::resolve_thread_count(-1, /*all_cores_default=*/false), std::min(all, 4));
    ASSERT_EQ(Renderer::resolve_thread_count(-1, /*all_cores_default=*/true), all);
    ASSERT_EQ(Renderer::resolve_thread_count(0, /*all_cores_default=*/false), all);
    ASSERT_EQ(Renderer::resolve_thread_count(1, /*all_cores_default=*/true), 1);
    ASSERT_EQ(Renderer::resolve_thread_count(INT_MAX, /*all_cores_default=*/true), all);
    // hw >= 1 always (hardware_concurrency()==0 floors to 1), so 1 passes through.
    ASSERT_EQ(Renderer::resolve_thread_count(1), 1);
    // Idempotent: a resolved value is a fixed point (why the ctor may re-resolve).
    ASSERT_EQ(Renderer::resolve_thread_count(Renderer::resolve_thread_count(INT_MAX)), all);
}
