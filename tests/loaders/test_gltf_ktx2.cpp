#include "tests/ktx2_fixtures.h"
#include "tests/loader_util.h"
#include "src/loaders/mesh.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

namespace
{
    // Accumulates a GLB BIN chunk, 4-aligning each region and returning its byte offset
    // so the JSON bufferViews can reference it.
    struct Bin
    {
        std::string data;

        void pad4()
        {
            while (data.size() % 4 != 0)
            {
                data.push_back('\0');
            }
        }
        size_t add_bytes(const unsigned char *p, size_t n)
        {
            pad4();
            const size_t off = data.size();
            for (size_t i = 0; i < n; i++)
            {
                data.push_back(static_cast<char>(p[i]));
            }
            return off;
        }
        size_t add_floats(std::initializer_list<float> fs)
        {
            pad4();
            const size_t off = data.size();
            for (float f : fs)
            {
                emit_f32_le(data, f);
            }
            return off;
        }
    };

    // Frame a JSON + BIN chunk into a GLB container (both 4-padded per spec).
    std::string assemble_glb(std::string json, std::string bin)
    {
        while (json.size() % 4 != 0)
        {
            json += ' ';
        }
        while (bin.size() % 4 != 0)
        {
            bin.push_back('\0');
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

    const char *const kTriPositionsAccessor =
        R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]})";

    // A GLB whose single material's baseColorTexture is a KHR_texture_basisu texture
    // backed by the given embedded KTX2 bytes.
    std::string embedded_basisu_glb(const unsigned char *ktx2, size_t klen)
    {
        Bin bin;
        bin.add_floats({ -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f }); // bufferView 0
        const size_t img_off = bin.add_bytes(ktx2, klen);                            // bufferView 1
        const auto S = [](size_t v) { return std::to_string(v); };
        std::string json;
        json += R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_texture_basisu"],)";
        json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
        json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
        json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
        json += R"("textures":[{"extensions":{"KHR_texture_basisu":{"source":0}}}],)";
        json += R"("images":[{"bufferView":1,"mimeType":"image/ktx2"}],)";
        json += R"("accessors":[)" + std::string(kTriPositionsAccessor) + "],";
        json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)";
        json += R"({"buffer":0,"byteOffset":)" + S(img_off) + R"(,"byteLength":)" + S(klen) + "}],";
        json += R"("buffers":[{"byteLength":)" + S(bin.data.size()) + "}]}";
        return assemble_glb(json, bin.data);
    }
} // namespace

// Embedded KHR_texture_basisu decodes: the headline "untextured -> textured".
// The 6x6 fixture is non-block-aligned (physical 8x8), so this also covers the
// transcoder's crop / row-pitch path that the 4x4 unit fixtures cannot.

