#include "args.h"
#include "camera.h"
#include "framebuffer.h"
#include "light.h"
#include "linalg.h"
#include "mesh.h"
#include "platform.h"
#include "renderer.h"
#include "shading.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ratio>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{

    volatile sig_atomic_t
        g_interrupted = // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) — written by signal handler
        0;
    void signal_handler(int /*signum*/)
    {
        g_interrupted = 1;
    }

    // Resolve -1 (auto), 0 (all cores), or N (explicit) to a positive thread count.
    int resolve_thread_count(int requested) noexcept
    {
        const int hw = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
        if (requested == 0)
        {
            return hw;
        }
        if (requested < 0)
        {
            return std::min(hw, 4);
        }
        return requested;
    }

    constexpr Color BG_BLACK = { 0, 0, 0 };
    constexpr Color BG_GRAY = { 128, 128, 128 };
    constexpr Color BG_WHITE = { 240, 240, 240 };
    constexpr vec3 FLAT_AMBIENT = { 0.85f, 0.85f, 0.85f };

    // "_of" rather than "_color" to avoid reading like the Renderer::wireframe_color member.
    constexpr Color wireframe_color_of(WireframeColor c) noexcept
    {
        switch (c)
        {
        case WireframeColor::White:
            return { 200, 200, 200 };
        case WireframeColor::Red:
            return { 220, 80, 80 };
        case WireframeColor::Green:
            return { 80, 200, 120 };
        case WireframeColor::Yellow:
            return { 230, 200, 80 };
        case WireframeColor::Cyan:
            return { 100, 200, 220 };
        case WireframeColor::Magenta:
            return { 220, 120, 200 };
        }
        return { 200, 200, 200 };
    }

    constexpr const char *wireframe_name(WireframeColor c) noexcept
    {
        switch (c)
        {
        case WireframeColor::White:
            return "white";
        case WireframeColor::Red:
            return "red";
        case WireframeColor::Green:
            return "green";
        case WireframeColor::Yellow:
            return "yellow";
        case WireframeColor::Cyan:
            return "cyan";
        case WireframeColor::Magenta:
            return "magenta";
        }
        return "white";
    }

    constexpr Color background_color(Background b) noexcept
    {
        switch (b)
        {
        case Background::Gray:
            return BG_GRAY;
        case Background::White:
            return BG_WHITE;
        case Background::Black:
            return BG_BLACK;
        }
        return BG_BLACK;
    }

    constexpr const char *background_name(Background b) noexcept
    {
        switch (b)
        {
        case Background::Gray:
            return "gray";
        case Background::White:
            return "white";
        case Background::Black:
            return "black";
        }
        return "black";
    }

    // Directional lights active for a lighting mode; Flat is ambient-only.
    constexpr int light_count(LightingMode m) noexcept
    {
        switch (m)
        {
        case LightingMode::Single:
            return 1;
        case LightingMode::Flat:
            return 0;
        case LightingMode::Dual:
            return 2;
        }
        return 2;
    }

    // Flat mode swaps the scene ambient for the bright uniform FLAT_AMBIENT so the
    // full model stays visible with no directional lights.
    constexpr vec3 lighting_ambient(LightingMode m, const vec3 &ambient) noexcept
    {
        return m == LightingMode::Flat ? FLAT_AMBIENT : ambient;
    }

    constexpr const char *lighting_name(LightingMode m) noexcept
    {
        switch (m)
        {
        case LightingMode::Single:
            return "single";
        case LightingMode::Flat:
            return "flat";
        case LightingMode::Dual:
            return "dual";
        }
        return "dual";
    }

    // Advance to the next enumerator, wrapping after count (the B/L/C keybindings).
    template <typename E> constexpr E cycle(E v, int count) noexcept
    {
        return static_cast<E>((static_cast<int>(v) + 1) % count);
    }

    Camera auto_fit_camera(const Mesh &mesh)
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
        const vec3 centre = (lo + hi) * 0.5f;
        float radius = (hi - lo).length() * 0.5f;
        if (radius < 1e-4f)
        {
            radius = 1.0f; // degenerate (all coincident vertices) — use sane defaults
        }

        Camera camera;
        camera.target = centre;
        camera.distance = radius * 2.0f;
        // Scale near/far to the model so arbitrarily-sized models aren't clipped.
        camera.near_plane = radius * 0.01f;
        camera.far_plane = radius * 20.0f;
        camera.orientation = quat::from_axis_angle({ 1.0f, 0.0f, 0.0f }, -0.3f);
        return camera;
    }

    void make_default_lights(Light out[2], vec3 &ambient)
    {
        out[0].direction = { 0.408f, 0.816f, 0.408f };
        out[0].color = { 0.85f, 0.77f, 0.64f };
        out[1].direction = { -0.667f, -0.333f, -0.667f };
        out[1].color = { 0.3f, 0.3f, 0.33f };
        ambient = { 0.32f, 0.32f, 0.33f };
    }

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

    void run_bench(const Mesh &mesh, const ParsedArgs &args, double load_ms)
    {
        using clock = std::chrono::steady_clock;

        const int n_threads = resolve_thread_count(args.n_threads);

        Camera camera = auto_fit_camera(mesh);
        Light lights[2];
        vec3 ambient;
        make_default_lights(lights, ambient);

        Framebuffer fb(args.bench_width, args.bench_height, /*headless=*/true);
        Renderer renderer(args.n_threads);
        renderer.mode = args.shading;
        renderer.cull_backfaces = args.cull;
        renderer.show_texture = args.texture;

        const int n_lights = light_count(args.lighting);
        const vec3 cur_ambient = lighting_ambient(args.lighting, ambient);

        const int n_warmup = args.bench_warmup;
        const int n_measure = args.bench;
        std::vector<int64_t> frame_ns;
        frame_ns.reserve(static_cast<size_t>(n_measure));

        auto loop_t0 = clock::now();
        for (int i = 0; i < n_warmup + n_measure; i++)
        {
            camera.spin_world_y(0.8f / 60.0f);
            fb.clear({ 0, 0, 0 });
            auto t0 = clock::now();
            renderer.render(mesh, camera, lights, n_lights, cur_ambient, fb);
            auto t1 = clock::now();
            if (i >= n_warmup)
            {
                frame_ns.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            }
        }
        const double total_ms = std::chrono::duration<double, std::milli>(clock::now() - loop_t0).count();

        // Compute stats via nth_element (linear) — no full sort needed.
        auto ms = [](int64_t ns) { return static_cast<double>(ns) * 1e-6; };

        auto [mn_it, mx_it] = std::minmax_element(frame_ns.begin(), frame_ns.end());
        const int64_t mn = *mn_it;
        const int64_t mx = *mx_it;

        const size_t mid = static_cast<size_t>(n_measure) / 2;
        std::nth_element(frame_ns.begin(), frame_ns.begin() + static_cast<ptrdiff_t>(mid), frame_ns.end());
        const int64_t med = frame_ns[mid];

        const auto p95i = static_cast<size_t>(n_measure * 95 / 100);
        std::nth_element(frame_ns.begin(), frame_ns.begin() + static_cast<ptrdiff_t>(p95i), frame_ns.end());
        const int64_t p95 = frame_ns[p95i];

        // Welford's online algorithm — single pass, numerically stable.
        double welford_mean = 0.0;
        double welford_m2 = 0.0;
        for (int k = 0; k < n_measure; k++)
        {
            const auto x = static_cast<double>(frame_ns[static_cast<size_t>(k)]);
            const double delta = x - welford_mean;
            welford_mean += delta / (k + 1);
            welford_m2 += delta * (x - welford_mean);
        }
        const double stddev_ms = std::sqrt(welford_m2 / static_cast<double>(n_measure)) * 1e-6;

        const double med_ms = ms(med);
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
        std::fprintf(stderr, "  min     %.2f ms\n", ms(mn));
        std::fprintf(
            stderr, "  median  %.2f ms   ~%d fps   %.1f MTri/s   %.1f MVert/s\n", med_ms,
            static_cast<int>(std::lround(fps)), mtri_s, mvert_s
        );
        std::fprintf(stderr, "  p95     %.2f ms\n", ms(p95));
        std::fprintf(stderr, "  max     %.2f ms\n", ms(mx));
        std::fprintf(stderr, "  stddev  %.2f ms\n", stddev_ms);
        std::fprintf(stderr, "  total   %.0f ms  (%d frames incl. warmup)\n", total_ms, n_warmup + n_measure);
    }

} // namespace

