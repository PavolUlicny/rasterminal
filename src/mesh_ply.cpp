#include "mesh.h"
#include "light.h"
#include "mesh_loader.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#define TINYPLY_IMPLEMENTATION
#include "tinyply.h"

// ─── typed-buffer helpers ────────────────────────────────────────────────────

namespace
{

    float rd_f(const uint8_t *buf, tinyply::Type t, size_t i)
    {
        if (t == tinyply::Type::FLOAT64)
        {
            double d = 0.0;
            std::memcpy(&d, buf + (i * 8), 8);
            return static_cast<float>(d);
        }
        float f = 0.0f;
        std::memcpy(&f, buf + (i * 4), 4);
        return f;
    }

    float rd_col(const uint8_t *buf, tinyply::Type t, size_t i)
    {
        if (t == tinyply::Type::UINT8)
        {
            return static_cast<float>(buf[i]) / 255.0f;
        }
        if (t == tinyply::Type::FLOAT64)
        {
            double d = 0.0;
            std::memcpy(&d, buf + (i * 8), 8);
            return static_cast<float>(d);
        }
        float f = 0.0f;
        std::memcpy(&f, buf + (i * 4), 4);
        return f;
    }

    // rd_col decodes only these three layouts (UINT8 normalized, FLOAT32, FLOAT64).
    // Any other declared type would hit rd_col's 4-byte fall-through read, which both
    // misinterprets the bytes and runs past a 1- or 2-byte-per-element buffer. Loaders
    // validate against this before using rd_col, then fail loud.
    bool rd_col_supported(tinyply::Type t)
    {
        return t == tinyply::Type::UINT8 || t == tinyply::Type::FLOAT32 || t == tinyply::Type::FLOAT64;
    }

    uint32_t rd_idx(const uint8_t *buf, tinyply::Type t, size_t i)
    {
        switch (t)
        {
        case tinyply::Type::UINT8:
            return buf[i];
        case tinyply::Type::INT8:
            return static_cast<uint32_t>(static_cast<int8_t>(buf[i]));
        case tinyply::Type::UINT16:
        {
            uint16_t v = 0;
            std::memcpy(&v, buf + (i * 2), 2);
            return v;
        }
        case tinyply::Type::INT16:
        {
            int16_t v = 0;
            std::memcpy(&v, buf + (i * 2), 2);
            return static_cast<uint32_t>(v);
        }
        case tinyply::Type::INT32:
        {
            int32_t v = 0;
            std::memcpy(&v, buf + (i * 4), 4);
            return static_cast<uint32_t>(v);
        }
        default:
        {
            uint32_t v = 0;
            std::memcpy(&v, buf + (i * 4), 4);
            return v;
        }
        }
    }

    size_t type_stride(tinyply::Type t)
    {
        switch (t)
        {
        case tinyply::Type::INT8:
        case tinyply::Type::UINT8:
            return 1;
        case tinyply::Type::INT16:
        case tinyply::Type::UINT16:
            return 2;
        case tinyply::Type::FLOAT64:
            return 8;
        default:
            return 4;
        }
    }

    uint32_t float_bits(float f)
    {
        uint32_t b = 0;
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }

    // Vertex-dedup key for the PLY split-by-UV path: source position index plus
    // bit-exact U and V. Stored as the map key (not just its hash) so that on a
    // hash collision unordered_map falls back to operator== and the distinct
    // tuples stay separate — silent merging on collision would be a one-vertex
    // texturing artifact at 1/2^64 probability per pair, which we don't accept.
    struct UVKey
    {
        uint32_t pos;
        uint32_t u;
        uint32_t v;
        bool operator==(const UVKey &o) const noexcept { return pos == o.pos && u == o.u && v == o.v; }
    };

    struct UVKeyHash
    {
        size_t operator()(const UVKey &k) const noexcept
        {
            const uint64_t h = (static_cast<uint64_t>(k.pos) * 0x9E3779B185EBCA87ull) ^
                               (static_cast<uint64_t>(k.u) * 0xC2B2AE3D27D4EB4Full) ^
                               (static_cast<uint64_t>(k.v) * 0x165667B19E3779F9ull);
            return static_cast<size_t>(h);
        }
    };

} // namespace

