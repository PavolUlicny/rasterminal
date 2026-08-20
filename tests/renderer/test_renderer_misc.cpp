#include "tests/renderer_test_util.h"

#include <algorithm>
#include <climits>

// Group N — renderer edge cases not covered elsewhere

// Renderer dispatches all workers regardless of framebuffer height. With 4
// workers and height=4, all workers participate (no fast-exit path). The CAS
// depth test arbitrates concurrent pixel writes safely. The render must
// complete and the triangle must be visible.
//
// make_large_triangle() (v0=(-4,-4,0), v1=(4,-4,0), v2=(0,4,0)) projects to
// x=[18.4..21.6], y=[0.4..3.6] on a 40x4 buffer (aspect=10, fov=pi/2), so it
// spans all four rows and several columns — enough for the rasterizer to hit.

TEST(renderer, n_active_capped_to_half_framebuffer_height)
{
    Renderer r(4);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_large_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.2f, 0.2f, 0.2f };
    // height=4; all 4 workers participate (no n_active cap in single-pass design).
    Framebuffer fb(40, 4, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    if (count_drawn_pixels(fb) == 0)
    {
        ASSERT_FAIL("n_active cap: no pixels drawn with height=4 and 4 workers");
    }
}

// Frame 1 uses a 40x20 buffer, frame 2 uses a 40x4 buffer.
// The single-pass design has no band buffers to resize; this test verifies
// that the work-stealing cursor resets and both frames draw the triangle.

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

    // Second render with smaller height — m_tri_cursor resets, all workers re-run.
    Framebuffer fb2(40, 4, /*headless=*/true);
    fb2.clear();
    r.render(mesh, cam, &light, 1, ambient, fb2);
    if (count_drawn_pixels(fb2) == 0)
    {
        ASSERT_FAIL("band_tris resize: frame 2 (40x4) drew nothing after resize");
    }
}

// Texture with alpha=0 everywhere; mat.alpha_cutoff=0.5; show_texture=true.
// Renderer sets rt.alpha_cutoff = show_tex ? mat.alpha_cutoff : 0 = 0.5.
// rasterize_flat() samples alpha=0 < 0.5 -> every pixel is culled -> nothing drawn.

TEST(renderer, alpha_cutoff_show_tex_on_transparent_not_drawn)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_unit_triangle();

    // Fully transparent texture (RGBA all zeros).
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

// Same mesh as the previous test (alpha=0 texture, alpha_cutoff=0.5) but show_texture=false.
// Renderer sets rt.alpha_cutoff = 0 and tex = nullptr -> rasterize_flat() never
// samples the texture -> pixel is drawn from the material colour instead.

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

// Triangle at world x=50, z=0 is in front of the near plane (clip_w=5 > 0.1)
// so clip_near passes it. But all three vertices have clip_x >> clip_w
// (NDC_x ~5) -> clip_reject returns true -> nothing rasterized.
//
// With the test camera (eye=(0,0,5), fov=pi/2, aspect=2 on 40x20):
//   clip_x = view_x * cot(fov/2) / aspect = view_x * 0.5
//   For world x=50: clip_x=25, clip_w=5 -> 25 > 5 -> outside right plane.

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

// Mesh with all-zero tangents (no TBN perturbation available).
// p_tans[i] = {0,0,0} → rasterize_phong receives zero tangents and falls
// back to the interpolated vertex normals for per-pixel lighting.
// Verifies that ambient lighting still reaches the framebuffer.

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

// Pins the shared thread-count resolver contract (-1 = auto, 0 = all cores,
// N clamped to hw) with machine-independent relations, so an unclamped path can't
// regress silently (the pre-fix bug: -j above hw reached the bench header and
// load-time threading unclamped). main.cpp resolves through this same function.
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
