#include "loader_util.h"

// ═══════════════════════════════════════════════════════════════════════════
//  DOUBLE-SIDED MATERIALS
// ═══════════════════════════════════════════════════════════════════════════

TEST(gltf_valid, pbr_material_mapping)
{
    // Known PBR values → verify Blinn-Phong mapping:
    //   diffuse  = baseColorFactor.rgb = {0.5, 0.2, 0.8}
    //   ambient  = diffuse
    //   specular = {metallicFactor, …} = {0.3, 0.3, 0.3}
    //   shininess = (1 - roughnessFactor) * 126 + 2 = 0.6*126+2 = 77.6
    std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0},\"material\":0}]}],\"materials\":[{\"pbrMetallicRoughness\":"
        "{\"baseColorFactor\":[0.5,0.2,0.8,1.0],\"metallicFactor\":0.3,"
        "\"roughnessFactor\":0.4}}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
        "\"buffers\":[{\"byteLength\":36}]}";
    while (json.size() % 4 != 0)
        json += ' ';
    const uint32_t jlen = static_cast<uint32_t>(json.size());

    std::string bin;
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    const uint32_t blen = static_cast<uint32_t>(bin.size());

    std::string glb;
    emit_u32_le(glb, 0x46546C67u);
    emit_u32_le(glb, 2u);
    emit_u32_le(glb, 12u + 8u + jlen + 8u + blen);
    emit_u32_le(glb, jlen);
    emit_u32_le(glb, 0x4E4F534Au);
    glb += json;
    emit_u32_le(glb, blen);
    emit_u32_le(glb, 0x004E4942u);
    glb += bin;

    TmpFile f(tmp_path("rast_pbr.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const Material &mat = m.materials[1];
    ASSERT_NEAR(mat.diffuse.x, 0.5f, 1e-4f);
    ASSERT_NEAR(mat.diffuse.y, 0.2f, 1e-4f);
    ASSERT_NEAR(mat.diffuse.z, 0.8f, 1e-4f);
    ASSERT_NEAR(mat.ambient.x, 0.5f, 1e-4f);
    ASSERT_NEAR(mat.specular.x, 0.3f, 1e-4f);
    ASSERT_NEAR(mat.shininess, 77.6f, 1e-2f);
}

TEST(gltf_valid, double_sided_flag_set)
{
    // Minimal GLB: one triangle, material with doubleSided:true.
    std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0},\"material\":0}]}],\"materials\":[{\"doubleSided\":true}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
        "\"buffers\":[{\"byteLength\":36}]}";
    while (json.size() % 4 != 0)
        json += ' ';
    const uint32_t jlen = static_cast<uint32_t>(json.size());

    std::string bin;
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    const uint32_t blen = static_cast<uint32_t>(bin.size()); // 36

    std::string glb;
    emit_u32_le(glb, 0x46546C67u);                 // magic "glTF"
    emit_u32_le(glb, 2u);                          // version
    emit_u32_le(glb, 12u + 8u + jlen + 8u + blen); // total length
    emit_u32_le(glb, jlen);
    emit_u32_le(glb, 0x4E4F534Au);
    glb += json; // JSON chunk
    emit_u32_le(glb, blen);
    emit_u32_le(glb, 0x004E4942u);
    glb += bin; // BIN chunk

    TmpFile f(tmp_path("rast_ds.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_double_sided);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].double_sided);
}

TEST(gltf_valid, missing_scene_falls_back_to_first_scene_and_mask_cutoff_is_loaded)
{
    std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],"
        "\"materials\":[{\"alphaMode\":\"MASK\",\"alphaCutoff\":0.25}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}"
        "],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
        "\"buffers\":[{\"byteLength\":36}]}";
    while (json.size() % 4 != 0)
        json += ' ';
    const uint32_t jlen = static_cast<uint32_t>(json.size());

    std::string bin;
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    const uint32_t blen = static_cast<uint32_t>(bin.size());

    std::string glb;
    emit_u32_le(glb, 0x46546C67u);
    emit_u32_le(glb, 2u);
    emit_u32_le(glb, 12u + 8u + jlen + 8u + blen);
    emit_u32_le(glb, jlen);
    emit_u32_le(glb, 0x4E4F534Au);
    glb += json;
    emit_u32_le(glb, blen);
    emit_u32_le(glb, 0x004E4942u);
    glb += bin;

    TmpFile f(tmp_path("rast_scene_fallback_mask.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].alpha_cutoff, 0.25f, 1e-6f);
}

TEST(gltf_valid, unused_vertex_keeps_ao_at_one)
{
    std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"NORMAL\":1},\"indices\":2}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}"
        "],"
        "\"buffers\":[{\"byteLength\":102}]}";
    while (json.size() % 4 != 0)
        json += ' ';
    const uint32_t jlen = static_cast<uint32_t>(json.size());

    std::string bin;
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    for (int i = 0; i < 4; i++)
    {
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 1.0f);
    }
    bin.push_back(static_cast<char>(0));
    bin.push_back(static_cast<char>(0));
    bin.push_back(static_cast<char>(1));
    bin.push_back(static_cast<char>(0));
    bin.push_back(static_cast<char>(2));
    bin.push_back(static_cast<char>(0));
    const uint32_t blen = static_cast<uint32_t>(bin.size());

    std::string glb;
    emit_u32_le(glb, 0x46546C67u);
    emit_u32_le(glb, 2u);
    emit_u32_le(glb, 12u + 8u + jlen + 8u + blen);
    emit_u32_le(glb, jlen);
    emit_u32_le(glb, 0x4E4F534Au);
    glb += json;
    emit_u32_le(glb, blen);
    emit_u32_le(glb, 0x004E4942u);
    glb += bin;

    TmpFile f(tmp_path("rast_unused_ao.glb"), glb.data(), glb.size());
    Mesh m;
    ASSERT_TRUE(m.load_model(f.path, /*ao=*/true));
    ASSERT_EQ(m.vertices.size(), static_cast<size_t>(4));
    ASSERT_NEAR(m.vertices[3].ao, 1.0f, 1e-6f);
}

// ═══════════════════════════════════════════════════════════════════════════
//  REJECTIONS
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, empty_file_gltf)
{
    TmpFile t(tmp_path("rast_empty.gltf"), "");
    assert_rejects(t.path);
}

TEST(reject, empty_file_glb)
{
    TmpFile t(tmp_path("rast_empty.glb"), "");
    assert_rejects(t.path);
}

// ═══════════════════════════════════════════════════════════════════════════
//  PRIMITIVE TYPE COVERAGE
// ═══════════════════════════════════════════════════════════════════════════

// Local helpers for constructing minimal GLBs without repeating the
// header-assembly boilerplate.

static std::string make_glb(const std::string &json_raw, const std::string &bin)
{
    std::string json = json_raw;
    while (json.size() % 4 != 0)
        json += ' ';
    const uint32_t jlen = static_cast<uint32_t>(json.size());
    const uint32_t blen = static_cast<uint32_t>(bin.size());
    std::string glb;
    emit_u32_le(glb, 0x46546C67u);
    emit_u32_le(glb, 2u);
    emit_u32_le(glb, 12u + 8u + jlen + 8u + blen);
    emit_u32_le(glb, jlen);
    emit_u32_le(glb, 0x4E4F534Au);
    glb += json;
    emit_u32_le(glb, blen);
    emit_u32_le(glb, 0x004E4942u);
    glb += bin;
    return glb;
}

static void emit_u16_le(std::string &s, uint16_t v)
{
    s.push_back(static_cast<char>(v & 0xFFu));
    s.push_back(static_cast<char>((v >> 8) & 0xFFu));
}

// Emit one triangle of VEC3 float positions into bin (36 bytes).
static void emit_tri_verts(std::string &bin)
{
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
}

// ─── Group A: non-triangle primitive types rejected ───────────────────────

TEST(reject, gltf_points_only_mesh)
{
    // mode:0 = POINTS — skipped by the loader, no triangles emitted → reject.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
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
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
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

TEST(reject, gltf_triangle_strip_only_mesh)
{
    // mode:5 = TRIANGLE_STRIP — not tessellated by loader, skipped → reject.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0},\"mode\":5}]}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
        "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_tristrip.glb"), make_glb(json, bin));
    assert_rejects(f.path);
}

