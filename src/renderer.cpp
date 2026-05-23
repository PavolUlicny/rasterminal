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

} // namespace

// ─── Renderer: constructor / destructor ───────────────────────────────────────

Renderer::Renderer(int n_threads)
{
    const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    // -1 = auto (default): min(hw, 4)
    //  0 = all hardware threads
    //  N = exactly N, clamped to [1, hw]
    const int req = (n_threads < 0) ? std::min(hw, 4) : (n_threads == 0) ? hw : n_threads;
    m_n_workers = std::clamp(req, 1, hw);
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

// ─── Renderer::worker_func ────────────────────────────────────────────────────

void Renderer::worker_func(int t)
{
    (void)t; // worker identity no longer drives band ownership
    int my_gen = 0;
    while (true)
    {
        // Sleep until a new frame is dispatched or the renderer is destroyed.
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv_work.wait(lk, [this, my_gen] { return m_generation != my_gen || m_stop; });
        if (m_stop)
        {
            return;
        }

        my_gen = m_generation;
        lk.unlock();

        // ── Single-pass: geometry + rasterize ────────────────────────────────
        // Each worker steals triangle chunks and rasterizes directly into the
        // framebuffer. The CAS-based depth test in Framebuffer ensures the
        // closest triangle wins per pixel across all threads. A narrow race
        // between a winning depth CAS and the following color write is
        // accepted: at most one wrong-coloured pixel per collision per frame,
        // invisible at interactive frame rates.
        {
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
            // Texture toggle gate. show_emissive controls the emissive texture sample. The
            // emissive factor is gated separately at each call site on Material::emissive_was_promoted:
            // authored factors always pass through; loader-promoted {1,1,1} factors are zeroed when
            // show_tex is off, otherwise toggling textures off on a BoomBox/DamagedHelmet-style
            // asset would render solid white from the leftover factor add.
            const bool show_emissive = mesh->has_emissive && show_tex;
            const bool show_metallic = mesh->has_metallic && show_tex;
            Framebuffer *fb = m_fb;
            const Light *shadow_lights = (n_lights > 0) ? lights + 1 : lights;
            const int n_shadow_lights = (n_lights > 0) ? n_lights - 1 : 0;

            const int total = static_cast<int>(mesh->triangles.size());
            const vec3 *p_tans = (smode == ShadingMode::Phong) ? mesh->tangents.data() : nullptr;
            const vec3 *p_vcols = mesh->has_vertex_colors ? mesh->vertex_colors.data() : nullptr;

            const int chunk = choose_phase1_chunk(total, m_n_workers);
            ClipVert clipped[2][3]; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — hoisted;
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

                    const Material &mat = mesh->mat_at(tri.material_idx);
                    const Texture *tex = show_tex ? mesh->tex_at(mat.diffuse_tex) : nullptr;

                    const vec3 ta = p_tans ? p_tans[tri.v[0]] : vec3{ 0.0f, 0.0f, 0.0f };
                    const vec3 tb = p_tans ? p_tans[tri.v[1]] : vec3{ 0.0f, 0.0f, 0.0f };
                    const vec3 tc = p_tans ? p_tans[tri.v[2]] : vec3{ 0.0f, 0.0f, 0.0f };
                    const vec3 ca = p_vcols ? p_vcols[tri.v[0]] : vec3{ 1.0f, 1.0f, 1.0f };
                    const vec3 cb = p_vcols ? p_vcols[tri.v[1]] : vec3{ 1.0f, 1.0f, 1.0f };
                    const vec3 cc = p_vcols ? p_vcols[tri.v[2]] : vec3{ 1.0f, 1.0f, 1.0f };
                    ClipVert cva = { vp * vec4(va.pos, 1.0f), va.pos, va.normal, ta, va.uv, va.ao, ca };
                    ClipVert cvb = { vp * vec4(vb.pos, 1.0f), vb.pos, vb.normal, tb, vb.uv, vb.ao, cb };
                    ClipVert cvc = { vp * vec4(vc.pos, 1.0f), vc.pos, vc.normal, tc, vc.uv, vc.ao, cc };
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
                        {
                            continue;
                        }

                        const vec3 sa = ndc_to_screen(a.c.perspective_divide(), width, height);
                        const vec3 sb = ndc_to_screen(b.c.perspective_divide(), width, height);
                        const vec3 sc = ndc_to_screen(c.c.perspective_divide(), width, height);

                        if (smode == ShadingMode::Phong)
                        {
                            // a.color/b.color/c.color already encode the has_vertex_colors
                            // condition: they are {1,1,1} when p_vcols == nullptr (set at
                            // ClipVert construction), so no ternary is needed here.
                            rasterize_phong(
                                *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, a.pos, b.pos, c.pos, a.normal, b.normal, c.normal,
                                a.tangent, b.tangent, c.tangent, a.uv, b.uv, c.uv, a.ao, b.ao, c.ao, a.color, b.color,
                                c.color, mesh->has_vertex_colors, eye, lights, n_lights, ambient, mat, tex,
                                show_tex ? mesh->tex_at(mat.normal_tex) : nullptr,
                                show_tex ? mesh->tex_at(mat.specular_tex) : nullptr, shadow_map, 0, height - 1,
                                show_metallic ? mesh->tex_at(mat.metallic_roughness_tex) : nullptr,
                                show_emissive ? mesh->tex_at(mat.emissive_tex) : nullptr,
                                mat.effective_emissive(show_emissive)
                            );
                        }
                        else
                        {
                            vec3 col_a; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — always
                                        // overwritten in Flat/Gouraud branches below
                            vec3 col_b; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
                            vec3 col_c; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
                            vec3 shad_a; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — only read
                                         // when shadow_map != nullptr; written before that read
                            vec3 shad_b; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)
                            vec3 shad_c; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

                            if (smode == ShadingMode::Flat)
                            {
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
                                col_a = col_b = col_c = compute_lighting(
                                    assume_unit, fc, face_n, eye, lights, n_lights, ambient, *flat_mat, face_ao
                                );
                                if (shadow_map)
                                {
                                    shad_a = shad_b = shad_c = compute_lighting(
                                        assume_unit, fc, face_n, eye, shadow_lights, n_shadow_lights, ambient,
                                        *flat_mat, face_ao
                                    );
                                }
                            }
                            else // Gouraud
                            {
                                if (p_vcols)
                                {
                                    Material gvcol_mat;
                                    auto gouraud_mat = [&](const vec3 &vcol) -> const Material &
                                    {
                                        if (vcol.x == 1.0f && vcol.y == 1.0f && vcol.z == 1.0f)
                                        {
                                            return mat;
                                        }
                                        gvcol_mat = mat;
                                        gvcol_mat.diffuse = gvcol_mat.diffuse * vcol;
                                        gvcol_mat.ambient = gvcol_mat.ambient * vcol;
                                        return gvcol_mat;
                                    };
                                    col_a = compute_lighting(
                                        assume_unit, a.pos, a.normal, eye, lights, n_lights, ambient,
                                        gouraud_mat(a.color), a.ao
                                    );
                                    col_b = compute_lighting(
                                        assume_unit, b.pos, b.normal, eye, lights, n_lights, ambient,
                                        gouraud_mat(b.color), b.ao
                                    );
                                    col_c = compute_lighting(
                                        assume_unit, c.pos, c.normal, eye, lights, n_lights, ambient,
                                        gouraud_mat(c.color), c.ao
                                    );
                                    if (shadow_map)
                                    {
                                        shad_a = compute_lighting(
                                            assume_unit, a.pos, a.normal, eye, shadow_lights, n_shadow_lights, ambient,
                                            gouraud_mat(a.color), a.ao
                                        );
                                        shad_b = compute_lighting(
                                            assume_unit, b.pos, b.normal, eye, shadow_lights, n_shadow_lights, ambient,
                                            gouraud_mat(b.color), b.ao
                                        );
                                        shad_c = compute_lighting(
                                            assume_unit, c.pos, c.normal, eye, shadow_lights, n_shadow_lights, ambient,
                                            gouraud_mat(c.color), c.ao
                                        );
                                    }
                                }
                                else
                                {
                                    col_a = compute_lighting(
                                        assume_unit, a.pos, a.normal, eye, lights, n_lights, ambient, mat, a.ao
                                    );
                                    col_b = compute_lighting(
                                        assume_unit, b.pos, b.normal, eye, lights, n_lights, ambient, mat, b.ao
                                    );
                                    col_c = compute_lighting(
                                        assume_unit, c.pos, c.normal, eye, lights, n_lights, ambient, mat, c.ao
                                    );
                                    if (shadow_map)
                                    {
                                        shad_a = compute_lighting(
                                            assume_unit, a.pos, a.normal, eye, shadow_lights, n_shadow_lights, ambient,
                                            mat, a.ao
                                        );
                                        shad_b = compute_lighting(
                                            assume_unit, b.pos, b.normal, eye, shadow_lights, n_shadow_lights, ambient,
                                            mat, b.ao
                                        );
                                        shad_c = compute_lighting(
                                            assume_unit, c.pos, c.normal, eye, shadow_lights, n_shadow_lights, ambient,
                                            mat, c.ao
                                        );
                                    }
                                }
                            }

                            rasterize(
                                *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, col_a, col_b, col_c, shad_a, shad_b, shad_c,
                                a.pos, b.pos, c.pos, a.uv, b.uv, c.uv, tex, show_tex ? mat.alpha_cutoff : 0.0f,
                                shadow_map, 0, height - 1, show_emissive ? mesh->tex_at(mat.emissive_tex) : nullptr,
                                mat.effective_emissive(show_emissive)
                            );
                        }
                    }
                }
            }
        }

        // Signal completion. If this is the last worker, wake render().
        if (m_active.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            const std::scoped_lock done_lk(m_mutex);
            m_cv_done.notify_one();
        }
    }
}

