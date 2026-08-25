#include "src/loaders/mesh.h"
#include "src/loaders/draco_decode.h"
#include "src/loaders/image_sniff.h"
#include "src/loaders/mesh_loader.h"
#include "src/math/light.h"
#include "src/math/linalg.h"
#include "src/platform/platform.h"
#include "src/render/texture.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "meshoptimizer.h"

namespace
{
    // Grow geometrically across primitives. Exact per-primitive reserve caused quadratic copying.
    template <typename T> void grow_for(std::vector<T> &v, size_t extra)
    {
        const size_t need = v.size() + extra;
        if (v.capacity() < need)
        {
            v.reserve(std::max(need, v.capacity() * 2));
        }
    }

    // Unpacked accessor data and its actual component stride.
    struct Attr
    {
        const float *data = nullptr;
        size_t stride = 0;

        explicit operator bool() const noexcept { return data != nullptr; }
        [[nodiscard]] const float *at(size_t i) const noexcept { return data + (i * stride); }
    };

    // Apply the node world matrix (column-major) to a position. Shared by the
    // uncompressed accessor path and the Draco decode path so both stay identical.
    vec3 apply_world_pos(const float w[16], const float p[3])
    {
        return { (w[0] * p[0]) + (w[4] * p[1]) + (w[8] * p[2]) + w[12],
                 (w[1] * p[0]) + (w[5] * p[1]) + (w[9] * p[2]) + w[13],
                 (w[2] * p[0]) + (w[6] * p[1]) + (w[10] * p[2]) + w[14] };
    }

    // Transform a normal by the upper-3x3 of w (rotation / uniform scale) or the
    // precomputed inverse-transpose nm (non-uniform scale), then normalize.
    vec3 apply_world_normal(bool uniform_scale, const float w[16], const float nm[9], const float n[3])
    {
        vec3 r;
        if (uniform_scale)
        {
            r = { (w[0] * n[0]) + (w[4] * n[1]) + (w[8] * n[2]), (w[1] * n[0]) + (w[5] * n[1]) + (w[9] * n[2]),
                  (w[2] * n[0]) + (w[6] * n[1]) + (w[10] * n[2]) };
        }
        else
        {
            r = { (nm[0] * n[0]) + (nm[3] * n[1]) + (nm[6] * n[2]), (nm[1] * n[0]) + (nm[4] * n[1]) + (nm[7] * n[2]),
                  (nm[2] * n[0]) + (nm[5] * n[1]) + (nm[8] * n[2]) };
        }
        const float len = r.length();
        if (len > 1e-6f)
        {
            r = r * (1.0f / len);
        }
        return r;
    }

    // Read an external image for the same content-sniffing path used by embedded images.
    // Failure leaves `out` unchanged.
    bool read_file(const std::string &path, std::vector<uint8_t> &out)
    {
        const auto fp = std::unique_ptr<std::FILE, int (*)(std::FILE *)>(std::fopen(path.c_str(), "rb"), std::fclose);
        if (!fp)
        {
            return false;
        }
        const int64_t len = platform::file_size(fp.get());
        if (len < 0 || std::fseek(fp.get(), 0, SEEK_SET) != 0)
        {
            return false;
        }
        std::vector<uint8_t> buf;
        // Reject files larger than this vector can represent, especially on ILP32.
        if (static_cast<uint64_t>(len) > buf.max_size())
        {
            return false;
        }
        // Worker decode has no exception boundary, so convert OOM into failure.
        try
        {
            buf.resize(static_cast<size_t>(len));
        }
        catch (const std::bad_alloc &)
        {
            return false;
        }
        if (len > 0 && std::fread(buf.data(), 1, static_cast<size_t>(len), fp.get()) != static_cast<size_t>(len))
        {
            return false;
        }
        out = std::move(buf);
        return true;
    }

    // Prefer KTX2, then WebP, then the ordinary image source.
    const cgltf_image *pick_image(const cgltf_texture *tex)
    {
        if (!tex)
        {
            return nullptr;
        }
        if (tex->has_basisu && tex->basisu_image)
        {
            return tex->basisu_image;
        }
        if (tex->has_webp && tex->webp_image)
        {
            return tex->webp_image;
        }
        return tex->image;
    }

