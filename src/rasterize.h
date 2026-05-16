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

struct ClipVert // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — ao is always set from vertex.ao before use
{
    vec4 c;       // clip-space position (w = -z_view; > 0 means in front of camera)
    vec3 pos;     // world-space position
    vec3 normal;  // world-space normal
    vec3 tangent; // world-space tangent (for TBN normal mapping)
    vec2 uv;      // texture coordinates
    float ao;
    vec3 color = {1.0f, 1.0f, 1.0f}; // vertex color (white = no tint); at end so existing aggregate inits keep working
};

// ─── RasterTriFg / RasterTriPh ───────────────────────────────────────────────
// All data needed to rasterize one visible, clipped, backface-culled triangle.
// Phase 1 workers fill these; Phase 2 workers consume them.
//
// Two separate structs — one per shading family — so Flat/Gouraud triangles
// don't pay for the larger Phong payload. Both carry the same shared header;
// mode-specific fields follow immediately with no union padding waste.

struct RasterTriFg // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — all fields written by Phase 1 before Phase 2 reads them
{
    vec3 sa, sb, sc;
    float wa, wb, wc;
    vec3 pa, pb, pc;
    vec2 uva, uvb, uvc;
    const Texture *tex;
    const ShadowMap *shadow_map;
    float alpha_cutoff = 0.0f;
    vec3 col_a, col_b, col_c;    // fully lit (all lights)
    vec3 shad_a, shad_b, shad_c; // shadowed (key light excluded)
    RasterTriFg() = default;     // NOLINT(clang-analyzer-optin.cplusplus.UninitializedObject) — fields intentionally uninitialised; written by Phase 1 before Phase 2 reads them
};

struct RasterTriPh // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — all fields written by Phase 1 before Phase 2 reads them
{
    vec3 sa, sb, sc;
    float wa, wb, wc;
    vec3 pa, pb, pc;
    vec2 uva, uvb, uvc;
    const Texture *tex;
    const ShadowMap *shadow_map;
    vec3 na, nb, nc;
    vec3 tana, tanb, tanc;
    vec3 vcola, vcolb, vcolc; // vertex colors (white = no tint)
    float aoa, aob, aoc;
    const Material *mat;
    const Texture *stex;
    const Texture *nmap;
    RasterTriPh() = default; // NOLINT(clang-analyzer-optin.cplusplus.UninitializedObject) — fields intentionally uninitialised; written by Phase 1 before Phase 2 reads them
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
