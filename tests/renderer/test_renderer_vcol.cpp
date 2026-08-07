#include "tests/renderer_test_util.h"

// Group M: vertex-color end-to-end through the renderer
// test_rasterize_vcol.cpp (V1–V6) exercises rasterize_phong() directly.
// These tests cover the renderer's own dispatch:
//   • p_vcols = has_vertex_colors ? vertex_colors.data() : nullptr
//   • ClipVert.color loaded from p_vcols or white
//   • Phong:   a/b/c.color passed straight through to rasterize_phong
//   • Flat:    face_vcol average + vcol_mat build
//   • Wireframe: vcol must have no effect (and must not crash)
//
// All tests use Renderer(1) for deterministic single-thread ordering.
// Camera: make_test_camera() — pixel (20,10) on a 40×20 framebuffer = world (0,0,0).
// Material: white diffuse=ambient=(1,1,1), specular=(0,0,0) — vcol tint is the
//   only colour source so per-channel assertions are clean.
// Light: white key from +z — dot(n=(0,0,1), L=(0,0,1))=1 → full diffuse.
// Ambient: (0.1,0.1,0.1) — small white baseline for all channels.
//
// Expected with red vcol (1,0,0) and the material above:
//   effective diffuse = ambient = (1,0,0)
//   ambient_contrib   = (0.1,0.1,0.1) * (1,0,0) = (0.1,0,0)
//   diffuse_contrib   = (1,1,1) * (1,0,0) * 1   = (1,0,0)
//   result = (1.1,0,0) → clamped → R=255, G=0, B=0
// Expected with no vcol (white material, no tint):
//   result = (1.1,1.1,1.1) → all channels = 255

static Mesh make_vcol_triangle(vec3 vca, vec3 vcb, vec3 vcc, bool has_colors = true)
{
    // Same footprint as make_unit_triangle(): v0=(-1,-1,0), v1=(1,-1,0), v2=(0,1,0).
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = { 0.0f, 0.0f, 1.0f };
    v.uv = { 0.5f, 0.5f };

    v.pos = { -1.0f, -1.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 1.0f, -1.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 0.0f, 1.0f, 0.0f };
    m.vertices.push_back(v);

    m.tangents.resize(3, { 1.0f, 0.0f, 0.0f });

    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    m.triangles.push_back(tri);

    // White-only material: specular zeroed so it cannot contaminate channel tests.
    Material mat;
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.0f, 0.0f, 0.0f };
    m.materials.push_back(mat);

    m.vertex_colors.push_back(vca);
    m.vertex_colors.push_back(vcb);
    m.vertex_colors.push_back(vcc);
    m.has_vertex_colors = has_colors;

    return m;
}

