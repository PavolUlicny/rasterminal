#include "args.h"
#include "camera.h"
#include "color.h"
#include "framebuffer.h"
#include "hud.h"
#include "input.h"
#include "light.h"
#include "linalg.h"
#include "mesh.h"
#include "platform.h"
#include "renderer.h"
#include "shading.h"
#include "text.h"

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

    // first_person is passed rather than read off `args` because --bench must not
    // inherit it: the bench camera spins every frame, which is a turntable operation
    // (it sweeps the model to sample viewpoints), and in first-person the same call
    // pans the view in place instead, sending the model out of frame and timing mostly
    // empty ones (measured on Duck.glb: 93.4 against 39.2 MTri/s). Passing false there
    // also drops the --pitch clamp, so the bench camera is identical either way.
    Camera auto_fit_camera(const Mesh &mesh, const ParsedArgs &args, bool first_person)
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
        camera.first_person = first_person;
        camera.fp_centre = centre;
        // Movement scaled to the model, for the same reason the zoom step scales with
        // distance: at multiplier 1 the model's diameter takes about two seconds to
        // cross, so the keys feel the same on a 0.01-unit model and a 10000-unit one.
        // `radius` is half the bounding-box diagonal, so a strongly anisotropic model
        // (a long thin one) is scaled by its long axis and even FP_SPEED_MIN crosses the
        // narrow one quickly. Accepted: sizing from the smallest extent instead would
        // make every ordinary model crawl, and the fix if it ever bites is to lower
        // FP_SPEED_MIN, now that the flag's range derives from that one constant.
        camera.fp_base_speed = radius;
        camera.fp_speed = args.first_person_speed;
        camera.target = centre;
        // --zoom's parse-time bound [0.2, 100] lands the distance inside the
        // interactive clamp [near*2, far*0.5] by construction, so no clamp here.
        camera.distance = radius * 2.0f / args.zoom;
        // Scale near/far to the model so arbitrarily-sized models aren't clipped.
        // Accepted in first-person: orbit can never approach closer than near_plane * 2,
        // but flying has no inner bound by design, so a surface can be pushed inside the
        // near plane and vanish before the camera reaches it. Shrinking near for the
        // mode would cost depth precision across the whole scene to fix the last few
        // centimetres of approach.
        camera.near_plane = radius * 0.01f;
        camera.far_plane = radius * 20.0f;
        // Initial pose via orbit() from the identity orientation, the owner of the
        // turntable composition, so the flags reach exactly the drag-reachable pose
        // family. orbit() negates dx (screen-drag convention), so pass -yaw to keep
        // positive --yaw = positive world-Y spin: the model's front moves left on
        // screen, like --spin-direction left. As with spin, that on-screen direction
        // reads mirrored when --pitch past +-90 puts the view upside down (accepted).
        //
        // orbit(), not look(): the launch pose is built as a turntable pose in both
        // modes, which is also what makes it a valid first-person state, since
        // `target` is left at the centre and the eye lands `distance` away along the
        // camera's back axis, exactly the target = eye + forward * distance invariant
        // first-person maintains. --first-person clamps the pitch, since a fly camera
        // must not start upside down and --pitch accepts past +-90. Clamped to
        // Camera::FP_MAX_PITCH, the same limit look() enforces, not to a round 90: the
        // launch pose has to be a pose the mode can hold, or the first look input of
        // any direction would be forced to pitch back off the pole.
        const float pitch_rad = first_person
                                    ? clamp(to_radians(args.pitch), -Camera::FP_MAX_PITCH, Camera::FP_MAX_PITCH)
                                    : to_radians(args.pitch);
        camera.orbit(-to_radians(args.yaw), pitch_rad);
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

    // Lowercase variant for the HUD, where the bar's fields are uniformly lowercase;
    // shading_mode_name keeps the capitalized form for the --bench report.
    constexpr const char *hud_shading_name(ShadingMode mode) noexcept
    {
        switch (mode)
        {
        case ShadingMode::Wireframe:
            return "wireframe";
        case ShadingMode::Flat:
            return "flat";
        case ShadingMode::Phong:
            return "phong";
        }
        return "phong";
    }

    void run_bench(const Mesh &mesh, const ParsedArgs &args, double load_ms)
    {
        using clock = std::chrono::steady_clock;

        const int n_threads = Renderer::resolve_thread_count(args.n_threads);

        Camera camera = auto_fit_camera(mesh, args, /*first_person=*/false);
        Light lights[2];
        vec3 ambient;
        make_default_lights(lights, ambient);

        Framebuffer fb(args.bench_width, args.bench_height, /*headless=*/true);
        Renderer renderer(n_threads);
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
    // --no-input deliberately does NOT relax this: it ignores the bindings but Q
    // still quits, so stdin must stay readable for the session to be exitable
    // without a signal.
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

    const int n_threads = Renderer::resolve_thread_count(args.n_threads);
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

    // Snapshotted before the loop so the R reset returns to the flag-specified launch state.
    Camera camera = auto_fit_camera(mesh, args, args.first_person);
    const Camera initial_camera = camera;

    // Extract model basename for the HUD (e.g. "models/suzanne.obj" → "suzanne.obj") and strip
    // control bytes, which a filename may legally contain and would otherwise reach the terminal
    // verbatim. Truncation is the composer's business (the budget depends on its drop level).
    std::string model_name = args.model_path;
    {
        const size_t slash = model_name.find_last_of("/\\");
        if (slash != std::string::npos)
        {
            model_name = model_name.substr(slash + 1);
        }
        model_name = sanitize_controls(model_name);
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

    Renderer renderer(n_threads);

    const bool has_textures = !mesh.textures.empty();
    // --no-input locks every binding but the quit key; see the drain loop below.
    const bool input_enabled = args.input;
    float fps_smooth = -1.0f;    // per-frame EMA of the framerate; -1 = uninitialised
    float fps_display = -1.0f;   // value shown in the HUD, latched from fps_smooth; -1 = none yet
    float fps_latch_time = 0.0f; // seconds since the HUD value was last latched
    int mouse_last_x = 0;        // last seen drag position (terminal cells)
    int mouse_last_y = 0;
    // Whether mouse_last_* holds a position from the drag in progress. A motion
    // report can arrive without its press (a malformed press is dropped by the
    // parser, and a drag can begin outside the window), and orbiting by the delta
    // from a stale position would snap the camera; the first such motion seeds
    // instead.
    //
    // Deliberately not also bounded by elapsed time. Button-event tracking reports
    // motion only on a change of character cell, so a slow or paused drag can go
    // seconds between reports and any timeout would re-seed mid-drag, which reads
    // as the camera refusing to move. A missed release then leaves the flag armed,
    // but the next drag opens with a press and that press re-seeds, so the stale
    // position is overwritten before it can be used. The one case that slips
    // through is a lost release followed by a dropped press, where the guard has
    // nothing to re-seed from and the first motion orbits from the old position;
    // that needs two malformed reports in a row and costs one jump, which is
    // cheaper than a timeout that would break every slow drag.
    bool mouse_dragging = false;
    // Flag-driven runtime state; value-initialised only pro forma, the real launch
    // values come from reset_to_launch_state() below.
    bool spinning{};
    bool culling{};
    bool texturing{};
    Background bg_mode{};
    LightingMode lighting_mode{};
    WireframeColor wf_color{};
    // Signed radians/sec for spin_world_y: a positive angle moves the model's
    // front face left on screen (verified visually), so left keeps the sign.
    // The sign is fixed for the session: while the view is upside down (pitched
    // past a pole) the raw world-Y spin sweeps the opposite way on screen.
    // Accepted: unlike orbit()'s yaw inversion there is no drag to keep faithful
    // to, and a mid-session direction flip would be the more surprising behavior.
    const float spin_speed = to_radians(args.spin_speed) * (args.spin_direction == SpinDirection::Left ? 1.0f : -1.0f);
    constexpr float FPS_LATCH = 0.1f; // seconds between HUD fps refreshes (~10 Hz)

    using clock = std::chrono::steady_clock;
    auto prev = clock::now();
    bool first_frame = true; // frame 1's dt is only the setup gap, not a real frame
    platform::Key held_cam_key = platform::Key::None;
    clock::time_point held_cam_key_tp = clock::now();

    // One definition of the flag-specified launch state, shared by the initial
    // assignment and the R key so the two sites cannot drift. Clearing held_cam_key
    // stops a still-latched camera key (WASD/arrows, +/-) from moving the just-reset
    // camera on every frame of its remaining 100 ms latch window.
    const auto reset_to_launch_state = [&]()
    {
        camera = initial_camera;
        renderer.mode = args.shading;
        spinning = args.spin;
        culling = args.cull;
        texturing = args.texture;
        bg_mode = args.bg;
        lighting_mode = args.lighting;
        wf_color = args.wireframe_color;
        held_cam_key = platform::Key::None;
    };
    reset_to_launch_state();

    bool running = true;
    while (running)
    {
        // ── Frame timing ──────────────────────────────────────────────────
        auto now = clock::now();
        const float raw_dt = std::chrono::duration<float>(now - prev).count();
        prev = now;
        // Capped so a stall cannot jump the camera in one step. The cap is the held-key
        // window on purpose, not coincidentally: a tapped key contributes whole frame
        // dts until the window elapses, so a cap below it would widen the gap between
        // a +/- tap and the wheel notch it is meant to match. It bounds that error
        // rather than removing it; see speed_key_factor() in camera.cpp.
        const float dt = std::min(raw_dt, Camera::HELD_KEY_WINDOW);

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
        // poll_event returns Type::None only when nothing is left to report, so this
        // retires a whole burst rather than one sequence per frame (see its contract
        // for the one platform where the non-blocking read is not a hard guarantee).
        //
        // Bounded so a source that produces events as fast as they are consumed (a
        // mouse flood, a stuck key) cannot hold the frame: without the cap the loop
        // never reaches the render, and the quit check sits outside it, so even
        // Ctrl+C would not get the viewer back. Far above a real burst, so ordinary
        // input is always retired in one pass; a leftover is picked up next frame.
        constexpr int MAX_EVENTS_PER_FRAME = 4096;
        for (int handled = 0; handled < MAX_EVENTS_PER_FRAME; handled++)
        {
            const platform::InputEvent ev = platform::poll_event();
            if (ev.type == platform::InputEvent::Type::None)
            {
                break;
            }

            // Q quits in every mode, --no-input included, so it is checked here rather
            // than inside the key chain below.
            if (ev.type == platform::InputEvent::Type::Key && ev.key == platform::Key::Q)
            {
                running = false;
                break;
            }

            // --no-input locks every other binding. Input is still read and parsed:
            // the bytes have to be consumed either way (unread ones would fill the
            // terminal's input queue), and the terminal stays in mouse-tracking mode,
            // so a drag neither orbits nor paints a text selection over the render.
            if (!input_enabled)
            {
                continue;
            }

            if (ev.type == platform::InputEvent::Type::Key)
            {
                const platform::Key k = ev.key;
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
                    reset_to_launch_state();
                }
                else if (k == platform::Key::E || k == platform::Key::V)
                {
                    // Vertical movement exists only in first-person. In orbit mode
                    // these are unbound, and an unbound key must not cancel a held
                    // camera key, so they are dropped here rather than latched.
                    if (args.first_person)
                    {
                        held_cam_key = k;
                        held_cam_key_tp = clock::now();
                    }
                }
                else
                {
                    // Every remaining key is a camera movement: the parser drops bytes
                    // with no binding rather than reporting them, so nothing unbound
                    // reaches here to cancel a movement in progress.
                    held_cam_key = k;
                    held_cam_key_tp = clock::now();
                }
            }
            else if (ev.type == platform::InputEvent::Type::ScrollUp)
            {
                // In first-person the wheel sets how fast you fly, the way every
                // editor's freelook does. Scaling `distance` there would move the eye,
                // but only straight along the view axis, which W and S already do.
                // Applied as an exact reciprocal downward so a notch up and a notch
                // back down restores the value: the HUD shows this number, and the
                // 1.08/0.92 pair below loses 0.64% per round trip. Orbit keeps that
                // pair, since nothing displays `distance`.
                if (args.first_person)
                {
                    camera.adjust_speed(Camera::FP_SPEED_WHEEL_STEP);
                }
                else
                {
                    camera.distance *= 0.92f;
                    camera.distance = std::max(camera.distance, camera.near_plane * 2.0f);
                }
            }
            else if (ev.type == platform::InputEvent::Type::ScrollDown)
            {
                if (args.first_person)
                {
                    camera.adjust_speed(1.0f / Camera::FP_SPEED_WHEEL_STEP);
                }
                else
                {
                    camera.distance *= 1.08f;
                    camera.distance = std::min(camera.distance, camera.max_eye_distance());
                }
            }
            else if (ev.type == platform::InputEvent::Type::MousePress)
            {
                // Record position so the first drag delta starts from here.
                mouse_last_x = ev.x;
                mouse_last_y = ev.y;
                mouse_dragging = true;
            }
            else if (ev.type == platform::InputEvent::Type::MouseRelease)
            {
                // Any button's release ends the drag. Without the button number
                // (which nothing reads, so it is not decoded) a second button
                // released mid-orbit also lands here; the cost is that the next
                // motion re-seeds, losing one frame of movement rather than jumping.
                mouse_dragging = false;
            }
            else if (ev.type == platform::InputEvent::Type::MouseMove)
            {
                // A single report cannot move the pointer further than the terminal is
                // wide or tall, so a delta that big is not a pointer movement: either
                // the origin is stale, or the report names a cell that does not exist.
                // The parser bounds coordinates against a fixed ceiling because it
                // cannot know the terminal size, and everything between that ceiling
                // and the real size lands here (column 9000 in an 80-column terminal
                // would otherwise orbit by a hundred turns).
                //
                // Checked on the delta rather than the position, which matters when
                // cols/rows are wrong: get_terminal_size falls back to 80x24 when every
                // ioctl fails, and rejecting positions outside that would leave a real
                // wider terminal unable to drag past column 80. Judging the delta costs
                // at worst one re-seeded report there, and no region stops working.
                // (The fallback already misscales the orbit below and the framebuffer
                // itself, so this is not a new dependency on that size being right.)
                //
                // Re-seeded, not clamped: clamping invents a movement, and the bogus
                // coordinate would still be the origin the NEXT delta is measured from,
                // which is the same snap one report later.
                const bool implausible = std::abs(ev.x - mouse_last_x) > cols || std::abs(ev.y - mouse_last_y) > rows;
                if (mouse_dragging && !implausible)
                {
                    const float dx_rad = static_cast<float>(ev.x - mouse_last_x) / static_cast<float>(cols) * 6.2832f;
                    const float dy_rad = static_cast<float>(ev.y - mouse_last_y) / static_cast<float>(rows) * 3.1416f;
                    camera.look(dx_rad, -dy_rad);
                }
                // Seeded either way, so a drag that began without its opening report
                // (or was interrupted by one of the above) continues from here rather
                // than from wherever the pointer last was.
                mouse_last_x = ev.x;
                mouse_last_y = ev.y;
                mouse_dragging = true;
            }
        }
        // The drain is over however it ended. poll_event releases its per-pass read
        // budget on its own only when it reports Type::None, and the loop above has two
        // other exits (the event cap, and the quit key), so say so unconditionally.
        platform::end_input_pass();

        if (!running)
        {
            break;
        }

        // ── Camera key movement (once per frame, frame-rate independent) ──
        if (held_cam_key != platform::Key::None)
        {
            const float since = std::chrono::duration<float>(clock::now() - held_cam_key_tp).count();
            if (since > Camera::HELD_KEY_WINDOW)
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
        // Composed every frame even though present() drops an unchanged line without emitting a
        // byte, so most of these composes are discarded. Deliberate: the compose measures 679 ns
        // for a name within budget and 713 ns for one long enough to be cut (200k iterations
        // each, -O3), so at most 0.005% of a 16.6 ms frame either way. Skipping it would mean
        // reintroducing a per-field snapshot of the whole displayed state in this loop purely to
        // decide whether to spend a microsecond. The skip exists for the terminal bytes, which
        // are the part that actually costs.
        if (args.hud)
        {
            HudInfo info;
            info.model_name = model_name;
            info.shading_name = hud_shading_name(renderer.mode);
            info.light_name = lighting_name(lighting_mode);
            info.bg_name = background_name(bg_mode);
            if (renderer.mode == ShadingMode::Wireframe)
            {
                info.wf_name = wireframe_name(wf_color);
                info.wf_color = wireframe_color_of(wf_color);
            }
            info.spinning = spinning;
            info.culling = culling;
            info.texturing = texturing;
            info.has_textures = has_textures;
            info.first_person = args.first_person;
            info.fp_speed = camera.fp_speed;
            info.fps = (fps_display < 0.0f) ? 0 : static_cast<int>(std::lround(fps_display));

            fb.set_hud(compose_hud(info, cols, color_mode));
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
