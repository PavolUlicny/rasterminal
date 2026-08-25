#pragma once

#include "src/math/light.h"
#include "src/math/linalg.h"
#include "src/render/texture.h"
#include "src/terminal/framebuffer.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <vector>

struct ClipVert // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): ao is always set from vertex.ao
                // before use
{
    vec4 c;       // clip-space position (w = -z_view; > 0 means in front of camera)
    vec3 pos;     // world-space position
    vec3 normal;  // world-space normal
    vec3 tangent; // world-space tangent (for TBN normal mapping)
    vec2 uv;      // texture coordinates (TEXCOORD_0)
    float ao;
    // Keep optional fields last: renderer.cpp aggregate-initializes ClipVert by position,
    // and moving them ahead of the hot fields changes the lit-path layout.
    vec3 color = { 1.0f, 1.0f, 1.0f }; // white means no vertex tint
    float color_a = 1.0f;
    vec2 uv1 = { 0.0f, 0.0f };
};

// Compile-time pixel destination; unused sink branches compile out.
enum class Sink : std::uint8_t
{
    Opaque,
    Transparent,
    Deferred // tiled opaque path: shade only the pixels a prior visibility pass assigned to this triangle
};

// Visibility data for a tile owned by one worker.
struct TileVis
{
    static constexpr uint32_t NONE = ~static_cast<uint32_t>(0);

    const uint32_t *ids = nullptr;
    uint32_t id = 0;
    int x_min = 0, x_max = 0, y_min = 0, y_max = 0; // tile pixel bounds, inclusive
    int stride = 0;
};

// Float color avoids cumulative quantization in deep transparent stacks.
struct Fragment // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): always aggregate-initialized
                // by ABuffer::push
{
    float depth;
    vec3 color;
    float alpha;
    uint64_t next;
};

// Inverted bounds represent an empty transparent region.
struct TouchBox
{
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;

    [[nodiscard]] bool empty() const noexcept { return x1 < x0; }
};

// Per-worker bump arena. Fragment references use indices and survive vector growth.
struct FragArena
{
    std::vector<Fragment> buf;
    uint32_t n = 0; // live fragments this frame; buf.size() is the high-water capacity

    void clear() noexcept { n = 0; }
    [[nodiscard]] uint32_t size() const noexcept { return n; }
    Fragment &operator[](uint32_t i) noexcept { return buf[i]; }

    // Allocate before publishing a fragment into the shared head array.
    void reserve_one()
    {
        if (n == buf.size())
        {
            buf.resize(buf.empty() ? 1024 : buf.size() * 2);
        }
    }
};

// Shared pixel heads plus this worker's private fragment arena and touched bounds.
struct ABuffer
{
    static constexpr uint64_t SENTINEL = ~static_cast<uint64_t>(0);

    std::atomic<uint64_t> *head = nullptr;
    FragArena *nodes = nullptr;
    TouchBox *box = nullptr;
    uint32_t worker_id = 0;

    // Widen before publishing any triangle fragment so exceptions cannot strand a head
    // outside the resolve region. Do this once per triangle, not per fragment.
    void widen(int x0, int x1, int y0, int y1) const noexcept
    {
        box->x0 = std::min(box->x0, x0);
        box->x1 = std::max(box->x1, x1);
        box->y0 = std::min(box->y0, y0);
        box->y1 = std::max(box->y1, y1);
    }

    // Reserve, publish the new head, write its successor, then expose the arena entry.
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

// Return the pixel centres covered by [lo, hi], clipped to the frame. EPS reconciles
// bounding-box and barycentric rounding so the early rejection cannot create holes.
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

// Reject triangles whose screen bounds contain no pixel centre.
inline bool tri_covers_no_pixel(vec3 sa, vec3 sb, vec3 sc, int width, int height) noexcept
{
    int p0 = 0;
    int p1 = 0;
    return !pixel_span(std::min({ sa.x, sb.x, sc.x }), std::max({ sa.x, sb.x, sc.x }), width, p0, p1) ||
           !pixel_span(std::min({ sa.y, sb.y, sc.y }), std::max({ sa.y, sb.y, sc.y }), height, p0, p1);
}

// Clip against w = near_w, preserving winding and producing zero to two triangles.
[[nodiscard]] int clip_near(const ClipVert &a, const ClipVert &b, const ClipVert &c, ClipVert out[2][3], float near_w);

// DDA line rasterizer with per-pixel depth testing (wireframe).
void draw_line(Framebuffer &fb, vec3 a, vec3 b, Color color);

// Rasterize precomputed Flat or unlit colors within the caller's row band.
// Transparent combines material, texture, and vertex alpha into the A-buffer.
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
    const Material *mat = nullptr, // per-slot UV mapping; null selects UV0
    const ABuffer *abuf = nullptr, // Transparent only: per-pixel fragment sink
    float base_alpha = 1.0f,       // Transparent only: material base opacity (mat.alpha)
    float caa = 1.0f,              // Transparent only: per-vertex opacity at a/b/c
    float cab = 1.0f,
    float cac = 1.0f,
    const TileVis *vis = nullptr // Deferred only: tile visibility handle
);

// Rasterize one triangle with per-pixel Blinn-Phong lighting in the caller's row band.
// Transparent uses material, texture, and vertex alpha, a <= depth test, top-left
// fill, and the A-buffer; Opaque commits directly.
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

// Record this triangle as the nearest visible id at each covered tile pixel.
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
