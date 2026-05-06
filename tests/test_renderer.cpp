#include "test.h"
#include "rasterize_test_util.h"
#include "../src/renderer.h"
#include "../src/shadow.h"

#include <cmath>
#include <limits>

// ─── helpers ──────────────────────────────────────────────────────────────────

// Camera at (0,0,5) looking at origin.  pitch=0 so the projection is symmetric
// and pixel (20,10) on a 40×20 framebuffer maps exactly to world (0,0,0).
// fov = π/2, aspect 2:1 → x_ndc = x_world/(2*(5-z)), y_ndc = y_world/(5-z).
static Camera make_test_camera()
{
    Camera c;
    c.target = {0.0f, 0.0f, 0.0f};
    c.distance = 5.0f;
    c.yaw = 0.0f;
    c.pitch = 0.0f;
    c.fov = 3.14159265f / 2.0f; // π/2  (60° vertical: swap to 90° for easier math)
    c.near_plane = 0.1f;
    c.far_plane = 100.0f;
    return c;
}

// Single-triangle mesh at z=0 facing the camera (+Z).
// Front face: v0=(-1,-1,0), v1=(1,-1,0), v2=(0,1,0).  Normal = (0,0,1).
// With camera above and fov=π/2 the triangle projects to screen area
// sa≈(18,12), sb≈(22,12), sc≈(20,8) on 40×20, covering centre pixel (20,10).
// flip_winding=true swaps v1/v2 so the triangle faces -Z (back-face from camera).
// double_sided=true marks the single material as double-sided.
static Mesh make_unit_triangle(bool flip_winding = false, bool double_sided = false)
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = {0.0f, 0.0f, 1.0f};
    v.uv = {0.5f, 0.5f};

    v.pos = {-1.0f, -1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {1.0f, -1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 1.0f, 0.0f};
    m.vertices.push_back(v);

    // Tangents required so Phong Phase 1 does not dereference an empty vector.
    m.tangents.resize(3, {1.0f, 0.0f, 0.0f});

    Triangle tri{};
    if (flip_winding)
    {
        tri.v[0] = 0;
        tri.v[1] = 2; // swap v1/v2 → face normal points -Z
        tri.v[2] = 1;
    }
    else
    {
        tri.v[0] = 0;
        tri.v[1] = 1;
        tri.v[2] = 2;
    }
    tri.material_idx = 0;
    m.triangles.push_back(tri);

    Material mat;
    mat.double_sided = double_sided;
    m.materials.push_back(mat);

    if (double_sided)
        m.has_double_sided = true;

    return m;
}

// Large screen-filling triangle: z=0, v0=(-4,-4,0), v1=(4,-4,0), v2=(0,4,0).
// With the test camera (fov=π/2, aspect 2:1, eye=(0,0,5)) this projects to
// screen sa≈(12,18), sb≈(28,18), sc≈(20,2) on 40×20 — spanning all 4 worker
// bands.  Face normal = +Z (front-facing from +Z camera).
static Mesh make_large_triangle()
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = {0.0f, 0.0f, 1.0f};
    v.uv = {0.5f, 0.5f};

    v.pos = {-4.0f, -4.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {4.0f, -4.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 4.0f, 0.0f};
    m.vertices.push_back(v);

    m.tangents.resize(3, {1.0f, 0.0f, 0.0f});

    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    m.triangles.push_back(tri);
    m.materials.push_back(Material{});
    return m;
}

static Light make_key_light_z(vec3 color = {1.0f, 0.0f, 0.0f})
{
    Light l{};
    l.direction = {0.0f, 0.0f, 1.0f};
    l.color = color;
    return l;
}

// Count pixels with stored depth < +inf (i.e. something was drawn).
// One-shot: mutates the depth buffer (like was_drawn).
static int count_drawn_pixels(Framebuffer &fb)
{
    int n = 0;
    for (int y = 0; y < fb.height(); y++)
        for (int x = 0; x < fb.width(); x++)
            if (was_drawn(fb, x, y))
                n++;
    return n;
}

// Build an in-memory RGBA texture — same helper pattern as test_rasterize_texture.cpp.
static Texture make_solid_tex_rgba(int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    Texture t;
    t.width = w;
    t.height = h;
    t.pixels.resize(static_cast<size_t>(w * h * 4));
    for (int i = 0; i < w * h; i++)
    {
        t.pixels[static_cast<size_t>(i * 4 + 0)] = r;
        t.pixels[static_cast<size_t>(i * 4 + 1)] = g;
        t.pixels[static_cast<size_t>(i * 4 + 2)] = b;
        t.pixels[static_cast<size_t>(i * 4 + 3)] = 255;
    }
    return t;
}

