#include "tests/renderer_test_util.h"

#include <cmath>

// Group A — constructor / lifecycle

// default construction and clean destruction (no hang or deadlock).
TEST(renderer, constructor_default_threads)
{
    Renderer r;
    ASSERT_TRUE(true); // reaching here means construction succeeded
}

// default shading mode is Phong.
// Pins the constructor default so a future reorder of the ShadingMode enum or a
// stray re-default can't silently change what an unconfigured Renderer renders.
TEST(renderer, default_mode_is_phong)
{
    Renderer r;
    ASSERT_TRUE(r.mode == ShadingMode::Phong);
}

// extreme and edge thread counts all clamp cleanly.
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

// Group B — wireframe path

// front-facing triangle draws pixels in wireframe mode.
TEST(renderer, wireframe_visible_triangle_drawn)
{
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) > 0);
}

// backface culled in wireframe mode → no pixels drawn.
TEST(renderer, wireframe_backface_culled)
{
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    r.cull_backfaces = true;
    Mesh mesh = make_unit_triangle(/*flip_winding=*/true);
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) == 0);
}

// culling disabled → backface still renders.
TEST(renderer, wireframe_culling_disabled_renders_backface)
{
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    r.cull_backfaces = false;
    Mesh mesh = make_unit_triangle(/*flip_winding=*/true);
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) > 0);
}

// wireframe_color is honoured — every drawn pixel must match.
TEST(renderer, wireframe_uses_wireframe_color)
{
    Renderer r(1);
    r.mode = ShadingMode::Wireframe;
    r.wireframe_color = { 255, 0, 0 }; // red
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);

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
                if (std::abs(static_cast<int>(c.r) - 255) > 2 || c.g > 2 || c.b > 2)
                {
                    wrong++;
                }
            }
        }
    }
    if (drawn == 0)
    {
        ASSERT_FAIL("wireframe drew no pixels");
    }
    if (wrong > 0)
    {
        ASSERT_FAIL("wireframe_color not honoured: " + std::to_string(wrong) + " pixels wrong");
    }
}

// Group B2 — wireframe parallelization (multi-worker)
//
// The wireframe pass now runs on the worker pool. Because every edge pixel is written
// with one uniform colour and depth resolves via draw_line's atomic CAS min, the output
// must be byte-identical for any worker count. These tests pin that, plus the cull /
// clip_near / off-screen / degenerate edge paths under real concurrency (the sanitizer
// CI jobs run this suite under TSAN, so they double as a data-race check).

// A dense mesh that exercises every wireframe edge path at once: a grid of overlapping,
// depth-staggered front faces (edges cross at shared pixels → CAS depth-min contention),
// a triangle spilling far off-screen (bounds rejection in draw_line), a near-plane
// straddler (clip_near), a back-facing single-sided triangle (culled), and a back-facing
// double-sided one (cull bypass). ~245 triangles → spans multiple steal chunks/workers.
static Mesh make_wireframe_stress_mesh()
{
    Mesh m;
    m.materials.push_back(Material{}); // 0: single-sided
    Material ds{};
    ds.double_sided = true;
    m.materials.push_back(ds); // 1: double-sided
    m.has_double_sided = true;

    auto add_tri = [&](vec3 p0, vec3 p1, vec3 p2, uint32_t mat)
    {
        Vertex v{};
        v.ao = 1.0f;
        v.normal = { 0.0f, 0.0f, 1.0f };
        v.uv = { 0.5f, 0.5f };
        const auto base = static_cast<uint32_t>(m.vertices.size());
        v.pos = p0;
        m.vertices.push_back(v);
        v.pos = p1;
        m.vertices.push_back(v);
        v.pos = p2;
        m.vertices.push_back(v);
        Triangle t{};
        t.v[0] = base;
        t.v[1] = base + 1;
        t.v[2] = base + 2;
        t.material_idx = mat;
        m.triangles.push_back(t);
    };

    // 12×10 grid, two overlapping front-facing triangles per cell, depth staggered.
    for (int gy = 0; gy < 10; gy++)
    {
        for (int gx = 0; gx < 12; gx++)
        {
            const float x = -2.0f + (4.0f * (static_cast<float>(gx) / 11.0f));
            const float y = -1.5f + (3.0f * (static_cast<float>(gy) / 9.0f));
            const float z = -0.5f + (static_cast<float>((gx + gy) % 5) / 4.0f);
            const float s = 0.5f; // overlaps neighbouring cells
            add_tri({ x - s, y - s, z }, { x + s, y - s, z }, { x, y + s, z }, 0);
            add_tri({ x + s, y + s, z }, { x - s, y + s, z }, { x, y - s, z }, 0);
        }
    }

    add_tri({ -20.0f, -20.0f, 0.0f }, { 20.0f, -20.0f, 0.0f }, { 0.0f, 20.0f, 0.0f }, 0); // off-screen spill
    add_tri({ -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 4.95f }, 0);     // near-plane straddle
    add_tri({ -1.0f, -1.0f, 0.2f }, { 0.0f, 1.0f, 0.2f }, { 1.0f, -1.0f, 0.2f }, 0);      // back-face, culled
    add_tri({ -1.5f, -1.0f, -0.2f }, { 0.0f, 1.0f, -0.2f }, { 1.5f, -1.0f, -0.2f }, 1);   // back-face, double-sided

    return m;
}

