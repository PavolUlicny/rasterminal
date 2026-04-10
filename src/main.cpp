#include "camera.h"
#include "framebuffer.h"
#include "light.h"
#include "mesh.h"
#include "platform.h"
#include "renderer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

int main(int argc, char *argv[])
{
    const char *obj_path = (argc > 1) ? argv[1] : "models/teapot.obj";

    Mesh mesh;
    if (!mesh.load_obj(obj_path))
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
    }

    platform::enable_raw_mode();

    int cols, rows;
    platform::get_terminal_size(cols, rows);

    // Each terminal cell is 2 vertical pixels (▀ half-block).
    Framebuffer fb(cols, rows * 2);

    Light light;
    Renderer renderer;

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

        // ── Input ─────────────────────────────────────────────────────────
        // Drain all queued keys so held keys feel responsive.
        while (true)
        {
            platform::Key k = platform::poll_key();
            if (k == platform::KEY_NONE)
                break;
            if (k == platform::KEY_Q || k == platform::KEY_ESCAPE)
                goto quit;
            if (k == platform::KEY_1)
                renderer.mode = ShadingMode::Wireframe;
            else if (k == platform::KEY_2)
                renderer.mode = ShadingMode::Flat;
            else if (k == platform::KEY_3)
                renderer.mode = ShadingMode::Gouraud;
            else
                camera.process_key(k, dt);
        }

        // ── Resize detection ─────────────────────────────────────────────
        {
            int new_cols, new_rows;
            platform::get_terminal_size(new_cols, new_rows);
            if (new_cols != cols || new_rows != rows)
            {
                cols = new_cols;
                rows = new_rows;
                fb.resize(cols, rows * 2);
            }
        }

        // ── Render ────────────────────────────────────────────────────────
        fb.clear();
        renderer.render(mesh, camera, light, fb);
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
