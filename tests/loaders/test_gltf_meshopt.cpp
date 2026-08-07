#include "tests/loader_util.h"

#include "meshoptimizer.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    // Frame a JSON chunk + BIN chunk into a GLB container (4-byte padded per spec).
    // Same framing as the Draco suite; duplicated to keep this file self-contained.
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

    // meshopt encoders (linked into the test binary via the unity shim). The vertex
    // codec is byte-lossless, so unfiltered attribute/index round-trips are bit-exact.
    std::string encode_vertices(const void *data, size_t count, size_t stride)
    {
        std::vector<unsigned char> buf(meshopt_encodeVertexBufferBound(count, stride));
        const size_t n = meshopt_encodeVertexBuffer(buf.data(), buf.size(), data, count, stride);
        return { reinterpret_cast<char *>(buf.data()), n };
    }
    std::string encode_index_triangles(const unsigned int *idx, size_t count, size_t vertex_count)
    {
        std::vector<unsigned char> buf(meshopt_encodeIndexBufferBound(count, vertex_count));
        const size_t n = meshopt_encodeIndexBuffer(buf.data(), buf.size(), idx, count);
        return { reinterpret_cast<char *>(buf.data()), n };
    }
    std::string encode_index_sequence(const unsigned int *idx, size_t count, size_t vertex_count)
    {
        std::vector<unsigned char> buf(meshopt_encodeIndexSequenceBound(count, vertex_count));
        const size_t n = meshopt_encodeIndexSequence(buf.data(), buf.size(), idx, count);
        return { reinterpret_cast<char *>(buf.data()), n };
    }
    // Raw little-endian uint16 index bytes — used to author a *plain* (uncompressed)
    // index buffer view alongside a meshopt-compressed one.
    std::string raw_indices_u16(const unsigned int *idx, size_t count)
    {
        std::string s;
        for (size_t i = 0; i < count; i++)
        {
            const auto v = static_cast<uint16_t>(idx[i]);
            s.push_back(static_cast<char>(v & 0xFFu));
            s.push_back(static_cast<char>((v >> 8) & 0xFFu));
        }
        return s;
    }

    // One buffer view fed to the assembler below. A meshopt view stores compressed
    // bytes (`bytes`) plus its decompressed stride/count; a plain view stores raw
    // bytes directly (no extension), exercising the loader's skip path.
    struct View
    {
        std::string bytes;
        size_t stride = 0;
        size_t count = 0;
        const char *mode = "ATTRIBUTES"; // ATTRIBUTES / TRIANGLES / INDICES
        const char *filter = "NONE";     // NONE / OCTAHEDRAL / QUATERNION / EXPONENTIAL / COLOR
        bool meshopt = true;
        bool is_attr = true; // attribute views carry a top-level byteStride; index views don't
    };

    // Assemble a GLB from a list of buffer views (order == bufferView index). The
    // single BIN buffer holds, for each meshopt view, a zero-filled fallback region
    // (cgltf requires a bufferView `buffer`, and the fallback byteLength must equal
    // the decompressed size) plus the compressed stream referenced by the extension;
    // plain views store their raw bytes once. meshes/accessors JSON (caller-supplied)
    // reference the bufferView indices.
    std::string assemble_views(
        const std::vector<View> &vs,
        const std::string &node_json,
        const std::string &meshes_json,
        const std::string &accessors_json,
        const char *ext_name = "EXT_meshopt_compression"
    )
    {
        auto pad4 = [](std::string &s)
        {
            while (s.size() % 4 != 0)
            {
                s.push_back('\0');
            }
        };
        const auto S = [](size_t v) { return std::to_string(v); };

        std::string bin;
        std::vector<size_t> region_off(vs.size());  // fallback offset (meshopt) or data offset (plain)
        std::vector<size_t> comp_off(vs.size(), 0); // compressed offset (meshopt only)
        for (size_t i = 0; i < vs.size(); i++)
        {
            region_off[i] = bin.size();
            if (vs[i].meshopt)
            {
                bin.append(vs[i].stride * vs[i].count, '\0'); // fallback zeros (never read)
            }
            else
            {
                bin += vs[i].bytes; // plain data lives here directly
            }
        }
        for (size_t i = 0; i < vs.size(); i++)
        {
            if (!vs[i].meshopt)
            {
                continue;
            }
            pad4(bin);
            comp_off[i] = bin.size();
            bin += vs[i].bytes;
        }

        std::string bvs;
        for (size_t i = 0; i < vs.size(); i++)
        {
            const View &v = vs[i];
            const size_t declen = v.meshopt ? v.stride * v.count : v.bytes.size();
            bvs += R"({"buffer":0,"byteOffset":)" + S(region_off[i]) + R"(,"byteLength":)" + S(declen);
            if (v.is_attr)
            {
                bvs += R"(,"byteStride":)" + S(v.stride);
            }
            if (v.meshopt)
            {
                bvs += R"(,"extensions":{")" + std::string(ext_name) + R"(":{"buffer":0,"byteOffset":)" +
                       S(comp_off[i]) + R"(,"byteLength":)" + S(v.bytes.size()) + R"(,"byteStride":)" + S(v.stride) +
                       R"(,"count":)" + S(v.count) + R"(,"mode":")" + v.mode + R"(","filter":")" + v.filter + R"("}})";
            }
            bvs += "}";
            if (i + 1 < vs.size())
            {
                bvs += ",";
            }
        }

        std::string json;
        json += R"({"asset":{"version":"2.0"},)";
        json += R"("extensionsUsed":[")" + std::string(ext_name) + R"("],)";
        json += R"("extensionsRequired":[")" + std::string(ext_name) + R"("],)";
        json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{)" + node_json + "}],";
        json += R"("meshes":)" + meshes_json + ",";
        json += R"("accessors":[)" + accessors_json + "],";
        json += R"("bufferViews":[)" + bvs + "],";
        json += R"("buffers":[{"byteLength":)" + S(bin.size()) + "}]}";
        return assemble_glb(json, bin);
    }

    // Convenience for the common single-primitive shape: one vertex view + one index
    // view. `attributes`/`vtx_accessors` describe the vertex attributes; the index
    // accessor (last slot) is appended automatically referencing the final view.
    struct Desc
    {
        std::string vtx_comp;
        size_t vtx_stride = 0;
        size_t vtx_count = 0;
        const char *vtx_filter = "NONE";
        std::string idx_comp;
        size_t idx_stride = 0;
        size_t idx_count = 0;
        const char *idx_mode = "TRIANGLES";
        int idx_component_type = 5123;
        bool idx_plain = false;
        std::string attributes;
        std::string vtx_accessors;
        size_t index_acc_slot = 0;
        std::string node_json = R"("mesh":0)";
        const char *ext_name = "EXT_meshopt_compression";
    };

    std::string make_glb(const Desc &d)
    {
        std::vector<View> vs;
        vs.push_back({ d.vtx_comp, d.vtx_stride, d.vtx_count, "ATTRIBUTES", d.vtx_filter, true, true });
        vs.push_back({ d.idx_comp, d.idx_stride, d.idx_count, d.idx_mode, "NONE", !d.idx_plain, false });
        const size_t index_bv = vs.size() - 1;
        const std::string meshes = R"([{"primitives":[{"attributes":)" + d.attributes + R"(,"indices":)" +
                                   std::to_string(d.index_acc_slot) + R"(,"mode":4}]}])";
        const std::string accessors = d.vtx_accessors + R"(,{"bufferView":)" + std::to_string(index_bv) +
                                      R"(,"componentType":)" + std::to_string(d.idx_component_type) + R"(,"count":)" +
                                      std::to_string(d.idx_count) + R"(,"type":"SCALAR"})";
        return assemble_views(vs, d.node_json, meshes, accessors, d.ext_name);
    }

    // A unit quad in the z=0 plane: 4 distinct positions, 2 triangles.
    const float quad_pos[12] = {
        -1.0f, -1.0f, 0.0f, // 0
        1.0f,  -1.0f, 0.0f, // 1
        1.0f,  1.0f,  0.0f, // 2
        -1.0f, 1.0f,  0.0f, // 3
    };
    const unsigned int quad_idx[6] = { 0, 1, 2, 0, 2, 3 };

    constexpr const char *quad_pos_accessor =
        R"({"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]})";

    // True iff every original quad position appears (bit-exact) in the mesh. vcache
    // may reorder vertices, but the codec is lossless so the position set is preserved.
    bool has_all_quad_positions(const Mesh &m)
    {
        for (int c = 0; c < 4; c++)
        {
            const float x = quad_pos[(c * 3) + 0];
            const float y = quad_pos[(c * 3) + 1];
            const float z = quad_pos[(c * 3) + 2];
            bool found = false;
            for (const auto &v : m.vertices)
            {
                if (v.pos.x == x && v.pos.y == y && v.pos.z == z)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return false;
            }
        }
        return true;
    }
} // namespace

