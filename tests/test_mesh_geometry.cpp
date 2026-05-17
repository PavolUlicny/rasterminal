#include "loader_util.h"
#include "test.h"

#include <string>

// All three functions (compute_normals, compute_tangents, compute_ao) are private
// and exercised indirectly through load_model with crafted OBJ strings.

// ─── compute_normals ──────────────────────────────────────────────────────────
// The OBJ loader calls compute_normals when the file contains no normal data.

TEST(normals, flat_xy_triangle_normal_points_along_z)
{
    // CCW triangle in the XY plane → face normal = cross((1,0,0),(0,1,0)) = (0,0,1).
    // All three vertices should get (0,0,1) after area-weighted averaging.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                            "f 1 2 3\n";
    TmpFile f(tmp_path("rast_norm_flat.obj"), obj);
    Mesh m = load_ok(f.path);
    for (const auto &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.x, 0.0f, 1e-5f);
        ASSERT_NEAR(v.normal.y, 0.0f, 1e-5f);
        ASSERT_NEAR(v.normal.z, 1.0f, 1e-5f);
    }
}

TEST(normals, all_normals_have_unit_length)
{
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                            "f 1 2 3\n";
    TmpFile f(tmp_path("rast_norm_unit.obj"), obj);
    Mesh m = load_ok(f.path);
    for (const auto &v : m.vertices)
        ASSERT_NEAR(v.normal.length(), 1.0f, 1e-5f);
}

TEST(normals, two_coplanar_triangles_normal_consistent)
{
    // Two CCW triangles in the XY plane sharing an edge.
    // All face normals point along +Z, so every vertex normal — regardless of
    // whether the loader deduplicates — should also point along +Z.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                            "f 1 2 3\n"
                            "f 1 3 4\n";
    TmpFile f(tmp_path("rast_norm_coplanar.obj"), obj);
    Mesh m = load_ok(f.path);
    for (const auto &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.x, 0.0f, 1e-5f);
        ASSERT_NEAR(v.normal.y, 0.0f, 1e-5f);
        ASSERT_NEAR(v.normal.z, 1.0f, 1e-5f);
    }
}

// ─── compute_tangents ─────────────────────────────────────────────────────────
// Called by load_model after every successful load.

TEST(tangents, all_tangents_have_unit_length)
{
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\n"
                            "f 1/1 2/2 3/3\n";
    TmpFile f(tmp_path("rast_tan_unit.obj"), obj);
    Mesh m = load_ok(f.path);
    for (const auto &t : m.tangents)
        ASSERT_NEAR(t.length(), 1.0f, 1e-5f);
}

TEST(tangents, tangents_are_orthogonal_to_normals)
{
    // Gram-Schmidt guarantees dot(T', N) == 0.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\n"
                            "f 1/1 2/2 3/3\n";
    TmpFile f(tmp_path("rast_tan_orth.obj"), obj);
    Mesh m = load_ok(f.path);
    for (size_t i = 0; i < m.vertices.size(); i++)
        ASSERT_NEAR(dot(m.tangents[i], m.vertices[i].normal), 0.0f, 1e-5f);
}

TEST(tangents, tangent_aligns_with_uv_u_gradient)
{
    // Triangle in the XY plane with UVs that map +U to +X.
    // v0=(0,0,0) uv=(0,0), v1=(1,0,0) uv=(1,0), v2=(0,1,0) uv=(0,1).
    // T = (dp1*dv2 − dp2*dv1)/det = ((1,0,0)·1 − (0,1,0)·0)/1 = (1,0,0).
    // After Gram-Schmidt against normal (0,0,1): tangent = (1,0,0).
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\n"
                            "f 1/1 2/2 3/3\n";
    TmpFile f(tmp_path("rast_tan_dir.obj"), obj);
    Mesh m = load_ok(f.path);
    for (const auto &t : m.tangents)
    {
        ASSERT_NEAR(t.x, 1.0f, 1e-5f);
        ASSERT_NEAR(t.y, 0.0f, 1e-5f);
        ASSERT_NEAR(t.z, 0.0f, 1e-5f);
    }
}

