#include "mesh.h"
#include "light.h"
#include "mesh_loader.h"

#define STL_READER_NO_EXCEPTIONS
#include "stl_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

bool Mesh::load_stl(const std::string &path, float crease_cos)
{
    MeshSnapshot snap(*this);

    // Pre-validate: read 80-byte header + 4-byte tri_count, then verify the file
    // is large enough to hold the declared triangles. Without this, a crafted
    // binary STL with tri_count=0xFFFFFFFF would let stl_reader attempt a ~200 GB
    // allocation even with STL_READER_NO_EXCEPTIONS.
    const auto f = std::unique_ptr<FILE, int (*)(FILE *)>(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!f)
    {
        return false;
    }

    char header[80];
    if (std::fread(header, 1, 80, f.get()) < 5)
    {
        return false;
    }

    // ASCII detection: header starts with "solid" (ignoring leading whitespace).
    const char *h = header;
    while (h < header + 80 && (*h == ' ' || *h == '\t'))
    {
        h++;
    }
    bool is_ascii = (h + 5 <= header + 80 && std::strncmp(h, "solid", 5) == 0);

    // Read tri_count (bytes 80–83). Explicit fseek resets the stream to a
    // defined position — the prior fread accepts partial reads (5–79 bytes),
    // after which the C standard says the position is indeterminate.
    uint8_t tcb[4];
    const bool have_tri_count = (std::fseek(f.get(), 80, SEEK_SET) == 0 && std::fread(tcb, 1, 4, f.get()) == 4);
    const uint32_t tri_count = have_tri_count
                                   ? (static_cast<uint32_t>(tcb[0]) | (static_cast<uint32_t>(tcb[1]) << 8U) |
                                      (static_cast<uint32_t>(tcb[2]) << 16U) | (static_cast<uint32_t>(tcb[3]) << 24U))
                                   : 0u;

    long file_size = -1;
    if (std::fseek(f.get(), 0, SEEK_END) == 0)
    {
        file_size = std::ftell(f.get());
    }

    const uint64_t expected_binary = 84ULL + (50ULL * static_cast<uint64_t>(tri_count));

    // A binary STL whose header happens to start with "solid" must be
    // disambiguated by exact file size.
    if (is_ascii && have_tri_count && file_size >= 0 && static_cast<uint64_t>(file_size) == expected_binary)
    {
        is_ascii = false;
    }

    // Reject binary files whose size doesn't satisfy 84 + 50 × tri_count bytes.
    if (!is_ascii)
    {
        if (!have_tri_count || file_size < 0 || static_cast<uint64_t>(file_size) < expected_binary)
        {
            return false;
        }
    }

    // Delegate parsing to stl_reader. STL_READER_NO_EXCEPTIONS converts internal
    // parse errors to return false instead of throwing.
    std::vector<float> coords;        // 3 floats per deduplicated vertex
    std::vector<float> face_norms;    // 3 floats per triangle face normal (ignored)
    std::vector<unsigned int> tris;   // 3 vertex indices per triangle
    std::vector<unsigned int> solids; // solid ranges (unused)

    if (!stl_reader::ReadStlFile(path.c_str(), coords, face_norms, tris, solids))
    {
        return false;
    }

    const size_t n_tris = tris.size() / 3;
    if (n_tris == 0)
    {
        return false;
    }

    materials.push_back(Material{});

    // Consume stl_reader's deduplicated output directly: coords holds one entry per unique
    // position and tris indexes into it, so shared corners become shared vertex indices.
    // This is what lets compute_normals smooth across sub-crease edges (OBJ/PLY parity);
    // re-expanding to unshared corners — as this loader once did — discarded the weld and
    // forced STL to render permanently faceted, with --smooth-angle a silent no-op.
    const size_t n_verts = coords.size() / 3;
    vertices.reserve(n_verts);
    for (size_t v = 0; v < n_verts; v++)
    {
        Vertex vert{};
        vert.pos = { coords[3 * v], coords[(3 * v) + 1], coords[(3 * v) + 2] };
        vert.ao = 1.0f;
        vertices.push_back(vert);
    }

    triangles.reserve(n_tris);
    for (size_t i = 0; i < n_tris; i++)
    {
        Triangle t;
        t.v[0] = static_cast<uint32_t>(tris[3 * i]);
        t.v[1] = static_cast<uint32_t>(tris[(3 * i) + 1]);
        t.v[2] = static_cast<uint32_t>(tris[(3 * i) + 2]);
        t.material_idx = 0;
        triangles.push_back(t);
    }
    compute_normals(crease_cos);

    snap.commit();
    return true;
}
