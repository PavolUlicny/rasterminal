#include "tests/renderer_test_util.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

// Force every scene through immediate and tiled opaque paths. Pixel colors may
// round differently, and only centers exactly on triangle edges may differ in coverage.

namespace
{
    constexpr int W = 40;
    constexpr int H = 20;

    struct Scene
    {
        Mesh mesh;
        int n_lights = 1;
        ShadingMode mode = ShadingMode::Phong;
        bool cull = true;
    };

    // Render `scene` through the given pass into a fresh (or the supplied) framebuffer.
    void render_with(const Scene &s, Renderer::OpaquePath path, Framebuffer &fb, int n_threads = 4)
    {
        Renderer r(n_threads);
        r.opaque_path = path;
        r.mode = s.mode;
        r.cull_backfaces = s.cull;
        Camera cam = make_test_camera();
        Light lights[2] = { make_key_light_z({ 1.0f, 1.0f, 1.0f }), Light{} };
        lights[1].direction = { -0.667f, -0.333f, -0.667f };
        lights[1].color = { 0.3f, 0.3f, 0.33f };
        r.render(s.mesh, cam, lights, s.n_lights, { 0.2f, 0.2f, 0.2f }, fb);
    }

    // Compare two framebuffers: coverage identical except for at most `edge_slack` pixels,
    // colours within `tol` per channel wherever both are drawn.
    void assert_same_picture(Framebuffer &a, Framebuffer &b, int tol, int edge_slack, const std::string &what)
    {
        int coverage_diff = 0;
        int drawn = 0;
        for (int y = 0; y < a.height(); y++)
        {
            for (int x = 0; x < a.width(); x++)
            {
                const bool da = was_drawn(a, x, y);
                const bool db = was_drawn(b, x, y);
                if (da != db)
                {
                    coverage_diff++;
                    continue;
                }
                if (!da)
                {
                    continue;
                }
                drawn++;
                const Color ca = a.get_pixel(x, y);
                const Color cb = b.get_pixel(x, y);
                const int dr = std::abs(static_cast<int>(ca.r) - static_cast<int>(cb.r));
                const int dg = std::abs(static_cast<int>(ca.g) - static_cast<int>(cb.g));
                const int dbl = std::abs(static_cast<int>(ca.b) - static_cast<int>(cb.b));
                if (dr > tol || dg > tol || dbl > tol)
                {
                    ASSERT_FAIL(
                        what + ": pixel (" + std::to_string(x) + "," + std::to_string(y) + ") differs: (" +
                        std::to_string(ca.r) + "," + std::to_string(ca.g) + "," + std::to_string(ca.b) + ") vs (" +
                        std::to_string(cb.r) + "," + std::to_string(cb.g) + "," + std::to_string(cb.b) + ")"
                    );
                }
            }
        }
        if (coverage_diff > edge_slack)
        {
            ASSERT_FAIL(what + ": coverage differs on " + std::to_string(coverage_diff) + " pixels");
        }
        ASSERT_TRUE(drawn > 0);
    }

    void check_both_paths(const Scene &s, const std::string &what, int tol = 2, int edge_slack = 2)
    {
        for (const int threads : { 1, 4 })
        {
            Framebuffer fa(W, H, /*headless=*/true);
            Framebuffer fb(W, H, /*headless=*/true);
            render_with(s, Renderer::OpaquePath::Immediate, fa, threads);
            render_with(s, Renderer::OpaquePath::Tiled, fb, threads);
            assert_same_picture(fa, fb, tol, edge_slack, what + " (threads=" + std::to_string(threads) + ")");
        }
    }

