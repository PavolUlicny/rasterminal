#include "tests/test.h"
#include "tests/gltf_test_util.h"
#include "tests/inline_bmp.h"
#include "tests/loader_util.h"
#include "src/loaders/mesh.h"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// windows.h comes after every project header because it defines near and far as macros.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near
#undef far
#endif

// Each loader opens a model, and any textures or buffers it references, from a directory that
// Windows opens only with a manifest setting. A name no ANSI code page can represent needs the
// UTF-8 process code page, and a path past MAX_PATH needs long-path awareness. Names use byte
// escapes because MSVC without /utf-8 re-encodes source literals.

namespace
{
    struct FileNames
    {
        std::string model;
        std::string texture;
        // The texture name as a glTF image URI.
        std::string texture_uri;
    };

    // "模型" and "纹理é.bmp"
    const FileNames kNonAsciiNames{ "\xe6\xa8\xa1\xe5\x9e\x8b", "\xe7\xba\xb9\xe7\x90\x86\xc3\xa9.bmp",
                                    "%E7%BA%B9%E7%90%86%C3%A9.bmp" };
    const FileNames kAsciiNames{ "model", "texture.bmp", "texture.bmp" };

    // "rasterminal_café_纹_한". The CJK code pages lack Hangul, except Korean 949, whose Han
    // characters are traditional forms and exclude simplified 纹.
    std::filesystem::path non_ascii_dir()
    {
        return std::filesystem::u8path("rasterminal_caf\xc3\xa9_\xe7\xba\xb9_\xed\x95\x9c");
    }

    // Two components keep each one under the 255-character component limit.
    std::filesystem::path long_dir()
    {
        return std::filesystem::path("rasterminal_long_path") / std::string(120, 'a') / std::string(120, 'b');
    }

    // Windows lifts MAX_PATH for a long-path-aware process only under this machine policy, so a
    // machine without it cannot run the long-path cases. With the policy set, a failure there
    // points at a missing longPathAware manifest entry.
    bool long_paths_supported()
    {
#ifdef _WIN32
        DWORD value = 0;
        DWORD size = sizeof value;
        return RegGetValueW(
                   HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"LongPathsEnabled",
                   RRF_RT_REG_DWORD, nullptr, &value, &size
               ) == ERROR_SUCCESS &&
               value == 1;
#else
        return true;
#endif
    }

    // Fixtures are written through std::filesystem::path so that setup does not depend on the
    // process code page that the loaders are tested against. Cleanup removes each written file
    // and directory by name because MinGW's directory listing, which remove_all uses, fails past
    // MAX_PATH. Setup ignores the same failure on a leftover tree, since every case rewrites the
    // files it loads.
    struct ModelDir
    {
        std::filesystem::path root;
        std::filesystem::path dir;
        FileNames names;
        std::vector<std::filesystem::path> files;

        ModelDir(const std::filesystem::path &relative, FileNames file_names)
            : root(std::filesystem::temp_directory_path() / *relative.begin()),
              dir(std::filesystem::temp_directory_path() / relative), names(std::move(file_names))
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(dir);
        }
        ~ModelDir()
        {
            std::error_code ec;
            for (const std::filesystem::path &file : files)
            {
                std::filesystem::remove(file, ec);
            }
            for (std::filesystem::path p = dir; p != root.parent_path(); p = p.parent_path())
            {
                std::filesystem::remove(p, ec);
            }
        }
        ModelDir(const ModelDir &) = delete;
        ModelDir &operator=(const ModelDir &) = delete;
        ModelDir(ModelDir &&) = delete;
        ModelDir &operator=(ModelDir &&) = delete;

        void write(const std::string &name, const void *data, size_t n)
        {
            files.push_back(dir / std::filesystem::u8path(name));
            std::ofstream out(files.back(), std::ios::binary);
            out.write(static_cast<const char *>(data), static_cast<std::streamsize>(n));
            out.close();
            ASSERT_TRUE(!out.fail());
        }
        void write(const std::string &name, const std::string &text) { write(name, text.data(), text.size()); }
        void write_red_texture() { write(names.texture, k1x1_red_bmp, sizeof(k1x1_red_bmp)); }

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

    void check_obj_with_material_and_texture(ModelDir &d)
    {
        const std::string &model = d.names.model;
        d.write_red_texture();
        d.write(model + ".mtl", "newmtl red\nKd 1 1 1\nmap_Kd " + d.names.texture + "\n");
        d.write(
            model + ".obj", "mtllib " + model + ".mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nusemtl red\nf 1/1 2/1 3/1\n"
        );
        const Mesh m = load_ok(d.path(model + ".obj"));
        assert_red_texture(m, 1);
    }

