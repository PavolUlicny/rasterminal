#include "mesh.h"
#include "mesh_loader.h"

#define STL_READER_NO_EXCEPTIONS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#include "stl_reader.h"
#pragma GCC diagnostic pop

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

bool Mesh::load_stl(const std::string &path)
{
    MeshSnapshot snap(*this);

    // Pre-validate: read 80-byte header + 4-byte tri_count, then verify the file
    // is large enough to hold the declared triangles. Without this, a crafted
    // binary STL with tri_count=0xFFFFFFFF would let stl_reader attempt a ~200 GB
    // allocation even with STL_READER_NO_EXCEPTIONS.
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;

    char header[80];
    if (std::fread(header, 1, 80, f) < 5)
    {
        std::fclose(f);
        return false;
    }

    // ASCII detection: header starts with "solid" (ignoring leading whitespace).
    const char *h = header;
    while (h < header + 80 && (*h == ' ' || *h == '\t'))
        h++;
    bool is_ascii = (h + 5 <= header + 80 && std::strncmp(h, "solid", 5) == 0);

    // Read tri_count (bytes 80–83).
    uint8_t tcb[4];
    const bool have_tri_count = (std::fread(tcb, 1, 4, f) == 4);
    const uint32_t tri_count = have_tri_count
                                   ? (static_cast<uint32_t>(tcb[0]) |
                                      (static_cast<uint32_t>(tcb[1]) << 8) |
                                      (static_cast<uint32_t>(tcb[2]) << 16) |
                                      (static_cast<uint32_t>(tcb[3]) << 24))
                                   : 0u;

    long file_size = -1;
    if (std::fseek(f, 0, SEEK_END) == 0)
        file_size = std::ftell(f);
    std::fclose(f);

    const uint64_t expected_binary = 84ULL + 50ULL * static_cast<uint64_t>(tri_count);

    // A binary STL whose header happens to start with "solid" must be
    // disambiguated by exact file size.
    if (is_ascii && have_tri_count && file_size >= 0 &&
        static_cast<uint64_t>(file_size) == expected_binary)
        is_ascii = false;

    // Reject binary files whose size doesn't satisfy 84 + 50 × tri_count bytes.
    if (!is_ascii)
    {
        if (!have_tri_count || file_size < 0 ||
            static_cast<uint64_t>(file_size) < expected_binary)
            return false;
    }

    // Delegate parsing to stl_reader. STL_READER_NO_EXCEPTIONS converts internal
    // parse errors to return false instead of throwing.
    std::vector<float> coords;        // 3 floats per deduplicated vertex
    std::vector<float> face_norms;    // 3 floats per triangle face normal (ignored)
    std::vector<unsigned int> tris;   // 3 vertex indices per triangle
    std::vector<unsigned int> solids; // solid ranges (unused)

    if (!stl_reader::ReadStlFile(path.c_str(), coords, face_norms, tris, solids))
        return false;

    const size_t n_verts = coords.size() / 3;
    const size_t n_tris = tris.size() / 3;
    if (n_verts == 0 || n_tris == 0)
        return false;

    materials.push_back(Material{});

    vertices.reserve(n_verts);
    for (size_t i = 0; i < n_verts; i++)
    {
        Vertex v{};
        v.pos = {coords[3 * i], coords[3 * i + 1], coords[3 * i + 2]};
        v.ao = 1.0f;
        vertices.push_back(v);
    }

    triangles.reserve(n_tris);
    for (size_t i = 0; i < n_tris; i++)
    {
        Triangle t;
        t.v[0] = tris[3 * i];
        t.v[1] = tris[3 * i + 1];
        t.v[2] = tris[3 * i + 2];
        t.material_idx = 0;
        triangles.push_back(t);
    }

    // stl_reader deduplicates vertices by position, so compute_normals()
    // averages adjacent face normals per shared vertex → smooth shading.
    // compute_ao() is skipped for STL in load_model(): scan meshes have
    // irregular topology that produces noisy AO values.
    compute_normals();

    snap.commit();
    return true;
}
