#include "mesh.h"
#include "light.h"
#include "linalg.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// ─── Mesh::load_model ─────────────────────────────────────────────────────────

void Mesh::clear()
{
    vertices.clear();
    triangles.clear();
    materials.clear();
    textures.clear();
    tangents.clear();
    vertex_colors.clear();
    has_vertex_colors = false;
    has_double_sided = false;
}

bool Mesh::load_model(const std::string &path, bool ao, int n_threads)
{
    clear();

    const size_t ext_pos = path.find_last_of('.');
    if (ext_pos == std::string::npos)
        return false;

    std::string ext = path.substr(ext_pos + 1);
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
    else if (ext == "gltf" || ext == "glb")
        ok = load_gltf(path);

    if (!ok)
    {
        clear();
        return false;
    }

    has_double_sided =
        std::any_of(materials.begin(), materials.end(), [](const Material &m) { return m.double_sided; });

    compute_tangents();
    if (ao && ext != "stl")
        compute_ao(n_threads);
    optimize_vertex_cache();

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
        const vec3 &p0 = vertices[tri.v[0]].pos;
        const vec3 &p1 = vertices[tri.v[1]].pos;
        const vec3 &p2 = vertices[tri.v[2]].pos;

        const vec3 face_normal = cross(p1 - p0, p2 - p0);

        vertices[tri.v[0]].normal += face_normal;
        vertices[tri.v[1]].normal += face_normal;
        vertices[tri.v[2]].normal += face_normal;
    }

    for (auto &v : vertices)
    {
        const float len_sq = v.normal.length_sq();
        if (len_sq > 1e-12f)
            v.normal = v.normal * (1.0f / std::sqrt(len_sq));
    }
}

// ─── Mesh::compute_tangents ───────────────────────────────────────────────────

