#include "mesh.h"
#include "light.h"
#include "linalg.h"
#include "meshoptimizer.h"

#include <algorithm>
#include <atomic>
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
    has_metallic = false;
    has_emissive = false;
    has_normal_scale = false;
}

bool Mesh::load_model(const std::string &path, bool ao, int n_threads)
{
    clear();

    const size_t ext_pos = path.find_last_of('.');
    if (ext_pos == std::string::npos)
    {
        return false;
    }

    std::string ext = path.substr(ext_pos + 1);
    for (char &c : ext)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c += 32;
        }
    }

    bool ok = false;
    if (ext == "obj")
    {
        ok = load_obj(path, n_threads);
    }
    else if (ext == "ply")
    {
        ok = load_ply(path);
    }
    else if (ext == "stl")
    {
        ok = load_stl(path);
    }
    else if (ext == "gltf" || ext == "glb")
    {
        ok = load_gltf(path, n_threads);
    }

    if (!ok)
    {
        clear();
        return false;
    }

    has_double_sided =
        std::any_of(materials.begin(), materials.end(), [](const Material &m) { return m.double_sided; });
    has_metallic = std::any_of(materials.begin(), materials.end(), [](const Material &m) { return m.metallic > 0.0f; });
    has_normal_scale = std::any_of(
        materials.begin(), materials.end(),
        [](const Material &m) { return m.normal_tex >= 0 && m.normal_scale != 1.0f; }
    );

    // Industry-convention promotion: when an emissive texture is bound but the factor is at
    // the spec default {0,0,0}, promote to {1,1,1} so the texture actually glows (matches
    // Unreal, Sketchfab, MSFS, Blender's PBR importer). Run AFTER the loader's decode pass
    // so emissive_tex reflects successful decodes — a failed decode (remapped to -1) must
    // NOT trigger promotion, otherwise the material would render uniform white.
    for (Material &m : materials)
    {
        if (m.emissive_tex >= 0 && m.emissive.x == 0.0f && m.emissive.y == 0.0f && m.emissive.z == 0.0f)
        {
            m.emissive = { 1.0f, 1.0f, 1.0f };
            m.emissive_was_promoted = true;
        }
    }

    has_emissive = std::any_of(
        materials.begin(), materials.end(), [](const Material &m)
        { return m.emissive_tex >= 0 || m.emissive.x > 0.0f || m.emissive.y > 0.0f || m.emissive.z > 0.0f; }
    );

    compute_tangents();
    if (ao && ext != "stl")
    {
        compute_ao(n_threads);
    }
    optimize_vertex_cache(n_threads);

    return true;
}

// ─── Mesh::compute_normals ────────────────────────────────────────────────────

