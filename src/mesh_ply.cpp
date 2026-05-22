#include "mesh.h"
#include "light.h"
#include "mesh_loader.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
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

} // namespace

// ─── Mesh::load_ply ───────────────────────────────────────────────────────────

bool Mesh::load_ply(const std::string &path)
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

    const size_t n_verts = positions->count;
    const size_t n_faces = faces->count;
    if (n_verts == 0 || n_faces == 0)
    {
        return false;
    }

    // Compute indices-per-face from buffer size (tinyply enforces uniform list length).
    const size_t idx_stride = type_stride(faces->t);
    const size_t total_idx = faces->buffer.size_bytes() / idx_stride;
    const size_t ipf = total_idx / n_faces;
    if (ipf < 3)
    {
        return false;
    }

    const bool use_face_colors = (fcolors != nullptr);

    materials.push_back(Material{});

    // ── Standard path: shared vertices ───────────────────────────────────────
    if (!use_face_colors)
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
        triangles.reserve(n_faces * (ipf - 2));
        vertices.reserve(n_faces * (ipf - 2) * 3);
        vertex_colors.reserve(n_faces * (ipf - 2) * 3);

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
                for (const uint32_t pi : { i0, iv, iw })
                {
                    Vertex vert{};
                    vert.pos = pool[pi].pos;
                    vert.normal = pool[pi].normal;
                    vert.uv = pool[pi].uv;
                    vert.ao = 1.0f;
                    vertices.push_back(vert);
                    vertex_colors.push_back(col);
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
    }

    if (vertices.empty() || triangles.empty())
    {
        return false;
    }

    if (!normals || use_face_colors)
    {
        compute_normals();
    }

    snap.commit();
    return true;
}
