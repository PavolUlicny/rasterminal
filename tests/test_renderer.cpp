#include "renderer_test_util.h"

#include <cmath>

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

// ─── Group J: double-sided lighting correctness ───────────────────────────────
// Existing D2 only checks pixels-drawn (ambient-only). These tests exercise the
// three normal-flip code paths under a directional light so that a dropped or
// misplaced flip_normals branch causes an actual failure.

// CW-from-+z winding → back-face from camera at +z.
// Vertex normals (0,0,-1) match the back winding:
//   without flip: dot(n=(0,0,-1), light=(0,0,1)) = -1 → no diffuse
//   with    flip: dot(n=(0,0, 1), light=(0,0,1)) = +1 → full diffuse
static Mesh make_back_facing_double_sided_triangle()
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = {0.0f, 0.0f, -1.0f};
    v.uv = {0.5f, 0.5f};

    v.pos = {-1.0f, -1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {0.0f, 1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {1.0f, -1.0f, 0.0f};
    m.vertices.push_back(v);

    m.tangents.resize(3, {1.0f, 0.0f, 0.0f});

    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1; // CW from +z → back-face; cross(vb-va,vc-va)=(0,0,-4)
    tri.v[2] = 2;
    tri.material_idx = 0;
    m.triangles.push_back(tri);

    Material mat;
    mat.double_sided = true;
    m.materials.push_back(mat);
    m.has_double_sided = true;
    return m;
}

// Same geometry, single-sided (for cull-off negative tests).
static Mesh make_back_facing_single_sided_triangle()
{
    Mesh m = make_back_facing_double_sided_triangle();
    m.materials[0].double_sided = false;
    m.has_double_sided = false;
    return m;
}

// Two non-overlapping back-facing triangles with different materials:
//   tri 0 (double-sided)  at world x∈[-3,-1] → centroid screen (16,10)
//   tri 1 (single-sided)  at world x∈[1, 3]  → centroid screen (24,10)
// Both CW from +z with vertex normals (0,0,-1).
static Mesh make_two_back_face_mixed_mesh()
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = {0.0f, 0.0f, -1.0f};
    v.uv = {0.5f, 0.5f};

    v.pos = {-3.0f, -1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {-2.0f, 1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {-1.0f, -1.0f, 0.0f};
    m.vertices.push_back(v);

    v.pos = {1.0f, -1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {2.0f, 1.0f, 0.0f};
    m.vertices.push_back(v);
    v.pos = {3.0f, -1.0f, 0.0f};
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
    tri.material_idx = 1;
    m.triangles.push_back(tri);

    Material mat0;
    mat0.double_sided = true;
    m.materials.push_back(mat0);
    m.materials.push_back(Material{}); // single-sided default

    m.has_double_sided = true;
    return m;
}

// J1: Phong — back-face double-sided triangle lit by a +z light.
// Vertex normals are (0,0,-1); the flip must turn them to (0,0,1) so diffuse fires.
TEST(renderer, phong_double_sided_back_face_lit_correctly)
{
    Mesh mesh = make_back_facing_double_sided_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 0.0f, 0.0f});
    vec3 ambient{0.0f, 0.0f, 0.0f};

    Renderer r;
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = true;
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
        ASSERT_FAIL("Phong double-sided back-face R too low (" + std::to_string(static_cast<int>(c.r)) + ") — normal flip not applied");
}

// J2: Gouraud — same scene.
TEST(renderer, gouraud_double_sided_back_face_lit_correctly)
{
    Mesh mesh = make_back_facing_double_sided_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 0.0f, 0.0f});
    vec3 ambient{0.0f, 0.0f, 0.0f};

    Renderer r;
    r.mode = ShadingMode::Gouraud;
    r.cull_backfaces = true;
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
        ASSERT_FAIL("Gouraud double-sided back-face R too low (" + std::to_string(static_cast<int>(c.r)) + ") — normal flip not applied");
}

// J3: Flat — same scene; covers the separate face-normal negation branch.
TEST(renderer, flat_double_sided_back_face_lit_correctly)
{
    Mesh mesh = make_back_facing_double_sided_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 0.0f, 0.0f});
    vec3 ambient{0.0f, 0.0f, 0.0f};

    Renderer r;
    r.mode = ShadingMode::Flat;
    r.cull_backfaces = true;
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
        ASSERT_FAIL("Flat double-sided back-face R too low (" + std::to_string(static_cast<int>(c.r)) + ") — face normal flip not applied");
}

// J4: cull off, single-sided back-face → drawn but dark.
// cull_backfaces=false → do_cull=false → if(do_cull) block skipped → flip_normals stays false.
// Normal remains (0,0,-1); dot(n, light=(0,0,1))=-1 → no diffuse → R=0.
TEST(renderer, single_sided_cull_off_back_face_dark)
{
    Mesh mesh = make_back_facing_single_sided_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 0.0f, 0.0f});
    vec3 ambient{0.0f, 0.0f, 0.0f};

    Renderer r;
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = false;
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 5)
        ASSERT_FAIL("cull-off single-sided back-face R too high (" + std::to_string(static_cast<int>(c.r)) + ") — flip applied when it should not be");
}

// J5: cull off, double-sided back-face → also drawn but dark.
// do_cull=false → flip_normals never set regardless of material.double_sided.
TEST(renderer, double_sided_cull_off_back_face_dark)
{
    Mesh mesh = make_back_facing_double_sided_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 0.0f, 0.0f});
    vec3 ambient{0.0f, 0.0f, 0.0f};

    Renderer r;
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = false;
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 5)
        ASSERT_FAIL("cull-off double-sided back-face R too high (" + std::to_string(static_cast<int>(c.r)) + ") — flip applied when cull is off");
}