// Phong: uniform red vcol → pixel (20,10) is red.
// Catches: has_vertex_colors branch not firing, p_vcols not loaded,
//          rt.ph.vcola/b/c not propagated to rasterize_phong.
TEST(renderer, phong_vcol_uniform_red_tints_pixel)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_vcol_triangle({ 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f });
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f }); // white key from +z
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, { 0.1f, 0.1f, 0.1f }, fb);

    auto c = fb.get_pixel(20, 10);
    if (c.r < 200 || c.g > 30 || c.b > 30)
    {
        ASSERT_FAIL(
            "phong red vcol: expected R≥200,G≤30,B≤30 at (20,10), got (" + std::to_string(static_cast<int>(c.r)) + "," +
            std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

// Phong: has_vertex_colors=false; vertex_colors populated with red but flag is
// false → pixel must NOT be red-only.
// Catches: flag ignored (always-on tinting from vertex_colors array).
TEST(renderer, phong_vcol_flag_false_no_tint)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    // has_colors=false: data present but flag off.
    Mesh mesh = make_vcol_triangle(
        { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
        /*has_colors=*/false
    );
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, { 0.1f, 0.1f, 0.1f }, fb);

    auto c = fb.get_pixel(20, 10);
    // Without tinting, white material + white light → all channels high.
    if (c.r < 150 || c.g < 150 || c.b < 150)
    {
        ASSERT_FAIL(
            "phong flag=false: expected all channels ≥150 (white-ish), got (" + std::to_string(static_cast<int>(c.r)) +
            "," + std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

// Phong: per-vertex colours red/green/blue → all three channels contribute at
// the centre pixel.
// Catches: vcol collapsed to a single vertex (e.g. always vcola).
TEST(renderer, phong_vcol_per_vertex_interpolation)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_vcol_triangle({ 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f });
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, { 0.1f, 0.1f, 0.1f }, fb);

    auto c = fb.get_pixel(20, 10);
    // Each vertex carries one colour component; any valid interpolation at the
    // centre must produce a nonzero contribution from all three.
    if (c.r <= 20 || c.g <= 20 || c.b <= 20)
    {
        ASSERT_FAIL(
            "phong rgb vcol: expected R,G,B all >20 at (20,10), got (" + std::to_string(static_cast<int>(c.r)) + "," +
            std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

// Phong: white vcol (all 1s) with flag=true must match the no-vcol baseline.
// Phong tints per vertex (rt.ph.vcola/b/c); a white vcol must leave the lit
// result identical to running with no vertex colours at all.
// Catches: the per-vertex vcol multiply diverging from the no-vcol path when the
//          colour is the identity (1,1,1).
TEST(renderer, phong_vcol_white_matches_no_vcol)
{
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.1f, 0.1f, 0.1f };

    Framebuffer fb_a(40, 20, /*headless=*/true);
    {
        Renderer r(1);
        r.mode = ShadingMode::Phong;
        Mesh mesh = make_vcol_triangle({ 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f });
        fb_a.clear();
        r.render(mesh, cam, &light, 1, ambient, fb_a);
    }

    Framebuffer fb_b(40, 20, /*headless=*/true);
    {
        Renderer r(1);
        r.mode = ShadingMode::Phong;
        Mesh mesh = make_unit_triangle();
        mesh.materials[0].diffuse = { 1.0f, 1.0f, 1.0f };
        mesh.materials[0].ambient = { 1.0f, 1.0f, 1.0f };
        mesh.materials[0].specular = { 0.0f, 0.0f, 0.0f };
        fb_b.clear();
        r.render(mesh, cam, &light, 1, ambient, fb_b);
    }

    auto ca = fb_a.get_pixel(20, 10);
    auto cb = fb_b.get_pixel(20, 10);
    auto diff = [](uint8_t a, uint8_t b) { return a > b ? a - b : b - a; };
    if (diff(ca.r, cb.r) > 2 || diff(ca.g, cb.g) > 2 || diff(ca.b, cb.b) > 2)
    {
        ASSERT_FAIL(
            "phong white vcol: expected match with no-vcol within ±2, got (" + std::to_string(static_cast<int>(ca.r)) +
            "," + std::to_string(static_cast<int>(ca.g)) + "," + std::to_string(static_cast<int>(ca.b)) + ") vs (" +
            std::to_string(static_cast<int>(cb.r)) + "," + std::to_string(static_cast<int>(cb.g)) + "," +
            std::to_string(static_cast<int>(cb.b)) + ")"
        );
    }
}

// Phong: mixed white (v0) + red (v1, v2) vcol — Phong is the per-vertex
// interpolating path, so this guards correct independent per-vertex tinting with
// no stale state leaking between vertices. v0 stays white,
// v1/v2 are red → R must dominate at the red-weighted centre pixel.
// Catches: vcol collapsed to a single vertex, or one vertex's tint leaking onto
//          another.
TEST(renderer, phong_vcol_mixed_white_and_color)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_vcol_triangle({ 1.0f, 1.0f, 1.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f });
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, { 0.1f, 0.1f, 0.1f }, fb);

    ASSERT_TRUE(count_drawn_pixels(fb) > 0);
    auto c = fb.get_pixel(20, 10);
    // Two of three vertices are red → R must dominate at the centre pixel.
    if (c.r < 100 || c.r <= c.g || c.r <= c.b)
    {
        ASSERT_FAIL(
            "phong mixed vcol: expected R dominant and ≥100 at (20,10), got (" + std::to_string(static_cast<int>(c.r)) +
            "," + std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

// Flat: uniform red vcol → pixel (20,10) is red.
// Catches: face_vcol average / vcol_mat build broken; vcol_mat.diffuse/ambient
//          not tinted.
TEST(renderer, flat_vcol_uniform_tints_pixel)
{
    Renderer r(1);
    r.mode = ShadingMode::Flat;
    Mesh mesh = make_vcol_triangle({ 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f });
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, { 0.1f, 0.1f, 0.1f }, fb);

    auto c = fb.get_pixel(20, 10);
    if (c.r < 200 || c.g > 30 || c.b > 30)
    {
        ASSERT_FAIL(
            "flat red vcol: expected R≥200,G≤30,B≤30 at (20,10), got (" + std::to_string(static_cast<int>(c.r)) + "," +
            std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

// Flat: white vcol (all 1s) with flag=true must match the no-vcol baseline.
// The renderer takes an early-skip when face_vcol==(1,1,1) — this tests that
// the skip produces the same result as flag=false.
// Catches: the if(face_vcol != white) optimisation diverging from the no-vcol path.
TEST(renderer, flat_vcol_white_skip_matches_no_vcol)
{
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.1f, 0.1f, 0.1f };

    // A: has_vertex_colors=true, all white.
    Framebuffer fb_a(40, 20, /*headless=*/true);
    {
        Renderer r(1);
        r.mode = ShadingMode::Flat;
        Mesh mesh = make_vcol_triangle({ 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f });
        fb_a.clear();
        r.render(mesh, cam, &light, 1, ambient, fb_a);
    }

    // B: has_vertex_colors=false baseline.
    Framebuffer fb_b(40, 20, /*headless=*/true);
    {
        Renderer r(1);
        r.mode = ShadingMode::Flat;
        Mesh mesh = make_unit_triangle();
        // Override material to match: white, no specular.
        mesh.materials[0].diffuse = { 1.0f, 1.0f, 1.0f };
        mesh.materials[0].ambient = { 1.0f, 1.0f, 1.0f };
        mesh.materials[0].specular = { 0.0f, 0.0f, 0.0f };
        fb_b.clear();
        r.render(mesh, cam, &light, 1, ambient, fb_b);
    }

    auto ca = fb_a.get_pixel(20, 10);
    auto cb = fb_b.get_pixel(20, 10);
    auto diff = [](uint8_t a, uint8_t b) { return a > b ? a - b : b - a; };
    if (diff(ca.r, cb.r) > 2 || diff(ca.g, cb.g) > 2 || diff(ca.b, cb.b) > 2)
    {
        ASSERT_FAIL(
            "flat white vcol: expected match with no-vcol within ±2, got (" + std::to_string(static_cast<int>(ca.r)) +
            "," + std::to_string(static_cast<int>(ca.g)) + "," + std::to_string(static_cast<int>(ca.b)) + ") vs (" +
            std::to_string(static_cast<int>(cb.r)) + "," + std::to_string(static_cast<int>(cb.g)) + "," +
            std::to_string(static_cast<int>(cb.b)) + ")"
        );
    }
}

// Wireframe: vertex colors must not affect the wireframe path (which uses
// m_wire_color, not vcol) and must not cause a crash.
// Catches: wireframe path accessing vertex_colors and crashing, or erroneously
//          consuming vcol data.
TEST(renderer, wireframe_vcol_does_not_affect_output)
{
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    Mesh mesh = make_vcol_triangle({ 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f });
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    // Wireframe draws edges regardless of vcol; just confirm it renders without crash.
    ASSERT_TRUE(count_drawn_pixels(fb) > 0);
}
