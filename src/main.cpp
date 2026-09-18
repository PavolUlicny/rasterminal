#include "src/args.h"
#include "src/loaders/mesh.h"
#include "src/math/light.h"
#include "src/math/linalg.h"
#include "src/platform/console.h"
#include "src/platform/control.h"
#include "src/platform/input.h"
#include "src/platform/input_reader.h"
#include "src/platform/terminal_io.h"
#include "src/platform/terminal_query.h"
#include "src/platform/timing.h"
#include "src/render/camera.h"
#include "src/render/renderer.h"
#include "src/shading.h"
#include "src/terminal/color.h"
#include "src/terminal/framebuffer.h"
#include "src/terminal/geometry.h"
#include "src/terminal/graphics.h"
#include "src/terminal/hud.h"
#include "src/terminal/text.h"
#include "src/viewer/frame_timing.h"
#include "src/viewer/input_controller.h"
#include "src/viewer/state.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ratio>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <stdexcept>
#include <vector>

namespace
{

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

    // Apply the escape-reply sanity bound to ioctl-derived cell sizes too.
    constexpr bool valid_cell_px(int v) noexcept
    {
        return v >= 1 && v <= platform::detail::MAX_CELL_REPORT_PX;
    }

    using terminal_geometry::FbSize;
    using terminal_geometry::TerminalGeometry;
    using viewer::FrameTiming;
    using viewer::InputController;
    using viewer::ViewerState;

    // Derive cell size as floor(px / cells) for startup and resize polling.
    // Accept only complete, valid reports; leave outputs unchanged on failure.
    bool derive_cell_from_pixels(int cols, int rows, int &cell_w, int &cell_h)
    {
        int px_w = 0;
        int px_h = 0;
        platform::get_terminal_pixel_size(px_w, px_h);
        if (px_w <= 0 || px_h <= 0 || cols <= 0 || rows <= 0)
        {
            return false;
        }
        const int w = px_w / cols;
        const int h = px_h / rows;
        if (!valid_cell_px(w) || !valid_cell_px(h))
        {
            return false;
        }
        cell_w = w;
        cell_h = h;
        return true;
    }

    terminal_geometry::Observation read_geometry(bool pixel_backend)
    {
        terminal_geometry::Observation observed;
        platform::get_terminal_size(observed.cols, observed.rows);
        if (pixel_backend)
        {
            observed.has_pixel_report =
                derive_cell_from_pixels(observed.cols, observed.rows, observed.pixel_cell_w, observed.pixel_cell_h);
        }
        return observed;
    }

    void request_geometry(const terminal_geometry::Requests &requests)
    {
        if (requests.cell_size)
        {
            platform::request_cell_size();
        }
        if (requests.sixel_geometry)
        {
            platform::request_sixel_geometry();
        }
    }

    bool poll_geometry(TerminalGeometry &geometry, Framebuffer &fb)
    {
        const terminal_geometry::Observation observed = read_geometry(geometry.pixel_backend());
        const FbSize presented = { fb.width(), fb.height(), fb.origin_col(), fb.origin_row() };
        const terminal_geometry::Update update = geometry.observe(observed, presented);
        if (update.cell_size_before_resize)
        {
            platform::request_cell_size();
        }
        if (!update.resize)
        {
            return false;
        }
        if (geometry.pixel_backend())
        {
            fb.resize(
                update.size.w, update.size.h, geometry.cols(), geometry.image_rows(), update.size.origin_col,
                update.size.origin_row
            );
        }
        else
        {
            fb.resize(update.size.w, update.size.h);
        }
        request_geometry(update.after_resize);
        return true;
    }

    // Interrupted queries must be retried. Detection errors are reported only
    // after the caller releases the terminal state recorded here.
    struct GraphicsSetup
    {
        GraphicsBackend backend = GraphicsBackend::Blocks;
        bool shm_ok = false;
        bool interrupted = false;
        int cell_w = 0;
        int cell_h = 0;
        // The terminal's max sixel image size (0 = unreported); see TermGraphics.
        int sixel_max_w = 0;
        int sixel_max_h = 0;
        // Set with exit_code 1; main prints it after restoring the terminal.
        const char *error = nullptr;
        int exit_code = -1;
    };