// Keystone: a multi-worker render must be byte-identical to a single-worker render
// of the same scene, across thread counts and chunk boundaries.
TEST(renderer, wireframe_multiworker_matches_singleworker)
{
    Mesh mesh = make_wireframe_stress_mesh();
    Camera cam = make_test_camera();
    const Color wfc{ 200, 150, 50 }; // distinct channels catch any channel swap

    Framebuffer fb1(40, 20, /*headless=*/true);
    fb1.clear();
    {
        Renderer r(1);
        r.mode = ShadingMode::Wireframe;
        r.wireframe_color = wfc;
        r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb1);
    }

    for (int nthreads : { 2, 4, 8 })
    {
        Framebuffer fbn(40, 20, /*headless=*/true);
        fbn.clear();
        Renderer r(nthreads);
        r.mode = ShadingMode::Wireframe;
        r.wireframe_color = wfc;
        r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fbn);

        for (int y = 0; y < fb1.height(); y++)
        {
            for (int x = 0; x < fb1.width(); x++)
            {
                const Color a = fb1.get_pixel(x, y);
                const Color b = fbn.get_pixel(x, y);
                if (a.r != b.r || a.g != b.g || a.b != b.b)
                {
                    ASSERT_FAIL(
                        "wireframe MT mismatch at (" + std::to_string(x) + "," + std::to_string(y) +
                        ") threads=" + std::to_string(nthreads)
                    );
                }
            }
        }
    }

    // Guard the vacuous case: the stress mesh must actually draw edges (else the
    // all-pixels-match loop above would pass trivially on two blank framebuffers).
    ASSERT_TRUE(count_drawn_pixels(fb1) > 0);
}

// the multi-worker path draws a visible front-facing triangle.
TEST(renderer, wireframe_multiworker_visible_triangle_drawn)
{
    Renderer r(4);
    r.mode = ShadingMode::Wireframe;
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) > 0);
}

// backface culling still rejects under the multi-worker path.
TEST(renderer, wireframe_multiworker_backface_culled)
{
    Renderer r(4);
    r.mode = ShadingMode::Wireframe;
    r.cull_backfaces = true;
    Mesh mesh = make_unit_triangle(/*flip_winding=*/true);
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) == 0);
}

// every drawn pixel is exactly wireframe_color under concurrency (no torn writes /
// per-worker colour corruption). Large triangle spans all worker bands.
TEST(renderer, wireframe_multiworker_color_uniform)
{
    Renderer r(4);
    r.mode = ShadingMode::Wireframe;
    r.wireframe_color = { 255, 0, 0 };
    Mesh mesh = make_large_triangle();
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);

    // Detect "drawn" via depth (was_drawn), not via non-black colour: a torn/interleaved
    // write that corrupted a pixel to {0,0,0} must still be caught here, not filtered out as
    // "not drawn" — corruption detection is the whole point of this test.
    int drawn = 0;
    for (int y = 0; y < fb.height(); y++)
    {
        for (int x = 0; x < fb.width(); x++)
        {
            if (!was_drawn(fb, x, y))
            {
                continue;
            }
            drawn++;
            const Color c = fb.get_pixel(x, y);
            if (c.r != 255 || c.g != 0 || c.b != 0)
            {
                ASSERT_FAIL("wireframe MT colour corrupted at (" + std::to_string(x) + "," + std::to_string(y) + ")");
            }
        }
    }
    ASSERT_TRUE(drawn > 0);
}

// an empty mesh (no triangles) on the multi-worker path is a clean no-op.
TEST(renderer, wireframe_multiworker_empty_mesh_no_crash)
{
    Renderer r(4);
    r.mode = ShadingMode::Wireframe;
    Mesh mesh; // no vertices, triangles, or materials
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) == 0);
}

