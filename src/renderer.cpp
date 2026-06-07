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
#include <cstdint>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
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
    m_arenas.resize(static_cast<size_t>(m_n_workers)); // one transparent-fragment arena per worker
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

// ─── Renderer::raster_triangles ───────────────────────────────────────────────
// Single-pass geometry + rasterize over this worker's stolen triangle chunks.
// Each worker steals triangle chunks and rasterizes directly into the framebuffer.
// The CAS-based depth test in Framebuffer ensures the closest triangle wins per
// pixel across all threads. A narrow race between a winning depth CAS and the
// following color write is accepted: at most one wrong-coloured pixel per collision
// per frame, invisible at interactive frame rates.
//
// S == Opaque processes [0, opaque_count) and commits to the framebuffer — codegen
// is identical to the pre-transparency single pass. S == Transparent processes the
// blend tail [opaque_count, total) and pushes shaded fragments into this worker's
// A-buffer arena instead (the per-pixel resolve composites them later).

template <Sink S> void Renderer::raster_triangles(int worker_id)
{
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
        // Texture toggle gates only the emissive texture sample. The authored factor
        // (mat.emissive) always passes through, mirroring how mat.diffuse stays in effect
        // even when diffuse_tex is hidden by the toggle.
        const bool show_emissive = mesh->has_emissive && show_tex;
        const bool show_metallic = mesh->has_metallic && show_tex;
        const bool apply_normal_scale = mesh->has_normal_scale && show_tex;
        const bool show_occlusion = mesh->has_occlusion && show_tex;
        const bool mesh_has_unlit = mesh->has_unlit;
        Framebuffer *fb = m_fb;
        const Light *shadow_lights = (n_lights > 0) ? lights + 1 : lights;
        const int n_shadow_lights = (n_lights > 0) ? n_lights - 1 : 0;

        // Opaque steals [0, opaque_count); transparent steals the blend tail
        // [opaque_count, total). render() seeds m_tri_cursor to the matching start.
        const int total = static_cast<int>(S == Sink::Opaque ? m_opaque_count : mesh->triangles.size());
        const int work = (S == Sink::Opaque) ? total : (total - static_cast<int>(m_opaque_count));
        const vec3 *p_tans = (smode == ShadingMode::Phong) ? mesh->tangents.data() : nullptr;
        const vec3 *p_vcols = mesh->has_vertex_colors ? mesh->vertex_colors.data() : nullptr;
        [[maybe_unused]] const float *p_valpha = mesh->has_vertex_alpha ? mesh->vertex_alpha.data() : nullptr;

        // Transparent: this worker's private fragment arena + the shared per-pixel head
        // array. clear() keeps capacity, acting as the per-frame high-water reserve so
        // steady-state pushes never reallocate. The handle is built once (const); for
        // Opaque it stays default (null) and unused.
        if constexpr (S == Sink::Transparent)
        {
            m_arenas[static_cast<size_t>(worker_id)].clear();
        }
        const ABuffer abuf = [&]
        {
            ABuffer a;
            if constexpr (S == Sink::Transparent)
            {
                a.head = m_frag_head.data();
                a.nodes = &m_arenas[static_cast<size_t>(worker_id)];
                a.worker_id = static_cast<uint32_t>(worker_id);
            }
            return a;
        }();

        const int chunk = choose_phase1_chunk(work, m_n_workers);
        ClipVert clipped[2][3]; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init) — hoisted;
                                // clip_near overwrites before read

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
                    if constexpr (S == Sink::Transparent)
                    {
                        if (p_valpha)
                        {
                            cva.color_a = p_valpha[tri.v[0]];
                            cvb.color_a = p_valpha[tri.v[1]];
                            cvc.color_a = p_valpha[tri.v[2]];
                        }
                    }
                    if (flip_normals)
                    {
                        cva.normal = cva.normal * -1.0f;
                        cvb.normal = cvb.normal * -1.0f;
                        cvc.normal = cvc.normal * -1.0f;
                    }

                    // Fast path: when all three vertices are in front of the near plane
                    // (the overwhelmingly common case) clip_near would only copy them into
                    // clipped[] verbatim and the loop would read them back. Skip the call and
                    // the ~228 B round trip by pointing straight at cva/cvb/cvc; clip_near
                    // runs only for the rare straddle cases (a vertex behind the near plane).
                    const ClipVert *tris[2][3];
                    int n_tris; // NOLINT(cppcoreguidelines-init-variables) — assigned in both branches before read
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

                        if (mesh_has_unlit && mat.unlit)
                        {
                            // KHR_materials_unlit: output baseColor * vertexColor * diffuse
                            // texture directly, bypassing lighting/shadow/ambient/emissive/
                            // normal/occlusion regardless of the active shading mode. Reuses the
                            // flat/gouraud rasterizer with the raw base colour as the per-vertex
                            // colour, a null shadow map, and zero emissive. a.color is {1,1,1}
                            // when the mesh has no vertex colours (set at ClipVert construction),
                            // so this reduces to mat.diffuse there. Shadow colours are passed as
                            // the same value to stay initialised; the null shadow map skips them.
                            const vec3 ua = mat.diffuse * a.color;
                            const vec3 ub = mat.diffuse * b.color;
                            const vec3 uc = mat.diffuse * c.color;
                            if constexpr (S == Sink::Opaque)
                            {
                                rasterize<Sink::Opaque>(
                                    *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, ua, ub, uc, ua, ub, uc, a.pos, b.pos, c.pos,
                                    a.uv, b.uv, c.uv, tex, show_tex ? mat.alpha_cutoff : 0.0f, nullptr, 0, height - 1,
                                    nullptr, vec3{ 0.0f, 0.0f, 0.0f }
                                );
                            }
                            else
                            {
                                rasterize<Sink::Transparent>(
                                    *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, ua, ub, uc, ua, ub, uc, a.pos, b.pos, c.pos,
                                    a.uv, b.uv, c.uv, tex, show_tex ? mat.alpha_cutoff : 0.0f, nullptr, 0, height - 1,
                                    nullptr, vec3{ 0.0f, 0.0f, 0.0f }, &abuf, mat.alpha, a.color_a, b.color_a, c.color_a
                                );
                            }
                        }
                        else if (smode == ShadingMode::Phong)
                        {
                            // a.color/b.color/c.color already encode the has_vertex_colors
                            // condition: they are {1,1,1} when p_vcols == nullptr (set at
                            // ClipVert construction), so no ternary is needed here.
                            if constexpr (S == Sink::Opaque)
                            {
                                rasterize_phong<Sink::Opaque>(
                                    *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, a.pos, b.pos, c.pos, a.normal, b.normal,
                                    c.normal, a.tangent, b.tangent, c.tangent, a.uv, b.uv, c.uv, a.ao, b.ao, c.ao,
                                    a.color, b.color, c.color, mesh->has_vertex_colors, eye, lights, n_lights, ambient,
                                    mat, tex, show_tex ? mesh->tex_at(mat.normal_tex) : nullptr,
                                    show_tex ? mesh->tex_at(mat.specular_tex) : nullptr, shadow_map, 0, height - 1,
                                    show_metallic ? mesh->tex_at(mat.metallic_roughness_tex) : nullptr,
                                    show_emissive ? mesh->tex_at(mat.emissive_tex) : nullptr, mat.emissive,
                                    apply_normal_scale, show_occlusion ? mesh->tex_at(mat.occlusion_tex) : nullptr,
                                    mat.occlusion_strength
                                );
                            }
                            else
                            {
                                rasterize_phong<Sink::Transparent>(
                                    *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, a.pos, b.pos, c.pos, a.normal, b.normal,
                                    c.normal, a.tangent, b.tangent, c.tangent, a.uv, b.uv, c.uv, a.ao, b.ao, c.ao,
                                    a.color, b.color, c.color, mesh->has_vertex_colors, eye, lights, n_lights, ambient,
                                    mat, tex, show_tex ? mesh->tex_at(mat.normal_tex) : nullptr,
                                    show_tex ? mesh->tex_at(mat.specular_tex) : nullptr, shadow_map, 0, height - 1,
                                    show_metallic ? mesh->tex_at(mat.metallic_roughness_tex) : nullptr,
                                    show_emissive ? mesh->tex_at(mat.emissive_tex) : nullptr, mat.emissive,
                                    apply_normal_scale, show_occlusion ? mesh->tex_at(mat.occlusion_tex) : nullptr,
                                    mat.occlusion_strength, &abuf, a.color_a, b.color_a, c.color_a
                                );
                            }
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

                            if constexpr (S == Sink::Opaque)
                            {
                                rasterize<Sink::Opaque>(
                                    *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, col_a, col_b, col_c, shad_a, shad_b, shad_c,
                                    a.pos, b.pos, c.pos, a.uv, b.uv, c.uv, tex, show_tex ? mat.alpha_cutoff : 0.0f,
                                    shadow_map, 0, height - 1, show_emissive ? mesh->tex_at(mat.emissive_tex) : nullptr,
                                    mat.emissive
                                );
                            }
                            else
                            {
                                rasterize<Sink::Transparent>(
                                    *fb, sa, sb, sc, a.c.w, b.c.w, c.c.w, col_a, col_b, col_c, shad_a, shad_b, shad_c,
                                    a.pos, b.pos, c.pos, a.uv, b.uv, c.uv, tex, show_tex ? mat.alpha_cutoff : 0.0f,
                                    shadow_map, 0, height - 1, show_emissive ? mesh->tex_at(mat.emissive_tex) : nullptr,
                                    mat.emissive, &abuf, mat.alpha, a.color_a, b.color_a, c.color_a
                                );
                            }
                        }
                    }
                }
            }
        }; // end steal_loop lambda

        if constexpr (S == Sink::Transparent)
        {
            try
            {
                steal_loop();
            }
            catch (const std::bad_alloc &) // NOLINT(bugprone-empty-catch)
            {
                // Best-effort: stop pushing this worker's fragments. The chain stays consistent
                // (push_back runs before the head swap), the worker still signals completion, and
                // resolve still composites + self-cleans — so the frame loses a few fragments
                // under extreme overdraw rather than crashing or corrupting the next frame.
            }
        }
        else
        {
            steal_loop();
        }
    }
}

