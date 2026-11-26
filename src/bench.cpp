#include "src/bench.h"

#include "src/args.h"
#include "src/loaders/mesh.h"
#include "src/math/linalg.h"
#include "src/render/camera.h"
#include "src/render/renderer.h"
#include "src/shading.h"
#include "src/terminal/color.h"
#include "src/terminal/framebuffer.h"
#include "src/terminal/graphics.h"
#include "src/terminal/sixel.h"
#include "src/viewer/scene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <ratio>
#include <vector>

namespace
{
    constexpr const char *shading_mode_name(ShadingMode mode) noexcept
    {
        switch (mode)
        {
        case ShadingMode::Wireframe:
            return "Wireframe";
        case ShadingMode::Flat:
            return "Flat";
        case ShadingMode::Phong:
            return "Phong";
        }
        return "Unknown";
    }

    constexpr const char *backend_name(GraphicsBackend backend) noexcept
    {
        switch (backend)
        {
        case GraphicsBackend::Kitty:
            return "kitty, direct transport";
        case GraphicsBackend::Sixel:
            return "sixel";
        case GraphicsBackend::Blocks:
            return "blocks";
        }
        return "unknown";
    }

    // Kitty's placement keys need a cell count, but no terminal exists to
    // report one. A nominal cell changes only those few digits of the frame.
    constexpr int NOMINAL_CELL_W = 10;
    constexpr int NOMINAL_CELL_H = 20;
} // namespace

void PresentSamples::add(const Framebuffer::PresentStats &stats)
{
    if (stats.bytes == 0)
    {
        empty++;
        return;
    }
    encode_ns.push_back(stats.encode_ns);
    bytes.push_back(static_cast<int64_t>(stats.bytes));
}

std::optional<GraphicsConfig> bench_graphics(const ParsedArgs &args)
{
    GraphicsConfig gfx;
    switch (args.bench_graphics)
    {
    case GraphicsChoice::Auto:
        return std::nullopt;
    case GraphicsChoice::Kitty:
        gfx.backend = GraphicsBackend::Kitty;
        break;
    case GraphicsChoice::Sixel:
        gfx.backend = GraphicsBackend::Sixel;
        break;
    case GraphicsChoice::Blocks:
        gfx.backend = GraphicsBackend::Blocks;
        break;
    }
    gfx.shm = false;
    gfx.cols = std::max(1, args.bench_width / NOMINAL_CELL_W);
    gfx.rows = std::max(1, args.bench_height / NOMINAL_CELL_H);
    gfx.sixel_colors = sixel::cap_to_registers(args.sixel_colors, sixel::MAX_REGISTERS);
    return gfx;
}

BenchStats summarize_frames(std::vector<int64_t> &frame_ns)
{
    BenchStats stats;
    const auto [mn_it, mx_it] = std::minmax_element(frame_ns.begin(), frame_ns.end());
    stats.min_ns = *mn_it;
    stats.max_ns = *mx_it;

    // nth_element is linear; the report needs no full sort.
    const size_t n = frame_ns.size();
    const size_t mid = n / 2;
    std::nth_element(frame_ns.begin(), frame_ns.begin() + static_cast<ptrdiff_t>(mid), frame_ns.end());
    stats.median_ns = frame_ns[mid];

    const size_t p95i = n * 95 / 100;
    std::nth_element(frame_ns.begin(), frame_ns.begin() + static_cast<ptrdiff_t>(p95i), frame_ns.end());
    stats.p95_ns = frame_ns[p95i];

    // Welford's online algorithm: single pass, numerically stable.
    double mean = 0.0;
    double m2 = 0.0;
    for (size_t k = 0; k < n; k++)
    {
        const auto x = static_cast<double>(frame_ns[k]);
        const double delta = x - mean;
        mean += delta / static_cast<double>(k + 1);
        m2 += delta * (x - mean);
    }
    stats.stddev_ms = std::sqrt(m2 / static_cast<double>(n)) * 1e-6;
    return stats;
}

