#include "tests/inline_bmp.h"
#include "tests/loader_util.h"

// ═══════════════════════════════════════════════════════════════════════════
//  HAND-CRAFTED VALID PLY
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, ascii_minimal_triangle)
{
    TmpFile t(
        tmp_path("rasterminal_test_min.ply"), "ply\n"
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
                                              "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{ 3 });
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(ply_valid, ascii_quad_fan_triangulates)
{
    TmpFile t(
        tmp_path("rasterminal_test_quad.ply"), "ply\n"
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
                                               "4 0 1 2 3\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 2 });
}

TEST(ply_valid, ascii_with_normals_skips_recompute)
{
    // File provides normals; loader should use them rather than recompute.
    TmpFile t(
        tmp_path("rasterminal_test_norm.ply"), "ply\n"
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
                                               "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_NEAR(m.vertices[0].normal.z, 1.0f, 1e-6f);
}

TEST(ply_valid, binary_little_endian_triangle)
{
    std::string s = "ply\n"
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
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(ply_valid, binary_big_endian_triangle)
{
    std::string s = "ply\n"
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
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(ply_valid, ascii_face_colors_expanded)
{
    // Face element with uchar red/green/blue: single red triangle.
    // Vertices must be expanded (unshared), vertex_colors filled from face color.
    TmpFile t(
        tmp_path("rast_fcol_ascii.ply"), "ply\nformat ascii 1.0\n"
                                         "element vertex 3\n"
                                         "property float x\nproperty float y\nproperty float z\n"
                                         "element face 1\n"
                                         "property list uchar int vertex_indices\n"
                                         "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                         "end_header\n"
                                         "0 0 0\n1 0 0\n0 1 0\n"
                                         "3 0 1 2 255 0 0\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{ 3 });
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[2].x, 1.0f, 1e-4f);
}

TEST(ply_valid, binary_le_face_colors)
{
    // Same as ascii_face_colors_expanded but binary_little_endian.
    std::string s = "ply\n"
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
    ASSERT_EQ(m.vertex_colors.size(), size_t{ 3 });
    ASSERT_NEAR(m.vertex_colors[0].x, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].z, 1.0f, 1e-4f);
}

TEST(ply_valid, ascii_vertex_colors_normalized)
{
    // uchar red=255, green=128, blue=0 → color {1.0, ~0.502, 0.0}
    TmpFile t(
        tmp_path("rast_vcol.ply"), "ply\nformat ascii 1.0\n"
                                   "element vertex 3\n"
                                   "property float x\nproperty float y\nproperty float z\n"
                                   "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                   "element face 1\n"
                                   "property list uchar int vertex_indices\n"
                                   "end_header\n"
                                   "0 0 0 255 128 0\n"
                                   "1 0 0 255 128 0\n"
                                   "0 1 0 255 128 0\n"
                                   "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{ 3 });
    ASSERT_EQ(m.vertex_colors.size(), size_t{ 3 });
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].y, 128.0f / 255.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.0f, 1e-4f);
}

TEST(ply_valid, vertex_colors_take_precedence_over_face_colors)
{
    TmpFile t(
        tmp_path("rast_vcol_precedence.ply"), "ply\nformat ascii 1.0\n"
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
                                              "3 0 1 2 0 0 0\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{ 3 });
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[1].x, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[1].y, 1.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[2].z, 1.0f, 1e-4f);
}

TEST(ply_valid, vertex_color_alias_r_g_b_is_supported)
{
    TmpFile t(
        tmp_path("rast_vcol_alias.ply"), "ply\nformat ascii 1.0\n"
                                         "element vertex 3\n"
                                         "property float x\nproperty float y\nproperty float z\n"
                                         "property uchar r\nproperty uchar g\nproperty uchar b\n"
                                         "element face 1\n"
                                         "property list uchar int vertex_indices\n"
                                         "end_header\n"
                                         "0 0 0 10 20 30\n"
                                         "1 0 0 10 20 30\n"
                                         "0 1 0 10 20 30\n"
                                         "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{ 3 });
    ASSERT_NEAR(m.vertex_colors[0].x, 10.0f / 255.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].y, 20.0f / 255.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].z, 30.0f / 255.0f, 1e-4f);
}

TEST(ply_valid, float_vertex_colors)
{
    // FLOAT32 colors are in 0..1 and pass through rd_col unchanged (no /255). Guards
    // that the type predicate keeps FLOAT32 accepted, not just UINT8.
    TmpFile t(
        tmp_path("rast_vcol_float.ply"), "ply\nformat ascii 1.0\n"
                                         "element vertex 3\n"
                                         "property float x\nproperty float y\nproperty float z\n"
                                         "property float red\nproperty float green\nproperty float blue\n"
                                         "element face 1\n"
                                         "property list uchar int vertex_indices\n"
                                         "end_header\n"
                                         "0 0 0 1.0 0.5 0.0\n"
                                         "1 0 0 1.0 0.5 0.0\n"
                                         "0 1 0 1.0 0.5 0.0\n"
                                         "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{ 3 });
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.5f, 1e-4f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.0f, 1e-4f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  REJECTIONS — malformed/corrupt PLY must not crash
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, ply_missing_magic)
{
    TmpFile t(
        tmp_path("rasterminal_test_nomagic.ply"), "format ascii 1.0\n"
                                                  "element vertex 0\n"
                                                  "end_header\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_no_end_header)
{
    TmpFile t(
        tmp_path("rasterminal_test_noend.ply"), "ply\n"
                                                "format ascii 1.0\n"
                                                "element vertex 1\n"
                                                "property float x\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_unknown_property_type)
{
    // "quadruple" is not a PLY type — ply_parse_ptype returns UNKNOWN and
    // the header parser rejects immediately (would desync binary reads).
    TmpFile t(
        tmp_path("rasterminal_test_badtype.ply"), "ply\n"
                                                  "format ascii 1.0\n"
                                                  "element vertex 1\n"
                                                  "property quadruple x\n"
                                                  "end_header\n"
                                                  "0\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_vertex_color_unsupported_type)
{
    // ushort vertex colors: rd_col handles only UINT8/FLOAT32/FLOAT64. A 2-byte-per-
    // element buffer indexed with rd_col's 4-byte stride would read out of bounds, so
    // the loader must reject rather than misread.
    TmpFile t(
        tmp_path("rasterminal_test_vcol_ushort.ply"),
        "ply\nformat ascii 1.0\n"
        "element vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property ushort red\nproperty ushort green\nproperty ushort blue\n"
        "element face 1\n"
        "property list uchar int vertex_indices\n"
        "end_header\n"
        "0 0 0 65535 32768 0\n"
        "1 0 0 65535 32768 0\n"
        "0 1 0 65535 32768 0\n"
        "3 0 1 2\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_vertex_alpha_unsupported_type)
{
    // Valid uchar RGB but a ushort alpha property — alpha rides the same rd_col
    // decode path, so an unsupported alpha type must reject too.
    TmpFile t(
        tmp_path("rasterminal_test_valpha_ushort.ply"),
        "ply\nformat ascii 1.0\n"
        "element vertex 3\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property uchar red\nproperty uchar green\nproperty uchar blue\nproperty ushort alpha\n"
        "element face 1\n"
        "property list uchar int vertex_indices\n"
        "end_header\n"
        "0 0 0 255 128 0 65535\n"
        "1 0 0 255 128 0 65535\n"
        "0 1 0 255 128 0 65535\n"
        "3 0 1 2\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_face_color_unsupported_type)
{
    // int (INT32) face colors with no vertex colors selects the face-color path,
    // which also decodes via rd_col. Unsupported type must reject.
    TmpFile t(
        tmp_path("rasterminal_test_fcol_int.ply"), "ply\nformat ascii 1.0\n"
                                                   "element vertex 3\n"
                                                   "property float x\nproperty float y\nproperty float z\n"
                                                   "element face 1\n"
                                                   "property list uchar int vertex_indices\n"
                                                   "property int red\nproperty int green\nproperty int blue\n"
                                                   "end_header\n"
                                                   "0 0 0\n"
                                                   "1 0 0\n"
                                                   "0 1 0\n"
                                                   "3 0 1 2 255 0 0\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_no_vertex_element)
{
    TmpFile t(
        tmp_path("rasterminal_test_novert.ply"), "ply\n"
                                                 "format ascii 1.0\n"
                                                 "element face 1\n"
                                                 "property list uchar int vertex_indices\n"
                                                 "end_header\n"
                                                 "3 0 1 2\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_no_face_element)
{
    TmpFile t(
        tmp_path("rasterminal_test_noface.ply"), "ply\n"
                                                 "format ascii 1.0\n"
                                                 "element vertex 3\n"
                                                 "property float x\n"
                                                 "property float y\n"
                                                 "property float z\n"
                                                 "end_header\n"
                                                 "0 0 0\n"
                                                 "1 0 0\n"
                                                 "0 1 0\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_zero_vertex_count)
{
    // Header parser rejects when vertex count <= 0.
    TmpFile t(
        tmp_path("rasterminal_test_zerovert.ply"), "ply\n"
                                                   "format ascii 1.0\n"
                                                   "element vertex 0\n"
                                                   "property float x\n"
                                                   "element face 0\n"
                                                   "property list uchar int vertex_indices\n"
                                                   "end_header\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_inflated_vertex_count)
{
    // Defence against header claiming INT_MAX vertices in a tiny file.
    // Pinned by the file-size bounds check added in commit c203488.
    TmpFile t(
        tmp_path("rasterminal_test_hugevert.ply"), "ply\n"
                                                   "format binary_little_endian 1.0\n"
                                                   "element vertex 2147483647\n"
                                                   "property float x\n"
                                                   "property float y\n"
                                                   "property float z\n"
                                                   "element face 1\n"
                                                   "property list uchar int vertex_indices\n"
                                                   "end_header\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_negative_vertex_count)
{
    TmpFile t(
        tmp_path("rasterminal_test_negvert.ply"), "ply\n"
                                                  "format binary_little_endian 1.0\n"
                                                  "element vertex -1\n"
                                                  "property float x\n"
                                                  "element face 1\n"
                                                  "property list uchar int vertex_indices\n"
                                                  "end_header\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_inflated_face_count)
{
    TmpFile t(
        tmp_path("rasterminal_test_hugeface.ply"), "ply\n"
                                                   "format ascii 1.0\n"
                                                   "element vertex 1\n"
                                                   "property float x\n"
                                                   "property float y\n"
                                                   "property float z\n"
                                                   "element face 2147483647\n"
                                                   "property list uchar int vertex_indices\n"
                                                   "end_header\n"
                                                   "0 0 0\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_truncated_binary_data)
{
    // Header promises 10 vertices but data section has room for < 10.
    // File-size bounds check allows it past reserve(), but the binary reader
    // then hits end-of-buffer and sets truncated=true.
    std::string s = "ply\n"
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
    {
        emit_f32_le(s, 0);
    }
    TmpFile t(tmp_path("rasterminal_test_trunc.ply"), s);
    assert_rejects(t.path);
}

TEST(reject, ply_zero_face_count_with_vertices)
{
    // element face 0 — n_faces==0 → rejected by the n_faces==0 guard.
    // Different from ply_no_face_element which omits the element entirely.
    TmpFile t(
        tmp_path("rasterminal_test_zeroface.ply"), "ply\n"
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
                                                   "0 1 0\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_missing_xyz_properties)
{
    // Vertex element present but no x/y/z — tinyply throws on
    // request_properties_from_element, which load_ply catches and returns false.
    TmpFile t(
        tmp_path("rasterminal_test_noxyz.ply"), "ply\n"
                                                "format ascii 1.0\n"
                                                "element vertex 3\n"
                                                "property float dummy\n"
                                                "element face 1\n"
                                                "property list uchar int vertex_indices\n"
                                                "end_header\n"
                                                "0\n"
                                                "0\n"
                                                "0\n"
                                                "3 0 1 2\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_non_finite_vertex)
{
    // A genuine +inf float (0x7F800000) in a binary-LE vertex must be rejected by
    // load_model's post-load finiteness scan. Binary is deliberate: tinyply's ASCII reader
    // rejects a bare "inf"/"nan" token on its own (a pre-existing path), so only a raw
    // bit-pattern actually exercises the new finiteness guard rather than the parser.
    std::string s = "ply\n"
                    "format binary_little_endian 1.0\n"
                    "element vertex 3\n"
                    "property float x\n"
                    "property float y\n"
                    "property float z\n"
                    "element face 1\n"
                    "property list uchar int vertex_indices\n"
                    "end_header\n";
    emit_u32_le(s, 0x7F800000u); // v0.x = +inf
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
    TmpFile t(tmp_path("rasterminal_test_ply_inf.ply"), s);
    assert_rejects(t.path);
}

// ═══════════════════════════════════════════════════════════════════════════
//  UV PROPERTY NAME FALLBACKS
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, ascii_uv_st_property_names)
{
    // Loader falls back to "s"/"t" when "u"/"v" are absent.
    TmpFile t(
        tmp_path("rast_uv_st.ply"), "ply\nformat ascii 1.0\n"
                                    "element vertex 3\n"
                                    "property float x\nproperty float y\nproperty float z\n"
                                    "property float s\nproperty float t\n"
                                    "element face 1\n"
                                    "property list uchar int vertex_indices\n"
                                    "end_header\n"
                                    "0 0 0 0.1 0.2\n"
                                    "1 0 0 0.3 0.4\n"
                                    "0 1 0 0.5 0.6\n"
                                    "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_NEAR(m.vertices[0].uv.x, 0.1f, 1e-5f);
    ASSERT_NEAR(m.vertices[0].uv.y, 0.2f, 1e-5f);
    ASSERT_NEAR(m.vertices[1].uv.x, 0.3f, 1e-5f);
    ASSERT_NEAR(m.vertices[2].uv.x, 0.5f, 1e-5f);
}

TEST(ply_valid, ascii_uv_texture_uv_property_names)
{
    // Loader falls back to "texture_u"/"texture_v" when "u"/"v" and "s"/"t" are absent.
    TmpFile t(
        tmp_path("rast_uv_tuvtv.ply"), "ply\nformat ascii 1.0\n"
                                       "element vertex 3\n"
                                       "property float x\nproperty float y\nproperty float z\n"
                                       "property float texture_u\nproperty float texture_v\n"
                                       "element face 1\n"
                                       "property list uchar int vertex_indices\n"
                                       "end_header\n"
                                       "0 0 0 0.25 0.75\n"
                                       "1 0 0 0.50 0.50\n"
                                       "0 1 0 0.75 0.25\n"
                                       "3 0 1 2\n"
    );
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
    uint64_t u = 0;
    std::memcpy(&u, &v, 8);
    for (int i = 0; i < 8; i++)
    {
        s.push_back(static_cast<char>((u >> (i * 8)) & 0xFFu));
    }
}

TEST(ply_valid, binary_le_float64_coordinates)
{
    // Vertices declared as "double" (FLOAT64) — loader must widen to float.
    std::string s = "ply\n"
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
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
    ASSERT_NEAR(m.vertices[1].pos.x, 1.5f, 1e-5f);
    ASSERT_NEAR(m.vertices[2].pos.y, 2.5f, 1e-5f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  FACE PROPERTY NAME ALIAS: vertex_index (singular)
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, ascii_vertex_index_singular_alias)
{
    // Some PLY files use "vertex_index" (singular) instead of "vertex_indices".
    TmpFile t(
        tmp_path("rast_vi_singular.ply"), "ply\nformat ascii 1.0\n"
                                          "element vertex 3\n"
                                          "property float x\nproperty float y\nproperty float z\n"
                                          "element face 1\n"
                                          "property list uchar int vertex_index\n"
                                          "end_header\n"
                                          "0 0 0\n"
                                          "1 0 0\n"
                                          "0 1 0\n"
                                          "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

// ═══════════════════════════════════════════════════════════════════════════
//  UV PROPERTY NAME: u/v (primary alias)
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, ascii_uv_u_v_property_names)
{
    // "u"/"v" is the first alias tried — prior tests only exercise the fallback
    // aliases ("s"/"t" and "texture_u"/"texture_v").
    TmpFile t(
        tmp_path("rast_uv_uv.ply"), "ply\nformat ascii 1.0\n"
                                    "element vertex 3\n"
                                    "property float x\nproperty float y\nproperty float z\n"
                                    "property float u\nproperty float v\n"
                                    "element face 1\n"
                                    "property list uchar int vertex_indices\n"
                                    "end_header\n"
                                    "0 0 0 0.1 0.9\n"
                                    "1 0 0 0.5 0.5\n"
                                    "0 1 0 0.75 0.25\n"
                                    "3 0 1 2\n"
    );
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
    TmpFile t(
        tmp_path("rast_vcol_f32.ply"), "ply\nformat ascii 1.0\n"
                                       "element vertex 3\n"
                                       "property float x\nproperty float y\nproperty float z\n"
                                       "property float red\nproperty float green\nproperty float blue\n"
                                       "element face 1\n"
                                       "property list uchar int vertex_indices\n"
                                       "end_header\n"
                                       "0 0 0 0.5 0.25 0.75\n"
                                       "1 0 0 0.5 0.25 0.75\n"
                                       "0 1 0 0.5 0.25 0.75\n"
                                       "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{ 3 });
    ASSERT_NEAR(m.vertex_colors[0].x, 0.5f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.25f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.75f, 1e-5f);
}

TEST(ply_valid, binary_le_vertex_colors_float64)
{
    // "property double red/green/blue" exercises the FLOAT64 path in rd_col.
    std::string s = "ply\n"
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
    TmpFile t(
        tmp_path("rast_fcol_rgb.ply"), "ply\nformat ascii 1.0\n"
                                       "element vertex 3\n"
                                       "property float x\nproperty float y\nproperty float z\n"
                                       "element face 1\n"
                                       "property list uchar int vertex_indices\n"
                                       "property uchar r\nproperty uchar g\nproperty uchar b\n"
                                       "end_header\n"
                                       "0 0 0\n1 0 0\n0 1 0\n"
                                       "3 0 1 2 100 150 200\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), size_t{ 3 });
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
    TmpFile t(
        tmp_path("rast_fcol_norm.ply"), "ply\nformat ascii 1.0\n"
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
                                        "3 0 1 2 255 0 0\n"
    );
    Mesh m = load_ok(t.path);
    // Geometry (0,0,0)→(1,0,0)→(0,1,0) CCW from +Z → computed normal is (0,0,+1).
    for (const Vertex &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.z, 1.0f, 1e-4f);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  FACE COLOR + NO UV PROPERTIES
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, face_color_path_no_uv_properties_zero_uvs)
{
    // No UV aliases present → uvs=nullptr; face-color path sets ub=nullptr and
    // skips the UV read block, leaving vertices with default zero UVs.
    TmpFile t(
        tmp_path("rast_fcol_nouv.ply"), "ply\nformat ascii 1.0\n"
                                        "element vertex 3\n"
                                        "property float x\nproperty float y\nproperty float z\n"
                                        "element face 1\n"
                                        "property list uchar int vertex_indices\n"
                                        "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                        "end_header\n"
                                        "0 0 0\n1 0 0\n0 1 0\n"
                                        "3 0 1 2 255 0 0\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
    for (const auto &v : m.vertices)
    {
        ASSERT_NEAR(v.uv.x, 0.0f, 1e-6f);
        ASSERT_NEAR(v.uv.y, 0.0f, 1e-6f);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  ipf < 3 REJECTION
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, ply_face_list_count_below_3)
{
    // Face list with only 2 indices per face → ipf=2 < 3 → load_ply returns false.
    TmpFile t(
        tmp_path("rast_ipf2.ply"), "ply\nformat ascii 1.0\n"
                                   "element vertex 3\n"
                                   "property float x\nproperty float y\nproperty float z\n"
                                   "element face 1\n"
                                   "property list uchar int vertex_indices\n"
                                   "end_header\n"
                                   "0 0 0\n1 0 0\n0 1 0\n"
                                   "2 0 1\n"
    );
    assert_rejects(t.path);
}

// ═══════════════════════════════════════════════════════════════════════════
//  OOB INDEX SKIPPING
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, ply_all_faces_have_oob_indices_standard_path)
{
    // All faces reference an out-of-bounds vertex index → every triangle
    // skipped → triangles empty → load_ply returns false.
    TmpFile t(
        tmp_path("rast_oob_std.ply"), "ply\nformat ascii 1.0\n"
                                      "element vertex 3\n"
                                      "property float x\nproperty float y\nproperty float z\n"
                                      "element face 1\n"
                                      "property list uchar int vertex_indices\n"
                                      "end_header\n"
                                      "0 0 0\n1 0 0\n0 1 0\n"
                                      "3 0 1 99\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_all_faces_have_oob_indices_face_color_path)
{
    // Same but with face colors — exercises the separate OOB guard in the
    // face-color expand path; all vertices skipped → empty → returns false.
    TmpFile t(
        tmp_path("rast_oob_fcol.ply"), "ply\nformat ascii 1.0\n"
                                       "element vertex 3\n"
                                       "property float x\nproperty float y\nproperty float z\n"
                                       "element face 1\n"
                                       "property list uchar int vertex_indices\n"
                                       "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                       "end_header\n"
                                       "0 0 0\n1 0 0\n0 1 0\n"
                                       "3 0 1 99 255 0 0\n"
    );
    assert_rejects(t.path);
}

TEST(ply_valid, partial_oob_indices_valid_faces_survive)
{
    // Two faces: first has valid indices, second has one OOB index. Only the
    // valid face produces a triangle; the bad one is silently skipped.
    TmpFile t(
        tmp_path("rast_partial_oob.ply"), "ply\nformat ascii 1.0\n"
                                          "element vertex 3\n"
                                          "property float x\nproperty float y\nproperty float z\n"
                                          "element face 2\n"
                                          "property list uchar int vertex_indices\n"
                                          "end_header\n"
                                          "0 0 0\n1 0 0\n0 1 0\n"
                                          "3 0 1 2\n"
                                          "3 0 1 99\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

// ═══════════════════════════════════════════════════════════════════════════
//  NON-INT32 FACE INDEX TYPES (rd_idx UINT16 and UINT32)
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, binary_le_uint16_face_indices)
{
    // "property list uchar ushort" exercises the UINT16 case in rd_idx.
    // Most PLY files use "int" (INT32); ushort is common in compact meshes.
    std::string s = "ply\n"
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
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
}

TEST(ply_valid, binary_le_uint32_face_indices)
{
    // "property list uchar uint" exercises the UINT32 default case in rd_idx.
    // Prior tests all use "int" (INT32) — the default branch is different.
    std::string s = "ply\n"
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
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
}

TEST(ply_valid, binary_le_uint8_face_indices)
{
    // "uchar" → UINT8 branch: direct buf[i] read.
    std::string s = "ply\n"
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
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
}

TEST(ply_valid, binary_le_int8_face_indices)
{
    // "char" → INT8 branch: cast<uint32_t>(cast<int8_t>(buf[i])).
    std::string s = "ply\n"
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
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
}

TEST(ply_valid, binary_le_int16_face_indices)
{
    // "short" → INT16 branch: int16_t memcpy then cast to uint32_t.
    std::string s = "ply\n"
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
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
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
    std::string s = "ply\n"
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
    ASSERT_EQ(m.vertex_colors.size(), size_t{ 3 });
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.5f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].z, 0.0f, 1e-5f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  FILE-OPEN FAILURE
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, ply_file_open_failure)
{
    // Non-existent .ply path → !ss.is_open() → load_ply returns false.
    assert_rejects(tmp_path("rast_ply_no_such.ply"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  FACE-COLOR MIXED VALID / INVALID INDICES
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, face_color_mixed_valid_invalid_indices)
{
    // Two faces with face colors: face 1 valid (0,1,2), face 2 has OOB index (0,1,99).
    // The continue in the face-color expansion skips face 2; face 1's triangle survives.
    TmpFile t(
        tmp_path("rast_fcol_mixed.ply"), "ply\nformat ascii 1.0\n"
                                         "element vertex 3\n"
                                         "property float x\nproperty float y\nproperty float z\n"
                                         "element face 2\n"
                                         "property list uchar int vertex_indices\n"
                                         "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                         "end_header\n"
                                         "0 0 0\n1 0 0\n0 1 0\n"
                                         "3 0 1 2 255 0 0\n"
                                         "3 0 1 99 0 255 0\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
    ASSERT_EQ(m.vertices.size(), size_t{ 3 });
    ASSERT_TRUE(m.has_vertex_colors);
}

// ═══════════════════════════════════════════════════════════════════════════
//  FACE COLORS + UV COORDINATES
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, ascii_face_color_with_uv_coords)
{
    // PLY with per-face colors AND per-vertex s/t UV coordinates.
    // The face-color expansion path builds a vertex pool that reads UVs into
    // pool[i].uv when ub != nullptr (line 345 in mesh_ply.cpp).
    TmpFile t(
        tmp_path("rast_fcol_uv.ply"), "ply\nformat ascii 1.0\n"
                                      "element vertex 3\n"
                                      "property float x\nproperty float y\nproperty float z\n"
                                      "property float s\nproperty float t\n"
                                      "element face 1\n"
                                      "property list uchar int vertex_indices\n"
                                      "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                      "end_header\n"
                                      "0 0 0 0.1 0.2\n"
                                      "1 0 0 0.3 0.4\n"
                                      "0 1 0 0.5 0.6\n"
                                      "3 0 1 2 255 0 0\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
    ASSERT_TRUE(m.has_vertex_colors);
}

// ═══════════════════════════════════════════════════════════════════════════
//  CREASE SMOOTHING: load_ply forwards crease_cos to compute_normals
// ═══════════════════════════════════════════════════════════════════════════

TEST(ply_valid, crease_smoothing_splits_hard_edge)
{
    // No normals + a 90 deg fold: load_model's default crease angle (60) is
    // forwarded through load_ply to compute_normals, so the shared edge splits.
    TmpFile t(
        tmp_path("rast_ply_crease.ply"), "ply\nformat ascii 1.0\n"
                                         "element vertex 4\n"
                                         "property float x\nproperty float y\nproperty float z\n"
                                         "element face 2\n"
                                         "property list uchar int vertex_indices\n"
                                         "end_header\n"
                                         "0 0 0\n1 0 0\n0 1 0\n0 0 1\n"
                                         "3 0 1 2\n"
                                         "3 1 0 3\n"
    );
    Mesh m = load_ok(t.path);
    int at_origin = 0;
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            at_origin++;
        }
    }
    ASSERT_EQ(at_origin, 2); // split because 90 deg > 60 deg default crease
}

TEST(ply_valid, crease_shallow_fold_stays_smooth)
{
    // The complement of the hard-edge case: a ~11 deg fold (< 60 deg default crease) keeps
    // the shared origin a single merged vertex with a blended normal. PLY shares vertex
    // elements across faces, so the standard path has real adjacency for crease smoothing.
    TmpFile t(
        tmp_path("rast_ply_crease_soft.ply"), "ply\nformat ascii 1.0\n"
                                              "element vertex 4\n"
                                              "property float x\nproperty float y\nproperty float z\n"
                                              "element face 2\n"
                                              "property list uchar int vertex_indices\n"
                                              "end_header\n"
                                              "0 0 0\n1 0 0\n0 1 0\n0 -1 0.2\n"
                                              "3 0 1 2\n"
                                              "3 1 0 3\n"
    );
    Mesh m = load_ok(t.path);
    int at_origin = 0;
    vec3 n{};
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            at_origin++;
            n = v.normal;
        }
    }
    ASSERT_EQ(at_origin, 1); // shallow fold stays merged
    ASSERT_NEAR(n.length(), 1.0f, 1e-5f);
    ASSERT_TRUE(n.z > 0.9f && n.y > 0.0f); // blend of +Z and the tilted face
}

TEST(ply_valid, crease_threshold_controls_split)
{
    // Same 90 deg fold; the forwarded crease angle decides the outcome, mirroring the OBJ
    // coverage. PLY had only the default-crease hard-edge case before.
    const char *ply = "ply\nformat ascii 1.0\n"
                      "element vertex 4\n"
                      "property float x\nproperty float y\nproperty float z\n"
                      "element face 2\n"
                      "property list uchar int vertex_indices\n"
                      "end_header\n"
                      "0 0 0\n1 0 0\n0 1 0\n0 0 1\n"
                      "3 0 1 2\n"
                      "3 1 0 3\n";
    TmpFile t(tmp_path("rast_ply_crease_thresh.ply"), ply);

    auto count_origin = [](const Mesh &m) -> int
    {
        int c = 0;
        for (const auto &v : m.vertices)
        {
            if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
            {
                c++;
            }
        }
        return c;
    };

    Mesh smooth;
    ASSERT_TRUE(smooth.load_model(t.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/180.0f));
    ASSERT_EQ(count_origin(smooth), 1); // 90 deg < 180 deg threshold → merged

    Mesh hard;
    ASSERT_TRUE(hard.load_model(t.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/45.0f));
    ASSERT_EQ(count_origin(hard), 2); // 90 deg > 45 deg threshold → split
}

// ═══════════════════════════════════════════════════════════════════════════
//  FACE-LIST TEXCOORD (per-corner UVs, photogrammetry / scanner PLYs)
// ═══════════════════════════════════════════════════════════════════════════

// Count split copies of a target position in the loaded mesh.
[[maybe_unused]] static int count_at(const Mesh &m, float x, float y, float z)
{
    int n = 0;
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == x && v.pos.y == y && v.pos.z == z)
        {
            n++;
        }
    }
    return n;
}

TEST(ply_valid, ascii_face_texcoord_basic_triangle)
{
    // Single triangle, face-list texcoord. UVs should land on the right vertices
    // in file-corner order.
    TmpFile t(
        tmp_path("rast_ftc_tri.ply"), "ply\nformat ascii 1.0\n"
                                      "element vertex 3\n"
                                      "property float x\nproperty float y\nproperty float z\n"
                                      "element face 1\n"
                                      "property list uchar int vertex_indices\n"
                                      "property list uchar float texcoord\n"
                                      "end_header\n"
                                      "0 0 0\n1 0 0\n0 1 0\n"
                                      "3 0 1 2  6 0.1 0.2 0.3 0.4 0.5 0.6\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{ 3 });
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
    // Walk the triangle's corners to locate each vertex by its UV (split-by-UV
    // path may reorder by hash insertion order).
    const Triangle &tri = m.triangles[0];
    ASSERT_NEAR(m.vertices[tri.v[0]].uv.x, 0.1f, 1e-5f);
    ASSERT_NEAR(m.vertices[tri.v[0]].uv.y, 0.2f, 1e-5f);
    ASSERT_NEAR(m.vertices[tri.v[1]].uv.x, 0.3f, 1e-5f);
    ASSERT_NEAR(m.vertices[tri.v[1]].uv.y, 0.4f, 1e-5f);
    ASSERT_NEAR(m.vertices[tri.v[2]].uv.x, 0.5f, 1e-5f);
    ASSERT_NEAR(m.vertices[tri.v[2]].uv.y, 0.6f, 1e-5f);
}

TEST(ply_valid, ascii_face_texcoord_quad_fan_triangulates)
{
    // Quad with 8 face-list UVs fan-triangulates into 2 triangles. Each corner's
    // UV must survive (no dedup needed — corners have distinct UVs).
    TmpFile t(
        tmp_path("rast_ftc_quad.ply"), "ply\nformat ascii 1.0\n"
                                       "element vertex 4\n"
                                       "property float x\nproperty float y\nproperty float z\n"
                                       "element face 1\n"
                                       "property list uchar int vertex_indices\n"
                                       "property list uchar float texcoord\n"
                                       "end_header\n"
                                       "0 0 0\n1 0 0\n1 1 0\n0 1 0\n"
                                       "4 0 1 2 3  8 0 0 1 0 1 1 0 1\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 2 });
    ASSERT_EQ(m.vertices.size(), size_t{ 4 });
}

TEST(ply_valid, ascii_face_texcoord_no_seam_dedupes)
{
    // Two adjacent triangles sharing positions v0 and v2 with IDENTICAL UVs at
    // the shared corners → hash-dedup collapses them; total vertex count == 4.
    TmpFile t(
        tmp_path("rast_ftc_noseam.ply"), "ply\nformat ascii 1.0\n"
                                         "element vertex 4\n"
                                         "property float x\nproperty float y\nproperty float z\n"
                                         "element face 2\n"
                                         "property list uchar int vertex_indices\n"
                                         "property list uchar float texcoord\n"
                                         "end_header\n"
                                         "0 0 0\n1 0 0\n1 1 0\n0 1 0\n"
                                         "3 0 1 2  6 0 0 1 0 1 1\n"
                                         "3 0 2 3  6 0 0 1 1 0 1\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 2 });
    ASSERT_EQ(m.vertices.size(), size_t{ 4 });
    ASSERT_EQ(count_at(m, 0, 0, 0), 1);
    ASSERT_EQ(count_at(m, 1, 1, 0), 1);
}

TEST(ply_valid, ascii_face_texcoord_seam_splits)
{
    // Same topology as above, but the shared positions v0 and v2 carry DIFFERENT
    // UVs across the seam → each splits into 2 vertices; total = 6.
    TmpFile t(
        tmp_path("rast_ftc_seam.ply"), "ply\nformat ascii 1.0\n"
                                       "element vertex 4\n"
                                       "property float x\nproperty float y\nproperty float z\n"
                                       "element face 2\n"
                                       "property list uchar int vertex_indices\n"
                                       "property list uchar float texcoord\n"
                                       "end_header\n"
                                       "0 0 0\n1 0 0\n1 1 0\n0 1 0\n"
                                       "3 0 1 2  6 0.0 0.0 1.0 0.0 1.0 1.0\n"
                                       "3 0 2 3  6 0.5 0.5 0.5 0.5 0.0 1.0\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 2 });
    ASSERT_EQ(m.vertices.size(), size_t{ 6 });
    ASSERT_EQ(count_at(m, 0, 0, 0), 2); // v0 split
    ASSERT_EQ(count_at(m, 1, 1, 0), 2); // v2 split
    ASSERT_EQ(count_at(m, 1, 0, 0), 1); // v1 not shared
    ASSERT_EQ(count_at(m, 0, 1, 0), 1); // v3 not shared
}

TEST(ply_valid, ascii_face_texcoord_seam_below_crease_smooths)
{
    // Coplanar quad, UV seam at the shared edge. With no input normals,
    // compute_normals runs and the weld map groups split halves of v0 and v2
    // so the seam smooths as one surface → every vertex normal = (0, 0, 1).
    TmpFile t(
        tmp_path("rast_ftc_seam_smooth.ply"), "ply\nformat ascii 1.0\n"
                                              "element vertex 4\n"
                                              "property float x\nproperty float y\nproperty float z\n"
                                              "element face 2\n"
                                              "property list uchar int vertex_indices\n"
                                              "property list uchar float texcoord\n"
                                              "end_header\n"
                                              "0 0 0\n1 0 0\n1 1 0\n0 1 0\n"
                                              "3 0 1 2  6 0.0 0.0 1.0 0.0 1.0 1.0\n"
                                              "3 0 2 3  6 0.5 0.5 0.5 0.5 0.0 1.0\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{ 6 });
    for (const auto &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.x, 0.0f, 1e-5f);
        ASSERT_NEAR(v.normal.y, 0.0f, 1e-5f);
        ASSERT_NEAR(v.normal.z, 1.0f, 1e-5f);
    }
}

TEST(ply_valid, ascii_face_texcoord_seam_above_crease_stays_split)
{
    // Two triangles sharing edge v0-v1, 90 deg fold. UV seam at the shared
    // corners. Crease angle (60 deg default) splits the geometric edge even in
    // welded space → normals at v0 differ between the two faces.
    TmpFile t(
        tmp_path("rast_ftc_seam_crease.ply"), "ply\nformat ascii 1.0\n"
                                              "element vertex 4\n"
                                              "property float x\nproperty float y\nproperty float z\n"
                                              "element face 2\n"
                                              "property list uchar int vertex_indices\n"
                                              "property list uchar float texcoord\n"
                                              "end_header\n"
                                              "0 0 0\n1 0 0\n0 1 0\n0 0 1\n"
                                              "3 0 1 2  6 0 0 1 0 0 1\n"
                                              "3 1 0 3  6 0.5 0.5 0.5 0.5 0 1\n"
    );
    Mesh m = load_ok(t.path);
    // v0=(0,0,0): UVs differ across the seam → split → 2 vertices.
    ASSERT_EQ(count_at(m, 0, 0, 0), 2);
    // The two halves should have different normals (one ~(0,0,1), one ~(0,1,0)).
    vec3 n0{}, n1{};
    int found = 0;
    for (const auto &v : m.vertices)
    {
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            if (found == 0)
            {
                n0 = v.normal;
            }
            else
            {
                n1 = v.normal;
            }
            found++;
        }
    }
    ASSERT_EQ(found, 2);
    // Normals must not be coincident — the 90 deg fold breaks weld smoothing.
    const float dot = (n0.x * n1.x) + (n0.y * n1.y) + (n0.z * n1.z);
    ASSERT_TRUE(dot < 0.5f);
}

TEST(ply_valid, ascii_face_texcoord_with_authored_normals_skip_recompute)
{
    // File supplies normals + face-list texcoord with a seam → no normal
    // recompute; both split halves at the shared position copy the authored
    // normal from that source vertex (no smoothing path engaged).
    TmpFile t(
        tmp_path("rast_ftc_with_norms.ply"), "ply\nformat ascii 1.0\n"
                                             "element vertex 4\n"
                                             "property float x\nproperty float y\nproperty float z\n"
                                             "property float nx\nproperty float ny\nproperty float nz\n"
                                             "element face 2\n"
                                             "property list uchar int vertex_indices\n"
                                             "property list uchar float texcoord\n"
                                             "end_header\n"
                                             "0 0 0  0 0 1\n1 0 0  0 0 1\n1 1 0  0 0 1\n0 1 0  0 0 1\n"
                                             "3 0 1 2  6 0.0 0.0 1.0 0.0 1.0 1.0\n"
                                             "3 0 2 3  6 0.5 0.5 0.5 0.5 0.0 1.0\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{ 6 }); // seam splits still happen
    for (const auto &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.z, 1.0f, 1e-5f);
    }
}

TEST(ply_valid, ascii_texture_uv_alias_accepted)
{
    // Older exporters use "texture_uv" instead of "texcoord". Same payload.
    TmpFile t(
        tmp_path("rast_ftc_alias.ply"), "ply\nformat ascii 1.0\n"
                                        "element vertex 3\n"
                                        "property float x\nproperty float y\nproperty float z\n"
                                        "element face 1\n"
                                        "property list uchar int vertex_indices\n"
                                        "property list uchar float texture_uv\n"
                                        "end_header\n"
                                        "0 0 0\n1 0 0\n0 1 0\n"
                                        "3 0 1 2  6 0.25 0.75 0.5 0.5 0.75 0.25\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{ 3 });
    const Triangle &tri = m.triangles[0];
    ASSERT_NEAR(m.vertices[tri.v[0]].uv.x, 0.25f, 1e-5f);
    ASSERT_NEAR(m.vertices[tri.v[2]].uv.x, 0.75f, 1e-5f);
}

TEST(ply_valid, ascii_face_texcoord_overrides_per_vertex_uv)
{
    // File authors BOTH per-vertex s/t AND face-list texcoord with deliberately
    // distinct values. Face-list must win (MeshLab convention).
    TmpFile t(
        tmp_path("rast_ftc_override.ply"), "ply\nformat ascii 1.0\n"
                                           "element vertex 3\n"
                                           "property float x\nproperty float y\nproperty float z\n"
                                           "property float s\nproperty float t\n"
                                           "element face 1\n"
                                           "property list uchar int vertex_indices\n"
                                           "property list uchar float texcoord\n"
                                           "end_header\n"
                                           "0 0 0  0.9 0.9\n"
                                           "1 0 0  0.9 0.9\n"
                                           "0 1 0  0.9 0.9\n"
                                           "3 0 1 2  6 0.1 0.2 0.3 0.4 0.5 0.6\n"
    );
    Mesh m = load_ok(t.path);
    for (const auto &v : m.vertices)
    {
        ASSERT_TRUE(v.uv.x < 0.7f); // face-list (0.1..0.5), not per-vertex (0.9)
        ASSERT_TRUE(v.uv.y < 0.7f);
    }
}

TEST(ply_valid, ascii_face_texcoord_seam_split_carries_vertex_colors)
{
    // Per-vertex red/green/blue + UV seam. Both split halves of v0 must inherit
    // v0's source color, not a default or zero color.
    TmpFile t(
        tmp_path("rast_ftc_seam_colors.ply"), "ply\nformat ascii 1.0\n"
                                              "element vertex 4\n"
                                              "property float x\nproperty float y\nproperty float z\n"
                                              "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                              "element face 2\n"
                                              "property list uchar int vertex_indices\n"
                                              "property list uchar float texcoord\n"
                                              "end_header\n"
                                              "0 0 0  255 0 0\n1 0 0  0 255 0\n1 1 0  0 0 255\n0 1 0  255 255 0\n"
                                              "3 0 1 2  6 0 0 1 0 1 1\n"
                                              "3 0 2 3  6 0.5 0.5 0.5 0.5 0 1\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), m.vertices.size());
    // Both halves of v0 should be red. Both halves of v2 should be blue.
    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        const auto &v = m.vertices[i];
        const auto &c = m.vertex_colors[i];
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            ASSERT_NEAR(c.x, 1.0f, 1e-4f);
            ASSERT_NEAR(c.y, 0.0f, 1e-4f);
            ASSERT_NEAR(c.z, 0.0f, 1e-4f);
        }
        else if (v.pos.x == 1.0f && v.pos.y == 1.0f && v.pos.z == 0.0f)
        {
            ASSERT_NEAR(c.x, 0.0f, 1e-4f);
            ASSERT_NEAR(c.y, 0.0f, 1e-4f);
            ASSERT_NEAR(c.z, 1.0f, 1e-4f);
        }
    }
}

TEST(ply_valid, ascii_face_texcoord_with_face_colors)
{
    // Combo: face-list texcoord + face colors. Face-color path engages (full
    // unshare, 3 verts per triangle), but UVs come from the face-list buffer
    // (not the per-vertex pool). Each triangle has 3 unique vertices, each with
    // its own face-list UV.
    TmpFile t(
        tmp_path("rast_ftc_fcol.ply"), "ply\nformat ascii 1.0\n"
                                       "element vertex 3\n"
                                       "property float x\nproperty float y\nproperty float z\n"
                                       "element face 1\n"
                                       "property list uchar int vertex_indices\n"
                                       "property list uchar float texcoord\n"
                                       "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                       "end_header\n"
                                       "0 0 0\n1 0 0\n0 1 0\n"
                                       "3 0 1 2  6 0.1 0.2 0.3 0.4 0.5 0.6  255 0 0\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
    ASSERT_EQ(m.vertices.size(), size_t{ 3 }); // full unshare on 1 triangle
    ASSERT_TRUE(m.has_vertex_colors);
    const Triangle &tri = m.triangles[0];
    ASSERT_NEAR(m.vertices[tri.v[0]].uv.x, 0.1f, 1e-5f);
    ASSERT_NEAR(m.vertices[tri.v[0]].uv.y, 0.2f, 1e-5f);
    ASSERT_NEAR(m.vertices[tri.v[1]].uv.x, 0.3f, 1e-5f);
    ASSERT_NEAR(m.vertices[tri.v[2]].uv.x, 0.5f, 1e-5f);
    for (const auto &c : m.vertex_colors)
    {
        ASSERT_NEAR(c.x, 1.0f, 1e-4f);
        ASSERT_NEAR(c.y, 0.0f, 1e-4f);
        ASSERT_NEAR(c.z, 0.0f, 1e-4f);
    }
}

TEST(ply_valid, binary_le_face_texcoord)
{
    std::string s = "ply\n"
                    "format binary_little_endian 1.0\n"
                    "element vertex 3\n"
                    "property float x\nproperty float y\nproperty float z\n"
                    "element face 1\n"
                    "property list uchar int vertex_indices\n"
                    "property list uchar float texcoord\n"
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
    s.push_back(3);
    emit_u32_le(s, 0);
    emit_u32_le(s, 1);
    emit_u32_le(s, 2);
    s.push_back(6); // texcoord list count
    emit_f32_le(s, 0.1f);
    emit_f32_le(s, 0.2f);
    emit_f32_le(s, 0.3f);
    emit_f32_le(s, 0.4f);
    emit_f32_le(s, 0.5f);
    emit_f32_le(s, 0.6f);
    TmpFile t(tmp_path("rast_ftc_le.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{ 3 });
    const Triangle &tri = m.triangles[0];
    ASSERT_NEAR(m.vertices[tri.v[0]].uv.x, 0.1f, 1e-5f);
    ASSERT_NEAR(m.vertices[tri.v[2]].uv.y, 0.6f, 1e-5f);
}

TEST(ply_valid, binary_be_face_texcoord)
{
    std::string s = "ply\n"
                    "format binary_big_endian 1.0\n"
                    "element vertex 3\n"
                    "property float x\nproperty float y\nproperty float z\n"
                    "element face 1\n"
                    "property list uchar int vertex_indices\n"
                    "property list uchar float texcoord\n"
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
    s.push_back(6);
    emit_f32_be(s, 0.1f);
    emit_f32_be(s, 0.2f);
    emit_f32_be(s, 0.3f);
    emit_f32_be(s, 0.4f);
    emit_f32_be(s, 0.5f);
    emit_f32_be(s, 0.6f);
    TmpFile t(tmp_path("rast_ftc_be.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{ 3 });
    const Triangle &tri = m.triangles[0];
    ASSERT_NEAR(m.vertices[tri.v[0]].uv.x, 0.1f, 1e-5f);
    ASSERT_NEAR(m.vertices[tri.v[2]].uv.y, 0.6f, 1e-5f);
}

TEST(reject, ply_face_texcoord_wrong_corner_count)
{
    // Triangle face declares 5 UV floats — must be 2 * 3 = 6. Reject.
    TmpFile t(
        tmp_path("rast_ftc_bad_count.ply"), "ply\nformat ascii 1.0\n"
                                            "element vertex 3\n"
                                            "property float x\nproperty float y\nproperty float z\n"
                                            "element face 1\n"
                                            "property list uchar int vertex_indices\n"
                                            "property list uchar float texcoord\n"
                                            "end_header\n"
                                            "0 0 0\n1 0 0\n0 1 0\n"
                                            "3 0 1 2  5 0 0 1 0 1\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_face_texcoord_mixed_face_sizes)
{
    // One triangle + one quad → list_sizes populated → reject (the uniform-ipf
    // assumption that per-corner indexing relies on no longer holds).
    TmpFile t(
        tmp_path("rast_ftc_mixed.ply"), "ply\nformat ascii 1.0\n"
                                        "element vertex 4\n"
                                        "property float x\nproperty float y\nproperty float z\n"
                                        "element face 2\n"
                                        "property list uchar int vertex_indices\n"
                                        "property list uchar float texcoord\n"
                                        "end_header\n"
                                        "0 0 0\n1 0 0\n1 1 0\n0 1 0\n"
                                        "3 0 1 2  6 0 0 1 0 1 1\n"
                                        "4 0 1 2 3  8 0 0 1 0 1 1 0 1\n"
    );
    assert_rejects(t.path);
}

TEST(ply_valid, face_texcoord_with_face_colors_smooths_seam)
{
    // Combo: face-list texcoord + face colors. The face-color path stays fully
    // unshared, but a weld map (one source position id per emitted vertex) is
    // threaded into compute_normals so a UV-seam shared position smooths as
    // one surface — without the weld, every corner would be its own group and
    // the surface would render faceted. Two coplanar triangles sharing v0 and
    // v2 with a UV seam at both shared corners → every vertex normal = (0,0,1).
    TmpFile t(
        tmp_path("rast_ftc_fcol_smooth.ply"), "ply\nformat ascii 1.0\n"
                                              "element vertex 4\n"
                                              "property float x\nproperty float y\nproperty float z\n"
                                              "element face 2\n"
                                              "property list uchar int vertex_indices\n"
                                              "property list uchar float texcoord\n"
                                              "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                              "end_header\n"
                                              "0 0 0\n1 0 0\n1 1 0\n0 1 0\n"
                                              "3 0 1 2  6 0.0 0.0 1.0 0.0 1.0 1.0  255 0 0\n"
                                              "3 0 2 3  6 0.5 0.5 0.5 0.5 0.0 1.0  0 255 0\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_TRUE(m.has_vertex_colors);
    for (const auto &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.x, 0.0f, 1e-4f);
        ASSERT_NEAR(v.normal.y, 0.0f, 1e-4f);
        ASSERT_NEAR(v.normal.z, 1.0f, 1e-4f);
    }
}

TEST(reject, ply_face_texcoord_all_oob_indices)
{
    // Face-list texcoord present but every index is OOB → all triangles skipped
    // → empty triangle list → load_ply returns false (existing OOB guard still
    // fires in the new path).
    TmpFile t(
        tmp_path("rast_ftc_oob.ply"), "ply\nformat ascii 1.0\n"
                                      "element vertex 3\n"
                                      "property float x\nproperty float y\nproperty float z\n"
                                      "element face 1\n"
                                      "property list uchar int vertex_indices\n"
                                      "property list uchar float texcoord\n"
                                      "end_header\n"
                                      "0 0 0\n1 0 0\n0 1 0\n"
                                      "3 0 1 99  6 0 0 1 0 0 1\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_mixed_size_index_lists)
{
    // Mixed n-gon index lists (a triangle + a quad) leave ipf undefined:
    // total_idx/n_faces = 7/2 = 3 rounds, and the per-face stride is then wrong.
    // The texcoord lists are coincidentally uniform (6 floats each) so the
    // texcoord gate alone would pass — only the faces->list_sizes guard rejects.
    TmpFile t(
        tmp_path("rast_mixed_idx.ply"), "ply\nformat ascii 1.0\n"
                                        "element vertex 4\n"
                                        "property float x\nproperty float y\nproperty float z\n"
                                        "element face 2\n"
                                        "property list uchar int vertex_indices\n"
                                        "property list uchar float texcoord\n"
                                        "end_header\n"
                                        "0 0 0\n1 0 0\n1 1 0\n0 1 0\n"
                                        "3 0 1 2  6 0 0 1 0 1 1\n"
                                        "4 0 1 2 3  6 0 0 1 0 1 1\n"
    );
    assert_rejects(t.path);
}

TEST(reject, ply_mixed_size_index_lists_no_texcoord)
{
    // Same mixed n-gon index lists, no texcoord at all. Pins that the
    // faces->list_sizes guard rejects independently of the texcoord path
    // (previously these loaded with silently mis-derived ipf).
    TmpFile t(
        tmp_path("rast_mixed_idx_notc.ply"), "ply\nformat ascii 1.0\n"
                                             "element vertex 4\n"
                                             "property float x\nproperty float y\nproperty float z\n"
                                             "element face 2\n"
                                             "property list uchar int vertex_indices\n"
                                             "end_header\n"
                                             "0 0 0\n1 0 0\n1 1 0\n0 1 0\n"
                                             "3 0 1 2\n"
                                             "4 0 1 2 3\n"
    );
    assert_rejects(t.path);
}

TEST(ply_valid, binary_le_face_texcoord_float64)
{
    // FLOAT64 (double) texcoord list exercises the stride-8 path in the size
    // gate and rd_f. All other texcoord tests use FLOAT32.
    std::string s = "ply\n"
                    "format binary_little_endian 1.0\n"
                    "element vertex 3\n"
                    "property float x\nproperty float y\nproperty float z\n"
                    "element face 1\n"
                    "property list uchar int vertex_indices\n"
                    "property list uchar double texcoord\n"
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
    s.push_back(3);
    emit_u32_le(s, 0);
    emit_u32_le(s, 1);
    emit_u32_le(s, 2);
    s.push_back(6); // texcoord list count
    emit_f64_le(s, 0.1);
    emit_f64_le(s, 0.2);
    emit_f64_le(s, 0.3);
    emit_f64_le(s, 0.4);
    emit_f64_le(s, 0.5);
    emit_f64_le(s, 0.6);
    TmpFile t(tmp_path("rast_ftc_f64.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{ 3 });
    const Triangle &tri = m.triangles[0];
    ASSERT_NEAR(m.vertices[tri.v[0]].uv.x, 0.1f, 1e-5f);
    ASSERT_NEAR(m.vertices[tri.v[2]].uv.y, 0.6f, 1e-5f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  comment TextureFile  (MeshLab / photogrammetry albedo binding)
// ═══════════════════════════════════════════════════════════════════════════
//
// The texture name is resolved relative to the PLY's directory; tmp_path() puts
// the .ply and the .bmp in the same temp dir, so the comment carries the bare
// BMP filename. k1x1_red_bmp (inline_bmp.h) decodes via stb to RGBA {255,0,0,255}.

TEST(ply_valid, texturefile_with_uvs_loads)
{
    // Per-vertex UVs + a TextureFile comment → diffuse texture bound to material 0.
    TmpFile bmp(tmp_path("rast_tf_basic.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile t(
        tmp_path("rast_tf_basic.ply"), "ply\nformat ascii 1.0\n"
                                       "comment TextureFile rast_tf_basic.bmp\n"
                                       "element vertex 3\n"
                                       "property float x\nproperty float y\nproperty float z\n"
                                       "property float u\nproperty float v\n"
                                       "element face 1\n"
                                       "property list uchar int vertex_indices\n"
                                       "end_header\n"
                                       "0 0 0 0.0 0.0\n"
                                       "1 0 0 1.0 0.0\n"
                                       "0 1 0 0.0 1.0\n"
                                       "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.materials[0].diffuse_map.tex, 0);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].valid());
    const vec3 c = m.textures[0].sample_rgb(0.0f, 0.0f);
    ASSERT_NEAR(c.x, 1.0f, 1e-3f);
    ASSERT_NEAR(c.y, 0.0f, 1e-3f);
    ASSERT_NEAR(c.z, 0.0f, 1e-3f);
}

TEST(ply_valid, texturefile_face_list_texcoord)
{
    // UVs via face-list texcoord (split-by-UV path) still satisfies the has_uv gate.
    TmpFile bmp(tmp_path("rast_tf_ftc.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile t(
        tmp_path("rast_tf_ftc.ply"), "ply\nformat ascii 1.0\n"
                                     "comment TextureFile rast_tf_ftc.bmp\n"
                                     "element vertex 3\n"
                                     "property float x\nproperty float y\nproperty float z\n"
                                     "element face 1\n"
                                     "property list uchar int vertex_indices\n"
                                     "property list uchar float texcoord\n"
                                     "end_header\n"
                                     "0 0 0\n1 0 0\n0 1 0\n"
                                     "3 0 1 2  6 0.1 0.2 0.3 0.4 0.5 0.6\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.materials[0].diffuse_map.tex, 0);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].valid());
}

TEST(ply_valid, texturefile_missing_file_silent_drop)
{
    // Comment names a file that does not exist: load still succeeds, no texture
    // bound (matches OBJ/glTF silent-drop on a failed decode).
    TmpFile t(
        tmp_path("rast_tf_missing.ply"), "ply\nformat ascii 1.0\n"
                                         "comment TextureFile rast_tf_does_not_exist.bmp\n"
                                         "element vertex 3\n"
                                         "property float x\nproperty float y\nproperty float z\n"
                                         "property float u\nproperty float v\n"
                                         "element face 1\n"
                                         "property list uchar int vertex_indices\n"
                                         "end_header\n"
                                         "0 0 0 0.0 0.0\n"
                                         "1 0 0 1.0 0.0\n"
                                         "0 1 0 0.0 1.0\n"
                                         "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.materials[0].diffuse_map.tex, -1);
    ASSERT_TRUE(m.textures.empty());
}

TEST(ply_valid, texturefile_without_uvs_ignored)
{
    // TextureFile present but the mesh has no UVs: the comment is ignored entirely
    // (a texture with nothing to sample it would just be wasted RAM).
    TmpFile bmp(tmp_path("rast_tf_nouv.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile t(
        tmp_path("rast_tf_nouv.ply"), "ply\nformat ascii 1.0\n"
                                      "comment TextureFile rast_tf_nouv.bmp\n"
                                      "element vertex 3\n"
                                      "property float x\nproperty float y\nproperty float z\n"
                                      "element face 1\n"
                                      "property list uchar int vertex_indices\n"
                                      "end_header\n"
                                      "0 0 0\n1 0 0\n0 1 0\n"
                                      "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.materials[0].diffuse_map.tex, -1);
    ASSERT_TRUE(m.textures.empty());
}

TEST(ply_valid, texturefile_first_wins)
{
    // Two TextureFile comments: the first (valid) is taken, the second ignored
    // (PLY has no per-face texture binding, so extra images are unusable).
    TmpFile bmp(tmp_path("rast_tf_first.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile t(
        tmp_path("rast_tf_first.ply"), "ply\nformat ascii 1.0\n"
                                       "comment TextureFile rast_tf_first.bmp\n"
                                       "comment TextureFile rast_tf_bogus.bmp\n"
                                       "element vertex 3\n"
                                       "property float x\nproperty float y\nproperty float z\n"
                                       "property float u\nproperty float v\n"
                                       "element face 1\n"
                                       "property list uchar int vertex_indices\n"
                                       "end_header\n"
                                       "0 0 0 0.0 0.0\n"
                                       "1 0 0 1.0 0.0\n"
                                       "0 1 0 0.0 1.0\n"
                                       "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.materials[0].diffuse_map.tex, 0);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].valid());
}

TEST(ply_valid, texturefile_name_with_spaces)
{
    // The whole remainder after the token is the filename — names with spaces
    // must survive (no whitespace tokenizing).
    TmpFile bmp(tmp_path("rast tf spaced.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile t(
        tmp_path("rast_tf_spaced.ply"), "ply\nformat ascii 1.0\n"
                                        "comment TextureFile rast tf spaced.bmp\n"
                                        "element vertex 3\n"
                                        "property float x\nproperty float y\nproperty float z\n"
                                        "property float u\nproperty float v\n"
                                        "element face 1\n"
                                        "property list uchar int vertex_indices\n"
                                        "end_header\n"
                                        "0 0 0 0.0 0.0\n"
                                        "1 0 0 1.0 0.0\n"
                                        "0 1 0 0.0 1.0\n"
                                        "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.materials[0].diffuse_map.tex, 0);
    ASSERT_TRUE(m.textures[0].valid());
}

TEST(ply_valid, texturefile_no_comment_no_texture)
{
    // Baseline: a textured PLY with no TextureFile comment never binds a texture.
    TmpFile t(
        tmp_path("rast_tf_none.ply"), "ply\nformat ascii 1.0\n"
                                      "element vertex 3\n"
                                      "property float x\nproperty float y\nproperty float z\n"
                                      "property float u\nproperty float v\n"
                                      "element face 1\n"
                                      "property list uchar int vertex_indices\n"
                                      "end_header\n"
                                      "0 0 0 0.0 0.0\n"
                                      "1 0 0 1.0 0.0\n"
                                      "0 1 0 0.0 1.0\n"
                                      "3 0 1 2\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.materials[0].diffuse_map.tex, -1);
    ASSERT_TRUE(m.textures.empty());
}

TEST(ply_valid, texturefile_binary_le)
{
    // Comment parsing is independent of the data encoding — the header is ASCII
    // either way. Binary-LE body with per-vertex UVs + a TextureFile comment.
    std::string s = "ply\nformat binary_little_endian 1.0\n"
                    "comment TextureFile rast_tf_bin.bmp\n"
                    "element vertex 3\n"
                    "property float x\nproperty float y\nproperty float z\n"
                    "property float u\nproperty float v\n"
                    "element face 1\n"
                    "property list uchar int vertex_indices\n"
                    "end_header\n";
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    s.push_back(3); // face index list count
    emit_u32_le(s, 0);
    emit_u32_le(s, 1);
    emit_u32_le(s, 2);
    TmpFile bmp(tmp_path("rast_tf_bin.bmp"), k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile t(tmp_path("rast_tf_bin.ply"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.materials[0].diffuse_map.tex, 0);
    ASSERT_TRUE(m.textures[0].valid());
}
