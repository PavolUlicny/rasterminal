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
    Transparent,
    Deferred // tiled opaque path: shade only the pixels a prior visibility pass assigned to this triangle
};

// Tile handle for the Deferred sink and the visibility pass that feeds it. `ids` is the
// tile-local visibility buffer (row-major from (x_min, y_min), `stride` per row): the index of
// the tile-list triangle nearest at each pixel, or NONE. A Deferred rasterization shades
// exactly the pixels whose id equals `id`, writes them to the framebuffer with plain stores
// (the tile is owned by one worker) and needs no depth test.
struct TileVis
{
    static constexpr uint32_t NONE = ~static_cast<uint32_t>(0);

    const uint32_t *ids = nullptr;
    uint32_t id = 0;
    int x_min = 0, x_max = 0, y_min = 0, y_max = 0; // tile pixel bounds, inclusive
    int stride = 0;
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

// One worker's fragment store: a bump index over a block whose capacity persists across frames,
// so a steady-state push is a bounds branch, one 32-byte store and an increment. It exists rather
// than a plain vector so that reserving room and publishing the node are SEPARATE steps: the only
// throwing call is then hoisted above the head swap, which lets push write the node once (with its
// successor already in it) instead of writing it, swapping, and coming back to patch `next`.
// Indexing stays valid across a grow because references are (worker, index) pairs, never pointers.
struct FragArena
{
    std::vector<Fragment> buf;
    uint32_t n = 0; // live fragments this frame; buf.size() is the high-water capacity

    void clear() noexcept { n = 0; }
    [[nodiscard]] uint32_t size() const noexcept { return n; }
    Fragment &operator[](uint32_t i) noexcept { return buf[i]; }

    // Make room for one more fragment. The only allocating step, so callers run it before they
    // publish anything. Growth is geometric through resize, which value-initializes the new tail
    // once per grow; steady state never reaches it.
    void reserve_one()
    {
        if (n == buf.size())
        {
            buf.resize(buf.empty() ? 1024 : buf.size() * 2);
        }
    }
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
    FragArena *nodes = nullptr;
    TouchBox *box = nullptr;
    uint32_t worker_id = 0;

    // Widen the touched-pixel box to cover [x0, x1] x [y0, y1]. Callers widen for a whole TRIANGLE
    // before pushing any of its fragments, which is what keeps the invariant "head set => pixel
    // inside box" true even if a push throws part-way: the box is already wide enough for every
    // head that triangle can publish. A box larger than the pixels actually written is harmless
    // (the extra pixels hold SENTINEL and the resolve skips them), whereas a box too small strands
    // published heads outside the resolve's sweep, which then never resets them, and the next frame
    // reads stale non-SENTINEL heads.
    //
    // Per triangle, not per push: as a per-fragment update this cost 22 ms of a 68 ms frame on a
    // 32-pane glass stack at 1080p, since four min/max through a pointer the compiler must assume
    // aliases the fragment arena is not free ten million times a frame.
    void widen(int x0, int x1, int y0, int y1) const noexcept
    {
        box->x0 = std::min(box->x0, x0);
        box->x1 = std::max(box->x1, x1);
        box->y0 = std::min(box->y0, y0);
        box->y1 = std::max(box->y1, y1);
    }

