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

namespace
{

    // Convert NDC to top-left-origin screen pixels; preserve depth.
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

    // The tiled path wins on large triangles. Use pixel-weighted area because it
    // represents the triangle under a shaded pixel; measured crossover is 15-25 px.
    constexpr double TILE_MIN_TRI_PX = 32.0;  // measured crossing is 15-25; enter with margin
    constexpr double TILE_KEEP_TRI_PX = 20.0; // stay tiled down to here rather than flip per frame
    constexpr double TILE_MIN_PIXELS = 65536.0;

    // Must match Renderer::TILE_MAX; smaller frames never qualify for tiling.
    constexpr int TILE_EDGE_PX = 32;

    // Thin triangles pay tiled bookkeeping across many tiles, so also require enough
    // covered pixels per tile. This ratio is more stable than sampled bbox fill.
    constexpr double TILE_MIN_TRI_PER_TILE = 34.0;

    // Eight consecutive votes suppress threshold noise without masking sustained changes.
    constexpr int PATH_SWITCH_FRAMES = 8;

    // Minimum tiles per worker before the tile edge is halved (see render()).
    constexpr int TILES_PER_WORKER = 4;

    // True screen area, not a bbox floor; off-screen clipping costs more than it corrects.
    inline float tri_screen_area(const vec3 &sa, const vec3 &sb, const vec3 &sc)
    {
        return 0.5f * std::fabs(((sb.x - sa.x) * (sc.y - sa.y)) - ((sc.x - sa.x) * (sb.y - sa.y)));
    }

    // Estimate touched tiles from bbox size, independent of grid placement. The one-tile
    // floor represents bookkeeping paid even by sub-tile triangles.
    inline float tri_tile_spans(const vec3 &sa, const vec3 &sb, const vec3 &sc)
    {
        const float w = std::max({ sa.x, sb.x, sc.x }) - std::min({ sa.x, sb.x, sc.x }) + 1.0f;
        const float h = std::max({ sa.y, sb.y, sc.y }) - std::min({ sa.y, sb.y, sc.y }) + 1.0f;
        const float tx = std::ceil(std::max(1.0f, w) / static_cast<float>(TILE_EDGE_PX));
        const float ty = std::ceil(std::max(1.0f, h) / static_cast<float>(TILE_EDGE_PX));
        return std::max(1.0f, tx * ty);
    }

    // Ratios cancel this sampling factor; full sampling cost 5-6% on dense meshes.
    constexpr uint32_t AREA_SAMPLE = 8;

} // namespace

