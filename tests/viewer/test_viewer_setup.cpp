#include "src/args.h"
#include "src/loaders/mesh.h"
#include "src/math/linalg.h"
#include "src/render/camera.h"
#include "src/shading.h"
#include "src/terminal/graphics.h"
#include "src/terminal/hud.h"
#include "src/viewer/graphics_setup.h"
#include "src/viewer/hud_info.h"
#include "src/viewer/scene.h"
#include "src/viewer/state.h"
#include "tests/test.h"

#include <cmath>
#include <string>

namespace
{
    Mesh mesh_spanning(vec3 lo, vec3 hi)
    {
        Mesh mesh;
        mesh.vertices.resize(2);
        mesh.vertices[0].pos = lo;
        mesh.vertices[1].pos = hi;
        return mesh;
    }

    float quat_distance(const quat &a, const quat &b)
    {
        return std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z) + std::fabs(a.w - b.w);
    }

    TermGraphics kitty_and_sixel()
    {
        TermGraphics tg;
        tg.kitty = true;
        tg.kitty_shm = true;
        tg.sixel = true;
        return tg;
    }

    bool same_text(const char *a, const char *b)
    {
        return a != nullptr && b != nullptr && std::string(a) == b;
    }
} // namespace

TEST(viewer_setup, multiplexer_detection_covers_tmux_and_screen)
{
    ASSERT_TRUE(viewer::under_multiplexer("/tmp/tmux-1000/default,1,0", "xterm-256color"));
    ASSERT_TRUE(viewer::under_multiplexer(nullptr, "screen"));
    ASSERT_TRUE(viewer::under_multiplexer(nullptr, "screen.xterm-256color"));
    ASSERT_TRUE(viewer::under_multiplexer(nullptr, "tmux-256color"));
    ASSERT_FALSE(viewer::under_multiplexer(nullptr, "xterm-256color"));
    ASSERT_FALSE(viewer::under_multiplexer(nullptr, nullptr));
}

TEST(viewer_setup, auto_graphics_prefers_kitty)
{
    const TermGraphics tg = kitty_and_sixel();
    const viewer::GraphicsSetup gfx = viewer::resolve_graphics(GraphicsChoice::Auto, &tg, false);
    ASSERT_EQ(gfx.backend, GraphicsBackend::Kitty);
    ASSERT_TRUE(gfx.shm_ok);
    ASSERT_EQ(gfx.exit_code, -1);
}

TEST(viewer_setup, forced_sixel_masks_kitty)
{
    const TermGraphics tg = kitty_and_sixel();
    const viewer::GraphicsSetup gfx = viewer::resolve_graphics(GraphicsChoice::Sixel, &tg, false);
    ASSERT_EQ(gfx.backend, GraphicsBackend::Sixel);
    ASSERT_FALSE(gfx.shm_ok);
    ASSERT_EQ(gfx.exit_code, -1);
}

TEST(viewer_setup, forced_kitty_masks_sixel_and_reports_missing_kitty)
{
    TermGraphics tg;
    tg.sixel = true;
    const viewer::GraphicsSetup gfx = viewer::resolve_graphics(GraphicsChoice::Kitty, &tg, false);
    ASSERT_EQ(gfx.backend, GraphicsBackend::Blocks);
    ASSERT_EQ(gfx.exit_code, 1);
    ASSERT_TRUE(same_text(gfx.error, "terminal does not answer the kitty graphics query"));
}

TEST(viewer_setup, auto_graphics_falls_back_to_blocks_without_error)
{
    const TermGraphics tg;
    const viewer::GraphicsSetup with_reply = viewer::resolve_graphics(GraphicsChoice::Auto, &tg, false);
    ASSERT_EQ(with_reply.backend, GraphicsBackend::Blocks);
    ASSERT_EQ(with_reply.exit_code, -1);
    const viewer::GraphicsSetup multiplexed = viewer::resolve_graphics(GraphicsChoice::Auto, nullptr, false);
    ASSERT_EQ(multiplexed.backend, GraphicsBackend::Blocks);
    ASSERT_EQ(multiplexed.exit_code, -1);
}

