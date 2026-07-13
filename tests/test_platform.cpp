#include "tests/test.h"
#include "src/platform.h"

// <stdlib.h>, not <cstdlib>: the POSIX pty functions (posix_openpt/grantpt/unlockpt/
// ptsname), the env mutators (setenv/unsetenv on POSIX, _putenv_s on the MSVC CRT), and
// getenv are all specified there and not guaranteed to reach the global namespace via the
// C++ header. Unconditional because the env helpers below are cross-platform.
#include <stdlib.h> // NOLINT(modernize-deprecated-headers,hicpp-deprecated-headers)

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{
    // ASSERT_EQ stringifies via std::to_string, which can't take an enum class, so
    // the classifier tests compare through ints for readable failure output.
    constexpr int DUMB = static_cast<int>(platform::TermColor::Dumb);
    constexpr int P256 = static_cast<int>(platform::TermColor::Palette256);
    constexpr int TC = static_cast<int>(platform::TermColor::TrueColor);

    constexpr int classify(const char *colorterm, const char *term, platform::TermColor unset_default)
    {
        return static_cast<int>(platform::classify_term_color(colorterm, term, unset_default));
    }

    // The TEST cases below all evaluate classify_term_color at runtime, so none would catch a
    // future edit that silently demotes it (or a helper it calls) from constexpr. These
    // static_asserts pin that at compile time. The -direct case is the one that covers both
    // string helpers on its own (it runs ieq for the dumb check, then icontains for the hint);
    // the COLORTERM case is kept because it is the only one that constant-evaluates the
    // COLORTERM branch, which the -direct case short-circuits past.
    static_assert(
        platform::classify_term_color(nullptr, "xterm-direct", platform::TermColor::Palette256) ==
            platform::TermColor::TrueColor,
        "classify_term_color / ieq / icontains must remain constexpr-evaluable"
    );
    static_assert(
        platform::classify_term_color("truecolor", "xterm-256color", platform::TermColor::Palette256) ==
            platform::TermColor::TrueColor,
        "classify_term_color COLORTERM branch must remain constexpr-evaluable"
    );

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

// ─── color-capability classifier ─────────────────────────────────────────────
// classify_term_color is pure (env values passed in), so every case incl. the
// platform-divergent unset default is testable identically on all platforms.

TEST(platform, classify_unset_env_uses_default)
{
    ASSERT_EQ(classify(nullptr, nullptr, platform::TermColor::Palette256), P256);
    ASSERT_EQ(classify(nullptr, nullptr, platform::TermColor::TrueColor), TC);
    ASSERT_EQ(classify("", "", platform::TermColor::Palette256), P256);
    ASSERT_EQ(classify("", "", platform::TermColor::TrueColor), TC);
}

TEST(platform, classify_dumb_always_fatal)
{
    ASSERT_EQ(classify(nullptr, "dumb", platform::TermColor::Palette256), DUMB);
    // Dumb beats a contradictory COLORTERM: a dumb terminal can't render escapes
    // regardless of what claims color support.
    ASSERT_EQ(classify("truecolor", "dumb", platform::TermColor::Palette256), DUMB);
    ASSERT_EQ(classify("24bit", "DUMB", platform::TermColor::TrueColor), DUMB);
}

TEST(platform, classify_dumb_is_exact_match_not_substring)
{
    // The dumb match is an exact ieq(), unlike the truecolor TERM hints which are substring
    // icontains(). Pin that asymmetry: a TERM merely containing "dumb" must NOT be fatally
    // rejected. Without this, a refactor unifying the four TERM checks onto icontains would
    // fatally reject real terminals (e.g. a "dumb"-embedding terminfo alias) with the suite
    // still green.
    ASSERT_EQ(classify(nullptr, "dumbo", platform::TermColor::Palette256), P256);
    ASSERT_EQ(classify(nullptr, "xterm-dumbnot", platform::TermColor::TrueColor), P256);
}

TEST(platform, classify_colorterm_truecolor)
{
    // COLORTERM beats a 256-only TERM.
    ASSERT_EQ(classify("truecolor", "xterm-256color", platform::TermColor::Palette256), TC);
    ASSERT_EQ(classify("24bit", "xterm", platform::TermColor::Palette256), TC);
    ASSERT_EQ(classify("TRUECOLOR", nullptr, platform::TermColor::Palette256), TC);
    ASSERT_EQ(classify("Truecolor", "screen", platform::TermColor::Palette256), TC);
}

TEST(platform, classify_colorterm_unrecognized_falls_through)
{
    // The historical COLORTERM=1/yes (rxvt-era "has color at all") is not a
    // truecolor signal; classification falls through to the TERM rules.
    ASSERT_EQ(classify("yes", "xterm-256color", platform::TermColor::TrueColor), P256);
    ASSERT_EQ(classify("1", "xterm", platform::TermColor::TrueColor), P256);
    ASSERT_EQ(classify("yes", nullptr, platform::TermColor::TrueColor), TC);
    ASSERT_EQ(classify("yes", nullptr, platform::TermColor::Palette256), P256);
}

TEST(platform, classify_term_direct_hints)
{
    ASSERT_EQ(classify(nullptr, "xterm-direct", platform::TermColor::Palette256), TC);
    ASSERT_EQ(classify(nullptr, "tmux-direct", platform::TermColor::Palette256), TC);
    ASSERT_EQ(classify(nullptr, "xterm-direct256", platform::TermColor::Palette256), TC);
    ASSERT_EQ(classify(nullptr, "xterm-truecolor", platform::TermColor::Palette256), TC);
    ASSERT_EQ(classify(nullptr, "XTERM-DIRECT", platform::TermColor::Palette256), TC);
    // The third hint: without this, deleting icontains(term, "24bit") from the classifier
    // leaves the whole suite green.
    ASSERT_EQ(classify(nullptr, "xterm-24bit", platform::TermColor::Palette256), TC);
    // A set-but-unrecognized COLORTERM must fall through (step 2 -> step 3) to the TERM
    // hint, not short-circuit to the floor. Without this every -direct case above passes
    // colorterm=nullptr, so a regression that returned Palette256 on a non-truecolor
    // COLORTERM would pass them all while breaking e.g. COLORTERM=gnome-terminal here.
    ASSERT_EQ(classify("gnome-terminal", "xterm-direct", platform::TermColor::Palette256), TC);
}

TEST(platform, classify_plain_terms_are_256)
{
    const char *terms[] = { "xterm", "xterm-256color", "screen-256color", "tmux-256color",
                            "linux", "vt100",          "st-256color" };
    for (const char *t : terms)
    {
        // Never Dumb, never TrueColor: sub-256-color entries still get the
        // 256-color floor rather than a fatal error (16-color is unsupported).
        ASSERT_EQ(classify(nullptr, t, platform::TermColor::TrueColor), P256);
    }
}

#ifndef _WIN32
// POSIX init_console_output is a no-op that must report VT support unconditionally
// (escape handling is the terminal emulator's job). The Windows side is deliberately
// untested at unit level: under the test runner stdout is redirected, so the console
// handle probe is environment-dependent (same reason is_tty has no Windows-positive
// test).
TEST(platform, init_console_output_ok_on_posix)
{
    ASSERT_TRUE(platform::init_console_output());
}
#endif

namespace
{
    // Portable env mutation for the tests. std::getenv (which detect_term_color reads)
    // sees the CRT environment, which setenv/unsetenv (POSIX) and _putenv_s (the MSVC CRT)
    // both update; the Win32 Set/GetEnvironmentVariable pair is a separate environment that
    // getenv does NOT see. _putenv_s(name, "") removes the variable, and even if a CRT left
    // it empty instead, the classifier treats an empty TERM/COLORTERM the same as unset, so
    // the assertions below hold either way. Single-threaded test binary, so mutation is safe.
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

// End-to-end over the getenv wrapper (the classifier itself is covered above). Cross-platform
// so the one platform-divergent constant in detection, the unset-env default, is exercised on
// the platform CI actually runs on rather than only asserted for POSIX.
TEST(platform, detect_term_color_reads_env)
{
    ScopedEnv colorterm_guard("COLORTERM");
    ScopedEnv term_guard("TERM");

    set_env("COLORTERM", "truecolor");
    set_env("TERM", "xterm");
    ASSERT_EQ(static_cast<int>(platform::detect_term_color()), TC);

    unset_env("COLORTERM");
    set_env("TERM", "dumb");
    ASSERT_EQ(static_cast<int>(platform::detect_term_color()), DUMB);

    unset_env("TERM");
    // Both unset: the platform default. POSIX floors to 256; native Windows defaults to
    // truecolor (VT-on conhost / Windows Terminal render 24-bit).
#ifdef _WIN32
    ASSERT_EQ(static_cast<int>(platform::detect_term_color()), TC);
#else
    ASSERT_EQ(static_cast<int>(platform::detect_term_color()), P256);
#endif
}
