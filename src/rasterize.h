#pragma once

#include "framebuffer.h"
#include "clip.h"
#include "light.h"
#include "linalg.h"
#include "mesh.h"
#include "shadow.h"
#include "texture.h"

// ─── ClipVert ─────────────────────────────────────────────────────────────────
// A clip-space vertex bundled with the world-space attributes needed for
// lighting and near-plane clipping.

struct ClipVert
{
    vec4 c;                          // clip-space position (w = -z_view; > 0 means in front of camera)
    vec3 pos;                        // world-space position
    vec3 normal;                     // world-space normal
    vec3 tangent;                    // world-space tangent (for TBN normal mapping)
    vec2 uv;                         // texture coordinates
    float ao;                        // baked ambient occlusion factor
    vec3 color = {1.0f, 1.0f, 1.0f}; // vertex color (white = no tint); at end so existing aggregate inits keep working
};

// ─── RasterTri ────────────────────────────────────────────────────────────────
// All data needed to rasterize one visible, clipped, backface-culled triangle.
// Phase 1 workers fill these; Phase 2 workers consume them.
//
// Mode-exclusive fields share storage via a union. Only the active branch's
// members are written and read — the other branch's storage is never accessed.

struct RasterTri
{
    // Shared across all shading modes
    vec3 sa, sb, sc;             // screen-space positions (x, y, ndc_z)
    float wa, wb, wc;            // clip-space w (for perspective-correct interp)
    vec3 pa, pb, pc;             // world-space positions
    vec2 uva, uvb, uvc;          // texture coordinates
    const Texture *tex;          // diffuse texture  (nullptr if none)
    const ShadowMap *shadow_map; // pre-built shadow map (nullptr if disabled)
    float alpha_cutoff = 0.0f;   // 0 = disabled; >0 = discard pixels with diffuse-tex alpha below this

    // Flat / Gouraud data (lighting evaluated once per vertex in Phase 1).
    struct FgData
    {
        vec3 col_a, col_b, col_c;    // fully lit (all lights)
        vec3 shad_a, shad_b, shad_c; // shadowed (key light excluded)
    };

    // Phong data (lighting evaluated per pixel in Phase 2).
    // eye / lights / n_lights / ambient are per-frame constants supplied
    // by the Phase 2 dispatcher, not duplicated into every triangle.
    struct PhData
    {
        vec3 na, nb, nc;          // world-space normals
        vec3 tana, tanb, tanc;    // world-space tangents
        vec3 vcola, vcolb, vcolc; // vertex colors (white = no tint)
        float aoa, aob, aoc;      // baked ambient occlusion
        const Material *mat;      // pointer into mesh.materials — valid for frame lifetime
        const Texture *stex;      // specular texture (nullptr if none)
        const Texture *nmap;      // normal map       (nullptr if none)
    };

    union
    {
        FgData fg;
        PhData ph;
    };

    RasterTri() {} // NOLINT(clang-analyzer-optin.cplusplus.UninitializedObject) — union fields are written before read; explicit ctor: vec3's non-trivial ctor deletes the union's implicit one
};

// ─── Rasterization primitives ─────────────────────────────────────────────────

// Clip triangle (a,b,c) against the near plane w = near_w.
// Produces 0 (fully behind), 1, or 2 output triangles written into out[0..n-1].
// Returns the count. Winding order is preserved for all outputs.
[[nodiscard]] int clip_near(const ClipVert &a, const ClipVert &b, const ClipVert &c,
                            ClipVert out[2][3], float near_w);

// DDA line rasterizer with per-pixel depth testing (wireframe).
void draw_line(Framebuffer &fb, vec3 a, vec3 b, Color color);

// Rasterize one triangle with pre-computed per-vertex Flat/Gouraud colours.
// y_min/y_max define this thread's exclusive pixel row band.
void rasterize(Framebuffer &fb,
               vec3 sa, vec3 sb, vec3 sc,
               float wa, float wb, float wc,
               vec3 col_a, vec3 col_b, vec3 col_c,
               vec3 shad_a, vec3 shad_b, vec3 shad_c,
               vec3 pa, vec3 pb, vec3 pc,
               vec2 uva, vec2 uvb, vec2 uvc,
               const Texture *tex,
               float alpha_cutoff,
               const ShadowMap *shadow_map,
               int y_min, int y_max);

// Rasterize one triangle with per-pixel Blinn-Phong lighting (Phong shading).
// y_min/y_max define this thread's exclusive pixel row band.
void rasterize_phong(Framebuffer &fb,
                     vec3 sa, vec3 sb, vec3 sc,
                     float wa, float wb, float wc,
                     vec3 pa, vec3 pb, vec3 pc,
                     vec3 na, vec3 nb, vec3 nc,
                     vec3 tana, vec3 tanb, vec3 tanc,
                     vec2 uva, vec2 uvb, vec2 uvc,
                     float aoa, float aob, float aoc,
                     vec3 vcola, vec3 vcolb, vec3 vcolc,
                     bool has_vcol,
                     const vec3 &eye,
                     const Light *lights, int n_lights,
                     const vec3 &ambient,
                     const Material &mat,
                     const Texture *tex,
                     const Texture *nmap,
                     const Texture *stex,
                     const ShadowMap *shadow_map,
                     int y_min, int y_max);
