#include "tests/draco_cube_alpha.h"
#include "tests/draco_cube_bitstream.h"
#include "tests/draco_cube_color.h"
#include "tests/draco_tri_uv1.h"
#include "tests/inline_bmp.h"
#include "tests/loader_util.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
    // Frame a JSON chunk + BIN chunk into a binary glTF (GLB) container, padding each
    // chunk to a 4-byte boundary per the spec. Shared by the single-primitive helper
    // below and the bespoke multi-primitive builders in the tests.
    std::string assemble_glb(std::string json, std::string bin)
    {
        while (json.size() % 4 != 0)
        {
            json += ' ';
        }
        while (bin.size() % 4 != 0)
        {
            bin += '\0';
        }
        const auto jlen = static_cast<uint32_t>(json.size());
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
        const std::string &accessors_json,
        const std::string &node_json = R"("mesh":0)"
    )
    {
        // %s slots in order: node / attrs / index_acc(%zu) / draco_attrs / accessors /
        // index_count(%zu) / byteLength(%zu) ×2. node_json defaults to "mesh":0 (no
        // transform); callers inject a "matrix" to test the world-transform path.
        constexpr const char *templ =
            R"({"asset":{"version":"2.0"},)"
            R"("extensionsUsed":["KHR_draco_mesh_compression"],)"
            R"("extensionsRequired":["KHR_draco_mesh_compression"],)"
            R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{%s}],)"
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
            nullptr, 0, templ, node_json.c_str(), attrs_json.c_str(), index_acc_slot, draco_attrs_json.c_str(),
            accessors_json.c_str(), index_count, drc_len, drc_len
        );
        std::string json(static_cast<size_t>(len), '\0');
        std::snprintf(
            json.data(), static_cast<size_t>(len) + 1, templ, node_json.c_str(), attrs_json.c_str(), index_acc_slot,
            draco_attrs_json.c_str(), accessors_json.c_str(), index_count, drc_len, drc_len
        );
        return assemble_glb(json, std::string(reinterpret_cast<const char *>(drc), drc_len));
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

// Multi-attribute decode: the common real-world case.

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

// Position-only Draco prim → compute_normals fallback runs.

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

// Corrupt bitstream → load fails (fail-loud, no silent degradation).

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

// COLOR_0 vertex colours (RGBA fixture → RGB, alpha dropped).

TEST(gltf_draco, decode_color0)
{
    // draco_cube_color: 8 verts, POSITION uid 0 + COLOR_0 uid 1 (4-component RGBA).
    // Known position→colour: (-1,-1,-1)=red, (1,-1,1)=cyan; alpha is dropped because
    // Mesh::vertex_colors is RGB. Accessors: POSITION(VEC3,8) COLOR_0(VEC4,8) idx(36).
    const std::string acc = R"({"componentType":5126,"count":8,"type":"VEC3"},)"
                            R"({"componentType":5126,"count":8,"type":"VEC4"})";
    const std::string glb = make_draco_glb(
        draco_cube_color_drc, draco_cube_color_drc_len, R"({"POSITION":0,"COLOR_0":1})",
        R"({"POSITION":0,"COLOR_0":1})", /*index_acc_slot=*/2, 36, acc
    );
    TmpFile f(tmp_path("rast_draco_color.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.has_vertex_colors);
    // 8 decoded points, but the colour fixture carries no normals → compute_normals
    // facets the 90° cube edges and splits shared verts (to 24); vertex_colors is
    // synced in lockstep, so the parallel-array invariant must still hold.
    ASSERT_EQ(m.vertex_colors.size(), m.vertices.size());
    ASSERT_TRUE(m.vertices.size() >= 8u);

    // Find the two known corners (order-independent: split/vcache reorder vertices, but
    // vertex_colors is remapped in lockstep) and confirm their decoded RGB survived.
    auto colour_at = [&](float x, float y, float z, vec3 &out) -> bool
    {
        for (size_t i = 0; i < m.vertices.size(); i++)
        {
            const vec3 &p = m.vertices[i].pos;
            if (std::abs(p.x - x) < 1e-3f && std::abs(p.y - y) < 1e-3f && std::abs(p.z - z) < 1e-3f)
            {
                out = m.vertex_colors[i];
                return true;
            }
        }
        return false;
    };
    vec3 red{};
    vec3 cyan{};
    ASSERT_TRUE(colour_at(-1.0f, -1.0f, -1.0f, red));
    ASSERT_TRUE(colour_at(1.0f, -1.0f, 1.0f, cyan));
    ASSERT_NEAR(red.x, 1.0f, 1e-2f);
    ASSERT_NEAR(red.y, 0.0f, 1e-2f);
    ASSERT_NEAR(red.z, 0.0f, 1e-2f);
    ASSERT_NEAR(cyan.x, 0.0f, 1e-2f);
    ASSERT_NEAR(cyan.y, 1.0f, 1e-2f);
    ASSERT_NEAR(cyan.z, 1.0f, 1e-2f);
}

// COLOR_0 per-vertex alpha (vec4) survives onto vertex_alpha under alphaMode=BLEND.

namespace
{
    // Bespoke GLB for the alpha fixture: make_draco_glb has no materials support, and the
    // vec4-COLOR_0-under-BLEND gate needs a material. alpha_mode is the only thing that
    // varies between the two tests below, so it is the single parameter here.
    std::string make_draco_alpha_glb(const char *alpha_mode)
    {
        std::string json =
            R"({"asset":{"version":"2.0"},)"
            R"("extensionsUsed":["KHR_draco_mesh_compression"],"extensionsRequired":["KHR_draco_mesh_compression"],)"
            R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
            R"("materials":[{"alphaMode":")" +
            std::string(alpha_mode) +
            R"(","pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1]}}],)"
            R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"COLOR_0":1},"indices":2,"material":0,"mode":4,)"
            R"("extensions":{"KHR_draco_mesh_compression":{"bufferView":0,"attributes":{"POSITION":0,"COLOR_0":1}}}}]}],)"
            R"("accessors":[{"componentType":5126,"count":8,"type":"VEC3"},)"
            R"({"componentType":5126,"count":8,"type":"VEC4"},)"
            R"({"componentType":5125,"count":36,"type":"SCALAR"}],)"
            R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" +
            std::to_string(draco_cube_alpha_drc_len) + R"(}],"buffers":[{"byteLength":)" +
            std::to_string(draco_cube_alpha_drc_len) + R"(}]})";
        std::string bin(reinterpret_cast<const char *>(draco_cube_alpha_drc), draco_cube_alpha_drc_len);
        return assemble_glb(json, bin);
    }
} // namespace