// ─── Group A — constructor / lifecycle ───────────────────────────────────────

// A1: default construction and clean destruction (no hang or deadlock).
TEST(renderer, constructor_default_threads)
{
    Renderer r;
    ASSERT_TRUE(true); // reaching here means construction succeeded
}

// A2: extreme and edge thread counts all clamp cleanly.
TEST(renderer, constructor_thread_count_clamping)
{
    {
        Renderer r(0);
    } // all hardware threads
    {
        Renderer r(1);
    } // exactly 1
    {
        Renderer r(2);
    } // exactly 2
    {
        Renderer r(1000000);
    } // overflow → clamp to hw_concurrency
    ASSERT_TRUE(true);
}

// A3: cycle_shading transitions: Gouraud→Phong→Wireframe→Flat→Gouraud.
TEST(renderer, cycle_shading_full_loop)
{
    Renderer r;
    ASSERT_TRUE(r.mode == ShadingMode::Gouraud);
    r.cycle_shading();
    ASSERT_TRUE(r.mode == ShadingMode::Phong);
    r.cycle_shading();
    ASSERT_TRUE(r.mode == ShadingMode::Wireframe);
    r.cycle_shading();
    ASSERT_TRUE(r.mode == ShadingMode::Flat);
    r.cycle_shading();
    ASSERT_TRUE(r.mode == ShadingMode::Gouraud);
    // Also test from Wireframe as start.
    r.mode = ShadingMode::Wireframe;
    r.cycle_shading();
    ASSERT_TRUE(r.mode == ShadingMode::Flat);
}

// ─── Group B — wireframe path ─────────────────────────────────────────────────

// B1: front-facing triangle draws pixels in wireframe mode.
TEST(renderer, wireframe_visible_triangle_drawn)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, {0.0f, 0.0f, 0.0f}, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) > 0);
}

// B2: backface culled in wireframe mode → no pixels drawn.
TEST(renderer, wireframe_backface_culled)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    r.cull_backfaces = true;
    Mesh mesh = make_unit_triangle(/*flip_winding=*/true);
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, {0.0f, 0.0f, 0.0f}, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) == 0);
}

// B3: culling disabled → backface still renders.
TEST(renderer, wireframe_culling_disabled_renders_backface)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    r.cull_backfaces = false;
    Mesh mesh = make_unit_triangle(/*flip_winding=*/true);
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, {0.0f, 0.0f, 0.0f}, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) > 0);
}

// B4: wireframe_color is honoured — every drawn pixel must match.
TEST(renderer, wireframe_uses_wireframe_color)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    r.wireframe_color = {255, 0, 0}; // red
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, {0.0f, 0.0f, 0.0f}, fb);

    int drawn = 0;
    int wrong = 0;
    for (int y = 0; y < fb.height(); y++)
    {
        for (int x = 0; x < fb.width(); x++)
        {
            Color c = fb.get_pixel(x, y);
            if (c.r > 0 || c.g > 0 || c.b > 0)
            {
                drawn++;
                if (std::abs(static_cast<int>(c.r) - 255) > 2 ||
                    c.g > 2 || c.b > 2)
                    wrong++;
            }
        }
    }
    if (drawn == 0)
        ASSERT_FAIL("wireframe drew no pixels");
    if (wrong > 0)
        ASSERT_FAIL("wireframe_color not honoured: " + std::to_string(wrong) + " pixels wrong");
}

// ─── Group C — shading dispatch ───────────────────────────────────────────────
//
// Scene: front-facing unit triangle, red light from +Z, tiny ambient.
// Normal (0,0,1) · light (0,0,1) = 1 → full diffuse → R≥150 at centre pixel.

// C1: Flat shading produces a lit centre pixel.
TEST(renderer, flat_shading_renders_lit_pixel)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Flat;
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 0.0f, 0.0f});
    vec3 ambient{0.05f, 0.05f, 0.05f};
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
        ASSERT_FAIL("flat: R too low (" + std::to_string(static_cast<int>(c.r)) + ")");
    if (c.g > 30)
        ASSERT_FAIL("flat: G too high (" + std::to_string(static_cast<int>(c.g)) + ")");
    if (c.b > 30)
        ASSERT_FAIL("flat: B too high (" + std::to_string(static_cast<int>(c.b)) + ")");
}

