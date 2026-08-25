#include "src/loaders/mesh.h"
#include "src/loaders/mesh_loader.h"
#include "src/math/light.h"
#include "src/platform/platform.h"

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

    // Read the binary header up front for truncation checks and ASCII disambiguation.
    const auto f = std::unique_ptr<FILE, int (*)(FILE *)>(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!f)
    {
        return false;
    }

    char header[80];
    const size_t n_read = std::fread(header, 1, 80, f.get());
    if (n_read < 5)
    {
        return false;
    }

    // Detect `solid` after a BOM and leading whitespace. Bound the scan by bytes read;
    // short files leave the rest of the header buffer uninitialized.
    size_t pos = 0;
    if (std::memcmp(header, "\xEF\xBB\xBF", 3) == 0) // in bounds: n_read >= 5
    {
        pos = 3;
    }
    while (pos < n_read)
    {
        const char c = header[pos];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        {
            break;
        }
        pos++;
    }
    // The vendored parser accepts case-insensitive `solid`; other keywords remain spec-case.
    const auto lower = [](char c) { return static_cast<char>(static_cast<unsigned char>(c) | 0x20U); };
    bool is_ascii = n_read - pos >= 5 && lower(header[pos]) == 's' && lower(header[pos + 1]) == 'o' &&
                    lower(header[pos + 2]) == 'l' && lower(header[pos + 3]) == 'i' && lower(header[pos + 4]) == 'd';

    // Reset after a partial fread because its resulting position may be indeterminate.
    uint8_t tcb[4];
    const bool have_tri_count = (std::fseek(f.get(), 80, SEEK_SET) == 0 && std::fread(tcb, 1, 4, f.get()) == 4);
    const uint32_t tri_count = have_tri_count
                                   ? (static_cast<uint32_t>(tcb[0]) | (static_cast<uint32_t>(tcb[1]) << 8U) |
                                      (static_cast<uint32_t>(tcb[2]) << 16U) | (static_cast<uint32_t>(tcb[3]) << 24U))
                                   : 0u;

    // Use the platform's 64-bit file size for multi-gigabyte scans.
    const int64_t file_size = platform::file_size(f.get());

    const uint64_t expected_binary = 84ULL + (50ULL * static_cast<uint64_t>(tri_count));

    // Share one binary-size predicate between classification and validation.
    const bool fits_binary = have_tri_count && file_size >= 0 && static_cast<uint64_t>(file_size) >= expected_binary;

    // A solid-prefixed binary file is identified by satisfying its declared binary size.
    // Allow trailing bytes. Truncated ambiguous files fall through to bounded ASCII rejection.
    if (is_ascii && fits_binary)
    {
        is_ascii = false;
    }

    // stl_reader reopens the path after these checks, so a racing file replacement can bypass
    // them. The threat model covers malformed input, not a hostile local filesystem race.
    if (!is_ascii)
    {
        if (!fits_binary)
        {
            return false;
        }
    }
    else
    {
        // Bound ASCII line length because the vendored token parser amplifies one huge line
        // into many temporary allocations. STL grammar needs nowhere near 64 KB per line.
        constexpr size_t MAX_ASCII_LINE_BYTES = size_t{ 64 } * 1024;
        if (file_size < 0 || static_cast<uint64_t>(file_size) > MAX_ASCII_LINE_BYTES)
        {
            if (std::fseek(f.get(), 0, SEEK_SET) != 0)
            {
                return false;
            }
            // Clear any stale stream error before attributing failures to this scan.
            std::clearerr(f.get());
            char buf[4096];
            size_t line_len = 0;
            for (;;)
            {
                const size_t got = std::fread(buf, 1, sizeof(buf), f.get());
                const char *p = buf;
                const char *const buf_end = buf + got;
                while (p < buf_end)
                {
                    const char *nl = static_cast<const char *>(std::memchr(p, '\n', static_cast<size_t>(buf_end - p)));
                    line_len += static_cast<size_t>((nl ? nl : buf_end) - p);
                    if (line_len > MAX_ASCII_LINE_BYTES)
                    {
                        return false;
                    }
                    if (!nl)
                    {
                        break;
                    }
                    line_len = 0;
                    p = nl + 1;
                }
                if (got < sizeof(buf))
                {
                    break; // short read: EOF, or a read error caught just below
                }
            }
            // A read error must not turn the line bound into a partial scan.
            if (std::ferror(f.get()) != 0)
            {
                return false;
            }
        }
    }

    // Use explicit vendor readers because its sniffer disagrees with the guarded classification.
    std::vector<float> coords;        // 3 floats per deduplicated vertex
    std::vector<float> face_norms;    // 3 floats per triangle face normal (ignored)
    std::vector<unsigned int> tris;   // 3 vertex indices per triangle
    std::vector<unsigned int> solids; // solid ranges (unused)

    const bool parsed = is_ascii ? stl_reader::ReadStlFile_ASCII(path.c_str(), coords, face_norms, tris, solids)
                                 : stl_reader::ReadStlFile_BINARY(path.c_str(), coords, face_norms, tris, solids);
    if (!parsed)
    {
        return false;
    }

    const size_t n_tris = tris.size() / 3;
    if (n_tris == 0)
    {
        return false;
    }

    materials.push_back(Material{});

    // Preserve stl_reader's shared vertices so crease-angle smoothing can cross edges.
    const size_t n_verts = coords.size() / 3;
    vertices.reserve(n_verts);
    for (size_t v = 0; v < n_verts; v++)
    {
        Vertex vert{};
        // Treat STL as Z-up and rotate to Y-up before deriving normals, tangents or AO.
        vert.pos = { coords[3 * v], coords[(3 * v) + 2], -coords[(3 * v) + 1] };
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
