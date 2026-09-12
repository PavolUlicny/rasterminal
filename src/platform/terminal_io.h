#pragma once

#include "src/platform/control.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace platform
{
    namespace detail
    {
        using WriteTerminalFn = int64_t (*)(const char *, size_t);

        inline int64_t write_terminal_bytes(const char *data, size_t size) noexcept
        {
            const size_t count = std::min(size, static_cast<size_t>(INT_MAX));
#ifdef _WIN32
            return _write(_fileno(stdout), data, static_cast<unsigned>(count));
#else
            return write(STDOUT_FILENO, data, count);
#endif
        }
#ifndef _WIN32
        using TcgetattrFn = int (*)(int, termios *);
        using TcsetattrFn = int (*)(int, int, const termios *);

        inline bool set_output_flags(int flags) noexcept
        {
            int result = fcntl(STDOUT_FILENO, F_SETFL, flags);
            while (result < 0 && errno == EINTR)
            {
                result = fcntl(STDOUT_FILENO, F_SETFL, flags);
            }
            return result == 0;
        }

        // stdout may share file-status flags with the shell.
        inline int make_output_nonblocking() noexcept
        {
            int flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
            while (flags < 0 && errno == EINTR)
            {
                flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
            }
            if (flags < 0 || !set_output_flags(static_cast<int>(static_cast<unsigned>(flags) | O_NONBLOCK)))
            {
                return -1;
            }
            return flags;
        }
#endif
    } // namespace detail

    // Flush stdio first. POSIX can abandon a write; Windows cancels between writes.
    inline bool write_terminal(
        const char *data,
        size_t size,
        bool cancel_on_control = false,
        detail::WriteTerminalFn write_bytes = detail::write_terminal_bytes,
        bool *canceled = nullptr
    ) noexcept
    {
        if (canceled != nullptr)
        {
            *canceled = false;
        }
#ifndef _WIN32
        int saved_flags = -1;
        if (cancel_on_control)
        {
            // Keep the syscall nonblocking to bound the signal race.
            saved_flags = detail::make_output_nonblocking();
            if (saved_flags < 0)
            {
                return false;
            }
        }
#endif
        size_t offset = 0;
        while (offset < size)
        {
            if (cancel_on_control && control_requested())
            {
                if (canceled != nullptr)
                {
                    *canceled = true;
                }
                break;
            }
            const int64_t written = write_bytes(data + offset, size - offset);
            if (written > 0)
            {
                offset += static_cast<size_t>(written);
            }
#ifndef _WIN32
            else if (written < 0 && errno == EAGAIN && cancel_on_control)
            {
                pollfd output = { STDOUT_FILENO, POLLOUT, 0 };
                const int ready = poll(&output, 1, 50);
                if ((ready < 0 && errno != EINTR) ||
                    (ready > 0 && (static_cast<unsigned>(output.revents) &
                                   static_cast<unsigned>(POLLERR | POLLHUP | POLLNVAL)) != 0))
                {
                    break;
                }
            }
#endif
            else if (written == 0 || errno != EINTR)
            {
                break;
            }
        }
#ifndef _WIN32
        if (saved_flags >= 0 && !detail::set_output_flags(saved_flags))
        {
            if (canceled != nullptr)
            {
                *canceled = false;
            }
            return false;
        }
#endif
        return offset == size;
    }

    inline bool write_terminal(const char *text) noexcept
    {
        return write_terminal(text, std::strlen(text));
    }

    namespace detail
    {
#ifndef _WIN32
        // Termios cleanup can raise SIGTTOU even without TOSTOP.
        template <typename Operation> inline bool with_sigttou_blocked(Operation operation) noexcept
        {
            sigset_t signals = {};
            sigset_t previous = {};
            if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGTTOU) != 0 ||
                pthread_sigmask(SIG_BLOCK, &signals, &previous) != 0)
            {
                return false;
            }
            const bool completed = operation();
            const bool restored = pthread_sigmask(SIG_SETMASK, &previous, nullptr) == 0;
            return completed && restored;
        }

        inline bool write_bounded_output(const char *text) noexcept
        {
            const int saved_flags = detail::make_output_nonblocking();
            if (saved_flags < 0)
            {
                return false;
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            const size_t size = std::strlen(text);
            size_t offset = 0;
            while (offset < size && std::chrono::steady_clock::now() < deadline)
            {
                const ssize_t written = write(STDOUT_FILENO, text + offset, size - offset);
                if (written > 0)
                {
                    offset += static_cast<size_t>(written);
                }
                else if (written < 0 && errno == EAGAIN)
                {
                    const auto remaining = deadline - std::chrono::steady_clock::now();
                    if (remaining <= std::chrono::steady_clock::duration::zero())
                    {
                        break;
                    }
                    pollfd output = { STDOUT_FILENO, POLLOUT, 0 };
                    const int timeout =
                        static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
                    const int ready = poll(&output, 1, timeout);
                    if (ready == 0 || (ready < 0 && errno != EINTR) ||
                        (ready > 0 && (static_cast<unsigned>(output.revents) &
                                       static_cast<unsigned>(POLLERR | POLLHUP | POLLNVAL)) != 0))
                    {
                        break;
                    }
                }
                else if (written == 0 || errno != EINTR)
                {
                    break;
                }
            }
            const bool restored = detail::set_output_flags(saved_flags);
            return restored && offset == size;
        }
#endif
    } // namespace detail

    // Disabling IXON restarts Linux output, but macOS may still require VSTART.