// ─── Renderer::resolve_pixels ─────────────────────────────────────────────────
// Transparent resolve pass: each worker steals disjoint pixel ranges and composites
// that pixel's accumulated fragment list back-to-front over the opaque colour already
// in the framebuffer. Disjoint pixels + the post-accumulate barrier make the
// single-threaded get_pixel/set_pixel safe here (no two workers touch one slot; the
// half-block 2-px-per-cell packing is a present()-only concern). Each resolved head is
// reset to SENTINEL so the array self-cleans for the next frame.

void Renderer::resolve_pixels(int worker_id)
{
    (void)worker_id;
    Framebuffer *fb = m_fb;
    const int width = m_width;
    const int total_px = width * m_height;
    constexpr int PX_CHUNK = 4096;
    constexpr float inv255 = 1.0f / 255.0f;

    std::vector<Fragment> stack; // reused across pixels; per-worker, no sharing

    while (true)
    {
        const int start = m_pixel_cursor.fetch_add(PX_CHUNK, std::memory_order_relaxed);
        if (start >= total_px)
        {
            break;
        }
        const int end = std::min(start + PX_CHUNK, total_px);
        for (int idx = start; idx < end; idx++)
        {
            uint64_t ref = m_frag_head[static_cast<size_t>(idx)].load(std::memory_order_relaxed);
            if (ref == ABuffer::SENTINEL)
            {
                continue;
            }

            // Gather this pixel's chain. push_back is the only allocating call in resolve;
            // guard it like the accumulate pass so an OOM here can't escape worker_func (a
            // std::thread entry) into std::terminate. On OOM we composite nothing for this
            // pixel (it keeps its opaque colour) — but the head reset below still runs
            // UNCONDITIONALLY, so no stale non-sentinel head survives to corrupt the next
            // frame (the self-cleaning invariant the design relies on). Far less likely than
            // accumulate OOM: the chain's fragments were already allocated in that pass.
            stack.clear();
            try
            {
                while (ref != ABuffer::SENTINEL)
                {
                    const Fragment &f = m_arenas[ref >> 32u][static_cast<uint32_t>(ref & 0xFFFFFFFFu)];
                    stack.push_back(f);
                    ref = f.next;
                }
            }
            catch (const std::bad_alloc &)
            {
                stack.clear();
            }

            // Back-to-front: composite far (greater depth) fragments first. The depth ties are
            // broken on the fragment payload so the composite is reproducible: the A-buffer chain
            // order is nondeterministic (cross-worker atomic exchanges) and the alpha-OVER is not
            // commutative, so without a deterministic tie-break two coplanar fragments at one pixel
            // would flicker frame-to-frame. Fragments equal on every field are identical, so their
            // relative order then cannot affect the result. Skip the sort for the common
            // single-fragment pixel (nothing to order).
            if (stack.size() > 1)
            {
                std::sort(
                    stack.begin(), stack.end(),
                    [](const Fragment &x, const Fragment &y)
                    {
                        if (x.depth != y.depth)
                        {
                            return x.depth > y.depth;
                        }
                        if (x.color.x != y.color.x)
                        {
                            return x.color.x < y.color.x;
                        }
                        if (x.color.y != y.color.y)
                        {
                            return x.color.y < y.color.y;
                        }
                        if (x.color.z != y.color.z)
                        {
                            return x.color.z < y.color.z;
                        }
                        return x.alpha < y.alpha;
                    }
                );
            }

            const Color base = fb->color_at(static_cast<size_t>(idx));
            vec3 dst{ static_cast<float>(base.r) * inv255, static_cast<float>(base.g) * inv255,
                      static_cast<float>(base.b) * inv255 };
            for (const Fragment &f : stack)
            {
                dst = f.color * f.alpha + dst * (1.0f - f.alpha);
            }
            fb->set_color_at(static_cast<size_t>(idx), vec3_to_color(dst));
            m_frag_head[static_cast<size_t>(idx)].store(ABuffer::SENTINEL, std::memory_order_relaxed);
        }
    }
}

