#pragma once

#include "src/platform/input.h"
#include "src/terminal/graphics.h" // TermGraphics, filled by query_term_graphics below
#include "src/terminal/kitty.h"    // the capability query escape, sent by query_term_graphics

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <atomic>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <conio.h>
#include <io.h>
#include <windows.h>
// Windows defines near/far as empty macros with no opt-out; contain the pollution here.
#undef near
#undef far
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace platform
{

    namespace detail
    {
#ifdef _WIN32
        using InterruptFlag = std::atomic_bool;
#else
        using InterruptFlag = volatile std::sig_atomic_t;
#endif
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): shared by the control handler and main
        inline InterruptFlag interrupt_flag = {};
    } // namespace detail

    [[nodiscard]] inline bool interrupt_requested() noexcept
    {
#ifdef _WIN32
        return detail::interrupt_flag.load(std::memory_order_relaxed);
#else
        return detail::interrupt_flag != 0;
#endif
    }

    namespace detail
    {
#ifdef _WIN32
        inline BOOL WINAPI console_interrupt_handler(DWORD event) noexcept
        {
            if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
            {
                return FALSE;
            }
            interrupt_flag.store(true, std::memory_order_relaxed);
            return TRUE;
        }
#else
        inline void signal_handler(int /*signal*/) noexcept
        {
            interrupt_flag = 1;
        }
#endif
    } // namespace detail

    inline bool install_interrupt_handler() noexcept
    {
#ifdef _WIN32
        // CREATE_NEW_PROCESS_GROUP and an ignoring parent can disable Ctrl+C in
        // the child. Clear that inherited process attribute before registering.
        if (SetConsoleCtrlHandler(nullptr, FALSE) == 0)
        {
            return false;
        }
        detail::interrupt_flag.store(false, std::memory_order_relaxed);
        return SetConsoleCtrlHandler(detail::console_interrupt_handler, TRUE) != 0;
#else
        detail::interrupt_flag = 0;
        const auto interrupt_handler = std::signal(SIGINT, detail::signal_handler);
        const auto terminate_handler = std::signal(SIGTERM, detail::signal_handler);
        return interrupt_handler != SIG_ERR && terminate_handler != SIG_ERR;
#endif
    }

    // Return a 64-bit stream size and leave the stream at EOF, or -1 on failure.
    inline int64_t file_size(std::FILE *f)
    {
#ifdef _WIN32
        if (_fseeki64(f, 0, SEEK_END) != 0)
        {
            return -1;
        }
        return _ftelli64(f);
#else
        // Enforce the global large-file ABI on ILP32 at compile time.
        static_assert(sizeof(off_t) == 8, "64-bit off_t required; build with -D_FILE_OFFSET_BITS=64");
        if (fseeko(f, 0, SEEK_END) != 0)
        {
            return -1;
        }
        return ftello(f);
#endif
    }

    inline void get_terminal_size(int &cols, int &rows)
    {
#ifdef _WIN32
        // Reject failed and empty console geometry.
        CONSOLE_SCREEN_BUFFER_INFO csbi = {};
        cols = 0; // never read the caller's incoming value on the failure path
        rows = 0;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi) != 0)
        {
            cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        }
        // A console can also report an empty window rectangle; treat that as failure
        // too, exactly as the POSIX path treats ws_col == 0.
        if (cols <= 0 || rows <= 0)
        {
            cols = 80;
            rows = 24;
        }
#else
        struct winsize ws = {};
        // Some ttys omit geometry on one fd; try the others before the default.
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0)
        {
            ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
        }
        if (ws.ws_col == 0)
        {
            ioctl(STDERR_FILENO, TIOCGWINSZ, &ws);
        }
        cols = ws.ws_col > 0 ? ws.ws_col : 80;
        rows = ws.ws_row > 0 ? ws.ws_row : 24;
