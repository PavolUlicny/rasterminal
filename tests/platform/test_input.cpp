#include "tests/test.h"
#include "src/platform/input.h"
#include "src/platform/input_reader.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

// Leftover sequence bytes become live keybindings.

namespace
{
    using PK = platform::detail::ParseResult::Kind;

    platform::detail::ParseResult parse(const std::string &bytes)
    {
        return platform::detail::parse_input(bytes.data(), static_cast<int>(bytes.size()));
    }

    // Require an unbound sequence to be consumed whole.
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

    // Every proper prefix must remain buffered as Incomplete.
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
    // Case folding makes Shift irrelevant.
    expect_key("E", platform::Key::E, 1);
    expect_key("V", platform::Key::V, 1);
    // Unbound bytes must not produce Key::None events.
    expect_dropped_whole("z");
    expect_dropped_whole("\a");
}

TEST(parse_input, bare_escape_is_incomplete_not_a_key)
{
    // ESC stays incomplete because it can begin any escape sequence.
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
    // DECCKM sends arrows as SS3.
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
    // Scan parameterized SS3 forms through the final byte.
    expect_dropped_whole("\033O1;2Q");
    expect_dropped_whole("\033O5P");
    expect_every_prefix_incomplete("\033O1;2Q");

    // Stop the SS3 scan before a non-parameter byte.
    const platform::detail::ParseResult plus = parse("\033O1+");
    ASSERT_EQ(plus.kind, PK::Drop);
    ASSERT_EQ(plus.consumed, 3);
    expect_key("+", platform::Key::Plus, 1);
}

TEST(parse_input, unknown_csi_consumed_to_final)
{
    expect_dropped_whole("\033[15~");  // F5
    expect_dropped_whole("\033[1;5A"); // modified ctrl+Up is unbound
    expect_dropped_whole("\033[H");    // Home
    expect_dropped_whole("\033[3~");   // Delete
    expect_dropped_whole("\033[Z");    // shift+tab
    expect_every_prefix_incomplete("\033[15~");
}

TEST(parse_input, cell_size_report)
{
    // XTWINOPS sends height before width.
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
    expect_dropped_whole("\033[4;33;15t");   // different XTWINOPS report
    expect_dropped_whole("\033[6;33t");      // short of its three parameters
    expect_dropped_whole("\033[6;33;15;2t"); // a fourth parameter
    expect_dropped_whole("\033[6;0;15t");    // zero is not a cell size
    expect_dropped_whole("\033[6;33;1001t"); // past the sanity ceiling
    expect_dropped_whole("\033[t");          // no parameters at all
    expect_dropped_whole("\033[6;3:3;15t");  // non-numeric parameter byte (sub-parameter colon)
}

TEST(parse_input, sixel_geometry_report)
{
    // XTSMGRAPHICS sends width before height.
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
    expect_dropped_whole("\033[?2;;480;312S");     // empty status
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
    // This real xterm DA1 reply ends with live keybindings.
    expect_dropped_whole("\033[?62;1;2;6;7;8;9;15;16;17;18;21;22;23;24;42;44;45;46c");
}

TEST(parse_input, linux_console_function_keys_dropped)
{
    // Linux VC function keys use a second '[' that is also a legal CSI final.
    expect_dropped_whole("\033[[A");
    expect_dropped_whole("\033[[C");
    expect_dropped_whole("\033[[E");
    expect_every_prefix_incomplete("\033[[A");
}

TEST(parse_input, x10_mouse_report_consumed_by_count)
{
    // X10 mouse reports have three payload bytes after the CSI final.
    expect_dropped_whole("\033[M\x20\x51\x21");
    expect_dropped_whole("\033[M\x20\x72\x21"); // 'r' would reset the view
    // Wrapped X10 coordinates may contain NUL or ESC.
    expect_dropped_whole(std::string("\033[M\x20\x00Q", 6));
    expect_dropped_whole("\033[M\x20\x1b\x21");
    expect_dropped_whole("\033[M\x20\x51\x1b"); // ESC as the y coordinate
    expect_every_prefix_incomplete("\033[M\x20\x51\x21");
}

