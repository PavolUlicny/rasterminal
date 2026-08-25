#include "src/loaders/mesh.h"
#include "src/math/light.h"
#include "src/math/linalg.h"
#include "meshoptimizer.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <utility>
#include <vector>

void Mesh::clear()
{
    vertices.clear();
    triangles.clear();
    materials.clear();
    textures.clear();
    tangents.clear();
    vertex_colors.clear();
    vertex_alpha.clear();
    uv1.clear();
    has_vertex_colors = false;
    has_vertex_alpha = false;
    has_uv1 = false;
    has_transparent = false;
    opaque_count = 0;
    has_double_sided = false;
    has_metallic = false;
    has_emissive = false;
    has_normal_scale = false;
    has_occlusion = false;
    has_unlit = false;
}

bool Mesh::load_model(const std::string &path, bool ao, int n_threads, float crease_angle_deg)
{
    clear();

    const float crease_cos = std::cos(to_radians(crease_angle_deg));

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

    // Convert allocation, thread and post-processing exceptions into a clean load failure.
    try
    {
        bool ok = false;
        if (ext == "obj")
        {
            ok = load_obj(path, n_threads, crease_cos);
        }
        else if (ext == "ply")
        {
            ok = load_ply(path, crease_cos);
        }
        else if (ext == "stl")
        {
            ok = load_stl(path, crease_cos);
        }
        else if (ext == "gltf" || ext == "glb")
        {
            ok = load_gltf(path, n_threads, crease_cos);
        }
        else
        {
            ok = load_assimp(path, n_threads, crease_angle_deg);
        }

        if (!ok)
        {
            clear();
            return false;
        }

        // Non-finite positions poison camera fitting; non-finite UV-to-int conversion is undefined
        // and can create out-of-bounds texel offsets. Check once here instead of per sample.
        if (std::any_of(
                vertices.begin(), vertices.end(),
                [](const Vertex &v)
                {
                    return !std::isfinite(v.pos.x) || !std::isfinite(v.pos.y) || !std::isfinite(v.pos.z) ||
                           !std::isfinite(v.uv.x) || !std::isfinite(v.uv.y);
                }
            ))
        {
            clear();
            return false;
        }
        // The parallel second UV set reaches the same sampler.
        if (std::any_of(
                uv1.begin(), uv1.end(), [](const vec2 &t) { return !std::isfinite(t.x) || !std::isfinite(t.y); }
            ))
        {
            clear();
            return false;
        }

        has_double_sided =
            std::any_of(materials.begin(), materials.end(), [](const Material &m) { return m.double_sided; });
        has_metallic =
            std::any_of(materials.begin(), materials.end(), [](const Material &m) { return m.metallic > 0.0f; });
        has_normal_scale = std::any_of(
            materials.begin(), materials.end(),
            [](const Material &m) { return m.normal_map.tex >= 0 && m.normal_scale != 1.0f; }
        );
        has_occlusion =
            std::any_of(materials.begin(), materials.end(), [](const Material &m) { return m.occlusion_map.tex >= 0; });
        has_unlit = std::any_of(materials.begin(), materials.end(), [](const Material &m) { return m.unlit; });
        has_transparent = std::any_of(materials.begin(), materials.end(), [](const Material &m) { return m.blend; });

        // A zero emissive factor nullifies any emissive texture.
        has_emissive = std::any_of(
            materials.begin(), materials.end(),
            [](const Material &m) { return m.emissive.x > 0.0f || m.emissive.y > 0.0f || m.emissive.z > 0.0f; }
        );

        // Drop all-opaque vertex alpha so later remaps and fragments do not multiply by one.
        if (has_vertex_alpha &&
            std::none_of(vertex_alpha.begin(), vertex_alpha.end(), [](float a) { return a < 1.0f; }))
        {
            vertex_alpha.clear();
            has_vertex_alpha = false;
        }

        // Translucent vertex colors require the transparent path even without a blend material.
        has_transparent = has_transparent || has_vertex_alpha;

        compute_tangents();
        if (ao)
        {
            compute_ao(n_threads);
        }

        optimize_vertex_cache(n_threads);

        // Partition by triangle, not material, so opaque regions retain the fast pass.
        // stable_partition preserves meshoptimizer's within-group order.
        opaque_count = static_cast<uint32_t>(triangles.size());
        if (has_transparent)
        {
            const float *va = has_vertex_alpha ? vertex_alpha.data() : nullptr;
            const auto is_opaque_tri = [&](const Triangle &t)
            {
                if (mat_at(t.material_idx).blend)
                {
                    return false;
                }
                if (va && (va[t.v[0]] < 1.0f || va[t.v[1]] < 1.0f || va[t.v[2]] < 1.0f))
                {
                    return false;
                }
                return true;
            };
            const auto mid = std::stable_partition(triangles.begin(), triangles.end(), is_opaque_tri);
            opaque_count = static_cast<uint32_t>(mid - triangles.begin());
        }

        return true;
    }
    catch (const std::exception &e)
    {
        // Preserve the exception detail; main prints the common load-failure summary.
        std::fprintf(stderr, "note: loading '%s' threw an exception: %s\n", path.c_str(), e.what());
        clear();
        return false;
    }
}

