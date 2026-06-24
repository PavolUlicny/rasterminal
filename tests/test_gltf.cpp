#include "inline_bmp.h"
#include "loader_util.h"

#include <cmath>

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
    {
        json += ' ';
    }
    const auto jlen = static_cast<uint32_t>(json.size());

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
    const auto blen = static_cast<uint32_t>(bin.size());

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
    ASSERT_EQ(mat.mr_map.tex, -1);
    ASSERT_TRUE(m.has_metallic); // metallicFactor 0.3 > 0
    // No emissive: factor defaults to {0,0,0}, no tex, has_emissive false.
    ASSERT_NEAR(mat.emissive.x, 0.0f, 1e-6f);
    ASSERT_NEAR(mat.emissive.y, 0.0f, 1e-6f);
    ASSERT_NEAR(mat.emissive.z, 0.0f, 1e-6f);
    ASSERT_EQ(mat.emissive_map.tex, -1);
    ASSERT_FALSE(m.has_emissive);
}

TEST(gltf_valid, emissive_factor_only_sets_has_emissive)
{
    // Material with only emissiveFactor (no texture).
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                       "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                       "{\"POSITION\":0},\"material\":0}]}],\"materials\":[{\"emissiveFactor\":[1.0,0.5,0.0]}],"
                       "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                       "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                       "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                       "\"buffers\":[{\"byteLength\":36}]}";
    while (json.size() % 4 != 0)
    {
        json += ' ';
    }
    const auto jlen = static_cast<uint32_t>(json.size());

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
    const auto blen = static_cast<uint32_t>(bin.size());

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

    TmpFile f(tmp_path("rast_emissive.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const Material &mat = m.materials[1];
    ASSERT_NEAR(mat.emissive.x, 1.0f, 1e-4f);
    ASSERT_NEAR(mat.emissive.y, 0.5f, 1e-4f);
    ASSERT_NEAR(mat.emissive.z, 0.0f, 1e-4f);
    ASSERT_EQ(mat.emissive_map.tex, -1);
    ASSERT_TRUE(m.has_emissive);
}

// Inline GLB assembly helper local to the emissive-strength tests (make_glb is defined
// further down the file for primitive-coverage tests).
static std::string build_minimal_glb(std::string json)
{
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
    while (json.size() % 4 != 0)
    {
        json += ' ';
    }
    const auto jlen = static_cast<uint32_t>(json.size());
    const auto blen = static_cast<uint32_t>(bin.size());
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

TEST(gltf_valid, emissive_strength_multiplies_factor)
{
    // KHR_materials_emissive_strength: emissive = emissiveFactor × emissiveStrength.
    // Baked into mat.emissive at load (rasterminal has no per-frame intensity uniform).
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"extensionsUsed\":[\"KHR_materials_emissive_strength\"],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"emissiveFactor\":[0.5,0.25,0.1],"
                             "\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":4.0}}}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    TmpFile f(tmp_path("rast_emissive_strength.glb"), build_minimal_glb(json));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const Material &mat = m.materials[1];
    ASSERT_NEAR(mat.emissive.x, 2.0f, 1e-4f);
    ASSERT_NEAR(mat.emissive.y, 1.0f, 1e-4f);
    ASSERT_NEAR(mat.emissive.z, 0.4f, 1e-4f);
    ASSERT_TRUE(m.has_emissive);
}

TEST(gltf_valid, emissive_strength_negative_clamped_to_zero)
{
    // Spec sets `minimum: 0.0` but cgltf doesn't validate; a negative would subtract from lit
    // colour before vec3_to_color. Loader clamps strength to ≥0 — assert the multiply zeros.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"extensionsUsed\":[\"KHR_materials_emissive_strength\"],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"emissiveFactor\":[1.0,1.0,1.0],"
                             "\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":-2.0}}}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    TmpFile f(tmp_path("rast_emissive_neg.glb"), build_minimal_glb(json));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const Material &mat = m.materials[1];
    ASSERT_NEAR(mat.emissive.x, 0.0f, 1e-6f);
    ASSERT_NEAR(mat.emissive.y, 0.0f, 1e-6f);
    ASSERT_NEAR(mat.emissive.z, 0.0f, 1e-6f);
    ASSERT_FALSE(m.has_emissive);
}

TEST(gltf_valid, emissive_factor_infinity_clamped)
{
    // Hostile asset: emissiveFactor=1e400 parses as +Inf via cgltf. Without an upper clamp,
    // +Inf reaches mat.emissive and a per-pixel `Inf * 0` from a zero texel channel produces
    // NaN — vec3_to_color's clamp is not NaN-safe and the uint8_t cast on NaN is UB. Symmetric
    // with the emissive_strength clamp.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"emissiveFactor\":[1e400,1e400,1e400]}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    TmpFile f(tmp_path("rast_emissive_factor_inf.glb"), build_minimal_glb(json));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const Material &mat = m.materials[1];
    ASSERT_TRUE(std::isfinite(mat.emissive.x));
    ASSERT_TRUE(std::isfinite(mat.emissive.y));
    ASSERT_TRUE(std::isfinite(mat.emissive.z));
    ASSERT_NEAR(mat.emissive.x, 1e6f, 1.0f);
}

TEST(gltf_valid, emissive_strength_infinity_clamped)
{
    // Hostile asset: emissiveStrength=1e400 parses as +Inf via cgltf. Without an upper
    // clamp, +Inf reaches mat.emissive, and a per-pixel `Inf * 0` from a zero texel channel
    // produces NaN — vec3_to_color's clamp is not NaN-safe and the uint8_t cast is UB.
    // Upper clamp at 1e6 keeps the result finite (and post-saturation in vec3_to_color it
    // looks identical to any other very-high LDR strength).
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"extensionsUsed\":[\"KHR_materials_emissive_strength\"],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"emissiveFactor\":[1.0,1.0,1.0],"
                             "\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":1e400}}}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    TmpFile f(tmp_path("rast_emissive_inf.glb"), build_minimal_glb(json));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const Material &mat = m.materials[1];
    // Each channel = 1.0 * clamp(1e400 → +Inf, 0, 1e6) = 1e6 — finite, no NaN downstream.
    ASSERT_TRUE(std::isfinite(mat.emissive.x));
    ASSERT_TRUE(std::isfinite(mat.emissive.y));
    ASSERT_TRUE(std::isfinite(mat.emissive.z));
    ASSERT_NEAR(mat.emissive.x, 1e6f, 1.0f);
}

TEST(gltf_valid, emissive_factor_negative_clamped_to_zero)
{
    // Spec sets emissiveFactor `minimum: 0.0` per channel but cgltf doesn't enforce; a
    // negative would subtract from lit colour before vec3_to_color. Loader clamps per
    // channel — mirrors the OBJ Ke clamp and the emissive_strength clamp.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"emissiveFactor\":[0.5,-1.0,-0.25]}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    TmpFile f(tmp_path("rast_emissive_factor_neg.glb"), build_minimal_glb(json));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const Material &mat = m.materials[1];
    ASSERT_NEAR(mat.emissive.x, 0.5f, 1e-4f);
    ASSERT_NEAR(mat.emissive.y, 0.0f, 1e-6f);
    ASSERT_NEAR(mat.emissive.z, 0.0f, 1e-6f);
    ASSERT_TRUE(m.has_emissive); // R survives the clamp
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
    {
        json += ' ';
    }
    const auto jlen = static_cast<uint32_t>(json.size());

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
    const auto blen = static_cast<uint32_t>(bin.size()); // 36

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

TEST(gltf_valid, unlit_extension_sets_flags)
{
    // Material carrying the KHR_materials_unlit extension object → mat.unlit and the
    // mesh-level has_unlit fast-path flag both set. cgltf reads the flag from the
    // material extension directly; extensionsUsed is listed for spec-conformance only.
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
                       R"("extensionsUsed":["KHR_materials_unlit"],)"
                       R"("nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":)"
                       R"({"POSITION":0},"material":0}]}],)"
                       R"("materials":[{"extensions":{"KHR_materials_unlit":{}}}],)"
                       R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,)"
                       R"("type":"VEC3","min":[-1,-1,0],"max":[1,1,0]}],)"
                       R"("bufferViews":[{"buffer":0,"byteLength":36}],)"
                       R"("buffers":[{"byteLength":36}]})";

    TmpFile f(tmp_path("rast_unlit.glb"), build_minimal_glb(json));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_unlit);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].unlit);
}

