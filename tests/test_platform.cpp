#include "tests/test.h"
#include "src/platform/input.h"
#include "src/platform/platform.h"
#include "src/terminal/framebuffer.h"

// <stdlib.h> declares the POSIX pty and cross-platform environment functions in
// the global namespace; <cstdlib> need not.
#include <stdlib.h> // NOLINT(modernize-deprecated-headers,hicpp-deprecated-headers)

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
#include "src/args.h"
#include "src/version.h"
#include <atomic>
// platform.h applies NOMINMAX and removes the near/far macros before this direct include.
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

    struct ScopedStdoutCapture
    {
        std::FILE *capture = nullptr;
        int saved = -1;
        bool valid = false;

        ScopedStdoutCapture() : capture(std::tmpfile()), saved(test_dup(TEST_STDOUT))
        {
            std::fflush(stdout);
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
            for (;;)
            {
                const size_t count = std::fread(buffer, 1, sizeof buffer, capture);
                text.append(buffer, count);
                if (count < sizeof buffer)
                {
                    break;
                }
            }
            return text;
        }

        ScopedStdoutCapture(const ScopedStdoutCapture &) = delete;
        ScopedStdoutCapture &operator=(const ScopedStdoutCapture &) = delete;
        ScopedStdoutCapture(ScopedStdoutCapture &&) = delete;
        ScopedStdoutCapture &operator=(ScopedStdoutCapture &&) = delete;
    };

#ifdef _WIN32
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
        platform::detail::console_input_wake_enabled.store(false);
        for (HANDLE output : { pipes.output_write, console.output })
        {
            ASSERT_TRUE(SetStdHandle(STD_OUTPUT_HANDLE, output) != 0);
            ASSERT_FALSE(platform::enable_raw_mode());
            ASSERT_FALSE(platform::detail::console_input_wake_enabled.load());
            ASSERT_FALSE(platform::enable_vt_input());
            bool canceled = true;
            ASSERT_FALSE(platform::enable_mouse(true, &canceled));
            ASSERT_FALSE(canceled);
            const TermGraphics query = platform::query_term_graphics();
            ASSERT_TRUE(query.failed);
            ASSERT_FALSE(query.interrupted);
        }
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
        ASSERT_TRUE(platform::enable_vt_input());
        ASSERT_TRUE(GetConsoleMode(console.input, &changed_input_mode) != 0);
        ASSERT_EQ(changed_input_mode, baseline_input_mode | ENABLE_VIRTUAL_TERMINAL_INPUT);
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
namespace
{
    enum class PartialTermiosField : std::uint8_t
    {
        None,
        Echo,
        Canonical,
        MinimumBytes,
        Timeout,
        InputFlags,
        OutputFlags,
        ControlFlags,
        SignalFlag,
        ControlCharacter,
        InputSpeed,
        OutputSpeed,
    };

    struct TermiosCalls
    {
        termios captured = {};
        termios current = {};
        termios writes[8] = {};
        int get_errors[8] = {};
        int set_errors[8] = {};
        int flush_errors[8] = {};
        int partial_set_call = -1;
        PartialTermiosField partial_field = PartialTermiosField::None;
        int get_calls = 0;
        int set_calls = 0;
        int set_calls_with_sigttou_blocked = 0;
        int flush_calls = 0;
        bool flush_clears_pending_input = false;
    };

    TermiosCalls &termios_calls()
    {
        static TermiosCalls calls;
        return calls;
    }

    void reset_termios_calls()
    {
        termios_calls() = {};
        termios_calls().captured.c_lflag = static_cast<tcflag_t>(ECHO | ICANON | ISIG);
        termios_calls().captured.c_cc[VMIN] = 1;
        termios_calls().captured.c_cc[VTIME] = 7;
        termios_calls().current = termios_calls().captured;
    }

    int scripted_tcgetattr(int fd, termios *value)
    {
        TermiosCalls &calls = termios_calls();
        if ((fd != STDIN_FILENO && fd != STDOUT_FILENO) || calls.get_calls >= 8)
        {
            errno = EINVAL;
            return -1;
        }
        const int error = calls.get_errors[calls.get_calls++];
        if (error != 0)
        {
            errno = error;
            return -1;
        }
        *value = calls.current;
        return 0;
    }

    int scripted_tcsetattr(int fd, int action, const termios *value)
    {
        TermiosCalls &calls = termios_calls();
        if ((fd != STDIN_FILENO && fd != STDOUT_FILENO) || action != TCSANOW || calls.set_calls >= 8)
        {
            errno = EINVAL;
            return -1;
        }
        const int call = calls.set_calls++;
        sigset_t blocked = {};
        if (pthread_sigmask(SIG_SETMASK, nullptr, &blocked) == 0 && sigismember(&blocked, SIGTTOU) == 1)
        {
            calls.set_calls_with_sigttou_blocked++;
        }
        calls.writes[call] = *value;
        const int error = calls.set_errors[call];
        if (error != 0)
        {
            errno = error;
            return -1;
        }
        const termios previous = calls.current;
        calls.current = *value;
        if (call == calls.partial_set_call)
        {
            switch (calls.partial_field)
            {
            case PartialTermiosField::Echo:
                calls.current.c_lflag = (calls.current.c_lflag & ~static_cast<tcflag_t>(ECHO)) |
                                        (previous.c_lflag & static_cast<tcflag_t>(ECHO));
                break;
            case PartialTermiosField::Canonical:
                calls.current.c_lflag = (calls.current.c_lflag & ~static_cast<tcflag_t>(ICANON)) |
                                        (previous.c_lflag & static_cast<tcflag_t>(ICANON));
                break;
            case PartialTermiosField::MinimumBytes:
                calls.current.c_cc[VMIN] = previous.c_cc[VMIN];
                break;
            case PartialTermiosField::Timeout:
                calls.current.c_cc[VTIME] = previous.c_cc[VTIME];
                break;
            case PartialTermiosField::InputFlags:
                calls.current.c_iflag = previous.c_iflag;
                break;
            case PartialTermiosField::OutputFlags:
                calls.current.c_oflag = previous.c_oflag;
                break;
            case PartialTermiosField::ControlFlags:
                calls.current.c_cflag = previous.c_cflag;
                break;
            case PartialTermiosField::SignalFlag:
                calls.current.c_lflag = (calls.current.c_lflag & ~static_cast<tcflag_t>(ISIG)) |
                                        (previous.c_lflag & static_cast<tcflag_t>(ISIG));
                break;
            case PartialTermiosField::ControlCharacter:
                calls.current.c_cc[VINTR] = previous.c_cc[VINTR];
                break;
            case PartialTermiosField::InputSpeed:
                cfsetispeed(&calls.current, cfgetispeed(&previous));
                break;
            case PartialTermiosField::OutputSpeed:
                cfsetospeed(&calls.current, cfgetospeed(&previous));
                break;
            case PartialTermiosField::None:
                break;
            }
        }
        return 0;
    }

    int scripted_tcflush(int fd, int queue)
    {
        TermiosCalls &calls = termios_calls();
        if (fd != STDIN_FILENO || queue != TCIFLUSH || calls.flush_calls >= 8)
        {
            errno = EINVAL;
            return -1;
        }
        errno = calls.flush_errors[calls.flush_calls++];
#ifdef PENDIN
        if (errno == 0 && calls.flush_clears_pending_input)
        {
            calls.current.c_lflag &= ~static_cast<tcflag_t>(PENDIN);
        }
#endif
        return errno == 0 ? 0 : -1;
    }

    bool alter_termios_field(termios &mode, PartialTermiosField field)
    {
        switch (field)
        {
        case PartialTermiosField::InputFlags:
            mode.c_iflag ^= IXON;
            return true;
        case PartialTermiosField::OutputFlags:
            mode.c_oflag ^= OPOST;
            return true;
        case PartialTermiosField::ControlFlags:
            mode.c_cflag ^= CLOCAL;
            return true;
        case PartialTermiosField::SignalFlag:
            mode.c_lflag ^= ISIG;
            return true;
        case PartialTermiosField::ControlCharacter:
            mode.c_cc[VINTR] = mode.c_cc[VINTR] == 0 ? static_cast<cc_t>(1) : static_cast<cc_t>(0);
            return true;
        case PartialTermiosField::InputSpeed:
            return cfsetispeed(&mode, cfgetispeed(&mode) == B9600 ? B4800 : B9600) == 0;
        case PartialTermiosField::OutputSpeed:
            return cfsetospeed(&mode, cfgetospeed(&mode) == B9600 ? B4800 : B9600) == 0;
        case PartialTermiosField::None:
        case PartialTermiosField::Echo:
        case PartialTermiosField::Canonical:
        case PartialTermiosField::MinimumBytes:
        case PartialTermiosField::Timeout:
            return true;
        }
        return false;
    }

    bool termios_equal(const termios &a, const termios &b)
    {
        return a.c_iflag == b.c_iflag && a.c_oflag == b.c_oflag && a.c_cflag == b.c_cflag && a.c_lflag == b.c_lflag &&
               std::memcmp(a.c_cc, b.c_cc, sizeof a.c_cc) == 0 && cfgetispeed(&a) == cfgetispeed(&b) &&
               cfgetospeed(&a) == cfgetospeed(&b);
    }
} // namespace

TEST(platform, posix_raw_mode_resume_captures_shell_changes)
{
    reset_termios_calls();
    platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
    ASSERT_TRUE(platform::enable_raw_mode(&guard));
    ASSERT_TRUE(platform::disable_raw_mode(&guard));
    termios_calls().current.c_cc[VINTR] = static_cast<cc_t>(7);
    const termios shell_mode = termios_calls().current;
    ASSERT_TRUE(platform::resume_raw_mode(guard));
    ASSERT_TRUE(platform::disable_raw_mode(&guard));
    ASSERT_TRUE(termios_equal(termios_calls().current, shell_mode));
}

TEST(platform, posix_raw_mode_rejects_failed_capture_without_writing)
{
    reset_termios_calls();
    termios_calls().get_errors[0] = EIO;

    platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
    ASSERT_FALSE(guard.valid());
    ASSERT_FALSE(platform::enable_raw_mode(&guard));
    ASSERT_EQ(termios_calls().get_calls, 1);
    ASSERT_EQ(termios_calls().set_calls, 0);
}

TEST(platform, posix_raw_mode_flushes_input_without_draining_output)
{
    reset_termios_calls();
    termios_calls().flush_errors[0] = EINTR;
    platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
    ASSERT_TRUE(guard.enable_raw_mode());
    ASSERT_EQ(termios_calls().flush_calls, 2);
    ASSERT_TRUE(guard.restore_raw_mode());
    ASSERT_EQ(termios_calls().flush_calls, 3);
}

TEST(platform, posix_raw_mode_rolls_back_failed_input_flush)
{
    reset_termios_calls();
    termios_calls().flush_errors[0] = EIO;
    platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
    ASSERT_FALSE(guard.enable_raw_mode());
    ASSERT_TRUE(termios_equal(termios_calls().current, termios_calls().captured));
    ASSERT_FALSE(guard.raw_mode_restore_pending());
}

#ifdef PENDIN
TEST(platform, posix_raw_mode_restores_settings_after_pending_input_is_flushed)
{
    reset_termios_calls();
    termios_calls().captured.c_lflag |= PENDIN;
    termios_calls().current = termios_calls().captured;
    termios_calls().flush_clears_pending_input = true;
    platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
    ASSERT_TRUE(guard.enable_raw_mode());
    ASSERT_TRUE(guard.restore_raw_mode());
    ASSERT_FALSE(guard.raw_mode_restore_pending());
    termios expected = termios_calls().captured;
    expected.c_lflag &= ~static_cast<tcflag_t>(PENDIN);
    ASSERT_TRUE(termios_equal(termios_calls().current, expected));
}
#endif

TEST(platform, posix_raw_mode_retains_restore_after_failed_input_flush)
{
    reset_termios_calls();
    platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
    ASSERT_TRUE(guard.enable_raw_mode());
    termios_calls().flush_errors[1] = EIO;
    termios_calls().flush_errors[2] = EIO;
    ASSERT_FALSE(guard.restore_raw_mode());
    ASSERT_TRUE(guard.raw_mode_restore_pending());
    ASSERT_TRUE(termios_equal(termios_calls().current, termios_calls().captured));
    ASSERT_TRUE(guard.restore_raw_mode());
    ASSERT_FALSE(guard.raw_mode_restore_pending());
}

TEST(platform, posix_raw_mode_rolls_back_failed_write)
{
    reset_termios_calls();
    termios_calls().set_errors[0] = EIO;

    platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
    ASSERT_TRUE(guard.valid());
    ASSERT_FALSE(platform::enable_raw_mode(&guard));
    ASSERT_EQ(termios_calls().set_calls, 2);
    ASSERT_TRUE((termios_calls().writes[0].c_lflag & static_cast<tcflag_t>(ECHO | ICANON)) == 0);
    ASSERT_EQ(termios_calls().writes[0].c_cc[VMIN], static_cast<cc_t>(0));
    ASSERT_EQ(termios_calls().writes[0].c_cc[VTIME], static_cast<cc_t>(0));
    ASSERT_TRUE(termios_equal(termios_calls().writes[1], termios_calls().captured));
    ASSERT_FALSE(guard.raw_mode_restore_pending());
}

