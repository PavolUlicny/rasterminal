#include "src/render/renderer.h"
#include "src/loaders/mesh.h"
#include "src/math/light.h"
#include "src/math/linalg.h"
#include "src/render/camera.h"
#include "src/render/clip.h"
#include "src/render/rasterize.h"
#include "src/render/texture.h"
#include "src/terminal/color.h"
#include "src/terminal/framebuffer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <ratio>
#include <thread>
#include <utility>
#include <vector>

// internal helpers

namespace
{

    // NDC → screen-space pixel coordinates.
    // NDC x/y ∈ [-1,1]; y is flipped (NDC +1 = top, screen y=0 = top).
    // z is kept as NDC depth for the z-buffer.
    constexpr vec3 ndc_to_screen(vec3 ndc, int width, int height) noexcept
    {
        return { (ndc.x + 1.0f) * 0.5f * static_cast<float>(width), (1.0f - ndc.y) * 0.5f * static_cast<float>(height),
                 ndc.z };
    }

    // Choose a conservative dynamic chunk size for Phase 1 work stealing.
    // This keeps enough claims per worker for balance while bounding overhead.
    constexpr int choose_phase1_chunk(int total_tris, int n_workers) noexcept
    {
        constexpr int MIN_CHUNK = 64;
        constexpr int MAX_CHUNK = 256;
        constexpr int TARGET_CLAIMS_PER_WORKER = 12;

        if (total_tris <= 0 || n_workers <= 0)
        {
            return MIN_CHUNK;
        }

        const int denom = n_workers * TARGET_CLAIMS_PER_WORKER;
        const int raw = (total_tris + denom - 1) / denom; // ceil(total/denom)
        return std::clamp(raw, MIN_CHUNK, MAX_CHUNK);
    }

    // Tiled opaque path thresholds (see render()). The predictor has to answer how big the triangle
    // under a shaded pixel is: the tiled pass rasterizes every triangle twice (visibility, then the
    // deferred gather) and shades in batches, so it wins on large triangles and loses badly on
    // pixel-sized ones (2.5x slower on a 38k-triangle mesh averaging 1.6 px, 1.9x faster on a 1.8k
    // one at 348 px). The statistic is the PIXEL-WEIGHTED mean area, sum(area^2)/sum(area), not the
    // plain mean: Sponza is bimodal (wall-sized triangles plus foliage), most shaded pixels belong
    // to the large ones, and the plain mean reads 10 px there and picks the pass that is 2.7x
    // slower. TILE_KEEP_TRI_PX is hysteresis, since the estimate comes from whichever pass ran.
    //
    // Sweeping zoom on two unrelated meshes puts the crossing at 15-25 px (DamagedHelmet reads
    // 1.35x against the tiled pass at 14 px and 0.96x at 17; a greyhound 1.33x at 4 px and 0.95x
    // at 30), so entry sits at 32 for margin over a cliff that is steep on the immediate side.
    // Accepted residual: a blend-heavy scene whose opaque share is small can read a few percent
    // the other way (a glazed vase at 38 px measures 1.02x against the tiled pass). That is the
    // predictor's own error rather than this threshold's, and it predates the value: a potted
    // plant at 83 px reads 1.04x and was already over the old entry threshold.
    constexpr double TILE_MIN_TRI_PX = 32.0;  // measured crossing is 15-25; enter with margin
    constexpr double TILE_KEEP_TRI_PX = 20.0; // stay tiled down to here rather than flip per frame
    constexpr double TILE_MIN_PIXELS = 65536.0;

    // Tile edge the span statistic below assumes. Mirrors Renderer::TILE_MAX rather than reading
    // it, which is private and out of reach from this namespace; keep the two in step. Nominal on
    // purpose even so: render() halves the real edge on a small frame, but the statistic only has
    // to RANK scenes against one threshold, and a frame small enough to halve is already below
    // TILE_MIN_PIXELS and never reaches the tiled pass anyway.
    constexpr int TILE_EDGE_PX = 32;

    // Area alone is not enough: it says how BIG a triangle is, never how much of each TILE it
    // lands in it actually covers, and the tiled pass pays per (triangle, tile) -- a bin entry, a
    // sort key, a setup, and a visibility scan of the box clipped to that tile -- where the
    // immediate pass pays once per triangle. So the quantity that decides is pixels covered per
    // tile touched, area-weighted the same way the area statistic is.
    //
    // The Eiffel tower at 1080p is the case that breaks the area-only rule: thin diagonal struts
    // read a pixel-weighted mean area of 44 px, comfortably "large", but each spans about 1.5
    // tiles, so it lands at 30 px per tile and the tiled pass is 1.3x SLOWER on it. Measured over
    // 28 scene/zoom points this ranks every area-gated case correctly, with the two nearest
    // neighbours (that mesh at 30, a zoomed greyhound at 38) about 9% clear either side.
    //
    // Sampling noise is why this is a RATIO PER TILE and not the bbox fill ratio, which separates
    // the same corpus but is measured as the fill of whichever giant triangle the 1-in-8 sampling
    // happened to take: on Sponza at zoom 4 it swings 0.10 to 0.50 frame to frame and flaps the
    // path every frame (17 ms -> 208 ms). This statistic puts that same scene at 240 px per tile,
    // five times clear of the threshold, so its noise cannot reach the decision.
    constexpr double TILE_MIN_TRI_PER_TILE = 34.0;

    // Consecutive frames the statistics must ask for the other pass before render() switches to
    // it. Sized against the noise rather than the frame rate: a scene sitting exactly on a
    // threshold flips a coin each frame, so 8 in a row is a 1-in-256 event and its switches cost
    // well under a percent, while a real change (a zoom, a resize) holds its side indefinitely and
    // is followed after 8 frames, a quarter-second at the default cap.
    constexpr int PATH_SWITCH_FRAMES = 8;

    // Minimum tiles per worker before the tile edge is halved (see render()).
    constexpr int TILES_PER_WORKER = 4;

    // Screen area of a screen-space triangle, in pixels, for the path predictor: half the edge
    // cross product, which is the same quantity setup_tri divides by. Deliberately NOT the
    // bounding box (a sub-pixel triangle must weigh under one pixel, and a bbox floors it at one,
    // which is the case the predictor has to get right) and deliberately not clipped to the frame
    // (that cost more than it corrected; a triangle hanging off the edge only ever reads BIGGER,
    // and every scene that happens on is one the tiled pass already wins).
    inline float tri_screen_area(const vec3 &sa, const vec3 &sb, const vec3 &sc)
    {
        return 0.5f * std::fabs(((sb.x - sa.x) * (sc.y - sa.y)) - ((sc.x - sa.x) * (sb.y - sa.y)));
    }

    // How many TILE_EDGE_PX tiles a screen-space triangle's bounding box touches, for the
    // statistic above. Always at least one: a triangle smaller than a tile still costs the tiled
    // pass a whole tile's worth of bookkeeping, and that floor is the point. Counted from the
    // box's SIZE rather than its position, so it ignores where the tile grid happens to fall; a
    // triangle straddling a boundary really touches more, but that is a coin flip on placement
    // and the statistic wants the shape.
    inline float tri_tile_spans(const vec3 &sa, const vec3 &sb, const vec3 &sc)
    {
        const float w = std::max({ sa.x, sb.x, sc.x }) - std::min({ sa.x, sb.x, sc.x }) + 1.0f;
        const float h = std::max({ sa.y, sb.y, sc.y }) - std::min({ sa.y, sb.y, sc.y }) + 1.0f;
        const float tx = std::ceil(std::max(1.0f, w) / static_cast<float>(TILE_EDGE_PX));
        const float ty = std::ceil(std::max(1.0f, h) / static_cast<float>(TILE_EDGE_PX));
        return std::max(1.0f, tx * ty);
    }

    // One in AREA_SAMPLE drawn triangles feeds the area stats. The predictor compares a ratio of
    // two sums, so the sampling factor cancels, and a mesh dense enough for the sampling error to
    // matter is one whose triangles are pixel-sized, far from the threshold. Sampling is what
    // makes the estimate affordable: measured over every triangle it cost 5-6% of a frame on a
    // 9-million-triangle mesh, which is the exact scene the predictor exists to keep fast.
    constexpr uint32_t AREA_SAMPLE = 8;

} // namespace

// Renderer: constructor / destructor

int Renderer::resolve_thread_count(int n_threads, bool all_cores_default) noexcept
{
    // hardware_concurrency() may return 0 ("not computable"); the max floors that to
    // hw = 1, so on such platforms even an explicit -j N runs single-threaded.
    // Accepted limitation: with no hw information, honoring an arbitrary N risks
    // oversubscription, and 1 was already the pre-existing worker-pool behavior.
    const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    // Two callers, two defaults. min(hw, 4) was chosen for half-block rendering, where a frame is a
    // few tens of thousands of pixels and already finishes well inside the frame cap: more threads
    // there buy latency nobody is waiting on at about twice the total CPU, which matters on a
    // laptop. Every core is right for the other two cases. A pixel backend at native resolution
    // renders a hundred times as many pixels, where the same choice is 175 ms a frame against 73 on
    // a 645k-triangle city at 3840x2160. And MODEL LOADING is a one-shot burst the user is sitting
    // and waiting through, so there is nothing to trade latency against: all cores finish it in
    // 571 ms instead of 780 and hand the machine back sooner.
    const int fallback = all_cores_default ? hw : std::min(hw, 4);
    const int req = (n_threads < 0) ? fallback : (n_threads == 0) ? hw : n_threads;
    return std::clamp(req, 1, hw);
}

Renderer::Renderer(int n_threads) : m_n_workers(resolve_thread_count(n_threads))
{
    m_arenas.resize(static_cast<size_t>(m_n_workers));    // one transparent-fragment arena per worker
    m_touch_box.resize(static_cast<size_t>(m_n_workers)); // one touched-pixel box per worker
    m_bins.resize(static_cast<size_t>(m_n_workers));      // one binning output + tile scratch per worker
    m_trans_ms.assign(static_cast<size_t>(m_n_workers), 0.0);
    m_area.assign(static_cast<size_t>(m_n_workers), 0.0);
    m_area2.assign(static_cast<size_t>(m_n_workers), 0.0);
    m_area_span.assign(static_cast<size_t>(m_n_workers), 0.0);
    m_threads.reserve(static_cast<size_t>(m_n_workers));
    for (int t = 0; t < m_n_workers; t++)
    {
        m_threads.emplace_back(&Renderer::worker_func, this, t);
    }
}

Renderer::~Renderer()
{
    {
        const std::scoped_lock lk(m_mutex);
        m_stop = true;
        ++m_generation; // ensure workers see the stop flag
    }
    m_cv_work.notify_all();
    for (auto &th : m_threads)
    {
        th.join();
    }
}

// Single-pass geometry + rasterize over this worker's stolen triangle chunks, committing
// straight into the framebuffer; the CAS depth test makes the closest triangle win per pixel
// across threads. A narrow race between a winning depth CAS and the following color write is
// accepted: at most one wrong-coloured pixel per collision per frame, invisible interactively.
// S == Opaque covers [0, opaque_count), codegen-identical to the pre-transparency single
// pass; S == Transparent covers the blend tail [opaque_count, total) and pushes shaded
// fragments into this worker's A-buffer arena for the later per-pixel resolve. M folds the
// Flat/Phong dispatch to `if constexpr`, so each instantiation carries only its own shading
// code. raster_wireframe runs a reduced copy of this geometry front-end (steal loop, cull,
// near-plane clip, clip_reject, ndc_to_screen); keep the two in sync when changing those.