#endif
    }

    // Return TIOCGWINSZ pixel geometry, or 0/0 when absent.
    inline void get_terminal_pixel_size(int &px_w, int &px_h)
    {
        px_w = 0;
        px_h = 0;
#ifndef _WIN32
        struct winsize ws = {};
        // Select by grid validity so grid and pixels always come from the same tty.
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0)
        {
            ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
        }
        if (ws.ws_col == 0)
        {
            ioctl(STDERR_FILENO, TIOCGWINSZ, &ws);
        }
        // Never combine real pixels with a fabricated grid axis.
        if (ws.ws_col > 0 && ws.ws_row > 0 && ws.ws_xpixel > 0 && ws.ws_ypixel > 0)
        {
            px_w = ws.ws_xpixel;
            px_h = ws.ws_ypixel;
        }
#endif
    }

#ifndef _WIN32
    // Kitty shm frame. Linux writes to the fd; other POSIX systems fill a mapping.
    // The aggregate owns its handle but is copyable: keep it local and close it exactly once.
    struct ShmFrame
    {
        int fd = -1;
        unsigned char *map = nullptr; // non-null on the mapping fill only
        size_t size = 0;
        size_t at = 0; // bytes appended so far

        [[nodiscard]] bool valid() const noexcept { return fd >= 0 || map != nullptr; }
    };

    inline ShmFrame shm_frame_open(const char *name, size_t size)
    {
        ShmFrame f;
        shm_unlink(name);
        const int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0)
        {
            return f;
        }
        if (ftruncate(fd, static_cast<off_t>(size)) != 0)
        {
            close(fd);
            shm_unlink(name);
            return f;
        }
        f.size = size;
#ifdef __linux__
        // Linux write() reports a full /dev/shm as ENOSPC instead of SIGBUS.
        f.fd = fd;
        return f;
#else
        void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd); // the mapping keeps the object alive; the fd is not needed
        if (p == MAP_FAILED || p == nullptr)
        {
            shm_unlink(name);
            // Empty, not `f`: its size is already set, and a frame carrying a size but no
            // sink would send shm_frame_append down its write branch on fd -1.
            return {};
        }
        f.map = static_cast<unsigned char *>(p);
        return f;
#endif
    }

    // Append one chunk. False means the object could not take it (a full tmpfs), and
    // the caller abandons the frame for the direct transport.
    inline bool shm_frame_append(ShmFrame &f, const unsigned char *data, size_t len)
    {
        if (len > f.size - f.at)
        {
            return false;
        }
        if (f.map != nullptr)
        {
            std::memcpy(f.map + f.at, data, len);
            f.at += len;
            return true;
        }
        while (len > 0)
        {
            const ssize_t w = write(f.fd, data, len);
            if (w <= 0)
            {
                if (w < 0 && errno == EINTR)
                {
                    continue;
                }
                return false;
            }
            len -= static_cast<size_t>(w);
            data += static_cast<size_t>(w);
            f.at += static_cast<size_t>(w);
        }
        return true;
    }

    inline void shm_frame_close(ShmFrame &f)
    {
        if (f.map != nullptr)
        {
            munmap(f.map, f.size);
            f.map = nullptr;
        }
        if (f.fd >= 0)
        {
            close(f.fd);
            f.fd = -1;
        }
    }

    inline void shm_frame_remove(const char *name)
    {
        shm_unlink(name);
    }

    inline unsigned long process_id()
    {
        // Unsigned: PIDs are nonnegative on POSIX, and Windows' DWORD id would
        // be implementation-defined narrowed by a signed 32-bit long (LLP64).
        return static_cast<unsigned long>(getpid());
    }
#else
    // Windows stubs keep shm conditionals inside this header and select kitty direct mode.
    struct ShmFrame
    {
        [[nodiscard]] bool valid() const noexcept { return false; }
    };

    inline ShmFrame shm_frame_open(const char * /*name*/, size_t /*size*/)
    {
        return {};
    }

    inline bool shm_frame_append(ShmFrame & /*f*/, const unsigned char * /*data*/, size_t /*len*/)
    {
        return false;
    }

    inline void shm_frame_close(ShmFrame & /*f*/) {}

    inline void shm_frame_remove(const char * /*name*/) {}

    inline unsigned long process_id()
    {
        return GetCurrentProcessId();
    }
