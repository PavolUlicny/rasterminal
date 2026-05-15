#include "loader_util.h"

// ═══════════════════════════════════════════════════════════════════════════
//  HAND-CRAFTED VALID PLY
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, ascii_minimal_triangle)
{
    TmpFile t(tmp_path("rasterminal_test_min.ply"),
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
    TmpFile t(tmp_path("rasterminal_test_quad.ply"),
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
    TmpFile t(tmp_path("rasterminal_test_norm.ply"),
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

    TmpFile t(tmp_path("rasterminal_test_le.ply"), s);
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

    TmpFile t(tmp_path("rasterminal_test_be.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

TEST(ply_valid, ascii_face_colors_expanded)
{
    // Face element with uchar red/green/blue: single red triangle.
    // Vertices must be expanded (unshared), vertex_colors filled from face color.
    TmpFile t(tmp_path("rast_fcol_ascii.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "property uchar red\nproperty uchar green\nproperty uchar blue\n"
              "end_header\n"
              "0 0 0\n1 0 0\n0 1 0\n"
              "3 0 1 2 255 0 0\n");
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{3});
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[2].x, 1.0f, 1e-4f);
}

TEST(ply_valid, binary_le_face_colors)
{
    // Same as ascii_face_colors_expanded but binary_little_endian.
    std::string s =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 1\n"
        "property list uchar int vertex_indices\n"
        "property uchar red\nproperty uchar green\nproperty uchar blue\n"
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
    s.push_back(3); // list count
    emit_u32_le(s, 0);
    emit_u32_le(s, 1);
    emit_u32_le(s, 2);
    s.push_back(static_cast<char>(0));   // red   = 0
    s.push_back(static_cast<char>(0));   // green = 0
    s.push_back(static_cast<char>(255)); // blue  = 255

    TmpFile t(tmp_path("rast_fcol_bin.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{3});
    ASSERT_NEAR(m.vertex_colors[0].x, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].z, 1.0f, 1e-4f);
}

TEST(ply_valid, ascii_vertex_colors_normalized)
{
    // uchar red=255, green=128, blue=0 → color {1.0, ~0.502, 0.0}
    TmpFile t(tmp_path("rast_vcol.ply"),
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

TEST(ply_valid, vertex_colors_take_precedence_over_face_colors)
{
    TmpFile t(tmp_path("rast_vcol_precedence.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "property uchar red\nproperty uchar green\nproperty uchar blue\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "property uchar red\nproperty uchar green\nproperty uchar blue\n"
              "end_header\n"
              "0 0 0 255 0 0\n"
              "1 0 0 0 255 0\n"
              "0 1 0 0 0 255\n"
              "3 0 1 2 0 0 0\n");
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{3});
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[1].x, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[1].y, 1.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[2].z, 1.0f, 1e-4f);
}

TEST(ply_valid, vertex_color_alias_r_g_b_is_supported)
{
    TmpFile t(tmp_path("rast_vcol_alias.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "property uchar r\nproperty uchar g\nproperty uchar b\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0 10 20 30\n"
              "1 0 0 10 20 30\n"
              "0 1 0 10 20 30\n"
              "3 0 1 2\n");
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{3});
    ASSERT_NEAR(m.vertex_colors[0].x, 10.0f / 255.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].y, 20.0f / 255.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].z, 30.0f / 255.0f, 1e-4f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  REJECTIONS — malformed/corrupt PLY must not crash
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, ply_missing_magic)
{
    TmpFile t(tmp_path("rasterminal_test_nomagic.ply"),
              "format ascii 1.0\n"
              "element vertex 0\n"
              "end_header\n");
    assert_rejects(t.path);
}

TEST(reject, ply_no_end_header)
{
    TmpFile t(tmp_path("rasterminal_test_noend.ply"),
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
    TmpFile t(tmp_path("rasterminal_test_badtype.ply"),
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
    TmpFile t(tmp_path("rasterminal_test_novert.ply"),
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
    TmpFile t(tmp_path("rasterminal_test_noface.ply"),
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
    TmpFile t(tmp_path("rasterminal_test_zerovert.ply"),
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
    TmpFile t(tmp_path("rasterminal_test_hugevert.ply"),
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
    TmpFile t(tmp_path("rasterminal_test_negvert.ply"),
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
    TmpFile t(tmp_path("rasterminal_test_hugeface.ply"),
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
    TmpFile t(tmp_path("rasterminal_test_trunc.ply"), s);
    assert_rejects(t.path);
}

TEST(reject, ply_zero_face_count_with_vertices)
{
    // element face 0 — n_faces==0 → rejected by the n_faces==0 guard.
    // Different from ply_no_face_element which omits the element entirely.
    TmpFile t(tmp_path("rasterminal_test_zeroface.ply"),
              "ply\n"
              "format ascii 1.0\n"
              "element vertex 3\n"
              "property float x\n"
              "property float y\n"
              "property float z\n"
              "element face 0\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0\n"
              "1 0 0\n"
              "0 1 0\n");
    assert_rejects(t.path);
}

TEST(reject, ply_missing_xyz_properties)
{
    // Vertex element present but no x/y/z — tinyply throws on
    // request_properties_from_element, which load_ply catches and returns false.
    TmpFile t(tmp_path("rasterminal_test_noxyz.ply"),
              "ply\n"
              "format ascii 1.0\n"
              "element vertex 3\n"
              "property float dummy\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0\n"
              "0\n"
              "0\n"
              "3 0 1 2\n");
    assert_rejects(t.path);
}

// ═══════════════════════════════════════════════════════════════════════════
//  UV PROPERTY NAME FALLBACKS
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, ascii_uv_st_property_names)
{
    // Loader falls back to "s"/"t" when "u"/"v" are absent.
    TmpFile t(tmp_path("rast_uv_st.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "property float s\nproperty float t\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0 0.1 0.2\n"
              "1 0 0 0.3 0.4\n"
              "0 1 0 0.5 0.6\n"
              "3 0 1 2\n");
    Mesh m = load_ok(t.path);
    ASSERT_NEAR(m.vertices[0].uv.x, 0.1f, 1e-5f);
    ASSERT_NEAR(m.vertices[0].uv.y, 0.2f, 1e-5f);
    ASSERT_NEAR(m.vertices[1].uv.x, 0.3f, 1e-5f);
    ASSERT_NEAR(m.vertices[2].uv.x, 0.5f, 1e-5f);
}

TEST(ply_valid, ascii_uv_texture_uv_property_names)
{
    // Loader falls back to "texture_u"/"texture_v" when "u"/"v" and "s"/"t" are absent.
    TmpFile t(tmp_path("rast_uv_tuvtv.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "property float texture_u\nproperty float texture_v\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0 0.25 0.75\n"
              "1 0 0 0.50 0.50\n"
              "0 1 0 0.75 0.25\n"
              "3 0 1 2\n");
    Mesh m = load_ok(t.path);
    ASSERT_NEAR(m.vertices[0].uv.x, 0.25f, 1e-5f);
    ASSERT_NEAR(m.vertices[0].uv.y, 0.75f, 1e-5f);
    ASSERT_NEAR(m.vertices[1].uv.x, 0.50f, 1e-5f);
    ASSERT_NEAR(m.vertices[2].uv.x, 0.75f, 1e-5f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  FLOAT64 COORDINATE TYPE
// ═══════════════════════════════════════════════════════════════════════════

static void emit_f64_le(std::string &s, double v)
{
    uint64_t u;
    std::memcpy(&u, &v, 8);
    for (int i = 0; i < 8; i++)
        s.push_back(static_cast<char>((u >> (i * 8)) & 0xFFu));
}

TEST(ply_valid, binary_le_float64_coordinates)
{
    // Vertices declared as "double" (FLOAT64) — loader must widen to float.
    std::string s =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 3\n"
        "property double x\n"
        "property double y\n"
        "property double z\n"
        "element face 1\n"
        "property list uchar int vertex_indices\n"
        "end_header\n";
    emit_f64_le(s, 0.0);
    emit_f64_le(s, 0.0);
    emit_f64_le(s, 0.0);
    emit_f64_le(s, 1.5);
    emit_f64_le(s, 0.0);
    emit_f64_le(s, 0.0);
    emit_f64_le(s, 0.0);
    emit_f64_le(s, 2.5);
    emit_f64_le(s, 0.0);
    s.push_back(3);
    emit_u32_le(s, 0);
    emit_u32_le(s, 1);
    emit_u32_le(s, 2);

    TmpFile t(tmp_path("rast_f64.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
    ASSERT_NEAR(m.vertices[1].pos.x, 1.5f, 1e-5f);
    ASSERT_NEAR(m.vertices[2].pos.y, 2.5f, 1e-5f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  FACE PROPERTY NAME ALIAS: vertex_index (singular)
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, ascii_vertex_index_singular_alias)
{
    // Some PLY files use "vertex_index" (singular) instead of "vertex_indices".
    TmpFile t(tmp_path("rast_vi_singular.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "element face 1\n"
              "property list uchar int vertex_index\n"
              "end_header\n"
              "0 0 0\n"
              "1 0 0\n"
              "0 1 0\n"
              "3 0 1 2\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}
