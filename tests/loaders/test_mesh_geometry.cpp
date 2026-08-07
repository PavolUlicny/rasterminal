#include "tests/loader_util.h"
#include "tests/test.h"

#include <cmath>
#include <string>

// All three functions (compute_normals, compute_tangents, compute_ao) are private
// and exercised indirectly through load_model with crafted OBJ strings.

// compute_normals
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
    {
        ASSERT_NEAR(v.normal.length(), 1.0f, 1e-5f);
    }
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

// compute_tangents
// Called by load_model after every successful load.

TEST(tangents, all_tangents_have_unit_length)
{
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\n"
                            "f 1/1 2/2 3/3\n";
    TmpFile f(tmp_path("rast_tan_unit.obj"), obj);
    Mesh m = load_ok(f.path);
    for (const auto &t : m.tangents)
    {
        ASSERT_NEAR(t.length(), 1.0f, 1e-5f);
    }
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
    {
        ASSERT_NEAR(dot(m.tangents[i], m.vertices[i].normal), 0.0f, 1e-5f);
    }
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

// compute_ao

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
    {
        ASSERT_NEAR(v.ao, 1.0f, 1e-5f);
    }
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

// Mesh::clear()

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

// compute_normals edge cases

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
    // Two triangles share edge (0,0,0)-(10,0,0) at a 45 deg fold (< 60 deg crease,
    // so the shared-edge vertices stay merged into one normal). The +Z triangle has
    // ~70x the area of the small tilted one, so the merged normal is dominated by
    // +Z. Without area weighting (equal-weight unit normals) the shared normal's z
    // would fall to ~0.92, so z > 0.99 specifically verifies area weighting.
    const std::string obj = "v 0 0 0\n"
                            "v 10 0 0\n"
                            "v 0 10 0\n"
                            "v 0 -0.1 0.1\n"
                            "f 1 2 3\n"
                            "f 2 1 4\n";
    TmpFile f(tmp_path("rast_norm_awt.obj"), obj);
    Mesh m = load_ok(f.path);
    bool found = false;
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            ASSERT_TRUE(v.normal.z > 0.99f);
            found = true;
        }
    }
    ASSERT_TRUE(found);
}

// compute_normals: crease-angle smoothing

TEST(normals, crease_splits_hard_edge)
{
    // Two faces share edge (0,0,0)-(1,0,0) at 90 deg (> 60 deg default crease).
    // Face A lies in XY (+Z normal), face B folds into XZ (+Y normal). The shared
    // edge vertices must split so each side keeps its own axis-aligned normal
    // instead of averaging to a 45 deg blend.
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 0 0 1\n"
                            "f 1 2 3\n"
                            "f 2 1 4\n";
    TmpFile f(tmp_path("rast_norm_crease_hard.obj"), obj);
    Mesh m = load_ok(f.path);
    int at_origin = 0;
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            at_origin++;
            // hard split → axis-aligned, never the (0,0.707,0.707) average
            ASSERT_TRUE(v.normal.z > 0.99f || v.normal.y > 0.99f);
        }
    }
    ASSERT_EQ(at_origin, 2); // the shared origin vertex split in two
}

TEST(normals, shallow_fold_stays_smooth)
{
    // Two faces share edge (0,0,0)-(1,0,0) at ~11 deg (< 60 deg crease) → they stay
    // merged: one vertex at the shared origin with a blended (averaged) normal.
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 0 -1 0.2\n"
                            "f 1 2 3\n"
                            "f 2 1 4\n";
    TmpFile f(tmp_path("rast_norm_crease_soft.obj"), obj);
    Mesh m = load_ok(f.path);
    int at_origin = 0;
    vec3 n{};
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            at_origin++;
            n = v.normal;
        }
    }
    ASSERT_EQ(at_origin, 1); // not split
    ASSERT_NEAR(n.length(), 1.0f, 1e-5f);
    ASSERT_TRUE(n.z > 0.9f && n.y > 0.0f); // blend of +Z and the tilted face
}

