#include "renderer.h"
#include "camera.h"
#include "clip.h"
#include "framebuffer.h"
#include "light.h"
#include "linalg.h"
#include "mesh.h"
#include "rasterize.h"
#include "shadow.h"
#include "texture.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

// ─── internal helpers ─────────────────────────────────────────────────────────

namespace
{

    // NDC → screen-space pixel coordinates.
    // NDC x/y ∈ [-1,1]; y is flipped (NDC +1 = top, screen y=0 = top).
    // z is kept as NDC depth for the z-buffer.
    constexpr vec3 ndc_to_screen(vec3 ndc, int width, int height) noexcept
    {
        return {
            (ndc.x + 1.0f) * 0.5f * static_cast<float>(width),
            (1.0f - ndc.y) * 0.5f * static_cast<float>(height),
            ndc.z};
    }

    // Choose a conservative dynamic chunk size for Phase 1 work stealing.
    // This keeps enough claims per worker for balance while bounding overhead.
    constexpr int choose_phase1_chunk(int total_tris, int n_workers) noexcept
    {
        constexpr int MIN_CHUNK = 64;
        constexpr int MAX_CHUNK = 256;
        constexpr int TARGET_CLAIMS_PER_WORKER = 12;

        if (total_tris <= 0 || n_workers <= 0)
            return MIN_CHUNK;

        const int denom = n_workers * TARGET_CLAIMS_PER_WORKER;
        const int raw = (total_tris + denom - 1) / denom; // ceil(total/denom)
        return std::clamp(raw, MIN_CHUNK, MAX_CHUNK);
    }

} // namespace

// ─── Renderer: constructor / destructor ───────────────────────────────────────

Renderer::Renderer(int n_threads)
{
    const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    // -1 = auto (default): min(hw, 4)
    //  0 = all hardware threads
    //  N = exactly N, clamped to [1, hw]
    const int req = (n_threads < 0) ? std::min(hw, 4) : (n_threads == 0) ? hw
                                                                         : n_threads;
    m_n_workers = std::clamp(req, 1, hw);
    m_threads.reserve(static_cast<size_t>(m_n_workers));
    for (int t = 0; t < m_n_workers; t++)
        m_threads.emplace_back(&Renderer::worker_func, this, t);
}