TEST(platform, posix_raw_mode_rolls_back_partial_write)
{
    static constexpr PartialTermiosField fields[] = {
        PartialTermiosField::Echo,
        PartialTermiosField::Canonical,
        PartialTermiosField::MinimumBytes,
        PartialTermiosField::Timeout,
    };
    for (const PartialTermiosField field : fields)
    {
        reset_termios_calls();
        termios_calls().partial_set_call = 0;
        termios_calls().partial_field = field;

        platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
        ASSERT_FALSE(platform::enable_raw_mode(&guard));
        ASSERT_EQ(termios_calls().get_calls, 3);
        ASSERT_EQ(termios_calls().set_calls, 2);
        ASSERT_TRUE(termios_equal(termios_calls().writes[1], termios_calls().captured));
        ASSERT_TRUE(termios_equal(termios_calls().current, termios_calls().captured));
        ASSERT_FALSE(guard.raw_mode_restore_pending());
    }
}

TEST(platform, posix_raw_mode_rolls_back_failed_verification)
{
    reset_termios_calls();
    termios_calls().get_errors[1] = EIO;

    platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
    ASSERT_FALSE(platform::enable_raw_mode(&guard));
    ASSERT_EQ(termios_calls().get_calls, 3);
    ASSERT_EQ(termios_calls().set_calls, 2);
    ASSERT_TRUE(termios_equal(termios_calls().current, termios_calls().captured));
    ASSERT_FALSE(guard.raw_mode_restore_pending());
}

TEST(platform, posix_raw_mode_retries_partial_restore)
{
    static constexpr PartialTermiosField fields[] = {
        PartialTermiosField::Echo,         PartialTermiosField::Canonical,   PartialTermiosField::MinimumBytes,
        PartialTermiosField::Timeout,      PartialTermiosField::InputFlags,  PartialTermiosField::OutputFlags,
        PartialTermiosField::ControlFlags, PartialTermiosField::SignalFlag,  PartialTermiosField::ControlCharacter,
        PartialTermiosField::InputSpeed,   PartialTermiosField::OutputSpeed,
    };
    for (const PartialTermiosField field : fields)
    {
        reset_termios_calls();

        platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
        ASSERT_TRUE(platform::enable_raw_mode(&guard));
        ASSERT_TRUE(alter_termios_field(termios_calls().current, field));
        termios_calls().partial_set_call = 1;
        termios_calls().partial_field = field;

        ASSERT_TRUE(platform::disable_raw_mode(&guard));
        ASSERT_EQ(termios_calls().set_calls, 3);
        ASSERT_TRUE(termios_equal(termios_calls().current, termios_calls().captured));
        ASSERT_FALSE(guard.raw_mode_restore_pending());
    }
}

TEST(platform, posix_raw_mode_verification_retries_eintr)
{
    reset_termios_calls();
    termios_calls().get_errors[1] = EINTR;

    platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
    ASSERT_TRUE(platform::enable_raw_mode(&guard));
    ASSERT_EQ(termios_calls().get_calls, 3);
    ASSERT_TRUE(platform::disable_raw_mode(&guard));
}

TEST(platform, posix_raw_mode_restore_retries_eintr)
{
    reset_termios_calls();
    termios_calls().set_errors[1] = EINTR;

    platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
    ASSERT_TRUE(platform::enable_raw_mode(&guard));
    ASSERT_TRUE(platform::disable_raw_mode(&guard));
    ASSERT_EQ(termios_calls().set_calls, 3);
    ASSERT_TRUE(termios_equal(termios_calls().writes[2], termios_calls().captured));
    ASSERT_FALSE(guard.raw_mode_restore_pending());
}

TEST(platform, posix_raw_mode_restore_makes_final_attempt)
{
    reset_termios_calls();
    termios_calls().set_errors[1] = EIO;

    platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
    ASSERT_TRUE(platform::enable_raw_mode(&guard));
    ASSERT_TRUE(platform::disable_raw_mode(&guard));
    ASSERT_EQ(termios_calls().set_calls, 3);
    ASSERT_TRUE(termios_equal(termios_calls().writes[2], termios_calls().captured));
    ASSERT_FALSE(guard.raw_mode_restore_pending());
}

TEST(platform, posix_raw_mode_keeps_failed_restore_pending)
{
    reset_termios_calls();
    termios_calls().set_errors[1] = EIO;
    termios_calls().set_errors[2] = EIO;

    {
        platform::ConsoleStateGuard guard(scripted_tcgetattr, scripted_tcsetattr, scripted_tcflush);
        ASSERT_TRUE(platform::enable_raw_mode(&guard));
        ASSERT_FALSE(platform::disable_raw_mode(&guard));
        ASSERT_EQ(termios_calls().set_calls, 3);
        ASSERT_TRUE(guard.raw_mode_restore_pending());
    }

    // The guard remains armed and retries after the session cleanup boundary.
    ASSERT_EQ(termios_calls().set_calls, 4);
    ASSERT_TRUE(termios_equal(termios_calls().writes[3], termios_calls().captured));
}

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

#ifndef _WIN32
namespace
{
    struct TerminalWriteScript
    {
        int64_t counts[6] = {};
        int errors[6] = {};
        int calls = 0;
        int suspend_at = -1;
        bool write_through = false;
        char output[256] = {};
        size_t size = 0;
    };

    TerminalWriteScript &terminal_write_script()
    {
        static TerminalWriteScript script;
        return script;
    }

    int64_t scripted_terminal_write(const char *data, size_t size)
    {
        auto &script = terminal_write_script();
        if (script.calls >= 6)
        {
            errno = EINVAL;
            return -1;
        }
        const int call = script.calls++;
        const int64_t count = script.counts[call];
        if (count > 0)
        {
            const auto bytes = static_cast<size_t>(count);
            if (bytes > size || bytes > sizeof script.output - script.size)
            {
                errno = EINVAL;
                return -1;
            }
            if (script.write_through && platform::detail::write_terminal_bytes(data, bytes) != count)
            {
                return -1;
            }
            std::memcpy(script.output + script.size, data, bytes);
            script.size += bytes;
        }
        if (call == script.suspend_at)
        {
            platform::detail::job_control_handler(SIGTSTP);
        }
        errno = script.errors[call];
        return count;
    }

    struct ScopedPosixSession
    {
        pid_t pid;
        pid_t app_group = -1;
        int *master_fd = nullptr;

        explicit ScopedPosixSession(pid_t leader, int *master = nullptr) : pid(leader), master_fd(master) {}
        ~ScopedPosixSession()
        {
            if (master_fd != nullptr && *master_fd >= 0 && pid > 0)
            {
                // Hang up the PTY before reaping failed children. Their terminal
                // close can otherwise wait for output that nobody will drain.
                close(*master_fd);
                *master_fd = -1;
            }
            if (app_group > 0)
            {
                kill(-app_group, SIGKILL);
            }
            if (pid <= 0)
            {
                return;
            }
            kill(-pid, SIGKILL);
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            {
            }
        }

        ScopedPosixSession(const ScopedPosixSession &) = delete;
        ScopedPosixSession &operator=(const ScopedPosixSession &) = delete;
        ScopedPosixSession(ScopedPosixSession &&) = delete;
        ScopedPosixSession &operator=(ScopedPosixSession &&) = delete;
    };

    void drain_pty_output(int fd, std::string &output)
    {
        char buffer[4096];
        ssize_t received = 0;
        while ((received = read(fd, buffer, sizeof buffer)) > 0)
        {
            output.append(buffer, static_cast<size_t>(received));
        }
    }

    struct PtyProcessResult
    {
        int status;
        int termios_restored;
    };

    bool write_pipe_value(int fd, const void *value, size_t size)
    {
        const auto *bytes = static_cast<const unsigned char *>(value);
        size_t written = 0;
        while (written < size)
        {
            const ssize_t result = write(fd, bytes + written, size - written);
            if (result > 0)
            {
                written += static_cast<size_t>(result);
            }
            else if (result == 0 || errno != EINTR)
            {
                return false;
            }
        }
        return true;
    }

    bool read_pipe_value(int fd, void *value, size_t size)
    {
        auto *bytes = static_cast<unsigned char *>(value);
        size_t received = 0;
        while (received < size)
        {
            const ssize_t result = read(fd, bytes + received, size - received);
            if (result > 0)
            {
                received += static_cast<size_t>(result);
            }
            else if (result == 0 || errno != EINTR)
            {
                return false;
            }
        }
        return true;
    }

    template <typename Predicate> void await_pty(int master, std::string &output, Predicate ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            drain_pty_output(master, output);
            if (ready())
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ASSERT_FAIL("timed out waiting for PTY job-control transition");
    }

    enum class JobControlCase : std::uint8_t
    {
        Startup,
        Idle,
        DragAndRepeat,
        Background,
        TerminateStopped,
        BlockedOutput,
        PausedOutput,
        PausedTerminateStopped,
        StartupPausedTerminateStopped,
        PausedContinuation,
    };

