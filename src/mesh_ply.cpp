#include "mesh.h"
#include "mesh_loader.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ─── types ────────────────────────────────────────────────────────────────────

enum class PlyFmt
{
    ASCII,
    LE,
    BE
};

enum class PlyPType
{
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    F32,
    F64,
    UNKNOWN
};

struct PlyProp
{
    enum Sem
    {
        X,
        Y,
        Z,
        NX,
        NY,
        NZ,
        S,
        T,
        R,
        G,
        B, // vertex color (alpha recognized as SKIP — no transparency support)
        SKIP
    } sem = SKIP;
    PlyPType type = PlyPType::F32;
    bool is_list = false;
    PlyPType list_count_t = PlyPType::U8;
    PlyPType list_elem_t = PlyPType::I32;
};

struct PlyElem
{
    std::string name;
    int count = 0;
    std::vector<PlyProp> props;
};

struct PlyHeader
{
    PlyFmt fmt = PlyFmt::ASCII;
    std::vector<PlyElem> elements;
    int vert_idx = -1; // index into elements (-1 = not found)
    int face_idx = -1;
    bool has_normals = false;
    bool has_colors = false; // true when vertex element declares red/green/blue props
};

// ─── pure helpers ─────────────────────────────────────────────────────────────

static int ply_ptype_size(PlyPType t)
{
    switch (t)
    {
    case PlyPType::I8:
    case PlyPType::U8:
        return 1;
    case PlyPType::I16:
    case PlyPType::U16:
        return 2;
    case PlyPType::I32:
    case PlyPType::U32:
    case PlyPType::F32:
        return 4;
    case PlyPType::F64:
        return 8;
    case PlyPType::UNKNOWN:
        return 0;
    }
    return 0;
}

static PlyPType ply_parse_ptype(const char *s)
{
    if (!std::strcmp(s, "char") || !std::strcmp(s, "int8"))
        return PlyPType::I8;
    if (!std::strcmp(s, "uchar") || !std::strcmp(s, "uint8"))
        return PlyPType::U8;
    if (!std::strcmp(s, "short") || !std::strcmp(s, "int16"))
        return PlyPType::I16;
    if (!std::strcmp(s, "ushort") || !std::strcmp(s, "uint16"))
        return PlyPType::U16;
    if (!std::strcmp(s, "int") || !std::strcmp(s, "int32"))
        return PlyPType::I32;
    if (!std::strcmp(s, "uint") || !std::strcmp(s, "uint32"))
        return PlyPType::U32;
    if (!std::strcmp(s, "float") || !std::strcmp(s, "float32"))
        return PlyPType::F32;
    if (!std::strcmp(s, "double") || !std::strcmp(s, "float64"))
        return PlyPType::F64;
    return PlyPType::UNKNOWN;
}

