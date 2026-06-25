#include "tests/loader_util.h"

// Format-agnostic rejection tests for Mesh::load_model's extension dispatch.
// Per-format validity and parser-level rejection live in test_obj/ply/stl.

TEST(reject, missing_file)
{
    assert_rejects(tmp_path("rasterminal_does_not_exist_12345.obj"));
}

TEST(reject, no_extension)
{
    TmpFile t(tmp_path("rasterminal_test_noext"), "v 0 0 0\n");
    assert_rejects(t.path);
}

TEST(reject, unknown_extension)
{
    TmpFile t(tmp_path("rasterminal_test.xyz"), "irrelevant");
    assert_rejects(t.path);
}

TEST(reject, empty_file_obj)
{
    TmpFile t(tmp_path("rasterminal_test_empty.obj"), "");
    assert_rejects(t.path);
}

TEST(reject, empty_file_ply)
{
    TmpFile t(tmp_path("rasterminal_test_empty.ply"), "");
    assert_rejects(t.path);
}

TEST(reject, empty_file_stl)
{
    TmpFile t(tmp_path("rasterminal_test_empty.stl"), "");
    assert_rejects(t.path);
}

TEST(dispatch, multiple_dots_uses_last_extension)
{
    // find_last_of('.') must pick the last dot, not the first.
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    TmpFile t(tmp_path("rast_my.model.obj"), obj);
    Mesh m = load_ok(t.path);
    ASSERT_FALSE(m.triangles.empty());
}

TEST(dispatch, uppercase_extension_is_recognized)
{
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    TmpFile t(tmp_path("rast_upper.OBJ"), obj);
    Mesh m = load_ok(t.path);
    ASSERT_FALSE(m.triangles.empty());
}