TEST(viewer_setup, forced_graphics_under_multiplexer_names_both_multiplexers)
{
    const viewer::GraphicsSetup kitty = viewer::resolve_graphics(GraphicsChoice::Kitty, nullptr, false);
    ASSERT_EQ(kitty.exit_code, 1);
    ASSERT_TRUE(same_text(kitty.error, "kitty graphics is not supported under tmux or GNU screen"));
    const viewer::GraphicsSetup sixel = viewer::resolve_graphics(GraphicsChoice::Sixel, nullptr, false);
    ASSERT_EQ(sixel.exit_code, 1);
    ASSERT_TRUE(same_text(sixel.error, "sixel graphics is not supported under tmux or GNU screen"));
}

TEST(viewer_setup, failed_query_is_an_error_even_when_interrupted)
{
    TermGraphics tg;
    tg.failed = true;
    tg.interrupted = true;
    const viewer::GraphicsSetup gfx = viewer::resolve_graphics(GraphicsChoice::Auto, &tg, true);
    ASSERT_EQ(gfx.exit_code, 1);
    ASSERT_TRUE(same_text(gfx.error, "failed to query terminal graphics"));
}

// A retry must not report the unsupported-backend error from a query a control request cut short.
TEST(viewer_setup, interrupted_query_retries_instead_of_failing)
{
    TermGraphics tg;
    tg.interrupted = true;
    const viewer::GraphicsSetup interrupted = viewer::resolve_graphics(GraphicsChoice::Sixel, &tg, false);
    ASSERT_TRUE(interrupted.interrupted);
    ASSERT_EQ(interrupted.exit_code, -1);

    const TermGraphics complete;
    const viewer::GraphicsSetup requested = viewer::resolve_graphics(GraphicsChoice::Kitty, &complete, true);
    ASSERT_TRUE(requested.interrupted);
    ASSERT_EQ(requested.exit_code, -1);
    const viewer::GraphicsSetup multiplexed = viewer::resolve_graphics(GraphicsChoice::Kitty, nullptr, true);
    ASSERT_TRUE(multiplexed.interrupted);
    ASSERT_EQ(multiplexed.exit_code, -1);
}

TEST(viewer_setup, blocks_choice_ignores_reply_and_control_requests)
{
    const TermGraphics tg = kitty_and_sixel();
    const viewer::GraphicsSetup gfx = viewer::resolve_graphics(GraphicsChoice::Blocks, &tg, true);
    ASSERT_EQ(gfx.backend, GraphicsBackend::Blocks);
    ASSERT_FALSE(gfx.interrupted);
    ASSERT_EQ(gfx.exit_code, -1);
}

// A sixel-only terminal's cell-size reply must not be discarded with the kitty verdict.
TEST(viewer_setup, reply_geometry_survives_backend_selection)
{
    TermGraphics tg;
    tg.sixel = true;
    tg.cell_w = 9;
    tg.cell_h = 18;
    tg.sixel_max_w = 1000;
    tg.sixel_max_h = 800;
    tg.sixel_registers = 256;
    const viewer::GraphicsSetup gfx = viewer::resolve_graphics(GraphicsChoice::Auto, &tg, false);
    ASSERT_EQ(gfx.backend, GraphicsBackend::Sixel);
    ASSERT_EQ(gfx.cell_w, 9);
    ASSERT_EQ(gfx.cell_h, 18);
    ASSERT_EQ(gfx.sixel_max_w, 1000);
    ASSERT_EQ(gfx.sixel_max_h, 800);
    ASSERT_EQ(gfx.sixel_registers, 256);
}

TEST(viewer_setup, auto_fit_frames_the_bounding_box)
{
    ParsedArgs args;
    args.zoom = 2.0f;
    args.first_person_speed = 3.0f;
    const Mesh mesh = mesh_spanning({ -1.0f, 0.0f, 2.0f }, { 3.0f, 2.0f, 6.0f });
    const Camera camera = viewer::auto_fit_camera(mesh, args, false);
    const float radius = 3.0f; // half the diagonal of a 4 x 2 x 4 box
    ASSERT_NEAR(camera.target.x, 1.0f, 1e-6f);
    ASSERT_NEAR(camera.target.y, 1.0f, 1e-6f);
    ASSERT_NEAR(camera.target.z, 4.0f, 1e-6f);
    ASSERT_NEAR(camera.distance, radius * 2.0f / args.zoom, 1e-5f);
    ASSERT_NEAR(camera.near_plane, radius * 0.01f, 1e-6f);
    ASSERT_NEAR(camera.far_plane, radius * 20.0f, 1e-5f);
    ASSERT_NEAR(camera.fp_base_speed, radius, 1e-6f);
    ASSERT_NEAR(camera.fp_speed, 3.0f, 0.0f);
    ASSERT_FALSE(camera.first_person);
}

