#include "tests/renderer_test_util.h"

// Group M: renderer vertex-color dispatch
// Cover ClipVert loading, Phong interpolation, Flat averaging, white fallback,
// and wireframe independence. A white material and frontal light isolate the tint:
// red vertex color yields red, while absent color yields white.

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

TEST(renderer, phong_vcol_flag_false_no_tint)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
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

// Two red vertices make red dominant at the centre without collapsing interpolation.
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

// White hits face_vcol's early skip and must match the no-colour path.
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

TEST(renderer, wireframe_vcol_does_not_affect_output)
{
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    Mesh mesh = make_vcol_triangle({ 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f });
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) > 0);
}
