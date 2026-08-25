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
    bool has_transparent = false;    // true if any material blends OR any vertex is translucent; gates the
                                     // transparent render passes. When set, opaque_count MUST be valid: the
                                     // render passes trust the range split and do NOT re-test per triangle.
                                     // load_model always sets both together; a hand-built Mesh that sets
                                     // has_transparent must also set opaque_count (a forgotten 0 routes every
                                     // triangle to the transparent pass).
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

    // Dispatch loader: native loaders own OBJ, PLY, STL, and glTF/GLB. Other extensions
    // advertised by the vendored Assimp build use the Assimp loader.
    // Clears all mesh state before loading. Returns false on failure or unknown extension.
    // crease_angle_deg is the smoothing threshold used when a file provides no normals:
    // adjacent faces are smoothed together below this angle and hard-edged above it.
    [[nodiscard]] bool
    load_model(const std::string &path, bool ao = true, int n_threads = 1, float crease_angle_deg = 60.0f);

    // Clear all geometry, material, and texture data.
    void clear();

    // Load geometry from an OBJ file; also loads the associated .mtl if present.
    // n_threads parallelizes texture decode. crease_cos is forwarded to
    // compute_normals() when the file has no normals. Returns false on failure.
    bool load_obj(const std::string &path, int n_threads = 1, float crease_cos = -1.0f);

    // Load geometry from a PLY file (ASCII or binary little/big-endian): positions, normals
    // (nx/ny/nz), UVs (s/t, u/v, texture_u/v). A `comment TextureFile <name>` header
    // (MeshLab / photogrammetry convention) binds an albedo texture to the default material,
    // resolved relative to the PLY directory, honoured only when the mesh has UVs; a missing
    // or undecodable file drops silently like the OBJ/glTF loaders. crease_cos is forwarded
    // to compute_normals() when normals are absent. Returns false on failure.
    bool load_ply(const std::string &path, float crease_cos = -1.0f);

    // Load geometry from an STL file (ASCII or binary).
    // No UV or material support (STL has none). Normals are always recomputed;
    // since the loader consumes stl_reader's deduplicated shared vertices,
    // crease_cos drives crease-angle smoothing as in the other formats
    // (crease_cos == 0 splits every shared vertex back into per-face wedges =
    // faceted). Returns false on failure.
    bool load_stl(const std::string &path, float crease_cos = -1.0f);

    // Load geometry from a glTF or GLB file.
    // Supports positions, normals, UVs, PBR materials, diffuse/normal textures,
    // and node transforms. n_threads parallelizes texture decode. crease_cos is
    // forwarded to compute_normals() when the file has no normals.
    // Returns false on failure.
    bool load_gltf(const std::string &path, int n_threads = 1, float crease_cos = -1.0f);

    // Compatibility loader for non-native formats. Native-load failures are not retried here.
    bool load_assimp(const std::string &path, int n_threads = 1, float crease_angle_deg = 60.0f);

  private:
    // Compute smooth per-vertex normals, splitting vertices across hard edges. crease_cos
    // is the cosine of the crease threshold: faces sharing an edge smooth together iff the
    // dot of their unit normals >= crease_cos, else the shared vertices split so each side
    // keeps its own normal. -1 (cos 180deg) never creases (full smoothing on manifold
    // input); bowtie (point-only-shared) vertices always split. May append to `vertices`
    // (and the parallel `vertex_colors` when present) and rewrite Triangle::v[] to the
    // split copies. Called by each loader when the file provides no normal data.
    //
    // weld (optional) maps vertex index -> position-group id so adjacency runs in welded
    // space: vertices sharing a group count as one position for smoothing but stay
    // separate outputs (each keeps its own UV) sharing the welded normal. OBJ supplies it
    // to smooth across UV seams (group id = source OBJ position index, n_groups = the OBJ
    // position count); nullptr means identity grouping, byte-identical to no welding
    // (PLY/STL/glTF; n_groups ignored).
    //
    // smooth_groups (optional, OBJ only): one smoothing-group id per triangle. When
    // non-null, groups are authoritative and crease_cos is ignored: faces sharing an edge
    // smooth iff they share the same non-zero id; id 0 (OBJ `s off`/`s 0`) never smooths, even with
    // another id-0 face. Composes with weld: a welded UV seam smooths iff the halves share
    // a position group AND the faces share a smoothing group. nullptr selects the
    // crease-angle path (PLY/STL/glTF and group-less OBJs).
    void compute_normals(
        float crease_cos,
        const std::vector<uint32_t> *weld = nullptr,
        size_t n_groups = 0,
        const std::vector<uint32_t> *smooth_groups = nullptr
    );

    // Compute per-vertex tangent vectors from UV layout (needed for normal mapping).
    // Called by load_model() after the format-specific loader returns.
    void compute_tangents();

    // Bake a per-vertex ambient occlusion factor from mesh curvature.
    // Concave areas (cavities, creases) receive lower AO; convex areas stay at 1.
    // Called by load_model() after compute_tangents(), unless ao=false was passed.
    void compute_ao(int n_threads = 1);

    // Run meshoptimizer's three-pass pipeline (vertex cache, overdraw, vertex
    // fetch) and remap the vertex array to first-use order. Multi-material
    // meshes are grouped by material_idx before optimization so meshopt cannot
    // strand per-triangle metadata; per-material groups optimize in parallel
    // when n_threads > 1. Called at the end of load_model().
    void optimize_vertex_cache(int n_threads = 1);
};
