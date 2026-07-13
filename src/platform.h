#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define NOMINMAX
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace platform
{

    // ─── terminal size ────────────────────────────────────────────────────────────

    inline void get_terminal_size(int &cols, int &rows)
    {
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
        cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
#else
        struct winsize ws = {};
        // The primary ioctl can fail or report ws_col==0 (some terminals and
        // multiplexers) even on a real tty; fall back across the other fds, then
        // to a sane default below.
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0)
        {
            ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
        }
        if (ws.ws_col == 0)
        {
            ioctl(STDERR_FILENO, TIOCGWINSZ, &ws);
        }
        cols = ws.ws_col > 0 ? ws.ws_col : 80; // sane fallback
        rows = ws.ws_row > 0 ? ws.ws_row : 24;
#endif
    }

    // ─── tty detection ───────────────────────────────────────────────────────────

    // True when the CRT/POSIX fd (0 = stdin, 1 = stdout) refers to a terminal.
    // Windows probes the console API rather than _isatty: _isatty is true for ANY
    // character device (NUL, COM ports), while GetConsoleMode succeeds only on a
    // real console handle. A mintty/MSYS pty is a named pipe, not a console, so it
    // is rejected too; that is intentional: interactive input (_kbhit/_getch)
    // cannot work on such a pty either, so rejecting it up front is fail-loud
    // rather than silently broken. Windows Terminal / conhost / winpty all pass.
    // _get_osfhandle (not GetStdHandle) so is_tty works on any CRT fd, not only a
    // std stream. Callers pass std fds (0/1) or freshly opened valid fds: for a
    // std fd on a no-console launch _get_osfhandle yields the -2 sentinel and
    // GetConsoleMode fails cleanly to false. (A closed/never-opened fd would trip
    // the CRT invalid-parameter handler instead, but no caller passes one.)
    // Accepted limitation: GetConsoleMode needs GENERIC_READ on the handle, so a
    // console std handle a launcher opened write-only would misreport as false.
    // Every normal shell (cmd/PowerShell/Windows Terminal/conhost) hands us
    // read+write console handles, so this does not arise in practice.
    inline bool is_tty(int fd)
    {
#ifdef _WIN32
        DWORD mode = 0;
        return GetConsoleMode(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), &mode) != 0;
#else
        return isatty(fd) != 0;