TEST(gltf_valid, no_unlit_extension_flags_false)
{
    // A plain material (no KHR_materials_unlit) leaves both flags false.
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
                       R"("nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":)"
                       R"({"POSITION":0},"material":0}]}],"materials":[{}],)"
                       R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,)"
                       R"("type":"VEC3","min":[-1,-1,0],"max":[1,1,0]}],)"
                       R"("bufferViews":[{"buffer":0,"byteLength":36}],)"
                       R"("buffers":[{"byteLength":36}]})";

    TmpFile f(tmp_path("rast_no_unlit.glb"), build_minimal_glb(json));
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.has_unlit);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_FALSE(m.materials[1].unlit);
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
    {
        json += ' ';
    }
    const auto jlen = static_cast<uint32_t>(json.size());

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
    const auto blen = static_cast<uint32_t>(bin.size());

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
    {
        json += ' ';
    }
    const auto jlen = static_cast<uint32_t>(json.size());

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
    const auto blen = static_cast<uint32_t>(bin.size());

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
    {
        json += ' ';
    }
    const auto jlen = static_cast<uint32_t>(json.size());
    const auto blen = static_cast<uint32_t>(bin.size());
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

TEST(gltf_valid, partial_color0_split_inherits_color)
{
    // Two primitives, NO normals anywhere (so compute_normals runs). P0 has COLOR_0
    // and a 90 deg fold whose shared edge splits at the default crease; P1 has no
    // COLOR_0 and comes after, so vertex_colors ends up shorter than vertices. The
    // split copies of the colored origin vertex must inherit its color, and
    // vertex_colors must stay the same length as vertices.
    // Regression: with the short-array gate, split copies were padded white instead.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0,\"COLOR_0\":1},\"indices\":2},"
        "{\"attributes\":{\"POSITION\":3}}"
        "]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,1]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC4\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[5,5,5],\"max\":[6,6,5]}"
        "],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteLength\":48,\"byteOffset\":0},"
        "{\"buffer\":0,\"byteLength\":64,\"byteOffset\":48},"
        "{\"buffer\":0,\"byteLength\":12,\"byteOffset\":112},"
        "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":124}"
        "],"
        "\"buffers\":[{\"byteLength\":160}]}";
    std::string bin;
    // P0 positions: origin, +X, +Y, +Z (two tris fold 90 deg across edge 0-1)
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    // P0 colors VEC4: vertex 0 red, 1 green, 2 blue, 3 yellow (alpha discarded)
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
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    // P0 indices: tri A (0,1,2), tri B (1,0,3) — both contain the origin vertex 0
    emit_u16_le(bin, 0);
    emit_u16_le(bin, 1);
    emit_u16_le(bin, 2);
    emit_u16_le(bin, 1);
    emit_u16_le(bin, 0);
    emit_u16_le(bin, 3);
    // P1 positions: a far, uncolored triangle
    emit_f32_le(bin, 5.0f);
    emit_f32_le(bin, 5.0f);
    emit_f32_le(bin, 5.0f);
    emit_f32_le(bin, 6.0f);
    emit_f32_le(bin, 5.0f);
    emit_f32_le(bin, 5.0f);
    emit_f32_le(bin, 5.0f);
    emit_f32_le(bin, 6.0f);
    emit_f32_le(bin, 5.0f);
    TmpFile f(tmp_path("rast_partial_color0.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), m.vertices.size()); // parallel array stays in sync
    int origin_count = 0;
    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        const Vertex &v = m.vertices[i];
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            origin_count++;
            ASSERT_NEAR(m.vertex_colors[i].x, 1.0f, 1e-5f); // red, not white
            ASSERT_NEAR(m.vertex_colors[i].y, 0.0f, 1e-5f);
            ASSERT_NEAR(m.vertex_colors[i].z, 0.0f, 1e-5f);
        }
    }
    ASSERT_EQ(origin_count, 2); // origin vertex split across the hard edge
}

TEST(gltf_valid, partial_color0_uncolored_primitive_first)
{
    // Reverse of the above: the UNCOLORED primitive (P0) comes FIRST, the colored
    // one (P1) second. The colored primitive's resize(vert_base + n_verts, white)
    // is absolute-indexed by vert_base, so it back-fills P0's leading gap with white
    // and writes P1's colors at the correct indices — colors must NOT shift. No
    // normals anywhere so compute_normals runs; the two triangles share nothing so
    // there is no split. Proves the leading/middle-gap ordering, not just tail-gap.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0}},"
        "{\"attributes\":{\"POSITION\":1,\"COLOR_0\":2}}"
        "]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[10,10,10],\"max\":[11,11,10]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}"
        "],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0},"
        "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":36},"
        "{\"buffer\":0,\"byteLength\":48,\"byteOffset\":72}"
        "],"
        "\"buffers\":[{\"byteLength\":120}]}";
    std::string bin;
    // P0 (uncolored) positions: a far triangle at ~(10,10,10)
    emit_f32_le(bin, 10.0f);
    emit_f32_le(bin, 10.0f);
    emit_f32_le(bin, 10.0f);
    emit_f32_le(bin, 11.0f);
    emit_f32_le(bin, 10.0f);
    emit_f32_le(bin, 10.0f);
    emit_f32_le(bin, 10.0f);
    emit_f32_le(bin, 11.0f);
    emit_f32_le(bin, 10.0f);
    // P1 (colored) positions: triangle at the origin
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    // P1 colors VEC4: all green (alpha discarded)
    for (int i = 0; i < 3; i++)
    {
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 1.0f);
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 1.0f);
    }
    TmpFile f(tmp_path("rast_partial_color0_rev.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), m.vertices.size());
    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        const Vertex &v = m.vertices[i];
        const vec3 &c = m.vertex_colors[i];
        if (v.pos.z == 0.0f) // P1 (origin triangle) → green
        {
            ASSERT_NEAR(c.x, 0.0f, 1e-5f);
            ASSERT_NEAR(c.y, 1.0f, 1e-5f);
            ASSERT_NEAR(c.z, 0.0f, 1e-5f);
        }
        else // P0 (far triangle, z=10) → white back-fill
        {
            ASSERT_NEAR(c.x, 1.0f, 1e-5f);
            ASSERT_NEAR(c.y, 1.0f, 1e-5f);
            ASSERT_NEAR(c.z, 1.0f, 1e-5f);
        }
    }
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
    TmpFile f(tmp_path("rast_noscenes.gltf"), R"({"asset":{"version":"2.0"}})");
    assert_rejects(f.path);
}

