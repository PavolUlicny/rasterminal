#pragma once

#include "camera.h"
#include "framebuffer.h"
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

// Build a shadow map for directional light. Fits an orthographic frustum to
// the mesh bounding sphere and depth-rasterizes all triangles with slope-scale
// bias to prevent self-shadowing acne.
ShadowMap build_shadow_map(const Mesh &mesh, const Light &light);

// ─── Renderer ─────────────────────────────────────────────────────────────────

enum class ShadingMode
{
    Wireframe,
    Flat,
    Gouraud,
    Phong
};

struct Renderer
{
    ShadingMode mode = ShadingMode::Gouraud;

    // smap: pre-built shadow map from build_shadow_map(). Pass nullptr to
    // disable shadows, or a valid pointer to reuse a cached map every frame.
    void render(const Mesh &, const Camera &,
                const Light *lights, int n_lights, const vec3 &ambient,
                Framebuffer &, const ShadowMap *smap = nullptr) const;

    // Cycle: Wireframe → Flat → Gouraud → Phong → Wireframe
    void cycle_shading();
};
