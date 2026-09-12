#pragma once

#include <csignal>
#include <cstdint>

#ifdef _WIN32
#include <atomic>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <conio.h>
#include <cstdio>
#include <io.h>
#include <windows.h>
// Windows defines near and far as macros.
#undef near
#undef far
#else
#include <poll.h>
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
#ifndef _WIN32
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): main-thread signal requests
        inline volatile std::sig_atomic_t suspend_flag = 0;
#endif
    } // namespace detail

    [[nodiscard]] inline bool interrupt_requested() noexcept
    {
#ifdef _WIN32
        return detail::interrupt_flag.load(std::memory_order_acquire);
#else
        return detail::interrupt_flag != 0;
#endif
    }

    [[nodiscard]] inline bool suspend_requested() noexcept
    {
#ifdef _WIN32
        return false;
#else
        return detail::suspend_flag != 0;
#endif
    }

    [[nodiscard]] inline bool control_requested() noexcept
    {
        return interrupt_requested() || suspend_requested();
    }

    namespace detail
    {
#ifdef _WIN32
        inline std::atomic_bool console_input_wake_enabled = {};
        inline std::atomic_uint console_input_wake_handlers = {};
        inline std::atomic_bool console_input_read_active = {};
        inline std::atomic<HANDLE> console_input_thread = {};

        using CancelSynchronousIoFn = BOOL(WINAPI *)(HANDLE);

        inline bool cancel_console_input_read(HANDLE input_thread, CancelSynchronousIoFn cancel) noexcept
        {
            if (input_thread == nullptr)
            {
                return false;
            }
            while (console_input_read_active.load())
            {
                if (cancel(input_thread) != 0)
                {
                    return true;
                }
                if (GetLastError() != ERROR_NOT_FOUND)
                {
                    return false;
                }
                // The reader may start a console request after checking the flag.
                Sleep(0);
            }
            return true;
        }

        inline bool wake_console_input(HANDLE input, HANDLE input_thread) noexcept
        {
            INPUT_RECORD wake = {};
            wake.EventType = KEY_EVENT;
            wake.Event.KeyEvent.bKeyDown = TRUE;
            wake.Event.KeyEvent.wRepeatCount = 1;
            wake.Event.KeyEvent.wVirtualKeyCode = static_cast<WORD>('X');
            wake.Event.KeyEvent.uChar.UnicodeChar = L'x';
            DWORD written = 0;
            if (WriteConsoleInputW(input, &wake, 1, &written) != 0 && written == 1)
            {
                return true;
            }

            // Cancel _getch when integrity rules prevent queue writes.
            return cancel_console_input_read(input_thread, CancelSynchronousIo);
        }

        inline void wake_console_input() noexcept
        {
            HANDLE input = CreateFileW(
                L"CONIN$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr
            );
            const bool close_input = input != INVALID_HANDLE_VALUE;
            if (!close_input)
            {
                input = GetStdHandle(STD_INPUT_HANDLE);
            }
            wake_console_input(input, console_input_thread.load());

            if (close_input)
            {
                CloseHandle(input);
            }
        }

        inline void arm_console_input_wake() noexcept
        {
            console_input_wake_enabled.store(true);
        }

        inline bool read_console_byte(char &out) noexcept
        {
            // Arm cancellation across the gap between _kbhit and _getch.
            console_input_read_active.store(true);
            if (interrupt_flag.load())
            {
                console_input_read_active.store(false);
                return false;
            }
            const int value = _getch();
            console_input_read_active.store(false);
            if (value == EOF || interrupt_flag.load())
            {
                return false;
            }
            out = static_cast<char>(value);
            return true;
        }

        inline void disarm_console_input_wake() noexcept
        {
            console_input_wake_enabled.store(false);
            // Wait for handlers that observed the armed state before flushing input.
            while (console_input_wake_handlers.load() != 0)
            {
                Sleep(0);
            }
        }

        inline BOOL WINAPI console_interrupt_handler(DWORD event) noexcept
        {
            // Decline close events because Windows may terminate before cleanup finishes.
            if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
            {
                return FALSE;
            }
            console_input_wake_handlers.fetch_add(1);
            // Count this handler before publishing the interrupt.
            interrupt_flag.store(true);
            if (console_input_wake_enabled.load())
            {
                // _kbhit can report input just before _getch blocks.
                wake_console_input();
            }
            console_input_wake_handlers.fetch_sub(1);
            return TRUE;
        }
#else
        struct TerminationSignal
        {
            int number;
            uint32_t bit;
        };

        inline constexpr uint32_t TERMINATION_SIGINT = 1U;
        inline constexpr uint32_t TERMINATION_SIGTERM = 2U;
        inline constexpr uint32_t TERMINATION_SIGQUIT = 4U;
        inline constexpr uint32_t TERMINATION_SIGHUP = 8U;
        inline constexpr uint32_t ALL_TERMINATION_SIGNALS =
            TERMINATION_SIGINT | TERMINATION_SIGTERM | TERMINATION_SIGQUIT | TERMINATION_SIGHUP;
        inline constexpr TerminationSignal TERMINATION_SIGNALS[] = {
            { SIGINT, TERMINATION_SIGINT },
            { SIGTERM, TERMINATION_SIGTERM },
            { SIGQUIT, TERMINATION_SIGQUIT },
            { SIGHUP, TERMINATION_SIGHUP },
        };

        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): set during startup and read at exit
        inline uint32_t termination_handler_mask = 0;

        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): owned by interactive startup and teardown
        inline bool job_control_installed = false;
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): inherited continuation policy
        inline struct sigaction previous_continue_action = {};

        inline bool add_job_control_signals(sigset_t &signals) noexcept
        {
            return sigaddset(&signals, SIGTSTP) == 0 && sigaddset(&signals, SIGCONT) == 0;
        }

        inline void job_control_handler(int signal) noexcept
        {
            suspend_flag = signal == SIGTSTP ? 1 : 0;
        }

        inline bool set_suspend_handler(void (*handler)(int)) noexcept
        {
            struct sigaction action = {};
            action.sa_handler = handler;
            return sigemptyset(&action.sa_mask) == 0 && add_job_control_signals(action.sa_mask) &&
                   sigaction(SIGTSTP, &action, nullptr) == 0;
        }

        using SetSuspendHandlerFn = bool (*)(void (*)(int));
        using RaiseSignalFn = int (*)(int);

        inline bool suspend_handoff(
            SetSuspendHandlerFn set_handler = set_suspend_handler, RaiseSignalFn raise_signal = std::raise
        ) noexcept
        {
            sigset_t signals = {};
            sigset_t previous = {};
            if (sigemptyset(&signals) != 0 || !add_job_control_signals(signals) ||
                pthread_sigmask(SIG_BLOCK, &signals, &previous) != 0)
            {
                return false;
            }
            bool ok = true;
            if (suspend_requested() && !interrupt_requested())
            {
                // Unblock only SIGCONT so the queued stop cannot discard it.
                sigset_t preparing = {};
                ok = sigfillset(&preparing) == 0 && sigdelset(&preparing, SIGCONT) == 0 &&
                     pthread_sigmask(SIG_SETMASK, &preparing, nullptr) == 0;
                // Keep SIGTSTP queued and SIGCONT observable while restoring handlers.
                preparing = previous;
                ok = ok && add_job_control_signals(preparing) && sigdelset(&preparing, SIGCONT) == 0 &&
                     pthread_sigmask(SIG_SETMASK, &preparing, nullptr) == 0;
                if (ok && suspend_requested() && !interrupt_requested())
                {
                    ok = raise_signal(SIGTSTP) == 0;
                    // Block handled signals before deciding whether to stop.
                    sigset_t stopping = {};
                    const bool blocked =
                        sigfillset(&stopping) == 0 && pthread_sigmask(SIG_SETMASK, &stopping, nullptr) == 0;
                    ok = blocked && ok;
                    if (ok && suspend_requested() && !interrupt_requested())
                    {
                        ok = set_handler(SIG_DFL);
                        if (ok)
                        {
                            sigset_t pending_signals = {};
                            ok = sigpending(&pending_signals) == 0 && sigdelset(&stopping, SIGTSTP) == 0;
                            bool terminating = interrupt_requested();
                            for (const TerminationSignal &entry : TERMINATION_SIGNALS)
                            {
                                terminating = terminating || ((termination_handler_mask & entry.bit) != 0 &&
                                                              sigismember(&previous, entry.number) == 0 &&
                                                              sigismember(&pending_signals, entry.number) == 1);
                            }
                            if (ok && terminating)
                            {
                                // Discard the synthetic stop before delivering termination.
                                ok = set_handler(SIG_IGN);
                            }
                            else if (ok)
                            {
                                // Unblocking SIGTSTP delivers it unless SIGCONT cancels it.
                                ok = pthread_sigmask(SIG_SETMASK, &stopping, nullptr) == 0;
                                const bool reblocked = pthread_sigmask(SIG_BLOCK, &signals, nullptr) == 0;
                                ok = reblocked && ok;
                            }
                        }
                    }
                    else if (ok)
                    {
                        // Discard a synthetic stop canceled just before raise().
                        ok = set_handler(SIG_IGN);
                    }
                    suspend_flag = 0;
                    ok = set_handler(job_control_handler) && ok;
                }
            }
            const bool restored = pthread_sigmask(SIG_SETMASK, &previous, nullptr) == 0;
            return ok && restored;
        }

        inline bool install_job_control_handlers() noexcept
        {
            struct sigaction previous = {};
            if (sigaction(SIGTSTP, nullptr, &previous) != 0)
            {
                return false;
            }
            if (previous.sa_handler == SIG_IGN)
            {
                return true;
            }
            if (sigaction(SIGCONT, nullptr, &previous_continue_action) != 0)
            {
                return false;
            }
            struct sigaction action = {};
            action.sa_handler = job_control_handler;
            if (sigemptyset(&action.sa_mask) != 0 || !add_job_control_signals(action.sa_mask))
            {
                return false;
            }
            // Even an ignored SIGCONT must cancel a deferred stop.
            if (sigaction(SIGCONT, &action, nullptr) != 0)
            {
                return false;
            }
            if (!set_suspend_handler(job_control_handler))
            {
                sigaction(SIGCONT, &previous_continue_action, nullptr);
                return false;
            }
            job_control_installed = true;
            return true;
        }

        inline bool add_termination_signals(sigset_t &signals, uint32_t mask) noexcept
        {
            for (const TerminationSignal &entry : TERMINATION_SIGNALS)
            {
                if ((mask & entry.bit) != 0 && sigaddset(&signals, entry.number) != 0)
                {
                    return false;
                }
            }
            return true;
        }

        inline bool termination_signal_set(sigset_t &signals, uint32_t mask = ALL_TERMINATION_SIGNALS) noexcept
        {
            return sigemptyset(&signals) == 0 && add_termination_signals(signals, mask);
        }

        inline void reset_termination_handlers(uint32_t mask) noexcept
        {
            struct sigaction action = {};
            action.sa_handler = SIG_DFL;
            sigemptyset(&action.sa_mask);
            for (const TerminationSignal &entry : TERMINATION_SIGNALS)
            {
                if ((mask & entry.bit) != 0)
                {
                    sigaction(entry.number, &action, nullptr);
                }
            }
        }

        inline int finish_termination_with_signals_blocked(
            int status, const sigset_t &previous_mask, uint32_t handler_mask
        ) noexcept
        {
            const int signal = static_cast<int>(interrupt_flag);
            // Keep late signals pending until their default handlers are restored.
            reset_termination_handlers(handler_mask);
            if (job_control_installed)
            {
                // Keep recording SIGTSTP during clean teardown.
                if (signal != 0)
                {
                    set_suspend_handler(SIG_DFL);
                }
                sigaction(SIGCONT, &previous_continue_action, nullptr);
            }
            if (signal == 0)
            {
                pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
                return status;
            }

            sigset_t delivery_mask = previous_mask;
            const bool mask_ready = add_termination_signals(delivery_mask, handler_mask) &&
                                    (!job_control_installed || add_job_control_signals(delivery_mask)) &&
                                    sigdelset(&delivery_mask, signal) == 0;
            if (mask_ready && pthread_sigmask(SIG_SETMASK, &delivery_mask, nullptr) == 0)
            {
                // Deliver the first termination signal before unblocking later ones.
                std::raise(signal);
            }
            return 128 + signal;
        }

        inline void signal_handler(int signal) noexcept
        {
            if (interrupt_flag == 0)
            {
                interrupt_flag = signal;
            }
        }
