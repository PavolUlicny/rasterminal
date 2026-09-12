#pragma once

#include "src/platform/terminal_io.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <algorithm>
#include <string>
#endif

namespace platform
{
    inline bool enable_vt_input()
    {
#ifdef _WIN32
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        return GetConsoleMode(hin, &mode) != 0 && SetConsoleMode(hin, mode | ENABLE_VIRTUAL_TERMINAL_INPUT) != 0;
#else
        return true;
#endif
    }
#ifdef _WIN32
    namespace detail
    {
        using GetConsoleModeFn = BOOL(WINAPI *)(HANDLE, LPDWORD);
        using GetConsoleOutputCPFn = UINT(WINAPI *)();
        using SetConsoleModeFn = BOOL(WINAPI *)(HANDLE, DWORD);
        using DiscardPendingInputFn = bool (*)() noexcept;

        inline bool discard_pending_standard_input() noexcept;
    } // namespace detail
#else
    namespace detail
    {
        using TcflushFn = int (*)(int, int);
    } // namespace detail
#endif

    // Windows cannot restore console state after logoff or shutdown.
    class ConsoleStateGuard
    {
      public:
        ConsoleStateGuard() noexcept
        {
#ifdef _WIN32
            capture(GetConsoleMode, GetConsoleOutputCP);
#else
            capture();
#endif
        }

#ifdef _WIN32
        ConsoleStateGuard(
            detail::GetConsoleModeFn get_mode,
            detail::GetConsoleOutputCPFn get_output_cp,
            detail::SetConsoleModeFn set_mode = SetConsoleMode,
            detail::DiscardPendingInputFn discard_pending_input = detail::discard_pending_standard_input
        ) noexcept
            : m_set_mode(set_mode), m_discard_pending_input(discard_pending_input)
        {
            capture(get_mode, get_output_cp);
        }
#else
        ConsoleStateGuard(
            detail::TcgetattrFn get_termios, detail::TcsetattrFn set_termios, detail::TcflushFn flush_input
        ) noexcept
            : m_get_termios(get_termios), m_set_termios(set_termios), m_flush_input(flush_input)
        {
            capture();
        }
#endif

        [[nodiscard]] bool valid() const noexcept { return m_snapshot_complete; }

        ~ConsoleStateGuard() noexcept
        {
#ifdef _WIN32
            if (!m_snapshot_complete)
            {
                return;
            }
            // Flush before restoring VT processing and the output code page.
            std::fflush(stdout);
            // Retry cleanup that disable_raw_mode could not finish.
            if (m_input_cleanup_pending)
            {
                retry_input_cleanup();
            }
            else
            {
                restore_input_mode();
            }
            SetConsoleMode(m_output, m_output_mode);
            SetConsoleOutputCP(m_output_cp);
#else
            if (m_restore_pending && !restore_raw_mode())
            {
                std::fputs("rasterminal: failed to restore terminal input mode\n", stderr);
            }
#endif
        }

#ifdef _WIN32
        bool restore_input_mode() noexcept
        {
            if (!m_snapshot_complete)
            {
                return false;
            }
            if (!m_input_mode_pending)
            {
                return true;
            }
            if (m_set_mode(m_input, m_input_mode) == 0)
            {
                return false;
            }
            m_input_mode_pending = false;
            return true;
        }

        void arm_input_restore() noexcept { m_input_mode_pending = m_snapshot_complete; }

        bool cleanup_input() noexcept
        {
            if (!m_snapshot_complete)
            {
                return false;
            }
            m_input_cleanup_pending = true;
            return retry_input_cleanup();
        }
#else
        bool enable_raw_mode() noexcept
        {
            if (!m_snapshot_complete)
            {
                return false;
            }

            termios raw = m_input_mode;
            // VMIN=0 and VTIME=0 make idle reads return without changing file flags.
            raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;

            // tcsetattr can change settings before it reports failure.
            m_restore_pending = true;
            if (set_input_mode(raw))
            {
                termios applied = {};
                if (read_input_mode(applied) && input_settings_match(applied, raw))
                {
                    return true;
                }
            }
            restore_raw_mode_once();
            return false;
        }

        bool restore_raw_mode() noexcept
        {
            if (!m_snapshot_complete)
            {
                return false;
            }
            if (!m_restore_pending)
            {
                return true;
            }
            if (restore_raw_mode_once())
            {
                return true;
            }
            // Retry once because normal teardown has already sent escape cleanup.
            return restore_raw_mode_once();
        }

