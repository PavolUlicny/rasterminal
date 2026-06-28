#include "tests/gltf_test_util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════════
//  TRIANGLE STRIP / FAN DE-STRIPIFY HELPERS
// ═══════════════════════════════════════════════════════════════════════════
//
// Vertices are authored with a unique integer x per index (a zig-zag in y so the
// triangles are non-degenerate), so a vertex id is recoverable from its world
// position as round(|x|). That survives both optimize_vertex_cache (which remaps
// vertex indices and reorders triangles) and negative-determinant node transforms
// (which only negate x). All strip/fan assertions are therefore position-based.

static void emit_pos(std::string &bin, float x, float y, float z)
{
    emit_f32_le(bin, x);
    emit_f32_le(bin, y);
    emit_f32_le(bin, z);
}

// n zig-zag VEC3 positions: vertex i at (i, i&1, 0) → id recoverable as round(|x|).
static void emit_zigzag(std::string &bin, int n)
{
    for (int i = 0; i < n; i++)
    {
        emit_pos(bin, static_cast<float>(i), static_cast<float>(i & 1), 0.0f);
    }
}

// n convex VEC3 positions for a fan: vertex i at (i, i*(i-1)/2, 0). The hub (v0) plus a
// convex chain makes every fan triangle wind the same way, so winding_signs is meaningful.
// id is still round(|x|).
static void emit_fan_verts(std::string &bin, int n)
{
    for (int i = 0; i < n; i++)
    {
        const int tri = (i * (i - 1)) / 2; // i-th triangular number (always exact: i*(i-1) is even)
        emit_pos(bin, static_cast<float>(i), static_cast<float>(tri), 0.0f);
    }
}

static int vid_of(const vec3 &p)
{
    return static_cast<int>(std::lround(static_cast<double>(std::fabs(p.x))));
}

// Rotate a vertex-id triple so the smallest id is first, preserving cyclic order
// (winding-preserving canonical form).
static std::array<int, 3> canon3(std::array<int, 3> t)
{
    size_t k = 0;
    if (t[1] < t[k])
    {
        k = 1;
    }
    if (t[2] < t[k])
    {
        k = 2;
    }
    return { t[k], t[(k + 1) % 3], t[(k + 2) % 3] };
}