TEST(tangents, missing_uvs_use_safe_perpendicular_fallback)
{
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "vn 0 0 1\n"
                            "vn 0 0 1\n"
                            "vn 0 0 1\n"
                            "f 1//1 2//2 3//3\n";
    TmpFile f(tmp_path("rast_tan_fallback.obj"), obj);
    Mesh m = load_ok(f.path);
    for (const auto &t : m.tangents)
    {
        ASSERT_NEAR(t.length(), 1.0f, 1e-5f);
        ASSERT_NEAR(dot(t, vec3{ 0.0f, 0.0f, 1.0f }), 0.0f, 1e-5f);
    }
}

TEST(tangents, collapsed_uvs_use_safe_perpendicular_fallback)
{
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "vn 0 0 1\n"
                            "vn 0 0 1\n"
                            "vn 0 0 1\n"
                            "vt 0.5 0.5\n"
                            "vt 0.5 0.5\n"
                            "vt 0.5 0.5\n"
                            "f 1/1/1 2/2/2 3/3/3\n";
    TmpFile f(tmp_path("rast_tan_collapse.obj"), obj);
    Mesh m = load_ok(f.path);
    for (const auto &t : m.tangents)
    {
        ASSERT_NEAR(t.length(), 1.0f, 1e-5f);
        ASSERT_NEAR(dot(t, vec3{ 0.0f, 0.0f, 1.0f }), 0.0f, 1e-5f);
    }
}

// ─── compute_ao ───────────────────────────────────────────────────────────────

TEST(ao, all_values_in_unit_range)
{
    // ao is clamped to [1 − 0.15, 1] in the current implementation; verify [0,1].
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                            "f 1 2 3\n";
    TmpFile f(tmp_path("rast_ao_range.obj"), obj);
    Mesh m;
    bool ok = m.load_model(f.path, /*ao=*/true);
    ASSERT_TRUE(ok);
    for (const auto &v : m.vertices)
    {
        ASSERT_TRUE(v.ao >= 0.0f);
        ASSERT_TRUE(v.ao <= 1.0f);
    }
}

TEST(ao, flat_triangle_vertices_are_one)
{
    // For a single flat triangle each vertex's centroid-to-normal projection is 0
    // (centroid lies in the same plane) → curvature = 0 → ao = 1.0.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                            "f 1 2 3\n";
    TmpFile f(tmp_path("rast_ao_flat.obj"), obj);
    Mesh m;
    bool ok = m.load_model(f.path, /*ao=*/true);
    ASSERT_TRUE(ok);
    for (const auto &v : m.vertices)
        ASSERT_NEAR(v.ao, 1.0f, 1e-5f);
}

TEST(ao, isolated_vertex_is_one_and_finite)
{
    // Create a mesh with an isolated fourth vertex (not referenced by any face).
    // The isolated vertex should get ao = 1.0 and remain finite.
    const std::string ply = "ply\n"
                            "format ascii 1.0\n"
                            "element vertex 4\n"
                            "property float x\n"
                            "property float y\n"
                            "property float z\n"
                            "element face 1\n"
                            "property list uchar int vertex_indices\n"
                            "end_header\n"
                            "0 0 0\n"
                            "1 0 0\n"
                            "0 1 0\n"
                            "10 10 10\n"
                            "3 0 1 2\n";
    TmpFile f(tmp_path("rast_ao_isolated.ply"), ply);
    Mesh m;
    bool ok = m.load_model(f.path, /*ao=*/true);
    ASSERT_TRUE(ok);
    ASSERT_NEAR(m.vertices[3].ao, 1.0f, 1e-5f);
    ASSERT_TRUE(std::isfinite(m.vertices[3].ao));
}

// ─── Mesh::clear() ────────────────────────────────────────────────────────────