    // A grid of quads on the plane z=0 spanning [-3,3]^2, with a bulging normal field so Phong
    // varies per pixel; per-vertex colours ramp across x; UVs cover [0,1]. `n` quads per side.
    Mesh make_grid(int n, bool vcol = false)
    {
        Mesh m;
        for (int j = 0; j <= n; j++)
        {
            for (int i = 0; i <= n; i++)
            {
                Vertex v{};
                const float fx = static_cast<float>(i) / static_cast<float>(n);
                const float fy = static_cast<float>(j) / static_cast<float>(n);
                v.pos = { -3.0f + (6.0f * fx) + 0.13f, -3.0f + (6.0f * fy) + 0.07f, 0.0f };
                v.normal = normalize(vec3{ (fx - 0.5f) * 0.8f, (fy - 0.5f) * 0.8f, 1.0f });
                v.uv = { fx, fy };
                v.ao = 0.5f + (0.5f * fx);
                m.vertices.push_back(v);
                m.tangents.emplace_back(1.0f, 0.0f, 0.0f);
                if (vcol)
                {
                    m.vertex_colors.emplace_back(fx, 1.0f - fx, fy);
                }
            }
        }
        for (int j = 0; j < n; j++)
        {
            for (int i = 0; i < n; i++)
            {
                const auto a = static_cast<uint32_t>((j * (n + 1)) + i);
                const uint32_t b = a + 1;
                const uint32_t c = a + static_cast<uint32_t>(n + 1);
                const uint32_t d = c + 1;
                Triangle t{};
                t.v[0] = a;
                t.v[1] = b;
                t.v[2] = d;
                t.material_idx = 0;
                m.triangles.push_back(t);
                t.v[0] = a;
                t.v[1] = d;
                t.v[2] = c;
                m.triangles.push_back(t);
            }
        }
        m.materials.push_back(Material{});
        m.has_vertex_colors = vcol;
        m.opaque_count = static_cast<uint32_t>(m.triangles.size());
        return m;
    }

    // Checkerboard RGBA texture with a transparent quadrant (alpha 0) for the cutout scenes.
    Texture make_checker_tex(int w, int h)
    {
        Texture t;
        t.width = w;
        t.height = h;
        t.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                const size_t base = ((static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(x)) * 4;
                const bool on = ((x / 4) + (y / 4)) % 2 == 0;
                t.pixels[base + 0] = on ? 220 : 40;
                t.pixels[base + 1] = static_cast<uint8_t>(x * 255 / w);
                t.pixels[base + 2] = static_cast<uint8_t>(y * 255 / h);
                t.pixels[base + 3] = (x < w / 2 && y < h / 2) ? 0 : 255;
            }
        }
        return t;
    }
} // namespace

TEST(renderer_tiled, a_resize_does_not_throw_away_the_pass_choice)
{
    // Resize must rescale the previous screen-space area statistic instead of forcing
    // an immediate frame. A 4x4 grid supplies enough samples to select tiling.
    constexpr int RW = 400;
    constexpr int RH = 200;
    Mesh mesh = make_grid(4);
    Renderer r(4);
    r.opaque_path = Renderer::OpaquePath::Auto;
    Camera cam = make_test_camera();
    Light lights[1] = { make_key_light_z({ 1.0f, 1.0f, 1.0f }) };

    Framebuffer fb(RW, RH, /*headless=*/true);
    // Frame 1 has nothing to predict from and is expected to take the immediate pass;
    // by frame 2 the statistic exists.
    for (int i = 0; i < 3; i++)
    {
        fb.clear();
        r.render(mesh, cam, lights, 1, { 0.2f, 0.2f, 0.2f }, fb);
    }
    ASSERT_TRUE(r.last_frame_tiled());

    // A resize must not undo that.
    fb.resize(RW + 1, RH);
    fb.clear();
    r.render(mesh, cam, lights, 1, { 0.2f, 0.2f, 0.2f }, fb);
    ASSERT_TRUE(r.last_frame_tiled());

    // Nor a large one that keeps the frame in tiled territory.
    fb.resize(RW * 2, RH * 2);
    fb.clear();
    r.render(mesh, cam, lights, 1, { 0.2f, 0.2f, 0.2f }, fb);
    ASSERT_TRUE(r.last_frame_tiled());

    // Shrinking below TILE_MIN_PIXELS must recompute the decision and fall back.
    fb.resize(64, 32);
    fb.clear();
    r.render(mesh, cam, lights, 1, { 0.2f, 0.2f, 0.2f }, fb);
    ASSERT_FALSE(r.last_frame_tiled());
}

TEST(renderer_tiled, phong_grid_matches_immediate)
{
    Scene s;
    s.mesh = make_grid(6);
    check_both_paths(s, "phong grid");
}

// MTL can produce negative shininess. The batch shader must read the material value rather
// than per-pixel storage that this path does not initialize.
TEST(renderer_tiled, negative_shininess_is_deterministic)
{
    Scene s;
    s.mesh = make_grid(4);
    s.mesh.materials[0].shininess = -5.0f;
    Color first{};
    for (int run = 0; run < 3; run++)
    {
        for (const auto path : { Renderer::OpaquePath::Immediate, Renderer::OpaquePath::Tiled })
        {
            Framebuffer fb(W, H, /*headless=*/true);
            render_with(s, path, fb);
            const Color c = fb.get_pixel(20, 10);
            ASSERT_TRUE(was_drawn(fb, 20, 10));
            if (run == 0 && path == Renderer::OpaquePath::Immediate)
            {
                first = c;
            }
            else
            {
                assert_pixel_near(fb, 20, 10, first, 1);
            }
        }
    }
    check_both_paths(s, "negative shininess");
}