// Order-independent triangle set: each triangle as a winding-preserving vertex-id
// triple, sorted. Two meshes with the same triangles AND winding compare equal.
static std::vector<std::array<int, 3>> canonical_tris(const Mesh &m)
{
    std::vector<std::array<int, 3>> out;
    out.reserve(m.triangles.size());
    for (const Triangle &t : m.triangles)
    {
        out.push_back(canon3({ vid_of(m.vertices[t.v[0]].pos), vid_of(m.vertices[t.v[1]].pos),
                               vid_of(m.vertices[t.v[2]].pos) }));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Assert a mesh's triangles equal `expected` (raw spec triples; canonicalised here,
// so winding matters but vertex/triangle ordering does not).
static void assert_tris_eq(const Mesh &m, std::vector<std::array<int, 3>> expected)
{
    const std::vector<std::array<int, 3>> got = canonical_tris(m);
    for (std::array<int, 3> &e : expected)
    {
        e = canon3(e);
    }
    std::sort(expected.begin(), expected.end());
    ASSERT_EQ(got.size(), expected.size());
    for (size_t i = 0; i < got.size(); i++)
    {
        ASSERT_TRUE(got[i] == expected[i]);
    }
}

// Sign of each triangle's world-space XY signed area in stored v[] order
// (+1 CCW, -1 CW, 0 degenerate). Captures winding directly.
static std::vector<int> winding_signs(const Mesh &m)
{
    std::vector<int> signs;
    signs.reserve(m.triangles.size());
    for (const Triangle &t : m.triangles)
    {
        const vec3 &a = m.vertices[t.v[0]].pos;
        const vec3 &b = m.vertices[t.v[1]].pos;
        const vec3 &c = m.vertices[t.v[2]].pos;
        const float cross = ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
        signs.push_back(cross > 1e-4f ? 1 : (cross < -1e-4f ? -1 : 0));
    }
    return signs;
}

// Assert every triangle is wound the same way (no degenerates) and return that sign.
static int assert_uniform_winding(const Mesh &m)
{
    const std::vector<int> signs = winding_signs(m);
    ASSERT_TRUE(!signs.empty());
    for (int s : signs)
    {
        ASSERT_EQ(s, signs[0]);
    }
    ASSERT_TRUE(signs[0] != 0);
    return signs[0];
}

// Build a single-node/mesh/primitive GLB: `mode` (4=tris,5=strip,6=fan,0=points),
// `n_verts` zig-zag positions, optional index list (u16, or u32 when `wide`), and an
// optional negative-determinant node scale (`mirror`) that drives flip_winding.
static std::string
prim_glb(int mode, int n_verts, const std::vector<uint32_t> &indices, bool wide = false, bool mirror = false)
{
    std::string bin;
    const bool fan_layout = mode == 6;
    if (fan_layout)
    {
        emit_fan_verts(bin, n_verts);
    }
    else
    {
        emit_zigzag(bin, n_verts);
    }
    const size_t pos_bytes = bin.size();

    const bool indexed = !indices.empty();
    for (uint32_t idx : indices)
    {
        if (wide)
        {
            emit_u32_le(bin, idx);
        }
        else
        {
            emit_u16_le(bin, static_cast<uint16_t>(idx));
        }
    }
    const size_t idx_bytes = bin.size() - pos_bytes;
    while (bin.size() % 4 != 0) // GLB BIN chunk must be 4-byte aligned
    {
        bin.push_back('\0');
    }

    const int last = n_verts > 0 ? n_verts - 1 : 0;
    const std::string maxx = std::to_string(last);
    const std::string maxy = std::to_string(fan_layout ? (last * (last - 1)) / 2 : 1);
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0)";
    if (mirror)
    {
        json += R"(,"scale":[-1,1,1])";
    }
    json += R"(}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":)" + std::to_string(mode);
    if (indexed)
    {
        json += R"(,"indices":1)";
    }
    json += R"(}]}],"accessors":[{"bufferView":0,"componentType":5126,"count":)" + std::to_string(n_verts) +
            R"(,"type":"VEC3","min":[0,0,0],"max":[)" + maxx + "," + maxy + R"(,0]})";
    if (indexed)
    {
        json += R"(,{"bufferView":1,"componentType":)" + std::to_string(wide ? 5125 : 5123) + R"(,"count":)" +
                std::to_string(indices.size()) + R"(,"type":"SCALAR"})";
    }
    json += R"(],"bufferViews":[{"buffer":0,"byteLength":)" + std::to_string(pos_bytes) + R"(,"byteOffset":0})";
    if (indexed)
    {
        json += R"(,{"buffer":0,"byteLength":)" + std::to_string(idx_bytes) + R"(,"byteOffset":)" +
                std::to_string(pos_bytes) + "}";
    }
    json += R"(],"buffers":[{"byteLength":)" + std::to_string(bin.size()) + "}]}";

    return make_glb(json, bin);
}

// Build a one-mesh GLB with two non-indexed primitives sharing one buffer, each with its
// own POSITION accessor. Used for mixed-primitive coverage (counts only; vertex ids of the
// two primitives overlap by design, so these tests assert sizes, not canonical_tris).
static std::string two_prim_glb(int mode_a, int n_a, int mode_b, int n_b)
{
    std::string bin;
    const auto emit_verts = [&](int mode, int n)
    {
        if (mode == 6)
        {
            emit_fan_verts(bin, n);
        }
        else
        {
            emit_zigzag(bin, n);
        }
    };
    emit_verts(mode_a, n_a);
    const size_t a_bytes = bin.size();
    emit_verts(mode_b, n_b);
    const size_t b_bytes = bin.size() - a_bytes;
    while (bin.size() % 4 != 0)
    {
        bin.push_back('\0');
    }

    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":)" + std::to_string(mode_a) +
            R"(},{"attributes":{"POSITION":1},"mode":)" + std::to_string(mode_b) + R"(}]}],)";
    json += R"("accessors":[{"bufferView":0,"componentType":5126,"count":)" + std::to_string(n_a) +
            R"(,"type":"VEC3","min":[0,0,0],"max":[100,100,0]},{"bufferView":1,"componentType":5126,"count":)" +
            std::to_string(n_b) + R"(,"type":"VEC3","min":[0,0,0],"max":[100,100,0]}],)";
    json += R"("bufferViews":[{"buffer":0,"byteLength":)" + std::to_string(a_bytes) +
            R"(,"byteOffset":0},{"buffer":0,"byteLength":)" + std::to_string(b_bytes) + R"(,"byteOffset":)" +
            std::to_string(a_bytes) + R"(}],"buffers":[{"byteLength":)" + std::to_string(bin.size()) + "}]}";
    return make_glb(json, bin);
}

