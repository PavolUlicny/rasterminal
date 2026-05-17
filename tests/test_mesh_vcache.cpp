#include "loader_util.h"
#include "test.h"

#include <cmath>
#include <vector>

// Tests for Mesh::optimize_vertex_cache() — the function is private, so all
// tests exercise it indirectly through load_model().
//
// Code paths covered:
//   n_tris < 2 early exit          — early_exit_single_triangle
//   nv == nt*3 early exit          — early_exit_fully_unshared_stl
//   max_adj < 2 early exit         — early_exit_max_adj_lt_2
//   Pass 1 linear-scan fallback    — geometry_* / vcache_order_* (first iteration)
//   Pass 1 cache-hit re-score      — geometry_* / vcache_order_* (subsequent iterations)
//   Pass 2 without vertex colors   — geometry_* (OBJ, no vcol)
//   Pass 2 with vertex colors      — vertex_colors_remapped_correctly (PLY with vcol)

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

TEST(vcache, early_exit_fully_unshared_stl)
{
    // STL always produces unshared vertices → nv == nt*3 (6 == 2*3) → second
    // guard fires before any Forsyth allocation.
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
    for (const auto &v : m.vertices)
        ASSERT_TRUE(std::isfinite(v.pos.x));
}

TEST(vcache, early_exit_max_adj_lt_2)
{
    // PLY: 7 vertices, 2 disconnected triangles (using vertices 0–1–2 and 3–4–5),
    // vertex 6 is unreferenced (pos 99,99,99).
    // nv=7 ≠ nt*3=6 so the nv==nt*3 guard passes; adj_count for every vertex is
    // at most 1 → max_adj=1 < 2 → third guard fires; no remap occurs.
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
    ASSERT_TRUE(m.vertices.size() == 7u);
    ASSERT_TRUE(m.triangles.size() == 2u);
    // Unreferenced vertex survives at index 6, position unchanged by the early exit.
    ASSERT_NEAR(m.vertices[6].pos.x, 99.0f, 1e-4f);
    ASSERT_NEAR(m.vertices[6].pos.y, 99.0f, 1e-4f);
    ASSERT_NEAR(m.vertices[6].pos.z, 99.0f, 1e-4f);
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
            if (std::abs(v.pos.x - ex) < 1e-4f && std::abs(v.pos.y - ey) < 1e-4f)
                found = true;
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
        for (uint32_t vi : tri.v)
            ASSERT_TRUE(vi < static_cast<uint32_t>(nv));
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
        ASSERT_NEAR(dot(m.tangents[i], m.vertices[i].normal), 0.0f, 1e-5f);
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
    // 3-triangle OBJ where the middle face (tri1) has the highest initial
    // Forsyth score and must be emitted first by the linear-scan fallback.
    //
    // Score derivation:
    //   v0 shared by tri0 and tri2  → adj_count=2 → val_score = 2/√2 ≈ 1.414
    //   all other vertices unique    → adj_count=1 → val_score = 2.0
    //   tscore[tri0] ≈ 1.414 + 2 + 2 = 5.414
    //   tscore[tri1] =   2   + 2 + 2 = 6.0   ← highest; linear scan picks this
    //   tscore[tri2] ≈ 1.414 + 2 + 2 = 5.414
    //
    // After Pass 1 tri1 is emitted first; Pass 2 remaps its vertices to indices
    // 0,1,2. Their positions are (3,0,0),(4,0,0),(3,1,0) — all at x≥3.
    // Sum of x-coordinates of the first triangle's vertices = 3+4+3 = 10.
    const std::string obj = "v  0 0 0\n" // shared (tri0, tri2); adj_count=2
                            "v  1 0 0\n" // only tri0
                            "v  0 1 0\n" // only tri0
                            "v  3 0 0\n" // only tri1
                            "v  4 0 0\n" // only tri1
                            "v  3 1 0\n" // only tri1
                            "v  0 0 1\n" // only tri2
                            "v  1 0 1\n" // only tri2
                            "f 1 2 3\n"  // tri0: tscore ≈ 5.414
                            "f 4 5 6\n"  // tri1: tscore  = 6.0 → emitted first
                            "f 1 7 8\n"; // tri2: tscore ≈ 5.414
    TmpFile f(tmp_path("rast_vc_order.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.triangles.size() == 3u);
    ASSERT_TRUE(m.vertices.size() == 8u);

    const auto &t0 = m.triangles[0];
    const float x_sum = m.vertices[t0.v[0]].pos.x + m.vertices[t0.v[1]].pos.x + m.vertices[t0.v[2]].pos.x;
    ASSERT_NEAR(x_sum, 10.0f, 1e-4f);
}

// ─── Group E: Remaining cache-path gaps ──────────────────────────────────────

TEST(vcache, cache_update_cur_zero)
{
    // Craft a mesh where Forsyth emits tri_A first with the shared vertex LAST
    // (tri.v[2]), so it lands at sim_cache[0]. tri_B then starts with the same
    // shared vertex (tri.v[0]), which is found at cache position 0 → cur=0.
    // The shift loop `for (k=0; k>0; k--)` doesn't execute; sim_cache[0]=v is a
    // no-op.  If the guard were ever changed to k>=0 it would write sim_cache[-1].
    //
    // Vertex loading order (tinyobjloader, first-encounter):
    //   f 2 3 1 → indices {0:(1,0,0), 1:(2,0,0), 2:(0,0,0)}  ← shared at v[2]
    //   f 1 4 5 → index 2 reused; new: {3:(3,0,0), 4:(4,0,0)}  ← shared at v[0]
    //
    // Both triangles have equal initial tscore (≈5.414); forward linear scan
    // selects tri_A (index 0) first.  Cache after tri_A: [2,1,0,−1,…].
    // tri_B's first vertex v=2 is found at cur=0.
    const std::string obj = "v 0 0 0\n"  // shared (pos 1)
                            "v 1 0 0\n"  // only tri_A (pos 2)
                            "v 2 0 0\n"  // only tri_A (pos 3)
                            "v 3 0 0\n"  // only tri_B (pos 4)
                            "v 4 0 0\n"  // only tri_B (pos 5)
                            "f 2 3 1\n"  // tri_A: shared vertex is tri.v[2]
                            "f 1 4 5\n"; // tri_B: shared vertex is tri.v[0] → cur=0 when processing it
    TmpFile f(tmp_path("rast_vc_cur0.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.triangles.size() == 2u);
    ASSERT_TRUE(m.vertices.size() == 5u);
    for (const auto &tri : m.triangles)
        for (uint32_t vi : tri.v)
            ASSERT_TRUE(vi < 5u);
    // Both triangles must reference the same remapped index for the shared vertex.
    ASSERT_TRUE(m.triangles[0].v[2] == m.triangles[1].v[0]);
}

TEST(vcache, cache_eviction_large_mesh)
{
    // Triangle strip with 34 vertices (2 rows of 17) and 32 triangles.
    // After ~33 unique vertices are inserted into the 32-slot cache, the next
    // insertion overwrites sim_cache[31] while it holds a valid vertex index
    // (not -1) — the first time genuine cache eviction fires in the test suite.
    // Assertions verify no vertex is corrupted or dropped as a result.
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
        const int bl = 2 * i + 1;
        const int tl = 2 * i + 2;
        const int br = 2 * i + 3;
        const int tr = 2 * i + 4;
        obj += "f " + std::to_string(bl) + " " + std::to_string(tl) + " " + std::to_string(br) + "\n";
        obj += "f " + std::to_string(tl) + " " + std::to_string(tr) + " " + std::to_string(br) + "\n";
    }
    TmpFile f(tmp_path("rast_vc_evict.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.triangles.size() == 32u);
    ASSERT_TRUE(m.vertices.size() == 34u);
    for (const auto &tri : m.triangles)
        for (uint32_t vi : tri.v)
            ASSERT_TRUE(vi < 34u);
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
                    found_bot = true;
                if (std::abs(v.pos.y - 1.0f) < 1e-4f)
                    found_top = true;
            }
        }
        ASSERT_TRUE(found_bot);
        ASSERT_TRUE(found_top);
    }
}
