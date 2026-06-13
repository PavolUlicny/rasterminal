#include "loader_util.h"
#include "inline_bmp.h"

#include <cmath>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════
//  HAND-CRAFTED VALID OBJ — exercises specific format features in isolation
// ═══════════════════════════════════════════════════════════════════════════

TEST(obj_valid, mtl_multi_material_loaded)
{
    TmpFile mtl(
        tmp_path("rast_cube.mtl"), "newmtl red\nKd 0.9 0.1 0.1\n"
                                   "newmtl blue\nKd 0.1 0.2 0.9\n"
    );
    TmpFile obj(
        tmp_path("rast_cube.obj"), "mtllib rast_cube.mtl\n"
                                   "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
                                   "usemtl red\nf 1 2 3\n"
                                   "usemtl blue\nf 1 2 4\n"
    );
    Mesh m = load_ok(obj.path);
    // Two usemtl directives → at least 2 non-default materials loaded.
    if (m.materials.size() < 2)
    {
        ASSERT_FAIL("multi-material OBJ should produce at least 2 materials");
    }
}

TEST(obj_valid, crease_split_across_multi_material)
{
    // A 90 deg fold whose two faces use different materials. compute_normals splits the
    // shared edge BEFORE optimize_vertex_cache runs its per-material-group reorder, so the
    // crease split and the per-group remap must both survive: each origin half keeps its
    // axis-aligned face normal, and the two triangles keep their distinct material_idx.
    TmpFile mtl(
        tmp_path("rast_crease_mm.mtl"), "newmtl red\nKd 0.9 0.1 0.1\n"
                                        "newmtl blue\nKd 0.1 0.2 0.9\n"
    );
    TmpFile obj(
        tmp_path("rast_crease_mm.obj"), "mtllib rast_crease_mm.mtl\n"
                                        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
                                        "usemtl red\nf 1 2 3\n"
                                        "usemtl blue\nf 2 1 4\n"
    );
    Mesh m = load_ok(obj.path);

    int at_origin = 0;
    bool saw_z = false;
    bool saw_y = false;
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            at_origin++;
            if (v.normal.z > 0.99f)
            {
                saw_z = true;
            }
            if (v.normal.y > 0.99f)
            {
                saw_y = true;
            }
        }
    }
    ASSERT_EQ(at_origin, 2);     // 90 deg > 60 deg crease → split despite the material boundary
    ASSERT_TRUE(saw_z && saw_y); // each half kept its own axis-aligned face normal

    uint32_t lo = m.triangles.front().material_idx;
    uint32_t hi = lo;
    for (const auto &t : m.triangles)
    {
        lo = std::min(lo, t.material_idx);
        hi = std::max(hi, t.material_idx);
    }
    ASSERT_TRUE(hi != lo); // two distinct materials still assigned after the per-group optimize
}

