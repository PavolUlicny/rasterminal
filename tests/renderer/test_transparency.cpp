#include "tests/loader_util.h"
#include "tests/renderer_test_util.h"

#include <cstdint>
#include <string>
#include <utility>

// ─── helpers ──────────────────────────────────────────────────────────────────

namespace
{
    // Append one unlit triangle with its own material. Unlit so the rasterizer emits
    // mat.diffuse directly (no lighting), making composite colours exactly predictable.
    // blend routes it to the transparent pass; alpha is the material base opacity.
    void add_unlit_tri(Mesh &m, vec3 p0, vec3 p1, vec3 p2, vec3 diffuse, float alpha, bool blend, float vcol_a = 1.0f)
    {
        const auto base = static_cast<uint32_t>(m.vertices.size());
        Vertex v{};
        v.ao = 1.0f;
        v.normal = { 0.0f, 0.0f, 1.0f };
        v.uv = { 0.5f, 0.5f };
        v.pos = p0;
        m.vertices.push_back(v);
        v.pos = p1;
        m.vertices.push_back(v);
        v.pos = p2;
        m.vertices.push_back(v);
        m.tangents.emplace_back(1.0f, 0.0f, 0.0f);
        m.tangents.emplace_back(1.0f, 0.0f, 0.0f);
        m.tangents.emplace_back(1.0f, 0.0f, 0.0f);

        if (vcol_a != 1.0f)
        {
            // Pad any earlier vertices to opaque so the parallel array stays length-matched.
            m.vertex_alpha.resize(base, 1.0f);
            m.vertex_alpha.push_back(vcol_a);
            m.vertex_alpha.push_back(vcol_a);
            m.vertex_alpha.push_back(vcol_a);
            m.has_vertex_alpha = true;
        }

        Triangle t{};
        t.v[0] = base;
        t.v[1] = base + 1;
        t.v[2] = base + 2;
        t.material_idx = static_cast<uint32_t>(m.materials.size());
        m.triangles.push_back(t);

        Material mat{};
        mat.diffuse = diffuse;
        mat.unlit = true;
        mat.blend = blend;
        mat.alpha = alpha;
        m.materials.push_back(mat);
    }

    // Finalize a hand-built mesh: set the mesh-level fast-path flags the renderer reads,
    // and (since these meshes bypass load_model) keep vertex_alpha length-matched.
    void finalize(Mesh &m, uint32_t opaque_count)
    {
        m.has_unlit = true;
        m.opaque_count = opaque_count;
        m.has_transparent =
            std::any_of(m.materials.begin(), m.materials.end(), [](const Material &mm) { return mm.blend; });
        if (m.has_vertex_alpha && m.vertex_alpha.size() < m.vertices.size())
        {
            m.vertex_alpha.resize(m.vertices.size(), 1.0f);
        }
    }

    // Large flat triangle at constant z, covering the central pixels (see make_large_triangle).
    void add_flat_large(Mesh &m, float z, vec3 diffuse, float alpha, bool blend)
    {
        add_unlit_tri(m, { -4.0f, -4.0f, z }, { 4.0f, -4.0f, z }, { 0.0f, 4.0f, z }, diffuse, alpha, blend);
    }

    constexpr vec3 RED{ 1.0f, 0.0f, 0.0f };
    constexpr vec3 GREEN{ 0.0f, 1.0f, 0.0f };
} // namespace

// ─── compositing over the background ────────────────────────────────────────────

// A red transparent triangle (alpha 0.5) over a blue background composites to purple.
TEST(transparency, blend_over_background)
{
    Renderer r(1);
    Mesh m;
    add_flat_large(m, 0.0f, RED, 0.5f, /*blend=*/true);
    finalize(m, /*opaque_count=*/0);

    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear({ 0, 0, 255 });
    r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);

    // 0.5*red + 0.5*blue = (128, 0, 128)
    assert_pixel_near(fb, 20, 10, { 128, 0, 128 }, 2);
}

// Multi-threaded resolve produces the same composite (no per-pixel races).
TEST(transparency, blend_over_background_threaded)
{
    Renderer r(4);
    Mesh m;
    add_flat_large(m, 0.0f, RED, 0.5f, true);
    finalize(m, 0);

    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, true);
    fb.clear({ 0, 0, 255 });
    r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    assert_pixel_near(fb, 20, 10, { 128, 0, 128 }, 2);
}