// C2: Gouraud shading produces a lit centre pixel.
TEST(renderer, gouraud_shading_renders_lit_pixel)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Gouraud;
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 0.0f, 0.0f});
    vec3 ambient{0.05f, 0.05f, 0.05f};
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
        ASSERT_FAIL("gouraud: R too low (" + std::to_string(static_cast<int>(c.r)) + ")");
    if (c.g > 30)
        ASSERT_FAIL("gouraud: G too high (" + std::to_string(static_cast<int>(c.g)) + ")");
    if (c.b > 30)
        ASSERT_FAIL("gouraud: B too high (" + std::to_string(static_cast<int>(c.b)) + ")");
}

// C3: Phong shading produces a lit centre pixel.
TEST(renderer, phong_shading_renders_lit_pixel)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 0.0f, 0.0f});
    vec3 ambient{0.05f, 0.05f, 0.05f};
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
        ASSERT_FAIL("phong: R too low (" + std::to_string(static_cast<int>(c.r)) + ")");
    if (c.g > 30)
        ASSERT_FAIL("phong: G too high (" + std::to_string(static_cast<int>(c.g)) + ")");
    if (c.b > 30)
        ASSERT_FAIL("phong: B too high (" + std::to_string(static_cast<int>(c.b)) + ")");
}

// ─── Group D — backface culling in MT path ────────────────────────────────────

// D1: backface triangle is culled — no pixels drawn in Phong mode.
TEST(renderer, mt_backface_culled)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = true;
    Mesh mesh = make_unit_triangle(/*flip_winding=*/true);
    Camera cam = make_test_camera();
    Light light = make_key_light_z();
    vec3 ambient{0.05f, 0.05f, 0.05f};
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) == 0);
}

// D2: double-sided material bypasses culling — pixels drawn even for a backface.
TEST(renderer, mt_double_sided_renders_backface)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = true;
    // flip_winding=true (back face from camera) + double_sided=true.
    Mesh mesh = make_unit_triangle(/*flip_winding=*/true, /*double_sided=*/true);
    Camera cam = make_test_camera();
    // Ambient-only so color is non-zero even with flip_normals darkening diffuse.
    vec3 ambient{0.5f, 0.5f, 0.5f};
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);
    if (count_drawn_pixels(fb) == 0)
        ASSERT_FAIL("double-sided backface must not be culled");
}

// ─── Group E — MT correctness / multi-frame ───────────────────────────────────

// E1: empty mesh completes without hanging; framebuffer stays undrawn.
TEST(renderer, empty_mesh_completes)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh;
    mesh.materials.push_back(Material{}); // at least one material required
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, {0.0f, 0.0f, 0.0f}, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) == 0);
}

// E2: rendering the same scene twice gives matching pixels (deterministic).
TEST(renderer, repeated_render_deterministic)
{
    FdRedirect rd;
    Renderer r; // default thread count
    r.mode = ShadingMode::Gouraud;
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 0.0f, 0.0f});
    vec3 ambient{0.05f, 0.05f, 0.05f};

    Framebuffer fb1(40, 20), fb2(40, 20);
    fb1.clear();
    fb2.clear();
    r.render(mesh, cam, &light, 1, ambient, fb1);
    r.render(mesh, cam, &light, 1, ambient, fb2);

    // Centre pixel must be present in both and match within ±1 rounding.
    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    ASSERT_TRUE(was_drawn(fb2, 20, 10));
    assert_pixel_near(fb2, 20, 10, fb1.get_pixel(20, 10), 1);
}

