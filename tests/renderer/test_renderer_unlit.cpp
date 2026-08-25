#include "tests/renderer_test_util.h"

#include <utility>

// Unlit output is baseColor * texture * vertex color. The unit triangle covers pixel (20,10).

namespace
{

    Mesh make_unlit_triangle(vec3 diffuse)
    {
        Mesh m = make_unit_triangle();
        m.materials[0].unlit = true;
        m.materials[0].diffuse = diffuse;
        m.has_unlit = true;
        return m;
    }

    // Disjoint unlit-blue and lit-red triangles exercise the per-triangle material gate.
    Mesh make_mixed_mesh()
    {
        Mesh m;
        Vertex v{};
        v.ao = 1.0f;
        v.normal = { 0.0f, 0.0f, 1.0f };
        v.uv = { 0.5f, 0.5f };

        v.pos = { -4.0f, -2.0f, 0.0f };
        m.vertices.push_back(v);
        v.pos = { -1.0f, -2.0f, 0.0f };
        m.vertices.push_back(v);
        v.pos = { -2.5f, 2.0f, 0.0f };
        m.vertices.push_back(v);

        v.pos = { 1.0f, -2.0f, 0.0f };
        m.vertices.push_back(v);
        v.pos = { 4.0f, -2.0f, 0.0f };
        m.vertices.push_back(v);
        v.pos = { 2.5f, 2.0f, 0.0f };
        m.vertices.push_back(v);

        m.tangents.resize(6, { 1.0f, 0.0f, 0.0f });

        Triangle ta{};
        ta.v[0] = 0;
        ta.v[1] = 1;
        ta.v[2] = 2;
        ta.material_idx = 0;
        m.triangles.push_back(ta);

        Triangle tb{};
        tb.v[0] = 3;
        tb.v[1] = 4;
        tb.v[2] = 5;
        tb.material_idx = 1;
        m.triangles.push_back(tb);

        Material unlit;
        unlit.unlit = true;
        unlit.diffuse = { 0.0f, 0.0f, 0.8f };
        m.materials.push_back(unlit);

        Material lit;
        lit.diffuse = { 0.8f, 0.0f, 0.0f };
        m.materials.push_back(lit);

        m.has_unlit = true;
        return m;
    }

} // namespace

TEST(renderer_unlit, outputs_base_color_ignoring_light)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_unlit_triangle({ 0.2f, 0.4f, 0.6f });
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, { 0.3f, 0.3f, 0.3f }, fb);
    assert_pixel_near(fb, 20, 10, Color{ 51, 102, 153 }, 1);
}

TEST(renderer_unlit, independent_of_light)
{
    Camera cam = make_test_camera();
    Renderer r(1);
    r.mode = ShadingMode::Phong;

    Mesh mesh = make_unlit_triangle({ 0.2f, 0.4f, 0.6f });

    Light l1;
    l1.direction = { 0.0f, 0.0f, 1.0f };
    l1.color = { 1.0f, 0.0f, 0.0f };
    Framebuffer fb1(40, 20, /*headless=*/true);
    fb1.clear();
    r.render(mesh, cam, &l1, 1, { 0.5f, 0.5f, 0.5f }, fb1);

    Light l2;
    l2.direction = { 1.0f, 0.0f, 0.0f };
    l2.color = { 0.0f, 1.0f, 0.0f };
    Framebuffer fb2(40, 20, /*headless=*/true);
    fb2.clear();
    r.render(mesh, cam, &l2, 1, { 0.1f, 0.1f, 0.1f }, fb2);

    assert_pixel_near(fb1, 20, 10, Color{ 51, 102, 153 }, 1);
    assert_pixel_near(fb2, 20, 10, Color{ 51, 102, 153 }, 1);

    // A lit control prevents the result from passing independently of material handling.
    Mesh lit = make_unit_triangle();
    lit.materials[0].diffuse = { 0.2f, 0.4f, 0.6f };
    Framebuffer fb3(40, 20, /*headless=*/true);
    fb3.clear();
    r.render(lit, cam, &l1, 1, { 0.0f, 0.0f, 0.0f }, fb3);
    Color c3 = fb3.get_pixel(20, 10);
    ASSERT_TRUE(c3.r != 51 || c3.g != 102 || c3.b != 153);
}