TEST(gltf_draco, color0_alpha_under_blend)
{
    // draco_cube_alpha: POSITION uid 0 + COLOR_0 uid 1 (4-component RGBA, varied alpha).
    // The BLEND material means the per-vertex alpha must reach Mesh::vertex_alpha — the Draco
    // analogue of the accessor-path vec4-COLOR_0-under-BLEND rule (this is the regression the
    // RGB-only decoder used to drop, forcing every alpha to 1.0). Anchors located by position
    // (split/vcache reorder, but vertex_alpha is synced in lockstep): measured at fixture mint.
    const std::string glb = make_draco_alpha_glb("BLEND");
    TmpFile f(tmp_path("rast_draco_alpha.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.has_vertex_alpha);
    ASSERT_EQ(m.vertex_alpha.size(), m.vertices.size());

    auto alpha_at = [&](float x, float y, float z, float &out) -> bool
    {
        for (size_t i = 0; i < m.vertices.size(); i++)
        {
            const vec3 &p = m.vertices[i].pos;
            if (std::abs(p.x - x) < 1e-3f && std::abs(p.y - y) < 1e-3f && std::abs(p.z - z) < 1e-3f)
            {
                out = m.vertex_alpha[i];
                return true;
            }
        }
        return false;
    };
    float a_lo{};
    float a_hi{};
    ASSERT_TRUE(alpha_at(-1.0f, -1.0f, -1.0f, a_lo));
    ASSERT_TRUE(alpha_at(1.0f, 1.0f, 1.0f, a_hi));
    ASSERT_NEAR(a_lo, 0.251f, 2e-2f);
    ASSERT_NEAR(a_hi, 0.753f, 2e-2f);
    // Regression guard: the RGB-only decoder forced both anchors to 1.0.
    ASSERT_TRUE(a_lo < 0.9f);
}

TEST(gltf_draco, color0_alpha_ignored_without_blend)
{
    // Same 4-component COLOR_0 fixture under an OPAQUE material: per spec the base-colour
    // alpha is honoured only for alphaMode=BLEND, so vertex_alpha must stay empty even though
    // the RGB colours still load. Guards the BLEND gate (mirrors the accessor-path semantics).
    const std::string glb = make_draco_alpha_glb("OPAQUE");
    TmpFile f(tmp_path("rast_draco_alpha_opaque.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_FALSE(m.has_vertex_alpha);
    ASSERT_TRUE(m.vertex_alpha.empty());
}

// Node world transform is applied to decoded Draco vertices.

TEST(gltf_draco, node_transform_applies_to_draco)
{
    // Wrap fixture A in a node translated +10 on X (column-major matrix). The cube
    // spans x∈[-1,1]; after the transform every vertex must land in x∈[9,11], proving
    // apply_world_pos runs on the Draco path rather than emitting raw decoded coords.
    const std::string glb = make_draco_glb(
        draco_cube_drc, draco_cube_drc_len, R"({"POSITION":0})", R"({"POSITION":0})", /*index_acc_slot=*/1, 36,
        acc_pos_24, R"("mesh":0,"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,10,0,0,1])"
    );
    TmpFile f(tmp_path("rast_draco_xform.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 24u);
    for (const auto &v : m.vertices)
    {
        ASSERT_TRUE(v.pos.x >= 8.9f);
        ASSERT_TRUE(v.pos.x <= 11.1f);
    }
}

// Mirror (negative-determinant) transform flips triangle winding.

TEST(gltf_draco, mirror_transform_flips_winding)
{
    // A node scaled −1 on X has determinant < 0; the loader swaps v[1]/v[2] so culling
    // stays correct. Load both the plain and mirrored fixture via load_gltf directly
    // (no vcache reorder → deterministic decode order), so the only triangle-index
    // difference is the winding swap.
    const std::string plain_glb = make_draco_glb(
        draco_cube_drc, draco_cube_drc_len, R"({"POSITION":0})", R"({"POSITION":0})", /*index_acc_slot=*/1, 36,
        acc_pos_24
    );
    const std::string mirror_glb = make_draco_glb(
        draco_cube_drc, draco_cube_drc_len, R"({"POSITION":0})", R"({"POSITION":0})", /*index_acc_slot=*/1, 36,
        acc_pos_24, R"("mesh":0,"matrix":[-1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1])"
    );
    TmpFile pf(tmp_path("rast_draco_plain.glb"), plain_glb.data(), plain_glb.size());
    TmpFile mf(tmp_path("rast_draco_mirror.glb"), mirror_glb.data(), mirror_glb.size());
    Mesh plain;
    Mesh mirror;
    ASSERT_TRUE(plain.load_gltf(pf.path));
    ASSERT_TRUE(mirror.load_gltf(mf.path));
    ASSERT_EQ(plain.triangles.size(), 12u);
    ASSERT_EQ(mirror.triangles.size(), 12u);
    // Same decode order; mirror swaps v[1]<->v[2] on every triangle.
    ASSERT_EQ(plain.triangles[0].v[0], mirror.triangles[0].v[0]);
    ASSERT_EQ(plain.triangles[0].v[1], mirror.triangles[0].v[2]);
    ASSERT_EQ(plain.triangles[0].v[2], mirror.triangles[0].v[1]);
}

// Mixed Draco + plain-accessor primitives in one mesh.

TEST(gltf_draco, mixed_draco_and_accessor_primitives)
{
    // prim0: Draco (fixture A, position-only → 24 verts / 12 tris). prim1: a plain
    // non-indexed accessor triangle (3 verts / 1 tri) backed by bufferView 1. Both
    // must contribute — exercises the `continue` after the Draco branch and the
    // accessor path running in the same mesh.
    const size_t draco_len = draco_cube_drc_len;
    const size_t bv1_off = (draco_len + 3u) & ~size_t{ 3 }; // 4-byte align
    std::string bin(reinterpret_cast<const char *>(draco_cube_drc), draco_len);
    while (bin.size() < bv1_off)
    {
        bin += '\0';
    }
    const float tri[9] = { 5, 0, 0, 7, 0, 0, 5, 2, 0 };
    for (float c : tri)
    {
        emit_f32_le(bin, c);
    }

    std::string json =
        R"({"asset":{"version":"2.0"},)"
        R"("extensionsUsed":["KHR_draco_mesh_compression"],"extensionsRequired":["KHR_draco_mesh_compression"],)"
        R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"meshes":[{"primitives":[)"
        R"({"attributes":{"POSITION":0},"indices":1,"mode":4,)"
        R"("extensions":{"KHR_draco_mesh_compression":{"bufferView":0,"attributes":{"POSITION":0}}}},)"
        R"({"attributes":{"POSITION":2},"mode":4}]}],)"
        R"("accessors":[{"componentType":5126,"count":24,"type":"VEC3"},)"
        R"({"componentType":5125,"count":36,"type":"SCALAR"},)"
        R"({"componentType":5126,"count":3,"type":"VEC3","bufferView":1}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" +
        std::to_string(draco_len) + R"(},{"buffer":0,"byteOffset":)" + std::to_string(bv1_off) +
        R"(,"byteLength":36}],"buffers":[{"byteLength":)" + std::to_string(bin.size()) + R"(}]})";

    const std::string glb = assemble_glb(json, bin);
    TmpFile f(tmp_path("rast_draco_mixed.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 27u);  // 24 (Draco) + 3 (accessor)
    ASSERT_EQ(m.triangles.size(), 13u); // 12 (Draco) + 1 (accessor)
}

// Two Draco primitives in one mesh accumulate with correct vert_base offset.

TEST(gltf_draco, multiple_draco_primitives)
{
    // Two identical Draco primitives sharing bufferView 0 (fixture A). The second
    // primitive's triangles must reference the second vertex block (indices ≥ 24),
    // i.e. vert_base is applied. load_gltf direct → deterministic, pre-vcache order.
    std::string json =
        R"({"asset":{"version":"2.0"},)"
        R"("extensionsUsed":["KHR_draco_mesh_compression"],"extensionsRequired":["KHR_draco_mesh_compression"],)"
        R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"meshes":[{"primitives":[)"
        R"({"attributes":{"POSITION":0},"indices":1,"mode":4,)"
        R"("extensions":{"KHR_draco_mesh_compression":{"bufferView":0,"attributes":{"POSITION":0}}}},)"
        R"({"attributes":{"POSITION":0},"indices":1,"mode":4,)"
        R"("extensions":{"KHR_draco_mesh_compression":{"bufferView":0,"attributes":{"POSITION":0}}}}]}],)"
        R"("accessors":[{"componentType":5126,"count":24,"type":"VEC3"},)"
        R"({"componentType":5125,"count":36,"type":"SCALAR"}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" +
        std::to_string(draco_cube_drc_len) + R"(}],"buffers":[{"byteLength":)" + std::to_string(draco_cube_drc_len) +
        R"(}]})";
    std::string bin(reinterpret_cast<const char *>(draco_cube_drc), draco_cube_drc_len);

    const std::string glb = assemble_glb(json, bin);
    TmpFile f(tmp_path("rast_draco_multi.glb"), glb.data(), glb.size());
    Mesh m;
    ASSERT_TRUE(m.load_gltf(f.path));
    ASSERT_EQ(m.vertices.size(), 48u);
    ASSERT_EQ(m.triangles.size(), 24u);
    // Triangles 12..23 belong to the second primitive → must index the [24,48) block.
    ASSERT_TRUE(m.triangles[12].v[0] >= 24u);
}

// Fail-loud guards.

TEST(gltf_draco, missing_position_fails)
{
    // Draco extension declares NORMAL but no POSITION → pos_id stays -1 → load fails.
    // (A standard POSITION accessor is present so cgltf_validate passes; the failure
    // is our loader's guard, not cgltf.)
    const std::string acc = std::string(acc_pos_24) + "," + acc_norm_24;
    const std::string glb = make_draco_glb(
        draco_cube_drc, draco_cube_drc_len, R"({"POSITION":0,"NORMAL":1})", R"({"NORMAL":2})", /*index_acc_slot=*/2, 36,
        acc
    );
    TmpFile f(tmp_path("rast_draco_nopos.glb"), glb.data(), glb.size());
    Mesh m;
    ASSERT_FALSE(m.load_model(f.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/60.0f));
}

TEST(gltf_draco, missing_accessors_array_fails)
{
    // A Draco primitive whose glTF omits the top-level "accessors" array. The loader's
    // unique-id recovery needs data->accessors as the pointer-subtraction base; the
    // null-accessors guard (or an earlier cgltf rejection) must fail the load loudly.
    std::string json =
        R"({"asset":{"version":"2.0"},)"
        R"("extensionsUsed":["KHR_draco_mesh_compression"],"extensionsRequired":["KHR_draco_mesh_compression"],)"
        R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"meshes":[{"primitives":[)"
        R"({"attributes":{"POSITION":0},"mode":4,)"
        R"("extensions":{"KHR_draco_mesh_compression":{"bufferView":0,"attributes":{"POSITION":0}}}}]}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" +
        std::to_string(draco_cube_drc_len) + R"(}],"buffers":[{"byteLength":)" + std::to_string(draco_cube_drc_len) +
        R"(}]})";
    std::string bin(reinterpret_cast<const char *>(draco_cube_drc), draco_cube_drc_len);

    const std::string glb = assemble_glb(json, bin);
    TmpFile f(tmp_path("rast_draco_noacc.glb"), glb.data(), glb.size());
    Mesh m;
    ASSERT_FALSE(m.load_model(f.path, /*ao=*/false, /*n_threads=*/1, /*crease_angle_deg=*/60.0f));
}

// Paths deliberately NOT unit-tested (documented rather than faked):
//   • !dc.buffer_view — reachability through cgltf's CGLTF_PTRFIXUP_REQ is uncertain;
//     a defensive guard.
//   • !cbytes (cgltf_buffer_view_data returns null) — unreachable for inputs that get
//     this far: a GLB's BIN chunk is always populated, and a missing external .bin
//     fails earlier at cgltf_load_buffers.
//   • MAX_EXPANSION bound — needs a bitstream that decodes successfully yet reports
//     implausible counts; not constructible without an adversarial/pathologically-
//     compressible input, and the size==0 sub-branch is unreachable (an empty buffer
//     fails the decode first). Covered by reasoning, not a unit test.
//   • COLOR_0 with >4 components — decode_draco_mesh clamps the ConvertValue out-count
//     to the 4-float buffer, so a malformed attribute declaring up to 255 components
//     can't overflow it. draco_encoder won't emit a >4-component COLOR attribute from
//     standard input (it'd require a hand-crafted generic attribute), so the guard is
//     reasoning-covered rather than fixture-tested; the clamp makes it safe regardless.

// TEXCOORD_1 (second UV set) decoded from the Draco bitstream.

TEST(gltf_draco, decode_texcoord1_second_uv_set)
{
    // The Draco primitive carries TEXCOORD_0 (uid 1) and TEXCOORD_1 (uid 2). A baseColorTexture
    // bound to texCoord:1 references the second set, so the loader keeps uv1 (has_uv1) and decodes
    // it from the bitstream (not a uv0 copy). Bespoke GLB: make_draco_glb has no material slots, so
    // the JSON is hand-built here with an embedded BMP after the Draco buffer view.
    std::string bin(reinterpret_cast<const char *>(kDracoTriUv1), kDracoTriUv1Len);
    while (bin.size() % 4 != 0)
    {
        bin.push_back('\0'); // 4-byte align the image that follows
    }
    const size_t bmp_off = bin.size();
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));

    const std::string json = R"({"asset":{"version":"2.0"},)"
                             R"("extensionsUsed":["KHR_draco_mesh_compression"],)"
                             R"("extensionsRequired":["KHR_draco_mesh_compression"],)"
                             R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
                             R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"TEXCOORD_1":2},)"
                             R"("indices":3,"material":0,"mode":4,"extensions":{"KHR_draco_mesh_compression":)"
                             R"({"bufferView":0,"attributes":{"POSITION":0,"TEXCOORD_0":1,"TEXCOORD_1":2}}}}]}],)"
                             R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0,"texCoord":1}}}],)"
                             R"("textures":[{"source":0}],"images":[{"bufferView":1,"mimeType":"image/bmp"}],)"
                             R"("accessors":[)"
                             R"({"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},)"
                             R"({"componentType":5126,"count":3,"type":"VEC2"},)"
                             R"({"componentType":5126,"count":3,"type":"VEC2"},)"
                             R"({"componentType":5125,"count":3,"type":"SCALAR"}],)"
                             R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" +
                             std::to_string(kDracoTriUv1Len) + R"(},{"buffer":0,"byteOffset":)" +
                             std::to_string(bmp_off) + R"(,"byteLength":)" + std::to_string(sizeof(k1x1_red_bmp)) +
                             R"(}],)" + R"("buffers":[{"byteLength":)" + std::to_string(bin.size()) + R"(}]})";

    TmpFile f(tmp_path("rast_draco_uv1.glb"), assemble_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 3u);
    ASSERT_EQ(m.triangles.size(), 1u);
    ASSERT_TRUE(m.has_uv1);
    ASSERT_EQ(m.uv1.size(), m.vertices.size());
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(static_cast<int>(m.materials[1].diffuse_map.uv_set), 1);
    // uv1 input (0.2,0.3) is stored v-flipped → (0.2,0.7); no uv0 vertex sits there
    // ((0,1)/(1,1)/(0.5,0)), so finding it proves the real second set was decoded, not copied.
    // Loose tolerance for the 16-bit quantization round-trip.
    bool found = false;
    for (const vec2 &t : m.uv1)
    {
        if (std::fabs(t.x - 0.2f) < 0.02f && std::fabs(t.y - 0.7f) < 0.02f)
        {
            found = true;
        }
    }
    ASSERT_TRUE(found);
}