        bool refresh_input_mode() noexcept
        {
            if (!m_snapshot_complete || m_restore_pending)
            {
                return false;
            }
            termios current = {};
            if (!read_input_mode(current))
            {
                return false;
            }
            m_input_mode = current;
            return true;
        }

        // cppcheck-suppress unusedFunction
        [[nodiscard]] bool raw_mode_restore_pending() const noexcept { return m_restore_pending; }
#endif

        ConsoleStateGuard(const ConsoleStateGuard &) = delete;
        ConsoleStateGuard &operator=(const ConsoleStateGuard &) = delete;
        ConsoleStateGuard(ConsoleStateGuard &&) = delete;
        ConsoleStateGuard &operator=(ConsoleStateGuard &&) = delete;

      private:
#ifdef _WIN32
        bool retry_input_cleanup() noexcept;

        void capture(detail::GetConsoleModeFn get_mode, detail::GetConsoleOutputCPFn get_output_cp) noexcept
        {
            m_snapshot_complete = false;
            const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
            const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD input_mode = 0;
            DWORD output_mode = 0;
            if (get_mode(input, &input_mode) == 0 || get_mode(output, &output_mode) == 0)
            {
                return;
            }
            const UINT output_cp = get_output_cp();
            if (output_cp == 0)
            {
                return;
            }

            m_input = input;
            m_output = output;
            m_input_mode = input_mode;
            m_output_mode = output_mode;
            m_output_cp = output_cp;
            m_input_mode_pending = true;
            m_snapshot_complete = true;
        }

        HANDLE m_input = INVALID_HANDLE_VALUE;
        HANDLE m_output = INVALID_HANDLE_VALUE;
        DWORD m_input_mode = 0;
        DWORD m_output_mode = 0;
        bool m_input_mode_pending = false;
        bool m_input_cleanup_pending = false;
        UINT m_output_cp = 0;
        detail::SetConsoleModeFn m_set_mode = SetConsoleMode;
        detail::DiscardPendingInputFn m_discard_pending_input = detail::discard_pending_standard_input;
#else
        bool read_input_mode(termios &mode) noexcept
        {
            int result = m_get_termios(STDIN_FILENO, &mode);
            while (result != 0 && errno == EINTR)
            {
                result = m_get_termios(STDIN_FILENO, &mode);
            }
            return result == 0;
        }

        void capture() noexcept { m_snapshot_complete = read_input_mode(m_input_mode); }

        static bool input_settings_match(const termios &actual, const termios &expected) noexcept
        {
            return (actual.c_lflag & static_cast<tcflag_t>(ECHO | ICANON)) ==
                       (expected.c_lflag & static_cast<tcflag_t>(ECHO | ICANON)) &&
                   actual.c_cc[VMIN] == expected.c_cc[VMIN] && actual.c_cc[VTIME] == expected.c_cc[VTIME];
        }

        static tcflag_t persistent_local_flags(tcflag_t flags) noexcept
        {
#ifdef PENDIN
            // Flushing input can clear PENDIN, notably on macOS.
            flags &= ~static_cast<tcflag_t>(PENDIN);
#endif
            return flags;
        }

        static bool terminal_settings_match(const termios &actual, const termios &expected) noexcept
        {
            return actual.c_iflag == expected.c_iflag && actual.c_oflag == expected.c_oflag &&
                   actual.c_cflag == expected.c_cflag &&
                   persistent_local_flags(actual.c_lflag) == persistent_local_flags(expected.c_lflag) &&
                   std::memcmp(actual.c_cc, expected.c_cc, sizeof actual.c_cc) == 0 &&
                   cfgetispeed(&actual) == cfgetispeed(&expected) && cfgetospeed(&actual) == cfgetospeed(&expected);
        }

        bool restore_raw_mode_once() noexcept
        {
            return detail::with_sigttou_blocked(
                [this]()
                {
                    if (!set_input_mode(m_input_mode))
                    {
                        return false;
                    }
                    termios restored = {};
                    if (!read_input_mode(restored) || !terminal_settings_match(restored, m_input_mode))
                    {
                        return false;
                    }
                    m_restore_pending = false;
                    return true;
                }
            );
        }