int Renderer::resolve_thread_count(int n_threads, bool all_cores_default) noexcept
{
    // Unknown hardware concurrency conservatively means one worker.
    const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
    // Blocks default to four workers to limit CPU; pixel rendering and loading use all cores.
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

// Rasterize stolen chunks through the compile-time sink and shading path. Keep the
// shared geometry front-end in sync with raster_wireframe().

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

    // Transparent uses a private fragment arena and shared pixel heads. clear()
    // retains the arena's high-water capacity; Opaque leaves the handle null.
    if constexpr (S == Sink::Transparent)
    {
        m_arenas[static_cast<size_t>(worker_id)].clear();
    }
    // widen() updates this local once per triangle, avoiding false sharing between
    // adjacent per-worker boxes. Publish it after the worker finishes.
    // NOLINTNEXTLINE(misc-const-correctness), mutated through abuf.box in widen()
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

    // Transparent claims adapt because material grouping clusters large blend surfaces.
    const int chunk = (S == Sink::Transparent) ? m_trans_chunk : choose_phase1_chunk(work, m_n_workers);
    ClipVert clipped[2][3]; // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init): hoisted;
                            // clip_near overwrites before read
    uint32_t drawn = 0;     // post-clip on-screen triangles this worker rasterized (paces the area sampling)
    double area = 0.0;      // summed on-screen triangle area (feeds the path choice)
    double area2 = 0.0;     // summed squared on-screen triangle area (feeds the path choice)
    double span = 0.0;      // summed area^2 / tiles touched (feeds the path choice)

    // Shared shading dispatch for unclipped and near-clipped triangles. Only Flat uses
    // flip_normals here; [[maybe_unused]] keeps the Phong MSVC instantiation warning-free.
    const auto emit = [&](const ClipVert &a, const ClipVert &b, const ClipVert &c, const vec3 &sa, const vec3 &sb,
                          const vec3 &sc, const Material &mat, [[maybe_unused]] bool flip_normals)
    {
        const Texture *tex = show_tex ? mesh->tex_at(mat.diffuse_map.tex) : nullptr;
        if (mesh_has_unlit && mat.unlit)
        {
            // KHR_materials_unlit outputs base color, vertex color, and diffuse texture
            // directly through the flat rasterizer, bypassing all lighting inputs.
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

    // Project, reject empty footprints, and sample surviving geometry for the path
    // predictor. Both unclipped and near-clipped paths use this population.
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

    // Transparent arena growth may throw. Stop that worker at the loop boundary;
    // published chains remain valid and resolve still cleans them. Opaque does not allocate.
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

                // Cull the world-space face plane before matrix transforms. Closed
                // meshes reject about half their triangles here; CCW remains front-facing.
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

                // Reject empty unclipped triangles from clip positions before gathering
                // attributes. Dense meshes can discard most triangles without touching tangents.
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
        // Feed steal-loop wall time to the granularity controller. Claims mix
        // fragment shading with culled front-end work, so counters cannot measure
        // their cost as directly.
        const auto t0 = std::chrono::steady_clock::now();
        try
        {
            steal_loop();
        }
        catch (const std::exception &) // NOLINT(bugprone-empty-catch)
        {
            // reserve_one() allocates before head publication, so a failure leaves all
            // published chains valid. Resolve can composite and clean the partial frame.
        }
        // widen() runs before publication, so allocation failure may overestimate the
        // touched region but can never leave a published head outside it.
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

// Select the compile-time shading path. Keep Wireframe as the unreachable exhaustive case.

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

// Tiled phase 1: run the shared geometry front-end, record surviving triangles,
// and group them by touched tile. Keep front-end changes in all three copies aligned.

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

    // Clear first so allocation failure cannot expose last frame's bins.
    WorkerBins &wb = m_bins[static_cast<size_t>(worker_id)];
    wb.recs.clear();
    wb.clip.clear();
    wb.tile_start.clear();
    wb.sorted.clear();
    // Clear statistics before work because render() sums every worker unconditionally.
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

    // Counting-sort records by tile. Keep tile_start empty on allocation failure so
    // shade_tiles treats this worker as having no contribution.
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
    // Tile touches can exceed the triangle count. Sum in uint64_t to prevent an ILP32
    // wrap from undersizing sorted; length_error is handled like allocation failure.
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

// Tiled phase 2: gather records per tile, resolve visibility, then shade each
// visible triangle through the Deferred sink. Keep shading arguments aligned.

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
        // Sort front-to-back for early depth rejection. Equal-depth z-fights are not
        // reproducible because adding a source-triangle tiebreak costs 1.1-2.5%.
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
        // Once near geometry covers the tile, skip triangles whose nearest vertex is
        // behind its farthest depth. Bound refresh count because each refresh scans the tile;
        // a stale, larger limit can only miss a skip, never reject visible geometry.
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

// Workers steal triangles and draw three depth-tested edges. This reduced geometry
// front-end omits material attributes; keep culling, clipping, and chunk rules aligned
// with raster_triangles().

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

// Resolve disjoint row bands back-to-front. One- and two-deep chains stay inline;
// deeper chains advance in groups to overlap dependent loads. Each resolved head
// returns to SENTINEL for the next frame.

namespace
{
    // One chain entry as the resolve sorts it: the depth it orders on and the ref that finds the
    // payload. 16 bytes against the fragment's 32, so a deep chain sorts half the bytes.
    struct FragKey
    {
        float depth;
        uint64_t ref;
    };

    // Sort farther fragments first. Payload tie-breaks make coplanar alpha compositing
    // reproducible despite nondeterministic cross-worker insertion order.
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
                // Walk several dependent fragment chains together so their cache misses overlap.
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
                    // Resolve one- and two-deep chains inline; deeper chains use the group buffers.
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

                // Guard the only allocating resolve operation. On failure, keep opaque
                // colors but still reset every affected head.
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
                    // Composite over the opaque framebuffer colour. color_at only loads,
                    // so read the order-independent base first and fold into dst.
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

        // Opaque and Wireframe must remain non-allocating because exceptions cannot leave
        // this thread entry point. Allocating passes catch without skipping their barriers.
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
            // Catch each allocating tiled phase separately so neither the in-pass barrier
            // nor final completion signal can be skipped. length_error matters on ILP32 too.
            try
            {
                dispatch_bin(worker_id);
            }
            catch (const std::exception &) // NOLINT(bugprone-empty-catch)
            {
            }
            // Shade only after every worker finishes binning. The already-awake pool
            // yields here instead of paying a second condition-variable dispatch.
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

// Retune transparent steal granularity from measured worker imbalance. Small claims
// balance clustered surfaces but add contention on evenly distributed meshes.
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
    // Atomics cannot resize in place. Retain at most twice the current frame to avoid
    // repeated allocation and first-touch during interactive resizing.
    if (n > m_frag_head.size() || n < m_frag_head.size() / 2)
    {
        m_frag_head = std::vector<std::atomic<uint64_t>>(n);
    }
    // Always sentinel-fill: a fresh atomic vector contains zero, a valid fragment ref.
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

    // Choose immediate or tiled opaque rendering from the previous coherent frame.
    // Tiling wins on large triangles but its extra setup is costly on dense meshes.
    const auto n_px = static_cast<double>(width) * static_cast<double>(height);
    // Under vertical-FOV projection both screen axes scale with framebuffer height,
    // so rescale previous triangle area by the squared height ratio.
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
    // Frame size is a hard same-frame gate; sampled geometry preferences are damped.
    // After growing above the threshold, leftover votes may delay tiling by up to five frames.
    const bool size_ok = same_mesh && n_px >= TILE_MIN_PIXELS;
    const bool stats_want_tiles =
        pxw_tri >= (m_prev_tiled ? TILE_KEEP_TRI_PX : TILE_MIN_TRI_PX) && pxw_per_tile >= TILE_MIN_TRI_PER_TILE;

    // Require persistent disagreement before switching paths. Time damping rejects
    // sampled noise without creating a value dead zone; the first informed choice is immediate.
    bool stats_verdict = stats_want_tiles;
    if (!same_mesh)
    {
        // Clear both verdict and statistics when the mesh changes. Otherwise stale area
        // data immediately re-settles the new scene and delays its first informed choice.
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
    // RasterTri stores tile bounds in uint16_t. Oversized benchmark frames must use
    // the immediate path instead of wrapping a tile axis.
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

    // Run transparency only for a nonempty blend tail. A declared but unused blend
    // material must not pay either barrier or the resolve sweep.
    if (mesh.has_transparent && m_opaque_count < static_cast<uint32_t>(mesh.triangles.size()))
    {
        ensure_abuffer(width, height);

        // Reuse transparent claim tuning for the same mesh across resizes; uniform pixel
        // scaling does not change its worker imbalance.
        if (!same_mesh)
        {
            m_trans_chunk = TRANS_CHUNK_MAX;
        }
        m_tri_cursor.store(static_cast<int>(m_opaque_count), std::memory_order_relaxed);
        dispatch_pass(Pass::TransAccum);
        retune_trans_chunk();

        // Merge worker bounds into the member read by resolve workers. Empty inverted
        // bounds are the min/max identity and skip fully occluded or culled passes.
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
            // Steal bounded row bands, with at least one row per claim. Wide, short
            // boxes may idle workers, but still avoid a full-frame sentinel scan.
            const int bh = m_res_box.y1 - m_res_box.y0 + 1;
            m_res_row_chunk = std::clamp(bh / (m_n_workers * 8), 1, 64);
            // Seed the row cursor to the box top; resolve_pixels() reads the y-start from here.
            m_pixel_cursor.store(m_res_box.y0, std::memory_order_relaxed);
            dispatch_pass(Pass::Resolve);
        }
    }
}