// ─── Renderer::render ─────────────────────────────────────────────────────────

void Renderer::render(
    const Mesh &mesh,
    const Camera &camera,
    const Light *lights,
    int n_lights,
    const vec3 &ambient,
    Framebuffer &fb,
    const ShadowMap *shadow_map
)
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
        ClipVert clipped[2][3]; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — hoisted; clip_near
                                // overwrites before read
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
                {
                    continue;
                }
            }

            const ClipVert cva = { vp * vec4(va.pos, 1.0f), va.pos, va.normal, {}, va.uv, va.ao };
            const ClipVert cvb = { vp * vec4(vb.pos, 1.0f), vb.pos, vb.normal, {}, vb.uv, vb.ao };
            const ClipVert cvc = { vp * vec4(vc.pos, 1.0f), vc.pos, vc.normal, {}, vc.uv, vc.ao };

            const int n_tris = clip_near(cva, cvb, cvc, clipped, camera.near_plane);

            for (int ti = 0; ti < n_tris; ti++)
            {
                const ClipVert &a = clipped[ti][0];
                const ClipVert &b = clipped[ti][1];
                const ClipVert &c = clipped[ti][2];

                if (clip_reject(a.c, b.c, c.c))
                {
                    continue;
                }

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

    // ── Dispatch workers for single-pass geometry+rasterize ─────────────────
    m_tri_cursor.store(0, std::memory_order_relaxed);
    {
        const std::scoped_lock lk(m_mutex);
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
        m_active.store(m_n_workers, std::memory_order_release);
        ++m_generation;
    }
    m_cv_work.notify_all();
    {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv_done.wait(lk, [this] { return m_active.load(std::memory_order_acquire) == 0; });
    }
}