Renderer::~Renderer()
{
    {
        const std::lock_guard<std::mutex> lk(m_mutex);
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
            if (m_active.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                const std::lock_guard<std::mutex> done_lk(m_mutex);
                m_cv_done.notify_one();
            }
            continue;
        }

        {
            // ── Phase 1: geometry ─────────────────────────────────────────────
            // Inputs are written once before dispatch — no lock needed to read.
            const Mesh *mesh = m_mesh;
            const mat4 &vp = m_vp;
            const vec3 &eye = m_eye;
            const Light *lights = m_lights;
            const int n_lights = m_n_lights;
            const vec3 &ambient = m_ambient;
            const ShadowMap *shadow_map = m_shadow_map;
            const float near_plane = m_near_plane;
            const int width = m_width;
            const int height = m_height;
            const ShadingMode smode = m_smode;
            const bool do_cull = m_cull_backfaces;
            const bool show_tex = m_show_texture;
            const Light *shadow_lights = (n_lights > 0) ? lights + 1 : lights;
            const int n_shadow_lights = (n_lights > 0) ? n_lights - 1 : 0;

            const int total = static_cast<int>(mesh->triangles.size());
            const vec3 *p_tans = (smode == ShadingMode::Phong) ? mesh->tangents.data() : nullptr;
            const vec3 *p_vcols = mesh->has_vertex_colors ? mesh->vertex_colors.data() : nullptr;

            // Precompute band start rows so the per-triangle band scan avoids
            // multiply+divide on every iteration. thread_local avoids per-frame
            // heap allocation; the vector only resizes when n changes (rare).
            thread_local std::vector<int> band_y;
            if (static_cast<int>(band_y.size()) != n + 1)
                band_y.resize(static_cast<size_t>(n) + 1);
            for (int k = 0; k <= n; k++)
                band_y[static_cast<size_t>(k)] = height * k / n;

            // Dynamic work stealing: each worker atomically claims the next
            // chunk of triangles. This balances load automatically regardless
            // of how visible triangles are distributed across the mesh.
            const int chunk = choose_phase1_chunk(total, n);
            ClipVert clipped[2][3]; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — hoisted out of per-triangle loop; clip_near overwrites before read
            while (true)
            {
                const int start = m_tri_cursor.fetch_add(chunk, std::memory_order_relaxed);
                if (start >= total)
                    break;
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
                                continue;
                            flip_normals = true;
                        }
                    }

                    const Material &mat = mesh->mat_at(tri.material_idx);
                    const Texture *tex = show_tex ? mesh->tex_at(mat.diffuse_tex) : nullptr;

                    const vec3 ta = p_tans ? p_tans[tri.v[0]] : vec3{0.0f, 0.0f, 0.0f};
                    const vec3 tb = p_tans ? p_tans[tri.v[1]] : vec3{0.0f, 0.0f, 0.0f};
                    const vec3 tc = p_tans ? p_tans[tri.v[2]] : vec3{0.0f, 0.0f, 0.0f};
                    const vec3 ca = p_vcols ? p_vcols[tri.v[0]] : vec3{1.0f, 1.0f, 1.0f};
                    const vec3 cb = p_vcols ? p_vcols[tri.v[1]] : vec3{1.0f, 1.0f, 1.0f};
                    const vec3 cc = p_vcols ? p_vcols[tri.v[2]] : vec3{1.0f, 1.0f, 1.0f};
                    ClipVert cva = {vp * vec4(va.pos, 1.0f), va.pos, va.normal, ta, va.uv, va.ao, ca};
                    ClipVert cvb = {vp * vec4(vb.pos, 1.0f), vb.pos, vb.normal, tb, vb.uv, vb.ao, cb};
                    ClipVert cvc = {vp * vec4(vc.pos, 1.0f), vc.pos, vc.normal, tc, vc.uv, vc.ao, cc};
                    if (flip_normals)
                    {
                        cva.normal = cva.normal * -1.0f;
                        cvb.normal = cvb.normal * -1.0f;
                        cvc.normal = cvc.normal * -1.0f;
                    }

                    const int n_tris = clip_near(cva, cvb, cvc, clipped, near_plane);

                    for (int ti = 0; ti < n_tris; ti++)
                    {
                        const ClipVert &a = clipped[ti][0];
                        const ClipVert &b = clipped[ti][1];
                        const ClipVert &c = clipped[ti][2];

                        if (clip_reject(a.c, b.c, c.c))
                            continue;

                        const vec3 sa = ndc_to_screen(a.c.perspective_divide(), width, height);
                        const vec3 sb = ndc_to_screen(b.c.perspective_divide(), width, height);
                        const vec3 sc = ndc_to_screen(c.c.perspective_divide(), width, height);

                        // Bucket into every y-band this triangle overlaps.
                        // Scan from the first band whose bottom edge reaches
                        // tri_y0 down to the last band whose top edge is still
                        // above tri_y1.  band_y is precomputed so each iteration
                        // is a single array read.
                        const int tri_y0 = std::max(0, static_cast<int>(std::floor(std::min({sa.y, sb.y, sc.y}))));
                        const int tri_y1 = std::min(height - 1, static_cast<int>(std::ceil(std::max({sa.y, sb.y, sc.y}))));
                        int b_lo = 0;
                        while (b_lo < n - 1 && band_y[static_cast<size_t>(b_lo) + 1] - 1 < tri_y0)
                            ++b_lo;
                        int b_hi = n - 1;
                        while (b_hi > b_lo && band_y[static_cast<size_t>(b_hi)] > tri_y1)
                            --b_hi;

                        auto fill_common = [&](auto &rt)
                        {
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
                            rt.tex = tex;
                            rt.shadow_map = shadow_map;
                        };

                        if (smode == ShadingMode::Phong)
                        {
                            RasterTriPh rt;
                            fill_common(rt);
                            rt.stex = show_tex ? mesh->tex_at(mat.specular_tex) : nullptr;
                            rt.nmap = show_tex ? mesh->tex_at(mat.normal_tex) : nullptr;
                            rt.na = a.normal;
                            rt.nb = b.normal;
                            rt.nc = c.normal;
                            rt.tana = a.tangent;
                            rt.tanb = b.tangent;
                            rt.tanc = c.tangent;
                            rt.aoa = a.ao;
                            rt.aob = b.ao;
                            rt.aoc = c.ao;
                            rt.mat = &mat;
                            if (mesh->has_vertex_colors)
                            {
                                rt.vcola = a.color;
                                rt.vcolb = b.color;
                                rt.vcolc = c.color;
                            }
                            else
                            {
                                rt.vcola = rt.vcolb = rt.vcolc = {1.0f, 1.0f, 1.0f};
                            }
                            for (int band = b_lo; band <= b_hi; band++)
                                m_band_tris_ph[static_cast<size_t>(t)][static_cast<size_t>(band)].push_back(rt);
                        }
                        else
                        {
                            RasterTriFg rt;
                            fill_common(rt);
                            rt.alpha_cutoff = show_tex ? mat.alpha_cutoff : 0.0f;

                            if (smode == ShadingMode::Flat)
                            {
                                vec3 face_n = normalize(cross(b.pos - a.pos, c.pos - a.pos));
                                if (flip_normals)
                                    face_n = face_n * -1.0f;
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
                                rt.col_a = rt.col_b = rt.col_c =
                                    compute_lighting(assume_unit, fc, face_n, eye, lights, n_lights, ambient, *flat_mat, face_ao);
                                if (shadow_map)
                                    rt.shad_a = rt.shad_b = rt.shad_c =
                                        compute_lighting(assume_unit, fc, face_n, eye, shadow_lights, n_shadow_lights, ambient, *flat_mat, face_ao);
                            }
                            else // Gouraud
                            {
                                if (mesh->has_vertex_colors)
                                {
                                    Material gvcol_mat;
                                    auto gouraud_mat = [&](const vec3 &vcol) -> const Material &
                                    {
                                        if (vcol.x == 1.0f && vcol.y == 1.0f && vcol.z == 1.0f)
                                            return mat;
                                        gvcol_mat = mat;
                                        gvcol_mat.diffuse = gvcol_mat.diffuse * vcol;
                                        gvcol_mat.ambient = gvcol_mat.ambient * vcol;
                                        return gvcol_mat;
                                    };
                                    rt.col_a = compute_lighting(assume_unit, a.pos, a.normal, eye, lights, n_lights, ambient, gouraud_mat(a.color), a.ao);
                                    rt.col_b = compute_lighting(assume_unit, b.pos, b.normal, eye, lights, n_lights, ambient, gouraud_mat(b.color), b.ao);
                                    rt.col_c = compute_lighting(assume_unit, c.pos, c.normal, eye, lights, n_lights, ambient, gouraud_mat(c.color), c.ao);
                                    if (shadow_map)
                                    {
                                        rt.shad_a = compute_lighting(assume_unit, a.pos, a.normal, eye, shadow_lights, n_shadow_lights, ambient, gouraud_mat(a.color), a.ao);
                                        rt.shad_b = compute_lighting(assume_unit, b.pos, b.normal, eye, shadow_lights, n_shadow_lights, ambient, gouraud_mat(b.color), b.ao);
                                        rt.shad_c = compute_lighting(assume_unit, c.pos, c.normal, eye, shadow_lights, n_shadow_lights, ambient, gouraud_mat(c.color), c.ao);
                                    }
                                }
                                else
                                {
                                    rt.col_a = compute_lighting(assume_unit, a.pos, a.normal, eye, lights, n_lights, ambient, mat, a.ao);
                                    rt.col_b = compute_lighting(assume_unit, b.pos, b.normal, eye, lights, n_lights, ambient, mat, b.ao);
                                    rt.col_c = compute_lighting(assume_unit, c.pos, c.normal, eye, lights, n_lights, ambient, mat, c.ao);
                                    if (shadow_map)
                                    {
                                        rt.shad_a = compute_lighting(assume_unit, a.pos, a.normal, eye, shadow_lights, n_shadow_lights, ambient, mat, a.ao);
                                        rt.shad_b = compute_lighting(assume_unit, b.pos, b.normal, eye, shadow_lights, n_shadow_lights, ambient, mat, b.ao);
                                        rt.shad_c = compute_lighting(assume_unit, c.pos, c.normal, eye, shadow_lights, n_shadow_lights, ambient, mat, c.ao);
                                    }
                                }
                            }
                            for (int band = b_lo; band <= b_hi; band++)
                                m_band_tris_fg[static_cast<size_t>(t)][static_cast<size_t>(band)].push_back(rt);
                        }
                    }
                }
            } // while (true) work-stealing loop
        }

        // ── Internal barrier: hybrid spin-then-park for Phase transition ─────
        // fetch_add returns the value BEFORE the increment, so the last worker
        // to arrive sees (n-1), releases m_phase2_ready, and wakes parked peers.
        if (m_phase1_done.fetch_add(1, std::memory_order_acq_rel) == n - 1)
        {
            {
                const std::lock_guard<std::mutex> phase_lk(m_phase_mutex);
                m_phase2_ready.store(true, std::memory_order_release);
            }
            m_cv_phase2.notify_all();
        }
        else
        {
            // Short optimistic spin keeps the common low-jitter case scheduler-free.
            // If one worker straggles, park on a CV so we do not burn CPU cycles.
            constexpr int PHASE2_SPIN_ITERS = 512;
            int spin = 0;
            while (spin < PHASE2_SPIN_ITERS && !m_phase2_ready.load(std::memory_order_acquire))
                ++spin;

            if (!m_phase2_ready.load(std::memory_order_acquire))
            {
                std::unique_lock<std::mutex> phase_lk(m_phase_mutex);
                m_cv_phase2.wait(phase_lk, [this]
                                 { return m_phase2_ready.load(std::memory_order_acquire); });
            }
        }

        // ── Phase 2: rasterize ────────────────────────────────────────────────
        // Each worker owns band t — only triangles pre-bucketed into that band,
        // so no wasted iterations over triangles from other parts of the screen.
        {
            const int y_min = m_height * t / n;
            const int y_max = m_height * (t + 1) / n - 1;

            // Per-frame Phong lighting constants — read from renderer state
            // instead of duplicating into every RasterTri. Safe to read without
            // a lock: m_* are written once under m_mutex before m_cv_work.notify_all()
            // in render(), and no worker modifies them during the frame.
            const vec3 &p2_eye = m_eye;
            const Light *p2_lights = m_lights;
            const int p2_n_lights = m_n_lights;
            const vec3 &p2_ambient = m_ambient;
            const bool p2_has_vcol = m_mesh->has_vertex_colors;

            if (m_smode == ShadingMode::Phong)
            {
                for (int w = 0; w < n; w++)
                    for (const RasterTriPh &rt : m_band_tris_ph[static_cast<size_t>(w)][static_cast<size_t>(t)])
                        rasterize_phong(*m_fb,
                                        rt.sa, rt.sb, rt.sc, rt.wa, rt.wb, rt.wc,
                                        rt.pa, rt.pb, rt.pc,
                                        rt.na, rt.nb, rt.nc,
                                        rt.tana, rt.tanb, rt.tanc,
                                        rt.uva, rt.uvb, rt.uvc,
                                        rt.aoa, rt.aob, rt.aoc,
                                        rt.vcola, rt.vcolb, rt.vcolc,
                                        p2_has_vcol,
                                        p2_eye, p2_lights, p2_n_lights, p2_ambient, *rt.mat,
                                        rt.tex, rt.nmap, rt.stex, rt.shadow_map,
                                        y_min, y_max);
            }
            else
            {
                for (int w = 0; w < n; w++)
                    for (const RasterTriFg &rt : m_band_tris_fg[static_cast<size_t>(w)][static_cast<size_t>(t)])
                        rasterize(*m_fb,
                                  rt.sa, rt.sb, rt.sc, rt.wa, rt.wb, rt.wc,
                                  rt.col_a, rt.col_b, rt.col_c,
                                  rt.shad_a, rt.shad_b, rt.shad_c,
                                  rt.pa, rt.pb, rt.pc,
                                  rt.uva, rt.uvb, rt.uvc,
                                  rt.tex, rt.alpha_cutoff, rt.shadow_map,
                                  y_min, y_max);
            }
        }

        // Signal completion. If this is the last worker, wake render().
        if (m_active.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            const std::lock_guard<std::mutex> done_lk(m_mutex);
            m_cv_done.notify_one();
        }
    }
}

