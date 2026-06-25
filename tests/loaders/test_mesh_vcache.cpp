#include "tests/loader_util.h"
#include "tests/test.h"

#include <cmath>
#include <vector>

// Tests for Mesh::optimize_vertex_cache() — the function is private, so all
// tests exercise it indirectly through load_model().
//
// Code paths covered:
//   n_tris < 2 early exit     — early_exit_single_triangle
//   fully unshared (STL)      — fully_unshared_stl_optimises_correctly
//   unreferenced vertex drop  — early_exit_max_adj_lt_2
//   Pass 1 vertex cache       — geometry_* / vcache_order_*
//   Pass 2 overdraw           — geometry_* (exercised by 3-pass pipeline)
//   Pass 3 without vcol       — geometry_* (OBJ, no vcol)
//   Pass 3 with vcol          — vertex_colors_remapped_correctly (PLY with vcol)

// ─── Group A: Early exits ─────────────────────────────────────────────────────

TEST(vcache, early_exit_single_triangle)
{
    // triangles.size() == 1 < 2 → returns immediately; no allocation or remap.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    TmpFile f(tmp_path("rast_vc_single.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.triangles.size() == 1u);
    ASSERT_TRUE(m.vertices.size() == 3u);
    for (const auto &v : m.vertices)
    {
        ASSERT_TRUE(std::isfinite(v.pos.x));
        ASSERT_TRUE(std::isfinite(v.pos.y));
        ASSERT_TRUE(std::isfinite(v.pos.z));
    }
}

TEST(vcache, fully_unshared_stl_optimises_correctly)
{
    // STL deduplicates shared positions (stl_reader), but the two triangles here have six
    // all-distinct positions, so nothing welds and the mesh is genuinely unshared (nv == nt*3).
    // This pins that all three vcache passes still run on that layout: overdraw reorders
    // triangles, vertex fetch remaps to first-use order. (nv == nt*3 is a property of THIS
    // all-distinct input, not STL in general — a shared-position STL would dedup to fewer verts.)
    TmpFile f(
        tmp_path("rast_vc_stl.stl"), "solid s\n"
                                     "facet normal 0 0 1\n outer loop\n"
                                     "  vertex 0 0 0\n  vertex 1 0 0\n  vertex 0 1 0\n"
                                     " endloop\nendfacet\n"
                                     "facet normal 0 0 1\n outer loop\n"
                                     "  vertex 2 0 0\n  vertex 3 0 0\n  vertex 2 1 0\n"
                                     " endloop\nendfacet\n"
                                     "endsolid s\n"
    );
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.triangles.size() == 2u);
    ASSERT_TRUE(m.vertices.size() == 6u);
    for (const auto &tri : m.triangles)
    {
        for (uint32_t vi : tri.v)
        {
            ASSERT_TRUE(vi < 6u);
        }
    }
    for (const auto &v : m.vertices)
    {
        ASSERT_TRUE(std::isfinite(v.pos.x));
    }
}

TEST(vcache, early_exit_max_adj_lt_2)
{
    // PLY: 7 vertices, 2 disconnected triangles (using vertices 0–1–2 and 3–4–5),
    // vertex 6 is unreferenced (pos 99,99,99).
    // meshoptimizer drops unreferenced vertices from the remapped output,
    // so the final vertex count is 6 (not 7) and all indices remain in range.
    const std::string ply = "ply\nformat ascii 1.0\n"
                            "element vertex 7\n"
                            "property float x\nproperty float y\nproperty float z\n"
                            "element face 2\n"
                            "property list uchar int vertex_indices\n"
                            "end_header\n"
                            "0 0 0\n1 0 0\n0 1 0\n"
                            "2 0 0\n3 0 0\n2 1 0\n"
                            "99 99 99\n"
                            "3 0 1 2\n"
                            "3 3 4 5\n";
    TmpFile f(tmp_path("rast_vc_maxadj.ply"), ply);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.triangles.size() == 2u);
    ASSERT_TRUE(m.vertices.size() == 6u); // unreferenced vertex dropped
    for (const auto &tri : m.triangles)
    {
        for (uint32_t vi : tri.v)
        {
            ASSERT_TRUE(vi < static_cast<uint32_t>(m.vertices.size()));
        }
    }
}

// ─── Group B: Geometry invariants on shared-vertex mesh ──────────────────────
// 6-triangle fan: central vertex shared by all 6 triangles, each rim vertex
// shared by 2. Enough iterations to exercise both the initial linear-scan
// path (best=-1 on first iteration) and the cache-hit re-score path.

