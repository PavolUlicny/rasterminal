#include "tests/test.h"
#include "src/input.h"
#include "src/platform.h"

// <stdlib.h>, not <cstdlib>: the POSIX pty functions (posix_openpt/grantpt/unlockpt/
// ptsname), the env mutators (setenv/unsetenv on POSIX, _putenv_s on the MSVC CRT), and
// getenv are all specified there and not guaranteed to reach the global namespace via the
// C++ header. Unconditional because the env helpers below are cross-platform.
#include <stdlib.h> // NOLINT(modernize-deprecated-headers,hicpp-deprecated-headers)

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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

// color-capability classifier
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

// file size
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

// input parser
// detail::parse_input is pure (a byte span in, a decision out), so the grammar is
// tested directly: no pipes, no dup2, no timing, and it runs on every platform
// rather than POSIX only. The contract it must uphold is that a sequence is
// consumed in full or not at all, because any byte left behind is dispatched as a
// keypress and several of those are destructive ('q' quits, 'r' resets the view).

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
    // Every byte value is legitimate payload: a coordinate past column 223 wraps
    // into the control range, so 0x00 and ESC must not be read as terminators.
    // This makes X10 the one arm that does NOT treat an embedded ESC as a sequence
    // boundary, and the asymmetry is deliberate: a click at column 251 sends ESC as
    // a coordinate, and stopping there would leak the remaining bytes, which are
    // printable and can be the quit key. Adding the boundary rule here was tried;
    // this case is what caught it.
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
    // A long digit run must not overflow the int accumulator (signed overflow is
    // UB). Stopping at the ceiling avoids that, but the clamped value is not a
    // coordinate anyone asked for, so the report is dropped rather than reported:
    // main.cpp would take it as a drag origin.
    expect_dropped_whole("\033[<0;99999999999999999999;6M");
}

