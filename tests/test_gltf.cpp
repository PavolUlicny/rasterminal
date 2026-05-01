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

    TmpFile f("/tmp/rast_ds.glb", glb.data(), glb.size());
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
    TmpFile t("/tmp/rast_empty.gltf", "");
    assert_rejects(t.path);
}

TEST(reject, empty_file_glb)
{
    TmpFile t("/tmp/rast_empty.glb", "");
    assert_rejects(t.path);
}