    void require_exclusive_terminal(int fd, const char *path)
    {
        ASSERT_TRUE(ioctl(fd, TIOCEXCL) == 0);
        ScopedFd reopened(open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK));
        if (reopened.fd < 0)
        {
            ASSERT_EQ(errno, EBUSY);
            return;
        }
        // Some PTY drivers let the owner bypass exclusivity. Remove permissions
        // too so this test exercises a terminal that cannot be reopened.
        ASSERT_TRUE(fchmod(fd, 0) == 0);
        ScopedFd denied(open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK));
        ASSERT_TRUE(denied.fd < 0);
        ASSERT_EQ(errno, EACCES);
    }

    int terminal_status_flags(int fd)
    {
        const int flags = fcntl(fd, F_GETFL, 0);
        ASSERT_TRUE(flags >= 0);
        // Darwin also reports internal flags such as FWASWRITTEN. Compare the
        // public access and I/O behavior bits, not kernel bookkeeping.
        return flags & (O_ACCMODE | O_APPEND | O_NONBLOCK | O_SYNC | O_ASYNC);
    }

    bool complete_frame_after(const std::string &output, size_t offset)
    {
        const size_t entered = output.find("\033[?1049h", offset);
        const size_t begin = output.find("\033[?2026h", entered);
        return output.find("\033[?2026l", begin) != std::string::npos;
    }

    void release_paused_output(bool paused, int fd)
    {
        if (paused)
        {
            // NOLINTNEXTLINE(concurrency-mt-unsafe): parent owns PTY flow control
            ASSERT_EQ(tcflow(fd, TCOON), 0);
        }
    }

    void flush_test_output_before_fork()
    {
        // TSan flushes streams even in _exit. Children must not inherit buffered
        // test output before redirecting their streams to a paused PTY.
        ASSERT_EQ(std::fflush(stdout), 0);
        ASSERT_EQ(std::fflush(stderr), 0);
    }

    void verify_job_control(JobControlCase scenario, bool exclusive = false)
    {
        const bool paused_continuation = scenario == JobControlCase::PausedContinuation;
        const bool startup =
            scenario == JobControlCase::Startup || scenario == JobControlCase::StartupPausedTerminateStopped;
        const bool paused_output = scenario == JobControlCase::PausedOutput ||
                                   scenario == JobControlCase::PausedTerminateStopped ||
                                   scenario == JobControlCase::StartupPausedTerminateStopped;
        const bool terminate_stopped = scenario == JobControlCase::TerminateStopped ||
                                       scenario == JobControlCase::PausedTerminateStopped ||
                                       scenario == JobControlCase::StartupPausedTerminateStopped;
        static constexpr char model_data[] = "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n";
        const std::string model_name = "rasterminal_job_control_" + std::to_string(getpid()) + ".obj";
        ScopedTmpFile model(model_name.c_str(), model_data, sizeof model_data - 1);
        ScopedFd master(posix_openpt(O_RDWR | O_NOCTTY));
        ASSERT_TRUE(master.fd >= 0);
        ASSERT_TRUE(grantpt(master.fd) == 0);
        ASSERT_TRUE(unlockpt(master.fd) == 0);
        const char *slave_name = ptsname(master.fd); // NOLINT(concurrency-mt-unsafe)
        ASSERT_TRUE(slave_name != nullptr);
        ScopedFd slave(open(slave_name, O_RDWR | O_NOCTTY));
        ASSERT_TRUE(slave.fd >= 0);
        winsize size = {};
        size.ws_row = scenario == JobControlCase::BlockedOutput ? 256 : 24;
        size.ws_col = scenario == JobControlCase::BlockedOutput ? 512 : 80;
        ASSERT_TRUE(ioctl(slave.fd, TIOCSWINSZ, &size) == 0);
        termios baseline = {};
        ASSERT_TRUE(tcgetattr(slave.fd, &baseline) == 0);
        if (paused_output || paused_continuation)
        {
            baseline.c_iflag |= IXON;
            baseline.c_iflag &= ~static_cast<tcflag_t>(IXANY);
        }
        if (terminate_stopped)
        {
            // Failed suspension cleanup must remain retryable after the shell
            // reclaims the foreground, even when background writes would stop us.
            baseline.c_lflag |= TOSTOP;
        }
        ASSERT_TRUE(tcsetattr(slave.fd, TCSANOW, &baseline) == 0);
        const int slave_flags = terminal_status_flags(slave.fd);
        ASSERT_TRUE(slave_flags >= 0);
        ScopedFd paused_probe(-1);
        if (paused_output || paused_continuation)
        {
            paused_probe.fd = open(slave_name, O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
            ASSERT_TRUE(paused_probe.fd >= 0);
        }
        if (exclusive)
        {
            require_exclusive_terminal(slave.fd, slave_name);
        }

        int reports[2] = { -1, -1 };
        int commands[2] = { -1, -1 };
        ASSERT_TRUE(pipe(reports) == 0);
        ScopedFd report_read(reports[0]);
        ScopedFd report_write(reports[1]);
        ASSERT_TRUE(pipe(commands) == 0);
        ScopedFd command_read(commands[0]);
        ScopedFd command_write(commands[1]);
        flush_test_output_before_fork();
        const pid_t supervisor = fork();
        ASSERT_TRUE(supervisor >= 0);
        if (supervisor == 0)
        {
            close(report_read.fd);
            close(command_write.fd);
            if (setsid() < 0 || ioctl(slave.fd, TIOCSCTTY, 0) < 0 || std::signal(SIGTTOU, SIG_IGN) == SIG_ERR ||
                dup2(slave.fd, STDIN_FILENO) < 0 || dup2(slave.fd, STDOUT_FILENO) < 0 ||
                dup2(slave.fd, STDERR_FILENO) < 0)
            {
                _exit(120);
            }
            close(master.fd);
            if (slave.fd > STDERR_FILENO)
            {
                close(slave.fd);
            }
            const pid_t app = fork();
            if (app < 0)
            {
                _exit(121);
            }
            if (app == 0)
            {
                close(report_write.fd);
                unsigned char start = 0;
                if (setpgid(0, 0) < 0 || std::signal(SIGTTOU, SIG_DFL) == SIG_ERR ||
                    !read_pipe_value(command_read.fd, &start, sizeof start))
                {
                    _exit(122);
                }
                close(command_read.fd);
                if (setenv("TERM", "xterm-256color", 1) != 0 || // NOLINT(concurrency-mt-unsafe)
                    unsetenv("TMUX") != 0 ||                    // NOLINT(concurrency-mt-unsafe)
                    unsetenv("STY") != 0)                       // NOLINT(concurrency-mt-unsafe)
                {
                    _exit(123);
                }
                execl(
                    RASTERMINAL_TEST_BINARY, "rasterminal", startup ? "--graphics=auto" : "--graphics=blocks",
                    "--color=256", "--no-ao", "--no-hud",
                    scenario == JobControlCase::Idle || scenario == JobControlCase::DragAndRepeat ? "--no-spin"
                                                                                                  : "--spin",
                    "--fps=30", "--threads=4", model.path.c_str(), static_cast<char *>(nullptr)
                );
                _exit(124);
            }
            // A separate foreground group with a parent in this session is not
            // orphaned, so default SIGTSTP really stops it, just as under a shell.
            if (setpgid(app, app) < 0 || tcsetpgrp(STDIN_FILENO, app) < 0 ||
                !write_pipe_value(report_write.fd, &app, sizeof app))
            {
                _exit(125);
            }
            while (true)
            {
                PtyProcessResult result = {};
                pid_t waited = waitpid(app, &result.status, WUNTRACED);
                while (waited < 0 && errno == EINTR)
                {
                    waited = waitpid(app, &result.status, WUNTRACED);
                }
                termios restored = {};
                result.termios_restored =
                    waited == app && tcgetattr(STDIN_FILENO, &restored) == 0 && termios_equal(restored, baseline);
                if (waited != app || tcsetpgrp(STDIN_FILENO, getpgrp()) < 0 ||
                    !write_pipe_value(report_write.fd, &result, sizeof result))
                {
                    _exit(126);
                }
                unsigned char command = 0;
                if (!read_pipe_value(command_read.fd, &command, sizeof command))
                {
                    _exit(127);
                }
                if (!WIFSTOPPED(result.status))
                {
                    // Keep the session alive until the parent drains output.
                    _exit(0);
                }
                if (command == 'b')
                {
                    if (kill(-app, SIGCONT) < 0 || !write_pipe_value(report_write.fd, &result, sizeof result) ||
                        !read_pipe_value(command_read.fd, &command, sizeof command))
                    {
                        _exit(128);
                    }
                }
                if (command == 't')
                {
                    if (kill(-app, SIGTERM) < 0)
                    {
                        _exit(129);
                    }
                }
                else if (command == 's')
                {
                    if (kill(-app, SIGTSTP) < 0)
                    {
                        _exit(132);
                    }
                    continue;
                }
                else if (command != 'f' || tcsetpgrp(STDIN_FILENO, app) < 0)
                {
                    _exit(130);
                }
                if (kill(-app, SIGCONT) < 0)
                {
                    _exit(131);
                }
            }
        }
        ScopedPosixSession session(supervisor, &master.fd);
        test_close(report_write.fd);
        report_write.fd = -1;
        test_close(command_read.fd);
        command_read.fd = -1;
        const int flags = fcntl(master.fd, F_GETFL, 0);
        ASSERT_TRUE(flags >= 0);
        ASSERT_TRUE(fcntl(master.fd, F_SETFL, flags | O_NONBLOCK) == 0);
        std::string output;
        const auto report_ready = [&]()
        {
            pollfd fd = { report_read.fd, POLLIN, 0 };
            return poll(&fd, 1, 0) > 0;
        };
        await_pty(master.fd, output, report_ready);
        pid_t app = -1;
        ASSERT_TRUE(read_pipe_value(report_read.fd, &app, sizeof app));
        ASSERT_TRUE(app > 0);
        session.app_group = app;
        const auto command = [&](unsigned char value)
        { ASSERT_TRUE(write_pipe_value(command_write.fd, &value, sizeof value)); };
        command('s');
        const auto result = [&]()
        {
            await_pty(master.fd, output, report_ready);
            PtyProcessResult value = {};
            ASSERT_TRUE(read_pipe_value(report_read.fd, &value, sizeof value));
            drain_pty_output(master.fd, output);
            return value;
        };
        size_t offset = 0;
        const auto rendered = [&]() { return output.find("\033[?2026l", offset) != std::string::npos; };
        if (startup)
        {
            await_pty(master.fd, output, [&]() { return output.find("\033[5n") != std::string::npos; });
        }
        else if (scenario == JobControlCase::BlockedOutput)
        {
            // Consume only the frame prefix, then let the large frame fill the
            // PTY queue. Draining the whole frame here would hide short writes.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (output.find("\033[?2026h") == std::string::npos && std::chrono::steady_clock::now() < deadline)
            {
                char bytes[128];
                const ssize_t count = read(master.fd, bytes, sizeof bytes);
                if (count > 0)
                {
                    output.append(bytes, static_cast<size_t>(count));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            ASSERT_TRUE(output.find("\033[?2026h") != std::string::npos);
            ASSERT_TRUE(output.find("\033[?2026l") == std::string::npos);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        else
        {
            await_pty(master.fd, output, [&]() { return complete_frame_after(output, offset); });
        }
        const int cycles = scenario == JobControlCase::DragAndRepeat ? 3 : 1;
        for (int cycle = 0; cycle < cycles; ++cycle)
        {
            if (scenario == JobControlCase::DragAndRepeat)
            {
                offset = output.size();
                constexpr char drag[] = "\033[<0;10;10M\033[<32;11;11M";
                ASSERT_TRUE(write_pipe_value(master.fd, drag, sizeof drag - 1));
                await_pty(master.fd, output, rendered);
                constexpr char partial[] = "\033[<32;";
                ASSERT_TRUE(write_pipe_value(master.fd, partial, sizeof partial - 1));
            }
            offset = output.size();
            if (paused_output)
            {
                // Keep output stopped until waitpid confirms suspension. Draining
                // the master must not release the writer as it does for a full queue.
                // NOLINTNEXTLINE(concurrency-mt-unsafe): parent owns PTY flow control
                ASSERT_TRUE(tcflow(slave.fd, TCOOFF) == 0);
                if (terminate_stopped)
                {
                    // A paused tty can still accept queued bytes on macOS. Exhaust
                    // that space so every suspension cleanup write must time out.
                    const std::string padding(4096, 'x');
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    ssize_t written = 0;
                    while (std::chrono::steady_clock::now() < deadline)
                    {
                        written = write(paused_probe.fd, padding.data(), padding.size());
                        if (written < 0 && errno != EINTR)
                        {
                            break;
                        }
                    }
                    ASSERT_TRUE(written < 0 && errno == EAGAIN);
                }
                if (!startup)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            ASSERT_TRUE(kill(-app, SIGTSTP) == 0);
            if (scenario == JobControlCase::BlockedOutput)
            {
                for (int i = 0; i < 8; ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    ASSERT_TRUE(kill(-app, SIGTSTP) == 0);
                }
            }
            const PtyProcessResult stopped = result();
            ASSERT_TRUE(WIFSTOPPED(stopped.status));
            ASSERT_EQ(WSTOPSIG(stopped.status), SIGTSTP);
            ASSERT_TRUE(stopped.termios_restored != 0);
            ASSERT_EQ(terminal_status_flags(slave.fd), slave_flags);
            if (paused_output)
            {
                // A separate nonblocking open also verifies cleanup did not
                // restart flow control in order to reach the stop handoff.
                constexpr char marker[] = "OUTPUT_MUST_STAY_PAUSED";
                const ssize_t written = write(paused_probe.fd, marker, sizeof marker - 1);
                ASSERT_TRUE(written >= 0 || errno == EAGAIN);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                drain_pty_output(master.fd, output);
                ASSERT_TRUE(output.find(marker, offset) == std::string::npos);
                // NOLINTNEXTLINE(concurrency-mt-unsafe): parent owns PTY flow control
                ASSERT_TRUE(tcflow(slave.fd, TCOON) == 0);
            }
            else
            {
                ASSERT_TRUE(output.find("\033[?1049l", offset) != std::string::npos);
            }
            if (!startup && !paused_output)
            {
                ASSERT_TRUE(output.find("\033[?25h\033[0m\033[?1049l", offset) != std::string::npos);
                ASSERT_TRUE(output.find("\033[?1002l\033[?1006l", offset) != std::string::npos);
                ASSERT_TRUE(output.find("\033[?1002l", offset) < output.find("\033[?1049l", offset));
                ASSERT_TRUE(output.find("\033[?2026l", offset) < output.find("\033[?1049l", offset));
                ASSERT_TRUE(output.find("\033[?7h", offset) < output.find("\033[?25h", offset));
            }
            offset = output.size();
            if (terminate_stopped)
            {
                command('t');
                break;
            }
            if (scenario == JobControlCase::Background)
            {
                command('b');
                result();
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
                await_pty(master.fd, output, [&]() { return std::chrono::steady_clock::now() >= until; });
                termios background = {};
                ASSERT_TRUE(tcgetattr(slave.fd, &background) == 0);
                ASSERT_TRUE(termios_equal(background, baseline));
                ASSERT_EQ(output.size(), offset);
                command('s');
                const PtyProcessResult stopped_again = result();
                ASSERT_TRUE(WIFSTOPPED(stopped_again.status));
                ASSERT_EQ(WSTOPSIG(stopped_again.status), SIGTSTP);
                ASSERT_TRUE(stopped_again.termios_restored != 0);
                ASSERT_EQ(output.size(), offset);
            }
            if (paused_continuation)
            {
                // Pause only after suspension succeeds, then exhaust the queue so
                // macOS cannot accept the small reacquisition writes either.
                // NOLINTNEXTLINE(concurrency-mt-unsafe): parent owns PTY flow control
                ASSERT_EQ(tcflow(slave.fd, TCOOFF), 0);
                const std::string padding(4096, 'x');
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                ssize_t written = 0;
                while (std::chrono::steady_clock::now() < deadline)
                {
                    written = write(paused_probe.fd, padding.data(), padding.size());
                    if (written < 0 && errno != EINTR)
                    {
                        break;
                    }
                }
                ASSERT_TRUE(written < 0 && errno == EAGAIN);
                command('f');
                await_pty(
                    master.fd, output,
                    [&]()
                    {
                        termios active = {};
                        return tcgetattr(slave.fd, &active) == 0 &&
                               (active.c_lflag & static_cast<tcflag_t>(ECHO | ICANON)) == 0;
                    }
                );
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                ASSERT_EQ(kill(-app, SIGTERM), 0);
                break;
            }
            command('f');
            if (startup)
            {
                await_pty(master.fd, output, [&]() { return output.find("\033[5n", offset) != std::string::npos; });
                constexpr char reply[] = "\033[0n";
                ASSERT_TRUE(write_pipe_value(master.fd, reply, sizeof reply - 1));
            }
            await_pty(master.fd, output, [&]() { return complete_frame_after(output, offset); });
            ASSERT_TRUE(output.find("\033[?1049h", offset) != std::string::npos);
            ASSERT_TRUE(output.find("\033[?25l", offset) != std::string::npos);
            ASSERT_TRUE(output.find("\033[?1006h\033[?1002h", offset) != std::string::npos);
            termios active = {};
            ASSERT_TRUE(tcgetattr(slave.fd, &active) == 0);
            ASSERT_EQ(active.c_lflag & static_cast<tcflag_t>(ECHO | ICANON), static_cast<tcflag_t>(0));
        }
        if (!terminate_stopped && !paused_continuation)
        {
            offset = output.size();
            constexpr char quit = 'Q';
            ASSERT_TRUE(write_pipe_value(master.fd, &quit, sizeof quit));
        }
        const PtyProcessResult exited = result();
        if (terminate_stopped || paused_continuation)
        {
            ASSERT_TRUE(WIFSIGNALED(exited.status));
            ASSERT_EQ(WTERMSIG(exited.status), SIGTERM);
            ASSERT_TRUE(output.find("\033[?1049h", offset) == std::string::npos);
            if (paused_output)
            {
                ASSERT_TRUE(output.find("\033[?1049l", offset) != std::string::npos);
                if (startup)
                {
                    ASSERT_TRUE(output.find("\033[?25l") == std::string::npos);
                    ASSERT_TRUE(output.find("\033[?1006h") == std::string::npos);
                }
                else
                {
                    ASSERT_TRUE(output.find("\033[?25h\033[0m\033[?1049l", offset) != std::string::npos);
                    ASSERT_TRUE(output.find("\033[?1002l\033[?1006l", offset) != std::string::npos);
                    ASSERT_TRUE(output.find("\033[?1002l", offset) < output.find("\033[?1049l", offset));
                }
            }
        }
        else
        {
            ASSERT_TRUE(WIFEXITED(exited.status));
            ASSERT_EQ(WEXITSTATUS(exited.status), 0);
            ASSERT_TRUE(output.find("\033[?25h\033[0m\033[?1049l", offset) != std::string::npos);
            ASSERT_TRUE(output.find("\033[?1002l\033[?1006l", offset) != std::string::npos);
        }
        ASSERT_TRUE(exited.termios_restored != 0);
        ASSERT_EQ(terminal_status_flags(slave.fd), slave_flags);
        // The viewer has exited with output still paused. Release the
        // supervisor's terminal drain before asking that session to exit.
        release_paused_output(paused_continuation, slave.fd);
        session.app_group = -1;
        command('q');
        int status = 0;
        await_pty(master.fd, output, [&]() { return waitpid(supervisor, &status, WNOHANG) == supervisor; });
        session.pid = -1;
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 0);
    }

    void verify_interactive_process_cleans_up_terminal(int exit_signal, bool exclusive = false)
    {
        static constexpr char model_data[] = "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n";
        const std::string model_name =
            "rasterminal_raw_mode_" + std::to_string(getpid()) + "_" + std::to_string(exit_signal) + ".obj";
        ScopedTmpFile model(model_name.c_str(), model_data, sizeof model_data - 1);

        ScopedFd master(posix_openpt(O_RDWR | O_NOCTTY));
        ASSERT_TRUE(master.fd >= 0);
        ASSERT_TRUE(grantpt(master.fd) == 0);
        ASSERT_TRUE(unlockpt(master.fd) == 0);
        const char *slave_name = ptsname(master.fd); // NOLINT(concurrency-mt-unsafe)
        ASSERT_TRUE(slave_name != nullptr);
        ScopedFd slave(open(slave_name, O_RDWR | O_NOCTTY));
        ASSERT_TRUE(slave.fd >= 0);

        if (exclusive)
        {
            require_exclusive_terminal(slave.fd, slave_name);
        }
        const int slave_flags = terminal_status_flags(slave.fd);
        ASSERT_TRUE(slave_flags >= 0);

        winsize size = {};
        size.ws_row = 24;
        size.ws_col = 80;
        ASSERT_TRUE(ioctl(slave.fd, TIOCSWINSZ, &size) == 0);

        termios configured = {};
        ASSERT_TRUE(tcgetattr(slave.fd, &configured) == 0);
        configured.c_lflag |= static_cast<tcflag_t>(ECHO | ICANON);
        configured.c_cc[VMIN] = 1;
        configured.c_cc[VTIME] = 0;
        ASSERT_TRUE(tcsetattr(slave.fd, TCSANOW, &configured) == 0);
        termios baseline = {};
        ASSERT_TRUE(tcgetattr(slave.fd, &baseline) == 0);

        int app_pid_fds[2] = { -1, -1 };
        int result_fds[2] = { -1, -1 };
        int release_fds[2] = { -1, -1 };
        ASSERT_TRUE(pipe(app_pid_fds) == 0);
        ScopedFd app_pid_read(app_pid_fds[0]);
        ScopedFd app_pid_write(app_pid_fds[1]);
        ASSERT_TRUE(pipe(result_fds) == 0);
        ScopedFd result_read(result_fds[0]);
        ScopedFd result_write(result_fds[1]);
        ASSERT_TRUE(pipe(release_fds) == 0);
        ScopedFd release_read(release_fds[0]);
        ScopedFd release_write(release_fds[1]);

        const pid_t supervisor_pid = fork();
        ASSERT_TRUE(supervisor_pid >= 0);
        if (supervisor_pid == 0)
        {
            close(app_pid_read.fd);
            close(result_read.fd);
            close(release_write.fd);
            if (setsid() < 0 || ioctl(slave.fd, TIOCSCTTY, 0) < 0 || dup2(slave.fd, STDIN_FILENO) < 0 ||
                dup2(slave.fd, STDOUT_FILENO) < 0 || dup2(slave.fd, STDERR_FILENO) < 0)
            {
                _exit(120);
            }
            close(master.fd);
            if (slave.fd > STDERR_FILENO)
            {
                close(slave.fd);
            }

            const pid_t app_pid = fork();
            if (app_pid < 0)
            {
                _exit(121);
            }
            if (app_pid == 0)
            {
                close(app_pid_write.fd);
                close(result_write.fd);
                close(release_read.fd);
                // The test runner has no live worker threads when it forks this child.
                if (setenv("TERM", "xterm-256color", 1) != 0 || // NOLINT(concurrency-mt-unsafe)
                    unsetenv("TMUX") != 0 ||                    // NOLINT(concurrency-mt-unsafe)
                    unsetenv("STY") != 0)                       // NOLINT(concurrency-mt-unsafe)
                {
                    _exit(122);
                }
                execl(
                    RASTERMINAL_TEST_BINARY, "rasterminal", "--graphics=blocks", "--color=256", "--no-ao", "--no-hud",
                    "--no-spin", "--no-input", "--fps=30", "--threads=1", model.path.c_str(),
                    static_cast<char *>(nullptr)
                );
                _exit(123);
            }

            if (!write_pipe_value(app_pid_write.fd, &app_pid, sizeof app_pid))
            {
                _exit(124);
            }
            close(app_pid_write.fd);

            PtyProcessResult process_result = {};
            pid_t waited = 0;
            while ((waited = waitpid(app_pid, &process_result.status, 0)) < 0 && errno == EINTR)
            {
            }
            termios restored = {};
            process_result.termios_restored =
                waited == app_pid && tcgetattr(STDIN_FILENO, &restored) == 0 && termios_equal(restored, baseline);
            if (!write_pipe_value(result_write.fd, &process_result, sizeof process_result))
            {
                _exit(125);
            }
            unsigned char release = 0;
            if (!read_pipe_value(release_read.fd, &release, sizeof release))
            {
                _exit(126);
            }
            _exit(0);
        }

        test_close(app_pid_write.fd);
        app_pid_write.fd = -1;
        test_close(result_write.fd);
        result_write.fd = -1;
        test_close(release_read.fd);
        release_read.fd = -1;

        pid_t app_pid = -1;
        ASSERT_TRUE(read_pipe_value(app_pid_read.fd, &app_pid, sizeof app_pid));
        ASSERT_TRUE(app_pid > 0);

        ScopedPosixSession session(supervisor_pid);
        const int flags = fcntl(master.fd, F_GETFL, 0);
        ASSERT_TRUE(flags >= 0);
        ASSERT_TRUE(fcntl(master.fd, F_SETFL, flags | O_NONBLOCK) == 0);

        bool saw_raw_mode = false;
        bool requested_exit = false;
        bool received_result = false;
        bool exited = false;
        size_t cleanup_offset = 0;
        std::string output;
        PtyProcessResult process_result = {};
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            drain_pty_output(master.fd, output);
            termios active = {};
            if (tcgetattr(slave.fd, &active) == 0 && (active.c_lflag & static_cast<tcflag_t>(ECHO | ICANON)) == 0 &&
                active.c_cc[VMIN] == 0 && active.c_cc[VTIME] == 0)
            {
                saw_raw_mode = true;
            }
            const bool session_ready = output.find("\033[?1006h\033[?1002h") != std::string::npos &&
                                       output.find("\033[?1049h") != std::string::npos &&
                                       output.find("\033[?25l") != std::string::npos;
            if (saw_raw_mode && session_ready && !requested_exit)
            {
                cleanup_offset = output.size();
                if (exit_signal == 0)
                {
                    const char quit = 'Q';
                    requested_exit = write(master.fd, &quit, 1) == 1;
                }
                else
                {
                    requested_exit = kill(app_pid, exit_signal) == 0;
                }
            }

            pollfd result_poll = { result_read.fd, POLLIN, 0 };
            const int poll_result = poll(&result_poll, 1, 0);
            if (poll_result > 0)
            {
                ASSERT_TRUE(read_pipe_value(result_read.fd, &process_result, sizeof process_result));
                received_result = true;
                break;
            }
            ASSERT_TRUE(poll_result == 0 || (poll_result < 0 && errno == EINTR));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        ASSERT_TRUE(saw_raw_mode);
        ASSERT_TRUE(requested_exit);
        ASSERT_TRUE(received_result);

        drain_pty_output(master.fd, output);
        const unsigned char release = 1;
        ASSERT_TRUE(write_pipe_value(release_write.fd, &release, sizeof release));
        test_close(release_write.fd);
        release_write.fd = -1;

        const auto exit_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < exit_deadline)
        {
            const pid_t waited = waitpid(supervisor_pid, &status, WNOHANG);
            if (waited == supervisor_pid)
            {
                session.pid = -1;
                exited = true;
                break;
            }
            ASSERT_TRUE(waited == 0 || (waited < 0 && errno == EINTR));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        ASSERT_TRUE(exited);
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 0);

        if (exit_signal == 0)
        {
            ASSERT_TRUE(WIFEXITED(process_result.status));
            ASSERT_EQ(WEXITSTATUS(process_result.status), 0);
        }
        else
        {
            ASSERT_TRUE(WIFSIGNALED(process_result.status));
            ASSERT_EQ(WTERMSIG(process_result.status), exit_signal);
        }
        ASSERT_TRUE(process_result.termios_restored != 0);
        ASSERT_EQ(terminal_status_flags(slave.fd), slave_flags);

        const std::string cleanup = output.substr(cleanup_offset);
        ASSERT_TRUE(cleanup.find("\033[?25h\033[0m\033[?1049l") != std::string::npos);
        ASSERT_TRUE(cleanup.find("\033[?1002l\033[?1006l") != std::string::npos);
        // Mouse cleanup runs on the restored shell screen. Ghostty 1.3.1 prints
        // a bare CAN there, so the remaining output must contain only escapes.
        const size_t shell_screen = cleanup.rfind("\033[?1049l") + std::strlen("\033[?1049l");
        ASSERT_TRUE(cleanup.substr(shell_screen) == "\033\\\033[?1002l\033[?1006l");
    }
} // namespace

TEST(platform, finish_termination_delivers_signal_pending_at_cleanup_boundary)
{
    const pid_t child = fork();
    ASSERT_TRUE(child >= 0);
    if (child == 0)
    {
        sigset_t signal = {};
        sigset_t signals = {};
        sigset_t previous_mask = {};
        if (sigemptyset(&signal) != 0 || sigaddset(&signal, SIGTERM) != 0 ||
            pthread_sigmask(SIG_UNBLOCK, &signal, nullptr) != 0 || !platform::install_interrupt_handler() ||
            !platform::detail::termination_signal_set(signals) ||
            pthread_sigmask(SIG_BLOCK, &signals, &previous_mask) != 0 || std::raise(SIGTERM) != 0)
        {
            _exit(120);
        }
        _exit(platform::detail::finish_termination_with_signals_blocked(
            0, previous_mask, platform::detail::termination_handler_mask
        ));
    }

    int status = 0;
    pid_t waited = 0;
    while ((waited = waitpid(child, &status, 0)) < 0 && errno == EINTR)
    {
    }
    ASSERT_EQ(waited, child);
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGTERM);
}

TEST(platform, finish_termination_preserves_caller_signal_mask)
{
    const pid_t child = fork();
    ASSERT_TRUE(child >= 0);
    if (child == 0)
    {
        sigset_t signal = {};
        if (sigemptyset(&signal) != 0 || sigaddset(&signal, SIGTERM) != 0 ||
            pthread_sigmask(SIG_BLOCK, &signal, nullptr) != 0 || !platform::install_interrupt_handler() ||
            std::raise(SIGTERM) != 0)
        {
            _exit(120);
        }

        platform::detail::interrupt_flag = 0;
        if (platform::finish_termination(17) != 17)
        {
            _exit(121);
        }
        sigset_t restored_mask = {};
        if (pthread_sigmask(SIG_BLOCK, nullptr, &restored_mask) != 0 || sigismember(&restored_mask, SIGTERM) != 1)
        {
            _exit(122);
        }
        _exit(0);
    }

    int status = 0;
    pid_t waited = 0;
    while ((waited = waitpid(child, &status, 0)) < 0 && errno == EINTR)
    {
    }
    ASSERT_EQ(waited, child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
}

TEST(platform, finish_termination_consumes_pending_suspend_during_clean_exit)
{
    const pid_t child = fork();
    ASSERT_TRUE(child >= 0);
    if (child == 0)
    {
        sigset_t signals = {};
        sigset_t previous_mask = {};
        if (sigemptyset(&signals) != 0 || !platform::detail::add_job_control_signals(signals) ||
            pthread_sigmask(SIG_UNBLOCK, &signals, nullptr) != 0 || !platform::install_interrupt_handler() ||
            !platform::install_job_control_handler() || pthread_sigmask(SIG_BLOCK, &signals, &previous_mask) != 0 ||
            std::raise(SIGTSTP) != 0)
        {
            _exit(120);
        }
        const int status = platform::detail::finish_termination_with_signals_blocked(
            17, previous_mask, platform::detail::termination_handler_mask
        );
        _exit(status == 17 && platform::suspend_requested() ? 0 : 121);
    }

    int status = 0;
    pid_t waited = 0;
    while ((waited = waitpid(child, &status, WUNTRACED)) < 0 && errno == EINTR)
    {
    }
    const int observed_status = status;
    if (waited == child && WIFSTOPPED(status))
    {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        {
        }
    }
    ASSERT_EQ(waited, child);
    ASSERT_TRUE(WIFEXITED(observed_status));
    ASSERT_EQ(WEXITSTATUS(observed_status), 0);
}

TEST(platform, finish_termination_preserves_inherited_ignored_signal)
{
    const pid_t child = fork();
    ASSERT_TRUE(child >= 0);
    if (child == 0)
    {
        if (std::signal(SIGHUP, SIG_IGN) == SIG_ERR || !platform::install_interrupt_handler())
        {
            _exit(120);
        }
        struct sigaction disposition = {};
        if (sigaction(SIGHUP, nullptr, &disposition) != 0 || disposition.sa_handler != SIG_IGN)
        {
            _exit(121);
        }
        if (platform::finish_termination(17) != 17)
        {
            _exit(122);
        }

        if (sigaction(SIGHUP, nullptr, &disposition) != 0 || disposition.sa_handler != SIG_IGN)
        {
            _exit(123);
        }
        _exit(0);
    }

    int status = 0;
    pid_t waited = 0;
    while ((waited = waitpid(child, &status, 0)) < 0 && errno == EINTR)
    {
    }
    ASSERT_EQ(waited, child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
}

TEST(platform, finish_termination_delivers_recorded_signal_before_later_pending_signal)
{
    const pid_t child = fork();
    ASSERT_TRUE(child >= 0);
    if (child == 0)
    {
        sigset_t signals = {};
        sigset_t previous_mask = {};
        if (!platform::detail::termination_signal_set(signals) ||
            pthread_sigmask(SIG_UNBLOCK, &signals, nullptr) != 0 || !platform::install_interrupt_handler() ||
            std::raise(SIGTERM) != 0 || pthread_sigmask(SIG_BLOCK, &signals, &previous_mask) != 0 ||
            std::raise(SIGINT) != 0)
        {
            _exit(120);
        }
        _exit(platform::detail::finish_termination_with_signals_blocked(
            0, previous_mask, platform::detail::termination_handler_mask
        ));
    }

    int status = 0;
    pid_t waited = 0;
    while ((waited = waitpid(child, &status, 0)) < 0 && errno == EINTR)
    {
    }
    ASSERT_EQ(waited, child);
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGTERM);
}

TEST(platform, terminal_write_retries_partial_writes_and_repeated_eintr)
{
    terminal_write_script() = {};
    auto &script = terminal_write_script();
    script.counts[0] = 2;
    script.counts[1] = -1;
    script.errors[1] = EINTR;
    script.counts[2] = -1;
    script.errors[2] = EINTR;
    script.counts[3] = 4;
    ASSERT_TRUE(platform::write_terminal("abcdef", 6, false, scripted_terminal_write));
    ASSERT_EQ(script.calls, 4);
    ASSERT_EQ(script.size, static_cast<size_t>(6));
    ASSERT_TRUE(std::memcmp(script.output, "abcdef", 6) == 0);
}

TEST(platform, terminal_write_cancels_with_paused_output_before_syscall_and_during_wait)
{
    for (const bool before_syscall : { true, false })
    {
        ScopedFd master(posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK));
        ASSERT_TRUE(master.fd >= 0);
        ASSERT_EQ(grantpt(master.fd), 0);
        ASSERT_EQ(unlockpt(master.fd), 0);
        const char *path = ptsname(master.fd); // NOLINT(concurrency-mt-unsafe)
        ASSERT_TRUE(path != nullptr);
        ScopedFd slave(open(path, O_RDWR | O_NOCTTY | O_NONBLOCK));
        ASSERT_TRUE(slave.fd >= 0);
        // NOLINTNEXTLINE(concurrency-mt-unsafe): parent owns PTY flow control
        ASSERT_EQ(tcflow(slave.fd, TCOOFF), 0);
        // macOS accepts queued bytes while paused. Exhaust the queue before
        // injecting the signal so even a one-byte write cannot complete.
        const std::string padding(4096, 'x');
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        ssize_t written = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            written = write(slave.fd, padding.data(), padding.size());
            if (written < 0 && errno != EINTR)
            {
                break;
            }
        }
        ASSERT_TRUE(written < 0 && errno == EAGAIN);
        const int flags = fcntl(slave.fd, F_GETFL, 0);
        ASSERT_TRUE(flags >= 0);
        const int blocking_flags = static_cast<int>(static_cast<unsigned>(flags) & ~static_cast<unsigned>(O_NONBLOCK));
        ASSERT_EQ(fcntl(slave.fd, F_SETFL, blocking_flags), 0);
        int ready[2] = { -1, -1 };
        ASSERT_EQ(pipe(ready), 0);
        ScopedFd ready_read(ready[0]);
        ScopedFd ready_write(ready[1]);
        flush_test_output_before_fork();
        const pid_t child = fork();
        ASSERT_TRUE(child >= 0);
        if (child == 0)
        {
            if (std::signal(SIGALRM, SIG_DFL) == SIG_ERR)
            {
                _exit(120);
            }
            alarm(3);
            sigset_t mask = {};
            sigemptyset(&mask);
            if (pthread_sigmask(SIG_SETMASK, &mask, nullptr) != 0 ||
                std::signal(SIGTSTP, platform::detail::job_control_handler) == SIG_ERR ||
                dup2(slave.fd, STDOUT_FILENO) < 0)
            {
                _exit(121);
            }
            platform::detail::suspend_flag = 0;
            static bool inject_before = false;
            static int notify_fd = -1;
            inject_before = before_syscall;
            notify_fd = ready_write.fd;
            const bool completed = platform::write_terminal(
                "x", 1, true,
                [](const char *data, size_t size) -> int64_t
                {
                    if (inject_before)
                    {
                        std::raise(SIGTSTP);
                    }
                    else if (notify_fd >= 0)
                    {
                        constexpr char marker = 'w';
                        if (write(notify_fd, &marker, 1) != 1)
                        {
                            _exit(122);
                        }
                        notify_fd = -1;
                    }
                    return platform::detail::write_terminal_bytes(data, size);
                }
            );
            _exit(
                !completed && platform::suspend_requested() && fcntl(STDOUT_FILENO, F_GETFL, 0) == blocking_flags ? 0
                                                                                                                  : 123
            );
        }
        close(ready_write.fd);
        ready_write.fd = -1;
        if (!before_syscall)
        {
            char marker = 0;
            const bool notified = read_pipe_value(ready_read.fd, &marker, 1);
            if (notified)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                kill(child, SIGTSTP);
            }
        }
        int status = 0;
        pid_t waited = 0;
        while ((waited = waitpid(child, &status, 0)) < 0 && errno == EINTR)
        {
        }
        ASSERT_EQ(waited, child);
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 0);
        ASSERT_EQ(fcntl(slave.fd, F_GETFL, 0), blocking_flags);
    }
}