// Group C — shading dispatch
//
// Scene: front-facing unit triangle, red light from +Z, tiny ambient.
// Normal (0,0,1) · light (0,0,1) = 1 → full diffuse → R≥150 at centre pixel.

// Flat shading produces a lit centre pixel.
TEST(renderer, flat_shading_renders_lit_pixel)
{
    Renderer r(1);
    r.mode = ShadingMode::Flat;
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 0.0f, 0.0f });
    vec3 ambient{ 0.05f, 0.05f, 0.05f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
    {
        ASSERT_FAIL("flat: R too low (" + std::to_string(static_cast<int>(c.r)) + ")");
    }
    if (c.g > 30)
    {
        ASSERT_FAIL("flat: G too high (" + std::to_string(static_cast<int>(c.g)) + ")");
    }
    if (c.b > 30)
    {
        ASSERT_FAIL("flat: B too high (" + std::to_string(static_cast<int>(c.b)) + ")");
    }
}

// Phong shading produces a lit centre pixel.
TEST(renderer, phong_shading_renders_lit_pixel)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 0.0f, 0.0f });
    vec3 ambient{ 0.05f, 0.05f, 0.05f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
    {
        ASSERT_FAIL("phong: R too low (" + std::to_string(static_cast<int>(c.r)) + ")");
    }
    if (c.g > 30)
    {
        ASSERT_FAIL("phong: G too high (" + std::to_string(static_cast<int>(c.g)) + ")");
    }
    if (c.b > 30)
    {
        ASSERT_FAIL("phong: B too high (" + std::to_string(static_cast<int>(c.b)) + ")");
    }
}

// an untextured white surface lit past 1.0 (the original "overblown" symptom) no longer
// hard-clips to flat white: the soft-knee tonemap rolls it off so the centre stays below 255 and
// keeps headroom for shading. ambient 0.4 + full white diffuse = 1.4 -> tonemap -> ~247.
TEST(renderer, untextured_overbright_phong_rolls_off_below_white)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_large_triangle(); // default material: white diffuse, no texture
    mesh.materials[0].specular = { 0.0f, 0.0f, 0.0f };
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f }); // white key, N.L = 1 at centre
    vec3 ambient{ 0.4f, 0.4f, 0.4f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    // Lit to 1.4, which the old hard clamp would have pinned at 255; the rolloff keeps it below.
    if (c.r >= 254 || c.g >= 254 || c.b >= 254)
    {
        ASSERT_FAIL(
            "overbright phong should roll off below white, got (" + std::to_string(static_cast<int>(c.r)) + "," +
            std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
    if (c.r < 235) // still clearly bright, just no longer blown
    {
        ASSERT_FAIL("overbright phong rolled off too far, got R=" + std::to_string(static_cast<int>(c.r)));
    }
}

// Group D — backface culling in MT path

// backface triangle is culled — no pixels drawn in Phong mode.
TEST(renderer, mt_backface_culled)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = true;
    Mesh mesh = make_unit_triangle(/*flip_winding=*/true);
    Camera cam = make_test_camera();
    Light light = make_key_light_z();
    vec3 ambient{ 0.05f, 0.05f, 0.05f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) == 0);
}

// double-sided material bypasses culling — pixels drawn even for a backface.
TEST(renderer, mt_double_sided_renders_backface)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = true;
    // flip_winding=true (back face from camera) + double_sided=true.
    Mesh mesh = make_unit_triangle(/*flip_winding=*/true, /*double_sided=*/true);
    Camera cam = make_test_camera();
    // Ambient-only so color is non-zero even with flip_normals darkening diffuse.
    vec3 ambient{ 0.5f, 0.5f, 0.5f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);
    if (count_drawn_pixels(fb) == 0)
    {
        ASSERT_FAIL("double-sided backface must not be culled");
    }
}

// Group E — MT correctness / multi-frame

// empty mesh completes without hanging; framebuffer stays undrawn.
TEST(renderer, empty_mesh_completes)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh;
    mesh.materials.push_back(Material{}); // at least one material required
    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) == 0);
}

