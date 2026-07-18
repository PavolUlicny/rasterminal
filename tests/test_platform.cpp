#include "tests/test.h"
#include "src/platform.h"

// <stdlib.h>, not <cstdlib>: the POSIX pty functions (posix_openpt/grantpt/unlockpt/
// ptsname), the env mutators (setenv/unsetenv on POSIX, _putenv_s on the MSVC CRT), and
// getenv are all specified there and not guaranteed to reach the global namespace via the
// C++ header. Unconditional because the env helpers below are cross-platform.
#include <stdlib.h> // NOLINT(modernize-deprecated-headers,hicpp-deprecated-headers)

#include <cstdio>
#include <filesystem>
#include <string>

#ifndef _WIN32
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

    // The TEST cases below all evaluate classify_term_color at runtime, so none would catch a
    // future edit that silently demotes it (or a helper it calls) from constexpr. These
    // static_asserts pin that at compile time. The -direct case is the one that covers both
    // string helpers on its own (it runs ieq for the dumb check, then icontains for the hint);
    // the COLORTERM case is kept because it is the only one that constant-evaluates the
    // COLORTERM branch, which the -direct case short-circuits past; the allowlist case is the
    // only one that constant-evaluates the TRUECOLOR_TERMS loop, which the -direct case never
    // reaches (xterm-kitty carries no -direct/truecolor/24bit hint, so the loop is its only path).
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
    static_assert(
        platform::classify_term_color(nullptr, "xterm-kitty", platform::TermColor::Palette256) ==
            platform::TermColor::TrueColor,
        "classify_term_color TRUECOLOR_TERMS loop must remain constexpr-evaluable"
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
    ASSERT_EQ(platform::classify_term_color(nullptr, nullptr, P256), P256);
    ASSERT_EQ(platform::classify_term_color(nullptr, nullptr, TC), TC);
    ASSERT_EQ(platform::classify_term_color("", "", P256), P256);
    ASSERT_EQ(platform::classify_term_color("", "", TC), TC);
}

TEST(platform, classify_dumb_always_fatal)
{
    ASSERT_EQ(platform::classify_term_color(nullptr, "dumb", P256), DUMB);
    // Dumb beats a contradictory COLORTERM: a dumb terminal can't render escapes
    // regardless of what claims color support.
    ASSERT_EQ(platform::classify_term_color("truecolor", "dumb", P256), DUMB);
    ASSERT_EQ(platform::classify_term_color("24bit", "DUMB", TC), DUMB);
}

TEST(platform, classify_dumb_is_exact_match_not_substring)
{
    // The dumb match is an exact ieq(), unlike the truecolor TERM hints which are substring
    // icontains(). Pin that asymmetry: a TERM merely containing "dumb" must NOT be fatally
    // rejected. Without this, a refactor unifying the four TERM checks onto icontains would
    // fatally reject real terminals (e.g. a "dumb"-embedding terminfo alias) with the suite
    // still green.
    ASSERT_EQ(platform::classify_term_color(nullptr, "dumbo", P256), P256);
    ASSERT_EQ(platform::classify_term_color(nullptr, "xterm-dumbnot", TC), P256);
}

TEST(platform, classify_colorterm_truecolor)
{
    // COLORTERM beats a 256-only TERM.
    ASSERT_EQ(platform::classify_term_color("truecolor", "xterm-256color", P256), TC);
    ASSERT_EQ(platform::classify_term_color("24bit", "xterm", P256), TC);
    ASSERT_EQ(platform::classify_term_color("TRUECOLOR", nullptr, P256), TC);
    ASSERT_EQ(platform::classify_term_color("Truecolor", "screen", P256), TC);
}

TEST(platform, classify_colorterm_unrecognized_falls_through)
{
    // The historical COLORTERM=1/yes (rxvt-era "has color at all") is not a
    // truecolor signal; classification falls through to the TERM rules.
    ASSERT_EQ(platform::classify_term_color("yes", "xterm-256color", TC), P256);
    ASSERT_EQ(platform::classify_term_color("1", "xterm", TC), P256);
    ASSERT_EQ(platform::classify_term_color("yes", nullptr, TC), TC);
    ASSERT_EQ(platform::classify_term_color("yes", nullptr, P256), P256);
}

TEST(platform, classify_term_direct_hints)
{
    ASSERT_EQ(platform::classify_term_color(nullptr, "xterm-direct", P256), TC);
    ASSERT_EQ(platform::classify_term_color(nullptr, "tmux-direct", P256), TC);
    ASSERT_EQ(platform::classify_term_color(nullptr, "xterm-direct256", P256), TC);
    ASSERT_EQ(platform::classify_term_color(nullptr, "xterm-truecolor", P256), TC);
    ASSERT_EQ(platform::classify_term_color(nullptr, "XTERM-DIRECT", P256), TC);
    // The third hint: without this, deleting icontains(term, "24bit") from the classifier
    // leaves the whole suite green.
    ASSERT_EQ(platform::classify_term_color(nullptr, "xterm-24bit", P256), TC);
    // A set-but-unrecognized COLORTERM must fall through (step 2 -> step 3) to the TERM
    // hint, not short-circuit to the floor. Without this every -direct case above passes
    // colorterm=nullptr, so a regression that returned Palette256 on a non-truecolor
    // COLORTERM would pass them all while breaking e.g. COLORTERM=gnome-terminal here.
    ASSERT_EQ(platform::classify_term_color("gnome-terminal", "xterm-direct", P256), TC);
}

TEST(platform, classify_known_truecolor_terms)
{
    // The TRUECOLOR_TERMS allowlist: TERM names set exclusively by truecolor
    // terminals, covering ssh sessions where COLORTERM is not forwarded. Matched
    // as substrings (xterm-kitty, xterm-ghostty carry the bare name as a suffix) and
    // case-insensitively.
    const char *terms[] = { "xterm-kitty", "wezterm", "alacritty", "xterm-ghostty", "foot", "contour" };
    for (const char *t : terms)
    {
        ASSERT_EQ(platform::classify_term_color(nullptr, t, P256), TC);
    }
    ASSERT_EQ(platform::classify_term_color(nullptr, "XTERM-KITTY", P256), TC);
}

TEST(platform, classify_plain_terms_are_256)
{
    const char *terms[] = { "xterm", "xterm-256color", "screen-256color", "tmux-256color",
                            "linux", "vt100",          "st-256color" };
    for (const char *t : terms)
    {
        // Never Dumb, never TrueColor: sub-256-color entries still get the
        // 256-color floor rather than a fatal error (16-color is unsupported).
        ASSERT_EQ(platform::classify_term_color(nullptr, t, TC), P256);
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
    ASSERT_EQ(platform::detect_term_color(), TC);

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

// ─── file size ───────────────────────────────────────────────────────────────
// platform::file_size sizes streams via the platform's 64-bit seek/tell so >= 2 GB
// model files work on Windows/ILP32. The large case itself is not creatable in CI
// (multi-GB files on every runner); these pin the exact byte count, the empty-file
// zero, and the documented leaves-position-at-EOF contract on the shared code path.

namespace
{
    // Minimal scoped temp file. tests/loader_util.h's TmpFile is the same idiom but
    // pulls in src/mesh.h, overkill for this standalone platform test.
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
    // 259 bytes, deliberately not a round number so an off-by-one or block-granular
    // size can't pass. Content is arbitrary binary (file_size only seeks and tells,
    // never reads, so content can't affect the result).
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
