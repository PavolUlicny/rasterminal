#include "inline_bmp.h"
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
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
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
    // PBR factors retained for the Phong metallic remap; no MR texture here.
    ASSERT_NEAR(mat.metallic, 0.3f, 1e-4f);
    ASSERT_NEAR(mat.roughness, 0.4f, 1e-4f);
    ASSERT_EQ(mat.metallic_roughness_tex, -1);
    ASSERT_TRUE(m.has_metallic); // metallicFactor 0.3 > 0
}

TEST(gltf_valid, double_sided_flag_set)
{
    // Minimal GLB: one triangle, material with doubleSided:true.
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
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

TEST(reject, gltf_triangle_strip_only_mesh)
{
    // mode:5 = TRIANGLE_STRIP — not tessellated by loader, skipped → reject.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
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
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
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

// ─── Group C: non-indexed triangles branch ───────────────────────────────

TEST(gltf_valid, non_indexed_triangles_loaded)
{
    // 6 vertices, no indices accessor → non-indexed branch → 2 triangles.
    // Triangle 0: verts 0,1,2 — triangle 1: verts 3,4,5.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
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
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
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
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
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

// ─── Group E: UV coordinate handling ──────────────────────────────────────

TEST(gltf_valid, uv_v_coordinate_flipped)
{
    // TEXCOORD_0 V=0.3 in file → loaded as 1.0-0.3=0.7.
    // glTF V=0 is top-left; pre-flipping avoids a double-flip in the renderer.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0,\"TEXCOORD_0\":1}}]}],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0},"
                             "{\"buffer\":0,\"byteLength\":24,\"byteOffset\":36}"
                             "],"
                             "\"buffers\":[{\"byteLength\":60}]}";
    std::string bin;
    emit_tri_verts(bin); // 36 bytes
    for (int i = 0; i < 3; ++i)
    {
        emit_f32_le(bin, 0.5f);
        emit_f32_le(bin, 0.3f);
    } // 24 bytes
    TmpFile f(tmp_path("rast_uvflip.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_NEAR(m.vertices[0].uv.x, 0.5f, 1e-5f);
    ASSERT_NEAR(m.vertices[0].uv.y, 0.7f, 1e-5f); // 1.0 - 0.3
}

TEST(gltf_valid, no_uv_accessor_uv_stays_zero)
{
    // uv_acc == nullptr -> Vertex{} zero-init leaves uv at {0,0}.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0}}]}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_nouv.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_NEAR(m.vertices[0].uv.x, 0.0f, 1e-6f);
    ASSERT_NEAR(m.vertices[0].uv.y, 0.0f, 1e-6f);
}

// ─── Group F: Normal handling ──────────────────────────────────────────────

TEST(gltf_valid, normals_loaded_from_accessor_not_recomputed)
{
    // NORMAL accessor carries {1,0,0} on all vertices.  Face normal is (0,0,1),
    // so if compute_normals() ran the x-component would be 0.
    // has_normals=true after the loop -> compute_normals() is skipped.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0,\"NORMAL\":1}}]}],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0},"
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":36}"
                             "],"
                             "\"buffers\":[{\"byteLength\":72}]}";
    std::string bin;
    emit_tri_verts(bin); // 36 bytes positions
    for (int i = 0; i < 3; ++i)
    {
        emit_f32_le(bin, 1.0f);
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 0.0f);
    } // 36 bytes normals all {1,0,0}
    TmpFile f(tmp_path("rast_normacc.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_NEAR(m.vertices[0].normal.x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertices[0].normal.y, 0.0f, 1e-5f);
    ASSERT_NEAR(m.vertices[0].normal.z, 0.0f, 1e-5f);
}

// ─── Group G: Node transforms ──────────────────────────────────────────────

TEST(gltf_valid, node_translation_applied_to_positions)
{
    // translation:[2,0,0] shifts world x by +2:
    //   vertex (-1,-1,0) -> (1,-1,0), vertex (1,-1,0) -> (3,-1,0), (0,1,0) -> (2,1,0).
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0,\"translation\":[2.0,0.0,0.0]}],"
                             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_translate.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_NEAR(m.vertices[0].pos.x, 1.0f, 1e-4f);
    ASSERT_NEAR(m.vertices[0].pos.y, -1.0f, 1e-4f);
    ASSERT_NEAR(m.vertices[1].pos.x, 3.0f, 1e-4f);
    ASSERT_NEAR(m.vertices[2].pos.x, 2.0f, 1e-4f);
}

TEST(gltf_valid, child_node_mesh_visited_recursively)
{
    // Root node (0) has no mesh but references child node (1) which has one.
    // visit() must recurse into node->children.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"children\":[1]},{\"mesh\":0}],"
                             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_child.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_EQ(m.vertices.size(), static_cast<size_t>(3));
}

// ─── Group H: COLOR_0 vertex colors ────────────────────────────────────────

TEST(gltf_valid, color0_sets_has_vertex_colors_and_loads_rgb)
{
    // COLOR_0 VEC4 float: red/green/blue per vertex.
    // has_vertex_colors=true; alpha channel discarded, only RGB stored.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0,\"COLOR_0\":1}}]}],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0},"
                             "{\"buffer\":0,\"byteLength\":48,\"byteOffset\":36}"
                             "],"
                             "\"buffers\":[{\"byteLength\":84}]}";
    std::string bin;
    emit_tri_verts(bin);
    // vertex 0: red, vertex 1: green, vertex 2: blue  (alpha=1 discarded)
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 1.0f);
    TmpFile f(tmp_path("rast_color0.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), static_cast<size_t>(3));
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[1].y, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[2].z, 1.0f, 1e-5f);
}

// ─── Group I: alpha mode variants ──────────────────────────────────────────

TEST(gltf_valid, alpha_mode_blend_leaves_alpha_cutoff_zero)
{
    // alphaMode:BLEND is not supported (rendered as opaque); only MASK triggers
    // a non-zero alpha_cutoff.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"alphaMode\":\"BLEND\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_blend.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].alpha_cutoff, 0.0f, 1e-6f);
}

TEST(gltf_valid, alpha_mode_opaque_leaves_alpha_cutoff_zero)
{
    // alphaMode:OPAQUE is the default; only MASK sets a non-zero cutoff.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"alphaMode\":\"OPAQUE\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_opaque.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].alpha_cutoff, 0.0f, 1e-6f);
}

// ─── Group J: multi-primitive material assignment ───────────────────────────

TEST(gltf_valid, multi_primitive_correct_material_indices)
{
    // Two primitives with different materials -> mat indices assigned in order
    // after the default white material at index 0.
    // triangles[0].material_idx=1 (red), triangles[1].material_idx=2 (green).
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":1},\"material\":1}"
                             "]}],"
                             "\"materials\":["
                             "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1.0,0.0,0.0,1.0]}},"
                             "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.0,1.0,0.0,1.0]}}"
                             "],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0},"
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":36}"
                             "],"
                             "\"buffers\":[{\"byteLength\":72}]}";
    std::string bin;
    emit_tri_verts(bin);
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_multiprim_mat.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    ASSERT_EQ(m.triangles[0].material_idx, 1u);
    ASSERT_EQ(m.triangles[1].material_idx, 2u);
    ASSERT_TRUE(m.materials.size() >= 3);
    ASSERT_NEAR(m.materials[1].diffuse.x, 1.0f, 1e-4f);
    ASSERT_NEAR(m.materials[2].diffuse.y, 1.0f, 1e-4f);
}

// ─── Group K: UINT32 index accessor ────────────────────────────────────────

TEST(gltf_valid, uint32_index_accessor_loaded)
{
    // componentType 5125 = UNSIGNED_INT (32-bit) indices [0,1,2].
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"indices\":1}]}],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0},"
                             "{\"buffer\":0,\"byteLength\":12,\"byteOffset\":36}"
                             "],"
                             "\"buffers\":[{\"byteLength\":48}]}";
    std::string bin;
    emit_tri_verts(bin);
    emit_u32_le(bin, 0u);
    emit_u32_le(bin, 1u);
    emit_u32_le(bin, 2u);
    TmpFile f(tmp_path("rast_u32idx.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[0].v[1], 1u);
    ASSERT_EQ(m.triangles[0].v[2], 2u);
}

// ─── Group L: two-node scene merge ─────────────────────────────────────────

TEST(gltf_valid, two_nodes_merged_with_correct_vertex_base)
{
    // Two scene-root nodes each pointing to a different mesh (3 verts each).
    // Second mesh's triangle indices must be offset by vert_base=3.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],"
                             "\"nodes\":[{\"mesh\":0},{\"mesh\":1}],"
                             "\"meshes\":["
                             "{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]},"
                             "{\"primitives\":[{\"attributes\":{\"POSITION\":1}}]}"
                             "],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0},"
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":36}"
                             "],"
                             "\"buffers\":[{\"byteLength\":72}]}";
    std::string bin;
    emit_tri_verts(bin);
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_twonodes.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), static_cast<size_t>(6));
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    ASSERT_EQ(m.triangles[0].v[0], 0u);
    ASSERT_EQ(m.triangles[1].v[0], 3u);
    ASSERT_EQ(m.triangles[1].v[1], 4u);
    ASSERT_EQ(m.triangles[1].v[2], 5u);
}

// ─── Group M: no-scenes rejection ──────────────────────────────────────────

TEST(reject, gltf_no_scenes_array_rejects)
{
    // No "scene" or "scenes" -> data->scene=nullptr, scenes_count=0 ->
    // visit() never called -> vertices empty -> load returns false.
    TmpFile f(tmp_path("rast_noscenes.gltf"), "{\"asset\":{\"version\":\"2.0\"}}");
    assert_rejects(f.path);
}

// ─── Group N: load_tex path coverage ──────────────────────────────────────────

TEST(gltf_valid, normal_tex_via_buffer_view_sets_normal_tex_index)
{
    // image[0] has bufferView=1 (4 garbage bytes) and no URI.
    // load_tex takes the else-if(img->buffer_view) branch; load_from_memory fails
    // on the garbage data → load_tex returns -1 → mat.normal_tex = -1.
    // Covers both the buffer_view branch in load_tex AND the mat.normal_tex
    // assignment in map_mat.  Mesh geometry still loads.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"normalTexture\":{\"index\":0}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":4}"
                             "],"
                             "\"buffers\":[{\"byteLength\":40}]}";
    std::string bin;
    emit_tri_verts(bin); // 36 bytes geometry
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0); // 4 bytes garbage image data

    TmpFile f(tmp_path("rast_nmap_bv.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].normal_tex < 0);
}

TEST(gltf_valid, diffuse_tex_via_external_uri_load_failure_sets_diffuse_tex_neg)
{
    // image[0] has uri="does_not_exist.tga" (no bufferView).
    // load_tex takes the URI branch; tex.load(dir + uri) fails because the file
    // does not exist → load_tex returns -1 → mat.diffuse_tex = -1.
    // Covers the if(img->uri) branch in load_tex.  Mesh geometry still loads.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"uri\":\"does_not_exist.tga\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);

    TmpFile f(tmp_path("rast_tex_uri.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].diffuse_tex < 0);
}

// ─── Group O: cgltf_load_buffers / cgltf_validate failures ────────────────────

TEST(reject, gltf_missing_external_buffer_fails_load_buffers)
{
    // .gltf file (not GLB) referencing an external buffer "missing_buf.bin" that
    // is never created.  cgltf_parse_file succeeds; cgltf_load_buffers fails trying
    // to open the missing file → load_gltf returns false.
    TmpFile f(
        tmp_path("rast_missingbuf.gltf"), "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                                          "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                                          "{\"POSITION\":0}}]}],"
                                          "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                                          "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                                          "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                                          "\"buffers\":[{\"byteLength\":36,\"uri\":\"missing_buf.bin\"}]}"
    );
    assert_rejects(f.path);
}

TEST(reject, gltf_oversized_buffer_view_fails_validate)
{
    // GLB whose buffer_view claims byteLength=1000 but the buffer is only 36 bytes.
    // cgltf_parse_file succeeds; cgltf_load_buffers succeeds (BIN chunk embedded);
    // cgltf_validate fails: bv->offset + bv->size (1000) > buffer->size (36).
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":1000}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin(36, '\0');
    TmpFile f(tmp_path("rast_bigbv.glb"), make_glb(json, bin));
    assert_rejects(f.path);
}

// ─── Group P: vertex_colors white-fill on second primitive ────────────────────

TEST(gltf_valid, second_primitive_color0_white_fills_first_primitive_verts)
{
    // Two-primitive mesh: prim 0 has no COLOR_0 (3 verts), prim 1 has COLOR_0
    // (3 verts: red, green, blue).  When prim 1 is processed with vert_base=3,
    // vertex_colors.resize(6, {1,1,1}) fills indices 0-5 with white then the
    // loop overwrites [3..5] with actual colors.
    // Covers the vert_base>0 resize path in the color_acc block.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0}},"
                             "{\"attributes\":{\"POSITION\":1,\"COLOR_0\":2}}"
                             "]}],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":48}"
                             "],"
                             "\"buffers\":[{\"byteLength\":120}]}";

    std::string bin;
    emit_tri_verts(bin); // prim 0 positions (36 bytes)
    emit_tri_verts(bin); // prim 1 positions (36 bytes)
    // prim 1 COLOR_0: red, green, blue (VEC4 FLOAT, alpha discarded)
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 1.0f);

    TmpFile f(tmp_path("rast_vcol_whitefill.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), static_cast<size_t>(6));
    // Prim 0 vertices: white-filled because prim 0 had no COLOR_0.
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].y, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].z, 1.0f, 1e-5f);
    // Prim 1 vertices: actual COLOR_0 values.
    ASSERT_NEAR(m.vertex_colors[3].x, 1.0f, 1e-5f); // red
    ASSERT_NEAR(m.vertex_colors[3].y, 0.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[4].y, 1.0f, 1e-5f); // green
    ASSERT_NEAR(m.vertex_colors[5].z, 1.0f, 1e-5f); // blue
}

// ─── Group Q: cgltf_parse_file failure ────────────────────────────────────────

TEST(reject, gltf_corrupt_header_fails_parse)
{
    // Wrong magic bytes → cgltf_parse_file fails before load_buffers/validate.
    TmpFile f(tmp_path("rast_corrupt.glb"), std::string(8, '\xFF'));
    assert_rejects(f.path);
}

// ─── Group R: zero/degenerate normals ─────────────────────────────────────────

TEST(gltf_valid, zero_normals_skip_normalise_and_load_succeeds)
{
    // NORMAL all {0,0,0}: len=0 <= 1e-6 → normalise skipped; normal stays {0,0,0}.
    // has_normals=true so compute_normals() is not called.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0,\"NORMAL\":1}}]}],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0},"
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":36}"
                             "],"
                             "\"buffers\":[{\"byteLength\":72}]}";
    std::string bin;
    emit_tri_verts(bin);
    for (int i = 0; i < 9; i++)
        emit_f32_le(bin, 0.0f); // 3 zero normals
    TmpFile f(tmp_path("rast_zeronorm.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_NEAR(m.vertices[0].normal.x, 0.0f, 1e-6f);
    ASSERT_NEAR(m.vertices[0].normal.y, 0.0f, 1e-6f);
    ASSERT_NEAR(m.vertices[0].normal.z, 0.0f, 1e-6f);
}

// ─── Group S: rotation node transform ────────────────────────────────────────

TEST(gltf_valid, rotation_transform_applied_to_positions_and_normals)
{
    // 90° CCW rotation around Z: quaternion [x,y,z,w]=[0,0,0.7071068,0.7071068].
    // Vertex (-1,-1,0) → (1,-1,0); normal (1,0,0) → (0,1,0) after normalisation.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0,\"rotation\":[0.0,0.0,0.7071068,0.7071068]}],"
                             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1}}]}],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0},"
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":36}"
                             "],"
                             "\"buffers\":[{\"byteLength\":72}]}";
    std::string bin;
    emit_tri_verts(bin); // positions
    for (int i = 0; i < 3; i++)
    {
        emit_f32_le(bin, 1.0f);
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 0.0f);
    } // normals all (1,0,0)
    TmpFile f(tmp_path("rast_rotation.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_NEAR(m.vertices[0].pos.x, 1.0f, 1e-4f);
    ASSERT_NEAR(m.vertices[0].pos.y, -1.0f, 1e-4f);
    ASSERT_NEAR(m.vertices[0].normal.x, 0.0f, 1e-4f);
    ASSERT_NEAR(m.vertices[0].normal.y, 1.0f, 1e-4f);
    ASSERT_NEAR(m.vertices[0].normal.z, 0.0f, 1e-4f);
}

// ─── Group T: first primitive COLOR_0, second has no color ───────────────────

TEST(gltf_valid, first_primitive_color0_second_has_no_color)
{
    // Prim 0: COLOR_0 (red/green/blue). Prim 1: no COLOR_0.
    // vertex_colors filled for prim 0 only (size=3); vertices.size()=6.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0,\"COLOR_0\":2}},"
                             "{\"attributes\":{\"POSITION\":1}}"
                             "]}],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":48}"
                             "],"
                             "\"buffers\":[{\"byteLength\":120}]}";
    std::string bin;
    emit_tri_verts(bin); // prim 0 positions
    emit_tri_verts(bin); // prim 1 positions
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 1.0f);
    TmpFile f(tmp_path("rast_vcol_fwd.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    // optimize_vertex_cache pads partially-initialised vertex_colors to vertices.size()
    // and then remaps, so size == vertices.size() and order may change.
    ASSERT_EQ(m.vertex_colors.size(), m.vertices.size());
    bool found_red = false, found_green = false, found_blue = false;
    for (const auto &c : m.vertex_colors)
    {
        if (c.x > 0.9f && c.y < 0.1f && c.z < 0.1f)
            found_red = true;
        if (c.x < 0.1f && c.y > 0.9f && c.z < 0.1f)
            found_green = true;
        if (c.x < 0.1f && c.y < 0.1f && c.z > 0.9f)
            found_blue = true;
    }
    ASSERT_TRUE(found_red);
    ASSERT_TRUE(found_green);
    ASSERT_TRUE(found_blue);
}

// ─── Group U: null material pointer ──────────────────────────────────────────

TEST(gltf_valid, null_material_uses_default_white_at_index_zero)
{
    // No "material" on primitive → prim.material == nullptr → mat_idx = 0.
    // Only the default Material{} at index 0; no additional material pushed.
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(
        tmp_path("rast_nullmat.glb"), make_glb(
                                          "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                                          "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                                          "{\"POSITION\":0}}]}],"
                                          "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                                          "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                                          "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                                          "\"buffers\":[{\"byteLength\":36}]}",
                                          bin
                                      )
    );
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_EQ(m.triangles[0].material_idx, 0u);
    ASSERT_EQ(m.materials.size(), static_cast<size_t>(1));
    ASSERT_NEAR(m.materials[0].diffuse.x, 1.0f, 1e-5f);
}

// ─── Group V: embedded texture load success ───────────────────────────────────

TEST(gltf_valid, diffuse_tex_via_embedded_buffer_view_loads_successfully)
{
    // image[0] has bufferView=1 pointing to a valid 1×1 BMP embedded in the GLB
    // binary buffer.  load_tex takes the buffer_view branch and load_from_memory
    // succeeds → ok=true → lines 62-64 in mesh_gltf.cpp (store idx, push texture,
    // return idx) are executed.  mat.diffuse_tex is set to a valid (≥0) index.
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin); // 36 bytes geometry at offset 0
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size);

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_embed_tex.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].diffuse_tex >= 0);
    ASSERT_FALSE(m.textures.empty());
}

// ─── Group W: primitive without POSITION attribute ────────────────────────────

TEST(gltf_valid, primitive_without_position_attribute_is_skipped)
{
    // Mesh has two primitives: primitive[0] has only NORMAL (no POSITION) so
    // pos_acc stays null → the "if (!pos_acc) continue;" guard fires (line 139
    // in mesh_gltf.cpp).  Primitive[1] has POSITION → 1 non-indexed triangle.
    std::string bin;
    emit_tri_verts(bin);

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],"
                             "\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"NORMAL\":0}},"  // no POSITION → skipped
                             "{\"attributes\":{\"POSITION\":0}}" // POSITION → 1 triangle
                             "]}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";

    TmpFile f(tmp_path("rast_nopos.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
}