template <Sink S, ShadingMode M> void Renderer::raster_triangles(int worker_id)
{
    const Mesh *mesh = m_mesh;
    const mat4 &vp = m_vp;
    const vec3 &eye = m_eye;
    const Light *lights = m_lights;
    const int n_lights = m_n_lights;
    const vec3 &ambient = m_ambient;
    const float near_plane = m_near_plane;
    const int width = m_width;
    const int height = m_height;
    const bool do_cull = m_cull_backfaces;
    const bool show_tex = m_show_texture;
    // Texture toggle gates only the emissive texture sample. The authored factor
    // (mat.emissive) always passes through, mirroring how mat.diffuse stays in effect
    // even when diffuse_map is hidden by the toggle.
    const bool show_emissive = mesh->has_emissive && show_tex;
    // Phong-only locals: unused in the Flat instantiation (those branches compile out).
    [[maybe_unused]] const bool show_metallic = mesh->has_metallic && show_tex;
    [[maybe_unused]] const bool apply_normal_scale = mesh->has_normal_scale && show_tex;
    [[maybe_unused]] const bool show_occlusion = mesh->has_occlusion && show_tex;
    const bool mesh_has_unlit = mesh->has_unlit;
    Framebuffer *fb = m_fb;

    // Opaque steals [0, opaque_count); transparent steals the blend tail
    // [opaque_count, total). render() seeds m_tri_cursor to the matching start.
    const int total = static_cast<int>(S == Sink::Opaque ? m_opaque_count : mesh->triangles.size());
    const int work = (S == Sink::Opaque) ? total : (total - static_cast<int>(m_opaque_count));
    const vec3 *p_tans = (M == ShadingMode::Phong) ? mesh->tangents.data() : nullptr;
    const vec3 *p_vcols = mesh->has_vertex_colors ? mesh->vertex_colors.data() : nullptr;
    [[maybe_unused]] const float *p_valpha = mesh->has_vertex_alpha ? mesh->vertex_alpha.data() : nullptr;
    const vec2 *p_uv1 = mesh->has_uv1 ? mesh->uv1.data() : nullptr;

    // Transparent: this worker's private fragment arena + the shared per-pixel head
    // array. clear() keeps capacity, acting as the per-frame high-water reserve so
    // steady-state pushes never reallocate. The handle is built once (const); for
    // Opaque it stays default (null) and unused.
    if constexpr (S == Sink::Transparent)
    {
        m_arenas[static_cast<size_t>(worker_id)].clear();
    }
    // Worker-local touched-pixel box. push() updates this (L1-hot, no coherence traffic)
    // rather than m_touch_box[worker_id] directly: the per-worker boxes are cache-line
    // adjacent, so per-push writes there would false-share catastrophically under the
    // millions of pushes a high-overdraw transparent mesh generates. Merged out once below.
    // NOLINTNEXTLINE(misc-const-correctness), not const: mutated through abuf.box in push()
    [[maybe_unused]] TouchBox local_box; // default-empty (NSDMI)
    const ABuffer abuf = [&]
    {
        ABuffer a;
        if constexpr (S == Sink::Transparent)
        {
            a.head = m_frag_head.data();
            a.nodes = &m_arenas[static_cast<size_t>(worker_id)];
            a.box = &local_box;
            a.worker_id = static_cast<uint32_t>(worker_id);
        }
        return a;
    }();

    // The transparent pass sizes its own claims: a blend tail is usually a handful of large
    // surfaces, and load_model groups them by material, so a fixed 256-triangle claim hands one
    // worker nearly the whole frame (see retune_trans_chunk()).
    const int chunk = (S == Sink::Transparent) ? m_trans_chunk : choose_phase1_chunk(work, m_n_workers);
    ClipVert clipped[2][3]; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): hoisted;
                            // clip_near overwrites before read
    uint32_t drawn = 0;     // post-clip on-screen triangles this worker rasterized (paces the area sampling)
    double area = 0.0;      // summed on-screen triangle area (feeds the path choice)
    double area2 = 0.0;     // summed squared on-screen triangle area (feeds the path choice)
    double span = 0.0;      // summed area^2 / tiles touched (feeds the path choice)

    // Shade and rasterize one screen-space triangle: the unlit / Phong / Flat dispatch, shared by
    // the unclipped fast path and the near-clipped pieces. flip_normals only reaches the Flat
    // branch here (the other modes carry it in the already-flipped ClipVert normals), so it is
    // unreferenced in the Phong instantiation once `if constexpr` discards that branch: GCC and
    // Clang count the use inside a discarded branch, MSVC warns C4100 and -WX makes it fatal.
    const auto emit = [&](const ClipVert &a, const ClipVert &b, const ClipVert &c, const vec3 &sa, const vec3 &sb,
                          const vec3 &sc, const Material &mat, [[maybe_unused]] bool flip_normals)
    {
        const Texture *tex = show_tex ? mesh->tex_at(mat.diffuse_map.tex) : nullptr;
        if (mesh_has_unlit && mat.unlit)
        {
            // KHR_materials_unlit: output baseColor * vertexColor * diffuse
            // texture directly, bypassing lighting/ambient/emissive/normal/occlusion
            // regardless of the active shading mode. Reuses the flat rasterizer with
            // the raw base colour as the per-vertex colour and zero emissive. a.color
            // is {1,1,1} when the mesh has no vertex colours (set at ClipVert
            // construction), so this reduces to mat.diffuse there.
            const vec3 ua = mat.diffuse * a.color;
            const vec3 ub = mat.diffuse * b.color;
            const vec3 uc = mat.diffuse * c.color;
            if constexpr (S == Sink::Opaque)
            {
                rasterize_flat<Sink::Opaque>(
                    *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, ua, ub, uc, a.uv, b.uv, c.uv, tex,
                    show_tex ? mat.alpha_cutoff : 0.0f, 0, height - 1, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, a.uv1, b.uv1,
                    c.uv1, &mat
                );
            }
            else
            {
                rasterize_flat<Sink::Transparent>(
                    *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, ua, ub, uc, a.uv, b.uv, c.uv, tex,
                    show_tex ? mat.alpha_cutoff : 0.0f, 0, height - 1, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, a.uv1, b.uv1,
                    c.uv1, &mat, &abuf, mat.alpha, a.color_a, b.color_a, c.color_a
                );
            }
        }
        else if constexpr (M == ShadingMode::Phong)
        {
            // a.color/b.color/c.color already encode the has_vertex_colors
            // condition: they are {1,1,1} when p_vcols == nullptr (set at
            // ClipVert construction), so no ternary is needed here.
            if constexpr (S == Sink::Opaque)
            {
                rasterize_phong<Sink::Opaque>(
                    *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, a.pos, b.pos, c.pos, a.normal, b.normal, c.normal, a.tangent,
                    b.tangent, c.tangent, a.uv, b.uv, c.uv, a.ao, b.ao, c.ao, a.color, b.color, c.color,
                    mesh->has_vertex_colors, eye, lights, n_lights, ambient, mat, tex,
                    show_tex ? mesh->tex_at(mat.normal_map.tex) : nullptr,
                    show_tex ? mesh->tex_at(mat.specular_map.tex) : nullptr, 0, height - 1,
                    show_metallic ? mesh->tex_at(mat.mr_map.tex) : nullptr,
                    show_emissive ? mesh->tex_at(mat.emissive_map.tex) : nullptr, mat.emissive, apply_normal_scale,
                    show_occlusion ? mesh->tex_at(mat.occlusion_map.tex) : nullptr, mat.occlusion_strength, a.uv1,
                    b.uv1, c.uv1
                );
            }
            else
            {
                rasterize_phong<Sink::Transparent>(
                    *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, a.pos, b.pos, c.pos, a.normal, b.normal, c.normal, a.tangent,
                    b.tangent, c.tangent, a.uv, b.uv, c.uv, a.ao, b.ao, c.ao, a.color, b.color, c.color,
                    mesh->has_vertex_colors, eye, lights, n_lights, ambient, mat, tex,
                    show_tex ? mesh->tex_at(mat.normal_map.tex) : nullptr,
                    show_tex ? mesh->tex_at(mat.specular_map.tex) : nullptr, 0, height - 1,
                    show_metallic ? mesh->tex_at(mat.mr_map.tex) : nullptr,
                    show_emissive ? mesh->tex_at(mat.emissive_map.tex) : nullptr, mat.emissive, apply_normal_scale,
                    show_occlusion ? mesh->tex_at(mat.occlusion_map.tex) : nullptr, mat.occlusion_strength, a.uv1,
                    b.uv1, c.uv1, &abuf, a.color_a, b.color_a, c.color_a
                );
            }
        }
        else
        {
            // Flat shading: one compute_lighting() at the centroid, constant across
            // the triangle (M is always Flat here: Phong and unlit are handled above).
            vec3 face_n = normalize(cross(b.pos - a.pos, c.pos - a.pos));
            if (flip_normals)
            {
                face_n = face_n * -1.0f;
            }
            const vec3 fc = (a.pos + b.pos + c.pos) * (1.0f / 3.0f);
            const float face_ao = (a.ao + b.ao + c.ao) * (1.0f / 3.0f);
            const Material *flat_mat = &mat;
            Material vcol_mat;
            if (mesh->has_vertex_colors)
            {
                const vec3 face_vcol = (a.color + b.color + c.color) * (1.0f / 3.0f);
                if (face_vcol.x != 1.0f || face_vcol.y != 1.0f || face_vcol.z != 1.0f)
                {
                    vcol_mat = mat;
                    vcol_mat.diffuse = vcol_mat.diffuse * face_vcol;
                    vcol_mat.ambient = vcol_mat.ambient * face_vcol;
                    flat_mat = &vcol_mat;
                }
            }
            const vec3 col =
                compute_lighting(assume_unit, fc, face_n, eye, lights, n_lights, ambient, *flat_mat, face_ao);

            if constexpr (S == Sink::Opaque)
            {
                rasterize_flat<Sink::Opaque>(
                    *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, col, col, col, a.uv, b.uv, c.uv, tex,
                    show_tex ? mat.alpha_cutoff : 0.0f, 0, height - 1,
                    show_emissive ? mesh->tex_at(mat.emissive_map.tex) : nullptr, mat.emissive, a.uv1, b.uv1, c.uv1,
                    &mat
                );
            }
            else
            {
                rasterize_flat<Sink::Transparent>(
                    *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, col, col, col, a.uv, b.uv, c.uv, tex,
                    show_tex ? mat.alpha_cutoff : 0.0f, 0, height - 1,
                    show_emissive ? mesh->tex_at(mat.emissive_map.tex) : nullptr, mat.emissive, a.uv1, b.uv1, c.uv1,
                    &mat, &abuf, mat.alpha, a.color_a, b.color_a, c.color_a
                );
            }
        }
    };

    // Project one triangle to screen space and say whether it draws anything: rejects the
    // frustum misses and the footprints too small to hold a pixel centre, and counts and samples
    // the survivors for the path predictor. Both the unclipped fast path (which runs it ahead of
    // the attribute gather) and the near-clipped sub-triangles go through it, so `drawn` and the
    // area statistic describe the same geometry either way.
    const auto project_tri =
        [&](const vec4 &ca4, const vec4 &cb4, const vec4 &cc4, vec3 &sa, vec3 &sb, vec3 &sc) noexcept
    {
        if (clip_reject(ca4, cb4, cc4))
        {
            return false;
        }
        sa = ndc_to_screen(ca4.perspective_divide(), width, height);
        sb = ndc_to_screen(cb4.perspective_divide(), width, height);
        sc = ndc_to_screen(cc4.perspective_divide(), width, height);
        if (tri_covers_no_pixel(sa, sb, sc, width, height))
        {
            return false;
        }
        drawn++;
        if (drawn % AREA_SAMPLE == 0)
        {
            const auto a = static_cast<double>(tri_screen_area(sa, sb, sc));
            area += a;
            area2 += a * a;
            span += (a * a) / static_cast<double>(tri_tile_spans(sa, sb, sc));
        }
        return true;
    };

    // The per-fragment arena push can throw bad_alloc under extreme overdraw +
    // memory pressure. Catch at the loop boundary: flag truncation and stop pushing.
    // The worker still returns and signals completion; resolve still runs and
    // self-cleans the heads, so the next frame is uncorrupted (best-effort, never a
    // crash). Opaque never allocates here, so it runs the loop directly.
    const auto steal_loop = [&]()
    {
        while (true)
        {
            const int start = m_tri_cursor.fetch_add(chunk, std::memory_order_relaxed);
            if (start >= total)
            {
                break;
            }
            const int end = std::min(start + chunk, total);
            for (int i = start; i < end; i++)
            {
                const Triangle &tri = mesh->triangles[static_cast<size_t>(i)];
                const Vertex &va = mesh->vertices[tri.v[0]];
                const Vertex &vb = mesh->vertices[tri.v[1]];
                const Vertex &vc = mesh->vertices[tri.v[2]];

                // Pre-projection backface cull: test the world-space face plane
                // against the eye before any matrix transforms. Rejects ~half the
                // triangles of a closed mesh before clip-space work. Winding
                // assumption matches the old screen-space cull (CCW front).
                bool flip_normals = false;
                if (do_cull)
                {
                    const vec3 face_normal = cross(vb.pos - va.pos, vc.pos - va.pos);
                    if (dot(face_normal, eye - va.pos) <= 0.0f)
                    {
                        if (!mesh->has_double_sided || !mesh->mat_at(tri.material_idx).double_sided)
                        {
                            continue;
                        }
                        flip_normals = true;
                    }
                }

                const vec4 pa = vp * vec4(va.pos, 1.0f);
                const vec4 pb = vp * vec4(vb.pos, 1.0f);
                const vec4 pc = vp * vec4(vc.pos, 1.0f);
                const bool in_front = pa.w > near_plane && pb.w > near_plane && pc.w > near_plane;

                // Deciding that an unclipped triangle draws nothing needs the three clip
                // positions and nothing else, so it runs BEFORE the attribute gather and the
                // ClipVerts below, which cost several times more. On a mesh with more triangles
                // than pixels that retires most of them without ever touching a tangent.
                const auto gather = [&](ClipVert &A, ClipVert &B, ClipVert &C) noexcept
                {
                    const vec3 ta = p_tans ? p_tans[tri.v[0]] : vec3{ 0.0f, 0.0f, 0.0f };
                    const vec3 tb = p_tans ? p_tans[tri.v[1]] : vec3{ 0.0f, 0.0f, 0.0f };
                    const vec3 tc = p_tans ? p_tans[tri.v[2]] : vec3{ 0.0f, 0.0f, 0.0f };
                    const vec3 ca = p_vcols ? p_vcols[tri.v[0]] : vec3{ 1.0f, 1.0f, 1.0f };
                    const vec3 cb = p_vcols ? p_vcols[tri.v[1]] : vec3{ 1.0f, 1.0f, 1.0f };
                    const vec3 cc = p_vcols ? p_vcols[tri.v[2]] : vec3{ 1.0f, 1.0f, 1.0f };
                    const vec2 u1a = p_uv1 ? p_uv1[tri.v[0]] : vec2{};
                    const vec2 u1b = p_uv1 ? p_uv1[tri.v[1]] : vec2{};
                    const vec2 u1c = p_uv1 ? p_uv1[tri.v[2]] : vec2{};
                    // uv1 is ClipVert's trailing field, so color_a (1.0f default) is spelled out to
                    // reach it positionally; the transparent path overwrites color_a just below.
                    A = ClipVert{ pa, va.pos, va.normal, ta, va.uv, va.ao, ca, 1.0f, u1a };
                    B = ClipVert{ pb, vb.pos, vb.normal, tb, vb.uv, vb.ao, cb, 1.0f, u1b };
                    C = ClipVert{ pc, vc.pos, vc.normal, tc, vc.uv, vc.ao, cc, 1.0f, u1c };
                    if constexpr (S == Sink::Transparent)
                    {
                        if (p_valpha)
                        {
                            A.color_a = p_valpha[tri.v[0]];
                            B.color_a = p_valpha[tri.v[1]];
                            C.color_a = p_valpha[tri.v[2]];
                        }
                    }
                    if (flip_normals)
                    {
                        A.normal = A.normal * -1.0f;
                        B.normal = B.normal * -1.0f;
                        C.normal = C.normal * -1.0f;
                    }
                };

                // Unclipped fast path (the overwhelmingly common case): project and test first,
                // and gather the attributes only for what draws. clip_near would copy these three
                // vertices into clipped[] verbatim, so it is skipped along with the round trip.
                if (in_front)
                {
                    vec3 sa{};
                    vec3 sb{};
                    vec3 sc{};
                    if (!project_tri(pa, pb, pc, sa, sb, sc))
                    {
                        continue;
                    }
                    ClipVert cva;
                    ClipVert cvb;
                    ClipVert cvc;
                    gather(cva, cvb, cvc);
                    emit(cva, cvb, cvc, sa, sb, sc, mesh->mat_at(tri.material_idx), flip_normals);
                    continue;
                }

                // A vertex behind the near plane: the pieces clip_near produces interpolate every
                // attribute, so this path has to gather before it can project.
                ClipVert cva;
                ClipVert cvb;
                ClipVert cvc;
                gather(cva, cvb, cvc);
                const int n_tris = clip_near(cva, cvb, cvc, clipped, near_plane);
                for (int ti = 0; ti < n_tris; ti++)
                {
                    const ClipVert &a = clipped[ti][0];
                    const ClipVert &b = clipped[ti][1];
                    const ClipVert &c = clipped[ti][2];
                    vec3 sa{};
                    vec3 sb{};
                    vec3 sc{};
                    if (!project_tri(a.c, b.c, c.c, sa, sb, sc))
                    {
                        continue;
                    }
                    emit(a, b, c, sa, sb, sc, mesh->mat_at(tri.material_idx), flip_normals);
                }
            }
        }
    }; // end steal_loop lambda

    if constexpr (S == Sink::Transparent)
    {
        // Wall time of this worker's steal loop, for the granularity controller. Two clock reads
        // per worker per frame; the only thing that can measure the imbalance honestly, since the
        // work a claim carries is part fragment shading and part front-end over triangles that
        // cull, and no counter available here weighs those against each other.
        const auto t0 = std::chrono::steady_clock::now();
        try
        {
            steal_loop();
        }
        catch (const std::exception &) // NOLINT(bugprone-empty-catch)
        {
            // Best-effort: stop pushing this worker's fragments. The chain stays consistent
            // (push_back runs before the head swap), the worker still signals completion, and
            // resolve still composites + self-cleans, so the frame loses a few fragments
            // under extreme overdraw rather than crashing or corrupting the next frame.
        }
        // Publish the accumulated extent once. On the bad_alloc path local_box still bounds
        // exactly the heads this worker did set (push updates it only after the head swap).
        m_touch_box[static_cast<size_t>(worker_id)] = local_box;
        m_trans_ms[static_cast<size_t>(worker_id)] =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    else
    {
        steal_loop();
    }
    // Opaque only. render() sums these straight after the opaque pass to choose the NEXT frame's
    // opaque path, so a transparent write is read by nobody, and publishing the blend tail's
    // triangle sizes under the same name invites a reader to think otherwise.
    if constexpr (S == Sink::Opaque)
    {
        m_area[static_cast<size_t>(worker_id)] = area;
        m_area2[static_cast<size_t>(worker_id)] = area2;
        m_area_span[static_cast<size_t>(worker_id)] = span;
    }
}