// rendering the same scene twice gives matching pixels (deterministic).
TEST(renderer, repeated_render_deterministic)
{
    Renderer r; // default thread count
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 0.0f, 0.0f });
    vec3 ambient{ 0.05f, 0.05f, 0.05f };

    Framebuffer fb1(40, 20, /*headless=*/true), fb2(40, 20, /*headless=*/true);
    fb1.clear();
    fb2.clear();
    r.render(mesh, cam, &light, 1, ambient, fb1);
    r.render(mesh, cam, &light, 1, ambient, fb2);

    // Centre pixel must be present in both and match within ±1 rounding.
    ASSERT_TRUE(was_drawn(fb1, 20, 10));
    ASSERT_TRUE(was_drawn(fb2, 20, 10));
    assert_pixel_near(fb2, 20, 10, fb1.get_pixel(20, 10), 1);
}

// large triangle spanning all 4 worker bands — pixels drawn in every band.
// Triangle sa≈(12,18), sb≈(28,18), sc≈(20,2) → covers y=2..18.
// With 4 workers on 40×20: bands are y=[0..4],[5..9],[10..14],[15..19].
// Checks: y=3 (band 0), y=7 (band 1), y=10 (band 2), y=17 (band 3).
TEST(renderer, large_triangle_spans_all_bands)
{
    Renderer r(4);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_large_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.2f, 0.2f, 0.2f };
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    if (!was_drawn(fb, 20, 3))
    {
        ASSERT_FAIL("no pixel drawn at y=3 (band 0): band bucketing gap");
    }
    if (!was_drawn(fb, 20, 7))
    {
        ASSERT_FAIL("no pixel drawn at y=7 (band 1): band bucketing gap");
    }
    if (!was_drawn(fb, 20, 10))
    {
        ASSERT_FAIL("no pixel drawn at y=10 (band 2): band bucketing gap");
    }
    if (!was_drawn(fb, 20, 17))
    {
        ASSERT_FAIL("no pixel drawn at y=17 (band 3): band bucketing gap");
    }
}

// Group F — lights, texture toggle

// n_lights=0 → ambient-only output.
TEST(renderer, zero_lights_renders_ambient_only)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();
    // Pure red ambient so we can distinguish it from black.
    vec3 ambient{ 0.5f, 0.0f, 0.0f };

    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    // ambient * mat.ambient * ao = (0.5,0,0) * (1,1,1) * 1 ≈ R=127.
    if (c.r < 80)
    {
        ASSERT_FAIL("ambient-only R too low (" + std::to_string(static_cast<int>(c.r)) + ")");
    }
    if (c.g > 20)
    {
        ASSERT_FAIL("ambient-only G too high (" + std::to_string(static_cast<int>(c.g)) + ")");
    }
    if (c.b > 20)
    {
        ASSERT_FAIL("ambient-only B too high (" + std::to_string(static_cast<int>(c.b)) + ")");
    }
}

// show_texture toggle changes pixel colour.
// Mesh has a solid green diffuse texture. With show_texture=true the pixel is
// green; with show_texture=false the pixel is white (white light × white mat).
TEST(renderer, show_texture_toggle_changes_pixel)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;

    Mesh mesh = make_unit_triangle();
    mesh.textures.push_back(make_solid_tex_rgba(2, 2, 0, 255, 0)); // solid green
    mesh.materials[0].diffuse_map.tex = 0;
    mesh.materials[0].diffuse = { 1.0f, 1.0f, 1.0f };
    mesh.materials[0].ambient = { 1.0f, 1.0f, 1.0f };
    mesh.materials[0].specular = { 0.0f, 0.0f, 0.0f };

    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f }); // white light
    vec3 ambient{ 0.0f, 0.0f, 0.0f };                     // no ambient so tex colour is clear

    // Render with texture enabled.
    Framebuffer fb_tex(40, 20, /*headless=*/true);
    fb_tex.clear();
    r.show_texture = true;
    r.render(mesh, cam, &light, 1, ambient, fb_tex);

    // Render with texture disabled.
    Framebuffer fb_notex(40, 20, /*headless=*/true);
    fb_notex.clear();
    r.show_texture = false;
    r.render(mesh, cam, &light, 1, ambient, fb_notex);

    ASSERT_TRUE(was_drawn(fb_tex, 20, 10));
    ASSERT_TRUE(was_drawn(fb_notex, 20, 10));

    Color ct = fb_tex.get_pixel(20, 10);
    Color cn = fb_notex.get_pixel(20, 10);

    // With texture: diffuse*tex = (1,1,1)*(0,1,0) → green.
    if (ct.g < 200)
    {
        ASSERT_FAIL("show_texture=true: G too low (" + std::to_string(static_cast<int>(ct.g)) + ")");
    }
    if (ct.r > 60)
    {
        ASSERT_FAIL("show_texture=true: R too high (" + std::to_string(static_cast<int>(ct.r)) + ")");
    }

    // Without texture: diffuse (1,1,1) × white light → white.
    if (cn.r < 200)
    {
        ASSERT_FAIL("show_texture=false: R too low (" + std::to_string(static_cast<int>(cn.r)) + ")");
    }
    if (cn.g < 200)
    {
        ASSERT_FAIL("show_texture=false: G too low (" + std::to_string(static_cast<int>(cn.g)) + ")");
    }
}

