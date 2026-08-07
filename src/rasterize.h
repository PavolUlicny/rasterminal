#pragma once

#include "framebuffer.h"
#include "light.h"
#include "linalg.h"
#include "texture.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <vector>

// A clip-space vertex bundled with the world-space attributes needed for
// lighting and near-plane clipping.

struct ClipVert // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — ao is always set from vertex.ao
                // before use
{
    vec4 c;       // clip-space position (w = -z_view; > 0 means in front of camera)
    vec3 pos;     // world-space position
    vec3 normal;  // world-space normal
    vec3 tangent; // world-space tangent (for TBN normal mapping)
    vec2 uv;      // texture coordinates (TEXCOORD_0)
    float ao;
    vec3 color = { 1.0f, 1.0f,
                   1.0f };     // vertex color (white = no tint); at end so existing aggregate inits keep working
    float color_a = 1.0f;      // per-vertex opacity (COLOR_0/PLY alpha); 1 = opaque; only read by the transparent pass
    vec2 uv1 = { 0.0f, 0.0f }; // second texture coordinates (glTF TEXCOORD_1); appended last (like color/color_a)
                               // so inserting it does NOT shift the offsets of the hot fields (pos/normal/color/ao)
                               // that the per-vertex lit paths read repeatedly — keeping their codegen baseline-
                               // identical. Only read when the material binds a uv_set==1 texture; == {} otherwise.
};

// Transparency: A-buffer
// Selects the rasterizer's per-pixel sink at compile time: Opaque commits to the
// framebuffer (depth-CAS); Transparent pushes the shaded fragment onto a per-pixel
// linked list for the later back-to-front resolve. The Opaque instantiation is
// codegen-identical to the pre-transparency rasterizer (the Transparent branches
// compile out).
enum class Sink : std::uint8_t
{
    Opaque,
    Transparent
};

// One shaded transparent fragment. Float color (not 8-bit) so deep stacks composite
// without per-layer banding; clamped once at resolve. `next` links within a worker's
// arena, encoded as (worker_id << 32) | node_index; ABuffer::SENTINEL ends the chain.
struct Fragment // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — always aggregate-initialized
                // (ABuffer::push); never default-constructed
{
    float depth;
    vec3 color;
    float alpha;
    uint64_t next;
};

// Screen-space bounding box of the pixels a worker actually wrote a transparent
// fragment to, accumulated during the Transparent pass and merged after the barrier
// so the Resolve pass sweeps only the transparent region instead of the whole frame.
// Inverted init = empty; all ops are nothrow int min/max (kept off the bad_alloc paths).
struct TouchBox
{
    // Default = empty; every use is a freshly constructed box (per-worker local or the merge
    // accumulator), so the inverted-init NSDMI is the only state setter — no reset() needed.
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;

    [[nodiscard]] bool empty() const noexcept { return x1 < x0; }
};

// Per-pixel linked-list A-buffer handle handed to the Transparent rasterizer. `head`
// is the framebuffer-sized array of per-pixel chain heads (shared across workers);
// `nodes` is THIS worker's private arena (no cross-worker contention on append). Only
// head[idx].exchange is shared, and it is uncontended except where two workers cover
// the same pixel in one frame. `box` accumulates this worker's touched-pixel extent.
struct ABuffer
{
    static constexpr uint64_t SENTINEL = ~static_cast<uint64_t>(0);

    std::atomic<uint64_t> *head = nullptr;
    std::vector<Fragment> *nodes = nullptr;
    TouchBox *box = nullptr;
    uint32_t worker_id = 0;

    // Append a shaded fragment for pixel `idx` at screen (x,y). The order is load-bearing:
    // push_back first (a bad_alloc then leaves `head` untouched, no chain pointing at a
    // missing node; the worker loop catches it and stops), then publish the node by swapping
    // it to the chain head and linking the previous head as its successor, then the nothrow
    // box update, so the invariant "head set ⇒ pixel
    // inside box" holds even if push_back throws. const: only the pointees (head array, node
    // arena, box) are mutated, not ABuffer's own members.
    //
    // Keep the box update PER-PUSH and post-publish; do not hoist/batch it per-triangle or
    // per-scanline even though that looks cheaper (O(tris) instead of O(frags)): a deferred
    // flush is exactly the code a mid-triangle bad_alloc skips, stranding that triangle's
    // already-published heads OUTSIDE the merged box, which the box-bounded resolve then never
    // resets, so the next frame reads stale non-SENTINEL heads. The only exception-safe hoist
    // (pre-loop clamped tri AABB) loosens the box and loses the occluded-skip; the per-push
    // cost benchmarked neutral anyway.
    void push(size_t idx, int x, int y, float depth, const vec3 &color, float alpha) const
    {
        const auto my_idx = static_cast<uint32_t>(nodes->size());
        nodes->push_back(Fragment{ depth, color, alpha, SENTINEL });
        const uint64_t my_ref = (static_cast<uint64_t>(worker_id) << 32u) | my_idx;
        const uint64_t prev = head[idx].exchange(my_ref, std::memory_order_relaxed);
        (*nodes)[my_idx].next = prev;
        box->x0 = std::min(box->x0, x);
        box->x1 = std::max(box->x1, x);
        box->y0 = std::min(box->y0, y);
        box->y1 = std::max(box->y1, y);
    }
};

