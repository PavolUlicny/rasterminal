#include "tests/test.h"
#include "tests/gltf_test_util.h"
#include "tests/inline_bmp.h"
#include "tests/loader_util.h"
#include "src/loaders/mesh.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <system_error>

// Each loader opens a model, and any textures or buffers it references, from a directory whose
// name no ANSI code page can represent. On Windows these files open only when the process code
// page is UTF-8. Names use byte escapes because MSVC without /utf-8 re-encodes source literals.

namespace
{
    // "rasterminal_café_纹_한". The CJK code pages lack Hangul, except Korean 949, whose Han
    // characters are traditional forms and exclude simplified 纹.
    const std::string kDirName = "rasterminal_caf\xc3\xa9_\xe7\xba\xb9_\xed\x95\x9c";
    // "模型"
    const std::string kModel = "\xe6\xa8\xa1\xe5\x9e\x8b";
    // "纹理é.bmp", and its percent-encoded form for the glTF image URI
    const std::string kTexture = "\xe7\xba\xb9\xe7\x90\x86\xc3\xa9.bmp";
    const std::string kTexturePercent = "%E7%BA%B9%E7%90%86%C3%A9.bmp";

    // Fixtures are written through std::filesystem::path so that setup does not depend on the
    // process code page that the loaders are tested against.
    struct NonAsciiDir
    {
        std::filesystem::path dir;

        NonAsciiDir() : dir(std::filesystem::temp_directory_path() / std::filesystem::u8path(kDirName))
        {
            std::filesystem::remove_all(dir);
            std::filesystem::create_directory(dir);
        }
        ~NonAsciiDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
        NonAsciiDir(const NonAsciiDir &) = delete;
        NonAsciiDir &operator=(const NonAsciiDir &) = delete;
        NonAsciiDir(NonAsciiDir &&) = delete;
        NonAsciiDir &operator=(NonAsciiDir &&) = delete;

        void write(const std::string &name, const void *data, size_t n) const
        {
            std::ofstream out(dir / std::filesystem::u8path(name), std::ios::binary);
            out.write(static_cast<const char *>(data), static_cast<std::streamsize>(n));
            out.close();
            ASSERT_TRUE(!out.fail());
        }
        void write(const std::string &name, const std::string &text) const { write(name, text.data(), text.size()); }
        void write_red_texture() const { write(kTexture, k1x1_red_bmp, sizeof(k1x1_red_bmp)); }

        // The UTF-8 narrow path that a command-line argument carries.
        [[nodiscard]] std::string path(const std::string &name) const { return dir.u8string() + "/" + name; }
    };

    void assert_red_texture(const Mesh &m, size_t material)
    {
        const Texture *tex = m.tex_at(m.materials[material].diffuse_map.tex);
        ASSERT_TRUE(tex != nullptr);
        ASSERT_TRUE(tex->valid());
        const vec3 c = tex->sample_rgb(0.0f, 0.0f);
        ASSERT_NEAR(c.x, 1.0f, 1e-3f);
        ASSERT_NEAR(c.y, 0.0f, 1e-3f);
    }
} // namespace

TEST(non_ascii_paths, obj_with_material_and_texture)
{
    const NonAsciiDir d;
    d.write_red_texture();
    d.write(kModel + ".mtl", "newmtl red\nKd 1 1 1\nmap_Kd " + kTexture + "\n");
    d.write(
        kModel + ".obj", "mtllib " + kModel + ".mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nusemtl red\nf 1/1 2/1 3/1\n"
    );
    const Mesh m = load_ok(d.path(kModel + ".obj"));
    assert_red_texture(m, 1);
}

TEST(non_ascii_paths, gltf_with_external_buffer_and_image)
{
    const NonAsciiDir d;
    std::string bin;
    emit_tri_verts(bin);
    d.write(kModel + ".bin", bin);
    d.write_red_texture();
    // The buffer URI carries raw UTF-8 and the image URI is percent-encoded; exporters write both.
    std::string json;
    json += R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
    json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
    json += R"("textures":[{"source":0}],"images":[{"uri":")" + kTexturePercent + R"("}],)";
    json += R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)";
    json += R"("min":[-1,-1,0],"max":[1,1,0]}],)";
    json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)";
    json += R"("buffers":[{"uri":")" + kModel + R"(.bin","byteLength":36}]})";
    d.write(kModel + ".gltf", json);
    const Mesh m = load_ok(d.path(kModel + ".gltf"));
    assert_red_texture(m, 1);
}

TEST(non_ascii_paths, ply_with_texture_file)
{
    const NonAsciiDir d;
    d.write_red_texture();
    d.write(
        kModel + ".ply", "ply\nformat ascii 1.0\ncomment TextureFile " + kTexture +
                             "\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
                             "property float u\nproperty float v\n"
                             "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
                             "0 0 0 0 0\n1 0 0 1 0\n0 1 0 0 1\n3 0 1 2\n"
    );
    const Mesh m = load_ok(d.path(kModel + ".ply"));
    assert_red_texture(m, 0);
}

TEST(non_ascii_paths, ascii_and_binary_stl)
{
    // The two STL forms open the file through different readers.
    const NonAsciiDir d;
    d.write(
        kModel + "_ascii.stl", "solid t\nfacet normal 0 0 1\nouter loop\n"
                               "vertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\n"
                               "endloop\nendfacet\nendsolid t\n"
    );
    std::string binary(80, '\0');
    binary += std::string("\x01\x00\x00\x00", 4);
    for (const float v : { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f })
    {
        emit_f32_le(binary, v);
    }
    binary += std::string(2, '\0');
    d.write(kModel + "_binary.stl", binary);

    (void)load_ok(d.path(kModel + "_ascii.stl"));
    (void)load_ok(d.path(kModel + "_binary.stl"));
}

TEST(non_ascii_paths, assimp_format_with_texture)
{
    // Assimp opens the model itself and decodes its path as UTF-8 under any code page. The
    // texture is read by rasterminal through the CRT, which depends on the process code page.
    const NonAsciiDir d;
    d.write_red_texture();
    const std::string x = R"(xof 0303txt 0032
Frame Root {
  Mesh Triangle {
    3;
    0;0;0;,
    1;0;0;,
    0;1;0;;
    1;
    3;0,1,2;;
    MeshTextureCoords {
      3;
      0;0;,
      1;0;,
      0;1;;
    }
    MeshMaterialList {
      1;
      1;
      0;
      Material {
        1;1;1;1;;
        0.0;
        0;0;0;;
        0;0;0;;
        TextureFilename { ")" +
                          kTexture +
                          R"("; }
      }
    }
  }
})";
    d.write(kModel + ".x", x);
    const Mesh m = load_ok(d.path(kModel + ".x"));
    assert_red_texture(m, m.triangles.front().material_idx);
}