// Build a 4-vertex TRIANGLE_STRIP GLB with optional per-vertex NORMAL / TEXCOORD_0 / COLOR_0
// and an optional material. color_dim: 0 none / 3 vec3 / 4 vec4. mat: 0 none / 1 plain / 2 BLEND.
// All attributes are constant across the 4 verts so a single sampled value verifies they survive
// the de-stripify + optimize remap.
static std::string strip_attr_glb(bool normal, bool uv, int color_dim, int mat)
{
    std::string bin;
    emit_zigzag(bin, 4);
    const size_t pos_bytes = bin.size();
    std::string accs = R"({"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[3,1,0]})";
    std::string bvs = R"({"buffer":0,"byteLength":)" + std::to_string(pos_bytes) + R"(,"byteOffset":0})";
    std::string attrs = R"("POSITION":0)";
    int next = 1;
    const auto add_attr = [&](const char *name, const char *type, auto emit_one)
    {
        const size_t off = bin.size();
        for (int i = 0; i < 4; i++)
        {
            emit_one(bin);
        }
        const size_t len = bin.size() - off;
        bvs += R"(,{"buffer":0,"byteLength":)" + std::to_string(len) + R"(,"byteOffset":)" + std::to_string(off) + "}";
        accs += R"(,{"bufferView":)" + std::to_string(next) + R"(,"componentType":5126,"count":4,"type":")" + type +
                R"("})";
        attrs += std::string(R"(,")") + name + R"(":)" + std::to_string(next);
        next++;
    };
    if (normal)
    {
        add_attr("NORMAL", "VEC3", [](std::string &b) { emit_pos(b, 0.0f, 0.0f, 1.0f); });
    }
    if (uv)
    {
        add_attr(
            "TEXCOORD_0", "VEC2",
            [](std::string &b)
            {
                emit_f32_le(b, 0.25f);
                emit_f32_le(b, 0.75f);
            }
        );
    }
    if (color_dim == 3)
    {
        add_attr("COLOR_0", "VEC3", [](std::string &b) { emit_pos(b, 0.2f, 0.4f, 0.6f); });
    }
    if (color_dim == 4)
    {
        add_attr(
            "COLOR_0", "VEC4",
            [](std::string &b)
            {
                emit_f32_le(b, 0.2f);
                emit_f32_le(b, 0.4f);
                emit_f32_le(b, 0.6f);
                emit_f32_le(b, 0.5f);
            }
        );
    }

    std::string mats;
    std::string mat_ref;
    if (mat == 1)
    {
        mats = R"(,"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1]}}])";
        mat_ref = R"(,"material":0)";
    }
    else if (mat == 2)
    {
        mats = R"(,"materials":[{"alphaMode":"BLEND","pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1]}}])";
        mat_ref = R"(,"material":0)";
    }

    while (bin.size() % 4 != 0)
    {
        bin.push_back('\0');
    }
    std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{)" +
        attrs + R"(},"mode":5)" + mat_ref + R"(}]}])" + mats + R"(,"accessors":[)" + accs + R"(],"bufferViews":[)" +
        bvs + R"(],"buffers":[{"byteLength":)" + std::to_string(bin.size()) + "}]}";
    return make_glb(json, bin);
}

// ─── Group A: non-triangle primitive types rejected ───────────────────────

TEST(reject, gltf_points_only_mesh)
{
    // mode:0 = POINTS — skipped by the loader, no triangles emitted → reject.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"mode\":0}]}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_points.glb"), make_glb(json, bin));
    assert_rejects(f.path);
}