// spec-literal emissive — a zero emissiveFactor + bound emissive texture must render
// dark (emissive = factor × texture = 0), matching three.js / Khronos Sample Viewer. This
// pins the rasterizer's do_emissive gate end-to-end: has_emissive is forced true so the
// renderer DOES forward the bright texture to the rasterizer, isolating do_emissive as the
// only thing keeping the pixel dark. A future change to do_emissive = (factor>0 || etex)
// would make this glow green and fail here.
TEST(renderer, zero_factor_with_emissive_texture_renders_dark)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;

    Mesh mesh = make_unit_triangle();
    mesh.textures.push_back(make_solid_tex_rgba(2, 2, 0, 255, 0)); // bright green emissive texture
    mesh.materials[0].emissive_map.tex = 0;
    mesh.materials[0].emissive = { 0.0f, 0.0f, 0.0f }; // spec default: no emission
    mesh.materials[0].diffuse = { 0.0f, 0.0f, 0.0f };
    mesh.materials[0].ambient = { 0.0f, 0.0f, 0.0f };
    mesh.materials[0].specular = { 0.0f, 0.0f, 0.0f };
    // Force the flag true so the renderer forwards the emissive texture — load_model would
    // derive false for a zero factor, but here we want the texture to reach the rasterizer so
    // do_emissive (factor-only) is the gate under test, not the mesh-level has_emissive prune.
    mesh.has_emissive = true;

    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 0.0f, 0.0f, 0.0f }); // no lighting contribution
    vec3 ambient{ 0.0f, 0.0f, 0.0f };

    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.show_texture = true; // textures on: the only thing keeping it dark is the zero factor
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 20 || c.g > 20 || c.b > 20)
    {
        ASSERT_FAIL(
            "zero factor + emissive texture: expected near-black, got (" + std::to_string(static_cast<int>(c.r)) + "," +
            std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

// Flat-mode parity for the zero-emissiveFactor test above. rasterize_flat() and rasterize_phong() are
// textually independent hand-typed do_emissive gates (rasterize.cpp:289 and :500), so each
// shading path needs its own coverage against a future `factor>0 || etex` regression.
TEST(renderer, flat_zero_factor_with_emissive_texture_renders_dark)
{
    Renderer r(1);
    r.mode = ShadingMode::Flat;

    Mesh mesh = make_unit_triangle();
    mesh.textures.push_back(make_solid_tex_rgba(2, 2, 0, 255, 0)); // bright green emissive texture
    mesh.materials[0].emissive_map.tex = 0;
    mesh.materials[0].emissive = { 0.0f, 0.0f, 0.0f };
    mesh.materials[0].diffuse = { 0.0f, 0.0f, 0.0f };
    mesh.materials[0].ambient = { 0.0f, 0.0f, 0.0f };
    mesh.materials[0].specular = { 0.0f, 0.0f, 0.0f };
    mesh.has_emissive = true; // force the texture to reach the rasterizer

    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 0.0f, 0.0f, 0.0f });
    vec3 ambient{ 0.0f, 0.0f, 0.0f };

    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.show_texture = true;
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 20 || c.g > 20 || c.b > 20)
    {
        ASSERT_FAIL(
            "Flat zero factor + emissive texture: expected near-black, got (" + std::to_string(static_cast<int>(c.r)) +
            "," + std::to_string(static_cast<int>(c.g)) + "," + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

// show_texture=false must NOT suppress the authored emissive factor. By analogy with
// mat.diffuse (which stays in effect when diffuse_tex is hidden by the toggle), the
// authored emissive factor must survive — a model with `Ke 1 0 0` should keep its red glow
// even with textures off.
TEST(renderer, show_texture_toggle_preserves_authored_emissive_factor)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;

    Mesh mesh = make_unit_triangle();
    mesh.materials[0].emissive_map.tex = -1;           // no texture: factor must be authored
    mesh.materials[0].emissive = { 1.0f, 0.0f, 0.0f }; // red glow, authored
    mesh.materials[0].diffuse = { 0.0f, 0.0f, 0.0f };
    mesh.materials[0].ambient = { 0.0f, 0.0f, 0.0f };
    mesh.materials[0].specular = { 0.0f, 0.0f, 0.0f };
    mesh.has_emissive = true;

    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 0.0f, 0.0f, 0.0f });
    vec3 ambient{ 0.0f, 0.0f, 0.0f };

    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.show_texture = false;
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 200)
    {
        ASSERT_FAIL(
            "show_texture=false with authored factor-only emissive: expected red glow, got R=" +
            std::to_string(static_cast<int>(c.r))
        );
    }
    if (c.g > 20 || c.b > 20)
    {
        ASSERT_FAIL(
            "show_texture=false: unexpected G/B leakage (" + std::to_string(static_cast<int>(c.g)) + "," +
            std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

// authored factor + bound emissive texture must keep the factor across the toggle.
// The texture is suppressed by show_emissive (consistent with diffuse_tex behavior) but the
// authored factor survives.
TEST(renderer, show_texture_toggle_preserves_authored_factor_with_texture)
{
    Renderer r(1);
    r.mode = ShadingMode::Phong;

    Mesh mesh = make_unit_triangle();
    mesh.textures.push_back(make_solid_tex_rgba(2, 2, 0, 0, 255)); // emissive texture: blue
    mesh.materials[0].emissive_map.tex = 0;
    mesh.materials[0].emissive = { 1.0f, 0.0f, 0.0f }; // authored red factor
    mesh.materials[0].diffuse = { 0.0f, 0.0f, 0.0f };
    mesh.materials[0].ambient = { 0.0f, 0.0f, 0.0f };
    mesh.materials[0].specular = { 0.0f, 0.0f, 0.0f };
    mesh.has_emissive = true;

    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 0.0f, 0.0f, 0.0f });
    vec3 ambient{ 0.0f, 0.0f, 0.0f };

    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.show_texture = false;
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    // Texture suppressed (no blue), authored red factor survives.
    if (c.r < 200)
    {
        ASSERT_FAIL(
            "show_texture=false with authored factor + texture: expected red, got R=" +
            std::to_string(static_cast<int>(c.r))
        );
    }
    if (c.b > 20)
    {
        ASSERT_FAIL(
            "show_texture=false: emissive texture leaked through (B=" + std::to_string(static_cast<int>(c.b)) + ")"
        );
    }
}