TEST(renderer_tiled, phong_grid_two_lights_matches_immediate)
{
    Scene s;
    s.mesh = make_grid(6);
    s.n_lights = 2;
    check_both_paths(s, "phong grid two lights");
}

TEST(renderer_tiled, flat_grid_matches_immediate)
{
    Scene s;
    s.mesh = make_grid(6);
    s.mode = ShadingMode::Flat;
    check_both_paths(s, "flat grid");
}

TEST(renderer_tiled, vertex_colors_match_immediate)
{
    Scene s;
    s.mesh = make_grid(5, /*vcol=*/true);
    check_both_paths(s, "vertex colours");
}

TEST(renderer_tiled, textured_and_cutout_match_immediate)
{
    Scene s;
    s.mesh = make_grid(6);
    s.mesh.textures.push_back(make_checker_tex(32, 32));
    s.mesh.materials[0].diffuse_map.tex = 0;
    s.mesh.materials[0].alpha_cutoff = 0.5f; // the transparent quadrant must be cut out in both
    check_both_paths(s, "textured cutout");
    Framebuffer fb(W, H, /*headless=*/true);
    render_with(s, Renderer::OpaquePath::Tiled, fb);
    ASSERT_TRUE(count_drawn_pixels(fb) < W * H); // the cutout quadrant left pixels undrawn
    s.mesh.materials[0].alpha_cutoff = 0.0f;
    check_both_paths(s, "textured");
    s.mode = ShadingMode::Flat;
    check_both_paths(s, "textured flat");
}

TEST(renderer_tiled, normal_map_and_metallic_match_immediate)
{
    Scene s;
    s.mesh = make_grid(6);
    s.mesh.textures.push_back(make_checker_tex(16, 16));
    s.mesh.materials[0].normal_map.tex = 0;
    s.mesh.materials[0].mr_map.tex = 0;
    s.mesh.materials[0].metallic = 0.7f;
    s.mesh.materials[0].roughness = 0.4f;
    s.mesh.has_metallic = true;
    check_both_paths(s, "normal map + metallic");
}

TEST(renderer_tiled, unlit_matches_immediate)
{
    Scene s;
    s.mesh = make_grid(4, /*vcol=*/true);
    s.mesh.materials[0].unlit = true;
    s.mesh.materials[0].diffuse = { 0.9f, 0.5f, 0.2f };
    s.mesh.has_unlit = true;
    check_both_paths(s, "unlit");
}

TEST(renderer_tiled, double_sided_backface_matches_immediate)
{
    Scene s;
    s.mesh = make_unit_triangle(/*flip_winding=*/true, /*double_sided=*/true);
    check_both_paths(s, "double-sided backface", 2, 0);
    Framebuffer fb(W, H, /*headless=*/true);
    render_with(s, Renderer::OpaquePath::Tiled, fb);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
}

TEST(renderer_tiled, near_clipped_matches_immediate)
{
    // One vertex behind the near plane: the tiled pass parks the clipped triangle in its
    // ClipVert arena instead of re-reading mesh attributes.
    Scene s;
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
    m.triangles.push_back(tri);
    m.materials.push_back(Material{});
    s.mesh = m;
    check_both_paths(s, "near clipped");
}

TEST(renderer_tiled, respects_existing_framebuffer_depth)
{
    // Content already in the framebuffer that is nearer than the mesh must survive both passes.
    Scene s;
    s.mesh = make_grid(6);
    for (const auto path : { Renderer::OpaquePath::Immediate, Renderer::OpaquePath::Tiled })
    {
        Framebuffer fb(W, H, /*headless=*/true);
        fb.clear({ 0, 0, 0 });
        for (int x = 10; x < 30; x++)
        {
            fb.commit_pixel(x, 10, -5.0f, Color{ 1, 2, 3 }); // nearer than anything the grid draws
        }
        render_with(s, path, fb);
        for (int x = 10; x < 30; x++)
        {
            assert_pixel_near(fb, x, 10, Color{ 1, 2, 3 }, 0);
        }
        ASSERT_TRUE(was_drawn(fb, 20, 5)); // the grid still landed elsewhere
    }
}

