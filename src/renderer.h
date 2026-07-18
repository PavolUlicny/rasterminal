#pragma once

#include "camera.h"
#include "framebuffer.h"
#include "light.h"
#include "linalg.h"
#include "mesh.h"
#include "rasterize.h" // Fragment / ABuffer
#include "shading.h"   // IWYU pragma: export

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// ─── Renderer ─────────────────────────────────────────────────────────────────

struct Renderer
{
    ShadingMode mode = ShadingMode::Phong;
    Color wireframe_color = { 200, 200, 200 };
    bool cull_backfaces = true;
    bool show_texture = true;

    // Spawns worker threads that persist for the lifetime of the Renderer.
    // n_threads: -1 = auto (min(hw_concurrency, 4)), 0 = all hw threads (bare -j), N = exactly N.
    // Clamped to [1, hardware_concurrency].
    explicit Renderer(int n_threads = -1);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    Renderer(Renderer &&) = delete;
    Renderer &operator=(Renderer &&) = delete;

    void render(
        const Mesh &mesh, const Camera &camera, const Light *lights, int n_lights, const vec3 &ambient, Framebuffer &fb
    );

  private:
    // Which phase the woken workers run this generation. Opaque is the unchanged
    // single-pass geometry+raster; the two transparent phases run only for meshes with
    // blend materials (see render()).
    enum class Pass : std::uint8_t
    {
        Opaque,
        Wireframe,  // draw triangle edges (uniform colour, atomic CAS depth-test) across the pool
        TransAccum, // shade transparent triangles, push fragments to the per-pixel A-buffer
        Resolve     // sort + composite each pixel's fragments over the opaque colour
    };

    void worker_func(int worker_id);

    // Single-pass geometry + rasterize over this worker's stolen triangle chunks.
    // S == Opaque is the original opaque path (byte-identical codegen); S == Transparent
    // shades blend triangles and pushes fragments into this worker's A-buffer arena.
    // M selects the shading path (Flat/Phong) at compile time so the per-triangle
    // mode branch folds away; Wireframe never reaches the worker pool.
    template <Sink S, ShadingMode M> void raster_triangles(int worker_id);

    // Bridge the runtime m_smode to the compile-time M instantiation of raster_triangles.
    template <Sink S> void dispatch_raster(int worker_id);

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

    int m_n_workers = 0; // total thread pool size, fixed at construction
    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_cv_work; // workers block here between frames
    std::condition_variable m_cv_done; // render() blocks here until all done

    // Frame inputs — written once under m_mutex before workers are woken,
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
    std::atomic<int> m_pixel_cursor{ 0 }; // row-band cursor for the Resolve pass
    std::atomic<int> m_active{ 0 };       // workers not yet done with the current frame
    int m_generation = 0;                 // bumped before each dispatch to wake workers
    bool m_stop = false;                  // set by destructor to terminate worker loops

    // ── Transparency state ────────────────────────────────────────────────────
    Pass m_pass = Pass::Opaque;
    uint32_t m_opaque_count = 0; // triangle range split: [0,opaque) opaque, [opaque,total) blend
    int m_ab_width = 0, m_ab_height = 0;

    // Per-pixel linked-list heads (framebuffer-sized) and per-worker fragment arenas.
    // head[idx] holds the most-recent fragment ref ((worker_id<<32)|node) or SENTINEL.
    std::vector<std::atomic<uint64_t>> m_frag_head;
    std::vector<std::vector<Fragment>> m_arenas; // one per worker; reused across frames
    std::vector<TouchBox> m_touch_box;           // one per worker; touched-pixel extent for Resolve

    // Resolve sweep extent (merged transparent bounding box) + row-band steal chunk.
    // Set in render() after the accumulate barrier; read by resolve_pixels() for the column
    // bounds [x0,x1] and last row y1. The y-start (box.y0) seeds m_pixel_cursor in render().
    TouchBox m_res_box; // default-empty; never read before render() assigns it (resolve only
                        // dispatches inside the non-empty-box guard)
    int m_res_row_chunk = 1;
};