// ─── ordering ─────────────────────────────────────────────────────────────────

// Two overlapping transparent triangles composite back-to-front regardless of the
// order they are rasterized in (the per-pixel resolve sorts by depth).
TEST(transparency, order_independent_composite)
{
    Camera cam = make_test_camera();

    // near red (z=0) over far green (z=-1) over black:
    //   far first → 0.5*green = (0,128,0); then 0.5*red + 0.5*(0,128,0) = (128,64,0)
    const Color expected{ 128, 64, 0 };

    Renderer r(1);
    {
        Mesh m; // red first, then green
        add_flat_large(m, 0.0f, RED, 0.5f, true);
        add_flat_large(m, -1.0f, GREEN, 0.5f, true);
        finalize(m, 0);
        Framebuffer fb(40, 20, true);
        fb.clear({ 0, 0, 0 });
        r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
        assert_pixel_near(fb, 20, 10, expected, 3);
    }
    {
        Mesh m; // reversed submission order — same result
        add_flat_large(m, -1.0f, GREEN, 0.5f, true);
        add_flat_large(m, 0.0f, RED, 0.5f, true);
        finalize(m, 0);
        Framebuffer fb(40, 20, true);
        fb.clear({ 0, 0, 0 });
        r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
        assert_pixel_near(fb, 20, 10, expected, 3);
    }
}

// Interpenetrating triangles: each crosses the other in depth across the screen, so
// the front-most surface differs per pixel. A per-PIXEL sort gets both sides right; a
// whole-triangle sort (equal centroids → one global order) cannot. Red is near on the
// left, green is near on the right.
TEST(transparency, interpenetration_per_pixel_order)
{
    Renderer r(1);
    Mesh m;
    // A (red): left near (z=+1), right far (z=-1).
    add_unlit_tri(m, { -4.0f, -4.0f, 1.0f }, { 4.0f, -4.0f, -1.0f }, { 0.0f, 4.0f, 0.0f }, RED, 0.5f, true);
    // B (green): left far (z=-1), right near (z=+1).
    add_unlit_tri(m, { -4.0f, -4.0f, -1.0f }, { 4.0f, -4.0f, 1.0f }, { 0.0f, 4.0f, 0.0f }, GREEN, 0.5f, true);
    finalize(m, 0);

    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, true);
    fb.clear({ 0, 0, 0 });
    r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);

    // Left of centre: red is in front → red-dominant. Right of centre: green in front.
    const Color left = fb.get_pixel(18, 11);
    const Color right = fb.get_pixel(22, 11);
    ASSERT_TRUE(left.r > left.g);
    ASSERT_TRUE(right.g > right.r);
}

// ─── shared-edge fill rule ──────────────────────────────────────────────────────

// A quad split into two transparent triangles sharing a diagonal. Every covered pixel
// — including those on the shared edge — must be composited exactly once: a missing
// top-left fill rule double-blends the seam (darker/over-saturated), a wrong one drops
// slivers (background shows through).
TEST(transparency, shared_edge_no_double_blend)
{
    Renderer r(1);
    Mesh m;
    const vec3 bl{ -2.0f, -2.0f, 0.0f };
    const vec3 br{ 2.0f, -2.0f, 0.0f };
    const vec3 tr{ 2.0f, 2.0f, 0.0f };
    const vec3 tl{ -2.0f, 2.0f, 0.0f };
    add_unlit_tri(m, bl, br, tr, RED, 0.5f, true); // shares edge bl–tr
    add_unlit_tri(m, bl, tr, tl, RED, 0.5f, true);
    finalize(m, 0);

    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, true);
    fb.clear({ 0, 0, 0 });
    r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);

    // The quad projects to roughly x∈[16,24], y∈[6,14]; scan a safe interior block that
    // straddles the diagonal. Single blend → r≈128; double → r≈191; dropped → r≈0.
    for (int y = 8; y <= 12; y++)
    {
        for (int x = 18; x <= 22; x++)
        {
            const Color c = fb.get_pixel(x, y);
            ASSERT_TRUE(c.r >= 118 && c.r <= 138);
            ASSERT_TRUE(c.g < 10 && c.b < 10);
        }
    }
}

// ─── bounding-box-limited resolve ────────────────────────────────────────────────