TEST(normals, crease_threshold_controls_split)
{
    // Same 90 deg fold; the crease angle decides whether the shared edge splits.
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 0 0 1\n"
                            "f 1 2 3\n"
                            "f 2 1 4\n";
    TmpFile f(tmp_path("rast_norm_crease_thresh.obj"), obj);

    auto count_origin = [](const Mesh &m) -> int
    {
        int c = 0;
        for (const auto &v : m.vertices)
        {
            if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
            {
                c++;
            }
        }
        return c;
    };

    Mesh smooth;
    ASSERT_TRUE(smooth.load_model(f.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/180.0f));
    ASSERT_EQ(count_origin(smooth), 1); // 90 deg < 180 deg threshold → merged

    Mesh hard;
    ASSERT_TRUE(hard.load_model(f.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/45.0f));
    ASSERT_EQ(count_origin(hard), 2); // 90 deg > 45 deg threshold → split
}

TEST(normals, crease_boundary_brackets_threshold)
{
    // Same 90 deg fold, but the crease angle now sits one degree on either side of the
    // dihedral to pin the comparison (cos_a >= crease_cos, mesh.cpp) right at the
    // boundary — earlier tests only bracket far away (45/180). The exact-90 case is left
    // unasserted: std::cos(to_radians(90)) is not exactly 0 in float, so equality there
    // is too fragile to depend on.
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 0 0 1\n"
                            "f 1 2 3\n"
                            "f 2 1 4\n";
    TmpFile f(tmp_path("rast_norm_crease_boundary.obj"), obj);

    auto count_origin = [](const Mesh &m) -> int
    {
        int c = 0;
        for (const auto &v : m.vertices)
        {
            if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
            {
                c++;
            }
        }
        return c;
    };

    Mesh just_below;
    ASSERT_TRUE(just_below.load_model(f.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/89.0f));
    ASSERT_EQ(count_origin(just_below), 2); // 90 deg > 89 deg threshold → split

    Mesh just_above;
    ASSERT_TRUE(just_above.load_model(f.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/91.0f));
    ASSERT_EQ(count_origin(just_above), 1); // 90 deg < 91 deg threshold → merged
}

TEST(normals, bowtie_point_share_splits)
{
    // Two triangles meet at ONLY vertex 0 (a point, no shared edge). They must
    // split into separate normals even at full smoothing (crease 180), because a
    // bowtie point is not a connected surface.
    const std::string obj = "v 0 0 0\n" // shared point (index 0)
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 0 0 1\n"
                            "v 0 1 1\n"
                            "f 1 2 3\n"  // +Z
                            "f 1 4 5\n"; // -X
    TmpFile f(tmp_path("rast_norm_bowtie.obj"), obj);
    Mesh m;
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/180.0f));
    int at_origin = 0;
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            at_origin++;
        }
    }
    ASSERT_EQ(at_origin, 2); // bowtie split despite full-smooth threshold
}

// compute_normals: smoothing groups (OBJ)
// When an OBJ authors `s` directives they are authoritative over the crease angle:
// faces smooth iff they share the same non-zero group; `s off`/`s 0` is faceted.
// Each test below uses geometry whose crease-angle outcome is the OPPOSITE of the
// group outcome, isolating the override.

TEST(normals, smoothing_group_splits_where_angle_would_merge)
{
    // Shallow ~11 deg fold (< 60 deg crease → angle alone keeps it merged), but the
    // two faces are in different smoothing groups → the shared origin must split.
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 0 -1 0.2\n"
                            "s 1\n"
                            "f 1 2 3\n"
                            "s 2\n"
                            "f 2 1 4\n";
    TmpFile f(tmp_path("rast_sg_split.obj"), obj);
    Mesh m = load_ok(f.path);
    int at_origin = 0;
    bool saw_z = false;
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            at_origin++;
            if (v.normal.z > 0.9999f)
            {
                saw_z = true;
            }
        }
    }
    ASSERT_EQ(at_origin, 2); // different groups override the shallow angle → split
    ASSERT_TRUE(saw_z);      // unblended +Z face normal (a merge would blend z down to ~0.995)
}