// E3: large triangle spanning all 4 worker bands — pixels drawn in every band.
// Triangle sa≈(12,18), sb≈(28,18), sc≈(20,2) → covers y=2..18.
// With 4 workers on 40×20: bands are y=[0..4],[5..9],[10..14],[15..19].
// Checks: y=3 (band 0), y=7 (band 1), y=10 (band 2), y=17 (band 3).
TEST(renderer, large_triangle_spans_all_bands)
{
    FdRedirect rd;
    Renderer r(4);
    r.mode = ShadingMode::Gouraud;
    Mesh mesh = make_large_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 1.0f, 1.0f});
    vec3 ambient{0.2f, 0.2f, 0.2f};
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    if (!was_drawn(fb, 20, 3))
        ASSERT_FAIL("no pixel drawn at y=3 (band 0): band bucketing gap");
    if (!was_drawn(fb, 20, 7))
        ASSERT_FAIL("no pixel drawn at y=7 (band 1): band bucketing gap");
    if (!was_drawn(fb, 20, 10))
        ASSERT_FAIL("no pixel drawn at y=10 (band 2): band bucketing gap");
    if (!was_drawn(fb, 20, 17))
        ASSERT_FAIL("no pixel drawn at y=17 (band 3): band bucketing gap");
}

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
    v.normal = {0.0f, 0.0f, 1.0f};
    v.uv = {0.5f, 0.5f};

    v.pos = {-2.0f, -2.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {2.0f, -2.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 0.0f, 4.95f};
    m.vertices.push_back(v);

    m.tangents.resize(3, {1.0f, 0.0f, 0.0f});

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
    v.normal = {0.0f, 0.0f, 1.0f};
    v.uv = {0.5f, 0.5f};

    v.pos = {0.0f, -2.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {0.09f, 0.0f, 4.95f};
    m.vertices.push_back(v);
    v.pos = {-0.09f, 0.0f, 4.95f};
    m.vertices.push_back(v);

    m.tangents.resize(3, {1.0f, 0.0f, 0.0f});

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
    v.normal = {0.0f, 0.0f, 1.0f};
    v.uv = {0.5f, 0.5f};

    v.pos = {-1.0f, -1.0f, 4.95f};
    m.vertices.push_back(v);
    v.pos = {1.0f, -1.0f, 4.95f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 1.0f, 4.95f};
    m.vertices.push_back(v);

    m.tangents.resize(3, {1.0f, 0.0f, 0.0f});

    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    m.triangles.push_back(tri);
    m.materials.push_back(Material{});
    return m;
}

// ─── Group F — lights, shadow, texture toggle ─────────────────────────────────

// F1: n_lights=0 → ambient-only output; passing a non-null shadow_map must not crash.
TEST(renderer, zero_lights_renders_ambient_only)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    // Pure red ambient so we can distinguish it from black.
    vec3 ambient{0.5f, 0.0f, 0.0f};

    // Build a dummy shadow map (not used when n_lights=0).
    ShadowMap sm;
    sm.clear();

    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb, &sm);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    // ambient * mat.ambient * ao = (0.5,0,0) * (1,1,1) * 1 ≈ R=127.
    if (c.r < 80)
        ASSERT_FAIL("ambient-only R too low (" + std::to_string(static_cast<int>(c.r)) + ")");
    if (c.g > 20)
        ASSERT_FAIL("ambient-only G too high (" + std::to_string(static_cast<int>(c.g)) + ")");
    if (c.b > 20)
        ASSERT_FAIL("ambient-only B too high (" + std::to_string(static_cast<int>(c.b)) + ")");
}

// F2: show_texture toggle changes pixel colour.
// Mesh has a solid green diffuse texture. With show_texture=true the pixel is
// green; with show_texture=false the pixel is white (white light × white mat).
TEST(renderer, show_texture_toggle_changes_pixel)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Phong;

    Mesh mesh = make_unit_triangle();
    mesh.textures.push_back(make_solid_tex_rgba(2, 2, 0, 255, 0)); // solid green
    mesh.materials[0].diffuse_tex = 0;
    mesh.materials[0].diffuse = {1.0f, 1.0f, 1.0f};
    mesh.materials[0].ambient = {1.0f, 1.0f, 1.0f};
    mesh.materials[0].specular = {0.0f, 0.0f, 0.0f};

    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 1.0f, 1.0f}); // white light
    vec3 ambient{0.0f, 0.0f, 0.0f};                     // no ambient so tex colour is clear

    // Render with texture enabled.
    Framebuffer fb_tex(40, 20);
    fb_tex.clear();
    r.show_texture = true;
    r.render(mesh, cam, &light, 1, ambient, fb_tex);

    // Render with texture disabled.
    Framebuffer fb_notex(40, 20);
    fb_notex.clear();
    r.show_texture = false;
    r.render(mesh, cam, &light, 1, ambient, fb_notex);

    ASSERT_TRUE(was_drawn(fb_tex, 20, 10));
    ASSERT_TRUE(was_drawn(fb_notex, 20, 10));

    Color ct = fb_tex.get_pixel(20, 10);
    Color cn = fb_notex.get_pixel(20, 10);

    // With texture: diffuse*tex = (1,1,1)*(0,1,0) → green.
    if (ct.g < 200)
        ASSERT_FAIL("show_texture=true: G too low (" + std::to_string(static_cast<int>(ct.g)) + ")");
    if (ct.r > 60)
        ASSERT_FAIL("show_texture=true: R too high (" + std::to_string(static_cast<int>(ct.r)) + ")");

    // Without texture: diffuse (1,1,1) × white light → white.
    if (cn.r < 200)
        ASSERT_FAIL("show_texture=false: R too low (" + std::to_string(static_cast<int>(cn.r)) + ")");
    if (cn.g < 200)
        ASSERT_FAIL("show_texture=false: G too low (" + std::to_string(static_cast<int>(cn.g)) + ")");
}