TEST(platform, cancelable_terminal_write_restores_flags_after_success_and_error)
{
    ScopedStdoutCapture capture;
    ASSERT_TRUE(capture.valid);
    const int original_flags = fcntl(STDOUT_FILENO, F_GETFL, 0);
    ASSERT_TRUE(original_flags >= 0);
    for (const int flags : { original_flags, original_flags | O_NONBLOCK })
    {
        ASSERT_EQ(fcntl(STDOUT_FILENO, F_SETFL, flags), 0);
        for (const bool success : { true, false })
        {
            terminal_write_script() = {};
            auto &script = terminal_write_script();
            script.counts[0] = 2;
            script.counts[1] = -1;
            script.errors[1] = success ? EINTR : EIO;
            script.counts[2] = 4;
            ASSERT_EQ(platform::write_terminal("abcdef", 6, true, scripted_terminal_write), success);
            ASSERT_EQ(fcntl(STDOUT_FILENO, F_GETFL, 0), flags);
        }
    }
}

TEST(platform, mouse_cleanup_resets_interrupted_escape_before_releasing_ownership)
{
    for (const char *partial : { "", "\033", "\033_Ga=T,m=1;AAAA", "\033Pq", "\033[?1002" })
    {
        ScopedStdoutCapture capture;
        ASSERT_TRUE(capture.valid);
        ASSERT_TRUE(platform::write_terminal(partial));
        const bool mouse_pending = !platform::disable_mouse();
        ASSERT_FALSE(mouse_pending);
        // A successful write must put mouse resets outside the unfinished escape.
        ASSERT_TRUE(capture.read() == std::string(partial) + "\033\\\033[?1002l\033[?1006l");
    }
}