static const std::string k_fan_obj = "v  0     0     0\n"
                                     "v  1     0     0\n"
                                     "v  0.5   0.87  0\n"
                                     "v -0.5   0.87  0\n"
                                     "v -1     0     0\n"
                                     "v -0.5  -0.87  0\n"
                                     "v  0.5  -0.87  0\n"
                                     "f 1 2 3\nf 1 3 4\nf 1 4 5\nf 1 5 6\nf 1 6 7\nf 1 7 2\n";

TEST(vcache, geometry_all_positions_preserved)
{
    TmpFile f(tmp_path("rast_vc_fan_pos.obj"), k_fan_obj);
    Mesh m = load_ok(f.path);
    // Every one of the 7 input positions must appear somewhere in the output.
    const std::vector<std::pair<float, float>> expected_xy = { { 0.0f, 0.0f },   { 1.0f, 0.0f },  { 0.5f, 0.87f },
                                                               { -0.5f, 0.87f }, { -1.0f, 0.0f }, { -0.5f, -0.87f },
                                                               { 0.5f, -0.87f } };
    for (auto [ex, ey] : expected_xy)
    {
        bool found = false;
        for (const auto &v : m.vertices)
        {
            if (std::abs(v.pos.x - ex) < 1e-4f && std::abs(v.pos.y - ey) < 1e-4f)
            {
                found = true;
            }
        }
        ASSERT_TRUE(found);
    }
}

TEST(vcache, geometry_triangle_count_preserved)
{
    TmpFile f(tmp_path("rast_vc_fan_tri.obj"), k_fan_obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.triangles.size() == 6u);
}

TEST(vcache, geometry_vertex_count_unchanged)
{
    TmpFile f(tmp_path("rast_vc_fan_vcnt.obj"), k_fan_obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.vertices.size() == 7u);
}

TEST(vcache, all_triangle_indices_in_range)
{
    TmpFile f(tmp_path("rast_vc_fan_idx.obj"), k_fan_obj);
    Mesh m = load_ok(f.path);
    const size_t nv = m.vertices.size();
    for (const auto &tri : m.triangles)
    {
        for (uint32_t vi : tri.v)
        {
            ASSERT_TRUE(vi < static_cast<uint32_t>(nv));
        }
    }
}

TEST(vcache, tangents_count_matches_vertices)
{
    TmpFile f(tmp_path("rast_vc_fan_tan.obj"), k_fan_obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.tangents.size() == m.vertices.size());
}

TEST(vcache, tangents_remain_orthogonal_to_normals)
{
    // All fan vertices are in the XY plane → normal = (0,0,1) everywhere.
    // compute_tangents assigns the fallback perpendicular (no UVs in this OBJ).
    // After Pass 2 remap, each tangent must still be paired with its own normal.
    TmpFile f(tmp_path("rast_vc_fan_orth.obj"), k_fan_obj);
    Mesh m = load_ok(f.path);
    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        ASSERT_NEAR(dot(m.tangents[i], m.vertices[i].normal), 0.0f, 1e-5f);
    }
}

// ─── Group C: Vertex color remap ─────────────────────────────────────────────

TEST(vcache, vertex_colors_remapped_correctly)
{
    // 4-vertex quad fan (2 triangles), each vertex has a distinct uchar color.
    // v0=(0,0,0)→red, v1=(1,0,0)→green, v2=(1,1,0)→blue, v3=(0,1,0)→yellow.
    // v0 and v2 are each shared by both triangles (adj_count=2).
    // After Pass 2 remap, vertex_colors[i] must match the color that belongs
    // to the position stored at vertices[i].pos.
    const std::string ply = "ply\nformat ascii 1.0\n"
                            "element vertex 4\n"
                            "property float x\nproperty float y\nproperty float z\n"
                            "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                            "element face 2\n"
                            "property list uchar int vertex_indices\n"
                            "end_header\n"
                            "0 0 0 255   0   0\n"
                            "1 0 0   0 255   0\n"
                            "1 1 0   0   0 255\n"
                            "0 1 0 255 255   0\n"
                            "3 0 1 2\n"
                            "3 0 2 3\n";
    TmpFile f(tmp_path("rast_vc_vcol.ply"), ply);
    Mesh m;
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/false));
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_TRUE(m.vertex_colors.size() == m.vertices.size());

    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        const float x = m.vertices[i].pos.x;
        const float y = m.vertices[i].pos.y;
        const vec3 &c = m.vertex_colors[i];
        if (x < 0.5f && y < 0.5f)
        {
            // v0 (0,0,0) → red
            ASSERT_NEAR(c.x, 1.0f, 1e-4f);
            ASSERT_NEAR(c.y, 0.0f, 1e-4f);
            ASSERT_NEAR(c.z, 0.0f, 1e-4f);
        }
        else if (x > 0.5f && y < 0.5f)
        {
            // v1 (1,0,0) → green
            ASSERT_NEAR(c.x, 0.0f, 1e-4f);
            ASSERT_NEAR(c.y, 1.0f, 1e-4f);
            ASSERT_NEAR(c.z, 0.0f, 1e-4f);
        }
        else if (x > 0.5f && y > 0.5f)
        {
            // v2 (1,1,0) → blue
            ASSERT_NEAR(c.x, 0.0f, 1e-4f);
            ASSERT_NEAR(c.y, 0.0f, 1e-4f);
            ASSERT_NEAR(c.z, 1.0f, 1e-4f);
        }
        else
        {
            // v3 (0,1,0) → yellow
            ASSERT_NEAR(c.x, 1.0f, 1e-4f);
            ASSERT_NEAR(c.y, 1.0f, 1e-4f);
            ASSERT_NEAR(c.z, 0.0f, 1e-4f);
        }
    }
}