// Pick the compile-time M instantiation of raster_triangles from the runtime shading
// mode. m_smode is a frame input (written once under the lock before workers wake),
// so this read is as safe as the other m_* reads inside raster_triangles. Wireframe runs
// its own Pass::Wireframe / raster_wireframe and never reaches dispatch_raster; the case
// below is unreachable but kept so the switch stays exhaustive over ShadingMode.

template <Sink S> void Renderer::dispatch_raster(int worker_id)
{
    switch (m_smode)
    {
    case ShadingMode::Flat:
        raster_triangles<S, ShadingMode::Flat>(worker_id);
        break;
    case ShadingMode::Phong:
        raster_triangles<S, ShadingMode::Phong>(worker_id);
        break;
    case ShadingMode::Wireframe:
        break;
    }
}

// Tiled opaque path, phase 1. Same geometry front-end as raster_triangles (steal loop, cull,
// near-plane fast path + clip_near, clip_reject, ndc_to_screen; INVARIANT: mirror any front-end
// rule change in all three copies, raster_wireframe included), but each surviving triangle is
// recorded and binned into the tiles its bbox touches instead of rasterized. Unclipped
// triangles keep only screen verts + w and re-read their attributes from the mesh in phase 2;
// the rare near-clipped ones stash their interpolated ClipVerts in this worker's arena. The
// worker ends by grouping its records per tile (counting sort over the tile rectangles), so
// phase 2 needs no further barrier or shared structure.