TEST(platform, terminal_reacquisition_skips_mouse_and_queries_for_pending_control)
{
    ScopedStdoutCapture capture;
    ASSERT_TRUE(capture.valid);
    platform::detail::suspend_flag = 1;
    bool canceled = false;
    const bool mouse_ready = platform::enable_mouse(true, &canceled);
    platform::request_cell_size();
    platform::request_sixel_geometry();
    platform::detail::suspend_flag = 0;
    ASSERT_FALSE(mouse_ready);
    ASSERT_TRUE(canceled);
    ASSERT_TRUE(capture.read().empty());
}

TEST(platform, terminal_cleanup_preserves_shared_flags_on_exclusive_terminal)
{
    ScopedFd master(posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK));
    ASSERT_TRUE(master.fd >= 0);
    ASSERT_TRUE(grantpt(master.fd) == 0);
    ASSERT_TRUE(unlockpt(master.fd) == 0);
    const char *path = ptsname(master.fd); // NOLINT(concurrency-mt-unsafe)
    ASSERT_TRUE(path != nullptr);
    ScopedFd slave(open(path, O_RDWR | O_NOCTTY));
    ASSERT_TRUE(slave.fd >= 0);
    require_exclusive_terminal(slave.fd, path);
    const int original_flags = terminal_status_flags(slave.fd);
    ASSERT_TRUE(original_flags >= 0);
    for (const int flags : { original_flags, original_flags | O_NONBLOCK })
    {
        ASSERT_EQ(fcntl(slave.fd, F_SETFL, flags), 0);
        {
            ScopedStdoutCapture capture;
            ASSERT_TRUE(capture.valid);
            ASSERT_TRUE(dup2(slave.fd, STDOUT_FILENO) >= 0);
            ASSERT_TRUE(platform::write_terminal_cleanup("cleanup"));
            ASSERT_EQ(terminal_status_flags(slave.fd), flags);
        }
        std::string output;
        await_pty(master.fd, output, [&]() { return output.size() >= 7; });
        ASSERT_TRUE(output == "cleanup");
    }
}

TEST(platform, terminal_cleanup_restart_restores_captured_termios_and_signal_mask)
{
    reset_termios_calls();
    auto &calls = termios_calls();
    calls.current.c_iflag |= static_cast<tcflag_t>(IXON | ICRNL);
    calls.current.c_oflag |= OPOST;
    calls.current.c_lflag |= ISIG;
    const termios captured = calls.current;

    sigset_t before = {};
    sigset_t after = {};
    ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &before), 0);
    ASSERT_TRUE(platform::restart_terminal_output_for_cleanup(scripted_tcgetattr, scripted_tcsetattr));
    ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &after), 0);

    ASSERT_EQ(calls.get_calls, 1);
    ASSERT_EQ(calls.set_calls, 2);
    ASSERT_EQ(calls.set_calls_with_sigttou_blocked, 2);
    ASSERT_EQ(calls.writes[0].c_iflag & static_cast<tcflag_t>(IXON), static_cast<tcflag_t>(0));
    ASSERT_TRUE(termios_equal(calls.writes[1], captured));
    ASSERT_TRUE(termios_equal(calls.current, captured));
    ASSERT_EQ(sigismember(&after, SIGTTOU), sigismember(&before, SIGTTOU));
}

