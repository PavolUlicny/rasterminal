#include "mesh.h"
#include "mesh_loader.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

// ─── internal helpers ─────────────────────────────────────────────────────────

// Read a 4-byte little-endian uint32 from f. Returns false on short read.
static bool stl_read_u32_le(FILE *f, uint32_t &out)
{
    uint8_t b[4];
    if (std::fread(b, 1, 4, f) != 4)
        return false;
    out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
          (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

// Decode a 4-byte little-endian float from a byte pointer.
static float stl_le_f32(const uint8_t *b)
{
    uint32_t u = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                 (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    float v;
    std::memcpy(&v, &u, 4);
    return v;
}

// ─── ASCII loader ─────────────────────────────────────────────────────────────
// Seeks to byte 0 and parses "facet normal / outer loop / vertex×3 / endfacet".
// Face normals from the file are discarded; compute_normals() produces smooth
// per-vertex normals instead. Returns false on malformed input.

static bool load_stl_ascii(FILE *f, Mesh &mesh)
{
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
            if (std::sscanf(p + 6, "%f %f %f", &v.x, &v.y, &v.z) != 3)
                return false;
            if (vert_count < 3)
                verts[vert_count] = v;
            vert_count++;
        }
        else if (std::strncmp(p, "endfacet", 8) == 0)
        {
            if (vert_count >= 3)
            {
                uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back({verts[0], {}, {}, {}});
                mesh.vertices.push_back({verts[1], {}, {}, {}});
                mesh.vertices.push_back({verts[2], {}, {}, {}});
                Triangle t;
                t.v[0] = base;
                t.v[1] = base + 1;
                t.v[2] = base + 2;
                t.material_idx = 0;
                mesh.triangles.push_back(t);
            }
            vert_count = 0;
        }
    }

    return true;
}

// ─── binary loader ────────────────────────────────────────────────────────────
// File must be positioned at byte 84 (immediately after the tri_count field).
// tri_count is passed by the caller who already read it during format detection.
// Returns false on truncated input.

static bool load_stl_binary(FILE *f, uint32_t tri_count, Mesh &mesh)
{
    mesh.vertices.reserve(static_cast<size_t>(tri_count) * 3);
    mesh.triangles.reserve(static_cast<size_t>(tri_count));

    for (uint32_t i = 0; i < tri_count; i++)
    {
        // 12 bytes: face normal (ignored — normals are recomputed)
        uint8_t ignored[12];
        if (std::fread(ignored, 1, 12, f) != 12)
            return false;

        // 36 bytes: 3 × (x, y, z) as little-endian floats
        uint8_t raw[36];
        if (std::fread(raw, 1, 36, f) != 36)
            return false;

        // 2-byte attribute (ignored)
        uint8_t attr[2];
        if (std::fread(attr, 1, 2, f) != 2)
            return false;

        uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({{stl_le_f32(raw + 0), stl_le_f32(raw + 4), stl_le_f32(raw + 8)}, {}, {}, {}});
        mesh.vertices.push_back({{stl_le_f32(raw + 12), stl_le_f32(raw + 16), stl_le_f32(raw + 20)}, {}, {}, {}});
        mesh.vertices.push_back({{stl_le_f32(raw + 24), stl_le_f32(raw + 28), stl_le_f32(raw + 32)}, {}, {}, {}});

        Triangle t;
        t.v[0] = base;
        t.v[1] = base + 1;
        t.v[2] = base + 2;
        t.material_idx = 0;
        mesh.triangles.push_back(t);
    }

    return true;
}

// ─── Mesh::load_stl ───────────────────────────────────────────────────────────
// Supports both ASCII and binary STL.
// STL has no UVs, materials, or vertex sharing. Each facet is independent.

bool Mesh::load_stl(const std::string &path)
{
    MeshSnapshot snap(*this);

    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;

    // Read 80-byte header.
    char header[80];
    if (std::fread(header, 1, 80, f) < 5)
    {
        std::fclose(f);
        return false;
    }

    // ASCII STL starts with "solid" (possibly with leading whitespace).
    // Binary STL has an 80-byte header that may also start with "solid", so
    // the size check below is used to disambiguate when that happens.
    const char *h = header;
    const char *h_end = header + 80;
    while (h < h_end && (*h == ' ' || *h == '\t'))
        h++;
    bool is_ascii = (h + 5 <= h_end && std::strncmp(h, "solid", 5) == 0);

    // Always read tri_count (bytes 80-83) here: it is needed for size-based
    // detection and passed directly to load_stl_binary, avoiding a second read
    // that would land at the wrong file position.
    uint32_t tri_count = 0;
    const bool have_tri_count = stl_read_u32_le(f, tri_count); // file position → 84

    // Capture file size once — used for both ASCII/binary disambiguation and
    // for validating tri_count against the actual file before reserve().
    long file_size = -1;
    if (have_tri_count)
    {
        long pos = std::ftell(f);
        if (pos >= 0 && std::fseek(f, 0, SEEK_END) == 0)
            file_size = std::ftell(f);
        if (pos < 0 || std::fseek(f, pos, SEEK_SET) != 0)
        {
            std::fclose(f);
            return false;
        }
    }

    // Expected size of a binary STL with this tri_count: 84-byte header plus
    // exactly 50 bytes per triangle. Computed in uint64_t to avoid overflow.
    const uint64_t expected_binary_size = 84ULL + 50ULL * static_cast<uint64_t>(tri_count);

    // If the header started with "solid", verify via exact file size match —
    // this disambiguates the rare case of a binary STL whose 80-byte header
    // happens to begin with "solid".
    if (is_ascii && have_tri_count && file_size >= 0 &&
        static_cast<uint64_t>(file_size) == expected_binary_size)
        is_ascii = false;

    materials.push_back(Material{});

    bool ok = false;
    if (is_ascii)
    {
        ok = load_stl_ascii(f, *this); // seeks to byte 0 itself
    }
    else if (have_tri_count)
    {
        // Defence against malicious/corrupt tri_count: a valid binary STL
        // must be at least 84 + 50 × tri_count bytes long. Without this
        // check, a crafted tri_count (e.g. 0xFFFFFFFF) would reach the
        // reserve() inside load_stl_binary and trigger std::bad_alloc.
        if (file_size < 0 || static_cast<uint64_t>(file_size) < expected_binary_size)
        {
            std::fclose(f);
            return false;
        }
        ok = load_stl_binary(f, tri_count, *this); // file already at byte 84
    }

    std::fclose(f);

    if (!ok || vertices.empty() || triangles.empty())
        return false;

    // STL has no vertex sharing so normals must always be computed.
    compute_normals();

    snap.commit();
    return true;
}
