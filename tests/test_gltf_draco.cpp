#include "draco_cube_bitstream.h"
#include "loader_util.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
    // Build a GLB wrapping the given Draco bitstream as a single mesh primitive.
    // Caller supplies the standard attribute mapping (e.g. "{\"POSITION\":0,\"NORMAL\":1}"),
    // the draco-extension attribute mapping (draco unique-ids), and the accessor list
    // that backs those slots; the indices accessor lands at the end of the accessor
    // list as the highest index. The JSON template lets callers vary the attribute
    // sets to test position-only vs full-attribute paths without duplicating the framing.
    std::string make_draco_glb(
        const unsigned char *drc,
        size_t drc_len,
        const std::string &attrs_json,
        const std::string &draco_attrs_json,
        size_t index_acc_slot,
        size_t index_count,
        const std::string &accessors_json
    )
    {
        // Five %s slots: attrs / index_acc / draco_attrs / accessors / index_count.
        // Two %zu slots at the tail: the buffer-view + buffer byteLength (both = drc_len).
        constexpr const char *templ =
            R"({"asset":{"version":"2.0"},)"
            R"("extensionsUsed":["KHR_draco_mesh_compression"],)"
            R"("extensionsRequired":["KHR_draco_mesh_compression"],)"
            R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
            R"("meshes":[{"primitives":[{"attributes":%s,"indices":%zu,"mode":4,)"
            R"("extensions":{"KHR_draco_mesh_compression":{"bufferView":0,"attributes":%s}}}]}],)"
            R"("accessors":[%s,{"componentType":5125,"count":%zu,"type":"SCALAR"}],)"
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":%zu}],)"
            R"("buffers":[{"byteLength":%zu}]})";
        // Two-pass snprintf: first call (n=0) returns the required length without
        // writing; second call fills a correctly-sized std::string. Avoids a fixed
        // stack buffer that would silently truncate if a future caller grows the
        // accessor JSON beyond the cap.
        const int len = std::snprintf(
            nullptr, 0, templ, attrs_json.c_str(), index_acc_slot, draco_attrs_json.c_str(), accessors_json.c_str(),
            index_count, drc_len, drc_len
        );
        std::string json(static_cast<size_t>(len), '\0');
        std::snprintf(
            json.data(), static_cast<size_t>(len) + 1, templ, attrs_json.c_str(), index_acc_slot,
            draco_attrs_json.c_str(), accessors_json.c_str(), index_count, drc_len, drc_len
        );
        while (json.size() % 4 != 0)
        {
            json += ' ';
        }
        const auto jlen = static_cast<uint32_t>(json.size());

        std::string bin(reinterpret_cast<const char *>(drc), drc_len);
        while (bin.size() % 4 != 0)
        {
            bin += '\0';
        }
        const auto blen = static_cast<uint32_t>(bin.size());

        std::string glb;
        emit_u32_le(glb, 0x46546C67u); // glTF
        emit_u32_le(glb, 2u);
        emit_u32_le(glb, 12u + 8u + jlen + 8u + blen);
        emit_u32_le(glb, jlen);
        emit_u32_le(glb, 0x4E4F534Au); // JSON
        glb += json;
        emit_u32_le(glb, blen);
        emit_u32_le(glb, 0x004E4942u); // BIN
        glb += bin;
        return glb;
    }

    // Per-vertex accessor stubs for the standard glTF attributes. cgltf doesn't
    // require these to point anywhere meaningful for a Draco-only primitive (the
    // decoder reads from the buffer view, not the accessors), but cgltf_validate
    // still walks the accessor list — every accessor needs a valid componentType /
    // count / type and either a buffer view or none (none is fine for Draco).
    constexpr const char *acc_pos_24 =
        R"({"componentType":5126,"count":24,"type":"VEC3","min":[-1,-1,-1],"max":[1,1,1]})";
    constexpr const char *acc_norm_24 = R"({"componentType":5126,"count":24,"type":"VEC3"})";
    constexpr const char *acc_uv_24 = R"({"componentType":5126,"count":24,"type":"VEC2"})";
} // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Multi-attribute decode: the common real-world case.
// ═══════════════════════════════════════════════════════════════════════════