    void check_gltf_with_external_buffer_and_image(ModelDir &d)
    {
        const std::string &model = d.names.model;
        std::string bin;
        emit_tri_verts(bin);
        d.write(model + ".bin", bin);
        d.write_red_texture();
        // The buffer URI carries the raw name and the image URI may be percent-encoded;
        // exporters write both.
        std::string json;
        json += R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
        json += R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)";
        json += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)";
        json += R"("textures":[{"source":0}],"images":[{"uri":")" + d.names.texture_uri + R"("}],)";
        json += R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)";
        json += R"("min":[-1,-1,0],"max":[1,1,0]}],)";
        json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)";
        json += R"("buffers":[{"uri":")" + model + R"(.bin","byteLength":36}]})";
        d.write(model + ".gltf", json);
        const Mesh m = load_ok(d.path(model + ".gltf"));
        assert_red_texture(m, 1);
    }

    void check_ply_with_texture_file(ModelDir &d)
    {
        d.write_red_texture();
        d.write(
            d.names.model + ".ply", "ply\nformat ascii 1.0\ncomment TextureFile " + d.names.texture +
                                        "\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
                                        "property float u\nproperty float v\n"
                                        "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
                                        "0 0 0 0 0\n1 0 0 1 0\n0 1 0 0 1\n3 0 1 2\n"
        );
        const Mesh m = load_ok(d.path(d.names.model + ".ply"));
        assert_red_texture(m, 0);
    }

    // The two STL forms open the file through different readers.
    void check_ascii_and_binary_stl(ModelDir &d)
    {
        const std::string &model = d.names.model;
        d.write(
            model + "_ascii.stl", "solid t\nfacet normal 0 0 1\nouter loop\n"
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
        d.write(model + "_binary.stl", binary);

        (void)load_ok(d.path(model + "_ascii.stl"));
        (void)load_ok(d.path(model + "_binary.stl"));
    }

    // Assimp opens the model itself and decodes its path as UTF-8 under any code page. The
    // texture is read by rasterminal through the CRT, which depends on the process code page.
    void check_assimp_format_with_texture(ModelDir &d)
    {
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
                              d.names.texture +
                              R"("; }
      }
    }
  }
})";
        d.write(d.names.model + ".x", x);
        const Mesh m = load_ok(d.path(d.names.model + ".x"));
        assert_red_texture(m, m.triangles.front().material_idx);
    }

    void run_from_long_dir(void (*check)(ModelDir &))
    {
        if (!long_paths_supported())
        {
            // The harness has no skip state, so say once why these cases pass without loading.
            static bool reported = false;
            if (!reported)
            {
                std::fprintf(stderr, "long_paths: LongPathsEnabled is off, so long-path cases are skipped\n");
                reported = true;
            }
            return;
        }
        ModelDir d(long_dir(), kAsciiNames);
        ASSERT_TRUE(d.dir.u8string().size() > 260);
        check(d);
    }
} // namespace

TEST(non_ascii_paths, obj_with_material_and_texture)
{
    ModelDir d(non_ascii_dir(), kNonAsciiNames);
    check_obj_with_material_and_texture(d);
}

TEST(non_ascii_paths, gltf_with_external_buffer_and_image)
{
    ModelDir d(non_ascii_dir(), kNonAsciiNames);
    check_gltf_with_external_buffer_and_image(d);
}

TEST(non_ascii_paths, ply_with_texture_file)
{
    ModelDir d(non_ascii_dir(), kNonAsciiNames);
    check_ply_with_texture_file(d);
}

TEST(non_ascii_paths, ascii_and_binary_stl)
{
    ModelDir d(non_ascii_dir(), kNonAsciiNames);
    check_ascii_and_binary_stl(d);
}

TEST(non_ascii_paths, assimp_format_with_texture)
{
    ModelDir d(non_ascii_dir(), kNonAsciiNames);
    check_assimp_format_with_texture(d);
}

TEST(long_paths, obj_with_material_and_texture)
{
    run_from_long_dir(check_obj_with_material_and_texture);
}

TEST(long_paths, gltf_with_external_buffer_and_image)
{
    run_from_long_dir(check_gltf_with_external_buffer_and_image);
}

TEST(long_paths, ply_with_texture_file)
{
    run_from_long_dir(check_ply_with_texture_file);
}

TEST(long_paths, ascii_and_binary_stl)
{
    run_from_long_dir(check_ascii_and_binary_stl);
}

TEST(long_paths, assimp_format_with_texture)
{
    run_from_long_dir(check_assimp_format_with_texture);
}