TEST(parse_input, sgr_mouse_scans_to_its_final_before_deciding)
{
    // Every CSI arm consumes through the terminator. Abandoning at the first
    // unexpected byte instead would leave the remainder to dispatch: the digits in
    // these would surface as shading-mode keys, and the trailing bytes of the
    // others reach the dispatch outright.
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
    // An ESC is a boundary, not payload. A sequence cut short by the next one
    // starting must stop at that ESC: consuming it strips the next sequence's
    // introducer, and its body then dispatches byte by byte as live bindings.
    // Every scanning arm has to honour this, so each is covered separately.
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
    // The other half of the boundary rule, and the opposite of the case above. A CSI
    // carries parameters and then a final, an alphabet an ESC can never belong to, so
    // an ESC really does end one. A string payload is arbitrary text delivered in
    // pieces, so an ESC inside it is either payload or a sequence the terminal wrote
    // between two of those pieces; the reply continues afterwards either way. Ending
    // it there hands the rest of the reply to the dispatch, which is a leak of exactly
    // the kind this design exists to stop (measured: a 307-byte clipboard reply with a
    // scroll notch inside it surfaced 102 keypresses, two of them quits).
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

    // Motion needs a button actually held and the press form. Button bits 3 is the
    // no-button motion a terminal in mode 1003 sends, and a motion-flagged release
    // is malformed; either reported as a drag would orbit on a bare pointer move.
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
    // The three invariants the buffered design rests on, checked exhaustively rather
    // than on hand-picked cases, over an alphabet holding every byte the grammar
    // reacts to (introducers, finals, terminators, separators, digits, and a few it
    // must treat as ordinary).
    //
    //   1. consumed stays within the input, or poll_event's compaction underflows.
    //   2. Incomplete exactly when nothing was consumed, or its loop stops making
    //      progress and spins on a buffer it can never drain.
    //   3. DECISION STABILITY: once a decision is made, appending a byte may not
    //      change it. This is the one that matters most and the hardest to get from
    //      examples: without it the result depends on how the terminal happened to
    //      split its writes, which is precisely the class of bug that reached the
    //      dispatch in earlier designs. A sequence must decode the same whether it
    //      arrives whole or a byte at a time.
    //
    // Depth 4 keeps this at about a million parses, which is milliseconds. It was
    // also run out-of-band to depth 7 (286 million strings, zero violations); the
    // depth here is what the suite can afford on every run, not the limit of the
    // evidence.
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
// The read path and the pending buffer, exercised over a real fd. POSIX-only:
// the Windows path reads the console via _kbhit/_getch, which a pipe cannot
// stand in for. The grammar itself is covered above, without any of this.

#ifndef _WIN32
namespace
{
    // Feeds bytes to poll_event by swapping a pipe's read end onto stdin. Writes
    // can be staged, so a sequence can be delivered in separate bursts.
    //
    // Precondition (new to this file): fd 0 must be open and dup-able, so that it
    // can be saved and restored. Every runner the suite targets inherits an open
    // stdin; if one ever does not, `ok` is false and the cases fail loudly rather
    // than silently testing nothing.
    struct StdinFeed
    {
        int saved_stdin;
        int write_fd = -1;
        bool ok = false;
        StdinFeed() : saved_stdin(test_dup(STDIN_FILENO))
        {
            // poll_event's buffer persists across calls by design (one continuous
            // input stream); reset it so cases cannot contaminate each other. The
            // whole struct, not just `len`: a case that ends mid-skip leaves the skip
            // flag and its rate-floor window set, which changes what the next case's
            // first poll does with a stalled partial.
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
        // Returns whether the whole string reached the pipe. Nothing reads the fd
        // until the case polls, so a feed larger than the pipe capacity short-writes
        // (or blocks); cases assert this so such a feed fails loudly instead of
        // silently testing a truncated one.
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

    // The discard needs one poll that sees no new bytes. That is the contract, not
    // an artifact: a poll which IS receiving bytes cannot tell a stalled partial
    // from a sequence still arriving, and abandoning one mid-flight would dispatch
    // its tail. Sleeping longer only makes this more certain, so no timing race.
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
}

TEST(poll_event, reassembly_survives_a_frame_slower_than_the_timeout)
{
    // The timestamp records when bytes were last appended, which is observed at the
    // caller's cadence rather than at arrival. If staleness were judged while bytes
    // are arriving, any frame longer than the timeout (a heavy model, or --fps 10)
    // would abandon every split sequence and dispatch its tail as loose keypresses:
    // here the trailing "[A" would orbit the camera instead of reporting Up.
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
    // The last path that could dispatch a reply's payload as keypresses. A single
    // sequence longer than the buffer cannot be held, so the introducer is kept and
    // the middle dropped; the parser then goes on looking for that family's
    // terminator, and nothing dispatches until it arrives.
    //
    // No clock is involved, which is the point. Earlier designs guessed from timing
    // or read size when the sequence had ended, and every such guess was wrong for
    // some real delivery pattern.
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
    // The same payload delivered in pieces, with polls landing between them. Chunk
    // size and spacing are exactly what the previous volume and deadline heuristics
    // were sensitive to: a full buffer, an ordinary 400-byte write, and a trickle
    // smaller than any threshold. The terminator rule is indifferent to all three.
    //
    // Every pattern delivers the same total, comfortably past MAX_PENDING, and the
    // skip is asserted rather than assumed. Fixing the chunk COUNT instead left the
    // trickle case at 96 bytes, which never fills the buffer, so the one pattern the
    // case exists to cover was the one that never entered a skip at all.
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
    // The dimension the other cases miss. They deliver chunks back to back, so the
    // inter-chunk gap is always zero; a reply arriving with real gaps between its
    // pieces is a different path, and it was the one that leaked. The staleness rule
    // would see the skip go quiet between chunks and tear it down, after which the
    // next chunk parses as fresh input and dispatches its payload as keypresses.
    //
    // Simulated by ageing last_growth past the reassembly window before each chunk,
    // which is what a gap does, without spending real time.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'q')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    for (int chunk = 0; chunk < 6; chunk++)
    {
        platform::detail::pending().last_growth =
            std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
        // The rate floor is not what this case is about, so hold its window open too.
        // Left on the real clock it makes the case a timing race: a scheduler stall
        // longer than RATE_WINDOW_MS would expire the window and fail a correct
        // implementation, since the chunks here are fed with no real time between them
        // and so cannot meet the floor.
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
    // A payload can stall before it ever fills the buffer, so no skip has been
    // entered. Abandoning it then would dispatch every byte already held. Anything
    // longer than a keypress is therefore switched to a skip instead, while a short
    // partial (a fragmented arrow key) is still abandoned as before.
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
    // A long reply does not arrive all at once, so the terminal can write an ordinary
    // sequence between two of its chunks: a scroll notch, a drag report, any key that
    // encodes as one. Every one of those starts with ESC, and ending the skip there
    // reads it as "the reply is over" when the reply goes on afterwards, so the rest
    // of it reaches the dispatch as keypresses. Measured before the fix: the drag was
    // reported and then 'q' arrived out of the payload and quit the viewer.
    //
    // The interleaved sequence is swallowed instead. That is the trade this design
    // always takes, and here it costs one scroll notch rather than the session.
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
    // The CSI half of the boundary rule, across a skip. A CSI's alphabet cannot
    // contain an ESC, so one ends the skip and the sequence it introduces is left to
    // parse whole. Scanning past it instead would end the skip at that sequence's own
    // '[' (0x5B is a legal CSI final) and strand the rest of it for the dispatch:
    // measured, an X10 report delivered inside a skipped CSI surfaced Space and Q.
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
    // ST is two bytes, so a reply long enough to overflow can put its ESC at the very
    // last byte of the buffer. That ESC is exactly why the parse returned Incomplete,
    // and collapsing to the bare introducer threw it away: the backslash then read as
    // payload, the reply could never terminate, and the skip ran on to the rate floor
    // swallowing whatever was typed meanwhile.
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
    // A skipped sequence must never resume as something decodable. This once failed
    // because the collapse erased the CSI's positional context, so a payload byte in
    // the A-D range landed at the arrow's index and orbited the camera. Both halves of
    // that are gone now: a skip runs skip_scan, which locates and never decodes, and
    // the resume prefix keeps no payload byte at all. The case stays as the regression
    // pin for the property, which no longer depends on how the prefix is built.
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
    // Sequences with no binding resolve without producing an event, so a call that
    // meets only those returns Type::None and ends the caller's drain. Reading once
    // per call therefore paced ANY such backlog at one bufferful per frame, not just
    // the over-length path round 20 fixed: 64 KB of unbound function keys took 66
    // frames and 1.07 s before the quit key behind them was seen.
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
    // A skip drops the middle of a sequence, so whatever the parser makes of the
    // fragment that resumes it is not what the terminal sent. The mouse arm is the
    // case that proves the rule has to be general: its introducer (ESC [ <) is a byte
    // longer than every other family's, so the resume prefix keeps no payload byte of
    // it and the tail parses as a whole report. Before the rule, this fabricated a
    // MouseMove at coordinates nobody sent, which orbits the camera.
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
    // The meter ticks when the caller polls, not on a timer, so a caller slower than
    // the window owes several windows at once. Charging one per tick turns the floor
    // into bytes-per-FRAME: at a frame interval past about four seconds, autorepeat
    // clears the bar and sustains a skip for as long as the user keeps typing.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'x')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // One frame's worth of typing at autorepeat speed, and nothing else: the credit is
    // set directly, because the burst that entered the skip is legitimately still
    // credited and would otherwise be what carries it. 150 bytes clears ONE window's
    // quota, which is exactly why charging one quota per tick sustained the skip; it
    // cannot clear the five windows that actually elapsed.
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
    // A pass that spends its whole read budget returns above the meter, so the credit
    // can be added to across many passes without once being spent. Unbounded that is
    // signed overflow, and a wrapped negative credit reads as below the floor and tears
    // down an active skip. The ceiling has to leave room for the largest quota a tick
    // can charge, or saturating would itself cost a fast stream its verdict.
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
    // The skip here is reached without the buffer ever filling, so there is no read
    // "just before" it to seed a rate from: the read that precedes it returned nothing,
    // which is the very condition that promoted the partial. Seeded from that, the skip
    // began below the floor and was torn down at its first window, dispatching the rest
    // of the reply as keypresses. The arrival meter runs regardless, so the bytes that
    // did arrive are already counted.
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
    // A reply delivered in bursts can leave a whole window empty while averaging far
    // above the floor. Resetting the count each window measured presence rather than
    // rate and tore such a skip down, dispatching the next burst as keypresses. The
    // surplus is carried instead, so one burst covers the windows after it.
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
    // The rate floor, which is the only thing that ends a skip whose sequence never
    // terminates and deliberately the only thing. A cap on how much may be skipped
    // was the alternative, and it is the mistake the whole design exists to avoid:
    // whatever the cap, the first reply to exceed it has its tail dispatched.
    //
    // Below the floor the bytes are not a sequence arriving, they are the user's, so
    // the skip is abandoned and typing works again.
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
    // A skip that advanced one bufferful per call would advance at the frame rate,
    // since main.cpp stops draining on Type::None: a megabyte reply measured 16.6 s
    // of total input lockout at 60 fps, and worse at a lower --fps. Asserted on the
    // state rather than on wall-clock, which would be a flaky proxy for it.
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
    // The frame-rate contract: an ESC with nothing behind it must return at once
    // rather than waiting for the rest. Holding Escape used to pay an inter-byte
    // timeout every frame, which cost a third of the frame rate.
    //
    // Measured over many calls rather than one, deliberately: a single-call
    // wall-clock assertion is the flaky kind this suite avoids. Blocking would cost
    // PARTIAL_TIMEOUT_MS per call, so the budget below is an order of magnitude
    // under a regression while leaving ample room for scheduler jitter.
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
