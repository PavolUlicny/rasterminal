#include "tests/gltf_test_util.h"

#include <cmath>
#include <vector>

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

// REJECTIONS

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

// Group C: non-indexed triangles branch

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

// Group D: indexed truncation

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

// Group E: UV coordinate handling

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

// Group F: Normal handling

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

// Group G: Node transforms

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

// Group I: alpha mode variants

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

// Group J: multi-primitive material assignment

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

// Group K: UINT32 index accessor

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

// Group L: two-node scene merge

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

// Group M: no-scenes rejection

TEST(reject, gltf_no_scenes_array_rejects)
{
    // No "scene" or "scenes" -> data->scene=nullptr, scenes_count=0 ->
    // visit() never called -> vertices empty -> load returns false.
    TmpFile f(tmp_path("rast_noscenes.gltf"), R"({"asset":{"version":"2.0"}})");
    assert_rejects(f.path);
}

// Group O: cgltf_load_buffers / cgltf_validate failures

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

// Group Q: cgltf_parse_file failure

TEST(reject, gltf_corrupt_header_fails_parse)
{
    // Wrong magic bytes → cgltf_parse_file fails before load_buffers/validate.
    TmpFile f(tmp_path("rast_corrupt.glb"), std::string(8, '\xFF'));
    assert_rejects(f.path);
}

// Group R: zero/degenerate normals

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

// Group S: rotation node transform

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

// Group U: null material pointer

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

// Group W: primitive without POSITION attribute

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

// Group X: non-uniform node scale → inverse-transpose for normals

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

// Group Y: negative-determinant node scale → triangle winding flips

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

// Group Z: sparse accessor resolution

TEST(gltf_valid, sparse_accessor_resolves_positions)
{
    // POSITION has no base bufferView (an all-zero implicit base per spec) and a
    // sparse override replacing all 4 of its indices with the actual quad positions.
    // Exercises cgltf_accessor_read_float's sparse-resolution path: before the cgltf
    // refresh, a sparse accessor's read silently returned 0 instead of resolving the
    // override, so every vertex would have landed at the origin.
    std::string bin;
    // Sparse indices: UNSIGNED_BYTE 0,1,2,3 (bufferView 0, 4 bytes).
    bin.push_back(static_cast<char>(0));
    bin.push_back(static_cast<char>(1));
    bin.push_back(static_cast<char>(2));
    bin.push_back(static_cast<char>(3));
    // Sparse values: the quad positions (bufferView 1, 48 bytes, tightly packed).
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, -1.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    // Triangle indices: 0,1,2,0,2,3 as UNSIGNED_SHORT (bufferView 2, 12 bytes).
    emit_u16_le(bin, 0);
    emit_u16_le(bin, 1);
    emit_u16_le(bin, 2);
    emit_u16_le(bin, 0);
    emit_u16_le(bin, 2);
    emit_u16_le(bin, 3);

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"mode\":4}]}],"
        "\"accessors\":["
        "{\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0],"
        "\"sparse\":{\"count\":4,"
        "\"indices\":{\"bufferView\":0,\"componentType\":5121},"
        "\"values\":{\"bufferView\":1}}},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}"
        "],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4},"
        "{\"buffer\":0,\"byteOffset\":4,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":52,\"byteLength\":12}"
        "],"
        "\"buffers\":[{\"byteLength\":64}]}";

    TmpFile f(tmp_path("rast_sparse.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), static_cast<size_t>(4));
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));

    const float expect[4][3] = {
        { -1.0f, -1.0f, 0.0f },
        { 1.0f, -1.0f, 0.0f },
        { 1.0f, 1.0f, 0.0f },
        { -1.0f, 1.0f, 0.0f },
    };
    bool found[4] = { false, false, false, false };
    for (const auto &v : m.vertices)
    {
        for (int c = 0; c < 4; c++)
        {
            if (std::abs(v.pos.x - expect[c][0]) < 1e-5f && std::abs(v.pos.y - expect[c][1]) < 1e-5f &&
                std::abs(v.pos.z - expect[c][2]) < 1e-5f)
            {
                found[c] = true;
            }
        }
    }
    for (bool c : found)
    {
        ASSERT_TRUE(c);
    }
}
