#include "tests/loader_util.h"

// Appends one binary-STL triangle record (50 bytes): a zero normal (the loader
// ignores file normals), the three given corners, and the 2 attribute bytes.
// Only the payload is shared; each test still builds its own header, since the
// malformed fixtures deliberately vary the preamble.
static void
stl_append_tri(std::string &s, float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz)
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
    s.push_back(0); // attribute bytes
}

// One standard single-facet ASCII body shared by the header-variation tests: each
// passes its own opening line (the prefix or solid-name variant is exactly what
// the test pins); the geometry below it is identical on purpose.
static std::string stl_ascii_one_facet(const std::string &first_line)
{
    return first_line + "\n"
                        "facet normal 0 0 1\n"
                        "  outer loop\n"
                        "    vertex 0 0 0\n"
                        "    vertex 1 0 0\n"
                        "    vertex 0 1 0\n"
                        "  endloop\n"
                        "endfacet\n"
                        "endsolid test\n";
}

// Valid hand-written STL fixtures

TEST(stl_valid, ascii_single_facet)
{
    TmpFile t(tmp_path("rasterminal_test_min.stl"), stl_ascii_one_facet("solid test"));
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, binary_single_triangle)
{
    std::string s(80, 'X'); // 80-byte header (non-"solid")
    emit_u32_le(s, 1);      // tri_count = 1
    stl_append_tri(s, 0, 0, 0, 1, 0, 0, 0, 1, 0);

    TmpFile t(tmp_path("rasterminal_test_bin.stl"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, binary_header_starts_with_solid_disambiguates_via_size)
{
    // 80-byte header beginning with "solid " would trigger the ASCII path,
    // but a file size satisfying the binary layout (>= 84 + 50 × tri_count)
    // forces the binary interpretation. Pinned by load_stl's disambiguation
    // logic; binary_solid_header_trailing_bytes pins the >= (surplus) case.
    std::string s = "solid binary-stl-masquerading-as-ascii";
    s.resize(80, ' '); // pad to exactly 80 bytes
    emit_u32_le(s, 1); // tri_count = 1
    stl_append_tri(s, 0, 0, 0, 1, 0, 0, 0, 1, 0);

    TmpFile t(tmp_path("rasterminal_test_solidbin.stl"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

// Malformed STL rejection

TEST(reject, stl_too_small_for_header)
{
    // Less than 80 bytes: can't even read the header.
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
    // 0xFFFFFFFF triangles imply about 200 GB, so the size guard rejects before parsing.
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
    // Declare two triangles but provide one plus 20 bytes. The 154-byte file must
    // fail load_stl's 184-byte size guard before stl_reader runs.
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
    // Second triangle (truncated, only first 20 bytes).
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
    // outer loop with only two vertex lines: stl_reader fails to parse the facet.
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

// Loaded geometry

TEST(stl_valid, ascii_vertex_positions_and_defaults)
{
    // Verify actual coordinate values, per-vertex ao, material index, and mesh
    // defaults: the count-only tests above don't exercise any of this.
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

    // stl_reader sorts deduplicated positions, so keep these coordinates ascending
    // unless assertions are reordered. Z-up (1,2,3) remaps to Y-up (1,3,-2).
    ASSERT_NEAR(m.vertices[0].pos.x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertices[0].pos.y, 3.0f, 1e-5f);
    ASSERT_NEAR(m.vertices[0].pos.z, -2.0f, 1e-5f);
    ASSERT_NEAR(m.vertices[1].pos.x, 4.0f, 1e-5f);
    ASSERT_NEAR(m.vertices[2].pos.x, 7.0f, 1e-5f);

    // ao is hardcoded to 1.0 when copying stl_reader's vertices (STL has no AO data).
    for (const Vertex &v : m.vertices)
    {
        ASSERT_NEAR(v.ao, 1.0f, 1e-6f);
    }
}

TEST(stl_valid, zup_remapped_to_yup)
{
    // STL is Z-up by ecosystem convention; the loader must remap to the renderer's Y-up,
    // (x,y,z) -> (x,z,-y), so a file vertex on +Z loads on +Y. Pins the orientation fix:
    // a verbatim position copy would leave models sideways.
    TmpFile t(
        tmp_path("rasterminal_test_zup.stl"), "solid test\n"
                                              "facet normal 0 0 0\n"
                                              "  outer loop\n"
                                              "    vertex 0 0 0\n"
                                              "    vertex 1 0 0\n"
                                              "    vertex 0 0 1\n"
                                              "  endloop\n"
                                              "endfacet\n"
                                              "endsolid test\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.vertices.size(), size_t{ 3 });

    // stl_reader sorts positions lexicographically on the raw file coords:
    // (0,0,0) < (0,0,1) < (1,0,0), so the file's +Z vertex is index 1.
    ASSERT_NEAR(m.vertices[1].pos.x, 0.0f, 1e-6f);
    ASSERT_NEAR(m.vertices[1].pos.y, 1.0f, 1e-6f);
    ASSERT_NEAR(m.vertices[1].pos.z, 0.0f, 1e-6f);
}

TEST(stl_valid, ascii_file_normal_ignored_compute_normals_runs)
{
    // Supply the wrong file normal -Z. Recomputed geometry normal +Z remaps to +Y,
    // proving stl_reader normals are discarded.
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
        ASSERT_NEAR(v.normal.y, 1.0f, 1e-4f);
    }
}

TEST(stl_valid, binary_two_triangles_dedup_shared_edge)
{
    // Two coplanar triangles tile a square and share its diagonal. stl_reader's
    // deduplication must yield four positions, not six re-expanded corners.
    std::string s(80, 'X');
    emit_u32_le(s, 2); // two triangles
    stl_append_tri(s, 0, 0, 0, 1, 0, 0, 1, 1, 0);
    stl_append_tri(s, 0, 0, 0, 1, 1, 0, 0, 1, 0);
    TmpFile t(tmp_path("rasterminal_test_2tri.stl"), s);
    Mesh m = load_ok(t.path);

    ASSERT_EQ(m.vertices.size(), size_t{ 4 }); // 6 corners -> 4 unique positions
    ASSERT_EQ(m.triangles.size(), size_t{ 2 });

    // The two triangles must reference exactly two common vertex indices (the shared edge).
    int shared = 0;
    for (const uint32_t a : m.triangles[0].v)
    {
        for (const uint32_t b : m.triangles[1].v)
        {
            shared += (a == b) ? 1 : 0;
        }
    }
    ASSERT_EQ(shared, 2);
}

// Emit a binary 90-degree fold sharing the X-axis edge. Z-up normals +Z/+Y
// remap to Y-up +Y/-Z, with shared corners deduplicated for crease processing.
static std::string stl_90deg_fold()
{
    std::string s(80, 'X');
    emit_u32_le(s, 2);
    stl_append_tri(s, 0, 0, 0, 1, 0, 0, 0, 1, 0); // file +Z face -> loads with normal +Y
    stl_append_tri(s, 1, 0, 0, 0, 0, 0, 0, 0, 1); // file +Y face -> loads with normal -Z
    return s;
}

TEST(stl_valid, smooth_angle_controls_crease)
{
    // Like OBJ and PLY, STL deduplicates vertices so the crease angle controls shared edges.
    TmpFile t(tmp_path("rasterminal_test_stl_facet.stl"), stl_90deg_fold());

    Mesh faceted;
    ASSERT_TRUE(faceted.load_model(t.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/0.0f));
    Mesh smoothed;
    ASSERT_TRUE(smoothed.load_model(t.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/180.0f));

    // At 0 degrees, shared vertices split into six per-face wedges with +Y or -Z normals.
    ASSERT_EQ(faceted.vertices.size(), size_t{ 6 });
    for (const Vertex &v : faceted.vertices)
    {
        ASSERT_TRUE(v.normal.y > 0.99f || v.normal.z < -0.99f);
    }

    // crease 180: shared verts stay merged -> 4 verts; the two on the shared edge carry the
    // blended normal normalize(+Y + -Z) ~ (0, 0.707, -0.707), the two others stay axis-aligned.
    ASSERT_EQ(smoothed.vertices.size(), size_t{ 4 });
    int blended = 0;
    for (const Vertex &v : smoothed.vertices)
    {
        if (v.normal.y > 0.6f && v.normal.z < -0.6f)
        {
            ASSERT_NEAR(v.normal.y, 0.70710678f, 1e-3f);
            ASSERT_NEAR(v.normal.z, -0.70710678f, 1e-3f);
            blended++;
        }
        else
        {
            ASSERT_TRUE(v.normal.y > 0.99f || v.normal.z < -0.99f);
        }
    }
    ASSERT_EQ(blended, 2); // the two shared-edge vertices
}

TEST(stl_valid, crease_threshold_brackets_split_stl)
{
    // A 90-degree fold splits at 89 degrees and merges at 91 degrees.
    TmpFile t(tmp_path("rasterminal_test_stl_crease_bracket.stl"), stl_90deg_fold());

    Mesh just_below; // crease 89 < 90 deg dihedral -> split
    ASSERT_TRUE(just_below.load_model(t.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/89.0f));
    ASSERT_EQ(just_below.vertices.size(), size_t{ 6 });

    Mesh just_above; // crease 91 > 90 deg dihedral -> merge
    ASSERT_TRUE(just_above.load_model(t.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/91.0f));
    ASSERT_EQ(just_above.vertices.size(), size_t{ 4 });
}

TEST(reject, stl_header_read_under_5_bytes)
{
    // Three bytes fail before format detection; the six-byte case reaches the parser.
    TmpFile t(tmp_path("rasterminal_test_3b.stl"), "abc");
    assert_rejects(t.path);
}

TEST(stl_valid, ascii_header_leading_whitespace)
{
    // Classification skips leading whitespace before matching "solid".
    TmpFile t(tmp_path("rasterminal_test_wsp.stl"), stl_ascii_one_facet(" solid test"));
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, binary_surplus_trailing_bytes_accepted)
{
    // Binary size is a lower bound; trailing bytes are allowed.
    std::string s(80, 'X');
    emit_u32_le(s, 1); // tri_count = 1
    stl_append_tri(s, 0, 0, 0, 1, 0, 0, 0, 1, 0);
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
    // Leading tab before "solid": a tab is in the whitespace-skip set, so the
    // "solid" comparison still matches and is_ascii is set.
    TmpFile t(tmp_path("rast_tab_hdr.stl"), stl_ascii_one_facet("\tsolid test"));
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, ascii_header_leading_newline)
{
    // A blank first line before "solid": '\n' is in the whitespace-skip set, so the
    // file still classifies as ASCII. Before the fix it classified as binary and was
    // rejected by the size guard (bytes 80-83 of text read as a huge tri_count).
    TmpFile t(tmp_path("rast_nl_hdr.stl"), stl_ascii_one_facet("\nsolid test"));
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, ascii_header_leading_crlf)
{
    // A CRLF blank first line: '\r' is in the whitespace-skip set too, so a
    // Windows-authored file with a leading blank line classifies as ASCII.
    TmpFile t(
        tmp_path("rast_crlf_hdr.stl"), "\r\nsolid test\r\n"
                                       "facet normal 0 0 1\r\n"
                                       "  outer loop\r\n"
                                       "    vertex 0 0 0\r\n"
                                       "    vertex 1 0 0\r\n"
                                       "    vertex 0 1 0\r\n"
                                       "  endloop\r\n"
                                       "endfacet\r\n"
                                       "endsolid test\r\n"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, ascii_first_newline_past_256_bytes)
{
    // A long solid name puts the first newline after byte 256. Explicit ASCII
    // dispatch must bypass stl_reader's short sniffer and load it successfully.
    TmpFile t(tmp_path("rast_longname.stl"), stl_ascii_one_facet("solid " + std::string(300, 'n')));
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, binary_chatty_header_text)
{
    // A binary header contains ASCII keywords and a newline but does not start with
    // "solid". Explicit binary dispatch must bypass stl_reader's substring sniffer.
    std::string s = "STL export of solid part; facet normal data included\n";
    s.resize(80, '\0');
    emit_u32_le(s, 1); // tri_count = 1
    stl_append_tri(s, 0, 0, 0, 1, 0, 0, 0, 1, 0);
    TmpFile t(tmp_path("rast_chatty_hdr.stl"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, binary_solid_header_trailing_bytes)
{
    // "solid"-prefixed header AND surplus trailing bytes: the disambiguation uses
    // >= expected_binary (not ==), matching the binary guard's trailing-bytes
    // policy, so this still takes the binary path. With == it would classify as
    // ASCII and fail to parse.
    std::string s = "solid binary-with-trailing-bytes";
    s.resize(80, ' ');
    emit_u32_le(s, 1); // tri_count = 1
    stl_append_tri(s, 0, 0, 0, 1, 0, 0, 0, 1, 0);
    s.append(20, '\xFF'); // 20 surplus trailing bytes
    TmpFile t(tmp_path("rast_solidbin_trail.stl"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, binary_skips_ascii_line_scan)
{
    // A 70 KB binary STL with no newline must bypass the ASCII line-length scan.
    // Unit-triangle float bytes guarantee the fixture contains no accidental 0x0A.
    std::string s(80, 'X');
    emit_u32_le(s, 1400); // 1400 x 50 bytes = 70000 bytes of triangle data
    for (int i = 0; i < 1400; i++)
    {
        stl_append_tri(s, 0, 0, 0, 1, 0, 0, 0, 1, 0);
    }
    ASSERT_TRUE(s.size() > size_t{ 64 } * 1024);
    ASSERT_TRUE(s.find('\n') == std::string::npos);
    TmpFile t(tmp_path("rast_bin_nolf.stl"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1400 });
}

TEST(stl_valid, ascii_larger_than_line_bound_loads)
{
    // A file above 64 KB still passes when each line stays within the limit.
    std::string s = "solid test\n";
    for (int i = 0; i < 700; i++)
    {
        s += "facet normal 0 0 1\n"
             "  outer loop\n"
             "    vertex 0 0 0\n"
             "    vertex 1 0 0\n"
             "    vertex 0 1 0\n"
             "  endloop\n"
             "endfacet\n";
    }
    s += "endsolid test\n";
    ASSERT_TRUE(s.size() > size_t{ 64 } * 1024); // exceed the detection bound
    TmpFile t(tmp_path("rast_big_ascii.stl"), s);
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 700 });
}

TEST(reject, stl_ascii_line_exceeds_bound)
{
    // Reject a 70,000-character ignored ASCII line before the vendored parser
    // tokenizes it into excessive temporary allocations.
    std::string s = "solid test\n"
                    "facet normal 0 0 1\n"
                    "  outer loop\n"
                    "    vertex 0 0 0\n"
                    "    vertex 1 0 0\n"
                    "    vertex 0 1 0\n"
                    "  endloop\n"
                    "endfacet\n";
    s.append(70000, 'j');
    s += "\nendsolid test\n";
    TmpFile t(tmp_path("rast_longline.stl"), s);
    assert_rejects(t.path);
}

TEST(stl_valid, ascii_uppercase_solid_keyword)
{
    // Classification matches the leading "solid" case-insensitively.
    TmpFile t(tmp_path("rast_upper_hdr.stl"), stl_ascii_one_facet("SOLID test"));
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, ascii_under_80_bytes)
{
    // A valid 72-byte ASCII file must classify from a partial 80-byte header read.
    TmpFile t(
        tmp_path("rast_tiny_ascii.stl"), "solid\n"
                                         "facet normal 0 0 0\n"
                                         "vertex 0 0 0\n"
                                         "vertex 1 0 0\n"
                                         "vertex 0 1 0\n"
                                         "endfacet"
    );
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}

TEST(stl_valid, ascii_utf8_bom_before_solid)
{
    // Skip a UTF-8 BOM before classifying "solid" as ASCII. stl_reader ignores the
    // BOM-prefixed first token and still parses following facets.
    TmpFile t(tmp_path("rast_bom_hdr.stl"), stl_ascii_one_facet("\xEF\xBB\xBFsolid test"));
    Mesh m = load_ok(t.path);
    ASSERT_EQ(m.triangles.size(), size_t{ 1 });
}
