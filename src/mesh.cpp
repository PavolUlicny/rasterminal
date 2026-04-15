#include "mesh.h"

#include <cmath>
#include <string>
#include <vector>

// ─── Mesh::load_model ─────────────────────────────────────────────────────────

bool Mesh::load_model(const std::string &path, bool ao)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return false;

    std::string ext = path.substr(dot + 1);
    for (char &c : ext)
        if (c >= 'A' && c <= 'Z')
            c += 32;

    bool ok = false;
    if (ext == "obj")
        ok = load_obj(path);
    else if (ext == "ply")
        ok = load_ply(path);
    else if (ext == "stl")
        ok = load_stl(path);

    if (!ok)
        return false;

    compute_tangents();
    if (ao)
        compute_ao();

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

// ─── Mesh::compute_ao ────────────────────────────────────────────────────────
// Bakes a per-vertex ambient occlusion factor from local surface curvature.
// For each vertex, the centroid of its edge-connected neighbors is computed.
// The vector from the vertex to that centroid, projected onto the vertex normal,
// gives the curvature sign: positive = concave (cavity) → darken; negative =
// convex → keep at 1.  This runs at load time so it costs nothing per frame.

void Mesh::compute_ao()
{
    const size_t n = vertices.size();

    // Build edge-adjacency: for each vertex, collect all vertices it shares a
    // triangle edge with (duplicates are harmless — they just weight denser areas).
    std::vector<std::vector<uint32_t>> adj(n);
    for (const auto &tri : triangles)
    {
        for (int i = 0; i < 3; i++)
        {
            uint32_t a = tri.v[i];
            uint32_t b = tri.v[(i + 1) % 3];
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
    }

    for (size_t i = 0; i < n; i++)
    {
        if (adj[i].empty())
        {
            vertices[i].ao = 1.0f;
            continue;
        }

        const vec3 &p = vertices[i].pos;
        const vec3 &N = vertices[i].normal;

        // Centroid of neighboring positions.
        vec3 centroid{};
        for (uint32_t j : adj[i])
            centroid += vertices[j].pos;
        centroid = centroid * (1.0f / (float)adj[i].size());

        vec3 d = centroid - p;
        float len = d.length();
        if (len < 1e-8f)
        {
            vertices[i].ao = 1.0f;
            continue;
        }

        // Positive curvature = concave = cavity → reduce AO.
        // Clamp so convex surfaces stay at 1 and deep cavities don't go fully black.
        float curvature = dot(d * (1.0f / len), N);
        vertices[i].ao = 1.0f - clamp(curvature * 0.5f, 0.0f, 0.15f);
    }
}
