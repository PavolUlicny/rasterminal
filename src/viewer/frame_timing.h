#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

namespace viewer
{
    class FrameTiming
    {
      public:
        using Clock = std::chrono::steady_clock;
        // Refresh HUD digits at 10 Hz.
        static constexpr float FPS_LATCH = 0.1f;
        // Cap otherwise uncapped idle loops.
        static constexpr int IDLE_FPS = 60;
        // Seconds; one frame at --fps 1.
        static constexpr float MAX_MOTION_DT = 1.0f;

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
            // Count rendered intervals only. Use raw time so slow frames count accurately.
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
            // Limit movement after a stall to the slowest interval --fps can request, so no frame
            // cap slows spin or held-key motion.
            return std::min(raw_dt, MAX_MOTION_DT);
        }

        [[nodiscard]] int hud_fps() const noexcept
        {
            return (fps_display_ < 0.0f) ? 0 : static_cast<int>(std::lround(fps_display_));
        }

        // Keep the wait in integer ticks. MSVC's sleep_for adds a float duration to a boot-relative
        // time point, which loses frame-sized steps after a few days of uptime.
        [[nodiscard]] Clock::duration end_frame(bool rendered, int configured_fps, Clock::time_point now) noexcept
        {
            prev_frame_rendered_ = rendered;
            const int frame_cap = (!rendered && configured_fps == 0) ? IDLE_FPS : configured_fps;
            if (frame_cap > 0)
            {
                const Clock::duration target = Clock::duration(std::chrono::seconds(1)) / frame_cap;
                const Clock::duration elapsed = now - frame_start_;
                if (elapsed < target)
                {
                    return target - elapsed;
                }
            }
            return Clock::duration::zero();
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
