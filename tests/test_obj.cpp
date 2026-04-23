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

TEST(shipped, obj_penguin_textured)
{
    Mesh m = load_ok("models/obj/PenguinBaseMesh.obj");
    // Penguin has a diffuse texture map referenced via map_Kd.
    if (m.textures.empty())
        ASSERT_FAIL("PenguinBaseMesh.obj should load its diffuse texture");
}

TEST(shipped, obj_katana_textured)
{
    Mesh m = load_ok("models/obj/katana.obj");
    if (m.textures.empty())
        ASSERT_FAIL("katana.obj should load textures from its MTL");
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
    TmpFile t("/tmp/rasterminal_test_tri.obj",
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 0 1 0\n"
              "f 1 2 3\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

TEST(obj_valid, quad_fan_triangulates_to_two)
{
    TmpFile t("/tmp/rasterminal_test_quad.obj",
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
    TmpFile t("/tmp/rasterminal_test_ngon.obj",
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
    TmpFile t("/tmp/rasterminal_test_neg.obj",
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 0 1 0\n"
              "f -3 -2 -1\n"); // equivalent to "f 1 2 3"
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

TEST(obj_valid, uv_and_normal_references)
{
    TmpFile t("/tmp/rasterminal_test_uvn.obj",
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
    TmpFile t("/tmp/rasterminal_test_shared.obj",
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
    TmpFile t("/tmp/rasterminal_test_comments.obj",
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
    TmpFile t("/tmp/rasterminal_test_cmts.obj",
              "# just a comment\n"
              "# another one\n");
    assert_rejects(t.path);
}

TEST(reject, obj_vertices_but_no_faces)
{
    TmpFile t("/tmp/rasterminal_test_nofaces.obj",
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 0 1 0\n");
    assert_rejects(t.path);
}

TEST(reject, obj_malformed_vertex)
{
    // "v" with only 2 coordinates should fail sscanf (needs 3).
    TmpFile t("/tmp/rasterminal_test_badv.obj",
              "v 1.0 2.0\n"
              "v 0 0 0\n"
              "v 1 0 0\n"
              "f 1 2 3\n");
    assert_rejects(t.path);
}

TEST(reject, obj_malformed_normal)
{
    TmpFile t("/tmp/rasterminal_test_badvn.obj",
              "v 0 0 0\n"
              "v 1 0 0\n"
              "v 0 1 0\n"
              "vn 0 0\n" // missing z
              "f 1 2 3\n");
    assert_rejects(t.path);
}

TEST(reject, obj_all_face_indices_out_of_range)
{
    // Faces reference non-existent vertices → every face is skipped → no
    // triangles → load fails with "triangles.empty()".
    TmpFile t("/tmp/rasterminal_test_oob.obj",
              "v 0 0 0\n"
              "v 1 0 0\n"
              "f 5 6 7\n"
              "f 10 11 12\n");
    assert_rejects(t.path);
}
