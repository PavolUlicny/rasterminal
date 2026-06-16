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
    std::vector<Texture> textures;   // loaded on demand; Material::diffuse_tex / normal_tex index here
    std::vector<vec3> tangents;      // per-vertex tangents; always vertices.size() after load_model()
    std::vector<vec3> vertex_colors; // per-vertex RGB; populated only when has_vertex_colors is true
    std::vector<float> vertex_alpha; // per-vertex opacity (COLOR_0 / PLY alpha); only when has_vertex_alpha
    bool has_vertex_colors = false;  // true when any loader populates vertex_colors (PLY, OBJ, glTF COLOR_0)
    bool has_vertex_alpha = false;   // true when any loader populates per-vertex alpha (transparent path only)
    bool has_transparent = false;    // true if any material blends OR any vertex is translucent; gates the
                                     // transparent render passes. When set, opaque_count MUST be valid: the
                                     // render and shadow passes trust the range split and do NOT re-test per
                                     // triangle. load_model always sets both together; a hand-built Mesh that
                                     // sets has_transparent must also set opaque_count (a forgotten 0 routes
                                     // every triangle to the transparent pass and casts no shadows).
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

    // Dispatch loader: picks load_obj, load_ply, or load_stl based on file extension.
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

    // Load geometry from a PLY file (ASCII or binary little/big-endian).
    // Supports vertex positions, normals (nx/ny/nz), and UVs (s/t, u/v, texture_u/v).
    // A `comment TextureFile <name>` header (MeshLab / photogrammetry convention)
    // binds an albedo texture to the default material, resolved relative to the PLY
    // directory; only honoured when the mesh has UVs, and a missing/undecodable file
    // is silently dropped (consistent with the OBJ/glTF loaders).
    // crease_cos is forwarded to compute_normals() when normals are absent.
    // Returns false on failure.
    bool load_ply(const std::string &path, float crease_cos = -1.0f);

    // Load geometry from an STL file (ASCII or binary).
    // No UV or material support (STL has none). Normals are always recomputed
    // for smooth per-vertex shading (crease_cos has no effect — STL vertices are
    // unshared, so there is nothing to merge). Returns false on failure.
    bool load_stl(const std::string &path, float crease_cos = -1.0f);

    // Load geometry from a glTF or GLB file.
    // Supports positions, normals, UVs, PBR materials, diffuse/normal textures,
    // and node transforms. n_threads parallelizes texture decode. crease_cos is
    // forwarded to compute_normals() when the file has no normals.
    // Returns false on failure.
    bool load_gltf(const std::string &path, int n_threads = 1, float crease_cos = -1.0f);

  private:
    // Compute smooth per-vertex normals from face geometry, splitting vertices
    // across hard edges. crease_cos is the cosine of the crease-angle threshold:
    // two faces sharing an edge are smoothed together iff the angle between their
    // normals stays below the threshold (dot of unit normals >= crease_cos),
    // otherwise the shared vertices split so each side keeps its own normal.
    // crease_cos = -1 (cos 180deg) never creases across edges — full smoothing
    // on manifold input; bowtie (point-only-shared) vertices always split.
    // May append new entries to `vertices` (and the parallel `vertex_colors` when
    // present) and rewrite Triangle::v[] indices to point at the split copies.
    // Called by each loader when the file provides no normal data.
    //
    // weld (optional) maps each vertex index to a position-group id so adjacency
    // is computed in welded space: distinct vertices sharing a group are treated
    // as one position for edge-sharing and smoothing, but stay separate output
    // vertices (each keeps its own UV) sharing the welded normal. OBJ supplies it
    // to smooth across UV seams (the loader splits one position into several
    // vertices by texcoord); the source OBJ vertex index is the group id, and
    // n_groups is the upper bound on those ids (the OBJ position count). Passing
    // weld = nullptr means identity grouping (group id == vertex index) —
    // byte-identical to no welding, used by PLY/STL/glTF; n_groups is ignored.
    //
    // smooth_groups (optional, OBJ only) carries one smoothing-group id per
    // triangle (parallel to `triangles`). When non-null, groups are authoritative
    // and crease_cos is ignored: two faces sharing an edge smooth iff they share
    // the same non-zero group id; id 0 (OBJ `s off`/`s 0`) never smooths, even
    // with another id-0 face. Composes with weld — a welded UV seam smooths iff
    // both halves share a position group AND the faces share a smoothing group.
    // nullptr selects the crease-angle path above (PLY/STL/glTF and group-less OBJs).
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