        bool set_input_mode(const termios &mode) noexcept
        {
            // TCSAFLUSH can block under flow control, so flush input separately.
            int result = m_set_termios(STDIN_FILENO, TCSANOW, &mode);
            while (result != 0 && errno == EINTR)
            {
                result = m_set_termios(STDIN_FILENO, TCSANOW, &mode);
            }
            if (result != 0)
            {
                return false;
            }
            result = m_flush_input(STDIN_FILENO, TCIFLUSH);
            while (result != 0 && errno == EINTR)
            {
                result = m_flush_input(STDIN_FILENO, TCIFLUSH);
            }
            return result == 0;
        }

        termios m_input_mode = {};
        bool m_restore_pending = false;
        detail::TcgetattrFn m_get_termios = tcgetattr;
        detail::TcsetattrFn m_set_termios = tcsetattr;
        detail::TcflushFn m_flush_input = tcflush;
#endif
        bool m_snapshot_complete = false;
    };

#ifdef _WIN32
    namespace detail
    {
        inline BOOL WINAPI version_output_control_handler(DWORD event) noexcept
        {
            return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
        }

        inline bool write_utf8_console(HANDLE output, const char *text, size_t byte_count)
        {
            if (byte_count == 0)
            {
                return true;
            }
            if (byte_count > static_cast<size_t>(INT_MAX))
            {
                return false;
            }
            const int input_count = static_cast<int>(byte_count);
            const int wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, input_count, nullptr, 0);
            if (wide_count <= 0)
            {
                return false;
            }
            std::wstring wide(static_cast<size_t>(wide_count), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, input_count, wide.data(), wide_count) !=
                wide_count)
            {
                return false;
            }

            std::wstring console_text;
            console_text.reserve(wide.size() + static_cast<size_t>(std::count(wide.begin(), wide.end(), L'\n')));
            for (const wchar_t c : wide)
            {
                if (c == L'\n' && (console_text.empty() || console_text.back() != L'\r'))
                {
                    console_text.push_back(L'\r');
                }
                console_text.push_back(c);
            }

            DWORD mode = 0;
            if (GetConsoleMode(output, &mode) == 0)
            {
                return false;
            }
            const bool change_mode = (mode & ENABLE_PROCESSED_OUTPUT) == 0;
            if (change_mode && SetConsoleCtrlHandler(version_output_control_handler, TRUE) == 0)
            {
                return false;
            }
            if (change_mode && SetConsoleMode(output, mode | ENABLE_PROCESSED_OUTPUT) == 0)
            {
                SetConsoleCtrlHandler(version_output_control_handler, FALSE);
                return false;
            }

            DWORD written = 0;
            const bool write_ok =
                WriteConsoleW(
                    output, console_text.data(), static_cast<DWORD>(console_text.size()), &written, nullptr
                ) != 0;
            if (change_mode)
            {
                SetConsoleMode(output, mode);
                SetConsoleCtrlHandler(version_output_control_handler, FALSE);
            }
            return write_ok;
        }
    } // namespace detail
#endif

    // Write Unicode through the Windows console API; keep UTF-8 for files and pipes.
    inline void write_utf8_stdout(const char *text)
    {
#ifdef _WIN32
        std::fflush(stdout);
        const intptr_t raw_output = _get_osfhandle(_fileno(stdout));
        if (raw_output != -1 &&
            detail::write_utf8_console(reinterpret_cast<HANDLE>(raw_output), text, std::strlen(text)))
        {
            return;
        }
#endif
        std::fputs(text, stdout);
    }

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
        // ConsoleStateGuard captured the old code page before this call.
        return SetConsoleOutputCP(65001) != 0;
#else
        return true;
#endif
    }

    inline bool enable_raw_mode(ConsoleStateGuard *console_state = nullptr)
    {
#ifdef _WIN32
        if (!init_console_output())
        {
            return false;
        }
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        // Disable cooked input so VT escapes reach the byte reader.
        if (GetConsoleMode(hin, &mode) == 0 ||
            SetConsoleMode(
                hin, (mode | ENABLE_PROCESSED_INPUT) & ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)
            ) == 0)
        {
            return false;
        }
        if (console_state != nullptr)
        {
            console_state->arm_input_restore();
        }
        detail::arm_console_input_wake();
        return true;
#else
        // Do not set O_NONBLOCK: tty stdio fds may share an open description.
        return console_state != nullptr && console_state->enable_raw_mode();
#endif
    }

