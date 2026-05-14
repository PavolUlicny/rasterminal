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

bool Mesh::load_stl(const std::string &path)
{
    MeshSnapshot snap(*this);

    // Pre-validate: read 80-byte header + 4-byte tri_count, then verify the file
    // is large enough to hold the declared triangles. Without this, a crafted
    // binary STL with tri_count=0xFFFFFFFF would let stl_reader attempt a ~200 GB
    // allocation even with STL_READER_NO_EXCEPTIONS.
    const auto f = std::unique_ptr<FILE, int (*)(FILE *)>(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!f)
        return false;

    char header[80];
    if (std::fread(header, 1, 80, f.get()) < 5)
        return false;

    // ASCII detection: header starts with "solid" (ignoring leading whitespace).
    const char *h = header;
    while (h < header + 80 && (*h == ' ' || *h == '\t'))
        h++;
    bool is_ascii = (h + 5 <= header + 80 && std::strncmp(h, "solid", 5) == 0);

    // Read tri_count (bytes 80–83).
    uint8_t tcb[4];
    const bool have_tri_count = (std::fread(tcb, 1, 4, f.get()) == 4);
    const uint32_t tri_count = have_tri_count
                                   ? (static_cast<uint32_t>(tcb[0]) |
                                      (static_cast<uint32_t>(tcb[1]) << 8U) |
                                      (static_cast<uint32_t>(tcb[2]) << 16U) |
                                      (static_cast<uint32_t>(tcb[3]) << 24U))
                                   : 0u;

    long file_size = -1;
    if (std::fseek(f.get(), 0, SEEK_END) == 0)
        file_size = std::ftell(f.get());

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

    const size_t n_tris = tris.size() / 3;
    if (n_tris == 0)
        return false;

    materials.push_back(Material{});

    // Expand stl_reader's deduplicated output to 3 unshared vertices per triangle.
    // Dedup produces random index access into the vertex array which defeats the
    // hardware prefetcher; sequential layout [3i, 3i+1, 3i+2] is measurably faster
    // on high-poly models. AO is also skipped for STL in load_model() — isolated
    // vertices produce no adjacency so ao stays at 1 everywhere anyway.
    vertices.reserve(n_tris * 3);
    triangles.reserve(n_tris);

    for (size_t i = 0; i < n_tris; i++)
    {
        const auto base = static_cast<uint32_t>(vertices.size());
        for (int j = 0; j < 3; j++)
        {
            const unsigned int vi = tris[3 * i + static_cast<size_t>(j)];
            Vertex v{};
            v.pos = {coords[3 * static_cast<size_t>(vi)], coords[3 * static_cast<size_t>(vi) + 1], coords[3 * static_cast<size_t>(vi) + 2]};
            v.ao = 1.0f;
            vertices.push_back(v);
        }
        Triangle t;
        t.v[0] = base;
        t.v[1] = base + 1;
        t.v[2] = base + 2;
        t.material_idx = 0;
        triangles.push_back(t);
    }
    compute_normals();

    snap.commit();
    return true;
}
