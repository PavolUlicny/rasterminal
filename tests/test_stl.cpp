#include "loader_util.h"

// ═══════════════════════════════════════════════════════════════════════════
//  SHIPPED STL MODELS
// ═══════════════════════════════════════════════════════════════════════════

TEST(shipped, stl_ascii_eiffel)
{
    load_ok("models/stl/Eiffel_tower.stl");
}

TEST(shipped, stl_binary_bunny)
{
    load_ok("models/stl/Stanford_Bunny.stl");
}

// ═══════════════════════════════════════════════════════════════════════════
//  HAND-CRAFTED VALID STL
// ═══════════════════════════════════════════════════════════════════════════

TEST(stl_valid, ascii_single_facet)
{
    TmpFile t(tmp_path("rasterminal_test_min.stl"),
              "solid test\n"
              "facet normal 0 0 1\n"
              "  outer loop\n"
              "    vertex 0 0 0\n"
              "    vertex 1 0 0\n"
              "    vertex 0 1 0\n"
              "  endloop\n"
              "endfacet\n"
              "endsolid test\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

TEST(stl_valid, binary_single_triangle)
{
    std::string s(80, 'X');     // 80-byte header (non-"solid")
    emit_u32_le(s, 1);          // tri_count = 1
    for (int i = 0; i < 3; i++) // normal (ignored)
        emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 1);
    emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 1);
    emit_f32_le(s, 0);
    s.push_back(0);
    s.push_back(0); // attribute bytes

    TmpFile t(tmp_path("rasterminal_test_bin.stl"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

TEST(stl_valid, binary_header_starts_with_solid_disambiguates_via_size)
{
    // 80-byte header beginning with "solid " would trigger the ASCII path,
    // but an exact file-size match (84 + 50 × tri_count) forces the binary
    // interpretation. Pinned by load_stl's disambiguation logic.
    std::string s = "solid binary-stl-masquerading-as-ascii";
    s.resize(80, ' '); // pad to exactly 80 bytes
    emit_u32_le(s, 1); // tri_count = 1
    for (int i = 0; i < 3; i++)
        emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 1);
    emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 1);
    emit_f32_le(s, 0);
    s.push_back(0);
    s.push_back(0);

    TmpFile t(tmp_path("rasterminal_test_solidbin.stl"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

// ═══════════════════════════════════════════════════════════════════════════
//  REJECTIONS — malformed/corrupt STL must not crash
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, stl_too_small_for_header)
{
    // Less than 80 bytes — can't even read the header.
    TmpFile t(tmp_path("rasterminal_test_tiny.stl"), "solid\n"); // 6 bytes
    assert_rejects(t.path);
}

TEST(reject, stl_header_only_no_tri_count)
{
    // Exactly 80 bytes: header read succeeds, but there's no tri_count.
    std::string s(80, 'X');
    TmpFile t(tmp_path("rasterminal_test_80.stl"), s);
    assert_rejects(t.path);
}

TEST(reject, stl_binary_inflated_tri_count)
{
    // tri_count = 0xFFFFFFFF in an 84-byte file — would otherwise cause
    // reserve() to throw bad_alloc. Pinned by commit c203488.
    uint8_t buf[84];
    std::memset(buf, 'X', 80);
    buf[80] = 0xFF;
    buf[81] = 0xFF;
    buf[82] = 0xFF;
    buf[83] = 0xFF;
    TmpFile t(tmp_path("rasterminal_test_inflated.stl"), buf, sizeof(buf));
    assert_rejects(t.path);
}

TEST(reject, stl_binary_short_file_nonzero_count)
{
    // tri_count claims 100 triangles, but no triangle data follows.
    uint8_t buf[84];
    std::memset(buf, 'X', 80);
    buf[80] = 100;
    buf[81] = 0;
    buf[82] = 0;
    buf[83] = 0;
    TmpFile t(tmp_path("rasterminal_test_short.stl"), buf, sizeof(buf));
    assert_rejects(t.path);
}

TEST(reject, stl_binary_truncated_mid_triangle)
{
    // tri_count = 2, but we only write one complete triangle + 20 bytes of
    // the second. File-size check accepts (file > 84+50), but load_stl_binary
    // sees short reads and returns false.
    std::string s(80, 'X');
    emit_u32_le(s, 2);
    // First triangle (complete, 50 bytes).
    for (int i = 0; i < 3; i++)
        emit_f32_le(s, 0); // normal
    for (int i = 0; i < 9; i++)
        emit_f32_le(s, 0); // 3 verts
    s.push_back(0);
    s.push_back(0); // attr
    // Second triangle (truncated — only first 20 bytes).
    for (int i = 0; i < 5; i++)
        emit_f32_le(s, 0);
    TmpFile t(tmp_path("rasterminal_test_truncfacet.stl"), s);
    assert_rejects(t.path);
}

TEST(reject, stl_binary_zero_triangles)
{
    // tri_count = 0, exact size match: header takes the binary path and
    // produces an empty mesh, which load_stl rejects.
    std::string s(80, 'X');
    emit_u32_le(s, 0);
    TmpFile t(tmp_path("rasterminal_test_zero.stl"), s);
    assert_rejects(t.path);
}

TEST(reject, stl_ascii_no_facets)
{
    TmpFile t(tmp_path("rasterminal_test_nofacet.stl"),
              "solid empty\n"
              "endsolid empty\n");
    assert_rejects(t.path);
}

TEST(reject, stl_ascii_missing_third_vertex)
{
    // outer loop with only two vertex lines — stl_reader fails to parse the facet.
    TmpFile t(tmp_path("rasterminal_test_2v.stl"),
              "solid test\n"
              "facet normal 0 0 1\n"
              "  outer loop\n"
              "    vertex 0 0 0\n"
              "    vertex 1 0 0\n"
              "  endloop\n"
              "endfacet\n"
              "endsolid test\n");
    assert_rejects(t.path);
}