// The resolve pass sweeps only the transparent region (the merged per-worker touched-pixel
// box), not the whole framebuffer. A single small blend triangle must composite inside its
// projection while every pixel outside the box keeps the exact background — proving the box
// neither under-covers (drops the composite) nor over-covers (touches the background).
TEST(transparency, localized_blend_leaves_outside_untouched)
{
    Renderer r(4); // multi-threaded: per-worker boxes get merged
    Mesh m;
    // Unit triangle at z=0 → screen x∈[18,22], y∈[8,12] (see make_test_camera).
    add_unlit_tri(m, { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, RED, 0.5f, /*blend=*/true);
    finalize(m, /*opaque_count=*/0);

    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear({ 0, 0, 255 });
    r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);

    // Inside the triangle: 0.5*red + 0.5*blue = (128, 0, 128).
    assert_pixel_near(fb, 20, 10, { 128, 0, 128 }, 2);
    // Far outside the box: exact background, byte-for-byte (never visited by resolve).
    for (const auto &p : { std::pair{ 2, 2 }, std::pair{ 38, 2 }, std::pair{ 2, 18 }, std::pair{ 38, 18 } })
    {
        const Color c = fb.get_pixel(p.first, p.second);
        ASSERT_TRUE(c.r == 0 && c.g == 0 && c.b == 255);
    }
}

// Two blend triangles in separated screen regions: the merged box must span both, so both
// composite. The gap between them stays background (it is inside the box's bounds but no
// fragment landed there, so its head stayed SENTINEL).
TEST(transparency, two_disjoint_regions_both_resolved)
{
    Renderer r(4);
    Mesh m;
    // Left region → screen x∈[12,16]; right region → x∈[24,28]; gap at x=20.
    add_unlit_tri(m, { -4.0f, -1.0f, 0.0f }, { -2.0f, -1.0f, 0.0f }, { -3.0f, 1.0f, 0.0f }, RED, 0.5f, /*blend=*/true);
    add_unlit_tri(m, { 2.0f, -1.0f, 0.0f }, { 4.0f, -1.0f, 0.0f }, { 3.0f, 1.0f, 0.0f }, RED, 0.5f, /*blend=*/true);
    finalize(m, /*opaque_count=*/0);

    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear({ 0, 0, 255 });
    r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);

    assert_pixel_near(fb, 14, 10, { 128, 0, 128 }, 4); // left region composited
    assert_pixel_near(fb, 26, 10, { 128, 0, 128 }, 4); // right region composited
    const Color gap = fb.get_pixel(20, 10);            // gap between them: untouched background
    ASSERT_TRUE(gap.r == 0 && gap.g == 0 && gap.b == 255);
}

// Each worker accumulates its touched-pixel box in a worker-local variable (kept off the
// shared, cache-line-adjacent box array to avoid false sharing under heavy overdraw); render()
// then merges the per-worker boxes. This stresses that merge: four high-overdraw clusters in
// the screen corners, with enough stacked triangles (256 total > 4 work-stealing chunks) that
// the clusters land on different workers. Every cluster must composite — dropping or mismerging
// any worker's box would leave that corner at the background — while the central gap, touched by
// no worker, stays the exact background.
TEST(transparency, multi_worker_box_merge_covers_all_regions)
{
    Renderer r(4);
    Mesh m;
    // Cluster centres (world); map to screen via screen_x = 20 + 2x, screen_y = 10 - 2y.
    // 64 stacked layers each → 256 triangles → spans 4 chunks (chunk size 64).
    for (const vec3 &c : { vec3{ -5.0f, 2.5f, 0.0f },   // TL ≈ (10, 5)
                           vec3{ 5.0f, 2.5f, 0.0f },    // TR ≈ (30, 5)
                           vec3{ -5.0f, -2.5f, 0.0f },  // BL ≈ (10, 15)
                           vec3{ 5.0f, -2.5f, 0.0f } }) // BR ≈ (30, 15)
    {
        for (int layer = 0; layer < 64; layer++)
        {
            add_unlit_tri(
                m, { c.x - 1.0f, c.y - 1.0f, c.z }, { c.x + 1.0f, c.y - 1.0f, c.z }, { c.x, c.y + 1.0f, c.z }, RED,
                0.5f, /*blend=*/true
            );
        }
    }
    finalize(m, /*opaque_count=*/0);

    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, /*headless=*/true);
    fb.clear({ 0, 0, 255 });
    r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);

    // 64 layers of 0.5-over-red drive each corner cluster to ~fully red (blue ≈ 0.5^64).
    for (const auto &p : { std::pair{ 10, 5 }, std::pair{ 30, 5 }, std::pair{ 10, 15 }, std::pair{ 30, 15 } })
    {
        const Color c = fb.get_pixel(p.first, p.second);
        ASSERT_TRUE(c.r > 240 && c.b < 30);
    }
    // Central gap: never touched by any worker → exact background.
    const Color mid = fb.get_pixel(20, 10);
    ASSERT_TRUE(mid.r == 0 && mid.g == 0 && mid.b == 255);
}

