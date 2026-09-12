#include "tests/test.h"
#include "tests/platform/test_util.h"
#include "tests/platform/test_termios_util.h"
#include "src/platform/console.h"
#include "src/platform/terminal_io.h"

// <cstdlib> need not expose POSIX functions in the global namespace.
#include <stdlib.h> // NOLINT(modernize-deprecated-headers,hicpp-deprecated-headers)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include "src/args.h"
#include "src/platform/control.h"
#include "src/platform/terminal_query.h"
#include "src/version.h"
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
// control.h removes the Windows near and far macros.
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using platform_test::ScopedFd;
#ifdef _WIN32
using platform_test::ScopedStdoutCapture;
#endif
#ifndef _WIN32
using platform_test::alter_termios_field;
using platform_test::PartialTermiosField;
using platform_test::reset_termios_calls;
using platform_test::scripted_tcflush;
using platform_test::scripted_tcgetattr;
using platform_test::scripted_tcsetattr;
using platform_test::termios_calls;
using platform_test::termios_equal;
#endif

namespace
{
    constexpr platform::TermColor DUMB = platform::TermColor::Dumb;
    constexpr platform::TermColor P256 = platform::TermColor::Palette256;
    constexpr platform::TermColor TC = platform::TermColor::TrueColor;

    // Exercise every classifier branch during constant evaluation.
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

    // CTest may replace standard handles with pipes, so open the console devices.
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
            // AllocConsole cannot replace a partially accessible attached console.
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
        // Verify the guard restored the host before the fixture repairs anything.
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
            // Deliver events while the reader can be blocked in _getch.
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
            // Repeat after cleanup without adding another wake record.
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
            // The flag proves the late handler registered before this barrier.
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
        // _getch has no documented error return; the interrupt flag rejects the byte.
        platform::detail::interrupt_flag.store(true);
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
        platform::detail::interrupt_flag.store(false);
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

// _isatty accepts NUL, but GetConsoleMode rejects it.
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
        // Repeated setup must preserve the original restoration state.
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

TEST(platform, console_state_guard_rearms_input_restore_after_raw_mode_resumes)
{
    ScopedWindowsConsole console;
    ASSERT_TRUE(console.valid);

    const DWORD input_mode = console.saved_input_mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT;
    ASSERT_TRUE(SetConsoleMode(console.input, input_mode) != 0);
    DWORD baseline_input_mode = 0;
    ASSERT_TRUE(GetConsoleMode(console.input, &baseline_input_mode) != 0);

    {
        platform::ConsoleStateGuard guard;
        ASSERT_TRUE(guard.valid());
        ASSERT_TRUE(platform::enable_raw_mode(&guard));
        ASSERT_TRUE(platform::disable_raw_mode(&guard));
        ASSERT_TRUE(platform::resume_raw_mode(guard));

        DWORD resumed_input_mode = 0;
        ASSERT_TRUE(GetConsoleMode(console.input, &resumed_input_mode) != 0);
        ASSERT_TRUE((resumed_input_mode & ENABLE_LINE_INPUT) == 0);
        ASSERT_TRUE((resumed_input_mode & ENABLE_ECHO_INPUT) == 0);
    }

    DWORD restored_input_mode = 0;
    ASSERT_TRUE(GetConsoleMode(console.input, &restored_input_mode) != 0);
    ASSERT_EQ(restored_input_mode, baseline_input_mode);
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
    // An incomplete snapshot must not restore unrelated state.
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
    // A partial capture must not overwrite newer state.
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

    // The guard retries after session cleanup.
    ASSERT_EQ(termios_calls().set_calls, 4);
    ASSERT_TRUE(termios_equal(termios_calls().writes[3], termios_calls().captured));
}

// PTY master behavior varies by OS, so probe only the slave.
TEST(platform, is_tty_true_for_pty_slave)
{
    ScopedFd master(posix_openpt(O_RDWR | O_NOCTTY));
    ASSERT_TRUE(master.fd >= 0);
    ASSERT_TRUE(grantpt(master.fd) == 0);
    ASSERT_TRUE(unlockpt(master.fd) == 0);
    // ptsname_r is unavailable on macOS; this test is single-threaded.
    const char *slave_name = ptsname(master.fd); // NOLINT(concurrency-mt-unsafe)
    ASSERT_TRUE(slave_name != nullptr);
    ScopedFd slave(open(slave_name, O_RDWR | O_NOCTTY));
    ASSERT_TRUE(slave.fd >= 0);
    ASSERT_TRUE(platform::is_tty(slave.fd));
}
#endif

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
    // TERM=dumb cannot render escapes regardless of COLORTERM.
    ASSERT_EQ(platform::classify_term_color("truecolor", "dumb", P256, false, false), DUMB);
    ASSERT_EQ(platform::classify_term_color("24bit", "DUMB", TC, false, false), DUMB);
}