// Flat-mode parity: authored factor + bound texture must survive the toggle.
TEST(renderer, flat_show_texture_toggle_preserves_authored_factor_with_texture)
{
    Renderer r(1);
    r.mode = ShadingMode::Flat;

    Mesh mesh = make_unit_triangle();
    mesh.textures.push_back(make_solid_tex_rgba(2, 2, 0, 0, 255));
    mesh.materials[0].emissive_map.tex = 0;
    mesh.materials[0].emissive = { 1.0f, 0.0f, 0.0f };
    mesh.materials[0].diffuse = { 0.0f, 0.0f, 0.0f };
    mesh.materials[0].ambient = { 0.0f, 0.0f, 0.0f };
    mesh.materials[0].specular = { 0.0f, 0.0f, 0.0f };
    mesh.has_emissive = true;

    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 0.0f, 0.0f, 0.0f });
    vec3 ambient{ 0.0f, 0.0f, 0.0f };

    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.show_texture = false;
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 200)
    {
        ASSERT_FAIL(
            "Flat show_texture=false with authored factor: expected red, got R=" + std::to_string(static_cast<int>(c.r))
        );
    }
    if (c.b > 20)
    {
        ASSERT_FAIL("Flat show_texture=false: blue texture leaked (B=" + std::to_string(static_cast<int>(c.b)) + ")");
    }
}

