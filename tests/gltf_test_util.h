#pragma once

// Shared GLB builders for the per-theme glTF loader test files
// (test_gltf.cpp + test_gltf_{topology,vertex_colors,textures,texcoord1,texture_transform}.cpp).
// Header-only: each translation unit gets its own `static` copy (tiny, test-only),
// matching tests/loader_util.h. [[maybe_unused]] silences -Wunused-function in
// translation units that pull in the header but do not call every helper.

#include "tests/loader_util.h"

#include <cstdint>
#include <string>

// Local helpers for constructing minimal GLBs without repeating the
// header-assembly boilerplate.

[[maybe_unused]] static std::string make_glb(const std::string &json_raw, const std::string &bin)
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

[[maybe_unused]] static void emit_u16_le(std::string &s, uint16_t v)
{
    s.push_back(static_cast<char>(v & 0xFFu));
    s.push_back(static_cast<char>((v >> 8) & 0xFFu));
}

// Emit one triangle of VEC3 float positions into bin (36 bytes).
[[maybe_unused]] static void emit_tri_verts(std::string &bin)
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

// Emit three VEC2 float UVs (24 bytes).
[[maybe_unused]] static void emit_uvs(std::string &bin, float u0, float v0, float u1, float v1, float u2, float v2)
{
    emit_f32_le(bin, u0);
    emit_f32_le(bin, v0);
    emit_f32_le(bin, u1);
    emit_f32_le(bin, v1);
    emit_f32_le(bin, u2);
    emit_f32_le(bin, v2);
}