TEST(parse_input, string_sequences_consumed_through_terminator)
{
    // BEL terminates OSC; ST terminates every string family.
    expect_dropped_whole("\033]0;quit me\a");
    // BEL is payload in DCS, SOS, PM, and APC.
    expect_dropped_whole("\033X status\033\\");
    expect_dropped_whole("\033^private\033\\");
    expect_dropped_whole("\033_app\033\\");
    expect_dropped_whole("\033Pdata\awith bel\033\\"); // BEL survives as payload
    ASSERT_EQ(parse("\033X status\a").kind, PK::Incomplete);
    expect_dropped_whole("\033P1$r0m\033\\");
    expect_dropped_whole("\033]11;rgb:1e1e/1e1e/1e1e\033\\");
    expect_every_prefix_incomplete("\033]0;title\a");
}

TEST(parse_input, long_string_payload_consumed_whole)
{
    // A long clipboard reply must not leak payload as keypresses.
    expect_dropped_whole("\033]52;c;" + std::string(400, 'A') + "q\a");
}

TEST(parse_input, alt_key_chords_dropped)
{
    expect_dropped_whole("\033x");
    expect_dropped_whole("\033z");
}

TEST(parse_input, alt_arrow_reports_the_arrow)
{
    // altSendsEscape prefixes the original sequence with another ESC.
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
    // Reject overflow instead of reporting a clamped drag origin.
    expect_dropped_whole("\033[<0;99999999999999999999;6M");
}

TEST(parse_input, sgr_mouse_scans_to_its_final_before_deciding)
{
    // Consume malformed CSI sequences through their terminator.
    expect_dropped_whole("\033[<0 1;2M");   // stray space among the parameters
    expect_dropped_whole("\033[<0;5:6;7M"); // sub-parameter separator
    expect_dropped_whole("\033[<0;5;6#M");  // CSI intermediate byte
    expect_dropped_whole("\033[<0;5;6;9M"); // a fourth parameter
    expect_dropped_whole("\033[<0;5;6A");   // terminated, but not a mouse final
}

TEST(parse_input, malformed_sgr_mouse_is_dropped_not_reported)
{
    // Private CSI sequences must not fall through to arrow parsing.
    expect_dropped_whole("\033[<0;1;1A");

    // Reject reports without all three mouse parameters.
    expect_dropped_whole("\033[<M");
    expect_dropped_whole("\033[<0M");
    expect_dropped_whole("\033[<0;5M");
    expect_dropped_whole("\033[<;;M"); // separators present, values absent
    const platform::detail::ParseResult ok = parse("\033[<0;5;6M");
    ASSERT_EQ(ok.kind, PK::Complete);
    ASSERT_EQ(ok.event.x, 5);
    ASSERT_EQ(ok.event.y, 6);
}

TEST(parse_input, a_truncated_sequence_never_swallows_the_next_one)
{
    // Leave ESC for the next sequence.
    struct Case
    {
        const char *bytes;
        int consumed;
    };
    const Case cases[] = {
        { "\033[1\033[A", 3 },    // generic CSI scan
        { "\033O1\033[A", 3 },    // parameterized SS3 scan
        { "\033[<0;1\033[A", 6 }, // SGR mouse scan
        // Counted forms must also leave the next ESC untouched.
        { "\033O\033[A", 2 },  // single-shot SS3
        { "\033[[\033[A", 3 }, // Linux VC F1-F5
    };
    for (const Case &c : cases)
    {
        const std::string s(c.bytes);
        const platform::detail::ParseResult r = parse(s);
        ASSERT_EQ(r.kind, PK::Drop);
        ASSERT_EQ(r.consumed, c.consumed);
        // The remaining sequence must still parse as one event.
        expect_key(s.substr(static_cast<size_t>(r.consumed)), platform::Key::Up, 3);
    }
}

TEST(parse_input, a_string_sequence_treats_an_embedded_esc_as_payload)
{
    // ESC ends CSI parameters but remains payload unless it begins ST in a string.
    ASSERT_EQ(parse("\033]0;abc\033[A").kind, PK::Incomplete);
    ASSERT_EQ(parse("\033]52;c;AAA\033[<64;1;1MAAA").kind, PK::Incomplete);

    expect_dropped_whole("\033]0;abc\033[A\a");
    expect_dropped_whole("\033P q \033[<64;1;1M more\033\\");
    expect_dropped_whole("\033]0;abc\033\\");
}

