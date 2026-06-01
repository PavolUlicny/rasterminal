#include "mesh.h"
#include "draco_decode.h"
#include "light.h"
#include "linalg.h"
#include "mesh_loader.h"
#include "texture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

    // The 12-byte KTX2 file identifier. KHR_texture_basisu images are KTX2 containers,
    // which stb_image cannot decode — they are routed to the basisu transcoder instead.
    bool is_ktx2(const uint8_t *data, size_t size)
    {
        static const uint8_t magic[12] = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
        return data && size >= sizeof(magic) && std::memcmp(data, magic, sizeof(magic)) == 0;
    }

    // Read an entire file into out. Returns false (out untouched) on any error. Used to
    // slurp external image files so they can be content-sniffed and routed uniformly,
    // the same way embedded (buffer_view) images already are. Uses the FILE idiom from
    // mesh_stl.cpp (fread takes void*, so no byte-pointer cast; unique_ptr owns the
    // handle; fseek/ftell instead of rewind).
    bool read_file(const std::string &path, std::vector<uint8_t> &out)
    {
        const auto fp = std::unique_ptr<std::FILE, int (*)(std::FILE *)>(std::fopen(path.c_str(), "rb"), std::fclose);
        if (!fp || std::fseek(fp.get(), 0, SEEK_END) != 0)
        {
            return false;
        }
        const long len = std::ftell(fp.get());
        if (len < 0 || std::fseek(fp.get(), 0, SEEK_SET) != 0)
        {
            return false;
        }
        // The file size is unbounded (arbitrary external sidecar). Decode runs on worker
        // threads with no exception boundary at the load site, so a bad_alloc here would
        // terminate the process; fail loud instead, matching decode_ktx2_rgba's guard.
        std::vector<uint8_t> buf;
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

    // Resolve the image a texture should sample. KHR_texture_basisu carries a separate
    // KTX2 image (basisu_image) alongside the standard fallback (image); prefer the KTX2
    // source when present so we decode the intended texture, not the optional PNG/JPEG
    // fallback (which may be absent).
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
        return tex->image;
    }
} // namespace

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

    // EXT_meshopt_compression: cgltf parses the extension but decodes nothing.
    // Decompress each compressed buffer view in place: allocate with cgltf's own
    // allocator, decode into it, then assign view->data. cgltf_buffer_view_data()
    // then transparently returns the decoded bytes to every accessor read, the
    // Draco path, and the texture loader. Ownership transfers to cgltf — cgltf_free
    // frees view->data (cgltf.h), so it MUST come from data->memory.alloc_func,
    // never new/malloc, or the two allocators disagree and double-free. Runs after
    // cgltf_validate, so its meshopt invariants (mc.buffer non-null, buffer size >=
    // offset+size, bv.size == stride*count, valid mode, per-mode/filter stride)
    // already hold here. view->data is assigned right after alloc, so any early
    // return below is cleaned up by the cgltf_free guard — no manual free path.
    for (size_t i = 0; i < data->buffer_views_count; i++)
    {
        cgltf_buffer_view &bv = data->buffer_views[i];
        if (!bv.has_meshopt_compression)
        {
            continue;
        }
        const cgltf_meshopt_compression &mc = bv.meshopt_compression;
        // Defensive: cgltf checked bv.size == stride*count, but if that product
        // overflowed size_t and wrapped to a small bv.size the check still passes —
        // then meshopt would write count*stride (huge) into our small buffer. Reject
        // the overflow before allocating. Not reachable via a well-formed file
        // (cgltf_validate covers the normal file-size bound); this guards the wrap.
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
            return false; // corrupt / truncated stream — fail loud
        }
        // Filters are applied in place after the vertex decode. cgltf maps any
        // unrecognized filter string to filter_none (zero-init default), so the
        // none/default arm also covers a hypothetical future filter parsed by an
        // older cgltf — it would skip filtering rather than fail. Harmless today:
        // the spec defines exactly these three filters and cgltf knows all of them.
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
        case cgltf_meshopt_compression_filter_none:
        default:
            break;
        }
    }

    // Default white material at index 0.
    materials.push_back(Material{});

    // A deferred texture decode: the preferred image, plus an optional fallback tried
    // if the preferred one fails to decode. For KHR_texture_basisu the preferred image
    // is the KTX2 and the fallback is the texture's ordinary source — the extension
    // provides it precisely so a renderer that can't transcode a given KTX2 can degrade
    // to the plain image instead of rendering untextured.
    struct TexRequest
    {
        const cgltf_image *primary;
        const cgltf_image *fallback; // nullptr when none
    };

    // Register a texture, returning its slot index. Each distinct decode is registered
    // once; the actual decode is deferred and run in parallel after the scene walk. Slot
    // order follows first-encounter order. Dedup is keyed on the (primary, fallback) pair,
    // not the primary alone: two basisu textures can share one KTX2 source yet declare
    // different ordinary-source fallbacks, and each must keep its own fallback. (Trade-off:
    // that rare same-source/different-fallback pattern then transcodes the shared KTX2
    // twice; the common no-fallback / identical-fallback cases still dedup to one decode.)
    using TexKey = std::pair<const cgltf_image *, const cgltf_image *>;
    struct TexKeyHash
    {
        size_t operator()(const TexKey &k) const
        {
            return (std::hash<const void *>{}(k.first) * 1099511628211ULL) ^ std::hash<const void *>{}(k.second);
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
        // Fallback applies only when the KTX2 source was preferred over a distinct
        // ordinary source on the same texture.
        const cgltf_image *fallback =
            (primary == tex->basisu_image && tex->image && tex->image != primary) ? tex->image : nullptr;
        const TexKey key{ primary, fallback };
        const auto it = tex_cache.find(key);
        if (it != tex_cache.end())
        {
            return it->second;
        }
        const int idx = static_cast<int>(tex_requests.size());
        tex_requests.push_back({ primary, fallback });
        tex_cache.emplace(key, idx);
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
            mat.metallic_roughness_tex = load_tex(pbr.metallic_roughness_texture.texture);
        }
        if (pbr.base_color_texture.texture)
        {
            mat.diffuse_tex = load_tex(pbr.base_color_texture.texture);
        }
        if (m->normal_texture.texture)
        {
            mat.normal_tex = load_tex(m->normal_texture.texture);
            mat.normal_scale = m->normal_texture.scale;
        }
        if (m->occlusion_texture.texture)
        {
            mat.occlusion_tex = load_tex(m->occlusion_texture.texture);
            // cgltf: scale field == occlusionTexture.strength. Spec caps it at [0,1] but cgltf
            // does not enforce; clamp so an out-of-range value can't drive ao negative per-pixel.
            mat.occlusion_strength = clamp(m->occlusion_texture.scale, 0.0f, 1.0f);
        }
        // Clamp emissiveFactor to [0, 1e6] per channel. Lower bound: spec sets `minimum: 0.0`
        // but cgltf doesn't enforce, and a negative would subtract from lit colour. Upper
        // bound: a hostile JSON literal like `1e400` parses to +Inf, which downstream produces
        // NaN via the per-pixel `Inf * 0` on a zero texel channel; since every consumer gates
        // on `> 0.0f` and vec3_to_color's clamp both mishandle NaN (uint8_t cast on NaN is UB),
        // we stop the +Inf at the source so the NaN can never arise. cgltf can't emit NaN
        // directly, so no NaN reaches this line.
        mat.emissive = { std::clamp(m->emissive_factor[0], 0.0f, 1e6f), std::clamp(m->emissive_factor[1], 0.0f, 1e6f),
                         std::clamp(m->emissive_factor[2], 0.0f, 1e6f) };
        if (m->has_emissive_strength)
        {
            // KHR_materials_emissive_strength: bake strength into the factor at load.
            // Rasterminal has no HDR/tonemap, so a load-time multiply is equivalent to the
            // per-frame intensity multiply real-time engines do in their shader. Clamp the
            // strength to [0, 1e6] for the same reason as the factor above (kill +Inf at the
            // source). Each input is bounded; the baked product can exceed 1e6 (up to ~1e12),
            // which is fine — the point is staying finite, and vec3_to_color saturates it.
            const float s = std::clamp(m->emissive_strength.emissive_strength, 0.0f, 1e6f);
            mat.emissive = mat.emissive * s;
        }
        // Spec-literal: emissive = factor × texture. A zero factor means the texture cannot
        // contribute (do_emissive in the rasterizer is gated on factor>0), so skip the decode
        // entirely — saves a stb_image_load (often multi-MB) and the permanent RAM footprint
        // for a texture no fragment will ever sample. dedup is unaffected: if the same image
        // is also bound as e.g. diffuse, that call still registers and decodes it.
        const bool emissive_active = (mat.emissive.x > 0.0f || mat.emissive.y > 0.0f || mat.emissive.z > 0.0f);
        if (emissive_active && m->emissive_texture.texture)
        {
            mat.emissive_tex = load_tex(m->emissive_texture.texture);
        }
        mat.double_sided = m->double_sided;
        if (m->alpha_mode == cgltf_alpha_mode_mask)
        {
            mat.alpha_cutoff = m->alpha_cutoff;
        }
        return mat;
    };

    bool has_normals = false;
    // Set by visit() when a Draco primitive fails to decode; checked after the
    // walk so a corrupt bitstream fails the whole load (visit() returns void).
    bool draco_error = false;

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

                // KHR_draco_mesh_compression: cgltf parses the extension but never
                // decodes. The real geometry is a compressed bitstream in the draco
                // buffer view; cgltf stores each Draco attribute's unique-id as
                // accessors[id], so we recover the id by pointer arithmetic and hand
                // the ids to decode_draco_mesh (which confines the Draco headers). The
                // decoded vertices go through the same world transform + winding flip
                // as the accessor path.
                if (prim.has_draco_mesh_compression)
                {
                    const cgltf_draco_mesh_compression &dc = prim.draco_mesh_compression;
                    if (!dc.buffer_view)
                    {
                        draco_error = true;
                        return;
                    }
                    // The unique-id recovery below subtracts attr.data (an accessor
                    // pointer) from data->accessors. Pointer subtraction with a null
                    // base is UB — a malformed glTF can omit the top-level "accessors"
                    // array (legal JSON, cgltf accepts it) which leaves data->accessors
                    // null. Fail-loud rather than risk UB.
                    if (!data->accessors)
                    {
                        draco_error = true;
                        return;
                    }
                    int pos_id = -1;
                    int norm_id = -1;
                    int uv_id = -1;
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

                    // cgltf_buffer_view_data handles EXT_meshopt_compression / sparse / offset
                    // indirection and can return null when the underlying buffer data was never
                    // populated; gate at the cgltf boundary so a null pointer can't reach Draco's
                    // DecoderBuffer::Init (which would happily read it as a valid range).
                    const uint8_t *cbytes = cgltf_buffer_view_data(dc.buffer_view);
                    if (!cbytes)
                    {
                        draco_error = true;
                        return;
                    }

                    DracoMesh dm;
                    if (!decode_draco_mesh(
                            cbytes, dc.buffer_view->size, static_cast<uint32_t>(pos_id), norm_id, uv_id, color_id, dm
                        ))
                    {
                        draco_error = true;
                        return;
                    }

                    const size_t n_verts = dm.num_points;
                    const size_t vert_base = vertices.size();
                    const bool has_dn = !dm.normals.empty();
                    const bool has_du = !dm.uvs.empty();
                    const bool has_dc = !dm.colors.empty();
                    vertices.reserve(vertices.size() + n_verts);
                    if (has_dc)
                    {
                        vertex_colors.resize(vert_base + n_verts, { 1.0f, 1.0f, 1.0f });
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
                    }
                    if (has_dn)
                    {
                        has_normals = true;
                    }
                    if (has_dc)
                    {
                        has_vertex_colors = true;
                    }

                    // Connectivity comes from the Draco bitstream itself
                    // (dm.indices), so prim.indices and the accessor-path's
                    // non-indexed branch are both intentionally bypassed — a Draco
                    // primitive's glTF-level indices accessor is metadata only per
                    // the KHR_draco_mesh_compression spec.
                    const size_t n_tris = dm.indices.size() / 3;
                    triangles.reserve(triangles.size() + n_tris);
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
                    v.pos = apply_world_pos(w, p);

                    if (norm_acc)
                    {
                        float n[3];
                        cgltf_accessor_read_float(norm_acc, i, n, 3);
                        v.normal = apply_world_normal(uniform_scale, w, nm, n);
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

    if (draco_error)
    {
        return false;
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

    // Decode one image (external uri or embedded buffer_view), routing KTX2 vs stb by
    // content sniff. Returns an invalid Texture on any failure.
    auto decode_one = [&](const cgltf_image *img) -> Texture
    {
        Texture tex;
        if (img->uri && img->uri[0] != '\0')
        {
            // data: URIs (inline base64 images) are not yet supported; skip explicitly
            // so the gap is visible here rather than a silent read_file failure below.
            if (std::strncmp(img->uri, "data:", 5) == 0)
            {
                return tex;
            }
            // Percent-decode the URI before opening the file. cgltf decodes escapes for
            // buffer URIs but not image URIs, so e.g. "my%20tex.ktx2" would otherwise fail
            // to open. Decode only the uri (not the dir prefix), matching cgltf's own
            // buffer-load path; cgltf_decode_uri rewrites in place and returns the new len.
            std::string uri = img->uri;
            uri.resize(cgltf_decode_uri(uri.data()));
            // Slurp external files so KTX2 and stb-decodable formats route the same way
            // (by content), independent of the file extension.
            std::vector<uint8_t> bytes;
            if (read_file(dir + uri, bytes))
            {
                if (is_ktx2(bytes.data(), bytes.size()))
                {
                    (void)tex.load_ktx2_from_memory(bytes.data(), bytes.size());
                }
                else
                {
                    (void)tex.load_from_memory(bytes.data(), bytes.size());
                }
            }
        }
        else if (img->buffer_view)
        {
            // cgltf_buffer_view_data honours EXT_meshopt_compression overrides; returns
            // null when the backing external buffer was never loaded.
            const uint8_t *ptr = cgltf_buffer_view_data(img->buffer_view);
            if (!ptr)
            {
                return tex;
            }
            const size_t size = img->buffer_view->size;
            if (is_ktx2(ptr, size))
            {
                (void)tex.load_ktx2_from_memory(ptr, size);
            }
            else
            {
                (void)tex.load_from_memory(ptr, size);
            }
        }
        return tex;
    };

    // Decode all registered images in parallel. data (held by guard) and dir outlive
    // the join, so worker reads of buffer_view->buffer->data are valid. On a failed
    // KTX2 transcode, fall back to the texture's ordinary source if it provided one.
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
            return tex;
        }
    );

    snap.commit();
    return true;
}