// ─── Renderer::render ─────────────────────────────────────────────────────────

void Renderer::render(const Mesh &mesh, const Camera &camera,
                      const Light *lights, int n_lights, const vec3 &ambient,
                      Framebuffer &fb, const ShadowMap *shadow_map)
{
    const vec3 eye = camera.eye();
    const mat4 view = camera.view(eye);
    const mat4 proj = camera.projection(fb.width(), fb.height());
    const mat4 vp = proj * view;
    const int width = fb.width();
    const int height = fb.height();
    const ShadowMap *active_shadow_map = (n_lights > 0) ? shadow_map : nullptr;

    // ── Wireframe: single-threaded (draw_line writes to framebuffer directly) ─
    if (mode == ShadingMode::Wireframe)
    {
        ClipVert clipped[2][3]; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — hoisted; clip_near overwrites before read
        for (const Triangle &tri : mesh.triangles)
        {
            const Vertex &va = mesh.vertices[tri.v[0]];
            const Vertex &vb = mesh.vertices[tri.v[1]];
            const Vertex &vc = mesh.vertices[tri.v[2]];

            if (cull_backfaces)
            {
                const vec3 face_normal = cross(vb.pos - va.pos, vc.pos - va.pos);
                if (dot(face_normal, eye - va.pos) <= 0.0f &&
                    (!mesh.has_double_sided || !mesh.mat_at(tri.material_idx).double_sided))
                    continue;
            }

            const ClipVert cva = {vp * vec4(va.pos, 1.0f), va.pos, va.normal, {}, va.uv, va.ao};
            const ClipVert cvb = {vp * vec4(vb.pos, 1.0f), vb.pos, vb.normal, {}, vb.uv, vb.ao};
            const ClipVert cvc = {vp * vec4(vc.pos, 1.0f), vc.pos, vc.normal, {}, vc.uv, vc.ao};

            const int n_tris = clip_near(cva, cvb, cvc, clipped, camera.near_plane);

            for (int ti = 0; ti < n_tris; ti++)
            {
                const ClipVert &a = clipped[ti][0];
                const ClipVert &b = clipped[ti][1];
                const ClipVert &c = clipped[ti][2];

                if (clip_reject(a.c, b.c, c.c))
                    continue;

                const vec3 sa = ndc_to_screen(a.c.perspective_divide(), width, height);
                const vec3 sb = ndc_to_screen(b.c.perspective_divide(), width, height);
                const vec3 sc = ndc_to_screen(c.c.perspective_divide(), width, height);

                const Color wf = wireframe_color;
                draw_line(fb, sa, sb, wf);
                draw_line(fb, sb, sc, wf);
                draw_line(fb, sc, sa, wf);
            }
        }
        return;
    }

    // ── Single dispatch: workers run Phase 1 + Phase 2 without returning ────
    // Cap active workers to framebuffer height / 2 so no band is empty.
    const int n_active = std::max(1, std::min(m_n_workers, height / 2));
    if (static_cast<int>(m_band_tris_fg.size()) != n_active)
        m_band_tris_fg.resize(static_cast<size_t>(n_active));
    if (static_cast<int>(m_band_tris_ph.size()) != n_active)
        m_band_tris_ph.resize(static_cast<size_t>(n_active));
    for (auto &row : m_band_tris_fg)
        if (static_cast<int>(row.size()) != n_active)
            row.resize(static_cast<size_t>(n_active));
    for (auto &row : m_band_tris_ph)
        if (static_cast<int>(row.size()) != n_active)
            row.resize(static_cast<size_t>(n_active));

    // An internal hybrid barrier (brief spin, then CV park) separates the
    // two phases. Reset barrier state before waking workers.
    // Only clear the active vector — the inactive one is never read in Phase 2
    // so stale data is harmless until that mode becomes active again.
    if (mode == ShadingMode::Phong)
        for (auto &row : m_band_tris_ph)
            for (auto &v : row)
                v.clear();
    else
        for (auto &row : m_band_tris_fg)
            for (auto &v : row)
                v.clear();
    m_tri_cursor.store(0, std::memory_order_relaxed);
    m_phase1_done.store(0, std::memory_order_relaxed);
    m_phase2_ready.store(false, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lk(m_mutex);
        m_mesh = &mesh;
        m_vp = vp;
        m_eye = eye;
        m_lights = lights;
        m_n_lights = n_lights;
        m_ambient = ambient;
        m_shadow_map = active_shadow_map;
        m_near_plane = camera.near_plane;
        m_width = width;
        m_height = height;
        m_smode = mode;
        m_cull_backfaces = cull_backfaces;
        m_show_texture = show_texture;
        m_fb = &fb;
        m_n_active = n_active;
        m_active.store(m_n_workers, std::memory_order_release); // all workers must check in, active or not
        ++m_generation;
    }
    m_cv_work.notify_all();
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv_done.wait(lk, [this]
                       { return m_active.load(std::memory_order_acquire) == 0; });
    }
}