    // glTF defaults missing sampler wrap modes to Repeat.
    WrapMode to_wrap_mode(cgltf_wrap_mode w)
    {
        switch (w)
        {
        case cgltf_wrap_mode_clamp_to_edge:
            return WrapMode::Clamp;
        case cgltf_wrap_mode_mirrored_repeat:
            return WrapMode::Mirror;
        default:
            return WrapMode::Repeat;
        }
    }
} // namespace

bool Mesh::load_gltf(const std::string &path, int n_threads, float crease_cos)
{
    MeshSnapshot snap(*this);

    // Resolve external files relative to the glTF directory.
    const std::string dir = dir_of(path);

    const cgltf_options opts{};
    cgltf_data *data = nullptr;

    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success)
    {
        return false;
    }

    // Free cgltf state on every exit.
    const auto guard = std::unique_ptr<cgltf_data, decltype(&cgltf_free)>(data, cgltf_free);

    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success)
    {
        return false;
    }
    if (cgltf_validate(data) != cgltf_result_success)
    {
        return false;
    }

    // Decode meshopt buffer views in place so cgltf consumers see ordinary bytes. Allocate
    // through cgltf because cgltf_free owns view->data. Validation has checked format invariants.
    for (size_t i = 0; i < data->buffer_views_count; i++)
    {
        cgltf_buffer_view &bv = data->buffer_views[i];
        if (!bv.has_meshopt_compression)
        {
            continue;
        }
        const cgltf_meshopt_compression &mc = bv.meshopt_compression;
        // Reject a wrapped count*stride product before meshopt writes the decoded view.
        if (mc.stride != 0 && mc.count > SIZE_MAX / mc.stride)
        {
            return false;
        }
        const size_t dst_size = mc.count * mc.stride;
        if (dst_size == 0)
        {
            continue; // empty view: nothing to decode, leave the override unset
        }
        const auto *src = static_cast<const uint8_t *>(mc.buffer->data);
        if (!src)
        {
            return false; // buffer never populated (load_buffers gap)
        }
        src += mc.offset;
        void *dst = data->memory.alloc_func(data->memory.user_data, dst_size);
        if (!dst)
        {
            return false;
        }
        bv.data = dst; // hand ownership to cgltf now; the guard frees it on any early return
        int rc = 0;
        switch (mc.mode)
        {
        case cgltf_meshopt_compression_mode_attributes:
            rc = meshopt_decodeVertexBuffer(dst, mc.count, mc.stride, src, mc.size);
            break;
        case cgltf_meshopt_compression_mode_triangles:
            rc = meshopt_decodeIndexBuffer(dst, mc.count, mc.stride, src, mc.size);
            break;
        case cgltf_meshopt_compression_mode_indices:
            rc = meshopt_decodeIndexSequence(dst, mc.count, mc.stride, src, mc.size);
            break;
        default:
            return false; // mode_invalid / max_enum
        }
        if (rc != 0)
        {
            return false; // corrupt / truncated stream, fail loud
        }
        // Apply the four specified meshopt filters after vertex decoding.
        switch (mc.filter)
        {
        case cgltf_meshopt_compression_filter_octahedral:
            meshopt_decodeFilterOct(dst, mc.count, mc.stride);
            break;
        case cgltf_meshopt_compression_filter_quaternion:
            meshopt_decodeFilterQuat(dst, mc.count, mc.stride);
            break;
        case cgltf_meshopt_compression_filter_exponential:
            meshopt_decodeFilterExp(dst, mc.count, mc.stride);
            break;
        case cgltf_meshopt_compression_filter_color:
            meshopt_decodeFilterColor(dst, mc.count, mc.stride);
            break;
        case cgltf_meshopt_compression_filter_none:
        default:
            break;
        }
    }

    // Reserve material zero for the default.
    materials.push_back(Material{});

    // Deferred preferred image and the ordinary glTF fallback source, if any.
    struct TexRequest
    {
        const cgltf_image *primary;
        const cgltf_image *fallback; // nullptr when none
        WrapMode wrap_s;
        WrapMode wrap_t;
    };

    // Deduplicate deferred decodes by preferred source, fallback source and wrap modes.
    // Shared images with different samplers must remain separate texture slots.
    struct TexKey
    {
        const cgltf_image *primary;
        const cgltf_image *fallback;
        WrapMode wrap_s;
        WrapMode wrap_t;
        bool operator==(const TexKey &o) const
        {
            return primary == o.primary && fallback == o.fallback && wrap_s == o.wrap_s && wrap_t == o.wrap_t;
        }
    };
    struct TexKeyHash
    {
        size_t operator()(const TexKey &k) const
        {
            // Mix wide, then accept normal size_t truncation on ILP32.
            const uint64_t wrap = (static_cast<uint64_t>(k.wrap_s) << 2U) | static_cast<uint64_t>(k.wrap_t);
            return static_cast<size_t>(
                ((std::hash<const void *>{}(k.primary) * 1099511628211ULL) ^ std::hash<const void *>{}(k.fallback)) +
                wrap
            );
        }
    };
    std::unordered_map<TexKey, int, TexKeyHash> tex_cache;
    std::vector<TexRequest> tex_requests;
    auto load_tex = [&](const cgltf_texture *tex) -> int
    {
        const cgltf_image *primary = pick_image(tex);
        if (!primary)
        {
            return -1;
        }
        const WrapMode wrap_s = tex->sampler ? to_wrap_mode(tex->sampler->wrap_s) : WrapMode::Repeat;
        const WrapMode wrap_t = tex->sampler ? to_wrap_mode(tex->sampler->wrap_t) : WrapMode::Repeat;
        // glTF defines one ordinary fallback source for extension images, not an extension chain.
        const cgltf_image *fallback = (tex->image && primary != tex->image) ? tex->image : nullptr;
        const TexKey key{ primary, fallback, wrap_s, wrap_t };
        const auto it = tex_cache.find(key);
        if (it != tex_cache.end())
        {
            return it->second;
        }
        const int idx = static_cast<int>(tex_requests.size());
        tex_requests.push_back({ primary, fallback, wrap_s, wrap_t });
        tex_cache.emplace(key, idx);
        return idx;
    };

    // Track whether any binding requests TEXCOORD_1.
    bool any_uv1_referenced = false;

    // Bake the static KHR_texture_transform into stored, v-flipped UV space. This negates
    // rotation while leaving scale and offset equivalent to the spec's v-down transform.
    auto bake_transform = [](TexSlot &slot, const cgltf_texture_transform &tr)
    {
        const float c = std::cos(tr.rotation);
        const float s = -std::sin(tr.rotation);
        const float sx = tr.scale[0];
        const float sy = tr.scale[1];
        const float ox = tr.offset[0];
        const float oy = tr.offset[1];
        slot.has_transform = true;
        slot.t[0] = c * sx;
        slot.t[1] = s * sy;
        slot.t[2] = ox - (s * sy);
        slot.t[3] = -s * sx;
        slot.t[4] = c * sy;
        slot.t[5] = 1.0f - oy - (c * sy);
    };

    // Resolve the binding UV set and transform override. Unsupported sets degrade to zero;
    // references to an absent second set are reconciled after the scene walk.
    auto bind_slot = [&](TexSlot &slot, const cgltf_texture_view &view)
    {
        int tc = view.texcoord;
        if (view.has_transform && view.transform.has_texcoord)
        {
            tc = view.transform.texcoord;
        }
        // The renderer stores two UV sets; higher indices degrade to set zero.
        slot.uv_set = (tc == 1) ? uint8_t{ 1 } : uint8_t{ 0 };
        if (tc == 1)
        {
            any_uv1_referenced = true;
        }
        if (view.has_transform)
        {
            bake_transform(slot, view.transform);
        }
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
            mat.mr_map.tex = load_tex(pbr.metallic_roughness_texture.texture);
            bind_slot(mat.mr_map, pbr.metallic_roughness_texture);
        }
        if (pbr.base_color_texture.texture)
        {
            mat.diffuse_map.tex = load_tex(pbr.base_color_texture.texture);
            bind_slot(mat.diffuse_map, pbr.base_color_texture);
        }
        if (m->normal_texture.texture)
        {
            mat.normal_map.tex = load_tex(m->normal_texture.texture);
            bind_slot(mat.normal_map, m->normal_texture);
            mat.normal_scale = m->normal_texture.scale;
        }
        if (m->occlusion_texture.texture)
        {
            mat.occlusion_map.tex = load_tex(m->occlusion_texture.texture);
            bind_slot(mat.occlusion_map, m->occlusion_texture);
            // cgltf does not enforce the spec's [0,1] occlusion strength.
            mat.occlusion_strength = clamp(m->occlusion_texture.scale, 0.0f, 1.0f);
        }
        // Bound emission before a zero texture channel can turn an infinite factor into NaN.
        mat.emissive = { std::clamp(m->emissive_factor[0], 0.0f, 1e6f), std::clamp(m->emissive_factor[1], 0.0f, 1e6f),
                         std::clamp(m->emissive_factor[2], 0.0f, 1e6f) };
        if (m->has_emissive_strength)
        {
            // Bake bounded emissive strength into the factor; display conversion saturates it.
            const float s = std::clamp(m->emissive_strength.emissive_strength, 0.0f, 1e6f);
            mat.emissive = mat.emissive * s;
        }
        // A zero emissive factor nullifies its texture, so skip an otherwise unused decode.
        const bool emissive_active = (mat.emissive.x > 0.0f || mat.emissive.y > 0.0f || mat.emissive.z > 0.0f);
        if (emissive_active && m->emissive_texture.texture)
        {
            mat.emissive_map.tex = load_tex(m->emissive_texture.texture);
            bind_slot(mat.emissive_map, m->emissive_texture);
        }
        mat.double_sided = m->double_sided;
        mat.unlit = m->unlit;
        if (m->alpha_mode == cgltf_alpha_mode_mask)
        {
            mat.alpha_cutoff = m->alpha_cutoff;
        }
        else if (m->alpha_mode == cgltf_alpha_mode_blend)
        {
            mat.blend = true;
            mat.alpha = pbr.base_color_factor[3];
        }
        // Approximate transmission with alpha blending. Preserve authored BLEND/MASK behavior,
        // and apply attenuation color as a uniform tint because thickness is not modeled.
        if (!mat.blend && mat.alpha_cutoff == 0.0f && m->has_transmission && m->transmission.transmission_factor > 0.0f)
        {
            constexpr float GLASS_ALPHA_FLOOR = 0.18f;
            mat.blend = true;
            mat.alpha = std::clamp(1.0f - m->transmission.transmission_factor, GLASS_ALPHA_FLOOR, 1.0f);
            // Keep both interfaces of approximated transmissive volumes in the A-buffer.
            mat.double_sided = true;
            if (m->has_volume)
            {
                const vec3 tint{ m->volume.attenuation_color[0], m->volume.attenuation_color[1],
                                 m->volume.attenuation_color[2] };
                mat.diffuse = mat.diffuse * tint;
                mat.ambient = mat.ambient * tint;
            }
        }
        return mat;
    };

    bool has_normals = false;
    // visit() is void, so defer Draco failure until the walk completes.
    bool draco_error = false;

    // Reuse grow-only attribute buffers. Bulk unpacking avoids cgltf's per-element sparse,
    // pointer and component dispatch.
    std::vector<float> buf_pos;
    std::vector<float> buf_norm;
    std::vector<float> buf_uv;
    std::vector<float> buf_uv1;
    std::vector<float> buf_color;
    // Size and index by the accessor's true component count because sparse writes use that stride.
    // Treat semantically undersized accessors as absent; cgltf validation does not reject them.
    const auto unpack = [](std::vector<float> &buf, const cgltf_accessor *acc, size_t comps) -> Attr
    {
        if (!acc)
        {
            return {};
        }
        const size_t stride = cgltf_num_components(acc->type);
        if (stride < comps)
        {
            return {};
        }
        const size_t need = acc->count * stride;
        if (buf.size() < need)
        {
            buf.resize(need);
        }
        // Zero the tail of a short read instead of retaining prior primitive data.
        const size_t got = cgltf_accessor_unpack_floats(acc, buf.data(), need);
        if (got < need)
        {
            std::fill(
                buf.begin() + static_cast<std::ptrdiff_t>(got), buf.begin() + static_cast<std::ptrdiff_t>(need), 0.0f
            );
        }
        return { buf.data(), stride };
    };

    // True once any primitive provides TEXCOORD_1; gates the parallel uv1 array.
    bool building_uv1 = false;

    // Keep uv1 parallel after its first real appearance. Vertices without TEXCOORD_1 inherit uv0.
    auto append_uv1 = [&](size_t vert_base, size_t n, bool has_real, auto &&read)
    {
        if (has_real && !building_uv1)
        {
            building_uv1 = true;
            // One allocation covers the initial backfill and current primitive.
            uv1.reserve(vertices.size());
            for (size_t k = 0; k < vert_base; k++)
            {
                uv1.push_back(vertices[k].uv);
            }
        }
        if (!building_uv1)
        {
            return;
        }
        for (size_t i = 0; i < n; i++)
        {
            uv1.push_back(has_real ? read(i) : vertices[vert_base + i].uv);
        }
    };

    // Walk the scene graph recursively, applying world transforms.
    std::function<void(const cgltf_node *)> visit = [&](const cgltf_node *node)
    {
        if (node->mesh)
        {
            float w[16];
            cgltf_node_transform_world(node, w);
            // Build one inverse-transpose per non-uniformly scaled node. Keep rotation and
            // uniform-scale transforms on the existing upper-3x3 path.

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
            // A singular transform has no valid normal matrix; avoid division by zero.
            const bool uniform_scale = (orthogonal && equal_scale) || std::fabs(det3) <= 1e-12f;

            // Zero-init avoids a GCC LTO false positive across the uniform-scale gate.
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
                // Convert strips and fans below; drop points and lines, which have no surface.
                if (prim.type != cgltf_primitive_type_triangles && prim.type != cgltf_primitive_type_triangle_strip &&
                    prim.type != cgltf_primitive_type_triangle_fan)
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

                // cgltf exposes Draco attribute IDs as accessor pointers but does not decode.
                // Decode separately, then reuse the accessor path's transforms and winding.
                if (prim.has_draco_mesh_compression)
                {
                    const cgltf_draco_mesh_compression &dc = prim.draco_mesh_compression;
                    if (!dc.buffer_view)
                    {
                        draco_error = true;
                        return;
                    }
                    // Attribute-ID pointer subtraction requires a non-null accessor array.
                    if (!data->accessors)
                    {
                        draco_error = true;
                        return;
                    }
                    int pos_id = -1;
                    int norm_id = -1;
                    int uv_id = -1;
                    int uv1_id = -1;
                    int color_id = -1;
                    for (size_t k = 0; k < dc.attributes_count; k++)
                    {
                        const cgltf_attribute &attr = dc.attributes[k];
                        const int uid = static_cast<int>(attr.data - data->accessors);
                        if (attr.type == cgltf_attribute_type_position)
                        {
                            pos_id = uid;
                        }
                        else if (attr.type == cgltf_attribute_type_normal)
                        {
                            norm_id = uid;
                        }
                        else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0)
                        {
                            uv_id = uid;
                        }
                        else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 1)
                        {
                            uv1_id = uid;
                        }
                        else if (attr.type == cgltf_attribute_type_color && attr.index == 0)
                        {
                            color_id = uid;
                        }
                    }
                    if (pos_id < 0)
                    {
                        draco_error = true;
                        return;
                    }

                    // Do not pass a missing buffer-view backing pointer into Draco.
                    const uint8_t *cbytes = cgltf_buffer_view_data(dc.buffer_view);
                    if (!cbytes)
                    {
                        draco_error = true;
                        return;
                    }

                    DracoMesh dm;
                    if (!decode_draco_mesh(
                            cbytes, dc.buffer_view->size, static_cast<uint32_t>(pos_id), norm_id, uv_id, uv1_id,
                            color_id, dm
                        ))
                    {
                        draco_error = true;
                        return;
                    }

                    const size_t n_verts = dm.num_points;
                    const size_t vert_base = vertices.size();
                    const bool has_dn = !dm.normals.empty();
                    const bool has_du = !dm.uvs.empty();
                    const bool has_du1 = !dm.uvs1.empty();
                    const bool has_dc = !dm.colors.empty();
                    // Honor RGBA vertex opacity only for alphaMode=BLEND, matching accessor data.
                    const bool color_has_alpha = !dm.colors_alpha.empty() && prim.material &&
                                                 prim.material->alpha_mode == cgltf_alpha_mode_blend;
                    grow_for(vertices, n_verts);
                    if (has_dc)
                    {
                        vertex_colors.resize(vert_base + n_verts, { 1.0f, 1.0f, 1.0f });
                    }
                    if (color_has_alpha)
                    {
                        vertex_alpha.resize(vert_base + n_verts, 1.0f);
                    }
                    for (size_t i = 0; i < n_verts; i++)
                    {
                        Vertex v{};
                        v.pos = apply_world_pos(w, &dm.positions[i * 3]);
                        if (has_dn)
                        {
                            v.normal = apply_world_normal(uniform_scale, w, nm, &dm.normals[i * 3]);
                        }
                        if (has_du)
                        {
                            v.uv = { dm.uvs[(i * 2) + 0], 1.0f - dm.uvs[(i * 2) + 1] };
                        }
                        v.ao = 1.0f;
                        vertices.push_back(v);
                        if (has_dc)
                        {
                            vertex_colors[vert_base + i] = { dm.colors[(i * 3) + 0], dm.colors[(i * 3) + 1],
                                                             dm.colors[(i * 3) + 2] };
                        }
                        if (color_has_alpha)
                        {
                            vertex_alpha[vert_base + i] = dm.colors_alpha[i];
                        }
                    }
                    append_uv1(
                        vert_base, n_verts, has_du1,
                        [&](size_t i) -> vec2 { return { dm.uvs1[(i * 2) + 0], 1.0f - dm.uvs1[(i * 2) + 1] }; }
                    );
                    if (has_dn)
                    {
                        has_normals = true;
                    }
                    if (has_dc)
                    {
                        has_vertex_colors = true;
                    }
                    if (color_has_alpha)
                    {
                        has_vertex_alpha = true;
                    }

                    // Draco connectivity comes from the bitstream, not prim.indices.
                    const size_t n_tris = dm.indices.size() / 3;
                    grow_for(triangles, n_tris);
                    for (size_t f = 0; f < n_tris; f++)
                    {
                        Triangle t;
                        t.v[0] = static_cast<uint32_t>(vert_base + dm.indices[(f * 3) + 0]);
                        t.v[1] = static_cast<uint32_t>(vert_base + dm.indices[(f * 3) + 1]);
                        t.v[2] = static_cast<uint32_t>(vert_base + dm.indices[(f * 3) + 2]);
                        if (flip_winding)
                        {
                            std::swap(t.v[1], t.v[2]);
                        }
                        t.material_idx = mat_idx;
                        triangles.push_back(t);
                    }
                    continue;
                }

                const cgltf_accessor *pos_acc = nullptr;
                const cgltf_accessor *norm_acc = nullptr;
                const cgltf_accessor *uv_acc = nullptr;
                const cgltf_accessor *uv1_acc = nullptr;
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
                    else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 1)
                    {
                        uv1_acc = attr.data;
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

                const Attr pos_a = unpack(buf_pos, pos_acc, 3);
                if (!pos_a)
                {
                    continue; // POSITION narrower than vec3: skip the primitive, as an absent one does
                }
                const Attr norm_a = unpack(buf_norm, norm_acc, 3);
                const Attr uv_a = unpack(buf_uv, uv_acc, 2);
                const Attr uv1_a = unpack(buf_uv1, uv1_acc, 2);

                grow_for(vertices, n_verts);
                for (size_t i = 0; i < n_verts; i++)
                {
                    Vertex v{};

                    v.pos = apply_world_pos(w, pos_a.at(i));

                    if (norm_a)
                    {
                        v.normal = apply_world_normal(uniform_scale, w, nm, norm_a.at(i));
                    }

                    if (uv_a)
                    {
                        v.uv = { uv_a.at(i)[0], 1.0f - uv_a.at(i)[1] };
                    }

                    v.ao = 1.0f;
                    vertices.push_back(v);
                }

                append_uv1(
                    vert_base, n_verts, static_cast<bool>(uv1_a),
                    [&](size_t i) -> vec2 { return { uv1_a.at(i)[0], 1.0f - uv1_a.at(i)[1] }; }
                );

                // Key on successfully unpacked normals, not accessor presence.
                if (norm_a)
                {
                    has_normals = true;
                }

                if (color_acc)
                {
                    vertex_colors.resize(vert_base + n_verts, { 1.0f, 1.0f, 1.0f });
                    // Only RGBA COLOR_0 under alphaMode=BLEND contributes vertex opacity.
                    const bool color_has_alpha = (color_acc->type == cgltf_type_vec4) && prim.material &&
                                                 prim.material->alpha_mode == cgltf_alpha_mode_blend;
                    if (color_has_alpha)
                    {
                        vertex_alpha.resize(vert_base + n_verts, 1.0f);
                    }
                    // Read the fourth component only when the accessor is RGBA.
                    const Attr col_a = unpack(buf_color, color_acc, 3);
                    for (size_t i = 0; col_a && i < n_verts; i++)
                    {
                        const float *c = col_a.at(i);
                        vertex_colors[vert_base + i] = { c[0], c[1], c[2] };
                        if (color_has_alpha)
                        {
                            vertex_alpha[vert_base + i] = c[3];
                        }
                    }
                    has_vertex_colors = true;
                    if (color_has_alpha)
                    {
                        has_vertex_alpha = true;
                    }
                }

                // Convert strips and fans to independent triangles with glTF winding. Keep stitch
                // degenerates for render-time rejection. Bounds must tolerate short and partial lists.
                if (prim.type == cgltf_primitive_type_triangle_strip || prim.type == cgltf_primitive_type_triangle_fan)
                {
                    const bool fan = prim.type == cgltf_primitive_type_triangle_fan;
                    const size_t count = prim.indices ? prim.indices->count : n_verts;
                    const auto src = [&](size_t i) -> uint32_t {
                        return static_cast<uint32_t>(
                            vert_base + (prim.indices ? cgltf_accessor_read_index(prim.indices, i) : i)
                        );
                    };
                    if (count >= 3)
                    {
                        grow_for(triangles, count - 2);
                        // A rolling window avoids rereading shared indices through cgltf.
                        uint32_t i0 = src(0);
                        uint32_t i1 = src(1);
                        for (size_t i = 2; i < count; i++)
                        {
                            const uint32_t i2 = src(i);
                            Triangle t;
                            if (fan || (i & 1u) == 0u)
                            {
                                t.v[0] = i0;
                                t.v[1] = i1;
                                t.v[2] = i2;
                            }
                            else
                            {
                                t.v[0] = i1;
                                t.v[1] = i0;
                                t.v[2] = i2;
                            }
                            if (flip_winding)
                            {
                                std::swap(t.v[1], t.v[2]);
                            }
                            t.material_idx = mat_idx;
                            triangles.push_back(t);
                            if (!fan)
                            {
                                i0 = i1;
                            }
                            i1 = i2;
                        }
                    }
                }
                else if (prim.indices)
                {
                    const size_t n_idx = prim.indices->count;
                    grow_for(triangles, n_idx / 3);
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
                    grow_for(triangles, n_verts / 3);
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

    if (draco_error)
    {
        return false;
    }

    if (vertices.empty() || triangles.empty())
    {
        return false;
    }

    // Pad primitives without COLOR_0 to keep the parallel color array valid.
    if (has_vertex_colors && vertex_colors.size() < vertices.size())
    {
        vertex_colors.resize(vertices.size(), vec3{ 1.0f, 1.0f, 1.0f });
    }
    if (has_vertex_alpha && vertex_alpha.size() < vertices.size())
    {
        vertex_alpha.resize(vertices.size(), 1.0f);
    }

    // Keep TEXCOORD_1 only when both supplied and referenced. Otherwise force bindings to uv0.
    // Reconcile before normal splitting and vertex remapping.
    has_uv1 = building_uv1 && any_uv1_referenced;
    if (!has_uv1)
    {
        uv1.clear();
        for (Material &m : materials)
        {
            m.diffuse_map.uv_set = 0;
            m.specular_map.uv_set = 0;
            m.normal_map.uv_set = 0;
            m.emissive_map.uv_set = 0;
            m.mr_map.uv_set = 0;
            m.occlusion_map.uv_set = 0;
        }
    }
    else if (uv1.size() < vertices.size())
    {
        for (size_t i = uv1.size(); i < vertices.size(); i++)
        {
            uv1.push_back(vertices[i].uv);
        }
    }

    if (!has_normals)
    {
        compute_normals(crease_cos);
    }

    // Choose KTX2, WebP or stb_image by content so embedded and external images behave alike.
    auto decode_bytes = [](const uint8_t *p, size_t n) -> Texture
    {
        Texture tex;
        if (is_ktx2(p, n))
        {
            (void)tex.load_ktx2_from_memory(p, n);
        }
        else if (is_webp(p, n))
        {
            (void)tex.load_webp_from_memory(p, n);
        }
        else
        {
            (void)tex.load_from_memory(p, n);
        }
        return tex;
    };

    // Decode one image, sourced from an external uri or an embedded buffer_view.
    auto decode_one = [&](const cgltf_image *img) -> Texture
    {
        if (img->uri && img->uri[0] != '\0')
        {
            // cgltf leaves image data URIs encoded. Support base64 and treat other forms as
            // an ordinary texture decode failure.
            if (std::strncmp(img->uri, "data:", 5) == 0)
            {
                const char *comma = std::strchr(img->uri, ',');
                // Require cgltf's exact `;base64,` marker without reading before the URI.
                if (!comma || comma - img->uri < 7 || std::strncmp(comma - 7, ";base64", 7) != 0)
                {
                    return Texture{};
                }
                const char *payload = comma + 1;
                // Count only the base64 run and compute decoded size without an ILP32 multiply.
                size_t nchars = 0;
                for (const char *q = payload; *q != '\0'; ++q)
                {
                    const char c = *q;
                    const bool b64 = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                     c == '+' || c == '/';
                    if (!b64)
                    {
                        break;
                    }
                    ++nchars;
                }
                size_t out_size = (nchars / 4) * 3;
                if (nchars % 4 == 2)
                {
                    out_size += 1;
                }
                else if (nchars % 4 == 3)
                {
                    out_size += 2;
                }
                if (out_size == 0)
                {
                    return Texture{}; // empty / degenerate payload
                }
                // cgltf decodes only the counted run and reports allocation failure without throwing.
                void *raw = nullptr;
                if (cgltf_load_buffer_base64(&opts, out_size, payload, &raw) != cgltf_result_success)
                {
                    return Texture{};
                }
                // Match cgltf's default malloc allocator.
                const auto owned = std::unique_ptr<void, void (*)(void *)>(raw, std::free);
                return decode_bytes(static_cast<const uint8_t *>(raw), out_size);
            }
            // cgltf does not percent-decode image URIs, so decode the URI before joining its directory.
            std::string uri = img->uri;
            uri.resize(cgltf_decode_uri(uri.data()));
            std::vector<uint8_t> bytes;
            if (read_file(dir + uri, bytes))
            {
                return decode_bytes(bytes.data(), bytes.size());
            }
            return Texture{};
        }
        if (img->buffer_view)
        {
            // cgltf_buffer_view_data includes meshopt overrides and may report missing backing data.
            const uint8_t *ptr = cgltf_buffer_view_data(img->buffer_view);
            if (!ptr)
            {
                return Texture{};
            }
            return decode_bytes(ptr, img->buffer_view->size);
        }
        return Texture{};
    };

    // Decode registered images in parallel while cgltf data remains alive. If an extension
    // image fails, try its single ordinary fallback source.
    decode_textures(
        textures, materials, tex_requests.size(), n_threads,
        [&](size_t i) -> Texture
        {
            const TexRequest &req = tex_requests[i];
            Texture tex = decode_one(req.primary);
            if (!tex.valid() && req.fallback)
            {
                tex = decode_one(req.fallback);
            }
            tex.wrap_s = req.wrap_s;
            tex.wrap_t = req.wrap_t;
            return tex;
        }
    );

    snap.commit();
    return true;
}
