#pragma once

#include "light.h"
#include "linalg.h"
#include "mesh.h"

#include <vector>

// ─── ShadowMap ────────────────────────────────────────────────────────────────
// Depth buffer rendered from the key light's perspective.
// Build once with build_shadow_map() and reuse every frame — the light and mesh
// geometry are static so the map never changes.

struct ShadowMap
{
    static constexpr int SIZE = 2048;
    std::vector<float> depth; // NDC z (slope-biased on write), initialised to 1.0
    mat4 light_vp;

    void clear()
    {
        depth.assign(static_cast<size_t>(SIZE * SIZE), 1.0f);
    }

    // Returns shadow factor in [0,1]: 0=fully lit, 1=fully shadowed.
    // Uses 3×3 PCF kernel for soft edges.
    float in_shadow(vec3 world_pos) const;
};

// Build a shadow map for a directional light. Fits an orthographic frustum to
// the mesh bounding sphere and depth-rasterizes all triangles with slope-scale
// bias to prevent self-shadowing acne.
ShadowMap build_shadow_map(const Mesh &mesh, const Light &light);