TEST(renderer_tiled, transparent_pass_composites_over_tiled_opaque)
{
    // A blend triangle in front of the opaque grid must composite identically after either pass.
    Scene s;
    s.mesh = make_grid(6);
    {
        Mesh &m = s.mesh;
        const auto base = static_cast<uint32_t>(m.vertices.size());
        Vertex v{};
        v.ao = 1.0f;
        v.normal = { 0.0f, 0.0f, 1.0f };
        v.uv = { 0.5f, 0.5f };
        v.pos = { -1.5f, -1.5f, 1.0f };
        m.vertices.push_back(v);
        v.pos = { 1.5f, -1.5f, 1.0f };
        m.vertices.push_back(v);
        v.pos = { 0.0f, 1.5f, 1.0f };
        m.vertices.push_back(v);
        m.tangents.resize(m.vertices.size(), { 1.0f, 0.0f, 0.0f });
        Triangle t{};
        t.v[0] = base;
        t.v[1] = base + 1;
        t.v[2] = base + 2;
        t.material_idx = 1;
        m.triangles.push_back(t);
        Material mat{};
        mat.diffuse = { 0.2f, 0.4f, 0.9f };
        mat.blend = true;
        mat.alpha = 0.5f;
        m.materials.push_back(mat);
        m.has_transparent = true;
        m.opaque_count = static_cast<uint32_t>(m.triangles.size()) - 1;
    }
    check_both_paths(s, "transparent over opaque");
}

TEST(renderer_tiled, auto_on_a_cold_small_frame_matches_the_forced_passes)
{
    // Auto on its two fallbacks at once: one frame gives the predictor nothing to read, and
    // W*H is below TILE_MIN_PIXELS anyway, so this pins the immediate pass it drops back to.
    // The frame where Auto does choose tiles is the test below.
    Scene s;
    s.mesh = make_grid(6);
    Framebuffer fauto(W, H, /*headless=*/true);
    Framebuffer ftiled(W, H, /*headless=*/true);
    render_with(s, Renderer::OpaquePath::Auto, fauto);
    render_with(s, Renderer::OpaquePath::Tiled, ftiled);
    assert_same_picture(fauto, ftiled, 2, 2, "auto vs tiled");
}

TEST(renderer_tiled, auto_choosing_tiles_matches_both_forced_passes)
{
    // Prime Auto's predictor, then compare its output with both forced paths.
    constexpr int AW = 400;
    constexpr int AH = 200;
    Scene s;
    s.mesh = make_grid(6);

    Renderer r(4);
    r.opaque_path = Renderer::OpaquePath::Auto;
    r.mode = s.mode;
    r.cull_backfaces = s.cull;
    Camera cam = make_test_camera();
    Light lights[2] = { make_key_light_z({ 1.0f, 1.0f, 1.0f }), Light{} };
    lights[1].direction = { -0.667f, -0.333f, -0.667f };
    lights[1].color = { 0.3f, 0.3f, 0.33f };
    Framebuffer fauto(AW, AH, /*headless=*/true);
    for (int i = 0; i < 3; i++)
    {
        fauto.clear();
        r.render(s.mesh, cam, lights, s.n_lights, { 0.2f, 0.2f, 0.2f }, fauto);
    }
    ASSERT_TRUE(r.last_frame_tiled());

    Framebuffer ftiled(AW, AH, /*headless=*/true);
    Framebuffer fimm(AW, AH, /*headless=*/true);
    ftiled.clear();
    fimm.clear();
    render_with(s, Renderer::OpaquePath::Tiled, ftiled);
    render_with(s, Renderer::OpaquePath::Immediate, fimm);
    assert_same_picture(fauto, ftiled, 2, 8, "auto vs tiled");
    assert_same_picture(fauto, fimm, 2, 8, "auto vs immediate");
}

TEST(renderer_tiled, large_frame_many_tiles)
{
    // A frame large enough for 32-pixel tiles (several per worker), so tile bounds, the
    // per-worker bin sort and the tile steal loop all run at full size.
    Scene s;
    s.mesh = make_grid(8);
    Framebuffer fa(320, 200, /*headless=*/true);
    Framebuffer fb(320, 200, /*headless=*/true);
    render_with(s, Renderer::OpaquePath::Immediate, fa, 4);
    render_with(s, Renderer::OpaquePath::Tiled, fb, 4);
    assert_same_picture(fa, fb, 2, 8, "320x200 grid");
}