void Mesh::compute_normals()
{
    for (auto &v : vertices)
    {
        v.normal = vec3{};
    }

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
        {
            v.normal = v.normal * (1.0f / std::sqrt(len_sq));
        }
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

        const float det = (du1 * dv2) - (du2 * dv1);
        if (std::abs(det) < 1e-8f)
        {
            continue;
        }

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
    {
        for (int i = 0; i < 3; i++)
        {
            adj_count[tri.v[i]]++;
            adj_count[tri.v[(i + 1) % 3]]++;
        }
    }

    std::vector<int> adj_start(n + 1, 0);
    for (size_t v = 0; v < n; v++)
    {
        adj_start[v + 1] = adj_start[v] + adj_count[v];
    }

    std::vector<uint32_t> adj_list(static_cast<size_t>(adj_start[n]));
    {
        std::vector<int> cursor(adj_start.begin(), adj_start.begin() + static_cast<std::ptrdiff_t>(n));
        for (const auto &tri : triangles)
        {
            for (int i = 0; i < 3; i++)
            {
                const uint32_t a = tri.v[i];
                const uint32_t b = tri.v[(i + 1) % 3];
                adj_list[static_cast<size_t>(cursor[a]++)] = b;
                adj_list[static_cast<size_t>(cursor[b]++)] = a;
            }
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
            {
                centroid += vertices[adj_list[static_cast<size_t>(ai)]].pos;
            }
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
        {
            t.join();
        }
    }
}

// ─── Mesh::optimize_vertex_cache ─────────────────────────────────────────────

void Mesh::optimize_vertex_cache(int n_threads)
{
    if (triangles.size() < 2)
    {
        return;
    }

    const size_t nv = vertices.size();
    const size_t nt = triangles.size();
    const size_t ni = nt * 3;

    // meshopt's vertex-cache and overdraw passes reorder triangles within the
    // index range we hand them but expose no permutation, so per-triangle
    // metadata (here material_idx) gets stranded. Group triangles by material
    // so each occupies a contiguous range, then call meshopt per range —
    // reordering within a single-material range is safe. Single-material
    // meshes skip grouping entirely. Loaders never write material_idx >=
    // materials.size(), so that's a safe bucket count.
    const bool multi_material = materials.size() > 1;
    const size_t n_buckets = std::max<size_t>(materials.size(), 1);
    std::vector<uint32_t> bucket_start;

    if (multi_material)
    {
        bucket_start.assign(n_buckets + 1, 0);
        bool already_sorted = true;
        uint32_t prev_mat = 0;
        for (const auto &t : triangles)
        {
            bucket_start[t.material_idx + 1]++;
            already_sorted &= (t.material_idx >= prev_mat);
            prev_mat = t.material_idx;
        }
        for (size_t i = 1; i <= n_buckets; i++)
        {
            bucket_start[i] += bucket_start[i - 1];
        }

        // glTF arrives in material order; OBJ may interleave. Skip the scatter
        // when triangles are already grouped.
        if (!already_sorted)
        {
            std::vector<Triangle> sorted(nt);
            std::vector<uint32_t> cursor(
                bucket_start.begin(), bucket_start.begin() + static_cast<std::ptrdiff_t>(n_buckets)
            );
            for (const auto &t : triangles)
            {
                sorted[cursor[t.material_idx]++] = t;
            }
            triangles = std::move(sorted);
        }
    }

    std::vector<uint32_t> idx(ni);
    for (size_t i = 0; i < nt; i++)
    {
        for (size_t j = 0; j < 3; j++)
        {
            idx[(i * 3) + j] = triangles[i].v[j];
        }
    }

    // 1.05 threshold: accept up to 5% vertex-cache regression for better
    // occlusion ordering.
    const auto optimize_range = [&](uint32_t *buf, size_t count)
    {
        meshopt_optimizeVertexCache(buf, buf, count, nv);
        meshopt_optimizeOverdraw(buf, buf, count, &vertices[0].pos.x, nv, sizeof(Vertex), 1.05f);
    };

    if (multi_material)
    {
        // Per-group calls touch disjoint idx slices and read vertices[] only,
        // so they parallelize cleanly — claws back the per-call allocator
        // overhead meshopt pays on every entry.
        const auto run_group = [&](size_t m)
        {
            const size_t g_start = bucket_start[m];
            const size_t g_end = bucket_start[m + 1];
            if (g_end - g_start < 2)
            {
                return;
            }
            optimize_range(idx.data() + (g_start * 3), (g_end - g_start) * 3);
        };

        if (n_threads <= 1)
        {
            for (size_t m = 0; m < n_buckets; m++)
            {
                run_group(m);
            }
        }
        else
        {
            std::atomic<size_t> next{ 0 };
            const auto worker = [&]()
            {
                for (size_t m = next.fetch_add(1, std::memory_order_relaxed); m < n_buckets;
                     m = next.fetch_add(1, std::memory_order_relaxed))
                {
                    run_group(m);
                }
            };
            // Main thread runs worker() too, so spawn one fewer; cap at n_buckets
            // since surplus workers would just hit the fetch_add guard and exit.
            const size_t spawn = std::min<size_t>(static_cast<size_t>(n_threads), n_buckets) - 1;
            std::vector<std::thread> threads;
            threads.reserve(spawn);
            for (size_t t = 0; t < spawn; t++)
            {
                threads.emplace_back(worker);
            }
            worker();
            for (auto &thr : threads)
            {
                thr.join();
            }
        }
    }
    else
    {
        optimize_range(idx.data(), ni);
    }

    std::vector<uint32_t> remap(nv);
    const size_t new_nv = meshopt_optimizeVertexFetchRemap(remap.data(), idx.data(), ni, nv);

    // Some glTF primitives have no COLOR_0, leaving vertex_colors shorter than nv.
    if (has_vertex_colors && vertex_colors.size() < nv)
    {
        vertex_colors.resize(nv, { 1.0f, 1.0f, 1.0f });
    }

    std::vector<Vertex> new_verts(new_nv);
    std::vector<vec3> new_tans(new_nv);
    std::vector<vec3> new_vcols;
    if (has_vertex_colors)
    {
        new_vcols.resize(new_nv);
    }

    for (size_t v = 0; v < nv; v++)
    {
        if (remap[v] == ~0u)
        {
            continue;
        }
        new_verts[remap[v]] = vertices[v];
        new_tans[remap[v]] = tangents[v];
        if (has_vertex_colors)
        {
            new_vcols[remap[v]] = vertex_colors[v];
        }
    }

    for (size_t i = 0; i < nt; i++)
    {
        for (size_t j = 0; j < 3; j++)
        {
            triangles[i].v[j] = remap[idx[(i * 3) + j]];
        }
    }

    vertices = std::move(new_verts);
    tangents = std::move(new_tans);
    if (has_vertex_colors)
    {
        vertex_colors = std::move(new_vcols);
    }
}
