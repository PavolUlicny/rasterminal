#include "tests/test.h"
#include "src/args.h"
#include "src/platform/input.h"
#include "src/platform/platform.h"
#include "src/version.h"

// <stdlib.h> declares the POSIX pty and cross-platform environment functions in
// the global namespace; <cstdlib> need not.
#include <stdlib.h> // NOLINT(modernize-deprecated-headers,hicpp-deprecated-headers)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
// platform.h applies NOMINMAX and removes the near/far macros before this direct include.
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
    // Short TermColor aliases keep each classifier assertion on one line
    // (ASSERT_EQ handles enum classes directly via testing::assert_str).
    constexpr platform::TermColor DUMB = platform::TermColor::Dumb;
    constexpr platform::TermColor P256 = platform::TermColor::Palette256;
    constexpr platform::TermColor TC = platform::TermColor::TrueColor;

    // Runtime tests cannot prove constexpr support. These assertions cover every
    // classifier branch and its three string helpers during constant evaluation.
    static_assert(
        platform::classify_term_color(nullptr, "xterm-direct", platform::TermColor::Palette256, false, false) ==
            platform::TermColor::TrueColor,
        "classify_term_color / ieq / istarts_with / icontains must remain constexpr-evaluable"
    );
    static_assert(
        platform::classify_term_color("truecolor", "xterm-256color", platform::TermColor::Palette256, false, false) ==
            platform::TermColor::TrueColor,
        "classify_term_color COLORTERM branch must remain constexpr-evaluable"
    );
    static_assert(
        platform::classify_term_color(nullptr, "xterm-kitty", platform::TermColor::Palette256, false, false) ==
            platform::TermColor::TrueColor,
        "classify_term_color TRUECOLOR_TERMS loop must remain constexpr-evaluable"
    );
    static_assert(
        platform::classify_term_color("truecolor", "screen", platform::TermColor::Palette256, false, false) ==
            platform::TermColor::Palette256,
        "classify_term_color screen floor branch must keep its compile-time result"
    );

    // Close through portable test_close even when an assertion aborts the case.
    struct ScopedFd
    {
        int fd;
        explicit ScopedFd(int f) : fd(f) {}
        ~ScopedFd()
        {
            if (fd >= 0)
            {
                test_close(fd);
            }
        }
        ScopedFd(const ScopedFd &) = delete;
        ScopedFd &operator=(const ScopedFd &) = delete;
        ScopedFd(ScopedFd &&) = delete;
        ScopedFd &operator=(ScopedFd &&) = delete;
    };

