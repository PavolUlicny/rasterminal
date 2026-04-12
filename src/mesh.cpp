#include "mesh.h"
#include "texture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ─── vertex deduplication key ─────────────────────────────────────────────────
// OBJ uses separate index streams for positions, normals, and UVs.
// A unique Vertex in the final buffer is identified by the combination of all three.

struct FaceVertex
{
    int pi, ni, ti; // position, normal, uv indices (0-based; -1 = absent)

    bool operator==(const FaceVertex &o) const
    {
        return pi == o.pi && ni == o.ni && ti == o.ti;
    }
};

struct FaceVertexHash
{
    size_t operator()(const FaceVertex &fv) const
    {
        size_t h = (size_t)(fv.pi + 1);
        h ^= (size_t)(fv.ni + 1) * 2654435761u;
        h ^= (size_t)(fv.ti + 1) * 40503u;
        return h;
    }
};

// ─── helpers ──────────────────────────────────────────────────────────────────

// Resolve a 1-based OBJ index (possibly negative) to a 0-based index.
static int resolve(int raw, int count)
{
    if (raw > 0)
        return raw - 1;
    if (raw < 0)
        return count + raw; // negative = relative to end
    return -1;              // 0 is invalid in OBJ
}

// Parse one face-vertex token ("pi", "pi/ti", "pi//ni", "pi/ti/ni").
// Advances *pp past the token. Returns false if nothing could be parsed.
static bool parse_face_vertex(const char **pp, FaceVertex &out,
                              int npos, int nnorm, int nuv)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '\n' || *p == '\r')
        return false;

    char *end;
    int raw_pi = (int)strtol(p, &end, 10);
    if (end == p)
        return false;
    p = end;

    out.pi = resolve(raw_pi, npos);
    out.ni = -1;
    out.ti = -1;

    if (*p == '/')
    {
        p++;
        if (*p != '/')
        {
            int raw_ti = (int)strtol(p, &end, 10);
            if (end != p)
            {
                out.ti = resolve(raw_ti, nuv);
                p = end;
            }
        }
        if (*p == '/')
        {
            p++;
            int raw_ni = (int)strtol(p, &end, 10);
            if (end != p)
            {
                out.ni = resolve(raw_ni, nnorm);
                p = end;
            }
        }
    }

    *pp = p;
    return true;
}

// ─── load_mtl ─────────────────────────────────────────────────────────────────
// Parse a .mtl file and append materials to mesh.materials.
// Textures referenced by map_Kd are loaded into mesh.textures.
// mtl_dir is the directory of the .mtl file (used to resolve relative paths).
// Returns a map from material name → index in mesh.materials.