// ─── interaction with opaque geometry ───────────────────────────────────────────

// A transparent triangle behind an opaque one is occluded (depth test vs the opaque
// buffer culls it): the pixel keeps the opaque colour.
TEST(transparency, occluded_by_opaque)
{
    Renderer r(1);
    Mesh m;
    add_flat_large(m, 0.0f, RED, 1.0f, /*blend=*/false);   // opaque, nearer
    add_flat_large(m, -1.0f, GREEN, 0.5f, /*blend=*/true); // transparent, behind
    finalize(m, /*opaque_count=*/1);

    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, true);
    fb.clear({ 0, 0, 0 });
    r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    assert_pixel_near(fb, 20, 10, { 255, 0, 0 }, 2);
}

// A transparent triangle in front of an opaque one blends over it.
TEST(transparency, blends_in_front_of_opaque)
{
    Renderer r(1);
    Mesh m;
    add_flat_large(m, -1.0f, RED, 1.0f, /*blend=*/false); // opaque, farther
    add_flat_large(m, 0.0f, GREEN, 0.5f, /*blend=*/true); // transparent, nearer
    finalize(m, /*opaque_count=*/1);

    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, true);
    fb.clear({ 0, 0, 0 });
    r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    // 0.5*green + 0.5*red = (128,128,0)
    assert_pixel_near(fb, 20, 10, { 128, 128, 0 }, 2);
}

// The transparent resolve composites over the opaque colour ALREADY stored in the framebuffer,
// which was tonemapped once at the opaque commit. The resolve must not tonemap it again (that would
// double-darken the background under glass). Verify by laying a near-zero-alpha overlay over a LIT
// (tonemapped, >1) opaque base: the covered pixel must stay ~equal to the same base rendered with no
// overlay. A double-tonemap would drop it ~25 levels.
TEST(transparency, resolve_does_not_double_tonemap_opaque_base)
{
    Camera cam = make_test_camera();
    Light light = make_key_light_z({ 1.0f, 1.0f, 1.0f }); // white key, N.L = 1 at centre
    const vec3 ambient{ 0.4f, 0.4f, 0.4f };               // lit centre = 0.4 + 1.0 = 1.4 (> 1, tonemapped)

    // Base only: a lit white opaque triangle (no overlay). Default flags → opaque_count = all.
    Renderer r0(1);
    Mesh base = make_large_triangle();
    base.materials[0].specular = { 0.0f, 0.0f, 0.0f }; // isolate diffuse+ambient for a clean value
    Framebuffer fb_base(40, 20, /*headless=*/true);
    fb_base.clear({ 0, 0, 0 });
    r0.render(base, cam, &light, 1, ambient, fb_base);
    const Color p = fb_base.get_pixel(20, 10);
    ASSERT_TRUE(p.r < 254); // sanity: the base really is tonemapped (rolled off below white)

    // Same base + a near-zero-alpha unlit white overlay in front (z=0.5 > the base's z=0).
    Renderer r1(1);
    Mesh over = make_large_triangle();
    over.materials[0].specular = { 0.0f, 0.0f, 0.0f };
    add_unlit_tri(
        over, { -4.0f, -4.0f, 0.5f }, { 4.0f, -4.0f, 0.5f }, { 0.0f, 4.0f, 0.5f }, vec3{ 1.0f, 1.0f, 1.0f },
        /*alpha=*/0.02f, /*blend=*/true
    );
    finalize(over, /*opaque_count=*/1);
    Framebuffer fb_over(40, 20, /*headless=*/true);
    fb_over.clear({ 0, 0, 0 });
    r1.render(over, cam, &light, 1, ambient, fb_over);
    const Color q = fb_over.get_pixel(20, 10);

    // The 2%-alpha overlay barely shifts the pixel; a re-tonemap of the base would not be subtle.
    assert_pixel_near(fb_over, 20, 10, p, 3);
    ASSERT_TRUE(q.r > p.r - 10); // explicitly rules out the ~25-level double-tonemap darkening
}