TEST(platform, terminal_cleanup_restart_releases_xoff_without_xon)
{
    ScopedFd master(posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK));
    ASSERT_TRUE(master.fd >= 0);
    ASSERT_EQ(grantpt(master.fd), 0);
    ASSERT_EQ(unlockpt(master.fd), 0);
    const char *path = ptsname(master.fd); // NOLINT(concurrency-mt-unsafe)
    ASSERT_TRUE(path != nullptr);
    ScopedFd slave(open(path, O_RDWR | O_NOCTTY | O_NONBLOCK));
    ASSERT_TRUE(slave.fd >= 0);

    termios configured = {};
    ASSERT_EQ(tcgetattr(slave.fd, &configured), 0);
    configured.c_iflag |= IXON;
    configured.c_iflag &= ~static_cast<tcflag_t>(IXANY);
    configured.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    ASSERT_EQ(tcsetattr(slave.fd, TCSANOW, &configured), 0);
    const termios captured = configured;

    const char stop = static_cast<char>(configured.c_cc[VSTOP]);
    ASSERT_EQ(write(master.fd, &stop, 1), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const ssize_t blocked_write = write(slave.fd, "x", 1);
    // Linux refuses the write while macOS may queue it. Neither may expose
    // output on the master until the cleanup helper releases XOFF.
    ASSERT_TRUE(
        blocked_write == 0 || blocked_write == 1 || (blocked_write < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    );
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::string output;
    drain_pty_output(master.fd, output);
    ASSERT_TRUE(output.empty());

    ScopedStdoutCapture capture;
    ASSERT_TRUE(capture.valid);
    ASSERT_TRUE(dup2(slave.fd, STDOUT_FILENO) >= 0);
    ASSERT_TRUE(platform::restart_terminal_output_for_cleanup());
    ASSERT_TRUE(platform::write_terminal_cleanup("cleanup"));

    const std::string expected = blocked_write == 1 ? "xcleanup" : "cleanup";
    await_pty(master.fd, output, [&]() { return output.size() >= expected.size(); });
    ASSERT_TRUE(output == expected);
    termios restored = {};
    ASSERT_EQ(tcgetattr(slave.fd, &restored), 0);
    ASSERT_TRUE(termios_equal(restored, captured));
}

TEST(platform, raw_mode_restoration_in_background_preserves_signal_mask)
{
    ScopedFd master(posix_openpt(O_RDWR | O_NOCTTY));
    ASSERT_TRUE(master.fd >= 0);
    ASSERT_EQ(grantpt(master.fd), 0);
    ASSERT_EQ(unlockpt(master.fd), 0);
    const char *path = ptsname(master.fd); // NOLINT(concurrency-mt-unsafe)
    ASSERT_TRUE(path != nullptr);
    ScopedFd slave(open(path, O_RDWR | O_NOCTTY));
    ASSERT_TRUE(slave.fd >= 0);
    flush_test_output_before_fork();
    const pid_t supervisor = fork();
    ASSERT_TRUE(supervisor >= 0);
    if (supervisor == 0)
    {
        if (setsid() < 0 || ioctl(slave.fd, TIOCSCTTY, 0) < 0 || dup2(slave.fd, STDIN_FILENO) < 0 ||
            std::signal(SIGTTOU, SIG_IGN) == SIG_ERR)
        {
            _exit(120);
        }
        const pid_t foreground = getpgrp();
        for (const bool initially_blocked : { false, true })
        {
            const pid_t worker = fork();
            if (worker < 0)
            {
                _exit(121);
            }
            if (worker == 0)
            {
                std::signal(SIGALRM, SIG_DFL);
                alarm(3);
                if (setpgid(0, 0) < 0 || tcsetpgrp(STDIN_FILENO, getpgrp()) < 0)
                {
                    _exit(122);
                }
                platform::ConsoleStateGuard state;
                if (!state.valid() || !state.enable_raw_mode() || tcsetpgrp(STDIN_FILENO, foreground) < 0 ||
                    std::signal(SIGTTOU, SIG_DFL) == SIG_ERR)
                {
                    _exit(123);
                }
                sigset_t signals = {};
                sigset_t after = {};
                if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGTTOU) != 0 ||
                    pthread_sigmask(initially_blocked ? SIG_BLOCK : SIG_UNBLOCK, &signals, nullptr) != 0 ||
                    !state.restore_raw_mode() || state.raw_mode_restore_pending() ||
                    pthread_sigmask(SIG_SETMASK, nullptr, &after) != 0 ||
                    sigismember(&after, SIGTTOU) != (initially_blocked ? 1 : 0))
                {
                    _exit(124);
                }
                _exit(0);
            }
            int status = 0;
            pid_t waited = 0;
            while ((waited = waitpid(worker, &status, WUNTRACED)) < 0 && errno == EINTR)
            {
            }
            if (waited != worker || !WIFEXITED(status))
            {
                kill(worker, SIGKILL);
                while (waitpid(worker, &status, 0) < 0 && errno == EINTR)
                {
                }
                _exit(125);
            }
            if (WEXITSTATUS(status) != 0)
            {
                _exit(WEXITSTATUS(status));
            }
        }
        _exit(0);
    }
    int status = 0;
    pid_t waited = 0;
    while ((waited = waitpid(supervisor, &status, 0)) < 0 && errno == EINTR)
    {
    }
    ASSERT_EQ(waited, supervisor);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
}

TEST(platform, terminal_cleanup_bounds_full_pipe_writes_and_restores_flags)
{
    flush_test_output_before_fork();
    const pid_t child = fork();
    ASSERT_TRUE(child >= 0);
    if (child == 0)
    {
        std::signal(SIGALRM, SIG_DFL);
        alarm(3);
        int fds[2] = { -1, -1 };
        if (pipe(fds) != 0 || fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0)
        {
            _exit(120);
        }
        char padding[4096] = {};
        while (write(fds[1], padding, sizeof padding) > 0)
        {
        }
        if (errno != EAGAIN || fcntl(fds[1], F_SETFL, 0) != 0 || dup2(fds[1], STDOUT_FILENO) < 0)
        {
            _exit(121);
        }
        const int flags = fcntl(fds[1], F_GETFL, 0);
        sigset_t before = {};
        sigset_t after = {};
        if (pthread_sigmask(SIG_SETMASK, nullptr, &before) != 0 || platform::write_terminal_cleanup("cleanup") ||
            fcntl(fds[1], F_GETFL, 0) != flags || pthread_sigmask(SIG_SETMASK, nullptr, &after) != 0 ||
            sigismember(&before, SIGTTOU) != sigismember(&after, SIGTTOU))
        {
            _exit(122);
        }
        _exit(0);
    }
    int status = 0;
    pid_t waited = 0;
    while ((waited = waitpid(child, &status, 0)) < 0 && errno == EINTR)
    {
    }
    ASSERT_EQ(waited, child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
}

TEST(platform, terminal_write_stops_on_permanent_error_or_no_progress)
{
    for (const int64_t result : { int64_t{ -1 }, int64_t{ 0 } })
    {
        terminal_write_script() = {};
        auto &script = terminal_write_script();
        script.counts[0] = 2;
        script.counts[1] = result;
        script.errors[1] = EIO;
        ASSERT_FALSE(platform::write_terminal("abcdef", 6, false, scripted_terminal_write));
        ASSERT_EQ(script.calls, 2);
        ASSERT_EQ(script.size, static_cast<size_t>(2));
        ASSERT_TRUE(std::memcmp(script.output, "ab", 2) == 0);
    }
}

TEST(platform, terminal_write_abandons_frame_but_finishes_cleanup_for_control_request)
{
    terminal_write_script() = {};
    auto &script = terminal_write_script();
    script.counts[0] = 8;
    script.suspend_at = 0;
    constexpr char frame[] = "\033[?2026hframe\033[?2026l";
    const bool frame_written = platform::write_terminal(frame, sizeof frame - 1, true, scripted_terminal_write);
    const int frame_calls = script.calls;
    script.counts[1] = -1;
    script.errors[1] = EINTR;
    script.counts[2] = 8;
    constexpr char cleanup[] = "\033[?2026l";
    const bool cleanup_written = platform::write_terminal(cleanup, sizeof cleanup - 1, false, scripted_terminal_write);
    platform::detail::suspend_flag = 0;
    ASSERT_FALSE(frame_written);
    ASSERT_EQ(frame_calls, 1);
    ASSERT_TRUE(cleanup_written);
    ASSERT_EQ(script.calls, 3);
    ASSERT_EQ(script.size, static_cast<size_t>(16));
    ASSERT_TRUE(std::memcmp(script.output, "\033[?2026h\033[?2026l", 16) == 0);
}

TEST(platform, frame_deadline_preserves_precision_after_long_uptime)
{
    using Clock = std::chrono::steady_clock;
    for (const auto uptime : { std::chrono::hours(24 * 7), std::chrono::hours(24 * 365) })
    {
        const Clock::time_point now(std::chrono::duration_cast<Clock::duration>(uptime));
        for (const float fps : { 30.0f, 60.0f, 240.0f, 2000.0f })
        {
            const std::chrono::duration<float> delay(1.0f / fps);
            const auto deadline = platform::detail::frame_deadline(now, delay);
            ASSERT_TRUE(deadline - now == std::chrono::duration_cast<Clock::duration>(delay));
        }
    }
}

TEST(platform, frame_wait_preserves_submillisecond_timeouts)
{
    using Clock = std::chrono::steady_clock;
    for (const float fps : { 240.0f, 2000.0f, 10000.0f })
    {
        auto now = Clock::time_point(std::chrono::hours(24 * 7));
        const auto deadline = platform::detail::frame_deadline(now, std::chrono::duration<float>(1.0f / fps));
        const auto expected = std::chrono::ceil<std::chrono::nanoseconds>(deadline - now);
        int calls = 0;
        platform::detail::wait_frame_until(
            deadline, [&]() { return now; },
            [&](const timespec *timeout, timespec *remainder)
            {
                ++calls;
                ASSERT_TRUE(remainder == nullptr);
                ASSERT_EQ(timeout->tv_sec, 0);
                ASSERT_EQ(timeout->tv_nsec, expected.count());
                now += std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(timeout->tv_nsec));
                return 0;
            }
        );
        ASSERT_EQ(calls, 1);
    }
}

TEST(platform, frame_wait_bounds_long_sleeps_and_skips_expired_deadlines)
{
    using Clock = std::chrono::steady_clock;
    for (const int milliseconds : { -1, 0, 125 })
    {
        Clock::time_point now;
        const auto deadline = now + std::chrono::milliseconds(milliseconds);
        std::vector<long> slices;
        platform::detail::wait_frame_until(
            deadline, [&]() { return now; },
            [&](const timespec *timeout, timespec *)
            {
                ASSERT_EQ(timeout->tv_sec, 0);
                slices.push_back(timeout->tv_nsec);
                now += std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(timeout->tv_nsec));
                return 0;
            }
        );
        if (milliseconds > 0)
        {
            ASSERT_TRUE(slices == std::vector<long>({ 50000000, 50000000, 25000000 }));
            ASSERT_TRUE(now == deadline);
        }
        else
        {
            ASSERT_TRUE(slices.empty());
        }
    }
}

TEST(platform, frame_wait_recomputes_deadline_after_unrelated_interruptions)
{
    using Clock = std::chrono::steady_clock;
    Clock::time_point now;
    const auto deadline = now + std::chrono::milliseconds(1);
    std::vector<long> slices;
    platform::detail::wait_frame_until(
        deadline, [&]() { return now; },
        [&](const timespec *timeout, timespec *)
        {
            slices.push_back(timeout->tv_nsec);
            if (slices.size() < 3)
            {
                now += std::chrono::microseconds(125);
                errno = EINTR;
                return -1;
            }
            now += std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(timeout->tv_nsec));
            return 0;
        }
    );
    ASSERT_TRUE(slices == std::vector<long>({ 1000000, 875000, 750000 }));
    ASSERT_TRUE(now == deadline);
}

TEST(platform, frame_wait_stops_for_control_requests_before_and_during_sleep)
{
    using Clock = std::chrono::steady_clock;
    for (const int signal : { SIGTSTP, SIGTERM })
    {
        for (const bool already_pending : { false, true })
        {
            const auto request = [signal]()
            {
                if (signal == SIGTSTP)
                {
                    platform::detail::job_control_handler(signal);
                }
                else
                {
                    platform::detail::signal_handler(signal);
                }
            };
            if (already_pending)
            {
                request();
            }
            int calls = 0;
            platform::detail::wait_frame_until(
                Clock::time_point(std::chrono::seconds(1)), []() { return Clock::time_point{}; },
                [&](const timespec *, timespec *)
                {
                    ++calls;
                    request();
                    errno = EINTR;
                    return -1;
                }
            );
            platform::detail::interrupt_flag = 0;
            platform::detail::suspend_flag = 0;
            ASSERT_EQ(calls, already_pending ? 0 : 1);
        }
    }
}

TEST(platform, frame_wait_stops_on_permanent_sleep_error)
{
    using Clock = std::chrono::steady_clock;
    int calls = 0;
    platform::detail::wait_frame_until(
        Clock::time_point(std::chrono::seconds(1)), []() { return Clock::time_point{}; },
        [&](const timespec *, timespec *)
        {
            ++calls;
            errno = EINVAL;
            return -1;
        }
    );
    ASSERT_EQ(calls, 1);
}

