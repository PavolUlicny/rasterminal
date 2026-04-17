#pragma once

#include "mesh.h"

// RAII snapshot of Mesh vector sizes for transactional loader rollback.
// Construct at the top of a loader; call commit() on the success path.
// Destruction without commit() restores all four vectors to their entry sizes.
struct MeshSnapshot
{
    explicit MeshSnapshot(Mesh &m)
        : m_mesh(m),
          m_v(m.vertices.size()), m_t(m.triangles.size()),
          m_mat(m.materials.size()), m_tex(m.textures.size()) {}

    ~MeshSnapshot()
    {
        if (!m_done)
            rollback();
    }

    void commit() { m_done = true; }

    void rollback()
    {
        m_mesh.vertices.resize(m_v);
        m_mesh.triangles.resize(m_t);
        m_mesh.materials.resize(m_mat);
        m_mesh.textures.resize(m_tex);
    }

    MeshSnapshot(const MeshSnapshot &) = delete;
    MeshSnapshot &operator=(const MeshSnapshot &) = delete;

private:
    Mesh &m_mesh;
    size_t m_v, m_t, m_mat, m_tex;
    bool m_done = false;
};
