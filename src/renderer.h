#pragma once

#include "camera.h"
#include "framebuffer.h"
#include "light.h"
#include "linalg.h"
#include "mesh.h"
#include "rasterize.h" // Fragment / ABuffer
#include "shadow.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

// ─── ShadingMode ──────────────────────────────────────────────────────────────

enum class ShadingMode : std::uint8_t
{
    Wireframe,
    Flat,
    Gouraud,
    Phong
};

// ─── Renderer ─────────────────────────────────────────────────────────────────

struct Renderer
{
    ShadingMode mode = ShadingMode::Gouraud;
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

    // shadow_map: pre-built shadow map from build_shadow_map(). Pass nullptr to
    // disable shadows, or a valid pointer to reuse a cached map every frame.
    void render(
        const Mesh &mesh,
        const Camera &camera,
        const Light *lights,
        int n_lights,
        const vec3 &ambient,
        Framebuffer &fb,
        const ShadowMap *shadow_map = nullptr
    );

  private:
    // Which phase the woken workers run this generation. Opaque is the unchanged
    // single-pass geometry+raster; the two transparent phases run only for meshes with
    // blend materials (see render()).
    enum class Pass : std::uint8_t
    {
        Opaque,
        TransAccum, // shade transparent triangles, push fragments to the per-pixel A-buffer
        Resolve     // sort + composite each pixel's fragments over the opaque colour
    };

    void worker_func(int worker_id);

    // Single-pass geometry + rasterize over this worker's stolen triangle chunks.
    // S == Opaque is the original opaque path (byte-identical codegen); S == Transparent
    // shades blend triangles and pushes fragments into this worker's A-buffer arena.
    template <Sink S> void raster_triangles(int worker_id);

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
    const ShadowMap *m_shadow_map = nullptr;
    float m_near_plane = 0.0f;
    int m_width = 0, m_height = 0;
    ShadingMode m_smode = ShadingMode::Gouraud;
    bool m_cull_backfaces = true;
    bool m_show_texture = true;
    Framebuffer *m_fb = nullptr;

    std::atomic<int> m_tri_cursor{ 0 };   // work-stealing cursor across all workers
    std::atomic<int> m_pixel_cursor{ 0 }; // pixel-range cursor for the Resolve pass
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
};