TEST(obj_valid, quad_multi_face_triangulates_to_twelve)
{
    // 6 quads (a cube) fan-triangulated → 12 triangles.
    TmpFile t(
        tmp_path("rast_cube6q.obj"), "v -1 -1  1\nv  1 -1  1\nv  1  1  1\nv -1  1  1\n"
                                     "v -1 -1 -1\nv  1 -1 -1\nv  1  1 -1\nv -1  1 -1\n"
                                     "f 1 2 3 4\n"
                                     "f 5 6 7 8\n"
                                     "f 1 2 6 5\n"
                                     "f 3 4 8 7\n"
                                     "f 1 4 8 5\n"
                                     "f 2 3 7 6\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 12 });
}

TEST(obj_valid, single_triangle)
{
    TmpFile t(
        tmp_path("rasterminal_test_tri.obj"), "v 0 0 0\n"
                                              "v 1 0 0\n"
                                              "v 0 1 0\n"
                                              "f 1 2 3\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(obj_valid, quad_fan_triangulates_to_two)
{
    TmpFile t(
        tmp_path("rasterminal_test_quad.obj"), "v 0 0 0\n"
                                               "v 1 0 0\n"
                                               "v 1 1 0\n"
                                               "v 0 1 0\n"
                                               "f 1 2 3 4\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 2 });
}

TEST(obj_valid, ngon_fan_triangulates_to_n_minus_two)
{
    // 5-gon → 3 triangles (fan: 1-2-3, 1-3-4, 1-4-5)
    TmpFile t(
        tmp_path("rasterminal_test_ngon.obj"), "v 0 0 0\n"
                                               "v 1 0 0\n"
                                               "v 2 1 0\n"
                                               "v 1 2 0\n"
                                               "v 0 1 0\n"
                                               "f 1 2 3 4 5\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 3 });
}

TEST(obj_valid, negative_indices_reference_end)
{
    // OBJ spec: negative indices count back from the current end of the list.
    TmpFile t(
        tmp_path("rasterminal_test_neg.obj"),
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f -3 -2 -1\n"
    ); // equivalent to "f 1 2 3"
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(obj_valid, uv_and_normal_references)
{
    TmpFile t(
        tmp_path("rasterminal_test_uvn.obj"), "v 0 0 0\n"
                                              "v 1 0 0\n"
                                              "v 0 1 0\n"
                                              "vt 0 0\n"
                                              "vt 1 0\n"
                                              "vt 0 1\n"
                                              "vn 0 0 1\n"
                                              "f 1/1/1 2/2/1 3/3/1\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(obj_valid, multiple_faces_deduplicate_shared_vertices)
{
    // Two triangles sharing an edge — the four unique FaceVertex keys
    // should produce exactly four vertices in the final buffer.
    TmpFile t(
        tmp_path("rasterminal_test_shared.obj"), "v 0 0 0\n"
                                                 "v 1 0 0\n"
                                                 "v 1 1 0\n"
                                                 "v 0 1 0\n"
                                                 "f 1 2 3\n"
                                                 "f 1 3 4\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{ 4 });
    ASSERT_EQ(m.triangles.size(), size_t{ 2 });
}

TEST(obj_valid, comments_and_blank_lines_ignored)
{
    TmpFile t(
        tmp_path("rasterminal_test_comments.obj"), "# this is a comment\n"
                                                   "\n"
                                                   "v 0 0 0\n"
                                                   "# another comment\n"
                                                   "v 1 0 0\n"
                                                   "\n"
                                                   "v 0 1 0\n"
                                                   "f 1 2 3\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

// ═══════════════════════════════════════════════════════════════════════════
//  REJECTIONS — malformed/corrupt OBJ must not crash
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, obj_only_comments)
{
    TmpFile t(
        tmp_path("rasterminal_test_cmts.obj"), "# just a comment\n"
                                               "# another one\n"
    );
    assert_rejects(t.path);
}

TEST(reject, obj_vertices_but_no_faces)
{
    TmpFile t(
        tmp_path("rasterminal_test_nofaces.obj"), "v 0 0 0\n"
                                                  "v 1 0 0\n"
                                                  "v 0 1 0\n"
    );
    assert_rejects(t.path);
}

TEST(reject, obj_all_face_indices_out_of_range)
{
    // Faces reference non-existent vertices → every face is skipped → no
    // triangles → load fails with "triangles.empty()".
    TmpFile t(
        tmp_path("rasterminal_test_oob.obj"), "v 0 0 0\n"
                                              "v 1 0 0\n"
                                              "f 5 6 7\n"
                                              "f 10 11 12\n"
    );
    assert_rejects(t.path);
}

TEST(reject, obj_oob_normal_index)
{
    // One vn declared; face references normal index 2 → bounds check rejects.
    TmpFile t(
        tmp_path("rasterminal_test_oob_vn.obj"), "v 0 0 0\n"
                                                 "v 1 0 0\n"
                                                 "v 0 1 0\n"
                                                 "vn 0 0 1\n"
                                                 "f 1//2 2//2 3//2\n"
    );
    assert_rejects(t.path);
}

TEST(reject, obj_oob_texcoord_index)
{
    // One vt declared; face references texcoord index 2 → bounds check rejects.
    TmpFile t(
        tmp_path("rasterminal_test_oob_vt.obj"), "v 0 0 0\n"
                                                 "v 1 0 0\n"
                                                 "v 0 1 0\n"
                                                 "vt 0 0\n"
                                                 "f 1/2 2/2 3/2\n"
    );
    assert_rejects(t.path);
}

TEST(reject, obj_non_finite_vertex)
{
    // An overflowing exponent (1e400) parses to +inf in tinyobjloader's float reader. A
    // non-finite position must be rejected: load_model scans positions after the loader
    // returns and fails loud (mesh.cpp), since inf would poison normals/bbox/camera-fit.
    // (A bare "nan"/"inf" token can't be used — tinyobj's parser fails them to 0.0.)
    TmpFile t(
        tmp_path("rasterminal_test_inf.obj"), "v 1e400 0 0\n"
                                              "v 1 0 0\n"
                                              "v 0 1 0\n"
                                              "f 1 2 3\n"
    );
    assert_rejects(t.path);
}

// ═══════════════════════════════════════════════════════════════════════════
//  MTL MATERIAL PARSING
// ═══════════════════════════════════════════════════════════════════════════

TEST(obj_valid, mtl_default_material_is_white)
{
    // Without any usemtl the default material (index 0) must be white diffuse.
    TmpFile t(
        tmp_path("rast_mat_default.obj"), "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                          "f 1 2 3\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_NEAR(m.materials[0].diffuse.x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.materials[0].diffuse.y, 1.0f, 1e-5f);
    ASSERT_NEAR(m.materials[0].diffuse.z, 1.0f, 1e-5f);
}

TEST(obj_valid, mtl_kd_parsed)
{
    TmpFile mtl(tmp_path("rast_kd.mtl"), "newmtl M\nKd 0.8 0.2 0.4\n");
    TmpFile obj(
        tmp_path("rast_kd.obj"), "mtllib rast_kd.mtl\n"
                                 "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                 "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].diffuse.x, 0.8f, 1e-5f);
    ASSERT_NEAR(m.materials[1].diffuse.y, 0.2f, 1e-5f);
    ASSERT_NEAR(m.materials[1].diffuse.z, 0.4f, 1e-5f);
}

TEST(obj_valid, mtl_ks_parsed)
{
    TmpFile mtl(tmp_path("rast_ks.mtl"), "newmtl M\nKs 0.3 0.6 0.9\n");
    TmpFile obj(
        tmp_path("rast_ks.obj"), "mtllib rast_ks.mtl\n"
                                 "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                 "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].specular.x, 0.3f, 1e-5f);
    ASSERT_NEAR(m.materials[1].specular.y, 0.6f, 1e-5f);
    ASSERT_NEAR(m.materials[1].specular.z, 0.9f, 1e-5f);
}

TEST(obj_valid, mtl_ns_parsed)
{
    TmpFile mtl(tmp_path("rast_ns.mtl"), "newmtl M\nNs 64.0\n");
    TmpFile obj(
        tmp_path("rast_ns.obj"), "mtllib rast_ns.mtl\n"
                                 "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                 "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].shininess, 64.0f, 1e-5f);
}

TEST(obj_valid, mtl_map_ks_parses_alongside_neighbors)
{
    TmpFile mtl(
        tmp_path("rast_map_ks.mtl"), "newmtl M\n"
                                     "Kd 0.5 0.5 0.5\n"
                                     "map_Ks nonexistent_specular.png\n"
                                     "Ks 0.7 0.7 0.7\n"
    );
    TmpFile obj(
        tmp_path("rast_map_ks.obj"), "mtllib rast_map_ks.mtl\n"
                                     "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                     "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    // Kd before and Ks after still parse correctly — map_Ks didn't swallow them.
    ASSERT_NEAR(m.materials[1].diffuse.x, 0.5f, 1e-5f);
    ASSERT_NEAR(m.materials[1].specular.x, 0.7f, 1e-5f);
    // Texture file doesn't exist, so specular_tex stays -1.
    ASSERT_EQ(m.materials[1].specular_tex, -1);
}

TEST(obj_valid, mtl_ke_emissive_parsed)
{
    TmpFile mtl(tmp_path("rast_ke.mtl"), "newmtl M\nKe 1.0 0.5 0.25\n");
    TmpFile obj(
        tmp_path("rast_ke.obj"), "mtllib rast_ke.mtl\n"
                                 "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                 "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].emissive.x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.materials[1].emissive.y, 0.5f, 1e-5f);
    ASSERT_NEAR(m.materials[1].emissive.z, 0.25f, 1e-5f);
    ASSERT_EQ(m.materials[1].emissive_tex, -1);
    ASSERT_TRUE(m.has_emissive);
}

TEST(obj_valid, mtl_negative_ke_clamped_to_zero)
{
    // Emission is physically non-negative; a negative Ke channel (malformed input) must clamp
    // to 0 at load so it can't subtract from lit colour. Mirrors the glTF emissiveFactor/
    // emissiveStrength minimum:0 guard. R stays, G/B clamp; has_emissive stays true via R.
    TmpFile mtl(tmp_path("rast_ke_neg.mtl"), "newmtl M\nKe 0.5 -1.0 -0.2\n");
    TmpFile obj(
        tmp_path("rast_ke_neg.obj"), "mtllib rast_ke_neg.mtl\n"
                                     "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                     "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].emissive.x, 0.5f, 1e-5f);
    ASSERT_NEAR(m.materials[1].emissive.y, 0.0f, 1e-6f);
    ASSERT_NEAR(m.materials[1].emissive.z, 0.0f, 1e-6f);
    ASSERT_TRUE(m.has_emissive);
}

TEST(obj_valid, mtl_no_ke_means_no_emissive)
{
    TmpFile mtl(tmp_path("rast_noke.mtl"), "newmtl M\nKd 0.5 0.5 0.5\n");
    TmpFile obj(
        tmp_path("rast_noke.obj"), "mtllib rast_noke.mtl\n"
                                   "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                   "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].emissive.x, 0.0f, 1e-6f);
    ASSERT_EQ(m.materials[1].emissive_tex, -1);
    ASSERT_FALSE(m.has_emissive);
}

TEST(obj_valid, mtl_ka_parsed)
{
    TmpFile mtl(tmp_path("rast_ka.mtl"), "newmtl M\nKa 0.3 0.4 0.5\n");
    TmpFile obj(
        tmp_path("rast_ka.obj"), "mtllib rast_ka.mtl\n"
                                 "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                 "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].ambient.x, 0.3f, 1e-5f);
    ASSERT_NEAR(m.materials[1].ambient.y, 0.4f, 1e-5f);
    ASSERT_NEAR(m.materials[1].ambient.z, 0.5f, 1e-5f);
}

TEST(obj_valid, mtl_ka_defaults_to_kd_when_absent)
{
    TmpFile mtl(tmp_path("rast_ka_absent.mtl"), "newmtl M\nKd 0.7 0.6 0.5\n");
    TmpFile obj(
        tmp_path("rast_ka_absent.obj"), "mtllib rast_ka_absent.mtl\n"
                                        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                        "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].ambient.x, 0.7f, 1e-5f);
    ASSERT_NEAR(m.materials[1].ambient.y, 0.6f, 1e-5f);
    ASSERT_NEAR(m.materials[1].ambient.z, 0.5f, 1e-5f);
}

TEST(obj_valid, default_material_ambient_is_white)
{
    TmpFile t(
        tmp_path("rast_default_ambient.obj"), "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                              "f 1 2 3\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_NEAR(m.materials[0].ambient.x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.materials[0].ambient.y, 1.0f, 1e-5f);
    ASSERT_NEAR(m.materials[0].ambient.z, 1.0f, 1e-5f);
}

TEST(obj_valid, usemtl_assigns_material_to_triangles)
{
    // Two materials applied to two separate faces — each triangle must carry
    // the material_idx of the active usemtl at the time it was declared.
    TmpFile mtl(
        tmp_path("rast_usemtl.mtl"), "newmtl A\nKd 1 0 0\n"
                                     "newmtl B\nKd 0 0 1\n"
    );
    TmpFile obj(
        tmp_path("rast_usemtl.obj"), "mtllib rast_usemtl.mtl\n"
                                     "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
                                     "usemtl A\nf 1 2 3\n"
                                     "usemtl B\nf 1 2 4\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 2 });
    const Material &matA = m.mat_at(m.triangles[0].material_idx);
    const Material &matB = m.mat_at(m.triangles[1].material_idx);
    ASSERT_NEAR(matA.diffuse.x, 1.0f, 1e-5f);
    ASSERT_NEAR(matA.diffuse.z, 0.0f, 1e-5f);
    ASSERT_NEAR(matB.diffuse.x, 0.0f, 1e-5f);
    ASSERT_NEAR(matB.diffuse.z, 1.0f, 1e-5f);
}

TEST(obj_valid, usemtl_does_not_apply_retroactively)
{
    // Face declared BEFORE usemtl must keep the default material, not the
    // subsequently declared one.
    TmpFile mtl(tmp_path("rast_retro.mtl"), "newmtl red\nKd 1 0 0\n");
    TmpFile obj(
        tmp_path("rast_retro.obj"), "mtllib rast_retro.mtl\n"
                                    "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 0 1 1\n"
                                    "f 1 2 3\n"
                                    "usemtl red\n"
                                    "f 4 5 6\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 2 });
    // The two triangles must reference different materials.
    ASSERT_TRUE(m.triangles[0].material_idx != m.triangles[1].material_idx);
    // The second face uses red (Kd 1 0 0).
    const Material &red = m.mat_at(m.triangles[1].material_idx);
    ASSERT_NEAR(red.diffuse.x, 1.0f, 1e-5f);
    ASSERT_NEAR(red.diffuse.z, 0.0f, 1e-5f);
}

TEST(obj_valid, mtl_map_d_sets_alpha_cutoff)
{
    // map_d in MTL marks the material as cutout — alpha_cutoff must be 0.5.
    TmpFile mtl(tmp_path("rast_mapd.mtl"), "newmtl M\nKd 1 1 1\nmap_d mask.png\n");
    TmpFile obj(
        tmp_path("rast_mapd.obj"), "mtllib rast_mapd.mtl\n"
                                   "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                   "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].alpha_cutoff, 0.5f, 1e-6f);
}

// Note: the src_has_vcol guard in mesh_obj.cpp checks
// attrib.colors.size() == attrib.vertices.size(). tinyobjloader either leaves
// attrib.colors empty (no color data) or fills it with exactly the same
// element count as attrib.vertices — the mismatched-non-empty case cannot
// occur through valid OBJ input and there is no test for it.

TEST(obj_valid, vertex_colors_extension)
{
    // OBJ "v x y z r g b" extension: per-vertex RGB encoded directly in the v line.
    TmpFile t(
        tmp_path("rast_vcol_obj.obj"), "v 0 0 0 1.0 0.0 0.0\n"
                                       "v 1 0 0 0.0 1.0 0.0\n"
                                       "v 0 1 0 0.0 0.0 1.0\n"
                                       "f 1 2 3\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{ 3 });
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[1].y, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[2].z, 1.0f, 1e-5f);
}

TEST(obj_valid, all_white_vertex_colors_flag_is_false)
{
    // When all "v x y z r g b" colors are white, has_vertex_colors must be false
    // (white is the neutral multiplicative identity, same as no color data).
    TmpFile t(
        tmp_path("rast_vcol_white.obj"), "v 0 0 0 1.0 1.0 1.0\n"
                                         "v 1 0 0 1.0 1.0 1.0\n"
                                         "v 0 1 0 1.0 1.0 1.0\n"
                                         "f 1 2 3\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_FALSE(m.has_vertex_colors);
    ASSERT_TRUE(m.vertex_colors.empty());
}

TEST(obj_valid, failed_load_rollback_preserves_previous_mesh_state)
{
    Mesh m;
    m.vertices.push_back({ vec3{ 1.0f, 2.0f, 3.0f }, vec3{ 0.0f, 0.0f, 1.0f }, vec2{ 0.25f, 0.75f }, 0.9f });
    m.vertices.push_back({ vec3{ 4.0f, 5.0f, 6.0f }, vec3{ 0.0f, 1.0f, 0.0f }, vec2{ 0.5f, 0.5f }, 0.8f });
    m.triangles.push_back({ { 0, 0, 0 }, 0 });
    m.materials.push_back(Material{});
    Material mat{};
    mat.diffuse = { 0.2f, 0.3f, 0.4f };
    mat.ambient = { 0.1f, 0.2f, 0.3f };
    mat.specular = { 0.4f, 0.5f, 0.6f };
    mat.shininess = 16.0f;
    m.materials.push_back(mat);
    m.textures.push_back(Texture{});
    m.tangents.emplace_back(1.0f, 0.0f, 0.0f);
    m.tangents.emplace_back(0.0f, 1.0f, 0.0f);
    m.vertex_colors.emplace_back(0.1f, 0.2f, 0.3f);
    m.vertex_colors.emplace_back(0.4f, 0.5f, 0.6f);
    m.has_vertex_colors = true;
    m.has_double_sided = true;

    const Mesh before = m;

    TmpFile bad(
        tmp_path("rast_obj_rollback.obj"), "v 0 0 0\n"
                                           "v 1 0 0\n"
                                           "v 0 1 0\n"
                                           "f 1 2 9\n"
    );

    ASSERT_FALSE(m.load_obj(bad.path));

    ASSERT_EQ(m.vertices.size(), before.vertices.size());
    ASSERT_EQ(m.triangles.size(), before.triangles.size());
    ASSERT_EQ(m.materials.size(), before.materials.size());
    ASSERT_EQ(m.textures.size(), before.textures.size());
    ASSERT_EQ(m.tangents.size(), before.tangents.size());
    ASSERT_EQ(m.vertex_colors.size(), before.vertex_colors.size());
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_TRUE(m.has_double_sided);
    ASSERT_NEAR(m.vertices[0].pos.x, before.vertices[0].pos.x, 1e-6f);
    ASSERT_NEAR(m.vertices[1].normal.y, before.vertices[1].normal.y, 1e-6f);
    ASSERT_NEAR(m.vertex_colors[0].x, before.vertex_colors[0].x, 1e-6f);
    ASSERT_NEAR(m.tangents[1].y, before.tangents[1].y, 1e-6f);
}

TEST(obj_valid, mtl_map_kn_normal_tex_missing_file)
{
    // map_Kn exercises the !normal_texname.empty() true branch in load_obj.
    // File is absent so load_tex returns -1, but the branch is exercised and
    // no crash occurs. Without this test, every material path goes through the
    // bump_texname fallback (else branch) only.
    TmpFile mtl(tmp_path("rast_mapkn.mtl"), "newmtl M\nKd 1 1 1\nmap_Kn nonexistent_normal.png\n");
    TmpFile obj(
        tmp_path("rast_mapkn.obj"), "mtllib rast_mapkn.mtl\n"
                                    "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                    "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].normal_tex, -1);
}

TEST(obj_valid, partial_normals_falls_back_to_compute_normals)
{
    // First face has explicit vn references; second has none. This sets
    // all_have_normals=false in get_vertex → compute_normals() runs at line 200.
    // Without this, the !attrib.normals.empty() && !all_have_normals branch
    // is never triggered — all prior tests either have all normals or none.
    TmpFile t(
        tmp_path("rast_partnorm.obj"), "v 0 0 0\n"
                                       "v 1 0 0\n"
                                       "v 0 1 0\n"
                                       "v 0 0 1\n"
                                       "vn 0 0 1\n"
                                       "f 1//1 2//1 3//1\n"
                                       "f 1 2 4\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 2 });
    for (const Vertex &v : m.vertices)
    {
        const float len_sq = (v.normal.x * v.normal.x) + (v.normal.y * v.normal.y) + (v.normal.z * v.normal.z);
        ASSERT_NEAR(len_sq, 1.0f, 0.01f);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  UV-SEAM NORMAL WELDING — normal-less OBJs split one position into several
//  vertices by texcoord; compute_normals welds them by source position so the
//  seam smooths like a desktop viewer, while the dihedral crease test still wins.
// ═══════════════════════════════════════════════════════════════════════════

namespace
{
    // Collect the normals of every vertex sitting exactly at the origin.
    [[maybe_unused]] std::vector<vec3> origin_normals(const Mesh &m)
    {
        std::vector<vec3> out;
        for (const auto &v : m.vertices)
        {
            if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
            {
                out.push_back(v.normal);
            }
        }
        return out;
    }
} // namespace

TEST(obj_valid, uv_seam_below_crease_smooths)
{
    // Two faces share edge (0,0,0)-(1,0,0) at ~11 deg (< 60 deg crease), but the
    // shared-edge vertices carry different vt in each face → the loader splits the
    // origin into two vertices. Welding by source position must smooth them: both
    // origin vertices stay (UV preserved) but share one blended normal.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 -1 0.2\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\nvt 0.5 0.5\nvt 0.6 0.6\nvt 0.2 0.8\n"
                            "f 1/1 2/2 3/3\n"
                            "f 2/5 1/4 4/6\n";
    TmpFile f(tmp_path("rast_seam_smooth.obj"), obj);
    Mesh m = load_ok(f.path);
    const std::vector<vec3> ns = origin_normals(m);
    ASSERT_EQ(ns.size(), size_t{ 2 }); // UV split preserved
    // Same welded normal on both halves, and it is the blend (not a face normal).
    ASSERT_NEAR(ns[0].x, ns[1].x, 1e-5f);
    ASSERT_NEAR(ns[0].y, ns[1].y, 1e-5f);
    ASSERT_NEAR(ns[0].z, ns[1].z, 1e-5f);
    ASSERT_TRUE(ns[0].z > 0.9f && ns[0].y > 0.0f);
}

TEST(obj_valid, uv_seam_above_crease_stays_split)
{
    // Same seam, but folded 90 deg (> 60 deg crease). Welding must NOT override the
    // dihedral test: the origin keeps two vertices with distinct axis-aligned
    // normals (+Z and +Y), exactly as a hard edge.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\nvt 0.5 0.5\nvt 0.6 0.6\nvt 0.2 0.8\n"
                            "f 1/1 2/2 3/3\n"
                            "f 2/5 1/4 4/6\n";
    TmpFile f(tmp_path("rast_seam_hard.obj"), obj);
    Mesh m = load_ok(f.path);
    const std::vector<vec3> ns = origin_normals(m);
    ASSERT_EQ(ns.size(), size_t{ 2 });
    const bool a_is_z = ns[0].z > 0.99f;
    const vec3 &z_n = a_is_z ? ns[0] : ns[1];
    const vec3 &y_n = a_is_z ? ns[1] : ns[0];
    ASSERT_TRUE(z_n.z > 0.99f);
    ASSERT_TRUE(y_n.y > 0.99f);
}

TEST(obj_valid, uv_seam_coplanar_one_wedge_spans_both_halves)
{
    // Two coplanar (+Z) faces share edge (0,0,0)-(0,1,0) but seam-split the origin
    // by vt. The shared edge unions both halves into ONE wedge whose normal must
    // broadcast to both original vertices — the core welded-materialization path.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv -1 0 0\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\nvt 0.5 0.5\nvt 0.6 0.6\nvt 0.2 0.8\n"
                            "f 1/1 2/2 3/3\n"
                            "f 1/4 3/5 4/6\n";
    TmpFile f(tmp_path("rast_seam_coplanar.obj"), obj);
    Mesh m = load_ok(f.path);
    const std::vector<vec3> ns = origin_normals(m);
    ASSERT_EQ(ns.size(), size_t{ 2 });
    for (const vec3 &n : ns)
    {
        ASSERT_NEAR(n.x, 0.0f, 1e-5f);
        ASSERT_NEAR(n.y, 0.0f, 1e-5f);
        ASSERT_NEAR(n.z, 1.0f, 1e-5f);
    }
}

TEST(obj_valid, no_seam_fold_merges_unchanged)
{
    // The common case: shared-edge sub-crease fold with the SAME vt on both faces
    // (no seam). The shared origin must remain a single merged vertex — welding
    // must not spuriously split or otherwise change the non-seam path.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 -1 0.2\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\nvt 0.5 0.5\n"
                            "f 1/1 2/2 3/3\n"
                            "f 2/2 1/1 4/4\n";
    TmpFile f(tmp_path("rast_seam_none.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_EQ(origin_normals(m).size(), size_t{ 1 }); // merged, not split
}

TEST(obj_valid, uv_seam_bowtie_point_share_splits)
{
    // Two faces touch only at the origin (a point, no shared edge), seam-split by
    // vt. Even at full smoothing they must stay split — a bowtie point is not a
    // connected surface, and welding the position does not connect it.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nv 0 1 1\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\nvt 0.5 0.5\nvt 0.6 0.6\nvt 0.2 0.8\n"
                            "f 1/1 2/2 3/3\n"
                            "f 1/4 4/5 5/6\n";
    TmpFile f(tmp_path("rast_seam_bowtie.obj"), obj);
    Mesh m;
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/180.0f));
    ASSERT_EQ(origin_normals(m).size(), size_t{ 2 });
}

TEST(obj_valid, uv_seam_degenerate_face_no_corruption)
{
    // A zero-area face references the origin twice with distinct vt. Its corners
    // are excluded from clustering (degenerate) and contribute no normal; the
    // valid face must still produce finite unit normals with no crash.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\nvt 0.5 0.5\nvt 0.6 0.6\n"
                            "f 1/1 2/2 3/3\n"
                            "f 1/4 1/5 2/2\n";
    TmpFile f(tmp_path("rast_seam_degen.obj"), obj);
    Mesh m = load_ok(f.path);
    for (const auto &v : m.vertices)
    {
        ASSERT_TRUE(std::isfinite(v.normal.x) && std::isfinite(v.normal.y) && std::isfinite(v.normal.z));
    }
    // The valid face's origin corner keeps a proper +Z normal.
    bool found_z = false;
    for (const vec3 &n : origin_normals(m))
    {
        if (n.z > 0.99f)
        {
            found_z = true;
        }
    }
    ASSERT_TRUE(found_z);
}

TEST(obj_valid, faceted_cube_appends_stay_in_bounds)
{
    // A normal-less, UV-less cube loaded fully faceted (crease 0) splits every
    // shared vertex into one wedge per incident face → many appended split
    // vertices, and later groups revisit triangles whose corners were already
    // rewritten to those appended indices. The welded-adjacency lookup must stay
    // in bounds for appended vertices (regression: it indexed the fixed-size weld
    // map, overflowing once a corner pointed past the original vertex count).
    const std::string obj = "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\n"
                            "v -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
                            "f 1 2 3 4\nf 5 8 7 6\nf 1 5 6 2\n"
                            "f 2 6 7 3\nf 3 7 8 4\nf 4 8 5 1\n";
    TmpFile f(tmp_path("rast_faceted_cube.obj"), obj);
    Mesh m;
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/0.0f));
    ASSERT_EQ(m.triangles.size(), size_t{ 12 });
    for (const auto &v : m.vertices)
    {
        const float len = v.normal.length();
        ASSERT_TRUE(std::isfinite(len));
        ASSERT_NEAR(len, 1.0f, 1e-4f);
    }
}

TEST(obj_valid, uv_seam_crease_split_syncs_vertex_colors)
{
    // OBJ vertex-color extension + a UV seam at a 90 deg fold. Each seam half
    // lands in its own wedge (crease > threshold), so Pass B appends one split
    // vertex per half — the has_weld branch on the vcol sync path. After:
    //   - vertex_colors stays length-matched to vertices (parallel-array invariant);
    //   - all four origin vertices inherit the source color, not white;
    //   - the two +Z halves carry the +Z normal, the two +Y halves carry +Y.
    const std::string obj = "v 0 0 0 0.8 0.2 0.1\n"
                            "v 1 0 0 0.8 0.2 0.1\n"
                            "v 0 1 0 0.8 0.2 0.1\n"
                            "v 0 0 1 0.8 0.2 0.1\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\nvt 0.5 0.5\nvt 0.6 0.6\nvt 0.2 0.8\n"
                            "f 1/1 2/2 3/3\n"
                            "f 2/5 1/4 4/6\n";
    TmpFile f(tmp_path("rast_seam_vcol.obj"), obj);
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), m.vertices.size()); // parallel-array invariant
    int origin_count = 0;
    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        const Vertex &v = m.vertices[i];
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            origin_count++;
            ASSERT_NEAR(m.vertex_colors[i].x, 0.8f, 1e-5f);
            ASSERT_NEAR(m.vertex_colors[i].y, 0.2f, 1e-5f);
            ASSERT_NEAR(m.vertex_colors[i].z, 0.1f, 1e-5f);
        }
    }
    ASSERT_EQ(origin_count, 2); // origin still split despite welding (90 deg > 60 deg crease)
}

TEST(obj_valid, uv_seam_smooth_angle_extremes)
{
    // 90 deg fold across a seam. --smooth-angle 0 → faceted (two distinct axis
    // normals); 180 → fully smooth (two vertices sharing one blended normal).
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
                            "vt 0 0\nvt 1 0\nvt 0 1\nvt 0.5 0.5\nvt 0.6 0.6\nvt 0.2 0.8\n"
                            "f 1/1 2/2 3/3\n"
                            "f 2/5 1/4 4/6\n";
    TmpFile f(tmp_path("rast_seam_extremes.obj"), obj);

    Mesh faceted;
    ASSERT_TRUE(faceted.load_model(f.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/0.0f));
    const std::vector<vec3> fn = origin_normals(faceted);
    ASSERT_EQ(fn.size(), size_t{ 2 });
    ASSERT_TRUE((fn[0].z > 0.99f && fn[1].y > 0.99f) || (fn[1].z > 0.99f && fn[0].y > 0.99f));

    Mesh smooth;
    ASSERT_TRUE(smooth.load_model(f.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/180.0f));
    const std::vector<vec3> sn = origin_normals(smooth);
    ASSERT_EQ(sn.size(), size_t{ 2 });
    ASSERT_NEAR(sn[0].x, sn[1].x, 1e-5f);
    ASSERT_NEAR(sn[0].y, sn[1].y, 1e-5f);
    ASSERT_NEAR(sn[0].z, sn[1].z, 1e-5f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEXTURE SUCCESS PATHS — load_tex() with a real BMP on disk
// ═══════════════════════════════════════════════════════════════════════════

TEST(obj_valid, mtl_map_kd_real_file_sets_diffuse_tex)
{
    // First test to exercise the load_tex success branch (mesh_obj.cpp:62-64):
    // a real file is present, so load_tex pushes a Texture and returns index 0.
    TmpFile bmp(tmp_path("rast_kd_tex.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(tmp_path("rast_kd_tex.mtl"), "newmtl M\nKd 1 1 1\nmap_Kd rast_kd_tex.bmp\n");
    TmpFile obj(
        tmp_path("rast_kd_tex.obj"), "mtllib rast_kd_tex.mtl\n"
                                     "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                     "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_tex, 0);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
}

TEST(obj_valid, mtl_map_ks_real_file_sets_specular_tex)
{
    TmpFile bmp(tmp_path("rast_ks_tex.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(tmp_path("rast_ks_tex.mtl"), "newmtl M\nKs 1 1 1\nmap_Ks rast_ks_tex.bmp\n");
    TmpFile obj(
        tmp_path("rast_ks_tex.obj"), "mtllib rast_ks_tex.mtl\n"
                                     "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                     "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].specular_tex, 0);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
}

TEST(obj_valid, mtl_map_kn_real_file_sets_normal_tex)
{
    // Exercises the !normal_texname.empty() true-branch success path.
    // tinyobjloader's keyword for normal_texname is "norm" (not "map_Kn").
    TmpFile bmp(tmp_path("rast_kn_tex.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(tmp_path("rast_kn_tex.mtl"), "newmtl M\nKd 1 1 1\nnorm rast_kn_tex.bmp\n");
    TmpFile obj(
        tmp_path("rast_kn_tex.obj"), "mtllib rast_kn_tex.mtl\n"
                                     "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                     "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].normal_tex >= 0);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
}

TEST(obj_valid, mtl_map_ke_real_file_sets_emissive_tex)
{
    // Exercises the parallel decoder for the emissive slot — same wiring as map_Kd,
    // proves emissive_tex flows through load_tex → decode_textures → compaction remap.
    TmpFile bmp(tmp_path("rast_ke_tex.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(tmp_path("rast_ke_tex.mtl"), "newmtl M\nKe 1 1 1\nmap_Ke rast_ke_tex.bmp\n");
    TmpFile obj(
        tmp_path("rast_ke_tex.obj"), "mtllib rast_ke_tex.mtl\n"
                                     "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                     "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].emissive_tex, 0);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.has_emissive);
}

TEST(obj_valid, mtl_map_ke_without_ke_stays_dark)
{
    // Spec-literal: emissive = Ke × map_Ke. With no Ke (default 0), the bound map_Ke must
    // not contribute — matches the glTF spec / three.js GLTFLoader. Authors must set
    // `Ke 1 1 1` explicitly to make the texture glow.
    TmpFile bmp(tmp_path("rast_ke_dark.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(tmp_path("rast_ke_dark.mtl"), "newmtl M\nKd 1 1 1\nmap_Ke rast_ke_dark.bmp\n");
    TmpFile obj(
        tmp_path("rast_ke_dark.obj"), "mtllib rast_ke_dark.mtl\n"
                                      "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                      "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    // Loader skips load_tex on zero-factor materials — saves a multi-MB stb_image_load and
    // permanent RAM for a texture no fragment will ever sample.
    ASSERT_EQ(m.materials[1].emissive_tex, -1);
    ASSERT_TRUE(m.textures.empty());
    ASSERT_NEAR(m.materials[1].emissive.x, 0.0f, 1e-6f);
    ASSERT_NEAR(m.materials[1].emissive.y, 0.0f, 1e-6f);
    ASSERT_NEAR(m.materials[1].emissive.z, 0.0f, 1e-6f);
    ASSERT_FALSE(m.has_emissive);
}

TEST(obj_valid, mtl_map_ke_with_explicit_ke_keeps_authored_factor)
{
    // Promotion only applies when factor is the default zero. Explicit Ke values must survive.
    TmpFile bmp(tmp_path("rast_ke_keep.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(tmp_path("rast_ke_keep.mtl"), "newmtl M\nKd 1 1 1\nKe 0.25 0.5 0.75\nmap_Ke rast_ke_keep.bmp\n");
    TmpFile obj(
        tmp_path("rast_ke_keep.obj"), "mtllib rast_ke_keep.mtl\n"
                                      "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                      "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].emissive.x, 0.25f, 1e-5f);
    ASSERT_NEAR(m.materials[1].emissive.y, 0.5f, 1e-5f);
    ASSERT_NEAR(m.materials[1].emissive.z, 0.75f, 1e-5f);
}

TEST(obj_valid, mtl_map_ke_without_ke_skips_decode_even_if_file_missing)
{
    // With no Ke (factor {0,0,0}) the loader skips load_tex entirely, so emissive_tex stays
    // -1 and the material renders dark. The map_Ke points at a missing file, but that's never
    // attempted — the zero-factor skip short-circuits before any decode. (The failed-decode
    // remap path itself is exercised by the non-zero-Ke remap test below.)
    TmpFile mtl(
        tmp_path("rast_ke_missing.mtl"), "newmtl M\n"
                                         "Kd 1 1 1\n"
                                         "map_Ke rast_ke_does_not_exist.bmp\n"
    );
    TmpFile obj(
        tmp_path("rast_ke_missing.obj"), "mtllib rast_ke_missing.mtl\n"
                                         "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                         "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].emissive_tex, -1);
    ASSERT_NEAR(m.materials[1].emissive.x, 0.0f, 1e-6f);
    ASSERT_NEAR(m.materials[1].emissive.y, 0.0f, 1e-6f);
    ASSERT_NEAR(m.materials[1].emissive.z, 0.0f, 1e-6f);
    ASSERT_FALSE(m.has_emissive);
}

TEST(obj_valid, mtl_map_ke_failed_decode_remaps_index_to_minus_one)
{
    // A non-existent map_Ke file fails decoding; decode_textures must compact it
    // out and remap emissive_tex through fix(m.emissive_tex). Without that fix,
    // emissive_tex would either stay pointing at a hole or rebind to the wrong slot.
    TmpFile bmp(tmp_path("rast_ke_remap_kd.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(
        tmp_path("rast_ke_remap.mtl"), "newmtl M\n"
                                       "Kd 1 1 1\n"
                                       "Ke 1 1 1\n"
                                       "map_Ke rast_ke_remap_missing.bmp\n"
                                       "map_Kd rast_ke_remap_kd.bmp\n"
    );
    TmpFile obj(
        tmp_path("rast_ke_remap.obj"), "mtllib rast_ke_remap.mtl\n"
                                       "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                       "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    // Failed emissive decode is dropped; diffuse_tex must remap to the surviving slot 0.
    ASSERT_EQ(m.materials[1].emissive_tex, -1);
    ASSERT_EQ(m.materials[1].diffuse_tex, 0);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
}

TEST(obj_valid, mtl_map_bump_fallback_when_map_kn_absent)
{
    // Only map_Bump declared (no map_Kn) → normal_texname is empty →
    // the else branch at mesh_obj.cpp:84 fires: load_tex(m.bump_texname).
    TmpFile bmp(tmp_path("rast_bump_tex.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(tmp_path("rast_bump_tex.mtl"), "newmtl M\nKd 1 1 1\nmap_Bump rast_bump_tex.bmp\n");
    TmpFile obj(
        tmp_path("rast_bump_tex.obj"), "mtllib rast_bump_tex.mtl\n"
                                       "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                       "usemtl M\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].normal_tex >= 0);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
}

TEST(obj_valid, shared_map_kd_deduplicates_texture)
{
    // Two materials declare the same map_Kd file. load_tex must decode it once
    // and hand both materials the same slot index rather than pushing twice.
    TmpFile bmp(tmp_path("rast_dedup_tex.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(
        tmp_path("rast_dedup_tex.mtl"), "newmtl A\nKd 1 1 1\nmap_Kd rast_dedup_tex.bmp\n"
                                        "newmtl B\nKd 1 1 1\nmap_Kd rast_dedup_tex.bmp\n"
    );
    TmpFile obj(
        tmp_path("rast_dedup_tex.obj"), "mtllib rast_dedup_tex.mtl\n"
                                        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                        "usemtl A\nf 1 2 3\n"
                                        "usemtl B\nf 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 3);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_EQ(m.materials[1].diffuse_tex, 0);
    ASSERT_EQ(m.materials[2].diffuse_tex, 0);
}

TEST(obj_valid, parallel_decode_two_distinct_textures)
{
    // Two materials naming two distinct map_Kd files. Loaded with n_threads=4 so
    // the decodes run on the parallel path; both must land in distinct valid slots.
    TmpFile bmpA(tmp_path("rast_par_a.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile bmpB(tmp_path("rast_par_b.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(
        tmp_path("rast_par.mtl"), "newmtl A\nKd 1 1 1\nmap_Kd rast_par_a.bmp\n"
                                  "newmtl B\nKd 1 1 1\nmap_Kd rast_par_b.bmp\n"
    );
    TmpFile obj(
        tmp_path("rast_par.obj"), "mtllib rast_par.mtl\n"
                                  "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                  "usemtl A\nf 1 2 3\n"
                                  "usemtl B\nf 1 2 3\n"
    );
    Mesh m;
    const bool ok = m.load_model(obj.path, /*ao=*/false, /*n_threads=*/4);
    ASSERT_TRUE(ok);
    ASSERT_EQ(m.textures.size(), size_t{ 2 });
    ASSERT_TRUE(m.materials.size() >= 3);
    ASSERT_TRUE(m.materials[1].diffuse_tex >= 0);
    ASSERT_TRUE(m.materials[2].diffuse_tex >= 0);
    ASSERT_TRUE(m.materials[1].diffuse_tex != m.materials[2].diffuse_tex);
}

TEST(obj_valid, face_without_usemtl_uses_default_material)
{
    // mtllib declares a material, but no usemtl → tinyobjloader sets mat_id=-1
    // for the face → the mat_id < 0 branch fires → material_idx = 0 (default white).
    TmpFile mtl(tmp_path("rast_no_usemtl.mtl"), "newmtl M\nKd 1 0 0\n");
    TmpFile obj(
        tmp_path("rast_no_usemtl.obj"), "mtllib rast_no_usemtl.mtl\n"
                                        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                                        "f 1 2 3\n"
    );
    Mesh m = load_ok(obj.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
    ASSERT_EQ(m.triangles[0].material_idx, 0u);
}
