#include "mesh.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

// ─── Mesh::load_stl ───────────────────────────────────────────────────────────
// Supports both ASCII and binary STL.
// STL has no UVs, materials, or vertex sharing. Each facet is independent.
// Face normals from the file are discarded; compute_normals() produces smooth
// per-vertex normals instead.

bool Mesh::load_stl(const std::string &path)
{
    // Save state so we can roll back on any failure path.
    const size_t v0 = vertices.size();
    const size_t t0 = triangles.size();
    const size_t m0 = materials.size();

    auto rollback = [&]
    {
        vertices.resize(v0);
        triangles.resize(t0);
        materials.resize(m0);
    };

    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;

    // STL stores all binary scalars as little-endian.
    auto read_u32_le = [](FILE *fp, uint32_t &out) -> bool
    {
        uint8_t b[4];
        if (std::fread(b, 1, 4, fp) != 4)
            return false;
        out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
              (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
        return true;
    };

    // ── Detect ASCII vs binary ────────────────────────────────────────────────
    // ASCII STL starts with "solid" (possibly with leading whitespace).
    // Binary STL has an 80-byte header that may also start with "solid", so
    // the size check below is used to disambiguate when that happens.

    char header[80];
    if (std::fread(header, 1, 80, f) < 5)
    {
        std::fclose(f);
        return false;
    }

    // Trim leading whitespace when checking for "solid".
    // Bound the scan to the 80-byte header — it is not null-terminated.
    const char *h = header;
    const char *h_end = header + 80;
    while (h < h_end && (*h == ' ' || *h == '\t'))
        h++;
    bool is_ascii = (h + 5 <= h_end && std::strncmp(h, "solid", 5) == 0);

    // Binary STL: after the 80-byte header comes a uint32 triangle count.
    // If that count × 50 + 84 equals the file size, it's almost certainly binary.
    // This disambiguates files that start with "solid" in their binary header.
    if (is_ascii)
    {
        uint32_t tri_count = 0;
        if (read_u32_le(f, tri_count))
        {
            long pos = std::ftell(f);
            std::fseek(f, 0, SEEK_END);
            long file_size = std::ftell(f);
            std::fseek(f, pos, SEEK_SET);

            if (file_size == static_cast<long>(84 + static_cast<uint64_t>(tri_count) * 50))
                is_ascii = false;
            else
                std::fseek(f, 80, SEEK_SET); // rewind past header for ASCII path
        }
    }

    materials.push_back(Material{});

    if (is_ascii)
    {
        // ── ASCII path ────────────────────────────────────────────────────────
        // Rewind to start; parse "facet normal / outer loop / vertex × 3".
        std::fseek(f, 0, SEEK_SET);

        char line[256];
        vec3 verts[3];
        int vert_count = 0;
        bool malformed = false;

        while (std::fgets(line, sizeof(line), f))
        {
            const char *p = line;
            while (*p == ' ' || *p == '\t')
                p++;

            if (std::strncmp(p, "vertex", 6) == 0)
            {
                vec3 v{};
                if (std::sscanf(p + 6, "%f %f %f", &v.x, &v.y, &v.z) != 3)
                {
                    malformed = true;
                    break;
                }
                if (vert_count < 3)
                    verts[vert_count] = v;
                vert_count++;
            }
            else if (std::strncmp(p, "endfacet", 8) == 0)
            {
                if (vert_count >= 3)
                {
                    uint32_t base = static_cast<uint32_t>(vertices.size());
                    vertices.push_back({verts[0], {}, {}, {}});
                    vertices.push_back({verts[1], {}, {}, {}});
                    vertices.push_back({verts[2], {}, {}, {}});
                    Triangle t;
                    t.v[0] = base;
                    t.v[1] = base + 1;
                    t.v[2] = base + 2;
                    t.material_idx = 0;
                    triangles.push_back(t);
                }
                vert_count = 0;
            }
        }

        if (malformed)
        {
            std::fclose(f);
            rollback();
            return false;
        }
    }
    else
    {
        // ── Binary path ───────────────────────────────────────────────────────
        // File position is right after the 80-byte header.
        // Next 4 bytes: triangle count.
        uint32_t tri_count = 0;
        if (!read_u32_le(f, tri_count))
        {
            std::fclose(f);
            rollback();
            return false;
        }

        vertices.reserve(static_cast<size_t>(tri_count) * 3);
        triangles.reserve(static_cast<size_t>(tri_count));

        // STL is always little-endian. Decode explicitly so the loader is
        // correct on big-endian hosts instead of reading host-native bytes.
        auto le_f32 = [](const uint8_t *b) -> float
        {
            uint32_t u = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
            float v;
            std::memcpy(&v, &u, 4);
            return v;
        };

        bool truncated = false;
        for (uint32_t i = 0; i < tri_count; i++)
        {
            // 12 bytes: face normal (ignored — we recompute per-vertex normals)
            uint8_t ignored[12];
            if (std::fread(ignored, 1, 12, f) != 12)
            {
                truncated = true;
                break;
            }

            // 36 bytes: 3 × 3 floats (vertices), little-endian
            uint8_t raw[36];
            if (std::fread(raw, 1, 36, f) != 36)
            {
                truncated = true;
                break;
            }

            // 2-byte attribute (ignored)
            uint8_t attr[2];
            if (std::fread(attr, 1, 2, f) != 2)
            {
                truncated = true;
                break;
            }

            uint32_t base = static_cast<uint32_t>(vertices.size());
            vertices.push_back({{le_f32(raw + 0), le_f32(raw + 4), le_f32(raw + 8)}, {}, {}, {}});
            vertices.push_back({{le_f32(raw + 12), le_f32(raw + 16), le_f32(raw + 20)}, {}, {}, {}});
            vertices.push_back({{le_f32(raw + 24), le_f32(raw + 28), le_f32(raw + 32)}, {}, {}, {}});

            Triangle t;
            t.v[0] = base;
            t.v[1] = base + 1;
            t.v[2] = base + 2;
            t.material_idx = 0;
            triangles.push_back(t);
        }

        if (truncated)
        {
            std::fclose(f);
            rollback();
            return false;
        }
    }

    std::fclose(f);

    if (vertices.empty() || triangles.empty())
    {
        rollback();
        return false;
    }

    // STL has no vertex sharing so normals must always be computed.
    compute_normals();

    return true;
}
