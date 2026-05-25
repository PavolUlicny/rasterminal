#include "mesh.h"
#include "light.h"
#include "linalg.h"
#include "mesh_loader.h"
#include "texture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

bool Mesh::load_gltf(const std::string &path, int n_threads, float crease_cos)
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
            mat.normal_scale = m->normal_texture.scale;
        }
        if (m->occlusion_texture.texture)
        {
            mat.occlusion_tex = load_tex(m->occlusion_texture.texture->image);
            // cgltf: scale field == occlusionTexture.strength. Spec caps it at [0,1] but cgltf
            // does not enforce; clamp so an out-of-range value can't drive ao negative per-pixel.
            mat.occlusion_strength = clamp(m->occlusion_texture.scale, 0.0f, 1.0f);
        }
        mat.emissive = { m->emissive_factor[0], m->emissive_factor[1], m->emissive_factor[2] };
        if (m->emissive_texture.texture)
        {
            mat.emissive_tex = load_tex(m->emissive_texture.texture->image);
            // Industry-convention factor promotion (emissiveFactor=0 + bound texture ⇒ {1,1,1})
            // happens in Mesh::load_model() after decode_textures, so failed decodes don't promote.
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
            // Position transform: p' = w * [px, py, pz, 1].
            // Normals need the inverse-transpose of the upper-3x3 under
            // non-uniform scale, otherwise they stop being perpendicular to
            // their surface. Build it once per node; fast-path
            // rotation/uniform-scale (column lengths equal and orthogonal) so
            // those assets stay bit-identical to the pre-fix path.

            const float c0[3] = { w[0], w[1], w[2] };
            const float c1[3] = { w[4], w[5], w[6] };
            const float c2[3] = { w[8], w[9], w[10] };
            const float det3 = (c0[0] * ((c1[1] * c2[2]) - (c1[2] * c2[1]))) -
                               (c1[0] * ((c0[1] * c2[2]) - (c0[2] * c2[1]))) +
                               (c2[0] * ((c0[1] * c1[2]) - (c0[2] * c1[1])));
            const bool flip_winding = det3 < 0.0f;

            const float l0sq = (c0[0] * c0[0]) + (c0[1] * c0[1]) + (c0[2] * c0[2]);
            const float l1sq = (c1[0] * c1[0]) + (c1[1] * c1[1]) + (c1[2] * c1[2]);
            const float l2sq = (c2[0] * c2[0]) + (c2[1] * c2[1]) + (c2[2] * c2[2]);
            const float d01 = (c0[0] * c1[0]) + (c0[1] * c1[1]) + (c0[2] * c1[2]);
            const float d02 = (c0[0] * c2[0]) + (c0[1] * c2[1]) + (c0[2] * c2[2]);
            const float d12 = (c1[0] * c2[0]) + (c1[1] * c2[1]) + (c1[2] * c2[2]);
            const float tol = 1e-5f * std::max({ l0sq, l1sq, l2sq });
            const bool orthogonal = std::fabs(d01) <= tol && std::fabs(d02) <= tol && std::fabs(d12) <= tol;
            const bool equal_scale = std::fabs(l0sq - l1sq) <= tol && std::fabs(l1sq - l2sq) <= tol;
            // Degenerate (|det| ~ 0): no usable inverse — fall back to the
            // upper-3x3 path so we don't divide by zero. Asset is broken; no
            // shading is right, but we don't crash.
            const bool uniform_scale = (orthogonal && equal_scale) || std::fabs(det3) <= 1e-12f;

            // Zero-init: GCC LTO can't prove the uniform_scale gate correlates
            // between fill and read, and warns -Wmaybe-uninitialized on the read.
            float nm[9]{};
            if (!uniform_scale)
            {
                // nm = transpose(inverse(upper3x3(w))), via adjugate / det.
                const float inv_det = 1.0f / det3;
                nm[0] = ((c1[1] * c2[2]) - (c1[2] * c2[1])) * inv_det;
                nm[1] = ((c1[2] * c2[0]) - (c1[0] * c2[2])) * inv_det;
                nm[2] = ((c1[0] * c2[1]) - (c1[1] * c2[0])) * inv_det;
                nm[3] = ((c0[2] * c2[1]) - (c0[1] * c2[2])) * inv_det;
                nm[4] = ((c0[0] * c2[2]) - (c0[2] * c2[0])) * inv_det;
                nm[5] = ((c0[1] * c2[0]) - (c0[0] * c2[1])) * inv_det;
                nm[6] = ((c0[1] * c1[2]) - (c0[2] * c1[1])) * inv_det;
                nm[7] = ((c0[2] * c1[0]) - (c0[0] * c1[2])) * inv_det;
                nm[8] = ((c0[0] * c1[1]) - (c0[1] * c1[0])) * inv_det;
            }

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
                    v.pos.x = (w[0] * p[0]) + (w[4] * p[1]) + (w[8] * p[2]) + w[12];
                    v.pos.y = (w[1] * p[0]) + (w[5] * p[1]) + (w[9] * p[2]) + w[13];
                    v.pos.z = (w[2] * p[0]) + (w[6] * p[1]) + (w[10] * p[2]) + w[14];

                    if (norm_acc)
                    {
                        float n[3];
                        cgltf_accessor_read_float(norm_acc, i, n, 3);
                        if (uniform_scale)
                        {
                            v.normal.x = (w[0] * n[0]) + (w[4] * n[1]) + (w[8] * n[2]);
                            v.normal.y = (w[1] * n[0]) + (w[5] * n[1]) + (w[9] * n[2]);
                            v.normal.z = (w[2] * n[0]) + (w[6] * n[1]) + (w[10] * n[2]);
                        }
                        else
                        {
                            v.normal.x = (nm[0] * n[0]) + (nm[3] * n[1]) + (nm[6] * n[2]);
                            v.normal.y = (nm[1] * n[0]) + (nm[4] * n[1]) + (nm[7] * n[2]);
                            v.normal.z = (nm[2] * n[0]) + (nm[5] * n[1]) + (nm[8] * n[2]);
                        }
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
                    triangles.reserve(triangles.size() + (n_idx / 3));
                    for (size_t i = 0; i + 2 < n_idx; i += 3)
                    {
                        Triangle t;
                        t.v[0] = static_cast<uint32_t>(vert_base + cgltf_accessor_read_index(prim.indices, i));
                        t.v[1] = static_cast<uint32_t>(vert_base + cgltf_accessor_read_index(prim.indices, i + 1));
                        t.v[2] = static_cast<uint32_t>(vert_base + cgltf_accessor_read_index(prim.indices, i + 2));
                        if (flip_winding)
                        {
                            std::swap(t.v[1], t.v[2]);
                        }
                        t.material_idx = mat_idx;
                        triangles.push_back(t);
                    }
                }
                else
                {
                    // Non-indexed: every 3 vertices form a triangle.
                    triangles.reserve(triangles.size() + (n_verts / 3));
                    for (size_t i = 0; i + 2 < n_verts; i += 3)
                    {
                        Triangle t;
                        t.v[0] = static_cast<uint32_t>(vert_base + i);
                        t.v[1] = static_cast<uint32_t>(vert_base + i + 1);
                        t.v[2] = static_cast<uint32_t>(vert_base + i + 2);
                        if (flip_winding)
                        {
                            std::swap(t.v[1], t.v[2]);
                        }
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

    // glTF assets may carry COLOR_0 on only part of the mesh; pad the gap white
    // so the parallel-array invariant (vertex_colors.size() == vertices.size())
    // holds before compute_normals splits and before tangents/AO/vcache run.
    if (has_vertex_colors && vertex_colors.size() < vertices.size())
    {
        vertex_colors.resize(vertices.size(), vec3{ 1.0f, 1.0f, 1.0f });
    }

    if (!has_normals)
    {
        compute_normals(crease_cos);
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