// Core round-trip: ATTRIBUTES vertex stream + TRIANGLES index stream.

TEST(gltf_meshopt, position_only_triangles)
{
    Desc d;
    d.vtx_comp = encode_vertices(quad_pos, 4, 12);
    d.vtx_stride = 12;
    d.vtx_count = 4;
    d.idx_comp = encode_index_triangles(quad_idx, 6, 4);
    d.idx_stride = 2;
    d.idx_count = 6;
    d.attributes = R"({"POSITION":0})";
    d.vtx_accessors = quad_pos_accessor;
    d.index_acc_slot = 1;

    const std::string glb = make_glb(d);
    TmpFile f(tmp_path("rast_meshopt_pos.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 4u);
    ASSERT_EQ(m.triangles.size(), 2u);
    ASSERT_TRUE(has_all_quad_positions(m));
    // No NORMAL declared → compute_normals ran and filled unit-length normals.
    for (const auto &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.length(), 1.0f, 1e-3f);
    }
}

// Interleaved POSITION + NORMAL in one ATTRIBUTES stream (stride 24).

TEST(gltf_meshopt, interleaved_position_normal)
{
    float vtx[4 * 6];
    for (int i = 0; i < 4; i++)
    {
        vtx[(i * 6) + 0] = quad_pos[(i * 3) + 0];
        vtx[(i * 6) + 1] = quad_pos[(i * 3) + 1];
        vtx[(i * 6) + 2] = quad_pos[(i * 3) + 2];
        vtx[(i * 6) + 3] = 0.0f;
        vtx[(i * 6) + 4] = 0.0f;
        vtx[(i * 6) + 5] = 1.0f;
    }
    Desc d;
    d.vtx_comp = encode_vertices(vtx, 4, 24);
    d.vtx_stride = 24;
    d.vtx_count = 4;
    d.idx_comp = encode_index_triangles(quad_idx, 6, 4);
    d.idx_stride = 2;
    d.idx_count = 6;
    d.attributes = R"({"POSITION":0,"NORMAL":1})";
    d.vtx_accessors =
        R"({"bufferView":0,"byteOffset":0,"componentType":5126,"count":4,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},)"
        R"({"bufferView":0,"byteOffset":12,"componentType":5126,"count":4,"type":"VEC3"})";
    d.index_acc_slot = 2;

    const std::string glb = make_glb(d);
    TmpFile f(tmp_path("rast_meshopt_pn.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 4u);
    ASSERT_EQ(m.triangles.size(), 2u);
    ASSERT_TRUE(has_all_quad_positions(m));
    // Normals came from the stream (all +Z), not from compute_normals.
    for (const auto &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.x, 0.0f, 1e-4f);
        ASSERT_NEAR(v.normal.y, 0.0f, 1e-4f);
        ASSERT_NEAR(v.normal.z, 1.0f, 1e-4f);
    }
}

// INDICES (sequence) mode and 32-bit index width.

TEST(gltf_meshopt, index_sequence_mode)
{
    Desc d;
    d.vtx_comp = encode_vertices(quad_pos, 4, 12);
    d.vtx_stride = 12;
    d.vtx_count = 4;
    d.idx_comp = encode_index_sequence(quad_idx, 6, 4);
    d.idx_stride = 2;
    d.idx_count = 6;
    d.idx_mode = "INDICES";
    d.attributes = R"({"POSITION":0})";
    d.vtx_accessors = quad_pos_accessor;
    d.index_acc_slot = 1;

    const std::string glb = make_glb(d);
    TmpFile f(tmp_path("rast_meshopt_seq.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 4u);
    ASSERT_EQ(m.triangles.size(), 2u);
    ASSERT_TRUE(has_all_quad_positions(m));
}

TEST(gltf_meshopt, uint32_indices)
{
    Desc d;
    d.vtx_comp = encode_vertices(quad_pos, 4, 12);
    d.vtx_stride = 12;
    d.vtx_count = 4;
    d.idx_comp = encode_index_triangles(quad_idx, 6, 4);
    d.idx_stride = 4; // uint32
    d.idx_count = 6;
    d.idx_component_type = 5125;
    d.attributes = R"({"POSITION":0})";
    d.vtx_accessors = quad_pos_accessor;
    d.index_acc_slot = 1;

    const std::string glb = make_glb(d);
    TmpFile f(tmp_path("rast_meshopt_u32.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 4u);
    ASSERT_EQ(m.triangles.size(), 2u);
    ASSERT_TRUE(has_all_quad_positions(m));
}

// Filters: each cgltf filter enum must dispatch to the right decode-filter call.

TEST(gltf_meshopt, filter_exponential_positions)
{
    // EXPONENTIAL decodes to float32 in place, so POSITION stays a FLOAT VEC3 accessor.
    // Encode the quad positions through the exp filter (15-bit mantissa → tight), then
    // the vertex codec; the loader must run decodeFilterExp and recover the positions.
    std::vector<unsigned char> filtered(static_cast<size_t>(4) * 12);
    meshopt_encodeFilterExp(filtered.data(), 4, 12, 15, quad_pos, meshopt_EncodeExpSeparate);
    Desc d;
    d.vtx_comp = encode_vertices(filtered.data(), 4, 12);
    d.vtx_stride = 12;
    d.vtx_count = 4;
    d.vtx_filter = "EXPONENTIAL";
    d.idx_comp = encode_index_triangles(quad_idx, 6, 4);
    d.idx_stride = 2;
    d.idx_count = 6;
    d.attributes = R"({"POSITION":0})";
    d.vtx_accessors = quad_pos_accessor;
    d.index_acc_slot = 1;

    const std::string glb = make_glb(d);
    TmpFile f(tmp_path("rast_meshopt_exp.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 4u);
    // Positions recovered within exp quantization.
    for (int c = 0; c < 4; c++)
    {
        bool found = false;
        for (const auto &v : m.vertices)
        {
            if (std::abs(v.pos.x - quad_pos[(c * 3) + 0]) < 1e-3f &&
                std::abs(v.pos.y - quad_pos[(c * 3) + 1]) < 1e-3f && std::abs(v.pos.z - quad_pos[(c * 3) + 2]) < 1e-3f)
            {
                found = true;
                break;
            }
        }
        ASSERT_TRUE(found);
    }
}

TEST(gltf_meshopt, filter_octahedral_normals)
{
    // OCTAHEDRAL decodes unit normals into normalized int8 (stride 4). POSITION is a
    // plain unfiltered view (bufferView 0); NORMAL is an oct-filtered view (bufferView
    // 1) read as a normalized BYTE VEC3. Normals are all +Z → recover ~ (0,0,1).
    std::vector<unsigned char> oct(static_cast<size_t>(4) * 4);
    float normals4[4 * 4];
    for (int i = 0; i < 4; i++)
    {
        normals4[(i * 4) + 0] = 0.0f;
        normals4[(i * 4) + 1] = 0.0f;
        normals4[(i * 4) + 2] = 1.0f;
        normals4[(i * 4) + 3] = 0.0f;
    }
    meshopt_encodeFilterOct(oct.data(), 4, 4, 8, normals4);

    std::vector<View> vs;
    vs.push_back({ encode_vertices(quad_pos, 4, 12), 12, 4, "ATTRIBUTES", "NONE", true, true });
    vs.push_back({ encode_vertices(oct.data(), 4, 4), 4, 4, "ATTRIBUTES", "OCTAHEDRAL", true, true });
    vs.push_back({ encode_index_triangles(quad_idx, 6, 4), 2, 6, "TRIANGLES", "NONE", true, false });
    const std::string meshes = R"([{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2,"mode":4}]}])";
    const std::string accessors = std::string(quad_pos_accessor) +
                                  R"(,{"bufferView":1,"componentType":5120,"normalized":true,"count":4,"type":"VEC3"},)"
                                  R"({"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"})";
    const std::string glb = assemble_views(vs, R"("mesh":0)", meshes, accessors);

    TmpFile f(tmp_path("rast_meshopt_oct.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 4u);
    for (const auto &v : m.vertices)
    {
        ASSERT_NEAR(v.normal.x, 0.0f, 2e-2f);
        ASSERT_NEAR(v.normal.y, 0.0f, 2e-2f);
        ASSERT_NEAR(v.normal.z, 1.0f, 2e-2f);
    }
}

TEST(gltf_meshopt, filter_quaternion_dispatch)
{
    // QUATERNION targets rotation/tangent data the loader doesn't consume, so this
    // is dispatch coverage: a quat-filtered TANGENT view (bufferView 1) must decode
    // without error while POSITION comes from a plain unfiltered view. A clean load
    // proves the QUATERNION branch ran and didn't fault.
    std::vector<unsigned char> quat(static_cast<size_t>(4) * 8);
    float quats4[4 * 4];
    for (int i = 0; i < 4; i++)
    {
        quats4[(i * 4) + 0] = 0.0f;
        quats4[(i * 4) + 1] = 0.0f;
        quats4[(i * 4) + 2] = 0.0f;
        quats4[(i * 4) + 3] = 1.0f; // identity rotation
    }
    meshopt_encodeFilterQuat(quat.data(), 4, 8, 16, quats4);

    std::vector<View> vs;
    vs.push_back({ encode_vertices(quad_pos, 4, 12), 12, 4, "ATTRIBUTES", "NONE", true, true });
    vs.push_back({ encode_vertices(quat.data(), 4, 8), 8, 4, "ATTRIBUTES", "QUATERNION", true, true });
    vs.push_back({ encode_index_triangles(quad_idx, 6, 4), 2, 6, "TRIANGLES", "NONE", true, false });
    const std::string meshes = R"([{"primitives":[{"attributes":{"POSITION":0,"TANGENT":1},"indices":2,"mode":4}]}])";
    const std::string accessors = std::string(quad_pos_accessor) +
                                  R"(,{"bufferView":1,"componentType":5122,"normalized":true,"count":4,"type":"VEC4"},)"
                                  R"({"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"})";
    const std::string glb = assemble_views(vs, R"("mesh":0)", meshes, accessors);

    TmpFile f(tmp_path("rast_meshopt_quat.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 4u);
    ASSERT_TRUE(has_all_quad_positions(m));
}

// Mixed: a meshopt-compressed view and a plain (uncompressed) view in one file.

TEST(gltf_meshopt, mixed_compressed_and_plain_views)
{
    // POSITION is meshopt-compressed (bufferView 0); the index buffer is a plain
    // uncompressed view (bufferView 1). Exercises the loader's `continue` skip for
    // non-meshopt views — both must coexist and read correctly.
    Desc d;
    d.vtx_comp = encode_vertices(quad_pos, 4, 12);
    d.vtx_stride = 12;
    d.vtx_count = 4;
    d.idx_comp = raw_indices_u16(quad_idx, 6);
    d.idx_stride = 2;
    d.idx_count = 6;
    d.idx_plain = true;
    d.attributes = R"({"POSITION":0})";
    d.vtx_accessors = quad_pos_accessor;
    d.index_acc_slot = 1;

    const std::string glb = make_glb(d);
    TmpFile f(tmp_path("rast_meshopt_mixed.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 4u);
    ASSERT_EQ(m.triangles.size(), 2u);
    ASSERT_TRUE(has_all_quad_positions(m));
}

// Fail-loud: corrupt / truncated streams reject the whole load.

TEST(gltf_meshopt, corrupt_stream_fails_load)
{
    Desc d;
    d.vtx_comp = encode_vertices(quad_pos, 4, 12);
    // Clobber the vertex-codec header byte (high nibble carries the format magic): a
    // bad header makes meshopt_decodeVertexBuffer return nonzero → load fails loud.
    d.vtx_comp[0] = '\0';
    d.vtx_stride = 12;
    d.vtx_count = 4;
    d.idx_comp = encode_index_triangles(quad_idx, 6, 4);
    d.idx_stride = 2;
    d.idx_count = 6;
    d.attributes = R"({"POSITION":0})";
    d.vtx_accessors = quad_pos_accessor;
    d.index_acc_slot = 1;

    const std::string glb = make_glb(d);
    TmpFile f(tmp_path("rast_meshopt_corrupt.glb"), glb.data(), glb.size());
    assert_rejects(f.path);
}

TEST(gltf_meshopt, truncated_stream_fails_load)
{
    Desc d;
    const std::string full = encode_vertices(quad_pos, 4, 12);
    // Drop the back half of the compressed stream — declared byteLength shrinks with
    // it so cgltf's bounds pass, but the codec runs out of input → nonzero rc.
    d.vtx_comp = full.substr(0, full.size() / 2);
    d.vtx_stride = 12;
    d.vtx_count = 4;
    d.idx_comp = encode_index_triangles(quad_idx, 6, 4);
    d.idx_stride = 2;
    d.idx_count = 6;
    d.attributes = R"({"POSITION":0})";
    d.vtx_accessors = quad_pos_accessor;
    d.index_acc_slot = 1;

    const std::string glb = make_glb(d);
    TmpFile f(tmp_path("rast_meshopt_trunc.glb"), glb.data(), glb.size());
    assert_rejects(f.path);
}

// KHR_meshopt_compression: the ratified extension name, same JSON shape as
// EXT_meshopt_compression. cgltf must parse it into the same fields and the
// loader's existing decode path must run unmodified.

TEST(gltf_meshopt, khr_extension_name)
{
    Desc d;
    d.vtx_comp = encode_vertices(quad_pos, 4, 12);
    d.vtx_stride = 12;
    d.vtx_count = 4;
    d.idx_comp = encode_index_triangles(quad_idx, 6, 4);
    d.idx_stride = 2;
    d.idx_count = 6;
    d.attributes = R"({"POSITION":0})";
    d.vtx_accessors = quad_pos_accessor;
    d.index_acc_slot = 1;
    d.ext_name = "KHR_meshopt_compression";

    const std::string glb = make_glb(d);
    TmpFile f(tmp_path("rast_meshopt_khr.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 4u);
    ASSERT_EQ(m.triangles.size(), 2u);
    ASSERT_TRUE(has_all_quad_positions(m));
}

// COLOR filter (KHR_meshopt_compression only): YCoCg(+A) vertex color decode.

TEST(gltf_meshopt, filter_color_vertex_colors)
{
    // COLOR decodes RGBA into YCoCg(+A) space, K-bit per channel (here K=8, stride 4).
    // POSITION is a plain unfiltered view (bufferView 0); COLOR_0 is a color-filtered
    // view (bufferView 1) read as a normalized UBYTE VEC4. All vertices encode opaque
    // red; recovered within YCoCg-quantization tolerance.
    std::vector<unsigned char> col(static_cast<size_t>(4) * 4);
    float colors4[4 * 4];
    for (int i = 0; i < 4; i++)
    {
        colors4[(i * 4) + 0] = 1.0f;
        colors4[(i * 4) + 1] = 0.0f;
        colors4[(i * 4) + 2] = 0.0f;
        colors4[(i * 4) + 3] = 1.0f;
    }
    meshopt_encodeFilterColor(col.data(), 4, 4, 8, colors4);

    std::vector<View> vs;
    vs.push_back({ encode_vertices(quad_pos, 4, 12), 12, 4, "ATTRIBUTES", "NONE", true, true });
    vs.push_back({ encode_vertices(col.data(), 4, 4), 4, 4, "ATTRIBUTES", "COLOR", true, true });
    vs.push_back({ encode_index_triangles(quad_idx, 6, 4), 2, 6, "TRIANGLES", "NONE", true, false });
    const std::string meshes = R"([{"primitives":[{"attributes":{"POSITION":0,"COLOR_0":1},"indices":2,"mode":4}]}])";
    const std::string accessors = std::string(quad_pos_accessor) +
                                  R"(,{"bufferView":1,"componentType":5121,"normalized":true,"count":4,"type":"VEC4"},)"
                                  R"({"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"})";
    const std::string glb = assemble_views(vs, R"("mesh":0)", meshes, accessors, "KHR_meshopt_compression");

    TmpFile f(tmp_path("rast_meshopt_color.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 4u);
    ASSERT_EQ(m.vertex_colors.size(), 4u);
    for (const auto &c : m.vertex_colors)
    {
        ASSERT_NEAR(c.x, 1.0f, 0.05f);
        ASSERT_NEAR(c.y, 0.0f, 0.05f);
        ASSERT_NEAR(c.z, 0.0f, 0.05f);
    }
}

// Node world transform applies to decoded meshopt vertices.

TEST(gltf_meshopt, node_transform_applies)
{
    Desc d;
    d.vtx_comp = encode_vertices(quad_pos, 4, 12);
    d.vtx_stride = 12;
    d.vtx_count = 4;
    d.idx_comp = encode_index_triangles(quad_idx, 6, 4);
    d.idx_stride = 2;
    d.idx_count = 6;
    d.attributes = R"({"POSITION":0})";
    d.vtx_accessors = quad_pos_accessor;
    d.index_acc_slot = 1;
    // Translate +10 on X (column-major). Quad spans x∈[-1,1] → must land in [9,11].
    d.node_json = R"("mesh":0,"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,10,0,0,1])";

    const std::string glb = make_glb(d);
    TmpFile f(tmp_path("rast_meshopt_xform.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.vertices.size(), 4u);
    for (const auto &v : m.vertices)
    {
        ASSERT_TRUE(v.pos.x >= 8.9f);
        ASSERT_TRUE(v.pos.x <= 11.1f);
    }
}
