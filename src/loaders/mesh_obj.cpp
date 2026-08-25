#include "src/loaders/mesh.h"
#include "src/loaders/mesh_loader.h"
#include "src/math/light.h"
#include "src/render/texture.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

namespace
{
    // tinyobjloader maps absent smoothing and explicit `s off` to zero. Scan the source
    // to decide whether zero means crease-angle smoothing or authored faceting.
    bool obj_has_smoothing_directive(const std::string &path)
    {
        std::ifstream in(path);
        if (!in)
        {
            return false;
        }
        std::string line;
        while (std::getline(in, line))
        {
            size_t i = 0;
            while (i < line.size() && (std::isspace(static_cast<unsigned char>(line[i])) != 0))
            {
                i++;
            }
            // Match the complete `s` token, not prefixes such as `surf`.
            if (i >= line.size() || line[i] != 's')
            {
                continue;
            }
            size_t j = i + 1;
            if (j < line.size() && (std::isspace(static_cast<unsigned char>(line[j])) == 0))
            {
                continue;
            }
            // Ignore a malformed bare `s`.
            while (j < line.size() && (std::isspace(static_cast<unsigned char>(line[j])) != 0))
            {
                j++;
            }
            if (j < line.size())
            {
                return true;
            }
        }
        return false;
    }

    // Bake MTL scale and offset into the shared 2x3 UV transform. OBJ UVs are already v-up,
    // so this path needs none of glTF's flip conversion. Ignore 3-D and turbulence terms.
    void bake_obj_transform(TexSlot &slot, const tinyobj::texture_option_t &opt)
    {
        const float sx = opt.scale[0];
        const float sy = opt.scale[1];
        const float ox = opt.origin_offset[0];
        const float oy = opt.origin_offset[1];
        if (sx == 1.0f && sy == 1.0f && ox == 0.0f && oy == 0.0f)
        {
            return;
        }
        slot.has_transform = true;
        slot.t[0] = sx;
        slot.t[1] = 0.0f;
        slot.t[2] = ox;
        slot.t[3] = 0.0f;
        slot.t[4] = sy;
        slot.t[5] = oy;
    }
} // namespace

