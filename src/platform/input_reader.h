#pragma once

#include "src/platform/input.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include "src/platform/control.h"
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace platform
{
    namespace detail
    {
        // Longer terminal replies use the skip path.
        constexpr int MAX_PENDING = 1024;

        // Maximum inter-byte gap, not total sequence time.
        constexpr int PARTIAL_TIMEOUT_MS = 50;

        // A stalled prefix beyond this is terminal payload, not a keypress.
        constexpr int MAX_KEY_SEQUENCE = 64;

        // Abandon skipped replies below this arrival rate.
        constexpr int RATE_WINDOW_MS = 500;
        constexpr int RATE_QUOTA = 128;

        // Carry burst credit for at most this many windows.
        constexpr int RATE_MAX_CARRY = 2 * RATE_QUOTA;

        // Cap the charge after a long pause.
        constexpr int RATE_MAX_WINDOWS = 4;

        // Leave room for one maximum charge plus carry.
        constexpr int RATE_MAX_CREDIT = (RATE_MAX_WINDOWS * RATE_QUOTA) + RATE_MAX_CARRY;

        struct ArrivalMeter
        {
            int credit = 0;
            std::chrono::steady_clock::time_point window;

            void record(int bytes) { credit = std::min(credit + bytes, RATE_MAX_CREDIT); }

            // Report starvation only after at least one full window closes.
            bool below_floor(std::chrono::steady_clock::time_point now)
            {
                const auto span = std::chrono::milliseconds(RATE_WINDOW_MS);
                if (window == std::chrono::steady_clock::time_point{})
                {
                    // Start the first window without charging it.
                    window = now;
                }
                if (now - window < span)
                {
                    return false;
                }
                const auto elapsed = (now - window) / span;
                const int windows = static_cast<int>(std::min<decltype(elapsed)>(elapsed, RATE_MAX_WINDOWS));
                const int quota = windows * RATE_QUOTA;
                const bool starved = credit < quota;
                window = now;
                credit = std::clamp(credit - quota, 0, RATE_MAX_CARRY);
                return starved;
            }
        };

        // Reaching this per-pass read limit drops nothing.
        constexpr int MAX_REFILLS_PER_PASS = 1024;

        // Pending bytes persist across calls; last_growth times partial reassembly.
        struct Pending
        {
            char buf[MAX_PENDING] = {};
            int len = 0;
            std::chrono::steady_clock::time_point last_growth;
            // Skip mode keeps only the introducer and a possible ST-prefix ESC.
            bool skipping = false;
            // Measure arrivals before skip mode starts.
            ArrivalMeter meter;
            // A drain pass spans poll_event calls until Type::None.
            int refills = 0;
        };

        inline Pending &pending()
        {
            static Pending p;
            return p;
        }

        // Read available bytes; zero means idle or closed.
        inline int read_available(char *out, int cap)
        {
#ifdef _WIN32
            // The console API only yields one byte at a time.
            int n = 0;
            while (n < cap && _kbhit())
            {
                if (!read_console_byte(out[n]))
                {
                    break;
                }
                n++;
            }
            return n;
#else
            // poll() keeps test pipes non-blocking; read the whole available burst.
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            if (poll(&pfd, 1, 0) <= 0)
            {
                return 0;
            }
            return static_cast<int>(read(STDIN_FILENO, out, static_cast<size_t>(cap)));
#endif
        }

        // Append one bounded read.
        inline int refill(Pending &p)
        {
            const int got = read_available(p.buf + p.len, MAX_PENDING - p.len);
            if (got <= 0)
            {
                return 0;
            }
            p.len += got;
            p.last_growth = std::chrono::steady_clock::now();
            return got;
        }

        // Retain the introducer and any trailing ST-prefix ESC.
        inline void enter_skip(Pending &p)
        {
            const bool ends_on_esc = p.buf[p.len - 1] == '\033';
            p.len = 2;
            if (ends_on_esc)
            {
                p.buf[2] = '\033';
                p.len = 3;
            }
            p.skipping = true;
        }
    } // namespace detail

    inline void reset_input_state() noexcept
    {
        detail::pending() = {};
    }

    // Reassemble split events and discard stale or unterminated sequences.

    // Reset the refill budget when a caller stops draining before Type::None.
    inline void end_input_pass()
    {
        detail::pending().refills = 0;
    }

    inline InputEvent poll_event()
    {
        detail::Pending &p = detail::pending();

        // Prevent staleness checks on a call that just extended the prefix.
        bool read_this_call = false;

        // Compact the fixed buffer instead of tracking a second offset.
        auto consume = [&p](int n)
        {
            p.len -= n;
            if (p.len > 0)
            {
                std::memmove(p.buf, p.buf + n, static_cast<size_t>(p.len));
            }
        };

        // Each loop consumes buffered bytes or performs one bounded read.
        for (;;)
        {
            if (p.skipping)
            {
                // A skipped sequence has a missing middle and must never be decoded.
                const int end = detail::skip_scan(p.buf, p.len);
                if (end > 0)
                {
                    consume(end);
                    p.skipping = false;
                    continue;
                }
                // Handle an unfinished skip like any other partial sequence.
            }
            else
            {
                const detail::ParseResult r = detail::parse_input(p.buf, p.len);
                if (r.kind != detail::ParseResult::Kind::Incomplete)
                {
                    consume(r.consumed);
                    if (r.kind == detail::ParseResult::Kind::Complete)
                    {
                        return r.event;
                    }
                    continue;
                }
            }

            // Keep the family prefix and scan later bytes for the terminator.
            if (p.len >= detail::MAX_PENDING)
            {
                detail::enter_skip(p);
                continue;
            }

            // Refill within the pass budget. A spent budget is not evidence of staleness.
            if (p.refills >= detail::MAX_REFILLS_PER_PASS)
            {
                end_input_pass();
                return InputEvent{};
            }
            p.refills++;
            const int just_read = detail::refill(p);
            if (just_read > 0)
            {
                read_this_call = true;
                p.meter.record(just_read);
                continue;
            }

            // Share one clock read between the stale-prefix and rate checks.
            const auto now = std::chrono::steady_clock::now();

            // A growing prefix is not stale; long skips use the arrival-rate floor.
            if (!p.skipping && !read_this_call && p.len > 0 &&
                now - p.last_growth > std::chrono::milliseconds(detail::PARTIAL_TIMEOUT_MS))
            {
                // Skip stale payload-sized prefixes; discard stale key-sized prefixes.
                if (p.len >= detail::MAX_KEY_SEQUENCE)
                {
                    detail::enter_skip(p);
                    continue;
                }
                p.len = 0;
                continue;
            }

            // Abandon an unterminated skip once measured arrivals fall below the floor.
            const bool starved = p.meter.below_floor(now);
            if (p.skipping && starved)
            {
                p.len = 0;
                p.skipping = false;
                continue;
            }

            // Type::None ends the caller's drain pass.
            end_input_pass();
            return InputEvent{};
        }
    }

} // namespace platform