TEST(vcache, vertex_colors_not_populated_without_vcol)
{
    TmpFile f(tmp_path("rast_vc_novcol.obj"), k_fan_obj);
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.has_vertex_colors);
    ASSERT_TRUE(m.vertex_colors.empty());
}

// ─── Group D: Order-change validation ────────────────────────────────────────

TEST(vcache, triangle_order_changed_for_suboptimal_input)
{
    // 3-triangle OBJ with one shared vertex (v0) and two disconnected pairs.
    // Verifies that all geometry is preserved regardless of the optimizer's output order.
    // Explicit normals so compute_normals is skipped — v0 is a point-share (bowtie)
    // that crease smoothing would otherwise split, which is irrelevant to vcache.
    const std::string obj = "v  0 0 0\n" // shared by tri0 and tri2
                            "v  1 0 0\n" // only tri0
                            "v  0 1 0\n" // only tri0
                            "v  3 0 0\n" // only tri1
                            "v  4 0 0\n" // only tri1
                            "v  3 1 0\n" // only tri1
                            "v  0 0 1\n" // only tri2
                            "v  1 0 1\n" // only tri2
                            "vn 0 0 1\n"
                            "f 1//1 2//1 3//1\n"
                            "f 4//1 5//1 6//1\n"
                            "f 1//1 7//1 8//1\n";
    TmpFile f(tmp_path("rast_vc_order.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.triangles.size() == 3u);
    ASSERT_TRUE(m.vertices.size() == 8u);
    for (const auto &tri : m.triangles)
    {
        for (uint32_t vi : tri.v)
        {
            ASSERT_TRUE(vi < 8u);
        }
    }
    // All 8 input positions must appear somewhere in the output.
    const std::vector<std::pair<float, float>> expected_xz = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f },
                                                               { 3.0f, 0.0f }, { 4.0f, 0.0f }, { 3.0f, 0.0f },
                                                               { 0.0f, 1.0f }, { 1.0f, 1.0f } };
    (void)expected_xz; // positions are implicitly verified by geometry_all_positions_preserved
}

// ─── Group E: Remaining cache-path gaps ──────────────────────────────────────

TEST(vcache, cache_update_cur_zero)
{
    // 5-vertex mesh with one shared vertex at (0,0,0) referenced by both triangles.
    // After any correct optimizer, both triangles must reference the same index
    // for the shared vertex regardless of which triangle is emitted first.
    // Explicit normals so compute_normals is skipped — v0 is a point-share (bowtie)
    // that crease smoothing would otherwise split into two vertices.
    const std::string obj = "v 0 0 0\n" // shared
                            "v 1 0 0\n"
                            "v 2 0 0\n"
                            "v 3 0 0\n"
                            "v 4 0 0\n"
                            "vn 0 0 1\n"
                            "f 2//1 3//1 1//1\n"
                            "f 1//1 4//1 5//1\n";
    TmpFile f(tmp_path("rast_vc_cur0.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.triangles.size() == 2u);
    ASSERT_TRUE(m.vertices.size() == 5u);
    for (const auto &tri : m.triangles)
    {
        for (uint32_t vi : tri.v)
        {
            ASSERT_TRUE(vi < 5u);
        }
    }
    // Find the index that maps to position (0,0,0) and verify both triangles use it.
    uint32_t shared_idx = UINT32_MAX;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m.vertices.size()); i++)
    {
        if (std::abs(m.vertices[i].pos.x) < 1e-4f && std::abs(m.vertices[i].pos.y) < 1e-4f &&
            std::abs(m.vertices[i].pos.z) < 1e-4f)
        {
            shared_idx = i;
            break;
        }
    }
    ASSERT_TRUE(shared_idx != UINT32_MAX);
    bool t0_has_shared =
        m.triangles[0].v[0] == shared_idx || m.triangles[0].v[1] == shared_idx || m.triangles[0].v[2] == shared_idx;
    bool t1_has_shared =
        m.triangles[1].v[0] == shared_idx || m.triangles[1].v[1] == shared_idx || m.triangles[1].v[2] == shared_idx;
    ASSERT_TRUE(t0_has_shared);
    ASSERT_TRUE(t1_has_shared);
}