TEST(platform, graphics_query_write_records_control_cancellation_at_each_prefix)
{
    constexpr char partial_query[] = "\033[?1049h\033_Gi=31,";
    // Include cancellation before alternate-screen entry, when recovery reaches the shell.
    for (size_t split = 0; split < sizeof partial_query; ++split)
    {
        ScopedStdoutCapture recovery;
        ASSERT_TRUE(recovery.valid);
        terminal_write_script() = {};
        auto &script = terminal_write_script();
        script.counts[0] = static_cast<int64_t>(split);
        script.suspend_at = 0;
        script.write_through = true;
        if (split == 0)
        {
            platform::detail::suspend_flag = 1;
        }
        constexpr char pending_output[] = "pending output";
        std::fputs(pending_output, stdout);
        const TermGraphics result = platform::query_term_graphics(
            [](char *, int, int)
            {
                ASSERT_FAIL("query read ran after its write was canceled");
                return -1;
            },
            scripted_terminal_write
        );
        platform::detail::suspend_flag = 0;
        ASSERT_TRUE(result.interrupted);
        ASSERT_FALSE(result.failed);
        ASSERT_EQ(script.calls, split == 0 ? 0 : 1);
        ASSERT_EQ(script.size, split);
        ASSERT_TRUE(std::memcmp(script.output, partial_query, split) == 0);
        ASSERT_TRUE(platform::exit_alt_screen());
        ASSERT_TRUE(
            recovery.read() ==
            std::string(pending_output) + std::string(partial_query, split) + "\033\\\033\\\033[?1049l"
        );
    }
}

TEST(platform, graphics_query_write_failure_is_not_missing_protocol_support)
{
    for (const size_t split : { size_t{ 0 }, size_t{ 2 }, size_t{ 12 } })
    {
        ScopedStdoutCapture recovery;
        ASSERT_TRUE(recovery.valid);
        terminal_write_script() = {};
        auto &script = terminal_write_script();
        const int failure = split == 0 ? 0 : 1;
        script.counts[0] = static_cast<int64_t>(split);
        script.counts[failure] = -1;
        script.errors[failure] = EIO;
        script.write_through = true;
        const TermGraphics result = platform::query_term_graphics(
            [](char *, int, int)
            {
                ASSERT_FAIL("query read ran after an output failure");
                return -1;
            },
            scripted_terminal_write
        );
        ASSERT_TRUE(result.failed);
        ASSERT_FALSE(result.interrupted);
        ASSERT_EQ(script.size, split);
        ASSERT_TRUE(recovery.read() == std::string(script.output, script.size) + "\033\\");
    }
}

TEST(platform, query_interruption_survives_canceled_suspend)
{
    ScopedStdoutCapture output;
    ASSERT_TRUE(output.valid);
    const TermGraphics interrupted = platform::query_term_graphics(
        [](char *, int, int timeout_ms)
        {
            if (timeout_ms == 0)
            {
                // Cancel the stop during the drain, after the query has aborted.
                platform::detail::job_control_handler(SIGCONT);
                return -1;
            }
            platform::detail::job_control_handler(SIGTSTP);
            return 0;
        }
    );
    ASSERT_TRUE(interrupted.interrupted);
    ASSERT_FALSE(platform::control_requested());

    const TermGraphics retried = platform::query_term_graphics(
        [](char *out, int cap, int timeout_ms)
        {
            if (timeout_ms == 0)
            {
                return -1;
            }
            constexpr char reply[] = "\033_Gi=31;OK\033\\\033[0n";
            constexpr int length = sizeof reply - 1;
            ASSERT_TRUE(cap >= length);
            std::memcpy(out, reply, length);
            return length;
        }
    );
    ASSERT_FALSE(retried.interrupted);
    ASSERT_TRUE(retried.kitty);
    const std::string queries = output.read();
    const size_t first = queries.find("\033[5n");
    ASSERT_TRUE(first != std::string::npos);
    ASSERT_TRUE(queries.find("\033[5n", first + 1) != std::string::npos);
}

TEST(platform, query_continues_when_suspend_is_canceled_before_observation)
{
    ScopedStdoutCapture output;
    ASSERT_TRUE(output.valid);
    const TermGraphics completed = platform::query_term_graphics(
        [](char *out, int cap, int timeout_ms)
        {
            if (timeout_ms == 0)
            {
                return -1;
            }
            platform::detail::job_control_handler(SIGTSTP);
            platform::detail::job_control_handler(SIGCONT);
            constexpr char reply[] = "\033[?62;4c\033[0n";
            constexpr int length = sizeof reply - 1;
            ASSERT_TRUE(cap >= length);
            std::memcpy(out, reply, length);
            return length;
        }
    );
    ASSERT_FALSE(completed.interrupted);
    ASSERT_TRUE(completed.sixel);
    ASSERT_FALSE(platform::control_requested());
}

TEST(platform, job_control_restarts_interrupted_startup_query)
{
    verify_job_control(JobControlCase::Startup);
}

namespace
{
    void verify_ctrl_z_and_fg_under_shell(const char *shell_path)
    {
        static constexpr char model_data[] = "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n";
        const std::string model_name = "rasterminal_shell_job_" + std::to_string(getpid()) + ".obj";
        ScopedTmpFile model(model_name.c_str(), model_data, sizeof model_data - 1);
        ScopedFd master(posix_openpt(O_RDWR | O_NOCTTY));
        ASSERT_TRUE(master.fd >= 0);
        ASSERT_TRUE(grantpt(master.fd) == 0);
        ASSERT_TRUE(unlockpt(master.fd) == 0);
        const char *slave_name = ptsname(master.fd); // NOLINT(concurrency-mt-unsafe)
        ASSERT_TRUE(slave_name != nullptr);
        ScopedFd slave(open(slave_name, O_RDWR | O_NOCTTY));
        ASSERT_TRUE(slave.fd >= 0);
        winsize size = {};
        size.ws_row = 24;
        size.ws_col = 80;
        ASSERT_TRUE(ioctl(slave.fd, TIOCSWINSZ, &size) == 0);
        termios baseline = {};
        int app_pids[2] = { -1, -1 };
        ASSERT_TRUE(pipe(app_pids) == 0);
        ScopedFd app_pid_read(app_pids[0]);
        ScopedFd app_pid_write(app_pids[1]);
        const pid_t shell = fork();
        ASSERT_TRUE(shell >= 0);
        if (shell == 0)
        {
            if (setsid() < 0 || ioctl(slave.fd, TIOCSCTTY, 0) < 0 || tcsetpgrp(slave.fd, getpgrp()) < 0 ||
                dup2(slave.fd, STDIN_FILENO) < 0 || dup2(slave.fd, STDOUT_FILENO) < 0 ||
                dup2(slave.fd, STDERR_FILENO) < 0)
            {
                _exit(120);
            }
            close(master.fd);
            if (slave.fd > STDERR_FILENO)
            {
                close(slave.fd);
            }
            close(app_pid_read.fd);
            if (dup2(app_pid_write.fd, 3) < 0)
            {
                _exit(123);
            }
            if (app_pid_write.fd != 3)
            {
                close(app_pid_write.fd);
            }
            if (setenv("PS1", "RASTERMINAL_SHELL_READY>", 1) != 0 || // NOLINT(concurrency-mt-unsafe)
                setenv("TERM", "xterm-256color", 1) != 0 ||          // NOLINT(concurrency-mt-unsafe)
                unsetenv("ENV") != 0)                                // NOLINT(concurrency-mt-unsafe)
            {
                _exit(121);
            }
            execl(shell_path, "sh", "-i", static_cast<char *>(nullptr));
            _exit(122);
        }
        ScopedPosixSession session(shell);
        const int flags = fcntl(master.fd, F_GETFL, 0);
        ASSERT_TRUE(flags >= 0);
        ASSERT_TRUE(fcntl(master.fd, F_SETFL, flags | O_NONBLOCK) == 0);
        std::string output;
        size_t offset = 0;
        const auto prompt = [&]() { return output.find("RASTERMINAL_SHELL_READY>", offset) != std::string::npos; };
        await_pty(master.fd, output, prompt);
        const auto capture_shell_termios = [&](termios &mode)
        {
            // Readline changes termios while displaying a prompt. A plain read
            // keeps the shell in command mode until the snapshot is complete.
            offset = output.size();
            constexpr char gate[] = "printf '%s%s\\n' RASTERMINAL_ TERMIOS_READY; read -r rasterminal_gate\n";
            ASSERT_TRUE(write_pipe_value(master.fd, gate, sizeof gate - 1));
            await_pty(
                master.fd, output,
                [&]() { return output.find("RASTERMINAL_TERMIOS_READY", offset) != std::string::npos; }
            );
            ASSERT_TRUE(tcgetattr(slave.fd, &mode) == 0);
            offset = output.size();
            constexpr char release = '\n';
            ASSERT_TRUE(write_pipe_value(master.fd, &release, sizeof release));
            await_pty(master.fd, output, prompt);
        };
        capture_shell_termios(baseline);
        const auto quote = [](const std::string &value)
        {
            std::string escaped = "'";
            for (const char c : value)
            {
                if (c == '\'')
                {
                    escaped += "'\\''";
                }
                else
                {
                    escaped += c;
                }
            }
            return escaped + "'";
        };
        const std::string launch =
            R"(sh -c 'printf "%s\n" "$$" >&3; exec "$@"' rasterminal )" + quote(RASTERMINAL_TEST_BINARY) +
            " --graphics=blocks --no-ao --no-hud --no-spin --no-input --threads=4 " + quote(model.path) + "\n";
        offset = output.size();
        ASSERT_TRUE(write_pipe_value(master.fd, launch.data(), launch.size()));
        await_pty(
            master.fd, output,
            [&]()
            {
                pollfd fd = { app_pid_read.fd, POLLIN, 0 };
                return poll(&fd, 1, 0) > 0;
            }
        );
        char app_pid[32] = {};
        ASSERT_TRUE(read(app_pid_read.fd, app_pid, sizeof app_pid - 1) > 0);
        session.app_group = static_cast<pid_t>(std::strtol(app_pid, nullptr, 10));
        ASSERT_TRUE(session.app_group > 0 && session.app_group != shell);
        const auto rendered = [&]() { return output.find("\033[?2026l", offset) != std::string::npos; };
        await_pty(master.fd, output, rendered);
        offset = output.size();
        const cc_t stop_key = baseline.c_cc[VSUSP];
        ASSERT_TRUE(write_pipe_value(master.fd, &stop_key, sizeof stop_key));
        await_pty(master.fd, output, prompt);
        ASSERT_TRUE(output.find("\033[?25h\033[0m\033[?1049l", offset) != std::string::npos);
        ASSERT_TRUE(output.find("\033[?1002l\033[?1006l", offset) != std::string::npos);
        termios stopped = {};
        capture_shell_termios(stopped);
        ASSERT_TRUE(termios_equal(stopped, baseline));
        offset = output.size();
        constexpr char foreground[] = "fg\n";
        ASSERT_TRUE(write_pipe_value(master.fd, foreground, sizeof foreground - 1));
        await_pty(master.fd, output, rendered);
        ASSERT_TRUE(output.find("\033[?1049h\033[?25l", offset) != std::string::npos);
        offset = output.size();
        constexpr char quit = 'Q';
        ASSERT_TRUE(write_pipe_value(master.fd, &quit, sizeof quit));
        await_pty(master.fd, output, prompt);
        session.app_group = -1;
        offset = output.size();
        constexpr char status_command[] = "printf 'JOB_RESULT=%s\\n' \"$?\"\n";
        ASSERT_TRUE(write_pipe_value(master.fd, status_command, sizeof status_command - 1));
        await_pty(master.fd, output, prompt);
        ASSERT_TRUE(output.find("JOB_RESULT=0\r\n", offset) != std::string::npos);
        constexpr char exit_command[] = "exit\n";
        ASSERT_TRUE(write_pipe_value(master.fd, exit_command, sizeof exit_command - 1));
        int status = 0;
        await_pty(master.fd, output, [&]() { return waitpid(shell, &status, WNOHANG) == shell; });
        session.pid = -1;
        ASSERT_TRUE(WIFEXITED(status));
        ASSERT_EQ(WEXITSTATUS(status), 0);
    }
} // namespace

TEST(platform, job_control_ctrl_z_and_fg_under_interactive_shell)
{
    verify_ctrl_z_and_fg_under_shell("/bin/sh");
}

TEST(platform, job_control_ctrl_z_and_fg_under_readline_shell)
{
    verify_ctrl_z_and_fg_under_shell("/bin/bash");
}

TEST(platform, job_control_resumes_idle_renderer)
{
    verify_job_control(JobControlCase::Idle);
}

TEST(platform, job_control_repeats_during_active_rendering_and_mouse_drag)
{
    verify_job_control(JobControlCase::DragAndRepeat);
}

TEST(platform, job_control_background_continuation_leaves_terminal_alone)
{
    verify_job_control(JobControlCase::Background);
}

TEST(platform, job_control_closes_synchronization_after_interrupted_frame_write)
{
    verify_job_control(JobControlCase::BlockedOutput);
}

TEST(platform, job_control_suspends_while_terminal_output_remains_paused)
{
    verify_job_control(JobControlCase::PausedOutput);
}

TEST(platform, job_control_suspends_with_paused_exclusive_terminal)
{
    verify_job_control(JobControlCase::PausedOutput, true);
}

