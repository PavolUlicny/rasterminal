#include "src/args.h"
#include "src/bench.h"
#include "src/loaders/mesh.h"
#include "src/math/linalg.h"
#include "src/platform/console.h"
#include "src/platform/control.h"
#include "src/platform/input_reader.h"
#include "src/platform/terminal_io.h"
#include "src/platform/terminal_query.h"
#include "src/render/renderer.h"
#include "src/shading.h"
#include "src/terminal/color.h"
#include "src/terminal/framebuffer.h"
#include "src/terminal/geometry.h"
#include "src/terminal/graphics.h"
#include "src/terminal/hud.h"
#include "src/terminal/input.h"
#include "src/terminal/sixel.h"
#include "src/viewer/frame_timing.h"
#include "src/viewer/frame_view.h"
#include "src/viewer/graphics_setup.h"
#include "src/viewer/hud_info.h"
#include "src/viewer/input_controller.h"
#include "src/viewer/scene.h"
#include "src/viewer/scene_colors.h"
#include "src/viewer/state.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <optional>
#include <ratio>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{

    // Apply the escape-reply sanity bound to ioctl-derived cell sizes too.
    constexpr bool valid_cell_px(int v) noexcept
    {
        return v >= 1 && v <= terminal_input::detail::MAX_CELL_REPORT_PX;
    }

    using terminal_geometry::FbSize;
    using terminal_geometry::TerminalGeometry;
    using viewer::background_color;
    using viewer::FrameTiming;
    using viewer::FrameView;
    using viewer::GraphicsSetup;
    using viewer::InputController;
    using viewer::same_image;
    using viewer::ViewerState;
    using viewer::wireframe_color_of;

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

    // Query before mouse tracking and input parsing. Record alternate-screen
    // ownership before the query so the caller can clean up if the query throws.
    GraphicsSetup negotiate_graphics(GraphicsChoice choice, bool &alt_screen_owned)
    {
        if (choice == GraphicsChoice::Blocks)
        {
            return {};
        }
        // Single-threaded startup, same as detect_term_color's env reads.
        const bool multiplexed = viewer::under_multiplexer(
            std::getenv("TMUX"), // NOLINT(concurrency-mt-unsafe)
            std::getenv("TERM")  // NOLINT(concurrency-mt-unsafe)
        );
        std::optional<TermGraphics> reply;
        if (!multiplexed)
        {
            alt_screen_owned = true;
            reply = platform::query_term_graphics();
        }
        return viewer::resolve_graphics(choice, reply ? &*reply : nullptr, platform::control_requested());
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

    ViewerState state(args, viewer::auto_fit_camera(mesh, args, args.first_person));
    const std::string model_name = viewer::hud_model_name(args.model_path);

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
            gfx_cfg.sixel_colors = sixel::cap_to_registers(args.sixel_colors, gfx.sixel_registers);
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

        const viewer::SceneLights scene = viewer::default_lights();

        // Borrow the idle render pool for presentation; tests and bench remain serial.
        fb.set_parallel_runner({ [&renderer](const std::function<void(int, int)> &fn) { renderer.run_on_workers(fn); },
                                 renderer.worker_count() });

        const bool has_textures = !mesh.textures.empty();
        InputController input(args.input, has_textures);
        // The view behind the image on screen. Clearing it forces a redraw when the
        // framebuffer or terminal changes under an unchanged view.
        std::optional<FrameView> rendered_view;
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
                rendered_view.reset();
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
                const terminal_input::InputEvent ev = platform::poll_event();
                if (ev.type == terminal_input::InputEvent::Type::None)
                {
                    break;
                }
                // Q remains active with --no-input.
                if (ev.type == terminal_input::InputEvent::Type::Key && ev.key == terminal_input::Key::Q)
                {
                    running = false;
                    break;
                }

                // Terminal geometry remains active with --no-input.
                if (ev.type == terminal_input::InputEvent::Type::CellSize)
                {
                    geometry.accept_cell_size(ev.x, ev.y);
                    continue;
                }
                if (ev.type == terminal_input::InputEvent::Type::SixelGeometry)
                {
                    geometry.accept_sixel_geometry(ev.x, ev.y);
                    continue;
                }

                input.handle(ev, state, geometry.cols(), geometry.rows(), FrameTiming::Clock::now());
            }
            // Release the read budget even when the event cap or quit key ends the pass.
            platform::end_input_pass();

            if (!running)
            {
                break;
            }
            if (platform::control_requested())
            {
                continue;
            }

            input.advance(state, dt, FrameTiming::Clock::now());

            if (state.settings.spinning)
            {
                state.camera.spin_world_y(spin_speed * dt);
            }

            // Font zoom and sixel replies can change the image without changing the grid.
            if (poll_geometry(geometry, fb))
            {
                rendered_view.reset();
            }

            // Compose every frame; present() cheaply drops an unchanged HUD before output.
            if (args.hud)
            {
                fb.set_hud(compose_hud(
                    viewer::make_hud_info(state, model_name, has_textures, timing.hud_fps()), geometry.cols(),
                    color_mode
                ));
            }

            // An unchanged view skips the render, and on the pixel backends the
            // retransmission with it; present() still runs for the HUD row.
            const FrameView view(state);
            const bool rendered = !rendered_view || !same_image(*rendered_view, view);
            if (rendered)
            {
                fb.clear(background_color(view.background));
                const int n_lights = viewer::light_count(view.lighting);
                const vec3 cur_ambient = viewer::lighting_ambient(view.lighting, scene.ambient);
                renderer.mode = view.shading;
                renderer.wireframe_color = wireframe_color_of(view.wireframe_color);
                fb.set_required_color(
                    view.shading == ShadingMode::Wireframe ? std::optional<Color>(renderer.wireframe_color)
                                                           : std::nullopt
                );
                renderer.cull_backfaces = view.culling;
                renderer.show_texture = view.texturing;
                renderer.render(mesh, view.camera, scene.lights, n_lights, cur_ambient, fb);
                rendered_view = view;
            }
            fb.present();
            std::this_thread::sleep_for(timing.end_frame(rendered, args.fps, FrameTiming::Clock::now()));
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