// ─── Group N: load_tex path coverage ──────────────────────────────────────────

TEST(gltf_valid, normal_tex_via_buffer_view_sets_normal_tex_index)
{
    // image[0] has bufferView=1 (4 garbage bytes) and no URI.
    // load_tex takes the else-if(img->buffer_view) branch; load_from_memory fails
    // on the garbage data → load_tex returns -1 → mat.normal_map.tex = -1.
    // Covers both the buffer_view branch in load_tex AND the mat.normal_map.tex
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
    ASSERT_TRUE(m.materials[1].normal_map.tex < 0);
}

TEST(gltf_valid, zero_emissive_factor_with_texture_skips_decode)
{
    // Spec-literal: emissive = factor × texture. A zero factor means no fragment can sample
    // the texture (do_emissive is gated on factor>0), so the loader skips load_tex entirely.
    // Saves a stb_image_load + permanent RAM footprint for a texture nothing will read.
    // Uses a valid 1×1 BMP so the texture WOULD have decoded successfully if we'd asked.
    std::string bin;
    emit_tri_verts(bin); // 36 bytes geometry
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));
    const auto img_len = static_cast<uint32_t>(sizeof(k1x1_red_bmp));

    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                       "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                       "{\"POSITION\":0},\"material\":0}]}],"
                       "\"materials\":[{\"emissiveTexture\":{\"index\":0}}]," // no emissiveFactor → default {0,0,0}
                       "\"textures\":[{\"source\":0}],"
                       "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                       "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                       "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                       "\"bufferViews\":["
                       "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                       "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":" +
                       std::to_string(img_len) +
                       "}"
                       "],"
                       "\"buffers\":[{\"byteLength\":" +
                       std::to_string(36u + img_len) + "}]}";

    TmpFile f(tmp_path("rast_emissive_skip_decode.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].emissive_map.tex, -1);
    ASSERT_TRUE(m.textures.empty());
    ASSERT_FALSE(m.has_emissive);
}

// ─── Group N+: glTF normalTexture.scale parsing + has_normal_scale gate ────────

TEST(gltf_valid, normal_scale_read_and_sets_has_flag)
{
    // normalTexture.scale=0.5 with a valid embedded BMP → normal_tex >= 0 and the
    // scale is read into mat.normal_scale; the != 1.0 + normal_tex>=0 predicate
    // arms Mesh::has_normal_scale.
    std::string bin;
    emit_tri_verts(bin); // 36 bytes geometry at offset 0
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"normalTexture\":{\"index\":0,\"scale\":0.5}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_nscale.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].normal_scale, 0.5f, 1e-4f);
    ASSERT_TRUE(m.materials[1].normal_map.tex >= 0);
    ASSERT_TRUE(m.has_normal_scale);
}

TEST(gltf_valid, normal_scale_default_one_no_has_flag)
{
    // normalTexture without an explicit scale → cgltf default 1.0 → mat.normal_scale
    // stays 1.0 and has_normal_scale stays false (predicate filters scale != 1.0).
    std::string bin;
    emit_tri_verts(bin);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"normalTexture\":{\"index\":0}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_nscale_default.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].normal_scale, 1.0f, 1e-4f);
    ASSERT_FALSE(m.has_normal_scale);
}

TEST(gltf_valid, normal_scale_with_failed_decode_no_has_flag)
{
    // scale=0.5 is parsed (assignment lives inside if(normal_texture.texture),
    // which is truthy whenever the JSON binds a texture — independent of decode),
    // but the 4 garbage image bytes make load_tex return -1. The has_normal_scale
    // predicate's normal_tex>=0 guard must keep the gate disarmed despite the
    // non-unit scale, so a failed normal-map decode never costs the per-pixel path.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"normalTexture\":{\"index\":0,\"scale\":0.5}}],"
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
    emit_tri_verts(bin);
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0); // 4 bytes garbage image data

    TmpFile f(tmp_path("rast_nscale_faildecode.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].normal_scale, 0.5f, 1e-4f);
    ASSERT_TRUE(m.materials[1].normal_map.tex < 0);
    ASSERT_FALSE(m.has_normal_scale);
}

// ─── glTF occlusionTexture parsing + has_occlusion gate ───────────────────────

TEST(gltf_valid, occlusion_strength_read_and_sets_has_flag)
{
    // occlusionTexture.strength=0.5 with a valid embedded BMP → occlusion_tex >= 0,
    // strength read into mat.occlusion_strength, and Mesh::has_occlusion armed
    // (the gate is occlusion_tex>=0 only — strength does not factor in).
    std::string bin;
    emit_tri_verts(bin);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"occlusionTexture\":{\"index\":0,\"strength\":0.5}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_occl.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].occlusion_strength, 0.5f, 1e-4f);
    ASSERT_TRUE(m.materials[1].occlusion_map.tex >= 0);
    ASSERT_TRUE(m.has_occlusion);
}

TEST(gltf_valid, occlusion_default_strength_one_still_sets_flag)
{
    // occlusionTexture without an explicit strength → cgltf default 1.0. Unlike
    // normalScale, has_occlusion gates on occlusion_tex>=0 alone, so the flag still
    // arms at default strength.
    std::string bin;
    emit_tri_verts(bin);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"occlusionTexture\":{\"index\":0}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_occl_default.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].occlusion_strength, 1.0f, 1e-4f);
    ASSERT_TRUE(m.materials[1].occlusion_map.tex >= 0);
    ASSERT_TRUE(m.has_occlusion);
}

TEST(gltf_valid, occlusion_failed_decode_no_has_flag)
{
    // strength=0.5 is parsed (assignment lives inside if(occlusion_texture.texture)),
    // but 4 garbage image bytes make load_tex return -1. The occlusion_tex>=0 gate
    // must keep has_occlusion disarmed so a failed decode never costs the per-pixel path.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"occlusionTexture\":{\"index\":0,\"strength\":0.5}}],"
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
    emit_tri_verts(bin);
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0);

    TmpFile f(tmp_path("rast_occl_faildecode.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].occlusion_strength, 0.5f, 1e-4f);
    ASSERT_TRUE(m.materials[1].occlusion_map.tex < 0);
    ASSERT_FALSE(m.has_occlusion);
}

TEST(gltf_valid, no_occlusion_texture_keeps_flag_false)
{
    // A material with no occlusionTexture → occlusion_tex stays -1, strength stays
    // 1.0, has_occlusion stays false.
    std::string bin;
    emit_tri_verts(bin);

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";

    TmpFile f(tmp_path("rast_occl_none.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].occlusion_map.tex, -1);
    ASSERT_NEAR(m.materials[1].occlusion_strength, 1.0f, 1e-4f);
    ASSERT_FALSE(m.has_occlusion);
}

TEST(gltf_valid, occlusion_strength_above_one_clamped)
{
    // glTF caps occlusionTexture.strength at 1.0; cgltf does not enforce. An out-of-range
    // strength=2.0 must clamp to 1.0 at load so it cannot drive the per-pixel ao negative.
    std::string bin;
    emit_tri_verts(bin);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"occlusionTexture\":{\"index\":0,\"strength\":2.0}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_occl_clamp.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].occlusion_strength, 1.0f, 1e-4f);
}

TEST(gltf_valid, diffuse_tex_via_external_uri_load_failure_sets_diffuse_tex_neg)
{
    // image[0] has uri="does_not_exist.tga" (no bufferView).
    // load_tex takes the URI branch; tex.load(dir + uri) fails because the file
    // does not exist → load_tex returns -1 → mat.diffuse_map.tex = -1.
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
    ASSERT_TRUE(m.materials[1].diffuse_map.tex < 0);
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
    {
        emit_f32_le(bin, 0.0f); // 3 zero normals
    }
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
        {
            found_red = true;
        }
        if (c.x < 0.1f && c.y > 0.9f && c.z < 0.1f)
        {
            found_green = true;
        }
        if (c.x < 0.1f && c.y < 0.1f && c.z > 0.9f)
        {
            found_blue = true;
        }
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
    // return idx) are executed.  mat.diffuse_map.tex is set to a valid (≥0) index.
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
    ASSERT_TRUE(m.materials[1].diffuse_map.tex >= 0);
    ASSERT_FALSE(m.textures.empty());
}

// ─── Group V+: shared image deduplicates texture ──────────────────────────────

TEST(gltf_valid, shared_image_deduplicates_texture)
{
    // Two primitives both reference material 0, whose baseColorTexture resolves
    // to image 0 (a valid embedded BMP).  map_mat runs once per primitive, so
    // load_tex is invoked twice with the same cgltf_image; dedup must collapse
    // them into a single texture slot rather than decoding twice.
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin); // 36 bytes geometry at offset 0
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size);

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":0},\"material\":0}]}],"
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

    TmpFile f(tmp_path("rast_tex_dedup.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    // Every material that carries a diffuse texture points at the single slot 0.
    bool found = false;
    for (const auto &mat : m.materials)
    {
        if (mat.diffuse_map.tex >= 0)
        {
            ASSERT_EQ(mat.diffuse_map.tex, 0);
            found = true;
        }
    }
    ASSERT_TRUE(found);
}

// ─── Group V++: parallel texture decode (n_threads > 1) ───────────────────────

TEST(gltf_valid, parallel_decode_two_distinct_textures)
{
    // Two images (two bufferViews over the same BMP bytes → two distinct
    // cgltf_image), two materials, two primitives. Loaded with n_threads=4 so the
    // two decodes run on the parallel path. Both must land in distinct valid slots.
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // 36 bytes at offset 0
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // 58 bytes at offset 36

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":0},\"material\":1}]}],"
                             "\"materials\":["
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}},"
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":1}}}],"
                             "\"textures\":[{\"source\":0},{\"source\":1}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"},"
                             "{\"bufferView\":2,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_par_tex.glb"), make_glb(json, bin));
    Mesh m;
    const bool ok = m.load_model(f.path, /*ao=*/false, /*n_threads=*/4);
    ASSERT_TRUE(ok);
    ASSERT_EQ(m.textures.size(), size_t{ 2 });
    ASSERT_TRUE(m.textures[0].valid());
    ASSERT_TRUE(m.textures[1].valid());
    ASSERT_TRUE(m.materials.size() >= 3);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, 0);
    ASSERT_EQ(m.materials[2].diffuse_map.tex, 1);
}

