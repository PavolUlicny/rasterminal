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

struct ClipVert // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — ao is always set from vertex.ao
                // before use
{
    vec4 c;       // clip-space position (w = -z_view; > 0 means in front of camera)
    vec3 pos;     // world-space position
    vec3 normal;  // world-space normal
    vec3 tangent; // world-space tangent (for TBN normal mapping)
    vec2 uv;      // texture coordinates
    float ao;
    vec3 color = { 1.0f, 1.0f,
                   1.0f }; // vertex color (white = no tint); at end so existing aggregate inits keep working
};

// ─── Rasterization primitives ─────────────────────────────────────────────────

// Clip triangle (a,b,c) against the near plane w = near_w.
// Produces 0 (fully behind), 1, or 2 output triangles written into out[0..n-1].
// Returns the count. Winding order is preserved for all outputs.
[[nodiscard]] int clip_near(const ClipVert &a, const ClipVert &b, const ClipVert &c, ClipVert out[2][3], float near_w);

// DDA line rasterizer with per-pixel depth testing (wireframe).
void draw_line(Framebuffer &fb, vec3 a, vec3 b, Color color);

// Rasterize one triangle with pre-computed per-vertex Flat/Gouraud colours.
// y_min/y_max define this thread's exclusive pixel row band.
void rasterize(
    Framebuffer &fb,
    vec3 sa,
    vec3 sb,
    vec3 sc,
    float wa,
    float wb,
    float wc,
    vec3 col_a,
    vec3 col_b,
    vec3 col_c,
    vec3 shad_a,
    vec3 shad_b,
    vec3 shad_c,
    vec3 pa,
    vec3 pb,
    vec3 pc,
    vec2 uva,
    vec2 uvb,
    vec2 uvc,
    const Texture *tex,
    float alpha_cutoff,
    const ShadowMap *shadow_map,
    int y_min,
    int y_max
);

// Rasterize one triangle with per-pixel Blinn-Phong lighting (Phong shading).
// y_min/y_max define this thread's exclusive pixel row band.
void rasterize_phong(
    Framebuffer &fb,
    vec3 sa,
    vec3 sb,
    vec3 sc,
    float wa,
    float wb,
    float wc,
    vec3 pa,
    vec3 pb,
    vec3 pc,
    vec3 na,
    vec3 nb,
    vec3 nc,
    vec3 tana,
    vec3 tanb,
    vec3 tanc,
    vec2 uva,
    vec2 uvb,
    vec2 uvc,
    float aoa,
    float aob,
    float aoc,
    vec3 vcola,
    vec3 vcolb,
    vec3 vcolc,
    bool has_vcol,
    const vec3 &eye,
    const Light *lights,
    int n_lights,
    const vec3 &ambient,
    const Material &mat,
    const Texture *tex,
    const Texture *nmap,
    const Texture *stex,
    const ShadowMap *shadow_map,
    int y_min,
    int y_max,
    const Texture *mrtex =
        nullptr // glTF metallic-roughness texture; trailing+defaulted so non-metallic callers omit it
);