#endif

    // Enable Windows VT input so query and mouse escapes arrive as bytes.
    inline void enable_vt_input()
    {
#ifdef _WIN32
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        // Never OR flags into a mode value from a failed probe.
        if (GetConsoleMode(hin, &mode) != 0)
        {
            SetConsoleMode(hin, mode | ENABLE_VIRTUAL_TERMINAL_INPUT);
        }
#endif
    }

    namespace detail
    {
        // Query replies are small; leave headroom for incidental input.
        constexpr int GRAPHICS_REPLY_BUF = 512;
        // Shared startup and resize query forms.
        inline constexpr char QUERY_CELL_SIZE[] = "\033[16t";
        inline constexpr char QUERY_SIXEL_GEOMETRY[] = "\033[?2;1;0S";
        // Bound terminals that never answer the DSR sentinel.
        constexpr int GRAPHICS_QUERY_TIMEOUT_MS = 1000;

        // Bounded query read: positive bytes, zero to retry, negative to stop.
#ifdef _WIN32
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
                out[n++] = static_cast<char>(_getch());
            }
            if (n == 0)
            {
                // Discard non-byte console records that would keep the handle signaled.
                INPUT_RECORD rec;
                DWORD got = 0;
                if (ReadConsoleInput(hin, &rec, 1, &got) == 0 || got == 0)
                {
                    return -1;
                }
            }
            return n;
        }
#else
        inline int read_query_bytes(char *out, int cap, int timeout_ms)
        {
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            const int pr = poll(&pfd, 1, timeout_ms);
            if (pr < 0 && errno == EINTR)
            {
                if (interrupt_requested())
                {
                    return -1; // a quit signal ends the wait
                }
                // Linux stop and SIGCONT can interrupt poll without a handler; retry
                // against the caller's deadline.
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
                if (interrupt_requested())
                {
                    return -1;
                }
                return 0;
            }
            return (got <= 0) ? -1 : static_cast<int>(got);
        }
