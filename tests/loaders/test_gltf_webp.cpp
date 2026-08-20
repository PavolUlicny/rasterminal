#include "tests/ktx2_fixtures.h" // k_png_fallback_green (2x2 PNG), k_ktx2_uastc_grid6 (6x6) for precedence
#include "tests/webp_fixtures.h"
#include "tests/loader_util.h"
#include "src/mesh.h"

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

    // A GLB whose single material's baseColorTexture is an EXT_texture_webp texture
    // backed by the given embedded WebP bytes.
    std::string embedded_webp_glb(const unsigned char *webp, size_t wlen)
    {
        Bin bin;
        bin.add_floats({ -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f }); // bufferView 0
        const size_t img_off = bin.add_bytes(webp, wlen);                            // bufferView 1
        const auto S = [](size_t v) { return std::to_string(v); };
        std::string json;
        json += R"({"asset":{"version":"2.0"},"extensionsUsed":["EXT_texture_webp"],)";
        json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
        json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
        json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
        json += R"("textures":[{"extensions":{"EXT_texture_webp":{"source":0}}}],)";
        json += R"("images":[{"bufferView":1,"mimeType":"image/webp"}],)";
        json += R"("accessors":[)" + std::string(kTriPositionsAccessor) + "],";
        json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)";
        json += R"({"buffer":0,"byteOffset":)" + S(img_off) + R"(,"byteLength":)" + S(wlen) + "}],";
        json += R"("buffers":[{"byteLength":)" + S(bin.data.size()) + "}]}";
        return assemble_glb(json, bin.data);
    }
} // namespace

// Embedded EXT_texture_webp decodes: the headline "untextured -> textured".
// The 6x2 non-square fixture also proves the loader keeps width vs height and
// row pitch straight end-to-end (a square fixture could not).

