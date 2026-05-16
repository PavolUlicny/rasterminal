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

// ═══════════════════════════════════════════════════════════════════════════
//  UV PROPERTY NAME: u/v (primary alias)
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, ascii_uv_u_v_property_names)
{
    // "u"/"v" is the first alias tried — prior tests only exercise the fallback
    // aliases ("s"/"t" and "texture_u"/"texture_v").
    TmpFile t(tmp_path("rast_uv_uv.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "property float u\nproperty float v\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0 0.1 0.9\n"
              "1 0 0 0.5 0.5\n"
              "0 1 0 0.75 0.25\n"
              "3 0 1 2\n");
    Mesh m = load_ok(t.path);
    ASSERT_NEAR(m.vertices[0].uv.x, 0.1f, 1e-5f);
    ASSERT_NEAR(m.vertices[0].uv.y, 0.9f, 1e-5f);
    ASSERT_NEAR(m.vertices[1].uv.x, 0.5f, 1e-5f);
    ASSERT_NEAR(m.vertices[2].uv.x, 0.75f, 1e-5f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  FLOAT-TYPED VERTEX COLORS (rd_col FLOAT32 and FLOAT64 paths)
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, ascii_vertex_colors_float32)
{
    // "property float red/green/blue" exercises the FLOAT32 default path in
    // rd_col. Prior tests only cover the UINT8 (÷255) path.
    TmpFile t(tmp_path("rast_vcol_f32.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "property float red\nproperty float green\nproperty float blue\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0 0.5 0.25 0.75\n"
              "1 0 0 0.5 0.25 0.75\n"
              "0 1 0 0.5 0.25 0.75\n"
              "3 0 1 2\n");
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{3});
    ASSERT_NEAR(m.vertex_colors[0].x, 0.5f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.25f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.75f, 1e-5f);
}

TEST(ply_valid, binary_le_vertex_colors_float64)
{
    // "property double red/green/blue" exercises the FLOAT64 path in rd_col.
    std::string s =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property double red\nproperty double green\nproperty double blue\n"
        "element face 1\n"
        "property list uchar int vertex_indices\n"
        "end_header\n";
    for (int i = 0; i < 3; i++)
    {
        emit_f32_le(s, static_cast<float>(i));
        emit_f32_le(s, 0.0f);
        emit_f32_le(s, 0.0f);
        emit_f64_le(s, 0.2);
        emit_f64_le(s, 0.4);
        emit_f64_le(s, 0.6);
    }
    s.push_back(3);
    emit_u32_le(s, 0);
    emit_u32_le(s, 1);
    emit_u32_le(s, 2);
    TmpFile t(tmp_path("rast_vcol_f64.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_NEAR(m.vertex_colors[0].x, 0.2f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.4f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.6f, 1e-5f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  FACE COLOR r/g/b ALIAS
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, ascii_face_colors_r_g_b_alias)
{
    // Face element using "r"/"g"/"b" rather than "red"/"green"/"blue" — the
    // second alias branch in load_ply was previously dead.
    TmpFile t(tmp_path("rast_fcol_rgb.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "property uchar r\nproperty uchar g\nproperty uchar b\n"
              "end_header\n"
              "0 0 0\n1 0 0\n0 1 0\n"
              "3 0 1 2 100 150 200\n");
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{3});
    ASSERT_NEAR(m.vertex_colors[0].x, 100.0f / 255.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].y, 150.0f / 255.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].z, 200.0f / 255.0f, 1e-4f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  FACE COLORS + FILE NORMALS: compute_normals always runs
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, face_colors_with_file_normals_recomputes)
{
    // When face colors are present, compute_normals() always runs regardless
    // of whether the file also supplies vertex normals. Deliberate wrong file
    // normal (0,0,-1) verifies the recompute path overwrites it with (0,0,+1).
    TmpFile t(tmp_path("rast_fcol_norm.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "property float nx\nproperty float ny\nproperty float nz\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "property uchar red\nproperty uchar green\nproperty uchar blue\n"
              "end_header\n"
              "0 0 0  0 0 -1\n"
              "1 0 0  0 0 -1\n"
              "0 1 0  0 0 -1\n"
              "3 0 1 2 255 0 0\n");
    Mesh m = load_ok(t.path);
    // Geometry (0,0,0)→(1,0,0)→(0,1,0) CCW from +Z → computed normal is (0,0,+1).
    for (const Vertex &v : m.vertices)
        ASSERT_NEAR(v.normal.z, 1.0f, 1e-4f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ipf < 3 REJECTION
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, ply_face_list_count_below_3)
{
    // Face list with only 2 indices per face → ipf=2 < 3 → load_ply returns false.
    TmpFile t(tmp_path("rast_ipf2.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0\n1 0 0\n0 1 0\n"
              "2 0 1\n");
    assert_rejects(t.path);
}

// ═══════════════════════════════════════════════════════════════════════════
//  OOB INDEX SKIPPING
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, ply_all_faces_have_oob_indices_standard_path)
{
    // All faces reference an out-of-bounds vertex index → every triangle
    // skipped → triangles empty → load_ply returns false.
    TmpFile t(tmp_path("rast_oob_std.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0\n1 0 0\n0 1 0\n"
              "3 0 1 99\n");
    assert_rejects(t.path);
}

TEST(reject, ply_all_faces_have_oob_indices_face_color_path)
{
    // Same but with face colors — exercises the separate OOB guard in the
    // face-color expand path; all vertices skipped → empty → returns false.
    TmpFile t(tmp_path("rast_oob_fcol.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "element face 1\n"
              "property list uchar int vertex_indices\n"
              "property uchar red\nproperty uchar green\nproperty uchar blue\n"
              "end_header\n"
              "0 0 0\n1 0 0\n0 1 0\n"
              "3 0 1 99 255 0 0\n");
    assert_rejects(t.path);
}

TEST(ply_valid, partial_oob_indices_valid_faces_survive)
{
    // Two faces: first has valid indices, second has one OOB index. Only the
    // valid face produces a triangle; the bad one is silently skipped.
    TmpFile t(tmp_path("rast_partial_oob.ply"),
              "ply\nformat ascii 1.0\n"
              "element vertex 3\n"
              "property float x\nproperty float y\nproperty float z\n"
              "element face 2\n"
              "property list uchar int vertex_indices\n"
              "end_header\n"
              "0 0 0\n1 0 0\n0 1 0\n"
              "3 0 1 2\n"
              "3 0 1 99\n");
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
}

// ═══════════════════════════════════════════════════════════════════════════
//  NON-INT32 FACE INDEX TYPES (rd_idx UINT16 and UINT32)
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, binary_le_uint16_face_indices)
{
    // "property list uchar ushort" exercises the UINT16 case in rd_idx.
    // Most PLY files use "int" (INT32); ushort is common in compact meshes.
    std::string s =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 1\n"
        "property list uchar ushort vertex_indices\n"
        "end_header\n";
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    s.push_back('\x03'); // list count (uchar)
    s.push_back('\x00');
    s.push_back('\x00'); // index 0 (uint16 LE)
    s.push_back('\x01');
    s.push_back('\x00'); // index 1
    s.push_back('\x02');
    s.push_back('\x00'); // index 2
    TmpFile t(tmp_path("rast_u16idx.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
}

TEST(ply_valid, binary_le_uint32_face_indices)
{
    // "property list uchar uint" exercises the UINT32 default case in rd_idx.
    // Prior tests all use "int" (INT32) — the default branch is different.
    std::string s =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 1\n"
        "property list uchar uint vertex_indices\n"
        "end_header\n";
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    s.push_back(3);
    emit_u32_le(s, 0);
    emit_u32_le(s, 1);
    emit_u32_le(s, 2);
    TmpFile t(tmp_path("rast_u32idx.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
}

TEST(ply_valid, binary_le_uint8_face_indices)
{
    // "uchar" → UINT8 branch: direct buf[i] read.
    std::string s =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 1\n"
        "property list uchar uchar vertex_indices\n"
        "end_header\n";
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    s.push_back(3);
    s.push_back(0);
    s.push_back(1);
    s.push_back(2);
    TmpFile t(tmp_path("rast_u8idx.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
}

TEST(ply_valid, binary_le_int8_face_indices)
{
    // "char" → INT8 branch: cast<uint32_t>(cast<int8_t>(buf[i])).
    std::string s =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 1\n"
        "property list uchar char vertex_indices\n"
        "end_header\n";
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    s.push_back(3);
    s.push_back(0);
    s.push_back(1);
    s.push_back(2);
    TmpFile t(tmp_path("rast_i8idx.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
}

TEST(ply_valid, binary_le_int16_face_indices)
{
    // "short" → INT16 branch: int16_t memcpy then cast to uint32_t.
    std::string s =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 1\n"
        "property list uchar short vertex_indices\n"
        "end_header\n";
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    s.push_back(3);
    s.push_back('\x00');
    s.push_back('\x00'); // index 0 (int16 LE)
    s.push_back('\x01');
    s.push_back('\x00'); // index 1
    s.push_back('\x02');
    s.push_back('\x00'); // index 2
    TmpFile t(tmp_path("rast_i16idx.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{1});
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
}

// ═══════════════════════════════════════════════════════════════════════════
//  FLOAT64 FACE COLORS (rd_col FLOAT64 path for face element)
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, binary_le_face_colors_float64)
{
    // "double" face colors → FLOAT64 path in rd_col; prior tests only cover
    // FLOAT64 vertex colors.
    std::string s =
        "ply\n"
        "format binary_little_endian 1.0\n"
        "element vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "element face 1\n"
        "property list uchar int vertex_indices\n"
        "property double red\nproperty double green\nproperty double blue\n"
        "end_header\n";
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    s.push_back(3);
    emit_u32_le(s, 0);
    emit_u32_le(s, 1);
    emit_u32_le(s, 2);
    emit_f64_le(s, 1.0);
    emit_f64_le(s, 0.5);
    emit_f64_le(s, 0.0);
    TmpFile t(tmp_path("rast_fcol_f64.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{3});
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.5f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.0f, 1e-5f);
}