int main(int argc, char *argv[])
{
    const ParseResult parsed = parse_args(argc, argv);
    if (!parsed.ok)
    {
        return parsed.exit_code;
    }
    const ParsedArgs &args = parsed.args;

    const bool bench_mode = args.bench > 0;

    // Interactive rendering writes the ANSI frame stream to stdout and reads raw
    // input from stdin, so both must be terminals; a piped/redirected fd is failed
    // loud before the load rather than fed escape soup (or left in a dead input
    // loop). --bench is headless and exempt. Rejecting the display-only stdin case
    // (e.g. `--spin < /dev/null`) is deliberate: without working input the session
    // can only be killed by signal, and raw-mode setup silently fails anyway.
    // color_mode is assigned in the block below (from detect_term_color, or forced by
    // --color when not auto). This initializer is never actually read: the interactive
    // path always overwrites it before the Framebuffer ctor, and --bench returns from
    // run_bench earlier with its own headless framebuffer; it only satisfies the declaration.
    ColorMode color_mode = ColorMode::TrueColor;
    if (!bench_mode)
    {
        if (!platform::is_tty(1))
        {
            std::fprintf(stderr, "%s: stdout is not a terminal\n", program_name(argv[0]));
            return 1;
        }
        if (!platform::is_tty(0))
        {
            std::fprintf(stderr, "%s: stdin is not a terminal\n", program_name(argv[0]));
            return 1;
        }
        // Detection is a pure env read, so both it and the TERM=dumb bail leave the
        // console untouched. The VT-capability gate that DOES mutate the Windows console
        // (init_console_output) is deferred until after the model load succeeds, so a
        // load failure (a typo'd path, the common case) also bails with the console
        // pristine; see below.
        const platform::TermColor tc = platform::detect_term_color();
        if (tc == platform::TermColor::Dumb)
        {
            std::fprintf(stderr, "%s: dumb terminal (TERM=dumb) cannot render\n", program_name(argv[0]));
            return 1;
        }
        // --color overrides only the truecolor-vs-256 choice. TERM=dumb stays fatal even
        // under --color truecolor (dumb means no escape sequences at all, which no color
        // depth fixes), and the Windows VT gate below is likewise not bypassed.
        switch (args.color)
        {
        case ColorChoice::TrueColor:
            color_mode = ColorMode::TrueColor;
            break;
        case ColorChoice::Palette256:
            color_mode = ColorMode::Palette256;
            break;
        case ColorChoice::Auto:
            color_mode = tc == platform::TermColor::TrueColor ? ColorMode::TrueColor : ColorMode::Palette256;
            break;
        }
    }

    const int n_threads = resolve_thread_count(args.n_threads);
    Mesh mesh;
    const auto load_t0 = std::chrono::steady_clock::now();
    if (!mesh.load_model(args.model_path, args.ao, n_threads, args.smooth_angle))
    {
        std::fprintf(
            stderr,
            "%s: failed to load '%s'\n"
            "Supported formats: .obj, .ply, .stl, .gltf, .glb\n",
            program_name(argv[0]), args.model_path.c_str()
        );
        return 1;
    }

    if (bench_mode)
    {
        const double load_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_t0).count();
        run_bench(mesh, args, load_ms);
        return 0;
    }

    // Auto-fit camera: target = bounding-box centre, distance = 2× radius.
    Camera camera = auto_fit_camera(mesh);
    const Camera initial_camera = camera;

    // Extract model basename for the HUD (e.g. "models/suzanne.obj" → "suzanne.obj").
    std::string model_name = args.model_path;
    {
        const size_t slash = model_name.find_last_of("/\\");
        if (slash != std::string::npos)
        {
            model_name = model_name.substr(slash + 1);
        }
    }

    std::signal(SIGINT, signal_handler);  // Ctrl+C
    std::signal(SIGTERM, signal_handler); // kill

    // VT-capability gate, deferred to here (past the model load and the --bench return) so
    // that a load failure leaves the Windows console untouched: on Windows this is the first
    // call that mutates persistent console state (VT flag + UTF-8 code page). A legacy
    // console that cannot enable VT processing cannot render ANSI at all, so fail loud rather
    // than emit escape garbage. POSIX always succeeds. cppcheck reads this condition as
    // constant (why, and the paired unmatchedSuppression guard, are in
    // cppcheck-suppressions.txt); the directive must sit alone on the line above the `if`.
    // cppcheck-suppress knownConditionTrueFalse
    if (!platform::init_console_output())
    {
        std::fprintf(stderr, "%s: console does not support ANSI escape sequences\n", program_name(argv[0]));
        return 1;
    }

    platform::enable_raw_mode();
    platform::enable_mouse();

    int cols = 0;
    int rows = 0;
    platform::get_terminal_size(cols, rows);

    // Each pixel cell covers 2 vertical pixels via ▀ half-block.
    // With the HUD enabled, the last terminal row is reserved for it;
    // --no-hud reclaims that row for rendering.
    const int hud_rows = args.hud ? 1 : 0;
    Framebuffer fb(cols, (rows - hud_rows) * 2, /*headless=*/false, color_mode);

    // Key light: warm white from upper-right-front.
    // Fill light: dim cool blue from lower-left-back, providing contrast.
    Light lights[2];
    vec3 ambient;
    make_default_lights(lights, ambient);

    Renderer renderer(args.n_threads);
    renderer.mode = args.shading;

    const bool has_textures = !mesh.textures.empty();
    float fps_smooth = -1.0f;    // per-frame EMA of the framerate; -1 = uninitialised
    float fps_display = -1.0f;   // value shown in the HUD, latched from fps_smooth; -1 = none yet
    float fps_latch_time = 0.0f; // seconds since the HUD value was last latched
    bool spinning = args.spin;
    bool culling = args.cull;
    bool texturing = args.texture;
    int mouse_last_x = 0; // last seen drag position (terminal cells)
    int mouse_last_y = 0;
    Background bg_mode = args.bg;
    LightingMode lighting_mode = args.lighting;
    WireframeColor wf_color = args.wireframe_color;
    constexpr float spin_speed = 0.8f; // radians/sec
    constexpr float FPS_LATCH = 0.1f;  // seconds between HUD fps refreshes (~10 Hz)

    using clock = std::chrono::steady_clock;
    auto prev = clock::now();
    bool first_frame = true; // frame 1's dt is only the setup gap, not a real frame
    platform::Key held_cam_key = platform::Key::None;
    clock::time_point held_cam_key_tp = clock::now();

    bool running = true;
    while (running)
    {
        // ── Frame timing ──────────────────────────────────────────────────
        auto now = clock::now();
        const float raw_dt = std::chrono::duration<float>(now - prev).count();
        prev = now;
        // Cap dt used for movement/spin so a stall doesn't cause a huge jump.
        const float dt = (raw_dt > 0.1f) ? 0.1f : raw_dt;

        // Smooth the framerate with a per-frame EMA. Skip frame 1: its raw_dt measures
        // only the trivial setup between prev's init and the first sample, so it would
        // seed fps_smooth from an unrepresentative interval. From frame 2 on, raw_dt
        // spans a full render+present. Uses raw_dt (not the movement-capped dt) so
        // slow-model readings stay accurate; the > 0 guard avoids a divide by an
        // exact-zero dt (two clock reads in the same tick).
        if (!first_frame && raw_dt > 0.0f)
        {
            const float fps = 1.0f / raw_dt;
            fps_smooth = (fps_smooth < 0.0f) ? fps : (fps_smooth * 0.9f) + (fps * 0.1f);
        }
        first_frame = false;
        // Latch the HUD value at a fixed ~10 Hz so the digits stay readable instead of
        // blurring at very high frame rates; seed immediately once fps_smooth is valid.
        fps_latch_time += raw_dt;
        if (fps_latch_time >= FPS_LATCH || (fps_display < 0.0f && fps_smooth >= 0.0f))
        {
            fps_display = fps_smooth;
            fps_latch_time = 0.0f;
        }

        // ── Input ─────────────────────────────────────────────────────────
        if (g_interrupted)
        {
            break;
        }

        // Drain all queued input events so held keys and mouse feel responsive.
        while (true)
        {
            const platform::InputEvent ev = platform::poll_event();
            if (ev.type == platform::InputEvent::Type::None)
            {
                break;
            }

            if (ev.type == platform::InputEvent::Type::Key)
            {
                const platform::Key k = ev.key;
                if (k == platform::Key::Q || k == platform::Key::Escape)
                {
                    running = false;
                    break;
                }
                if (k == platform::Key::Space)
                {
                    spinning = !spinning;
                }
                else if (k == platform::Key::Num1)
                {
                    renderer.mode = ShadingMode::Wireframe;
                }
                else if (k == platform::Key::Num2)
                {
                    renderer.mode = ShadingMode::Flat;
                }
                else if (k == platform::Key::Num3)
                {
                    renderer.mode = ShadingMode::Phong;
                }
                else if (k == platform::Key::B)
                {
                    bg_mode = cycle(bg_mode, BACKGROUND_COUNT);
                }
                else if (k == platform::Key::L)
                {
                    lighting_mode = cycle(lighting_mode, LIGHTING_MODE_COUNT);
                }
                else if (k == platform::Key::C)
                {
                    wf_color = cycle(wf_color, WIREFRAME_COLOR_COUNT);
                }
                else if (k == platform::Key::K)
                {
                    culling = !culling;
                }
                else if (k == platform::Key::T)
                {
                    if (has_textures)
                    {
                        texturing = !texturing;
                    }
                }
                else if (k == platform::Key::R)
                {
                    camera = initial_camera;
                    renderer.mode = ShadingMode::Phong;
                    lighting_mode = LightingMode::Dual;
                    bg_mode = Background::Black;
                    wf_color = WireframeColor::White;
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
                const int dx = ev.x - mouse_last_x;
                const int dy = ev.y - mouse_last_y;
                const float dx_rad = static_cast<float>(dx) / static_cast<float>(cols) * 6.2832f;
                const float dy_rad = static_cast<float>(dy) / static_cast<float>(rows) * 3.1416f;
                camera.orbit(dx_rad, -dy_rad);
                mouse_last_x = ev.x;
                mouse_last_y = ev.y;
            }
        }
        if (!running)
        {
            break;
        }

        // ── Camera key movement (once per frame, frame-rate independent) ──
        if (held_cam_key != platform::Key::None)
        {
            const float since = std::chrono::duration<float>(clock::now() - held_cam_key_tp).count();
            if (since > 0.1f)
            {
                held_cam_key = platform::Key::None; // key released
            }
            else
            {
                camera.process_key(held_cam_key, dt);
            }
        }

        // ── Auto-rotation ────────────────────────────────────────────────
        if (spinning)
        {
            camera.spin_world_y(spin_speed * dt);
        }

        // ── Resize detection ─────────────────────────────────────────────
        {
            int new_cols = 0;
            int new_rows = 0;
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
            const char *lighting_str = lighting_name(lighting_mode);
            const char *bg_str = background_name(bg_mode);
            const char *tex_suffix = has_textures ? (texturing ? "  ·  tex: ON  " : "  ·  tex: OFF  ") : "  ";
            const int fps_shown = (fps_display < 0.0f) ? 0 : static_cast<int>(std::lround(fps_display));
            char hud[256];
            if (renderer.mode == ShadingMode::Wireframe)
            {
                std::snprintf(
                    hud, sizeof(hud),
                    "  %s  ·  %d fps  ·  %s  ·  %s  ·  light: %s  ·  bg: %s  ·  wf: %s  ·  cull: %s%s",
                    shading_mode_name(renderer.mode), fps_shown, model_name.c_str(), spinning ? "spin ON" : "spin OFF",
                    lighting_str, bg_str, wireframe_name(wf_color), culling ? "ON" : "OFF", tex_suffix
                );
            }
            else
            {
                std::snprintf(
                    hud, sizeof(hud), "  %s  ·  %d fps  ·  %s  ·  %s  ·  light: %s  ·  bg: %s  ·  cull: %s%s",
                    shading_mode_name(renderer.mode), fps_shown, model_name.c_str(), spinning ? "spin ON" : "spin OFF",
                    lighting_str, bg_str, culling ? "ON" : "OFF", tex_suffix
                );
            }
            fb.set_hud(hud);
        }

        // ── Render ────────────────────────────────────────────────────────
        fb.clear(background_color(bg_mode));
        const int n_lights = light_count(lighting_mode);
        const vec3 cur_ambient = lighting_ambient(lighting_mode, ambient);
        renderer.wireframe_color = wireframe_color_of(wf_color);
        renderer.cull_backfaces = culling;
        renderer.show_texture = texturing;
        renderer.render(mesh, camera, lights, n_lights, cur_ambient, fb);
        fb.present();

        // ── Frame cap ────────────────────────────────────────────────────
        // fps == 0 means uncapped (skip the sleep entirely).
        if (args.fps > 0)
        {
            const float target_dt = 1.0f / static_cast<float>(args.fps);
            auto frame_end = clock::now();
            const float elapsed = std::chrono::duration<float>(frame_end - now).count();
            if (elapsed < target_dt)
            {
                std::this_thread::sleep_for(std::chrono::duration<float>(target_dt - elapsed));
            }
        }
    }

    platform::disable_mouse();
    platform::disable_raw_mode();
    return 0;
}
