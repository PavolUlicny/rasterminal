#include "src/viewer/frame_timing.h"
#include "src/viewer/input_controller.h"
#include "src/viewer/state.h"
#include "tests/test.h"

#include <chrono>
#include <cmath>

namespace
{
    using Clock = std::chrono::steady_clock;

    platform::InputEvent key(platform::Key value)
    {
        return { platform::InputEvent::Type::Key, value };
    }

    Clock::duration seconds(float value)
    {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(value));
    }

    platform::InputEvent mouse(platform::InputEvent::Type type, int x, int y)
    {
        return { type, platform::Key::None, x, y };
    }
} // namespace

TEST(viewer, reset_restores_launch_settings_and_clears_held_key)
{
    ParsedArgs args;
    args.shading = ShadingMode::Flat;
    args.bg = Background::Gray;
    args.cull = false;
    Camera launch;
    viewer::ViewerState state(args, launch);
    viewer::InputController input(true, true);
    const Clock::time_point start{};

    ASSERT_TRUE(input.handle(key(platform::Key::Plus), state, 80, 24, start));
    ASSERT_TRUE(input.advance(state, 0.05f, start + std::chrono::milliseconds(50)));
    ASSERT_TRUE(state.camera.distance != launch.distance);
    for (const platform::Key k : { platform::Key::Num1, platform::Key::B, platform::Key::K, platform::Key::Space,
                                   platform::Key::L, platform::Key::C, platform::Key::T })
    {
        ASSERT_TRUE(input.handle(key(k), state, 80, 24, start));
    }
    ASSERT_TRUE(state.settings.shading != ShadingMode::Flat);
    ASSERT_TRUE(state.settings.background != Background::Gray);
    ASSERT_TRUE(state.settings.culling);
    ASSERT_TRUE(state.settings.spinning);
    ASSERT_TRUE(state.settings.lighting != LightingMode::Dual);
    ASSERT_TRUE(state.settings.wireframe_color != WireframeColor::White);
    ASSERT_FALSE(state.settings.texturing);
    ASSERT_TRUE(input.handle(key(platform::Key::R), state, 80, 24, start));

    ASSERT_EQ(state.settings.shading, ShadingMode::Flat);
    ASSERT_EQ(state.settings.background, Background::Gray);
    ASSERT_FALSE(state.settings.culling);
    ASSERT_FALSE(state.settings.spinning);
    ASSERT_EQ(state.settings.lighting, LightingMode::Dual);
    ASSERT_EQ(state.settings.wireframe_color, WireframeColor::White);
    ASSERT_TRUE(state.settings.texturing);
    ASSERT_EQ(state.camera.distance, launch.distance);
    ASSERT_FALSE(input.advance(state, 0.05f, start + std::chrono::milliseconds(60)));
}

TEST(viewer, held_key_expires_from_supplied_time)
{
    ParsedArgs args;
    viewer::ViewerState state(args, Camera{});
    viewer::InputController input(true, false);
    const Clock::time_point start{};
    ASSERT_TRUE(input.handle(key(platform::Key::Plus), state, 80, 24, start));
    ASSERT_TRUE(input.advance(state, 0.01f, start + std::chrono::milliseconds(99)));
    ASSERT_FALSE(input.advance(state, 0.01f, start + std::chrono::milliseconds(101)));
}