// ─── Renderer::worker_func ────────────────────────────────────────────────────
// Persistent worker loop: sleep until render() dispatches a phase, run it, signal done.

void Renderer::worker_func(int worker_id)
{
    int my_gen = 0;
    while (true)
    {
        Pass pass; // NOLINT(cppcoreguidelines-init-variables) — assigned under the lock below before use
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

        switch (pass)
        {
        case Pass::Opaque:
            raster_triangles<Sink::Opaque>(worker_id);
            break;
        case Pass::TransAccum:
            raster_triangles<Sink::Transparent>(worker_id);
            break;
        case Pass::Resolve:
            resolve_pixels(worker_id);
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

// ─── Renderer::ensure_abuffer ─────────────────────────────────────────────────
// Size the per-pixel head array to the framebuffer and sentinel-fill it. Only does
// work when the dimensions change (or on first use): in steady state the resolve pass
// restores every touched head to SENTINEL, so the array is already clean each frame.
// std::vector<std::atomic> can't be resized in place (atomics aren't movable), so a
// size change rebuilds the vector.

void Renderer::ensure_abuffer(int width, int height)
{
    if (width == m_ab_width && height == m_ab_height && !m_frag_head.empty())
    {
        return;
    }
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<std::atomic<uint64_t>> head(n);
    for (auto &h : head)
    {
        h.store(ABuffer::SENTINEL, std::memory_order_relaxed);
    }
    m_frag_head = std::move(head);
    m_ab_width = width;
    m_ab_height = height;
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

            // Fast path: skip clip_near and its copy when no vertex is behind the near
            // plane (see worker_func). clip_near runs only for the rare straddle cases.
            const ClipVert *tris[2][3];
            int n_tris; // NOLINT(cppcoreguidelines-init-variables) — assigned in both branches before read
            if (cva.c.w > camera.near_plane && cvb.c.w > camera.near_plane && cvc.c.w > camera.near_plane)
            {
                n_tris = 1;
                tris[0][0] = &cva;
                tris[0][1] = &cvb;
                tris[0][2] = &cvc;
            }
            else
            {
                n_tris = clip_near(cva, cvb, cvc, clipped, camera.near_plane);
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

                const Color wf = wireframe_color;
                draw_line(fb, sa, sb, wf);
                draw_line(fb, sb, sc, wf);
                draw_line(fb, sc, sa, wf);
            }
        }
        return;
    }

    // Frame inputs are written once under the lock and stay constant across all phases.
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
        // Opaque range. has_transparent meshes carry a real opaque_count from load_model;
        // for everything else the opaque pass covers all triangles (a manually built Mesh
        // may leave opaque_count at 0, so don't trust it unless has_transparent is set).
        m_opaque_count = mesh.has_transparent ? mesh.opaque_count : static_cast<uint32_t>(mesh.triangles.size());
    }

    // dispatch(pass): bump the generation, wake the workers, block until all finish.
    const auto dispatch = [this](Pass pass)
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
    };

    // Phase 1: opaque geometry over [0, opaque_count).
    m_tri_cursor.store(0, std::memory_order_relaxed);
    dispatch(Pass::Opaque);

    // Phases 2-3: only meshes with blend materials. Accumulate transparent fragments into
    // the per-pixel A-buffer, then resolve (sort + composite) over the opaque framebuffer.
    if (mesh.has_transparent)
    {
        ensure_abuffer(width, height);

        m_tri_cursor.store(static_cast<int>(m_opaque_count), std::memory_order_relaxed);
        dispatch(Pass::TransAccum);

        m_pixel_cursor.store(0, std::memory_order_relaxed);
        dispatch(Pass::Resolve);
    }
}
