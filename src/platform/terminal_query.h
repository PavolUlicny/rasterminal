#pragma once

#include "src/platform/console.h"
#include "src/platform/control.h"
#include "src/platform/shared_memory.h"
#include "src/platform/terminal_io.h"
#include "src/terminal/graphics.h"
#include "src/terminal/kitty.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace platform
{
    namespace detail
    {
        // Query replies are small; leave headroom for incidental input.
        constexpr int GRAPHICS_REPLY_BUF = 512;
        inline constexpr char QUERY_CELL_SIZE[] = "\033[16t";
        inline constexpr char QUERY_SIXEL_GEOMETRY[] = "\033[?2;1;0S";
        constexpr int GRAPHICS_QUERY_TIMEOUT_MS = 1000;

        // Bounded query read: positive bytes, zero to retry, negative to stop.
#ifdef _WIN32
        using ReadConsoleInputFn = BOOL(WINAPI *)(HANDLE, PINPUT_RECORD, DWORD, LPDWORD);

        inline int finish_query_read(HANDLE input, int byte_count, ReadConsoleInputFn read_input)
        {
            if (byte_count != 0)
            {
                return byte_count;
            }
            if (interrupt_requested())
            {
                return -1;
            }
            // Discard non-byte console records that would keep the handle signaled.
            INPUT_RECORD record;
            DWORD read = 0;
            return read_input(input, &record, 1, &read) != 0 && read != 0 ? 0 : -1;
        }

        inline int read_query_bytes(char *out, int cap, int timeout_ms)
        {
            HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
            // Slice Windows waits so another thread's Ctrl+C flag is noticed promptly.
            constexpr DWORD SLICE_MS = 50;
            const auto want = static_cast<DWORD>(timeout_ms);
            const DWORD rc = WaitForSingleObject(hin, std::min(want, SLICE_MS));
            if (rc == WAIT_TIMEOUT)
            {
                return 0;
            }
            if (rc != WAIT_OBJECT_0)
            {
                return -1;
            }
            int n = 0;
            while (n < cap && _kbhit())
            {
                if (!read_console_byte(out[n]))
                {
                    break;
                }
                n++;
            }
            return finish_query_read(hin, n, ReadConsoleInputW);
        }
#else
        inline int read_query_bytes(char *out, int cap, int timeout_ms)
        {
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            const int pr = poll(&pfd, 1, std::min(timeout_ms, 50));
            if (pr < 0 && errno == EINTR)
            {
                // The query loop records control requests before retrying. A
                // canceled suspension can instead continue this same query.
                return 0;
            }
            if (pr < 0)
            {
                return -1;
            }
            if (pr == 0)
            {
                return 0; // the caller's deadline check turns this into the timeout
            }
            const auto got = read(STDIN_FILENO, out, static_cast<size_t>(cap));
            if (got < 0 && errno == EINTR)
            {
                return 0;
            }
            return (got <= 0) ? -1 : static_cast<int>(got);
        }
#endif
        using ReadQueryBytesFn = int (*)(char *, int, int);
    } // namespace detail

    // Bail paths reset partial escapes and leave the alternate screen.
    inline bool exit_alt_screen()
    {
        const bool output_restarted = restart_terminal_output_for_cleanup();
        const bool released = write_terminal_cleanup("\033\\\033[?1049l");
        // cppcheck-suppress knownConditionTrueFalse
        return output_restarted && released;
    }

    // DSR ends the reply batch; Framebuffer adopts the alternate screen.
    inline TermGraphics query_term_graphics(
        detail::ReadQueryBytesFn read_bytes = detail::read_query_bytes,
        detail::WriteTerminalFn write_bytes = detail::write_terminal_bytes
    )
    {
        TermGraphics tg;
        // cppcheck-suppress knownConditionTrueFalse
        if (!enable_vt_input())
        {
            tg.failed = true;
            return tg;
        }
        // Probe kitty shm end-to-end with one pixel; real-frame capacity can still fall back.
        char shm_name[64];
        std::snprintf(shm_name, sizeof shm_name, "/rasterminal-%lu-q", process_id());
        bool shm_probe = false;
        // Windows stubs make both probes false in cppcheck's multi-config scan.
        ShmFrame probe = shm_frame_open(shm_name, 3);
        // cppcheck-suppress knownConditionTrueFalse
        if (probe.valid())
        {
            const unsigned char white[3] = { 0xFF, 0xFF, 0xFF };
            shm_probe = shm_frame_append(probe, white, sizeof white);
            shm_frame_close(probe);
            // Leave a filled probe for the terminal; reclaim failed probes here.
            // cppcheck-suppress knownConditionTrueFalse
            if (!shm_probe)
            {
                shm_frame_remove(shm_name);
            }
        }

        // Keep query escapes out of scrollback.
        std::string query = "\033[?1049h";
        query += kitty::QUERY;
        // cppcheck-suppress knownConditionTrueFalse
        if (shm_probe)
        {
            kitty::append_query_shm(query, shm_name);
        }
        query += detail::QUERY_CELL_SIZE;
        query += "\033[c";
        query += detail::QUERY_SIXEL_GEOMETRY;
        query += "\033[5n";
        bool canceled = false;
        const bool flushed = std::fflush(stdout) == 0;
        if (!flushed || !write_terminal(query.data(), query.size(), true, write_bytes, &canceled))
        {
            // End any protocol string split by cancellation.
            write_terminal_cleanup("\033\\");
            if (shm_probe)
            {
                shm_frame_remove(shm_name);
            }
            tg.interrupted = canceled || (!flushed && control_requested());
            tg.failed = !tg.interrupted;
            return tg;
        }

        char buf[detail::GRAPHICS_REPLY_BUF];
        int len = 0;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(detail::GRAPHICS_QUERY_TIMEOUT_MS);
        for (;;)
        {
            // Check before each wait because Ctrl+C does not interrupt Windows waits.
            if (control_requested())
            {
                tg.interrupted = true;
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                break;
            }
            // Round up to avoid spinning during the deadline's final fraction of a millisecond.
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
            const int got = read_bytes(buf + len, detail::GRAPHICS_REPLY_BUF - len, static_cast<int>(remaining));
            if (got < 0)
            {
                break;
            }
            if (got == 0)
            {
                continue;
            }
            len += got;
            const ReplyScan r = parse_graphics_replies(buf, len, tg);
            if (r.done)
            {
                break;
            }
            if (r.consumed > 0)
            {
                len -= r.consumed;
                std::memmove(buf, buf + r.consumed, static_cast<size_t>(len));
            }
            if (len >= detail::GRAPHICS_REPLY_BUF)
            {
                break;
            }
        }

        // Drain the bounded startup backlog so partial replies cannot enter input parsing.
        for (int i = 0; i < 64; i++)
        {
            char junk[256];
            // Zero is retryable; any negative result stops the drain.
            if (read_bytes(junk, sizeof junk, 0) < 0)
            {
                break;
            }
        }

        if (shm_probe)
        {
            shm_frame_remove(shm_name);
        }
        // A t=s OK without the base query's OK names no usable backend.
        tg.kitty_shm = tg.kitty_shm && tg.kitty;
        return tg;
    }

    // Request cell pixels asynchronously; parse_input emits the eventual reply.
    inline void request_cell_size()
    {
        write_terminal(detail::QUERY_CELL_SIZE, std::strlen(detail::QUERY_CELL_SIZE), true);
    }

    // Refresh the window-dependent sixel geometry limit asynchronously.
    inline void request_sixel_geometry()
    {
        write_terminal(detail::QUERY_SIXEL_GEOMETRY, std::strlen(detail::QUERY_SIXEL_GEOMETRY), true);
    }

} // namespace platform
