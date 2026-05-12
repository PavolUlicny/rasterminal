#include "loader_util.h"
#include "test.h"

// All three functions (compute_normals, compute_tangents, compute_ao) are private
// and exercised indirectly through load_model with crafted OBJ strings.

// ─── compute_normals ──────────────────────────────────────────────────────────
// The OBJ loader calls compute_normals when the file contains no normal data.

TEST(normals, flat_xy_triangle_normal_points_along_z)
{
    // CCW triangle in the XY plane → face normal = cross((1,0,0),(0,1,0)) = (0,0,1).
    // All three vertices should get (0,0,1) after area-weighted averaging.
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
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
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
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
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
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
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
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
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
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
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
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
    const std::string obj =
        "v 0 0 0\n"
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
        ASSERT_NEAR(dot(t, vec3{0.0f, 0.0f, 1.0f}), 0.0f, 1e-5f);
    }
}

TEST(tangents, collapsed_uvs_use_safe_perpendicular_fallback)
{
    const std::string obj =
        "v 0 0 0\n"
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
        ASSERT_NEAR(dot(t, vec3{0.0f, 0.0f, 1.0f}), 0.0f, 1e-5f);
    }
}

// ─── compute_ao ───────────────────────────────────────────────────────────────

TEST(ao, all_values_in_unit_range)
{
    // ao is clamped to [1 − 0.15, 1] in the current implementation; verify [0,1].
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
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
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
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
    const std::string ply =
        "ply\n"
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
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
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
