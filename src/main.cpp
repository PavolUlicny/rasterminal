#include "camera.h"
#include "framebuffer.h"
#include "light.h"
#include "mesh.h"
#include "platform.h"
#include "renderer.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

static volatile sig_atomic_t g_interrupted = 0;
static void signal_handler(int) { g_interrupted = 1; }

int main(int argc, char *argv[])
{
    const char *obj_path = nullptr;
    int n_threads = -1; // -1 = auto (min(hw_concurrency, 4))
    ShadingMode init_shading = ShadingMode::Gouraud;
    int init_bg = 0;       // 0=black, 1=gray, 2=white
    int init_lighting = 0; // 0=dual, 1=single, 2=flat
    bool init_spin = false;
    bool init_shadow = true;
    bool init_ao = true;

    // Returns the next argv value for a flag that requires one, or nullptr on error.
    auto require_val = [&](int &i, const char *flag) -> const char *
    {
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "Error: %s requires a value\n", flag);
            return nullptr;
        }
        return argv[++i];
    };

    auto parse_threads = [](const char *flag, const char *val, int &out) -> bool
    {
        char *end = nullptr;
        errno = 0;
        long v = std::strtol(val, &end, 10);
        if (end == val || *end != '\0' || v < 0 || v > INT_MAX || errno == ERANGE)
        {
            std::fprintf(stderr, "Error: %s requires a non-negative integer, got '%s'\n", flag, val);
            return false;
        }
        out = (int)v;
        return true;
    };

    auto parse_shading = [](const char *flag, const char *val, ShadingMode &out) -> bool
    {
        std::string v = val;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c)
                       { return (char)std::tolower(c); });
        if (v == "wireframe" || v == "1")
            out = ShadingMode::Wireframe;
        else if (v == "flat" || v == "2")
            out = ShadingMode::Flat;
        else if (v == "gouraud" || v == "3")
            out = ShadingMode::Gouraud;
        else if (v == "phong" || v == "4")
            out = ShadingMode::Phong;
        else
        {
            std::fprintf(stderr, "Error: %s: invalid value '%s' (expected wireframe|flat|gouraud|phong or 1-4)\n", flag, val);
            return false;
        }
        return true;
    };

    auto parse_bg = [](const char *flag, const char *val, int &out) -> bool
    {
        std::string v = val;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c)
                       { return (char)std::tolower(c); });
        if (v == "black" || v == "1")
            out = 0;
        else if (v == "gray" || v == "grey" || v == "2")
            out = 1;
        else if (v == "white" || v == "3")
            out = 2;
        else
        {
            std::fprintf(stderr, "Error: %s: invalid value '%s' (expected black|gray|white or 1-3)\n", flag, val);
            return false;
        }
        return true;
    };

    auto parse_lighting = [](const char *flag, const char *val, int &out) -> bool
    {
        std::string v = val;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c)
                       { return (char)std::tolower(c); });
        if (v == "dual" || v == "1")
            out = 0;
        else if (v == "single" || v == "2")
            out = 1;
        else if (v == "flat" || v == "3")
            out = 2;
        else
        {
            std::fprintf(stderr, "Error: %s: invalid value '%s' (expected dual|single|flat or 1-3)\n", flag, val);
            return false;
        }
        return true;
    };

    for (int i = 1; i < argc; i++)
    {
        // Split --flag=value into flag name + value pointer.
        // For --flag value or -f value forms, eq_val stays nullptr.
        const char *eq_val = nullptr;
        std::string arg = argv[i];
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-')
        {
            size_t eq = arg.find('=');
            if (eq != std::string::npos)
            {
                eq_val = argv[i] + eq + 1;
                arg = arg.substr(0, eq);
            }
        }
        const char *flag = arg.c_str();

        // Helper: get value either from --flag=val or the next argv token.
        auto get_val = [&](int &i) -> const char *
        {
            if (eq_val != nullptr)
                return eq_val;
            return require_val(i, flag);
        };

        if (arg == "-j" || arg == "--threads")
        {
            const char *val = get_val(i);
            if (!val || !parse_threads(flag, val, n_threads))
                return 1;
        }
        else if (std::strncmp(argv[i], "-j", 2) == 0 && argv[i][2] != '\0' && argv[i][2] != '=' && eq_val == nullptr)
        {
            if (!parse_threads("-j", argv[i] + 2, n_threads))
                return 1;
        }
        else if (arg == "-s" || arg == "--shading")
        {
            const char *val = get_val(i);
            if (!val || !parse_shading(flag, val, init_shading))
                return 1;
        }
        else if (std::strncmp(argv[i], "-s", 2) == 0 && argv[i][2] != '\0' && argv[i][2] != '=' && eq_val == nullptr)
        {
            if (!parse_shading("-s", argv[i] + 2, init_shading))
                return 1;
        }
        else if (arg == "-b" || arg == "--bg")
        {
            const char *val = get_val(i);
            if (!val || !parse_bg(flag, val, init_bg))
                return 1;
        }
        else if (std::strncmp(argv[i], "-b", 2) == 0 && argv[i][2] != '\0' && argv[i][2] != '=' && eq_val == nullptr)
        {
            if (!parse_bg("-b", argv[i] + 2, init_bg))
                return 1;
        }
        else if (arg == "-l" || arg == "--lighting")
        {
            const char *val = get_val(i);
            if (!val || !parse_lighting(flag, val, init_lighting))
                return 1;
        }
        else if (std::strncmp(argv[i], "-l", 2) == 0 && argv[i][2] != '\0' && argv[i][2] != '=' && eq_val == nullptr)
        {
            if (!parse_lighting("-l", argv[i] + 2, init_lighting))
                return 1;
        }
        else if (arg == "-h" || arg == "--help")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "Error: %s does not take a value\n", flag);
                return 1;
            }
            std::printf(
                "Usage: rasterminal [options] [model]\n"
                "\n"
                "Render a 3D model in the terminal using unicode half-block characters\n"
                "and 24-bit ANSI color. Defaults to models/obj/teapot.obj.\n"
                "\n"
                "Supported formats:\n"
                "  .obj   Wavefront OBJ with optional .mtl (diffuse/specular/normal maps)\n"
                "  .ply   ASCII or binary (little/big-endian)\n"
                "  .stl   ASCII or binary\n"
                "\n"
                "Options:\n"
                "  -s, --shading <mode>    Initial shading mode (default: gouraud)\n"
                "                           wireframe|flat|gouraud|phong  or  1-4\n"
                "  -b, --bg <color>        Initial background color (default: black)\n"
                "                           black|gray|white  or  1-3\n"
                "  -l, --lighting <mode>   Initial lighting mode (default: dual)\n"
                "                           dual|single|flat  or  1-3\n"
                "  -S, --spin              Start with auto-rotation enabled\n"
                "  -j, --threads <n>       Worker thread count (0=all, default=min(hw,4))\n"
                "      --no-shadow         Disable shadow map\n"
                "      --no-ao             Disable ambient occlusion\n"
                "  -h, --help              Show this message\n"
                "\n"
                "Controls:\n"
                "  1-4          Shading mode           B       Cycle background\n"
                "  Space        Toggle spin            L       Cycle lighting\n"
                "  WASD/arrows  Orbit camera           R       Reset view\n"
                "  +/-          Zoom                   Q       Quit\n"
                "  Mouse drag   Orbit                  Scroll  Zoom\n");
            return 0;
        }
        else if (arg == "-S" || arg == "--spin")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "Error: %s does not take a value\n", flag);
                return 1;
            }
            init_spin = true;
        }
        else if (arg == "--no-ao")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "Error: %s does not take a value\n", flag);
                return 1;
            }
            init_ao = false;
        }
        else if (arg == "--no-shadow")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "Error: %s does not take a value\n", flag);
                return 1;
            }
            init_shadow = false;
        }
        else if (argv[i][0] == '-')
        {
            std::fprintf(stderr, "Error: unknown flag '%s'\n", argv[i]);
            return 1;
        }
        else if (obj_path != nullptr)
        {
            std::fprintf(stderr, "Error: unexpected argument '%s'\n", argv[i]);
            return 1;
        }
        else
            obj_path = argv[i];
    }
    if (obj_path == nullptr)
        obj_path = "models/obj/teapot.obj";

    Mesh mesh;
    if (!mesh.load_model(obj_path, init_ao))
    {
        std::fprintf(stderr, "Error: failed to load '%s'\n"
                             "       Supported formats: .obj, .ply, .stl\n",
                     obj_path);
        return 1;
    }

    // Auto-fit camera: target = bounding-box centre, distance = 2× radius.
    Camera camera;
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
        camera.target = centre;
        camera.distance = radius * 2.0f;
        // Scale near/far to the model so arbitrarily-sized models aren't clipped.
        camera.near_plane = radius * 0.01f;
        camera.far_plane = radius * 20.0f;
    }
    const Camera initial_camera = camera;

    // Extract model basename for the HUD (e.g. "models/suzanne.obj" → "suzanne.obj").
    std::string model_name = obj_path;
    {
        size_t slash = model_name.find_last_of("/\\");
        if (slash != std::string::npos)
            model_name = model_name.substr(slash + 1);
    }

    std::signal(SIGINT, signal_handler);  // Ctrl+C
    std::signal(SIGTERM, signal_handler); // kill

    platform::enable_raw_mode();
    platform::clear_screen();
    platform::enable_mouse();

    int cols, rows;
    platform::get_terminal_size(cols, rows);

    // Reserve the last terminal row for the HUD status line.
    // Each pixel cell covers 2 vertical pixels via ▀ half-block.
    Framebuffer fb(cols, (rows - 1) * 2);

    // Key light: warm white from upper-right-front.
    // Fill light: dim cool blue from lower-left-back, providing contrast.
    Light lights[2];
    lights[0].direction = {0.408f, 0.816f, 0.408f};
    lights[0].color = {1.0f, 0.9f, 0.75f};
    lights[1].direction = {-0.667f, -0.333f, -0.667f};
    lights[1].color = {0.15f, 0.25f, 0.5f};
    const vec3 ambient = {0.2f, 0.2f, 0.25f};

    // Build shadow map once — the key light and mesh geometry are static,
    // so the map never changes regardless of camera movement or spin.
    std::optional<ShadowMap> shadow_map;
    if (init_shadow)
        shadow_map = build_shadow_map(mesh, lights[0]);

    Renderer renderer(n_threads);
    renderer.mode = init_shading;

    float fps_smooth = -1.0f; // exponential moving average; -1 = uninitialised
    bool spinning = init_spin;
    int mouse_last_x = 0, mouse_last_y = 0; // last seen drag position (terminal cells)
    int bg_mode = init_bg;                  // 0=black, 1=gray, 2=white
    int lighting_mode = init_lighting;      // 0=dual, 1=single, 2=flat ambient
    const float spin_speed = 0.8f;          // radians/sec

    using clock = std::chrono::steady_clock;
    auto prev = clock::now();

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
                else if (k == platform::KEY_R)
                {
                    camera = initial_camera;
                    renderer.mode = ShadingMode::Gouraud;
                    lighting_mode = 0;
                    bg_mode = 0;
                    spinning = false;
                }
                else
                    camera.process_key(k, dt);
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
                camera.yaw -= (float)dx * 0.02f;
                camera.pitch += (float)dy * 0.04f;
                mouse_last_x = ev.x;
                mouse_last_y = ev.y;
            }
        }

        // ── Auto-rotation ────────────────────────────────────────────────
        if (spinning)
            camera.yaw += spin_speed * dt;

        // ── Resize detection ─────────────────────────────────────────────
        {
            int new_cols, new_rows;
            platform::get_terminal_size(new_cols, new_rows);
            if (new_cols != cols || new_rows != rows)
            {
                cols = new_cols;
                rows = new_rows;
                fb.resize(cols, (rows - 1) * 2);
            }
        }

        // ── HUD ───────────────────────────────────────────────────────────
        {
            const char *mode_str = nullptr;
            switch (renderer.mode)
            {
            case ShadingMode::Wireframe:
                mode_str = "Wireframe";
                break;
            case ShadingMode::Flat:
                mode_str = "Flat";
                break;
            case ShadingMode::Gouraud:
                mode_str = "Gouraud";
                break;
            case ShadingMode::Phong:
                mode_str = "Phong";
                break;
            }
            const char *lighting_str = lighting_mode == 0 ? "dual" : lighting_mode == 1 ? "single"
                                                                                        : "flat";
            char hud[160];
            std::snprintf(hud, sizeof(hud), "  %s  ·  %d fps  ·  %s  ·  %s  ·  light: %s  ·  bg: %s  ",
                          mode_str, (fps_smooth < 0.0f) ? 0 : (int)fps_smooth, model_name.c_str(),
                          spinning ? "spin ON" : "spin OFF",
                          lighting_str,
                          bg_mode == 0 ? "black" : bg_mode == 1 ? "gray"
                                                                : "white");
            fb.set_hud(hud);
        }

        // ── Render ────────────────────────────────────────────────────────
        const Color bg_color = bg_mode == 0   ? Color{0, 0, 0}
                               : bg_mode == 1 ? Color{128, 128, 128}
                                              : Color{240, 240, 240};
        fb.clear(bg_color);
        // Select light set based on lighting mode.
        // Flat ambient: no directional lights, bright ambient so the full model is visible.
        const vec3 flat_ambient = {0.85f, 0.85f, 0.85f};
        int n_lights = lighting_mode == 0 ? 2 : lighting_mode == 1 ? 1
                                                                   : 0;
        const vec3 &cur_ambient = lighting_mode == 2 ? flat_ambient : ambient;
        renderer.render(mesh, camera, lights, n_lights, cur_ambient, fb,
                        shadow_map ? &*shadow_map : nullptr);
        fb.present();

        // ── Frame cap (≈60 fps) ───────────────────────────────────────────
        // Keeps dt consistent so camera speed feels uniform regardless of
        // how fast the renderer is.
        constexpr float target_dt = 1.0f / 60.0f;
        auto frame_end = clock::now();
        float elapsed = std::chrono::duration<float>(frame_end - now).count();
        if (elapsed < target_dt)
            std::this_thread::sleep_for(
                std::chrono::duration<float>(target_dt - elapsed));
    }

quit:
    platform::disable_mouse();
    platform::disable_raw_mode();
    return 0;
}