#endif
    } // namespace detail

    // Bail paths after graphics detection must leave the alternate screen here;
    // the normal path transfers ownership to Framebuffer.
    inline void exit_alt_screen()
    {
        std::fputs("\033[?1049l", stdout);
        std::fflush(stdout);
    }

    // Query graphics, cell, and sixel capabilities before the input loop starts.
    // DSR terminates the ordered reply batch. The terminal remains on the alternate screen.
    inline TermGraphics query_term_graphics()
    {
        TermGraphics tg;
        // Windows: without VT input the console never surfaces the replies as
        // bytes, and enable_mouse (the other place the flag is set) runs only
        // after the query window closes.
        enable_vt_input();
        // Keep unconsumed query escapes out of scrollback; Framebuffer adopts this screen.
        std::fputs("\033[?1049h", stdout);

        // Probe kitty shm end-to-end with one pixel; real-frame capacity can still fall back.
        char shm_name[64];
        std::snprintf(shm_name, sizeof shm_name, "/rasterminal-%lu-q", process_id());
        bool shm_probe = false;
        // On Windows, the invalid shm stub makes both probes statically false in
        // cppcheck's multi-config scan.
        ShmFrame probe = shm_frame_open(shm_name, 3);
        // cppcheck-suppress knownConditionTrueFalse
        if (probe.valid())
        {
            const unsigned char white[3] = { 0xFF, 0xFF, 0xFF };
            shm_probe = shm_frame_append(probe, white, sizeof white);
            shm_frame_close(probe);
            // A probe object that could not be filled is ours to reclaim; a filled
            // one is left for the terminal to read and unlink, which is the query.
            // cppcheck-suppress knownConditionTrueFalse
            if (!shm_probe)
            {
                shm_frame_remove(shm_name);
            }
        }

        std::fputs(kitty::QUERY, stdout);
        // cppcheck-suppress knownConditionTrueFalse
        if (shm_probe)
        {
            std::string shm_query;
            kitty::append_query_shm(shm_query, shm_name);
            std::fputs(shm_query.c_str(), stdout);
        }
        std::fputs(detail::QUERY_CELL_SIZE, stdout);
        std::fputs("\033[c", stdout);
        std::fputs(detail::QUERY_SIXEL_GEOMETRY, stdout);
        std::fputs("\033[5n", stdout);
        std::fflush(stdout);

        char buf[detail::GRAPHICS_REPLY_BUF];
        int len = 0;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(detail::GRAPHICS_QUERY_TIMEOUT_MS);
        for (;;)
        {
            // Check before each wait. Windows waits are sliced because Ctrl+C does
            // not interrupt them; POSIX may set the flag outside an EINTR path.
            if (interrupt_requested())
            {
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                break;
            }
            // ceil, not duration_cast: truncation makes the last fraction of a
            // millisecond before the deadline a zero-timeout spin (poll returns
            // instantly, the loop re-enters); rounding up waits it out instead.
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
            const int got =
                detail::read_query_bytes(buf + len, detail::GRAPHICS_REPLY_BUF - len, static_cast<int>(remaining));
            if (got < 0)
            {
                break; // quit signal, closed stream, or failure: nothing more will arrive
            }
            if (got == 0)
            {
                continue; // spurious wakeup or EINTR retry; the deadline re-check bounds it
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
                break; // a bufferful nothing could consume: stop reading, run blocks
            }
        }

        // Drain the bounded startup backlog so partial replies cannot enter input parsing.
        for (int i = 0; i < 64; i++)
        {
            char junk[256];
            // Zero may be a Windows timeout or discarded non-key record; only a
            // dead stream stops the bounded drain early.
            if (detail::read_query_bytes(junk, sizeof junk, 0) < 0)
            {
                break;
            }
        }

        if (shm_probe)
        {
            shm_frame_remove(shm_name); // normally already unlinked by the terminal
        }
        // A t=s OK without the base query's OK names no usable backend.
        tg.kitty_shm = tg.kitty_shm && tg.kitty;
        return tg;
    }

    // Request cell pixels asynchronously; parse_input emits the eventual reply.
    inline void request_cell_size()
    {
        std::fputs(detail::QUERY_CELL_SIZE, stdout);
        std::fflush(stdout);
    }

    // Refresh the window-dependent sixel geometry limit asynchronously.
    inline void request_sixel_geometry()
    {
        std::fputs(detail::QUERY_SIXEL_GEOMETRY, stdout);
        std::fflush(stdout);
    }

    // Windows requires a real console because input uses console APIs; POSIX accepts a tty.
    inline bool is_tty(int fd)
    {
#ifdef _WIN32
        DWORD mode = 0;
        return GetConsoleMode(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), &mode) != 0;
#else
        return isatty(fd) != 0;