// ─── per-vertex alpha ───────────────────────────────────────────────────────────

// Per-vertex opacity (COLOR_0 / PLY alpha) folds into the fragment alpha. A uniform
// vertex alpha of 0.5 on an otherwise-opaque (mat.alpha=1) red triangle halves it.
TEST(transparency, vertex_alpha_modulates)
{
    Renderer r(1);
    Mesh m;
    add_unlit_tri(
        m, { -4.0f, -4.0f, 0.0f }, { 4.0f, -4.0f, 0.0f }, { 0.0f, 4.0f, 0.0f }, RED, /*alpha=*/1.0f,
        /*blend=*/true, /*vcol_a=*/0.5f
    );
    finalize(m, 0);

    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, true);
    fb.clear({ 0, 0, 0 });
    r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    // alpha = mat.alpha(1) * tex(1) * vcol_a(0.5) = 0.5 → 0.5*red over black
    assert_pixel_near(fb, 20, 10, { 128, 0, 0 }, 2);
}

// ─── opaque path unaffected ─────────────────────────────────────────────────────

// A purely opaque mesh (no blend material) skips the transparent passes and renders
// its solid colour unchanged.
TEST(transparency, opaque_only_unchanged)
{
    Renderer r(1);
    Mesh m;
    add_flat_large(m, 0.0f, RED, 1.0f, /*blend=*/false);
    finalize(m, /*opaque_count=*/1);
    ASSERT_FALSE(m.has_transparent);

    Camera cam = make_test_camera();
    Framebuffer fb(40, 20, true);
    fb.clear({ 0, 0, 0 });
    r.render(m, cam, nullptr, 0, { 0.0f, 0.0f, 0.0f }, fb);
    assert_pixel_near(fb, 20, 10, { 255, 0, 0 }, 2);
}

// ─── loaders ────────────────────────────────────────────────────────────────────

// MTL dissolve (d < 1) marks the material as blended and records the opacity.
TEST(transparency, mtl_dissolve_sets_blend)
{
    TmpFile mtl(tmp_path("rast_blend.mtl"), "newmtl glass\nKd 0.2 0.4 0.8\nd 0.5\n");
    TmpFile obj(
        tmp_path("rast_blend.obj"), "mtllib rast_blend.mtl\n"
                                    "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                    "usemtl glass\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.has_transparent);
    const Material &mat = m.materials[1];
    ASSERT_TRUE(mat.blend);
    ASSERT_NEAR(mat.alpha, 0.5f, 1e-5f);
}