    struct TerminalSessionGuard
    {
        const char *program;
        platform::ConsoleStateGuard &console_state;
        bool alt_screen_owned = false;
        bool mouse_enabled = false;
        bool raw_enabled = false;
        bool reacquisition_pending = false;
        const char *error = nullptr;

        TerminalSessionGuard(const char *prog, platform::ConsoleStateGuard &state) noexcept
            : program(prog), console_state(state)
        {
        }

        ~TerminalSessionGuard() noexcept
        {
            // Framebuffer adopts a successful query's alternate screen and releases it
            // in its destructor. Until construction finishes, this guard owns it.
            if (alt_screen_owned)
            {
                platform::exit_alt_screen();
            }
            if (mouse_enabled)
            {
                platform::disable_mouse();
            }
            if (raw_enabled)
            {
                platform::disable_raw_mode(&console_state);
            }
            if (error != nullptr)
            {
                std::fprintf(stderr, "%s: %s\n", program, error);
            }
        }

        bool resume_output(Framebuffer *framebuffer)
        {
            if (!reacquisition_pending)
            {
                return true;
            }
            bool canceled = false;
            if ((framebuffer != nullptr && !framebuffer->resume_terminal(&canceled)) ||
                (mouse_enabled && !platform::enable_mouse(true, &canceled)))
            {
                // SIGCONT can clear a stop request after setup was abandoned.
                if (canceled)
                {
                    return true;
                }
                error = "failed to enable terminal output";
                return false;
            }
            reacquisition_pending = false;
            return true;
        }

        bool handle_suspend(Framebuffer *framebuffer = nullptr)
        {
            // The Windows stub never requests suspension; POSIX needs this guard.
            // cppcheck-suppress knownConditionTrueFalse
            if (!platform::suspend_requested())
            {
                return resume_output(framebuffer);
            }
            const bool restore_mouse = mouse_enabled;
            if (framebuffer != nullptr)
            {
                framebuffer->suspend_terminal(nullptr, &mouse_enabled);
            }
            else if (mouse_enabled)
            {
                mouse_enabled = !platform::disable_mouse();
            }
            if (alt_screen_owned)
            {
                alt_screen_owned = !platform::exit_alt_screen();
            }
            if (!platform::disable_raw_mode(&console_state))
            {
                error = "failed to restore terminal state before suspension";
                return false;
            }
            raw_enabled = false;
            platform::reset_input_state();
            // The Windows stub always succeeds; POSIX stop and resume can fail.
            // cppcheck-suppress knownConditionTrueFalse
            if (!platform::suspend_process())
            {
                error = "failed to suspend or resume terminal session";
                return false;
            }
            if (platform::interrupt_requested())
            {
                return true;
            }

            // Exit must restore any termios changes the shell made while stopped.
            raw_enabled = true;
            if (!platform::resume_raw_mode(console_state))
            {
                error = "failed to restore raw input mode after continuation";
                return false;
            }
            // Retain setup intent and cleanup ownership across interrupted writes.
            mouse_enabled = restore_mouse;
            reacquisition_pending = true;
            return resume_output(framebuffer);
        }

        TerminalSessionGuard(const TerminalSessionGuard &) = delete;
        TerminalSessionGuard &operator=(const TerminalSessionGuard &) = delete;
        TerminalSessionGuard(TerminalSessionGuard &&) = delete;
        TerminalSessionGuard &operator=(TerminalSessionGuard &&) = delete;
    };