TEST(viewer, key_tap_lands_within_a_frame_of_one_wheel_notch)
{
    // The first applied dt began before the key byte, making the one-frame error
    // two-sided; uneven pacing is required to expose undershoot.
    struct Pacing
    {
        float press_frame, rest;
    };
    const Pacing pacings[] = {
        { 1.0f / 60.0f, 1.0f / 60.0f }, // even, 60 fps
        { 0.05f, 0.05f },               // even, 20 fps
        { 0.005f, 0.030f },             // short frame at the press, then longer
        { 0.001f, 0.049f },             // the same, more extreme
        { 0.050f, 0.005f },             // long frame at the press, then shorter
    };
    ParsedArgs args;
    Camera launch;
    launch.first_person = true;
    for (const Pacing &p : pacings)
    {
        const float widest = (p.press_frame > p.rest) ? p.press_frame : p.rest;
        const float slack = std::pow(Camera::FP_SPEED_WHEEL_STEP, widest / Camera::HELD_KEY_WINDOW);
        for (int i = 0; i <= 16; i++)
        {
            viewer::ViewerState state(args, launch);
            viewer::InputController input(true, false);
            const Clock::time_point pressed{};
            ASSERT_TRUE(input.handle(key(platform::Key::Plus), state, 80, 24, pressed));
            Clock::time_point now = pressed + seconds(p.press_frame * (static_cast<float>(i) / 16.0f));
            float dt = p.press_frame;
            while (input.advance(state, dt, now))
            {
                dt = p.rest;
                now += seconds(p.rest);
            }
            ASSERT_TRUE(state.camera.fp_speed >= (Camera::FP_SPEED_WHEEL_STEP / slack) - 1e-4f);
            ASSERT_TRUE(state.camera.fp_speed <= (Camera::FP_SPEED_WHEEL_STEP * slack) + 1e-4f);
        }
    }
}

TEST(viewer, idle_interval_does_not_change_rendered_fps)
{
    const Clock::time_point start{};
    viewer::FrameTiming timing(start);
    ASSERT_EQ(timing.hud_fps(), 0);
    timing.begin_frame(start);
    ASSERT_EQ(timing.end_frame(true, 0, start).count(), 0.0f);
    timing.begin_frame(start + std::chrono::milliseconds(20));
    ASSERT_EQ(timing.hud_fps(), 50);
    const auto idle_wait = timing.end_frame(false, 0, start + std::chrono::milliseconds(20));
    ASSERT_TRUE(idle_wait.count() > 0.0f);
    // The 200 ms idle interval would pull the EMA down to 45.5 FPS if counted.
    timing.begin_frame(start + std::chrono::milliseconds(220));
    ASSERT_EQ(timing.hud_fps(), 50);
}

TEST(viewer, resume_discards_stall_and_first_frame_interval)
{
    const Clock::time_point start{};
    viewer::FrameTiming timing(start);
    timing.begin_frame(start);
    ASSERT_TRUE(timing.end_frame(true, 30, start).count() > 0.0f);
    timing.begin_frame(start + std::chrono::milliseconds(20));
    ASSERT_EQ(timing.hud_fps(), 50);

    timing.after_resume(start + std::chrono::seconds(1));
    ASSERT_EQ(timing.hud_fps(), 0);
    const float dt = timing.begin_frame(start + std::chrono::milliseconds(1010));
    ASSERT_NEAR(dt, 0.01f, 0.0001f);
    ASSERT_EQ(timing.hud_fps(), 0);
    ASSERT_TRUE(timing.end_frame(true, 30, start + std::chrono::milliseconds(1015)).count() > 0.0f);
    timing.begin_frame(start + std::chrono::milliseconds(1040));
    ASSERT_EQ(timing.hud_fps(), 33);
}

TEST(viewer, disabled_input_drains_without_changing_state)
{
    ParsedArgs args;
    viewer::ViewerState state(args, Camera{});
    viewer::InputController input(false, true);
    const Clock::time_point start{};
    ASSERT_FALSE(input.handle(key(platform::Key::Num1), state, 80, 24, start));
    ASSERT_FALSE(input.handle(mouse(platform::InputEvent::Type::MousePress, 1, 1), state, 80, 24, start));
    ASSERT_FALSE(input.handle(mouse(platform::InputEvent::Type::MouseMove, 2, 1), state, 80, 24, start));
    ASSERT_EQ(state.settings.shading, ShadingMode::Phong);
    ASSERT_EQ(state.camera.orientation.w, 1.0f);
}

TEST(viewer, texture_toggle_requires_a_texture)
{
    ParsedArgs args;
    viewer::ViewerState state(args, Camera{});
    const Clock::time_point start{};
    viewer::InputController no_textures(true, false);
    ASSERT_TRUE(no_textures.handle(key(platform::Key::T), state, 80, 24, start));
    ASSERT_TRUE(state.settings.texturing);
    viewer::InputController textures(true, true);
    ASSERT_TRUE(textures.handle(key(platform::Key::T), state, 80, 24, start));
    ASSERT_FALSE(state.settings.texturing);
}

