#include "args.h"
#include "camera.h"
#include "framebuffer.h"
#include "light.h"
#include "mesh.h"
#include "platform.h"
#include "renderer.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

static volatile sig_atomic_t g_interrupted = 0;
static void signal_handler(int) { g_interrupted = 1; }

static constexpr Color BG_BLACK = {0, 0, 0};
static constexpr Color BG_GRAY = {128, 128, 128};
static constexpr Color BG_WHITE = {240, 240, 240};

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
        if (radius < 1e-4f)
            radius = 1.0f; // degenerate (all coincident vertices) — use sane defaults
        camera.target = centre;
        camera.distance = radius * 2.0f;
        // Scale near/far to the model so arbitrarily-sized models aren't clipped.
        camera.near_plane = radius * 0.01f;
        camera.far_plane = radius * 20.0f;
    }
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
    lights[0].direction = {0.408f, 0.816f, 0.408f};
    lights[0].color = {1.0f, 0.9f, 0.75f};
    lights[1].direction = {-0.667f, -0.333f, -0.667f};
    lights[1].color = {0.15f, 0.25f, 0.5f};
    const vec3 ambient = {0.2f, 0.2f, 0.25f};

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
                camera.yaw -= static_cast<float>(dx) * 0.02f;
                camera.pitch += static_cast<float>(dy) * 0.04f;
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
                fb.resize(cols, (rows - hud_rows) * 2);
            }
        }

        // ── HUD ───────────────────────────────────────────────────────────
        if (args.hud)
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
                              mode_str, (fps_smooth < 0.0f) ? 0 : static_cast<int>(fps_smooth), model_name.c_str(),
                              spinning ? "spin ON" : "spin OFF",
                              lighting_str, bg_str, WIREFRAME_NAMES[wf_color],
                              culling ? "ON" : "OFF", tex_suffix);
            else
                std::snprintf(hud, sizeof(hud), "  %s  ·  %d fps  ·  %s  ·  %s  ·  light: %s  ·  bg: %s  ·  cull: %s%s",
                              mode_str, (fps_smooth < 0.0f) ? 0 : static_cast<int>(fps_smooth), model_name.c_str(),
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
        const vec3 flat_ambient = {0.85f, 0.85f, 0.85f};
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
        const vec3 &cur_ambient = lighting_mode == 2 ? flat_ambient : ambient;
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
