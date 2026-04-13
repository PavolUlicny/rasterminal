#include "renderer.h"

#include <algorithm>
#include <thread>

// ─── internal helpers ─────────────────────────────────────────────────────────

// NDC → screen-space pixel coordinates.
// NDC x/y ∈ [-1,1]; y is flipped (NDC +1 = top, screen y=0 = top).
// z is kept as NDC depth for the z-buffer.
static vec3 ndc_to_screen(vec3 ndc, int W, int H)
{
    return {
        (ndc.x + 1.0f) * 0.5f * (float)W,
        (1.0f - ndc.y) * 0.5f * (float)H,
        ndc.z};
}

// ─── Renderer: constructor / destructor ───────────────────────────────────────

Renderer::Renderer()
{
    m_n_workers = std::max(1, (int)std::thread::hardware_concurrency());
    m_threads.reserve((size_t)m_n_workers);
    m_band_tris.resize((size_t)m_n_workers);
    m_band_mutexes = std::make_unique<std::mutex[]>((size_t)m_n_workers);
    for (int t = 0; t < m_n_workers; t++)
        m_threads.emplace_back(&Renderer::worker_func, this, t);
}

Renderer::~Renderer()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stop = true;
        ++m_generation; // ensure workers see the stop flag
    }
    m_cv_work.notify_all();
    for (auto &th : m_threads)
        th.join();
}

// ─── Renderer::worker_func ────────────────────────────────────────────────────

