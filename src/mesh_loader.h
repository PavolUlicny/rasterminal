#pragma once

#include "mesh.h"

// RAII snapshot of Mesh state for transactional loader rollback.
// Construct at the top of a loader; call commit() on the success path.
// Destruction without commit() restores all vectors and flags to their entry state.
struct MeshSnapshot
{
    explicit MeshSnapshot(Mesh &m)
        : m_mesh(m), m_v(m.vertices.size()), m_t(m.triangles.size()), m_mat(m.materials.size()),
          m_tex(m.textures.size()), m_tans(m.tangents.size()), m_vcols(m.vertex_colors.size()),
          m_has_vcol(m.has_vertex_colors)
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
        m_mesh.has_vertex_colors = m_has_vcol;
    }

    MeshSnapshot(const MeshSnapshot &) = delete;
    MeshSnapshot &operator=(const MeshSnapshot &) = delete;
    MeshSnapshot(MeshSnapshot &&) = delete;
    MeshSnapshot &operator=(MeshSnapshot &&) = delete;

  private:
    Mesh &m_mesh;
    size_t m_v, m_t, m_mat, m_tex, m_tans, m_vcols;
    bool m_has_vcol;
    bool m_done = false;
};