// show_texture=false must null-out stex and nmap even when specular_tex and
// normal_tex are set on the material.  A black specular texture would zero out all
// specular if stex were incorrectly forwarded — that mismatch catches the bug.
// Camera at +Z, light at +Z → H=(0,0,1), n·h=1 → specular fires strongly.
TEST(renderer, show_tex_false_suppresses_stex_and_nmap)
{
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f });
    vec3 ambient{ 0.0f, 0.0f, 0.0f };

    // specular_tex → solid black (zeroes specular if sampled).
    // normal_tex  → flat neutral (128,128,255 = straight-up tangent normal).
    Mesh mesh = make_unit_triangle();
    mesh.materials[0].specular = { 1.0f, 1.0f, 1.0f };
    mesh.materials[0].shininess = 32.0f;
    mesh.textures.push_back(make_solid_tex_rgba(2, 2, 0, 0, 0));       // idx 0: black stex
    mesh.textures.push_back(make_solid_tex_rgba(2, 2, 128, 128, 255)); // idx 1: flat nmap
    mesh.materials[0].specular_map.tex = 0;
    mesh.materials[0].normal_map.tex = 1;

    // Baseline: same material, specular_tex/normal_tex at default -1.
    Mesh base = make_unit_triangle();
    base.materials[0].specular = { 1.0f, 1.0f, 1.0f };
    base.materials[0].shininess = 32.0f;

    Renderer r(1);
    r.mode = ShadingMode::Phong;
    r.show_texture = false;

    Framebuffer fb_with(40, 20, /*headless=*/true);
    fb_with.clear();
    r.render(mesh, cam, &light, 1, ambient, fb_with);

    Framebuffer fb_base(40, 20, /*headless=*/true);
    fb_base.clear();
    r.render(base, cam, &light, 1, ambient, fb_base);

    ASSERT_TRUE(was_drawn(fb_with, 20, 10));
    ASSERT_TRUE(was_drawn(fb_base, 20, 10));

    Color cw = fb_with.get_pixel(20, 10);
    Color cb = fb_base.get_pixel(20, 10);
    auto diff = [](uint8_t a, uint8_t b) { return a > b ? a - b : b - a; };
    if (diff(cw.r, cb.r) > 2 || diff(cw.g, cb.g) > 2 || diff(cw.b, cb.b) > 2)
    {
        ASSERT_FAIL(
            "show_tex=false: stex/nmap not suppressed — output differs from baseline (" +
            std::to_string(static_cast<int>(cw.r)) + "," + std::to_string(static_cast<int>(cw.g)) + "," +
            std::to_string(static_cast<int>(cw.b)) + ") vs (" + std::to_string(static_cast<int>(cb.r)) + "," +
            std::to_string(static_cast<int>(cb.g)) + "," + std::to_string(static_cast<int>(cb.b)) + ")"
        );
    }
}

// Group J: double-sided lighting correctness
// The double-sided backface test above only checks pixels-drawn (ambient-only). These tests exercise the
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
    v.normal = { 0.0f, 0.0f, -1.0f };
    v.uv = { 0.5f, 0.5f };

    v.pos = { -1.0f, -1.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 0.0f, 1.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 1.0f, -1.0f, 0.0f };
    m.vertices.push_back(v);

    m.tangents.resize(3, { 1.0f, 0.0f, 0.0f });

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
    v.normal = { 0.0f, 0.0f, -1.0f };
    v.uv = { 0.5f, 0.5f };

    v.pos = { -3.0f, -1.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { -2.0f, 1.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { -1.0f, -1.0f, 0.0f };
    m.vertices.push_back(v);

    v.pos = { 1.0f, -1.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 2.0f, 1.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 3.0f, -1.0f, 0.0f };
    m.vertices.push_back(v);

    m.tangents.resize(6, { 1.0f, 0.0f, 0.0f });

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

// Phong — back-face double-sided triangle lit by a +z light.
// Vertex normals are (0,0,-1); the flip must turn them to (0,0,1) so diffuse fires.
TEST(renderer, phong_double_sided_back_face_lit_correctly)
{
    Mesh mesh = make_back_facing_double_sided_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 0.0f, 0.0f });
    vec3 ambient{ 0.0f, 0.0f, 0.0f };

    Renderer r;
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = true;
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
    {
        ASSERT_FAIL(
            "Phong double-sided back-face R too low (" + std::to_string(static_cast<int>(c.r)) +
            ") — normal flip not applied"
        );
    }
}

// Flat — same scene; covers the separate face-normal negation branch.
TEST(renderer, flat_double_sided_back_face_lit_correctly)
{
    Mesh mesh = make_back_facing_double_sided_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 0.0f, 0.0f });
    vec3 ambient{ 0.0f, 0.0f, 0.0f };

    Renderer r;
    r.mode = ShadingMode::Flat;
    r.cull_backfaces = true;
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
    {
        ASSERT_FAIL(
            "Flat double-sided back-face R too low (" + std::to_string(static_cast<int>(c.r)) +
            ") — face normal flip not applied"
        );
    }
}