void Mesh::compute_tangents()
{
    tangents.assign(vertices.size(), vec3{});

    // Accumulate tangent vectors from each triangle's UV layout.
    // For triangle (P0,P1,P2) with UVs (u0,v0),(u1,v1),(u2,v2):
    //   T = (dP1*dv2 - dP2*dv1) / (du1*dv2 - du2*dv1)
    for (const auto &tri : triangles)
    {
        const Vertex &v0 = vertices[tri.v[0]];
        const Vertex &v1 = vertices[tri.v[1]];
        const Vertex &v2 = vertices[tri.v[2]];

        const vec3 dp1 = v1.pos - v0.pos;
        const vec3 dp2 = v2.pos - v0.pos;
        const float du1 = v1.uv.x - v0.uv.x;
        const float dv1 = v1.uv.y - v0.uv.y;
        const float du2 = v2.uv.x - v0.uv.x;
        const float dv2 = v2.uv.y - v0.uv.y;

        const float det = du1 * dv2 - du2 * dv1;
        if (std::abs(det) < 1e-8f)
            continue;

        const vec3 T = (dp1 * dv2 - dp2 * dv1) * (1.0f / det);

        tangents[tri.v[0]] += T;
        tangents[tri.v[1]] += T;
        tangents[tri.v[2]] += T;
    }

    // Gram-Schmidt orthonormalize each tangent against its vertex normal.
    // If no UV data produced a tangent, fall back to an arbitrary perpendicular.
    for (size_t i = 0; i < vertices.size(); i++)
    {
        const vec3 &n = vertices[i].normal;
        vec3 &t = tangents[i];

        if (t.length_sq() < 1e-12f)
        {
            // No UV contribution — pick an arbitrary vector perpendicular to n.
            const vec3 up = (std::abs(n.z) < 0.9f) ? vec3{ 0.0f, 0.0f, 1.0f } : vec3{ 1.0f, 0.0f, 0.0f };
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

void Mesh::compute_ao(int n_threads)
{
    const size_t n = vertices.size();

    // Build CSR edge-adjacency: for each vertex, collect all vertices it shares a
    // triangle edge with (duplicates are harmless — they just weight denser areas).
    // CSR avoids N separate heap allocations and keeps neighbor indices contiguous.
    std::vector<int> adj_count(n, 0);
    for (const auto &tri : triangles)
        for (int i = 0; i < 3; i++)
        {
            adj_count[tri.v[i]]++;
            adj_count[tri.v[(i + 1) % 3]]++;
        }

    std::vector<int> adj_start(n + 1, 0);
    for (size_t v = 0; v < n; v++)
        adj_start[v + 1] = adj_start[v] + adj_count[v];

    std::vector<uint32_t> adj_list(static_cast<size_t>(adj_start[n]));
    {
        std::vector<int> cursor(adj_start.begin(), adj_start.begin() + static_cast<std::ptrdiff_t>(n));
        for (const auto &tri : triangles)
            for (int i = 0; i < 3; i++)
            {
                const uint32_t a = tri.v[i];
                const uint32_t b = tri.v[(i + 1) % 3];
                adj_list[static_cast<size_t>(cursor[a]++)] = b;
                adj_list[static_cast<size_t>(cursor[b]++)] = a;
            }
    }

    // Per-vertex AO is independent — each vertex only writes to its own ao field.
    constexpr size_t AO_PARALLEL_THRESHOLD = 1024;
    const int eff_threads = (n_threads <= 1 || n < AO_PARALLEL_THRESHOLD)
                                ? 1
                                : static_cast<int>(std::min(n, static_cast<size_t>(n_threads)));

    auto ao_range = [&](size_t begin, size_t end)
    {
        for (size_t i = begin; i < end; i++)
        {
            const int deg = adj_start[i + 1] - adj_start[i];
            if (deg == 0)
            {
                vertices[i].ao = 1.0f;
                continue;
            }

            const vec3 &p = vertices[i].pos;
            const vec3 &N = vertices[i].normal;

            // Centroid of neighboring positions.
            vec3 centroid{};
            for (int ai = adj_start[i]; ai < adj_start[i + 1]; ai++)
                centroid += vertices[adj_list[static_cast<size_t>(ai)]].pos;
            centroid = centroid * (1.0f / static_cast<float>(deg));

            const vec3 d = centroid - p;
            const float len_sq = d.length_sq();
            if (len_sq < 1e-16f)
            {
                vertices[i].ao = 1.0f;
                continue;
            }

            // Positive curvature = concave = cavity → reduce AO.
            // Clamp so convex surfaces stay at 1 and deep cavities don't go fully black.
            const float curvature = dot(d * (1.0f / std::sqrt(len_sq)), N);
            vertices[i].ao = 1.0f - clamp(curvature * 0.5f, 0.0f, 0.15f);
        }
    };

    if (eff_threads <= 1)
    {
        ao_range(0, n);
    }
    else
    {
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(eff_threads - 1));
        for (int t = 0; t < eff_threads - 1; t++)
        {
            const size_t begin = static_cast<size_t>(t) * n / static_cast<size_t>(eff_threads);
            const size_t end = static_cast<size_t>(t + 1) * n / static_cast<size_t>(eff_threads);
            threads.emplace_back(ao_range, begin, end);
        }
        ao_range(static_cast<size_t>(eff_threads - 1) * n / static_cast<size_t>(eff_threads), n);
        for (auto &t : threads)
            t.join();
    }
}

// ─── Mesh::optimize_vertex_cache ─────────────────────────────────────────────
// Reorders triangles (Forsyth) then remaps vertex arrays to first-use order
// to maximise hardware vertex cache reuse. Output is bit-identical.

namespace
{
    constexpr int VCACHE_SIZE = 32;
    constexpr float LAST_TRI_SCORE = 0.75f;
    constexpr float VALENCE_BOOST_SCALE = 2.0f;

    float forsyth_pos_score(int pos) noexcept
    {
        if (pos < 3)
            return LAST_TRI_SCORE;
        const float t = 1.0f - static_cast<float>(pos - 3) / static_cast<float>(VCACHE_SIZE - 3);
        return t * std::sqrt(t); // pow(t, 1.5) closed form
    }

    float forsyth_val_score(int remaining) noexcept
    {
        return remaining > 0
                   ? VALENCE_BOOST_SCALE / std::sqrt(static_cast<float>(remaining)) // pow(r, -0.5) closed form
                   : 0.0f;
    }

    // Core Tom Forsyth triangle reorder. `in` holds the triangles to sort;
    // `adj_count[v]` is the number of triangles in `in` that reference vertex v
    // (pre-computed by the caller so it is not recomputed here).
    // Returns the reordered triangle list.
    std::vector<Triangle> forsyth_core(std::vector<Triangle> in, std::vector<int> adj_count)
    {
        if (in.empty())
            return in;

        const size_t nt = in.size();
        const size_t nv = adj_count.size();

        std::vector<int> adj_start(nv + 1, 0);
        for (size_t v = 0; v < nv; v++)
            adj_start[v + 1] = adj_start[v] + adj_count[v];

        std::vector<int> adj_list(static_cast<size_t>(adj_start[nv]));
        {
            std::vector<int> cursor(adj_start.begin(), adj_start.begin() + static_cast<std::ptrdiff_t>(nv));
            for (int ti = 0; ti < static_cast<int>(nt); ti++)
                for (const uint32_t vi : in[static_cast<size_t>(ti)].v)
                    adj_list[static_cast<size_t>(cursor[vi]++)] = ti;
        }

        std::vector<int> remaining = std::move(adj_count); // triangles not yet emitted per vertex
        std::vector<float> vscore(nv);
        for (size_t v = 0; v < nv; v++)
            vscore[v] = forsyth_val_score(remaining[v]);

        std::vector<float> tscore(nt);
        std::vector<uint8_t> tdone(nt, 0); // uint8_t avoids std::vector<bool> bit-proxy overhead
        for (size_t ti = 0; ti < nt; ti++)
            tscore[ti] = vscore[in[ti].v[0]] + vscore[in[ti].v[1]] + vscore[in[ti].v[2]];

        std::vector<int> sim_cache(VCACHE_SIZE, -1);

        std::vector<Triangle> out;
        out.reserve(nt);

        int best = -1;
        while (out.size() < nt)
        {
            // Fall back to linear scan when the cache yielded no candidate.
            if (best < 0)
            {
                float bs = -1.0f;
                for (size_t ti = 0; ti < nt; ti++)
                {
                    if (!tdone[ti] && tscore[ti] > bs)
                    {
                        bs = tscore[ti];
                        best = static_cast<int>(ti);
                    }
                }
            }

            tdone[static_cast<size_t>(best)] = 1;
            out.push_back(in[static_cast<size_t>(best)]);

            for (const uint32_t vraw : in[static_cast<size_t>(best)].v)
            {
                const int v = static_cast<int>(vraw);
                remaining[static_cast<size_t>(v)]--;

                int cur = -1;
                for (int k = 0; k < VCACHE_SIZE; k++)
                    if (sim_cache[static_cast<size_t>(k)] == v)
                    {
                        cur = k;
                        break;
                    }

                // Shift entries to open slot 0. If v wasn't cached, the last entry
                // is silently evicted (it will not appear in the re-derive below).
                const int shift_from = (cur >= 0) ? cur : VCACHE_SIZE - 1;
                for (int k = shift_from; k > 0; k--)
                    sim_cache[static_cast<size_t>(k)] = sim_cache[static_cast<size_t>(k - 1)];
                sim_cache[0] = v;
            }

            best = -1;
            float bs = -1.0f;
            for (int k = 0; k < VCACHE_SIZE; k++)
            {
                const int v = sim_cache[static_cast<size_t>(k)];
                if (v < 0)
                    break;
                vscore[static_cast<size_t>(v)] =
                    forsyth_pos_score(k) + forsyth_val_score(remaining[static_cast<size_t>(v)]);
                for (int ai = adj_start[static_cast<size_t>(v)]; ai < adj_start[static_cast<size_t>(v) + 1]; ai++)
                {
                    const int ti = adj_list[static_cast<size_t>(ai)];
                    if (!tdone[static_cast<size_t>(ti)])
                    {
                        const auto sti = static_cast<size_t>(ti);
                        tscore[sti] = vscore[in[sti].v[0]] + vscore[in[sti].v[1]] + vscore[in[sti].v[2]];
                        if (tscore[sti] > bs)
                        {
                            bs = tscore[sti];
                            best = ti;
                        }
                    }
                }
            }
        }

        return out;
    }
} // namespace

void Mesh::optimize_vertex_cache()
{
    if (triangles.size() < 2)
        return;

    const size_t nv = vertices.size();
    const size_t nt = triangles.size();

    // Fully unshared meshes (STL, per-face-color PLY) have nv == nt*3.
    // Forsyth degenerates to O(n²) there — bail out before any allocation.
    if (nv == nt * 3)
        return;

    // Compute adj_count once — used for the max_adj guard and passed into
    // forsyth_core so the O(nt) count is not repeated inside it.
    std::vector<int> adj_count(nv, 0);
    for (const auto &tri : triangles)
        for (const uint32_t vi : tri.v)
            adj_count[vi]++;
    int max_adj = 0;
    for (const int c : adj_count)
        if (c > max_adj)
            max_adj = c;
    if (max_adj < 2)
        return;

    // ── Pass 1: Forsyth triangle reorder ────────────────────────────────────

    triangles = forsyth_core(std::move(triangles), std::move(adj_count));

    // ── Pass 2: vertex fetch remap ───────────────────────────────────────────
    // Rebuild vertex arrays in first-use order so sequential triangle access
    // produces sequential vertex reads.

    std::vector<int> remap(nv, -1);
    std::vector<Vertex> new_verts;
    std::vector<vec3> new_tans;
    std::vector<vec3> new_vcols;
    new_verts.reserve(nv);
    new_tans.reserve(nv);
    if (has_vertex_colors)
        new_vcols.reserve(nv);

    int next = 0;
    for (auto &tri : triangles)
    {
        for (uint32_t &vi : tri.v)
        {
            const size_t old_v = vi;
            if (remap[old_v] < 0)
            {
                remap[old_v] = next++;
                new_verts.push_back(vertices[old_v]);
                new_tans.push_back(tangents[old_v]);
                if (has_vertex_colors)
                    new_vcols.push_back(vertex_colors[old_v]);
            }
            vi = static_cast<uint32_t>(remap[old_v]);
        }
    }

    vertices = std::move(new_verts);
    tangents = std::move(new_tans);
    if (has_vertex_colors)
        vertex_colors = std::move(new_vcols);
}