#endif
    }

    // Terminal color capability classified once at startup.
    enum class TermColor : std::uint8_t
    {
        Dumb,       // TERM=dumb: cannot render escape sequences at all (caller fails loud)
        Palette256, // xterm-256 palette output
        TrueColor,  // 24-bit SGR output
    };

    namespace detail
    {
        // ASCII-only case folding: env values are plain ASCII, and std::tolower has
        // locale cost plus UB on negative char.
        constexpr char ascii_lower(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        constexpr bool ieq(const char *a, const char *b) noexcept
        {
            for (; *a != '\0' && *b != '\0'; ++a, ++b)
            {
                if (ascii_lower(*a) != ascii_lower(*b))
                {
                    return false;
                }
            }
            return *a == '\0' && *b == '\0';
        }

        // Case-insensitive substring scan. Naive O(n*m) is fine: inputs are short
        // env values checked once at startup.
        constexpr bool icontains(const char *hay, const char *needle) noexcept
        {
            for (;; ++hay)
            {
                const char *h = hay;
                const char *n = needle;
                while (*n != '\0' && *h != '\0' && ascii_lower(*h) == ascii_lower(*n))
                {
                    ++h;
                    ++n;
                }
                if (*n == '\0')
                {
                    return true;
                }
                if (*hay == '\0')
                {
                    return false;
                }
            }
        }

        // Case-insensitive prefix match, the anchored sibling of icontains. Used
        // where the signal is the entry FAMILY (screen, screen-256color,
        // screen.xterm-256color all lead with it), not an embedded component.
        constexpr bool istarts_with(const char *hay, const char *prefix) noexcept
        {
            for (; *prefix != '\0'; ++hay, ++prefix)
            {
                if (*hay == '\0' || ascii_lower(*hay) != ascii_lower(*prefix))
                {
                    return false;
                }
            }
            return true;
        }

        // Common TERM names that remain useful when ssh drops COLORTERM.
        inline constexpr const char *TRUECOLOR_TERMS[] = {
            "kitty", "wezterm", "alacritty", "ghostty", "foot", "contour",
        };
    } // namespace detail

    // TERM=dumb is fatal. GNU screen 4.x is capped at 256 colors because it
    // misparses truecolor SGR; tmux is exempt. Otherwise prefer COLORTERM, then
    // known TERM hints, with a conservative 256-color floor.
    constexpr TermColor classify_term_color(
        const char *colorterm, const char *term, TermColor unset_default, bool under_tmux, bool in_screen
    ) noexcept
    {
        const bool has_term = term != nullptr && *term != '\0';
        if (has_term && detail::ieq(term, "dumb"))
        {
            return TermColor::Dumb;
        }
        if (in_screen || (has_term && !under_tmux && detail::istarts_with(term, "screen")))
        {
            return TermColor::Palette256;
        }
        if (colorterm != nullptr && (detail::ieq(colorterm, "truecolor") || detail::ieq(colorterm, "24bit")))
        {
            return TermColor::TrueColor;
        }
        if (!has_term)
        {
            return unset_default;
        }
        // term is non-null and non-empty from here: the remaining checks all read it.
        if (detail::icontains(term, "-direct") || detail::icontains(term, "truecolor") ||
            detail::icontains(term, "24bit"))
        {
            return TermColor::TrueColor;
        }
        // Raw loop, not std::any_of: any_of is not constexpr until C++20.
        for (const char *name : detail::TRUECOLOR_TERMS)
        {
            if (detail::icontains(term, name))
            {
                return TermColor::TrueColor;
            }
        }
        return TermColor::Palette256;
    }

    // Read the environment; unset TERM defaults to truecolor on Windows and 256 on POSIX.
    inline TermColor detect_term_color() noexcept
    {
        // Single-threaded startup; nothing in the program calls setenv.
        const char *colorterm = std::getenv("COLORTERM"); // NOLINT(concurrency-mt-unsafe)
        const char *term = std::getenv("TERM");           // NOLINT(concurrency-mt-unsafe)
        // Empty multiplexer variables mean inactive.
        const char *tmux = std::getenv("TMUX"); // NOLINT(concurrency-mt-unsafe)
        const char *sty = std::getenv("STY");   // NOLINT(concurrency-mt-unsafe)
        const bool under_tmux = tmux != nullptr && *tmux != '\0';
        const bool in_screen = sty != nullptr && *sty != '\0';
#ifdef _WIN32
        constexpr TermColor unset_default = TermColor::TrueColor;
#else
        constexpr TermColor unset_default = TermColor::Palette256;
#endif
        return classify_term_color(colorterm, term, unset_default, under_tmux, in_screen);
    }

#ifndef _WIN32
    namespace detail
    {
        inline termios &saved_termios()
        {
            static termios t;
            return t;
        }
    } // namespace detail
#endif

    // Snapshot Windows console buffer modes and the process output code page before setup.
    // main keeps one guard alive for the whole invocation so every orderly return restores
    // the shell. Console close, logoff, and shutdown events still terminate the process.
    class ConsoleStateGuard
    {
      public:
        ConsoleStateGuard() noexcept { capture(); }

        ~ConsoleStateGuard() noexcept
        {
#ifdef _WIN32
            // Buffered output must reach the console while VT processing and the
            // UTF-8 code page are still active.
            std::fflush(stdout);
            // A destructor cannot report cleanup failures. Restore each mode through the
            // handle used to read it. Restore the code page only when output setup could
            // have changed it, after the output-mode probe succeeded.
            if (m_have_input_mode)
            {
                SetConsoleMode(m_input, m_input_mode);
            }
            if (m_have_output_mode)
            {
                SetConsoleMode(m_output, m_output_mode);
                if (m_output_cp != 0)
                {
                    SetConsoleOutputCP(m_output_cp);
                }
            }
#endif
        }

        ConsoleStateGuard(const ConsoleStateGuard &) = delete;
        ConsoleStateGuard &operator=(const ConsoleStateGuard &) = delete;
        ConsoleStateGuard(ConsoleStateGuard &&) = delete;
        ConsoleStateGuard &operator=(ConsoleStateGuard &&) = delete;

      private:
        void capture() noexcept
        {
#ifdef _WIN32
            m_input = GetStdHandle(STD_INPUT_HANDLE);
            m_output = GetStdHandle(STD_OUTPUT_HANDLE);
            m_have_input_mode = GetConsoleMode(m_input, &m_input_mode) != 0;
            m_have_output_mode = GetConsoleMode(m_output, &m_output_mode) != 0;
            m_output_cp = GetConsoleOutputCP();
#endif
        }

#ifdef _WIN32
        HANDLE m_input = INVALID_HANDLE_VALUE;
        HANDLE m_output = INVALID_HANDLE_VALUE;
        DWORD m_input_mode = 0;
        DWORD m_output_mode = 0;
        bool m_have_input_mode = false;
        bool m_have_output_mode = false;
        UINT m_output_cp = 0;
#endif
    };

    // Idempotently enable UTF-8 and VT output. POSIX terminals handle escapes themselves.
    inline bool init_console_output()
    {
#ifdef _WIN32
        HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        // Processed output is a documented prerequisite for VT output.
        if (GetConsoleMode(hout, &mode) == 0)
        {
            return false;
        }
        if (SetConsoleMode(hout, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0)
        {
            return false;
        }
        // ConsoleStateGuard uses its successful GetConsoleMode probe as a proxy
        // for code-page restoration. Keep every code-page change after that probe.
        SetConsoleOutputCP(65001);
        return true;
#else
        return true;
#endif
    }

    inline void enable_raw_mode()
    {
#ifdef _WIN32
        init_console_output();
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(hin, &mode) != 0)
        {
            // VT input must not run through cmd.exe's cooked line editor. Processed
            // input makes Ctrl+C reach the handler even when the shell left it disabled.
            SetConsoleMode(
                hin, (mode | ENABLE_PROCESSED_INPUT) & ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)
            );
        }
#else
        termios raw = {};
        tcgetattr(STDIN_FILENO, &raw);
        detail::saved_termios() = raw;

        // Disable echo and canonical (line-buffered) mode.
        // VMIN=0 / VTIME=0: read() returns immediately with 0 bytes if nothing available.
        raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        // Do not set O_NONBLOCK: tty stdio fds may share an open description.
#endif
    }

    inline void disable_raw_mode()
    {
#ifdef _WIN32
        // Mouse reports queued before tracking was disabled must not reach the shell.
        FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE));