TEST(renderer_unlit, honored_in_flat_and_phong)
{
    Camera cam = make_test_camera();
    Light light = make_key_light_z();
    for (ShadingMode m : { ShadingMode::Flat, ShadingMode::Phong })
    {
        Renderer r(1);
        r.mode = m;
        Mesh mesh = make_unlit_triangle({ 0.2f, 0.4f, 0.6f });
        Framebuffer fb(40, 20, /*headless=*/true);
        fb.clear();
        r.render(mesh, cam, &light, 1, { 0.4f, 0.4f, 0.4f }, fb);
        assert_pixel_near(fb, 20, 10, Color{ 51, 102, 153 }, 1);
    }
}

TEST(renderer_unlit, modulates_diffuse_texture)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    r.show_texture = true;
    Camera cam = make_test_camera();
    Light light = make_key_light_z();

    Mesh white = make_unlit_triangle({ 1.0f, 1.0f, 1.0f });
    white.textures.push_back(make_solid_tex_rgba(2, 2, 0, 255, 0)); // green
    white.materials[0].diffuse_map.tex = 0;
    Framebuffer fb1(40, 20, /*headless=*/true);
    fb1.clear();
    r.render(white, cam, &light, 1, { 0.0f, 0.0f, 0.0f }, fb1);
    assert_pixel_near(fb1, 20, 10, Color{ 0, 255, 0 }, 2);

    Mesh grey = make_unlit_triangle({ 0.5f, 0.5f, 0.5f });
    grey.textures.push_back(make_solid_tex_rgba(2, 2, 0, 255, 0));
    grey.materials[0].diffuse_map.tex = 0;
    Framebuffer fb2(40, 20, /*headless=*/true);
    fb2.clear();
    r.render(grey, cam, &light, 1, { 0.0f, 0.0f, 0.0f }, fb2);
    assert_pixel_near(fb2, 20, 10, Color{ 0, 127, 0 }, 2);
}

TEST(renderer_unlit, modulates_vertex_color)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Camera cam = make_test_camera();
    Light light = make_key_light_z();
    Mesh mesh = make_unlit_triangle({ 1.0f, 1.0f, 1.0f });
    mesh.has_vertex_colors = true;
    mesh.vertex_colors = { { 0.5f, 0.25f, 0.75f }, { 0.5f, 0.25f, 0.75f }, { 0.5f, 0.25f, 0.75f } };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, { 0.0f, 0.0f, 0.0f }, fb);
    assert_pixel_near(fb, 20, 10, Color{ 127, 63, 191 }, 2);
}

TEST(renderer_unlit, ignores_emissive)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Camera cam = make_test_camera();
    Light light = make_key_light_z();
    Mesh mesh = make_unlit_triangle({ 0.0f, 0.0f, 0.5f }); // blue
    mesh.materials[0].emissive = { 1.0f, 0.0f, 0.0f };     // red emissive
    mesh.has_emissive = true;
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, { 0.0f, 0.0f, 0.0f }, fb);
    assert_pixel_near(fb, 20, 10, Color{ 0, 0, 127 }, 2);
}

TEST(renderer_unlit, alpha_cutout_discards)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    r.show_texture = true;
    Camera cam = make_test_camera();
    Light light = make_key_light_z();
    Mesh mesh = make_unlit_triangle({ 1.0f, 1.0f, 1.0f });
    Texture t = make_solid_tex_rgba(2, 2, 255, 255, 255);
    for (size_t i = 3; i < t.pixels.size(); i += 4)
    {
        t.pixels[i] = 0; // alpha 0 → below cutoff everywhere
    }
    mesh.textures.push_back(std::move(t));
    mesh.materials[0].diffuse_map.tex = 0;
    mesh.materials[0].alpha_cutoff = 0.5f;
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, { 0.0f, 0.0f, 0.0f }, fb);
    ASSERT_FALSE(was_drawn(fb, 20, 10));
}

TEST(renderer_unlit, mixed_unlit_and_lit)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Camera cam = make_test_camera();
    Mesh mesh = make_mixed_mesh();
    Light light;
    light.direction = { 0.0f, 0.0f, 1.0f };
    light.color = { 1.0f, 1.0f, 1.0f };
    // Strong ambient distinguishes the lit triangle from the unlit one.
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, { 0.5f, 0.5f, 0.5f }, fb);
    assert_pixel_near(fb, 15, 10, Color{ 0, 0, 204 }, 2);
    Color lit = fb.get_pixel(25, 10);
    ASSERT_TRUE(lit.g > 50);
}

TEST(renderer_unlit, no_effect_in_wireframe)
{
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    Camera cam = make_test_camera();
    Mesh mesh = make_unlit_triangle({ 0.2f, 0.4f, 0.6f });
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) > 0);
}
