#include "tests/test.h"
#include "src/platform.h"

#ifndef _WIN32
#include <fcntl.h>
// <stdlib.h>, not <cstdlib>: the POSIX pty functions (posix_openpt/grantpt/
// unlockpt/ptsname) are specified in <stdlib.h> and are not guaranteed to reach
// the global namespace through the C++ header.
#include <stdlib.h> // NOLINT(modernize-deprecated-headers,hicpp-deprecated-headers)
#include <unistd.h>
#endif

namespace
{
    // Closes a fd on scope exit (via the portable test_close) so a thrown ASSERT
    // mid-test can't leak it (the framework catches AssertionError and continues
    // in-process).
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
} // namespace

// The null device is a character device but not a terminal, so is_tty must
// return false for it. A more interesting negative case than a plain file: on
// Windows NUL is exactly what _isatty answers true for, so it pins the
// GetConsoleMode-based probe (not _isatty) that platform.h documents.
TEST(platform, is_tty_false_for_null_device)
{
    ScopedFd dev(test_devnull());
    ASSERT_TRUE(dev.fd >= 0);
    ASSERT_FALSE(platform::is_tty(dev.fd));
}

#ifndef _WIN32
// Positive case via a freshly allocated pty. is_tty is probed on the SLAVE side,
// which POSIX defines as a terminal on every platform; the master side is a
// terminal on Linux but not guaranteed elsewhere (e.g. FreeBSD returns false).
// POSIX-only: Windows has no portable pty fixture (the console API needs a real
// console session).
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