TEST(normals, smoothing_group_merges_where_angle_would_split)
{
    // 90 deg fold (> 60 deg crease → angle alone would split), but both faces share
    // group 1 → the shared origin stays a single vertex with a blended normal.
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 0 0 1\n"
                            "s 1\n"
                            "f 1 2 3\n"
                            "f 2 1 4\n";
    TmpFile f(tmp_path("rast_sg_merge.obj"), obj);
    Mesh m = load_ok(f.path);
    int at_origin = 0;
    vec3 n{};
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            at_origin++;
            n = v.normal;
        }
    }
    ASSERT_EQ(at_origin, 1); // same group overrides the steep angle → merged
    ASSERT_NEAR(n.length(), 1.0f, 1e-5f);
    ASSERT_TRUE(n.y > 0.4f && n.z > 0.4f); // blend of +Z and +Y, not axis-aligned
}

TEST(normals, smoothing_off_facets_below_crease)
{
    // Shallow ~11 deg fold (angle alone would merge), but `s off` on both faces is
    // explicit faceting → the origin splits. Exercises the directive scan: every
    // face id is 0, yet the file must be treated as group mode, not angle fallback.
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 0 -1 0.2\n"
                            "s off\n"
                            "f 1 2 3\n"
                            "f 2 1 4\n";
    TmpFile f(tmp_path("rast_sg_off.obj"), obj);
    Mesh m = load_ok(f.path);
    int at_origin = 0;
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            at_origin++;
        }
    }
    ASSERT_EQ(at_origin, 2); // `s off` facets despite the shallow angle
}

TEST(normals, smoothing_group_smooths_across_uv_seam)
{
    // 90 deg fold (angle alone would split) with a UV seam splitting the origin by
    // vt, and both faces in group 1. Groups override the angle AND weld across the
    // seam: both origin halves stay (UV preserved) sharing one blended normal.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\nvt 0.5 0.5\nvt 0.6 0.6\nvt 0.2 0.8\n"
                            "s 1\n"
                            "f 1/1 2/2 3/3\n"
                            "f 2/5 1/4 4/6\n";
    TmpFile f(tmp_path("rast_sg_seam.obj"), obj);
    Mesh m = load_ok(f.path);
    int at_origin = 0;
    vec3 first{};
    vec3 second{};
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            if (at_origin == 0)
            {
                first = v.normal;
            }
            else
            {
                second = v.normal;
            }
            at_origin++;
        }
    }
    ASSERT_EQ(at_origin, 2);               // UV split preserved
    ASSERT_NEAR(first.x, second.x, 1e-5f); // both halves share the welded normal
    ASSERT_NEAR(first.y, second.y, 1e-5f);
    ASSERT_NEAR(first.z, second.z, 1e-5f);
    ASSERT_TRUE(first.y > 0.4f && first.z > 0.4f); // blended, not a single face normal
}

TEST(normals, smoothing_group_differs_across_uv_seam_no_weld)
{
    // Same geometry as smoothing_group_smooths_across_uv_seam (90 deg fold, UV seam
    // splitting the origin), but the two faces are in DIFFERENT groups. The seam already
    // splits the origin by UV; the open question is whether the group machinery welds the
    // two halves' normals back together. Different groups must NOT unite — each half keeps
    // its own axis-aligned face normal, the opposite of the same-group welded case above.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\nvt 0.5 0.5\nvt 0.6 0.6\nvt 0.2 0.8\n"
                            "s 1\n"
                            "f 1/1 2/2 3/3\n"
                            "s 2\n"
                            "f 2/5 1/4 4/6\n";
    TmpFile f(tmp_path("rast_sg_seam_diff.obj"), obj);
    Mesh m = load_ok(f.path);
    int at_origin = 0;
    bool saw_z = false;
    bool saw_y = false;
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            at_origin++;
            if (v.normal.z > 0.9999f)
            {
                saw_z = true; // unblended +Z face (group 1)
            }
            if (v.normal.y > 0.9999f)
            {
                saw_y = true; // unblended +Y face (group 2)
            }
        }
    }
    ASSERT_EQ(at_origin, 2);     // UV split preserved
    ASSERT_TRUE(saw_z && saw_y); // each half kept its own face normal → no cross-group weld
}

