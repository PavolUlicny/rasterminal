#pragma once

#include "input.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define NOMINMAX
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace platform
{

    // ─── file size ───────────────────────────────────────────────────────────────

    // 64-bit size of an open binary stream, or -1 on failure. std::ftell returns long,
    // which is 32-bit on Windows (LLP64) and ILP32, so files >= 2 GB need the platform's
    // 64-bit seek/tell. On success the stream position is left at end-of-file; callers
    // that go on to read seek back themselves.
    inline int64_t file_size(std::FILE *f)
    {
#ifdef _WIN32
        if (_fseeki64(f, 0, SEEK_END) != 0)
        {
            return -1;
        }
        return _ftelli64(f);
#else
        // ftello returns off_t, 64-bit on every POSIX target via -D_FILE_OFFSET_BITS=64
        // (both build systems define it); returned without a cast because on LP64 the
        // types coincide and static_cast<int64_t>(off_t) would trip -Wuseless-cast.
        // The assert makes that build-system contract self-enforcing: a flag-set edit
        // that drops the define would otherwise silently regress 32-bit builds to the
        // 2 GB EOVERFLOW behavior, and no CI test can catch it (multi-GB fixtures are
        // uncreatable on runners); this turns it into a cross32-job compile error.
        static_assert(sizeof(off_t) == 8, "64-bit off_t required; build with -D_FILE_OFFSET_BITS=64");
        if (fseeko(f, 0, SEEK_END) != 0)
        {
            return -1;
        }
        return ftello(f);
#endif
    }

    // ─── terminal size ────────────────────────────────────────────────────────────

    inline void get_terminal_size(int &cols, int &rows)
    {
#ifdef _WIN32
        // Zero-initialized and the call checked, so a failure lands on the same
        // fallback as POSIX rather than on whatever the stack held. Callers treat
        // these as positive (the framebuffer is sized from them, and main.cpp measures
        // a drag delta against them), and a redirected or closed stdout fails the call.
        CONSOLE_SCREEN_BUFFER_INFO csbi = {};
        cols = 0; // never read the caller's incoming value on the failure path
        rows = 0;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi) != 0)
        {
            cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        }
        // A console can also report an empty window rectangle; treat that as failure
        // too, exactly as the POSIX path treats ws_col == 0.
        if (cols <= 0 || rows <= 0)
        {
            cols = 80;
            rows = 24;
        }
#else
        struct winsize ws = {};
        // The primary ioctl can fail or report ws_col==0 (some terminals and
        // multiplexers) even on a real tty; fall back across the other fds, then
        // to a sane default below.
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0)
        {
            ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
        }
        if (ws.ws_col == 0)
        {
            ioctl(STDERR_FILENO, TIOCGWINSZ, &ws);
        }
        cols = ws.ws_col > 0 ? ws.ws_col : 80; // sane fallback
        rows = ws.ws_row > 0 ? ws.ws_row : 24;
#endif
    }

    // ─── tty detection ───────────────────────────────────────────────────────────

    // True when the CRT/POSIX fd (0 = stdin, 1 = stdout) refers to a terminal.
    // Windows probes the console API rather than _isatty: _isatty is true for ANY
    // character device (NUL, COM ports), while GetConsoleMode succeeds only on a
    // real console handle. A mintty/MSYS pty is a named pipe, not a console, so it
    // is rejected too; that is intentional: interactive input (_kbhit/_getch)
    // cannot work on such a pty either, so rejecting it up front is fail-loud
    // rather than silently broken. Windows Terminal / conhost / winpty all pass.
    // _get_osfhandle (not GetStdHandle) so is_tty works on any CRT fd, not only a
    // std stream. Callers pass std fds (0/1) or freshly opened valid fds: for a
    // std fd on a no-console launch _get_osfhandle yields the -2 sentinel and
    // GetConsoleMode fails cleanly to false. (A closed/never-opened fd would trip
    // the CRT invalid-parameter handler instead, but no caller passes one.)
    // Accepted limitation: GetConsoleMode needs GENERIC_READ on the handle, so a
    // console std handle a launcher opened write-only would misreport as false.
    // Every normal shell (cmd/PowerShell/Windows Terminal/conhost) hands us
    // read+write console handles, so this does not arise in practice.
    inline bool is_tty(int fd)
    {
#ifdef _WIN32
        DWORD mode = 0;
        return GetConsoleMode(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), &mode) != 0;
#else
        return isatty(fd) != 0;
