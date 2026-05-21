#include "mesh.h"
#include "light.h"
#include "mesh_loader.h"
#include "texture.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

bool Mesh::load_gltf(const std::string &path, int n_threads)
{
    MeshSnapshot snap(*this);

    // Resolve external files relative to the glTF directory.
    std::string dir;
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos)
    {
        dir = path.substr(0, slash + 1);
    }

    const cgltf_options opts{};
    cgltf_data *data = nullptr;

    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success)
    {
        return false;
    }

    // RAII guard — ensures cgltf_free on every exit path.
    const auto guard = std::unique_ptr<cgltf_data, decltype(&cgltf_free)>(data, cgltf_free);

    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success)
    {
        return false;
    }
    if (cgltf_validate(data) != cgltf_result_success)
    {
        return false;
    }

    // Default white material at index 0.
    materials.push_back(Material{});

    // Register a texture by cgltf_image, returning its slot index. Each distinct
    // image is registered once (dedup); the actual decode is deferred and run in
    // parallel after the scene walk. Slot order follows first-encounter order.
    std::unordered_map<const cgltf_image *, int> tex_cache;
    std::vector<const cgltf_image *> tex_requests;
    auto load_tex = [&](const cgltf_image *img) -> int
    {
        if (!img)
        {
            return -1;
        }
        const auto it = tex_cache.find(img);
        if (it != tex_cache.end())
        {
            return it->second;
        }
        const int idx = static_cast<int>(tex_requests.size());
        tex_requests.push_back(img);
        tex_cache.emplace(img, idx);
        return idx;
    };

    // Map a cgltf_material to our Blinn-Phong Material.
    auto map_mat = [&](const cgltf_material *m) -> Material
    {
        Material mat{};
        if (!m)
        {
            return mat;
        }
        const cgltf_pbr_metallic_roughness &pbr = m->pbr_metallic_roughness;
        mat.diffuse = { pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2] };
        mat.ambient = mat.diffuse;
        const float mf = pbr.metallic_factor;
        mat.specular = { mf, mf, mf };
        mat.shininess = roughness_to_shininess(pbr.roughness_factor);
        mat.metallic = mf;
        mat.roughness = pbr.roughness_factor;
        if (pbr.metallic_roughness_texture.texture)
        {
            mat.metallic_roughness_tex = load_tex(pbr.metallic_roughness_texture.texture->image);
        }
        if (pbr.base_color_texture.texture)
        {
            mat.diffuse_tex = load_tex(pbr.base_color_texture.texture->image);
        }
        if (m->normal_texture.texture)
        {
            mat.normal_tex = load_tex(m->normal_texture.texture->image);
        }
        mat.double_sided = m->double_sided;
        if (m->alpha_mode == cgltf_alpha_mode_mask)
        {
            mat.alpha_cutoff = m->alpha_cutoff;
        }
        return mat;
    };

    bool has_normals = false;

    // Walk the scene graph recursively, applying world transforms.
    std::function<void(const cgltf_node *)> visit = [&](const cgltf_node *node)
    {
        if (node->mesh)
        {
            float w[16];
            cgltf_node_transform_world(node, w);
            // w is column-major: element [col*4+row].
            // Position transform: p' = w * [px, py, pz, 1]
            // Normal transform:   n' = normalize(w * [nx, ny, nz, 0])
            // Correct for uniform scale; sufficient for a viewer.

            for (size_t pi = 0; pi < node->mesh->primitives_count; pi++)
            {
                const cgltf_primitive &prim = node->mesh->primitives[pi];
                if (prim.type != cgltf_primitive_type_triangles)
                {
                    continue;
                }

                const uint32_t mat_idx = [&]() -> uint32_t
                {
                    if (!prim.material)
                    {
                        return 0u;
                    }
                    const auto idx = static_cast<uint32_t>(materials.size());
                    materials.push_back(map_mat(prim.material));
                    return idx;
                }();

                const cgltf_accessor *pos_acc = nullptr;
                const cgltf_accessor *norm_acc = nullptr;
                const cgltf_accessor *uv_acc = nullptr;
                const cgltf_accessor *color_acc = nullptr;

                for (size_t ai = 0; ai < prim.attributes_count; ai++)
                {
                    const cgltf_attribute &attr = prim.attributes[ai];
                    if (attr.type == cgltf_attribute_type_position)
                    {
                        pos_acc = attr.data;
                    }
                    else if (attr.type == cgltf_attribute_type_normal)
                    {
                        norm_acc = attr.data;
                    }
                    else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0)
                    {
                        uv_acc = attr.data;
                    }
                    else if (attr.type == cgltf_attribute_type_color && attr.index == 0)
                    {
                        color_acc = attr.data;
                    }
                }

                if (!pos_acc)
                {
                    continue;
                }

                const size_t n_verts = pos_acc->count;
                const size_t vert_base = vertices.size();

                vertices.reserve(vertices.size() + n_verts);
                for (size_t i = 0; i < n_verts; i++)
                {
                    Vertex v{};

                    float p[3];
                    cgltf_accessor_read_float(pos_acc, i, p, 3);
                    v.pos.x = w[0] * p[0] + w[4] * p[1] + w[8] * p[2] + w[12];
                    v.pos.y = w[1] * p[0] + w[5] * p[1] + w[9] * p[2] + w[13];
                    v.pos.z = w[2] * p[0] + w[6] * p[1] + w[10] * p[2] + w[14];

                    if (norm_acc)
                    {
                        float n[3];
                        cgltf_accessor_read_float(norm_acc, i, n, 3);
                        v.normal.x = w[0] * n[0] + w[4] * n[1] + w[8] * n[2];
                        v.normal.y = w[1] * n[0] + w[5] * n[1] + w[9] * n[2];
                        v.normal.z = w[2] * n[0] + w[6] * n[1] + w[10] * n[2];
                        const float len = v.normal.length();
                        if (len > 1e-6f)
                        {
                            v.normal = v.normal * (1.0f / len);
                        }
                    }

                    if (uv_acc)
                    {
                        float uv[2];
                        cgltf_accessor_read_float(uv_acc, i, uv, 2);
                        v.uv = { uv[0], 1.0f - uv[1] };
                    }

                    v.ao = 1.0f;
                    vertices.push_back(v);
                }

                if (norm_acc)
                {
                    has_normals = true;
                }

                if (color_acc)
                {
                    vertex_colors.resize(vert_base + n_verts, { 1.0f, 1.0f, 1.0f });
                    for (size_t i = 0; i < n_verts; i++)
                    {
                        float c[4];
                        cgltf_accessor_read_float(color_acc, i, c, 4);
                        vertex_colors[vert_base + i] = { c[0], c[1], c[2] };
                    }
                    has_vertex_colors = true;
                }

                // Push triangles.
                if (prim.indices)
                {
                    const size_t n_idx = prim.indices->count;
                    triangles.reserve(triangles.size() + n_idx / 3);
                    for (size_t i = 0; i + 2 < n_idx; i += 3)
                    {
                        Triangle t;
                        t.v[0] = static_cast<uint32_t>(vert_base + cgltf_accessor_read_index(prim.indices, i));
                        t.v[1] = static_cast<uint32_t>(vert_base + cgltf_accessor_read_index(prim.indices, i + 1));
                        t.v[2] = static_cast<uint32_t>(vert_base + cgltf_accessor_read_index(prim.indices, i + 2));
                        t.material_idx = mat_idx;
                        triangles.push_back(t);
                    }
                }
                else
                {
                    // Non-indexed: every 3 vertices form a triangle.
                    triangles.reserve(triangles.size() + n_verts / 3);
                    for (size_t i = 0; i + 2 < n_verts; i += 3)
                    {
                        Triangle t;
                        t.v[0] = static_cast<uint32_t>(vert_base + i);
                        t.v[1] = static_cast<uint32_t>(vert_base + i + 1);
                        t.v[2] = static_cast<uint32_t>(vert_base + i + 2);
                        t.material_idx = mat_idx;
                        triangles.push_back(t);
                    }
                }
            }
        }

        for (size_t i = 0; i < node->children_count; i++)
        {
            visit(node->children[i]);
        }
    };

    const cgltf_scene *scene = data->scene ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (scene)
    {
        for (size_t i = 0; i < scene->nodes_count; i++)
        {
            visit(scene->nodes[i]);
        }
    }

    if (vertices.empty() || triangles.empty())
    {
        return false;
    }

    if (!has_normals)
    {
        compute_normals();
    }

    // Decode all registered images in parallel. data (held by guard) and dir
    // outlive the join, so worker reads of buffer_view->buffer->data are valid.
    decode_textures(
        textures, materials, tex_requests.size(), n_threads,
        [&](size_t i) -> Texture
        {
            const cgltf_image *img = tex_requests[i];
            Texture tex;
            if (img->uri && img->uri[0] != '\0')
            {
                (void)tex.load(dir + img->uri);
            }
            else if (img->buffer_view)
            {
                // cgltf_buffer_view_data honours EXT_meshopt_compression overrides.
                const uint8_t *ptr = cgltf_buffer_view_data(img->buffer_view);
                (void)tex.load_from_memory(ptr, img->buffer_view->size);
            }
            return tex;
        }
    );

    snap.commit();
    return true;
}
