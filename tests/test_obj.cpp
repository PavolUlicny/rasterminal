#include "loader_util.h"

// ═══════════════════════════════════════════════════════════════════════════
//  SHIPPED OBJ MODELS
// ═══════════════════════════════════════════════════════════════════════════

TEST(shipped, obj_suzanne)
{
    load_ok("models/obj/suzanne.obj");
}

TEST(shipped, obj_teapot)
{
    load_ok("models/obj/teapot.obj");
}

TEST(shipped, obj_cube_with_mtl)
{
    Mesh m = load_ok("models/obj/cube.obj");
    // cube.mtl defines multiple materials; index 0 is always default.
    if (m.materials.size() < 2)
        ASSERT_FAIL("cube.obj should load materials from cube.mtl");
}

TEST(shipped, obj_cube_quad_triangulation)
{
    Mesh m = load_ok("models/obj/cube.obj");
    // 6 quad faces each fan-triangulated to 2 → 12 triangles total.
    ASSERT_EQ(m.triangles.size(), size_t{12});
}

TEST(shipped, obj_xyzrgb_dragon)
{
    // ~11 MB, high-poly — exercises the loader on a realistic large mesh.
    load_ok("models/obj/xyzrgb_dragon.obj");
}

// ═══════════════════════════════════════════════════════════════════════════
//  HAND-CRAFTED VALID OBJ — exercises specific format features in isolation
// ═══════════════════════════════════════════════════════════════════════════

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
