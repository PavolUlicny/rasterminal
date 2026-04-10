#pragma once

#include <cstdio>

#ifdef _WIN32
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
        // Try all three standard fds — one of them is guaranteed to be a tty.
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0)
            ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
        if (ws.ws_col == 0)
            ioctl(STDERR_FILENO, TIOCGWINSZ, &ws);
        cols = ws.ws_col > 0 ? ws.ws_col : 80; // sane fallback
        rows = ws.ws_row > 0 ? ws.ws_row : 24;
#endif
    }

    // ─── raw mode (non-blocking keyboard) ────────────────────────────────────────

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
        // Windows console: enable ANSI and UTF-8, nothing else needed for _kbhit.
        SetConsoleOutputCP(65001);
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(h, &mode);
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
        termios raw;
        tcgetattr(STDIN_FILENO, &raw);
        detail::saved_termios() = raw;

        // Disable echo and canonical (line-buffered) mode.
        // VMIN=0 / VTIME=0: read() returns immediately with 0 bytes if nothing available.
        raw.c_lflag &= ~(ECHO | ICANON);
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

    // Erase the screen and move cursor to top-left. Call once at startup.
    inline void clear_screen()
    {
        // \033[2J  — erase display
        // \033[H   — cursor to top-left
        std::fputs("\033[2J\033[H", stdout);
        std::fflush(stdout);
    }

    // ─── key codes ───────────────────────────────────────────────────────────────

    enum Key
    {
        KEY_NONE = 0,
        KEY_W,
        KEY_A,
        KEY_S,
        KEY_D, // camera movement
        KEY_Q,
        KEY_ESCAPE, // quit
        KEY_1,
        KEY_2,
        KEY_3, // shading modes
        KEY_UP,
        KEY_DOWN, // zoom / pitch
        KEY_LEFT,
        KEY_RIGHT, // yaw
        KEY_PLUS,
        KEY_MINUS, // fov
    };

    // Returns the next pressed key, or KEY_NONE if nothing is in the buffer.
    inline Key poll_key()
    {
#ifdef _WIN32
        if (!_kbhit())
            return KEY_NONE;
        int c = _getch();
        if (c == 224) // extended key prefix
        {
            c = _getch();
            switch (c)
            {
            case 72:
                return KEY_UP;
            case 80:
                return KEY_DOWN;
            case 75:
                return KEY_LEFT;
            case 77:
                return KEY_RIGHT;
            }
            return KEY_NONE;
        }
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
        case '+':
            return KEY_PLUS;
        case '-':
            return KEY_MINUS;
        }
        return KEY_NONE;

#else
        // poll() with timeout=0 checks for available input without blocking
        // and without O_NONBLOCK (which would corrupt stdout's blocking mode).
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, 0) <= 0)
            return KEY_NONE;

        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0)
            return KEY_NONE;

        // Escape sequence (arrow keys): ESC [ A/B/C/D
        if (c == '\033')
        {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0)
                return KEY_ESCAPE;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0)
                return KEY_ESCAPE;
            if (seq[0] == '[')
            {
                switch (seq[1])
                {
                case 'A':
                    return KEY_UP;
                case 'B':
                    return KEY_DOWN;
                case 'C':
                    return KEY_RIGHT;
                case 'D':
                    return KEY_LEFT;
                }
            }
            return KEY_ESCAPE;
        }

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
        case '1':
            return KEY_1;
        case '2':
            return KEY_2;
        case '3':
            return KEY_3;
        case '+':
            return KEY_PLUS;
        case '-':
            return KEY_MINUS;
        }
        return KEY_NONE;
#endif
    }

} // namespace platform
