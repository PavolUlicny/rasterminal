#include "loader_util.h"

// ═══════════════════════════════════════════════════════════════════════════
//  SHIPPED GLTF/GLB MODELS
// ═══════════════════════════════════════════════════════════════════════════

TEST(shipped, gltf_duck)
{
    Mesh m = load_ok("models/gltf/Duck.gltf");
    ASSERT_TRUE(m.triangles.size() > 0);
    ASSERT_TRUE(!m.textures.empty());
    ASSERT_TRUE(m.materials.size() >= 2);
}

TEST(shipped, glb_duck)
{
    Mesh m = load_ok("models/glb/Duck.glb");
    ASSERT_TRUE(m.triangles.size() > 0);
    ASSERT_TRUE(!m.textures.empty());
    ASSERT_TRUE(m.materials.size() >= 2);
}

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

TEST(gltf_valid, single_sided_flag_clear)
{
    // GLB Duck has no doubleSided flag (defaults to false).
    Mesh m = load_ok("models/glb/Duck.glb");
    ASSERT_FALSE(m.has_double_sided);
    for (const auto &mat : m.materials)
        ASSERT_FALSE(mat.double_sided);
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