void Renderer::worker_func(int t)
{
    int my_gen = 0;
    while (true)
    {
        // Sleep until a new frame is dispatched or the renderer is destroyed.
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv_work.wait(lk, [this, my_gen]
                       { return m_generation != my_gen || m_stop; });
        if (m_stop)
            return;

        my_gen = m_generation;
        const int n = m_n_active; // local copy — stable for this frame
        lk.unlock();

        // Workers beyond the active cap for this frame go back to sleep.
        if (t >= n)
        {
            std::lock_guard<std::mutex> done_lk(m_mutex);
            if (--m_active == 0)
                m_cv_done.notify_one();
            continue;
        }

        {
            // ── Phase 1: geometry ─────────────────────────────────────────────
            // Inputs are written once before dispatch — no lock needed to read.
            const Mesh *mesh = m_mesh;
            const mat4 &vp = m_vp;
            const vec3 &eye = m_eye;
            const Light *lights = m_lights;
            int n_lights = m_n_lights;
            const vec3 &ambient = m_ambient;
            const ShadowMap *psmap = m_psmap;
            float near_plane = m_near_plane;
            int W = m_W;
            int H = m_H;
            ShadingMode smode = m_smode;

            int total = (int)mesh->triangles.size();

            // Thread-local per-band staging: persists across frames so vector
            // capacity is retained; only cleared, never reallocated after warmup.
            static thread_local std::vector<std::vector<RasterTri>> local_bands;
            if ((int)local_bands.size() != n)
                local_bands.resize((size_t)n);
            for (auto &v : local_bands)
                v.clear();

            // Dynamic work stealing: each worker atomically claims the next
            // chunk of triangles. This balances load automatically regardless
            // of how visible triangles are distributed across the mesh.
            constexpr int CHUNK = 256;
            while (true)
            {
                int start = m_tri_cursor.fetch_add(CHUNK, std::memory_order_relaxed);
                if (start >= total)
                    break;
                int end = std::min(start + CHUNK, total);
                for (int i = start; i < end; i++)
                {
                    const Triangle &tri = mesh->triangles[i];
                    const Vertex &va = mesh->vertices[tri.v[0]];
                    const Vertex &vb = mesh->vertices[tri.v[1]];
                    const Vertex &vc = mesh->vertices[tri.v[2]];

                    const Material &mat = (tri.material_idx < mesh->materials.size())
                                              ? mesh->materials[tri.material_idx]
                                              : mesh->materials[0];

                    const Texture *tex = nullptr;
                    if (mat.diffuse_tex >= 0 && mat.diffuse_tex < (int)mesh->textures.size())
                        tex = &mesh->textures[(size_t)mat.diffuse_tex];

                    const Texture *nmap = nullptr;
                    if (mat.normal_tex >= 0 && mat.normal_tex < (int)mesh->textures.size())
                        nmap = &mesh->textures[(size_t)mat.normal_tex];

                    ClipVert cva = {vp * vec4(va.pos, 1.0f), va.pos, va.normal, va.tangent, va.uv, va.ao};
                    ClipVert cvb = {vp * vec4(vb.pos, 1.0f), vb.pos, vb.normal, vb.tangent, vb.uv, vb.ao};
                    ClipVert cvc = {vp * vec4(vc.pos, 1.0f), vc.pos, vc.normal, vc.tangent, vc.uv, vc.ao};

                    ClipVert clipped[2][3];
                    int n_tris = clip_near(cva, cvb, cvc, clipped, near_plane);

                    for (int ti = 0; ti < n_tris; ti++)
                    {
                        const ClipVert &a = clipped[ti][0];
                        const ClipVert &b = clipped[ti][1];
                        const ClipVert &c = clipped[ti][2];

                        if (clip_reject(a.c, b.c, c.c))
                            continue;

                        vec3 sa = ndc_to_screen(a.c.perspective_divide(), W, H);
                        vec3 sb = ndc_to_screen(b.c.perspective_divide(), W, H);
                        vec3 sc = ndc_to_screen(c.c.perspective_divide(), W, H);

                        float area = (sb.x - sa.x) * (sc.y - sa.y) - (sc.x - sa.x) * (sb.y - sa.y);
                        if (area >= 0.0f)
                            continue;

                        RasterTri rt{};
                        rt.sa = sa;
                        rt.sb = sb;
                        rt.sc = sc;
                        rt.wa = a.c.w;
                        rt.wb = b.c.w;
                        rt.wc = c.c.w;
                        rt.pa = a.pos;
                        rt.pb = b.pos;
                        rt.pc = c.pos;
                        rt.uva = a.uv;
                        rt.uvb = b.uv;
                        rt.uvc = c.uv;
                        rt.aoa = a.ao;
                        rt.aob = b.ao;
                        rt.aoc = c.ao;
                        rt.tex = tex;
                        rt.nmap = nmap;
                        rt.smap = psmap;

                        if (smode == ShadingMode::Phong)
                        {
                            rt.na = a.normal;
                            rt.nb = b.normal;
                            rt.nc = c.normal;
                            rt.tana = a.tangent;
                            rt.tanb = b.tangent;
                            rt.tanc = c.tangent;
                            rt.eye = eye;
                            rt.lights = lights;
                            rt.n_lights = n_lights;
                            rt.ambient = ambient;
                            rt.mat = &mat;
                        }
                        else if (smode == ShadingMode::Flat)
                        {
                            vec3 fn = normalize(cross(b.pos - a.pos, c.pos - a.pos));
                            vec3 fc = (a.pos + b.pos + c.pos) * (1.0f / 3.0f);
                            float face_ao = (a.ao + b.ao + c.ao) * (1.0f / 3.0f);
                            const Light *sl = (n_lights > 0) ? lights + 1 : lights;
                            const int sc = (n_lights > 0) ? n_lights - 1 : 0;
                            rt.col_a = rt.col_b = rt.col_c =
                                compute_lighting(fc, fn, eye, lights, n_lights, ambient, mat, face_ao);
                            rt.shad_a = rt.shad_b = rt.shad_c =
                                compute_lighting(fc, fn, eye, sl, sc, ambient, mat, face_ao);
                        }
                        else // Gouraud
                        {
                            const Light *sl = (n_lights > 0) ? lights + 1 : lights;
                            const int sc = (n_lights > 0) ? n_lights - 1 : 0;
                            rt.col_a = compute_lighting(a.pos, a.normal, eye, lights, n_lights, ambient, mat, a.ao);
                            rt.col_b = compute_lighting(b.pos, b.normal, eye, lights, n_lights, ambient, mat, b.ao);
                            rt.col_c = compute_lighting(c.pos, c.normal, eye, lights, n_lights, ambient, mat, c.ao);
                            rt.shad_a = compute_lighting(a.pos, a.normal, eye, sl, sc, ambient, mat, a.ao);
                            rt.shad_b = compute_lighting(b.pos, b.normal, eye, sl, sc, ambient, mat, b.ao);
                            rt.shad_c = compute_lighting(c.pos, c.normal, eye, sl, sc, ambient, mat, c.ao);
                        }

                        // Bucket into every y-band this triangle overlaps.
                        // Scan from the first band whose bottom edge reaches
                        // tri_y0 down to the last band whose top edge is still
                        // above tri_y1.  n is small (≤16) so the scan is cheap.
                        int tri_y0 = std::max(0, (int)std::floor(std::min({sa.y, sb.y, sc.y})));
                        int tri_y1 = std::min(H - 1, (int)std::ceil(std::max({sa.y, sb.y, sc.y})));
                        int b_lo = 0;
                        while (b_lo < n - 1 && H * (b_lo + 1) / n - 1 < tri_y0)
                            ++b_lo;
                        int b_hi = n - 1;
                        while (b_hi > b_lo && H * b_hi / n > tri_y1)
                            --b_hi;
                        for (int b = b_lo; b <= b_hi; b++)
                            local_bands[b].push_back(rt);
                    }
                }
            } // while (true) work-stealing loop

            // Flush local staging into the shared per-band lists.
            // Each band has its own mutex so workers rarely contend.
            for (int b = 0; b < n; b++)
            {
                if (!local_bands[b].empty())
                {
                    std::lock_guard<std::mutex> lk(m_band_mutexes[b]);
                    auto &dst = m_band_tris[b];
                    dst.insert(dst.end(), local_bands[b].begin(), local_bands[b].end());
                }
            }
        }

        // ── Internal barrier: spin until all workers finish Phase 1 ──────────
        // fetch_add returns the value BEFORE the increment, so the last worker
        // to arrive sees (n-1) and sets m_phase2_ready to unblock the others.
        if (m_phase1_done.fetch_add(1, std::memory_order_acq_rel) == n - 1)
        {
            m_phase2_ready.store(true, std::memory_order_release);
        }
        else
        {
            // Brief busy-spin before yielding: when workers finish Phase 1 within
            // microseconds of each other (the common case), the signal is caught
            // immediately without entering the OS scheduler. Only yield on heavy
            // imbalance where the wait would otherwise burn a full core.
            for (int spin = 0; !m_phase2_ready.load(std::memory_order_acquire); ++spin)
                if (spin == 1000)
                {
                    spin = 0;
                    std::this_thread::yield();
                }
        }

        // ── Phase 2: rasterize ────────────────────────────────────────────────
        // Each worker owns band t — only triangles pre-bucketed into that band,
        // so no wasted iterations over triangles from other parts of the screen.
        {
            int y_min = m_H * t / n;
            int y_max = m_H * (t + 1) / n - 1;

            for (const RasterTri &rt : m_band_tris[t])
            {
                if (m_phong)
                    rasterize_phong(*m_fb,
                                    rt.sa, rt.sb, rt.sc, rt.wa, rt.wb, rt.wc,
                                    rt.pa, rt.pb, rt.pc,
                                    rt.na, rt.nb, rt.nc,
                                    rt.tana, rt.tanb, rt.tanc,
                                    rt.uva, rt.uvb, rt.uvc,
                                    rt.aoa, rt.aob, rt.aoc,
                                    rt.eye, rt.lights, rt.n_lights, rt.ambient, *rt.mat,
                                    rt.tex, rt.nmap, rt.smap,
                                    y_min, y_max);
                else
                    rasterize(*m_fb,
                              rt.sa, rt.sb, rt.sc, rt.wa, rt.wb, rt.wc,
                              rt.col_a, rt.col_b, rt.col_c,
                              rt.shad_a, rt.shad_b, rt.shad_c,
                              rt.pa, rt.pb, rt.pc,
                              rt.uva, rt.uvb, rt.uvc,
                              rt.tex, rt.smap,
                              y_min, y_max);
            }
        }

        // Signal completion. If this is the last worker, wake render().
        std::lock_guard<std::mutex> done_lk(m_mutex);
        if (--m_active == 0)
            m_cv_done.notify_one();
    }
}