TEST(viewer_setup, auto_fit_uses_unit_radius_for_a_point_mesh)
{
    const ParsedArgs args;
    const Mesh mesh = mesh_spanning({ 5.0f, 5.0f, 5.0f }, { 5.0f, 5.0f, 5.0f });
    const Camera camera = viewer::auto_fit_camera(mesh, args, false);
    ASSERT_NEAR(camera.distance, 2.0f, 1e-6f);
    ASSERT_NEAR(camera.near_plane, 0.01f, 1e-7f);
}

// Pitches past the pole would flip a first-person view upside down; orbit allows them.
TEST(viewer_setup, auto_fit_clamps_only_first_person_pitch)
{
    const Mesh mesh = mesh_spanning({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f });
    ParsedArgs pole;
    pole.pitch = 90.0f;
    ParsedArgs past_pole;
    past_pole.pitch = 150.0f;

    const Camera fp_pole = viewer::auto_fit_camera(mesh, pole, true);
    const Camera fp_past = viewer::auto_fit_camera(mesh, past_pole, true);
    ASSERT_TRUE(fp_pole.first_person);
    ASSERT_TRUE(quat_distance(fp_pole.orientation, fp_past.orientation) < 1e-5f);

    const Camera orbit_pole = viewer::auto_fit_camera(mesh, pole, false);
    const Camera orbit_past = viewer::auto_fit_camera(mesh, past_pole, false);
    ASSERT_TRUE(quat_distance(orbit_pole.orientation, orbit_past.orientation) > 0.1f);
}

TEST(viewer_setup, flat_lighting_replaces_ambient_and_drops_lights)
{
    const viewer::SceneLights scene = viewer::default_lights();
    ASSERT_EQ(viewer::light_count(LightingMode::Dual), 2);
    ASSERT_EQ(viewer::light_count(LightingMode::Single), 1);
    ASSERT_EQ(viewer::light_count(LightingMode::Flat), 0);
    const vec3 flat = viewer::lighting_ambient(LightingMode::Flat, scene.ambient);
    ASSERT_NEAR(flat.x, viewer::FLAT_AMBIENT.x, 0.0f);
    const vec3 dual = viewer::lighting_ambient(LightingMode::Dual, scene.ambient);
    ASSERT_NEAR(dual.x, scene.ambient.x, 0.0f);
}

TEST(viewer_setup, hud_model_name_is_a_sanitized_basename)
{
    ASSERT_TRUE(viewer::hud_model_name("models/sponza.glb") == "sponza.glb");
    ASSERT_TRUE(viewer::hud_model_name("C:\\models\\cube.obj") == "cube.obj");
    ASSERT_TRUE(viewer::hud_model_name("dir/a\x1b[2Jb.obj") == "a?[2Jb.obj");
    ASSERT_TRUE(viewer::hud_model_name("plain.stl") == "plain.stl");
}

TEST(viewer_setup, hud_info_names_wireframe_colour_only_in_wireframe)
{
    ParsedArgs args;
    args.shading = ShadingMode::Wireframe;
    args.wireframe_color = WireframeColor::Cyan;
    args.lighting = LightingMode::Single;
    args.bg = Background::Gray;
    viewer::ViewerState state(args, Camera{});
    const std::string model = "cube.obj";

    const HudInfo wire = viewer::make_hud_info(state, model, true, 42);
    ASSERT_TRUE(wire.model_name == "cube.obj");
    ASSERT_TRUE(wire.shading_name == "wireframe");
    ASSERT_TRUE(wire.light_name == "single");
    ASSERT_TRUE(wire.bg_name == "gray");
    ASSERT_TRUE(wire.wf_name == "cyan");
    ASSERT_EQ(wire.wf_color.r, viewer::wireframe_color_of(WireframeColor::Cyan).r);
    ASSERT_TRUE(wire.has_textures);
    ASSERT_EQ(wire.fps, 42);

    state.settings.shading = ShadingMode::Phong;
    const HudInfo phong = viewer::make_hud_info(state, model, false, 0);
    ASSERT_TRUE(phong.shading_name == "phong");
    ASSERT_TRUE(phong.wf_name.empty());
    ASSERT_FALSE(phong.has_textures);
}