TEST(platform, interactive_process_cleans_up_exclusive_terminal_after_quit)
{
    verify_interactive_process_cleans_up_terminal(0, true);
}

TEST(platform, job_control_preserves_termination_while_stopped)
{
    verify_job_control(JobControlCase::TerminateStopped);
}

TEST(platform, job_control_retries_timed_out_terminal_release_when_terminated_while_stopped)
{
    verify_job_control(JobControlCase::PausedTerminateStopped);
}

TEST(platform, job_control_retries_startup_terminal_release_when_terminated_while_stopped)
{
    verify_job_control(JobControlCase::StartupPausedTerminateStopped);
}

TEST(platform, job_control_terminates_during_continuation_with_output_still_paused)
{
    verify_job_control(JobControlCase::PausedContinuation);
}

TEST(platform, job_control_workers_inherit_blocked_signals)
{
    const pid_t child = fork();
    ASSERT_TRUE(child >= 0);
    if (child == 0)
    {
        for (const int signal : { SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGTSTP, SIGCONT })
        {
            if (std::signal(signal, SIG_DFL) == SIG_ERR)
            {
                _exit(119);
            }
        }
        if (!platform::install_interrupt_handler() || !platform::install_job_control_handler())
        {
            _exit(120);
        }
        sigset_t before = {};
        if (pthread_sigmask(SIG_SETMASK, nullptr, &before) != 0)
        {
            _exit(121);
        }
        bool worker_blocked = false;
        std::thread worker;
        {
            const platform::WorkerSignalMask guard;
            if (!guard.valid())
            {
                _exit(122);
            }
            worker = std::thread(
                [&]()
                {
                    sigset_t mask = {};
                    worker_blocked = pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0;
                    for (const int signal : { SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGTSTP, SIGCONT })
                    {
                        worker_blocked = worker_blocked && sigismember(&mask, signal) == 1;
                    }
                }
            );
        }
        worker.join();
        sigset_t after = {};
        if (!worker_blocked || pthread_sigmask(SIG_SETMASK, nullptr, &after) != 0)
        {
            _exit(123);
        }
        for (const int signal : { SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGTSTP, SIGCONT })
        {
            if (sigismember(&before, signal) != sigismember(&after, signal))
            {
                _exit(124);
            }
        }
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
}

TEST(platform, input_reset_discards_partial_reports_and_rate_state)
{
    auto &pending = platform::detail::pending();
    pending.len = 4;
    pending.skipping = true;
    pending.refills = 7;
    pending.meter.record(100);
    pending.meter.window = std::chrono::steady_clock::now();
    pending.last_growth = pending.meter.window;
    platform::reset_input_state();
    ASSERT_EQ(pending.len, 0);
    ASSERT_FALSE(pending.skipping);
    ASSERT_EQ(pending.refills, 0);
    ASSERT_EQ(pending.meter.credit, 0);
    ASSERT_TRUE(pending.meter.window == std::chrono::steady_clock::time_point{});
    ASSERT_TRUE(pending.last_growth == std::chrono::steady_clock::time_point{});
}

TEST(platform, job_control_requests_are_separate_and_continue_cancels_stop)
{
    platform::detail::suspend_flag = 0;
    platform::detail::job_control_handler(SIGTSTP);
    ASSERT_TRUE(platform::suspend_requested());
    ASSERT_FALSE(platform::interrupt_requested());
    platform::detail::job_control_handler(SIGCONT);
    ASSERT_FALSE(platform::suspend_requested());
}

namespace
{
    bool checked_handoff_handler(void (*handler)(int))
    {
        if (handler == platform::detail::job_control_handler)
        {
            sigset_t pending = {};
            sigset_t mask = {};
            if (sigpending(&pending) != 0 || pthread_sigmask(SIG_SETMASK, nullptr, &mask) != 0 ||
                sigismember(&pending, SIGTSTP) != 0 || sigismember(&mask, SIGTSTP) != 1)
            {
                return false;
            }
        }
        return platform::detail::set_suspend_handler(handler);
    }
} // namespace

TEST(platform, job_control_handoff_preserves_continuation_at_each_stop_boundary)
{
    enum class ContinueAt : uint8_t
    {
        BeforeHandoff,
        BeforeRaise,
        AfterRaise,
        DefaultHandler,
        AfterStop,
    };
    for (const ContinueAt when : { ContinueAt::BeforeHandoff, ContinueAt::BeforeRaise, ContinueAt::AfterRaise,
                                   ContinueAt::DefaultHandler, ContinueAt::AfterStop })
    {
        for (const bool ignored_continue : { false, true })
        {
            const bool expect_stop = when == ContinueAt::AfterStop;
            const pid_t child = fork();
            ASSERT_TRUE(child >= 0);
            if (child == 0)
            {
                // A separate group with a live parent in the same session is not
                // orphaned, so a mistakenly delivered SIGTSTP really stops it.
                if (setpgid(0, 0) != 0 || std::signal(SIGALRM, SIG_DFL) == SIG_ERR ||
                    std::signal(SIGTSTP, SIG_DFL) == SIG_ERR ||
                    std::signal(SIGCONT, ignored_continue ? SIG_IGN : SIG_DFL) == SIG_ERR)
                {
                    _exit(120);
                }
                alarm(5);
                sigset_t mask = {};
                if (sigemptyset(&mask) != 0 || sigaddset(&mask, SIGUSR1) != 0 ||
                    pthread_sigmask(SIG_SETMASK, &mask, nullptr) != 0 || !platform::install_job_control_handler() ||
                    std::raise(SIGTSTP) != 0)
                {
                    _exit(121);
                }
                if (when == ContinueAt::BeforeHandoff)
                {
                    if (sigaddset(&mask, SIGCONT) != 0 || pthread_sigmask(SIG_SETMASK, &mask, nullptr) != 0 ||
                        std::raise(SIGCONT) != 0)
                    {
                        _exit(122);
                    }
                }
                platform::detail::RaiseSignalFn raise_signal = std::raise;
                platform::detail::SetSuspendHandlerFn set_handler = checked_handoff_handler;
                if (when == ContinueAt::BeforeRaise)
                {
                    raise_signal = [](int signal) { return std::raise(SIGCONT) == 0 ? std::raise(signal) : -1; };
                }
                else if (when == ContinueAt::AfterRaise)
                {
                    raise_signal = [](int signal) { return std::raise(signal) == 0 ? std::raise(SIGCONT) : -1; };
                }
                else if (when == ContinueAt::DefaultHandler)
                {
                    set_handler = [](void (*handler)(int))
                    { return checked_handoff_handler(handler) && (handler != SIG_DFL || std::raise(SIGCONT) == 0); };
                }
                if (!platform::detail::suspend_handoff(set_handler, raise_signal) || platform::suspend_requested())
                {
                    _exit(123);
                }
                sigset_t restored = {};
                struct sigaction action = {};
                if (pthread_sigmask(SIG_SETMASK, nullptr, &restored) != 0 ||
                    sigaction(SIGTSTP, nullptr, &action) != 0 ||
                    action.sa_handler != platform::detail::job_control_handler ||
                    sigaction(SIGCONT, nullptr, &action) != 0 ||
                    action.sa_handler != platform::detail::job_control_handler)
                {
                    _exit(124);
                }
                for (const int signal : { SIGTSTP, SIGCONT, SIGUSR1, SIGUSR2, SIGINT, SIGTERM, SIGQUIT, SIGHUP })
                {
                    if (sigismember(&mask, signal) != sigismember(&restored, signal))
                    {
                        _exit(125);
                    }
                }
                _exit(0);
            }
            int status = 0;
            pid_t waited = 0;
            while ((waited = waitpid(child, &status, WUNTRACED)) < 0 && errno == EINTR)
            {
            }
            const bool stopped = waited == child && WIFSTOPPED(status);
            const int stop_signal = stopped ? WSTOPSIG(status) : 0;
            if (stopped)
            {
                // Do not rescue a lost continuation: terminate the child and fail.
                kill(child, expect_stop ? SIGCONT : SIGKILL);
                while ((waited = waitpid(child, &status, 0)) < 0 && errno == EINTR)
                {
                }
            }
            ASSERT_EQ(waited, child);
            ASSERT_EQ(stopped, expect_stop);
            if (stopped)
            {
                ASSERT_EQ(stop_signal, SIGTSTP);
            }
            ASSERT_TRUE(WIFEXITED(status));
            ASSERT_EQ(WEXITSTATUS(status), 0);
        }
    }
}

TEST(platform, job_control_handoff_preserves_termination_at_default_handler)
{
    for (const int termination : { SIGINT, SIGTERM, SIGQUIT, SIGHUP })
    {
        for (const bool initially_blocked : { false, true })
        {
            const pid_t child = fork();
            ASSERT_TRUE(child >= 0);
            if (child == 0)
            {
                // Keep the group non-orphaned so the regression produces a real stop.
                sigset_t mask = {};
                if (setpgid(0, 0) != 0 || sigemptyset(&mask) != 0 || sigaddset(&mask, SIGUSR1) != 0 ||
                    (initially_blocked && sigaddset(&mask, termination) != 0) ||
                    pthread_sigmask(SIG_SETMASK, &mask, nullptr) != 0 || std::signal(termination, SIG_DFL) == SIG_ERR ||
                    std::signal(SIGTSTP, SIG_DFL) == SIG_ERR || !platform::install_interrupt_handler() ||
                    !platform::install_job_control_handler() || std::raise(SIGTSTP) != 0)
                {
                    _exit(120);
                }
                static int injected_signal = 0;
                injected_signal = termination;
                const bool completed = platform::detail::suspend_handoff(
                    [](void (*handler)(int)) {
                        return checked_handoff_handler(handler) &&
                               (handler != SIG_DFL || std::raise(injected_signal) == 0);
                    }
                );
                sigset_t restored = {};
                if (!completed || platform::suspend_requested() ||
                    platform::detail::interrupt_flag != (initially_blocked ? 0 : termination) ||
                    pthread_sigmask(SIG_SETMASK, nullptr, &restored) != 0)
                {
                    _exit(121);
                }
                for (const int signal : { SIGTSTP, SIGCONT, SIGUSR1, SIGINT, SIGTERM, SIGQUIT, SIGHUP })
                {
                    if (sigismember(&mask, signal) != sigismember(&restored, signal))
                    {
                        _exit(122);
                    }
                }
                _exit(0);
            }
            int status = 0;
            pid_t waited = 0;
            while ((waited = waitpid(child, &status, WUNTRACED)) < 0 && errno == EINTR)
            {
            }
            const bool stopped = waited == child && WIFSTOPPED(status);
            if (stopped)
            {
                kill(child, initially_blocked ? SIGCONT : SIGKILL);
                while ((waited = waitpid(child, &status, 0)) < 0 && errno == EINTR)
                {
                }
            }
            ASSERT_EQ(waited, child);
            ASSERT_EQ(stopped, initially_blocked);
            ASSERT_TRUE(WIFEXITED(status));
            ASSERT_EQ(WEXITSTATUS(status), 0);
        }
    }
}

TEST(platform, job_control_install_preserves_ignored_stop)
{
    const pid_t child = fork();
    ASSERT_TRUE(child >= 0);
    if (child == 0)
    {
        if (std::signal(SIGTSTP, SIG_IGN) == SIG_ERR || !platform::install_job_control_handler())
        {
            _exit(1);
        }
        struct sigaction action = {};
        _exit(
            sigaction(SIGTSTP, nullptr, &action) == 0 && action.sa_handler == SIG_IGN &&
                    !platform::detail::job_control_installed
                ? 0
                : 2
        );
    }
    int status = 0;
    pid_t waited = 0;
    while ((waited = waitpid(child, &status, 0)) < 0 && errno == EINTR)
    {
    }
    ASSERT_EQ(waited, child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
}

TEST(platform, job_control_preserves_ignored_continue_through_cleanup)
{
    const pid_t child = fork();
    ASSERT_TRUE(child >= 0);
    if (child == 0)
    {
        if (std::signal(SIGTSTP, SIG_DFL) == SIG_ERR || std::signal(SIGCONT, SIG_IGN) == SIG_ERR ||
            !platform::install_job_control_handler())
        {
            _exit(120);
        }
        struct sigaction action = {};
        if (sigaction(SIGCONT, nullptr, &action) != 0 || action.sa_handler != platform::detail::job_control_handler ||
            platform::finish_termination(0) != 0)
        {
            _exit(121);
        }
        _exit(sigaction(SIGCONT, nullptr, &action) == 0 && action.sa_handler == SIG_IGN ? 0 : 122);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
}

TEST(platform, interactive_process_cleans_up_terminal_after_quit)
{
    verify_interactive_process_cleans_up_terminal(0);
}

TEST(platform, interactive_process_cleans_up_terminal_after_sigint)
{
    verify_interactive_process_cleans_up_terminal(SIGINT);
}

TEST(platform, interactive_process_cleans_up_terminal_after_sigterm)
{
    verify_interactive_process_cleans_up_terminal(SIGTERM);
}

TEST(platform, interactive_process_cleans_up_terminal_after_sigquit)
{
    verify_interactive_process_cleans_up_terminal(SIGQUIT);
}

TEST(platform, interactive_process_cleans_up_terminal_after_sighup)
{
    verify_interactive_process_cleans_up_terminal(SIGHUP);
}
#endif

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
