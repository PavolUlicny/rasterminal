#pragma once

#include "src/platform/input.h"
#include "src/viewer/state.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace viewer
{
    class InputController
    {
      public:
        InputController(bool enabled, bool has_textures) : enabled_(enabled), has_textures_(has_textures) {}

        void clear_motion() noexcept
        {
            held_cam_key_ = platform::Key::None;
            mouse_dragging_ = false;
        }

        // Enabled input conservatively marks the scene dirty, even if state did not change.
        bool handle(
            const platform::InputEvent &ev,
            ViewerState &state,
            int cols,
            int rows,
            std::chrono::steady_clock::time_point now
        )
        {
            // Terminal replies belong to geometry, even if a caller passes them here.
            if (!enabled_ || ev.type == platform::InputEvent::Type::None ||
                ev.type == platform::InputEvent::Type::CellSize || ev.type == platform::InputEvent::Type::SixelGeometry)
            {
                return false;
            }
            Camera &camera = state.camera;
            if (ev.type == platform::InputEvent::Type::Key)
            {
                ViewerSettings &settings = state.settings;
                const platform::Key k = ev.key;
                switch (k)
                {
                case platform::Key::Space:
                    settings.spinning = !settings.spinning;
                    break;
                case platform::Key::Num1:
                    settings.shading = ShadingMode::Wireframe;
                    break;
                case platform::Key::Num2:
                    settings.shading = ShadingMode::Flat;
                    break;
                case platform::Key::Num3:
                    settings.shading = ShadingMode::Phong;
                    break;
                case platform::Key::B:
                    settings.background = cycle(settings.background, BACKGROUND_COUNT);
                    break;
                case platform::Key::L:
                    settings.lighting = cycle(settings.lighting, LIGHTING_MODE_COUNT);
                    break;
                case platform::Key::C:
                    settings.wireframe_color = cycle(settings.wireframe_color, WIREFRAME_COLOR_COUNT);
                    break;
                case platform::Key::K:
                    settings.culling = !settings.culling;
                    break;
                case platform::Key::T:
                    if (has_textures_)
                    {
                        settings.texturing = !settings.texturing;
                    }
                    break;
                case platform::Key::R:
                    // Cancel held movement when restoring the launch camera.
                    state.reset();
                    held_cam_key_ = platform::Key::None;
                    break;
                case platform::Key::E:
                case platform::Key::V:
                    // In orbit mode, E/V leave the previous movement key latched.
                    if (camera.first_person)
                    {
                        held_cam_key_ = k;
                        held_cam_key_tp_ = now;
                    }
                    break;
                default:
                    // The parser drops unbound keys before they reach this branch.
                    held_cam_key_ = k;
                    held_cam_key_tp_ = now;
                    break;
                }
            }
            else if (ev.type == platform::InputEvent::Type::ScrollUp)
            {
                // Wheel reports have no magnitude. Reciprocal speed steps cancel.
                if (camera.first_person)
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
                if (camera.first_person)
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
                mouse_last_x_ = ev.x;
                mouse_last_y_ = ev.y;
                mouse_dragging_ = true;
            }
            else if (ev.type == platform::InputEvent::Type::MouseRelease)
            {
                // Button identity is unknown; any release ends the drag.
                mouse_dragging_ = false;
            }
            else if (ev.type == platform::InputEvent::Type::MouseMove)
            {
                // A missing press, impossible delta, or zero grid resets the drag origin.
                const bool implausible = cols <= 0 || rows <= 0 || std::abs(ev.x - mouse_last_x_) > cols ||
                                         std::abs(ev.y - mouse_last_y_) > rows;
                if (mouse_dragging_ && !implausible)
                {
                    const float dx_rad = static_cast<float>(ev.x - mouse_last_x_) / static_cast<float>(cols) * 6.2832f;
                    const float dy_rad = static_cast<float>(ev.y - mouse_last_y_) / static_cast<float>(rows) * 3.1416f;
                    camera.look(dx_rad, -dy_rad);
                }
                mouse_last_x_ = ev.x;
                mouse_last_y_ = ev.y;
                mouse_dragging_ = true;
            }
            return true;
        }

        bool advance(ViewerState &state, float dt, std::chrono::steady_clock::time_point now)
        {
            if (held_cam_key_ == platform::Key::None)
            {
                return false;
            }
            const float since = std::chrono::duration<float>(now - held_cam_key_tp_).count();
            if (since > Camera::HELD_KEY_WINDOW)
            {
                held_cam_key_ = platform::Key::None;
                return false;
            }
            state.camera.process_key(held_cam_key_, dt);
            return true;
        }

      private:
        template <typename E> static constexpr E cycle(E value, int count) noexcept
        {
            return static_cast<E>((static_cast<int>(value) + 1) % count);
        }

        bool enabled_;
        bool has_textures_;
        platform::Key held_cam_key_ = platform::Key::None;
        std::chrono::steady_clock::time_point held_cam_key_tp_;
        int mouse_last_x_ = 0;
        int mouse_last_y_ = 0;
        bool mouse_dragging_ = false;
    };

} // namespace viewer