TEST(gltf_valid, parallel_decode_failure_compacts_and_remaps)
{
    // image0 valid (BMP), image1 bad (4 garbage bytes). Loaded with n_threads=4.
    // The failed slot must be compacted out and the referencing material remapped
    // to -1, while the survivor occupies slot 0.
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // 36 at offset 0
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // 58 at offset 36
    // 4 zero bytes at offset 94: too short for any stb_image format probe to
    // accept (PNG needs 8-byte sig, BMP/PSD/GIF need magic, TGA needs ≥18-byte
    // header). Decode reliably fails — exercises the failure-compaction path.
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0);

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":0},\"material\":1}]}],"
                             "\"materials\":["
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}},"
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":1}}}],"
                             "\"textures\":[{\"source\":0},{\"source\":1}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"},"
                             "{\"bufferView\":2,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58},"
                             "{\"buffer\":0,\"byteOffset\":94,\"byteLength\":4}"
                             "],"
                             "\"buffers\":[{\"byteLength\":98}]}";

    TmpFile f(tmp_path("rast_par_fail.glb"), make_glb(json, bin));
    Mesh m;
    const bool ok = m.load_model(f.path, /*ao=*/false, /*n_threads=*/4);
    ASSERT_TRUE(ok);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].valid());
    ASSERT_TRUE(m.materials.size() >= 3);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, 0);
    ASSERT_EQ(m.materials[2].diffuse_map.tex, -1);
}

// ─── Group: sampler wrap modes ────────────────────────────────────────────────

// One-triangle GLB: single material whose baseColorTexture is image 0 (embedded BMP),
// with caller-supplied `textures` and `samplers` JSON segments. `samplers` is the full
// "samplers":[...], segment or "" for none.
static std::string sampler_glb(const std::string &textures, const std::string &samplers)
{
    std::string bin;
    emit_tri_verts(bin);                                                            // 36 @ 0
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp)); // 58 @ 36
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"textures\":" +
        textures +
        ","
        "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}]," +
        samplers +
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":94}]}";
    return make_glb(json, bin);
}

TEST(gltf_sampler, wrap_clamp_repeat_per_axis)
{
    // wrapS=33071 (CLAMP_TO_EDGE), wrapT=10497 (REPEAT) — carried independently per axis.
    TmpFile f(
        tmp_path("rast_wrap_cr.glb"),
        sampler_glb(R"([{"source":0,"sampler":0}])", R"("samplers":[{"wrapS":33071,"wrapT":10497}],)")
    );
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].wrap_s == WrapMode::Clamp);
    ASSERT_TRUE(m.textures[0].wrap_t == WrapMode::Repeat);
}

TEST(gltf_sampler, wrap_mirror)
{
    // wrapS=33648 (MIRRORED_REPEAT).
    TmpFile f(
        tmp_path("rast_wrap_mirror.glb"),
        sampler_glb(R"([{"source":0,"sampler":0}])", R"("samplers":[{"wrapS":33648,"wrapT":33648}],)")
    );
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].wrap_s == WrapMode::Mirror);
    ASSERT_TRUE(m.textures[0].wrap_t == WrapMode::Mirror);
}

TEST(gltf_sampler, no_sampler_defaults_to_repeat)
{
    // Texture with no sampler → both axes Repeat (glTF spec default).
    TmpFile f(tmp_path("rast_wrap_nosamp.glb"), sampler_glb(R"([{"source":0}])", ""));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].wrap_s == WrapMode::Repeat);
    ASSERT_TRUE(m.textures[0].wrap_t == WrapMode::Repeat);
}

TEST(gltf_sampler, omitted_wrap_fields_default_to_repeat)
{
    // Sampler present but wrapS/wrapT omitted → cgltf reports default repeat.
    TmpFile f(tmp_path("rast_wrap_empty.glb"), sampler_glb(R"([{"source":0,"sampler":0}])", R"("samplers":[{}],)"));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].wrap_s == WrapMode::Repeat);
    ASSERT_TRUE(m.textures[0].wrap_t == WrapMode::Repeat);
}