// cull off, single-sided back-face → drawn but dark.
// cull_backfaces=false → do_cull=false → if(do_cull) block skipped → flip_normals stays false.
// Normal remains (0,0,-1); dot(n, light=(0,0,1))=-1 → no diffuse → R=0.
TEST(renderer, single_sided_cull_off_back_face_dark)
{
    Mesh mesh = make_back_facing_single_sided_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 0.0f, 0.0f });
    vec3 ambient{ 0.0f, 0.0f, 0.0f };

    Renderer r;
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = false;
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 5)
    {
        ASSERT_FAIL(
            "cull-off single-sided back-face R too high (" + std::to_string(static_cast<int>(c.r)) +
            ") — flip applied when it should not be"
        );
    }
}

// cull off, double-sided back-face → also drawn but dark.
// do_cull=false → flip_normals never set regardless of material.double_sided.
TEST(renderer, double_sided_cull_off_back_face_dark)
{
    Mesh mesh = make_back_facing_double_sided_triangle();
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 0.0f, 0.0f });
    vec3 ambient{ 0.0f, 0.0f, 0.0f };

    Renderer r;
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = false;
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r > 5)
    {
        ASSERT_FAIL(
            "cull-off double-sided back-face R too high (" + std::to_string(static_cast<int>(c.r)) +
            ") — flip applied when cull is off"
        );
    }
}

// front-facing double-sided triangle — flip must NOT fire.
// make_unit_triangle(false, true): normals (0,0,1), CCW → front-face cull passes,
// flip_normals stays false, dot(n=(0,0,1), light=(0,0,1))=1 → full diffuse.
TEST(renderer, double_sided_front_face_lit_normally)
{
    Mesh mesh = make_unit_triangle(false, true);
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 0.0f, 0.0f });
    vec3 ambient{ 0.0f, 0.0f, 0.0f };

    Renderer r;
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = true;
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, &light, 1, ambient, fb);

    ASSERT_TRUE(was_drawn(fb, 20, 10));
    Color c = fb.get_pixel(20, 10);
    if (c.r < 150)
    {
        ASSERT_FAIL(
            "front-facing double-sided R too low (" + std::to_string(static_cast<int>(c.r)) +
            ") — flip incorrectly applied to front face"
        );
    }
}

// mixed mesh — only the double-sided back-face triangle is drawn.
// Tri 0 (double-sided) centre → screen (16,10); tri 1 (single-sided) centre → screen (24,10).
// Uses ambient (0.5,0,0) and no light so drawn = ambient red; not-drawn = depth=inf.
TEST(renderer, mixed_mesh_only_double_sided_back_face_drawn)
{
    Mesh mesh = make_two_back_face_mixed_mesh();
    Camera cam = make_test_camera();
    vec3 ambient{ 0.5f, 0.0f, 0.0f };

    Renderer r;
    r.mode = ShadingMode::Phong;
    r.cull_backfaces = true;
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);

    // Double-sided triangle should be drawn with ambient red.
    ASSERT_TRUE(was_drawn(fb, 16, 10));
    Color c_ds = fb.get_pixel(16, 10);
    if (c_ds.r < 80)
    {
        ASSERT_FAIL(
            "double-sided back-face not lit (R=" + std::to_string(static_cast<int>(c_ds.r)) + ") — may have been culled"
        );
    }

    // Single-sided triangle must be culled — pixel stays undrawn.
    if (was_drawn(fb, 24, 10))
    {
        ASSERT_FAIL("single-sided back-face was drawn — per-material check bypassed");
    }
}

// wireframe double-sided back-face — covers the wireframe path's separate cull bypass.
TEST(renderer, wireframe_double_sided_back_face_drawn)
{
    Mesh mesh = make_back_facing_double_sided_triangle();
    Camera cam = make_test_camera();
    vec3 ambient{ 0.5f, 0.5f, 0.5f };

    Renderer r;
    r.mode = ShadingMode::Wireframe;
    r.cull_backfaces = true;
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    r.render(mesh, cam, nullptr, 0, ambient, fb);

    if (count_drawn_pixels(fb) == 0)
    {
        ASSERT_FAIL("wireframe double-sided back-face drew no pixels — cull bypass missing");
    }
}

TEST(renderer, wireframe_show_texture_toggle_is_noop)
{
    Mesh mesh = make_unit_triangle();
    Camera cam = make_test_camera();

    auto draw = [&](bool show_tex)
    {
        Renderer r(1);
        r.mode = ShadingMode::Wireframe;
        r.show_texture = show_tex;
        Framebuffer fb(40, 20, /*headless=*/true);
        fb.clear();
        r.render(mesh, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
        return count_drawn_pixels(fb);
    };

    ASSERT_EQ(draw(true), draw(false));
}