TEST(parse_input, sgr_wheel_is_detected_through_modifier_bits)
{
    // Wheel direction uses the low bits; modifier bits must not change it.
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

    // Buttons 6 and 7 are horizontal wheel events, not vertical scroll.
    expect_dropped_whole("\033[<66;1;1M");
    expect_dropped_whole("\033[<67;1;1M");

    // Wheel releases are malformed and must not produce a second scroll.
    expect_dropped_whole("\033[<64;1;1m");
    expect_dropped_whole("\033[<65;1;1m");

    // Motion requires a held button and the press form.
    expect_dropped_whole("\033[<35;5;6M"); // motion without a button
    expect_dropped_whole("\033[<32;5;6m"); // motion flag on a release

    // Reject zero and out-of-range SGR coordinates.
    expect_dropped_whole("\033[<0;0;6M");
    expect_dropped_whole("\033[<0;5;0M");
    expect_dropped_whole("\033[<0;999999;6M");
    expect_dropped_whole("\033[<0;5;999999M");

    // The button parameter contains exactly eight flag bits.
    expect_dropped_whole("\033[<999999;5;6M");
    expect_dropped_whole("\033[<256;5;6M");

    // A no-button report cannot be a press.
    expect_dropped_whole("\033[<3;5;6M");

    // Bit 7 changes the low bits to extended buttons 8 through 11.
    const platform::detail::ParseResult ext = parse("\033[<163;5;6M");
    ASSERT_EQ(ext.kind, PK::Complete);
    ASSERT_EQ(ext.event.type, platform::InputEvent::Type::MouseMove);
    const platform::detail::ParseResult ext_press = parse("\033[<131;5;6M");
    ASSERT_EQ(ext_press.kind, PK::Complete);
    ASSERT_EQ(ext_press.event.type, platform::InputEvent::Type::MousePress);
}