TEST(gltf_sampler, dedup_splits_on_differing_sampler)
{
    // Two textures share image 0 but reference different samplers (CLAMP vs REPEAT). Wrap
    // belongs to the (image, sampler) pair, so they must NOT collapse — two distinct slots.
    std::string bin;
    emit_tri_verts(bin);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":0},\"material\":1}]}],"
                             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}},"
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":1}}}],"
                             "\"textures\":[{\"source\":0,\"sampler\":0},{\"source\":0,\"sampler\":1}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"samplers\":[{\"wrapS\":33071,\"wrapT\":33071},{\"wrapS\":10497,\"wrapT\":10497}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}],"
                             "\"buffers\":[{\"byteLength\":94}]}";
    TmpFile f(tmp_path("rast_wrap_split.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.textures.size(), size_t{ 2 });
    ASSERT_TRUE(m.materials.size() >= 3);
    const Texture &a = m.textures[static_cast<size_t>(m.materials[1].diffuse_map.tex)];
    const Texture &b = m.textures[static_cast<size_t>(m.materials[2].diffuse_map.tex)];
    ASSERT_TRUE(a.wrap_s == WrapMode::Clamp);
    ASSERT_TRUE(b.wrap_s == WrapMode::Repeat);
}

TEST(gltf_sampler, dedup_keeps_identical_sampler)
{
    // Two textures, same image, same sampler → still one decoded slot (no dedup regression).
    std::string bin;
    emit_tri_verts(bin);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":0},\"material\":1}]}],"
                             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}},"
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":1}}}],"
                             "\"textures\":[{\"source\":0,\"sampler\":0},{\"source\":0,\"sampler\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"samplers\":[{\"wrapS\":33071,\"wrapT\":33071}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}],"
                             "\"buffers\":[{\"byteLength\":94}]}";
    TmpFile f(tmp_path("rast_wrap_keep.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].wrap_s == WrapMode::Clamp);
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

// ─── Group X: non-uniform node scale → inverse-transpose for normals ─────────

TEST(gltf_valid, non_uniform_scale_uses_inverse_transpose_for_normals)
{
    // Triangle in local space with surface normal n = (1,0,1)/sqrt(2):
    //   p0=(0,0,0)  p1=(1,0,-1)/sqrt(2)  p2=(0,1,0)
    // Node scale [2,1,1]. The naive upper-3x3 transform gives (2,0,1)/sqrt(5);
    // the correct inverse-transpose gives (1,0,2)/sqrt(5). Geometric check:
    // after scaling, edges (sqrt(2),0,-sqrt(2)/2) and (0,1,0) cross to
    // (sqrt(2)/2, 0, sqrt(2)) ∝ (1,0,2). The supplied normals are identical
    // on every vertex, so any vertex-cache permutation preserves the test.
    const float s = 0.70710678f; // 1/sqrt(2)

    std::string bin;
    // POSITION (accessor 0, bufferView 0, 36 bytes)
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, s);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, -s);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    // NORMAL (accessor 1, bufferView 1, 36 bytes) — all three vertices = (1,0,1)/sqrt(2)
    for (int i = 0; i < 3; i++)
    {
        emit_f32_le(bin, s);
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, s);
    }

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0,\"scale\":[2.0,1.0,1.0]}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1}}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,-1],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,0,1]}"
        "],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36}"
        "],"
        "\"buffers\":[{\"byteLength\":72}]}";

    TmpFile f(tmp_path("rast_nonuniform.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), static_cast<size_t>(3));

    const float inv_sqrt5 = 0.4472136f;
    for (const Vertex &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.x, 1.0f * inv_sqrt5, 1e-4f);
        ASSERT_NEAR(v.normal.y, 0.0f, 1e-4f);
        ASSERT_NEAR(v.normal.z, 2.0f * inv_sqrt5, 1e-4f);
    }
}

// ─── Group Y: negative-determinant node scale → triangle winding flips ───────

TEST(gltf_valid, negative_determinant_flips_triangle_winding)
{
    // Triangle p0=(0,0,0) p1=(1,0,0) p2=(0,1,0); winding gives normal +Z.
    // Node scale [-1,1,1] mirrors X (det = -1). Without a winding flip the
    // world-space triangle's edge cross would point at -Z (backface-culled);
    // the loader must swap v[1]/v[2] so the geometric normal stays at +Z.
    std::string bin;
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0,\"scale\":[-1.0,1.0,1.0]}],"
                             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";

    TmpFile f(tmp_path("rast_mirror.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));

    const Triangle &t = m.triangles[0];
    const vec3 &a = m.vertices[t.v[0]].pos;
    const vec3 &b = m.vertices[t.v[1]].pos;
    const vec3 &c = m.vertices[t.v[2]].pos;
    const vec3 e1 = b - a;
    const vec3 e2 = c - a;
    const vec3 face_n = cross(e1, e2);
    ASSERT_TRUE(face_n.z > 0.0f);
}

TEST(gltf_valid, negative_determinant_flips_winding_for_indexed_primitive)
{
    // Same setup as the non-indexed mirror test, but with an indices accessor —
    // covers the indexed branch in load_gltf (the flip is duplicated in both
    // branches, so the non-indexed test alone wouldn't catch a regression here).
    std::string bin;
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    // Index buffer: 0,1,2 as UNSIGNED_SHORT (componentType 5123); padded to 4-byte align.
    emit_u16_le(bin, 0);
    emit_u16_le(bin, 1);
    emit_u16_le(bin, 2);
    bin.push_back('\0');
    bin.push_back('\0');

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0,\"scale\":[-1.0,1.0,1.0]}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}"
        "],"
        "\"buffers\":[{\"byteLength\":44}]}";

    TmpFile f(tmp_path("rast_mirror_indexed.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));

    const Triangle &t = m.triangles[0];
    const vec3 &a = m.vertices[t.v[0]].pos;
    const vec3 &b = m.vertices[t.v[1]].pos;
    const vec3 &c = m.vertices[t.v[2]].pos;
    const vec3 face_n = cross(b - a, c - a);
    ASSERT_TRUE(face_n.z > 0.0f);
}

// ─── Group: inline data: URI images ───────────────────────────────────────────

// One-triangle GLB whose single material's baseColorTexture is image 0 carrying `uri`.
static std::string data_uri_tex_glb(const std::string &uri)
{
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"uri\":\"" +
                             uri +
                             "\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    return make_glb(json, bin);
}

// Build the data: URI for the given bytes with the given mediatype, base64-encoded.
static std::string data_uri(const std::string &mediatype, const uint8_t *bytes, size_t n)
{
    return "data:" + mediatype + ";base64," + b64encode(bytes, n);
}

TEST(gltf_data_uri, base64_bmp_decodes)
{
    // Happy path: a padded-base64 BMP data URI decodes through the stb route. Proves the
    // base64 round-trip (k1x1_red_bmp is 58 bytes -> padded encoding) and content-sniff.
    const std::string uri = data_uri("image/bmp", k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile f(tmp_path("rast_datauri_bmp.glb"), data_uri_tex_glb(uri));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0);
    ASSERT_TRUE(idx < static_cast<int>(m.textures.size()));
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(t.width, 1);
    ASSERT_EQ(t.height, 1);
}

TEST(gltf_data_uri, no_base64_marker_dropped)
{
    // data:<mediatype>,<raw> without ";base64" is not handled -> slot dropped, load ok.
    TmpFile f(tmp_path("rast_datauri_nomarker.glb"), data_uri_tex_glb("data:image/bmp,Qk0"));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1);
}

TEST(gltf_data_uri, invalid_alphabet_dropped)
{
    // Payload has no base64-alphabet chars -> out_size 0 -> slot dropped, load ok.
    TmpFile f(tmp_path("rast_datauri_badalpha.glb"), data_uri_tex_glb("data:image/bmp;base64,@@@@"));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1);
}

TEST(gltf_data_uri, empty_payload_dropped)
{
    // Comma is the last char -> empty payload -> out_size 0 -> slot dropped, load ok.
    TmpFile f(tmp_path("rast_datauri_empty.glb"), data_uri_tex_glb("data:image/bmp;base64,"));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1);
}

TEST(gltf_data_uri, valid_base64_non_image_dropped)
{
    // Well-formed base64 that decodes to non-image bytes: the base64 step succeeds but the
    // content-sniff / stb decode rejects it -> slot dropped, load ok.
    const uint8_t hello[] = { 'h', 'e', 'l', 'l', 'o' };
    const std::string uri = data_uri("application/octet-stream", hello, sizeof(hello));
    TmpFile f(tmp_path("rast_datauri_nonimage.glb"), data_uri_tex_glb(uri));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1);
}