TEST(gltf_ktx2, embedded_basisu_texture_decodes_with_correct_layout)
{
    const std::string glb = embedded_basisu_glb(k_ktx2_uastc_grid6, k_ktx2_uastc_grid6_len);
    TmpFile f(tmp_path("rast_ktx2_embedded.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0);
    ASSERT_TRUE(idx < static_cast<int>(m.textures.size()));
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(t.width, 6);
    ASSERT_EQ(t.height, 6);

    // Rows top->bottom: red, green, blue, yellow, cyan, magenta. Checking distinct rows
    // proves the crop kept correct row alignment (a wrong pitch would shuffle these).
    // pixels are row-major top-left first, RGBA.
    const auto chan = [&](int x, int y, int c)
    { return t.pixels[(((static_cast<size_t>(y) * 6) + static_cast<size_t>(x)) * 4) + static_cast<size_t>(c)]; };
    ASSERT_TRUE(chan(0, 0, 0) > 200 && chan(0, 0, 1) < 80);  // row0 red
    ASSERT_TRUE(chan(5, 1, 1) > 170 && chan(5, 1, 0) < 80);  // row1 green
    ASSERT_TRUE(chan(0, 2, 2) > 180 && chan(0, 2, 0) < 80);  // row2 blue
    ASSERT_TRUE(chan(5, 5, 0) > 180 && chan(5, 5, 2) > 170); // row5 magenta
}

// Precedence: a texture carrying BOTH a plain image and a KHR_texture_basisu
// image must decode the KTX2 source, not the fallback.

TEST(gltf_ktx2, basisu_source_preferred_over_fallback_image)
{
    Bin bin;
    bin.add_floats({ -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f });          // bufferView 0
    const size_t png_off = bin.add_bytes(k_png_fallback_green, k_png_fallback_green_len); // bufferView 1
    const size_t ktx_off = bin.add_bytes(k_ktx2_uastc_grid6, k_ktx2_uastc_grid6_len);     // bufferView 2
    const auto S = [](size_t v) { return std::to_string(v); };

    std::string json;
    json += R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_texture_basisu"],)";
    json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
    json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
    // Plain source 0 (2x2 PNG) and basisu source 1 (6x6 KTX2). The 6x6 dimensions prove
    // which one was decoded.
    json += R"("textures":[{"source":0,"extensions":{"KHR_texture_basisu":{"source":1}}}],)";
    json += R"("images":[{"bufferView":1,"mimeType":"image/png"},{"bufferView":2,"mimeType":"image/ktx2"}],)";
    json += R"("accessors":[)" + std::string(kTriPositionsAccessor) + "],";
    json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)";
    json += R"({"buffer":0,"byteOffset":)" + S(png_off) + R"(,"byteLength":)" + S(k_png_fallback_green_len) + "},";
    json += R"({"buffer":0,"byteOffset":)" + S(ktx_off) + R"(,"byteLength":)" + S(k_ktx2_uastc_grid6_len) + "}],";
    json += R"("buffers":[{"byteLength":)" + S(bin.data.size()) + "}]}";

    const std::string glb = assemble_glb(json, bin.data);
    TmpFile f(tmp_path("rast_ktx2_precedence.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0);
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_EQ(t.width, 6); // KTX2 (6x6) chosen, not the 2x2 PNG fallback
    ASSERT_EQ(t.height, 6);
}

// Graceful degradation: when the preferred KTX2 fails to transcode but the
// texture also supplies an ordinary source, fall back to it (KHR_texture_basisu
// intent) rather than dropping the texture.

TEST(gltf_ktx2, falls_back_to_ordinary_source_when_ktx2_fails)
{
    Bin bin;
    bin.add_floats({ -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f });          // bufferView 0
    const size_t png_off = bin.add_bytes(k_png_fallback_green, k_png_fallback_green_len); // bufferView 1
    // A truncated (corrupt) KTX2 as the basisu source: valid identifier, unusable body.
    const size_t bad_ktx_len = 32;
    const size_t ktx_off = bin.add_bytes(k_ktx2_uastc_grid6, bad_ktx_len); // bufferView 2
    const auto S = [](size_t v) { return std::to_string(v); };

    std::string json;
    json += R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_texture_basisu"],)";
    json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
    json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
    json += R"("textures":[{"source":0,"extensions":{"KHR_texture_basisu":{"source":1}}}],)";
    json += R"("images":[{"bufferView":1,"mimeType":"image/png"},{"bufferView":2,"mimeType":"image/ktx2"}],)";
    json += R"("accessors":[)" + std::string(kTriPositionsAccessor) + "],";
    json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)";
    json += R"({"buffer":0,"byteOffset":)" + S(png_off) + R"(,"byteLength":)" + S(k_png_fallback_green_len) + "},";
    json += R"({"buffer":0,"byteOffset":)" + S(ktx_off) + R"(,"byteLength":)" + S(bad_ktx_len) + "}],";
    json += R"("buffers":[{"byteLength":)" + S(bin.data.size()) + "}]}";

    const std::string glb = assemble_glb(json, bin.data);
    TmpFile f(tmp_path("rast_ktx2_fallback.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0); // not dropped: fell back to the PNG
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_EQ(t.width, 2); // the 2x2 PNG fallback, since the KTX2 transcode failed
    ASSERT_EQ(t.height, 2);
}

// External (.gltf + sidecar .ktx2) routed by content sniff, not extension.

TEST(gltf_ktx2, external_uri_ktx2_decodes)
{
    // Three sidecar files in the same dir: .gltf + .bin (positions) + .ktx2 (image).
    std::string pos;
    for (float v : { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f })
    {
        emit_f32_le(pos, v);
    }
    TmpFile bin_file(tmp_path("rast_ktx2_ext.bin"), pos.data(), pos.size());
    TmpFile img_file(tmp_path("rast_ktx2_ext.ktx2"), k_ktx2_uastc_grid6, k_ktx2_uastc_grid6_len);

    std::string json;
    json += R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_texture_basisu"],)";
    json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
    json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
    json += R"("textures":[{"extensions":{"KHR_texture_basisu":{"source":0}}}],)";
    json += R"("images":[{"uri":"rast_ktx2_ext.ktx2"}],)";
    json += R"("accessors":[)" + std::string(kTriPositionsAccessor) + "],";
    json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)";
    json += R"("buffers":[{"uri":"rast_ktx2_ext.bin","byteLength":36}]})";

    TmpFile gltf_file(tmp_path("rast_ktx2_ext.gltf"), json);
    Mesh m = load_ok(gltf_file.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0);
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_EQ(t.width, 6);
    ASSERT_EQ(t.height, 6);
}

TEST(gltf_ktx2, external_uri_percent_encoded_filename_decodes)
{
    // The on-disk sidecar name contains a space; the glTF URI percent-encodes it as
    // %20. cgltf does not decode image URIs, so the loader must percent-decode before
    // opening the file, else it would look for the literal "...%20..." name and drop it.
    std::string pos;
    for (float v : { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f })
    {
        emit_f32_le(pos, v);
    }
    TmpFile bin_file(tmp_path("rast_ktx2_pct.bin"), pos.data(), pos.size());
    TmpFile img_file(tmp_path("rast ktx2 pct img.ktx2"), k_ktx2_uastc_grid6, k_ktx2_uastc_grid6_len);

    std::string json;
    json += R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_texture_basisu"],)";
    json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
    json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
    json += R"("textures":[{"extensions":{"KHR_texture_basisu":{"source":0}}}],)";
    json += R"("images":[{"uri":"rast%20ktx2%20pct%20img.ktx2"}],)";
    json += R"("accessors":[)" + std::string(kTriPositionsAccessor) + "],";
    json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)";
    json += R"("buffers":[{"uri":"rast_ktx2_pct.bin","byteLength":36}]})";

    TmpFile gltf_file(tmp_path("rast_ktx2_pct.gltf"), json);
    Mesh m = load_ok(gltf_file.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0); // decoded: the %20 was decoded to a space and the file opened
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_EQ(t.width, 6);
    ASSERT_EQ(t.height, 6);
}

// Fail-loud: a corrupt KTX2 drops the texture slot (index -> -1) but the model
// still loads, matching how a failed stb image decode is handled.

TEST(gltf_ktx2, corrupt_basisu_drops_texture_but_loads)
{
    // Truncate the KTX2 to the first 32 bytes: a valid identifier but no usable body.
    const size_t klen = 32;
    const std::string glb = embedded_basisu_glb(k_ktx2_uastc_grid6, klen);
    TmpFile f(tmp_path("rast_ktx2_corrupt.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1); // decode failed -> slot dropped, load still ok
    ASSERT_TRUE(m.textures.empty());
}

// A KTX2 carried by an inline base64 data: URI decodes, proving the data-URI
// path routes through decode_bytes to the KTX2 transcoder, not just stb.

TEST(gltf_ktx2, data_uri_basisu_decodes)
{
    const std::string uri = "data:image/ktx2;base64," + b64encode(k_ktx2_uastc_grid6, k_ktx2_uastc_grid6_len);

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

    std::string json;
    json += R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_texture_basisu"],)";
    json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
    json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
    json += R"("textures":[{"extensions":{"KHR_texture_basisu":{"source":0}}}],)";
    json += R"("images":[{"uri":")" + uri + R"(","mimeType":"image/ktx2"}],)";
    json += R"("accessors":[)" + std::string(kTriPositionsAccessor) + "],";
    json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)";
    json += R"("buffers":[{"byteLength":36}]})";

    const std::string glb = assemble_glb(json, bin);
    TmpFile f(tmp_path("rast_ktx2_datauri.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0);
    ASSERT_TRUE(idx < static_cast<int>(m.textures.size()));
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(t.width, 6);
    ASSERT_EQ(t.height, 6);
}
