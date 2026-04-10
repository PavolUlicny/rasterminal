#pragma once

#include "linalg.h"
#include <cstdint>
#include <string>
#include <vector>

struct Vertex
{
    vec3 pos;
    vec3 normal;
    vec2 uv;
};

struct Triangle
{
    uint32_t v[3]; // indices into Mesh::vertices
};

struct Mesh
{
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;

    // Load geometry from an OBJ file. Returns false on failure.
    bool load_obj(const std::string &path);

private:
    // Average adjacent face normals to produce smooth per-vertex normals.
    // Called automatically by load_obj when the file has no vn data.
    void compute_normals();
};