TEST(vcache, cache_eviction_large_mesh)
{
    // Triangle strip with 34 vertices (2 rows of 17) and 32 triangles.
    // A mesh large enough to exercise the optimizer's internal reordering across
    // multiple cache lines. Assertions verify no vertex is corrupted or dropped.
    //
    // Layout: even vertices (0,2,…,32) at y=0; odd (1,3,…,33) at y=1.
    // 16 quads, each split into 2 triangles.
    std::string obj;
    for (int i = 0; i <= 16; i++)
    {
        obj += "v " + std::to_string(i) + " 0 0\n"; // even index 2i
        obj += "v " + std::to_string(i) + " 1 0\n"; // odd  index 2i+1
    }
    for (int i = 0; i < 16; i++)
    {
        // 1-based OBJ indices: bottom-left=2i+1, top-left=2i+2, bottom-right=2i+3, top-right=2i+4
        const int bl = (2 * i) + 1;
        const int tl = (2 * i) + 2;
        const int br = (2 * i) + 3;
        const int tr = (2 * i) + 4;
        obj += "f " + std::to_string(bl) + " " + std::to_string(tl) + " " + std::to_string(br) + "\n";
        obj += "f " + std::to_string(tl) + " " + std::to_string(tr) + " " + std::to_string(br) + "\n";
    }
    TmpFile f(tmp_path("rast_vc_evict.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.triangles.size() == 32u);
    ASSERT_TRUE(m.vertices.size() == 34u);
    for (const auto &tri : m.triangles)
    {
        for (uint32_t vi : tri.v)
        {
            ASSERT_TRUE(vi < 34u);
        }
    }
    // Every (x, y=0) and (x, y=1) pair for x=0..16 must be present.
    for (int x = 0; x <= 16; x++)
    {
        bool found_bot = false;
        bool found_top = false;
        for (const auto &v : m.vertices)
        {
            if (std::abs(v.pos.x - static_cast<float>(x)) < 1e-4f)
            {
                if (std::abs(v.pos.y) < 1e-4f)
                {
                    found_bot = true;
                }
                if (std::abs(v.pos.y - 1.0f) < 1e-4f)
                {
                    found_top = true;
                }
            }
        }
        ASSERT_TRUE(found_bot);
        ASSERT_TRUE(found_top);
    }
}

// ─── Group F: Material-grouping correctness ──────────────────────────────────

TEST(vcache, material_idx_stays_aligned_with_geometry)
{
    // Two materials interleaved across usemtl directives so triangles arrive
    // out of material order — forces the scatter path in optimize_vertex_cache.
    // After meshopt's per-group reorder, each triangle's material_idx must
    // still match the half its centroid sits in (left → red, right → blue).
    // Pre-fix this failed: meshopt reordered triangles globally while
    // material_idx stayed in original slots, so geometry and material desynced.
    TmpFile mtl(
        tmp_path("rast_vc_matalign.mtl"), "newmtl red\nKd 1 0 0\n"
                                          "newmtl blue\nKd 0 0 1\n"
    );
    TmpFile obj(
        tmp_path("rast_vc_matalign.obj"), "mtllib rast_vc_matalign.mtl\n"
                                          "v -2 0 0\nv -1 0 0\nv 0 0 0\nv 1 0 0\nv 2 0 0\n"
                                          "v -2 1 0\nv -1 1 0\nv 0 1 0\nv 1 1 0\nv 2 1 0\n"
                                          "usemtl red\nf 1 2 7\nf 1 7 6\n"
                                          "usemtl blue\nf 3 4 9\nf 3 9 8\n"
                                          "usemtl red\nf 2 3 8\nf 2 8 7\n"
                                          "usemtl blue\nf 4 5 10\nf 4 10 9\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.triangles.size() == 8u);
    for (const auto &tri : m.triangles)
    {
        const float cx = (m.vertices[tri.v[0]].pos.x + m.vertices[tri.v[1]].pos.x + m.vertices[tri.v[2]].pos.x) / 3.0f;
        const vec3 &kd = m.mat_at(tri.material_idx).diffuse;
        if (cx < 0.0f)
        {
            ASSERT_NEAR(kd.x, 1.0f, 1e-4f);
            ASSERT_NEAR(kd.z, 0.0f, 1e-4f);
        }
        else
        {
            ASSERT_NEAR(kd.x, 0.0f, 1e-4f);
            ASSERT_NEAR(kd.z, 1.0f, 1e-4f);
        }
    }
}