void run_bench(const Mesh &mesh, const ParsedArgs &args, int n_threads, double load_ms)
{
    using clock = std::chrono::steady_clock;

    Camera camera = viewer::auto_fit_camera(mesh, args, /*first_person=*/false);
    const viewer::SceneLights scene = viewer::default_lights();

    const std::optional<GraphicsConfig> gfx = bench_graphics(args);
    const ColorMode color_mode = args.color == ColorChoice::Palette256 ? ColorMode::Palette256 : ColorMode::TrueColor;
    // Renderer must outlive Framebuffer because its borrowed runner captures it.
    Renderer renderer(n_threads);
    Framebuffer fb(args.bench_width, args.bench_height, /*headless=*/true, color_mode, gfx.value_or(GraphicsConfig{}));
    if (gfx)
    {
        // Compose frames without writing them: stdout is not the measured terminal.
        fb.discard_output();
        // Present as the interactive viewer does, on the idle render pool.
        fb.set_parallel_runner({ [&renderer](const std::function<void(int, int)> &fn) { renderer.run_on_workers(fn); },
                                 renderer.worker_count() });
    }
    renderer.mode = args.shading;
    renderer.cull_backfaces = args.cull;
    renderer.show_texture = args.texture;

    const int n_lights = viewer::light_count(args.lighting);
    const vec3 cur_ambient = viewer::lighting_ambient(args.lighting, scene.ambient);

    const int n_warmup = args.bench_warmup;
    const int n_measure = args.bench;
    std::vector<int64_t> frame_ns;
    frame_ns.reserve(static_cast<size_t>(n_measure));
    PresentSamples present;
    if (gfx)
    {
        present.encode_ns.reserve(static_cast<size_t>(n_measure));
        present.bytes.reserve(static_cast<size_t>(n_measure));
    }

    auto loop_t0 = clock::now();
    for (int i = 0; i < n_warmup + n_measure; i++)
    {
        camera.spin_world_y(0.8f / 60.0f);
        fb.clear({ 0, 0, 0 });
        auto t0 = clock::now();
        renderer.render(mesh, camera, scene.lights, n_lights, cur_ambient, fb);
        auto t1 = clock::now();
        if (gfx)
        {
            fb.present();
        }
        if (i >= n_warmup)
        {
            frame_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            if (gfx)
            {
                present.add(fb.last_present());
            }
        }
    }
    const double total_ms = std::chrono::duration<double, std::milli>(clock::now() - loop_t0).count();

    const BenchStats stats = summarize_frames(frame_ns);
    auto ms = [](int64_t ns) { return static_cast<double>(ns) * 1e-6; };
    const double med_ms = ms(stats.median_ns);
    const double fps = med_ms > 0.0 ? 1000.0 / med_ms : 0.0;
    const double mtri_s = med_ms > 0.0 ? static_cast<double>(mesh.triangles.size()) / med_ms * 1e-3 : 0.0;
    const double mvert_s = med_ms > 0.0 ? static_cast<double>(mesh.vertices.size()) / med_ms * 1e-3 : 0.0;

    std::fprintf(
        stderr, "bench: %d frames  %dx%d px  threads=%d  mode=%s  mesh=%zu Tris / %zu Verts\n", n_measure,
        args.bench_width, args.bench_height, n_threads, shading_mode_name(renderer.mode), mesh.triangles.size(),
        mesh.vertices.size()
    );

    std::fprintf(stderr, "startup:\n");
    std::fprintf(stderr, "  load    %.2f ms\n", load_ms);

    std::fprintf(stderr, "runtime:\n");
    std::fprintf(stderr, "  min     %.2f ms\n", ms(stats.min_ns));
    std::fprintf(
        stderr, "  median  %.2f ms   ~%d fps   %.1f MTri/s   %.1f MVert/s\n", med_ms,
        static_cast<int>(std::lround(fps)), mtri_s, mvert_s
    );
    std::fprintf(stderr, "  p95     %.2f ms\n", ms(stats.p95_ns));
    std::fprintf(stderr, "  max     %.2f ms\n", ms(stats.max_ns));
    std::fprintf(stderr, "  stddev  %.2f ms\n", stats.stddev_ms);
    // The parenthesis qualifies the time, not the frame count.
    std::fprintf(
        stderr, "  total   %.0f ms%s  %d frames including warmup\n", total_ms, gfx ? " (render and present)" : "",
        n_warmup + n_measure
    );

    if (!gfx)
    {
        return;
    }
    std::fprintf(stderr, "present (%s):\n", backend_name(gfx->backend));
    if (!present.encode_ns.empty())
    {
        const BenchStats enc = summarize_frames(present.encode_ns);
        const BenchStats bytes = summarize_frames(present.bytes);
        // Small frames, such as at the default bench size, would all print as 0.00 MB.
        const bool in_mb = bytes.max_ns >= 1'000'000;
        const double unit = in_mb ? 1e-6 : 1e-3;
        const char *name = in_mb ? "MB" : "KB";
        auto size = [unit](int64_t n) { return static_cast<double>(n) * unit; };
        std::fprintf(
            stderr, "  encode  median %.2f ms   p95 %.2f ms   max %.2f ms\n", ms(enc.median_ns), ms(enc.p95_ns),
            ms(enc.max_ns)
        );
        std::fprintf(
            stderr, "  bytes   median %.2f %s   min %.2f %s   max %.2f %s per frame\n", size(bytes.median_ns), name,
            size(bytes.min_ns), name, size(bytes.max_ns), name
        );
    }
    if (present.empty > 0)
    {
        std::fprintf(stderr, "  %d of %d frames had nothing to send\n", present.empty, n_measure);
    }
}