TEST(mesh_clear, empties_all_containers)
{
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                            "f 1 2 3\n";
    TmpFile f(tmp_path("rast_clear.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.vertices.empty());
    ASSERT_FALSE(m.triangles.empty());

    m.clear();
    ASSERT_TRUE(m.vertices.empty());
    ASSERT_TRUE(m.triangles.empty());
    ASSERT_TRUE(m.materials.empty());
    ASSERT_TRUE(m.textures.empty());
}

TEST(mesh_clear, resets_flags)
{
    Mesh m;
    m.has_double_sided = true;
    m.has_vertex_colors = true;
    m.clear();
    ASSERT_FALSE(m.has_double_sided);
    ASSERT_FALSE(m.has_vertex_colors);
}

TEST(mesh_clear, idempotent)
{
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    TmpFile f(tmp_path("rast_idem.obj"), obj);
    Mesh m = load_ok(f.path);
    m.clear();
    m.clear(); // second clear must not crash or corrupt state
    ASSERT_TRUE(m.vertices.empty());
    ASSERT_TRUE(m.triangles.empty());
}

// ─── compute_normals edge cases ───────────────────────────────────────────────

TEST(normals, degenerate_zero_area_triangle_no_nan)
{
    // All three vertices coincident — cross product is (0,0,0).
    // Normals should stay at (0,0,0) after normalization, never NaN.
    const std::string obj = "v 1 2 3\nv 1 2 3\nv 1 2 3\nf 1 2 3\n";
    TmpFile f(tmp_path("rast_degen.obj"), obj);
    Mesh m = load_ok(f.path);
    for (const auto &v : m.vertices)
    {
        ASSERT_TRUE(std::isfinite(v.normal.x));
        ASSERT_TRUE(std::isfinite(v.normal.y));
        ASSERT_TRUE(std::isfinite(v.normal.z));
    }
}

TEST(normals, winding_order_determines_sign)
{
    // CCW winding (f 1 2 3) vs CW winding (f 1 3 2) on the same vertices
    // should produce normals pointing in opposite Z directions.
    const std::string ccw = "v -1 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const std::string cw = "v -1 0 0\nv 1 0 0\nv 0 1 0\nf 1 3 2\n";
    TmpFile f1(tmp_path("rast_ccw.obj"), ccw);
    TmpFile f2(tmp_path("rast_cw.obj"), cw);
    Mesh m1 = load_ok(f1.path);
    Mesh m2 = load_ok(f2.path);
    // Both normals should be unit length pointing in opposite Z directions.
    ASSERT_TRUE(m1.vertices[0].normal.z > 0.9f);
    ASSERT_TRUE(m2.vertices[0].normal.z < -0.9f);
}

TEST(normals, area_weighted_averaging)
{
    // OBJ vertex 1 (m.vertices[0]) is shared by two triangles:
    //   large XY triangle: face_normal magnitude ~100
    //   small XZ triangle: face_normal magnitude ~0.01
    // The accumulated normal is dominated by the large triangle → z > 0.99.
    const std::string obj = "v 0 0 0\n"
                            "v 10 0 0\n"
                            "v 0 10 0\n"
                            "v 0.1 0 0\n"
                            "v 0 0 0.1\n"
                            "f 1 2 3\n"
                            "f 1 4 5\n";
    TmpFile f(tmp_path("rast_norm_awt.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.vertices[0].normal.z > 0.99f);
}

// ─── compute_tangents: Z-up fallback branch ───────────────────────────────────

TEST(tangents, fallback_z_up_branch_for_non_z_normal)
{
    // Normal along +Y: abs(n.z)=0 < 0.9 → up={0,0,1} → t=normalize(cross({0,1,0},{0,0,1}))={1,0,0}.
    // Existing fallback tests use n=(0,0,1) which hits the X-up branch (abs(n.z)>=0.9);
    // this covers the complementary Z-up branch.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 0 1\n"
                            "vn 0 1 0\nvn 0 1 0\nvn 0 1 0\n"
                            "f 1//1 2//2 3//3\n";
    TmpFile f(tmp_path("rast_tan_yfb.obj"), obj);
    Mesh m = load_ok(f.path);
    for (size_t i = 0; i < m.tangents.size(); i++)
    {
        ASSERT_NEAR(m.tangents[i].length(), 1.0f, 1e-5f);
        ASSERT_NEAR(dot(m.tangents[i], m.vertices[i].normal), 0.0f, 1e-5f);
    }
}

TEST(tangents, zero_normal_fallback_is_finite)
{
    // All vertices coincident → compute_normals leaves every normal at {0,0,0}.
    // No UVs → tangent accumulation produces {0,0,0} → fallback branch fires.
    // cross({0,0,0}, up) = {0,0,0} → normalize({0,0,0}) = {0,0,0} (linalg guard).
    // Asserts the guard fires and no NaN escapes.
    const std::string obj = "v 0 0 0\nv 0 0 0\nv 0 0 0\nf 1 2 3\n";
    TmpFile f(tmp_path("rast_tan_zeronorm.obj"), obj);
    Mesh m = load_ok(f.path);
    for (const auto &t : m.tangents)
    {
        ASSERT_TRUE(std::isfinite(t.x));
        ASSERT_TRUE(std::isfinite(t.y));
        ASSERT_TRUE(std::isfinite(t.z));
    }
}

// ─── compute_ao: missing branches ────────────────────────────────────────────

TEST(ao, concave_vertex_darkened)
{
    // Pyramid pit: vertex 0 at origin, 4 rim vertices at z=1.
    // Centroid of vertex 0's neighbors = (0,0,1); normal = (0,0,1);
    // curvature = 1.0 → ao = 1 − clamp(0.5, 0, 0.15) = 0.85.
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 1\n"
                            "v 0 1 1\n"
                            "v -1 0 1\n"
                            "v 0 -1 1\n"
                            "f 1 2 3\n"
                            "f 1 3 4\n"
                            "f 1 4 5\n"
                            "f 1 5 2\n";
    TmpFile f(tmp_path("rast_ao_concave.obj"), obj);
    Mesh m;
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/true));
    ASSERT_NEAR(m.vertices[0].ao, 0.85f, 1e-4f);
}

TEST(ao, all_neighbors_coincident_fallback)
{
    // All three vertices at the same position → centroid == vertex pos → d.length() < 1e-8 → ao=1.0.
    const std::string obj = "v 0 0 0\nv 0 0 0\nv 0 0 0\nf 1 2 3\n";
    TmpFile f(tmp_path("rast_ao_coinc.obj"), obj);
    Mesh m;
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/true));
    for (const auto &v : m.vertices)
    {
        ASSERT_NEAR(v.ao, 1.0f, 1e-5f);
        ASSERT_TRUE(std::isfinite(v.ao));
    }
}