template <ShadingMode M> void Renderer::bin_triangles(int worker_id)
{
    const Mesh *mesh = m_mesh;
    const mat4 &vp = m_vp;
    const vec3 &eye = m_eye;
    const float near_plane = m_near_plane;
    const int width = m_width;
    const int height = m_height;
    const bool do_cull = m_cull_backfaces;
    const int tiles_x = m_tiles_x;
    const int tiles_y = m_tiles_y;
    const int n_tiles = tiles_x * tiles_y;
    const int tile = m_tile;
    const int total = static_cast<int>(m_opaque_count);
    const vec3 *p_tans = (M == ShadingMode::Phong) ? mesh->tangents.data() : nullptr;
    const vec3 *p_vcols = mesh->has_vertex_colors ? mesh->vertex_colors.data() : nullptr;
    const vec2 *p_uv1 = mesh->has_uv1 ? mesh->uv1.data() : nullptr;

    // Emptied up front, not just refilled: an allocation failure anywhere below leaves this
    // worker's bins consistent-but-empty rather than holding the previous frame's index arrays,
    // which shade_tiles would read against this frame's records (it checks tile_start's size).
    WorkerBins &wb = m_bins[static_cast<size_t>(worker_id)];
    wb.recs.clear();
    wb.clip.clear();
    wb.tile_start.clear();
    wb.sorted.clear();
    // Zeroed before the loop that fills them, not only written after it: a worker that gives up
    // part-way would otherwise leave last frame's numbers in its slots, and render() sums the
    // slots unconditionally. On a blend mesh those are the TRANSPARENT pass's, which would mix
    // two populations into the statistic the opaque path choice reads.
    m_area[static_cast<size_t>(worker_id)] = 0.0;
    m_area2[static_cast<size_t>(worker_id)] = 0.0;
    m_area_span[static_cast<size_t>(worker_id)] = 0.0;

    const int chunk = choose_phase1_chunk(total, m_n_workers);
    ClipVert clipped[2][3]; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): hoisted;
                            // clip_near overwrites before read
    double area = 0.0;      // summed on-screen triangle area (feeds the path choice)
    double area2 = 0.0;     // summed squared on-screen triangle area (feeds the path choice)
    double span = 0.0;      // summed area^2 / tiles touched (feeds the path choice)

    // Record one post-clip triangle if it can shade a pixel. The tile rectangle comes from the
    // same pixel-centre span the drop test uses, so a tile listed here always holds a pixel this
    // triangle could claim, and setup_tri (which clamps to the tile) can only narrow that.
    const auto emit = [&](const vec4 &ca, const vec4 &cb, const vec4 &cc, uint32_t tri, uint32_t clip, bool flip)
    {
        if (clip_reject(ca, cb, cc))
        {
            return;
        }
        const vec3 sa = ndc_to_screen(ca.perspective_divide(), width, height);
        const vec3 sb = ndc_to_screen(cb.perspective_divide(), width, height);
        const vec3 sc = ndc_to_screen(cc.perspective_divide(), width, height);
        int x0 = 0;
        int x1 = 0;
        int y0 = 0;
        int y1 = 0;
        if (!pixel_span(std::min({ sa.x, sb.x, sc.x }), std::max({ sa.x, sb.x, sc.x }), width, x0, x1) ||
            !pixel_span(std::min({ sa.y, sb.y, sc.y }), std::max({ sa.y, sb.y, sc.y }), height, y0, y1))
        {
            return;
        }
        RasterTri r; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): every field set below
        r.sx[0] = sa.x;
        r.sx[1] = sb.x;
        r.sx[2] = sc.x;
        r.sy[0] = sa.y;
        r.sy[1] = sb.y;
        r.sy[2] = sc.y;
        r.sz[0] = sa.z;
        r.sz[1] = sb.z;
        r.sz[2] = sc.z;
        r.w[0] = ca.w;
        r.w[1] = cb.w;
        r.w[2] = cc.w;
        r.tri = tri;
        r.clip = clip;
        r.tx0 = static_cast<uint16_t>(x0 / tile);
        r.tx1 = static_cast<uint16_t>(x1 / tile);
        r.ty0 = static_cast<uint16_t>(y0 / tile);
        r.ty1 = static_cast<uint16_t>(y1 / tile);
        r.flip = flip;
        wb.recs.push_back(r);
        if (wb.recs.size() % AREA_SAMPLE == 0)
        {
            const auto a = static_cast<double>(tri_screen_area(sa, sb, sc));
            area += a;
            area2 += a * a;
            span += (a * a) / static_cast<double>(tri_tile_spans(sa, sb, sc));
        }
    };

    while (true)
    {
        const int start = m_tri_cursor.fetch_add(chunk, std::memory_order_relaxed);
        if (start >= total)
        {
            break;
        }
        const int end = std::min(start + chunk, total);
        for (int i = start; i < end; i++)
        {
            const Triangle &tri = mesh->triangles[static_cast<size_t>(i)];
            const Vertex &va = mesh->vertices[tri.v[0]];
            const Vertex &vb = mesh->vertices[tri.v[1]];
            const Vertex &vc = mesh->vertices[tri.v[2]];

            bool flip_normals = false;
            if (do_cull)
            {
                const vec3 face_normal = cross(vb.pos - va.pos, vc.pos - va.pos);
                if (dot(face_normal, eye - va.pos) <= 0.0f)
                {
                    if (!mesh->has_double_sided || !mesh->mat_at(tri.material_idx).double_sided)
                    {
                        continue;
                    }
                    flip_normals = true;
                }
            }

            const vec4 ca = vp * vec4(va.pos, 1.0f);
            const vec4 cb = vp * vec4(vb.pos, 1.0f);
            const vec4 cc = vp * vec4(vc.pos, 1.0f);
            if (ca.w > near_plane && cb.w > near_plane && cc.w > near_plane)
            {
                emit(ca, cb, cc, static_cast<uint32_t>(i), RasterTri::CLIP_NONE, flip_normals);
                continue;
            }

            // Straddles the near plane: build full ClipVerts (attributes interpolate through the
            // clip), pre-flipping normals like the immediate pass, and park the outputs in the arena.
            const vec3 ta = p_tans ? p_tans[tri.v[0]] : vec3{ 0.0f, 0.0f, 0.0f };
            const vec3 tb = p_tans ? p_tans[tri.v[1]] : vec3{ 0.0f, 0.0f, 0.0f };
            const vec3 tc = p_tans ? p_tans[tri.v[2]] : vec3{ 0.0f, 0.0f, 0.0f };
            const vec3 cola = p_vcols ? p_vcols[tri.v[0]] : vec3{ 1.0f, 1.0f, 1.0f };
            const vec3 colb = p_vcols ? p_vcols[tri.v[1]] : vec3{ 1.0f, 1.0f, 1.0f };
            const vec3 colc = p_vcols ? p_vcols[tri.v[2]] : vec3{ 1.0f, 1.0f, 1.0f };
            const vec2 u1a = p_uv1 ? p_uv1[tri.v[0]] : vec2{};
            const vec2 u1b = p_uv1 ? p_uv1[tri.v[1]] : vec2{};
            const vec2 u1c = p_uv1 ? p_uv1[tri.v[2]] : vec2{};
            ClipVert cva = { ca, va.pos, va.normal, ta, va.uv, va.ao, cola, 1.0f, u1a };
            ClipVert cvb = { cb, vb.pos, vb.normal, tb, vb.uv, vb.ao, colb, 1.0f, u1b };
            ClipVert cvc = { cc, vc.pos, vc.normal, tc, vc.uv, vc.ao, colc, 1.0f, u1c };
            if (flip_normals)
            {
                cva.normal = cva.normal * -1.0f;
                cvb.normal = cvb.normal * -1.0f;
                cvc.normal = cvc.normal * -1.0f;
            }
            const int n_tris = clip_near(cva, cvb, cvc, clipped, near_plane);
            for (int ti = 0; ti < n_tris; ti++)
            {
                const auto base = static_cast<uint32_t>(wb.clip.size());
                wb.clip.push_back(clipped[ti][0]);
                wb.clip.push_back(clipped[ti][1]);
                wb.clip.push_back(clipped[ti][2]);
                emit(
                    clipped[ti][0].c, clipped[ti][1].c, clipped[ti][2].c, static_cast<uint32_t>(i), base, flip_normals
                );
            }
        }
    }

    m_area[static_cast<size_t>(worker_id)] = area;
    m_area2[static_cast<size_t>(worker_id)] = area2;
    m_area_span[static_cast<size_t>(worker_id)] = span;

    // Group this worker's records by tile: count per tile over each record's tile rectangle,
    // prefix-sum, scatter. Records stay in claim order within a tile.
    //
    // A throw from the assign leaves tile_start empty, which shade_tiles reads as "this worker
    // contributed nothing" and skips. The resize below is the one call that does NOT land in that
    // state: by then tile_start is already full size and holding a valid prefix sum, so a throw
    // there would pass the size check and send shade_tiles indexing an empty `sorted`. Hence the
    // guard on that call alone, which also keeps the loops out of a try block.
    wb.tile_start.assign(static_cast<size_t>(n_tiles) + 1, 0);
    for (const RasterTri &r : wb.recs)
    {
        for (int ty = r.ty0; ty <= r.ty1; ty++)
        {
            for (int tx = r.tx0; tx <= r.tx1; tx++)
            {
                wb.tile_start[(static_cast<size_t>(ty) * static_cast<size_t>(tiles_x)) + static_cast<size_t>(tx) + 1]++;
            }
        }
    }
    // Summed in uint64_t: not the uint32_t the array holds, and not size_t either, which is 32
    // bits on the ILP32 targets and would wrap just the same. The total counts TILE RECTANGLES
    // rather than triangles, so the triangle count does not bound it: long thin diagonals each
    // take a rectangle spanning much of the grid while their AREA stays small, and area is what
    // puts a frame on this path at all. A wrapped total would undersize `sorted` below and send
    // the scatter past its end, the one failure here that is silent; an unwrapped total that
    // large throws out of the resize instead. The catch below takes std::exception and not
    // bad_alloc for that: on ILP32 a vector<uint32_t> tops out at PTRDIFF_MAX/4 (536870911), well
    // under the UINT32_MAX this loop allows, and resize past max_size throws LENGTH_ERROR without
    // ever attempting an allocation. Clearing tile_start is what makes shade_tiles skip this
    // worker; letting the throw past this frame would leave the prefix sized against a `sorted`
    // still holding the previous frame's contents, and the scatter would read off its end.
    uint64_t touches = 0;
    for (int t = 0; t < n_tiles; t++)
    {
        touches += wb.tile_start[static_cast<size_t>(t) + 1];
        if (touches > UINT32_MAX)
        {
            wb.tile_start.clear();
            return;
        }
        wb.tile_start[static_cast<size_t>(t) + 1] = static_cast<uint32_t>(touches);
    }
    try
    {
        wb.sorted.resize(wb.tile_start[static_cast<size_t>(n_tiles)]);
    }
    catch (const std::exception &)
    {
        wb.tile_start.clear();
        return;
    }
    // Fill from the running cursor held in tile_start[t]; the last-processed cursor of tile t
    // ends where tile t+1 starts, so shifting restores the prefix (start of t = end of t-1).
    for (uint32_t ri = 0; ri < static_cast<uint32_t>(wb.recs.size()); ri++)
    {
        const RasterTri &r = wb.recs[ri];
        for (int ty = r.ty0; ty <= r.ty1; ty++)
        {
            for (int tx = r.tx0; tx <= r.tx1; tx++)
            {
                wb.sorted[wb.tile_start
                              [(static_cast<size_t>(ty) * static_cast<size_t>(tiles_x)) + static_cast<size_t>(tx)]++] =
                    ri;
            }
        }
    }
    for (int t = n_tiles; t > 0; t--)
    {
        wb.tile_start[static_cast<size_t>(t)] = wb.tile_start[static_cast<size_t>(t) - 1];
    }
    wb.tile_start[0] = 0;
}

// Tiled opaque path, phase 2: steal tiles; per tile gather every worker's records, run the
// visibility pass into the tile-local depth (seeded from the framebuffer, so pre-existing
// content still wins where nearer) + id buffers, then shade each triangle that owns at least
// one pixel through the Deferred sink, which writes the framebuffer directly. The shading call
// mirrors raster_triangles' dispatch (unlit / Phong / Flat) with the same arguments, so both
// paths render identically; keep them in sync.

