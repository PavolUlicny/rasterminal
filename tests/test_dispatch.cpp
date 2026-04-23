#include "loader_util.h"

// Format-agnostic rejection tests for Mesh::load_model's extension dispatch.
// Per-format validity and parser-level rejection live in test_obj/ply/stl.

TEST(reject, missing_file)
{
    assert_rejects("/tmp/rasterminal_does_not_exist_12345.obj");
}

TEST(reject, no_extension)
{
    TmpFile t("/tmp/rasterminal_test_noext", "v 0 0 0\n");
    assert_rejects(t.path);
}

TEST(reject, unknown_extension)
{
    TmpFile t("/tmp/rasterminal_test.xyz", "irrelevant");
    assert_rejects(t.path);
}

TEST(reject, empty_file_obj)
{
    TmpFile t("/tmp/rasterminal_test_empty.obj", "");
    assert_rejects(t.path);
}

TEST(reject, empty_file_ply)
{
    TmpFile t("/tmp/rasterminal_test_empty.ply", "");
    assert_rejects(t.path);
}

TEST(reject, empty_file_stl)
{
    TmpFile t("/tmp/rasterminal_test_empty.stl", "");
    assert_rejects(t.path);
}