TEST(viewer, orbit_vertical_keys_leave_held_movement_intact)
{
    ParsedArgs args;
    viewer::ViewerState state(args, Camera{});
    viewer::InputController input(true, false);
    const Clock::time_point start{};
    ASSERT_TRUE(input.handle(key(platform::Key::Plus), state, 80, 24, start));
    ASSERT_TRUE(input.handle(key(platform::Key::E), state, 80, 24, start + std::chrono::milliseconds(10)));
    ASSERT_TRUE(input.handle(key(platform::Key::V), state, 80, 24, start + std::chrono::milliseconds(20)));
    ASSERT_TRUE(input.advance(state, 0.05f, start + std::chrono::milliseconds(90)));
    ASSERT_TRUE(state.camera.distance < Camera{}.distance);
}

TEST(viewer, impossible_mouse_delta_reseeds_drag)
{
    ParsedArgs args;
    viewer::ViewerState state(args, Camera{});
    viewer::InputController input(true, false);
    const Clock::time_point start{};
    ASSERT_TRUE(input.handle(mouse(platform::InputEvent::Type::MousePress, 1, 1), state, 80, 24, start));
    ASSERT_TRUE(input.handle(mouse(platform::InputEvent::Type::MouseMove, 100, 100), state, 80, 24, start));
    ASSERT_EQ(state.camera.orientation.w, 1.0f);
    ASSERT_TRUE(input.handle(mouse(platform::InputEvent::Type::MouseMove, 101, 100), state, 80, 24, start));
    ASSERT_TRUE(state.camera.orientation.w < 1.0f);
}

TEST(viewer, zero_grid_mouse_move_does_not_corrupt_camera)
{
    ParsedArgs args;
    viewer::ViewerState state(args, Camera{});
    viewer::InputController input(true, false);
    const Clock::time_point start{};
    ASSERT_TRUE(input.handle(mouse(platform::InputEvent::Type::MousePress, 3, 3), state, 0, 0, start));
    ASSERT_TRUE(input.handle(mouse(platform::InputEvent::Type::MouseMove, 3, 3), state, 0, 0, start));
    ASSERT_TRUE(std::isfinite(state.camera.orientation.x));
    ASSERT_TRUE(std::isfinite(state.camera.orientation.y));
    ASSERT_TRUE(std::isfinite(state.camera.orientation.z));
    ASSERT_TRUE(std::isfinite(state.camera.orientation.w));
}

TEST(viewer, scroll_respects_orbit_bounds_and_first_person_steps)
{
    ParsedArgs args;
    viewer::ViewerState state(args, Camera{});
    viewer::InputController input(true, false);
    const Clock::time_point start{};
    state.camera.distance = state.camera.near_plane * 2.01f;
    ASSERT_TRUE(input.handle({ platform::InputEvent::Type::ScrollUp }, state, 80, 24, start));
    ASSERT_NEAR(state.camera.distance, state.camera.near_plane * 2.0f, 1e-6f);
    state.camera.distance = state.camera.max_eye_distance();
    ASSERT_TRUE(input.handle({ platform::InputEvent::Type::ScrollDown }, state, 80, 24, start));
    ASSERT_NEAR(state.camera.distance, state.camera.max_eye_distance(), 1e-6f);

    state.camera.first_person = true;
    const float speed = state.camera.fp_speed;
    ASSERT_TRUE(input.handle({ platform::InputEvent::Type::ScrollUp }, state, 80, 24, start));
    ASSERT_NEAR(state.camera.fp_speed, speed * Camera::FP_SPEED_WHEEL_STEP, 1e-6f);
    ASSERT_TRUE(input.handle({ platform::InputEvent::Type::ScrollDown }, state, 80, 24, start));
    ASSERT_NEAR(state.camera.fp_speed, speed, 1e-6f);
}
