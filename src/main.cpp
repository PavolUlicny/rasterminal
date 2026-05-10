#include "args.h"
#include "camera.h"
#include "framebuffer.h"
#include "light.h"
#include "mesh.h"
#include "platform.h"
#include "renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

static volatile sig_atomic_t g_interrupted = 0;
static void signal_handler(int) { g_interrupted = 1; }

static constexpr Color BG_BLACK = {0, 0, 0};
static constexpr Color BG_GRAY = {128, 128, 128};
static constexpr Color BG_WHITE = {240, 240, 240};
static const vec3 FLAT_AMBIENT = {0.85f, 0.85f, 0.85f};

static constexpr Color WIREFRAME_PALETTE[6] = {
    {200, 200, 200}, // white
    {220, 80, 80},   // red
    {80, 200, 120},  // green
    {230, 200, 80},  // yellow
    {100, 200, 220}, // cyan
    {220, 120, 200}, // magenta
};
static const char *const WIREFRAME_NAMES[6] = {
    "white", "red", "green", "yellow", "cyan", "magenta"};

static Camera auto_fit_camera(const Mesh &mesh)
{
    vec3 lo = mesh.vertices[0].pos;
    vec3 hi = lo;
    for (const Vertex &v : mesh.vertices)
    {
        lo.x = std::min(lo.x, v.pos.x);
        lo.y = std::min(lo.y, v.pos.y);
        lo.z = std::min(lo.z, v.pos.z);
        hi.x = std::max(hi.x, v.pos.x);
        hi.y = std::max(hi.y, v.pos.y);
        hi.z = std::max(hi.z, v.pos.z);
    }
    vec3 centre = (lo + hi) * 0.5f;
    float radius = (hi - lo).length() * 0.5f;
    if (radius < 1e-4f)
        radius = 1.0f; // degenerate (all coincident vertices) — use sane defaults

    Camera camera;
    camera.target = centre;
    camera.distance = radius * 2.0f;
    // Scale near/far to the model so arbitrarily-sized models aren't clipped.
    camera.near_plane = radius * 0.01f;
    camera.far_plane = radius * 20.0f;
    camera.orientation = quat::from_axis_angle({1.0f, 0.0f, 0.0f}, -0.3f);
    return camera;
}

static void make_default_lights(Light out[2], vec3 &ambient)
{
    out[0].direction = {0.408f, 0.816f, 0.408f};
    out[0].color = {1.0f, 0.9f, 0.75f};
    out[1].direction = {-0.667f, -0.333f, -0.667f};
    out[1].color = {0.15f, 0.25f, 0.5f};
    ambient = {0.2f, 0.2f, 0.25f};
}

static const char *shading_mode_name(ShadingMode mode)
{
    switch (mode)
    {
    case ShadingMode::Wireframe:
        return "Wireframe";
    case ShadingMode::Flat:
        return "Flat";
    case ShadingMode::Gouraud:
        return "Gouraud";
    case ShadingMode::Phong:
        return "Phong";
    }
    return "Unknown";
}

