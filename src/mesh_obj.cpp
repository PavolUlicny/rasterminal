#include "mesh.h"
#include "texture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
                    materials[(size_t)current].diffuse = {r, g, b};
            }
            else if (std::strncmp(p, "Ks", 2) == 0 && (p[2] == ' ' || p[2] == '\t'))
            {
                float r, g, b;
                if (std::sscanf(p + 3, "%f %f %f", &r, &g, &b) == 3)
                    materials[(size_t)current].specular = {r, g, b};
            }
            else if (std::strncmp(p, "Ns", 2) == 0 && (p[2] == ' ' || p[2] == '\t'))
            {
                float ns;
                if (std::sscanf(p + 3, "%f", &ns) == 1)
                    materials[(size_t)current].shininess = ns;
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
                    if (tex.load(mtl_dir + tex_name))
                    {
                        materials[(size_t)current].diffuse_tex = (int)textures.size();
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
                    if (tex.load(mtl_dir + tex_name))
                    {
                        materials[(size_t)current].normal_tex = (int)textures.size();
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
    bool all_have_normals = true; // false if any face vertex has no normal reference
    char line[512];

    // Returns the index of an existing or newly created Vertex.
    // Returns UINT32_MAX if the position index is out of range (invalid OBJ reference).
    auto get_vertex = [&](FaceVertex fv) -> uint32_t
    {
        auto it = vertex_map.find(fv);
        if (it != vertex_map.end())
            return it->second;

        // Position is mandatory; reject the vertex if the index is invalid.
        if (fv.pi < 0 || fv.pi >= (int)pos_pool.size())
            return UINT32_MAX;

        Vertex v;
        v.pos = pos_pool[(size_t)fv.pi];
        if (fv.ni >= 0 && fv.ni < (int)norm_pool.size())
            v.normal = norm_pool[(size_t)fv.ni];
        else
        {
            v.normal = vec3{};
            all_have_normals = false; // this vertex has no file-provided normal
        }
        v.uv = (fv.ti >= 0 && fv.ti < (int)uv_pool.size()) ? uv_pool[(size_t)fv.ti] : vec2{};

        uint32_t idx = (uint32_t)vertices.size();
        vertices.push_back(v);
        vertex_map[fv] = idx;
        return idx;
    };

    while (std::fgets(line, sizeof(line), f))
    {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++; // OBJ allows leading whitespace

        if (p[0] == 'v' && p[1] == ' ')
        {
            vec3 v{};
            std::sscanf(p + 2, "%f %f %f", &v.x, &v.y, &v.z);
            pos_pool.push_back(v);
        }
        else if (p[0] == 'v' && p[1] == 'n')
        {
            vec3 n{};
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
            // Merge rather than replace so multiple mtllib blocks accumulate.
            auto new_map = load_mtl(obj_dir + mtl_name, materials, textures, obj_dir);
            for (auto &kv : new_map)
                mat_map[kv.first] = kv.second;
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
            if (count < 3)
                continue;
            uint32_t v0 = get_vertex(fverts[0]);
            if (v0 == UINT32_MAX)
                continue;
            for (int i = 1; i + 1 < count; i++)
            {
                uint32_t v1 = get_vertex(fverts[i]);
                uint32_t v2 = get_vertex(fverts[i + 1]);
                if (v1 == UINT32_MAX || v2 == UINT32_MAX)
                    continue;
                Triangle t;
                t.v[0] = v0;
                t.v[1] = v1;
                t.v[2] = v2;
                t.material_idx = current_mat;
                triangles.push_back(t);
            }
        }
    }

    std::fclose(f);

    if (triangles.empty())
    {
        rollback();
        return false;
    }

    // Recompute if no normals were in the file, or if some face vertices
    // had no normal reference (mixed file) — zero normals light incorrectly.
    if (!has_normals || !all_have_normals)
        compute_normals();

    return true;
}