#ifdef _WIN32
    namespace detail
    {
        inline bool drain_console_input_records_nowait(HANDLE input, DWORD remaining) noexcept
        {
            using ReadConsoleInputExWFn = BOOL(WINAPI *)(HANDLE, PINPUT_RECORD, DWORD, LPDWORD, USHORT);
            const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
            const FARPROC address = kernel32 == nullptr ? nullptr : GetProcAddress(kernel32, "ReadConsoleInputExW");
            static_assert(sizeof address == sizeof(ReadConsoleInputExWFn));
            ReadConsoleInputExWFn read_nowait = nullptr;
            std::memcpy(&read_nowait, &address, sizeof read_nowait);
            if (read_nowait == nullptr)
            {
                return false;
            }

            INPUT_RECORD records[64];
            while (remaining > 0)
            {
                constexpr DWORD capacity = static_cast<DWORD>(sizeof records / sizeof *records);
                const DWORD requested = std::min(remaining, capacity);
                DWORD read = 0;
                constexpr USHORT read_nowait_flag = 0x0002;
                if (read_nowait(input, records, requested, &read, read_nowait_flag) == 0)
                {
                    return false;
                }
                // NOWAIT avoids blocking if another reader consumes the snapshot.
                if (read == 0)
                {
                    return true;
                }
                remaining -= read;
            }
            return true;
        }

        inline bool drain_console_input_snapshot(HANDLE input) noexcept
        {
            DWORD pending_records = 0;
            return GetNumberOfConsoleInputEvents(input, &pending_records) != 0 &&
                   drain_console_input_records_nowait(input, pending_records);
        }

        inline bool discard_pending_console_input(HANDLE standard_input, HANDLE writable_input) noexcept
        {
            if (writable_input == INVALID_HANDLE_VALUE)
            {
                // GENERIC_READ is enough for ReadConsoleInputEx to remove records.
                return drain_console_input_snapshot(standard_input);
            }
            return FlushConsoleInputBuffer(writable_input) != 0 || drain_console_input_snapshot(writable_input);
        }

        inline bool discard_pending_standard_input() noexcept
        {
            HANDLE input = CreateFileW(
                L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                nullptr
            );
            const bool discarded = discard_pending_console_input(GetStdHandle(STD_INPUT_HANDLE), input);
            if (input != INVALID_HANDLE_VALUE)
            {
                CloseHandle(input);
            }
            return discarded;
        }
    } // namespace detail

    inline bool ConsoleStateGuard::retry_input_cleanup() noexcept
    {
        if (!restore_input_mode())
        {
            return false;
        }
        if (!m_discard_pending_input())
        {
            return false;
        }
        m_input_cleanup_pending = false;
        return true;
    }
#endif

    inline bool disable_raw_mode(ConsoleStateGuard *console_state = nullptr)
    {
#ifdef _WIN32
        detail::disarm_console_input_wake();
        if (console_state != nullptr)
        {
            // Leave failed cleanup armed for the guard's destructor.
            return console_state->cleanup_input();
        }
        return detail::discard_pending_standard_input();
#else
        return console_state != nullptr && console_state->restore_raw_mode();
#endif
    }

    inline bool resume_raw_mode(ConsoleStateGuard &console_state)
    {
#ifdef _WIN32
        return enable_raw_mode(&console_state);
#else
        return console_state.refresh_input_mode() && console_state.enable_raw_mode();
#endif
    }

    inline bool enable_mouse(bool cancel_on_control = false, bool *canceled = nullptr)
    {
        if (canceled != nullptr)
        {
            *canceled = false;
        }
        // cppcheck-suppress knownConditionTrueFalse
        if (!enable_vt_input())
        {
            return false;
        }
        // Use SGR encoding and report motion only while a button is held.
        constexpr char setup[] = "\033[?1006h\033[?1002h";
        return write_terminal(setup, sizeof setup - 1, cancel_on_control, detail::write_terminal_bytes, canceled);
    }

    inline bool disable_mouse(bool (*write_cleanup)(const char *) = write_terminal_cleanup)
    {
        // End partial escapes before mouse resets reach the shell screen.
        const bool output_restarted = restart_terminal_output_for_cleanup();
        const bool released = write_cleanup("\033\\\033[?1002l\033[?1006l");
        // cppcheck-suppress knownConditionTrueFalse
        return output_restarted && released;
    }

} // namespace platform
