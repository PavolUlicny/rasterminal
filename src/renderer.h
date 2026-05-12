#pragma once

#include "camera.h"
#include "framebuffer.h"
#include "rasterize.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// ─── ShadingMode ──────────────────────────────────────────────────────────────

enum class ShadingMode
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
    Color wireframe_color = {200, 200, 200};
    bool cull_backfaces = true;
    bool show_texture = true;

    // Spawns worker threads that persist for the lifetime of the Renderer.
    // n_threads: -1 = auto (min(hw_concurrency, 4)), 0 = all hw threads (bare -j), N = exactly N.
    // Clamped to [1, hardware_concurrency].
    explicit Renderer(int n_threads = -1);
    ~Renderer();

    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // shadow_map: pre-built shadow map from build_shadow_map(). Pass nullptr to
    // disable shadows, or a valid pointer to reuse a cached map every frame.
    void render(const Mesh &, const Camera &,
                const Light *lights, int n_lights, const vec3 &ambient,
                Framebuffer &, const ShadowMap *shadow_map = nullptr);

private:
    void worker_func(int t);

    int m_n_workers = 0; // total thread pool size, fixed at construction
    int m_n_active = 0;  // workers used this frame: min(m_n_workers, height/2)
    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_cv_work; // workers block here between frames
    std::condition_variable m_cv_done; // render() blocks here until all done
    std::mutex m_phase_mutex;
    std::condition_variable m_cv_phase2; // parked workers wait here for Phase 2

    // Phase 1 (geometry) inputs:
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

    // 2D staging: [worker][band]. Each worker writes its own row in Phase 1
    // with no locks; Phase 2 worker t reads column t across all rows.
    std::vector<std::vector<std::vector<RasterTri>>> m_band_tris;

    // Phase 2 (rasterize) inputs:
    Framebuffer *m_fb = nullptr;
    bool m_phong = false;

    std::atomic<int> m_tri_cursor{0};        // dynamic work cursor for Phase 1
    std::atomic<int> m_phase1_done{0};       // counts workers that finished Phase 1
    std::atomic<bool> m_phase2_ready{false}; // set when all Phase 1 workers are done

    std::atomic<int> m_active{0}; // workers not yet done with the current frame
    int m_generation = 0;         // bumped before each dispatch to wake workers
    bool m_stop = false;          // set by destructor to terminate worker loops
};