#ifdef _WIN32
    struct ScopedStdoutCapture
    {
        std::FILE *capture = nullptr;
        int saved = -1;
        bool valid = false;

        ScopedStdoutCapture()
        {
            std::fflush(stdout);
            capture = std::tmpfile();
            saved = test_dup(TEST_STDOUT);
            if (capture != nullptr && saved >= 0)
            {
                valid = test_dup2(test_fileno(capture), TEST_STDOUT) >= 0;
            }
        }

        ~ScopedStdoutCapture()
        {
            std::fflush(stdout);
            if (saved >= 0)
            {
                test_dup2(saved, TEST_STDOUT);
                test_close(saved);
            }
            if (capture != nullptr)
            {
                std::fclose(capture);
            }
        }

        [[nodiscard]] std::string read() const
        {
            std::fflush(stdout);
            if (capture == nullptr || std::fseek(capture, 0, SEEK_SET) != 0)
            {
                return {};
            }
            std::string text;
            char buffer[512];
            size_t count = 0;
            while ((count = std::fread(buffer, 1, sizeof buffer, capture)) > 0)
            {
                text.append(buffer, count);
            }
            return text;
        }

        ScopedStdoutCapture(const ScopedStdoutCapture &) = delete;
        ScopedStdoutCapture &operator=(const ScopedStdoutCapture &) = delete;
        ScopedStdoutCapture(ScopedStdoutCapture &&) = delete;
        ScopedStdoutCapture &operator=(ScopedStdoutCapture &&) = delete;
    };

    struct ScopedStdoutHandle
    {
        int saved = -1;
        bool valid = false;

        explicit ScopedStdoutHandle(HANDLE output)
        {
            std::fflush(stdout);
            HANDLE duplicate = INVALID_HANDLE_VALUE;
            if (DuplicateHandle(
                    GetCurrentProcess(), output, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS
                ) == 0)
            {
                return;
            }
            const int output_fd = _open_osfhandle(reinterpret_cast<intptr_t>(duplicate), _O_WRONLY | _O_TEXT);
            if (output_fd < 0)
            {
                CloseHandle(duplicate);
                return;
            }
            saved = test_dup(TEST_STDOUT);
            valid = saved >= 0 && test_dup2(output_fd, TEST_STDOUT) >= 0;
            test_close(output_fd);
        }

        ~ScopedStdoutHandle()
        {
            std::fflush(stdout);
            if (saved >= 0)
            {
                test_dup2(saved, TEST_STDOUT);
                test_close(saved);
            }
        }

        ScopedStdoutHandle(const ScopedStdoutHandle &) = delete;
        ScopedStdoutHandle &operator=(const ScopedStdoutHandle &) = delete;
        ScopedStdoutHandle(ScopedStdoutHandle &&) = delete;
        ScopedStdoutHandle &operator=(ScopedStdoutHandle &&) = delete;
    };

    struct ScopedWindowsStandardInput
    {
        HANDLE saved = GetStdHandle(STD_INPUT_HANDLE);
        bool valid = false;

        explicit ScopedWindowsStandardInput(HANDLE input) { valid = SetStdHandle(STD_INPUT_HANDLE, input) != 0; }
        ~ScopedWindowsStandardInput() { SetStdHandle(STD_INPUT_HANDLE, saved); }

        ScopedWindowsStandardInput(const ScopedWindowsStandardInput &) = delete;
        ScopedWindowsStandardInput &operator=(const ScopedWindowsStandardInput &) = delete;
        ScopedWindowsStandardInput(ScopedWindowsStandardInput &&) = delete;
        ScopedWindowsStandardInput &operator=(ScopedWindowsStandardInput &&) = delete;
    };

    // CTest may replace the Win32 standard handles with pipes. Attach a console when
    // needed and point the platform helpers at CONIN$/CONOUT$ for lifecycle tests.
    // Windows CI must provide a console or permit AllocConsole; the test harness
    // cannot skip these tests if neither path works.
    struct ScopedWindowsConsole
    {
        HANDLE saved_input = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE saved_output = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE saved_error = GetStdHandle(STD_ERROR_HANDLE);
        HANDLE input = INVALID_HANDLE_VALUE;
        HANDLE output = INVALID_HANDLE_VALUE;
        DWORD saved_input_mode = 0;
        DWORD saved_output_mode = 0;
        UINT saved_output_cp = 0;
        bool allocated = false;
        bool have_input_mode = false;
        bool have_output_mode = false;
        bool restore_state = true;
        bool valid = false;

        explicit ScopedWindowsConsole(bool replace_standard_input = true)
        {
            open_handles();
            // A partial open means a console is already attached, so AllocConsole
            // cannot replace it. Allocate only when neither console device opened.
            if (input == INVALID_HANDLE_VALUE && output == INVALID_HANDLE_VALUE && AllocConsole() != 0)
            {
                allocated = true;
                close_handles();
                open_handles();
            }
            have_input_mode = input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &saved_input_mode) != 0;
            have_output_mode = output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &saved_output_mode) != 0;
            valid = have_input_mode && have_output_mode;
            if (valid)
            {
                saved_output_cp = GetConsoleOutputCP();
                valid = saved_output_cp != 0 &&
                        (!replace_standard_input || SetStdHandle(STD_INPUT_HANDLE, input) != 0) &&
                        SetStdHandle(STD_OUTPUT_HANDLE, output) != 0;
            }
        }

        ~ScopedWindowsConsole()
        {
            if (restore_state && have_input_mode)
            {
                SetConsoleMode(input, saved_input_mode);
            }
            if (restore_state && have_output_mode)
            {
                SetConsoleMode(output, saved_output_mode);
            }
            if (restore_state && saved_output_cp != 0)
            {
                SetConsoleOutputCP(saved_output_cp);
            }
            SetStdHandle(STD_INPUT_HANDLE, saved_input);
            SetStdHandle(STD_OUTPUT_HANDLE, saved_output);
            SetStdHandle(STD_ERROR_HANDLE, saved_error);
            close_handles();
            if (allocated)
            {
                FreeConsole();
            }
        }

        ScopedWindowsConsole(const ScopedWindowsConsole &) = delete;
        ScopedWindowsConsole &operator=(const ScopedWindowsConsole &) = delete;
        ScopedWindowsConsole(ScopedWindowsConsole &&) = delete;
        ScopedWindowsConsole &operator=(ScopedWindowsConsole &&) = delete;

      private:
        void close_handles()
        {
            if (input != INVALID_HANDLE_VALUE)
            {
                CloseHandle(input);
                input = INVALID_HANDLE_VALUE;
            }
            if (output != INVALID_HANDLE_VALUE)
            {
                CloseHandle(output);
                output = INVALID_HANDLE_VALUE;
            }
        }

        void open_handles()
        {
            input = CreateFileA(
                "CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                nullptr
            );
            output = CreateFileA(
                "CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                nullptr
            );
        }
    };

    struct ScopedWindowsPipes
    {
        HANDLE saved_input = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE saved_output = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE input_read = INVALID_HANDLE_VALUE;
        HANDLE input_write = INVALID_HANDLE_VALUE;
        HANDLE output_read = INVALID_HANDLE_VALUE;
        HANDLE output_write = INVALID_HANDLE_VALUE;
        bool valid = false;

        ScopedWindowsPipes()
        {
            valid = CreatePipe(&input_read, &input_write, nullptr, 0) != 0 &&
                    CreatePipe(&output_read, &output_write, nullptr, 0) != 0 &&
                    SetStdHandle(STD_INPUT_HANDLE, input_read) != 0 &&
                    SetStdHandle(STD_OUTPUT_HANDLE, output_write) != 0;
        }

        ~ScopedWindowsPipes()
        {
            SetStdHandle(STD_INPUT_HANDLE, saved_input);
            SetStdHandle(STD_OUTPUT_HANDLE, saved_output);
            close_handle(input_read);
            close_handle(input_write);
            close_handle(output_read);
            close_handle(output_write);
        }

        ScopedWindowsPipes(const ScopedWindowsPipes &) = delete;
        ScopedWindowsPipes &operator=(const ScopedWindowsPipes &) = delete;
        ScopedWindowsPipes(ScopedWindowsPipes &&) = delete;
        ScopedWindowsPipes &operator=(ScopedWindowsPipes &&) = delete;

      private:
        static void close_handle(HANDLE handle)
        {
            if (handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
            }
        }
    };

    struct ScopedWindowsHandle
    {
        HANDLE value = nullptr;

        ScopedWindowsHandle() = default;
        explicit ScopedWindowsHandle(HANDLE handle) : value(handle) {}
        ~ScopedWindowsHandle()
        {
            if (value != nullptr && value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
            }
        }

        ScopedWindowsHandle(const ScopedWindowsHandle &) = delete;
        ScopedWindowsHandle &operator=(const ScopedWindowsHandle &) = delete;
        ScopedWindowsHandle(ScopedWindowsHandle &&) = delete;
        ScopedWindowsHandle &operator=(ScopedWindowsHandle &&) = delete;
    };

    bool executable_path(std::wstring &path)
    {
        std::vector<wchar_t> buffer(32768);
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length == buffer.size())
        {
            return false;
        }
        path.assign(buffer.data(), length);
        return true;
    }

    bool launch_process(const std::wstring &command, DWORD flags, bool inherit_handles, PROCESS_INFORMATION &process)
    {
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup = {};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        return CreateProcessW(
                   nullptr, mutable_command.data(), nullptr, nullptr, inherit_handles ? TRUE : FALSE, flags, nullptr,
                   nullptr, &startup, &process
               ) != 0;
    }

    bool wait_for_clean_exit(PROCESS_INFORMATION &process, DWORD timeout_ms)
    {
        const DWORD wait = WaitForSingleObject(process.hProcess, timeout_ms);
        if (wait != WAIT_OBJECT_0)
        {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
            return false;
        }
        DWORD exit_code = 1;
        if (GetExitCodeProcess(process.hProcess, &exit_code) == 0)
        {
            return false;
        }
        if (exit_code != 0)
        {
            std::fprintf(
                stderr, "Windows console helper exited with code %lu\n", static_cast<unsigned long>(exit_code)
            );
            return false;
        }
        return true;
    }

    bool console_modes_equal(HANDLE input, HANDLE output, DWORD input_mode, DWORD output_mode, UINT output_cp)
    {
        DWORD actual_input = 0;
        DWORD actual_output = 0;
        return GetConsoleMode(input, &actual_input) != 0 && GetConsoleMode(output, &actual_output) != 0 &&
               actual_input == input_mode && actual_output == output_mode && GetConsoleOutputCP() == output_cp;
    }

    bool queue_console_bytes(HANDLE input, const wchar_t *bytes, size_t count)
    {
        std::vector<INPUT_RECORD> records(count);
        for (size_t i = 0; i < count; i++)
        {
            records[i].EventType = KEY_EVENT;
            records[i].Event.KeyEvent.bKeyDown = TRUE;
            records[i].Event.KeyEvent.wRepeatCount = 1;
            records[i].Event.KeyEvent.uChar.UnicodeChar = bytes[i];
        }
        DWORD written = 0;
        const auto record_count = static_cast<DWORD>(records.size());
        return WriteConsoleInputW(input, records.data(), record_count, &written) != 0 && written == record_count;
    }

    unsigned &query_record_read_call_count()
    {
        static unsigned calls = 0;
        return calls;
    }

    BOOL WINAPI count_query_record_read(HANDLE, PINPUT_RECORD, DWORD, LPDWORD read)
    {
        query_record_read_call_count()++;
        *read = 1;
        return TRUE;
    }

    std::atomic_uint &cancel_console_input_call_count()
    {
        static std::atomic_uint calls = 0;
        return calls;
    }

    BOOL WINAPI cancel_console_input_after_miss(HANDLE /*thread*/)
    {
        if (cancel_console_input_call_count().fetch_add(1) == 0)
        {
            SetLastError(ERROR_NOT_FOUND);
            return FALSE;
        }
        return TRUE;
    }

    int &console_mode_probe_count()
    {
        static int count = 0;
        return count;
    }

    int &console_mode_probe_failure()
    {
        static int failure = 0;
        return failure;
    }

    bool &console_output_cp_probe_failure()
    {
        static bool failure = false;
        return failure;
    }

    BOOL WINAPI probe_console_mode(HANDLE /*console*/, LPDWORD mode)
    {
        console_mode_probe_count()++;
        if (console_mode_probe_count() == console_mode_probe_failure())
        {
            return FALSE;
        }
        *mode = 0;
        return TRUE;
    }

    UINT WINAPI probe_console_output_cp()
    {
        return console_output_cp_probe_failure() ? 0U : 437U;
    }

    int &input_mode_restore_attempts()
    {
        static int attempts = 0;
        return attempts;
    }

    BOOL WINAPI fail_first_input_mode_restore(HANDLE /*input*/, DWORD /*mode*/)
    {
        input_mode_restore_attempts()++;
        if (input_mode_restore_attempts() == 1)
        {
            SetLastError(ERROR_INVALID_FUNCTION);
            return FALSE;
        }
        return TRUE;
    }

    int &input_discard_attempts()
    {
        static int attempts = 0;
        return attempts;
    }

    bool fail_first_input_discard() noexcept
    {
        input_discard_attempts()++;
        return input_discard_attempts() != 1;
    }

    bool count_input_discard() noexcept
    {
        input_discard_attempts()++;
        return true;
    }

    bool run_console_control_case(
        const std::wstring &executable,
        ScopedWindowsConsole &console,
        DWORD input_mode,
        DWORD output_mode,
        UINT output_cp,
        wchar_t event_name,
        bool queue_mouse_report
    )
    {
        SECURITY_ATTRIBUTES security = {};
        security.nLength = sizeof security;
        security.bInheritHandle = TRUE;
        ScopedWindowsHandle ready(CreateEventW(&security, TRUE, FALSE, nullptr));
        ScopedWindowsHandle teardown_ready(CreateEventW(&security, TRUE, FALSE, nullptr));
        if (ready.value == nullptr || teardown_ready.value == nullptr)
        {
            return false;
        }

        const auto handle_value = reinterpret_cast<std::uintptr_t>(ready.value);
        const auto teardown_handle_value = reinterpret_cast<std::uintptr_t>(teardown_ready.value);
        const wchar_t processed_name = (input_mode & ENABLE_PROCESSED_INPUT) != 0 ? L'p' : L'u';
        const std::wstring command = L"\"" + executable + L"\" --windows-console-control-helper child " + event_name +
                                     L" " + processed_name + L" " + std::to_wstring(handle_value) + L" " +
                                     std::to_wstring(teardown_handle_value);

        PROCESS_INFORMATION process = {};
        const bool launched = launch_process(command, CREATE_NEW_PROCESS_GROUP, true, process);
        if (!launched)
        {
            return false;
        }
        ScopedWindowsHandle process_handle(process.hProcess);
        ScopedWindowsHandle thread_handle(process.hThread);

        if (WaitForSingleObject(ready.value, 5000) != WAIT_OBJECT_0)
        {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
            return false;
        }

        if (queue_mouse_report)
        {
            constexpr wchar_t report[] = L"\033[<32;10;20M";
            constexpr size_t report_size = (sizeof report / sizeof *report) - 1;
            DWORD pending = 0;
            if (!queue_console_bytes(console.input, report, report_size) ||
                GetNumberOfConsoleInputEvents(console.input, &pending) == 0 || pending < report_size)
            {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 1000);
                return false;
            }
        }

        const DWORD event = event_name == L'c' ? CTRL_C_EVENT : CTRL_BREAK_EVENT;
        const DWORD group = event == CTRL_C_EVENT ? 0 : process.dwProcessId;
        if (GenerateConsoleCtrlEvent(event, group) == 0)
        {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 1000);
            return false;
        }
        if (event == CTRL_C_EVENT)
        {
            if (WaitForSingleObject(teardown_ready.value, 5000) != WAIT_OBJECT_0)
            {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 1000);
                return false;
            }
            DWORD teardown_input_mode = 0;
            if (GetConsoleMode(console.input, &teardown_input_mode) == 0 || teardown_input_mode != input_mode)
            {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 1000);
                return false;
            }
            if (GenerateConsoleCtrlEvent(event, group) == 0)
            {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 1000);
                return false;
            }
        }

        if (!wait_for_clean_exit(process, 5000) ||
            !console_modes_equal(console.input, console.output, input_mode, output_mode, output_cp))
        {
            return false;
        }
        DWORD pending = 0;
        return GetNumberOfConsoleInputEvents(console.input, &pending) != 0 && pending == 0;
    }

    int run_console_control_host()
    {
        ScopedWindowsConsole console;
        if (!console.valid || SetConsoleCtrlHandler(nullptr, TRUE) == 0)
        {
            return 1;
        }

        const DWORD input_mode =
            (console.saved_input_mode | ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT) &
            ~static_cast<DWORD>(ENABLE_VIRTUAL_TERMINAL_INPUT);
        const DWORD output_mode = console.saved_output_mode &
                                  ~static_cast<DWORD>(ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        constexpr UINT output_cp = 437;
        if (IsValidCodePage(output_cp) == 0 || SetConsoleMode(console.input, input_mode) == 0 ||
            SetConsoleMode(console.output, output_mode) == 0 || SetConsoleOutputCP(output_cp) == 0)
        {
            return 2;
        }

        std::wstring executable;
        if (!executable_path(executable))
        {
            return 3;
        }
        if (!run_console_control_case(executable, console, input_mode, output_mode, output_cp, L'c', false))
        {
            return 4;
        }
        if (!run_console_control_case(executable, console, input_mode, output_mode, output_cp, L'b', false))
        {
            return 5;
        }
        const DWORD unprocessed_input_mode = input_mode & ~static_cast<DWORD>(ENABLE_PROCESSED_INPUT);
        if (SetConsoleMode(console.input, unprocessed_input_mode) == 0)
        {
            return 6;
        }
        if (!run_console_control_case(executable, console, unprocessed_input_mode, output_mode, output_cp, L'c', true))
        {
            return 7;
        }
        return 0;
    }

    int run_console_control_child(int argc, char *argv[])
    {
        if (argc != 7 || (argv[3][0] != 'c' && argv[3][0] != 'b') || argv[3][1] != '\0' ||
            (argv[4][0] != 'p' && argv[4][0] != 'u') || argv[4][1] != '\0')
        {
            return 10;
        }
        char *end = nullptr;
        const unsigned long long raw_handle = std::strtoull(argv[5], &end, 10);
        if (end == argv[5] || *end != '\0')
        {
            return 11;
        }
        ScopedWindowsHandle ready(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw_handle)));
        const unsigned long long raw_teardown_handle = std::strtoull(argv[6], &end, 10);
        if (end == argv[6] || *end != '\0')
        {
            return 22;
        }
        ScopedWindowsHandle teardown_ready(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw_teardown_handle)));
        ScopedWindowsConsole console;
        if (!console.valid)
        {
            return 12;
        }
        const bool inherited_processed = (console.saved_input_mode & ENABLE_PROCESSED_INPUT) != 0;
        if (inherited_processed != (argv[4][0] == 'p'))
        {
            return 13;
        }
        // The host must observe ConsoleStateGuard's restoration, not a repair by
        // this fixture after the guard has been destroyed.
        console.restore_state = false;

        platform::ConsoleStateGuard guard;
        if (!guard.valid())
        {
            return 14;
        }
        if (!platform::install_interrupt_handler())
        {
            return 15;
        }
        platform::enable_raw_mode();
        platform::enable_vt_input();
        DWORD active_input_mode = 0;
        if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &active_input_mode) == 0 ||
            (active_input_mode & ENABLE_PROCESSED_INPUT) == 0 || (active_input_mode & ENABLE_LINE_INPUT) != 0 ||
            (active_input_mode & ENABLE_ECHO_INPUT) != 0)
        {
            return 16;
        }
        if (SetEvent(ready.value) == 0)
        {
            return 17;
        }

        if (argv[3][0] == 'b' || argv[4][0] == 'p')
        {
            // The production reader can be waiting inside _getch when either event arrives.
            if (_getch() != 'x')
            {
                return 21;
            }
        }

        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (!platform::interrupt_requested() && GetTickCount64() < deadline)
        {
            Sleep(10);
        }
        if (!platform::interrupt_requested())
        {
            return 18;
        }
        if (!platform::disable_raw_mode(&guard))
        {
            return 19;
        }
        if (argv[3][0] == 'c')
        {
            // Deliver the repeat after the production cleanup boundary. The handler
            // must still consume it without adding another wake record to the queue.
            platform::detail::interrupt_flag.store(false, std::memory_order_relaxed);
            if (SetEvent(teardown_ready.value) == 0)
            {
                return 23;
            }
            const ULONGLONG repeat_deadline = GetTickCount64() + 5000;
            while (!platform::interrupt_requested() && GetTickCount64() < repeat_deadline)
            {
                Sleep(10);
            }
            if (!platform::interrupt_requested())
            {
                return 24;
            }
            // The interrupt flag is published after the handler registers itself,
            // so this barrier proves the late handler has finished without flushing.
            platform::detail::disarm_console_input_wake();
        }
        return 0;
    }

    int run_console_cancel_read_child()
    {
        ScopedWindowsConsole console;
        if (!console.valid || FlushConsoleInputBuffer(console.input) == 0)
        {
            return 30;
        }

        ScopedWindowsHandle read_only_input(
            CreateFileW(L"CONIN$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)
        );
        if (read_only_input.value == INVALID_HANDLE_VALUE)
        {
            return 31;
        }

        platform::detail::interrupt_flag.store(false);
        platform::detail::console_input_read_active.store(false);
        std::atomic_bool returned = false;
        bool read = true;
        char input_byte = 0;
        std::thread reader(
            [&]()
            {
                read = platform::detail::read_console_byte(input_byte);
                returned.store(true, std::memory_order_release);
            }
        );

        const ULONGLONG active_deadline = GetTickCount64() + 1000;
        while (!platform::detail::console_input_read_active.load() && GetTickCount64() < active_deadline)
        {
            Sleep(1);
        }
        const bool active = platform::detail::console_input_read_active.load();
        const bool woke = active && platform::detail::wake_console_input(read_only_input.value, reader.native_handle());

        const ULONGLONG return_deadline = GetTickCount64() + 1000;
        while (!returned.load(std::memory_order_acquire) && GetTickCount64() < return_deadline)
        {
            Sleep(1);
        }
        const bool cancelled = returned.load(std::memory_order_acquire);
        if (!cancelled)
        {
            constexpr wchar_t rescue[] = L"x";
            queue_console_bytes(console.input, rescue, 1);
        }
        reader.join();
        platform::detail::console_input_read_active.store(false);

        return active && woke && cancelled && !read ? 0 : 32;
    }

#endif
} // namespace