TEST(platform, classify_dumb_is_exact_match_not_substring)
{
    // Only an exact TERM=dumb match is fatal.
    ASSERT_EQ(platform::classify_term_color(nullptr, "dumbo", P256, false, false), P256);
    ASSERT_EQ(platform::classify_term_color(nullptr, "xterm-dumbnot", TC, false, false), P256);
}

TEST(platform, classify_colorterm_truecolor)
{
    ASSERT_EQ(platform::classify_term_color("truecolor", "xterm-256color", P256, false, false), TC);
    ASSERT_EQ(platform::classify_term_color("24bit", "xterm", P256, false, false), TC);
    ASSERT_EQ(platform::classify_term_color("TRUECOLOR", nullptr, P256, false, false), TC);
    ASSERT_EQ(platform::classify_term_color("Truecolor", "xterm", P256, false, false), TC);
}

TEST(platform, classify_colorterm_unrecognized_falls_through)
{
    // COLORTERM=1 or yes does not specify color depth.
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
    ASSERT_EQ(platform::classify_term_color(nullptr, "xterm-24bit", P256, false, false), TC);
    ASSERT_EQ(platform::classify_term_color("gnome-terminal", "xterm-direct", P256, false, false), TC);
}

TEST(platform, classify_known_truecolor_terms)
{
    // Recognize known truecolor TERM values without COLORTERM.
    const char *terms[] = { "xterm-kitty", "wezterm", "alacritty", "xterm-ghostty", "foot", "contour" };
    for (const char *t : terms)
    {
        ASSERT_EQ(platform::classify_term_color(nullptr, t, P256, false, false), TC);
    }
    ASSERT_EQ(platform::classify_term_color(nullptr, "XTERM-KITTY", P256, false, false), TC);
}

TEST(platform, classify_real_screen_floors_colorterm)
{
    // GNU screen 4.x misparses 24-bit SGR.
    ASSERT_EQ(platform::classify_term_color("truecolor", "screen", P256, false, false), P256);
    ASSERT_EQ(platform::classify_term_color("truecolor", "screen-256color", P256, false, false), P256);
    ASSERT_EQ(platform::classify_term_color("truecolor", "screen.xterm-256color", P256, false, false), P256);
    ASSERT_EQ(platform::classify_term_color("24bit", "screen", P256, false, false), P256);
    ASSERT_EQ(platform::classify_term_color("truecolor", "SCREEN-256COLOR", P256, false, false), P256);
    // Match screen only as a TERM prefix.
    ASSERT_EQ(platform::classify_term_color("truecolor", "xterm-screenish", P256, false, false), TC);
}

TEST(platform, classify_sty_floors_regardless_of_term_and_tmux)
{
    // STY identifies screen even if TERM was rewritten.
    ASSERT_EQ(platform::classify_term_color("truecolor", "xterm-256color", P256, false, true), P256);
    ASSERT_EQ(platform::classify_term_color("truecolor", "screen", P256, true, true), P256);
    ASSERT_EQ(platform::classify_term_color(nullptr, nullptr, TC, false, true), P256);
    ASSERT_EQ(platform::classify_term_color("truecolor", "dumb", P256, true, true), DUMB);
}

TEST(platform, classify_screen_under_tmux_keeps_colorterm)
{
    // TMUX distinguishes tmux from GNU screen with the same TERM value.
    ASSERT_EQ(platform::classify_term_color("truecolor", "screen-256color", P256, true, false), TC);
    ASSERT_EQ(platform::classify_term_color("truecolor", "screen", P256, true, false), TC);
    ASSERT_EQ(platform::classify_term_color(nullptr, "screen-256color", P256, true, false), P256);
}

TEST(platform, classify_plain_terms_are_256)
{
    const char *terms[] = { "xterm", "xterm-256color", "tmux-256color", "linux", "vt100", "st-256color" };
    for (const char *t : terms)
    {
        ASSERT_EQ(platform::classify_term_color(nullptr, t, TC, false, false), P256);
    }
}

#ifndef _WIN32
// Windows VT support depends on the attached stdout handle.
TEST(platform, init_console_output_ok_on_posix)
{
    ASSERT_TRUE(platform::init_console_output());
}
#endif

namespace
{
    // The test binary is single-threaded, so environment mutation is safe.
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

    // Restore environment changes even after a fatal assertion.
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

    // screen caps color depth; tmux preserves inherited truecolor support.
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
    // POSIX preserves empty variables; _putenv_s deletes them on Windows.
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
    // Unset values use each platform's default.
#ifdef _WIN32
    ASSERT_EQ(platform::detect_term_color(), TC);
#else
    ASSERT_EQ(platform::detect_term_color(), P256);
#endif
}
