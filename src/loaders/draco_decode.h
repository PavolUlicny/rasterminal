#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Plain decoded geometry. Keeping Draco headers in the implementation isolates their
// warning failures from the rest of the project. Missing optional attributes stay empty.
struct DracoMesh
{
    std::vector<float> positions; // 3 * num_points
    std::vector<float> normals;   // 3 * num_points, or empty
    std::vector<float> uvs;       // 2 * num_points (TEXCOORD_0), or empty
    std::vector<float> uvs1;      // 2 * num_points (TEXCOORD_1), or empty
    // RGB values parallel Mesh::vertex_colors.
    std::vector<float> colors; // 3 * num_points (RGB), or empty
    // Opacity for RGBA COLOR_0; empty for RGB or absent color data.
    std::vector<float> colors_alpha; // num_points (alpha), or empty
    std::vector<uint32_t> indices;   // 3 * num_faces
    size_t num_points = 0;
};

// Decode a Draco mesh. POSITION is required; optional attribute IDs use -1 when absent.
// The void pointer bridges cgltf's bytes to Draco's char input without reinterpret_cast.
bool decode_draco_mesh(
    const void *data, size_t size, uint32_t pos_id, int normal_id, int uv_id, int uv1_id, int color_id, DracoMesh &out
);