template <ShadingMode M> void Renderer::shade_tiles(int worker_id)
{
    const Mesh *mesh = m_mesh;
    const vec3 &eye = m_eye;
    const Light *lights = m_lights;
    const int n_lights = m_n_lights;
    const vec3 &ambient = m_ambient;
    const int width = m_width;
    const int height = m_height;
    const bool show_tex = m_show_texture;
    const bool show_emissive = mesh->has_emissive && show_tex;
    [[maybe_unused]] const bool show_metallic = mesh->has_metallic && show_tex;
    [[maybe_unused]] const bool apply_normal_scale = mesh->has_normal_scale && show_tex;
    [[maybe_unused]] const bool show_occlusion = mesh->has_occlusion && show_tex;
    const bool mesh_has_unlit = mesh->has_unlit;
    Framebuffer *fb = m_fb;
    const int tiles_x = m_tiles_x;
    const int n_tiles = tiles_x * m_tiles_y;
    const int n_workers = m_n_workers;
    const vec3 *p_tans = (M == ShadingMode::Phong) ? mesh->tangents.data() : nullptr;
    const vec3 *p_vcols = mesh->has_vertex_colors ? mesh->vertex_colors.data() : nullptr;
    const vec2 *p_uv1 = mesh->has_uv1 ? mesh->uv1.data() : nullptr;

    WorkerBins &wb = m_bins[static_cast<size_t>(worker_id)];
    const int tile = m_tile;
    wb.tile_depth.resize(static_cast<size_t>(tile) * static_cast<size_t>(tile));
    wb.tile_ids.resize(static_cast<size_t>(tile) * static_cast<size_t>(tile));
    float *depth = wb.tile_depth.data();
    uint32_t *ids = wb.tile_ids.data();
    ClipVert cv[3]; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): filled per triangle before use

    while (true)
    {
        const int t = m_tile_cursor.fetch_add(1, std::memory_order_relaxed);
        if (t >= n_tiles)
        {
            break;
        }
        const auto ts = static_cast<size_t>(t);
        wb.tile_list.clear();
        for (int w = 0; w < n_workers; w++)
        {
            const WorkerBins &src = m_bins[static_cast<size_t>(w)];
            // A worker whose binning ran out of memory left its bins empty (see bin_triangles).
            if (src.tile_start.size() != static_cast<size_t>(n_tiles) + 1)
            {
                continue;
            }
            for (uint32_t k = src.tile_start[ts]; k < src.tile_start[ts + 1]; k++)
            {
                wb.tile_list.push_back((static_cast<uint64_t>(w) << 32u) | src.sorted[k]);
            }
        }
        if (wb.tile_list.empty())
        {
            continue;
        }
        // Front-to-back by nearest vertex depth: the visibility pass then rejects most later
        // pixels on the depth test, and once the tile is fully covered whole triangles are
        // skipped against the tile's farthest depth (below). Only the tie order changes.
        //
        // That tie order is NOT reproducible across runs, and deliberately so. Nearest-vertex
        // depth ties constantly (two triangles of one quad share vertices) and the entries were
        // gathered in binning-worker order, which the work stealing decides afresh each frame, so
        // a z-fight can resolve either way from one run to the next: measured one pixel of 580000
        // on a 645k-triangle city, on coplanar geometry the model itself leaves ambiguous. Adding
        // the source triangle as a tiebreak does fix it and costs 1.1-2.5% on every tiled scene
        // measured (sponza, city, helmet, 4K city), because a two-key comparator optimizes far
        // worse inside std::sort than a single float compare; that is a bad trade for a pixel
        // nobody can see, so the frame is fast rather than bit-reproducible. Note this when
        // diffing two builds' output: expect a handful of z-fighting pixels to disagree.
        wb.tile_order.resize(wb.tile_list.size());
        for (size_t li = 0; li < wb.tile_list.size(); li++)
        {
            const uint64_t ref = wb.tile_list[li];
            const RasterTri &r = m_bins[static_cast<uint32_t>(ref >> 32u)].recs[static_cast<uint32_t>(ref)];
            wb.tile_order[li] = { std::min({ r.sz[0], r.sz[1], r.sz[2] }), ref };
        }
        std::sort(
            wb.tile_order.begin(), wb.tile_order.end(),
            [](const std::pair<float, uint64_t> &a, const std::pair<float, uint64_t> &b) { return a.first < b.first; }
        );
        for (size_t li = 0; li < wb.tile_list.size(); li++)
        {
            wb.tile_list[li] = wb.tile_order[li].second;
        }

        TileVis vis;
        vis.ids = ids;
        vis.x_min = (t % tiles_x) * tile;
        vis.y_min = (t / tiles_x) * tile;
        vis.x_max = std::min(width - 1, vis.x_min + tile - 1);
        vis.y_max = std::min(height - 1, vis.y_min + tile - 1);
        vis.stride = tile;
        const int tw = vis.x_max - vis.x_min + 1;
        for (int y = vis.y_min; y <= vis.y_max; y++)
        {
            const size_t row = static_cast<size_t>(y - vis.y_min) * static_cast<size_t>(tile);
            const size_t fb_row =
                (static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(vis.x_min);
            for (int x = 0; x < tw; x++)
            {
                depth[row + static_cast<size_t>(x)] = fb->depth_at(fb_row + static_cast<size_t>(x));
                ids[row + static_cast<size_t>(x)] = TileVis::NONE;
            }
        }

        const auto n_list = static_cast<uint32_t>(wb.tile_list.size());
        wb.tile_claims.assign(n_list, 0);
        // Farthest depth in the tile: a triangle whose nearest vertex is not nearer than it cannot
        // claim a pixel (strict < test) and is skipped whole. Starts at +inf whenever any pixel is
        // still uncovered, so the skip only bites once the near geometry (sorted first) has covered
        // the tile. A stale value is only ever LARGER, so the cadence is a pure speed knob: it can
        // cost skips, never make an unsafe one.
        // Bound the refresh COUNT, not their spacing: a refresh costs a full tile scan, so even
        // spacing makes the cost scale with list length while the benefit scales with triangle
        // SIZE, and the tradeoff then reverses with density. Against a fixed 16, a fixed 64 is
        // 1.06x on audi_r8 but 0.95x on Sponza at zoom 4; this form takes both sides.
        constexpr uint32_t ZMAX_MIN_SPACING = 8;
        constexpr uint32_t ZMAX_MAX_REFRESH = 8;
        const uint32_t zmax_spacing = std::max(ZMAX_MIN_SPACING, n_list / ZMAX_MAX_REFRESH);
        float tile_zmax = std::numeric_limits<float>::infinity();
        const auto refresh_zmax = [&]
        {
            float m = -std::numeric_limits<float>::infinity();
            for (int y = 0; y <= vis.y_max - vis.y_min; y++)
            {
                const float *row = depth + (static_cast<size_t>(y) * static_cast<size_t>(tile));
                for (int x = 0; x < tw; x++)
                {
                    m = std::max(m, row[x]);
                }
            }
            tile_zmax = m;
        };
        uint32_t zmax_next = zmax_spacing - 1;
        for (uint32_t li = 0; li < n_list; li++)
        {
            if (li == zmax_next)
            {
                refresh_zmax();
                zmax_next += zmax_spacing;
            }
            if (!(wb.tile_order[li].first < tile_zmax))
            {
                continue;
            }
            const uint64_t ref = wb.tile_list[li];
            const RasterTri &r = m_bins[static_cast<uint32_t>(ref >> 32u)].recs[static_cast<uint32_t>(ref)];
            const Triangle &tri = mesh->triangles[r.tri];
            const Material &mat = mesh->mat_at(tri.material_idx);
            const float cutoff = show_tex ? mat.alpha_cutoff : 0.0f;
            const Texture *tex = (cutoff > 0.0f) ? mesh->tex_at(mat.diffuse_map.tex) : nullptr;
            vis.id = li;
            vec2 uva{};
            vec2 uvb{};
            vec2 uvc{};
            vec2 u1a{};
            vec2 u1b{};
            vec2 u1c{};
            if (tex != nullptr)
            {
                if (r.clip == RasterTri::CLIP_NONE)
                {
                    uva = mesh->vertices[tri.v[0]].uv;
                    uvb = mesh->vertices[tri.v[1]].uv;
                    uvc = mesh->vertices[tri.v[2]].uv;
                    if (p_uv1)
                    {
                        u1a = p_uv1[tri.v[0]];
                        u1b = p_uv1[tri.v[1]];
                        u1c = p_uv1[tri.v[2]];
                    }
                }
                else
                {
                    const ClipVert *c = &m_bins[static_cast<uint32_t>(ref >> 32u)].clip[r.clip];
                    uva = c[0].uv;
                    uvb = c[1].uv;
                    uvc = c[2].uv;
                    u1a = c[0].uv1;
                    u1b = c[1].uv1;
                    u1c = c[2].uv1;
                }
            }
            raster_visibility(
                { r.sx[0], r.sy[0], r.sz[0] }, { r.sx[1], r.sy[1], r.sz[1] }, { r.sx[2], r.sy[2], r.sz[2] }, r.w[0],
                r.w[1], r.w[2], vis, depth, ids, tex, cutoff, &mat.diffuse_map, uva, uvb, uvc, u1a, u1b, u1c
            );
        }
        // Visible pixel count per list entry, so fully occluded triangles skip the shade pass.
        for (int y = vis.y_min; y <= vis.y_max; y++)
        {
            const size_t row = static_cast<size_t>(y - vis.y_min) * static_cast<size_t>(tile);
            for (int x = 0; x < tw; x++)
            {
                const uint32_t id = ids[row + static_cast<size_t>(x)];
                if (id != TileVis::NONE)
                {
                    wb.tile_claims[id]++;
                }
            }
        }

        for (uint32_t li = 0; li < n_list; li++)
        {
            if (wb.tile_claims[li] == 0)
            {
                continue;
            }
            const uint64_t ref = wb.tile_list[li];
            const WorkerBins &src = m_bins[static_cast<uint32_t>(ref >> 32u)];
            const RasterTri &r = src.recs[static_cast<uint32_t>(ref)];
            const Triangle &tri = mesh->triangles[r.tri];
            const Material &mat = mesh->mat_at(tri.material_idx);
            const Texture *tex = show_tex ? mesh->tex_at(mat.diffuse_map.tex) : nullptr;
            vis.id = li;

            const ClipVert *a = nullptr;
            const ClipVert *b = nullptr;
            const ClipVert *c = nullptr;
            if (r.clip == RasterTri::CLIP_NONE)
            {
                const Vertex &va = mesh->vertices[tri.v[0]];
                const Vertex &vb = mesh->vertices[tri.v[1]];
                const Vertex &vc = mesh->vertices[tri.v[2]];
                const vec3 ta = p_tans ? p_tans[tri.v[0]] : vec3{ 0.0f, 0.0f, 0.0f };
                const vec3 tb = p_tans ? p_tans[tri.v[1]] : vec3{ 0.0f, 0.0f, 0.0f };
                const vec3 tc = p_tans ? p_tans[tri.v[2]] : vec3{ 0.0f, 0.0f, 0.0f };
                const vec3 cola = p_vcols ? p_vcols[tri.v[0]] : vec3{ 1.0f, 1.0f, 1.0f };
                const vec3 colb = p_vcols ? p_vcols[tri.v[1]] : vec3{ 1.0f, 1.0f, 1.0f };
                const vec3 colc = p_vcols ? p_vcols[tri.v[2]] : vec3{ 1.0f, 1.0f, 1.0f };
                const vec2 u1a = p_uv1 ? p_uv1[tri.v[0]] : vec2{};
                const vec2 u1b = p_uv1 ? p_uv1[tri.v[1]] : vec2{};
                const vec2 u1c = p_uv1 ? p_uv1[tri.v[2]] : vec2{};
                cv[0] = { vec4{}, va.pos, va.normal, ta, va.uv, va.ao, cola, 1.0f, u1a };
                cv[1] = { vec4{}, vb.pos, vb.normal, tb, vb.uv, vb.ao, colb, 1.0f, u1b };
                cv[2] = { vec4{}, vc.pos, vc.normal, tc, vc.uv, vc.ao, colc, 1.0f, u1c };
                if (r.flip)
                {
                    cv[0].normal = cv[0].normal * -1.0f;
                    cv[1].normal = cv[1].normal * -1.0f;
                    cv[2].normal = cv[2].normal * -1.0f;
                }
                a = &cv[0];
                b = &cv[1];
                c = &cv[2];
            }
            else
            {
                a = &src.clip[r.clip];
                b = a + 1;
                c = a + 2;
            }
            const vec3 sa{ r.sx[0], r.sy[0], r.sz[0] };
            const vec3 sb{ r.sx[1], r.sy[1], r.sz[1] };
            const vec3 sc{ r.sx[2], r.sy[2], r.sz[2] };

            if (mesh_has_unlit && mat.unlit)
            {
                const vec3 ua = mat.diffuse * a->color;
                const vec3 ub = mat.diffuse * b->color;
                const vec3 uc = mat.diffuse * c->color;
                rasterize_flat<Sink::Deferred>(
                    *fb, sa, sb, sc, r.w[0], r.w[1], r.w[2], ua, ub, uc, a->uv, b->uv, c->uv, tex,
                    show_tex ? mat.alpha_cutoff : 0.0f, 0, height - 1, nullptr, vec3{ 0.0f, 0.0f, 0.0f }, a->uv1,
                    b->uv1, c->uv1, &mat, nullptr, 1.0f, 1.0f, 1.0f, 1.0f, &vis
                );
            }
            else if constexpr (M == ShadingMode::Phong)
            {
                rasterize_phong<Sink::Deferred>(
                    *fb, sa, sb, sc, r.w[0], r.w[1], r.w[2], a->pos, b->pos, c->pos, a->normal, b->normal, c->normal,
                    a->tangent, b->tangent, c->tangent, a->uv, b->uv, c->uv, a->ao, b->ao, c->ao, a->color, b->color,
                    c->color, mesh->has_vertex_colors, eye, lights, n_lights, ambient, mat, tex,
                    show_tex ? mesh->tex_at(mat.normal_map.tex) : nullptr,
                    show_tex ? mesh->tex_at(mat.specular_map.tex) : nullptr, 0, height - 1,
                    show_metallic ? mesh->tex_at(mat.mr_map.tex) : nullptr,
                    show_emissive ? mesh->tex_at(mat.emissive_map.tex) : nullptr, mat.emissive, apply_normal_scale,
                    show_occlusion ? mesh->tex_at(mat.occlusion_map.tex) : nullptr, mat.occlusion_strength, a->uv1,
                    b->uv1, c->uv1, nullptr, 1.0f, 1.0f, 1.0f, &vis
                );
            }
            else
            {
                vec3 face_n = normalize(cross(b->pos - a->pos, c->pos - a->pos));
                if (r.flip)
                {
                    face_n = face_n * -1.0f;
                }
                const vec3 fc = (a->pos + b->pos + c->pos) * (1.0f / 3.0f);
                const float face_ao = (a->ao + b->ao + c->ao) * (1.0f / 3.0f);
                const Material *flat_mat = &mat;
                Material vcol_mat;
                if (mesh->has_vertex_colors)
                {
                    const vec3 face_vcol = (a->color + b->color + c->color) * (1.0f / 3.0f);
                    if (face_vcol.x != 1.0f || face_vcol.y != 1.0f || face_vcol.z != 1.0f)
                    {
                        vcol_mat = mat;
                        vcol_mat.diffuse = vcol_mat.diffuse * face_vcol;
                        vcol_mat.ambient = vcol_mat.ambient * face_vcol;
                        flat_mat = &vcol_mat;
                    }
                }
                const vec3 col =
                    compute_lighting(assume_unit, fc, face_n, eye, lights, n_lights, ambient, *flat_mat, face_ao);
                rasterize_flat<Sink::Deferred>(
                    *fb, sa, sb, sc, r.w[0], r.w[1], r.w[2], col, col, col, a->uv, b->uv, c->uv, tex,
                    show_tex ? mat.alpha_cutoff : 0.0f, 0, height - 1,
                    show_emissive ? mesh->tex_at(mat.emissive_map.tex) : nullptr, mat.emissive, a->uv1, b->uv1, c->uv1,
                    &mat, nullptr, 1.0f, 1.0f, 1.0f, 1.0f, &vis
                );
            }
        }
    }
}

