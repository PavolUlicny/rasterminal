#include "loader_util.h"
#include "inline_bmp.h"

// ═══════════════════════════════════════════════════════════════════════════
//  HAND-CRAFTED VALID OBJ — exercises specific format features in isolation
// ═══════════════════════════════════════════════════════════════════════════

TEST(obj_valid, mtl_multi_material_loaded)
{
    TmpFile mtl(tmp_path("rast_cube.mtl"),
                "newmtl red\nKd 0.9 0.1 0.1\n"
                "newmtl blue\nKd 0.1 0.2 0.9\n");
    TmpFile obj(tmp_path("rast_cube.obj"),
                "mtllib rast_cube.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
                "usemtl red\nf 1 2 3\n"
                "usemtl blue\nf 1 2 4\n");
    Mesh m = load_ok(obj.path);
    // Two usemtl directives → at least 2 non-default materials loaded.
    if (m.materials.size() < 2)
        ASSERT_FAIL("multi-material OBJ should produce at least 2 materials");
}

TEST(obj_valid, quad_multi_face_triangulates_to_twelve)
{
    // 6 quads (a cube) fan-triangulated → 12 triangles.
    TmpFile t(tmp_path("rast_cube6q.obj"),
              "v -1 -1  1\nv  1 -1  1\nv  1  1  1\nv -1  1  1\n"
              "v -1 -1 -1\nv  1 -1 -1\nv  1  1 -1\nv -1  1 -1\n"
              "f 1 2 3 4\n"
              "f 5 6 7 8\n"
              "f 1 2 6 5\n"
              "f 3 4 8 7\n"
              "f 1 4 8 5\n"
              "f 2 3 7 6\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{12});
}

TEST(obj_valid, single_triangle)
{
    TmpFile t(tmp_path("rasterminal_test_tri.obj"),
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 0 1 0\n"
              "f 1 2 3\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

TEST(obj_valid, quad_fan_triangulates_to_two)
{
    TmpFile t(tmp_path("rasterminal_test_quad.obj"),
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 1 1 0\n"
              "v 0 1 0\n"
              "f 1 2 3 4\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{2});
}

TEST(obj_valid, ngon_fan_triangulates_to_n_minus_two)
{
    // 5-gon → 3 triangles (fan: 1-2-3, 1-3-4, 1-4-5)
    TmpFile t(tmp_path("rasterminal_test_ngon.obj"),
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 2 1 0\n"
              "v 1 2 0\n"
              "v 0 1 0\n"
              "f 1 2 3 4 5\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{3});
}

TEST(obj_valid, negative_indices_reference_end)
{
    // OBJ spec: negative indices count back from the current end of the list.
    TmpFile t(tmp_path("rasterminal_test_neg.obj"),
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 0 1 0\n"
              "f -3 -2 -1\n"); // equivalent to "f 1 2 3"
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

TEST(obj_valid, uv_and_normal_references)
{
    TmpFile t(tmp_path("rasterminal_test_uvn.obj"),
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 0 1 0\n"
              "vt 0 0\n"
              "vt 1 0\n"
              "vt 0 1\n"
              "vn 0 0 1\n"
              "f 1/1/1 2/2/1 3/3/1\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

TEST(obj_valid, multiple_faces_deduplicate_shared_vertices)
{
    // Two triangles sharing an edge — the four unique FaceVertex keys
    // should produce exactly four vertices in the final buffer.
    TmpFile t(tmp_path("rasterminal_test_shared.obj"),
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 1 1 0\n"
              "v 0 1 0\n"
              "f 1 2 3\n"
              "f 1 3 4\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{4});
    ASSERT_EQ(m.triangles.size(), size_t{2});
}

TEST(obj_valid, comments_and_blank_lines_ignored)
{
    TmpFile t(tmp_path("rasterminal_test_comments.obj"),
              "# this is a comment\n"
              "\n"
              "v 0 0 0\n"
              "# another comment\n"
              "v 1 0 0\n"
              "\n"
              "v 0 1 0\n"
              "f 1 2 3\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

// ═══════════════════════════════════════════════════════════════════════════
//  REJECTIONS — malformed/corrupt OBJ must not crash
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, obj_only_comments)
{
    TmpFile t(tmp_path("rasterminal_test_cmts.obj"),
              "# just a comment\n"
              "# another one\n");
    assert_rejects(t.path);
}

TEST(reject, obj_vertices_but_no_faces)
{
    TmpFile t(tmp_path("rasterminal_test_nofaces.obj"),
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 0 1 0\n");
    assert_rejects(t.path);
}

TEST(reject, obj_all_face_indices_out_of_range)
{
    // Faces reference non-existent vertices → every face is skipped → no
    // triangles → load fails with "triangles.empty()".
    TmpFile t(tmp_path("rasterminal_test_oob.obj"),
              "v 0 0 0\n"
              "v 1 0 0\n"
              "f 5 6 7\n"
              "f 10 11 12\n");
    assert_rejects(t.path);
}

TEST(reject, obj_oob_normal_index)
{
    // One vn declared; face references normal index 2 → bounds check rejects.
    TmpFile t(tmp_path("rasterminal_test_oob_vn.obj"),
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 0 1 0\n"
              "vn 0 0 1\n"
              "f 1//2 2//2 3//2\n");
    assert_rejects(t.path);
}

TEST(reject, obj_oob_texcoord_index)
{
    // One vt declared; face references texcoord index 2 → bounds check rejects.
    TmpFile t(tmp_path("rasterminal_test_oob_vt.obj"),
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 0 1 0\n"
              "vt 0 0\n"
              "f 1/2 2/2 3/2\n");
    assert_rejects(t.path);
}

// ═══════════════════════════════════════════════════════════════════════════
//  MTL MATERIAL PARSING
// ═══════════════════════════════════════════════════════════════════════════

TEST(obj_valid, mtl_default_material_is_white)
{
    // Without any usemtl the default material (index 0) must be white diffuse.
    TmpFile t(tmp_path("rast_mat_default.obj"),
              "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
              "f 1 2 3\n");
    Mesh m = load_ok(t.path);
    ASSERT_NEAR(m.materials[0].diffuse.x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.materials[0].diffuse.y, 1.0f, 1e-5f);
    ASSERT_NEAR(m.materials[0].diffuse.z, 1.0f, 1e-5f);
}

TEST(obj_valid, mtl_kd_parsed)
{
    TmpFile mtl(tmp_path("rast_kd.mtl"), "newmtl M\nKd 0.8 0.2 0.4\n");
    TmpFile obj(tmp_path("rast_kd.obj"),
                "mtllib rast_kd.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "usemtl M\nf 1 2 3\n");
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].diffuse.x, 0.8f, 1e-5f);
    ASSERT_NEAR(m.materials[1].diffuse.y, 0.2f, 1e-5f);
    ASSERT_NEAR(m.materials[1].diffuse.z, 0.4f, 1e-5f);
}

TEST(obj_valid, mtl_ks_parsed)
{
    TmpFile mtl(tmp_path("rast_ks.mtl"), "newmtl M\nKs 0.3 0.6 0.9\n");
    TmpFile obj(tmp_path("rast_ks.obj"),
                "mtllib rast_ks.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "usemtl M\nf 1 2 3\n");
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].specular.x, 0.3f, 1e-5f);
    ASSERT_NEAR(m.materials[1].specular.y, 0.6f, 1e-5f);
    ASSERT_NEAR(m.materials[1].specular.z, 0.9f, 1e-5f);
}

TEST(obj_valid, mtl_ns_parsed)
{
    TmpFile mtl(tmp_path("rast_ns.mtl"), "newmtl M\nNs 64.0\n");
    TmpFile obj(tmp_path("rast_ns.obj"),
                "mtllib rast_ns.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "usemtl M\nf 1 2 3\n");
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].shininess, 64.0f, 1e-5f);
}

TEST(obj_valid, mtl_map_ks_parses_alongside_neighbors)
{
    TmpFile mtl(tmp_path("rast_map_ks.mtl"),
                "newmtl M\n"
                "Kd 0.5 0.5 0.5\n"
                "map_Ks nonexistent_specular.png\n"
                "Ks 0.7 0.7 0.7\n");
    TmpFile obj(tmp_path("rast_map_ks.obj"),
                "mtllib rast_map_ks.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "usemtl M\nf 1 2 3\n");
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    // Kd before and Ks after still parse correctly — map_Ks didn't swallow them.
    ASSERT_NEAR(m.materials[1].diffuse.x, 0.5f, 1e-5f);
    ASSERT_NEAR(m.materials[1].specular.x, 0.7f, 1e-5f);
    // Texture file doesn't exist, so specular_tex stays -1.
    ASSERT_EQ(m.materials[1].specular_tex, -1);
}

TEST(obj_valid, mtl_ka_parsed)
{
    TmpFile mtl(tmp_path("rast_ka.mtl"), "newmtl M\nKa 0.3 0.4 0.5\n");
    TmpFile obj(tmp_path("rast_ka.obj"),
                "mtllib rast_ka.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "usemtl M\nf 1 2 3\n");
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].ambient.x, 0.3f, 1e-5f);
    ASSERT_NEAR(m.materials[1].ambient.y, 0.4f, 1e-5f);
    ASSERT_NEAR(m.materials[1].ambient.z, 0.5f, 1e-5f);
}

TEST(obj_valid, mtl_ka_defaults_to_kd_when_absent)
{
    TmpFile mtl(tmp_path("rast_ka_absent.mtl"), "newmtl M\nKd 0.7 0.6 0.5\n");
    TmpFile obj(tmp_path("rast_ka_absent.obj"),
                "mtllib rast_ka_absent.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "usemtl M\nf 1 2 3\n");
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].ambient.x, 0.7f, 1e-5f);
    ASSERT_NEAR(m.materials[1].ambient.y, 0.6f, 1e-5f);
    ASSERT_NEAR(m.materials[1].ambient.z, 0.5f, 1e-5f);
}

TEST(obj_valid, default_material_ambient_is_white)
{
    TmpFile t(tmp_path("rast_default_ambient.obj"),
              "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
              "f 1 2 3\n");
    Mesh m = load_ok(t.path);
    ASSERT_NEAR(m.materials[0].ambient.x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.materials[0].ambient.y, 1.0f, 1e-5f);
    ASSERT_NEAR(m.materials[0].ambient.z, 1.0f, 1e-5f);
}

TEST(obj_valid, usemtl_assigns_material_to_triangles)
{
    // Two materials applied to two separate faces — each triangle must carry
    // the material_idx of the active usemtl at the time it was declared.
    TmpFile mtl(tmp_path("rast_usemtl.mtl"),
                "newmtl A\nKd 1 0 0\n"
                "newmtl B\nKd 0 0 1\n");
    TmpFile obj(tmp_path("rast_usemtl.obj"),
                "mtllib rast_usemtl.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
                "usemtl A\nf 1 2 3\n"
                "usemtl B\nf 1 2 4\n");
    Mesh m = load_ok(obj.path);
    ASSERT_EQ(m.triangles.size(), size_t{2});
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
    TmpFile obj(tmp_path("rast_retro.obj"),
                "mtllib rast_retro.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 0 1 1\n"
                "f 1 2 3\n"
                "usemtl red\n"
                "f 4 5 6\n");
    Mesh m = load_ok(obj.path);
    ASSERT_EQ(m.triangles.size(), size_t{2});
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
    TmpFile mtl(tmp_path("rast_mapd.mtl"),
                "newmtl M\nKd 1 1 1\nmap_d mask.png\n");
    TmpFile obj(tmp_path("rast_mapd.obj"),
                "mtllib rast_mapd.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "usemtl M\nf 1 2 3\n");
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
    TmpFile t(tmp_path("rast_vcol_obj.obj"),
              "v 0 0 0 1.0 0.0 0.0\n"
              "v 1 0 0 0.0 1.0 0.0\n"
              "v 0 1 0 0.0 0.0 1.0\n"
              "f 1 2 3\n");
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{3});
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
    TmpFile t(tmp_path("rast_vcol_white.obj"),
              "v 0 0 0 1.0 1.0 1.0\n"
              "v 1 0 0 1.0 1.0 1.0\n"
              "v 0 1 0 1.0 1.0 1.0\n"
              "f 1 2 3\n");
    Mesh m = load_ok(t.path);
    ASSERT_FALSE(m.has_vertex_colors);
    ASSERT_TRUE(m.vertex_colors.empty());
}

TEST(obj_valid, failed_load_rollback_preserves_previous_mesh_state)
{
    Mesh m;
    m.vertices.push_back({vec3{1.0f, 2.0f, 3.0f}, vec3{0.0f, 0.0f, 1.0f}, vec2{0.25f, 0.75f}, 0.9f});
    m.vertices.push_back({vec3{4.0f, 5.0f, 6.0f}, vec3{0.0f, 1.0f, 0.0f}, vec2{0.5f, 0.5f}, 0.8f});
    m.triangles.push_back({{0, 0, 0}, 0});
    m.materials.push_back(Material{});
    Material mat{};
    mat.diffuse = {0.2f, 0.3f, 0.4f};
    mat.ambient = {0.1f, 0.2f, 0.3f};
    mat.specular = {0.4f, 0.5f, 0.6f};
    mat.shininess = 16.0f;
    m.materials.push_back(mat);
    m.textures.push_back(Texture{});
    m.tangents.push_back(vec3{1.0f, 0.0f, 0.0f});
    m.tangents.push_back(vec3{0.0f, 1.0f, 0.0f});
    m.vertex_colors.push_back(vec3{0.1f, 0.2f, 0.3f});
    m.vertex_colors.push_back(vec3{0.4f, 0.5f, 0.6f});
    m.has_vertex_colors = true;
    m.has_double_sided = true;

    const Mesh before = m;

    TmpFile bad(tmp_path("rast_obj_rollback.obj"),
                "v 0 0 0\n"
                "v 1 0 0\n"
                "v 0 1 0\n"
                "f 1 2 9\n");

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
    TmpFile mtl(tmp_path("rast_mapkn.mtl"),
                "newmtl M\nKd 1 1 1\nmap_Kn nonexistent_normal.png\n");
    TmpFile obj(tmp_path("rast_mapkn.obj"),
                "mtllib rast_mapkn.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "usemtl M\nf 1 2 3\n");
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
    TmpFile t(tmp_path("rast_partnorm.obj"),
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 0 1 0\n"
              "v 0 0 1\n"
              "vn 0 0 1\n"
              "f 1//1 2//1 3//1\n"
              "f 1 2 4\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{2});
    for (const Vertex &v : m.vertices)
    {
        const float len_sq = v.normal.x * v.normal.x +
                             v.normal.y * v.normal.y +
                             v.normal.z * v.normal.z;
        ASSERT_NEAR(len_sq, 1.0f, 0.01f);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  TEXTURE SUCCESS PATHS — load_tex() with a real BMP on disk
// ═══════════════════════════════════════════════════════════════════════════

TEST(obj_valid, mtl_map_kd_real_file_sets_diffuse_tex)
{
    // First test to exercise the load_tex success branch (mesh_obj.cpp:62-64):
    // a real file is present, so load_tex pushes a Texture and returns index 0.
    TmpFile bmp(tmp_path("rast_kd_tex.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(tmp_path("rast_kd_tex.mtl"),
                "newmtl M\nKd 1 1 1\nmap_Kd rast_kd_tex.bmp\n");
    TmpFile obj(tmp_path("rast_kd_tex.obj"),
                "mtllib rast_kd_tex.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "usemtl M\nf 1 2 3\n");
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_tex, 0);
    ASSERT_EQ(m.textures.size(), size_t{1});
}

TEST(obj_valid, mtl_map_ks_real_file_sets_specular_tex)
{
    TmpFile bmp(tmp_path("rast_ks_tex.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(tmp_path("rast_ks_tex.mtl"),
                "newmtl M\nKs 1 1 1\nmap_Ks rast_ks_tex.bmp\n");
    TmpFile obj(tmp_path("rast_ks_tex.obj"),
                "mtllib rast_ks_tex.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "usemtl M\nf 1 2 3\n");
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].specular_tex, 0);
    ASSERT_EQ(m.textures.size(), size_t{1});
}

TEST(obj_valid, mtl_map_kn_real_file_sets_normal_tex)
{
    // Exercises the !normal_texname.empty() true-branch success path.
    // tinyobjloader's keyword for normal_texname is "norm" (not "map_Kn").
    TmpFile bmp(tmp_path("rast_kn_tex.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(tmp_path("rast_kn_tex.mtl"),
                "newmtl M\nKd 1 1 1\nnorm rast_kn_tex.bmp\n");
    TmpFile obj(tmp_path("rast_kn_tex.obj"),
                "mtllib rast_kn_tex.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "usemtl M\nf 1 2 3\n");
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].normal_tex >= 0);
    ASSERT_EQ(m.textures.size(), size_t{1});
}

TEST(obj_valid, mtl_map_bump_fallback_when_map_kn_absent)
{
    // Only map_Bump declared (no map_Kn) → normal_texname is empty →
    // the else branch at mesh_obj.cpp:84 fires: load_tex(m.bump_texname).
    TmpFile bmp(tmp_path("rast_bump_tex.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile mtl(tmp_path("rast_bump_tex.mtl"),
                "newmtl M\nKd 1 1 1\nmap_Bump rast_bump_tex.bmp\n");
    TmpFile obj(tmp_path("rast_bump_tex.obj"),
                "mtllib rast_bump_tex.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "usemtl M\nf 1 2 3\n");
    Mesh m = load_ok(obj.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].normal_tex >= 0);
    ASSERT_EQ(m.textures.size(), size_t{1});
}

TEST(obj_valid, face_without_usemtl_uses_default_material)
{
    // mtllib declares a material, but no usemtl → tinyobjloader sets mat_id=-1
    // for the face → the mat_id < 0 branch fires → material_idx = 0 (default white).
    TmpFile mtl(tmp_path("rast_no_usemtl.mtl"), "newmtl M\nKd 1 0 0\n");
    TmpFile obj(tmp_path("rast_no_usemtl.obj"),
                "mtllib rast_no_usemtl.mtl\n"
                "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
                "f 1 2 3\n");
    Mesh m = load_ok(obj.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
    ASSERT_EQ(m.triangles[0].material_idx, 0u);
}