TEST(ao, disabled_skips_computation)
{
    // Concave geometry that gives ao=0.85 with ao=true, but ao=false bypasses
    // compute_ao entirely → all vertices keep the loader's default ao=1.0.
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 1\n"
                            "v 0 1 1\n"
                            "v -1 0 1\n"
                            "v 0 -1 1\n"
                            "f 1 2 3\n"
                            "f 1 3 4\n"
                            "f 1 4 5\n"
                            "f 1 5 2\n";
    TmpFile f(tmp_path("rast_ao_off.obj"), obj);
    Mesh m;
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/false));
    for (const auto &v : m.vertices)
        ASSERT_NEAR(v.ao, 1.0f, 1e-5f);
}

TEST(ao, stl_load_skips_compute_ao)
{
    // STL uses unshared vertices so compute_ao is explicitly skipped (ext=="stl" guard).
    // All vertex ao values must stay at the loader default of 1.0.
    TmpFile f(
        tmp_path("rast_ao_stl.stl"), "solid test\n"
                                     "facet normal 0 0 1\n"
                                     "  outer loop\n"
                                     "    vertex 0 0 0\n"
                                     "    vertex 1 0 0\n"
                                     "    vertex 0 1 0\n"
                                     "  endloop\n"
                                     "endfacet\n"
                                     "endsolid test\n"
    );
    Mesh m;
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/true));
    for (const auto &v : m.vertices)
        ASSERT_NEAR(v.ao, 1.0f, 1e-6f);
}

TEST(ao, ply_load_runs_compute_ao)
{
    // PLY + ao=true must run compute_ao() (unlike STL where ext=="stl" skips it).
    // Same concave-pit geometry as ao/concave_vertex_darkened: vertex 0 → ao = 0.85.
    const std::string ply = "ply\n"
                            "format ascii 1.0\n"
                            "element vertex 5\n"
                            "property float x\n"
                            "property float y\n"
                            "property float z\n"
                            "element face 4\n"
                            "property list uchar int vertex_indices\n"
                            "end_header\n"
                            "0 0 0\n"
                            "1 0 1\n"
                            "0 1 1\n"
                            "-1 0 1\n"
                            "0 -1 1\n"
                            "3 0 1 2\n"
                            "3 0 2 3\n"
                            "3 0 3 4\n"
                            "3 0 4 1\n";
    TmpFile f(tmp_path("rast_ao_ply_concave.ply"), ply);
    Mesh m;
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/true));
    ASSERT_NEAR(m.vertices[0].ao, 0.85f, 1e-4f);
}