// ─── Renderer::render ─────────────────────────────────────────────────────────

void Renderer::render(const Mesh &mesh, const Camera &camera,
                      const Light *lights, int n_lights, const vec3 &ambient,
                      Framebuffer &fb, const ShadowMap *smap)
{
    const mat4 view = camera.view();
    const mat4 proj = camera.projection(fb.width(), fb.height());
    const mat4 vp = proj * view;
    const vec3 eye = camera.eye();
    const int W = fb.width();
    const int H = fb.height();
    const ShadowMap *psmap = (n_lights > 0) ? smap : nullptr;

    // ── Wireframe: single-threaded (draw_line writes to framebuffer directly) ─
    if (mode == ShadingMode::Wireframe)
    {
        for (const Triangle &tri : mesh.triangles)
        {
            const Vertex &va = mesh.vertices[tri.v[0]];
            const Vertex &vb = mesh.vertices[tri.v[1]];
            const Vertex &vc = mesh.vertices[tri.v[2]];

            ClipVert cva = {vp * vec4(va.pos, 1.0f), va.pos, va.normal, va.tangent, va.uv, va.ao};
            ClipVert cvb = {vp * vec4(vb.pos, 1.0f), vb.pos, vb.normal, vb.tangent, vb.uv, vb.ao};
            ClipVert cvc = {vp * vec4(vc.pos, 1.0f), vc.pos, vc.normal, vc.tangent, vc.uv, vc.ao};

            ClipVert clipped[2][3];
            int n_tris = clip_near(cva, cvb, cvc, clipped, camera.near_plane);

            for (int ti = 0; ti < n_tris; ti++)
            {
                const ClipVert &a = clipped[ti][0];
                const ClipVert &b = clipped[ti][1];
                const ClipVert &c = clipped[ti][2];

                if (clip_reject(a.c, b.c, c.c))
                    continue;

                vec3 sa = ndc_to_screen(a.c.perspective_divide(), W, H);
                vec3 sb = ndc_to_screen(b.c.perspective_divide(), W, H);
                vec3 sc = ndc_to_screen(c.c.perspective_divide(), W, H);

                float area = (sb.x - sa.x) * (sc.y - sa.y) - (sc.x - sa.x) * (sb.y - sa.y);
                if (area >= 0.0f)
                    continue;

                const Color wf = {200, 200, 200};
                draw_line(fb, sa, sb, wf);
                draw_line(fb, sb, sc, wf);
                draw_line(fb, sc, sa, wf);
            }
        }
        return;
    }

    // ── Single dispatch: workers run Phase 1 + Phase 2 without returning ────
    // Cap active workers to framebuffer height / 2 so no band is empty.
    // On resize the band vectors and mutexes are rebuilt to match.
    const int n_active = std::max(1, std::min(m_n_workers, H / 2));
    if (n_active != (int)m_band_tris.size())
    {
        m_band_tris.resize((size_t)n_active);
        m_band_mutexes = std::make_unique<std::mutex[]>((size_t)n_active);
    }

    // An internal spin-barrier inside worker_func separates the two phases,
    // avoiding a second CV roundtrip. Reset barrier state before waking workers.
    for (auto &v : m_band_tris)
        v.clear();
    m_tri_cursor.store(0, std::memory_order_relaxed);
    m_phase1_done.store(0, std::memory_order_relaxed);
    m_phase2_ready.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_mesh = &mesh;
        m_vp = vp;
        m_eye = eye;
        m_lights = lights;
        m_n_lights = n_lights;
        m_ambient = ambient;
        m_psmap = psmap;
        m_near_plane = camera.near_plane;
        m_W = W;
        m_H = H;
        m_smode = mode;
        m_fb = &fb;
        m_phong = (mode == ShadingMode::Phong);
        m_n_active = n_active;
        m_active = m_n_workers; // all workers must check in, active or not
        ++m_generation;
    }
    m_cv_work.notify_all();
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv_done.wait(lk, [this]
                       { return m_active == 0; });
    }
}

// ─── Renderer::cycle_shading ──────────────────────────────────────────────────

void Renderer::cycle_shading()
{
    switch (mode)
    {
    case ShadingMode::Wireframe:
        mode = ShadingMode::Flat;
        break;
    case ShadingMode::Flat:
        mode = ShadingMode::Gouraud;
        break;
    case ShadingMode::Gouraud:
        mode = ShadingMode::Phong;
        break;
    case ShadingMode::Phong:
        mode = ShadingMode::Wireframe;
        break;
    }
}
