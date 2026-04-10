#include "mesh.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

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

// ─── Mesh::load_obj ───────────────────────────────────────────────────────────

bool Mesh::load_obj(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f)
        return false;

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
                triangles.push_back(t);
            }
        }
    }

    std::fclose(f);

    if (!has_normals)
        compute_normals();

    return !triangles.empty();
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
