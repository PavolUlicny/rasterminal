#include "loader_util.h"

// ═══════════════════════════════════════════════════════════════════════════
//  SHIPPED PLY MODELS
// ═══════════════════════════════════════════════════════════════════════════

TEST(shipped, ply_ascii_sphere)
{
    load_ok("models/ply/sphere.ply");
}

TEST(shipped, ply_binary_sphere)
{
    load_ok("models/ply/sphere_binary.ply");
}

// ═══════════════════════════════════════════════════════════════════════════
//  HAND-CRAFTED VALID PLY
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, ascii_minimal_triangle)
{
    TmpFile t("/tmp/rasterminal_test_min.ply",
              "ply\n"
              "format ascii 1.0\n"
              "element vertex 3\n"
              "property float x\n"
              "property float y\n"
              "property float z\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0\n"
              "1 0 0\n"
              "0 1 0\n"
              "3 0 1 2\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{3});
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

TEST(ply_valid, ascii_quad_fan_triangulates)
{
    TmpFile t("/tmp/rasterminal_test_quad.ply",
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
              "1 1 0\n"
              "0 1 0\n"
              "4 0 1 2 3\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{2});
}

TEST(ply_valid, ascii_with_normals_skips_recompute)
{
    // File provides normals; loader should use them rather than recompute.
    TmpFile t("/tmp/rasterminal_test_norm.ply",
              "ply\n"
              "format ascii 1.0\n"
              "element vertex 3\n"
              "property float x\n"
              "property float y\n"
              "property float z\n"
              "property float nx\n"
              "property float ny\n"
              "property float nz\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0 0 0 1\n"
              "1 0 0 0 0 1\n"
              "0 1 0 0 0 1\n"
              "3 0 1 2\n");
    Mesh m = load_ok(t.path);
    ASSERT_NEAR(m.vertices[0].normal.z, 1.0f, 1e-6f);
}

TEST(ply_valid, binary_little_endian_triangle)
{
    std::string s =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 3\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "element face 1\n"
        "property list uchar int vertex_indices\n"
        "end_header\n";
    emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 1);
    emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 0);
    emit_f32_le(s, 1);
    emit_f32_le(s, 0);
    s.push_back(3); // list count (uchar)
    emit_u32_le(s, 0);
    emit_u32_le(s, 1);
    emit_u32_le(s, 2);

    TmpFile t("/tmp/rasterminal_test_le.ply", s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

TEST(ply_valid, binary_big_endian_triangle)
{
    std::string s =
        "ply\n"
        "format binary_big_endian 1.0\n"
        "element vertex 3\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "element face 1\n"
        "property list uchar int vertex_indices\n"
        "end_header\n";
    emit_f32_be(s, 0);
    emit_f32_be(s, 0);
    emit_f32_be(s, 0);
    emit_f32_be(s, 1);
    emit_f32_be(s, 0);
    emit_f32_be(s, 0);
    emit_f32_be(s, 0);
    emit_f32_be(s, 1);
    emit_f32_be(s, 0);
    s.push_back(3);
    emit_u32_be(s, 0);
    emit_u32_be(s, 1);
    emit_u32_be(s, 2);

    TmpFile t("/tmp/rasterminal_test_be.ply", s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

TEST(ply_valid, ascii_vertex_colors_normalized)
{
    // uchar red=255, green=128, blue=0 → color {1.0, ~0.502, 0.0}
    TmpFile t("/tmp/rast_vcol.ply",
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "property uchar red\nproperty uchar green\nproperty uchar blue\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0 255 128 0\n"
              "1 0 0 255 128 0\n"
              "0 1 0 255 128 0\n"
              "3 0 1 2\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{3});
    ASSERT_EQ(m.vertex_colors.size(), size_t{3});
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].y, 128.0f / 255.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.0f, 1e-4f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  REJECTIONS — malformed/corrupt PLY must not crash
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, ply_missing_magic)
{
    TmpFile t("/tmp/rasterminal_test_nomagic.ply",
              "format ascii 1.0\n"
              "element vertex 0\n"
              "end_header\n");
    assert_rejects(t.path);
}

TEST(reject, ply_no_end_header)
{
    TmpFile t("/tmp/rasterminal_test_noend.ply",
              "ply\n"
              "format ascii 1.0\n"
              "element vertex 1\n"
              "property float x\n");
    assert_rejects(t.path);
}

TEST(reject, ply_unknown_property_type)
{
    // "quadruple" is not a PLY type — ply_parse_ptype returns UNKNOWN and
    // the header parser rejects immediately (would desync binary reads).
    TmpFile t("/tmp/rasterminal_test_badtype.ply",
              "ply\n"
              "format ascii 1.0\n"
              "element vertex 1\n"
              "property quadruple x\n"
              "end_header\n"
              "0\n");
    assert_rejects(t.path);
}

TEST(reject, ply_no_vertex_element)
{
    TmpFile t("/tmp/rasterminal_test_novert.ply",
              "ply\n"
              "format ascii 1.0\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "3 0 1 2\n");
    assert_rejects(t.path);
}

TEST(reject, ply_no_face_element)
{
    TmpFile t("/tmp/rasterminal_test_noface.ply",
              "ply\n"
              "format ascii 1.0\n"
              "element vertex 3\n"
              "property float x\n"
              "property float y\n"
              "property float z\n"
              "end_header\n"
              "0 0 0\n"
              "1 0 0\n"
              "0 1 0\n");
    assert_rejects(t.path);
}

TEST(reject, ply_zero_vertex_count)
{
    // Header parser rejects when vertex count <= 0.
    TmpFile t("/tmp/rasterminal_test_zerovert.ply",
              "ply\n"
              "format ascii 1.0\n"
              "element vertex 0\n"
              "property float x\n"
              "element face 0\n"
              "property list uchar int vertex_indices\n"
              "end_header\n");
    assert_rejects(t.path);
}

TEST(reject, ply_inflated_vertex_count)
{
    // Defence against header claiming INT_MAX vertices in a tiny file.
    // Pinned by the file-size bounds check added in commit c203488.
    TmpFile t("/tmp/rasterminal_test_hugevert.ply",
              "ply\n"
              "format binary_little_endian 1.0\n"
              "element vertex 2147483647\n"
              "property float x\n"
              "property float y\n"
              "property float z\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n");
    assert_rejects(t.path);
}

TEST(reject, ply_negative_vertex_count)
{
    TmpFile t("/tmp/rasterminal_test_negvert.ply",
              "ply\n"
              "format binary_little_endian 1.0\n"
              "element vertex -1\n"
              "property float x\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n");
    assert_rejects(t.path);
}

TEST(reject, ply_inflated_face_count)
{
    TmpFile t("/tmp/rasterminal_test_hugeface.ply",
              "ply\n"
              "format ascii 1.0\n"
              "element vertex 1\n"
              "property float x\n"
              "property float y\n"
              "property float z\n"
              "element face 2147483647\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0\n");
    assert_rejects(t.path);
}

TEST(reject, ply_truncated_binary_data)
{
    // Header promises 10 vertices but data section has room for < 10.
    // File-size bounds check allows it past reserve(), but the binary reader
    // then hits end-of-buffer and sets truncated=true.
    std::string s =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 10\n"
        "property float x\n"
        "property float y\n"
        "property float z\n"
        "element face 1\n"
        "property list uchar int vertex_indices\n"
        "end_header\n";
    // Only 2 vertices of data (24 bytes) — far short of 10 × 12 = 120.
    for (int i = 0; i < 6; i++)
        emit_f32_le(s, 0);
    TmpFile t("/tmp/rasterminal_test_trunc.ply", s);
    assert_rejects(t.path);
}