static std::unordered_map<std::string, uint32_t>
load_mtl(const std::string &path, std::vector<Material> &materials,
         std::vector<Texture> &textures, const std::string &mtl_dir)
{
    std::unordered_map<std::string, uint32_t> mat_map;

    FILE *f = std::fopen(path.c_str(), "r");
    if (!f)
        return mat_map;

    char line[512];
    int current = -1; // index of material being built (-1 = none yet)

    while (std::fgets(line, sizeof(line), f))
    {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;

        if (std::strncmp(p, "newmtl", 6) == 0 && (p[6] == ' ' || p[6] == '\t'))
        {
            p += 7;
            while (*p == ' ' || *p == '\t')
                p++;
            std::string name(p);
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' '))
                name.pop_back();

            current = (int)materials.size();
            materials.push_back(Material{});
            mat_map[name] = (uint32_t)current;
        }
        else if (current >= 0)
        {
            if (std::strncmp(p, "Kd", 2) == 0 && (p[2] == ' ' || p[2] == '\t'))
            {
                float r, g, b;
                if (std::sscanf(p + 3, "%f %f %f", &r, &g, &b) == 3)
                    materials[current].diffuse = {r, g, b};
            }
            else if (std::strncmp(p, "Ks", 2) == 0 && (p[2] == ' ' || p[2] == '\t'))
            {
                float r, g, b;
                if (std::sscanf(p + 3, "%f %f %f", &r, &g, &b) == 3)
                    materials[current].specular = {r, g, b};
            }
            else if (std::strncmp(p, "Ns", 2) == 0 && (p[2] == ' ' || p[2] == '\t'))
            {
                float ns;
                if (std::sscanf(p + 3, "%f", &ns) == 1)
                    materials[current].shininess = ns;
            }
            else if (std::strncmp(p, "map_Kd", 6) == 0 && (p[6] == ' ' || p[6] == '\t'))
            {
                // Diffuse texture map.  The filename follows the directive.
                const char *q = p + 7;
                while (*q == ' ' || *q == '\t')
                    q++;
                std::string tex_name(q);
                while (!tex_name.empty() &&
                       (tex_name.back() == '\n' || tex_name.back() == '\r' || tex_name.back() == ' '))
                    tex_name.pop_back();

                if (!tex_name.empty())
                {
                    Texture tex;
                    if (tex.load_png(mtl_dir + tex_name))
                    {
                        materials[current].diffuse_tex = (int)textures.size();
                        textures.push_back(std::move(tex));
                    }
                }
            }
            else if ((std::strncmp(p, "map_Kn", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) ||
                     (std::strncmp(p, "map_bump", 8) == 0 && (p[8] == ' ' || p[8] == '\t')))
            {
                // Normal map (tangent-space).  Handles both map_Kn and map_bump.
                int prefix_len = (p[4] == 'K') ? 7 : 9; // "map_Kn " or "map_bump "
                const char *q = p + prefix_len;
                // Skip any MTL option tokens of the form "-flag [value]" (e.g. -bm 0.5).
                while (*q == '-')
                {
                    while (*q && *q != ' ' && *q != '\t')
                        q++; // skip flag name
                    while (*q == ' ' || *q == '\t')
                        q++; // skip whitespace
                    // If what follows looks like a number, skip it too (it's the value).
                    if (*q == '-' || (*q >= '0' && *q <= '9') || *q == '.')
                        while (*q && *q != ' ' && *q != '\t')
                            q++;
                    while (*q == ' ' || *q == '\t')
                        q++;
                }
                std::string tex_name(q);
                while (!tex_name.empty() &&
                       (tex_name.back() == '\n' || tex_name.back() == '\r' || tex_name.back() == ' '))
                    tex_name.pop_back();

                if (!tex_name.empty())
                {
                    Texture tex;
                    if (tex.load_png(mtl_dir + tex_name))
                    {
                        materials[current].normal_tex = (int)textures.size();
                        textures.push_back(std::move(tex));
                    }
                }
            }
        }
    }

    std::fclose(f);
    return mat_map;
}

// ─── Mesh::load_obj ───────────────────────────────────────────────────────────