void Mesh::compute_normals(
    float crease_cos, const std::vector<uint32_t> *weld, size_t n_groups, const std::vector<uint32_t> *smooth_groups
)
{
    const size_t n_verts = vertices.size();
    const size_t n_tris = triangles.size();
    if (n_verts == 0 || n_tris == 0)
    {
        return;
    }

    // Parallel arrays must match the geometry they describe.
    assert(smooth_groups == nullptr || smooth_groups->size() == n_tris);

    assert(weld == nullptr || weld->size() == n_verts);

    // Fold OBJ seam vertices into welded adjacency while keeping separate output vertices.
    // Copy manually to avoid a GCC LTO false positive inside std::copy.
    std::vector<uint32_t> group_of;
    if (weld != nullptr)
    {
        group_of.reserve(weld->size());
        for (const uint32_t g : *weld)
        {
            group_of.push_back(g);
        }
    }
    else
    {
        n_groups = n_verts;
    }
    const bool has_weld = (weld != nullptr);
    auto grp = [has_weld, &group_of](uint32_t v) -> uint32_t { return has_weld ? group_of[v] : v; };

    // Nonzero smoothing groups override the crease angle.
    const bool has_groups = (smooth_groups != nullptr);

    // Raw cross products provide area weighting; zero reciprocal marks degenerates.
    std::vector<vec3> face_n(n_tris);
    std::vector<float> face_inv_len(n_tris);
    for (size_t t = 0; t < n_tris; t++)
    {
        const Triangle &tri = triangles[t];
        const vec3 fn =
            cross(vertices[tri.v[1]].pos - vertices[tri.v[0]].pos, vertices[tri.v[2]].pos - vertices[tri.v[0]].pos);
        face_n[t] = fn;
        const float len_sq = fn.length_sq();
        face_inv_len[t] = (len_sq > 1e-24f) ? (1.0f / std::sqrt(len_sq)) : 0.0f;
    }

    // CSR adjacency: position group -> incident corners. Each triangle adds 3.
    std::vector<uint32_t> offsets(n_groups + 1, 0);
    for (size_t t = 0; t < n_tris; t++)
    {
        for (const uint32_t vi : triangles[t].v)
        {
            offsets[grp(vi) + 1]++;
        }
    }
    for (size_t i = 0; i < n_groups; i++)
    {
        offsets[i + 1] += offsets[i];
    }
    const size_t n_corners = offsets[n_groups]; // == 3 * n_tris
    std::vector<uint32_t> corner_tri(n_corners);
    std::vector<uint8_t> corner_c(n_corners);
    {
        std::vector<uint32_t> cursor(offsets.begin(), offsets.end() - 1);
        for (size_t t = 0; t < n_tris; t++)
        {
            for (uint8_t c = 0; c < 3; c++)
            {
                const uint32_t slot = cursor[grp(triangles[t].v[c])]++;
                corner_tri[slot] = static_cast<uint32_t>(t);
                corner_c[slot] = c;
            }
        }
    }

    // Union incident faces into smooth wedges.
    std::vector<uint32_t> parent; // union-find over local incident indices
    std::vector<uint32_t> roots;  // find(k) cached after Pass A so Pass B reuses
    // Sorting endpoint/corner pairs groups corners that share an edge.
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    std::vector<vec3> wedge_n; // local root -> summed (area-weighted) normal
    // Map each original vertex and wedge root to one output vertex. Groups are usually tiny.
    struct OutSlot
    {
        uint32_t ov;
        uint32_t root;
        uint32_t out;
    };
    std::vector<OutSlot> out_map;
    auto find = [&parent](uint32_t x) -> uint32_t
    {
        while (parent[x] != x)
        {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };

    // Propagate only length-matched parallel attributes.
    const bool has_vcol = (vertex_colors.size() == n_verts);
    const bool has_valpha = (vertex_alpha.size() == n_verts);
    const bool has_uv1_arr = (uv1.size() == n_verts);

    for (size_t g = 0; g < n_groups; g++)
    {
        const uint32_t start = offsets[g];
        const uint32_t deg = offsets[g + 1] - start;
        if (deg == 0)
        {
            continue; // group has no corners (unreferenced position)
        }

        parent.resize(deg);
        // Emit each non-degenerate corner's two edge-endpoints (in welded space).
        // Degenerate faces are omitted so they never union (they stay singleton
        // wedges, as before).
        edges.clear();
        for (uint32_t k = 0; k < deg; k++)
        {
            parent[k] = k;
            const uint32_t t = corner_tri[start + k];
            if (face_inv_len[t] == 0.0f)
            {
                continue;
            }
            const Triangle &tri = triangles[t];
            const uint8_t c = corner_c[start + k];
            edges.emplace_back(grp(tri.v[(c + 1u) % 3u]), k);
            edges.emplace_back(grp(tri.v[(c + 2u) % 3u]), k);
        }

        // Compare only corners sharing an edge. Sorting makes manifold groups O(deg log deg)
        // instead of quadratic at high-valence vertices.
        std::sort(edges.begin(), edges.end());
        for (size_t a = 0; a < edges.size();)
        {
            size_t b = a + 1;
            while (b < edges.size() && edges[b].first == edges[a].first)
            {
                b++;
            }
            for (size_t p = a; p < b; p++)
            {
                const uint32_t ci = edges[p].second;
                const uint32_t ti = corner_tri[start + ci];
                for (size_t q = p + 1; q < b; q++)
                {
                    const uint32_t cj = edges[q].second;
                    const uint32_t tj = corner_tri[start + cj];
                    bool unite = false;
                    if (has_groups)
                    {
                        const uint32_t gi = (*smooth_groups)[ti];
                        unite = (gi != 0u) && (gi == (*smooth_groups)[tj]);
                    }
                    else
                    {
                        const float cos_a = dot(face_n[ti], face_n[tj]) * face_inv_len[ti] * face_inv_len[tj];
                        unite = (cos_a >= crease_cos);
                    }
                    if (unite)
                    {
                        const uint32_t ri = find(ci);
                        const uint32_t rj = find(cj);
                        if (ri != rj)
                        {
                            parent[ri] = rj;
                        }
                    }
                }
            }
            a = b;
        }

        // Sum each wedge normal and cache roots for materialization.
        wedge_n.assign(deg, vec3{});
        roots.resize(deg);
        for (uint32_t k = 0; k < deg; k++)
        {
            const uint32_t r = find(k);
            roots[k] = r;
            wedge_n[r] += face_n[corner_tri[start + k]];
        }

        // Materialize one vertex per original-vertex/wedge pair, preserving parallel attributes.
        out_map.clear();
        for (uint32_t k = 0; k < deg; k++)
        {
            const uint32_t t = corner_tri[start + k];
            const uint8_t c = corner_c[start + k];
            const uint32_t ov = triangles[t].v[c]; // original vertex (its UV/ao)
            const uint32_t r = roots[k];

            uint32_t out_v = UINT32_MAX;
            bool ov_seen = false;
            for (const OutSlot &slot : out_map)
            {
                if (slot.ov == ov)
                {
                    ov_seen = true;
                    if (slot.root == r)
                    {
                        out_v = slot.out;
                        break;
                    }
                }
            }
            if (out_v == UINT32_MAX)
            {
                if (!ov_seen)
                {
                    out_v = ov;
                }
                else
                {
                    out_v = static_cast<uint32_t>(vertices.size());
                    const Vertex split = vertices[ov]; // copies pos/uv/ao
                    vertices.push_back(split);
                    if (has_vcol)
                    {
                        vertex_colors.push_back(vertex_colors[ov]);
                    }
                    if (has_valpha)
                    {
                        vertex_alpha.push_back(vertex_alpha[ov]);
                    }
                    if (has_uv1_arr)
                    {
                        uv1.push_back(uv1[ov]);
                    }
                    if (has_weld)
                    {
                        // A split remains in its source's weld group.
                        group_of.push_back(group_of[ov]);
                    }
                }
                vertices[out_v].normal = wedge_n[r];
                out_map.push_back({ ov, r, out_v });
            }
            triangles[t].v[c] = out_v;
        }
    }

    for (auto &vert : vertices)
    {
        const float len_sq = vert.normal.length_sq();
        if (len_sq > 1e-12f)
        {
            vert.normal = vert.normal * (1.0f / std::sqrt(len_sq));
        }
    }
}

void Mesh::compute_tangents()
{
    tangents.assign(vertices.size(), vec3{});

    // Loaders do not share vertices across different material UV bindings, so each vertex has
    // one normal-map UV set.
    const vec2 *p_uv1 = has_uv1 ? uv1.data() : nullptr;

    // Accumulate tangent vectors from each triangle's UV layout.
    // For triangle (P0,P1,P2) with UVs (u0,v0),(u1,v1),(u2,v2):
    //   T = (dP1*dv2 - dP2*dv1) / (du1*dv2 - du2*dv1)
    for (const auto &tri : triangles)
    {
        const Vertex &v0 = vertices[tri.v[0]];
        const Vertex &v1 = vertices[tri.v[1]];
        const Vertex &v2 = vertices[tri.v[2]];

        // Do not fold KHR_texture_transform into TBN. The glTF spec does not define rotating
        // the tangent frame, and reference viewers apply the transform only while sampling.
        const bool s1 = p_uv1 && mat_at(tri.material_idx).normal_map.uv_set != 0;
        const vec2 uv0 = s1 ? p_uv1[tri.v[0]] : v0.uv;
        const vec2 uv1v = s1 ? p_uv1[tri.v[1]] : v1.uv;
        const vec2 uv2 = s1 ? p_uv1[tri.v[2]] : v2.uv;

        const vec3 dp1 = v1.pos - v0.pos;
        const vec3 dp2 = v2.pos - v0.pos;
        const float du1 = uv1v.x - uv0.x;
        const float dv1 = uv1v.y - uv0.y;
        const float du2 = uv2.x - uv0.x;
        const float dv2 = uv2.y - uv0.y;

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
            // No UV contribution: pick an arbitrary vector perpendicular to n.
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

// Bake scale-independent curvature AO from centroid depth divided by local RMS edge length.
// Normalizing the offset loses cavity depth and exaggerates scan noise.

void Mesh::compute_ao(int n_threads)
{
    const size_t n = vertices.size();

    // CSR keeps edge neighbors contiguous without one allocation per vertex. Duplicates
    // intentionally weight denser connectivity.
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

    // Per-vertex AO is independent: each vertex only writes to its own ao field.
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

            // Centroid of neighboring positions, plus the summed squared edge length so the
            // curvature can be normalized by spacing instead of by its own magnitude.
            vec3 centroid{};
            float edge_sq_sum = 0.0f;
            for (int ai = adj_start[i]; ai < adj_start[i + 1]; ai++)
            {
                const vec3 &q = vertices[adj_list[static_cast<size_t>(ai)]].pos;
                centroid += q;
                edge_sq_sum += (q - p).length_sq();
            }
            const float inv_deg = 1.0f / static_cast<float>(deg);
            centroid = centroid * inv_deg;

            // RMS edge length: one sqrt per vertex. Zero means all neighbors coincide with the
            // vertex (degenerate fan): no curvature is defined, so leave it fully lit.
            const float mean_edge = std::sqrt(edge_sq_sum * inv_deg);
            if (mean_edge < 1e-12f)
            {
                vertices[i].ao = 1.0f;
                continue;
            }

            // Positive signed depth/width means a concave cavity; reduce AO.
            // Clamp so convex surfaces stay at 1 and deep cavities don't go fully black.
            const float curvature = dot(centroid - p, N) / mean_edge;
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

void Mesh::optimize_vertex_cache(int n_threads)
{
    if (triangles.size() < 2)
    {
        return;
    }

    const size_t nv = vertices.size();
    const size_t nt = triangles.size();
    const size_t ni = nt * 3;

    // meshopt exposes no triangle permutation. Optimize one material range at a time so
    // reordering cannot detach material_idx from its triangle.
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

        // Skip regrouping when the loader already produced material order.
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
        // Material groups touch disjoint index slices and can run in parallel.
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
            // The main thread participates; never spawn more workers than groups.
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
    if (has_vertex_alpha && vertex_alpha.size() < nv)
    {
        vertex_alpha.resize(nv, 1.0f);
    }
    // Missing uv1 entries inherit each vertex's uv0, so resize cannot supply the fill value.
    if (has_uv1 && uv1.size() < nv)
    {
        for (size_t v = uv1.size(); v < nv; v++)
        {
            uv1.push_back(vertices[v].uv);
        }
    }

    std::vector<Vertex> new_verts(new_nv);
    std::vector<vec3> new_tans(new_nv);
    std::vector<vec3> new_vcols;
    if (has_vertex_colors)
    {
        new_vcols.resize(new_nv);
    }
    std::vector<float> new_valpha;
    if (has_vertex_alpha)
    {
        new_valpha.resize(new_nv);
    }
    std::vector<vec2> new_uv1;
    if (has_uv1)
    {
        new_uv1.resize(new_nv);
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
        if (has_vertex_alpha)
        {
            new_valpha[remap[v]] = vertex_alpha[v];
        }
        if (has_uv1)
        {
            new_uv1[remap[v]] = uv1[v];
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
    if (has_vertex_alpha)
    {
        vertex_alpha = std::move(new_valpha);
    }
    if (has_uv1)
    {
        uv1 = std::move(new_uv1);
    }
}