TEST(normals, smoothing_group_smooths_across_object_boundary)
{
    // The same group 1 spans two `g` shapes. OBJ smoothing-group ids are file-global
    // (tinyobj keeps the active id across `g`/`o`), and the loader builds the
    // per-triangle id array across all shapes — so two same-group faces in different
    // shapes sharing an edge must merge at a 90 deg fold (where the angle would
    // split). Guards both the cross-shape semantics and the multi-shape build loop.
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 0 0 1\n"
                            "s 1\n"
                            "g shapeA\n"
                            "f 1 2 3\n"
                            "g shapeB\n"
                            "f 2 1 4\n";
    TmpFile f(tmp_path("rast_sg_obj_boundary.obj"), obj);
    Mesh m = load_ok(f.path);
    int at_origin = 0;
    vec3 n{};
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            at_origin++;
            n = v.normal;
        }
    }
    ASSERT_EQ(at_origin, 1); // same global group across `g` → merged, not split
    ASSERT_NEAR(n.length(), 1.0f, 1e-5f);
    ASSERT_TRUE(n.y > 0.4f && n.z > 0.4f); // blend of +Z and +Y
}

TEST(normals, smoothing_scan_ignores_comments_and_bare_s)
{
    // A `# s 1` comment and a value-less bare `s` are NOT smoothing directives, so
    // the file authors no groups → the crease-angle fallback must run. At a shallow
    // ~11 deg fold (< 60 deg) the origin stays merged. If the scan false-positived
    // into group mode, every id would be 0 and the origin would facet (split) → 2.
    const std::string obj = "# s 1\n"
                            "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 0 -1 0.2\n"
                            "s\n"
                            "f 1 2 3\n"
                            "f 2 1 4\n";
    TmpFile f(tmp_path("rast_sg_scan_noise.obj"), obj);
    Mesh m = load_ok(f.path);
    int at_origin = 0;
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            at_origin++;
        }
    }
    ASSERT_EQ(at_origin, 1); // angle fallback merged the shallow fold (no group mode)
}

TEST(normals, crease_split_syncs_vertex_colors)
{
    // 90 deg fold with per-vertex colors. The shared edge splits, and the split
    // copies must inherit the source color so vertex_colors stays the same length
    // as the (now larger) vertices array.
    const std::string obj = "v 0 0 0 0.2 0.4 0.6\n"
                            "v 1 0 0 0.2 0.4 0.6\n"
                            "v 0 1 0 0.2 0.4 0.6\n"
                            "v 0 0 1 0.2 0.4 0.6\n"
                            "f 1 2 3\n"
                            "f 2 1 4\n";
    TmpFile f(tmp_path("rast_norm_crease_vcol.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), m.vertices.size());
    ASSERT_TRUE(m.vertices.size() > 4u); // a split occurred
    for (const auto &c : m.vertex_colors)
    {
        ASSERT_NEAR(c.x, 0.2f, 1e-4f);
        ASSERT_NEAR(c.y, 0.4f, 1e-4f);
        ASSERT_NEAR(c.z, 0.6f, 1e-4f);
    }
}

TEST(normals, degenerate_face_in_fan_no_corruption)
{
    // A real +Z triangle shares edge (0,0,0)-(1,0,0) with a zero-area (collinear)
    // triangle. The degenerate face must not be smoothed into the real one and
    // must not produce NaN; the real wedge keeps its +Z normal.
    const std::string obj = "v 0 0 0\n"
                            "v 1 0 0\n"
                            "v 0 1 0\n"
                            "v 2 0 0\n" // collinear with 0,1 → degenerate tri
                            "f 1 2 3\n"
                            "f 1 2 4\n";
    TmpFile f(tmp_path("rast_norm_degen_fan.obj"), obj);
    Mesh m = load_ok(f.path);
    bool found_real = false;
    for (const auto &v : m.vertices)
    {
        ASSERT_TRUE(std::isfinite(v.normal.x) && std::isfinite(v.normal.y) && std::isfinite(v.normal.z));
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f && v.normal.z > 0.99f)
        {
            found_real = true;
        }
    }
    ASSERT_TRUE(found_real);
}

TEST(normals, high_valence_fan_apex_merges)
{
    // A flat disk fan: one apex shared by N triangles (apex valence = N), all
    // coplanar in z=0. Exercises the high-valence clustering path (N edge-runs,
    // transitive union around the fan) that low-valence tests never reach. All
    // faces are parallel so the apex merges to a single +Z vertex with no split.
    constexpr int N = 16;
    std::string obj = "v 0 0 0\n"; // apex = vertex 1
    for (int i = 0; i < N; i++)
    {
        const double a = 2.0 * 3.14159265358979323846 * i / N;
        obj += "v " + std::to_string(std::cos(a)) + " " + std::to_string(std::sin(a)) + " 0\n";
    }
    for (int i = 0; i < N; i++)
    {
        const int r0 = 2 + i;
        const int r1 = 2 + ((i + 1) % N);
        obj += "f 1 " + std::to_string(r0) + " " + std::to_string(r1) + "\n";
    }
    TmpFile f(tmp_path("rast_norm_fan.obj"), obj);
    Mesh m = load_ok(f.path);

    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(N));
    ASSERT_EQ(m.vertices.size(), static_cast<size_t>(N + 1)); // apex + rim, nothing split
    int apex_count = 0;
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            apex_count++;
            ASSERT_NEAR(v.normal.z, 1.0f, 1e-5f); // all faces coplanar -> +Z
        }
    }
    ASSERT_EQ(apex_count, 1); // the whole fan merged into one apex wedge
}