TEST(gltf_draco, decode_position_normal_uv)
{
    // Standard accessors at slots 0/1/2; indices accessor at slot 3.
    // Draco attribute unique-ids encode POSITION->0, NORMAL->2, TEXCOORD_0->1
    // (the encoder-assigned mapping verified at fixture-mint time).
    const std::string acc = std::string(acc_pos_24) + "," + acc_norm_24 + "," + acc_uv_24;
    const std::string glb = make_draco_glb(
        draco_cube_drc, draco_cube_drc_len, R"({"POSITION":0,"NORMAL":1,"TEXCOORD_0":2})",
        R"({"POSITION":0,"NORMAL":2,"TEXCOORD_0":1})", /*index_acc_slot=*/3, 36, acc
    );

    TmpFile f(tmp_path("rast_draco_pnu.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 24u);
    ASSERT_EQ(m.triangles.size(), 12u);
    // Encoded cube spans [-1, 1] on every axis; pick a known vertex and
    // confirm it survived the decode (encoder/decoder are lossless at the
    // default quantization for integer coordinates).
    bool found_corner = false;
    for (const auto &v : m.vertices)
    {
        if (std::abs(v.pos.x + 1.0f) < 1e-3f && std::abs(v.pos.y - 1.0f) < 1e-3f && std::abs(v.pos.z + 1.0f) < 1e-3f)
        {
            found_corner = true;
            break;
        }
    }
    ASSERT_TRUE(found_corner);
    // Normals came from the bitstream: should be unit-length and non-zero.
    for (const auto &v : m.vertices)
    {
        const float len = v.normal.length();
        ASSERT_NEAR(len, 1.0f, 1e-2f);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Position-only Draco prim → compute_normals fallback runs.
// ═══════════════════════════════════════════════════════════════════════════

TEST(gltf_draco, position_only_runs_compute_normals)
{
    // Same bitstream, but the glTF only advertises POSITION. The decoder still
    // gets the normal data (it's in the bitstream), but the loader ignores it
    // because no glTF NORMAL attribute is declared — has_normals stays false,
    // so compute_normals(crease_cos) runs at end-of-load and fills the field.
    const std::string glb = make_draco_glb(
        draco_cube_drc, draco_cube_drc_len, R"({"POSITION":0})", R"({"POSITION":0})", /*index_acc_slot=*/1, 36,
        acc_pos_24
    );
    TmpFile f(tmp_path("rast_draco_pos.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), 12u);
    // compute_normals must have populated unit-length normals.
    for (const auto &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.length(), 1.0f, 1e-2f);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Corrupt bitstream → load fails (fail-loud, no silent degradation).
// ═══════════════════════════════════════════════════════════════════════════

TEST(gltf_draco, corrupt_bitstream_fails_load)
{
    // Truncate the bitstream to ~half its size — Draco's decoder must reject;
    // the loader must propagate that as load failure (per the "fail loud, don't
    // degrade" rule), not silently produce an empty/partial mesh.
    constexpr size_t truncated_len = draco_cube_drc_len / 2;
    const std::string acc = std::string(acc_pos_24) + "," + acc_norm_24 + "," + acc_uv_24;
    const std::string glb = make_draco_glb(
        draco_cube_drc, truncated_len, R"({"POSITION":0,"NORMAL":1,"TEXCOORD_0":2})",
        R"({"POSITION":0,"NORMAL":2,"TEXCOORD_0":1})", /*index_acc_slot=*/3, 36, acc
    );
    TmpFile f(tmp_path("rast_draco_bad.glb"), glb.data(), glb.size());
    Mesh m;
    ASSERT_FALSE(m.load_model(f.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/60.0f));
}
