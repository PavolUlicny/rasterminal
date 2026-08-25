#pragma once

#include "src/math/light.h" // for Material
#include "src/math/linalg.h"
#include "src/render/texture.h" // for Texture
#include <cstdint>
#include <string>
#include <vector>

struct Vertex
{
    vec3 pos;
    vec3 normal;
    vec2 uv;
    float ao = 1.0f; // baked ambient occlusion (1 = fully lit, 0 = fully occluded)
};

struct Triangle
{
    uint32_t v[3] = {};        // indices into Mesh::vertices
    uint32_t material_idx = 0; // index into Mesh::materials (0 = default)
};

struct Mesh
{
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
    std::vector<Material> materials; // index 0 is always the default white material
    std::vector<Texture> textures;   // loaded on demand; Material::diffuse_map.tex / normal_map.tex index here
    std::vector<vec3> tangents;      // per-vertex tangents; always vertices.size() after load_model()
    std::vector<vec3> vertex_colors; // per-vertex RGB; populated only when has_vertex_colors is true
    std::vector<float> vertex_alpha; // per-vertex opacity (COLOR_0 / PLY alpha); only when has_vertex_alpha
    std::vector<vec2> uv1;           // second UV set; parallel, populated only when has_uv1
    bool has_vertex_colors = false;  // true when a loader populates the parallel vertex_colors array
    bool has_vertex_alpha = false;   // true when any loader populates per-vertex alpha (transparent path only)
    bool has_uv1 = false;            // true when a texture uses UV set 1; uv1 stays parallel to vertices
    bool has_transparent = false;    // gates transparent passes; requires a valid opaque_count split
    uint32_t opaque_count = 0;       // triangles [0, opaque_count) are opaque; [opaque_count, size()) are blend
    bool has_double_sided = false;   // true if any material in the mesh has double_sided = true
    bool has_metallic = false;       // true if any material has metallic > 0 (gates the Phong metallic path)
    bool has_emissive = false;       // true if any material has a non-zero emissive factor
    bool has_normal_scale = false;   // true if any normal-mapped material has a non-unit glTF normalScale
    bool has_occlusion = false;      // true if any material has an occlusion texture (gates the Phong AO override)
    bool has_unlit = false;          // true if any material is unlit (gates the per-triangle unlit branch)

    [[nodiscard]] const Material &mat_at(uint32_t idx) const
    {
        return idx < materials.size() ? materials[idx] : materials[0];
    }

    [[nodiscard]] const Texture *tex_at(int idx) const
    {
        return (idx >= 0 && idx < static_cast<int>(textures.size())) ? &textures[static_cast<size_t>(idx)] : nullptr;
    }

    // Clear existing state, dispatch by extension, and run shared post-processing.
    // crease_angle_deg applies only when the format provides no normals or smoothing groups.
    [[nodiscard]] bool
    load_model(const std::string &path, bool ao = true, int n_threads = 1, float crease_angle_deg = 60.0f);

    void clear();

    // Load OBJ and any referenced MTL. Texture decoding may run in parallel.
    bool load_obj(const std::string &path, int n_threads = 1, float crease_cos = -1.0f);

    // Load ASCII or binary PLY, including common UV and TextureFile conventions.
    bool load_ply(const std::string &path, float crease_cos = -1.0f);

    // Load ASCII or binary STL and recompute normals over deduplicated vertices.
    bool load_stl(const std::string &path, float crease_cos = -1.0f);

    // Load glTF or GLB. Texture decoding may run in parallel.
    bool load_gltf(const std::string &path, int n_threads = 1, float crease_cos = -1.0f);

    // Compatibility loader for non-native formats. Native-load failures are not retried here.
    bool load_assimp(const std::string &path, int n_threads = 1, float crease_angle_deg = 60.0f);

  private:
    // Compute area-weighted normals and split vertices across hard edges or bowties.
    // `weld` groups distinct vertices that share a position, such as OBJ UV seams.
    // When present, nonzero OBJ smoothing groups override crease_cos; group zero stays faceted.
    void compute_normals(
        float crease_cos,
        const std::vector<uint32_t> *weld = nullptr,
        size_t n_groups = 0,
        const std::vector<uint32_t> *smooth_groups = nullptr
    );

    // Compute per-vertex tangents for normal mapping.
    void compute_tangents();

    // Bake curvature-based per-vertex ambient occlusion.
    void compute_ao(int n_threads = 1);

    // Run meshoptimizer without separating triangles from their material metadata.
    void optimize_vertex_cache(int n_threads = 1);
};