// compute_tangents: Z-up fallback branch

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

// compute_ao: missing branches

TEST(ao, concave_vertex_darkened)
{
    // Pyramid pit: vertex 0 at origin, 4 rim vertices at z=1.
    // Centroid of vertex 0's neighbors = (0,0,1); normal = (0,0,1); RMS edge = sqrt(2).
    // curvature = dot((0,0,1),(0,0,1)) / sqrt(2) = 0.707
    //   → ao = 1 − clamp(0.354, 0, 0.15) = 0.85 (still clamped, so the value is unchanged).
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
    // crease_angle 180 = full smoothing: the pit's faces stay merged at v0 so its
    // averaged normal is (0,0,1) and the curvature/AO calc matches the comment.
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/true, /*n_threads=*/1, /*crease_angle_deg=*/180.0f));
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
    {
        ASSERT_NEAR(v.ao, 1.0f, 1e-5f);
    }
}

TEST(ao, stl_load_runs_compute_ao)
{
    // STL now consumes stl_reader's shared (deduplicated) vertices, so compute_ao runs for STL
    // like every other format (the old ext=="stl" skip is gone). Same concave pyramid pit as
    // ao/concave_vertex_darkened and ao/ply_load_runs_compute_ao: a center vertex at (0,0,0)
    // shared by 4 facets, rim at z=1 → curvature 1/sqrt(2) → clamps to ao = 0.85. The center is written
    // bit-identically in every facet so stl_reader welds it into one shared, 4-incident vertex.
    auto facet = [](const char *a, const char *b, const char *c)
    {
        return std::string("facet normal 0 0 0\n  outer loop\n    vertex ") + a + "\n    vertex " + b +
               "\n    vertex " + c + "\n  endloop\nendfacet\n";
    };
    const std::string stl = "solid test\n" + facet("0 0 0", "1 0 1", "0 1 1") + facet("0 0 0", "0 1 1", "-1 0 1") +
                            facet("0 0 0", "-1 0 1", "0 -1 1") + facet("0 0 0", "0 -1 1", "1 0 1") + "endsolid test\n";
    TmpFile f(tmp_path("rast_ao_stl_concave.stl"), stl);
    Mesh m;
    // crease_angle 180 = full smoothing so the pit faces stay merged at the center (see above).
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/true, /*n_threads=*/1, /*crease_angle_deg=*/180.0f));

    // stl_reader sorts coords, so the center isn't vertices[0] — find it by position.
    int center = -1;
    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        const vec3 &p = m.vertices[i].pos;
        if (std::abs(p.x) < 1e-5f && std::abs(p.y) < 1e-5f && std::abs(p.z) < 1e-5f)
        {
            center = static_cast<int>(i);
            break;
        }
    }
    ASSERT_TRUE(center >= 0);
    ASSERT_NEAR(m.vertices[static_cast<size_t>(center)].ao, 0.85f, 1e-4f);
}

TEST(ao, ply_load_runs_compute_ao)
{
    // PLY + ao=true must run compute_ao(). Same concave-pit geometry as
    // ao/concave_vertex_darkened: vertex 0 → ao = 0.85.
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
    // crease_angle 180 = full smoothing so v0's pit faces stay merged (see above).
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/true, /*n_threads=*/1, /*crease_angle_deg=*/180.0f));
    ASSERT_NEAR(m.vertices[0].ao, 0.85f, 1e-4f);
}

