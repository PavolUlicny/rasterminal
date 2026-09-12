#pragma once

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <termios.h>
#include <unistd.h>

namespace platform_test
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

    // Each test must reset this shared state.
    inline TermiosCalls &termios_calls()
    {
        static TermiosCalls calls;
        return calls;
    }

    inline void reset_termios_calls()
    {
        termios_calls() = {};
        termios_calls().captured.c_lflag = static_cast<tcflag_t>(ECHO | ICANON | ISIG);
        termios_calls().captured.c_cc[VMIN] = 1;
        termios_calls().captured.c_cc[VTIME] = 7;
        termios_calls().current = termios_calls().captured;
    }

    inline int scripted_tcgetattr(int fd, termios *value)
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

    inline int scripted_tcsetattr(int fd, int action, const termios *value)
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

    inline int scripted_tcflush(int fd, int queue)
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

    inline bool alter_termios_field(termios &mode, PartialTermiosField field)
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

    inline bool termios_equal(const termios &a, const termios &b)
    {
        return a.c_iflag == b.c_iflag && a.c_oflag == b.c_oflag && a.c_cflag == b.c_cflag && a.c_lflag == b.c_lflag &&
               std::memcmp(a.c_cc, b.c_cc, sizeof a.c_cc) == 0 && cfgetispeed(&a) == cfgetispeed(&b) &&
               cfgetospeed(&a) == cfgetospeed(&b);
    }
} // namespace platform_test
#endif