void Renderer::dispatch_bin(int worker_id)
{
    switch (m_smode)
    {
    case ShadingMode::Flat:
        bin_triangles<ShadingMode::Flat>(worker_id);
        break;
    case ShadingMode::Phong:
        bin_triangles<ShadingMode::Phong>(worker_id);
        break;
    case ShadingMode::Wireframe:
        break;
    }
}

void Renderer::dispatch_tiles(int worker_id)
{
    switch (m_smode)
    {
    case ShadingMode::Flat:
        shade_tiles<ShadingMode::Flat>(worker_id);
        break;
    case ShadingMode::Phong:
        shade_tiles<ShadingMode::Phong>(worker_id);
        break;
    case ShadingMode::Wireframe:
        break;
    }
}

// Work-stealing wireframe pass: each worker claims triangle chunks over [0, total) and draws
// their three edges as DDA lines in m_wireframe_color; one shared colour plus draw_line's
// atomic CAS depth min make the output identical to a serial draw for any worker count. The
// geometry front-end (chunked steal loop, world-space backface cull incl. the double-sided bypass,
// near-plane fast path + clip_near straddle fallback, clip_reject, ndc_to_screen) is a
// deliberately reduced copy of raster_triangles': wireframe carries no material/texture/
// tangent/vcol/uv1 attributes and no normal flip, so the two are not worth unifying.
// INVARIANT: if a shared front-end rule changes (cull winding, near_plane semantics, chunk
// sizing), mirror it in BOTH this function and raster_triangles.

void Renderer::raster_wireframe()
{
    const Mesh *mesh = m_mesh;
    const mat4 vp = m_vp;
    const vec3 eye = m_eye;
    const float near_plane = m_near_plane;
    const int width = m_width;
    const int height = m_height;
    const bool do_cull = m_cull_backfaces;
    const Color wf = m_wireframe_color;
    Framebuffer *fb = m_fb;

    const int total = static_cast<int>(mesh->triangles.size());
    const int chunk = choose_phase1_chunk(total, m_n_workers);
    ClipVert clipped[2][3]; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): hoisted;
                            // clip_near overwrites before read

    while (true)
    {
        const int start = m_tri_cursor.fetch_add(chunk, std::memory_order_relaxed);
        if (start >= total)
        {
            break;
        }
        const int end = std::min(start + chunk, total);
        for (int i = start; i < end; i++)
        {
            const Triangle &tri = mesh->triangles[static_cast<size_t>(i)];
            const Vertex &va = mesh->vertices[tri.v[0]];
            const Vertex &vb = mesh->vertices[tri.v[1]];
            const Vertex &vc = mesh->vertices[tri.v[2]];

            if (do_cull)
            {
                const vec3 face_normal = cross(vb.pos - va.pos, vc.pos - va.pos);
                if (dot(face_normal, eye - va.pos) <= 0.0f &&
                    (!mesh->has_double_sided || !mesh->mat_at(tri.material_idx).double_sided))
                {
                    continue;
                }
            }

            const ClipVert cva = { vp * vec4(va.pos, 1.0f), va.pos, va.normal, {}, va.uv, va.ao };
            const ClipVert cvb = { vp * vec4(vb.pos, 1.0f), vb.pos, vb.normal, {}, vb.uv, vb.ao };
            const ClipVert cvc = { vp * vec4(vc.pos, 1.0f), vc.pos, vc.normal, {}, vc.uv, vc.ao };

            // Fast path: skip clip_near and its copy when no vertex is behind the near
            // plane. clip_near runs only for the rare straddle cases.
            const ClipVert *tris[2][3];
            int n_tris; // NOLINT(cppcoreguidelines-init-variables): assigned in both branches before read
            if (cva.c.w > near_plane && cvb.c.w > near_plane && cvc.c.w > near_plane)
            {
                n_tris = 1;
                tris[0][0] = &cva;
                tris[0][1] = &cvb;
                tris[0][2] = &cvc;
            }
            else
            {
                n_tris = clip_near(cva, cvb, cvc, clipped, near_plane);
                for (int ti = 0; ti < n_tris; ti++)
                {
                    tris[ti][0] = &clipped[ti][0];
                    tris[ti][1] = &clipped[ti][1];
                    tris[ti][2] = &clipped[ti][2];
                }
            }

            for (int ti = 0; ti < n_tris; ti++)
            {
                const ClipVert &a = *tris[ti][0];
                const ClipVert &b = *tris[ti][1];
                const ClipVert &c = *tris[ti][2];

                if (clip_reject(a.c, b.c, c.c))
                {
                    continue;
                }

                const vec3 sa = ndc_to_screen(a.c.perspective_divide(), width, height);
                const vec3 sb = ndc_to_screen(b.c.perspective_divide(), width, height);
                const vec3 sc = ndc_to_screen(c.c.perspective_divide(), width, height);

                draw_line(*fb, sa, sb, wf);
                draw_line(*fb, sb, sc, wf);
                draw_line(*fb, sc, sa, wf);
            }
        }
    }
}

// Transparent resolve pass: each worker steals disjoint row bands within the merged
// transparent bounding box (set by render()) and composites each pixel's fragment list
// back-to-front over the opaque colour already in the framebuffer; pixels outside the box
// were never touched (heads still SENTINEL). One- and two-deep pixels composite inline, which
// covers all but a fraction of a percent of a typical frame (6% and 93% of covered pixels on a
// 645k-triangle city at 4K) and needs neither a gather nor a sort; deeper pixels are collected
// into a group and walked together, since a chain walk is a chain of DEPENDENT loads and doing
// one pixel at a time serialises a cache miss per layer. Disjoint pixels plus the
// post-accumulate barrier make the single-threaded color_at/set_color_at safe here (no two
// workers touch one slot; the half-block 2-px-per-cell packing is a present()-only concern).
// Each resolved head is reset to SENTINEL so the array self-cleans for the next frame.

namespace
{
    // One chain entry as the resolve sorts it: the depth it orders on and the ref that finds the
    // payload. 16 bytes against the fragment's 32, so a deep chain sorts half the bytes.
    struct FragKey
    {
        float depth;
        uint64_t ref;
    };

    // Composite order for two fragments of one pixel: true when `lhs` goes first, i.e. is the
    // farther one. The depth ties break on the fragment payload so the composite is reproducible:
    // the A-buffer chain order is nondeterministic (cross-worker atomic exchanges) and alpha-OVER
    // is not commutative, so without a deterministic tie-break two coplanar fragments at one pixel
    // would flicker frame to frame. Fragments equal on every field are identical, so their relative
    // order then cannot affect the result. Shared by the two-deep fast path and the general sort so
    // the two orderings cannot drift apart.
    bool frag_before(const Fragment &lhs, const Fragment &rhs) noexcept
    {
        if (lhs.depth != rhs.depth)
        {
            return lhs.depth > rhs.depth;
        }
        if (lhs.color.x != rhs.color.x)
        {
            return lhs.color.x < rhs.color.x;
        }
        if (lhs.color.y != rhs.color.y)
        {
            return lhs.color.y < rhs.color.y;
        }
        if (lhs.color.z != rhs.color.z)
        {
            return lhs.color.z < rhs.color.z;
        }
        return lhs.alpha < rhs.alpha;
    }
} // namespace

void Renderer::resolve_pixels()
{
    Framebuffer *fb = m_fb;
    const int width = m_width;
    const int x0 = m_res_box.x0;
    const int x1 = m_res_box.x1;
    const int y1 = m_res_box.y1;
    const int chunk = m_res_row_chunk;
    constexpr float inv255 = 1.0f / 255.0f;

    // Chain state for one group of pixels resolved together; all per-worker, no sharing. The
    // chains are cleared rather than destroyed between pixels, so a pass allocates only while
    // its deepest chain so far is growing.
    constexpr int GROUP = 8;
    uint64_t cur[GROUP];
    size_t slot_px[GROUP];
    std::vector<FragKey> chains[GROUP];

    // Steal row bands over the box's rows: columns [x0,x1], from the box top (m_pixel_cursor,
    // seeded by render()) down to the last row y1. Only these rows can hold a non-SENTINEL head.
    while (true)
    {
        const int r0 = m_pixel_cursor.fetch_add(chunk, std::memory_order_relaxed);
        if (r0 > y1)
        {
            break;
        }
        const int r1 = std::min(r0 + chunk - 1, y1);
        for (int y = r0; y <= r1; y++)
        {
            const int row = y * width;
            int x = x0;
            while (x <= x1)
            {
                // Collect the next group of covered pixels, then walk their chains IN LOCKSTEP.
                // A chain walk is a dependent load per node (each fragment holds the next ref), so
                // resolving one pixel at a time serialises a cache miss per layer: a 32-pane glass
                // stack averages 17 fragments a pixel and spends the pass waiting. Stepping GROUP
                // chains together makes those loads independent, so the misses overlap.
                int gn = 0;
                while (x <= x1 && gn < GROUP)
                {
                    const size_t pidx = static_cast<size_t>(row) + static_cast<size_t>(x);
                    const uint64_t ref = m_frag_head[pidx].load(std::memory_order_relaxed);
                    x++;
                    if (ref == ABuffer::SENTINEL)
                    {
                        continue;
                    }
                    // One and two deep are resolved right here, without the group buffers: they
                    // are the overwhelming majority away from stacked glass (6% and 93% of covered
                    // pixels on a 645k-triangle city at 4K), they need no ordering work beyond a
                    // single compare, and queueing them would pay a vector push per fragment for
                    // nothing.
                    const Fragment &f0 = frag_at(ref);
                    if (f0.next != ABuffer::SENTINEL)
                    {
                        const Fragment &f1 = frag_at(f0.next);
                        if (f1.next != ABuffer::SENTINEL)
                        {
                            cur[gn] = ref;
                            slot_px[gn] = pidx;
                            chains[gn].clear();
                            gn++;
                            continue;
                        }
                        const Color base2 = fb->color_at(pidx);
                        vec3 d2{ static_cast<float>(base2.r) * inv255, static_cast<float>(base2.g) * inv255,
                                 static_cast<float>(base2.b) * inv255 };
                        const bool f0_first = frag_before(f0, f1);
                        const Fragment &far_f = f0_first ? f0 : f1;
                        const Fragment &near_f = f0_first ? f1 : f0;
                        d2 = far_f.color * far_f.alpha + d2 * (1.0f - far_f.alpha);
                        d2 = near_f.color * near_f.alpha + d2 * (1.0f - near_f.alpha);
                        fb->set_color_at(pidx, vec3_to_color(d2));
                        m_frag_head[pidx].store(ABuffer::SENTINEL, std::memory_order_relaxed);
                        continue;
                    }
                    const Color base1 = fb->color_at(pidx);
                    vec3 d1{ static_cast<float>(base1.r) * inv255, static_cast<float>(base1.g) * inv255,
                             static_cast<float>(base1.b) * inv255 };
                    d1 = f0.color * f0.alpha + d1 * (1.0f - f0.alpha);
                    fb->set_color_at(pidx, vec3_to_color(d1));
                    m_frag_head[pidx].store(ABuffer::SENTINEL, std::memory_order_relaxed);
                }
                if (gn == 0)
                {
                    continue;
                }

                // push_back is the only allocating call in resolve; guard it like the accumulate
                // pass so an OOM here cannot escape worker_func (a std::thread entry) into
                // std::terminate. On OOM the affected pixels keep their opaque colour, but the head
                // reset below still runs UNCONDITIONALLY, so no stale non-SENTINEL head survives to
                // corrupt the next frame (the self-cleaning invariant the design relies on).
                try
                {
                    bool more = true;
                    while (more)
                    {
                        more = false;
                        for (int k = 0; k < gn; k++)
                        {
                            if (cur[k] == ABuffer::SENTINEL)
                            {
                                continue;
                            }
                            const Fragment &f = frag_at(cur[k]);
                            chains[k].push_back(FragKey{ f.depth, cur[k] });
                            cur[k] = f.next;
                            more = true;
                        }
                    }
                }
                catch (const std::exception &)
                {
                    for (int k = 0; k < gn; k++)
                    {
                        chains[k].clear();
                    }
                }

                for (int k = 0; k < gn; k++)
                {
                    const size_t pidx = slot_px[k];
                    std::vector<FragKey> &chain = chains[k];
                    // Composite over the opaque colour already in the framebuffer. color_at is just
                    // a load (order-independent), so read the base first and fold into dst.
                    const Color base = fb->color_at(pidx);
                    vec3 dst{ static_cast<float>(base.r) * inv255, static_cast<float>(base.g) * inv255,
                              static_cast<float>(base.b) * inv255 };

                    // Only chains of three or more reach here (the scan above resolved the rest),
                    // except that an OOM in the walk can leave one empty.
                    if (chain.size() > 1)
                    {
                        // Back-to-front, sorted as (depth, ref) keys rather than as fragments: the
                        // payload is 32 bytes and a sort moves it O(n log n) times, while the
                        // composite below dereferences each fragment exactly once.
                        std::sort(
                            chain.begin(), chain.end(),
                            [this](const FragKey &lhs, const FragKey &rhs)
                            {
                                // Depth decides all but the coplanar case, which falls back to the
                                // payload compare so the order stays reproducible (frag_before).
                                if (lhs.depth != rhs.depth)
                                {
                                    return lhs.depth > rhs.depth;
                                }
                                return frag_before(frag_at(lhs.ref), frag_at(rhs.ref));
                            }
                        );
                        for (const FragKey &key : chain)
                        {
                            const Fragment &f = frag_at(key.ref);
                            dst = f.color * f.alpha + dst * (1.0f - f.alpha);
                        }
                    }

                    fb->set_color_at(pidx, vec3_to_color(dst));
                    m_frag_head[pidx].store(ABuffer::SENTINEL, std::memory_order_relaxed);
                }
            }
        }
    }
}