    // Query before mouse tracking and input parsing. tmux needs protocol
    // passthrough this build does not implement, so it uses blocks. Record alternate-screen
    // ownership before the query so the caller can clean up if the query throws.
    GraphicsSetup negotiate_graphics(GraphicsChoice choice, bool &alt_screen_owned)
    {
        GraphicsSetup gfx;
        if (choice == GraphicsChoice::Blocks)
        {
            return gfx;
        }
        // Single-threaded startup, same as detect_term_color's env reads.
        const char *term_env = std::getenv("TERM");               // NOLINT(concurrency-mt-unsafe)
        const bool under_tmux = std::getenv("TMUX") != nullptr || // NOLINT(concurrency-mt-unsafe)
                                (term_env != nullptr &&
                                 (std::strncmp(term_env, "screen", 6) == 0 || std::strncmp(term_env, "tmux", 4) == 0));
        if (!under_tmux)
        {
            alt_screen_owned = true;
            const TermGraphics tg = platform::query_term_graphics();
            gfx.interrupted = tg.interrupted;
            if (tg.failed)
            {
                gfx.error = "failed to query terminal graphics";
                gfx.exit_code = 1;
                return gfx;
            }
            // Outside the backend selection: a sixel-only terminal's cell-size
            // reply must not be thrown away with the kitty verdict.
            gfx.cell_w = tg.cell_w;
            gfx.cell_h = tg.cell_h;
            gfx.sixel_max_w = tg.sixel_max_w;
            gfx.sixel_max_h = tg.sixel_max_h;
            // Auto prefers kitty to sixel. A forced choice masks the other pixel
            // backend, so --graphics sixel exercises sixel on terminals with both.
            if (tg.kitty && choice != GraphicsChoice::Sixel)
            {
                gfx.backend = GraphicsBackend::Kitty;
                gfx.shm_ok = tg.kitty_shm;
            }
            else if (tg.sixel && choice != GraphicsChoice::Kitty)
            {
                gfx.backend = GraphicsBackend::Sixel;
            }
        }
        // Suspension restarts detection after continuation. Termination unwinds
        // the terminal guards before finish_termination re-raises the signal.
        if (gfx.interrupted || platform::control_requested())
        {
            gfx.interrupted = true;
            return gfx;
        }
        // Forced pixel modes still require detection; unsupported escapes would
        // otherwise produce a blank screen with no diagnostic.
        if (gfx.backend == GraphicsBackend::Blocks &&
            (choice == GraphicsChoice::Kitty || choice == GraphicsChoice::Sixel))
        {
            const bool kitty_choice = choice == GraphicsChoice::Kitty;
            const char *reason = kitty_choice ? "terminal does not answer the kitty graphics query"
                                              : "terminal does not report sixel support";
            if (under_tmux)
            {
                // The gate covers both multiplexers (TMUX env, TERM screen*/tmux*),
                // so the message must not blame tmux for a GNU screen session.
                reason = kitty_choice ? "kitty graphics is not supported under tmux or GNU screen"
                                      : "sixel graphics is not supported under tmux or GNU screen";
            }
            gfx.error = reason;
            gfx.exit_code = 1;
            return gfx;
        }
        return gfx;
    }

    // The bench passes false because its spinning camera is a turntable operation;
    // first-person would pan the model out of frame and clamp the requested pitch.
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
            radius = 1.0f; // degenerate (all coincident vertices): use sane defaults
        }

        Camera camera;
        camera.first_person = first_person;
        camera.fp_centre = centre;
        // Scale movement by bounding radius so multiplier 1 crosses the model in about
        // two seconds. Long thin models therefore use their long axis.
        camera.fp_base_speed = radius;
        camera.fp_speed = args.first_person_speed;
        camera.target = centre;
        // --zoom's parse-time bound [0.2, 100] lands the distance inside the
        // interactive clamp [near*2, far*0.5] by construction, so no clamp here.
        camera.distance = radius * 2.0f / args.zoom;
        // Scale clipping planes by model size. First-person may fly through the near
        // plane; shrinking it would reduce depth precision for the whole scene.
        camera.near_plane = radius * 0.01f;
        camera.far_plane = radius * 20.0f;
        // Build the launch pose through orbit(), matching drag composition and the
        // first-person target invariant. Negate yaw for orbit's screen-drag convention.
        // First-person uses the same pitch clamp as look().
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

    void run_bench(const Mesh &mesh, const ParsedArgs &args, int n_threads, double load_ms)
    {
        using clock = std::chrono::steady_clock;

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

        // Compute stats via nth_element (linear): no full sort needed.
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

        // Welford's online algorithm: single pass, numerically stable.
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
        std::fprintf(stderr, "  total   %.0f ms  %d frames including warmup\n", total_ms, n_warmup + n_measure);
    }

} // namespace