// d == 1 (default) stays opaque.
TEST(transparency, mtl_opaque_default)
{
    TmpFile mtl(tmp_path("rast_opaque.mtl"), "newmtl solid\nKd 0.2 0.4 0.8\n");
    TmpFile obj(
        tmp_path("rast_opaque.obj"), "mtllib rast_opaque.mtl\n"
                                     "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                     "usemtl solid\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_FALSE(m.has_transparent);
    ASSERT_FALSE(m.materials[1].blend);
}

// Mixed opaque + blend materials: load_model partitions triangles per-triangle so opaque
// ones occupy [0, opaque_count). File order lists the glass (blend) face first to prove the
// partition moves it to the tail; mat_at still resolves each triangle's original material.
TEST(transparency, material_blend_partitions_triangles)
{
    TmpFile mtl(
        tmp_path("rast_mix.mtl"), "newmtl glass\nKd 0.2 0.4 0.8\nd 0.5\n"
                                  "newmtl solid\nKd 0.9 0.1 0.1\n"
    );
    TmpFile obj(
        tmp_path("rast_mix.obj"), "mtllib rast_mix.mtl\n"
                                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\n"
                                  "usemtl glass\nf 1 2 3\n"
                                  "usemtl solid\nf 1 2 4\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.has_transparent);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    ASSERT_EQ(m.opaque_count, static_cast<uint32_t>(1));
    // Front range is opaque, tail is blend.
    ASSERT_FALSE(m.mat_at(m.triangles[0].material_idx).blend);
    ASSERT_TRUE(m.mat_at(m.triangles[1].material_idx).blend);
}

// PLY per-vertex alpha is read; a sub-opaque value marks the mesh blended.
TEST(transparency, ply_vertex_alpha)
{
    TmpFile ply(
        tmp_path("rast_alpha.ply"), "ply\nformat ascii 1.0\n"
                                    "element vertex 3\n"
                                    "property float x\nproperty float y\nproperty float z\n"
                                    "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                    "property uchar alpha\n"
                                    "element face 1\n"
                                    "property list uchar int vertex_indices\n"
                                    "end_header\n"
                                    "0 0 0 255 0 0 128\n"
                                    "1 0 0 255 0 0 128\n"
                                    "0 1 0 255 0 0 128\n"
                                    "3 0 1 2\n"
    );
    Mesh m = load_ok(ply.path);
    ASSERT_TRUE(m.has_vertex_alpha);
    ASSERT_TRUE(m.has_transparent);
    ASSERT_EQ(m.vertex_alpha.size(), m.vertices.size());
    for (float a : m.vertex_alpha)
    {
        ASSERT_NEAR(a, 128.0f / 255.0f, 1e-3f);
    }
}

// Per-triangle partition: a PLY with one fully-opaque triangle (all vertex alpha == 255) and
// one translucent triangle must keep the opaque one in [0, opaque_count) — only the translucent
// triangle is routed to the transparent pass, so the opaque region still casts shadows / takes
// the fast path. (Per-material routing would have flipped the whole single-material mesh.)
TEST(transparency, ply_vertex_alpha_per_triangle_partition)
{
    TmpFile ply(
        tmp_path("rast_alpha_mix.ply"), "ply\nformat ascii 1.0\n"
                                        "element vertex 6\n"
                                        "property float x\nproperty float y\nproperty float z\n"
                                        "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                        "property uchar alpha\n"
                                        "element face 2\n"
                                        "property list uchar int vertex_indices\n"
                                        "end_header\n"
                                        "0 0 0 255 0 0 255\n" // opaque triangle (alpha 255)
                                        "1 0 0 255 0 0 255\n"
                                        "0 1 0 255 0 0 255\n"
                                        "2 0 0 0 0 255 128\n" // translucent triangle (alpha 128)
                                        "3 0 0 0 0 255 128\n"
                                        "2 1 0 0 0 255 128\n"
                                        "3 0 1 2\n"
                                        "3 3 4 5\n"
    );
    Mesh m = load_ok(ply.path);
    ASSERT_TRUE(m.has_vertex_alpha);
    ASSERT_TRUE(m.has_transparent);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    // Exactly one triangle is opaque; it must sit in the opaque prefix.
    ASSERT_EQ(m.opaque_count, static_cast<uint32_t>(1));
    const Triangle &op = m.triangles[0];
    ASSERT_TRUE(m.vertex_alpha[op.v[0]] >= 1.0f);
    ASSERT_TRUE(m.vertex_alpha[op.v[1]] >= 1.0f);
    ASSERT_TRUE(m.vertex_alpha[op.v[2]] >= 1.0f);
    const Triangle &tr = m.triangles[1];
    const bool tr_translucent =
        m.vertex_alpha[tr.v[0]] < 1.0f || m.vertex_alpha[tr.v[1]] < 1.0f || m.vertex_alpha[tr.v[2]] < 1.0f;
    ASSERT_TRUE(tr_translucent);
}

// Per-vertex alpha must also be read on the split-by-UV PLY path (face-list texcoords
// + vertex colors), not just the standard shared-vertex path.
TEST(transparency, ply_vertex_alpha_split_by_uv)
{
    TmpFile ply(
        tmp_path("rast_alpha_uv.ply"), "ply\nformat ascii 1.0\n"
                                       "element vertex 3\n"
                                       "property float x\nproperty float y\nproperty float z\n"
                                       "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                       "property uchar alpha\n"
                                       "element face 1\n"
                                       "property list uchar int vertex_indices\n"
                                       "property list uchar float texcoord\n"
                                       "end_header\n"
                                       "0 0 0 255 0 0 128\n"
                                       "1 0 0 255 0 0 128\n"
                                       "0 1 0 255 0 0 128\n"
                                       "3 0 1 2 6 0 0 1 0 0 1\n"
    );
    Mesh m = load_ok(ply.path);
    ASSERT_TRUE(m.has_vertex_alpha);
    ASSERT_TRUE(m.has_transparent);
    ASSERT_EQ(m.vertex_alpha.size(), m.vertices.size());
    for (float a : m.vertex_alpha)
    {
        ASSERT_NEAR(a, 128.0f / 255.0f, 1e-3f);
    }
}

// glTF alphaMode=BLEND sets blend + base opacity from baseColorFactor.a.
TEST(transparency, gltf_alpha_mode_blend)
{
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                       "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                       "{\"POSITION\":0},\"material\":0}]}],\"materials\":[{\"alphaMode\":\"BLEND\","
                       "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.2,0.4,0.8,0.5]}}],"
                       "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                       "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                       "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                       "\"buffers\":[{\"byteLength\":36}]}";
    while (json.size() % 4 != 0)
    {
        json += ' ';
    }
    const auto jlen = static_cast<uint32_t>(json.size());

    std::string bin;
    for (float f : { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f })
    {
        emit_f32_le(bin, f);
    }
    const auto blen = static_cast<uint32_t>(bin.size());

    std::string glb;
    emit_u32_le(glb, 0x46546C67u);
    emit_u32_le(glb, 2u);
    emit_u32_le(glb, 12u + 8u + jlen + 8u + blen);
    emit_u32_le(glb, jlen);
    emit_u32_le(glb, 0x4E4F534Au);
    glb += json;
    emit_u32_le(glb, blen);
    emit_u32_le(glb, 0x004E4942u);
    glb += bin;

    TmpFile f(tmp_path("rast_blend.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_transparent);
    const Material &mat = m.materials[1];
    ASSERT_TRUE(mat.blend);
    ASSERT_NEAR(mat.alpha, 0.5f, 1e-4f);
    ASSERT_NEAR(mat.alpha_cutoff, 0.0f, 1e-6f); // BLEND must not set the cutout path
}

// KHR_materials_transmission (no alphaMode=BLEND) is approximated as alpha blending so
// glass renders see-through instead of opaque. transmissionFactor 1 floors to a small,
// non-zero alpha (the glass stays visible).
TEST(transparency, gltf_transmission_maps_to_blend)
{
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"KHR_materials_transmission\"],"
                       "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
                       "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],"
                       "\"materials\":[{\"extensions\":{\"KHR_materials_transmission\":{\"transmissionFactor\":1.0}}}],"
                       "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                       "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                       "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                       "\"buffers\":[{\"byteLength\":36}]}";
    while (json.size() % 4 != 0)
    {
        json += ' ';
    }
    const auto jlen = static_cast<uint32_t>(json.size());

    std::string bin;
    for (float f : { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f })
    {
        emit_f32_le(bin, f);
    }
    const auto blen = static_cast<uint32_t>(bin.size());

    std::string glb;
    emit_u32_le(glb, 0x46546C67u);
    emit_u32_le(glb, 2u);
    emit_u32_le(glb, 12u + 8u + jlen + 8u + blen);
    emit_u32_le(glb, jlen);
    emit_u32_le(glb, 0x4E4F534Au);
    glb += json;
    emit_u32_le(glb, blen);
    emit_u32_le(glb, 0x004E4942u);
    glb += bin;

    TmpFile f(tmp_path("rast_transmission.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_transparent);
    ASSERT_TRUE(m.materials[1].blend);
    ASSERT_TRUE(m.materials[1].alpha > 0.0f && m.materials[1].alpha < 0.5f);
    // Transmissive glass is forced double-sided so both shells of a closed mesh composite
    // (the approximation would otherwise drop the back faces and look thin).
    ASSERT_TRUE(m.materials[1].double_sided);
    ASSERT_TRUE(m.has_double_sided);
}

// glTF alphaMode=MASK still maps to the (opaque) cutout path, not blend.
TEST(transparency, gltf_alpha_mode_mask_not_blend)
{
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                       "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                       "{\"POSITION\":0},\"material\":0}]}],\"materials\":[{\"alphaMode\":\"MASK\","
                       "\"alphaCutoff\":0.25,\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1]}}],"
                       "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                       "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                       "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                       "\"buffers\":[{\"byteLength\":36}]}";
    while (json.size() % 4 != 0)
    {
        json += ' ';
    }
    const auto jlen = static_cast<uint32_t>(json.size());

    std::string bin;
    for (float f : { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f })
    {
        emit_f32_le(bin, f);
    }
    const auto blen = static_cast<uint32_t>(bin.size());

    std::string glb;
    emit_u32_le(glb, 0x46546C67u);
    emit_u32_le(glb, 2u);
    emit_u32_le(glb, 12u + 8u + jlen + 8u + blen);
    emit_u32_le(glb, jlen);
    emit_u32_le(glb, 0x4E4F534Au);
    glb += json;
    emit_u32_le(glb, blen);
    emit_u32_le(glb, 0x004E4942u);
    glb += bin;

    TmpFile f(tmp_path("rast_mask.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.has_transparent);
    ASSERT_FALSE(m.materials[1].blend);
    ASSERT_NEAR(m.materials[1].alpha_cutoff, 0.25f, 1e-4f);
}

// MASK + KHR_materials_transmission: the cutout (MASK) path wins; the material must NOT
// become a contradictory cutout+blend hybrid. It stays opaque with its alpha_cutoff intact.
TEST(transparency, gltf_mask_with_transmission_stays_cutout)
{
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"KHR_materials_transmission\"],"
                       "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
                       "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],"
                       "\"materials\":[{\"alphaMode\":\"MASK\",\"alphaCutoff\":0.25,"
                       "\"extensions\":{\"KHR_materials_transmission\":{\"transmissionFactor\":1.0}}}],"
                       "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                       "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                       "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                       "\"buffers\":[{\"byteLength\":36}]}";
    while (json.size() % 4 != 0)
    {
        json += ' ';
    }
    const auto jlen = static_cast<uint32_t>(json.size());

    std::string bin;
    for (float f : { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f })
    {
        emit_f32_le(bin, f);
    }
    const auto blen = static_cast<uint32_t>(bin.size());

    std::string glb;
    emit_u32_le(glb, 0x46546C67u);
    emit_u32_le(glb, 2u);
    emit_u32_le(glb, 12u + 8u + jlen + 8u + blen);
    emit_u32_le(glb, jlen);
    emit_u32_le(glb, 0x4E4F534Au);
    glb += json;
    emit_u32_le(glb, blen);
    emit_u32_le(glb, 0x004E4942u);
    glb += bin;

    TmpFile f(tmp_path("rast_mask_transmission.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.has_transparent);
    ASSERT_FALSE(m.materials[1].blend);
    ASSERT_NEAR(m.materials[1].alpha_cutoff, 0.25f, 1e-4f);
}

// glTF spec: an OPAQUE material ignores baseColor/COLOR_0 alpha. A vec4 COLOR_0 with alpha < 1
// under an OPAQUE material must NOT make the mesh transparent (and the wasted alpha array is
// dropped). Guards the loader gate that only honours COLOR_0 alpha under alphaMode=BLEND.
TEST(transparency, gltf_opaque_ignores_vertex_alpha)
{
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                       "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                       "{\"POSITION\":0,\"COLOR_0\":1},\"material\":0}]}],\"materials\":[{}],"
                       "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                       "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                       "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}],"
                       "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36},"
                       "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":48}],"
                       "\"buffers\":[{\"byteLength\":84}]}";
    while (json.size() % 4 != 0)
    {
        json += ' ';
    }
    const auto jlen = static_cast<uint32_t>(json.size());

    std::string bin;
    for (float f : { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f }) // 3 positions
    {
        emit_f32_le(bin, f);
    }
    for (int i = 0; i < 3; i++) // 3 colours, alpha 0.5 (must be ignored under OPAQUE)
    {
        emit_f32_le(bin, 1.0f);
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 0.5f);
    }
    const auto blen = static_cast<uint32_t>(bin.size());

    std::string glb;
    emit_u32_le(glb, 0x46546C67u);
    emit_u32_le(glb, 2u);
    emit_u32_le(glb, 12u + 8u + jlen + 8u + blen);
    emit_u32_le(glb, jlen);
    emit_u32_le(glb, 0x4E4F534Au);
    glb += json;
    emit_u32_le(glb, blen);
    emit_u32_le(glb, 0x004E4942u);
    glb += bin;

    TmpFile f(tmp_path("rast_opaque_vcol_alpha.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors); // rgb tint still read
    ASSERT_FALSE(m.has_vertex_alpha); // alpha ignored under OPAQUE
    ASSERT_FALSE(m.has_transparent);
}