TEST(ao, convex_vertex_stays_at_one)
{
    // Spike tip at origin, 4 base verts at z=-1.
    // Tip normal = (0,0,1); centroid of neighbors = (0,0,-1);
    // curvature = dot((0,0,-1),(0,0,1)) = -1 (convex) → clamp(-0.5,0,0.15)=0 → ao=1.0.
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 -1\n"
                            "v 0 1 -1\n"
                            "v -1 0 -1\n"
                            "v 0 -1 -1\n"
                            "f 1 2 3\n"
                            "f 1 3 4\n"
                            "f 1 4 5\n"
                            "f 1 5 2\n";
    TmpFile f(tmp_path("rast_ao_convex.obj"), obj);
    Mesh m;
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/true));
    ASSERT_NEAR(m.vertices[0].ao, 1.0f, 1e-5f);
}

TEST(mesh, load_model_sets_has_double_sided_false_for_obj)
{
    // std::any_of in load_model() checks all materials; OBJ never sets double_sided.
    // Verifies the flag is computed via the std::any_of path, not left at its clear() default.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    TmpFile f(tmp_path("rast_ds_obj.obj"), obj);
    Mesh m;
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/false));
    ASSERT_FALSE(m.has_double_sided);
}

// ─── load_model failure path ──────────────────────────────────────────────────

TEST(mesh_clear, failed_load_clears_previous_state)
{
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    TmpFile f(tmp_path("rast_fail_preload.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.vertices.empty());

    bool ok = m.load_model(tmp_path("rast_does_not_exist_xyz.obj"));
    ASSERT_FALSE(ok);
    ASSERT_TRUE(m.vertices.empty());
    ASSERT_TRUE(m.triangles.empty());
    ASSERT_TRUE(m.materials.empty());
}

// ─── mat_at / tex_at accessors ────────────────────────────────────────────────

TEST(mesh_accessors, mat_at_oob_returns_first_material)
{
    Mesh m;
    Material mat{};
    mat.diffuse = { 0.3f, 0.5f, 0.7f };
    m.materials.push_back(mat);

    const Material &r = m.mat_at(5u);
    ASSERT_NEAR(r.diffuse.x, 0.3f, 1e-6f);
    ASSERT_NEAR(r.diffuse.y, 0.5f, 1e-6f);
    ASSERT_NEAR(r.diffuse.z, 0.7f, 1e-6f);
}

TEST(mesh_accessors, tex_at_negative_and_oob_return_nullptr)
{
    Mesh m;
    m.textures.push_back(Texture{});

    ASSERT_TRUE(m.tex_at(-1) == nullptr);
    ASSERT_TRUE(m.tex_at(1) == nullptr);
    ASSERT_TRUE(m.tex_at(0) != nullptr);
}

// ─── compute_ao: multi-threaded path ─────────────────────────────────────────

TEST(ao, mt_matches_single_threaded)
{
    // 33×33 grid OBJ: 1089 vertices (≥ AO_PARALLEL_THRESHOLD=1024) so the
    // parallel AO path fires for n_threads=4.  optimize_vertex_cache always runs
    // sequentially (no MT path), so vertex ordering after Pass 2 is identical for
    // both thread counts — allowing direct index comparison of ao values.
    constexpr int N = 33;
    std::string obj;
    obj.reserve(65536);
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            obj += "v " + std::to_string(c) + " " + std::to_string(r) + " 0\n";
    for (int r = 0; r < N - 1; r++)
        for (int c = 0; c < N - 1; c++)
        {
            const int bl = r * N + c + 1;
            const int br = bl + 1;
            const int tl = bl + N;
            const int tr = tl + 1;
            obj += "f " + std::to_string(bl) + " " + std::to_string(br) + " " + std::to_string(tl) + "\n";
            obj += "f " + std::to_string(br) + " " + std::to_string(tr) + " " + std::to_string(tl) + "\n";
        }

    TmpFile f(tmp_path("rast_ao_mt.obj"), obj);
    Mesh m1;
    Mesh m4;
    ASSERT_TRUE(m1.load_model(f.path, /*ao=*/true, /*n_threads=*/1));
    ASSERT_TRUE(m4.load_model(f.path, /*ao=*/true, /*n_threads=*/4));

    ASSERT_TRUE(m1.vertices.size() == m4.vertices.size());
    for (size_t i = 0; i < m1.vertices.size(); i++)
        ASSERT_NEAR(m1.vertices[i].ao, m4.vertices[i].ao, 1e-6f);
}
