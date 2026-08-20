#include "mesh.h"
#include "light.h"
#include "mesh_loader.h"
#include "platform.h"

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
    // is large enough to hold the declared triangles. The vendored binary reader
    // streams per-triangle with no upfront allocation, so this is not an
    // allocation guard: it fail-fasts a crafted or truncated tri_count without a
    // parse attempt (defense-in-depth should the vendor code ever pre-reserve),
    // and the same size test drives the ASCII-vs-binary disambiguation below.
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

    // ASCII detection: header starts with "solid" (ignoring a UTF-8 BOM and leading
    // whitespace, including a blank first line or CRLF). The scan is bounded by
    // n_read, not 80: a 5-79 byte file leaves the tail of the buffer uninitialized
    // (valgrind-caught). Index arithmetic, not a walking pointer: `p + 5 <= end`
    // can form a pointer past one-past-the-end ([expr.add] UB). The skip loop
    // deliberately breaks mid-body instead of testing the character in the loop
    // condition: the condition form makes cppcheck 2.13 assume the loop exits at
    // pos == n_read, a path-analysis false positive that cascades
    // knownConditionTrueFalse through every later is_ascii test and fails CI.
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
    // "solid" is matched ASCII-case-insensitively: a capitalized keyword ("SOLID
    // part") still classifies as ASCII, and the vendored parser loads such a file
    // fine, since the solid line's own tokens are never required (facet/vertex
    // keywords stay case-sensitive in the parser, as in the spec). Case folds via
    // |0x20, valid because all five expected bytes are letters.
    const auto lower = [](char c) { return static_cast<char>(static_cast<unsigned char>(c) | 0x20U); };
    bool is_ascii = n_read - pos >= 5 && lower(header[pos]) == 's' && lower(header[pos + 1]) == 'o' &&
                    lower(header[pos + 2]) == 'l' && lower(header[pos + 3]) == 'i' && lower(header[pos + 4]) == 'd';

    // Read tri_count (bytes 80-83). Explicit fseek resets the stream to a
    // defined position: the prior fread accepts partial reads (5-79 bytes),
    // after which the C standard says the position is indeterminate.
    uint8_t tcb[4];
    const bool have_tri_count = (std::fseek(f.get(), 80, SEEK_SET) == 0 && std::fread(tcb, 1, 4, f.get()) == 4);
    const uint32_t tri_count = have_tri_count
                                   ? (static_cast<uint32_t>(tcb[0]) | (static_cast<uint32_t>(tcb[1]) << 8U) |
                                      (static_cast<uint32_t>(tcb[2]) << 16U) | (static_cast<uint32_t>(tcb[3]) << 24U))
                                   : 0u;

    // 64-bit size: plain ftell's long is 32-bit on Windows/ILP32, which rejected
    // every binary STL of >= 2 GB (~43M triangles, real for scanned meshes).
    const int64_t file_size = platform::file_size(f.get());

    const uint64_t expected_binary = 84ULL + (50ULL * static_cast<uint64_t>(tri_count));

    // Single source for the binary-layout size test: the solid-header
    // disambiguation and the binary reject guard below are complements of this
    // one predicate, so the two cannot drift apart.
    const bool fits_binary = have_tri_count && file_size >= 0 && static_cast<uint64_t>(file_size) >= expected_binary;

    // A binary STL whose header happens to start with "solid" is disambiguated by size:
    // >= expected_binary, not ==, matching the binary guard's own surplus-trailing-bytes
    // policy so such a file with trailing bytes still loads. >= cannot misfire on a real
    // ASCII file: bytes 80-83 of ASCII text are all >= 0x09, so the little-endian tri_count
    // is >= 9 * 2^24 (~151M) and expected_binary >= ~7.5 GB. A misfire would NOT reject
    // cleanly (the binary reader would parse text bytes as floats into garbage geometry);
    // the safety rests entirely on no real ASCII file being that large. Accepted reverse
    // ambiguity: a TRUNCATED solid-headed binary (declared count exceeding the file) is
    // indistinguishable from genuine ASCII here, so it keeps is_ascii and reaches the ASCII
    // parser's rejection: slower than the binary size guard but the same outcome, with memory
    // bounded by the line guard below.
    if (is_ascii && fits_binary)
    {
        is_ascii = false;
    }

    // Reject binary files whose size doesn't satisfy 84 + 50 × tri_count bytes. Accepted
    // TOCTOU covering BOTH pre-parse guards (this size check and the ASCII line bound below):
    // they read this FILE handle while stl_reader reopens the file by path, so a concurrent
    // swap bypasses them. The consequence is a failed parse (the binary reader streams
    // per-triangle with no upfront reserve, so a crafted count cannot force a large
    // allocation) or, for the line bound, one unbounded-line parse at the vendored parser's
    // ~20x transient-allocation cost; the threat model is malformed files, not an adversary
    // racing the local filesystem.
    if (!is_ascii)
    {
        if (!fits_binary)
        {
            return false;
        }
    }
    else
    {
        // The ASCII branch gets the complementary guard, a line-length bound: the vendored
        // parser is line-based and materializes one heap string per token of the current
        // line, so a multi-megabyte line costs a large multiple of its size in transient
        // allocations (measured 1.17 GB peak on a 52 MB single-line file). Bounding the
        // LINE, not the file or the mesh, rejects no real STL: the grammar puts one
        // `solid <name>` or facet/vertex statement per line, so legitimate lines are tens
        // of bytes and 64 KB is orders of magnitude of headroom; any number of lines
        // remains fine, since that is mesh data. A file no larger than the bound cannot
        // contain a longer line, so the extra read pass is skipped for it; larger files pay one full
        // sequential pass before stl_reader's own read, accepted since ASCII files are the
        // small ones in practice and the pass warms the page cache for the parse.
        constexpr size_t MAX_ASCII_LINE_BYTES = size_t{ 64 } * 1024;
        if (file_size < 0 || static_cast<uint64_t>(file_size) > MAX_ASCII_LINE_BYTES)
        {
            if (std::fseek(f.get(), 0, SEEK_SET) != 0)
            {
                return false;
            }
            // fseek clears only the EOF indicator; a stale ERROR indicator from
            // the earlier header/tri_count reads would otherwise be charged to
            // this scan by the ferror check below.
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
            // Fail loud on a read error: treating it as EOF would leave the
            // file's tail unscanned and the bound fail-open.
            if (std::ferror(f.get()) != 0)
            {
                return false;
            }
        }
    }

    // Delegate parsing to stl_reader. STL_READER_NO_EXCEPTIONS converts internal
    // parse errors to return false instead of throwing. Call the format-explicit
    // readers, never ReadStlFile: its own sniffer disagrees with the classification
    // above in both directions (it requires a '\n' within the first 256 bytes, so a
    // long solid name reads as binary; and it substring-matches "solid"/"facet"/
    // "normal" anywhere in those bytes, so a chatty binary header reads as ASCII),
    // and the size guard must bind the path actually parsed.
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

    // Consume stl_reader's deduplicated output directly: coords holds one entry per unique
    // position and tris indexes into it, so shared corners become shared vertex indices.
    // This is what lets compute_normals smooth across sub-crease edges (OBJ/PLY parity);
    // re-expanding to unshared corners, as this loader once did, discarded the weld and
    // forced STL to render permanently faceted, with --smooth-angle a silent no-op.
    const size_t n_verts = coords.size() / 3;
    vertices.reserve(n_verts);
    for (size_t v = 0; v < n_verts; v++)
    {
        Vertex vert{};
        // STL carries no orientation metadata and its ecosystem (CAD/3D printing, mainstream
        // viewers) is Z-up, while the renderer is Y-up: remap (x,y,z) -> (x,z,-y), a -90 deg X
        // rotation (det +1, so winding and culling are unaffected). Done before compute_normals
        // so all derived data (normals, tangents, AO) lands upright.
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