bool Mesh::load_obj(const std::string &path, int n_threads, float crease_cos)
{
    MeshSnapshot snap(*this);

    // Texture paths in MTL are resolved relative to the OBJ file's directory.
    const std::string obj_dir = dir_of(path);

    tinyobj::ObjReaderConfig cfg;
    cfg.mtl_search_path = obj_dir;
    cfg.triangulate = true;

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, cfg))
    {
        return false;
    }

    const auto &attrib = reader.GetAttrib();
    const auto &shapes = reader.GetShapes();
    const auto &mats = reader.GetMaterials();

    // tinyobjloader only warns about out-of-range face indices; reject them here.
    const size_t n_pos = attrib.vertices.size() / 3;
    const size_t n_nor = attrib.normals.size() / 3;
    const size_t n_tex = attrib.texcoords.size() / 2;
    for (const auto &shape : shapes)
    {
        for (const auto &idx : shape.mesh.indices)
        {
            if ((idx.vertex_index >= 0 && static_cast<size_t>(idx.vertex_index) >= n_pos) ||
                (idx.normal_index >= 0 && static_cast<size_t>(idx.normal_index) >= n_nor) ||
                (idx.texcoord_index >= 0 && static_cast<size_t>(idx.texcoord_index) >= n_tex))
            {
                return false;
            }
        }
    }

    materials.push_back(Material{});

    // Deduplicate texture requests by path and clamp mode, then decode them in parallel.
    struct ObjTexRequest
    {
        std::string path;
        bool clamp;
    };
    std::unordered_map<std::string, int> tex_cache;
    std::vector<ObjTexRequest> tex_requests;

    // Defer bump classification until pixels are decoded. Material indices survive compaction.
    struct BumpBinding
    {
        size_t material_index;
        float bump_multiplier;
        char imfchan;
    };
    std::vector<BumpBinding> bump_bindings;
    auto load_tex = [&](const std::string &name, bool clamp) -> int
    {
        if (name.empty())
        {
            return -1;
        }
        const std::string key = (clamp ? "1:" : "0:") + name;
        const auto it = tex_cache.find(key);
        if (it != tex_cache.end())
        {
            return it->second;
        }
        const int idx = static_cast<int>(tex_requests.size());
        tex_requests.push_back({ obj_dir + name, clamp });
        tex_cache.emplace(key, idx);
        return idx;
    };

    // Reserve material zero for the default.
    for (const auto &m : mats)
    {
        Material mat{};
        mat.diffuse = { m.diffuse[0], m.diffuse[1], m.diffuse[2] };
        mat.specular = { m.specular[0], m.specular[1], m.specular[2] };
        mat.shininess = m.shininess;
        // Use diffuse when ambient is absent or zero.
        const bool ka_zero = (m.ambient[0] == 0.0f && m.ambient[1] == 0.0f && m.ambient[2] == 0.0f);
        mat.ambient = ka_zero ? mat.diffuse : vec3{ m.ambient[0], m.ambient[1], m.ambient[2] };
        mat.diffuse_map.tex = load_tex(m.diffuse_texname, m.diffuse_texopt.clamp);
        bake_obj_transform(mat.diffuse_map, m.diffuse_texopt);
        mat.specular_map.tex = load_tex(m.specular_texname, m.specular_texopt.clamp);
        bake_obj_transform(mat.specular_map, m.specular_texopt);
        // `norm` wins. Exporters often put RGB normal maps in map_Bump, so classify map_Bump
        // after decoding and convert only scalar height maps.
        if (!m.normal_texname.empty())
        {
            mat.normal_map.tex = load_tex(m.normal_texname, m.normal_texopt.clamp);
            bake_obj_transform(mat.normal_map, m.normal_texopt);
        }
        else if (!m.bump_texname.empty())
        {
            // Clamp -bm before deduplication so equal effective strengths share a conversion.
            // Ignore rare -mm modulation; -bm controls bump strength.
            mat.normal_map.tex = load_tex(m.bump_texname, m.bump_texopt.clamp);
            bake_obj_transform(mat.normal_map, m.bump_texopt);
            const float bm = std::clamp(m.bump_texopt.bump_multiplier, -1e6f, 1e6f);
            // Accept uppercase -imfchan values instead of silently treating them as luminance.
            const char imfchan = static_cast<char>(std::tolower(static_cast<unsigned char>(m.bump_texopt.imfchan)));
            bump_bindings.push_back({ materials.size(), bm, imfchan });
        }
        // Bound emission before a zero texture channel can turn an infinite factor into NaN.
        mat.emissive = { std::clamp(m.emission[0], 0.0f, 1e6f), std::clamp(m.emission[1], 0.0f, 1e6f),
                         std::clamp(m.emission[2], 0.0f, 1e6f) };
        // A zero Ke nullifies map_Ke, so skip an otherwise unused decode.
        const bool emissive_active = (mat.emissive.x > 0.0f || mat.emissive.y > 0.0f || mat.emissive.z > 0.0f);
        if (emissive_active)
        {
            mat.emissive_map.tex = load_tex(m.emissive_texname, m.emissive_texopt.clamp);
            bake_obj_transform(mat.emissive_map, m.emissive_texopt);
        }
        // map_d selects cutout using map_Kd alpha. Otherwise d/Tr selects scalar blending.
        if (!m.alpha_texname.empty())
        {
            mat.alpha_cutoff = 0.5f;
        }
        else if (m.dissolve < 1.0f)
        {
            mat.blend = true;
            // Preserve authored d/Tr exactly, including fully invisible d=0.
            mat.alpha = m.dissolve;
        }
        materials.push_back(mat);
    }

    // Deduplicate identical position, normal and UV index tuples.
    std::unordered_map<size_t, uint32_t> vert_cache;
    vert_cache.reserve(attrib.vertices.size() / 3);

    bool all_have_normals = !attrib.normals.empty();

    // Preserve source position IDs so computed normals can cross UV seams.
    std::vector<uint32_t> weld;
    weld.reserve(n_pos);

    auto get_vertex = [&](const tinyobj::index_t &idx) -> uint32_t
    {
        const size_t key = (static_cast<size_t>(idx.vertex_index + 1) * size_t{ 2654435761 }) ^
                           (static_cast<size_t>(idx.normal_index + 1) * size_t{ 2246822519 }) ^
                           (static_cast<size_t>(idx.texcoord_index + 1) * size_t{ 3266489917 });

        const auto [it, inserted] = vert_cache.emplace(key, static_cast<uint32_t>(vertices.size()));
        if (!inserted)
        {
            return it->second;
        }

        Vertex v{};
        if (idx.vertex_index >= 0)
        {
            const size_t vi = static_cast<size_t>(idx.vertex_index) * 3;
            v.pos = { attrib.vertices[vi], attrib.vertices[vi + 1], attrib.vertices[vi + 2] };
        }
        if (idx.normal_index >= 0)
        {
            const size_t ni = static_cast<size_t>(idx.normal_index) * 3;
            v.normal = { attrib.normals[ni], attrib.normals[ni + 1], attrib.normals[ni + 2] };
        }
        else
        {
            all_have_normals = false;
        }
        if (idx.texcoord_index >= 0)
        {
            const size_t ti = static_cast<size_t>(idx.texcoord_index) * 2;
            v.uv = { attrib.texcoords[ti], attrib.texcoords[ti + 1] };
        }
        v.ao = 1.0f;
        vertices.push_back(v);
        weld.push_back(idx.vertex_index >= 0 ? static_cast<uint32_t>(idx.vertex_index) : 0u);
        return it->second;
    };

    // Check for OBJ vertex colors ("v x y z r g b" extension).
    const bool src_has_vcol = (!attrib.colors.empty() && attrib.colors.size() == attrib.vertices.size());

    for (const auto &shape : shapes)
    {
        size_t idx_off = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++)
        {
            const int fv = static_cast<int>(shape.mesh.num_face_vertices[f]);
            const int mat_id = shape.mesh.material_ids[f];
            const uint32_t mat_idx =
                (mat_id >= 0 && mat_id < static_cast<int>(mats.size())) ? static_cast<uint32_t>(mat_id + 1) : 0u;

            // triangulate=true should guarantee this.
            if (fv >= 3)
            {
                std::array<uint32_t, 3> fv3{};
                for (int v = 0; v < 3; v++)
                {
                    fv3[static_cast<size_t>(v)] = get_vertex(shape.mesh.indices[idx_off + static_cast<size_t>(v)]);
                }

                Triangle t;
                t.v[0] = fv3[0];
                t.v[1] = fv3[1];
                t.v[2] = fv3[2];
                t.material_idx = mat_idx;
                triangles.push_back(t);
            }
            idx_off += static_cast<size_t>(fv);
        }
    }

    if (triangles.empty())
    {
        return false;
    }

    // Populate vertex colors when "v x y z r g b" data is present.
    if (src_has_vcol)
    {
        vertex_colors.assign(vertices.size(), { 1.0f, 1.0f, 1.0f });
        for (const auto &shape : shapes)
        {
            for (const auto &idx : shape.mesh.indices)
            {
                if (idx.vertex_index >= 0)
                {
                    const size_t ci = static_cast<size_t>(idx.vertex_index) * 3;
                    const size_t key = (static_cast<size_t>(idx.vertex_index + 1) * size_t{ 2654435761 }) ^
                                       (static_cast<size_t>(idx.normal_index + 1) * size_t{ 2246822519 }) ^
                                       (static_cast<size_t>(idx.texcoord_index + 1) * size_t{ 3266489917 });
                    const auto it = vert_cache.find(key);
                    if (it != vert_cache.end())
                    {
                        vertex_colors[it->second] = { attrib.colors[ci], attrib.colors[ci + 1], attrib.colors[ci + 2] };
                    }
                }
            }
        }
        has_vertex_colors = std::any_of(
            vertex_colors.begin(), vertex_colors.end(),
            [](const vec3 &c) { return c.x < 0.999f || c.y < 0.999f || c.z < 0.999f; }
        );
        if (!has_vertex_colors)
        {
            vertex_colors.clear();
        }
    }

    if (attrib.normals.empty() || !all_have_normals)
    {
        // Authored smoothing groups override the crease angle. Rescan only when every parsed
        // group is zero, which may mean either no directive or explicit `s off`.
        std::vector<uint32_t> smooth_groups;
        const bool any_nonzero = std::any_of(
            shapes.begin(), shapes.end(),
            [](const tinyobj::shape_t &s)
            {
                return std::any_of(
                    s.mesh.smoothing_group_ids.begin(), s.mesh.smoothing_group_ids.end(),
                    [](unsigned int id) { return id != 0u; }
                );
            }
        );
        const bool use_groups = any_nonzero || obj_has_smoothing_directive(path);
        if (use_groups)
        {
            for (const auto &shape : shapes)
            {
                for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++)
                {
                    if (shape.mesh.num_face_vertices[f] >= 3)
                    {
                        smooth_groups.push_back(shape.mesh.smoothing_group_ids[f]);
                    }
                }
            }
        }
        compute_normals(crease_cos, &weld, n_pos, use_groups ? &smooth_groups : nullptr);
    }

    // Decode resolved texture requests in parallel.
    decode_textures(
        textures, materials, tex_requests.size(), n_threads,
        [&](size_t i) -> Texture
        {
            Texture tex;
            (void)tex.load(tex_requests[i].path);
            if (tex_requests[i].clamp)
            {
                tex.wrap_s = WrapMode::Clamp;
                tex.wrap_t = WrapMode::Clamp;
            }
            return tex;
        }
    );

    // Convert scalar bump maps while preserving mislabeled RGB normal maps. An explicit
    // non-luminance channel forces height conversion. Run serially because shared bump maps
    // require classification and conversion deduplication.
    if (!bump_bindings.empty())
    {
        std::map<std::tuple<int, char, uint32_t>, int> converted; // (source, channel, bump bits) to texture
        std::map<int, bool> grayscale_of;                         // per-source classification, scanned once
        for (const auto &bind : bump_bindings)
        {
            const int src = materials[bind.material_index].normal_map.tex;
            if (src < 0)
            {
                continue; // decode dropped this texture; nothing bound
            }
            const char ch = bind.imfchan;
            const bool explicit_channel = (ch == 'r' || ch == 'g' || ch == 'b' || ch == 'm' || ch == 'z');
            if (!explicit_channel)
            {
                // Cache the full-image grayscale scan per source texture.
                const auto [cit, inserted] = grayscale_of.try_emplace(src, false);
                if (inserted)
                {
                    cit->second = is_grayscale(textures[static_cast<size_t>(src)]);
                }
                if (!cit->second)
                {
                    continue; // Keep a chromatic normal map mislabeled as bump.
                }
            }

            uint32_t bm_bits = 0;
            std::memcpy(&bm_bits, &bind.bump_multiplier, sizeof bm_bits);
            // Raw channel keys may duplicate equivalent aliases, a rare and harmless extra texture.
            const auto key = std::make_tuple(src, ch, bm_bits);
            const auto it = converted.find(key);
            if (it != converted.end())
            {
                materials[bind.material_index].normal_map.tex = it->second;
                continue;
            }

            // Keep the source because different materials may require different conversions.
            // A bump-only source may remain unreferenced; reclaiming it needs reference tracking.
            Texture nmap = height_to_normal_map(textures[static_cast<size_t>(src)], ch, bind.bump_multiplier);
            const int new_idx = static_cast<int>(textures.size());
            textures.push_back(std::move(nmap));
            converted.emplace(key, new_idx);
            materials[bind.material_index].normal_map.tex = new_idx;
        }
    }

    snap.commit();
    return true;
}
