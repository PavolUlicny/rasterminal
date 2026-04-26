#pragma once

#include "light.h" // for Material
#include "linalg.h"
#include "texture.h" // for Texture
#include <cstdint>
#include <string>
#include <vector>

struct Vertex
{
    vec3 pos;
    vec3 normal;
    vec3 tangent; // world-space tangent; bitangent = cross(normal, tangent)
    vec2 uv;
    vec3 color = {1.0f, 1.0f, 1.0f}; // vertex color (white = no tint; from PLY red/green/blue)
    float ao = 1.0f;                 // baked ambient occlusion (1 = fully lit, 0 = fully occluded)
};

struct Triangle
{
    uint32_t v[3];             // indices into Mesh::vertices
    uint32_t material_idx = 0; // index into Mesh::materials (0 = default)
};

struct Mesh
{
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
    std::vector<Material> materials; // index 0 is always the default white material
    std::vector<Texture> textures;   // loaded on demand; Material::diffuse_tex / normal_tex index here

    const Material &mat_at(uint32_t idx) const
    {
        return idx < materials.size() ? materials[idx] : materials[0];
    }

    const Texture *tex_at(int idx) const
    {
        return (idx >= 0 && idx < static_cast<int>(textures.size()))
                   ? &textures[static_cast<size_t>(idx)]
                   : nullptr;
    }

    // Dispatch loader: picks load_obj, load_ply, or load_stl based on file extension.
    // Clears all mesh state before loading. Returns false on failure or unknown extension.
    bool load_model(const std::string &path, bool ao = true);

    // Clear all geometry, material, and texture data.
    void clear();

    // Load geometry from an OBJ file; also loads the associated .mtl if present.
    // Returns false on failure.
    bool load_obj(const std::string &path);

    // Load geometry from a PLY file (ASCII or binary little/big-endian).
    // Supports vertex positions, normals (nx/ny/nz), and UVs (s/t, u/v, texture_u/v).
    // Returns false on failure.
    bool load_ply(const std::string &path);

    // Load geometry from an STL file (ASCII or binary).
    // No UV or material support (STL has none). Normals are always recomputed
    // for smooth per-vertex shading. Returns false on failure.
    bool load_stl(const std::string &path);

private:
    // Average adjacent face normals to produce smooth per-vertex normals.
    // Called by each loader when the file provides no normal data.
    void compute_normals();

    // Compute per-vertex tangent vectors from UV layout (needed for normal mapping).
    // Called by load_model() after the format-specific loader returns.
    void compute_tangents();

    // Bake a per-vertex ambient occlusion factor from mesh curvature.
    // Concave areas (cavities, creases) receive lower AO; convex areas stay at 1.
    // Called by load_model() after compute_tangents(), unless ao=false was passed.
    void compute_ao();
};