static void run_bench(const Mesh &mesh, const ParsedArgs &args)
{
    using clock = std::chrono::steady_clock;

    Camera camera = auto_fit_camera(mesh);
    Light lights[2];
    vec3 ambient;
    make_default_lights(lights, ambient);

    std::optional<double> shadow_ms;
    std::optional<ShadowMap> shadow_map;
    if (args.shadow)
    {
        auto ts = clock::now();
        shadow_map = build_shadow_map(mesh, lights[0]);
        auto te = clock::now();
        shadow_ms = std::chrono::duration<double, std::milli>(te - ts).count();
    }

    Framebuffer fb(args.bench_width, args.bench_height, /*headless=*/true);
    Renderer renderer(args.n_threads);
    renderer.mode = static_cast<ShadingMode>(args.shading);
    renderer.cull_backfaces = args.cull;
    renderer.show_texture = args.texture;

    const int n_lights = args.lighting == 1 ? 1 : (args.lighting == 2 ? 0 : 2);
    const vec3 &cur_ambient = args.lighting == 2 ? FLAT_AMBIENT : ambient;

    const int n_warmup = args.bench_warmup;
    const int n_measure = args.bench;
    std::vector<int64_t> frame_ns;
    frame_ns.reserve(static_cast<size_t>(n_measure));

    auto loop_t0 = clock::now();
    for (int i = 0; i < n_warmup + n_measure; i++)
    {
        camera.spin_world_y(0.8f / 60.0f);
        fb.clear({0, 0, 0});
        auto t0 = clock::now();
        renderer.render(mesh, camera, lights, n_lights, cur_ambient, fb,
                        shadow_map ? &*shadow_map : nullptr);
        auto t1 = clock::now();
        if (i >= n_warmup)
            frame_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    double total_ms = std::chrono::duration<double, std::milli>(clock::now() - loop_t0).count();

    // Compute stats via nth_element (linear) — no full sort needed.
    auto ms = [](int64_t ns)
    { return static_cast<double>(ns) * 1e-6; };

    auto [mn_it, mx_it] = std::minmax_element(frame_ns.begin(), frame_ns.end());
    int64_t mn = *mn_it, mx = *mx_it;

    size_t mid = static_cast<size_t>(n_measure) / 2;
    std::nth_element(frame_ns.begin(), frame_ns.begin() + static_cast<ptrdiff_t>(mid), frame_ns.end());
    int64_t med = frame_ns[mid];

    size_t p95i = static_cast<size_t>(n_measure * 95 / 100);
    std::nth_element(frame_ns.begin(), frame_ns.begin() + static_cast<ptrdiff_t>(p95i), frame_ns.end());
    int64_t p95 = frame_ns[p95i];

    // Welford's online algorithm — single pass, numerically stable.
    double welford_mean = 0.0, welford_m2 = 0.0;
    for (int k = 0; k < n_measure; k++)
    {
        double x = static_cast<double>(frame_ns[static_cast<size_t>(k)]);
        double delta = x - welford_mean;
        welford_mean += delta / (k + 1);
        welford_m2 += delta * (x - welford_mean);
    }
    double stddev_ms = std::sqrt(welford_m2 / static_cast<double>(n_measure)) * 1e-6;

    double med_ms = ms(med);
    double fps = med_ms > 0.0 ? 1000.0 / med_ms : 0.0;
    double mtri_s = med_ms > 0.0 ? static_cast<double>(mesh.triangles.size()) / med_ms * 1e-3 : 0.0;
    double mvert_s = med_ms > 0.0 ? static_cast<double>(mesh.vertices.size()) / med_ms * 1e-3 : 0.0;

    int hw = static_cast<int>(std::thread::hardware_concurrency());
    int threads = args.n_threads == 0  ? hw
                  : args.n_threads < 0 ? std::min(hw, 4)
                                       : args.n_threads;

    std::fprintf(stderr, "bench: %d frames  %dx%d px  threads=%d  mode=%s  (%.0f ms total)\n",
                 n_measure, args.bench_width, args.bench_height, threads, shading_mode_name(renderer.mode), total_ms);
    std::fprintf(stderr, "mesh:    %zu tris  %zu verts\n",
                 mesh.triangles.size(), mesh.vertices.size());
    if (shadow_ms)
        std::fprintf(stderr, "shadow:  %.2f ms  (one-time)\n", *shadow_ms);
    std::fprintf(stderr, "frame:   min=%.2f  med=%.2f  p95=%.2f  max=%.2f  stddev=%.2f ms  (≈ %d fps)\n",
                 ms(mn), med_ms, ms(p95), ms(mx), stddev_ms, static_cast<int>(fps + 0.5));
    std::fprintf(stderr, "         %.1f MTri/s  %.1f MVert/s  (input / median frame)\n", mtri_s, mvert_s);
}

int main(int argc, char *argv[])
{
    ParseResult parsed = parse_args(argc, argv);
    if (!parsed.ok)
        return parsed.exit_code;
    const ParsedArgs &args = parsed.args;

    Mesh mesh;
    if (!mesh.load_model(args.model_path, args.ao))
    {
        std::fprintf(stderr, "Error: failed to load '%s'\n"
                             "       Supported formats: .obj, .ply, .stl, .gltf, .glb\n",
                     args.model_path.c_str());
        return 1;
    }

    if (args.bench >= 1)
    {
        run_bench(mesh, args);
        return 0;
    }

    // Auto-fit camera: target = bounding-box centre, distance = 2× radius.
    Camera camera = auto_fit_camera(mesh);
    const Camera initial_camera = camera;

    // Extract model basename for the HUD (e.g. "models/suzanne.obj" → "suzanne.obj").
    std::string model_name = args.model_path;
    {
        size_t slash = model_name.find_last_of("/\\");
        if (slash != std::string::npos)
            model_name = model_name.substr(slash + 1);
    }

    std::signal(SIGINT, signal_handler);  // Ctrl+C
    std::signal(SIGTERM, signal_handler); // kill

    platform::enable_raw_mode();
    platform::enable_mouse();

    int cols, rows;
    platform::get_terminal_size(cols, rows);

    // Each pixel cell covers 2 vertical pixels via ▀ half-block.
    // With the HUD enabled, the last terminal row is reserved for it;
    // --no-hud reclaims that row for rendering.
    const int hud_rows = args.hud ? 1 : 0;
    Framebuffer fb(cols, (rows - hud_rows) * 2);

    // Key light: warm white from upper-right-front.
    // Fill light: dim cool blue from lower-left-back, providing contrast.
    Light lights[2];
    vec3 ambient;
    make_default_lights(lights, ambient);

    // Build shadow map once — the key light and mesh geometry are static,
    // so the map never changes regardless of camera movement or spin.
    std::optional<ShadowMap> shadow_map;
    if (args.shadow)
        shadow_map = build_shadow_map(mesh, lights[0]);

    Renderer renderer(args.n_threads);
    renderer.mode = static_cast<ShadingMode>(args.shading);

    const bool has_textures = !mesh.textures.empty();
    float fps_smooth = -1.0f; // exponential moving average; -1 = uninitialised
    bool spinning = args.spin;
    bool culling = args.cull;
    bool texturing = args.texture;
    int mouse_last_x = 0, mouse_last_y = 0; // last seen drag position (terminal cells)
    int bg_mode = args.bg;                  // 0=black, 1=gray, 2=white
    int lighting_mode = args.lighting;      // 0=dual, 1=single, 2=flat ambient
    int wf_color = args.wireframe_color;    // 0=white..5=magenta
    const float spin_speed = 0.8f;          // radians/sec

    using clock = std::chrono::steady_clock;
    auto prev = clock::now();
    platform::Key held_cam_key = platform::KEY_NONE;
    clock::time_point held_cam_key_tp = clock::now();

    while (true)
    {
        // ── Frame timing ──────────────────────────────────────────────────
        auto now = clock::now();
        float raw_dt = std::chrono::duration<float>(now - prev).count();
        prev = now;
        // Cap dt used for movement/spin so a stall doesn't cause a huge jump.
        float dt = (raw_dt > 0.1f) ? 0.1f : raw_dt;

        // Use raw_dt for fps so the 0.1s cap doesn't corrupt slow-model readings.
        // Skip the near-zero first frame (prev was just set); seed directly on
        // the second frame so the display is accurate from the start.
        if (raw_dt > 0.001f)
        {
            float fps = 1.0f / raw_dt;
            fps_smooth = (fps_smooth < 0.0f) ? fps : fps_smooth * 0.9f + fps * 0.1f;
        }

        // ── Input ─────────────────────────────────────────────────────────
        if (g_interrupted)
            goto quit;

        // Drain all queued input events so held keys and mouse feel responsive.
        while (true)
        {
            platform::InputEvent ev = platform::poll_event();
            if (ev.type == platform::InputEvent::Type::None)
                break;

            if (ev.type == platform::InputEvent::Type::Key)
            {
                platform::Key k = ev.key;
                if (k == platform::KEY_Q || k == platform::KEY_ESCAPE)
                    goto quit;
                if (k == platform::KEY_SPACE)
                    spinning = !spinning;
                else if (k == platform::KEY_1)
                    renderer.mode = ShadingMode::Wireframe;
                else if (k == platform::KEY_2)
                    renderer.mode = ShadingMode::Flat;
                else if (k == platform::KEY_3)
                    renderer.mode = ShadingMode::Gouraud;
                else if (k == platform::KEY_4)
                    renderer.mode = ShadingMode::Phong;
                else if (k == platform::KEY_B)
                    bg_mode = (bg_mode + 1) % 3;
                else if (k == platform::KEY_L)
                    lighting_mode = (lighting_mode + 1) % 3;
                else if (k == platform::KEY_C)
                    wf_color = (wf_color + 1) % 6;
                else if (k == platform::KEY_K)
                    culling = !culling;
                else if (k == platform::KEY_T)
                {
                    if (has_textures)
                        texturing = !texturing;
                }
                else if (k == platform::KEY_R)
                {
                    camera = initial_camera;
                    renderer.mode = ShadingMode::Gouraud;
                    lighting_mode = 0;
                    bg_mode = 0;
                    wf_color = 0;
                    spinning = false;
                    culling = true;
                    texturing = true;
                }
                else
                {
                    held_cam_key = k;
                    held_cam_key_tp = clock::now();
                }
            }
            else if (ev.type == platform::InputEvent::Type::ScrollUp)
            {
                camera.distance *= 0.92f;
                camera.distance = std::max(camera.distance, camera.near_plane * 2.0f);
            }
            else if (ev.type == platform::InputEvent::Type::ScrollDown)
            {
                camera.distance *= 1.08f;
                camera.distance = std::min(camera.distance, camera.far_plane * 0.5f);
            }
            else if (ev.type == platform::InputEvent::Type::MousePress)
            {
                // Record position so the first drag delta starts from here.
                mouse_last_x = ev.x;
                mouse_last_y = ev.y;
            }
            else if (ev.type == platform::InputEvent::Type::MouseMove)
            {
                int dx = ev.x - mouse_last_x;
                int dy = ev.y - mouse_last_y;
                float dx_rad = static_cast<float>(dx) / static_cast<float>(cols) * 6.2832f;
                float dy_rad = static_cast<float>(dy) / static_cast<float>(rows) * 3.1416f;
                camera.orbit(dx_rad, -dy_rad);
                mouse_last_x = ev.x;
                mouse_last_y = ev.y;
            }
        }

        // ── Camera key movement (once per frame, frame-rate independent) ──
        if (held_cam_key != platform::KEY_NONE)
        {
            float since = std::chrono::duration<float>(clock::now() - held_cam_key_tp).count();
            if (since > 0.1f)
                held_cam_key = platform::KEY_NONE; // key released
            else
                camera.process_key(held_cam_key, dt);
        }

        // ── Auto-rotation ────────────────────────────────────────────────
        if (spinning)
            camera.spin_world_y(spin_speed * dt);

        // ── Resize detection ─────────────────────────────────────────────
        {
            int new_cols, new_rows;
            platform::get_terminal_size(new_cols, new_rows);
            if (new_cols != cols || new_rows != rows)
            {
                cols = new_cols;
                rows = new_rows;
                fb.resize(cols, (rows - hud_rows) * 2);
            }
        }

        // ── HUD ───────────────────────────────────────────────────────────
        if (args.hud)
        {
            const char *lighting_str;
            switch (lighting_mode)
            {
            case 1:
                lighting_str = "single";
                break;
            case 2:
                lighting_str = "flat";
                break;
            default:
                lighting_str = "dual";
                break;
            }

            const char *bg_str;
            switch (bg_mode)
            {
            case 1:
                bg_str = "gray";
                break;
            case 2:
                bg_str = "white";
                break;
            default:
                bg_str = "black";
                break;
            }

            const char *tex_suffix = has_textures ? (texturing ? "  ·  tex: ON  " : "  ·  tex: OFF  ") : "  ";
            char hud[256];
            if (renderer.mode == ShadingMode::Wireframe)
                std::snprintf(hud, sizeof(hud), "  %s  ·  %d fps  ·  %s  ·  %s  ·  light: %s  ·  bg: %s  ·  wf: %s  ·  cull: %s%s",
                              shading_mode_name(renderer.mode), (fps_smooth < 0.0f) ? 0 : static_cast<int>(fps_smooth), model_name.c_str(),
                              spinning ? "spin ON" : "spin OFF",
                              lighting_str, bg_str, WIREFRAME_NAMES[wf_color],
                              culling ? "ON" : "OFF", tex_suffix);
            else
                std::snprintf(hud, sizeof(hud), "  %s  ·  %d fps  ·  %s  ·  %s  ·  light: %s  ·  bg: %s  ·  cull: %s%s",
                              shading_mode_name(renderer.mode), (fps_smooth < 0.0f) ? 0 : static_cast<int>(fps_smooth), model_name.c_str(),
                              spinning ? "spin ON" : "spin OFF",
                              lighting_str, bg_str, culling ? "ON" : "OFF", tex_suffix);
            fb.set_hud(hud);
        }

        // ── Render ────────────────────────────────────────────────────────
        Color bg_color;
        switch (bg_mode)
        {
        case 1:
            bg_color = BG_GRAY;
            break;
        case 2:
            bg_color = BG_WHITE;
            break;
        default:
            bg_color = BG_BLACK;
            break;
        }
        fb.clear(bg_color);
        // Select light set based on lighting mode.
        // Flat ambient: no directional lights, bright ambient so the full model is visible.
        int n_lights;
        switch (lighting_mode)
        {
        case 1:
            n_lights = 1;
            break;
        case 2:
            n_lights = 0;
            break;
        default:
            n_lights = 2;
            break;
        }
        const vec3 &cur_ambient = lighting_mode == 2 ? FLAT_AMBIENT : ambient;
        renderer.wireframe_color = WIREFRAME_PALETTE[wf_color];
        renderer.cull_backfaces = culling;
        renderer.show_texture = texturing;
        renderer.render(mesh, camera, lights, n_lights, cur_ambient, fb,
                        shadow_map ? &*shadow_map : nullptr);
        fb.present();

        // ── Frame cap ────────────────────────────────────────────────────
        // fps == 0 means uncapped (skip the sleep entirely).
        if (args.fps > 0)
        {
            const float target_dt = 1.0f / static_cast<float>(args.fps);
            auto frame_end = clock::now();
            float elapsed = std::chrono::duration<float>(frame_end - now).count();
            if (elapsed < target_dt)
                std::this_thread::sleep_for(
                    std::chrono::duration<float>(target_dt - elapsed));
        }
    }

quit:
    platform::disable_mouse();
    platform::disable_raw_mode();
    return 0;
}