#endif
    }

    // ─── color capability ────────────────────────────────────────────────────────

    // Terminal color capability, classified from the environment at startup.
    enum class TermColor : std::uint8_t
    {
        Dumb,       // TERM=dumb: cannot render escape sequences at all (caller fails loud)
        Palette256, // xterm-256 palette output
        TrueColor,  // 24-bit SGR output
    };

    namespace detail
    {
        // ASCII-only case folding: env values are plain ASCII, and std::tolower has
        // locale cost plus UB on negative char.
        constexpr char ascii_lower(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        constexpr bool ieq(const char *a, const char *b) noexcept
        {
            for (; *a != '\0' && *b != '\0'; ++a, ++b)
            {
                if (ascii_lower(*a) != ascii_lower(*b))
                {
                    return false;
                }
            }
            return *a == '\0' && *b == '\0';
        }

        // Case-insensitive substring scan. Naive O(n*m) is fine: inputs are short
        // env values checked once at startup.
        constexpr bool icontains(const char *hay, const char *needle) noexcept
        {
            for (;; ++hay)
            {
                const char *h = hay;
                const char *n = needle;
                while (*n != '\0' && *h != '\0' && ascii_lower(*h) == ascii_lower(*n))
                {
                    ++h;
                    ++n;
                }
                if (*n == '\0')
                {
                    return true;
                }
                if (*hay == '\0')
                {
                    return false;
                }
            }
        }

        // The most common terminals whose TERM name is set exclusively by a truecolor
        // terminal, matched as substrings so variants hit too (xterm-kitty, xterm-ghostty).
        // Exists for the ssh case: COLORTERM is not forwarded but TERM is, so without this
        // these terminals would be downgraded to 256 colors. Deliberately a curated common
        // set, not exhaustive (a less common truecolor terminal such as rio is intentionally
        // omitted): an unlisted terminal falls to the safe 256 floor, and its user forces
        // 24-bit with --color truecolor. Keeping the list short keeps a false positive from a
        // short substring unlikely.
        inline constexpr const char *TRUECOLOR_TERMS[] = {
            "kitty", "wezterm", "alacritty", "ghostty", "foot", "contour",
        };
    } // namespace detail

    // Pure classifier over the COLORTERM/TERM env values (either may be null). All
    // comparisons are ASCII case-insensitive. Decision order:
    //   1. TERM=dumb wins over everything, even a contradictory COLORTERM: a dumb
    //      terminal cannot render escape sequences regardless of what claims color.
    //      Only the literal "dumb" is fatal, by choice: terminfo's wider non-addressable
    //      family (unknown = use=dumb+gn, etc.) is left on the 256 floor rather than
    //      fatal, since in practice such a TERM is usually a misconfig inside a real
    //      terminal that renders fine, and the policy is "never fatal except dumb".
    //   2. COLORTERM in {truecolor, 24bit} is the canonical truecolor signal.
    //   3. TERM unset/empty: platform default (unset_default), parameterized so
    //      both platform branches are unit-testable everywhere.
    //   4. TERM hints (the -direct terminfo family, truecolor, 24bit) and the
    //      TRUECOLOR_TERMS known-terminal names cover terminals whose sessions carry
    //      no COLORTERM (not exported, or stripped by ssh).
    //   5. Everything else gets the conservative 256-color floor; never fatal even
    //      for sub-256-color terminfo entries (16-color output is not supported).
    constexpr TermColor classify_term_color(const char *colorterm, const char *term, TermColor unset_default) noexcept
    {
        const bool has_term = term != nullptr && *term != '\0';
        if (has_term && detail::ieq(term, "dumb"))
        {
            return TermColor::Dumb;
        }
        if (colorterm != nullptr && (detail::ieq(colorterm, "truecolor") || detail::ieq(colorterm, "24bit")))
        {
            return TermColor::TrueColor;
        }
        if (!has_term)
        {
            return unset_default;
        }
        // term is non-null and non-empty from here: the remaining checks all read it.
        if (detail::icontains(term, "-direct") || detail::icontains(term, "truecolor") ||
            detail::icontains(term, "24bit"))
        {
            return TermColor::TrueColor;
        }
        // Raw loop, not std::any_of: any_of is not constexpr until C++20.
        for (const char *name : detail::TRUECOLOR_TERMS)
        {
            if (detail::icontains(term, name))
            {
                return TermColor::TrueColor;
            }
        }
        return TermColor::Palette256;
    }

    // Env-reading wrapper around classify_term_color. The platform default applies when
    // TERM is unset/empty and COLORTERM carries no truecolor signal (see the classifier's
    // step order). Windows defaults it to truecolor (the native Windows Terminal / conhost
    // norm; both render 24-bit once VT processing is on). A Windows ssh session runs over a
    // real ConPTY console, so is_tty accepts it and it sets TERM, reaching the classifier
    // proper; a mintty/MSYS pty is a named pipe that is_tty rejects before detection runs, so
    // it never gets here. POSIX defaults to the conservative 256-color floor.
    inline TermColor detect_term_color() noexcept
    {
        // Single-threaded startup; nothing in the program calls setenv.
        const char *colorterm = std::getenv("COLORTERM"); // NOLINT(concurrency-mt-unsafe)
        const char *term = std::getenv("TERM");           // NOLINT(concurrency-mt-unsafe)
#ifdef _WIN32
        constexpr TermColor unset_default = TermColor::TrueColor;
#else
        constexpr TermColor unset_default = TermColor::Palette256;
#endif
        return classify_term_color(colorterm, term, unset_default);
    }

    // ─── raw mode ────────────────────────────────────────────────────────────────

#ifndef _WIN32
    namespace detail
    {
        inline termios &saved_termios()
        {
            static termios t;
            return t;
        }
    } // namespace detail
#endif

    // Output-side console setup (UTF-8 + ANSI). Idempotent. Split out of enable_raw_mode
    // so it can run before any output — incl. the UTF-8 in --version — which on Windows
    // happens before the main loop and thus before raw mode would otherwise set the CP.
    // Returns whether the console accepts VT escape sequences: a legacy Windows console
    // can lack ENABLE_VIRTUAL_TERMINAL_PROCESSING, making ANSI output impossible, and
    // the render path must fail loud rather than print escape garbage. POSIX always
    // returns true (escape handling is the terminal emulator's job, not the kernel's).
    // Deliberately not [[nodiscard]]: the enable_raw_mode and --version call sites
    // legitimately ignore it; only the render path needs the verdict.
    inline bool init_console_output()
    {
#ifdef _WIN32
        // Probes GetStdHandle(STD_OUTPUT_HANDLE), like get_terminal_size/enable_mouse, whereas
        // is_tty(1) probes _get_osfhandle(1). Both reference the same console object under any
        // normal launch (divergence needs a deliberate SetStdHandle), and VT is a property of
        // that shared object, so enabling it here reaches the output fd 1 writes through.
        HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        // Probe before mutating: a failed GetConsoleMode leaves mode at 0, and ORing into that
        // would clear the console's other bits. ENABLE_PROCESSED_OUTPUT rides along because
        // Microsoft documents it as a prerequisite for the VT flag. SetConsoleMode changes
        // nothing when it fails, so either bail leaves the console exactly as found.
        if (GetConsoleMode(hout, &mode) == 0)
        {
            return false;
        }
        if (SetConsoleMode(hout, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0)
        {
            return false;
        }
        // Code page last, so a VT failure never leaves the console switched. Its return is
        // deliberately unchecked: CP 65001 ships with every supported Windows and the handle is
        // already a proven live console (the two calls above), so a failure has no realistic
        // trigger, and a false "VT ok" here would only mean the ▀ half-blocks garble rather
        // than not render. Accepted too: neither mode bit is restored on exit (Windows has no
        // disable_raw_mode counterpart), harmless since both are default-on, though a host that
        // had cleared PROCESSED_OUTPUT for raw byte output would find it left on.
        SetConsoleOutputCP(65001);
        return true;
#else
        return true;
#endif
    }

    inline void enable_raw_mode()
    {
#ifdef _WIN32
        init_console_output();
#else
        termios raw = {};
        tcgetattr(STDIN_FILENO, &raw);
        detail::saved_termios() = raw;

        // Disable echo and canonical (line-buffered) mode.
        // VMIN=0 / VTIME=0: read() returns immediately with 0 bytes if nothing available.
        raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        // Do NOT set O_NONBLOCK on stdin: in a terminal fd 0/1/2 share the same
        // open file description, so O_NONBLOCK on fd 0 also makes stdout
        // non-blocking, causing large fwrites to silently truncate.
#endif
    }

    inline void disable_raw_mode()
    {
#ifndef _WIN32
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &detail::saved_termios());
#endif
    }

    // ─── mouse support ───────────────────────────────────────────────────────────
    // Uses SGR extended mouse mode (\033[?1006h) — supported by all modern terminals
    // including Windows Terminal. Reports: scroll wheel, and button drags (the
    // button number is not decoded, so any button drags).

    inline void enable_mouse()
    {
#ifdef _WIN32
        // Allow VT input sequences (including mouse) on the input handle.
        HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(hin, &mode);
        SetConsoleMode(hin, mode | ENABLE_VIRTUAL_TERMINAL_INPUT);
#endif
        // \033[?1006h — SGR extended format: \033[<btn;x;yM / \033[<btn;x;ym
        // \033[?1002h — button-events mode: reports motion only while a button is held
        std::fputs("\033[?1006h\033[?1002h", stdout);
        std::fflush(stdout);
    }

    inline void disable_mouse()
    {
        std::fputs("\033[?1002l\033[?1006l", stdout);
        std::fflush(stdout);
    }

    // Input event vocabulary and the escape-sequence grammar live in input.h,
    // included above; poll_event below is the reading half, along with the buffer
    // it reads into and the policy governing how long it waits for the rest of a
    // sequence. That policy is stated in terms of clocks and read sizes, so it is
    // the reader's, not the grammar's.

    namespace detail
    {
        // Pending-input buffer size. Comfortably holds every sequence a key or a
        // mouse report can produce, which is what the parser needs to reassemble.
        // It does NOT hold every possible terminal reply: an OSC 52 clipboard answer
        // carries the whole selection and routinely runs longer, which is exactly
        // why the skip path below exists rather than being a corner case.
        constexpr int MAX_PENDING = 1024;

        // How long a partial sequence may sit without growing before it is abandoned.
        // This is an inter-byte gap, not a total budget: bytes of one sequence arrive
        // in a single write, while keystrokes are tens of ms apart even under
        // autorepeat, so the gap separates "rest of this sequence" from "next key".
        constexpr int PARTIAL_TIMEOUT_MS = 50;

        // Longest a partial sequence can get and still plausibly be a keypress. The
        // longest a key or mouse report produces is on the order of twenty bytes (a
        // full SGR report, a device-attributes reply), so a stalled partial past this
        // is a payload, not something someone pressed.
        //
        // Used to decide what a stalled partial means. Below it, the sequence is
        // abandoned and its bytes are gone, which is right for a fragmented arrow key.
        // Above it, abandoning would dispatch hundreds of payload bytes as keypresses,
        // so the skip is entered instead and the rate floor takes over.
        constexpr int MAX_KEY_SEQUENCE = 64;

        // ─── arrival rate meter ──────────────────────────────────────────────────
        // How fast bytes are arriving, measured continuously as a token bucket. Its
        // one consumer is the skip below: a sequence being skipped to its terminator
        // is given up on once the arrivals fall under RATE_QUOTA bytes per
        // RATE_WINDOW_MS, about 256 B/s. The meter itself runs unconditionally, which
        // is why none of these names says "skip": a skip that had to start the meter
        // would have to seed it with a guess, and no number available at that moment
        // is meaningful (tried twice, leaked both times).
        //
        // A rate, not a per-read size, and that is what makes it safe. The fastest a
        // person can produce bytes is key autorepeat, around 30 a second, so this
        // floor sits roughly eight times above anything a human can sustain and well
        // below any program. Set with that much headroom deliberately: a floor close
        // to the delivery rate of a slow link turns ordinary jitter into a leak, and
        // the margin that matters is the one over typing, not the one under a stream.
        // A per-read byte count was tried instead and cannot work, because the
        // terminal writes on the user's behalf and a mouse drag delivers reads as
        // large and as fast as a program does.
        //
        // This is the only thing that ends a skip whose sequence never terminates,
        // and it is deliberately the only thing. A cap on how much may be skipped was
        // tried and is exactly the mistake this whole design exists to avoid: every
        // bounded-consumption rule dispatches whatever falls past its bound, so a cap
        // large enough not to fire on a real reply still leaks that reply's tail once
        // a larger one arrives. Raising it only moves the boundary.
        constexpr int RATE_WINDOW_MS = 500;
        constexpr int RATE_QUOTA = 128;

        // How much unspent credit carries between windows, so the floor measures a
        // rate and not merely presence-per-window. A window that resets to zero is
        // phase-sensitive: a reply delivered in 1 KB bursts every 700 ms averages more
        // than five times the floor, yet whichever window happens to fall between two
        // bursts sees nothing and tears the skip down (measured, 5120 payload bytes
        // dispatched as quit keys). Carrying the surplus fixes that; the cap is what
        // stops it turning into an unbounded hold, since a stream that has really
        // stopped must still be given up on. At two windows' worth, a burst covers
        // about a second of silence, and arrivals that fall below the floor are given
        // up on within about two seconds (up to half a second of window phase, then
        // three windows to spend a full carry; measured 1.9 s). ALL arrivals count,
        // not just the skipped sequence's, so input arriving fast enough to clear the
        // floor holds a skip open. Typing cannot (it is ~8x under); a mouse drag can.
        // See skip_scan for why that is the accepted side of the trade.
        constexpr int RATE_MAX_CARRY = 2 * RATE_QUOTA;

        // Most windows one tick may charge for at once. The tick runs when the caller
        // polls, not on a timer, so a caller slower than RATE_WINDOW_MS owes several
        // windows by the time it arrives and must be charged for all of them; charging
        // one per tick makes the floor bytes-per-FRAME instead of a rate, and at a
        // frame interval past about four seconds ordinary typing clears the bar and
        // sustains a skip for as long as the user keeps typing (measured: 60 s and
        // counting at a 5 s frame). Capped so an arbitrarily long gap cannot demand an
        // arbitrary burst to survive, and so the multiply cannot overflow.
        constexpr int RATE_MAX_WINDOWS = 4;

        // Ceiling the credit saturates at, so a sustained flood cannot overflow it
        // (signed overflow is UB, and a wrapped negative credit reads as below the
        // floor and tears down an active skip). Needed because a pass that spends its
        // whole read budget returns before ticking the meter, so credit can grow
        // across many passes without once being spent.
        //
        // Sized to cover the largest quota a tick can charge plus a full carry, so
        // saturating never costs a stream its verdict: a ceiling below that would tear
        // down a fast stream purely for having been measured across a long gap.
        constexpr int RATE_MAX_CREDIT = (RATE_MAX_WINDOWS * RATE_QUOTA) + RATE_MAX_CARRY;

        struct ArrivalMeter
        {
            int credit = 0;
            std::chrono::steady_clock::time_point window;

            void record(int bytes) { credit = std::min(credit + bytes, RATE_MAX_CREDIT); }

            // Spends every window that has closed since the last tick and reports
            // whether the arrivals covered what they owed. Returns false until a
            // window actually closes, so a caller polling faster than the window sees
            // no verdict rather than a premature one.
            bool below_floor(std::chrono::steady_clock::time_point now)
            {
                const auto span = std::chrono::milliseconds(RATE_WINDOW_MS);
                if (window == std::chrono::steady_clock::time_point{})
                {
                    // First tick since this meter was created or reset. Without it the
                    // window starts at the clock's epoch and the very first tick
                    // charges the maximum quota against a credit of nothing.
                    window = now;
                }
                if (now - window < span)
                {
                    return false;
                }
                const auto elapsed = (now - window) / span;
                const int windows = static_cast<int>(std::min<decltype(elapsed)>(elapsed, RATE_MAX_WINDOWS));
                const int quota = windows * RATE_QUOTA;
                const bool starved = credit < quota;
                window = now;
                credit = std::clamp(credit - quota, 0, RATE_MAX_CARRY);
                return starved;
            }
        };

        // Fairness bound on one drain pass, not a limit on the input. poll_event
        // refills the buffer as often as the bytes keep coming rather than once per
        // call, because anything that resolves without producing an event (a skip, a
        // backlog of sequences with no binding) would otherwise advance one bufferful
        // per rendered frame: the caller drains until Type::None, so one fill per call
        // paces input at the frame rate instead of the arrival rate.
        //
        // Counted per pass rather than per call, and reset only when Type::None is
        // returned, precisely because that is what ends the caller's drain. Per call
        // it would bound nothing: the caller is free to call again, so the two bounds
        // would not compose into a bound on one frame. Nothing is dropped on reaching
        // it; the next pass resumes where this one left off.
        //
        // Roughly a megabyte per pass, which no ordinary reply comes near. What that
        // costs is not the same on both platforms: POSIX pays about a thousand bulk
        // reads for it (well under a millisecond), while the Windows console API
        // yields one byte per CRT round-trip and so pays a million. The bound is not
        // lowered there anyway, because the only alternative is spreading the same
        // reads over later frames, which is the frame-rate pacing above; a console
        // that could actually deliver a megabyte this way would be the more
        // surprising thing.
        constexpr int MAX_REFILLS_PER_PASS = 1024;

        // Bytes read but not yet parsed, carried across poll_event calls so a
        // sequence split over several frames is reassembled instead of decided on
        // partial evidence. `last_growth` stamps the most recent append.
        struct Pending
        {
            char buf[MAX_PENDING] = {};
            int len = 0;
            std::chrono::steady_clock::time_point last_growth;
            // Whether the sequence at the front is being skipped, i.e. is too long
            // to hold in full: either it filled the buffer, or it stalled past
            // MAX_KEY_SEQUENCE, which is longer than any keypress. enter_skip keeps a
            // resume prefix (the introducer, plus a trailing ESC when the buffer ends
            // on one) and detail::skip_scan looks through what follows for that
            // family's end. While set, parse_input is not called at all, so the flag is
            // cleared only by skip_scan finding that end or by the rate floor giving
            // up.
            bool skipping = false;
            // Runs whether or not a skip is in progress, so a skip never has to be
            // seeded with a guess at the rate.
            ArrivalMeter meter;
            // Reads spent in the current drain pass, against MAX_REFILLS_PER_PASS.
            // Lives here rather than in poll_event because a pass spans calls: it is
            // reset when Type::None ends the caller's drain, not on entry.
            int refills = 0;
        };

        inline Pending &pending()
        {
            static Pending p;
            return p;
        }

        // Reads whatever has already arrived, without waiting for what has not, and
        // returns the count. Self-guarding on both platforms so callers need no
        // separate readiness probe: a second probe would be a wasted syscall on POSIX
        // and a second _kbhit on Windows, and having two places encode "is a byte
        // available" invites them to disagree.
        //
        // A zero return ends the drain, and it has to mean "nothing arrived" rather
        // than "not ready": at end of stream poll() reports POLLHUP as ready forever
        // while read() returns 0, which would otherwise pack the buffer with NULs.
        inline int read_available(char *out, int cap)
        {
#ifdef _WIN32
            // The console API only yields one byte at a time.
            int n = 0;
            while (n < cap && _kbhit())
            {
                out[n++] = static_cast<char>(_getch());
            }
            return n;
#else
            // One read for the whole burst rather than one per byte. The poll() is
            // what keeps it from blocking on a pipe (raw-mode stdin would not block,
            // but the tests feed a pipe).
            struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
            if (poll(&pfd, 1, 0) <= 0)
            {
                return 0;
            }
            return static_cast<int>(read(STDIN_FILENO, out, static_cast<size_t>(cap)));
#endif
        }

        // Appends whatever has arrived to the tail of the buffer and returns how much.
        // One read per call, not a loop: it is capped by the space left, so a large
        // reply does leave bytes queued, and poll_event is what comes back for them
        // (bounded by MAX_REFILLS_PER_PASS). Looping here instead would move that bound
        // out of the caller's reach, which is the distinction MAX_REFILLS_PER_PASS
        // exists to keep. No full-buffer guard, because there is no path here with a
        // full buffer: poll_event collapses an overflowing sequence to its resume
        // prefix before ever looping back.
        inline int refill(Pending &p)
        {
            const int got = read_available(p.buf + p.len, MAX_PENDING - p.len);
            if (got <= 0)
            {
                return 0;
            }
            p.len += got;
            p.last_growth = std::chrono::steady_clock::now();
            return got;
        }

        // Starts a skip of the sequence at the front, or carries one on: the buffer
        // collapses to a prefix skip_scan can resume the SAME sequence from, so it
        // goes on looking through the bytes still to come for that family's
        // terminator. Two things have to survive the collapse:
        //   - the two-byte introducer, which names the family and so its terminator;
        //   - a trailing ESC, if the buffer ends on one. The scan stopped there
        //     precisely because that ESC may be the first half of an ST, so dropping
        //     it leaves the reply unterminatable: the skip then runs to the rate floor
        //     and swallows whatever is typed in the meantime. This one was a measured
        //     leak when it was missing.
        // Both entry points hold far more than three bytes, so the ESC being appended
        // never overwrites part of the introducer. Nothing of the payload is kept,
        // because skip_scan reads only the introducer and then scans: an earlier
        // version held the first payload byte for the decoding grammar's sake, and
        // that grammar no longer runs during a skip.
        //
        // Deliberately does not touch the rate floor's accounting: the arrival meter
        // has been running all along, so a skip inherits a rate that was actually
        // measured rather than one seeded from whatever happened last. Seeding it was
        // tried twice and cannot be made to work, because the number available at
        // entry is arbitrary: the read that crosses the buffer is capped by the space
        // left in it, and a skip reached from a stalled partial has no such read at
        // all, so both entered below the floor and were torn down at the first window,
        // dispatching the reply as keypresses. Measured across burst sizes, every size
        // leaked except one, and that one was the buffer size itself.
        inline void enter_skip(Pending &p)
        {
            const bool ends_on_esc = p.buf[p.len - 1] == '\033';
            p.len = 2;
            if (ends_on_esc)
            {
                p.buf[2] = '\033';
                p.len = 3;
            }
            p.skipping = true;
        }
    } // namespace detail

    // ─── poll_event ──────────────────────────────────────────────────────────────
    // Returns the next keyboard or mouse event, or InputEvent{Type::None} when no
    // complete event is available. On POSIX it never blocks: the readiness probe and
    // the read are both zero-timeout, so a caller in a render loop never pays a stall
    // for input it did not get. On Windows the pairing of _kbhit() with _getch() is
    // the CRT's only non-blocking idiom and is not a hard guarantee, since _kbhit()
    // can report a queued console event that _getch() then waits on; this predates
    // the buffered design and would need the ReadConsoleInput API to close.
    //
    // Bytes are drained into a pending buffer and parsed from there by
    // detail::parse_input, so a sequence split across frames is reassembled rather
    // than decided on partial evidence. A partial that stops growing for
    // PARTIAL_TIMEOUT_MS is discarded, because an unparsable fragment must not reach
    // the dispatch as keypresses: 'q' quits. Losing the user's next keystroke to such
    // a discard is the deliberate trade. One that has grown past MAX_KEY_SEQUENCE is
    // too long to be a keypress at all, so it is skipped to its terminator instead of
    // discarded, on the same terms as a sequence too long for the buffer.
    //
    // What is discarded is the buffered prefix, not the sequence. Anything of that
    // same sequence that arrives after the gap is indistinguishable from fresh input
    // and is parsed as such, so its tail can dispatch. That is a property of the
    // input, not a gap in the implementation: a sequence whose bytes are spaced
    // further apart than the timeout cannot be told from someone typing those same
    // bytes, and choosing to reassemble it instead would abandon live sequences on
    // every slow frame.
    //
    // A sequence too long for the buffer is handled differently, and without any such
    // ambiguity: its introducer is kept and the middle dropped, so detail::skip_scan
    // keeps looking for that family's terminator. Nothing there depends on how fast or
    // in what sized pieces the rest arrives. Such a skip ends in exactly two ways: the
    // family's terminator, or the rate floor giving up on one that never terminates.
    //
    // An ESC is deliberately NOT one of them, unlike everywhere else in the grammar.
    // The rule that a bare ESC aborts a string sequence is a terminal parser's rule,
    // and applying it here reads the abort as "this reply is over" when what actually
    // happened is that the terminal wrote an ordinary sequence between two chunks of
    // the reply: the reply goes on afterwards, and ending the skip hands its tail to
    // the dispatch as keypresses, quit key included (measured, with a scroll notch
    // delivered inside a clipboard reply). Whichever it was, those bytes are nobody's
    // input, so they are swallowed. See detail::skip_scan.
    //
    // Only 7-bit ESC-prefixed forms are recognized. An 8-bit C1 introducer (0x9B
    // CSI, 0x9D OSC) is not: rasterminal requires a UTF-8 terminal for its
    // half-block output, and in UTF-8 those bytes are continuation bytes, never
    // standalone controls, so treating them as introducers would corrupt input
    // rather than fix anything.

    // Ends a drain pass, releasing the read budget poll_event spends within one (see
    // MAX_REFILLS_PER_PASS). poll_event releases it itself when it returns Type::None,
    // which is how a drain ordinarily ends; a caller that leaves its loop for any other
    // reason has to say so here, because only the caller knows where its pass ended.
    // Without it the budget carries into the next frame and can be found already spent,
    // with input still queued. Idempotent, so calling it unconditionally after a drain
    // is both correct and the simplest thing to write.
    inline void end_input_pass()
    {
        detail::pending().refills = 0;
    }

    inline InputEvent poll_event()
    {
        detail::Pending &p = detail::pending();

        // Whether any read in THIS CALL returned bytes. Call-scoped, unlike the read
        // budget beside it, and deliberately: it exists only so the staleness rule
        // below never judges a partial on a call that did not look for its rest, and
        // a call is the unit that either looked or did not.
        bool read_this_call = false;

        // Drops `n` bytes from the front, keeping whatever follows them.
        //
        // Compacts rather than carrying a read offset, which makes a backlog of short
        // sequences quadratic in how full the buffer is: a bufferful of four-byte
        // sequences moves about 100 KB to consume 1 KB. Left alone deliberately, and
        // measured rather than assumed: one whole drain pass of nothing but four-byte
        // unbound sequences, the shape that maximises the amplification, costs 3.1 ms
        // for 900 KB, against a 16.6 ms frame at 60 fps. The same volume as one skipped
        // reply costs 1.1 ms (a skip holds only its resume prefix, so it has no
        // amplification at all), and a flood of mouse reports 0.12 ms. A read offset
        // would put a second index into the buffer invariant that the compaction, the
        // resume-prefix collapse and the refill capacity would all have to agree on,
        // and mis-agreeing invariants in exactly this buffer are what most of this
        // file's history consists of.
        auto consume = [&p](int n)
        {
            p.len -= n;
            if (p.len > 0)
            {
                std::memmove(p.buf, p.buf + n, static_cast<size_t>(p.len));
            }
        };

        // Parse before reading: when the buffer already holds a complete event this
        // returns without a syscall or a clock read at all, which matters during a
        // mouse drag where one burst yields many events.
        //
        // Every iteration either consumes at least one byte of a bounded buffer or
        // spends one of a bounded number of reads, so this terminates. Dropped
        // sequences do not end the pass, so Type::None reliably means "nothing more to
        // report this frame".
        for (;;)
        {
            if (p.skipping)
            {
                // A sequence being skipped is located, never decoded, so it reports
                // nothing whatever its bytes look like: the middle is missing, and any
                // decode of the fragment that resumed it would be a fabrication (probe:
                // the tail of a skipped mouse report parsed as a whole report and
                // orbited the camera by coordinates nobody sent). Keeping the skip on
                // its own scanner is what makes that structural rather than a rule the
                // reporting path has to remember.
                const int end = detail::skip_scan(p.buf, p.len);
                if (end > 0)
                {
                    consume(end);
                    p.skipping = false;
                    continue;
                }
                // No terminator yet: fall through to the overflow, read and rate-floor
                // handling below, which is the same for a skip as for a partial.
            }
            else
            {
                const detail::ParseResult r = detail::parse_input(p.buf, p.len);
                if (r.kind != detail::ParseResult::Kind::Incomplete)
                {
                    consume(r.consumed);
                    if (r.kind == detail::ParseResult::Kind::Complete)
                    {
                        return r.event;
                    }
                    continue;
                }
            }

            // A full buffer that still does not parse means one sequence is longer
            // than the buffer. Keep a resume prefix and drop the middle: the parser
            // then goes on scanning the bytes still to come for that family's
            // terminator, which is the one signal that says where the sequence ends.
            //
            // Neither this branch nor the stalled-partial one below has to check for
            // that introducer: any first byte other than ESC parses as a key, so an
            // incomplete buffer always begins with one.
            //
            // Deliberately not a guess about who is producing the bytes. Volume and
            // timing were both tried and neither can separate a program writing from
            // a person typing, because the terminal writes on the person's behalf: a
            // mouse drag delivers machine-sized reads at machine speed and locked the
            // viewer out entirely, quit key included. The terminator is a property of
            // the protocol, so it holds at any delivery rate and needs no such guess.
            if (p.len >= detail::MAX_PENDING)
            {
                detail::enter_skip(p);
                continue; // the read below carries the skip on, within the pass budget
            }

            // Read whatever has arrived and parse again. Bounded per pass, not per
            // call: see MAX_REFILLS_PER_PASS for why one read per call paces input at
            // the frame rate (measured at 16.6 s of lockout on a megabyte reply, and
            // 1.07 s on 64 KB of sequences with no binding).
            //
            // With the budget spent, end the pass here rather than falling through.
            // Nothing below may run: every rule down there reads "no bytes arrived" off
            // a read that did not happen, and would abandon a live partial whose rest
            // is already queued in the kernel.
            if (p.refills >= detail::MAX_REFILLS_PER_PASS)
            {
                end_input_pass();
                return InputEvent{};
            }
            p.refills++;
            const int just_read = detail::refill(p);
            if (just_read > 0)
            {
                read_this_call = true;
                p.meter.record(just_read);
                continue; // re-parse: the sequence may now be whole
            }

            // One clock read for both rules below, which is also the only one on the
            // idle path: a poll that finds nothing reaches exactly here.
            const auto now = std::chrono::steady_clock::now();

            // The partial is abandoned only when THIS CALL saw no new bytes. The
            // timestamp records when bytes were last appended, which is observed at
            // the caller's cadence, not at arrival: judging staleness while bytes are
            // still arriving would abandon a perfectly good sequence whenever the
            // frame interval exceeds the timeout (a heavy model, or --fps 10), and its
            // tail would then dispatch as loose keypresses.
            //
            // The cost is more than a swallowed keystroke. Keys struck close enough
            // together are indistinguishable from a fragmented sequence, so typing
            // Escape then [ then A reports one arrow. That much is inherent: the
            // bytes are identical and only arrival timing separates them.
            //
            // The window is wider than PARTIAL_TIMEOUT_MS, though, in two ways, and the
            // read-this-call guard is why. Bytes that arrive during a long frame are
            // all read by the same drain, which then never takes this branch however
            // much time has passed, so the window is at least max(timeout, frame): at
            // fps 10 two keys 80 ms apart still merge. And because ANY byte arriving
            // both refreshes last_growth and sets the guard, a partial survives for as
            // long as input keeps coming faster than the timeout, whatever that input
            // is: hold a key at the usual 25/s autorepeat in front of a truncated
            // ESC [ (which is also what alt+[ sends) and those keystrokes are swallowed
            // for as long as the key is held, since none of them is a CSI final. It
            // self-heals on release, which is what makes it the accepted side of the
            // trade rather than a lockout. Widening the window is deliberate either
            // way: the alternative is abandoning live sequences on every slow frame,
            // and fragmented delivery is far more common than hand-typing an escape
            // sequence this fast.
            // Not applied to a skip in progress. A skip holds only its resume prefix
            // between chunks, so this rule would see it go quiet on any inter-chunk gap
            // wider than the timeout and tear the skip down; the next chunk would then
            // parse as fresh input and dispatch the payload. That is what the rate
            // floor below is for, and it never got the chance because this fired first.
            if (!p.skipping && !read_this_call && p.len > 0 &&
                now - p.last_growth > std::chrono::milliseconds(detail::PARTIAL_TIMEOUT_MS))
            {
                // Too long to be a keypress means it is a payload arriving in pieces
                // wider apart than the reassembly window. Abandoning it would dispatch
                // every byte already held, so switch to skipping instead and let the
                // rate floor decide when it has really stopped. Shorter partials are
                // abandoned as before, which is what a fragmented arrow key needs.
                if (p.len >= detail::MAX_KEY_SEQUENCE)
                {
                    detail::enter_skip(p);
                    continue;
                }
                p.len = 0;
                continue;
            }

            // The rate floor. A skip whose window closes below the quota is given up
            // on, because the sequence is not really arriving any more and the bytes
            // still coming are the user's. The staleness rule above cannot cover that
            // case: typing faster than its gap keeps the buffer growing, so it never
            // fires. The meter is ticked unconditionally and only its verdict is
            // conditional, so it always reports a rate it has measured itself.
            const bool starved = p.meter.below_floor(now);
            if (p.skipping && starved)
            {
                p.len = 0;
                p.skipping = false;
                continue;
            }

            // Nothing more to report this frame, which is what ends the caller's
            // drain and so releases the pass budget.
            end_input_pass();
            return InputEvent{};
        }
    }

} // namespace platform