    // Append a shaded fragment for pixel `idx`. The order is load-bearing: reserve first (a
    // bad_alloc then leaves `head` untouched, so no chain points at a missing node; the worker loop
    // catches it and stops), then swap this node to the chain head, which hands back the successor
    // to store WITH it, and only then bump. Writing the node after the swap rather than before is
    // what makes it a single store: the earlier spelling stored the fragment with a SENTINEL
    // successor and came back to patch `next` once the swap returned the real one. const: only the
    // pointees (head array, node arena) are mutated, not ABuffer's own members.
    void push(size_t idx, float depth, const vec3 &color, float alpha) const
    {
        nodes->reserve_one();
        const uint32_t my_idx = nodes->size();
        const uint64_t my_ref = (static_cast<uint64_t>(worker_id) << 32u) | my_idx;
        const uint64_t prev = head[idx].exchange(my_ref, std::memory_order_relaxed);
        (*nodes)[my_idx] = Fragment{ depth, color, alpha, prev };
        nodes->n = my_idx + 1;
    }
};

// Rasterization primitives

// Pixel span of the screen interval [lo, hi] clipped to [0, limit - 1], written to p0/p1;
// false when it holds no pixel at all. Pixel x is sampled at x + 0.5, so a centre falls in
// [lo, hi] only for ceil(lo - 0.5) <= x <= floor(hi - 0.5). EPS widens that by a fraction of a
// pixel: this and the rasterizer's barycentric coverage test are different arithmetic, and a
// pixel excluded here that the walk would have covered is a hole in the image. 1/256 of a pixel
// is orders of magnitude above the disagreement either can produce (the walk's own worst case
// is ~1e-7 of the bounding box width) and still excludes nearly everything worth excluding,
// since the gaps between sub-pixel triangles average most of a pixel.
//
// The comparisons stay in float until both ends are clamped into the frame, so a vertex
// projected far off screen (a near-plane straddle survives clip_reject) never reaches an int
// conversion. Truncation is floor for the non-negative values that do.
inline bool pixel_span(float lo, float hi, int limit, int &p0, int &p1) noexcept
{
    constexpr float EPS = 1.0f / 256.0f;
    const float a = std::max(0.0f, lo - 0.5f - EPS);
    const float b = std::min(static_cast<float>(limit - 1), hi - 0.5f + EPS);
    if (!(a <= b))
    {
        return false;
    }
    const auto ia = static_cast<int>(a);
    p0 = ia + static_cast<int>(a > static_cast<float>(ia));
    p1 = static_cast<int>(b);
    return p0 <= p1;
}

// True when no pixel centre inside the frame can lie in the triangle's screen bounding box, so
// every rasterizer below would set up and then shade nothing. Both geometry front-ends run it
// before doing any per-vertex attribute work, which is where it pays: a mesh with far more
// triangles than pixels spends most of its time on triangles that shade nothing at all
// (measured 93% of them on a 10-million-triangle sphere at 1080p, 92% on a 69k bunny at
// 200x120). Both passes reject on it so that the area statistic behind the path choice
// describes the same population either way.
inline bool tri_covers_no_pixel(vec3 sa, vec3 sb, vec3 sc, int width, int height) noexcept
{
    int p0 = 0;
    int p1 = 0;
    return !pixel_span(std::min({ sa.x, sb.x, sc.x }), std::max({ sa.x, sb.x, sc.x }), width, p0, p1) ||
           !pixel_span(std::min({ sa.y, sb.y, sc.y }), std::max({ sa.y, sb.y, sc.y }), height, p0, p1);
}

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
    float cac = 1.0f,
    const TileVis *vis = nullptr // Deferred only: tile visibility handle
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
    float cac = 1.0f,
    const TileVis *vis = nullptr // Deferred only: tile visibility handle
);

// Visibility pass of the tiled opaque path: rasterize one triangle's coverage into the tile's
// depth + id buffers (nearest wins, strict <, the Opaque sink's edge test), honouring the alpha
// cutout when `tex` and `alpha_cutoff` are set (the sample uses `slot` for the UV set and
// KHR_texture_transform, with uv1* as the second set). `depth` and `ids` are laid out like
// TileVis::ids; the triangle claims pixels under vis.id.
void raster_visibility(
    vec3 sa,
    vec3 sb,
    vec3 sc,
    float wa,
    float wb,
    float wc,
    const TileVis &vis,
    float *depth,
    uint32_t *ids,
    const Texture *tex,
    float alpha_cutoff,
    const TexSlot *slot,
    vec2 uva,
    vec2 uvb,
    vec2 uvc,
    vec2 uv1a = {},
    vec2 uv1b = {},
    vec2 uv1c = {}
);