TEST(gltf_webp, embedded_webp_texture_decodes_with_correct_layout)
{
    const std::string glb = embedded_webp_glb(k_webp_rect, k_webp_rect_len);
    TmpFile f(tmp_path("rast_webp_embedded.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0);
    ASSERT_TRUE(idx < static_cast<int>(m.textures.size()));
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(t.width, 6);
    ASSERT_EQ(t.height, 2);

    // row0 px x = (x*40,0,0); row1 px x = (0,x*40,0). Lossless, so exact. A transposed or
    // wrong-pitch decode would shuffle these.
    const auto chan = [&](int x, int y, int c)
    { return t.pixels[(((static_cast<size_t>(y) * 6) + static_cast<size_t>(x)) * 4) + static_cast<size_t>(c)]; };
    ASSERT_EQ(chan(5, 0, 0), 200); // (5,0) row0 -> red
    ASSERT_EQ(chan(5, 0, 1), 0);
    ASSERT_EQ(chan(5, 1, 0), 0); // (5,1) row1 -> green
    ASSERT_EQ(chan(5, 1, 1), 200);
}

// Routing is by content sniff, not the declared mimeType: a texture declaring
// EXT_texture_webp whose bytes are actually a PNG still decodes (via stb).

TEST(gltf_webp, image_routed_by_content_not_declared_webp_type)
{
    // embedded_webp_glb labels the image image/webp and references it through the
    // EXT_texture_webp extension, but the bytes here are a 2x2 PNG. decode_bytes sniffs the
    // content (is_webp returns false on PNG magic) and routes to stb, so it decodes correctly.
    const std::string glb = embedded_webp_glb(k_png_fallback_green, k_png_fallback_green_len);
    TmpFile f(tmp_path("rast_webp_mislabeled.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0);
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(t.width, 2); // decoded as the 2x2 PNG via stb, despite the image/webp label
    ASSERT_EQ(t.height, 2);
}

// Precedence: a texture carrying BOTH a plain image and an EXT_texture_webp
// image must decode the WebP source, not the fallback.

TEST(gltf_webp, webp_source_preferred_over_fallback_image)
{
    Bin bin;
    bin.add_floats({ -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f });          // bufferView 0
    const size_t png_off = bin.add_bytes(k_png_fallback_green, k_png_fallback_green_len); // bufferView 1
    const size_t webp_off = bin.add_bytes(k_webp_rect, k_webp_rect_len);                  // bufferView 2
    const auto S = [](size_t v) { return std::to_string(v); };

    std::string json;
    json += R"({"asset":{"version":"2.0"},"extensionsUsed":["EXT_texture_webp"],)";
    json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
    json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
    // Plain source 0 (2x2 PNG) and webp source 1 (6x2). The 6-wide dimensions prove which
    // one was decoded.
    json += R"("textures":[{"source":0,"extensions":{"EXT_texture_webp":{"source":1}}}],)";
    json += R"("images":[{"bufferView":1,"mimeType":"image/png"},{"bufferView":2,"mimeType":"image/webp"}],)";
    json += R"("accessors":[)" + std::string(kTriPositionsAccessor) + "],";
    json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)";
    json += R"({"buffer":0,"byteOffset":)" + S(png_off) + R"(,"byteLength":)" + S(k_png_fallback_green_len) + "},";
    json += R"({"buffer":0,"byteOffset":)" + S(webp_off) + R"(,"byteLength":)" + S(k_webp_rect_len) + "}],";
    json += R"("buffers":[{"byteLength":)" + S(bin.data.size()) + "}]}";

    const std::string glb = assemble_glb(json, bin.data);
    TmpFile f(tmp_path("rast_webp_precedence.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0);
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_EQ(t.width, 6); // WebP (6x2) chosen, not the 2x2 PNG fallback
    ASSERT_EQ(t.height, 2);
}

// Graceful degradation: when the preferred WebP fails to decode but the texture
// also supplies an ordinary source, fall back to it rather than dropping it.

TEST(gltf_webp, falls_back_to_ordinary_source_when_webp_fails)
{
    Bin bin;
    bin.add_floats({ -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f });          // bufferView 0
    const size_t png_off = bin.add_bytes(k_png_fallback_green, k_png_fallback_green_len); // bufferView 1
    // A WebP with a valid RIFF/WEBP header but a corrupt body: sniffs as WebP, decode fails.
    const size_t webp_off = bin.add_bytes(k_webp_corrupt_body, k_webp_corrupt_body_len); // bufferView 2
    const auto S = [](size_t v) { return std::to_string(v); };

    std::string json;
    json += R"({"asset":{"version":"2.0"},"extensionsUsed":["EXT_texture_webp"],)";
    json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
    json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
    json += R"("textures":[{"source":0,"extensions":{"EXT_texture_webp":{"source":1}}}],)";
    json += R"("images":[{"bufferView":1,"mimeType":"image/png"},{"bufferView":2,"mimeType":"image/webp"}],)";
    json += R"("accessors":[)" + std::string(kTriPositionsAccessor) + "],";
    json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)";
    json += R"({"buffer":0,"byteOffset":)" + S(png_off) + R"(,"byteLength":)" + S(k_png_fallback_green_len) + "},";
    json += R"({"buffer":0,"byteOffset":)" + S(webp_off) + R"(,"byteLength":)" + S(k_webp_corrupt_body_len) + "}],";
    json += R"("buffers":[{"byteLength":)" + S(bin.data.size()) + "}]}";

    const std::string glb = assemble_glb(json, bin.data);
    TmpFile f(tmp_path("rast_webp_fallback.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0); // not dropped: fell back to the PNG
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_EQ(t.width, 2); // the 2x2 PNG fallback, since the WebP decode failed
    ASSERT_EQ(t.height, 2);
}

// Cross-extension precedence: a texture carrying BOTH KHR_texture_basisu and
// EXT_texture_webp must pick the KTX2 source (KTX2 -> WebP -> plain).

TEST(gltf_webp, basisu_preferred_over_webp)
{
    Bin bin;
    bin.add_floats({ -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f });           // bufferView 0
    const size_t ktx_off = bin.add_bytes(k_ktx2_uastc_grid6, k_ktx2_uastc_grid6_len);      // bufferView 1
    const size_t webp_off = bin.add_bytes(k_webp_lossless_quad, k_webp_lossless_quad_len); // bufferView 2
    const auto S = [](size_t v) { return std::to_string(v); };

    std::string json;
    json += R"({"asset":{"version":"2.0"},"extensionsUsed":["KHR_texture_basisu","EXT_texture_webp"],)";
    json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
    json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
    // KTX2 source 0 (6x6) and WebP source 1 (4x4). The 6x6 dimensions prove KTX2 won.
    json += R"("textures":[{"extensions":{"KHR_texture_basisu":{"source":0},"EXT_texture_webp":{"source":1}}}],)";
    json += R"("images":[{"bufferView":1,"mimeType":"image/ktx2"},{"bufferView":2,"mimeType":"image/webp"}],)";
    json += R"("accessors":[)" + std::string(kTriPositionsAccessor) + "],";
    json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)";
    json += R"({"buffer":0,"byteOffset":)" + S(ktx_off) + R"(,"byteLength":)" + S(k_ktx2_uastc_grid6_len) + "},";
    json += R"({"buffer":0,"byteOffset":)" + S(webp_off) + R"(,"byteLength":)" + S(k_webp_lossless_quad_len) + "}],";
    json += R"("buffers":[{"byteLength":)" + S(bin.data.size()) + "}]}";

    const std::string glb = assemble_glb(json, bin.data);
    TmpFile f(tmp_path("rast_webp_vs_ktx2.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0);
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_EQ(t.width, 6); // KTX2 (6x6) chosen over WebP (4x4)
    ASSERT_EQ(t.height, 6);
}

// External (.gltf + sidecar .webp) routed by content sniff, not extension.

TEST(gltf_webp, external_uri_webp_decodes)
{
    std::string pos;
    for (float v : { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f })
    {
        emit_f32_le(pos, v);
    }
    TmpFile bin_file(tmp_path("rast_webp_ext.bin"), pos.data(), pos.size());
    TmpFile img_file(tmp_path("rast_webp_ext.webp"), k_webp_rect, k_webp_rect_len);

    std::string json;
    json += R"({"asset":{"version":"2.0"},"extensionsUsed":["EXT_texture_webp"],)";
    json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
    json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
    json += R"("textures":[{"extensions":{"EXT_texture_webp":{"source":0}}}],)";
    json += R"("images":[{"uri":"rast_webp_ext.webp"}],)";
    json += R"("accessors":[)" + std::string(kTriPositionsAccessor) + "],";
    json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)";
    json += R"("buffers":[{"uri":"rast_webp_ext.bin","byteLength":36}]})";

    TmpFile gltf_file(tmp_path("rast_webp_ext.gltf"), json);
    Mesh m = load_ok(gltf_file.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0);
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_EQ(t.width, 6);
    ASSERT_EQ(t.height, 2);
}

// Fail-loud: a corrupt WebP with no fallback drops the texture slot (-> -1) but
// the model still loads, matching how a failed stb/KTX2 decode is handled.

TEST(gltf_webp, corrupt_webp_drops_texture_but_loads)
{
    const std::string glb = embedded_webp_glb(k_webp_corrupt_body, k_webp_corrupt_body_len);
    TmpFile f(tmp_path("rast_webp_corrupt.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1); // decode failed -> slot dropped, load still ok
    ASSERT_TRUE(m.textures.empty());
}

// Animated WebP is not a still texture: decode_webp_rgba rejects it, so with no
// fallback the slot drops and the model still loads (end-to-end of the still-only
// contract through the loader).

TEST(gltf_webp, animated_webp_dropped_but_loads)
{
    const std::string glb = embedded_webp_glb(k_webp_animated, k_webp_animated_len);
    TmpFile f(tmp_path("rast_webp_animated.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1); // animation rejected -> slot dropped, load ok
    ASSERT_TRUE(m.textures.empty());
}

// A WebP carried by an inline base64 data: URI decodes, proving the data-URI
// path routes through decode_bytes to libwebp (the is_webp sniff branch), the
// third decode_bytes branch the data-URI tests cover (KTX2 and stb are covered
// in test_gltf_ktx2.cpp and test_gltf.cpp).

TEST(gltf_webp, data_uri_webp_decodes)
{
    const std::string uri = "data:image/webp;base64," + b64encode(k_webp_rect, k_webp_rect_len);

    Bin bin;
    bin.add_floats({ -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f }); // bufferView 0
    std::string json;
    json += R"({"asset":{"version":"2.0"},"extensionsUsed":["EXT_texture_webp"],)";
    json += R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
    json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
    json += R"("textures":[{"extensions":{"EXT_texture_webp":{"source":0}}}],)";
    json += R"("images":[{"uri":")" + uri + R"(","mimeType":"image/webp"}],)";
    json += R"("accessors":[)" + std::string(kTriPositionsAccessor) + "],";
    json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)";
    json += R"("buffers":[{"byteLength":36}]})";

    const std::string glb = assemble_glb(json, bin.data);
    TmpFile f(tmp_path("rast_webp_datauri.glb"), glb.data(), glb.size());
    Mesh m = load_ok(f.path);

    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0);
    ASSERT_TRUE(idx < static_cast<int>(m.textures.size()));
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(t.width, 6);
    ASSERT_EQ(t.height, 2);
}
