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
    TmpFile f("/tmp/rast_norm_flat.obj", obj);
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
    TmpFile f("/tmp/rast_norm_unit.obj", obj);
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
    TmpFile f("/tmp/rast_norm_coplanar.obj", obj);
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
    TmpFile f("/tmp/rast_tan_unit.obj", obj);
    Mesh m = load_ok(f.path);
    for (const auto &v : m.vertices)
        ASSERT_NEAR(v.tangent.length(), 1.0f, 1e-5f);
}

TEST(tangents, tangents_are_orthogonal_to_normals)
{
    // Gram-Schmidt guarantees dot(T', N) == 0.
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\n"
        "f 1/1 2/2 3/3\n";
    TmpFile f("/tmp/rast_tan_orth.obj", obj);
    Mesh m = load_ok(f.path);
    for (const auto &v : m.vertices)
        ASSERT_NEAR(dot(v.tangent, v.normal), 0.0f, 1e-5f);
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
    TmpFile f("/tmp/rast_tan_dir.obj", obj);
    Mesh m = load_ok(f.path);
    for (const auto &v : m.vertices)
    {
        ASSERT_NEAR(v.tangent.x, 1.0f, 1e-5f);
        ASSERT_NEAR(v.tangent.y, 0.0f, 1e-5f);
        ASSERT_NEAR(v.tangent.z, 0.0f, 1e-5f);
    }
}

// ─── compute_ao ───────────────────────────────────────────────────────────────

TEST(ao, all_values_in_unit_range)
{
    // ao is clamped to [1 − 0.15, 1] in the current implementation; verify [0,1].
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "f 1 2 3\n";
    TmpFile f("/tmp/rast_ao_range.obj", obj);
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
    TmpFile f("/tmp/rast_ao_flat.obj", obj);
    Mesh m;
    bool ok = m.load_model(f.path, /*ao=*/true);
    ASSERT_TRUE(ok);
    for (const auto &v : m.vertices)
        ASSERT_NEAR(v.ao, 1.0f, 1e-5f);
}

// ─── Mesh::clear() ────────────────────────────────────────────────────────────

TEST(mesh_clear, empties_all_containers)
{
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "f 1 2 3\n";
    TmpFile f("/tmp/rast_clear.obj", obj);
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.vertices.empty());
    ASSERT_FALSE(m.triangles.empty());

    m.clear();
    ASSERT_TRUE(m.vertices.empty());
    ASSERT_TRUE(m.triangles.empty());
    ASSERT_TRUE(m.materials.empty());
    ASSERT_TRUE(m.textures.empty());
}
