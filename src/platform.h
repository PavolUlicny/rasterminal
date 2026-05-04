#pragma once

#include <cstdio>

#ifdef _WIN32
#define NOMINMAX
#include <conio.h>
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
        // Try all three standard fds; depending on how the process was launched,
        // any subset may be attached to a terminal.
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0)
            ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
        if (ws.ws_col == 0)
            ioctl(STDERR_FILENO, TIOCGWINSZ, &ws);
        cols = ws.ws_col > 0 ? ws.ws_col : 80; // sane fallback
        rows = ws.ws_row > 0 ? ws.ws_row : 24;
#endif
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
    }
#endif

    inline void enable_raw_mode()
    {
#ifdef _WIN32
        // Enable ANSI output and UTF-8.
        SetConsoleOutputCP(65001);
        HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(hout, &mode);
        SetConsoleMode(hout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
        termios raw;
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

    enum Key
    {
        KEY_NONE = 0,
        KEY_W,
        KEY_A,
        KEY_S,
        KEY_D, // camera orbit
        KEY_Q,
        KEY_ESCAPE, // quit
        KEY_1,
        KEY_2,
        KEY_3,
        KEY_4, // shading modes
        KEY_UP,
        KEY_DOWN, // pitch
        KEY_LEFT,
        KEY_RIGHT, // yaw
        KEY_PLUS,
        KEY_MINUS, // zoom
        KEY_SPACE, // toggle auto-rotation
        KEY_B,     // toggle background colour
        KEY_L,     // cycle lighting preset
        KEY_R,     // reset camera
        KEY_C,     // cycle wireframe colour
        KEY_K,     // toggle backface culling
        KEY_T,     // toggle texture rendering
    };

    struct InputEvent
    {
        enum class Type
        {
            None,
            Key,
            ScrollUp,
            ScrollDown,
            MousePress,   // left button pressed
            MouseRelease, // left button released
            MouseMove,    // left button held and dragging
        } type = Type::None;

        Key key = KEY_NONE; // valid when type == Key
        int btn = 0;        // mouse button (0 = left, 1 = middle, 2 = right)
        int x = 0, y = 0;   // terminal cell position (1-based)
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
                return KEY_W;
            case 'a':
            case 'A':
                return KEY_A;
            case 's':
            case 'S':
                return KEY_S;
            case 'd':
            case 'D':
                return KEY_D;
            case 'q':
            case 'Q':
                return KEY_Q;
            case 27:
                return KEY_ESCAPE;
            case '1':
                return KEY_1;
            case '2':
                return KEY_2;
            case '3':
                return KEY_3;
            case '4':
                return KEY_4;
            case '+':
                return KEY_PLUS;
            case '-':
                return KEY_MINUS;
            case ' ':
                return KEY_SPACE;
            case 'b':
            case 'B':
                return KEY_B;
            case 'l':
            case 'L':
                return KEY_L;
            case 'r':
            case 'R':
                return KEY_R;
            case 'c':
            case 'C':
                return KEY_C;
            case 'k':
            case 'K':
                return KEY_K;
            case 't':
            case 'T':
                return KEY_T;
            default:
                return KEY_NONE;
            }
        }
    }

    // ─── poll_event ──────────────────────────────────────────────────────────────
    // Returns the next keyboard or mouse event, or InputEvent{Type::None} if the
    // input buffer is empty. Non-blocking.

    inline InputEvent poll_event()
    {
        InputEvent ev;

#ifdef _WIN32
        if (!_kbhit())
            return ev;
        // On Windows with ENABLE_VIRTUAL_TERMINAL_INPUT, mouse events arrive as
        // VT escape sequences readable via _getch() byte-by-byte.
        auto rb = []() -> char
        { return static_cast<char>(_getch()); };
        // Wait up to `ms` ms for the next byte of an escape sequence.
        // Returns 0 on timeout so callers can treat it as bare ESC.
        auto rb_timeout = [](int ms) -> char
        {
            HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
            if (WaitForSingleObject(hin, static_cast<DWORD>(ms)) != WAIT_OBJECT_0)
                return 0;
            return _kbhit() ? static_cast<char>(_getch()) : 0;
        };
        char c = rb();
#else
        {
            struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
            if (poll(&pfd, 1, 0) <= 0)
                return ev;
        }
        auto rb = []() -> char
        {
            char b = 0;
            ssize_t n = read(STDIN_FILENO, &b, 1);
            (void)n;
            return b;
        };
        // Like rb() but returns 0 if no byte arrives within `ms` milliseconds.
        // Used inside escape sequences to avoid blocking on a bare ESC or a
        // fragmented sequence that will never complete.
        auto rb_timeout = [](int ms) -> char
        {
            struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
            if (poll(&pfd, 1, ms) <= 0)
                return 0;
            char b = 0;
            ssize_t n = read(STDIN_FILENO, &b, 1);
            (void)n;
            return b;
        };
        char c = rb();
        if (c == 0)
            return ev;
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
        char b1 = rb_timeout(50);
        if (b1 != '[')
        {
            ev.type = InputEvent::Type::Key;
            ev.key = KEY_ESCAPE;
            return ev;
        }

        char b2 = rb_timeout(50);
        if (b2 == 0)
        {
            ev.type = InputEvent::Type::Key;
            ev.key = KEY_ESCAPE;
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
                char d = rb_timeout(50);
                if (d == 0)
                    break; // incomplete sequence — discard
                if (d == ';')
                {
                    if (ni < 2)
                        ni++;
                }
                else if (d >= '0' && d <= '9')
                {
                    nums[ni] = nums[ni] * 10 + (d - '0');
                }
                else
                {
                    fin = d; // 'M' = press/motion/scroll, 'm' = release
                    break;
                }
            }

            // Discard if sequence never completed (timeout or unknown terminator).
            if (fin != 'M' && fin != 'm')
                return ev; // Type::None

            int btn = nums[0];
            ev.x = nums[1];
            ev.y = nums[2];
            ev.btn = btn & 3; // low 2 bits = button number

            if (btn == 64)
                ev.type = InputEvent::Type::ScrollUp;
            else if (btn == 65)
                ev.type = InputEvent::Type::ScrollDown;
            else if (btn >= 32)
                ev.type = InputEvent::Type::MouseMove; // motion + button held
            else if (fin == 'M')
                ev.type = InputEvent::Type::MousePress;
            else
                ev.type = InputEvent::Type::MouseRelease;
            return ev;
        }

        // ── Arrow keys: \033[A/B/C/D ────────────────────────────────────────────
        ev.type = InputEvent::Type::Key;
        switch (b2)
        {
        case 'A':
            ev.key = KEY_UP;
            return ev;
        case 'B':
            ev.key = KEY_DOWN;
            return ev;
        case 'C':
            ev.key = KEY_RIGHT;
            return ev;
        case 'D':
            ev.key = KEY_LEFT;
            return ev;
        default:
            ev.key = KEY_ESCAPE;
            return ev;
        }
    }

    // poll_key: convenience wrapper for callers that only care about keyboard input.
    inline Key poll_key()
    {
        InputEvent ev = poll_event();
        return (ev.type == InputEvent::Type::Key) ? ev.key : KEY_NONE;
    }

} // namespace platform