TEST(reject, gltf_lines_only_mesh)
{
    // mode:1 = LINES — skipped by the loader → reject.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"mode\":1}]}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_lines.glb"), make_glb(json, bin));
    assert_rejects(f.path);
}

// mode:5/6 are now de-stripified into the triangle list (see the STRIP/FAN group below).
// A minimal 3-vertex strip and fan both yield exactly one triangle.

TEST(gltf_valid, gltf_triangle_strip_min)
{
    TmpFile f(tmp_path("rast_tristrip.glb"), prim_glb(5, 3, {}));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    assert_tris_eq(m, { { 0, 1, 2 } });
}

TEST(gltf_valid, gltf_triangle_fan_min)
{
    TmpFile f(tmp_path("rast_trifan.glb"), prim_glb(6, 3, {}));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    assert_tris_eq(m, { { 0, 1, 2 } });
}

// ═══════════════════════════════════════════════════════════════════════════
//  TRIANGLE STRIP / FAN DE-STRIPIFY
// ═══════════════════════════════════════════════════════════════════════════

// ─── A: strip topology + winding ──────────────────────────────────────────

TEST(gltf_strip, non_indexed_4verts)
{
    // 4 verts → 2 tris. Spec winding: even (0,1,2), odd (2,1,3). Uniform winding
    // is the key check — a missing odd-triangle swap flips tri1's winding.
    TmpFile f(tmp_path("rast_strip4.glb"), prim_glb(5, 4, {}));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    assert_tris_eq(m, { { 0, 1, 2 }, { 2, 1, 3 } });
    assert_uniform_winding(m);
}

TEST(gltf_strip, non_indexed_5verts)
{
    // 5 verts → 3 tris, covering even→odd→even: (0,1,2),(2,1,3),(2,3,4).
    TmpFile f(tmp_path("rast_strip5.glb"), prim_glb(5, 5, {}));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(3));
    assert_tris_eq(m, { { 0, 1, 2 }, { 2, 1, 3 }, { 2, 3, 4 } });
    assert_uniform_winding(m);
}

TEST(gltf_strip, indexed_u16)
{
    // Indexed strip over 4 verts, u16 indices → same triangles as non_indexed_4verts.
    TmpFile f(tmp_path("rast_strip_u16.glb"), prim_glb(5, 4, { 0, 1, 2, 3 }));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    assert_tris_eq(m, { { 0, 1, 2 }, { 2, 1, 3 } });
    assert_uniform_winding(m);
}

TEST(gltf_strip, indexed_u32_wide)
{
    // u32 (componentType 5125) index read through the strip path; 5 indices → 3 tris.
    TmpFile f(tmp_path("rast_strip_u32.glb"), prim_glb(5, 5, { 0, 1, 2, 3, 4 }, /*wide=*/true));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(3));
    assert_tris_eq(m, { { 0, 1, 2 }, { 2, 1, 3 }, { 2, 3, 4 } });
    assert_uniform_winding(m);
}

// ─── B: fan topology + winding ────────────────────────────────────────────

TEST(gltf_fan, non_indexed_4verts)
{
    // Fan: every triangle shares the hub v0 → (0,1,2),(0,2,3).
    TmpFile f(tmp_path("rast_fan4.glb"), prim_glb(6, 4, {}));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    assert_tris_eq(m, { { 0, 1, 2 }, { 0, 2, 3 } });
    assert_uniform_winding(m);
    // Hub present in every triangle (distinguishes fan from strip).
    for (const Triangle &t : m.triangles)
    {
        const bool has_hub = vid_of(m.vertices[t.v[0]].pos) == 0 || vid_of(m.vertices[t.v[1]].pos) == 0 ||
                             vid_of(m.vertices[t.v[2]].pos) == 0;
        ASSERT_TRUE(has_hub);
    }
}

TEST(gltf_fan, indexed_u16)
{
    // 5 indices → 3 tris: (0,1,2),(0,2,3),(0,3,4).
    TmpFile f(tmp_path("rast_fan_u16.glb"), prim_glb(6, 5, { 0, 1, 2, 3, 4 }));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(3));
    assert_tris_eq(m, { { 0, 1, 2 }, { 0, 2, 3 }, { 0, 3, 4 } });
    assert_uniform_winding(m);
}