// Persistent worker loop: sleep until render() dispatches a phase, run it, signal done.

void Renderer::dispatch_pass(Pass pass)
{
    {
        const std::scoped_lock lk(m_mutex);
        m_pass = pass;
        m_active.store(m_n_workers, std::memory_order_release);
        ++m_generation;
    }
    m_cv_work.notify_all();
    std::unique_lock<std::mutex> lk(m_mutex);
    m_cv_done.wait(lk, [this] { return m_active.load(std::memory_order_acquire) == 0; });
}

void Renderer::run_on_workers(const std::function<void(int, int)> &fn)
{
    // The pointer is only read by workers woken by this dispatch, which have all
    // returned before it is cleared, so borrowing the caller's callable is safe and
    // saves copying it into a member every present.
    m_task = &fn;
    dispatch_pass(Pass::Task);
    m_task = nullptr;
}

void Renderer::worker_func(int worker_id)
{
    int my_gen = 0;
    while (true)
    {
        Pass pass; // NOLINT(cppcoreguidelines-init-variables): assigned under the lock below before use
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv_work.wait(lk, [this, my_gen] { return m_generation != my_gen || m_stop; });
            if (m_stop)
            {
                return;
            }
            my_gen = m_generation;
            pass = m_pass;
        }

        // Only Task and Tiled catch at this level; TransAccum and Resolve guard their own
        // allocating calls further in. Opaque and Wireframe are unguarded because nothing they
        // reach allocates at all: rasterize.cpp itself holds no vector, no new and no throw, and
        // the one growing container the rasterizer headers do hold (FragArena) belongs to the
        // A-buffer, which only the Transparent sink touches. Treat that as an invariant rather
        // than a coincidence, since this is a thread entry point and giving the opaque rasterizer
        // a heap scratch buffer would turn an OOM into terminate instead of the lost geometry
        // every guarded pass degrades to.
        switch (pass)
        {
        case Pass::Opaque:
            dispatch_raster<Sink::Opaque>(worker_id);
            break;
        case Pass::Wireframe:
            raster_wireframe();
            break;
        case Pass::TransAccum:
            dispatch_raster<Sink::Transparent>(worker_id);
            break;
        case Pass::Resolve:
            resolve_pixels();
            break;
        case Pass::Task:
            // Same reasoning as the tiled pass below: this is a thread entry point, so a
            // throw must not escape. The task reports its own failures through storage the
            // caller owns, so swallowing here loses no information.
            try
            {
                (*m_task)(worker_id, m_n_workers);
            }
            catch (const std::exception &) // NOLINT(bugprone-empty-catch)
            {
            }
            break;
        case Pass::Tiled:
            // Both phases allocate (the bins, and the per-tile scratch), and this is a thread
            // entry point, so nothing may escape. std::exception rather than bad_alloc because a
            // vector growth past max_size throws length_error instead, which ILP32 reaches long
            // before it runs out of memory. Each catch is INSIDE its phase: a throw
            // that skipped the barrier below would hang every other worker, and one that skipped
            // m_active would hang render(). A worker that gives up leaves its bins empty or its
            // tiles unshaded, so the frame loses geometry rather than crashing, and the next frame
            // is unaffected (nothing persists but capacity).
            try
            {
                dispatch_bin(worker_id);
            }
            catch (const std::exception &) // NOLINT(bugprone-empty-catch)
            {
            }
            // In-pass barrier: tiles may only be shaded once every worker's bins are complete.
            // A spin (with yield) rather than a second condition-variable dispatch: the whole
            // pool is awake and about to continue, so the wake-up round trip would only add
            // latency, which dominates small frames (measured on Duck at 200x120).
            if (m_bin_done.fetch_add(1, std::memory_order_acq_rel) + 1 < m_n_workers)
            {
                while (m_bin_done.load(std::memory_order_acquire) < m_n_workers)
                {
                    std::this_thread::yield();
                }
            }
            try
            {
                dispatch_tiles(worker_id);
            }
            catch (const std::exception &) // NOLINT(bugprone-empty-catch)
            {
            }
            break;
        }

        // Signal completion. If this is the last worker, wake render().
        if (m_active.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            const std::scoped_lock done_lk(m_mutex);
            m_cv_done.notify_one();
        }
    }
}

// Resize the transparent pass's steal granularity from the imbalance the last one produced.
//
// A blend tail is a handful of large surfaces far more often than a field of small ones (glass,
// decals, water, foliage cards), and load_model partitions the tail by material, so those surfaces
// sit CONTIGUOUS in it: at the opaque pass's 256-triangle claim one worker takes nearly the whole
// frame's fragment work while the rest idle. Measured on a 645k-triangle city at 1080p, where 1000
// of 59000 drawn transparent triangles carry 96.5% of the fragments, the busiest worker held 317k
// of 1.06M and the pass ran 41 ms against a 17 ms balanced ideal.
//
// A smaller claim is not simply better: the steal range covers every tail triangle including the
// ones that cull, and the claim is a contended fetch_add (~14 ns, serialized across the pool), so
// on a 10M-triangle all-blend mesh chunk 16 costs 76% over chunk 256 while buying no balance at
// all (its work is already even). Nothing computable before the pass separates those two cases:
// triangle counts, fragment counts and screen areas were each tried as a predictor and each reads
// the wrong way round on one of them, because a claim's cost is part shading and part front-end
// over culled triangles and the mix is what differs. The imbalance itself is the one quantity that
// says which regime the frame is in, so the controller measures it and steers, rather than
// predicting it: shrink proportionally when the pool is uneven, double back up when it is even,
// and hold inside the band so a settled scene stops moving.
void Renderer::retune_trans_chunk()
{
    double sum = 0.0;
    double peak = 0.0;
    for (int w = 0; w < m_n_workers; w++)
    {
        const double ms = m_trans_ms[static_cast<size_t>(w)];
        sum += ms;
        peak = std::max(peak, ms);
    }
    // Too short to measure: the pass cost nothing, so its granularity does not matter and a clock
    // this coarse would only inject noise into the next frame's choice.
    constexpr double MIN_MEASURABLE_MS = 0.05;
    if (peak < MIN_MEASURABLE_MS || sum <= 0.0)
    {
        return;
    }
    const double imbalance = peak * m_n_workers / sum; // 1.0 = every worker finished together
    if (imbalance > TRANS_IMB_SHRINK)
    {
        // Proportional: a claim carrying k times its share of the work needs to be k times smaller.
        const double scaled = std::floor(m_trans_chunk * TRANS_IMB_TARGET / imbalance);
        m_trans_chunk = std::clamp(static_cast<int>(scaled), 1, TRANS_CHUNK_MAX);
    }
    else if (imbalance < TRANS_IMB_TARGET)
    {
        // Doubling, not the same proportional step: growth only has to be fast enough that a scene
        // that stops needing fine claims (a zoom out, a camera turn) recovers in a few frames
        // rather than the ~60 a 10%-per-frame climb from 1 would take.
        m_trans_chunk = std::min(m_trans_chunk * 2, TRANS_CHUNK_MAX);
    }
}

// Size the per-pixel head array to the framebuffer and sentinel-fill it. Only does
// work when the dimensions change (or on first use): in steady state the resolve pass
// restores every touched head to SENTINEL, so the array is already clean each frame.
void Renderer::ensure_abuffer(int width, int height)
{
    if (width == m_ab_width && height == m_ab_height && !m_frag_head.empty())
    {
        return;
    }
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    // The buffer is REUSED while the frame is within half of it, rather than rebuilt every
    // time. A vector of atomics cannot be resized in place (they are neither copyable nor
    // movable), so a rebuild reallocates AND value-initializes the whole thing before the
    // sentinel fill writes it a second time, and every page is then first-touched: 7.8 ms
    // at 1080p, which a blend scene paid on EVERY resize (a glazed vase went 3.0 ms to
    // 11.2 ms a frame, and interactive resizing delivers a stream of resize events).
    // Reuse is safe because every reader indexes by a pixel index derived from width and
    // height, never by size(), so a buffer larger than the frame is inert. A shrink past
    // half still reallocates so the memory comes back, matching Framebuffer::resize's
    // policy of releasing frame-sized buffers; retention is bounded at 2x the frame.
    if (n > m_frag_head.size() || n < m_frag_head.size() / 2)
    {
        m_frag_head = std::vector<std::atomic<uint64_t>>(n);
    }
    // Filled on reuse too, rather than trusting the resolve pass to have left every head at
    // SENTINEL. That invariant does hold (a mutation that skipped this fill still passed
    // the resize test), but leaning on it would make a resize silently depend on it, and
    // the cost of not doing so is one store pass (~0.8 ms at 1080p) against the 7.8 ms
    // saved above. Removing the fill ENTIRELY is not merely wrong but unsafe: a fresh
    // vector is value-initialized to zero, and zero is a valid-looking fragment ref, so
    // the resolve pass then chases garbage (the test binary crashes).
    for (size_t i = 0; i < n; i++)
    {
        m_frag_head[i].store(ABuffer::SENTINEL, std::memory_order_relaxed);
    }
    m_ab_width = width;
    m_ab_height = height;
}

