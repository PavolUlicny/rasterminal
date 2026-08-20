#include "src/loaders/mesh.h"
#include "src/loaders/draco_decode.h"
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
    // Make room for `extra` more elements, doubling rather than fitting exactly.
    //
    // reserve(size() + extra) allocates EXACTLY that, so calling it once per primitive reallocates
    // and copies everything read so far, every time: a scene of a hundred primitives moves
    // gigabytes for a mesh of tens of megabytes. Doubling makes the growth amortized, which is what
    // push_back would have done on its own had the reserve not pinned the capacity each round. On a
    // 645k-triangle city with 113 primitives this was most of the load time (1.8 s -> 0.6 s).
    template <typename T> void grow_for(std::vector<T> &v, size_t extra)
    {
        const size_t need = v.size() + extra;
        if (v.capacity() < need)
        {
            v.reserve(std::max(need, v.capacity() * 2));
        }
    }

    // One unpacked attribute accessor: its floats, plus the stride cgltf actually wrote them at.
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

    // The 12-byte KTX2 file identifier. KHR_texture_basisu images are KTX2 containers,
    // which stb_image cannot decode; they are routed to the basisu transcoder instead.
    bool is_ktx2(const uint8_t *data, size_t size)
    {
        static const uint8_t magic[12] = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
        return data && size >= sizeof(magic) && std::memcmp(data, magic, sizeof(magic)) == 0;
    }

    // The WebP RIFF container signature: "RIFF" at byte 0 and "WEBP" at byte 8. EXT_texture_webp
    // images are WebP, which stb_image cannot decode; they are routed to libwebp instead.
    bool is_webp(const uint8_t *data, size_t size)
    {
        return data && size >= 12 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBP", 4) == 0;
    }

    // Read an entire file into out. Returns false (out untouched) on any error. Used to
    // slurp external image files so they can be content-sniffed and routed uniformly,
    // the same way embedded (buffer_view) images already are. Uses the FILE idiom from
    // mesh_stl.cpp (fread takes void*, so no byte-pointer cast; unique_ptr owns the
    // handle; platform::file_size sizes it 64-bit so >= 2 GB sidecars work on Windows).
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
        // A size the vector can never hold (possible on ILP32 now that sizing is 64-bit;
        // max_size() there is PTRDIFF_MAX = 2 GiB-1, NOT the 4 GiB-1 of size_t) must be
        // rejected up front: resize would throw length_error, which the bad_alloc-only
        // catch below misses, escaping onto a worker thread (= std::terminate).
        if (static_cast<uint64_t>(len) > buf.max_size())
        {
            return false;
        }
        // The file size is unbounded (arbitrary external sidecar). Decode runs on worker
        // threads with no exception boundary at the load site, so a bad_alloc here would
        // terminate the process; fail loud instead, matching decode_ktx2_rgba's guard.
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

    // Resolve the image a texture should sample. KHR_texture_basisu (KTX2) and EXT_texture_webp
    // each carry a separate image alongside the standard fallback (image); prefer an extension
    // source when present so we decode the intended texture, not the optional PNG/JPEG fallback
    // (which may be absent). Precedence is KTX2 -> WebP -> plain; a texture carrying both
    // extensions is rare, but the order must be deterministic.
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

    // glTF sampler wrap -> WrapMode. A null sampler (texture declared none) defaults to
    // Repeat per the glTF spec; cgltf reports 0 for an omitted field, which also maps to Repeat.
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

    // RAII guard: ensures cgltf_free on every exit path.
    const auto guard = std::unique_ptr<cgltf_data, decltype(&cgltf_free)>(data, cgltf_free);

    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success)
    {
        return false;
    }
    if (cgltf_validate(data) != cgltf_result_success)
    {
        return false;
    }

    // EXT_meshopt_compression / KHR_meshopt_compression: cgltf parses both (same JSON shape,
    // same fields) but decodes nothing, so decompress each compressed buffer view in place and
    // assign view->data; cgltf_buffer_view_data() then transparently serves the decoded bytes
    // to every consumer. The buffer MUST come from data->memory.alloc_func, never new/malloc:
    // cgltf_free owns view->data, and mismatched allocators double-free. Runs after
    // cgltf_validate, so its meshopt invariants (mc.buffer non-null, buffer size >= offset+size,
    // bv.size == stride*count, valid
    // mode/filter strides) already hold. view->data is assigned right after alloc, so any early
    // return below is cleaned up by the cgltf_free guard; no manual free path.
    for (size_t i = 0; i < data->buffer_views_count; i++)
    {
        cgltf_buffer_view &bv = data->buffer_views[i];
        if (!bv.has_meshopt_compression)
        {
            continue;
        }
        const cgltf_meshopt_compression &mc = bv.meshopt_compression;
        // Defensive: cgltf checked bv.size == stride*count, but if that product
        // overflowed size_t and wrapped to a small bv.size the check still passes;
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
            return false; // corrupt / truncated stream, fail loud
        }
        // Filters are applied in place after the vertex decode. cgltf maps any
        // unrecognized filter string to filter_none (zero-init default), so the
        // none/default arm also covers a hypothetical future filter parsed by an
        // older cgltf: it would skip filtering rather than fail. Harmless today:
        // the KHR_meshopt_compression spec defines exactly these four filters and
        // cgltf knows all of them.
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

    // Default white material at index 0.
    materials.push_back(Material{});

    // A deferred texture decode: the preferred image, plus an optional fallback tried
    // if the preferred one fails to decode. For KHR_texture_basisu / EXT_texture_webp the
    // preferred image is the extension source and the fallback is the texture's ordinary
    // source: the extension provides it precisely so a renderer that can't decode a given
    // extension image can degrade to the plain image instead of rendering untextured.
    struct TexRequest
    {
        const cgltf_image *primary;
        const cgltf_image *fallback; // nullptr when none
        WrapMode wrap_s;
        WrapMode wrap_t;
    };

    // Register a texture, returning its slot index. Each distinct decode is registered
    // once; the actual decode is deferred and run in parallel after the scene walk. Slot
    // order follows first-encounter order. Dedup is keyed on (primary, fallback, wrap_s,
    // wrap_t), not the images alone: wrap mode is a property of the (image, sampler) pair =
    // the glTF texture, so two textures sharing one image but different samplers must stay
    // distinct slots. (The fallback is in the key for the same reason as before: two basisu
    // textures can share one KTX2 source yet declare different ordinary-source fallbacks.
    // Trade-off: those rare splits decode the shared source twice; the common case dedups.)
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
            // The 64-bit FNV-prime mix is computed wide, then truncated to size_t (a no-op at
            // LP64; an accepted hash truncation at ILP32 where size_t is 32-bit). The two wrap
            // enums fold into the low bits (each <4, so a 2-bit shift keeps them disjoint).
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
        // Fallback applies only when an extension source (KTX2 or WebP) was preferred over a
        // distinct ordinary source on the same texture. The single fallback is deliberately the
        // plain image (tex->image), not a chain: that is the one fallback glTF defines (both
        // KHR_texture_basisu and EXT_texture_webp designate texture.source for clients that
        // cannot decode the extension image). So a texture carrying BOTH extensions but no plain
        // image is NOT tried as KTX2->WebP->plain: a failed KTX2 drops the texture without trying
        // the WebP peer, which lost precedence in pick_image and is not a defined fallback. That
        // input is self-contradictory in practice (an author encoding both compressed forms would
        // not omit the universally-decodable plain source) and degrades gracefully, so we mirror
        // the spec's single fallback slot rather than invent an inter-extension chain.
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

    // True once any texture binding selects TEXCOORD_1 (textureInfo.texCoord == 1). Combined
    // after the walk with whether any primitive actually provided TEXCOORD_1 to set Mesh::has_uv1.
    bool any_uv1_referenced = false;

    // Bake KHR_texture_transform into a slot's 2x3 affine. The spec composes T = translate ·
    // rotate · scale on v-down glTF UVs, but we store UVs v-flipped (uv.y = 1 - v_gltf) and the
    // sampler re-flips; working that round trip through shows the texel actually read sits at
    // the spec transform with the ROTATION ANGLE NEGATED (only the sin terms flip; cos, offset
    // and axis-aligned scale are unchanged), the standard flipY compensation
    // every glTF reference renderer makes (three.js, Babylon, Filament). The naive +θ points the
    // Khronos TextureTransformTest arrow at its dedicated red "opposite direction" marker, the
    // asset's purpose-built diagnostic for exactly this sign slip. So
    // s = -sin below, and the offset coefficients carry the v-flip fold; verified end-to-end
    // against the reference render (matches_gltf_spec_sampling, rotation_handedness_end_to_end).
    // Acts on stored UVs (c=cos, s=-sin, sx/sy=scale, ox/oy=offset; TexSlot in light.h).
    // Animated transforms are not handled: static authored values baked once at load.
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

    // Resolve a binding's UV set and KHR_texture_transform. glTF caps meaningfully at two
    // sets here: texCoord 1 selects TEXCOORD_1; 0 (or an unsupported texCoord >= 2)
    // degrades to TEXCOORD_0. KHR_texture_transform's own texcoord (when present) overrides
    // textureInfo.texCoord per spec. A reference to an absent set is reconciled after the
    // walk (see the finalize block), not here.
    auto bind_slot = [&](TexSlot &slot, const cgltf_texture_view &view)
    {
        int tc = view.texcoord;
        if (view.has_transform && view.transform.has_texcoord)
        {
            tc = view.transform.texcoord;
        }
        // Only TEXCOORD_0 and _1 are stored; texCoord >= 2 deliberately degrades to set 0.
        // The spec asks clients to support at least two UV sets (SHOULD, not MUST), and >2
        // sets are vanishingly rare: supporting them means parallel uv2/uv3… arrays through
        // the whole pipeline, out of proportion to the gain.
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
            // rasterminal has no HDR/tonemap, so a load-time multiply is equivalent to the
            // per-frame intensity multiply real-time engines do in their shader. Clamp the
            // strength to [0, 1e6] for the same reason as the factor above (kill +Inf at the
            // source). Each input is bounded; the baked product can exceed 1e6 (up to ~1e12),
            // which is fine: the point is staying finite, and vec3_to_color saturates it.
            const float s = std::clamp(m->emissive_strength.emissive_strength, 0.0f, 1e6f);
            mat.emissive = mat.emissive * s;
        }
        // Spec-literal: emissive = factor × texture. A zero factor means the texture cannot
        // contribute (do_emissive in the rasterizer is gated on factor>0), so skip the decode
        // entirely: saves a stb_image_load (often multi-MB) and the permanent RAM footprint
        // for a texture no fragment will ever sample. dedup is unaffected: if the same image
        // is also bound as e.g. diffuse, that call still registers and decodes it.
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
        // KHR_materials_transmission: no refraction in this CPU viewer, so approximate glass as
        // alpha blending, with the alpha floored so the shape and highlights stay visible at
        // transmissionFactor 1. Applied only when the base material declared neither BLEND nor
        // MASK (the alpha_cutoff == 0 guard: a MASK surface declaring transmission keeps its
        // authored binary cutout). KHR_materials_volume.attenuationColor folds in as a faint
        // uniform surface tint; thickness is not modelled, so the reference render's
        // depth-dependent deepening is not reproduced.
        if (!mat.blend && mat.alpha_cutoff == 0.0f && m->has_transmission && m->transmission.transmission_factor > 0.0f)
        {
            constexpr float GLASS_ALPHA_FLOOR = 0.18f;
            mat.blend = true;
            mat.alpha = std::clamp(1.0f - m->transmission.transmission_factor, GLASS_ALPHA_FLOOR, 1.0f);
            // Force double-sided: a transmissive surface is physically a volume (light passes
            // through both the near and far interface), but glass is commonly authored
            // single-sided because a real transmission renderer traces through the volume. In our
            // alpha-blend approximation, culling the back faces would drop the far shell of a
            // closed glass mesh and make it look thin, so both shells must enter the A-buffer and
            // composite back-to-front. Scoped to the transmission approximation; genuine
            // alphaMode=BLEND still honours the authored doubleSided flag.
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
    // Set by visit() when a Draco primitive fails to decode; checked after the
    // walk so a corrupt bitstream fails the whole load (visit() returns void).
    bool draco_error = false;

    // Attribute staging, reused across primitives. cgltf_accessor_read_float re-tests sparseness,
    // re-derives the buffer-view pointer and re-dispatches on component type for EVERY element;
    // unpacking the accessor once hoists all of that out of the vertex loop and reduces the common
    // case (float32, tightly packed, not sparse) to a single memcpy inside cgltf. On a 645k-triangle
    // city this took the scene walk from 1406 ms to a fraction of it. Grow-only, so a scene of many
    // primitives allocates a handful of times; one buffer per attribute, since building a vertex
    // needs several of them at once.
    std::vector<float> buf_pos;
    std::vector<float> buf_norm;
    std::vector<float> buf_uv;
    std::vector<float> buf_uv1;
    std::vector<float> buf_color;
    // Unpack `acc` into `buf`, empty when there is no accessor or it carries fewer than `comps`
    // components. The stride is the ACCESSOR's component count, never `comps`: cgltf writes that
    // many floats per element and scatters a sparse accessor's overrides at that stride across the
    // full element count. Sizing or indexing by `comps` therefore reads interleaved garbage from a
    // wider accessor and, when that accessor is also sparse, writes past the end of the buffer.
    // A file reaches this because cgltf_validate constrains an attribute's type only to "valid",
    // never to the one its semantic prescribes, so POSITION declared VEC4 passes it. Refusing a
    // NARROWER accessor reads it as absent, which every caller already handles, rather than leaving
    // the components it does not carry uninitialized.
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
        // A short read (a malformed accessor that cgltf_validate let through) leaves the tail
        // zeroed rather than holding the previous primitive's values.
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

    // Maintain the parallel uv1 array (glTF TEXCOORD_1) for one primitive's vertex range
    // [vert_base, vert_base + n). On the first TEXCOORD_1-bearing primitive, back-fill every
    // earlier vertex with its own uv0 (so a uv_set==1 sample on a vertex that lacks a real
    // second set degrades to TEXCOORD_0); thereafter every primitive contributes n entries:
    // the real flipped uv1 when has_real, else a copy of that vertex's uv0. This keeps
    // uv1.size() == vertices.size() once building, so compute_normals/optimize carry it like
    // vertex_colors. read(i) is only invoked when has_real. Shared by the accessor and Draco paths.
    auto append_uv1 = [&](size_t vert_base, size_t n, bool has_real, auto &&read)
    {
        if (has_real && !building_uv1)
        {
            building_uv1 = true;
            // Sized for the spike this absorbs: the back-fill loop + this primitive together push
            // exactly vertices.size() entries, so one alloc covers both. Later primitives grow it
            // again (amortized O(1)), deliberately matching the per-primitive growth of the sibling
            // vertex_colors/vertex_alpha arrays rather than pre-walking the scene for a global total.
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
            // Degenerate (|det| ~ 0): no usable inverse, so fall back to the
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
                // Triangle strips/fans are de-stripified into the triangle list below; points and
                // lines have no surface to rasterize and are unsupported (dropped here).
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
                    // base is UB: a malformed glTF can omit the top-level "accessors"
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

                    // cgltf_buffer_view_data handles meshopt-compression overrides / sparse / offset
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
                    // dm.colors_alpha is non-empty only when COLOR_0 was 4-component. Honour that
                    // opacity only under alphaMode=BLEND, the same vec4-under-BLEND gate the accessor
                    // path uses (see the COLOR_0 block below), so Draco and uncompressed primitives
                    // behave identically. Without BLEND, vertex_alpha stays empty (zero-cost, and no
                    // mis-classification in the per-triangle blend partition).
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

                    // Connectivity comes from the Draco bitstream itself
                    // (dm.indices), so prim.indices and the accessor-path's
                    // non-indexed branch are both intentionally bypassed: a Draco
                    // primitive's glTF-level indices accessor is metadata only per
                    // the KHR_draco_mesh_compression spec.
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

                // norm_a, not norm_acc: a NORMAL narrower than vec3 was read as absent above, so
                // these vertices carry no normal. Keying the flag on the accessor's mere presence
                // would skip compute_normals and leave the whole primitive at a zero normal.
                if (norm_a)
                {
                    has_normals = true;
                }

                if (color_acc)
                {
                    vertex_colors.resize(vert_base + n_verts, { 1.0f, 1.0f, 1.0f });
                    // COLOR_0 may be vec3 or vec4; only vec4 carries opacity, and per the glTF spec
                    // that opacity is honoured only under alphaMode=BLEND (OPAQUE/MASK ignore the
                    // base-colour alpha entirely). Populating vertex_alpha only for vec4-under-BLEND
                    // keeps the array empty (zero-cost) on the common no-alpha case AND prevents the
                    // per-triangle blend partition from mis-classifying an OPAQUE primitive that
                    // happens to author a sub-1 COLOR_0 alpha.
                    const bool color_has_alpha = (color_acc->type == cgltf_type_vec4) && prim.material &&
                                                 prim.material->alpha_mode == cgltf_alpha_mode_blend;
                    if (color_has_alpha)
                    {
                        vertex_alpha.resize(vert_base + n_verts, 1.0f);
                    }
                    // Three components asked for, so a vec3 accessor is accepted; the fourth is read
                    // only under color_has_alpha, which requires vec4 and so a stride of four.
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

                // Push triangles. Strips/fans are de-stripified here into the triangle list; the rest
                // of the pipeline only ever sees independent triangles. count-2 triangles, winding per
                // glTF/GL spec (odd strip triangles swap the first two verts so all share one winding).
                // Degenerate stitch triangles (repeated index) are kept: render-time backface/zero-area
                // drop handles them. cgltf_validate bounds every index < n_verts but allows count<3 and
                // non-multiples of 3, so the loop bound and the count>=3 reserve guard are load-bearing.
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
                        // Rolling window: read each source index exactly once.
                        // cgltf_accessor_read_index re-derives the buffer pointer per call, so
                        // re-reading the two shared edge indices on every triangle would triple
                        // the reads. i0/i1 hold the previous two indices; for a fan i0 stays
                        // pinned to the hub (src(0)). Parity of i (== parity of triangle i-2)
                        // selects the strip's odd-triangle swap.
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

    // glTF assets may carry COLOR_0 on only part of the mesh; pad the gap white
    // so the parallel-array invariant (vertex_colors.size() == vertices.size())
    // holds before compute_normals splits and before tangents/AO/vcache run.
    if (has_vertex_colors && vertex_colors.size() < vertices.size())
    {
        vertex_colors.resize(vertices.size(), vec3{ 1.0f, 1.0f, 1.0f });
    }
    if (has_vertex_alpha && vertex_alpha.size() < vertices.size())
    {
        vertex_alpha.resize(vertices.size(), 1.0f);
    }

    // Reconcile TEXCOORD_1 (run before compute_normals so the split carries uv1, and before
    // tangents/optimize). uv1 is worth keeping only when the geometry actually provided a second
    // set (building_uv1) AND some texture binding references it (any_uv1_referenced). Otherwise
    // drop it and force every binding back to set 0: this covers both "referenced but absent"
    // (degrade per runtime-loader convention, like a missing texture) and "present but unused"
    // (read-then-drop). When kept, append_uv1 already holds uv1.size() == vertices.size(); the
    // defensive pad mirrors the vertex_colors block above for any future partial-fill path.
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

    // Route a raw image blob to the right decoder by content sniff (not file extension), so
    // external sidecars and embedded buffer_view images are handled identically. KTX2 and WebP
    // have their own decoders; everything else goes through stb_image. Returns an invalid
    // Texture on any failure.
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
            // Inline data: URIs. cgltf expands base64 *buffer* URIs during
            // cgltf_load_buffers but leaves *image* URIs untouched, so decode the base64
            // payload here and route the bytes through the same content-sniff as
            // buffer_view images. Only the ";base64" form is handled; a non-base64 data:
            // URI (percent-encoded raw bytes) is rare and dropped, matching every other
            // decode-failure path here (return an invalid Texture, model still loads).
            if (std::strncmp(img->uri, "data:", 5) == 0)
            {
                const char *comma = std::strchr(img->uri, ',');
                // Require the ";base64" marker immediately before the comma (cgltf's own
                // rule). The `comma - img->uri >= 7` guard both rejects a comma-less data:
                // string and prevents the strncmp from reading before the buffer when the
                // metadata is shorter than ";base64".
                if (!comma || comma - img->uri < 7 || std::strncmp(comma - 7, ";base64", 7) != 0)
                {
                    return Texture{};
                }
                const char *payload = comma + 1;
                // Output size from the count of base64-alphabet chars, stopping at the
                // first non-alphabet char so `=` padding and any junk/whitespace tail are
                // excluded (a glTF data URI is never MIME-wrapped, so a stop on whitespace
                // can only mean a malformed payload, which then fails to decode below).
                // Computed without a multiply so it cannot wrap size_t on the ILP32 build.
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
                // cgltf_load_buffer_base64 reads ceil(out_size*8/6) <= nchars chars, so it
                // never reads a pad byte or past the valid run. It allocates via opts'
                // allocator and returns a result code instead of throwing, safe at this
                // no-exception-boundary worker site.
                void *raw = nullptr;
                if (cgltf_load_buffer_base64(&opts, out_size, payload, &raw) != cgltf_result_success)
                {
                    return Texture{};
                }
                // opts carries no custom allocator, so cgltf used cgltf_default_alloc ==
                // malloc; free with the matching deallocator on every exit path.
                const auto owned = std::unique_ptr<void, void (*)(void *)>(raw, std::free);
                return decode_bytes(static_cast<const uint8_t *>(raw), out_size);
            }
            // Percent-decode the URI before opening the file. cgltf decodes escapes for
            // buffer URIs but not image URIs, so e.g. "my%20tex.ktx2" would otherwise fail
            // to open. Decode only the uri (not the dir prefix), matching cgltf's own
            // buffer-load path; cgltf_decode_uri rewrites in place and returns the new len.
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
            // cgltf_buffer_view_data honours meshopt-compression overrides; returns
            // null when the backing external buffer was never loaded.
            const uint8_t *ptr = cgltf_buffer_view_data(img->buffer_view);
            if (!ptr)
            {
                return Texture{};
            }
            return decode_bytes(ptr, img->buffer_view->size);
        }
        return Texture{};
    };

    // Decode all registered images in parallel. data (held by guard) and dir outlive
    // the join, so worker reads of buffer_view->buffer->data are valid. On a failed
    // extension decode (KTX2 transcode or WebP decode), fall back to the texture's
    // ordinary source if it provided one (see load_tex for why that fallback is single).
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
