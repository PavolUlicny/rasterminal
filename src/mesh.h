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

    // Dispatch loader: picks load_obj or load_ply based on file extension.
    // Returns false on failure or unknown extension.
    bool load_model(const std::string &path);

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
    // Called automatically by load_obj when the file has no vn data.
    void compute_normals();

    // Compute per-vertex tangent vectors from UV layout (needed for normal mapping).
    // Called automatically by load_obj after normals are finalised.
    void compute_tangents();
};
