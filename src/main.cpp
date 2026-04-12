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
#include <cstring>
#include <string>
#include <thread>

static volatile sig_atomic_t g_interrupted = 0;
static void signal_handler(int) { g_interrupted = 1; }

int main(int argc, char *argv[])
{
    const char *obj_path = (argc > 1) ? argv[1] : "models/obj/teapot.obj";

    Mesh mesh;
    if (!mesh.load_model(obj_path))
    {
        std::fprintf(stderr, "Failed to load '%s'\n", obj_path);
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

    Renderer renderer;

    float fps_smooth = 60.0f; // exponential moving average
    bool spinning = false;
    int bg_mode = 0;               // 0=black, 1=gray, 2=white
    const float spin_speed = 0.8f; // radians/sec

    using clock = std::chrono::steady_clock;
    auto prev = clock::now();

    while (true)
    {
        // ── Frame timing ──────────────────────────────────────────────────
        auto now = clock::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;
        // Cap dt so a stall doesn't cause a huge jump.
        if (dt > 0.1f)
            dt = 0.1f;

        if (dt > 0.0f)
            fps_smooth = fps_smooth * 0.9f + (1.0f / dt) * 0.1f;

        // ── Input ─────────────────────────────────────────────────────────
        if (g_interrupted)
            goto quit;

        // Drain all queued keys so held keys feel responsive.
        while (true)
        {
            platform::Key k = platform::poll_key();
            if (k == platform::KEY_NONE)
                break;
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
            else
                camera.process_key(k, dt);
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
            char hud[128];
            std::snprintf(hud, sizeof(hud), "  %s  ·  %d fps  ·  %s  ·  %s  ·  bg: %s  ",
                          mode_str, (int)fps_smooth, model_name.c_str(),
                          spinning ? "spin ON" : "spin OFF",
                          bg_mode == 0 ? "black" : bg_mode == 1 ? "gray"
                                                                : "white");
            fb.set_hud(hud);
        }

        // ── Render ────────────────────────────────────────────────────────
        const Color bg_color = bg_mode == 0   ? Color{0, 0, 0}
                               : bg_mode == 1 ? Color{128, 128, 128}
                                              : Color{240, 240, 240};
        fb.clear(bg_color);
        renderer.render(mesh, camera, lights, 2, ambient, fb);
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
    platform::disable_raw_mode();
    return 0;
}