TEST(renderer_tiled, a_single_odd_frame_does_not_flip_the_pass)
{
    // Require several consecutive votes before switching paths so a moving view's
    // sampled tail cannot cause one-frame oscillation. Perturb the camera because a
    // mesh change intentionally resets damping.
    constexpr int RW = 400;
    constexpr int RH = 200;
    Mesh mesh = make_grid(4);
    Renderer r(4);
    r.opaque_path = Renderer::OpaquePath::Auto;
    Camera near_cam = make_test_camera();
    Camera far_cam = make_test_camera();
    far_cam.distance = 60.0f; // triangles shrink to a few pixels: the statistics ask for immediate
    Light lights[1] = { make_key_light_z({ 1.0f, 1.0f, 1.0f }) };

    Framebuffer fb(RW, RH, /*headless=*/true);
    for (int i = 0; i < 4; i++) // settle on the tiled pass
    {
        fb.clear();
        r.render(mesh, near_cam, lights, 1, { 0.2f, 0.2f, 0.2f }, fb);
    }
    ASSERT_TRUE(r.last_frame_tiled());

    // One distant frame does not overcome damping.
    fb.clear();
    r.render(mesh, far_cam, lights, 1, { 0.2f, 0.2f, 0.2f }, fb);
    fb.clear();
    r.render(mesh, near_cam, lights, 1, { 0.2f, 0.2f, 0.2f }, fb);
    ASSERT_TRUE(r.last_frame_tiled());

    // Sustained distant frames switch away from tiling.
    for (int i = 0; i < 24; i++)
    {
        fb.clear();
        r.render(mesh, far_cam, lights, 1, { 0.2f, 0.2f, 0.2f }, fb);
    }
    ASSERT_FALSE(r.last_frame_tiled());

    // Sustained near frames switch back to tiling.
    for (int i = 0; i < 24; i++)
    {
        fb.clear();
        r.render(mesh, near_cam, lights, 1, { 0.2f, 0.2f, 0.2f }, fb);
    }
    ASSERT_TRUE(r.last_frame_tiled());
}

TEST(renderer_tiled, a_worker_task_throwing_length_error_does_not_terminate)
{
    // Worker entry points must catch any std::exception, including length_error from
    // vector max_size on ILP32, or std::thread calls terminate. run_on_workers pins
    // that catch without requiring half a billion tile touches.
    Renderer r(4);
    // Not the 4 requested: resolve_thread_count clamps to the hardware thread count, so a
    // two- or three-core runner builds a smaller pool.
    const int workers = r.worker_count();
    std::atomic<int> ran{ 0 };
    r.run_on_workers(
        [&ran](int, int)
        {
            ran.fetch_add(1);
            throw std::length_error("worker");
        }
    );
    ASSERT_EQ(ran.load(), workers);

    // The pool is still usable afterwards: a swallowed throw must not leave a worker out of
    // the generation handshake, or the next dispatch would block forever.
    std::atomic<int> again{ 0 };
    r.run_on_workers([&again](int, int) { again.fetch_add(1); });
    ASSERT_EQ(again.load(), workers);

    Mesh mesh = make_large_triangle();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear();
    Camera cam = make_test_camera();
    r.render(mesh, cam, nullptr, 0, { 1.0f, 1.0f, 1.0f }, fb);
    ASSERT_TRUE(was_drawn(fb, 20, 10));
}

TEST(renderer_tiled, a_new_mesh_is_judged_at_once_rather_than_damped)
{
    // A mesh change clears settled state and statistics, bypassing damping on the new scene.
    constexpr int RW = 400;
    constexpr int RH = 200;
    Renderer r(4);
    r.opaque_path = Renderer::OpaquePath::Auto;
    Camera cam = make_test_camera();
    Light lights[1] = { make_key_light_z({ 1.0f, 1.0f, 1.0f }) };
    Framebuffer fb(RW, RH, /*headless=*/true);

    Mesh first = make_grid(4);
    for (int i = 0; i < 6; i++) // settle the first scene on the tiled pass
    {
        fb.clear();
        r.render(first, cam, lights, 1, { 0.2f, 0.2f, 0.2f }, fb);
    }
    ASSERT_TRUE(r.last_frame_tiled());

    // A second, equally tile-friendly mesh through the SAME renderer. Frame 1 has no statistics
    // for it yet and must take the immediate pass; frame 2 has them and must act on them at once.
    Mesh second = make_grid(4);
    fb.clear();
    r.render(second, cam, lights, 1, { 0.2f, 0.2f, 0.2f }, fb);
    ASSERT_FALSE(r.last_frame_tiled()); // nothing to predict from yet
    fb.clear();
    r.render(second, cam, lights, 1, { 0.2f, 0.2f, 0.2f }, fb);
    ASSERT_TRUE(r.last_frame_tiled()); // undamped: acted on the first real statistics
}