bool Mesh::load_obj(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f)
        return false;

    // Directory of the OBJ file, used to resolve relative mtllib paths.
    std::string obj_dir;
    {
        size_t slash = path.find_last_of("/\\");
        obj_dir = (slash != std::string::npos) ? path.substr(0, slash + 1) : "";
    }

    // Index 0 is always the default material (white, slight specular).
    materials.push_back(Material{});
    std::unordered_map<std::string, uint32_t> mat_map;
    uint32_t current_mat = 0;

    std::vector<vec3> pos_pool;
    std::vector<vec3> norm_pool;
    std::vector<vec2> uv_pool;

    std::unordered_map<FaceVertex, uint32_t, FaceVertexHash> vertex_map;

    bool has_normals = false;
    char line[512];

    // Returns the index of an existing or newly created Vertex.
    auto get_vertex = [&](FaceVertex fv) -> uint32_t
    {
        auto it = vertex_map.find(fv);
        if (it != vertex_map.end())
            return it->second;

        Vertex v;
        v.pos = (fv.pi >= 0 && fv.pi < (int)pos_pool.size()) ? pos_pool[fv.pi] : vec3{};
        v.normal = (fv.ni >= 0 && fv.ni < (int)norm_pool.size()) ? norm_pool[fv.ni] : vec3{};
        v.uv = (fv.ti >= 0 && fv.ti < (int)uv_pool.size()) ? uv_pool[fv.ti] : vec2{};

        uint32_t idx = (uint32_t)vertices.size();
        vertices.push_back(v);
        vertex_map[fv] = idx;
        return idx;
    };

    while (std::fgets(line, sizeof(line), f))
    {
        const char *p = line;

        if (p[0] == 'v' && p[1] == ' ')
        {
            vec3 v;
            std::sscanf(p + 2, "%f %f %f", &v.x, &v.y, &v.z);
            pos_pool.push_back(v);
        }
        else if (p[0] == 'v' && p[1] == 'n')
        {
            vec3 n;
            std::sscanf(p + 3, "%f %f %f", &n.x, &n.y, &n.z);
            norm_pool.push_back(n);
            has_normals = true;
        }
        else if (p[0] == 'v' && p[1] == 't')
        {
            vec2 uv;
            std::sscanf(p + 3, "%f %f", &uv.x, &uv.y);
            uv_pool.push_back(uv);
        }
        else if (std::strncmp(p, "mtllib", 6) == 0 && (p[6] == ' ' || p[6] == '\t'))
        {
            // Load the material library.  The filename follows "mtllib ".
            std::string mtl_name(p + 7);
            while (!mtl_name.empty() && (mtl_name.back() == '\n' || mtl_name.back() == '\r' || mtl_name.back() == ' '))
                mtl_name.pop_back();

            // MTL materials are appended starting at index 1 (0 = default).
            // Pass obj_dir so relative texture paths resolve correctly.
            mat_map = load_mtl(obj_dir + mtl_name, materials, textures, obj_dir);
        }
        else if (std::strncmp(p, "usemtl", 6) == 0 && (p[6] == ' ' || p[6] == '\t'))
        {
            std::string mat_name(p + 7);
            while (!mat_name.empty() && (mat_name.back() == '\n' || mat_name.back() == '\r' || mat_name.back() == ' '))
                mat_name.pop_back();

            auto it = mat_map.find(mat_name);
            current_mat = (it != mat_map.end()) ? it->second : 0;
        }
        else if (p[0] == 'f' && p[1] == ' ')
        {
            p += 2;

            // Read all face vertices (OBJ supports arbitrary polygon sizes).
            FaceVertex fverts[32]; // 32 is well beyond any real-world polygon
            int count = 0;

            while (count < (int)(sizeof(fverts) / sizeof(fverts[0])))
            {
                FaceVertex fv;
                if (!parse_face_vertex(&p, fv,
                                       (int)pos_pool.size(),
                                       (int)norm_pool.size(),
                                       (int)uv_pool.size()))
                    break;
                fverts[count++] = fv;
            }

            // Fan triangulation: (0,1,2), (0,2,3), (0,3,4), …
            // Works correctly for convex polygons (the common case in OBJ).
            uint32_t v0 = get_vertex(fverts[0]);
            for (int i = 1; i + 1 < count; i++)
            {
                Triangle t;
                t.v[0] = v0;
                t.v[1] = get_vertex(fverts[i]);
                t.v[2] = get_vertex(fverts[i + 1]);
                t.material_idx = current_mat;
                triangles.push_back(t);
            }
        }
    }

    std::fclose(f);

    if (!has_normals)
        compute_normals();

    compute_tangents();

    return !triangles.empty();
}

// ─── Mesh::compute_tangents ───────────────────────────────────────────────────

void Mesh::compute_tangents()
{
    for (auto &v : vertices)
        v.tangent = vec3{};

    // Accumulate tangent vectors from each triangle's UV layout.
    // For triangle (P0,P1,P2) with UVs (u0,v0),(u1,v1),(u2,v2):
    //   T = (dP1*dv2 - dP2*dv1) / (du1*dv2 - du2*dv1)
    for (const auto &tri : triangles)
    {
        const Vertex &v0 = vertices[tri.v[0]];
        const Vertex &v1 = vertices[tri.v[1]];
        const Vertex &v2 = vertices[tri.v[2]];

        vec3 dp1 = v1.pos - v0.pos;
        vec3 dp2 = v2.pos - v0.pos;
        float du1 = v1.uv.x - v0.uv.x;
        float dv1 = v1.uv.y - v0.uv.y;
        float du2 = v2.uv.x - v0.uv.x;
        float dv2 = v2.uv.y - v0.uv.y;

        float det = du1 * dv2 - du2 * dv1;
        if (std::abs(det) < 1e-8f)
            continue;

        vec3 T = (dp1 * dv2 - dp2 * dv1) * (1.0f / det);

        vertices[tri.v[0]].tangent += T;
        vertices[tri.v[1]].tangent += T;
        vertices[tri.v[2]].tangent += T;
    }

    // Gram-Schmidt orthonormalize each tangent against its vertex normal.
    // If no UV data produced a tangent, fall back to an arbitrary perpendicular.
    for (auto &v : vertices)
    {
        const vec3 &n = v.normal;
        vec3 &t = v.tangent;

        float len = t.length();
        if (len < 1e-6f)
        {
            // No UV contribution — pick an arbitrary vector perpendicular to n.
            vec3 up = (std::abs(n.z) < 0.9f) ? vec3{0.0f, 0.0f, 1.0f}
                                             : vec3{1.0f, 0.0f, 0.0f};
            t = normalize(cross(n, up));
        }
        else
        {
            // T' = normalize(T - (N·T)*N)
            t = normalize(t - n * dot(n, t));
        }
    }
}