// ─── Group G — near-plane clip integration ────────────────────────────────────
//
// Camera: eye=(0,0,5), near_plane=0.1. Clip w = 5 − world_z.
// Vertices at z=4.95 have w=0.05 → behind near plane (w < 0.1).
// All tests verify that Renderer::render() forwards clip_near results correctly
// for both the MT (Gouraud/Phong) and wireframe code paths.

// G1: one vertex behind near plane, two in front.
// clip_near produces 2 output tris → pixels must be drawn (MT path).
TEST(renderer, near_clip_one_vertex_behind_renders)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Gouraud;
    Mesh mesh = make_straddling_triangle_one_behind();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 1.0f, 1.0f});
    vec3 ambient{0.1f, 0.1f, 0.1f};
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    if (count_drawn_pixels(fb) == 0)
        ASSERT_FAIL("one-vertex-behind triangle: clip_near must produce visible output");
}

// G2: two vertices behind near plane, one in front.
// clip_near produces 1 output tri → pixels must be drawn (MT path).
TEST(renderer, near_clip_two_vertices_behind_renders)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Gouraud;
    Mesh mesh = make_straddling_triangle_two_behind();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 1.0f, 1.0f});
    vec3 ambient{0.1f, 0.1f, 0.1f};
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    if (count_drawn_pixels(fb) == 0)
        ASSERT_FAIL("two-vertices-behind triangle: clip_near must produce visible output");
}

// G3: all three vertices behind near plane → clip_near returns 0 → no pixels (MT path).
TEST(renderer, near_clip_fully_behind_draws_nothing)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Gouraud;
    Mesh mesh = make_fully_behind_triangle();
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, {0.1f, 0.1f, 0.1f}, fb);
    if (count_drawn_pixels(fb) != 0)
        ASSERT_FAIL("fully-behind triangle must not produce any pixels");
}

// G4: wireframe path with a straddling triangle → clip_near fires → pixels drawn.
// Covers the separate clip_near call in the wireframe branch (renderer.cpp:430).
TEST(renderer, near_clip_wireframe_straddling_renders)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    Mesh mesh = make_straddling_triangle_one_behind();
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, {0.0f, 0.0f, 0.0f}, fb);
    if (count_drawn_pixels(fb) == 0)
        ASSERT_FAIL("wireframe: straddling triangle must draw clipped edges");
}

// G5: wireframe path with all vertices behind → no pixels.
TEST(renderer, near_clip_wireframe_fully_behind_draws_nothing)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    Mesh mesh = make_fully_behind_triangle();
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, {0.0f, 0.0f, 0.0f}, fb);
    if (count_drawn_pixels(fb) != 0)
        ASSERT_FAIL("wireframe: fully-behind triangle must produce no pixels");
}

// G6: raising camera.near_plane above all clip-w values rejects a front-facing
// triangle that would otherwise render. Verifies m_near_plane is forwarded from
// camera.near_plane in the MT dispatch path (renderer.cpp:480).
TEST(renderer, near_clip_uses_camera_near_plane_value)
{
    FdRedirect rd;
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_unit_triangle(); // all vertices at z=0, clip w=5
    Camera cam = make_test_camera();
    cam.near_plane = 10.0f; // all w=5 < 10 → clip_near returns 0 for every tri
    Light light = make_key_light_z({1.0f, 1.0f, 1.0f});
    vec3 ambient{0.1f, 0.1f, 0.1f};
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    if (count_drawn_pixels(fb) != 0)
        ASSERT_FAIL("near_plane=10 must reject all vertices (clip w=5 < near_plane)");
}
