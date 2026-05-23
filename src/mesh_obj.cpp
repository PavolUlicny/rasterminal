#include "mesh.h"
#include "light.h"
#include "mesh_loader.h"
#include "texture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

bool Mesh::load_obj(const std::string &path, int n_threads)
{
    MeshSnapshot snap(*this);

    // Texture paths in MTL are resolved relative to the OBJ file's directory.
    std::string obj_dir;
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos)
    {
        obj_dir = path.substr(0, slash + 1);
    }

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

    // Bounds-check all face indices. tinyobjloader is permissive (warns but succeeds)
    // on OOB references; we reject to avoid degenerate geometry in the renderer.
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

    // Default white material at index 0 — always present.
    materials.push_back(Material{});

    // Register a texture by name (relative to obj_dir), returning its slot index.
    // Each distinct name is registered once (dedup); the actual decode is deferred
    // and run in parallel after material parsing. Empty name -> no texture (-1).
    std::unordered_map<std::string, int> tex_cache;
    std::vector<std::string> tex_requests;
    auto load_tex = [&](const std::string &name) -> int
    {
        if (name.empty())
        {
            return -1;
        }
        const auto it = tex_cache.find(name);
        if (it != tex_cache.end())
        {
            return it->second;
        }
        const int idx = static_cast<int>(tex_requests.size());
        tex_requests.push_back(obj_dir + name);
        tex_cache.emplace(name, idx);
        return idx;
    };

    // Materials: tinyobjloader index i maps to our index i+1 (0 = default).
    for (const auto &m : mats)
    {
        Material mat{};
        mat.diffuse = { m.diffuse[0], m.diffuse[1], m.diffuse[2] };
        mat.specular = { m.specular[0], m.specular[1], m.specular[2] };
        mat.shininess = m.shininess;
        // Ka → Kd fallback: if ambient is all-zero (absent or unset), use diffuse.
        const bool ka_zero = (m.ambient[0] == 0.0f && m.ambient[1] == 0.0f && m.ambient[2] == 0.0f);
        mat.ambient = ka_zero ? mat.diffuse : vec3{ m.ambient[0], m.ambient[1], m.ambient[2] };
        mat.diffuse_tex = load_tex(m.diffuse_texname);
        mat.specular_tex = load_tex(m.specular_texname);
        // Prefer map_Kn (normal_texname); fall back to map_bump (bump_texname).
        mat.normal_tex = !m.normal_texname.empty() ? load_tex(m.normal_texname) : load_tex(m.bump_texname);
        mat.emissive = { m.emission[0], m.emission[1], m.emission[2] };
        mat.emissive_tex = load_tex(m.emissive_texname);
        // Industry-convention factor promotion (Ke unset + map_Ke present ⇒ {1,1,1}) happens
        // in Mesh::load_model() after decode_textures, so a failed map_Ke decode doesn't promote.
        // map_d present: treat map_Kd's alpha channel as an opacity mask.
        // map_d is not loaded as a separate texture — map_Kd's RGBA is used.
        if (!m.alpha_texname.empty())
        {
            mat.alpha_cutoff = 0.5f;
        }
        materials.push_back(mat);
    }

    // Vertex deduplication: identical (vertex_index, normal_index, texcoord_index)
    // tuples share one Vertex — same semantics as the previous hand-rolled loader.
    std::unordered_map<size_t, uint32_t> vert_cache;
    vert_cache.reserve(attrib.vertices.size() / 3);

    bool all_have_normals = !attrib.normals.empty();

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
        return it->second;
    };

    // Check for OBJ vertex colors ("v x y z r g b" extension).
    const bool src_has_vcol = (!attrib.colors.empty() && attrib.colors.size() == attrib.vertices.size());

    // Walk all shapes and merge into one Mesh.
    for (const auto &shape : shapes)
    {
        size_t idx_off = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++)
        {
            const int fv = static_cast<int>(shape.mesh.num_face_vertices[f]);
            const int mat_id = shape.mesh.material_ids[f];
            const uint32_t mat_idx =
                (mat_id >= 0 && mat_id < static_cast<int>(mats.size())) ? static_cast<uint32_t>(mat_id + 1) : 0u;

            // triangulate=true guarantees fv==3; guard for safety.
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
        compute_normals();
    }

    // tex_requests holds obj_dir-resolved paths, decoded in parallel.
    decode_textures(
        textures, materials, tex_requests.size(), n_threads,
        [&](size_t i) -> Texture
        {
            Texture tex;
            (void)tex.load(tex_requests[i]);
            return tex;
        }
    );

    snap.commit();
    return true;
}
