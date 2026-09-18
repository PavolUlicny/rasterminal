#pragma once

#include "src/render/camera.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace viewer
{
    class FrameTiming
    {
      public:
        using Clock = std::chrono::steady_clock;
        // Refresh HUD digits at 10 Hz so high frame rates remain readable.
        static constexpr float FPS_LATCH = 0.1f;
        // Uncapped idle sessions still wait so an unchanged view does not consume a core.
        static constexpr int IDLE_FPS = 60;

        explicit FrameTiming(Clock::time_point start) : prev_(start), frame_start_(start) {}

        void after_resume(Clock::time_point now) noexcept
        {
            first_frame_ = true;
            fps_smooth_ = -1.0f;
            fps_display_ = -1.0f;
            fps_latch_time_ = 0.0f;
            prev_ = now;
        }

        float begin_frame(Clock::time_point now) noexcept
        {
            frame_start_ = now;
            const float raw_dt = std::chrono::duration<float>(frame_start_ - prev_).count();
            prev_ = frame_start_;
            // Idle intervals and the setup gap do not describe rendered FPS. The EMA uses
            // the uncapped interval so slow frames stay accurate.
            if (!first_frame_ && raw_dt > 0.0f && prev_frame_rendered_)
            {
                const float fps = 1.0f / raw_dt;
                fps_smooth_ = (fps_smooth_ < 0.0f) ? fps : (fps_smooth_ * 0.9f) + (fps * 0.1f);
            }
            first_frame_ = false;
            fps_latch_time_ += raw_dt;
            if (fps_latch_time_ >= FPS_LATCH || (fps_display_ < 0.0f && fps_smooth_ >= 0.0f))
            {
                fps_display_ = fps_smooth_;
                fps_latch_time_ = 0.0f;
            }
            // Bound camera motion and held-key steps after a stall.
            return std::min(raw_dt, Camera::HELD_KEY_WINDOW);
        }

        [[nodiscard]] int hud_fps() const noexcept
        {
            return (fps_display_ < 0.0f) ? 0 : static_cast<int>(std::lround(fps_display_));
        }

        [[nodiscard]] std::chrono::duration<float>
        end_frame(bool rendered, int configured_fps, Clock::time_point now) noexcept
        {
            prev_frame_rendered_ = rendered;
            const int frame_cap = (!rendered && configured_fps == 0) ? IDLE_FPS : configured_fps;
            if (frame_cap > 0)
            {
                const float target_dt = 1.0f / static_cast<float>(frame_cap);
                const float elapsed = std::chrono::duration<float>(now - frame_start_).count();
                if (elapsed < target_dt)
                {
                    return std::chrono::duration<float>(target_dt - elapsed);
                }
            }
            return std::chrono::duration<float>::zero();
        }

      private:
        Clock::time_point prev_;
        Clock::time_point frame_start_;
        bool first_frame_ = true;
        bool prev_frame_rendered_ = true;
        float fps_smooth_ = -1.0f;
        float fps_display_ = -1.0f;
        float fps_latch_time_ = 0.0f;
    };

} // namespace viewer