// J6: front-facing double-sided triangle — flip must NOT fire.
// make_unit_triangle(false, true): normals (0,0,1), CCW → front-face cull passes,
// flip_normals stays false, dot(n=(0,0,1), light=(0,0,1))=1 → full diffuse.
TEST(renderer, double_sided_front_face_lit_normally)
{
    Mesh mesh = make_unit_triangle(false, true);
    Camera cam = make_test_camera();
    Light light = make_key_light_z({1.0f, 0.0f, 0.0f});
    vec3 ambient{0.0f, 0.0f, 0.0f};

    Renderer r;
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = true;
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
        ASSERT_FAIL("front-facing double-sided R too low (" + std::to_string(static_cast<int>(c.r)) + ") — flip incorrectly applied to front face");
}

// J7: mixed mesh — only the double-sided back-face triangle is drawn.
// Tri 0 (double-sided) centre → screen (16,10); tri 1 (single-sided) centre → screen (24,10).
// Uses ambient (0.5,0,0) and no light so drawn = ambient red; not-drawn = depth=inf.
TEST(renderer, mixed_mesh_only_double_sided_back_face_drawn)
{
    Mesh mesh = make_two_back_face_mixed_mesh();
    Camera cam = make_test_camera();
    vec3 ambient{0.5f, 0.0f, 0.0f};

    Renderer r;
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = true;
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);

    // Double-sided triangle should be drawn with ambient red.
    ASSERT_TRUE(was_drawn(fb, 16, 10));
    Color c_ds = fb.get_pixel(16, 10);
    if (c_ds.r < 80)
        ASSERT_FAIL("double-sided back-face not lit (R=" + std::to_string(static_cast<int>(c_ds.r)) + ") — may have been culled");

    // Single-sided triangle must be culled — pixel stays undrawn.
    if (was_drawn(fb, 24, 10))
        ASSERT_FAIL("single-sided back-face was drawn — per-material check bypassed");
}

// J8: wireframe double-sided back-face — covers the wireframe path's separate cull bypass.
TEST(renderer, wireframe_double_sided_back_face_drawn)
{
    Mesh mesh = make_back_facing_double_sided_triangle();
    Camera cam = make_test_camera();
    vec3 ambient{0.5f, 0.5f, 0.5f};

    Renderer r;
    r.mode = ShadingMode::Wireframe;
    r.cull_backfaces = true;
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);

    if (count_drawn_pixels(fb) == 0)
        ASSERT_FAIL("wireframe double-sided back-face drew no pixels — cull bypass missing");
}

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
    Framebuffer fb_lit(40, 20);
    fb_lit.clear();
    r.render(receiver, cam, &light, 1, ambient, fb_lit);
    ASSERT_TRUE(was_drawn(fb_lit, 20, 10));
    Color c_lit = fb_lit.get_pixel(20, 10);
    if (c_lit.r < 150)
        ASSERT_FAIL("Phong lit baseline R too low (" + std::to_string(static_cast<int>(c_lit.r)) + ")");

    // Shadow map built from full scene (receiver+occluder) so frustum covers both.
    ShadowMap sm = build_shadow_map(make_shadow_scene(), light);
    Framebuffer fb_shadow(40, 20);
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

    Framebuffer fb_lit(40, 20);
    fb_lit.clear();
    r.render(receiver, cam, &light, 1, ambient, fb_lit);
    ASSERT_TRUE(was_drawn(fb_lit, 20, 10));
    Color c_lit = fb_lit.get_pixel(20, 10);
    if (c_lit.r < 150)
        ASSERT_FAIL("Flat lit baseline R too low (" + std::to_string(static_cast<int>(c_lit.r)) + ")");

    ShadowMap sm = build_shadow_map(make_shadow_scene(), light);
    Framebuffer fb_shadow(40, 20);
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

    Framebuffer fb_lit(40, 20);
    fb_lit.clear();
    r.render(receiver, cam, &light, 1, ambient, fb_lit);
    ASSERT_TRUE(was_drawn(fb_lit, 20, 10));
    Color c_lit = fb_lit.get_pixel(20, 10);
    if (c_lit.r < 150)
        ASSERT_FAIL("Gouraud lit baseline R too low (" + std::to_string(static_cast<int>(c_lit.r)) + ")");

    ShadowMap sm = build_shadow_map(make_shadow_scene(), light);
    Framebuffer fb_shadow(40, 20);
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
    Framebuffer fb(40, 20);
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
    Framebuffer fb_sm(40, 20);
    fb_sm.clear();
    r.render(receiver, cam, &light, 0, ambient, fb_sm, &sm);

    // n_lights=0 with nullptr — baseline.
    Framebuffer fb_null(40, 20);
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
    Framebuffer fb(40, 20);
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
    Framebuffer fb(40, 20);
    fb.clear();
    r.render(receiver, cam, lights, 2, ambient, fb, &sm);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 60)
        ASSERT_FAIL("Gouraud multi-light: key (red) not shadowed, R=" + std::to_string(static_cast<int>(c.r)));
    if (c.g < 150)
        ASSERT_FAIL("Gouraud multi-light: fill (green) incorrectly shadowed, G=" + std::to_string(static_cast<int>(c.g)));
}
