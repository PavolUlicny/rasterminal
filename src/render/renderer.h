#pragma once

#include "src/loaders/mesh.h"
#include "src/math/light.h"
#include "src/math/linalg.h"
#include "src/render/camera.h"
#include "src/render/rasterize.h" // Fragment / ABuffer
#include "src/shading.h"          // IWYU pragma: export
#include "src/terminal/framebuffer.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

struct Renderer
{
    ShadingMode mode = ShadingMode::Phong;
    Color wireframe_color = { 200, 200, 200 };
    bool cull_backfaces = true;
    bool show_texture = true;

    // Which opaque pass render() runs: Auto picks per frame between the immediate pass and the
    // tiled visibility-buffer pass (see render()); the forced values exist so the tests can
    // render one scene through both and check they agree. No CLI flag: the choice is a pure
    // performance decision with no visible effect beyond rounding.
    enum class OpaquePath : std::uint8_t
    {
        Auto,
        Immediate,
        Tiled
    };
    OpaquePath opaque_path = OpaquePath::Auto;

    // Resolves the ctor's n_threads contract to the worker count that would be
    // spawned: -1 = auto (see all_cores_default), 0 = all hw threads (bare -j),
    // N = exactly N; result clamped to [1, hw_concurrency]. Static and public so
    // main.cpp resolves once through the same code path the ctor uses, keeping the
    // bench report and load-time threading in lockstep with the actual pool.
    // all_cores_default picks the fallback used when n_threads is unset (an explicit request is
    // honoured either way): true takes every core, false the historical min(hw, 4). See the
    // definition for which callers ask for which.
    [[nodiscard]] static int resolve_thread_count(int n_threads, bool all_cores_default = false) noexcept;

    // Spawns worker threads that persist for the lifetime of the Renderer.
    // n_threads is interpreted by resolve_thread_count above.
    explicit Renderer(int n_threads = -1);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    Renderer(Renderer &&) = delete;
    Renderer &operator=(Renderer &&) = delete;

    void render(
        const Mesh &mesh, const Camera &camera, const Light *lights, int n_lights, const vec3 &ambient, Framebuffer &fb
    );

    // Runs fn once per worker as (worker_id, n_workers) and blocks until all return.
    // Exists so the present path can use the pool that is idle between frames (the
    // direct transport's deflate is 16 ms of a 1080p frame, more than the render on a
    // 4K one) without the framebuffer owning threads of its own. Call only from the
    // thread that calls render(), and never from inside fn: the pool is not reentrant.
    // fn should report its own failures through storage the caller owns. Anything deriving from
    // std::exception is swallowed per worker, the way the tiled pass swallows its own; anything
    // else escapes, and this is a thread entry point, so that is std::terminate.
    void run_on_workers(const std::function<void(int worker_id, int n_workers)> &fn);

    [[nodiscard]] int worker_count() const noexcept { return m_n_workers; }

    // Which opaque pass the last render() actually ran. The counterpart to opaque_path:
    // that forces a choice, this observes the one Auto made, which is otherwise invisible
    // and so untestable. Pins that a resize does not throw the choice away (see render()).
    //
    // cppcheck's unusedFunction is whole-program over the src TUs only, so a member whose
    // sole caller is a test always reads as unused. Suppressed here rather than by adding
    // src/render/renderer.h to the suppressions file, which would mask every future one too.
    // cppcheck-suppress unusedFunction
    [[nodiscard]] bool last_frame_tiled() const noexcept { return m_prev_tiled; }

  private:
    // Which phase the woken workers run this generation. Opaque is the unchanged
    // single-pass geometry+raster; the two transparent phases run only for meshes with
    // blend materials (see render()).
    enum class Pass : std::uint8_t
    {
        Opaque,
        Wireframe,  // draw triangle edges (uniform colour, atomic CAS depth-test) across the pool
        TransAccum, // shade transparent triangles, push fragments to the per-pixel A-buffer
        Resolve,    // sort + composite each pixel's fragments over the opaque colour
        Tiled,      // tiled opaque path: bin (geometry front-end + per-tile bins), in-pass barrier,
                    // then per-tile visibility pass + deferred shading
        Task        // run m_task once per worker (run_on_workers); not a render phase
    };

    void worker_func(int worker_id);

    // Bump the generation, wake the workers on `pass`, block until all finish.
    void dispatch_pass(Pass pass);

