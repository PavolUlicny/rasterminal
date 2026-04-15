#include "mesh.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ─── Mesh::load_ply ───────────────────────────────────────────────────────────
// Supports ASCII, binary little-endian, and binary big-endian PLY files.
// Reads vertex positions, normals (nx/ny/nz), and UVs (s/t, u/v, texture_u/v).
// Unknown properties are skipped. Only element vertex and element face are used.

bool Mesh::load_ply(const std::string &path)
{
    // Save state so we can roll back on any failure path.
    const size_t v0 = vertices.size();
    const size_t t0 = triangles.size();
    const size_t m0 = materials.size();
    const size_t tx0 = textures.size();

    auto rollback = [&]
    {
        vertices.resize(v0);
        triangles.resize(t0);
        materials.resize(m0);
        textures.resize(tx0);
    };

    FILE *f = std::fopen(path.c_str(), "rb"); // binary mode for both ASCII and binary PLY
    if (!f)
        return false;

    // ── Types ─────────────────────────────────────────────────────────────────

    enum class Fmt
    {
        ASCII,
        LE,
        BE
    };

    enum class PType
    {
        I8,
        U8,
        I16,
        U16,
        I32,
        U32,
        F32,
        F64,
        UNKNOWN // unrecognised type — size 0 triggers truncation guard
    };

    auto ptype_size = [](PType t) -> int
    {
        switch (t)
        {
        case PType::I8:
        case PType::U8:
            return 1;
        case PType::I16:
        case PType::U16:
            return 2;
        case PType::I32:
        case PType::U32:
        case PType::F32:
            return 4;
        case PType::F64:
            return 8;
        case PType::UNKNOWN:
            return 0; // triggers bounds check → truncated flag
        }
        return 0;
    };

    auto parse_ptype = [](const char *s) -> PType
    {
        if (!std::strcmp(s, "char") || !std::strcmp(s, "int8"))
            return PType::I8;
        if (!std::strcmp(s, "uchar") || !std::strcmp(s, "uint8"))
            return PType::U8;
        if (!std::strcmp(s, "short") || !std::strcmp(s, "int16"))
            return PType::I16;
        if (!std::strcmp(s, "ushort") || !std::strcmp(s, "uint16"))
            return PType::U16;
        if (!std::strcmp(s, "int") || !std::strcmp(s, "int32"))
            return PType::I32;
        if (!std::strcmp(s, "uint") || !std::strcmp(s, "uint32"))
            return PType::U32;
        if (!std::strcmp(s, "float") || !std::strcmp(s, "float32"))
            return PType::F32;
        if (!std::strcmp(s, "double") || !std::strcmp(s, "float64"))
            return PType::F64;
        return PType::UNKNOWN;
    };

    // Byte-swap helpers for big-endian binary files.
    auto bs16 = [](uint16_t v) -> uint16_t
    { return static_cast<uint16_t>((v >> 8) | (v << 8)); };
    auto bs32 = [](uint32_t v) -> uint32_t
    {
        return ((v >> 24) & 0xFFu) | ((v >> 8) & 0xFF00u) |
               ((v << 8) & 0xFF0000u) | ((v << 24) & 0xFF000000u);
    };
    auto bs64 = [&](uint64_t v) -> uint64_t
    {
        uint32_t lo = bs32(static_cast<uint32_t>(v & 0xFFFFFFFFu));
        uint32_t hi = bs32(static_cast<uint32_t>(v >> 32));
        return (static_cast<uint64_t>(lo) << 32) | static_cast<uint64_t>(hi);
    };

    struct Prop
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
            SKIP
        } sem = SKIP;
        PType type = PType::F32;
        bool is_list = false;
        PType list_count_t = PType::U8;
        PType list_elem_t = PType::I32;
    };

    struct Elem
    {
        std::string name;
        int count = 0;
        std::vector<Prop> props;
    };

    // Helpers shared by ASCII and binary paths.
    auto apply_prop = [](Vertex &v, Prop::Sem sem, float val)
    {
        switch (sem)
        {
        case Prop::X:
            v.pos.x = val;
            break;
        case Prop::Y:
            v.pos.y = val;
            break;
        case Prop::Z:
            v.pos.z = val;
            break;
        case Prop::NX:
            v.normal.x = val;
            break;
        case Prop::NY:
            v.normal.y = val;
            break;
        case Prop::NZ:
            v.normal.z = val;
            break;
        case Prop::S:
            v.uv.x = val;
            break;
        case Prop::T:
            v.uv.y = val;
            break;
        default:
            break;
        }
    };

    auto push_face = [&](uint32_t *fv, uint32_t actual)
    {
        uint32_t n_verts = static_cast<uint32_t>(vertices.size());
        for (uint32_t j = 1; j + 1 < actual; j++)
        {
            if (fv[0] >= n_verts || fv[j] >= n_verts || fv[j + 1] >= n_verts)
                continue;
            Triangle t;
            t.v[0] = fv[0];
            t.v[1] = fv[j];
            t.v[2] = fv[j + 1];
            t.material_idx = 0;
            triangles.push_back(t);
        }
    };

    // ── Parse header ──────────────────────────────────────────────────────────

    char line[1024];

    if (!std::fgets(line, sizeof(line), f) || std::strncmp(line, "ply", 3) != 0)
    {
        std::fclose(f);
        return false;
    }

    Fmt fmt = Fmt::ASCII;
    std::vector<Elem> elements;
    int cur = -1; // index of element currently being built

    while (std::fgets(line, sizeof(line), f))
    {
        // Strip trailing whitespace/newlines.
        int len = static_cast<int>(std::strlen(line));
        while (len > 0 && line[len - 1] <= ' ')
            line[--len] = '\0';

        if (std::strcmp(line, "end_header") == 0)
            break;

        if (std::strncmp(line, "format ", 7) == 0)
        {
            if (std::strstr(line, "binary_little_endian"))
                fmt = Fmt::LE;
            else if (std::strstr(line, "binary_big_endian"))
                fmt = Fmt::BE;
        }
        else if (std::strncmp(line, "element ", 8) == 0)
        {
            char name[64] = {};
            int count = 0;
            std::sscanf(line + 8, "%63s %d", name, &count);
            elements.push_back({name, count, {}});
            cur = static_cast<int>(elements.size()) - 1;
        }
        else if (std::strncmp(line, "property ", 9) == 0 && cur >= 0)
        {
            const char *p = line + 9;
            Prop prop;

            if (std::strncmp(p, "list ", 5) == 0)
            {
                char ct[32] = {}, et[32] = {}, name[64] = {};
                std::sscanf(p + 5, "%31s %31s %63s", ct, et, name);
                prop.is_list = true;
                prop.list_count_t = parse_ptype(ct);
                prop.list_elem_t = parse_ptype(et);
                prop.type = prop.list_elem_t;
                // sem stays SKIP; handled as face indices in the read loop
            }
            else
            {
                char tstr[32] = {}, name[64] = {};
                std::sscanf(p, "%31s %63s", tstr, name);
                prop.type = parse_ptype(tstr);
                prop.is_list = false;

                if (!std::strcmp(name, "x"))
                    prop.sem = Prop::X;
                else if (!std::strcmp(name, "y"))
                    prop.sem = Prop::Y;
                else if (!std::strcmp(name, "z"))
                    prop.sem = Prop::Z;
                else if (!std::strcmp(name, "nx"))
                    prop.sem = Prop::NX;
                else if (!std::strcmp(name, "ny"))
                    prop.sem = Prop::NY;
                else if (!std::strcmp(name, "nz"))
                    prop.sem = Prop::NZ;
                else if (!std::strcmp(name, "s") || !std::strcmp(name, "u") ||
                         !std::strcmp(name, "texture_u") || !std::strcmp(name, "texture_s"))
                    prop.sem = Prop::S;
                else if (!std::strcmp(name, "t") || !std::strcmp(name, "v") ||
                         !std::strcmp(name, "texture_v") || !std::strcmp(name, "texture_t"))
                    prop.sem = Prop::T;
                // else SKIP
            }

            // Reject unknown types immediately — size 0 would desync the binary reader.
            bool unknown_type = prop.is_list
                                    ? (prop.list_count_t == PType::UNKNOWN || prop.list_elem_t == PType::UNKNOWN)
                                    : (prop.type == PType::UNKNOWN);
            if (unknown_type)
            {
                std::fclose(f);
                rollback();
                return false;
            }

            elements[static_cast<size_t>(cur)].props.push_back(prop);
        }
        // Comments and other directives are ignored.
    }

    // Find vertex and face elements by name.
    Elem *vert_elem = nullptr, *face_elem = nullptr;
    for (auto &e : elements)
    {
        if (e.name == "vertex")
            vert_elem = &e;
        else if (e.name == "face")
            face_elem = &e;
    }

    if (!vert_elem || !face_elem || vert_elem->count <= 0)
    {
        std::fclose(f);
        return false;
    }

    bool has_normals = false;
    for (const auto &p : vert_elem->props)
        if (p.sem == Prop::NX)
            has_normals = true;

    // ── Read data ─────────────────────────────────────────────────────────────

    vertices.reserve(static_cast<size_t>(vert_elem->count));
    if (face_elem && face_elem->count > 0)
        triangles.reserve(static_cast<size_t>(face_elem->count));

    bool truncated = false; // set by binary read_scalar on premature EOF

    if (fmt == Fmt::ASCII)
    {
        // ASCII: fscanf skips whitespace and newlines automatically.
        // Set truncated on any read failure so the caller rejects partial data.
        auto sf = [&](float &v)
        { if (std::fscanf(f, " %f", &v) != 1) truncated = true; };
        auto si = [&](int &v)
        { if (std::fscanf(f, " %d", &v) != 1) truncated = true; };
        auto su = [&](unsigned int &v)
        { if (std::fscanf(f, " %u", &v) != 1) truncated = true; };

        for (auto &elem : elements)
        {
            for (int i = 0; i < elem.count; i++)
            {
                if (&elem == vert_elem)
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
                        apply_prop(v, prop.sem, val);
                    }
                    vertices.push_back(v);
                }
                else if (face_elem && &elem == face_elem)
                {
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
                        uint32_t fv[64];
                        uint32_t actual = (cnt < 64u) ? cnt : 64u;
                        for (unsigned int j = 0; j < cnt; j++)
                        {
                            unsigned int idx = 0;
                            su(idx);
                            if (j < actual)
                                fv[j] = idx;
                        }
                        push_face(fv, actual);
                        break; // only the first list property is face indices
                    }
                }
                else
                {
                    // Skip other elements.
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
    }
    else
    {
        // Binary: read entire remainder into a buffer, then walk it.
        long data_start = std::ftell(f);
        std::fseek(f, 0, SEEK_END);
        long file_end = std::ftell(f);
        std::fseek(f, data_start, SEEK_SET);

        size_t data_len = static_cast<size_t>(file_end - data_start);
        std::vector<uint8_t> buf(data_len);
        if (std::fread(buf.data(), 1, data_len, f) != data_len)
        {
            std::fclose(f);
            rollback();
            return false;
        }

        const uint8_t *p = buf.data();
        const uint8_t *end = buf.data() + data_len;
        bool be = (fmt == Fmt::BE);

        // Read a typed scalar from the buffer and advance the pointer.
        // Sets truncated=true and returns 0 if the buffer is exhausted early.
        auto read_scalar = [&](PType t) -> float
        {
            if (p + ptype_size(t) > end)
            {
                truncated = true;
                return 0.0f;
            }
            float result = 0.0f;
            switch (t)
            {
            case PType::I8:
            {
                int8_t v;
                std::memcpy(&v, p, 1);
                p += 1;
                result = static_cast<float>(v);
                break;
            }
            case PType::U8:
            {
                result = static_cast<float>(*p);
                p += 1;
                break;
            }
            case PType::I16:
            {
                uint16_t b;
                std::memcpy(&b, p, 2);
                p += 2;
                if (be)
                    b = bs16(b);
                int16_t v;
                std::memcpy(&v, &b, 2);
                result = static_cast<float>(v);
                break;
            }
            case PType::U16:
            {
                uint16_t b;
                std::memcpy(&b, p, 2);
                p += 2;
                if (be)
                    b = bs16(b);
                result = static_cast<float>(b);
                break;
            }
            case PType::I32:
            {
                uint32_t b;
                std::memcpy(&b, p, 4);
                p += 4;
                if (be)
                    b = bs32(b);
                int32_t v;
                std::memcpy(&v, &b, 4);
                result = static_cast<float>(v);
                break;
            }
            case PType::U32:
            {
                uint32_t b;
                std::memcpy(&b, p, 4);
                p += 4;
                if (be)
                    b = bs32(b);
                result = static_cast<float>(b);
                break;
            }
            case PType::F32:
            {
                uint32_t b;
                std::memcpy(&b, p, 4);
                p += 4;
                if (be)
                    b = bs32(b);
                std::memcpy(&result, &b, 4);
                break;
            }
            case PType::F64:
            {
                uint64_t b;
                std::memcpy(&b, p, 8);
                p += 8;
                if (be)
                    b = bs64(b);
                double d;
                std::memcpy(&d, &b, 8);
                result = static_cast<float>(d);
                break;
            }
            case PType::UNKNOWN:
                break; // size 0 — bounds check above already set truncated
            }
            return result;
        };

        // Read an integer type directly as uint32 — avoids float conversion,
        // which loses precision for indices >= 2^24.
        auto read_uint_direct = [&](PType t) -> uint32_t
        {
            if (p + static_cast<size_t>(ptype_size(t)) > end)
            {
                truncated = true;
                return 0;
            }
            switch (t)
            {
            case PType::I8:
            {
                int8_t v;
                std::memcpy(&v, p, 1);
                p += 1;
                return static_cast<uint32_t>(static_cast<int32_t>(v));
            }
            case PType::U8:
            {
                uint8_t v = *p;
                p += 1;
                return v;
            }
            case PType::I16:
            {
                uint16_t b;
                std::memcpy(&b, p, 2);
                p += 2;
                if (be)
                    b = bs16(b);
                int16_t v;
                std::memcpy(&v, &b, 2);
                return static_cast<uint32_t>(static_cast<int32_t>(v));
            }
            case PType::U16:
            {
                uint16_t b;
                std::memcpy(&b, p, 2);
                p += 2;
                if (be)
                    b = bs16(b);
                return b;
            }
            case PType::I32:
            {
                uint32_t b;
                std::memcpy(&b, p, 4);
                p += 4;
                if (be)
                    b = bs32(b);
                return b; // reinterpret signed as unsigned bits
            }
            case PType::U32:
            {
                uint32_t b;
                std::memcpy(&b, p, 4);
                p += 4;
                if (be)
                    b = bs32(b);
                return b;
            }
            default:
                // F32/F64 face indices are non-standard; fall back to float path.
                return static_cast<uint32_t>(static_cast<int32_t>(read_scalar(t)));
            }
        };

        for (auto &elem : elements)
        {
            for (int i = 0; i < elem.count; i++)
            {
                if (p >= end)
                {
                    truncated = true;
                    break;
                }

                if (&elem == vert_elem)
                {
                    Vertex v{};
                    for (const auto &prop : elem.props)
                    {
                        if (prop.is_list)
                        {
                            uint32_t cnt = read_uint_direct(prop.list_count_t);
                            // Reject count that exceeds remaining buffer — fail-fast on malformed data.
                            size_t elem_sz = static_cast<size_t>(ptype_size(prop.list_elem_t));
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
                        apply_prop(v, prop.sem, val);
                    }
                    vertices.push_back(v);
                }
                else if (face_elem && &elem == face_elem)
                {
                    for (const auto &prop : elem.props)
                    {
                        if (!prop.is_list)
                        {
                            read_scalar(prop.type);
                            continue;
                        }

                        uint32_t cnt = read_uint_direct(prop.list_count_t);
                        // Reject count that exceeds remaining buffer — fail-fast on malformed data.
                        size_t elem_sz = static_cast<size_t>(ptype_size(prop.list_elem_t));
                        size_t remaining = (p < end) ? static_cast<size_t>(end - p) : 0u;
                        if (elem_sz > 0 && cnt > remaining / elem_sz)
                        {
                            truncated = true;
                            break;
                        }
                        uint32_t fv[64];
                        uint32_t actual = (cnt < 64u) ? cnt : 64u;
                        for (uint32_t j = 0; j < cnt; j++)
                        {
                            uint32_t idx = read_uint_direct(prop.list_elem_t);
                            if (j < actual)
                                fv[j] = idx;
                        }
                        push_face(fv, actual);
                        break; // only the first list property is face indices
                    }
                }
                else
                {
                    // Skip other elements byte-exactly.
                    for (const auto &prop : elem.props)
                    {
                        if (prop.is_list)
                        {
                            // Read count directly as uint32 to avoid negative/overflow via float.
                            uint32_t cnt = read_uint_direct(prop.list_count_t);
                            size_t skip = static_cast<size_t>(cnt) * static_cast<size_t>(ptype_size(prop.list_elem_t));
                            // Use remaining-bytes check to avoid pointer-arithmetic overflow.
                            if (skip > static_cast<size_t>(end - p))
                            {
                                truncated = true;
                                break;
                            }
                            p += skip;
                        }
                        else
                        {
                            size_t skip = static_cast<size_t>(ptype_size(prop.type));
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
    }

    std::fclose(f);

    if (truncated)
    {
        rollback();
        return false;
    }

    if (vertices.empty() || triangles.empty())
    {
        rollback();
        return false;
    }

    materials.push_back(Material{});

    if (!has_normals)
        compute_normals();

    return true;
}