#ifdef _WIN32
    inline bool restart_terminal_output_for_cleanup() noexcept
    {
        return true;
    }
#else
    inline bool restart_terminal_output_for_cleanup(
        detail::TcgetattrFn get_termios = tcgetattr, detail::TcsetattrFn set_termios = tcsetattr
    ) noexcept
    {
        return detail::with_sigttou_blocked(
            [get_termios, set_termios]()
            {
                termios captured = {};
                int result = get_termios(STDOUT_FILENO, &captured);
                while (result < 0 && errno == EINTR)
                {
                    result = get_termios(STDOUT_FILENO, &captured);
                }
                if (result < 0)
                {
                    return errno == ENOTTY;
                }
                if ((captured.c_iflag & static_cast<tcflag_t>(IXON)) == 0)
                {
                    return true;
                }

                termios running = captured;
                running.c_iflag &= ~static_cast<tcflag_t>(IXON);
                result = set_termios(STDOUT_FILENO, TCSANOW, &running);
                while (result < 0 && errno == EINTR)
                {
                    result = set_termios(STDOUT_FILENO, TCSANOW, &running);
                }
                const bool restarted = result == 0;
                result = set_termios(STDOUT_FILENO, TCSANOW, &captured);
                while (result < 0 && errno == EINTR)
                {
                    result = set_termios(STDOUT_FILENO, TCSANOW, &captured);
                }
                return restarted && result == 0;
            }
        );
    }
#endif

    // Bound POSIX cleanup writes so terminal restoration and signal handoff still run.
    inline bool write_terminal_cleanup(const char *text) noexcept
    {
#ifdef _WIN32
        return write_terminal(text);
#else
        return detail::with_sigttou_blocked([text]() { return detail::write_bounded_output(text); });
#endif
    }

    inline bool end_terminal_frame(bool (*write_cleanup)(const char *) = write_terminal_cleanup) noexcept
    {
        // ST ends a truncated kitty APC or sixel DCS; its ESC aborts a partial CSI.
        // Use it without CAN, which Ghostty 1.3.1 prints in the ground state.
        return write_cleanup("\033\\\033[?2026l\033[?7h");
    }

    inline void get_terminal_size(int &cols, int &rows)
    {
#ifdef _WIN32
        // Reject failed and empty console geometry.
        CONSOLE_SCREEN_BUFFER_INFO csbi = {};
        cols = 0;
        rows = 0;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi) != 0)
        {
            cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        }
        // Reject empty console windows like empty POSIX geometry.
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
        Dumb,       // no escape support
        Palette256, // xterm-256 palette
        TrueColor,  // 24-bit SGR
    };

    namespace detail
    {
        // Environment values here are ASCII; std::tolower also needs unsigned input.
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

        // A simple scan is enough for short startup-only environment values.
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

        // Match TERM families such as screen and screen-256color.
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

    // Cap GNU screen at 256 colors; otherwise prefer COLORTERM, then TERM.
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
        if (detail::icontains(term, "-direct") || detail::icontains(term, "truecolor") ||
            detail::icontains(term, "24bit"))
        {
            return TermColor::TrueColor;
        }
        // std::any_of is not constexpr until C++20.
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
        // Startup is single-threaded and the program never calls setenv.
        const char *colorterm = std::getenv("COLORTERM"); // NOLINT(concurrency-mt-unsafe)
        const char *term = std::getenv("TERM");           // NOLINT(concurrency-mt-unsafe)
        const char *tmux = std::getenv("TMUX");           // NOLINT(concurrency-mt-unsafe)
        const char *sty = std::getenv("STY");             // NOLINT(concurrency-mt-unsafe)
        const bool under_tmux = tmux != nullptr && *tmux != '\0';
        const bool in_screen = sty != nullptr && *sty != '\0';
#ifdef _WIN32
        constexpr TermColor unset_default = TermColor::TrueColor;
#else
        constexpr TermColor unset_default = TermColor::Palette256;
#endif
        return classify_term_color(colorterm, term, unset_default, under_tmux, in_screen);
    }

} // namespace platform