// Rasterization primitives

// Clip triangle (a,b,c) against the near plane w = near_w.
// Produces 0 (fully behind), 1, or 2 output triangles written into out[0..n-1].
// Returns the count. Winding order is preserved for all outputs.
[[nodiscard]] int clip_near(const ClipVert &a, const ClipVert &b, const ClipVert &c, ClipVert out[2][3], float near_w);

// DDA line rasterizer with per-pixel depth testing (wireframe).
void draw_line(Framebuffer &fb, vec3 a, vec3 b, Color color);

// Rasterize one triangle with pre-computed per-vertex base colours (Flat shading and
// the unlit path).
// y_min/y_max define this thread's exclusive pixel row band.
// Sink::Transparent uses sample_rgba for the diffuse (alpha = base_alpha * tex.a *
// vertex-alpha), tests depth <= against the opaque buffer (decals visible), applies a
// top-left fill rule, and pushes to *abuf instead of committing; Opaque is unchanged.
template <Sink S = Sink::Opaque>
void rasterize_flat(
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
    vec2 uva,
    vec2 uvb,
    vec2 uvc,
    const Texture *tex,
    float alpha_cutoff,
    int y_min,
    int y_max,
    const Texture *etex = nullptr,            // emissive texture (modulates emissive factor)
    vec3 emissive = vec3{ 0.0f, 0.0f, 0.0f }, // emissive factor (added post-color)
    vec2 uv1a = {},                           // second UV set (TEXCOORD_1) at a/b/c; sampled when a slot selects it
    vec2 uv1b = {},
    vec2 uv1c = {},
    const Material *mat = nullptr, // per-slot uv_set (and future texture-transform) source; null ⇒ all set 0
    const ABuffer *abuf = nullptr, // Transparent only: per-pixel fragment sink
    float base_alpha = 1.0f,       // Transparent only: material base opacity (mat.alpha)
    float caa = 1.0f,              // Transparent only: per-vertex opacity at a/b/c
    float cab = 1.0f,
    float cac = 1.0f
);

// Rasterize one triangle with per-pixel Blinn-Phong lighting (Phong shading).
// y_min/y_max define this thread's exclusive pixel row band.
// Sink::Transparent behaves as in rasterize_flat() (alpha = mat.alpha * tex.a * vertex-alpha,
// depth <= test, top-left fill rule, push to *abuf); Opaque is unchanged.
template <Sink S = Sink::Opaque>
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
    int y_min,
    int y_max,
    const Texture *mrtex =
        nullptr, // glTF metallic-roughness texture; trailing+defaulted so non-metallic callers omit it
    const Texture *etex = nullptr,            // emissive texture; modulates the emissive factor when present
    vec3 emissive = vec3{ 0.0f, 0.0f, 0.0f }, // emissive factor (gated by caller, not read from mat)
    bool apply_normal_scale = false,          // gates the glTF normalScale per-pixel multiply (Mesh::has_normal_scale)
    const Texture *octex = nullptr,  // glTF occlusion texture; R channel overrides baked AO (Mesh::has_occlusion)
    float occlusion_strength = 1.0f, // occlusionTexture.strength: ao = 1 + strength*(R-1)
    vec2 uv1a = {},                  // second UV set (TEXCOORD_1) at a/b/c; per-slot uv_set read from mat
    vec2 uv1b = {},
    vec2 uv1c = {},
    const ABuffer *abuf = nullptr, // Transparent only: per-pixel fragment sink
    float caa = 1.0f,              // Transparent only: per-vertex opacity at a/b/c (mat.alpha read from mat)
    float cab = 1.0f,
    float cac = 1.0f
);