#endif
    } // namespace detail

    inline bool install_interrupt_handler() noexcept
    {
#ifdef _WIN32
        // Clear inherited Ctrl+C suppression from CREATE_NEW_PROCESS_GROUP.
        if (SetConsoleCtrlHandler(nullptr, FALSE) == 0)
        {
            return false;
        }
        detail::interrupt_flag.store(false, std::memory_order_relaxed);
        detail::console_input_wake_enabled.store(false);
        detail::console_input_read_active.store(false);
        HANDLE input_thread = nullptr;
        if (DuplicateHandle(
                GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &input_thread, THREAD_TERMINATE, FALSE, 0
            ) == 0)
        {
            return false;
        }
        detail::console_input_thread.store(input_thread);
        if (SetConsoleCtrlHandler(detail::console_interrupt_handler, TRUE) == 0)
        {
            detail::console_input_thread.store(nullptr);
            CloseHandle(input_thread);
            return false;
        }
        return true;
#else
        detail::interrupt_flag = 0;
        detail::termination_handler_mask = 0;
        struct sigaction action = {};
        action.sa_handler = detail::signal_handler;
        if (!detail::termination_signal_set(action.sa_mask))
        {
            return false;
        }
        for (const detail::TerminationSignal &entry : detail::TERMINATION_SIGNALS)
        {
            struct sigaction previous = {};
            if (sigaction(entry.number, nullptr, &previous) != 0)
            {
                return false;
            }
            if (previous.sa_handler == SIG_IGN)
            {
                continue;
            }
            if (sigaction(entry.number, &action, nullptr) != 0)
            {
                return false;
            }
            detail::termination_handler_mask |= entry.bit;
        }
        return true;
#endif
    }

    inline bool install_job_control_handler() noexcept
    {
#ifdef _WIN32
        return true;
#else
        return detail::install_job_control_handlers();
#endif
    }

    // New threads inherit this blocked mask until the guard restores it.
    class WorkerSignalMask
    {
      public:
        WorkerSignalMask() noexcept
        {
#ifndef _WIN32
            sigset_t signals = {};
            m_valid = detail::termination_signal_set(signals, detail::termination_handler_mask) &&
                      (!detail::job_control_installed || detail::add_job_control_signals(signals)) &&
                      pthread_sigmask(SIG_BLOCK, &signals, &m_previous) == 0;
#endif
        }
        ~WorkerSignalMask() noexcept
        {
#ifndef _WIN32
            if (m_valid)
            {
                pthread_sigmask(SIG_SETMASK, &m_previous, nullptr);
            }
#endif
        }
        [[nodiscard]] bool valid() const noexcept { return m_valid; }
        WorkerSignalMask(const WorkerSignalMask &) = delete;
        WorkerSignalMask &operator=(const WorkerSignalMask &) = delete;
        WorkerSignalMask(WorkerSignalMask &&) = delete;
        WorkerSignalMask &operator=(WorkerSignalMask &&) = delete;

      private:
        bool m_valid = true;
#ifndef _WIN32
        sigset_t m_previous = {};
#endif
    };

    // Call after releasing terminal state; resume only in the foreground.
    inline bool suspend_process() noexcept
    {
#ifdef _WIN32
        return true;
#else
        for (;;)
        {
            if (!detail::suspend_handoff())
            {
                return false;
            }
            while (!control_requested())
            {
                const pid_t foreground = tcgetpgrp(STDIN_FILENO);
                if (foreground < 0)
                {
                    return false;
                }
                if (foreground == getpgrp())
                {
                    return true;
                }
                poll(nullptr, 0, 50);
            }
            if (interrupt_requested())
            {
                return true;
            }
        }
#endif
    }

    // Re-raise after terminal guards unwind; return a fallback status on failure.
    [[nodiscard]] inline int finish_termination(int status) noexcept
    {
#ifdef _WIN32
        return status;
#else
        const uint32_t handler_mask = detail::termination_handler_mask;
        if (handler_mask == 0 && !detail::job_control_installed)
        {
            return status;
        }
        sigset_t signals = {};
        sigset_t previous_mask = {};
        const bool blocked = detail::termination_signal_set(signals, handler_mask) &&
                             (!detail::job_control_installed || detail::add_job_control_signals(signals)) &&
                             pthread_sigmask(SIG_BLOCK, &signals, &previous_mask) == 0;
        if (blocked)
        {
            return detail::finish_termination_with_signals_blocked(status, previous_mask, handler_mask);
        }
        const int signal = static_cast<int>(detail::interrupt_flag);
        if (signal == 0)
        {
            return status;
        }
        if (std::signal(signal, SIG_DFL) == SIG_ERR)
        {
            return 128 + signal;
        }
        std::raise(signal);
        return 128 + signal;
#endif
    }

    class InterruptHandlerGuard
    {
      public:
        ~InterruptHandlerGuard() noexcept
        {
#ifdef _WIN32
            SetConsoleCtrlHandler(detail::console_interrupt_handler, FALSE);
            while (detail::console_input_wake_handlers.load() != 0)
            {
                Sleep(0);
            }
            HANDLE input_thread = detail::console_input_thread.exchange(nullptr);
            if (input_thread != nullptr)
            {
                CloseHandle(input_thread);
            }
#endif
        }

        InterruptHandlerGuard() noexcept = default;
        InterruptHandlerGuard(const InterruptHandlerGuard &) = delete;
        InterruptHandlerGuard &operator=(const InterruptHandlerGuard &) = delete;
        InterruptHandlerGuard(InterruptHandlerGuard &&) = delete;
        InterruptHandlerGuard &operator=(InterruptHandlerGuard &&) = delete;
    };

} // namespace platform