TEST(ao, convex_vertex_stays_at_one)
{
    // Spike tip at origin, 4 base verts at z=-1.
    // Tip normal = (0,0,1); centroid of neighbors = (0,0,-1); RMS edge = sqrt(2).
    // curvature = dot((0,0,-1),(0,0,1)) / sqrt(2) = -0.707 (convex) → clamp(-0.354,0,0.15)=0 → ao=1.0.
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

TEST(ao, shallow_concavity_darkens_less_than_deep)
{
    // The point of the magnitude-aware curvature: depth matters relative to spacing, so a shallow
    // dip darkens far less than a deep one of the same width. The old normalized formula discarded
    // depth and gave both the SAME 0.85 — reverting to dot(normalize(d), N) fails this test.
    //
    // Same 4-facet pyramid pit as ao/concave_vertex_darkened, parameterized by rim height z.
    auto pit = [](const char *z)
    {
        return std::string("v 0 0 0\n") + "v 1 0 " + z + "\n" + "v 0 1 " + z + "\n" + "v -1 0 " + z + "\n" + "v 0 -1 " +
               z + "\n" + "f 1 2 3\nf 1 3 4\nf 1 4 5\nf 1 5 2\n";
    };

    // Deep pit, rim z=1: curvature 1/sqrt(2)=0.707 → clamps to 0.15 → ao 0.85 (as the sibling test).
    TmpFile deep_f(tmp_path("rast_ao_deep.obj"), pit("1"));
    Mesh deep;
    ASSERT_TRUE(deep.load_model(deep_f.path, /*ao=*/true, /*n_threads=*/1, /*crease_angle_deg=*/180.0f));
    ASSERT_NEAR(deep.vertices[0].ao, 0.85f, 1e-4f);

    // Shallow pit, rim z=0.1: centroid offset 0.1, RMS edge sqrt(1.01)=1.005, curvature 0.0995,
    // *0.5=0.0497 (below the 0.15 clamp) → ao = 1 - 0.0497 = 0.9503.
    TmpFile shallow_f(tmp_path("rast_ao_shallow.obj"), pit("0.1"));
    Mesh shallow;
    ASSERT_TRUE(shallow.load_model(shallow_f.path, /*ao=*/true, /*n_threads=*/1, /*crease_angle_deg=*/180.0f));
    ASSERT_NEAR(shallow.vertices[0].ao, 0.9503f, 1e-3f);

    // The shallow dip must be visibly lighter than the deep one (the regression the fix targets).
    ASSERT_TRUE(shallow.vertices[0].ao > deep.vertices[0].ao + 0.05f);
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

// load_model failure path

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

// mat_at / tex_at accessors

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

// compute_ao: multi-threaded path

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
    {
        for (int c = 0; c < N; c++)
        {
            obj += "v " + std::to_string(c) + " " + std::to_string(r) + " 0\n";
        }
    }
    for (int r = 0; r < N - 1; r++)
    {
        for (int c = 0; c < N - 1; c++)
        {
            const int bl = (r * N) + c + 1;
            const int br = bl + 1;
            const int tl = bl + N;
            const int tr = tl + 1;
            obj += "f " + std::to_string(bl) + " " + std::to_string(br) + " " + std::to_string(tl) + "\n";
            obj += "f " + std::to_string(br) + " " + std::to_string(tr) + " " + std::to_string(tl) + "\n";
        }
    }

    TmpFile f(tmp_path("rast_ao_mt.obj"), obj);
    Mesh m1;
    Mesh m4;
    ASSERT_TRUE(m1.load_model(f.path, /*ao=*/true, /*n_threads=*/1));
    ASSERT_TRUE(m4.load_model(f.path, /*ao=*/true, /*n_threads=*/4));

    ASSERT_TRUE(m1.vertices.size() == m4.vertices.size());
    for (size_t i = 0; i < m1.vertices.size(); i++)
    {
        ASSERT_NEAR(m1.vertices[i].ao, m4.vertices[i].ao, 1e-6f);
    }
}
