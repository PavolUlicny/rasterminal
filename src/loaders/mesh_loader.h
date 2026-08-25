#pragma once

#include "src/loaders/mesh.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <string>
#include <thread>
#include <vector>

// Model directory with its trailing separator, or empty for the current directory.
// Accepts both path separators.
inline std::string dir_of(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
}

// Rolls Mesh back to its entry state unless commit() is called.
struct MeshSnapshot
{
    explicit MeshSnapshot(Mesh &m)
        : m_mesh(m), m_v(m.vertices.size()), m_t(m.triangles.size()), m_mat(m.materials.size()),
          m_tex(m.textures.size()), m_tans(m.tangents.size()), m_vcols(m.vertex_colors.size()),
          m_valpha(m.vertex_alpha.size()), m_uv1(m.uv1.size()), m_has_vcol(m.has_vertex_colors),
          m_has_valpha(m.has_vertex_alpha), m_has_uv1(m.has_uv1)
    {
    }

    ~MeshSnapshot()
    {
        if (!m_done)
        {
            rollback();
        }
    }

    void commit() { m_done = true; }

    void rollback()
    {
        m_mesh.vertices.resize(m_v);
        m_mesh.triangles.resize(m_t);
        m_mesh.materials.resize(m_mat);
        m_mesh.textures.resize(m_tex);
        m_mesh.tangents.resize(m_tans);
        m_mesh.vertex_colors.resize(m_vcols);
        m_mesh.vertex_alpha.resize(m_valpha);
        m_mesh.uv1.resize(m_uv1);
        m_mesh.has_vertex_colors = m_has_vcol;
        m_mesh.has_vertex_alpha = m_has_valpha;
        m_mesh.has_uv1 = m_has_uv1;
    }

    MeshSnapshot(const MeshSnapshot &) = delete;
    MeshSnapshot &operator=(const MeshSnapshot &) = delete;
    MeshSnapshot(MeshSnapshot &&) = delete;
    MeshSnapshot &operator=(MeshSnapshot &&) = delete;

  private:
    Mesh &m_mesh;
    size_t m_v, m_t, m_mat, m_tex, m_tans, m_vcols, m_valpha, m_uv1;
    bool m_has_vcol;
    bool m_has_valpha;
    bool m_has_uv1;
    bool m_done = false;
};

// Decode independent texture requests in parallel. Successful results keep request order;
// failures are removed and material indices are remapped to -1. `textures` must be empty,
// and decode(i) must not mutate shared state.
template <class Decode>
inline void decode_textures(
    std::vector<Texture> &textures, std::vector<Material> &materials, size_t count, int n_threads, Decode decode
)
{
    assert(textures.empty());
    if (count == 0)
    {
        return;
    }

    // This format-agnostic layer has no texture name to include in a diagnostic.
    std::vector<Texture> decoded(count);

    const int eff =
        (n_threads <= 1 || count < 2) ? 1 : static_cast<int>(std::min(static_cast<size_t>(n_threads), count));
    if (eff <= 1)
    {
        for (size_t i = 0; i < count; i++)
        {
            decoded[i] = decode(i);
        }
    }
    else
    {
        std::atomic<size_t> next{ 0 };
        const auto worker = [&]()
        {
            for (size_t i = next.fetch_add(1, std::memory_order_relaxed); i < count;
                 i = next.fetch_add(1, std::memory_order_relaxed))
            {
                decoded[i] = decode(i);
            }
        };
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(eff - 1));
        for (int t = 0; t < eff - 1; t++)
        {
            threads.emplace_back(worker);
        }
        worker();
        for (auto &th : threads)
        {
            th.join();
        }
    }

    const bool any_failed = std::any_of(decoded.begin(), decoded.end(), [](const Texture &t) { return !t.valid(); });
    if (!any_failed)
    {
        // No failures means provisional indices are final.
        textures = std::move(decoded);
        return;
    }

    std::vector<int> remap(count, -1);
    std::vector<Texture> compacted;
    compacted.reserve(count);
    for (size_t i = 0; i < count; i++)
    {
        if (decoded[i].valid())
        {
            remap[i] = static_cast<int>(compacted.size());
            compacted.push_back(std::move(decoded[i]));
        }
    }
    textures = std::move(compacted);
    for (auto &m : materials)
    {
        const auto fix = [&](int &f)
        {
            if (f >= 0)
            {
                f = remap[static_cast<size_t>(f)];
            }
        };
        fix(m.diffuse_map.tex);
        fix(m.specular_map.tex);
        fix(m.normal_map.tex);
        fix(m.mr_map.tex);
        fix(m.emissive_map.tex);
        fix(m.occlusion_map.tex);
    }
}
