#include "loader_util.h"

// ═══════════════════════════════════════════════════════════════════════════
//  HAND-CRAFTED VALID STL
// ═══════════════════════════════════════════════════════════════════════════

TEST(stl_valid, ascii_single_facet)
{
    TmpFile t(
        tmp_path("rasterminal_test_min.stl"), "solid test\n"
                                              "facet normal 0 0 1\n"
                                              "  outer loop\n"
                                              "    vertex 0 0 0\n"
                                              "    vertex 1 0 0\n"
                                              "    vertex 0 1 0\n"
                                              "  endloop\n"
                                              "endfacet\n"
                                              "endsolid test\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, binary_single_triangle)
{
    std::string s(80, 'X');     // 80-byte header (non-"solid")
    emit_u32_le(s, 1);          // tri_count = 1
    for (int i = 0; i < 3; i++) // normal (ignored)
    {
        emit_f32_le(s, 0);
    }
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
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
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
    {
        emit_f32_le(s, 0);
    }
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
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
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
    {
        emit_f32_le(s, 0); // normal
    }
    for (int i = 0; i < 9; i++)
    {
        emit_f32_le(s, 0); // 3 verts
    }
    s.push_back(0);
    s.push_back(0); // attr
    // Second triangle (truncated — only first 20 bytes).
    for (int i = 0; i < 5; i++)
    {
        emit_f32_le(s, 0);
    }
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

TEST(reject, stl_non_finite_vertex)
{
    // A NaN position (0x7FC00000) in an otherwise valid 1-triangle binary STL. The binary
    // parse succeeds, but load_model's post-load finiteness scan rejects it before the NaN
    // can poison normals/bbox/camera-fit.
    std::string s(80, 'X');
    emit_u32_le(s, 1); // tri_count = 1
    for (int i = 0; i < 3; i++)
    {
        emit_f32_le(s, 0.0f); // normal (ignored)
    }
    emit_u32_le(s, 0x7FC00000u); // v0.x = NaN
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    s.push_back(0);
    s.push_back(0); // attribute bytes
    TmpFile t(tmp_path("rasterminal_test_stl_nan.stl"), s);
    assert_rejects(t.path);
}

TEST(reject, stl_ascii_no_facets)
{
    TmpFile t(
        tmp_path("rasterminal_test_nofacet.stl"), "solid empty\n"
                                                  "endsolid empty\n"
    );
    assert_rejects(t.path);
}

TEST(reject, stl_ascii_missing_third_vertex)
{
    // outer loop with only two vertex lines — stl_reader fails to parse the facet.
    TmpFile t(
        tmp_path("rasterminal_test_2v.stl"), "solid test\n"
                                             "facet normal 0 0 1\n"
                                             "  outer loop\n"
                                             "    vertex 0 0 0\n"
                                             "    vertex 1 0 0\n"
                                             "  endloop\n"
                                             "endfacet\n"
                                             "endsolid test\n"
    );
    assert_rejects(t.path);
}

// ═══════════════════════════════════════════════════════════════════════════
//  CORRECTNESS — verify loaded geometry values
// ═══════════════════════════════════════════════════════════════════════════

TEST(stl_valid, ascii_vertex_positions_and_defaults)
{
    // Verify actual coordinate values, per-vertex ao, material index, and mesh
    // defaults — the count-only tests above don't exercise any of this.
    TmpFile t(
        tmp_path("rasterminal_test_pos.stl"), "solid test\n"
                                              "facet normal 0 0 1\n"
                                              "  outer loop\n"
                                              "    vertex 1 2 3\n"
                                              "    vertex 4 5 6\n"
                                              "    vertex 7 8 9\n"
                                              "  endloop\n"
                                              "endfacet\n"
                                              "endsolid test\n"
    );
    Mesh m = load_ok(t.path);

    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
    ASSERT_EQ(m.vertices.size(), size_t{ 3 });
    ASSERT_EQ(m.materials.size(), size_t{ 1 });
    ASSERT_EQ(m.triangles[0].material_idx, 0u);
    ASSERT_FALSE(m.has_vertex_colors);

    // Positions; stl_reader preserves declaration order for a single triangle.
    ASSERT_NEAR(m.vertices[0].pos.x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertices[0].pos.y, 2.0f, 1e-5f);
    ASSERT_NEAR(m.vertices[0].pos.z, 3.0f, 1e-5f);
    ASSERT_NEAR(m.vertices[1].pos.x, 4.0f, 1e-5f);
    ASSERT_NEAR(m.vertices[2].pos.x, 7.0f, 1e-5f);

    // ao is hardcoded to 1.0 in the expansion loop (STL has no AO data).
    for (const Vertex &v : m.vertices)
    {
        ASSERT_NEAR(v.ao, 1.0f, 1e-6f);
    }
}

TEST(stl_valid, ascii_file_normal_ignored_compute_normals_runs)
{
    // STL face normals are read by stl_reader but discarded by our loader;
    // compute_normals() always runs.  Use a deliberate wrong file normal so
    // the test fails if file normals are ever accidentally applied.
    //
    // Geometry: (0,0,0)→(1,0,0)→(0,1,0) CCW from +Z → computed normal = (0,0,+1).
    // File says normal 0 0 -1.  Loaded vertex normals must have z > 0.
    TmpFile t(
        tmp_path("rasterminal_test_wrongnorm.stl"), "solid test\n"
                                                    "facet normal 0 0 -1\n"
                                                    "  outer loop\n"
                                                    "    vertex 0 0 0\n"
                                                    "    vertex 1 0 0\n"
                                                    "    vertex 0 1 0\n"
                                                    "  endloop\n"
                                                    "endfacet\n"
                                                    "endsolid test\n"
    );
    Mesh m = load_ok(t.path);
    for (const Vertex &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.z, 1.0f, 1e-4f);
    }
}

TEST(stl_valid, binary_two_triangles_unshared_vertex_expansion)
{
    // The loader re-expands stl_reader's deduplicated output to 3 unshared
    // vertices per triangle. Verify counts and triangle index offsets for n=2.
    std::string s(80, 'X');
    emit_u32_le(s, 2); // two triangles
    for (int tri = 0; tri < 2; tri++)
    {
        for (int i = 0; i < 3; i++)
        {
            emit_f32_le(s, 0.0f); // normal (ignored)
        }
        // Non-degenerate triangle, shifted per tri so no vertex deduplication.
        emit_f32_le(s, static_cast<float>(tri * 10));
        emit_f32_le(s, 0.0f);
        emit_f32_le(s, 0.0f); // v0
        emit_f32_le(s, static_cast<float>((tri * 10) + 1));
        emit_f32_le(s, 0.0f);
        emit_f32_le(s, 0.0f); // v1
        emit_f32_le(s, static_cast<float>(tri * 10));
        emit_f32_le(s, 1.0f);
        emit_f32_le(s, 0.0f); // v2
        s.push_back(0);
        s.push_back(0); // attr
    }
    TmpFile t(tmp_path("rasterminal_test_2tri.stl"), s);
    Mesh m = load_ok(t.path);

    ASSERT_EQ(m.vertices.size(), size_t{ 6 });
    ASSERT_EQ(m.triangles.size(), size_t{ 2 });
    // Second triangle's indices must be offset by vert_base=3.
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
    ASSERT_EQ(m.triangles[1].v[0], 3u);
    ASSERT_EQ(m.triangles[1].v[1], 4u);
    ASSERT_EQ(m.triangles[1].v[2], 5u);
}

TEST(stl_valid, smooth_angle_is_noop_faceted_always)
{
    // STL is ALWAYS faceted: load_stl expands to 3 unshared vertices per triangle and calls
    // compute_normals with no weld map (mesh_stl.cpp), so no two triangles share a vertex
    // index and the crease angle has no adjacency to act on. This pins that --smooth-angle is
    // a silent no-op for STL — the same file at crease 0 and crease 180 must produce byte-
    // identical, per-face (never blended) normals. (The dedup+weld that would make crease
    // meaningful lives only on an abandoned branch; main keeps the unshared layout.)
    //
    // Two triangles share the edge (0,0,0)-(1,0,0) at a 90 deg fold: triangle A in XY
    // (normal +Z), triangle B folded into XZ (normal +Y). A crease-aware loader would blend
    // them toward 45 deg at crease 180; the faceted loader must not.
    std::string s(80, 'X');
    emit_u32_le(s, 2); // two triangles
    auto emit_tri = [&](float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz)
    {
        for (int i = 0; i < 3; i++)
        {
            emit_f32_le(s, 0.0f); // normal (ignored)
        }
        emit_f32_le(s, ax);
        emit_f32_le(s, ay);
        emit_f32_le(s, az);
        emit_f32_le(s, bx);
        emit_f32_le(s, by);
        emit_f32_le(s, bz);
        emit_f32_le(s, cx);
        emit_f32_le(s, cy);
        emit_f32_le(s, cz);
        s.push_back(0);
        s.push_back(0); // attr
    };
    emit_tri(0, 0, 0, 1, 0, 0, 0, 1, 0); // +Z face
    emit_tri(1, 0, 0, 0, 0, 0, 0, 0, 1); // +Y face
    TmpFile t(tmp_path("rasterminal_test_stl_facet.stl"), s);

    Mesh faceted;
    ASSERT_TRUE(faceted.load_model(t.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/0.0f));
    Mesh smoothed;
    ASSERT_TRUE(smoothed.load_model(t.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/180.0f));

    // Unshared layout: 3 vertices per triangle, regardless of the crease angle.
    ASSERT_EQ(faceted.vertices.size(), size_t{ 6 });
    ASSERT_EQ(smoothed.vertices.size(), size_t{ 6 });

    // Crease angle changes nothing — normals are identical between the two loads, and every
    // normal is its own face's axis-aligned normal (never a 45 deg blend of the two faces).
    for (size_t i = 0; i < faceted.vertices.size(); i++)
    {
        const vec3 nf = faceted.vertices[i].normal;
        const vec3 ns = smoothed.vertices[i].normal;
        ASSERT_NEAR(nf.x, ns.x, 1e-6f);
        ASSERT_NEAR(nf.y, ns.y, 1e-6f);
        ASSERT_NEAR(nf.z, ns.z, 1e-6f);
        ASSERT_TRUE(nf.z > 0.99f || nf.y > 0.99f); // axis-aligned, not blended
    }
}

TEST(reject, stl_header_read_under_5_bytes)
{
    // fread(header, 1, 80, ...) returns 3 < 5 — the early-exit guard fires before
    // any ASCII/binary detection.  stl_too_small_for_header uses 6 bytes (returns
    // 6 ≥ 5) so that test exercises stl_reader failure, not this guard.
    TmpFile t(tmp_path("rasterminal_test_3b.stl"), "abc");
    assert_rejects(t.path);
}

TEST(stl_valid, ascii_header_leading_whitespace)
{
    // Header starts with a space before "solid" — the whitespace-skip loop must
    // advance h past the space before strncmp fires, otherwise is_ascii stays
    // false and the binary-size check rejects the (non-binary) file.
    TmpFile t(
        tmp_path("rasterminal_test_wsp.stl"), " solid test\n"
                                              "facet normal 0 0 1\n"
                                              "  outer loop\n"
                                              "    vertex 0 0 0\n"
                                              "    vertex 1 0 0\n"
                                              "    vertex 0 1 0\n"
                                              "  endloop\n"
                                              "endfacet\n"
                                              "endsolid test\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, binary_surplus_trailing_bytes_accepted)
{
    // Size guard uses < not ==, so extra bytes after the last triangle are
    // silently ignored. Build a valid 1-triangle binary STL then append 20
    // extra bytes and verify the load still succeeds.
    std::string s(80, 'X');
    emit_u32_le(s, 1); // tri_count = 1
    for (int i = 0; i < 3; i++)
    {
        emit_f32_le(s, 0.0f); // normal (ignored)
    }
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 0.0f);
    emit_f32_le(s, 1.0f);
    emit_f32_le(s, 0.0f);
    s.push_back(0);
    s.push_back(0);       // attribute bytes
    s.append(20, '\xFF'); // 20 surplus trailing bytes
    TmpFile t(tmp_path("rasterminal_test_surplus.stl"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(reject, stl_file_open_failure)
{
    // Non-existent .stl path → !f (fopen returns null) → load_stl returns false.
    assert_rejects(tmp_path("rast_stl_no_such.stl"));
}

TEST(stl_valid, ascii_header_leading_tab_whitespace)
{
    // Leading tab before "solid": the `*h == '\t'` branch in the whitespace-skip
    // loop advances h so strncmp("solid",...) matches, setting is_ascii=true.
    // stl_reader::StlFileHasASCIIFormat uses find("solid") (finds it after the tab)
    // and ReadStlFile_ASCII tokenizes with >> (skips tab), so parsing succeeds.
    TmpFile t(
        tmp_path("rast_tab_hdr.stl"), "\tsolid test\n"
                                      "facet normal 0 0 1\n"
                                      "  outer loop\n"
                                      "    vertex 0 0 0\n"
                                      "    vertex 1 0 0\n"
                                      "    vertex 0 1 0\n"
                                      "  endloop\n"
                                      "endfacet\n"
                                      "endsolid test\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}