// ─── Mesh::load_ply ───────────────────────────────────────────────────────────

bool Mesh::load_ply(const std::string &path, float crease_cos)
{
    MeshSnapshot snap(*this);

    std::ifstream ss(path, std::ios::binary);
    if (!ss.is_open())
    {
        return false;
    }

    tinyply::PlyFile file;
    try
    {
        file.parse_header(ss);
    }
    catch (...)
    {
        return false;
    }

    // Security: validate element counts vs remaining file size before tinyply
    // allocates any buffers. Guards against maliciously large element counts.
    {
        const auto data_start = ss.tellg();
        ss.seekg(0, std::ios::end);
        const uint64_t data_bytes = static_cast<uint64_t>(ss.tellg()) - static_cast<uint64_t>(data_start);
        ss.seekg(data_start);
        for (const auto &el : file.get_elements())
        {
            if (el.size > 0 && static_cast<uint64_t>(el.size) > data_bytes)
            {
                return false;
            }
        }
    }

    std::shared_ptr<tinyply::PlyData> positions;
    std::shared_ptr<tinyply::PlyData> normals;
    std::shared_ptr<tinyply::PlyData> uvs;
    std::shared_ptr<tinyply::PlyData> vcolors;
    std::shared_ptr<tinyply::PlyData> valpha;
    std::shared_ptr<tinyply::PlyData> faces;
    std::shared_ptr<tinyply::PlyData> fcolors;

    try
    {
        positions = file.request_properties_from_element("vertex", { "x", "y", "z" });
    }
    catch (...)
    {
        return false;
    }

    try
    {
        normals = file.request_properties_from_element("vertex", { "nx", "ny", "nz" });
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
    }

    // UV aliases tried in priority order.
    if (!uvs)
    {
        try
        {
            uvs = file.request_properties_from_element("vertex", { "u", "v" });
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
    }
    if (!uvs)
    {
        try
        {
            uvs = file.request_properties_from_element("vertex", { "s", "t" });
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
    }
    if (!uvs)
    {
        try
        {
            uvs = file.request_properties_from_element("vertex", { "texture_u", "texture_v" });
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
    }

    // Vertex colors: red/green/blue first, then r/g/b.
    try
    {
        vcolors = file.request_properties_from_element("vertex", { "red", "green", "blue" });
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
    }
    if (!vcolors)
    {
        try
        {
            vcolors = file.request_properties_from_element("vertex", { "r", "g", "b" });
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
    }

    // Optional per-vertex opacity (separate property so a file without it still loads).
    // Consumed by both the shared-vertex and welded (split-by-UV) paths below; a sub-1
    // value marks the mesh blended (PLY has no material system, so opacity rides on the
    // default mat). Gated on vcolors by design: PLY's `alpha` is conventionally the 4th
    // channel of RGB vertex color, not a standalone opacity property, so a file with alpha
    // but no RGB is not treated as translucent (CloudCompare likewise reads alpha only
    // alongside RGB).
    if (vcolors)
    {
        try
        {
            valpha = file.request_properties_from_element("vertex", { "alpha" });
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
        if (!valpha)
        {
            try
            {
                valpha = file.request_properties_from_element("vertex", { "a" });
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
            }
        }
    }

    // Face indices (try both common property names).
    try
    {
        faces = file.request_properties_from_element("face", { "vertex_indices" });
    }
    catch (...)
    {
        try
        {
            faces = file.request_properties_from_element("face", { "vertex_index" });
        }
        catch (...)
        {
            return false;
        }
    }

    // Face-list texcoords (per-corner UVs). Photogrammetry / scanner PLYs put
    // UVs here, not on vertices, because face-list UVs can represent UV seams
    // (one position with distinct UVs on different faces) and per-vertex UVs
    // cannot. When present, they win over any per-vertex UV aliases — matches
    // MeshLab. `texcoord` is the canonical name; `texture_uv` is an older alias.
    std::shared_ptr<tinyply::PlyData> tc;
    try
    {
        tc = file.request_properties_from_element("face", { "texcoord" });
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
    }
    if (!tc)
    {
        try
        {
            tc = file.request_properties_from_element("face", { "texture_uv" });
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
    }

    // Per-face colors only when vertex colors are absent (vertex colors take priority).
    if (!vcolors)
    {
        try
        {
            fcolors = file.request_properties_from_element("face", { "red", "green", "blue" });
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
        if (!fcolors)
        {
            try
            {
                fcolors = file.request_properties_from_element("face", { "r", "g", "b" });
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
            }
        }
    }

    try
    {
        file.read(ss);
    }
    catch (...)
    {
        return false;
    }

    // Every color/alpha property below is decoded with rd_col, which only handles
    // UINT8/FLOAT32/FLOAT64. A different declared type would make rd_col read past
    // the buffer (its 4-byte stride exceeds a 1- or 2-byte element) — the file-size
    // guard above checks counts, not rd_col's stride assumption. Reject up front.
    if ((vcolors && !rd_col_supported(vcolors->t)) || (valpha && !rd_col_supported(valpha->t)) ||
        (fcolors && !rd_col_supported(fcolors->t)))
    {
        return false;
    }

    const size_t n_verts = positions->count;
    const size_t n_faces = faces->count;
    if (n_verts == 0 || n_faces == 0)
    {
        return false;
    }

    // Mixed n-gon faces (tinyply populates list_sizes only when per-face index
    // counts differ) leave ipf undefined — total_idx / n_faces rounds and every
    // per-face stride below is then wrong. Reject rather than silently mis-index,
    // consistent with the loader's fail-loud strictness.
    if (!faces->list_sizes.empty())
    {
        return false;
    }

    // Uniform list length is now guaranteed, so ipf = total_idx / n_faces is exact.
    const size_t idx_stride = type_stride(faces->t);
    const size_t total_idx = faces->buffer.size_bytes() / idx_stride;
    const size_t ipf = total_idx / n_faces;
    if (ipf < 3)
    {
        return false;
    }

    // Validate face-list texcoord against the uniform-ipf assumption the index
    // path also relies on. Mixed n-gon counts (non-empty list_sizes), wrong
    // float count per face, or a non-float element type make per-corner indexing
    // ambiguous — hard-reject the file, consistent with the loader's strictness.
    if (tc)
    {
        const bool float_type = (tc->t == tinyply::Type::FLOAT32 || tc->t == tinyply::Type::FLOAT64);
        if (!float_type || !tc->list_sizes.empty() || tc->count != n_faces)
        {
            return false;
        }
        // Per-face float count must be exactly 2 * ipf. Checked via division, not
        // a `n_faces * ipf * 2 * tc_stride` multiply: that product is computed in
        // size_t and on a crafted multi-GB file could wrap to match the actual
        // buffer size, letting later rd_f() reads run past the buffer. The file's
        // security model is file-size-based bounds, so the gate must not overflow.
        const size_t tc_stride = type_stride(tc->t);
        const size_t tc_bytes = tc->buffer.size_bytes();
        if (tc_bytes % tc_stride != 0)
        {
            return false;
        }
        const size_t tc_floats = tc_bytes / tc_stride;
        if (tc_floats % n_faces != 0 || tc_floats / n_faces != ipf * 2)
        {
            return false;
        }
    }

    const bool use_face_colors = (fcolors != nullptr);
    const bool use_face_texcoord = (tc != nullptr);

    materials.push_back(Material{});

    // Captured by the split-by-UV path; consumed by the post-load compute_normals
    // call so UV-seam halves smooth across the seam (mirrors the OBJ loader).
    std::vector<uint32_t> ply_weld;
    bool use_weld = false;

    // ── Split-by-UV path: per-face texcoord lists, no face colors ────────────
    // Hash-dedups vertices by (pos_idx, u, v) so corners that genuinely share
    // position+UV collapse to one vertex, but distinct UVs at the same position
    // (UV seams) become separate vertices. The weld map records the source
    // position id per output vertex so compute_normals smooths seams as one
    // surface. Mirrors the OBJ loader.
    if (use_face_texcoord && !use_face_colors)
    {
        const uint8_t *pb = positions->buffer.get();
        const uint8_t *nb = normals ? normals->buffer.get() : nullptr;
        const uint8_t *cb = vcolors ? vcolors->buffer.get() : nullptr;
        const uint8_t *ab = valpha ? valpha->buffer.get() : nullptr;
        const uint8_t *tcb = tc->buffer.get();
        const tinyply::Type tct = tc->t;
        const uint8_t *fb = faces->buffer.get();

        // Reserve to the realistic floor (n_verts), not the no-sharing worst case
        // (n_faces * ipf). The typical photogrammetry scan has one UV seam per
        // ~20 positions, so unique-tuple count is close to n_verts; sizing to the
        // worst case wastes 3–6× memory for the program lifetime (vertices keeps
        // its capacity) and forces a giant bucket array we never use. Vector and
        // hashmap growth handles the seam-split overshoot in amortized O(1).
        // The weld map is only consumed by compute_normals, which the post-load
        // block below skips when the file authored its own normals. Skip the
        // allocate-and-fill when it would be discarded — for a multi-million-
        // vertex authored-normal scan, that's the difference between ~20 MB of
        // throwaway weld work and zero.
        const bool need_weld = !normals;

        std::unordered_map<UVKey, uint32_t, UVKeyHash> vert_cache;
        vert_cache.reserve(n_verts);
        vertices.reserve(n_verts);
        if (need_weld)
        {
            ply_weld.reserve(n_verts);
        }
        if (vcolors)
        {
            vertex_colors.reserve(n_verts);
        }
        if (ab)
        {
            vertex_alpha.reserve(n_verts);
        }

        auto get_vertex = [&](uint32_t pos_idx, size_t corner_float_off) -> uint32_t
        {
            const float u = rd_f(tcb, tct, corner_float_off);
            const float v = rd_f(tcb, tct, corner_float_off + 1);
            // Bit-exact float dedup: legitimately-shared corners are written with
            // identical bytes by every exporter; any non-bit-equal pair is a
            // genuine seam and stays split.
            const UVKey key{ pos_idx, float_bits(u), float_bits(v) };
            const auto [it, inserted] = vert_cache.emplace(key, static_cast<uint32_t>(vertices.size()));
            if (!inserted)
            {
                return it->second;
            }
            const size_t p3 = static_cast<size_t>(pos_idx) * 3;
            Vertex vert{};
            vert.pos = { rd_f(pb, positions->t, p3), rd_f(pb, positions->t, p3 + 1), rd_f(pb, positions->t, p3 + 2) };
            if (nb)
            {
                vert.normal = { rd_f(nb, normals->t, p3), rd_f(nb, normals->t, p3 + 1), rd_f(nb, normals->t, p3 + 2) };
            }
            vert.uv = { u, v };
            vert.ao = 1.0f;
            vertices.push_back(vert);
            if (cb)
            {
                vertex_colors.emplace_back(
                    rd_col(cb, vcolors->t, p3), rd_col(cb, vcolors->t, p3 + 1), rd_col(cb, vcolors->t, p3 + 2)
                );
            }
            if (ab)
            {
                vertex_alpha.push_back(rd_col(ab, valpha->t, pos_idx));
            }
            if (need_weld)
            {
                ply_weld.push_back(pos_idx);
            }
            return it->second;
        };

        triangles.reserve(n_faces * (ipf - 2));
        for (size_t f = 0; f < n_faces; f++)
        {
            const size_t face_uv_base = f * ipf * 2;
            const uint32_t i0_raw = rd_idx(fb, faces->t, f * ipf);
            for (size_t v = 1; v + 1 < ipf; v++)
            {
                const uint32_t iv_raw = rd_idx(fb, faces->t, (f * ipf) + v);
                const uint32_t iw_raw = rd_idx(fb, faces->t, (f * ipf) + v + 1);
                if (i0_raw >= n_verts || iv_raw >= n_verts || iw_raw >= n_verts)
                {
                    continue;
                }
                const uint32_t v0 = get_vertex(i0_raw, face_uv_base);
                const uint32_t vi = get_vertex(iv_raw, face_uv_base + (v * 2));
                const uint32_t vj = get_vertex(iw_raw, face_uv_base + ((v + 1) * 2));
                Triangle t;
                t.v[0] = v0;
                t.v[1] = vi;
                t.v[2] = vj;
                t.material_idx = 0;
                triangles.push_back(t);
            }
        }

        if (vcolors)
        {
            has_vertex_colors = true;
        }
        if (ab)
        {
            // PLY has no per-material opacity mode, so per-vertex alpha is the opacity signal.
            // load_model's per-triangle partition routes only the genuinely-translucent triangles
            // to the transparent pass (so an opaque region keeps the fast path and casts shadows).
            has_vertex_alpha = true;
        }
        use_weld = need_weld;
    }
    // ── Standard path: shared vertices ───────────────────────────────────────
    else if (!use_face_colors)
    {
        const uint8_t *pb = positions->buffer.get();
        const uint8_t *nb = normals ? normals->buffer.get() : nullptr;
        const uint8_t *ub = uvs ? uvs->buffer.get() : nullptr;

        vertices.reserve(n_verts);
        for (size_t i = 0; i < n_verts; i++)
        {
            Vertex v{};
            v.pos = { rd_f(pb, positions->t, i * 3), rd_f(pb, positions->t, (i * 3) + 1),
                      rd_f(pb, positions->t, (i * 3) + 2) };
            if (nb)
            {
                v.normal = { rd_f(nb, normals->t, i * 3), rd_f(nb, normals->t, (i * 3) + 1),
                             rd_f(nb, normals->t, (i * 3) + 2) };
            }
            if (ub)
            {
                v.uv = { rd_f(ub, uvs->t, i * 2), rd_f(ub, uvs->t, (i * 2) + 1) };
            }
            v.ao = 1.0f;
            vertices.push_back(v);
        }

        if (vcolors)
        {
            const uint8_t *cb = vcolors->buffer.get();
            vertex_colors.resize(n_verts);
            for (size_t i = 0; i < n_verts; i++)
            {
                vertex_colors[i] = { rd_col(cb, vcolors->t, i * 3), rd_col(cb, vcolors->t, (i * 3) + 1),
                                     rd_col(cb, vcolors->t, (i * 3) + 2) };
            }
            has_vertex_colors = true;

            if (valpha)
            {
                // Per-vertex alpha is PLY's only opacity signal; load_model's per-triangle
                // partition routes just the translucent triangles to the transparent pass.
                const uint8_t *ab = valpha->buffer.get();
                vertex_alpha.resize(n_verts);
                for (size_t i = 0; i < n_verts; i++)
                {
                    vertex_alpha[i] = rd_col(ab, valpha->t, i);
                }
                has_vertex_alpha = true;
            }
        }

        const uint8_t *fb = faces->buffer.get();
        triangles.reserve(n_faces * (ipf - 2));
        for (size_t f = 0; f < n_faces; f++)
        {
            const uint32_t v0 = rd_idx(fb, faces->t, f * ipf);
            for (size_t v = 1; v + 1 < ipf; v++)
            {
                const uint32_t vi = rd_idx(fb, faces->t, (f * ipf) + v);
                const uint32_t vj = rd_idx(fb, faces->t, (f * ipf) + v + 1);
                if (v0 < n_verts && vi < n_verts && vj < n_verts)
                {
                    Triangle t;
                    t.v[0] = v0;
                    t.v[1] = vi;
                    t.v[2] = vj;
                    t.material_idx = 0;
                    triangles.push_back(t);
                }
            }
        }
    }
    // ── Face-color path: expand to unshared vertices ──────────────────────────
    else
    {
        // Build a vertex pool, then expand per triangle with face color.
        struct PoolVert
        {
            vec3 pos, normal;
            vec2 uv;
        };
        std::vector<PoolVert> pool(n_verts);
        const uint8_t *pb = positions->buffer.get();
        const uint8_t *nb = normals ? normals->buffer.get() : nullptr;
        const uint8_t *ub = uvs ? uvs->buffer.get() : nullptr;
        for (size_t i = 0; i < n_verts; i++)
        {
            pool[i].pos = { rd_f(pb, positions->t, i * 3), rd_f(pb, positions->t, (i * 3) + 1),
                            rd_f(pb, positions->t, (i * 3) + 2) };
            if (nb)
            {
                pool[i].normal = { rd_f(nb, normals->t, i * 3), rd_f(nb, normals->t, (i * 3) + 1),
                                   rd_f(nb, normals->t, (i * 3) + 2) };
            }
            if (ub)
            {
                pool[i].uv = { rd_f(ub, uvs->t, i * 2), rd_f(ub, uvs->t, (i * 2) + 1) };
            }
        }

        const uint8_t *fb = faces->buffer.get();
        const uint8_t *fcb = fcolors->buffer.get();
        const uint8_t *tcb = tc ? tc->buffer.get() : nullptr;
        const tinyply::Type tct = tc ? tc->t : tinyply::Type::FLOAT32;
        triangles.reserve(n_faces * (ipf - 2));
        vertices.reserve(n_faces * (ipf - 2) * 3);
        vertex_colors.reserve(n_faces * (ipf - 2) * 3);
        // When face-list texcoord is bundled with face colors, the path stays
        // fully-unshared (3 verts per triangle) but every emitted vertex still
        // has a source position id (`pi`). Threading a weld map keeps seam-shared
        // positions smoothing across the seam in compute_normals — without it,
        // every corner would be its own group and the surface would render
        // faceted. Plain face-color PLYs (no `tc`) keep their previous faceted
        // behavior — `tcb == nullptr` skips the weld.
        if (tcb)
        {
            ply_weld.reserve(n_faces * (ipf - 2) * 3);
        }

        for (size_t f = 0; f < n_faces; f++)
        {
            const vec3 col = { rd_col(fcb, fcolors->t, f * 3), rd_col(fcb, fcolors->t, (f * 3) + 1),
                               rd_col(fcb, fcolors->t, (f * 3) + 2) };

            const uint32_t i0 = rd_idx(fb, faces->t, f * ipf);
            for (size_t v = 1; v + 1 < ipf; v++)
            {
                const uint32_t iv = rd_idx(fb, faces->t, (f * ipf) + v);
                const uint32_t iw = rd_idx(fb, faces->t, (f * ipf) + v + 1);
                if (i0 >= n_verts || iv >= n_verts || iw >= n_verts)
                {
                    continue;
                }

                const auto base = static_cast<uint32_t>(vertices.size());
                const std::array<uint32_t, 3> pis = { i0, iv, iw };
                const std::array<size_t, 3> corner_in_face = { 0, v, v + 1 };
                for (size_t k = 0; k < 3; k++)
                {
                    const uint32_t pi = pis[k];
                    Vertex vert{};
                    vert.pos = pool[pi].pos;
                    vert.normal = pool[pi].normal;
                    if (tcb)
                    {
                        const size_t off = (f * ipf * 2) + (corner_in_face[k] * 2);
                        vert.uv = { rd_f(tcb, tct, off), rd_f(tcb, tct, off + 1) };
                    }
                    else
                    {
                        vert.uv = pool[pi].uv;
                    }
                    vert.ao = 1.0f;
                    vertices.push_back(vert);
                    vertex_colors.push_back(col);
                    if (tcb)
                    {
                        ply_weld.push_back(pi);
                    }
                }
                Triangle t;
                t.v[0] = base;
                t.v[1] = base + 1;
                t.v[2] = base + 2;
                t.material_idx = 0;
                triangles.push_back(t);
            }
        }
        has_vertex_colors = true;
        if (tcb)
        {
            use_weld = true;
        }
    }

    if (vertices.empty() || triangles.empty())
    {
        return false;
    }

    if (!normals || use_face_colors)
    {
        if (use_weld)
        {
            compute_normals(crease_cos, &ply_weld, n_verts);
        }
        else
        {
            compute_normals(crease_cos);
        }
    }

    snap.commit();
    return true;
}