const auto run_main = [](int argc, char *argv[]) -> int
{
    const ParseResult parsed = parse_args(argc, argv);
    if (!parsed.ok)
    {
        return parsed.exit_code;
    }
    const ParsedArgs &args = parsed.args;

    const bool bench_mode = args.bench > 0;

    // Interactive mode requires terminal input and output. --no-input still accepts Q.
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
        // Environment detection does not mutate the console; VT setup waits until after loading.
        const platform::TermColor tc = platform::detect_term_color();
        if (tc == platform::TermColor::Dumb)
        {
            std::fprintf(stderr, "%s: dumb terminal (TERM=dumb) cannot render\n", program_name(argv[0]));
            return 1;
        }
        // A forced color depth cannot override missing escape support.
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

    // Loading and rendering share one resolved thread count.
    const int n_threads = Renderer::resolve_thread_count(args.n_threads);
    Mesh mesh;
    const auto load_t0 = std::chrono::steady_clock::now();
    if (!mesh.load_model(args.model_path, args.ao, n_threads, args.smooth_angle))
    {
        std::fprintf(
            stderr,
            "%s: failed to load '%s'\n"
            "Native formats: .obj, .ply, .stl, .gltf, .glb; see --help for Assimp-backed formats\n",
            program_name(argv[0]), args.model_path.c_str()
        );
        return 1;
    }

    if (bench_mode)
    {
        const double load_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_t0).count();
        run_bench(mesh, args, n_threads, load_ms);
        return 0;
    }

    ViewerState state(args, auto_fit_camera(mesh, args, args.first_person));

    // Use a control-safe basename in the HUD; the composer handles truncation.
    std::string model_name = args.model_path;
    {
        const size_t slash = model_name.find_last_of("/\\");
        if (slash != std::string::npos)
        {
            model_name = model_name.substr(slash + 1);
        }
        model_name = sanitize_controls(model_name);
    }

    if (!platform::install_interrupt_handler() || !platform::install_job_control_handler())
    {
        std::fprintf(stderr, "%s: failed to install signal handlers\n", program_name(argv[0]));
        return 1;
    }

    // Keep interrupt-handler resources alive through terminal cleanup.
    const platform::InterruptHandlerGuard interrupt_handler;

    // Capture terminal state only when this run is about to change it. Keep this
    // outside the session guard so escape-based cleanup runs before final restoration.
    platform::ConsoleStateGuard console_state;
    if (!console_state.valid())
    {
        std::fprintf(stderr, "%s: failed to capture terminal state\n", program_name(argv[0]));
        return 1;
    }

    {
        TerminalSessionGuard terminal(program_name(argv[0]), console_state);

        // Defer persistent Windows VT mutation until loading succeeds. cppcheck sees
        // the platform branch as constant; keep the suppression directly above the if.
        // cppcheck-suppress knownConditionTrueFalse
        if (!platform::init_console_output())
        {
            terminal.error = "console does not support ANSI escape sequences";
            return 1;
        }

        // Arm cleanup before the write. A failed tcsetattr may still need rollback.
        terminal.raw_enabled = true;
        if (!platform::enable_raw_mode(&console_state))
        {
            terminal.error = "failed to enable raw input mode";
            return 1;
        }

        GraphicsSetup gfx;
        for (;;)
        {
            if (!terminal.handle_suspend())
            {
                return 1;
            }
            if (platform::interrupt_requested())
            {
                return 0;
            }
            gfx = negotiate_graphics(args.graphics, terminal.alt_screen_owned);
            if (!gfx.interrupted && !platform::suspend_requested())
            {
                break;
            }
        }
        if (gfx.exit_code >= 0)
        {
            terminal.error = gfx.error;
            return gfx.exit_code;
        }
        const GraphicsBackend backend = gfx.backend;
        TerminalGeometry geometry(
            backend, args.hud ? 1 : 0, terminal_geometry::ExactCellSize{ gfx.cell_w, gfx.cell_h },
            terminal_geometry::SixelMaxSize{ gfx.sixel_max_w, gfx.sixel_max_h },
            read_geometry(backend != GraphicsBackend::Blocks)
        );

        // Arm cleanup before deferred framebuffer and mouse setup.
        terminal.mouse_enabled = true;
        terminal.reacquisition_pending = true;

        GraphicsConfig gfx_cfg;
        const FbSize initial_size = geometry.framebuffer_size();
        if (geometry.pixel_backend())
        {
            gfx_cfg.backend = backend;
            // Use kitty shm only after the end-to-end probe succeeds.
            gfx_cfg.shm = gfx.shm_ok;
            gfx_cfg.cols = geometry.cols();
            gfx_cfg.rows = geometry.image_rows();
            gfx_cfg.origin_col = initial_size.origin_col;
            gfx_cfg.origin_row = initial_size.origin_row;
        }

        // Renderer must outlive Framebuffer because its borrowed runner captures it.
        Renderer renderer = [&]()
        {
            const platform::WorkerSignalMask worker_signals;
            if (!worker_signals.valid())
            {
                throw std::runtime_error("failed to block control signals for render workers");
            }
            return Renderer(n_threads);
        }();

        Framebuffer fb(
            initial_size.w, initial_size.h, /*headless=*/false, color_mode, gfx_cfg,
            /*adopt_alt_screen=*/terminal.alt_screen_owned,
            /*write_frame=*/nullptr
        );
        terminal.alt_screen_owned = false;
        while (terminal.reacquisition_pending)
        {
            if (platform::interrupt_requested())
            {
                return 0;
            }
            if (!terminal.handle_suspend(&fb))
            {
                return 1;
            }
        }

        // Key light: warm white from upper-right-front.
        // Fill light: dim cool blue from lower-left-back, providing contrast.
        Light lights[2];
        vec3 ambient;
        make_default_lights(lights, ambient);

        // Borrow the idle render pool for presentation; tests and bench remain serial.
        fb.set_parallel_runner({ [&renderer](const std::function<void(int, int)> &fn) { renderer.run_on_workers(fn); },
                                 renderer.worker_count() });

        const bool has_textures = !mesh.textures.empty();
        InputController input(args.input, has_textures);
        // Conservative image invalidation; unchanged frames neither render nor transmit.
        bool scene_dirty = true;
        // Positive world-Y rotation moves an upright model left on screen. Keep the
        // world direction fixed even when an upside-down view reads oppositely.
        const float spin_speed =
            to_radians(args.spin_speed) * (args.spin_direction == SpinDirection::Left ? 1.0f : -1.0f);
        FrameTiming timing(FrameTiming::Clock::now());

        bool running = true;
        while (running)
        {
            if (platform::interrupt_requested())
            {
                break;
            }
            if (platform::suspend_requested() || terminal.reacquisition_pending)
            {
                if (!terminal.handle_suspend(&fb))
                {
                    return 1;
                }
                if (platform::control_requested() || terminal.reacquisition_pending)
                {
                    continue;
                }
                input.clear_motion();
                scene_dirty = true;
                timing.after_resume(FrameTiming::Clock::now());
                request_geometry(geometry.after_resume());
            }
            const float dt = timing.begin_frame(FrameTiming::Clock::now());

            if (platform::interrupt_requested())
            {
                break;
            }

            // Drain a whole input burst, but cap it so a flood cannot starve rendering
            // and the outer interrupt check. Leftovers resume next frame.
            constexpr int MAX_EVENTS_PER_FRAME = 4096;
            for (int handled = 0; handled < MAX_EVENTS_PER_FRAME; handled++)
            {
                if (platform::control_requested())
                {
                    break;
                }
                const platform::InputEvent ev = platform::poll_event();
                if (ev.type == platform::InputEvent::Type::None)
                {
                    break;
                }
                // Q quits even with --no-input, so handle it before the input controller.
                if (ev.type == platform::InputEvent::Type::Key && ev.key == platform::Key::Q)
                {
                    running = false;
                    break;
                }

                // Geometry replies must update the resize tracker even with --no-input.
                if (ev.type == platform::InputEvent::Type::CellSize)
                {
                    geometry.accept_cell_size(ev.x, ev.y);
                    continue;
                }
                if (ev.type == platform::InputEvent::Type::SixelGeometry)
                {
                    geometry.accept_sixel_geometry(ev.x, ev.y);
                    continue;
                }

                scene_dirty |= input.handle(ev, state, geometry.cols(), geometry.rows(), FrameTiming::Clock::now());
            }
            // The drain is over however it ended. poll_event releases its per-pass read
            // budget on its own only when it reports Type::None, and the loop above has two
            // other exits (the event cap, and the quit key), so say so unconditionally.
            platform::end_input_pass();

            if (!running)
            {
                break;
            }
            if (platform::control_requested())
            {
                continue;
            }

            scene_dirty |= input.advance(state, dt, FrameTiming::Clock::now());

            if (state.settings.spinning)
            {
                state.camera.spin_world_y(spin_speed * dt);
                scene_dirty = true;
            }

            // Poll every frame: font zoom can change pixel dimensions without a
            // grid change, and sixel replies can arrive between grid changes.
            if (poll_geometry(geometry, fb))
            {
                scene_dirty = true;
            }

            // Compose every frame; present() cheaply drops an unchanged HUD before output.
            if (args.hud)
            {
                HudInfo info;
                info.model_name = model_name;
                info.shading_name = hud_shading_name(state.settings.shading);
                info.light_name = lighting_name(state.settings.lighting);
                info.bg_name = background_name(state.settings.background);
                if (state.settings.shading == ShadingMode::Wireframe)
                {
                    info.wf_name = wireframe_name(state.settings.wireframe_color);
                    info.wf_color = wireframe_color_of(state.settings.wireframe_color);
                }
                info.spinning = state.settings.spinning;
                info.culling = state.settings.culling;
                info.texturing = state.settings.texturing;
                info.has_textures = has_textures;
                info.first_person = state.camera.first_person;
                info.fp_speed = state.camera.fp_speed;
                info.fps = timing.hud_fps();

                fb.set_hud(compose_hud(info, geometry.cols(), color_mode));
            }

            // Render. A clean frame skips it (see scene_dirty), and on the pixel
            // backends the retransmission with it; present() still runs for the HUD row.
            const bool rendered = scene_dirty;
            if (rendered)
            {
                fb.clear(background_color(state.settings.background));
                const int n_lights = light_count(state.settings.lighting);
                const vec3 cur_ambient = lighting_ambient(state.settings.lighting, ambient);
                renderer.mode = state.settings.shading;
                renderer.wireframe_color = wireframe_color_of(state.settings.wireframe_color);
                renderer.cull_backfaces = state.settings.culling;
                renderer.show_texture = state.settings.texturing;
                renderer.render(mesh, state.camera, lights, n_lights, cur_ambient, fb);
            }
            fb.present();
            scene_dirty = false;
            const auto remaining = timing.end_frame(rendered, args.fps, FrameTiming::Clock::now());
            if (remaining > std::chrono::duration<float>::zero())
            {
                platform::wait_frame(remaining);
            }
        }
        return 0;
    }
};

int main(int argc, char *argv[])
{
    // run_main owns the cleanup guards so exceptions restore terminal state before reporting.
    int status = 0;
    try
    {
        status = run_main(argc, argv);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "%s: %s\n", program_name(argv[0]), e.what());
        status = 1;
    }
    catch (...)
    {
        std::fprintf(stderr, "%s: unexpected exception\n", program_name(argv[0]));
        status = 1;
    }
    return platform::finish_termination(status);
}