TEST(gltf_data_uri, no_comma_dropped)
{
    // No comma at all -> strchr returns null -> slot dropped without UB, load ok.
    TmpFile f(tmp_path("rast_datauri_nocomma.glb"), data_uri_tex_glb("data:image/bmp;base64"));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1);
}

TEST(gltf_data_uri, short_metadata_no_underread)
{
    // Comma at index 5: the `comma - uri >= 7` guard rejects before the strncmp would read
    // before the start of the string -> slot dropped, load ok.
    TmpFile f(tmp_path("rast_datauri_short.glb"), data_uri_tex_glb("data:,x"));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1);
}

TEST(gltf_data_uri, shared_data_uri_dedup)
{
    // Two materials, two textures, both pointing at image 0 (one data: URI). Both load_tex
    // calls resolve the same cgltf_image* -> TexKey dedup -> one decode, one texture slot,
    // and both materials reference it.
    const std::string uri = data_uri("image/bmp", k1x1_red_bmp, sizeof(k1x1_red_bmp));
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":0},\"material\":1}]}],"
                             "\"materials\":["
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}},"
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":1}}}],"
                             "\"textures\":[{\"source\":0},{\"source\":0}],"
                             "\"images\":[{\"uri\":\"" +
                             uri +
                             "\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_datauri_dedup.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 3);
    const int a = m.materials[1].diffuse_map.tex;
    const int b = m.materials[2].diffuse_map.tex;
    ASSERT_TRUE(a >= 0);
    ASSERT_EQ(a, b);
    ASSERT_EQ(m.textures.size(), static_cast<size_t>(1));
}

// ─── Group T1: TEXCOORD_1 (second UV set) ─────────────────────────────────────

// Emit three VEC2 float UVs (24 bytes).
static void emit_uvs(std::string &bin, float u0, float v0, float u1, float v1, float u2, float v2)
{
    emit_f32_le(bin, u0);
    emit_f32_le(bin, v0);
    emit_f32_le(bin, u1);
    emit_f32_le(bin, v1);
    emit_f32_le(bin, u2);
    emit_f32_le(bin, v2);
}

// A baseColorTexture bound to texCoord:1 with a TEXCOORD_1 accessor present →
// has_uv1 set, uv1 length-matched, the binding records uv_set 1, and the stored
// uv1 carries the real (v-flipped) second-set values rather than a uv0 copy.
TEST(gltf_valid, texcoord1_referenced_populates_uv1)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // POSITION  36 @ 0
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);                  // TEXCOORD_0 24 @ 36
    emit_uvs(bin, 0.2f, 0.3f, 0.8f, 0.3f, 0.5f, 0.9f);                  // TEXCOORD_1 24 @ 60
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 84

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1,\"TEXCOORD_1\":2},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":1}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":3,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":84,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":142}]}";

    TmpFile f(tmp_path("rast_texcoord1.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_uv1);
    ASSERT_EQ(m.uv1.size(), m.vertices.size());
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(static_cast<int>(m.materials[1].diffuse_map.uv_set), 1);
    // The single triangle is not reordered (optimize_vertex_cache skips <2 tris, no creasing),
    // so vertex 0 keeps its input attributes. uv1 input (0.2,0.3) is stored v-flipped → (0.2,0.7),
    // distinct from uv0 (0.0, flipped 1.0) — proving the real second set was read, not a uv0 copy.
    ASSERT_TRUE(std::fabs(m.uv1[0].x - 0.2f) < 0.01f);
    ASSERT_TRUE(std::fabs(m.uv1[0].y - 0.7f) < 0.01f);
}

// TEXCOORD_1 present but no texture references it (texCoord defaults to 0) → uv1 is
// dropped (read-then-drop) and has_uv1 stays false; the binding records set 0.
TEST(gltf_valid, texcoord1_present_but_unreferenced_is_dropped)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
    emit_uvs(bin, 0.2f, 0.3f, 0.8f, 0.3f, 0.5f, 0.9f);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size);

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1,\"TEXCOORD_1\":2},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":3,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":84,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":142}]}";

    TmpFile f(tmp_path("rast_texcoord1_unused.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.has_uv1);
    ASSERT_TRUE(m.uv1.empty());
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(static_cast<int>(m.materials[1].diffuse_map.uv_set), 0);
}

// texCoord:1 referenced but the primitive provides no TEXCOORD_1 → degrade to set 0
// (runtime-loader convention): load still succeeds, has_uv1 false, uv_set forced to 0.
TEST(gltf_valid, texcoord1_referenced_but_absent_degrades_to_zero)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // POSITION  36 @ 0
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);                  // TEXCOORD_0 24 @ 36
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 60

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":1}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":2,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":118}]}";

    TmpFile f(tmp_path("rast_texcoord1_absent.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.has_uv1);
    ASSERT_TRUE(m.uv1.empty());
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(static_cast<int>(m.materials[1].diffuse_map.uv_set), 0);
}

// texCoord:2 (>= 2) is unsupported and capped to set 0, even with TEXCOORD_1 present.
// Nothing then references set 1, so uv1 is dropped and the binding records set 0.
TEST(gltf_valid, texcoord_ge_2_caps_to_zero)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
    emit_uvs(bin, 0.2f, 0.3f, 0.8f, 0.3f, 0.5f, 0.9f);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size);

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1,\"TEXCOORD_1\":2},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":2}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":3,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":84,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":142}]}";

    TmpFile f(tmp_path("rast_texcoord2.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.has_uv1);
    ASSERT_TRUE(m.uv1.empty());
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(static_cast<int>(m.materials[1].diffuse_map.uv_set), 0);
}

// A two-triangle quad (>= 2 tris, so optimize_vertex_cache actually runs and remaps the
// vertex arrays) must keep uv1 paired with each vertex through the remap. Locate the vertex
// by position post-remap and confirm its uv1 survived correctly.
TEST(gltf_valid, texcoord1_survives_vertex_cache_remap)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    // 4 positions (quad corners): VEC3 ×4 = 48 bytes @ 0
    const float quad[4][3] = { { -1, -1, 0 }, { 1, -1, 0 }, { 1, 1, 0 }, { -1, 1, 0 } };
    for (const auto &p : quad)
    {
        emit_f32_le(bin, p[0]);
        emit_f32_le(bin, p[1]);
        emit_f32_le(bin, p[2]);
    }
    // uv0 ×4 = 32 bytes @ 48
    const float uv0[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (const auto &t : uv0)
    {
        emit_f32_le(bin, t[0]);
        emit_f32_le(bin, t[1]);
    }
    // uv1 ×4 = 32 bytes @ 80 (distinct from uv0 so a mix-up is visible)
    const float uv1[4][2] = { { 0.25f, 0.0f }, { 0.75f, 0.0f }, { 0.75f, 0.5f }, { 0.25f, 0.5f } };
    for (const auto &t : uv1)
    {
        emit_f32_le(bin, t[0]);
        emit_f32_le(bin, t[1]);
    }
    // indices: two tris (0,1,2)(0,2,3) — 6 × uint16 = 12 bytes @ 112
    for (int i : { 0, 1, 2, 0, 2, 3 })
    {
        emit_u16_le(bin, static_cast<uint16_t>(i));
    }
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // 58 @ 124

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1,\"TEXCOORD_1\":2},\"indices\":3,\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":1}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":32},"
        "{\"buffer\":0,\"byteOffset\":80,\"byteLength\":32},"
        "{\"buffer\":0,\"byteOffset\":112,\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":124,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":182}]}";

    TmpFile f(tmp_path("rast_texcoord1_quad.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_uv1);
    ASSERT_EQ(m.uv1.size(), m.vertices.size());
    // Find the corner at (1,-1,0): its uv1 was file (0.75,0.0) → stored v-flipped (0.75,1.0).
    bool found = false;
    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        const vec3 &p = m.vertices[i].pos;
        if (std::fabs(p.x - 1.0f) < 0.01f && std::fabs(p.y + 1.0f) < 0.01f)
        {
            ASSERT_TRUE(std::fabs(m.uv1[i].x - 0.75f) < 0.01f);
            ASSERT_TRUE(std::fabs(m.uv1[i].y - 1.0f) < 0.01f);
            found = true;
        }
    }
    ASSERT_TRUE(found);
}

// Partial coverage across primitives: prim0 (no TEXCOORD_1), prim1 (TEXCOORD_1), prim2 (no
// TEXCOORD_1), all referencing one material whose baseColorTexture is on texCoord:1. The lazy
// builder must back-fill prim0's vertices (from their uv0) when prim1 first provides a second
// set, push prim1's real values, and pad prim2 — keeping uv1 length-matched. Verts lacking a
// real second set degrade to uv0; the one primitive that has it keeps distinct values.
TEST(gltf_valid, texcoord1_partial_coverage_backfills_and_pads)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // prim0 POS  36 @ 0
    emit_uvs(bin, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f);                  // prim0 uv0  24 @ 36
    emit_tri_verts(bin);                                                // prim1 POS  36 @ 60
    emit_uvs(bin, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f);                  // prim1 uv0  24 @ 96
    emit_uvs(bin, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f);                  // prim1 uv1  24 @ 120
    emit_tri_verts(bin);                                                // prim2 POS  36 @ 144
    emit_uvs(bin, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f);                  // prim2 uv0  24 @ 180
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 204

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},\"material\":0},"
        "{\"attributes\":{\"POSITION\":2,\"TEXCOORD_0\":3,\"TEXCOORD_1\":4},\"material\":0},"
        "{\"attributes\":{\"POSITION\":5,\"TEXCOORD_0\":6},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":1}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":7,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":5,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":6,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":120,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":144,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":180,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":204,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":262}]}";

    TmpFile f(tmp_path("rast_texcoord1_partial.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_uv1);
    ASSERT_EQ(m.uv1.size(), m.vertices.size());
    // 9 vertices: 6 degrade (uv1 == own uv0), 3 carry the real second set (uv1 != uv0).
    int n_equal = 0;
    int n_diff = 0;
    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        const vec2 d{ m.uv1[i].x - m.vertices[i].uv.x, m.uv1[i].y - m.vertices[i].uv.y };
        if (std::fabs(d.x) < 0.01f && std::fabs(d.y) < 0.01f)
        {
            n_equal++;
        }
        else
        {
            n_diff++;
        }
    }
    ASSERT_EQ(n_equal, 6);
    ASSERT_EQ(n_diff, 3);
}