#ifdef _WIN32
namespace platform_test
{
    int run_windows_console_control_helper(int argc, char *argv[])
    {
        if (argc == 3 && std::strcmp(argv[2], "host") == 0)
        {
            return run_console_control_host();
        }
        if (argc == 3 && std::strcmp(argv[2], "cancel-read") == 0)
        {
            return run_console_cancel_read_child();
        }
        if (argc >= 3 && std::strcmp(argv[2], "child") == 0)
        {
            return run_console_control_child(argc, argv);
        }
        return 20;
    }
} // namespace platform_test
#endif

// NUL is a character device that _isatty accepts on Windows, but GetConsoleMode
// rejects it. This pins the probe used by platform::is_tty.
TEST(platform, is_tty_false_for_null_device)
{
    ScopedFd dev(test_devnull());
    ASSERT_TRUE(dev.fd >= 0);
    ASSERT_FALSE(platform::is_tty(dev.fd));
}

#ifdef _WIN32
TEST(platform, query_read_interrupt_skips_record_fallback)
{
    query_record_read_call_count() = 0;
    platform::detail::interrupt_flag.store(true, std::memory_order_release);
    const int interrupted = platform::detail::finish_query_read(INVALID_HANDLE_VALUE, 0, count_query_record_read);
    platform::detail::interrupt_flag.store(false, std::memory_order_relaxed);
    const int discarded = platform::detail::finish_query_read(INVALID_HANDLE_VALUE, 0, count_query_record_read);
    const int bytes = platform::detail::finish_query_read(INVALID_HANDLE_VALUE, 2, count_query_record_read);

    ASSERT_EQ(interrupted, -1);
    ASSERT_EQ(discarded, 0);
    ASSERT_EQ(bytes, 2);
    ASSERT_EQ(query_record_read_call_count(), 1U);
}

