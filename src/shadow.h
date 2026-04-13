#pragma once

#include "light.h"
#include "linalg.h"
#include "mesh.h"

// ─── ShadowMap ────────────────────────────────────────────────────────────────
// Depth buffer rendered from the key light's perspective.
// Build once with build_shadow_map() and reuse every frame — the light and mesh
// geometry are static so the map never changes.

struct ShadowMap
{
    static constexpr int SIZE = 256;
    float depth[SIZE * SIZE]; // NDC z (slope-biased on write), initialised to 1.0
    mat4 light_vp;

    void clear()
    {
        for (auto &d : depth)
            d = 1.0f;
    }

    bool in_shadow(vec3 world_pos) const;
};

// Build a shadow map for a directional light. Fits an orthographic frustum to
// the mesh bounding sphere and depth-rasterizes all triangles with slope-scale
// bias to prevent self-shadowing acne.
ShadowMap build_shadow_map(const Mesh &mesh, const Light &light);