#endif
    }

    // ─── color capability ────────────────────────────────────────────────────────

    // Terminal color capability, classified from the environment at startup.
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
    } // namespace detail

    // Pure classifier over the COLORTERM/TERM env values (either may be null). All
    // comparisons are ASCII case-insensitive. Decision order:
    //   1. TERM=dumb wins over everything, even a contradictory COLORTERM: a dumb
    //      terminal cannot render escape sequences regardless of what claims color.
    //      Only the literal "dumb" is fatal, by choice: terminfo's wider non-addressable
    //      family (unknown = use=dumb+gn, etc.) is left on the 256 floor rather than
    //      fatal, since in practice such a TERM is usually a misconfig inside a real
    //      terminal that renders fine, and the policy is "never fatal except dumb".
    //   2. COLORTERM in {truecolor, 24bit} is the canonical truecolor signal.
    //   3. TERM hints (the -direct terminfo family, truecolor, 24bit) cover
    //      terminals whose users did not export COLORTERM.
    //   4. TERM unset/empty: platform default (unset_default), parameterized so
    //      both platform branches are unit-testable everywhere.
    //   5. Everything else gets the conservative 256-color floor; never fatal even
    //      for sub-256-color terminfo entries (16-color output is not supported).
    constexpr TermColor classify_term_color(const char *colorterm, const char *term, TermColor unset_default) noexcept
    {
        const bool has_term = term != nullptr && *term != '\0';
        if (has_term && detail::ieq(term, "dumb"))
        {
            return TermColor::Dumb;
        }
        if (colorterm != nullptr && (detail::ieq(colorterm, "truecolor") || detail::ieq(colorterm, "24bit")))
        {
            return TermColor::TrueColor;
        }
        if (has_term && (detail::icontains(term, "-direct") || detail::icontains(term, "truecolor") ||
                         detail::icontains(term, "24bit")))
        {
            return TermColor::TrueColor;
        }
        if (!has_term)
        {
            return unset_default;
        }
        return TermColor::Palette256;
    }

    // Env-reading wrapper around classify_term_color. The platform default applies when
    // TERM is unset/empty and COLORTERM carries no truecolor signal (see the classifier's
    // step order). Windows defaults it to truecolor (the native Windows Terminal / conhost
    // norm; both render 24-bit once VT processing is on). A Windows ssh session runs over a
    // real ConPTY console, so is_tty accepts it and it sets TERM, reaching the classifier
    // proper; a mintty/MSYS pty is a named pipe that is_tty rejects before detection runs, so
    // it never gets here. POSIX defaults to the conservative 256-color floor.
    inline TermColor detect_term_color() noexcept
    {
        // Single-threaded startup; nothing in the program calls setenv.
        const char *colorterm = std::getenv("COLORTERM"); // NOLINT(concurrency-mt-unsafe)
        const char *term = std::getenv("TERM");           // NOLINT(concurrency-mt-unsafe)
#ifdef _WIN32
        constexpr TermColor unset_default = TermColor::TrueColor;
#else
        constexpr TermColor unset_default = TermColor::Palette256;
#endif
        return classify_term_color(colorterm, term, unset_default);
    }

    // ─── raw mode ────────────────────────────────────────────────────────────────

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

    // Output-side console setup (UTF-8 + ANSI). Idempotent. Split out of enable_raw_mode
    // so it can run before any output — incl. the UTF-8 in --version — which on Windows
    // happens before the main loop and thus before raw mode would otherwise set the CP.
    // Returns whether the console accepts VT escape sequences: a legacy Windows console
    // can lack ENABLE_VIRTUAL_TERMINAL_PROCESSING, making ANSI output impossible, and
    // the render path must fail loud rather than print escape garbage. POSIX always
    // returns true (escape handling is the terminal emulator's job, not the kernel's).
    // Deliberately not [[nodiscard]]: the enable_raw_mode and --version call sites
    // legitimately ignore it; only the render path needs the verdict.
    inline bool init_console_output()
    {
#ifdef _WIN32
        // Probes GetStdHandle(STD_OUTPUT_HANDLE), like get_terminal_size/enable_mouse, whereas
        // is_tty(1) probes _get_osfhandle(1). Both reference the same console object under any
        // normal launch (divergence needs a deliberate SetStdHandle), and VT is a property of
        // that shared object, so enabling it here reaches the output fd 1 writes through.
        HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        // Probe before mutating: a failed GetConsoleMode leaves mode at 0, and ORing into that
        // would clear the console's other bits. ENABLE_PROCESSED_OUTPUT rides along because
        // Microsoft documents it as a prerequisite for the VT flag. SetConsoleMode changes
        // nothing when it fails, so either bail leaves the console exactly as found.
        if (GetConsoleMode(hout, &mode) == 0)
        {
            return false;
        }
        if (SetConsoleMode(hout, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0)
        {
            return false;
        }
        // Code page last, so a VT failure never leaves the console switched. Its return is
        // deliberately unchecked: CP 65001 ships with every supported Windows and the handle is
        // already a proven live console (the two calls above), so a failure has no realistic
        // trigger, and a false "VT ok" here would only mean the ▀ half-blocks garble rather
        // than not render. Accepted too: neither mode bit is restored on exit (Windows has no
        // disable_raw_mode counterpart), harmless since both are default-on, though a host that
        // had cleared PROCESSED_OUTPUT for raw byte output would find it left on.
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
        // Do NOT set O_NONBLOCK on stdin: in a terminal fd 0/1/2 share the same
        // open file description, so O_NONBLOCK on fd 0 also makes stdout
        // non-blocking, causing large fwrites to silently truncate.
#endif
    }

    inline void disable_raw_mode()
    {
#ifndef _WIN32
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &detail::saved_termios());
#endif
    }

    // ─── mouse support ───────────────────────────────────────────────────────────
    // Uses SGR extended mouse mode (\033[?1006h) — supported by all modern terminals
    // including Windows Terminal. Reports: scroll wheel, left-button drag.

    inline void enable_mouse()
    {
#ifdef _WIN32
        // Allow VT input sequences (including mouse) on the input handle.
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(hin, &mode);
        SetConsoleMode(hin, mode | ENABLE_VIRTUAL_TERMINAL_INPUT);
#endif
        // \033[?1006h — SGR extended format: \033[<btn;x;yM / \033[<btn;x;ym
        // \033[?1002h — button-events mode: reports motion only while a button is held
        std::fputs("\033[?1006h\033[?1002h", stdout);
        std::fflush(stdout);
    }

    inline void disable_mouse()
    {
        std::fputs("\033[?1002l\033[?1006l", stdout);
        std::fflush(stdout);
    }

    // ─── input events ────────────────────────────────────────────────────────────

    enum class Key : std::uint8_t
    {
        None = 0,
        W,
        A,
        S,
        D, // camera orbit
        Q,
        Escape, // quit
        Num1,
        Num2,
        Num3, // shading modes
        Up,
        Down, // pitch
        Left,
        Right, // yaw
        Plus,
        Minus, // zoom
        Space, // toggle auto-rotation
        B,     // toggle background colour
        L,     // cycle lighting preset
        R,     // reset camera
        C,     // cycle wireframe colour
        K,     // toggle backface culling
        T,     // toggle texture rendering
    };

    struct InputEvent
    {
        enum class Type : std::uint8_t
        {
            None,
            Key,
            ScrollUp,
            ScrollDown,
            MousePress,   // left button pressed
            MouseRelease, // left button released
            MouseMove,    // left button held and dragging
        } type = Type::None;

        Key key = Key::None; // valid when type == Key
        int btn = 0;         // mouse button (0 = left, 1 = middle, 2 = right)
        int x = 0, y = 0;    // terminal cell position (1-based)
    };

    // ─── input parsing helpers ───────────────────────────────────────────────────

    namespace detail
    {
        inline Key key_from_char(char c)
        {
            switch (c)
            {
            case 'w':
            case 'W':
                return Key::W;
            case 'a':
            case 'A':
                return Key::A;
            case 's':
            case 'S':
                return Key::S;
            case 'd':
            case 'D':
                return Key::D;
            case 'q':
            case 'Q':
                return Key::Q;
            case 27:
                return Key::Escape;
            case '1':
                return Key::Num1;
            case '2':
                return Key::Num2;
            case '3':
                return Key::Num3;
            case '+':
                return Key::Plus;
            case '-':
                return Key::Minus;
            case ' ':
                return Key::Space;
            case 'b':
            case 'B':
                return Key::B;
            case 'l':
            case 'L':
                return Key::L;
            case 'r':
            case 'R':
                return Key::R;
            case 'c':
            case 'C':
                return Key::C;
            case 'k':
            case 'K':
                return Key::K;
            case 't':
            case 'T':
                return Key::T;
            default:
                return Key::None;
            }
        }
    } // namespace detail

    // ─── poll_event ──────────────────────────────────────────────────────────────
    // Returns the next keyboard or mouse event, or InputEvent{Type::None} if the
    // input buffer is empty. Non-blocking.

    inline InputEvent poll_event()
    {
        InputEvent ev;

#ifdef _WIN32
        if (!_kbhit())
        {
            return ev;
        }
        // On Windows with ENABLE_VIRTUAL_TERMINAL_INPUT, mouse events arrive as
        // VT escape sequences readable via _getch() byte-by-byte.
        auto rb = []() -> char { return static_cast<char>(_getch()); };
        // Wait up to `ms` ms for the next byte of an escape sequence.
        // Returns 0 on timeout so callers can treat it as bare ESC.
        auto rb_timeout = [](int ms) -> char
        {
            HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
            if (WaitForSingleObject(hin, static_cast<DWORD>(ms)) != WAIT_OBJECT_0)
            {
                return 0;
            }
            return _kbhit() ? static_cast<char>(_getch()) : 0;
        };
        char c = rb();
#else
        {
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            if (poll(&pfd, 1, 0) <= 0)
            {
                return ev;
            }
        }
        auto rb = []() -> char
        {
            char b = 0;
            const ssize_t n = read(STDIN_FILENO, &b, 1);
            (void)n;
            return b;
        };
        // Like rb() but returns 0 if no byte arrives within `ms` milliseconds.
        // Used inside escape sequences to avoid blocking on a bare ESC or a
        // fragmented sequence that will never complete.
        auto rb_timeout = [](int ms) -> char
        {
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            if (poll(&pfd, 1, ms) <= 0)
            {
                return 0;
            }
            char b = 0;
            const ssize_t n = read(STDIN_FILENO, &b, 1);
            (void)n;
            return b;
        };
        const char c = rb();
        if (c == 0)
        {
            return ev;
        }
#endif

        // ── Regular character ────────────────────────────────────────────────────
        if (c != '\033')
        {
            ev.type = InputEvent::Type::Key;
            ev.key = detail::key_from_char(c);
            return ev;
        }

        // ── Escape sequence ──────────────────────────────────────────────────────
        // Use a 50 ms timeout: imperceptible to the user but long enough to cover
        // fragmented delivery over SSH.  If nothing arrives, treat \033 as bare ESC.
        const char b1 = rb_timeout(50);
        if (b1 != '[')
        {
            ev.type = InputEvent::Type::Key;
            ev.key = Key::Escape;
            return ev;
        }

        const char b2 = rb_timeout(50);
        if (b2 == 0)
        {
            ev.type = InputEvent::Type::Key;
            ev.key = Key::Escape;
            return ev;
        }

        // ── SGR mouse: \033[<btn;x;yM (press/scroll) or \033[<btn;x;ym (release) ──
        if (b2 == '<')
        {
            int nums[3] = {};
            int ni = 0;
            char fin = 0;
            for (;;)
            {
                const char d = rb_timeout(50);
                if (d == 0)
                {
                    break; // incomplete sequence — discard
                }
                if (d == ';')
                {
                    if (ni < 2)
                    {
                        ni++;
                    }
                }
                else if (d >= '0' && d <= '9')
                {
                    nums[ni] = (nums[ni] * 10) + (d - '0');
                }
                else
                {
                    fin = d; // 'M' = press/motion/scroll, 'm' = release
                    break;
                }
            }

            // Discard if sequence never completed (timeout or unknown terminator).
            if (fin != 'M' && fin != 'm')
            {
                return ev; // Type::None
            }

            const int btn = nums[0];
            ev.x = nums[1];
            ev.y = nums[2];
            ev.btn = static_cast<int>(static_cast<unsigned int>(btn) & 3U); // low 2 bits = button number

            if (btn == 64)
            {
                ev.type = InputEvent::Type::ScrollUp;
            }
            else if (btn == 65)
            {
                ev.type = InputEvent::Type::ScrollDown;
            }
            else if (btn >= 32)
            {
                ev.type = InputEvent::Type::MouseMove; // motion + button held
            }
            else if (fin == 'M')
            {
                ev.type = InputEvent::Type::MousePress;
            }
            else
            {
                ev.type = InputEvent::Type::MouseRelease;
            }
            return ev;
        }

        // ── Arrow keys: \033[A/B/C/D ────────────────────────────────────────────
        ev.type = InputEvent::Type::Key;
        switch (b2)
        {
        case 'A':
            ev.key = Key::Up;
            return ev;
        case 'B':
            ev.key = Key::Down;
            return ev;
        case 'C':
            ev.key = Key::Right;
            return ev;
        case 'D':
            ev.key = Key::Left;
            return ev;
        default:
            ev.key = Key::Escape;
            return ev;
        }
    }

} // namespace platform