// Tangents must be built from the UV set the normal map samples (glTF spec). Geometry: a
// triangle in the XY plane (normal +Z) with uv0 running U along +X but uv1 running U along
// +Y. With the normalTexture bound to texCoord:1, compute_tangents must use uv1, yielding a
// +Y-dominant tangent (a uv0-derived tangent would be +X-dominant).
TEST(gltf_valid, normal_map_texcoord1_drives_tangents)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    // POSITION (0,0,0)(1,0,0)(0,1,0) — 36 @ 0
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    // NORMAL (0,0,1)×3 — 36 @ 36
    for (int i = 0; i < 3; i++)
    {
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 1.0f);
    }
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);                  // uv0: U along +X — 24 @ 72
    emit_uvs(bin, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f);                  // uv1: U along +Y — 24 @ 96
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 120

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"TEXCOORD_1\":3},\"material\":0}]}],"
        "\"materials\":[{\"normalTexture\":{\"index\":0,\"texCoord\":1}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":120,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":178}]}";

    TmpFile f(tmp_path("rast_texcoord1_tangent.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_uv1);
    ASSERT_EQ(static_cast<int>(m.materials[1].normal_map.uv_set), 1);
    ASSERT_EQ(m.tangents.size(), m.vertices.size());
    // uv1-derived tangent is +Y; a uv0-derived one would be +X. Assert +Y dominance.
    for (const vec3 &t : m.tangents)
    {
        if (std::fabs(t.y) <= std::fabs(t.x))
        {
            ASSERT_FAIL("tangent should be +Y-dominant (built from TEXCOORD_1), got X-dominant");
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  KHR_texture_transform
// ═══════════════════════════════════════════════════════════════════════════

// baseColorTexture with KHR_texture_transform (offset + scale, no rotation) → the slot bakes
// the flip-folded 2x3 affine. With rotation 0: c=1,s=0, so for offset (ox,oy) and scale
// (sx,sy) the coefficients are {sx,0,ox, 0,sy,1-oy-sy} — exercising the v-flip terms (a02 has
// no rotation contribution here, a12 = 1 - oy - sy carries the fold).
TEST(gltf_valid, texture_transform_bakes_affine)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // POSITION  36 @ 0
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);                  // TEXCOORD_0 24 @ 36
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 60

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1},\"material\":0}]}],"
        "\"extensionsUsed\":[\"KHR_texture_transform\"],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{\"offset\":[0.1,0.2],\"scale\":[2.0,3.0]}}}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":2,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":118}]}";

    TmpFile f(tmp_path("rast_tex_transform.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const TexSlot &d = m.materials[1].diffuse_map;
    ASSERT_TRUE(d.has_transform);
    const float expect[6] = { 2.0f, 0.0f, 0.1f, 0.0f, 3.0f, 1.0f - 0.2f - 3.0f };
    for (int i = 0; i < 6; i++)
    {
        if (std::fabs(d.t[i] - expect[i]) > 1e-5f)
        {
            ASSERT_FAIL("baked KHR_texture_transform affine coefficient mismatch");
        }
    }
}

// KHR_texture_transform's own texCoord overrides textureInfo.texCoord (spec). baseColorTexture
// declares texCoord 0 but the transform declares texCoord 1, and TEXCOORD_1 is present → the
// slot resolves to set 1.
TEST(gltf_valid, texture_transform_texcoord_override)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // POSITION  36 @ 0
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);                  // TEXCOORD_0 24 @ 36
    emit_uvs(bin, 0.2f, 0.3f, 0.8f, 0.3f, 0.5f, 0.9f);                  // TEXCOORD_1 24 @ 60
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 84

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1,\"TEXCOORD_1\":2},\"material\":0}]}],"
        "\"extensionsUsed\":[\"KHR_texture_transform\"],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{\"texCoord\":1}}}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":3,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":84,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":142}]}";

    TmpFile f(tmp_path("rast_tex_transform_override.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_uv1);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].diffuse_map.has_transform);
    ASSERT_EQ(static_cast<int>(m.materials[1].diffuse_map.uv_set), 1);
}

// A baseColorTexture with no KHR_texture_transform → has_transform false and t stays identity,
// so apply_tex_transform is a no-op even if (incorrectly) invoked.
TEST(gltf_valid, texture_transform_absent_is_identity)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size);

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":2,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":118}]}";

    TmpFile f(tmp_path("rast_tex_transform_absent.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const TexSlot &d = m.materials[1].diffuse_map;
    ASSERT_FALSE(d.has_transform);
    const float identity[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    for (int i = 0; i < 6; i++)
    {
        if (std::fabs(d.t[i] - identity[i]) > 1e-6f)
        {
            ASSERT_FAIL("no-transform slot must keep an identity affine");
        }
    }
}

// Keystone correctness test for the v-flip handling, independent of bake_transform's own
// derivation. Author a non-trivial offset+rotation+scale, load it (running the real bake), then
// for several UV points assert that our full pipeline — store v-flip (uv.y = 1 - v_gltf) → baked
// affine → the sampler's internal v-flip — lands on the EXACT image pixel the glTF transform with
// NEGATED rotation (Tr·R(-θ)·S, the flipY convention for v-flipped textures) intends. The expected
// value encodes that spec-with-negated-rotation directly; the implementation reaches it by a
// different route, so a wrong rotation sign (or any sign slip in the affine) makes expected !=
// actual. This is the check that catches a bad derivation — the exact-coefficient test above only
// pins the formula against itself, and would happily accept the un-negated (red-marker) bake.
TEST(gltf_valid, texture_transform_matches_gltf_spec_sampling)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // POSITION  36 @ 0
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);                  // TEXCOORD_0 24 @ 36
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 60

    const float ox = 0.1f, oy = 0.2f, rot = 0.5f, sx = 1.5f, sy = 2.0f;
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1},\"material\":0}]}],"
        "\"extensionsUsed\":[\"KHR_texture_transform\"],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{\"offset\":[0.1,0.2],\"rotation\":0.5,\"scale\":[1.5,2.0]}}}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":2,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":118}]}";

    TmpFile f(tmp_path("rast_tex_transform_spec.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const TexSlot &d = m.materials[1].diffuse_map;
    ASSERT_TRUE(d.has_transform);

    const float c = std::cos(rot), s = std::sin(rot);
    const vec2 pts[] = { { 0.0f, 0.0f }, { 0.3f, 0.2f }, { 1.0f, 1.0f }, { 0.7f, 0.4f } };
    for (const vec2 &p : pts)
    {
        const float u_g = p.x, v_g = p.y;
        // The image texel our pipeline reads (sampler does v = 1 - feed.y, so the v-down sampling
        // coordinate is (feed.x, 1 - feed.y)) must equal the glTF transform with the rotation angle
        // NEGATED: Tr · R(-θ) · S · g. That -θ is the flipY convention every glTF reference renderer
        // uses for v-flipped textures; the naive +θ (R(+θ)) points the TextureTransformTest arrow at
        // the red "opposite direction" marker. Only the sin terms flip sign, so offset and
        // axis-aligned scale are identical either way — this isolates the rotation handedness.
        const float u_exp = (c * sx * u_g) + (s * sy * v_g) + ox;
        const float v_exp = (-s * sx * u_g) + (c * sy * v_g) + oy;
        const vec2 feed = apply_tex_transform(d, vec2{ u_g, 1.0f - v_g });
        const float u_act = feed.x;
        const float v_act = 1.0f - feed.y;
        if (std::fabs(u_act - u_exp) > 1e-4f || std::fabs(v_act - v_exp) > 1e-4f)
        {
            ASSERT_FAIL("baked transform does not reproduce glTF Tr·R(-rot)·S sampling");
        }
    }
}

// End-to-end rotation-handedness guard. The keystone above only checks apply_tex_transform's
// coordinates against a 1x1 texture; it cannot see which way a rotation actually turns the image.
// This drives the REAL pipeline: load a pure +rotation (real bake_transform) → apply the baked
// affine → sample a real multi-texel texture through the REAL Texture::sample, and assert the
// rotated brightness gradient runs the direction the official Khronos TextureTransformTest render
// shows. A regression that drops the v-flip rotation negation flips the cross term (t[3]) sign and
// reverses this inequality — the exact failure that pointed the arrow at the "opposite direction"
// marker. Robust by construction: a vertical gradient + an inequality assertion (no bilinear/wrap
// edge fragility), and the sampled colours come from this test's own texture, not the loaded glTF's.
TEST(gltf_valid, texture_transform_rotation_handedness_end_to_end)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // POSITION  36 @ 0
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);                  // TEXCOORD_0 24 @ 36
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 60

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1},\"material\":0}]}],"
        "\"extensionsUsed\":[\"KHR_texture_transform\"],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{\"rotation\":0.5}}}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":2,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":118}]}";

    TmpFile f(tmp_path("rast_tex_transform_handed.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const TexSlot &d = m.materials[1].diffuse_map;
    ASSERT_TRUE(d.has_transform);

    // Vertical gradient: top row black, bottom row white. Sampling depends only on the feed's v.
    Texture grad;
    grad.width = 1;
    grad.height = 2;
    grad.pixels = { 0, 0, 0, 255, 255, 255, 255, 255 };

    // Two stored UVs differing only in u. Under the authored +rotation the cross term t[3] couples
    // u into the sampled row, so the brightness must change monotonically with u. Both feeds land
    // inside [0,1] (no wrap), so the only thing under test is the rotation's direction.
    auto brightness = [&](float su)
    {
        const vec2 feed = apply_tex_transform(d, vec2{ su, 0.5f });
        return grad.sample_rgb(feed.x, feed.y).x;
    };
    const float lo_u = brightness(0.2f);
    const float hi_u = brightness(0.8f);

    // Khronos-correct sense (t[3] = +sin θ): larger u samples a lower image row → darker. The
    // pre-fix bug negated t[3], making larger u brighter. Margin guards against noise, not sign.
    if (hi_u >= lo_u - 0.15f)
    {
        ASSERT_FAIL("rotation handedness reversed: +rotation must darken with increasing u "
                    "(v-flip rotation negation dropped — see bake_transform)");
    }
}

// A transform that specifies only offset → cgltf defaults scale to [1,1] (not [0,0]), so the
// texture is shifted, not collapsed. Guards the cgltf default our bake depends on across bumps.
TEST(gltf_valid, texture_transform_defaults_scale_to_one)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size);

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1},\"material\":0}]}],"
        "\"extensionsUsed\":[\"KHR_texture_transform\"],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{\"offset\":[0.25,0.0]}}}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":2,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":118}]}";

    TmpFile f(tmp_path("rast_tex_transform_defscale.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const TexSlot &d = m.materials[1].diffuse_map;
    ASSERT_TRUE(d.has_transform);
    // scale 1, rotation 0, offset.u 0.25 → {1,0,0.25, 0,1,0} (a12 = 1 - 0 - 1 = 0).
    const float expect[6] = { 1.0f, 0.0f, 0.25f, 0.0f, 1.0f, 0.0f };
    for (int i = 0; i < 6; i++)
    {
        if (std::fabs(d.t[i] - expect[i]) > 1e-5f)
        {
            ASSERT_FAIL("offset-only transform must keep unit scale (cgltf default), not collapse");
        }
    }
}