TEST(platform, version_output_preserves_windows_console_state_and_redirected_utf8)
{
    ScopedWindowsConsole console;
    ASSERT_TRUE(console.valid);

    const DWORD output_mode =
        console.saved_output_mode & ~static_cast<DWORD>(ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    ASSERT_TRUE(SetConsoleMode(console.output, output_mode) != 0);
    DWORD baseline_output_mode = 0;
    ASSERT_TRUE(GetConsoleMode(console.output, &baseline_output_mode) != 0);

    UINT output_cp = GetConsoleOutputCP();
    if (output_cp == 65001)
    {
        output_cp = 437;
    }
    ASSERT_TRUE(IsValidCodePage(output_cp) != 0);
    ASSERT_TRUE(SetConsoleOutputCP(output_cp) != 0);

    char program[] = "rasterminal";
    char option[] = "--version";
    char *argv[] = { program, option };
    ParseResult parsed;
    std::string version_text;
    {
        ScopedStdoutCapture output;
        ASSERT_TRUE(output.valid);
        parsed = parse_args(2, argv);
        version_text = output.read();
    }
    ASSERT_FALSE(parsed.ok);
    ASSERT_EQ(parsed.exit_code, 0);
    ASSERT_TRUE(version_text.find(RASTERMINAL_AUTHOR) != std::string::npos);

    DWORD current_output_mode = 0;
    ASSERT_TRUE(GetConsoleMode(console.output, &current_output_mode) != 0);
    ASSERT_EQ(current_output_mode, baseline_output_mode);
    ASSERT_EQ(GetConsoleOutputCP(), output_cp);
}

TEST(platform, version_output_formats_lines_with_processed_output_disabled)
{
    ScopedWindowsConsole console;
    ASSERT_TRUE(console.valid);
    ScopedWindowsHandle screen(CreateConsoleScreenBuffer(
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CONSOLE_TEXTMODE_BUFFER, nullptr
    ));
    ASSERT_TRUE(screen.value != INVALID_HANDLE_VALUE);

    DWORD original_mode = 0;
    ASSERT_TRUE(GetConsoleMode(screen.value, &original_mode) != 0);
    const DWORD baseline_mode =
        original_mode & ~static_cast<DWORD>(ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    ASSERT_TRUE(SetConsoleMode(screen.value, baseline_mode) != 0);
    const UINT output_cp = GetConsoleOutputCP();
    ASSERT_TRUE(SetConsoleCursorPosition(screen.value, { 0, 0 }) != 0);

    char program[] = "rasterminal";
    char option[] = "--version";
    char *argv[] = { program, option };
    ParseResult parsed;
    {
        ScopedStdoutHandle output(screen.value);
        ASSERT_TRUE(output.valid);
        parsed = parse_args(2, argv);
    }
    ASSERT_FALSE(parsed.ok);
    ASSERT_EQ(parsed.exit_code, 0);

    wchar_t row[80] = {};
    DWORD read = 0;
    ASSERT_TRUE(ReadConsoleOutputCharacterW(screen.value, row, 80, { 0, 0 }, &read) != 0);
    ASSERT_TRUE(std::wstring(row, read).find(L"rasterminal ") == 0);
    ASSERT_TRUE(ReadConsoleOutputCharacterW(screen.value, row, 80, { 0, 1 }, &read) != 0);
    ASSERT_TRUE(std::wstring(row, read).find(L"Pavol Uli\u010dn\u00fd") != std::wstring::npos);

    DWORD current_mode = 0;
    ASSERT_TRUE(GetConsoleMode(screen.value, &current_mode) != 0);
    ASSERT_EQ(current_mode, baseline_mode);
    ASSERT_EQ(GetConsoleOutputCP(), output_cp);
}

TEST(platform, console_state_guard_restores_exact_windows_state)
{
    ScopedWindowsConsole console;
    ASSERT_TRUE(console.valid);

    const DWORD input_mode = (console.saved_input_mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT) &
                             ~static_cast<DWORD>(ENABLE_PROCESSED_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
    const DWORD output_mode =
        console.saved_output_mode & ~static_cast<DWORD>(ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    ASSERT_TRUE(SetConsoleMode(console.input, input_mode) != 0);
    ASSERT_TRUE(SetConsoleMode(console.output, output_mode) != 0);
    DWORD baseline_input_mode = 0;
    DWORD baseline_output_mode = 0;
    ASSERT_TRUE(GetConsoleMode(console.input, &baseline_input_mode) != 0);
    ASSERT_TRUE(GetConsoleMode(console.output, &baseline_output_mode) != 0);
    ASSERT_TRUE((baseline_input_mode & ENABLE_PROCESSED_INPUT) == 0);

    UINT output_cp = GetConsoleOutputCP();
    if (output_cp == 65001)
    {
        output_cp = 437;
    }
    ASSERT_TRUE(IsValidCodePage(output_cp) != 0);
    ASSERT_TRUE(SetConsoleOutputCP(output_cp) != 0);

    {
        const platform::ConsoleStateGuard guard;
        ASSERT_TRUE(guard.valid());
        // Repeating setup must not change the captured restoration baseline.
        ASSERT_TRUE(platform::init_console_output());
        ASSERT_TRUE(platform::init_console_output());
        platform::enable_raw_mode();
        platform::enable_vt_input();

        DWORD changed_input_mode = 0;
        DWORD changed_output_mode = 0;
        ASSERT_TRUE(GetConsoleMode(console.input, &changed_input_mode) != 0);
        ASSERT_TRUE(GetConsoleMode(console.output, &changed_output_mode) != 0);
        const DWORD expected_input_mode = ((baseline_input_mode | ENABLE_PROCESSED_INPUT) &
                                           ~static_cast<DWORD>(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)) |
                                          ENABLE_VIRTUAL_TERMINAL_INPUT;
        ASSERT_EQ(changed_input_mode, expected_input_mode);
        ASSERT_TRUE((changed_input_mode & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0);
        ASSERT_TRUE((changed_input_mode & ENABLE_LINE_INPUT) == 0);
        ASSERT_TRUE((changed_input_mode & ENABLE_ECHO_INPUT) == 0);
        ASSERT_TRUE((changed_input_mode & ENABLE_PROCESSED_INPUT) != 0);
        ASSERT_TRUE((changed_output_mode & ENABLE_PROCESSED_OUTPUT) != 0);
        ASSERT_TRUE((changed_output_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0);
        ASSERT_EQ(GetConsoleOutputCP(), static_cast<UINT>(65001));
    }

    DWORD restored_input_mode = 0;
    DWORD restored_output_mode = 0;
    ASSERT_TRUE(GetConsoleMode(console.input, &restored_input_mode) != 0);
    ASSERT_TRUE(GetConsoleMode(console.output, &restored_output_mode) != 0);
    ASSERT_EQ(restored_input_mode, baseline_input_mode);
    ASSERT_TRUE((restored_input_mode & ENABLE_PROCESSED_INPUT) == 0);
    ASSERT_EQ(restored_output_mode, baseline_output_mode);
    ASSERT_EQ(GetConsoleOutputCP(), output_cp);
}

TEST(platform, console_close_event_uses_default_handler)
{
    platform::detail::interrupt_flag.store(false, std::memory_order_relaxed);
    platform::detail::console_input_wake_enabled.store(false, std::memory_order_relaxed);
    platform::detail::console_input_wake_handlers.store(0, std::memory_order_relaxed);

    ASSERT_TRUE(platform::detail::console_interrupt_handler(CTRL_CLOSE_EVENT) == FALSE);
    ASSERT_TRUE(!platform::interrupt_requested());
    ASSERT_EQ(platform::detail::console_input_wake_handlers.load(std::memory_order_relaxed), 0U);
}

TEST(platform, console_input_wake_cancels_read_when_queue_write_fails)
{
    std::wstring executable;
    ASSERT_TRUE(executable_path(executable));
    const std::wstring command = L"\"" + executable + L"\" --windows-console-control-helper cancel-read";
    PROCESS_INFORMATION process = {};
    ASSERT_TRUE(launch_process(command, CREATE_NEW_CONSOLE, false, process));
    ScopedWindowsHandle process_handle(process.hProcess);
    ScopedWindowsHandle thread_handle(process.hThread);
    ASSERT_TRUE(wait_for_clean_exit(process, 5000));
}

TEST(platform, console_input_cancellation_retries_when_read_has_not_started)
{
    platform::detail::console_input_read_active.store(true);
    cancel_console_input_call_count().store(0);
    const bool cancelled = platform::detail::cancel_console_input_read(
        reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(1)), cancel_console_input_after_miss
    );
    platform::detail::console_input_read_active.store(false);

    ASSERT_TRUE(cancelled);
    ASSERT_EQ(cancel_console_input_call_count().load(), 2U);
}

TEST(platform, console_state_guard_rejects_each_failed_probe)
{
    for (int failed_mode_probe = 1; failed_mode_probe <= 2; failed_mode_probe++)
    {
        console_mode_probe_count() = 0;
        console_mode_probe_failure() = failed_mode_probe;
        console_output_cp_probe_failure() = false;
        platform::ConsoleStateGuard guard(probe_console_mode, probe_console_output_cp);
        ASSERT_FALSE(guard.valid());
        ASSERT_FALSE(guard.restore_input_mode());
    }

    console_mode_probe_count() = 0;
    console_mode_probe_failure() = 0;
    console_output_cp_probe_failure() = true;
    platform::ConsoleStateGuard guard(probe_console_mode, probe_console_output_cp);
    ASSERT_FALSE(guard.valid());
    ASSERT_FALSE(guard.restore_input_mode());
    console_output_cp_probe_failure() = false;
}

TEST(platform, console_state_guard_ignores_pipe_handles)
{
    ScopedWindowsConsole console;
    ASSERT_TRUE(console.valid);
    ASSERT_TRUE(IsValidCodePage(437) != 0);
    ASSERT_TRUE(SetConsoleOutputCP(437) != 0);

    ScopedWindowsPipes pipes;
    ASSERT_TRUE(pipes.valid);

    DWORD mode = 0;
    ASSERT_FALSE(GetConsoleMode(pipes.input_read, &mode) != 0);
    ASSERT_FALSE(GetConsoleMode(pipes.output_write, &mode) != 0);
    {
        const platform::ConsoleStateGuard guard;
        ASSERT_FALSE(guard.valid());
        ASSERT_FALSE(platform::init_console_output());
        platform::enable_vt_input();
        ASSERT_TRUE(SetConsoleOutputCP(65001) != 0);
    }
    // An incomplete snapshot owns no state and must not restore this unrelated change.
    ASSERT_EQ(GetConsoleOutputCP(), static_cast<UINT>(65001));
}

TEST(platform, console_state_guard_does_not_restore_partial_snapshot)
{
    ScopedWindowsConsole console;
    ASSERT_TRUE(console.valid);

    const DWORD input_mode = console.saved_input_mode & ~static_cast<DWORD>(ENABLE_VIRTUAL_TERMINAL_INPUT);
    ASSERT_TRUE(SetConsoleMode(console.input, input_mode) != 0);
    DWORD baseline_input_mode = 0;
    ASSERT_TRUE(GetConsoleMode(console.input, &baseline_input_mode) != 0);
    ASSERT_TRUE(IsValidCodePage(437) != 0);
    ASSERT_TRUE(SetConsoleOutputCP(437) != 0);

    ScopedWindowsPipes pipes;
    ASSERT_TRUE(pipes.valid);
    ASSERT_TRUE(SetStdHandle(STD_INPUT_HANDLE, console.input) != 0);

    DWORD changed_input_mode = 0;
    {
        const platform::ConsoleStateGuard guard;
        ASSERT_FALSE(guard.valid());
        ASSERT_FALSE(platform::init_console_output());
        platform::enable_vt_input();
        ASSERT_TRUE(GetConsoleMode(console.input, &changed_input_mode) != 0);
        ASSERT_TRUE((changed_input_mode & ENABLE_VIRTUAL_TERMINAL_INPUT) != 0);
        ASSERT_TRUE(SetConsoleOutputCP(65001) != 0);
    }

    DWORD restored_input_mode = 0;
    ASSERT_TRUE(GetConsoleMode(console.input, &restored_input_mode) != 0);
    ASSERT_EQ(restored_input_mode, changed_input_mode);
    // The caller must reject an incomplete snapshot before mutation. If it does not,
    // the guard still avoids overwriting newer state with a partial capture.
    ASSERT_EQ(GetConsoleOutputCP(), static_cast<UINT>(65001));
}

TEST(platform, disable_raw_mode_discards_pending_windows_input)
{
    ScopedWindowsConsole console;
    ASSERT_TRUE(console.valid);
    ASSERT_TRUE(FlushConsoleInputBuffer(console.input) != 0);

    INPUT_RECORD records[3] = {};
    constexpr wchar_t bytes[] = { L'\033', L'[', L'M' };
    for (size_t i = 0; i < 3; i++)
    {
        records[i].EventType = KEY_EVENT;
        records[i].Event.KeyEvent.bKeyDown = TRUE;
        records[i].Event.KeyEvent.wRepeatCount = 1;
        records[i].Event.KeyEvent.uChar.UnicodeChar = bytes[i];
    }
    DWORD written = 0;
    ASSERT_TRUE(WriteConsoleInputW(console.input, records, 3, &written) != 0);
    ASSERT_EQ(written, static_cast<DWORD>(3));

    DWORD pending = 0;
    ASSERT_TRUE(GetNumberOfConsoleInputEvents(console.input, &pending) != 0);
    ASSERT_TRUE(pending >= 3);
    ASSERT_TRUE(platform::disable_raw_mode());
    ASSERT_TRUE(GetNumberOfConsoleInputEvents(console.input, &pending) != 0);
    ASSERT_EQ(pending, static_cast<DWORD>(0));
}

TEST(platform, console_state_guard_retries_input_cleanup_after_restore_failure)
{
    ScopedWindowsConsole console;
    ASSERT_TRUE(console.valid);
    input_mode_restore_attempts() = 0;
    input_discard_attempts() = 0;

    {
        platform::ConsoleStateGuard guard(
            GetConsoleMode, GetConsoleOutputCP, fail_first_input_mode_restore, count_input_discard
        );
        ASSERT_TRUE(guard.valid());
        ASSERT_FALSE(platform::disable_raw_mode(&guard));
        ASSERT_EQ(input_mode_restore_attempts(), 1);
        ASSERT_EQ(input_discard_attempts(), 0);
    }

    ASSERT_EQ(input_mode_restore_attempts(), 2);
    ASSERT_EQ(input_discard_attempts(), 1);
}

TEST(platform, console_state_guard_retries_input_cleanup_after_discard_failure)
{
    ScopedWindowsConsole console;
    ASSERT_TRUE(console.valid);
    input_discard_attempts() = 0;

    {
        platform::ConsoleStateGuard guard(GetConsoleMode, GetConsoleOutputCP, SetConsoleMode, fail_first_input_discard);
        ASSERT_TRUE(guard.valid());
        ASSERT_FALSE(platform::disable_raw_mode(&guard));
        ASSERT_EQ(input_discard_attempts(), 1);
    }

    ASSERT_EQ(input_discard_attempts(), 2);
}

TEST(platform, disable_raw_mode_handles_read_only_standard_input)
{
    ScopedWindowsConsole console;
    ASSERT_TRUE(console.valid);
    ASSERT_TRUE(FlushConsoleInputBuffer(console.input) != 0);

    ScopedWindowsHandle restricted_input(
        CreateFileW(L"CONIN$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)
    );
    ASSERT_TRUE(restricted_input.value != INVALID_HANDLE_VALUE);
    ScopedWindowsStandardInput standard_input(restricted_input.value);
    ASSERT_TRUE(standard_input.valid);
    ASSERT_FALSE(FlushConsoleInputBuffer(GetStdHandle(STD_INPUT_HANDLE)) != 0);

    constexpr wchar_t report[] = L"\033[<32;10;20M";
    constexpr size_t report_size = (sizeof report / sizeof *report) - 1;
    ASSERT_TRUE(queue_console_bytes(console.input, report, report_size));
    DWORD pending = 0;
    ASSERT_TRUE(GetNumberOfConsoleInputEvents(console.input, &pending) != 0);
    ASSERT_TRUE(pending >= report_size);

    ASSERT_TRUE(platform::disable_raw_mode());
    ASSERT_TRUE(GetNumberOfConsoleInputEvents(console.input, &pending) != 0);
    ASSERT_EQ(pending, static_cast<DWORD>(0));
}

TEST(platform, discard_pending_input_falls_back_to_read_only_console_handle)
{
    ScopedWindowsConsole console;
    ASSERT_TRUE(console.valid);
    ASSERT_TRUE(FlushConsoleInputBuffer(console.input) != 0);

    ScopedWindowsHandle restricted_input(
        CreateFileW(L"CONIN$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr)
    );
    ASSERT_TRUE(restricted_input.value != INVALID_HANDLE_VALUE);
    ASSERT_FALSE(FlushConsoleInputBuffer(restricted_input.value) != 0);

    constexpr wchar_t report[] = L"\033[<32;10;20M";
    constexpr size_t report_size = (sizeof report / sizeof *report) - 1;
    ASSERT_TRUE(queue_console_bytes(console.input, report, report_size));
    ASSERT_TRUE(platform::detail::discard_pending_console_input(restricted_input.value, INVALID_HANDLE_VALUE));

    DWORD pending = 0;
    ASSERT_TRUE(GetNumberOfConsoleInputEvents(restricted_input.value, &pending) != 0);
    ASSERT_EQ(pending, static_cast<DWORD>(0));
    // Simulate another reader consuming a record after the snapshot count.
    ASSERT_TRUE(platform::detail::drain_console_input_records_nowait(restricted_input.value, 1));
}

TEST(platform, console_cleanup_handles_control_events)
{
    std::wstring executable;
    ASSERT_TRUE(executable_path(executable));
    const std::wstring command = L"\"" + executable + L"\" --windows-console-control-helper host";
    PROCESS_INFORMATION process = {};
    ASSERT_TRUE(launch_process(command, CREATE_NEW_CONSOLE, false, process));
    ScopedWindowsHandle process_handle(process.hProcess);
    ScopedWindowsHandle thread_handle(process.hThread);
    ASSERT_TRUE(wait_for_clean_exit(process, 15000));
}
#endif

#ifndef _WIN32
// Probe a fresh pty slave, which POSIX defines as a terminal. Master behavior
// varies by OS, and Windows has no equivalent portable fixture.
TEST(platform, is_tty_true_for_pty_slave)
{
    ScopedFd master(posix_openpt(O_RDWR | O_NOCTTY));
    ASSERT_TRUE(master.fd >= 0);
    ASSERT_TRUE(grantpt(master.fd) == 0);
    ASSERT_TRUE(unlockpt(master.fd) == 0);
    // ptsname's static-buffer return is mt-unsafe, harmless in this single-
    // threaded test; ptsname_r is glibc-only so not portable to the macOS CI.
    const char *slave_name = ptsname(master.fd); // NOLINT(concurrency-mt-unsafe)
    ASSERT_TRUE(slave_name != nullptr);
    ScopedFd slave(open(slave_name, O_RDWR | O_NOCTTY));
    ASSERT_TRUE(slave.fd >= 0);
    ASSERT_TRUE(platform::is_tty(slave.fd));
}
#endif

// color-capability classifier
// Pure inputs make every branch portable except the platform-specific unset default.

TEST(platform, classify_unset_env_uses_default)
{
    ASSERT_EQ(platform::classify_term_color(nullptr, nullptr, P256, false, false), P256);
    ASSERT_EQ(platform::classify_term_color(nullptr, nullptr, TC, false, false), TC);
    ASSERT_EQ(platform::classify_term_color("", "", P256, false, false), P256);
    ASSERT_EQ(platform::classify_term_color("", "", TC, false, false), TC);
}

TEST(platform, classify_dumb_always_fatal)
{
    ASSERT_EQ(platform::classify_term_color(nullptr, "dumb", P256, false, false), DUMB);
    // Dumb beats a contradictory COLORTERM: a dumb terminal can't render escapes
    // regardless of what claims color support.
    ASSERT_EQ(platform::classify_term_color("truecolor", "dumb", P256, false, false), DUMB);
    ASSERT_EQ(platform::classify_term_color("24bit", "DUMB", TC, false, false), DUMB);
}

TEST(platform, classify_dumb_is_exact_match_not_substring)
{
    // "dumb" is an exact match, unlike truecolor substring hints. A containing
    // terminfo alias must not be rejected.
    ASSERT_EQ(platform::classify_term_color(nullptr, "dumbo", P256, false, false), P256);
    ASSERT_EQ(platform::classify_term_color(nullptr, "xterm-dumbnot", TC, false, false), P256);
}

TEST(platform, classify_colorterm_truecolor)
{
    // COLORTERM beats a 256-only TERM.
    ASSERT_EQ(platform::classify_term_color("truecolor", "xterm-256color", P256, false, false), TC);
    ASSERT_EQ(platform::classify_term_color("24bit", "xterm", P256, false, false), TC);
    ASSERT_EQ(platform::classify_term_color("TRUECOLOR", nullptr, P256, false, false), TC);
    ASSERT_EQ(platform::classify_term_color("Truecolor", "xterm", P256, false, false), TC);
}

TEST(platform, classify_colorterm_unrecognized_falls_through)
{
    // COLORTERM=1/yes means only "has color," so TERM still decides the color depth.
    ASSERT_EQ(platform::classify_term_color("yes", "xterm-256color", TC, false, false), P256);
    ASSERT_EQ(platform::classify_term_color("1", "xterm", TC, false, false), P256);
    ASSERT_EQ(platform::classify_term_color("yes", nullptr, TC, false, false), TC);
    ASSERT_EQ(platform::classify_term_color("yes", nullptr, P256, false, false), P256);
}

TEST(platform, classify_term_direct_hints)
{
    ASSERT_EQ(platform::classify_term_color(nullptr, "xterm-direct", P256, false, false), TC);
    ASSERT_EQ(platform::classify_term_color(nullptr, "tmux-direct", P256, false, false), TC);
    ASSERT_EQ(platform::classify_term_color(nullptr, "xterm-direct256", P256, false, false), TC);
    ASSERT_EQ(platform::classify_term_color(nullptr, "xterm-truecolor", P256, false, false), TC);
    ASSERT_EQ(platform::classify_term_color(nullptr, "XTERM-DIRECT", P256, false, false), TC);
    // The third hint: without this, deleting icontains(term, "24bit") from the classifier
    // leaves the whole suite green.
    ASSERT_EQ(platform::classify_term_color(nullptr, "xterm-24bit", P256, false, false), TC);
    // An unknown COLORTERM falls through to TERM rather than forcing Palette256.
    ASSERT_EQ(platform::classify_term_color("gnome-terminal", "xterm-direct", P256, false, false), TC);
}

TEST(platform, classify_known_truecolor_terms)
{
    // The case-insensitive allowlist covers terminals whose TERM suffix identifies
    // truecolor when ssh does not forward COLORTERM.
    const char *terms[] = { "xterm-kitty", "wezterm", "alacritty", "xterm-ghostty", "foot", "contour" };
    for (const char *t : terms)
    {
        ASSERT_EQ(platform::classify_term_color(nullptr, t, P256, false, false), TC);
    }
    ASSERT_EQ(platform::classify_term_color(nullptr, "XTERM-KITTY", P256, false, false), TC);
}

TEST(platform, classify_real_screen_floors_colorterm)
{
    // GNU screen must override an inherited outer-terminal COLORTERM because 4.x
    // misparses 24-bit SGR. Cover plain, derived, suffixed, and case-folded names.
    ASSERT_EQ(platform::classify_term_color("truecolor", "screen", P256, false, false), P256);
    ASSERT_EQ(platform::classify_term_color("truecolor", "screen-256color", P256, false, false), P256);
    ASSERT_EQ(platform::classify_term_color("truecolor", "screen.xterm-256color", P256, false, false), P256);
    ASSERT_EQ(platform::classify_term_color("24bit", "screen", P256, false, false), P256);
    ASSERT_EQ(platform::classify_term_color("truecolor", "SCREEN-256COLOR", P256, false, false), P256);
    // The prefix is anchored: a hypothetical non-screen TERM merely containing
    // the word is untouched.
    ASSERT_EQ(platform::classify_term_color("truecolor", "xterm-screenish", P256, false, false), TC);
}

TEST(platform, classify_sty_floors_regardless_of_term_and_tmux)
{
    // STY is screen's own session marker, exported to every child: it floors
    // even when a .screenrc/`screen -T` rewrote TERM to a non-screen name, and
    // even under an inherited TMUX (screen nested inside tmux).
    ASSERT_EQ(platform::classify_term_color("truecolor", "xterm-256color", P256, false, true), P256);
    ASSERT_EQ(platform::classify_term_color("truecolor", "screen", P256, true, true), P256);
    ASSERT_EQ(platform::classify_term_color(nullptr, nullptr, TC, false, true), P256);
    // Dumb still exits fatal whatever the multiplexer signals say.
    ASSERT_EQ(platform::classify_term_color("truecolor", "dumb", P256, true, true), DUMB);
}

TEST(platform, classify_screen_under_tmux_keeps_colorterm)
{
    // tmux always sets TMUX, even in older configs that set TERM=screen-256color,
    // and it translates 24-bit SGR for the outer terminal, so COLORTERM stays
    // trusted there (the surviving half of the original decision).
    ASSERT_EQ(platform::classify_term_color("truecolor", "screen-256color", P256, true, false), TC);
    ASSERT_EQ(platform::classify_term_color("truecolor", "screen", P256, true, false), TC);
    // Without COLORTERM, screen-family TERMs stay on the floor either way.
    ASSERT_EQ(platform::classify_term_color(nullptr, "screen-256color", P256, true, false), P256);
}

TEST(platform, classify_plain_terms_are_256)
{
    // No screen-family entry here: those return at the screen floor, not the
    // final fall-through this test exercises (they have their own tests above).
    const char *terms[] = { "xterm", "xterm-256color", "tmux-256color", "linux", "vt100", "st-256color" };
    for (const char *t : terms)
    {
        // Never Dumb, never TrueColor: sub-256-color entries still get the
        // 256-color floor rather than a fatal error (16-color is unsupported).
        ASSERT_EQ(platform::classify_term_color(nullptr, t, TC, false, false), P256);
    }
}

#ifndef _WIN32
// POSIX delegates escapes to the terminal and always reports VT support. A Windows
// unit test would depend on whether redirected stdout has a real console handle.
TEST(platform, init_console_output_ok_on_posix)
{
    ASSERT_TRUE(platform::init_console_output());
}
#endif

namespace
{
    // Mutate the CRT environment read by std::getenv. Empty and absent values are
    // equivalent here; the single-threaded test binary makes mutation safe.
    void set_env(const char *name, const char *value)
    {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 1); // NOLINT(concurrency-mt-unsafe)
#endif
    }

    void unset_env(const char *name)
    {
#ifdef _WIN32
        _putenv_s(name, "");
#else
        unsetenv(name); // NOLINT(concurrency-mt-unsafe)
#endif
    }

    // Saves one env var and restores it (or re-unsets it) on scope exit, so a thrown
    // ASSERT mid-test can't leak mutated env into later tests (the ScopedFd idiom).
    struct ScopedEnv
    {
        const char *name;
        std::string saved;
        bool was_set;
        explicit ScopedEnv(const char *n) : name(n)
        {
            const char *v = getenv(name); // NOLINT(concurrency-mt-unsafe)
            was_set = v != nullptr;
            saved = was_set ? v : "";
        }
        ~ScopedEnv()
        {
            if (was_set)
            {
                set_env(name, saved.c_str());
            }
            else
            {
                unset_env(name);
            }
        }
        ScopedEnv(const ScopedEnv &) = delete;
        ScopedEnv &operator=(const ScopedEnv &) = delete;
        ScopedEnv(ScopedEnv &&) = delete;
        ScopedEnv &operator=(ScopedEnv &&) = delete;
    };
} // namespace

// Exercise getenv integration and each platform's real unset-env default.
TEST(platform, detect_term_color_reads_env)
{
    ScopedEnv colorterm_guard("COLORTERM");
    ScopedEnv term_guard("TERM");
    ScopedEnv tmux_guard("TMUX");
    ScopedEnv sty_guard("STY");

    set_env("COLORTERM", "truecolor");
    set_env("TERM", "xterm");
    unset_env("TMUX");
    unset_env("STY");
    ASSERT_EQ(platform::detect_term_color(), TC);

    // The wrapper feeds both multiplexer signals through: real screen floors
    // the inherited COLORTERM, tmux (which sets TMUX) keeps it, and screen's
    // own STY floors even under a rewritten TERM.
    set_env("TERM", "screen");
    ASSERT_EQ(platform::detect_term_color(), P256);
    set_env("TMUX", "/tmp/fake-tmux,1,0");
    ASSERT_EQ(platform::detect_term_color(), TC);
    unset_env("TMUX");
    set_env("TERM", "xterm");
    set_env("STY", "1234.pts-0.host");
    ASSERT_EQ(platform::detect_term_color(), P256);
    unset_env("STY");
#ifndef _WIN32
    // Empty means absent for both multiplexer signals (tmux's own nesting
    // workaround is TMUX=), which unset_env alone cannot pin since it truly
    // unsets. POSIX-only: _putenv_s(name, "") deletes the variable instead.
    set_env("TERM", "screen");
    set_env("TMUX", "");
    ASSERT_EQ(platform::detect_term_color(), P256);
    unset_env("TMUX");
    set_env("TERM", "xterm");
    set_env("STY", "");
    ASSERT_EQ(platform::detect_term_color(), TC);
    unset_env("STY");
#endif

    unset_env("COLORTERM");
    set_env("TERM", "dumb");
    ASSERT_EQ(platform::detect_term_color(), DUMB);

    unset_env("TERM");
    // Both unset: the platform default. POSIX floors to 256; native Windows defaults to
    // truecolor (VT-on conhost / Windows Terminal render 24-bit).
#ifdef _WIN32
    ASSERT_EQ(platform::detect_term_color(), TC);
#else
    ASSERT_EQ(platform::detect_term_color(), P256);
#endif
}

// file size
// Pin exact and empty sizes plus the EOF-position contract. Multi-GB fixtures are
// impractical in CI, but these use the same 64-bit seek/tell path.

namespace
{
    // Minimal scoped temp file. tests/loader_util.h's TmpFile is the same idiom but
    // pulls in src/loaders/mesh.h, overkill for this standalone platform test.
    struct ScopedTmpFile
    {
        std::string path;
        ScopedTmpFile(const char *name, const void *data, size_t n)
            : path((std::filesystem::temp_directory_path() / name).string())
        {
            // The destructor never runs on a ctor throw, so any assertion below would leak
            // the partial file; remove it and rethrow to keep the test failure intact.
            try
            {
                std::FILE *f = std::fopen(path.c_str(), "wb");
                ASSERT_TRUE(f != nullptr);
                const size_t written = std::fwrite(data, 1, n, f);
                // fclose flushes: a deferred write failure (e.g. ENOSPC) surfaces only here, not
                // in the fwrite count, so check both. Close before asserting so the fd never leaks.
                const int closed = std::fclose(f);
                ASSERT_EQ(written, n);
                ASSERT_EQ(closed, 0);
            }
            catch (...)
            {
                std::remove(path.c_str());
                throw;
            }
        }
        ~ScopedTmpFile() { std::remove(path.c_str()); }
        ScopedTmpFile(const ScopedTmpFile &) = delete;
        ScopedTmpFile &operator=(const ScopedTmpFile &) = delete;
        ScopedTmpFile(ScopedTmpFile &&) = delete;
        ScopedTmpFile &operator=(ScopedTmpFile &&) = delete;
    };
} // namespace

TEST(platform, file_size_reports_exact_byte_count)
{
    // A non-round 259 bytes catches off-by-one and block-granularity errors.
    unsigned char data[259];
    for (size_t i = 0; i < sizeof(data); i++)
    {
        data[i] = static_cast<unsigned char>(i % 256);
    }
    ScopedTmpFile t("rasterminal_test_file_size.bin", data, sizeof(data));

    std::FILE *f = std::fopen(t.path.c_str(), "rb");
    ASSERT_TRUE(f != nullptr);
    const int64_t size = platform::file_size(f);
    // Position is documented to be left at end-of-file on success, so a caller that
    // forgets to seek back reads nothing rather than garbage.
    const int next = std::fgetc(f);
    std::fclose(f);
    ASSERT_EQ(size, int64_t{ 259 });
    ASSERT_EQ(next, EOF);
}

TEST(platform, file_size_empty_file_is_zero)
{
    ScopedTmpFile t("rasterminal_test_file_size_empty.bin", "", 0);

    std::FILE *f = std::fopen(t.path.c_str(), "rb");
    ASSERT_TRUE(f != nullptr);
    const int64_t size = platform::file_size(f);
    std::fclose(f);
    ASSERT_EQ(size, int64_t{ 0 });
}

// input parser
// Test the pure grammar directly on every platform. A sequence must be consumed
// whole or not at all because leftover bytes become live keybindings.

namespace
{
    using PK = platform::detail::ParseResult::Kind;

    platform::detail::ParseResult parse(const std::string &bytes)
    {
        return platform::detail::parse_input(bytes.data(), static_cast<int>(bytes.size()));
    }

    // Asserts the front of `bytes` is a valid sequence with no binding, consumed
    // whole. `consumed == size` means nothing is left over to leak.
    void expect_dropped_whole(const std::string &bytes)
    {
        const platform::detail::ParseResult r = parse(bytes);
        ASSERT_EQ(r.kind, PK::Drop);
        ASSERT_EQ(r.consumed, static_cast<int>(bytes.size()));
    }

    void expect_key(const std::string &bytes, platform::Key key, int consumed)
    {
        const platform::detail::ParseResult r = parse(bytes);
        ASSERT_EQ(r.kind, PK::Complete);
        ASSERT_EQ(r.consumed, consumed);
        ASSERT_EQ(r.event.type, platform::InputEvent::Type::Key);
        ASSERT_EQ(r.event.key, key);
    }

    // A prefix must never be decided on partial evidence: every proper prefix of a
    // sequence has to report Incomplete so the bytes stay buffered.
    void expect_every_prefix_incomplete(const std::string &bytes)
    {
        for (size_t n = 1; n < bytes.size(); n++)
        {
            const platform::detail::ParseResult r = parse(bytes.substr(0, n));
            ASSERT_EQ(r.kind, PK::Incomplete);
        }
    }
} // namespace

TEST(parse_input, plain_characters)
{
    expect_key("q", platform::Key::Q, 1);
    expect_key("w", platform::Key::W, 1);
    expect_key("1", platform::Key::Num1, 1);
    expect_key("e", platform::Key::E, 1);
    expect_key("v", platform::Key::V, 1);
    // Case is folded, so shift is not a distinct binding on any key.
    expect_key("E", platform::Key::E, 1);
    expect_key("V", platform::Key::V, 1);
    // A byte with no binding is consumed and reported as nothing, so a Key event
    // always names a key and no caller has to filter Key::None at the dispatch.
    expect_dropped_whole("z");
    expect_dropped_whole("\a");
}

TEST(parse_input, bare_escape_is_incomplete_not_a_key)
{
    // ESC alone can still grow into any sequence, so it must never resolve on its
    // own. poll_event ages it out and discards it; it is never a keypress.
    ASSERT_EQ(parse("\033").kind, PK::Incomplete);
    ASSERT_EQ(parse("").kind, PK::Incomplete);
}

TEST(parse_input, csi_arrows)
{
    expect_key("\033[A", platform::Key::Up, 3);
    expect_key("\033[B", platform::Key::Down, 3);
    expect_key("\033[C", platform::Key::Right, 3);
    expect_key("\033[D", platform::Key::Left, 3);
    expect_every_prefix_incomplete("\033[A");
}

TEST(parse_input, ss3_arrows)
{
    // DECCKM application cursor mode sends arrows as SS3.
    expect_key("\033OA", platform::Key::Up, 3);
    expect_key("\033OD", platform::Key::Left, 3);
    expect_every_prefix_incomplete("\033OA");
}

TEST(parse_input, ss3_function_keys_dropped)
{
    expect_dropped_whole("\033OP"); // F1
    expect_dropped_whole("\033OS"); // F4, whose final 'S' would otherwise orbit
}

TEST(parse_input, parameterized_ss3_consumed_whole)
{
    // Non-standard but real: consuming only the three-byte form would leave ";2Q"
    // behind, and that 'Q' quits. A parameter byte cannot begin a single-shot SS3,
    // so it is unambiguous enough to scan to the final.
    expect_dropped_whole("\033O1;2Q");
    expect_dropped_whole("\033O5P");
    expect_every_prefix_incomplete("\033O1;2Q");

    // The scan accepts only parameter bytes before the final. Running to any byte
    // in the CSI final range instead would swallow every following key encoded
    // below 0x40, so a stray chord would eat a '+' or a '-' as well as digits.
    const platform::detail::ParseResult plus = parse("\033O1+");
    ASSERT_EQ(plus.kind, PK::Drop);
    ASSERT_EQ(plus.consumed, 3); // stops before the '+', which stays a keypress
    expect_key("+", platform::Key::Plus, 1);
}

TEST(parse_input, unknown_csi_consumed_to_final)
{
    expect_dropped_whole("\033[15~");  // F5
    expect_dropped_whole("\033[1;5A"); // ctrl+Up: the modified form is not an arrow
    expect_dropped_whole("\033[H");    // Home
    expect_dropped_whole("\033[3~");   // Delete
    expect_dropped_whole("\033[Z");    // shift+tab
    expect_every_prefix_incomplete("\033[15~");
}

TEST(parse_input, cell_size_report)
{
    // XTWINOPS 16 reply, \033[6;<height>;<width>t: height leads on the wire,
    // the event carries x = width, y = height.
    const platform::detail::ParseResult r = parse("\033[6;33;15t");
    ASSERT_EQ(r.kind, PK::Complete);
    ASSERT_EQ(r.consumed, 10);
    ASSERT_EQ(r.event.type, platform::InputEvent::Type::CellSize);
    ASSERT_EQ(r.event.x, 15);
    ASSERT_EQ(r.event.y, 33);
    expect_every_prefix_incomplete("\033[6;33;15t");
}

TEST(parse_input, cell_size_report_malformed_drops)
{
    expect_dropped_whole("\033[4;33;15t");   // wrong leading param: another XTWINOPS report
    expect_dropped_whole("\033[6;33t");      // short of its three parameters
    expect_dropped_whole("\033[6;33;15;2t"); // a fourth parameter
    expect_dropped_whole("\033[6;0;15t");    // zero is not a cell size
    expect_dropped_whole("\033[6;33;1001t"); // past the sanity ceiling
    expect_dropped_whole("\033[t");          // no parameters at all
    expect_dropped_whole("\033[6;3:3;15t");  // non-numeric parameter byte (sub-parameter colon)
}

TEST(parse_input, sixel_geometry_report)
{
    // XTSMGRAPHICS item-2 read reply, \033[?2;0;<width>;<height>S: width leads
    // on the wire, the event carries x = width, y = height (asymmetric values
    // so a swap cannot pass).
    const platform::detail::ParseResult r = parse("\033[?2;0;480;312S");
    ASSERT_EQ(r.kind, PK::Complete);
    ASSERT_EQ(r.consumed, 15);
    ASSERT_EQ(r.event.type, platform::InputEvent::Type::SixelGeometry);
    ASSERT_EQ(r.event.x, 480);
    ASSERT_EQ(r.event.y, 312);
    expect_every_prefix_incomplete("\033[?2;0;480;312S");
}

TEST(parse_input, sixel_geometry_report_malformed_drops)
{
    expect_dropped_whole("\033[?1;0;1024S");       // item 1: colour registers, not geometry
    expect_dropped_whole("\033[?1;0;100;100S");    // wrong item at the full arity
    expect_dropped_whole("\033[?2;3;0S");          // failure status
    expect_dropped_whole("\033[?2;3;10;10S");      // failure status at the full arity
    expect_dropped_whole("\033[?2;;480;312S");     // EMPTY status: nums[1]==0 must not read as success
    expect_dropped_whole("\033[?2;0;1000S");       // short of its four parameters
    expect_dropped_whole("\033[?2;0;10;10;2S");    // a fifth parameter
    expect_dropped_whole("\033[?2;0;0;100S");      // zero is not a size
    expect_dropped_whole("\033[2;0;100;100S");     // no private marker
    expect_dropped_whole("\033[?2;0;1000001;10S"); // past the accumulation cap
    expect_dropped_whole("\033[?2;0;10:10;5S");    // sub-parameter colon
    expect_dropped_whole("\033[?S");               // no parameters at all
    expect_dropped_whole("\033[S");                // bare final (scroll-up CSI)
}

TEST(parse_input, device_attributes_reply_consumed_whole)
{
    // A real xterm DA1 response. It far exceeds any short parameter cap, and its
    // tail is live bindings: '2'/'3' switch shading, 'c' cycles wireframe colour.
    expect_dropped_whole("\033[?62;1;2;6;7;8;9;15;16;17;18;21;22;23;24;42;44;45;46c");
}

TEST(parse_input, linux_console_function_keys_dropped)
{
    // \033[[A..E. The second '[' is 0x5B, itself a legal CSI final, so a generic
    // scan would stop there and leak the letter ('A' orbits, 'C' cycles colour).
    expect_dropped_whole("\033[[A");
    expect_dropped_whole("\033[[C");
    expect_dropped_whole("\033[[E");
    expect_every_prefix_incomplete("\033[[A");
}

TEST(parse_input, x10_mouse_report_consumed_by_count)
{
    // \033[M + 3 payload bytes. 'M' is a legal CSI final, so the payload must be
    // counted, not scanned: column 49 encodes as 'Q' and would quit.
    expect_dropped_whole("\033[M\x20\x51\x21");
    expect_dropped_whole("\033[M\x20\x72\x21"); // 'r' would reset the view
    // X10 coordinates may contain any byte, including NUL and ESC after wrapping.
    // Treating ESC as a boundary would leak the remaining coordinates as keybindings.
    expect_dropped_whole(std::string("\033[M\x20\x00Q", 6));
    expect_dropped_whole("\033[M\x20\x1b\x21");
    expect_dropped_whole("\033[M\x20\x51\x1b"); // ESC as the y coordinate
    expect_every_prefix_incomplete("\033[M\x20\x51\x21");
}

TEST(parse_input, string_sequences_consumed_through_terminator)
{
    // OSC/DCS/SOS/PM/APC carry a text payload ended by BEL or ST. Their payloads
    // are arbitrary text, so any byte left behind dispatches a live binding.
    // BEL ends an OSC: that shorthand is an xterm convenience for this one family.
    expect_dropped_whole("\033]0;quit me\a");
    // The other four families are ST-terminated only. A 0x07 in their payload is
    // ordinary data, so treating it as the end would cut the reply short and
    // dispatch the remainder as keypresses.
    expect_dropped_whole("\033X status\033\\");
    expect_dropped_whole("\033^private\033\\");
    expect_dropped_whole("\033_app\033\\");
    expect_dropped_whole("\033Pdata\awith bel\033\\"); // BEL survives as payload
    ASSERT_EQ(parse("\033X status\a").kind, PK::Incomplete);
    // ST is two bytes (ESC backslash) and both are consumed, so nothing trails.
    expect_dropped_whole("\033P1$r0m\033\\");
    expect_dropped_whole("\033]11;rgb:1e1e/1e1e/1e1e\033\\");
    expect_every_prefix_incomplete("\033]0;title\a");
}

TEST(parse_input, long_string_payload_consumed_whole)
{
    // A clipboard reply is far longer than any sequence a fixed scan cap would
    // tolerate; truncating it would dispatch the remainder, 'q' included.
    expect_dropped_whole("\033]52;c;" + std::string(400, 'A') + "q\a");
}

TEST(parse_input, alt_key_chords_dropped)
{
    expect_dropped_whole("\033x");
    expect_dropped_whole("\033z");
}

TEST(parse_input, alt_arrow_reports_the_arrow)
{
    // altSendsEscape prefixes a whole sequence with a second ESC. Dropping just the
    // prefix lets the arrow parse normally; consuming both bytes would leave "[A"
    // to dispatch as a literal bracket followed by an orbit.
    const platform::detail::ParseResult r = parse("\033\033[A");
    ASSERT_EQ(r.kind, PK::Drop);
    ASSERT_EQ(r.consumed, 1);
    expect_key("\033[A", platform::Key::Up, 3);
}

TEST(parse_input, sgr_mouse_press_and_release)
{
    const platform::detail::ParseResult press = parse("\033[<0;5;6M");
    ASSERT_EQ(press.kind, PK::Complete);
    ASSERT_EQ(press.event.type, platform::InputEvent::Type::MousePress);
    ASSERT_EQ(press.event.x, 5);
    ASSERT_EQ(press.event.y, 6);

    const platform::detail::ParseResult rel = parse("\033[<0;5;6m");
    ASSERT_EQ(rel.kind, PK::Complete);
    ASSERT_EQ(rel.event.type, platform::InputEvent::Type::MouseRelease);

    const platform::detail::ParseResult wheel = parse("\033[<64;1;1M");
    ASSERT_EQ(wheel.kind, PK::Complete);
    ASSERT_EQ(wheel.event.type, platform::InputEvent::Type::ScrollUp);

    const platform::detail::ParseResult drag = parse("\033[<32;7;8M");
    ASSERT_EQ(drag.kind, PK::Complete);
    ASSERT_EQ(drag.event.type, platform::InputEvent::Type::MouseMove);

    expect_every_prefix_incomplete("\033[<0;5;6M");
}

TEST(parse_input, sgr_mouse_oversized_params_are_rejected)
{
    // Bound the integer accumulator without reporting a clamped coordinate that
    // main.cpp would accept as a drag origin.
    expect_dropped_whole("\033[<0;99999999999999999999;6M");
}

TEST(parse_input, sgr_mouse_scans_to_its_final_before_deciding)
{
    // Every CSI arm consumes through its terminator so malformed tails cannot
    // escape as keybindings.
    expect_dropped_whole("\033[<0 1;2M");   // stray space among the parameters
    expect_dropped_whole("\033[<0;5:6;7M"); // sub-parameter separator
    expect_dropped_whole("\033[<0;5;6#M");  // CSI intermediate byte
    expect_dropped_whole("\033[<0;5;6;9M"); // a fourth parameter
    expect_dropped_whole("\033[<0;5;6A");   // terminated, but not a mouse final
}

TEST(parse_input, malformed_sgr_mouse_is_dropped_not_reported)
{
    // A private-marker CSI that never reaches M/m must not fall through to the
    // arrow dispatch and report a movement.
    expect_dropped_whole("\033[<0;1;1A");

    // Properly terminated but short of its three parameters. Reporting one of
    // these hands main.cpp a press at the zero-initialized (0,0), which becomes the
    // drag origin, so the next genuine motion snaps the camera the whole way.
    expect_dropped_whole("\033[<M");
    expect_dropped_whole("\033[<0M");
    expect_dropped_whole("\033[<0;5M");
    expect_dropped_whole("\033[<;;M"); // separators present, values absent
    // The valid form still reports, so the check cannot be over-eager.
    const platform::detail::ParseResult ok = parse("\033[<0;5;6M");
    ASSERT_EQ(ok.kind, PK::Complete);
    ASSERT_EQ(ok.event.x, 5);
    ASSERT_EQ(ok.event.y, 6);
}

TEST(parse_input, a_truncated_sequence_never_swallows_the_next_one)
{
    // ESC starts the next sequence. Consuming it here would strip that introducer
    // and dispatch the following body byte by byte.
    struct Case
    {
        const char *bytes;
        int consumed;
    };
    const Case cases[] = {
        { "\033[1\033[A", 3 },    // generic CSI scan
        { "\033O1\033[A", 3 },    // parameterized SS3 scan
        { "\033[<0;1\033[A", 6 }, // SGR mouse scan
        // The counted arms need the rule as much as the scanning ones: their fixed
        // length would otherwise consume the next sequence's introducer outright.
        { "\033O\033[A", 2 },  // single-shot SS3
        { "\033[[\033[A", 3 }, // Linux VC F1-F5
    };
    for (const Case &c : cases)
    {
        const std::string s(c.bytes);
        const platform::detail::ParseResult r = parse(s);
        ASSERT_EQ(r.kind, PK::Drop);
        ASSERT_EQ(r.consumed, c.consumed);
        // What is left must be exactly the chasing sequence, which then parses
        // normally rather than as loose keypresses.
        expect_key(s.substr(static_cast<size_t>(r.consumed)), platform::Key::Up, 3);
    }
}

TEST(parse_input, a_string_sequence_treats_an_embedded_esc_as_payload)
{
    // ESC ends CSI parameters but not an arbitrary string payload. Ending a string
    // there would dispatch the rest of a split reply as keybindings.
    ASSERT_EQ(parse("\033]0;abc\033[A").kind, PK::Incomplete);
    ASSERT_EQ(parse("\033]52;c;AAA\033[<64;1;1MAAA").kind, PK::Incomplete);

    // Only its own terminator ends it, and then it is consumed whole.
    expect_dropped_whole("\033]0;abc\033[A\a");
    expect_dropped_whole("\033P q \033[<64;1;1M more\033\\");
    // ST still wins over the payload rule, since ST is how these end.
    expect_dropped_whole("\033]0;abc\033\\");
}

TEST(parse_input, sgr_wheel_is_detected_through_modifier_bits)
{
    // Modifiers ride in bits 2-4, so an equality test against 64/65 misses
    // ctrl+wheel (80/81) and shift+wheel (68/69) and reports them as drags, which
    // snaps the camera by the delta from a stale drag position.
    struct Case
    {
        const char *bytes;
        platform::InputEvent::Type type;
    };
    const Case cases[] = {
        { "\033[<64;1;1M", platform::InputEvent::Type::ScrollUp },
        { "\033[<65;1;1M", platform::InputEvent::Type::ScrollDown },
        { "\033[<80;1;1M", platform::InputEvent::Type::ScrollUp },   // ctrl+wheel up
        { "\033[<81;1;1M", platform::InputEvent::Type::ScrollDown }, // ctrl+wheel down
        { "\033[<68;1;1M", platform::InputEvent::Type::ScrollUp },   // shift+wheel up
        { "\033[<69;1;1M", platform::InputEvent::Type::ScrollDown }, // shift+wheel down
    };
    for (const Case &c : cases)
    {
        const platform::detail::ParseResult r = parse(c.bytes);
        ASSERT_EQ(r.kind, PK::Complete);
        ASSERT_EQ(r.event.type, c.type);
    }

    // The low two bits select the wheel axis, so only 0 and 1 are the vertical
    // pair. A horizontal wheel (buttons 6/7, encoded 66/67) has no binding and must
    // be consumed, not decoded as vertical scroll, which would zoom the camera.
    expect_dropped_whole("\033[<66;1;1M");
    expect_dropped_whole("\033[<67;1;1M");

    // A wheel notch is a press with no release, so the 'm' form is malformed.
    // Selecting the branch from the button bits alone would report it as a second
    // scroll and zoom twice per notch.
    expect_dropped_whole("\033[<64;1;1m");
    expect_dropped_whole("\033[<65;1;1m");

    // Motion requires a held button and the press form. Button bits 3 means no-button
    // motion; a motion-flagged release is malformed. Neither may orbit the model.
    expect_dropped_whole("\033[<35;5;6M"); // 32 + 3: motion, no button
    expect_dropped_whole("\033[<32;5;6m"); // motion flag on a release

    // SGR coordinates are 1-based, so a zero is not a position, and a value past
    // any terminal's width is not one either. Accepting one puts the drag origin
    // somewhere impossible and the next motion snaps the camera.
    expect_dropped_whole("\033[<0;0;6M");
    expect_dropped_whole("\033[<0;5;0M");
    expect_dropped_whole("\033[<0;999999;6M");
    expect_dropped_whole("\033[<0;5;999999M");

    // The button parameter is range-checked for the same reason as the coordinates:
    // the encoding is eight flag bits, so a larger value is not a button
    // description, and without the check it is bit-decoded anyway and selects an arm.
    expect_dropped_whole("\033[<999999;5;6M");
    expect_dropped_whole("\033[<256;5;6M");

    // "No button" is also not a press: there is no such thing as pressing nothing,
    // and letting it through seeds the drag origin from a report naming no button.
    expect_dropped_whole("\033[<3;5;6M");

    // But bit 7 selects the extended buttons 8-11, so with it set the same low bits
    // are button 11 and a drag with it is ordinary input, not a no-button motion.
    const platform::detail::ParseResult ext = parse("\033[<163;5;6M"); // 128 + 32 + 3
    ASSERT_EQ(ext.kind, PK::Complete);
    ASSERT_EQ(ext.event.type, platform::InputEvent::Type::MouseMove);
    const platform::detail::ParseResult ext_press = parse("\033[<131;5;6M"); // 128 + 3
    ASSERT_EQ(ext_press.kind, PK::Complete);
    ASSERT_EQ(ext_press.event.type, platform::InputEvent::Type::MousePress);
}

TEST(parse_input, grammar_properties_hold_over_every_short_byte_string)
{
    // Exhaustively check the buffered parser's invariants over every meaningful byte:
    // consumed stays in bounds, Incomplete consumes nothing, and appending a byte
    // cannot change a completed decision. Depth 4 costs about one million parses.
    const char alphabet[] = { '\033', '[', ']', 'O', 'M', '<', ';', '0', 'A', 'q', '\a', '\\', 'P', '\0', ' ' };
    const auto same_event = [](const platform::InputEvent &a, const platform::InputEvent &b)
    { return a.type == b.type && a.key == b.key && a.x == b.x && a.y == b.y; };

    // Explicit stack rather than recursion: clang-tidy rejects a recursive lambda
    // here, and the traversal is the incidental part of the case.
    std::vector<std::string> stack = { std::string() };
    while (!stack.empty())
    {
        const std::string s = stack.back();
        stack.pop_back();
        const platform::detail::ParseResult r = parse(s);

        ASSERT_TRUE(r.consumed >= 0);
        ASSERT_TRUE(r.consumed <= static_cast<int>(s.size()));
        ASSERT_EQ(r.kind == PK::Incomplete, r.consumed == 0);

        if (r.kind != PK::Incomplete)
        {
            for (char extra : alphabet)
            {
                const platform::detail::ParseResult r2 = parse(s + extra);
                ASSERT_EQ(r2.kind, r.kind);
                ASSERT_EQ(r2.consumed, r.consumed);
                ASSERT_TRUE(same_event(r2.event, r.event));
            }
        }
        if (s.size() < 4)
        {
            for (char c : alphabet)
            {
                stack.push_back(s + c);
            }
        }
    }
}

TEST(parse_input, consumed_length_never_exceeds_input)
{
    // The buffer invariant poll_event relies on: a decision may never claim more
    // bytes than it was given, or the buffer compaction would underflow.
    const char *cases[] = { "q",          "\033",         "\033[",    "\033[A",     "\033O",
                            "\033OA",     "\033OP",       "\033[15~", "\033[[A",    "\033[M\x20\x51\x21",
                            "\033]0;t\a", "\033P1\033\\", "\033x",    "\033\033[A", "\033[<0;5;6M" };
    for (const char *c : cases)
    {
        const std::string s(c);
        const platform::detail::ParseResult r = parse(s);
        ASSERT_TRUE(r.consumed >= 0);
        ASSERT_TRUE(r.consumed <= static_cast<int>(s.size()));
        if (r.kind == PK::Incomplete)
        {
            ASSERT_EQ(r.consumed, 0);
        }
        else
        {
            ASSERT_TRUE(r.consumed > 0); // or poll_event's loop would not terminate
        }
    }
}

// poll_event integration
// Exercise buffering over a real POSIX fd. A pipe cannot emulate Windows console input.

#ifndef _WIN32
namespace
{
    // Swap a pipe onto stdin so poll_event can receive staged bursts. `ok` fails
    // loudly if a runner does not provide a dup-able fd 0.
    struct StdinFeed
    {
        int saved_stdin;
        int write_fd = -1;
        bool ok = false;
        StdinFeed() : saved_stdin(test_dup(STDIN_FILENO))
        {
            // Reset the whole persistent parser state, including skip and rate windows,
            // so cases cannot contaminate one another.
            platform::detail::pending() = platform::detail::Pending{};

            int fds[2] = { -1, -1 };
            if (saved_stdin < 0 || pipe(fds) != 0)
            {
                return;
            }
            write_fd = fds[1];
            ok = test_dup2(fds[0], STDIN_FILENO) >= 0;
            test_close(fds[0]);
        }
        // Return false on a short write so no case silently tests truncated input.
        [[nodiscard]] bool push(const std::string &bytes) const
        {
            const ssize_t n = write(write_fd, bytes.data(), bytes.size());
            return n >= 0 && static_cast<size_t>(n) == bytes.size();
        }
        ~StdinFeed()
        {
            if (write_fd >= 0)
            {
                test_close(write_fd);
            }
            if (saved_stdin >= 0)
            {
                test_dup2(saved_stdin, STDIN_FILENO);
                test_close(saved_stdin);
            }
        }
        StdinFeed(const StdinFeed &) = delete;
        StdinFeed &operator=(const StdinFeed &) = delete;
        StdinFeed(StdinFeed &&) = delete;
        StdinFeed &operator=(StdinFeed &&) = delete;
    };

    platform::InputEvent::Type next_type()
    {
        return platform::poll_event().type;
    }
} // namespace

TEST(poll_event, reads_a_key_from_the_stream)
{
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
}

TEST(poll_event, drains_every_event_in_one_burst)
{
    // Type::None must mean "nothing left", not "I consumed something unbound":
    // a dropped sequence in the middle may not cut the drain short, or a caller
    // draining once per frame would retire only one sequence per frame.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033[15~q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
}

TEST(poll_event, reassembles_a_sequence_split_across_calls)
{
    // The point of buffering: a sequence delivered in pieces is held until it is
    // whole, instead of being decided on the bytes that arrived first.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("["));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("A"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Up);
}

TEST(poll_event, stalled_partial_is_discarded_whole_not_dispatched)
{
    // A sequence that stops arriving must be dropped in its entirety once it stops
    // growing. Dispatching what did arrive is exactly the bug this design removes:
    // "\033[1" would otherwise surface '1' and switch shading mode.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033[1"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    std::this_thread::sleep_for(std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS * 2));

    // Discard only after a poll receives no bytes; a growing partial may still be
    // in flight, and abandoning it would dispatch its tail.
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
}

TEST(poll_event, reassembly_survives_a_frame_slower_than_the_timeout)
{
    // Judge staleness only after a poll without growth. Otherwise a slow frame could
    // abandon a sequence while bytes arrive and dispatch its tail as keys.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    std::this_thread::sleep_for(std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS * 2));
    ASSERT_TRUE(in.push("[A"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Up);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
}

TEST(poll_event, over_length_sequence_is_consumed_to_its_terminator)
{
    // An over-length sequence keeps its introducer, drops the middle, and scans to
    // the family terminator without dispatching payload. No timing guess is involved.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const int n = platform::detail::MAX_PENDING;
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(n) * 3, 'q')));
    for (int frame = 0; frame < 8; frame++)
    {
        ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    }

    // The terminator ends it, and ordinary input works again immediately.
    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, over_length_sequence_survives_any_delivery_pattern)
{
    // Deliver the same over-length payload as a full buffer, 400-byte writes, and a
    // trickle. All must enter skip mode and wait for the terminator, independent of chunking.
    for (int chunk : { platform::detail::MAX_PENDING, 400, 8 })
    {
        StdinFeed in;
        ASSERT_TRUE(in.ok);
        ASSERT_TRUE(in.push("\033]52;c;"));
        const int chunks = (3 * platform::detail::MAX_PENDING) / chunk;
        for (int i = 0; i < chunks; i++)
        {
            ASSERT_TRUE(in.push(std::string(static_cast<size_t>(chunk), 'q')));
            ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
        }
        ASSERT_TRUE(platform::detail::pending().skipping);
        ASSERT_TRUE(in.push("\a"));
        ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
        ASSERT_TRUE(in.push("q"));
        const platform::InputEvent ev = platform::poll_event();
        ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
        ASSERT_EQ(ev.key, platform::Key::Q);
    }
}

TEST(poll_event, over_length_sequence_survives_gaps_between_chunks)
{
    // Simulate gaps by ageing last_growth before each chunk. Skip mode must survive
    // quiet intervals rather than parse the next payload chunk as fresh key input.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'q')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    for (int chunk = 0; chunk < 6; chunk++)
    {
        platform::detail::pending().last_growth =
            std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
        // Hold the unrelated rate window open so scheduler stalls cannot make this
        // deterministic gap simulation flaky.
        platform::detail::pending().meter.window = std::chrono::steady_clock::now();
        ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
        ASSERT_TRUE(in.push(std::string(400, 'q')));
        ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    }

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_stalled_long_partial_becomes_a_skip_not_a_dispatch)
{
    // Promote a stalled, keypress-long partial to skip mode before discarding its
    // buffered payload. Short fragmented keys still time out normally.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(400, 'q')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping); // skipping, not dispatched

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_sequence_arriving_inside_a_skip_does_not_end_it)
{
    // An ordinary ESC sequence may arrive between chunks of a long string reply.
    // Swallow it as payload rather than ending the skip and dispatching the remainder.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const int n = platform::detail::MAX_PENDING;
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(n) * 2, 'A')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // The drag report, then the rest of the reply, then its terminator.
    ASSERT_TRUE(in.push("\033[<32;40;12M" + std::string("AAqAA")));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None); // neither the drag nor the 'q'
    ASSERT_TRUE(platform::detail::pending().skipping);        // the reply has not ended

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    // And ordinary input works again the moment the reply really is over.
    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_sequence_arriving_inside_a_skipped_csi_parses_whole)
{
    // ESC cannot belong to CSI payload, so it ends the skip and remains available
    // as the next sequence's introducer.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const int n = platform::detail::MAX_PENDING;
    ASSERT_TRUE(in.push("\033[" + std::string(static_cast<size_t>(n) * 2, '1')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // An X10 report, whose payload byte is 'q' (a click at column 81). Consumed whole
    // and reported as nothing, rather than surfacing its payload as keypresses.
    ASSERT_TRUE(in.push(std::string("\033[M\x20\x71\x21", 6)));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    // And ordinary input works immediately afterwards.
    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_skipped_csi_still_ends_at_its_own_final_byte)
{
    // With no ESC involved, the skip ends where the CSI does.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const int n = platform::detail::MAX_PENDING;
    ASSERT_TRUE(in.push("\033[" + std::string(static_cast<size_t>(n) * 2, '1')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("~")); // a final byte ends it
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);
}

TEST(poll_event, a_terminator_split_across_the_buffer_boundary_still_ends_the_skip)
{
    // Preserve a trailing ESC when collapsing an over-length ST sequence; its
    // following backslash must still terminate the skip.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const auto n = static_cast<size_t>(platform::detail::MAX_PENDING);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(n - 8, 'A') + "\033")); // exactly a full buffer, ending on ESC
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("\\")); // the other half of ST, arriving after the collapse
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, an_over_long_csi_does_not_resume_as_a_plain_arrow)
{
    // A skipped sequence may locate its terminator but must never resume decoding;
    // payload bytes in the A-D range must not become arrows.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const auto n = static_cast<size_t>(platform::detail::MAX_PENDING);
    ASSERT_TRUE(in.push("\033[" + std::string(n - 2, '0')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("A")); // a legal CSI final, and the byte an Up arrow ends with
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_backlog_of_unbound_sequences_clears_at_the_rate_it_arrived)
{
    // Drain a backlog of unbound sequences in one pass; returning None after each
    // bufferful would delay the live key behind it by many frames.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    std::string backlog;
    while (backlog.size() < static_cast<size_t>(16) * 1024)
    {
        backlog += "\033[15~"; // F5, consumed and dropped
    }
    ASSERT_TRUE(in.push(backlog));
    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q); // one call, not one per bufferful
}

TEST(poll_event, a_skipped_sequence_never_reports_an_event)
{
    // Never decode a fragment after skip mode dropped its middle. A mouse tail can
    // otherwise parse as a complete report with fabricated coordinates.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033[<0;" + std::string(70, '9'))); // past MAX_KEY_SEQUENCE, then stalls
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // Exactly three parameters and a final: a valid report on its own terms.
    ASSERT_TRUE(in.push("32;40;12M"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping); // ended, but reported nothing

    ASSERT_TRUE(in.push("q")); // and ordinary input works immediately after
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, the_rate_floor_charges_for_every_window_that_elapsed)
{
    // Charge every elapsed rate window. Charging once per poll would make the floor
    // depend on frame rate and let autorepeat sustain a skip.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'x')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // Credit one autorepeat-speed burst directly. It clears one window's quota but
    // cannot cover all five elapsed windows.
    const int one_frame_of_typing = 30 * 5; // ~30 B/s over a 5 s frame
    ASSERT_TRUE(one_frame_of_typing > platform::detail::RATE_QUOTA);
    platform::detail::pending().meter.credit = one_frame_of_typing;
    platform::detail::pending().meter.window =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(5 * platform::detail::RATE_WINDOW_MS);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping); // charged for every window
}

TEST(poll_event, the_arrival_credit_saturates_instead_of_overflowing)
{
    // Saturate accumulated credit before it can overflow negative. Leave room for
    // the largest quota charged by one meter update.
    static_assert(
        platform::detail::RATE_MAX_CREDIT >= platform::detail::RATE_MAX_WINDOWS * platform::detail::RATE_QUOTA,
        "saturated credit must still be able to meet the largest quota a tick charges"
    );
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;"));
    for (int i = 0; i < 8; i++)
    {
        ASSERT_TRUE(in.push(std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'q')));
        ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
        ASSERT_TRUE(platform::detail::pending().meter.credit <= platform::detail::RATE_MAX_CREDIT);
        ASSERT_TRUE(platform::detail::pending().meter.credit >= 0);
    }
}

TEST(poll_event, end_input_pass_releases_the_read_budget)
{
    // The budget is per drain pass, and poll_event can only release it where it
    // reports Type::None. A caller that leaves its loop any other way (an event cap,
    // a quit key) has to say so, or the next pass finds the budget already spent.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    platform::detail::pending().refills = platform::detail::MAX_REFILLS_PER_PASS;
    platform::end_input_pass();
    ASSERT_EQ(platform::detail::pending().refills, 0);

    // And with it released, a read happens again on the very next call.
    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_spent_read_budget_does_not_abandon_a_live_partial)
{
    // With the budget gone, "no bytes arrived" is not something poll_event has looked
    // for, so none of the rules below the read may run: they would abandon a partial
    // whose rest is already queued.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_EQ(platform::detail::pending().len, 1);

    platform::detail::pending().refills = platform::detail::MAX_REFILLS_PER_PASS;
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_EQ(platform::detail::pending().len, 1); // held, not discarded
    // Hitting the cap is itself the end of the pass, so that same call released the
    // budget on its way out. Asserted rather than assumed, because the next half
    // depends on it and would otherwise be testing nothing.
    ASSERT_EQ(platform::detail::pending().refills, 0);

    // With the budget back, the same stale partial is judged and abandoned.
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_EQ(platform::detail::pending().len, 0);
}

TEST(poll_event, a_skip_promoted_from_a_stalled_partial_inherits_the_measured_rate)
{
    // A stalled partial enters skip mode after an empty read. Seed its rate from the
    // independent arrival meter, not that empty read, or the first window drops it.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(400, 'q'))); // well under MAX_PENDING: never overflows
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // First window of the skip closes. Those 400 bytes are what has to carry it.
    platform::detail::pending().meter.window =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::RATE_WINDOW_MS + 1);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, the_rate_floor_measures_a_rate_not_presence_in_each_window)
{
    // Carry burst surplus across empty windows so the floor measures average rate,
    // not whether every window received bytes.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'q')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // Two windows with nothing in them, aged rather than waited out. A burst that big
    // has to survive both.
    for (int window = 0; window < 2; window++)
    {
        platform::detail::pending().meter.window =
            std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::RATE_WINDOW_MS + 1);
        ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
        ASSERT_TRUE(platform::detail::pending().skipping);
    }

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, an_unterminated_sequence_is_given_up_on_when_it_stops_arriving)
{
    // Only the rate floor ends an unterminated skip; a size cap would dispatch the
    // first larger reply's tail. Below the floor, abandon the skip so typing resumes.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'q')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // Delivery stops: the window ages out with nothing to show for it. Aged rather
    // than waited out, so the case costs no real time.
    platform::detail::pending().meter.window =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::RATE_WINDOW_MS + 1);
    platform::detail::pending().meter.credit = 0;
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_skip_advances_at_the_rate_bytes_arrive_not_one_buffer_per_call)
{
    // Consume multiple bufferfuls per drain pass so a large reply does not advance
    // at the frame rate. Assert parser state rather than wall time.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const auto n = static_cast<size_t>(platform::detail::MAX_PENDING);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(n * 8, 'q') + "\a"));
    // One call is enough to consume all eight buffersful and the terminator behind
    // them, so nothing of the reply is left to hold up the next keypress.
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, never_blocks_on_an_incomplete_sequence)
{
    // A lone ESC must return without an inter-byte wait. Measure many calls so the
    // budget tolerates scheduler jitter but remains far below blocking behavior.
    constexpr int CALLS = 20;
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033"));
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < CALLS; i++)
    {
        ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(ms < (CALLS * platform::detail::PARTIAL_TIMEOUT_MS) / 10);
}
#endif