    // Single-pass geometry + rasterize over this worker's stolen triangle chunks.
    // S == Opaque is the original opaque path (byte-identical codegen); S == Transparent
    // shades blend triangles and pushes fragments into this worker's A-buffer arena.
    // M selects the shading path (Flat/Phong) at compile time so the per-triangle
    // mode branch folds away; Wireframe never reaches the worker pool.
    template <Sink S, ShadingMode M> void raster_triangles(int worker_id);

    // Bridge the runtime m_smode to the compile-time M instantiation of raster_triangles.
    template <Sink S> void dispatch_raster(int worker_id);

    // Tiled opaque path (see render() for when it is chosen over the immediate raster_triangles
    // pass). bin_triangles runs the same geometry front-end as raster_triangles but, instead of
    // rasterizing, records each on-screen triangle (screen verts + clip w) and bins it into the
    // TILE x TILE screen tiles its bbox touches; shade_tiles then steals whole tiles, resolves
    // visibility per tile into a private depth + id buffer (no framebuffer atomics, no
    // overshading) and shades each visible triangle's pixels once through the Deferred sink.
    // A tile is one steal unit, so a wall-sized triangle is shaded by every worker rather than
    // one, which is what the immediate pass could not do.
    template <ShadingMode M> void bin_triangles(int worker_id);
    template <ShadingMode M> void shade_tiles(int worker_id);
    void dispatch_bin(int worker_id);
    void dispatch_tiles(int worker_id);

    static constexpr int TILE_MAX =
        32; // tile edge in pixels at large frames (TILE_MAX^2 floats + ids stay L1-resident);
            // render() halves it for small frames so the grid still feeds the whole pool

    // Transparent accumulate pass: bounds and targets for the steal-granularity controller in
    // render(). The upper bound is the opaque pass's chunk, the point below which claim atomics
    // start to show; the imbalance band is the controller's dead zone (see retune_trans_chunk()).
    static constexpr int TRANS_CHUNK_MAX = 256;
    static constexpr double TRANS_IMB_TARGET = 1.10;
    static constexpr double TRANS_IMB_SHRINK = 1.25;

    // One binned triangle: screen-space verts + clip w for setup, the source triangle (material
    // and, when clip == CLIP_NONE, its vertex attributes), else the first of three near-clipped
    // ClipVerts in the binning worker's clip arena, and the inclusive tile rectangle.
    struct RasterTri
    {
        static constexpr uint32_t CLIP_NONE = ~static_cast<uint32_t>(0);
        float sx[3], sy[3], sz[3], w[3];
        uint32_t tri;
        uint32_t clip;
        uint16_t tx0, ty0, tx1, ty1;
        bool flip; // double-sided back face: negate normals (Phong) / the face normal (Flat)
    };

    // Per-worker binning output + tile scratch, reused across frames (capacity persists).
    struct WorkerBins
    {
        std::vector<RasterTri> recs;
        std::vector<ClipVert> clip;                         // near-clipped vertex triples referenced by recs
        std::vector<uint32_t> tile_start;                   // n_tiles + 1 prefix offsets into sorted
        std::vector<uint32_t> sorted;                       // rec indices grouped by tile
        std::vector<uint64_t> tile_list;                    // shade_tiles scratch: (worker << 32 | rec) per tile
        std::vector<std::pair<float, uint64_t>> tile_order; // shade_tiles scratch: (nearest z, ref) sort keys
        std::vector<uint32_t> tile_claims;                  // shade_tiles scratch: visible pixel count per list entry
        std::vector<float> tile_depth;                      // TILE_MAX^2
        std::vector<uint32_t> tile_ids;                     // TILE_MAX^2
    };

    // Wireframe pass: each worker steals triangle chunks and draws their three edges as
    // DDA lines in m_wireframe_color. No shading/texture; correctness is in draw_line's
    // atomic CAS commit (uniform colour + order-independent depth-min ⇒ output is identical
    // to a serial draw for any worker count). Takes no worker_id (like resolve_pixels): work
    // is claimed from the shared m_tri_cursor, not a per-worker arena.
    void raster_wireframe();

    // Resolve pass: composite each owned pixel's fragment list back-to-front over the
    // opaque colour already in the framebuffer.
    void resolve_pixels();

    // Resize the per-pixel head array + reset per-worker arenas to match the framebuffer.
    // One full sentinel clear on size change; steady-state heads self-clean in Resolve.
    void ensure_abuffer(int width, int height);
    void retune_trans_chunk();

    // Resolve the (worker << 32 | index) fragment ref the A-buffer chains use.
    [[nodiscard]] Fragment &frag_at(uint64_t ref) noexcept
    {
        return m_arenas[static_cast<uint32_t>(ref >> 32u)][static_cast<uint32_t>(ref & 0xFFFFFFFFu)];
    }