TEST(parse_input, grammar_properties_hold_over_every_short_byte_string)
{
    // Check parser invariants over about one million prefixes up to four bytes.
    const char alphabet[] = { '\033', '[', ']', 'O', 'M', '<', ';', '0', 'A', 'q', '\a', '\\', 'P', '\0', ' ' };
    const auto same_event = [](const platform::InputEvent &a, const platform::InputEvent &b)
    { return a.type == b.type && a.key == b.key && a.x == b.x && a.y == b.y; };

    // clang-tidy rejects a recursive lambda here.
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
    // Over-consumption would underflow poll_event's buffer compaction.
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

// Pipes exercise POSIX buffering but cannot emulate Windows console input.

#ifndef _WIN32
namespace
{
    // Replace stdin with a pipe for staged reads.
    struct StdinFeed
    {
        int saved_stdin;
        int write_fd = -1;
        bool ok = false;
        StdinFeed() : saved_stdin(test_dup(STDIN_FILENO))
        {
            // Reset all persistent parser state between tests.
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
        // Reject short writes.
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
    // Dropped sequences must not stop the current drain pass.
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
    // Hold a fragmented sequence until it is complete.
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
    // A stale partial must not leak its payload as keypresses.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033[1"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    std::this_thread::sleep_for(std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS * 2));

    // Do not discard a partial on the same poll that extends it.
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
}

TEST(poll_event, reassembly_survives_a_frame_slower_than_the_timeout)
{
    // A slow poll must not make a growing partial stale.
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
    // Skip over-length sequences through their family terminator.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const int n = platform::detail::MAX_PENDING;
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(n) * 3, 'q')));
    for (int frame = 0; frame < 8; frame++)
    {
        ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    }

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, over_length_sequence_survives_any_delivery_pattern)
{
    // Chunking must not change over-length sequence handling.
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
    // Skip mode must survive gaps between payload chunks.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'q')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    for (int chunk = 0; chunk < 6; chunk++)
    {
        platform::detail::pending().last_growth =
            std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
        // Isolate the inter-chunk timeout from the rate floor.
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
    // Promote a payload-sized stale partial to skip mode.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(400, 'q')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_sequence_arriving_inside_a_skip_does_not_end_it)
{
    // An ESC sequence inside a string reply is payload, not a skip terminator.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const int n = platform::detail::MAX_PENDING;
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(n) * 2, 'A')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("\033[<32;40;12M" + std::string("AAqAA")));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_sequence_arriving_inside_a_skipped_csi_parses_whole)
{
    // ESC ends a skipped CSI and starts the next sequence.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const int n = platform::detail::MAX_PENDING;
    ASSERT_TRUE(in.push("\033[" + std::string(static_cast<size_t>(n) * 2, '1')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // The following X10 payload contains a live 'q' binding.
    ASSERT_TRUE(in.push(std::string("\033[M\x20\x71\x21", 6)));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_skipped_csi_still_ends_at_its_own_final_byte)
{
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const int n = platform::detail::MAX_PENDING;
    ASSERT_TRUE(in.push("\033[" + std::string(static_cast<size_t>(n) * 2, '1')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("~"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);
}

TEST(poll_event, a_terminator_split_across_the_buffer_boundary_still_ends_the_skip)
{
    // Preserve a trailing ESC that may begin an ST terminator.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const auto n = static_cast<size_t>(platform::detail::MAX_PENDING);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(n - 8, 'A') + "\033"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("\\"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, an_over_long_csi_does_not_resume_as_a_plain_arrow)
{
    // A skipped CSI final must not decode as an arrow.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const auto n = static_cast<size_t>(platform::detail::MAX_PENDING);
    ASSERT_TRUE(in.push("\033[" + std::string(n - 2, '0')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(in.push("A"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_backlog_of_unbound_sequences_clears_at_the_rate_it_arrived)
{
    // Drain unbound backlogs in one pass.
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
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_skipped_sequence_never_reports_an_event)
{
    // Never decode a tail after skip mode drops the sequence middle.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033[<0;" + std::string(70, '9'))); // past MAX_KEY_SEQUENCE, then stalls
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // This tail is a valid mouse report by itself.
    ASSERT_TRUE(in.push("32;40;12M"));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, the_rate_floor_charges_for_every_window_that_elapsed)
{
    // Charge every elapsed window so the rate floor is frame-rate independent.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'x')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // One autorepeat burst cannot cover five elapsed windows.
    const int one_frame_of_typing = 30 * 5; // 30 B/s for 5 s
    ASSERT_TRUE(one_frame_of_typing > platform::detail::RATE_QUOTA);
    platform::detail::pending().meter.credit = one_frame_of_typing;
    platform::detail::pending().meter.window =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(5 * platform::detail::RATE_WINDOW_MS);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);
}

TEST(poll_event, the_arrival_credit_saturates_instead_of_overflowing)
{
    // Saturate credit before signed overflow.
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
    // Callers that stop before Type::None must release the per-pass budget.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    platform::detail::pending().refills = platform::detail::MAX_REFILLS_PER_PASS;
    platform::end_input_pass();
    ASSERT_EQ(platform::detail::pending().refills, 0);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, a_spent_read_budget_does_not_abandon_a_live_partial)
{
    // A spent read budget is not evidence that a partial sequence is stale.
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
    // Reaching the cap ends the pass and releases its budget.
    ASSERT_EQ(platform::detail::pending().refills, 0);

    // The next pass can judge the partial stale.
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_EQ(platform::detail::pending().len, 0);
}

TEST(poll_event, a_skip_promoted_from_a_stalled_partial_inherits_the_measured_rate)
{
    // Seed a promoted skip from the measured arrival rate.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(400, 'q'))); // below MAX_PENDING
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // The initial 400 bytes must carry the first rate window.
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
    // Carry burst credit so the floor measures average rate.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'q')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // Age two empty windows without sleeping.
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
    // The rate floor must end an unterminated skip.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'q')));
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // Age an empty window without sleeping.
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
    // Consume multiple bufferfuls per drain pass.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const auto n = static_cast<size_t>(platform::detail::MAX_PENDING);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(n * 8, 'q') + "\a"));
    // One call consumes all eight buffers and the terminator.
    ASSERT_EQ(next_type(), platform::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const platform::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, platform::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, platform::Key::Q);
}

TEST(poll_event, never_blocks_on_an_incomplete_sequence)
{
    // Repeated calls distinguish scheduler jitter from an inter-byte wait.
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