// ─── C: count edge cases ──────────────────────────────────────────────────
// (count == 3 is covered by gltf_triangle_strip_min / gltf_triangle_fan_min above.)

TEST(reject, gltf_strip_2verts_empty)
{
    // count == 2 → 0 triangles → empty mesh → rejected.
    TmpFile f(tmp_path("rast_strip2.glb"), prim_glb(5, 2, {}));
    assert_rejects(f.path);
}

TEST(reject, gltf_strip_1index_empty)
{
    // Indexed strip, 1 index → 0 triangles → rejected.
    TmpFile f(tmp_path("rast_strip1.glb"), prim_glb(5, 1, { 0 }));
    assert_rejects(f.path);
}

TEST(gltf_fan, indexed_count4_boundary)
{
    // count == 4 → exactly count-2 == 2 tris (the i+2 < count loop boundary).
    TmpFile f(tmp_path("rast_fan_b.glb"), prim_glb(6, 4, { 0, 1, 2, 3 }));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    assert_tris_eq(m, { { 0, 1, 2 }, { 0, 2, 3 } });
}

// ─── D: winding under a mirror (negative-determinant) node transform ───────

TEST(gltf_strip, mirror_preserves_consistent_winding)
{
    // A scale:[-1,1,1] node flips winding; the de-stripify must compose its odd-parity
    // swap with flip_winding so all triangles stay consistently — and identically — wound.
    TmpFile fi(tmp_path("rast_strip_id.glb"), prim_glb(5, 4, {}));
    TmpFile fm(tmp_path("rast_strip_mir.glb"), prim_glb(5, 4, {}, /*wide=*/false, /*mirror=*/true));
    Mesh mi = load_ok(fi.path);
    Mesh mm = load_ok(fm.path);
    ASSERT_EQ(mm.triangles.size(), static_cast<size_t>(2));
    const int sid = assert_uniform_winding(mi);
    const int smir = assert_uniform_winding(mm);
    ASSERT_EQ(smir, sid); // flip_winding keeps world-space facing identical to the un-mirrored mesh
}

TEST(gltf_fan, mirror_preserves_consistent_winding)
{
    TmpFile fi(tmp_path("rast_fan_id.glb"), prim_glb(6, 4, {}));
    TmpFile fm(tmp_path("rast_fan_mir.glb"), prim_glb(6, 4, {}, /*wide=*/false, /*mirror=*/true));
    Mesh mi = load_ok(fi.path);
    Mesh mm = load_ok(fm.path);
    ASSERT_EQ(mm.triangles.size(), static_cast<size_t>(2));
    const int sid = assert_uniform_winding(mi);
    const int smir = assert_uniform_winding(mm);
    ASSERT_EQ(smir, sid);
}

// ─── E: degenerate stitch triangles kept ──────────────────────────────────

TEST(gltf_strip, degenerate_stitch_triangles_kept)
{
    // A repeated index makes some triangles zero-area. They are NOT filtered (render-time
    // backface/zero-area drop handles them): triangle count stays count-2.
    TmpFile f(tmp_path("rast_strip_deg.glb"), prim_glb(5, 4, { 0, 1, 2, 2, 3 }));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(3)); // count(5) - 2, degenerates included
}

// ─── F: mixed primitives in one mesh ──────────────────────────────────────

TEST(gltf_mixed, triangles_plus_strip)
{
    TmpFile f(tmp_path("rast_mix_ts.glb"), two_prim_glb(/*tris*/ 4, 3, /*strip*/ 5, 4));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(3)); // 1 + 2
    ASSERT_EQ(m.vertices.size(), static_cast<size_t>(7));
}

TEST(gltf_mixed, triangles_plus_fan)
{
    TmpFile f(tmp_path("rast_mix_tf.glb"), two_prim_glb(/*tris*/ 4, 3, /*fan*/ 6, 4));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(3)); // 1 + 2
}

TEST(gltf_mixed, strip_plus_points_drops_points)
{
    // POINTS primitive is skipped entirely (its 5 verts are never pushed).
    TmpFile f(tmp_path("rast_mix_sp.glb"), two_prim_glb(/*strip*/ 5, 4, /*points*/ 0, 5));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    ASSERT_EQ(m.vertices.size(), static_cast<size_t>(4));
}