TEST(reject, gltf_triangle_fan_only_mesh)
{
    // mode:6 = TRIANGLE_FAN — not tessellated by loader, skipped → reject.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0},\"mode\":6}]}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
        "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_trifan.glb"), make_glb(json, bin));
    assert_rejects(f.path);
}

TEST(gltf_valid, default_mode_is_triangles)
{
    // No "mode" field → cgltf defaults to TRIANGLES. Non-indexed, 3 verts → 1 tri.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
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
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
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

// ─── Group C: non-indexed triangles branch ───────────────────────────────

TEST(gltf_valid, non_indexed_triangles_loaded)
{
    // 6 vertices, no indices accessor → non-indexed branch → 2 triangles.
    // Triangle 0: verts 0,1,2 — triangle 1: verts 3,4,5.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0}}]}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":6,"
        "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":72}],"
        "\"buffers\":[{\"byteLength\":72}]}";

    std::string bin;
    emit_tri_verts(bin); // verts 0,1,2
    emit_f32_le(bin, -0.5f);
    emit_f32_le(bin, -0.5f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.5f);
    emit_f32_le(bin, -0.5f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.5f);
    emit_f32_le(bin, 0.0f);

    TmpFile f(tmp_path("rast_nonidx.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    ASSERT_EQ(m.vertices.size(), static_cast<size_t>(6));
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
    ASSERT_EQ(m.triangles[1].v[0], 3u);
    ASSERT_EQ(m.triangles[1].v[1], 4u);
    ASSERT_EQ(m.triangles[1].v[2], 5u);
}

TEST(gltf_valid, non_indexed_partial_triangle_truncated)
{
    // 4 vertices, no indices → i+2<4 passes only for i=0 → 1 triangle,
    // 4th vertex still pushed into vertices[] but no triangle references it.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0}}]}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,"
        "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":48}],"
        "\"buffers\":[{\"byteLength\":48}]}";

    std::string bin;
    emit_tri_verts(bin); // verts 0,1,2
    emit_f32_le(bin, 2.0f);
    emit_f32_le(bin, 2.0f);
    emit_f32_le(bin, 0.0f); // vert 3

    TmpFile f(tmp_path("rast_partial_nonidx.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_EQ(m.vertices.size(), static_cast<size_t>(4));
}

// ─── Group D: indexed truncation ─────────────────────────────────────────

TEST(gltf_valid, indexed_partial_triangle_truncated)
{
    // 6 vertices, 4 indices [0,1,2,3]. i+2<4 passes only for i=0 → 1 triangle.
    // Index 3 is silently dropped.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0},\"indices\":1}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\","
        "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":4,\"type\":\"SCALAR\"}"
        "],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteLength\":72,\"byteOffset\":0},"
        "{\"buffer\":0,\"byteLength\":8,\"byteOffset\":72}"
        "],"
        "\"buffers\":[{\"byteLength\":80}]}";

    std::string bin;
    emit_tri_verts(bin); // verts 0,1,2 (36 bytes)
    emit_f32_le(bin, 0.5f);
    emit_f32_le(bin, 0.5f);
    emit_f32_le(bin, 0.0f); // vert 3
    emit_f32_le(bin, 0.7f);
    emit_f32_le(bin, 0.7f);
    emit_f32_le(bin, 0.0f); // vert 4
    emit_f32_le(bin, 0.9f);
    emit_f32_le(bin, 0.9f);
    emit_f32_le(bin, 0.0f); // vert 5
    emit_u16_le(bin, 0);
    emit_u16_le(bin, 1);
    emit_u16_le(bin, 2);
    emit_u16_le(bin, 3); // 4 indices (8 bytes)

    TmpFile f(tmp_path("rast_partial_idx.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
}
