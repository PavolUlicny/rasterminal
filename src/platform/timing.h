#pragma once

#include "src/platform/control.h"

#include <algorithm>
#include <cerrno>
#include <chrono>

#ifdef _WIN32
#include <thread>
#else
#include <ctime>
#endif

namespace platform
{
    namespace detail
    {
        inline auto frame_deadline(std::chrono::steady_clock::time_point now, std::chrono::duration<float> delay)
        {
            // Float loses frame-sized increments after long uptimes.
            return now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
        }

#ifndef _WIN32
        template <typename Now, typename Sleep>
        inline void wait_frame_until(std::chrono::steady_clock::time_point deadline, Now now, Sleep sleep_for)
        {
            while (!control_requested())
            {
                const auto remaining = std::chrono::ceil<std::chrono::nanoseconds>(deadline - now());
                if (remaining <= std::chrono::nanoseconds::zero())
                {
                    break;
                }
                // Bound the check-to-sleep signal race without rounding short frame waits.
                const auto slice = std::min(remaining, std::chrono::nanoseconds(std::chrono::milliseconds(50)));
                const timespec timeout{ 0, static_cast<long>(slice.count()) };
                if (sleep_for(&timeout, nullptr) != 0 && errno != EINTR)
                {
                    break;
                }
            }
        }
#endif
    } // namespace detail

    inline void wait_frame(std::chrono::duration<float> duration)
    {
#ifdef _WIN32
        std::this_thread::sleep_for(duration);
#else
        const auto deadline = detail::frame_deadline(std::chrono::steady_clock::now(), duration);
        detail::wait_frame_until(deadline, std::chrono::steady_clock::now, nanosleep);
#endif
    }

} // namespace platform