#else
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &detail::saved_termios());
#endif
    }

    // Enable SGR mouse reports for wheel and button-drag input.

    inline void enable_mouse()
    {
        // VT input covers mouse sequences too; the graphics query normally set
        // it already, but a --graphics blocks session skips the query.
        enable_vt_input();
        // \033[?1006h: SGR extended format: \033[<btn;x;yM / \033[<btn;x;ym
        // \033[?1002h: button-events mode: reports motion only while a button is held
        std::fputs("\033[?1006h\033[?1002h", stdout);
        std::fflush(stdout);
    }

    inline void disable_mouse()
    {
        std::fputs("\033[?1002l\033[?1006l", stdout);
        std::fflush(stdout);
    }

    // Buffered, timed input reader; input.h owns the stateless grammar.

    namespace detail
    {
        // Holds key and mouse sequences; longer terminal replies use the skip path.
        constexpr int MAX_PENDING = 1024;

        // Maximum inter-byte gap, not total sequence time.
        constexpr int PARTIAL_TIMEOUT_MS = 50;

        // A stalled prefix beyond this is terminal payload, not a keypress.
        constexpr int MAX_KEY_SEQUENCE = 64;

        // Continuously measured rate floor for abandoning an unterminated skipped reply.
        // It sits above human typing but below program output.
        constexpr int RATE_WINDOW_MS = 500;
        constexpr int RATE_QUOTA = 128;

        // Carry credit across bursty windows, but bound recovery after a stream stops.
        constexpr int RATE_MAX_CARRY = 2 * RATE_QUOTA;

        // Charge elapsed windows, capped so a long pause cannot demand an arbitrary burst.
        constexpr int RATE_MAX_WINDOWS = 4;

        // Saturate before signed overflow while retaining one maximum charge plus carry.
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
                    // First tick since this meter was created or reset. Without it the
                    // window starts at the clock's epoch and the very first tick
                    // charges the maximum quota against a credit of nothing.
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

        // Fairness bound per caller drain pass; reaching it drops nothing.
        constexpr int MAX_REFILLS_PER_PASS = 1024;

        // Pending bytes persist across calls; last_growth times partial reassembly.
        struct Pending
        {
            char buf[MAX_PENDING] = {};
            int len = 0;
            std::chrono::steady_clock::time_point last_growth;
            // A long sequence keeps only its introducer and possible ST-prefix ESC;
            // skip_scan, never parse_input, consumes it to its terminator or rate floor.
            bool skipping = false;
            // Runs whether or not a skip is in progress, so a skip never has to be
            // seeded with a guess at the rate.
            ArrivalMeter meter;
            // Reads spent in the current drain pass, against MAX_REFILLS_PER_PASS.
            // Lives here rather than in poll_event because a pass spans calls: it is
            // reset when Type::None ends the caller's drain, not on entry.
            int refills = 0;
        };

        inline Pending &pending()
        {
            static Pending p;
            return p;
        }

        // Read available bytes without blocking; zero means the drain is idle or closed.
        inline int read_available(char *out, int cap)
        {
#ifdef _WIN32
            // The console API only yields one byte at a time.
            int n = 0;
            while (n < cap && _kbhit())
            {
                out[n++] = static_cast<char>(_getch());
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

        // Append one bounded read; poll_event owns the refill loop and fairness limit.
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

        // Retain only the introducer and a trailing ESC that may begin ST. Keep the
        // continuously measured arrival rate instead of seeding a guess.
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

    // Reassemble split events. Discard short stale prefixes; skip long replies to their
    // terminator or rate-floor timeout. Only UTF-8-safe 7-bit ESC forms are recognized.

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

        // Compact the small fixed buffer instead of maintaining a second offset invariant.
        auto consume = [&p](int n)
        {
            p.len -= n;
            if (p.len > 0)
            {
                std::memmove(p.buf, p.buf + n, static_cast<size_t>(p.len));
            }
        };

        // Parse buffered events before reading. Each loop consumes bytes or one bounded refill.
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
                // No terminator yet: fall through to the overflow, read and rate-floor
                // handling below, which is the same for a skip as for a partial.
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

            // A full incomplete buffer is one long ESC sequence; retain its family prefix
            // and scan future bytes for the protocol terminator.
            if (p.len >= detail::MAX_PENDING)
            {
                detail::enter_skip(p);
                continue; // the read below carries the skip on, within the pass budget
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
                continue; // re-parse: the sequence may now be whole
            }

            // One clock read for both rules below, which is also the only one on the
            // idle path: a poll that finds nothing reaches exactly here.
            const auto now = std::chrono::steady_clock::now();

            // Judge a short prefix stale only after a call that received no bytes. Long
            // payload skips use the rate floor because ordinary inter-chunk gaps are expected.
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

            // Nothing more to report this frame, which is what ends the caller's
            // drain and so releases the pass budget.
            end_input_pass();
            return InputEvent{};
        }
    }

} // namespace platform