TEST(gltf_mixed, strip_plus_fan)
{
    TmpFile f(tmp_path("rast_mix_sf.glb"), two_prim_glb(/*strip*/ 5, 4, /*fan*/ 6, 4));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(4)); // 2 + 2
}

// ─── G: per-vertex attributes carried through de-stripify ──────────────────

TEST(gltf_strip, carries_normals)
{
    TmpFile f(tmp_path("rast_strip_nrm.glb"), strip_attr_glb(/*normal=*/true, false, 0, 0));
    Mesh m = load_ok(f.path);
    for (const Vertex &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.x, 0.0f, 1e-3f);
        ASSERT_NEAR(v.normal.y, 0.0f, 1e-3f);
        ASSERT_NEAR(v.normal.z, 1.0f, 1e-3f);
    }
}

TEST(gltf_strip, carries_uv_with_vflip)
{
    // Authored v = 0.75 → stored 1 - 0.75 = 0.25 (loader v-flip).
    TmpFile f(tmp_path("rast_strip_uv.glb"), strip_attr_glb(false, /*uv=*/true, 0, 0));
    Mesh m = load_ok(f.path);
    for (const Vertex &v : m.vertices)
    {
        ASSERT_NEAR(v.uv.x, 0.25f, 1e-4f);
        ASSERT_NEAR(v.uv.y, 0.25f, 1e-4f);
    }
}

TEST(gltf_strip, carries_vertex_colors)
{
    TmpFile f(tmp_path("rast_strip_col.glb"), strip_attr_glb(false, false, /*color_dim=*/3, 0));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), m.vertices.size());
    for (const vec3 &c : m.vertex_colors)
    {
        ASSERT_NEAR(c.x, 0.2f, 1e-4f);
        ASSERT_NEAR(c.y, 0.4f, 1e-4f);
        ASSERT_NEAR(c.z, 0.6f, 1e-4f);
    }
}

TEST(gltf_strip, carries_vertex_alpha_blend)
{
    // COLOR_0 vec4 alpha 0.5 under alphaMode=BLEND → transparent strip; all triangles blend,
    // so opaque_count == 0 and the partition stays valid.
    TmpFile f(tmp_path("rast_strip_a.glb"), strip_attr_glb(false, false, /*color_dim=*/4, /*mat=*/2));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_alpha);
    ASSERT_TRUE(m.has_transparent);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    ASSERT_EQ(m.opaque_count, 0u);
}

TEST(gltf_strip, carries_material_index)
{
    // A primitive material is pushed at index 1 (default white is 0); every emitted triangle
    // must reference it.
    TmpFile f(tmp_path("rast_strip_mat.glb"), strip_attr_glb(false, false, 0, /*mat=*/1));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    for (const Triangle &t : m.triangles)
    {
        ASSERT_EQ(t.material_idx, 1u);
    }
}

TEST(gltf_valid, default_mode_is_triangles)
{
    // No "mode" field → cgltf defaults to TRIANGLES. Non-indexed, 3 verts → 1 tri.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0}}]}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_defmode.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_EQ(m.vertices.size(), static_cast<size_t>(3));
}

// ─── Group B: mixed primitive types ──────────────────────────────────────

TEST(gltf_valid, mixed_mesh_triangles_and_points_only_loads_triangles)
{
    // One mesh, two primitives: TRIANGLES (3 verts) + POINTS (5 verts).
    // POINTS primitive is entirely skipped (including its vertex push).
    // Expected: 1 triangle, 3 vertices.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0}},"
                             "{\"attributes\":{\"POSITION\":1},\"mode\":0}"
                             "]}],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":5,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0},"
                             "{\"buffer\":0,\"byteLength\":60,\"byteOffset\":36}"
                             "],"
                             "\"buffers\":[{\"byteLength\":96}]}";

    std::string bin;
    emit_tri_verts(bin);        // accessor 0: 3 verts (36 bytes)
    for (int i = 0; i < 5; ++i) // accessor 1: 5 verts (60 bytes)
    {
        emit_f32_le(bin, static_cast<float>(i));
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 0.0f);
    }

    TmpFile f(tmp_path("rast_mixed.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_EQ(m.vertices.size(), static_cast<size_t>(3));
}