void Renderer::render(
    const Mesh &mesh, const Camera &camera, const Light *lights, int n_lights, const vec3 &ambient, Framebuffer &fb
)
{
    const vec3 eye = camera.eye();
    const mat4 view = camera.view(eye);
    const mat4 proj = camera.projection(fb.width(), fb.height());
    const mat4 vp = proj * view;
    const int width = fb.width();
    const int height = fb.height();

    // Frame inputs are written once under the lock and stay constant across all phases.
    {
        const std::scoped_lock lk(m_mutex);
        m_mesh = &mesh;
        m_vp = vp;
        m_eye = eye;
        m_lights = lights;
        m_n_lights = n_lights;
        m_ambient = ambient;
        m_near_plane = camera.near_plane;
        m_width = width;
        m_height = height;
        m_smode = mode;
        m_cull_backfaces = cull_backfaces;
        m_show_texture = show_texture;
        m_wireframe_color = wireframe_color;
        m_fb = &fb;
        // Opaque range. has_transparent meshes carry a real opaque_count from load_model;
        // for everything else the opaque pass covers all triangles (a manually built Mesh
        // may leave opaque_count at 0, so don't trust it unless has_transparent is set).
        m_opaque_count = mesh.has_transparent ? mesh.opaque_count : static_cast<uint32_t>(mesh.triangles.size());
    }

    // Wireframe: a single edge-drawing pass over every triangle [0, total). No opaque /
    // transparent split: wireframe ignores materials and draws all edges in one colour.
    if (mode == ShadingMode::Wireframe)
    {
        m_tri_cursor.store(0, std::memory_order_relaxed);
        dispatch_pass(Pass::Wireframe);
        return;
    }

    // Phase 1: opaque geometry over [0, opaque_count). Two implementations: the immediate pass
    // (each stolen triangle rasterized straight into the framebuffer through the depth CAS) and
    // the tiled pass (bin, then per-tile visibility + deferred shading). The tiled pass wins on
    // big triangles: it removes the framebuffer atomics, shades each pixel once instead of once
    // per overdraw layer, balances a wall-sized triangle across the pool, and shades in batches.
    // It loses on small ones: every recorded triangle costs a 64-byte record round trip, a second
    // setup and a second rasterization, and its batches then hold a pixel or two. See
    // TILE_MIN_TRI_PX for the statistic that separates the two and the measurements behind it.
    // The split needs the on-screen size of the triangles that survive culling and clipping,
    // which is only known after the front-end runs, so it is taken from the previous frame of the
    // same mesh at the same size (frames are temporally coherent; both passes report it through
    // m_area/m_area2). With no such frame there is nothing to predict from, and the immediate
    // pass is the safe guess: it is never catastrophic, while the tiled pass is on a dense mesh.
    const auto n_px = static_cast<double>(width) * static_cast<double>(height);
    // A RESIZE does not invalidate the statistic, it rescales it, so the predictor keeps
    // working across one. The statistic is an on-screen AREA and the projection is
    // vertical-fov: m[0][0] carries 1/aspect with aspect = width/height, so ndc.x is
    // proportional to height/width and ndc_to_screen's multiply by width leaves screen x
    // proportional to HEIGHT; screen y is too. Both axes therefore scale with the frame
    // height, and a triangle's area with its square.
    //
    // Without this every resize spent one frame on the immediate pass, which on the very
    // scenes the tiled pass exists for is not a rounding error: measured 170 ms against a
    // settled 7.8 ms on Sponza at zoom 4 (21.8x), 27 ms against 3.5 ms at zoom 1.
    // Interactive resizing delivers a stream of resize events, so that was a stream of
    // those frames.
    const bool same_mesh = m_prev_mesh == &mesh && m_prev_tris == mesh.triangles.size();
    double pxw_tri = m_prev_area > 0.0 ? m_prev_area2 / m_prev_area : 0.0;
    if (same_mesh && m_prev_height > 0 && m_prev_height != height)
    {
        const double s = static_cast<double>(height) / static_cast<double>(m_prev_height);
        pxw_tri *= s * s;
    }
    // Pixels per tile touched. A ratio of two areas, so a resize scales both and it needs no
    // rescale of its own, unlike pxw_tri above.
    const double pxw_per_tile = m_prev_area > 0.0 ? m_prev_span / m_prev_area : 0.0;
    // Split deliberately: the frame-size test is a HARD constraint and is obeyed the moment it
    // changes, while the two triangle statistics are sampled estimates and are damped below. A
    // resize below TILE_MIN_PIXELS must fall back on the same frame, not several frames later.
    //
    // KNOWN ASYMMETRY, accepted rather than fixed: the fall is instant but the climb back is not.
    // m_prev_tiled records what actually RAN, so a frame forced immediate by size sets it false
    // while the statistics keep asking for tiles; the vote counter then cycles for the whole small
    // period and, when the frame grows back, holds the immediate pass for whatever votes remain
    // (measured up to 5 frames after 3 small ones). Reachable by dragging a kitty or sixel window
    // under 65536 px and back; blocks-mode frames are always below the threshold and never tile,
    // so they cannot hit it. Fixing it means tracking the damped statistical preference separately
    // from the pass that ran, which is new predictor state, and every past change to this
    // predictor that looked safe on medians cost 12-21x on some scene's WORST frame. So it wants
    // the full max-frame-time campaign, not a drive-by.
    const bool size_ok = same_mesh && n_px >= TILE_MIN_PIXELS;
    const bool stats_want_tiles =
        pxw_tri >= (m_prev_tiled ? TILE_KEEP_TRI_PX : TILE_MIN_TRI_PX) && pxw_per_tile >= TILE_MIN_TRI_PER_TILE;

    // Switching paths costs a frame at the wrong pass's price, so the statistics deciding it must
    // be believed only when they PERSIST. Both are sampled ratios over a spinning view, and their
    // tails are wide (the per-tile one reads a p90/p10 of 6 on Sponza at zoom 4), so a single
    // frame's dip across a threshold means nothing while several in a row mean the scene really
    // moved. Requiring PATH_SWITCH_FRAMES consecutive frames of disagreement turns a lone outlier
    // into no switch at all.
    //
    // Deliberately damping in TIME rather than widening the thresholds into a value band, which
    // was tried first and is worse in two ways: a band is a dead zone where a scene keeps whatever
    // path it happened to start on (a zoomed greyhound sits there and wants tiles by 1.44x), and
    // it stalls a mesh in the tiled pass when a 4K-to-1080p resize drops it into the band. Time
    // damping has no dead zone: it delays a real move by a few frames and rejects noise outright.
    //
    // The first stats-backed decision is taken at once (`m_path_settled`). Damping it instead
    // would spend PATH_SWITCH_FRAMES frames on the immediate pass whenever the mesh changes,
    // which on the scenes the tiled pass exists for is 58 ms a frame against 10.
    bool stats_verdict = stats_want_tiles;
    if (!same_mesh)
    {
        // A different mesh is a different question; do not carry the old scene's evidence. The
        // STATISTICS have to go with the flag, not just the flag: m_prev_area is only rewritten
        // after this frame's dispatch, so leaving it set re-latches m_path_settled from the old
        // mesh on this very frame, and the new mesh's first stats-backed choice is then damped
        // like any other, which is the 8 slow frames the flag exists to avoid. Clearing here and
        // not earlier is deliberate: pxw_tri and pxw_per_tile were already read above, and
        // size_ok requires same_mesh, so this frame takes the immediate pass either way.
        m_path_settled = false;
        m_prev_area = 0.0;
        m_prev_area2 = 0.0;
        m_prev_span = 0.0;
    }
    if (!m_path_settled)
    {
        m_path_votes = 0;
        m_path_settled = m_prev_area > 0.0; // settled once a real frame has reported statistics
    }
    else if (stats_want_tiles != m_prev_tiled)
    {
        m_path_votes++;
        if (m_path_votes < PATH_SWITCH_FRAMES)
        {
            stats_verdict = m_prev_tiled; // hold the current pass until the evidence repeats
        }
        else
        {
            m_path_votes = 0;
        }
    }
    else
    {
        m_path_votes = 0;
    }
    const bool auto_tiles = size_ok && stats_verdict;
    // RasterTri holds a triangle's tile rectangle in uint16_t, so a grid with more than 65536
    // tiles on an axis would wrap it and bin triangles into the wrong tiles (or, when only one
    // end wraps, into none at all). Bound the frame by the smallest tile edge, 8 px: an
    // interactive frame cannot come close (main.cpp caps a side at 8192), and --bench-size, which
    // can, falls back to the immediate pass.
    const bool tiles_fit = std::max(width, height) <= 8 * 65536;
    const bool use_tiles = m_opaque_count > 0 && tiles_fit &&
                           (opaque_path == OpaquePath::Tiled || (opaque_path == OpaquePath::Auto && auto_tiles));
    m_prev_mesh = &mesh;
    m_prev_tris = mesh.triangles.size();
    m_prev_height = height;
    m_prev_tiled = use_tiles;
    if (use_tiles)
    {
        // Tile edge: TILE_MAX unless that leaves too few tiles to keep the pool busy (a tile is
        // the steal unit), halving down to 8 for small frames.
        m_tile = TILE_MAX;
        while (m_tile > 8 &&
               ((width + m_tile - 1) / m_tile) * ((height + m_tile - 1) / m_tile) < TILES_PER_WORKER * m_n_workers)
        {
            m_tile /= 2;
        }
        m_tiles_x = (width + m_tile - 1) / m_tile;
        m_tiles_y = (height + m_tile - 1) / m_tile;
        m_tri_cursor.store(0, std::memory_order_relaxed);
        m_tile_cursor.store(0, std::memory_order_relaxed);
        m_bin_done.store(0, std::memory_order_relaxed);
        dispatch_pass(Pass::Tiled);
    }
    else
    {
        m_tri_cursor.store(0, std::memory_order_relaxed);
        dispatch_pass(Pass::Opaque);
    }
    m_prev_area = 0.0;
    m_prev_area2 = 0.0;
    m_prev_span = 0.0;
    for (int w = 0; w < m_n_workers; w++)
    {
        m_prev_area += m_area[static_cast<size_t>(w)];
        m_prev_area2 += m_area2[static_cast<size_t>(w)];
        m_prev_span += m_area_span[static_cast<size_t>(w)];
    }

    // Phases 2-3: only meshes that actually have a blend tail. Accumulate transparent
    // fragments into the per-pixel A-buffer, then resolve (sort + composite) over the opaque
    // framebuffer. The opaque_count < total guard skips both barrier round-trips (and the
    // O(pixels) resolve sweep) when has_transparent is set by a declared-but-unused blend
    // material: no triangle reached the tail, so there is nothing to accumulate or resolve.
    if (mesh.has_transparent && m_opaque_count < static_cast<uint32_t>(mesh.triangles.size()))
    {
        ensure_abuffer(width, height);

        // The controller steers from the previous frame of the same mesh, for the reason the
        // opaque path choice does: frames are temporally coherent, and the measurement only exists
        // once a pass has run. A different MESH starts again from the coarse claim.
        //
        // A resize deliberately does NOT reset it. The claim size is about how the blend tail's
        // work spreads across claims, which is a property of the geometry and materials: a resize
        // scales every triangle's fragment count by the same factor, so the max/mean imbalance the
        // controller steers on is unchanged. Resetting on one cost a frame at the coarse claim
        // every resize, measured 10.98 ms against a settled 3.33 ms on a glazed vase (3.3x) and
        // 11.95 vs 4.71 on a glass candle, and interactive resizing delivers a stream of them.
        if (!same_mesh)
        {
            m_trans_chunk = TRANS_CHUNK_MAX;
        }
        m_tri_cursor.store(static_cast<int>(m_opaque_count), std::memory_order_relaxed);
        dispatch_pass(Pass::TransAccum);
        retune_trans_chunk();

        // Merge the per-worker touched-pixel boxes so the Resolve sweep covers only the
        // transparent region. Merge straight into m_res_box (the member resolve_pixels()
        // reads; workers cannot see a render() stack local), resetting it first since it
        // persists across frames. An empty merged box means zero pushes (a box only grows
        // inside push, after the head is published), so every head is still SENTINEL and the
        // whole pass is skipped, subsuming the fully-occluded / fully-culled case. A single
        // AABB degrades toward full-frame when transparency occupies separated screen
        // regions; that is the floor, not a regression: the sweep is never larger than the old
        // unconditional full-frame one and measured neutral-to-faster even in that worst
        // case, while a multi-box / per-tile dirty mask would add per-frame cost that loses
        // on the common single-region case, so it is deliberately not done. An empty box
        // ({INT_MAX, INT_MIN}) is the min/max identity, so workers that pushed nothing fold
        // in without a guard.
        m_res_box = TouchBox{};
        for (int w = 0; w < m_n_workers; w++)
        {
            const TouchBox &b = m_touch_box[static_cast<size_t>(w)];
            m_res_box.x0 = std::min(m_res_box.x0, b.x0);
            m_res_box.y0 = std::min(m_res_box.y0, b.y0);
            m_res_box.x1 = std::max(m_res_box.x1, b.x1);
            m_res_box.y1 = std::max(m_res_box.y1, b.y1);
        }
        if (!m_res_box.empty())
        {
            // Steal in row bands: enough bands for balance, capped so a tall box doesn't
            // over-fragment the cursor. At least one row so a short box still dispatches.
            // A wide-but-short box (bh < workers) under-parallelizes (some workers idle), but
            // benched faster than the old full-frame sweep even at 1-2 rows × deep overdraw:
            // those few rows' pixels are contiguous (≈one old chunk → already ≈serial there),
            // and the box skips the rest of the frame's SENTINEL scan. Not worth an aspect-aware
            // column-band fallback.
            const int bh = m_res_box.y1 - m_res_box.y0 + 1;
            m_res_row_chunk = std::clamp(bh / (m_n_workers * 8), 1, 64);
            // Seed the row cursor to the box top; resolve_pixels() reads the y-start from here.
            m_pixel_cursor.store(m_res_box.y0, std::memory_order_relaxed);
            dispatch_pass(Pass::Resolve);
        }
    }
}