// ─── Mesh::load_model ─────────────────────────────────────────────────────────

bool Mesh::load_model(const std::string &path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return false;

    std::string ext = path.substr(dot + 1);
    // Case-insensitive compare
    for (char &c : ext)
        if (c >= 'A' && c <= 'Z')
            c += 32;

    if (ext == "obj")
        return load_obj(path);
    if (ext == "ply")
        return load_ply(path);

    return false;
}

// ─── Mesh::load_ply ───────────────────────────────────────────────────────────
// Supports ASCII, binary little-endian, and binary big-endian PLY files.
// Reads vertex positions, normals (nx/ny/nz), and UVs (s/t, u/v, texture_u/v).
// Unknown properties are skipped. Only element vertex and element face are used.

bool Mesh::load_ply(const std::string &path)
{
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
        F64
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
        }
        return 4;
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
        return PType::F32;
    };

    // Byte-swap helpers for big-endian binary files.
    auto bs16 = [](uint16_t v) -> uint16_t
    { return (uint16_t)((v >> 8) | (v << 8)); };
    auto bs32 = [](uint32_t v) -> uint32_t
    {
        return ((v >> 24) & 0xFFu) | ((v >> 8) & 0xFF00u) |
               ((v << 8) & 0xFF0000u) | ((v << 24) & 0xFF000000u);
    };
    auto bs64 = [&](uint64_t v) -> uint64_t
    {
        uint32_t lo = bs32((uint32_t)(v & 0xFFFFFFFFu));
        uint32_t hi = bs32((uint32_t)(v >> 32));
        return ((uint64_t)lo << 32) | (uint64_t)hi;
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

    // ── Parse header ──────────────────────────────────────────────────────────

    char line[1024];

    if (!std::fgets(line, sizeof(line), f) || std::strncmp(line, "ply", 3) != 0)
    {
        std::fclose(f);
        return false;
    }

    Fmt fmt = Fmt::ASCII;
    std::vector<Elem> elements;
    int cur = -1; // index of current element being built

    while (std::fgets(line, sizeof(line), f))
    {
        // Strip trailing whitespace/newlines
        int len = (int)std::strlen(line);
        while (len > 0 && (line[len - 1] <= ' '))
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
            cur = (int)elements.size() - 1;
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

            elements[(size_t)cur].props.push_back(prop);
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

    vertices.reserve((size_t)vert_elem->count);
    if (face_elem)
        triangles.reserve((size_t)face_elem->count);

    if (fmt == Fmt::ASCII)
    {
        // ASCII: fscanf skips whitespace and newlines automatically.
        // Store return values to satisfy warn_unused_result on GCC.
        int _r;
        auto sf = [&](float &v)
        { _r = std::fscanf(f, " %f", &v); };
        auto si = [&](int &v)
        { _r = std::fscanf(f, " %d", &v); };
        auto su = [&](unsigned int &v)
        { _r = std::fscanf(f, " %u", &v); };
        (void)_r;

        // Process elements in file order so position matches file layout.
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
                            // Unusual for vertices — skip the list.
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
                        switch (prop.sem)
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
                        // Face index list.
                        int cnt = 0;
                        si(cnt);
                        uint32_t fv[64];
                        int actual = (cnt < 64) ? cnt : 64;
                        for (int j = 0; j < cnt; j++)
                        {
                            unsigned int idx = 0;
                            su(idx);
                            if (j < actual)
                                fv[j] = idx;
                        }
                        // Fan triangulation.
                        for (int j = 1; j + 1 < actual; j++)
                        {
                            Triangle t;
                            t.v[0] = fv[0];
                            t.v[1] = fv[j];
                            t.v[2] = fv[j + 1];
                            t.material_idx = 0;
                            triangles.push_back(t);
                        }
                        break; // only the first list property is face indices
                    }
                }
                else
                {
                    // Skip other elements: read and discard all their properties.
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

        size_t data_len = (size_t)(file_end - data_start);
        std::vector<uint8_t> buf(data_len);
        if (std::fread(buf.data(), 1, data_len, f) != data_len)
        {
            std::fclose(f);
            return false;
        }

        const uint8_t *p = buf.data();
        const uint8_t *end = buf.data() + data_len;
        bool be = (fmt == Fmt::BE);

        // Read a typed scalar from the buffer and advance the pointer.
        auto read_scalar = [&](PType t) -> float
        {
            if (p + ptype_size(t) > end)
                return 0.0f;
            float result = 0.0f;
            switch (t)
            {
            case PType::I8:
            {
                int8_t v;
                std::memcpy(&v, p, 1);
                p += 1;
                result = (float)v;
                break;
            }
            case PType::U8:
            {
                result = (float)*p;
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
                result = (float)v;
                break;
            }
            case PType::U16:
            {
                uint16_t b;
                std::memcpy(&b, p, 2);
                p += 2;
                if (be)
                    b = bs16(b);
                result = (float)b;
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
                result = (float)v;
                break;
            }
            case PType::U32:
            {
                uint32_t b;
                std::memcpy(&b, p, 4);
                p += 4;
                if (be)
                    b = bs32(b);
                result = (float)b;
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
                result = (float)d;
                break;
            }
            }
            return result;
        };

        auto read_int = [&](PType t) -> int
        { return (int)read_scalar(t); };

        for (auto &elem : elements)
        {
            for (int i = 0; i < elem.count; i++)
            {
                if (p >= end)
                    break;

                if (&elem == vert_elem)
                {
                    Vertex v{};
                    for (const auto &prop : elem.props)
                    {
                        if (prop.is_list)
                        {
                            int cnt = read_int(prop.list_count_t);
                            for (int j = 0; j < cnt; j++)
                                read_scalar(prop.list_elem_t);
                            continue;
                        }
                        float val = read_scalar(prop.type);
                        switch (prop.sem)
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
                        int cnt = read_int(prop.list_count_t);
                        uint32_t fv[64];
                        int actual = (cnt < 64) ? cnt : 64;
                        for (int j = 0; j < cnt; j++)
                        {
                            int idx = read_int(prop.list_elem_t);
                            if (j < actual)
                                fv[j] = (uint32_t)idx;
                        }
                        for (int j = 1; j + 1 < actual; j++)
                        {
                            Triangle t;
                            t.v[0] = fv[0];
                            t.v[1] = fv[j];
                            t.v[2] = fv[j + 1];
                            t.material_idx = 0;
                            triangles.push_back(t);
                        }
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
                            int cnt = read_int(prop.list_count_t);
                            p += (size_t)cnt * (size_t)ptype_size(prop.list_elem_t);
                        }
                        else
                        {
                            p += (size_t)ptype_size(prop.type);
                        }
                    }
                }
            }
        }
    }

    std::fclose(f);

    if (vertices.empty() || triangles.empty())
        return false;

    materials.push_back(Material{});

    if (!has_normals)
        compute_normals();
    compute_tangents();

    return true;
}

// ─── Mesh::compute_normals ────────────────────────────────────────────────────

void Mesh::compute_normals()
{
    for (auto &v : vertices)
        v.normal = vec3{};

    // Accumulate face normals. The cross product magnitude is proportional to
    // triangle area, giving area-weighted averaging for free.
    for (const auto &tri : triangles)
    {
        vec3 &p0 = vertices[tri.v[0]].pos;
        vec3 &p1 = vertices[tri.v[1]].pos;
        vec3 &p2 = vertices[tri.v[2]].pos;

        vec3 fn = cross(p1 - p0, p2 - p0);

        vertices[tri.v[0]].normal += fn;
        vertices[tri.v[1]].normal += fn;
        vertices[tri.v[2]].normal += fn;
    }

    for (auto &v : vertices)
    {
        float len = v.normal.length();
        if (len > 1e-6f)
            v.normal = v.normal / len;
    }
}