    int m_n_workers = 0; // total thread pool size, fixed at construction
    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_cv_work; // workers block here between frames
    std::condition_variable m_cv_done; // render() blocks here until all done

    // Frame inputs, written once under m_mutex before workers are woken,
    // then read-only for the duration of the frame.
    const Mesh *m_mesh = nullptr;
    mat4 m_vp;
    vec3 m_eye;
    const Light *m_lights = nullptr;
    int m_n_lights = 0;
    vec3 m_ambient;
    float m_near_plane = 0.0f;
    int m_width = 0, m_height = 0;
    ShadingMode m_smode = ShadingMode::Phong;
    bool m_cull_backfaces = true;
    bool m_show_texture = true;
    Color m_wireframe_color = { 200, 200, 200 };
    Framebuffer *m_fb = nullptr;

    std::atomic<int> m_tri_cursor{ 0 };   // work-stealing cursor across all workers
    std::atomic<int> m_tile_cursor{ 0 };  // tile-stealing cursor of the tiled pass
    std::atomic<int> m_bin_done{ 0 };     // workers finished binning (the tiled pass's in-pass barrier)
    std::atomic<int> m_pixel_cursor{ 0 }; // row-band cursor for the Resolve pass
    std::atomic<int> m_active{ 0 };       // workers not yet done with the current frame
    int m_generation = 0;                 // bumped before each dispatch to wake workers
    int m_tiles_x = 0, m_tiles_y = 0;     // tile grid of the current frame
    int m_tile = TILE_MAX;                // tile edge of the current frame (power of two)
    uint32_t m_opaque_count = 0;          // triangle range split: [0,opaque) opaque, [opaque,total) blend
    int m_ab_width = 0, m_ab_height = 0;
    std::vector<WorkerBins> m_bins; // one per worker
    // Per worker, this frame, from whichever opaque pass ran: the summed on-screen area of the
    // drawn triangles, the summed square of that area, and the same square divided by the tiles
    // each triangle touches. area2/area is the pixel-weighted mean triangle area and span/area
    // the pixels per tile touched; together they pick the pass for the next frame (see render()).
    std::vector<double> m_area;
    std::vector<double> m_area2;
    std::vector<double> m_area_span;
    // Previous-frame state for the tiled/immediate predictor (see render()): the scene it drew,
    // the areas the pass reported, the pass it took (the hysteresis band reads this), and the
    // frame HEIGHT, which is what the predictor rescales its on-screen area by across a resize,
    // both screen axes scaling with the height under a vertical-fov projection. Width is never
    // needed and so is not kept.
    const Mesh *m_prev_mesh = nullptr;
    size_t m_prev_tris = 0;
    double m_prev_area = 0.0;
    double m_prev_area2 = 0.0;
    double m_prev_span = 0.0;
    // Path-switch damping (see render()): consecutive frames the statistics have asked for the
    // pass we are NOT running, and whether a stats-backed choice has been made for this scene yet.
    int m_path_votes = 0;
    bool m_path_settled = false;
    int m_prev_height = 0;
    bool m_prev_tiled = false;
    // Transparent accumulate pass only: how long each worker spent in its steal loop last frame,
    // and the triangles-per-claim that produced it. render() feeds the first back into the second.
    std::vector<double> m_trans_ms;
    int m_trans_chunk = TRANS_CHUNK_MAX;
    bool m_stop = false; // set by destructor to terminate worker loops
    Pass m_pass = Pass::Opaque;
    // Borrowed for the duration of one run_on_workers call; null outside it.
    const std::function<void(int, int)> *m_task = nullptr;

    // Per-pixel linked-list heads (framebuffer-sized) and per-worker fragment arenas.
    // head[idx] holds the most-recent fragment ref ((worker_id<<32)|node) or SENTINEL.
    std::vector<std::atomic<uint64_t>> m_frag_head;
    std::vector<FragArena> m_arenas;   // one per worker; reused across frames
    std::vector<TouchBox> m_touch_box; // one per worker; touched-pixel extent for Resolve

    // Resolve sweep extent (merged transparent bounding box) + row-band steal chunk.
    // Set in render() after the accumulate barrier; read by resolve_pixels() for the column
    // bounds [x0,x1] and last row y1. The y-start (box.y0) seeds m_pixel_cursor in render().
    TouchBox m_res_box; // default-empty; never read before render() assigns it (resolve only
                        // dispatches inside the non-empty-box guard)
    int m_res_row_chunk = 1;
};