static uint16_t ply_bs16(uint16_t v)
{
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

static uint32_t ply_bs32(uint32_t v)
{
    return ((v >> 24) & 0xFFu) | ((v >> 8) & 0xFF00u) |
           ((v << 8) & 0xFF0000u) | ((v << 24) & 0xFF000000u);
}

static uint64_t ply_bs64(uint64_t v)
{
    uint32_t lo = ply_bs32(static_cast<uint32_t>(v & 0xFFFFFFFFu));
    uint32_t hi = ply_bs32(static_cast<uint32_t>(v >> 32));
    return (static_cast<uint64_t>(lo) << 32) | static_cast<uint64_t>(hi);
}

static void ply_apply_prop(Vertex &v, PlyProp::Sem sem, float val, PlyPType type)
{
    switch (sem)
    {
    case PlyProp::X:
        v.pos.x = val;
        break;
    case PlyProp::Y:
        v.pos.y = val;
        break;
    case PlyProp::Z:
        v.pos.z = val;
        break;
    case PlyProp::NX:
        v.normal.x = val;
        break;
    case PlyProp::NY:
        v.normal.y = val;
        break;
    case PlyProp::NZ:
        v.normal.z = val;
        break;
    case PlyProp::S:
        v.uv.x = val;
        break;
    case PlyProp::T:
        v.uv.y = val;
        break;
    case PlyProp::R:
        v.color.x = (type == PlyPType::F32 || type == PlyPType::F64) ? val : val / 255.0f;
        break;
    case PlyProp::G:
        v.color.y = (type == PlyPType::F32 || type == PlyPType::F64) ? val : val / 255.0f;
        break;
    case PlyProp::B:
        v.color.z = (type == PlyPType::F32 || type == PlyPType::F64) ? val : val / 255.0f;
        break;
    default:
        break;
    }
}

static void ply_push_face(Mesh &mesh, const uint32_t *fv, uint32_t count)
{
    uint32_t n_verts = static_cast<uint32_t>(mesh.vertices.size());
    for (uint32_t j = 1; j + 1 < count; j++)
    {
        if (fv[0] >= n_verts || fv[j] >= n_verts || fv[j + 1] >= n_verts)
            continue;
        Triangle t;
        t.v[0] = fv[0];
        t.v[1] = fv[j];
        t.v[2] = fv[j + 1];
        t.material_idx = 0;
        mesh.triangles.push_back(t);
    }
}

// ─── ply_parse_header ─────────────────────────────────────────────────────────
// Reads from byte 0 (file must be freshly opened).
// Does NOT close f — caller owns the file handle.
// On success, f is positioned immediately after the end_header line.

static bool ply_parse_header(FILE *f, PlyHeader &hdr)
{
    char line[1024];

    if (!std::fgets(line, sizeof(line), f) || std::strncmp(line, "ply", 3) != 0)
        return false;

    hdr = PlyHeader{};
    int cur = -1;
    bool found_end_header = false;

    while (std::fgets(line, sizeof(line), f))
    {
        int len = static_cast<int>(std::strlen(line));
        while (len > 0 && line[len - 1] <= ' ')
            line[--len] = '\0';

        if (std::strcmp(line, "end_header") == 0)
        {
            found_end_header = true;
            break;
        }

        if (std::strncmp(line, "format ", 7) == 0)
        {
            if (std::strstr(line, "binary_little_endian"))
                hdr.fmt = PlyFmt::LE;
            else if (std::strstr(line, "binary_big_endian"))
                hdr.fmt = PlyFmt::BE;
        }
        else if (std::strncmp(line, "element ", 8) == 0)
        {
            char name[64] = {};
            int count = 0;
            std::sscanf(line + 8, "%63s %d", name, &count);
            hdr.elements.push_back({name, count, {}});
            cur = static_cast<int>(hdr.elements.size()) - 1;
        }
        else if (std::strncmp(line, "property ", 9) == 0 && cur >= 0)
        {
            const char *p = line + 9;
            PlyProp prop;

            if (std::strncmp(p, "list ", 5) == 0)
            {
                char ct[32] = {}, et[32] = {}, name[64] = {};
                std::sscanf(p + 5, "%31s %31s %63s", ct, et, name);
                prop.is_list = true;
                prop.list_count_t = ply_parse_ptype(ct);
                prop.list_elem_t = ply_parse_ptype(et);
                prop.type = prop.list_elem_t;
                // sem stays SKIP; handled as face indices in the read loop
            }
            else
            {
                char tstr[32] = {}, name[64] = {};
                std::sscanf(p, "%31s %63s", tstr, name);
                prop.type = ply_parse_ptype(tstr);
                prop.is_list = false;

                if (!std::strcmp(name, "x"))
                    prop.sem = PlyProp::X;
                else if (!std::strcmp(name, "y"))
                    prop.sem = PlyProp::Y;
                else if (!std::strcmp(name, "z"))
                    prop.sem = PlyProp::Z;
                else if (!std::strcmp(name, "nx"))
                    prop.sem = PlyProp::NX;
                else if (!std::strcmp(name, "ny"))
                    prop.sem = PlyProp::NY;
                else if (!std::strcmp(name, "nz"))
                    prop.sem = PlyProp::NZ;
                else if (!std::strcmp(name, "s") || !std::strcmp(name, "u") ||
                         !std::strcmp(name, "texture_u") || !std::strcmp(name, "texture_s"))
                    prop.sem = PlyProp::S;
                else if (!std::strcmp(name, "t") || !std::strcmp(name, "v") ||
                         !std::strcmp(name, "texture_v") || !std::strcmp(name, "texture_t"))
                    prop.sem = PlyProp::T;
                else if (!std::strcmp(name, "red") || !std::strcmp(name, "r"))
                    prop.sem = PlyProp::R;
                else if (!std::strcmp(name, "green") || !std::strcmp(name, "g"))
                    prop.sem = PlyProp::G;
                else if (!std::strcmp(name, "blue") || !std::strcmp(name, "b"))
                    prop.sem = PlyProp::B;
            }

            // Reject unknown types immediately — size 0 would desync the binary reader.
            bool unknown = prop.is_list
                               ? (prop.list_count_t == PlyPType::UNKNOWN || prop.list_elem_t == PlyPType::UNKNOWN)
                               : (prop.type == PlyPType::UNKNOWN);
            if (unknown)
                return false;

            hdr.elements[static_cast<size_t>(cur)].props.push_back(prop);
        }
        // Comments and other directives are ignored.
    }

    if (!found_end_header)
        return false;

    for (int i = 0; i < static_cast<int>(hdr.elements.size()); i++)
    {
        const std::string &name = hdr.elements[static_cast<size_t>(i)].name;
        if (name == "vertex")
            hdr.vert_idx = i;
        else if (name == "face")
            hdr.face_idx = i;
    }

    if (hdr.vert_idx < 0 || hdr.face_idx < 0 ||
        hdr.elements[static_cast<size_t>(hdr.vert_idx)].count <= 0)
        return false;

    for (const auto &prop : hdr.elements[static_cast<size_t>(hdr.vert_idx)].props)
    {
        if (prop.sem == PlyProp::NX)
            hdr.has_normals = true;
        if (prop.sem == PlyProp::R || prop.sem == PlyProp::G || prop.sem == PlyProp::B)
            hdr.has_colors = true;
    }

    return true;
}

// ─── load_ply_ascii ───────────────────────────────────────────────────────────
// File must be positioned immediately after end_header.
// Returns false on truncated input.

static bool load_ply_ascii(FILE *f, const PlyHeader &hdr, Mesh &mesh)
{
    bool truncated = false;

    auto sf = [&](float &v)
    { if (std::fscanf(f, " %f", &v) != 1) truncated = true; };
    auto si = [&](int &v)
    { if (std::fscanf(f, " %d", &v) != 1) truncated = true; };
    auto su = [&](unsigned int &v)
    { if (std::fscanf(f, " %u", &v) != 1) truncated = true; };

    const PlyElem &vert_elem = hdr.elements[static_cast<size_t>(hdr.vert_idx)];
    const PlyElem &face_elem = hdr.elements[static_cast<size_t>(hdr.face_idx)];

    for (const auto &elem : hdr.elements)
    {
        for (int i = 0; i < elem.count; i++)
        {
            if (&elem == &vert_elem)
            {
                Vertex v{};
                for (const auto &prop : elem.props)
                {
                    if (prop.is_list)
                    {
                        int cnt = 0;
                        si(cnt);
                        for (int j = 0; j < cnt; j++)
                        {
                            float tmp;
                            sf(tmp);
                        }
                        continue;
                    }
                    float val = 0.0f;
                    sf(val);
                    ply_apply_prop(v, prop.sem, val, prop.type);
                }
                mesh.vertices.push_back(v);
            }
            else if (&elem == &face_elem)
            {
                bool got_indices = false;
                uint32_t fv[64];
                uint32_t actual = 0;
                for (const auto &prop : elem.props)
                {
                    if (!prop.is_list)
                    {
                        float tmp;
                        sf(tmp);
                        continue;
                    }

                    unsigned int cnt = 0;
                    su(cnt);
                    if (!got_indices)
                    {
                        actual = (cnt < 64u) ? cnt : 64u;
                        for (unsigned int j = 0; j < cnt; j++)
                        {
                            unsigned int idx = 0;
                            su(idx);
                            if (j < actual)
                                fv[j] = idx;
                        }
                        got_indices = true;
                    }
                    else
                    {
                        for (unsigned int j = 0; j < cnt; j++)
                        {
                            float tmp;
                            sf(tmp);
                        }
                    }
                }
                if (got_indices)
                    ply_push_face(mesh, fv, actual);
            }
            else
            {
                for (const auto &prop : elem.props)
                {
                    if (prop.is_list)
                    {
                        int cnt = 0;
                        si(cnt);
                        for (int j = 0; j < cnt; j++)
                        {
                            float tmp;
                            sf(tmp);
                        }
                    }
                    else
                    {
                        float tmp;
                        sf(tmp);
                    }
                }
            }
        }
    }

    return !truncated;
}

// ─── load_ply_binary ──────────────────────────────────────────────────────────
// File must be positioned immediately after end_header.
// Returns false on read failure or truncated input.

static bool load_ply_binary(FILE *f, const PlyHeader &hdr, Mesh &mesh)
{
    long data_start = std::ftell(f);
    if (data_start < 0)
        return false;
    if (std::fseek(f, 0, SEEK_END) != 0)
        return false;
    long file_end = std::ftell(f);
    if (file_end < 0 || file_end < data_start)
        return false;
    if (std::fseek(f, data_start, SEEK_SET) != 0)
        return false;

    size_t data_len = static_cast<size_t>(file_end - data_start);
    std::vector<uint8_t> buf(data_len);
    if (std::fread(buf.data(), 1, data_len, f) != data_len)
        return false;

    const uint8_t *p = buf.data();
    const uint8_t *end = buf.data() + data_len;
    bool truncated = false;
    bool be = (hdr.fmt == PlyFmt::BE);

    auto read_scalar = [&](PlyPType t) -> float
    {
        if (p + ply_ptype_size(t) > end)
        {
            truncated = true;
            return 0.0f;
        }
        float result = 0.0f;
        switch (t)
        {
        case PlyPType::I8:
        {
            int8_t v;
            std::memcpy(&v, p, 1);
            p += 1;
            result = static_cast<float>(v);
            break;
        }
        case PlyPType::U8:
        {
            result = static_cast<float>(*p);
            p += 1;
            break;
        }
        case PlyPType::I16:
        {
            uint16_t b;
            std::memcpy(&b, p, 2);
            p += 2;
            if (be)
                b = ply_bs16(b);
            int16_t v;
            std::memcpy(&v, &b, 2);
            result = static_cast<float>(v);
            break;
        }
        case PlyPType::U16:
        {
            uint16_t b;
            std::memcpy(&b, p, 2);
            p += 2;
            if (be)
                b = ply_bs16(b);
            result = static_cast<float>(b);
            break;
        }
        case PlyPType::I32:
        {
            uint32_t b;
            std::memcpy(&b, p, 4);
            p += 4;
            if (be)
                b = ply_bs32(b);
            int32_t v;
            std::memcpy(&v, &b, 4);
            result = static_cast<float>(v);
            break;
        }
        case PlyPType::U32:
        {
            uint32_t b;
            std::memcpy(&b, p, 4);
            p += 4;
            if (be)
                b = ply_bs32(b);
            result = static_cast<float>(b);
            break;
        }
        case PlyPType::F32:
        {
            uint32_t b;
            std::memcpy(&b, p, 4);
            p += 4;
            if (be)
                b = ply_bs32(b);
            std::memcpy(&result, &b, 4);
            break;
        }
        case PlyPType::F64:
        {
            uint64_t b;
            std::memcpy(&b, p, 8);
            p += 8;
            if (be)
                b = ply_bs64(b);
            double d;
            std::memcpy(&d, &b, 8);
            result = static_cast<float>(d);
            break;
        }
        case PlyPType::UNKNOWN:
            break;
        }
        return result;
    };

    auto read_uint_direct = [&](PlyPType t) -> uint32_t
    {
        if (p + static_cast<size_t>(ply_ptype_size(t)) > end)
        {
            truncated = true;
            return 0;
        }
        switch (t)
        {
        case PlyPType::I8:
        {
            int8_t v;
            std::memcpy(&v, p, 1);
            p += 1;
            return static_cast<uint32_t>(static_cast<int32_t>(v));
        }
        case PlyPType::U8:
        {
            uint8_t v = *p;
            p += 1;
            return v;
        }
        case PlyPType::I16:
        {
            uint16_t b;
            std::memcpy(&b, p, 2);
            p += 2;
            if (be)
                b = ply_bs16(b);
            int16_t v;
            std::memcpy(&v, &b, 2);
            return static_cast<uint32_t>(static_cast<int32_t>(v));
        }
        case PlyPType::U16:
        {
            uint16_t b;
            std::memcpy(&b, p, 2);
            p += 2;
            if (be)
                b = ply_bs16(b);
            return b;
        }
        case PlyPType::I32:
        {
            uint32_t b;
            std::memcpy(&b, p, 4);
            p += 4;
            if (be)
                b = ply_bs32(b);
            return b;
        }
        case PlyPType::U32:
        {
            uint32_t b;
            std::memcpy(&b, p, 4);
            p += 4;
            if (be)
                b = ply_bs32(b);
            return b;
        }
        default:
            return static_cast<uint32_t>(static_cast<int32_t>(read_scalar(t)));
        }
    };

    const PlyElem &vert_elem = hdr.elements[static_cast<size_t>(hdr.vert_idx)];
    const PlyElem &face_elem = hdr.elements[static_cast<size_t>(hdr.face_idx)];

    for (const auto &elem : hdr.elements)
    {
        for (int i = 0; i < elem.count; i++)
        {
            if (p >= end)
            {
                truncated = true;
                break;
            }

            if (&elem == &vert_elem)
            {
                Vertex v{};
                for (const auto &prop : elem.props)
                {
                    if (prop.is_list)
                    {
                        uint32_t cnt = read_uint_direct(prop.list_count_t);
                        size_t elem_sz = static_cast<size_t>(ply_ptype_size(prop.list_elem_t));
                        size_t remaining = (p < end) ? static_cast<size_t>(end - p) : 0u;
                        if (elem_sz > 0 && cnt > remaining / elem_sz)
                        {
                            truncated = true;
                            break;
                        }
                        for (uint32_t j = 0; j < cnt; j++)
                            read_scalar(prop.list_elem_t);
                        continue;
                    }
                    float val = read_scalar(prop.type);
                    ply_apply_prop(v, prop.sem, val, prop.type);
                }
                mesh.vertices.push_back(v);
            }
            else if (&elem == &face_elem)
            {
                bool got_indices = false;
                uint32_t fv[64];
                uint32_t actual = 0;
                for (const auto &prop : elem.props)
                {
                    if (!prop.is_list)
                    {
                        read_scalar(prop.type);
                        continue;
                    }

                    uint32_t cnt = read_uint_direct(prop.list_count_t);
                    size_t elem_sz = static_cast<size_t>(ply_ptype_size(prop.list_elem_t));
                    size_t remaining = (p < end) ? static_cast<size_t>(end - p) : 0u;
                    if (elem_sz > 0 && cnt > remaining / elem_sz)
                    {
                        truncated = true;
                        break;
                    }
                    if (!got_indices)
                    {
                        actual = (cnt < 64u) ? cnt : 64u;
                        for (uint32_t j = 0; j < cnt; j++)
                        {
                            uint32_t idx = read_uint_direct(prop.list_elem_t);
                            if (j < actual)
                                fv[j] = idx;
                        }
                        got_indices = true;
                    }
                    else
                    {
                        p += static_cast<size_t>(cnt) * elem_sz;
                    }
                }
                if (got_indices)
                    ply_push_face(mesh, fv, actual);
            }
            else
            {
                for (const auto &prop : elem.props)
                {
                    if (prop.is_list)
                    {
                        uint32_t cnt = read_uint_direct(prop.list_count_t);
                        size_t skip = static_cast<size_t>(cnt) * static_cast<size_t>(ply_ptype_size(prop.list_elem_t));
                        if (skip > static_cast<size_t>(end - p))
                        {
                            truncated = true;
                            break;
                        }
                        p += skip;
                    }
                    else
                    {
                        size_t skip = static_cast<size_t>(ply_ptype_size(prop.type));
                        if (skip > static_cast<size_t>(end - p))
                        {
                            truncated = true;
                            break;
                        }
                        p += skip;
                    }
                }
            }
        }
    }

    return !truncated;
}

// ─── Mesh::load_ply ───────────────────────────────────────────────────────────
// Supports ASCII, binary little-endian, and binary big-endian PLY files.
// Reads vertex positions, normals (nx/ny/nz), and UVs (s/t, u/v, texture_u/v).
// Unrecognized property names are skipped; unrecognized types are rejected.

bool Mesh::load_ply(const std::string &path)
{
    MeshSnapshot snap(*this);

    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;

    PlyHeader hdr;
    if (!ply_parse_header(f, hdr))
    {
        std::fclose(f);
        return false;
    }

    const PlyElem &vert_elem = hdr.elements[static_cast<size_t>(hdr.vert_idx)];
    const PlyElem &face_elem = hdr.elements[static_cast<size_t>(hdr.face_idx)];

    // Validate element counts against the file's actual data section before
    // reserve(). Each element occupies at least one byte in the data section
    // (binary: smallest scalar/list-count is 1 B; ASCII: one digit + newline),
    // so a count that exceeds the remaining bytes is definitionally malformed.
    // This rejects the trivial DoS (e.g. count = INT_MAX, UINT32_MAX cast to
    // int) without imposing an arbitrary cap — any file that *could* fit its
    // claimed counts passes.
    long data_start = std::ftell(f);
    long file_end = -1;
    if (data_start >= 0 && std::fseek(f, 0, SEEK_END) == 0)
        file_end = std::ftell(f);
    if (data_start < 0 || file_end < 0 || file_end < data_start ||
        std::fseek(f, data_start, SEEK_SET) != 0)
    {
        std::fclose(f);
        return false;
    }
    const uint64_t data_bytes = static_cast<uint64_t>(file_end - data_start);
    if (vert_elem.count < 0 || static_cast<uint64_t>(vert_elem.count) > data_bytes ||
        face_elem.count < 0 || static_cast<uint64_t>(face_elem.count) > data_bytes)
    {
        std::fclose(f);
        return false;
    }

    vertices.reserve(static_cast<size_t>(vert_elem.count));
    if (face_elem.count > 0)
        triangles.reserve(static_cast<size_t>(face_elem.count));

    materials.push_back(Material{});

    bool ok = (hdr.fmt == PlyFmt::ASCII)
                  ? load_ply_ascii(f, hdr, *this)
                  : load_ply_binary(f, hdr, *this);

    std::fclose(f);

    if (!ok || vertices.empty() || triangles.empty())
        return false;

    if (!hdr.has_normals)
        compute_normals();

    has_vertex_colors = hdr.has_colors;
    snap.commit();
    return true;
}
