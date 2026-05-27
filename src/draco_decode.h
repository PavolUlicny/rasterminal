#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Decoded geometry from a KHR_draco_mesh_compression bitstream, flattened into
// plain arrays. This keeps the Draco headers confined to draco_decode.cpp (the
// glTF loader includes only this header) — Draco's templated decoder trips the
// project's strict -Werror set, so it must not be compiled into src/ TUs, the
// same isolation the meshoptimizer unity shim gets. Optional attributes are
// left empty when the source lacks them.
struct DracoMesh
{
    std::vector<float> positions; // 3 * num_points
    std::vector<float> normals;   // 3 * num_points, or empty
    std::vector<float> uvs;       // 2 * num_points, or empty
    // RGB only: matches Mesh::vertex_colors (std::vector<vec3>). When a Draco asset
    // authors RGBA COLOR_0, ConvertValue still writes all four floats into a local
    // scratch buffer, but only the first three are copied here — alpha is dropped
    // intentionally, matching the accessor-path behaviour.
    std::vector<float> colors;     // 3 * num_points (RGB), or empty
    std::vector<uint32_t> indices; // 3 * num_faces
    size_t num_points = 0;
};

// Decode a Draco mesh bitstream into plain arrays. pos_id is the Draco attribute
// unique-id for POSITION (required); normal_id / uv_id / color_id are unique-ids,
// or -1 when that attribute is absent. Returns false on any decode failure or if
// the POSITION attribute is missing. Takes `void *` so the byte-pointer reinterpret
// stays a one-step static_cast inside the implementation (cgltf returns uint8_t*,
// Draco wants char*); the project bans reinterpret_cast via clang-tidy.
bool decode_draco_mesh(
    const void *data, size_t size, uint32_t pos_id, int normal_id, int uv_id, int color_id, DracoMesh &out
);
