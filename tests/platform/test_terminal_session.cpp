#include "tests/test.h"
#include "tests/platform/test_util.h"
#include "tests/platform/test_termios_util.h"
#include "src/platform/console.h"
#include "src/platform/control.h"
#include "src/platform/input_reader.h"
#include "src/platform/terminal_io.h"
#include "src/platform/terminal_query.h"
#include "src/platform/timing.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

using platform_test::ScopedFd;
using platform_test::ScopedStdoutCapture;
using platform_test::ScopedTmpFile;
#ifndef _WIN32
using platform_test::reset_termios_calls;
using platform_test::scripted_tcgetattr;
using platform_test::scripted_tcsetattr;
using platform_test::termios_calls;
using platform_test::termios_equal;
#endif

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
                // Hang up before reaping children blocked on undrained output.
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
        // Some PTY drivers let owners bypass exclusivity, so also remove permissions.
        ASSERT_TRUE(fchmod(fd, 0) == 0);
        ScopedFd denied(open(path, O_WRONLY | O_NOCTTY | O_NONBLOCK));
        ASSERT_TRUE(denied.fd < 0);
        ASSERT_EQ(errno, EACCES);
    }

    int terminal_status_flags(int fd)
    {
        const int flags = fcntl(fd, F_GETFL, 0);
        ASSERT_TRUE(flags >= 0);
        // Ignore Darwin's internal file-status flags such as FWASWRITTEN.
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
        // TSan flushes streams even in _exit.
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
            // Retry failed cleanup after the shell regains the foreground.
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
            // Keep the foreground group non-orphaned so SIGTSTP stops it.
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
                    // Wait for the parent to drain output.
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
            // Fill the PTY queue to expose short writes.
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
                // Keep output stopped until waitpid confirms suspension.
                // NOLINTNEXTLINE(concurrency-mt-unsafe): parent owns PTY flow control
                ASSERT_TRUE(tcflow(slave.fd, TCOOFF) == 0);
                if (terminate_stopped)
                {
                    // Exhaust space that macOS still queues while paused.
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
                // Verify cleanup did not restart flow control.
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
                // Exhaust the paused queue after suspension succeeds.
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
        // Release the supervisor's drain before ending its paused session.
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
                // No worker threads are live across this fork.
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
        // Ghostty 1.3.1 prints a bare CAN on the restored shell screen.
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
        // Exhaust bytes that macOS queues while paused.
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
        // Mouse resets must follow the terminated partial escape.
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

TEST(platform, terminal_cleanup_survives_xoff_without_losing_bytes)
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
    // Output must remain hidden until cleanup releases XOFF.
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
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    drain_pty_output(master.fd, output);
    if (output.size() < expected.size())
    {
        // macOS does not restart queued output when IXON changes.
        const char start = static_cast<char>(configured.c_cc[VSTART]);
        ASSERT_EQ(write(master.fd, &start, 1), 1);
    }
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
    // Include shell-screen recovery before alternate-screen entry.
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
                // Cancel the stop after the query aborts during the drain.
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
            // Avoid Readline termios changes until the snapshot completes.
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
                // Keep the group non-orphaned so a stray SIGTSTP stops it.
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
                // Terminate instead of masking a lost continuation.
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
                // Keep the group non-orphaned so SIGTSTP produces a real stop.
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
