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
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;

    // ── Detect ASCII vs binary ────────────────────────────────────────────────
    // Binary STL starts with an 80-byte header that is never "solid".
    // ASCII STL starts with "solid" (possibly with leading whitespace).
    // Read the first 80 bytes and check.

    char header[80];
    if (std::fread(header, 1, 80, f) < 5)
    {
        std::fclose(f);
        return false;
    }

    // Trim leading whitespace when checking for "solid".
    const char *h = header;
    while (*h == ' ' || *h == '\t')
        h++;
    bool is_ascii = (std::strncmp(h, "solid", 5) == 0);

    // Binary STL: after the 80-byte header comes a uint32 triangle count.
    // If that count × 50 + 84 equals the file size, it's almost certainly binary.
    // This disambiguates files that start with "solid" in their binary header.
    if (is_ascii)
    {
        uint32_t tri_count = 0;
        if (std::fread(&tri_count, 4, 1, f) == 1)
        {
            long pos = std::ftell(f);
            std::fseek(f, 0, SEEK_END);
            long file_size = std::ftell(f);
            std::fseek(f, pos, SEEK_SET);

            if (file_size == (long)(84 + (uint64_t)tri_count * 50))
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

        while (std::fgets(line, sizeof(line), f))
        {
            const char *p = line;
            while (*p == ' ' || *p == '\t')
                p++;

            if (std::strncmp(p, "vertex", 6) == 0)
            {
                vec3 v{};
                std::sscanf(p + 6, "%f %f %f", &v.x, &v.y, &v.z);
                if (vert_count < 3)
                    verts[vert_count] = v;
                vert_count++;
            }
            else if (std::strncmp(p, "endfacet", 8) == 0)
            {
                if (vert_count >= 3)
                {
                    uint32_t base = (uint32_t)vertices.size();
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
    }
    else
    {
        // ── Binary path ───────────────────────────────────────────────────────
        // File position is right after the 80-byte header.
        // Next 4 bytes: triangle count.
        uint32_t tri_count = 0;
        if (std::fread(&tri_count, 4, 1, f) != 1)
        {
            std::fclose(f);
            return false;
        }

        vertices.reserve(tri_count * 3);
        triangles.reserve(tri_count);

        for (uint32_t i = 0; i < tri_count; i++)
        {
            // 3 floats: face normal (ignored — we recompute per-vertex normals)
            float ignored[3];
            if (std::fread(ignored, 4, 3, f) != 3)
                break;

            // 3 × 3 floats: vertices
            float raw[9];
            if (std::fread(raw, 4, 9, f) != 9)
                break;

            // 2-byte attribute (ignored)
            uint16_t attr;
            if (std::fread(&attr, 2, 1, f) != 1)
                break;

            uint32_t base = (uint32_t)vertices.size();
            vertices.push_back({{raw[0], raw[1], raw[2]}, {}, {}, {}});
            vertices.push_back({{raw[3], raw[4], raw[5]}, {}, {}, {}});
            vertices.push_back({{raw[6], raw[7], raw[8]}, {}, {}, {}});

            Triangle t;
            t.v[0] = base;
            t.v[1] = base + 1;
            t.v[2] = base + 2;
            t.material_idx = 0;
            triangles.push_back(t);
        }
    }

    std::fclose(f);

    if (vertices.empty() || triangles.empty())
        return false;

    // STL has no vertex sharing so normals must always be computed.
    compute_normals();
    compute_tangents();
    compute_ao();

    return true;
}
