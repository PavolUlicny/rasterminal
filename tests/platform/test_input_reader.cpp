#include "tests/test.h"
#include "src/platform/input_reader.h"
#include "src/terminal/input.h"

#include <chrono>
#include <string>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

// Leftover sequence bytes become live keybindings.

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

    terminal_input::InputEvent::Type next_type()
    {
        return platform::poll_event().type;
    }
} // namespace

TEST(poll_event, reads_a_key_from_the_stream)
{
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
}

TEST(poll_event, drains_every_event_in_one_burst)
{
    // Dropped sequences must not stop the current drain pass.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033[15~q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
}

TEST(poll_event, reassembles_a_sequence_split_across_calls)
{
    // Hold a fragmented sequence until it is complete.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(in.push("["));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(in.push("A"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Up);
}

TEST(poll_event, stalled_partial_is_discarded_whole_not_dispatched)
{
    // A stale partial must not leak its payload as keypresses.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033[1"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    std::this_thread::sleep_for(std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS * 2));

    // Do not discard a partial on the same poll that extends it.
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);

    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
}

TEST(poll_event, reassembly_survives_a_frame_slower_than_the_timeout)
{
    // A slow poll must not make a growing partial stale.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    std::this_thread::sleep_for(std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS * 2));
    ASSERT_TRUE(in.push("[A"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Up);
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
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
        ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    }

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
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
            ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
        }
        ASSERT_TRUE(platform::detail::pending().skipping);
        ASSERT_TRUE(in.push("\a"));
        ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
        ASSERT_TRUE(in.push("q"));
        const terminal_input::InputEvent ev = platform::poll_event();
        ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
        ASSERT_EQ(ev.key, terminal_input::Key::Q);
    }
}

TEST(poll_event, over_length_sequence_survives_gaps_between_chunks)
{
    // Skip mode must survive gaps between payload chunks.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'q')));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    for (int chunk = 0; chunk < 6; chunk++)
    {
        platform::detail::pending().last_growth =
            std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
        // Isolate the inter-chunk timeout from the rate floor.
        platform::detail::pending().meter.window = std::chrono::steady_clock::now();
        ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
        ASSERT_TRUE(in.push(std::string(400, 'q')));
        ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    }

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
}

TEST(poll_event, a_stalled_long_partial_becomes_a_skip_not_a_dispatch)
{
    // Promote a payload-sized stale partial to skip mode.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(400, 'q')));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
}

TEST(poll_event, a_sequence_arriving_inside_a_skip_does_not_end_it)
{
    // An ESC sequence inside a string reply is payload, not a skip terminator.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const int n = platform::detail::MAX_PENDING;
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(n) * 2, 'A')));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("\033[<32;40;12M" + std::string("AAqAA")));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
}

TEST(poll_event, a_sequence_arriving_inside_a_skipped_csi_parses_whole)
{
    // ESC ends a skipped CSI and starts the next sequence.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const int n = platform::detail::MAX_PENDING;
    ASSERT_TRUE(in.push("\033[" + std::string(static_cast<size_t>(n) * 2, '1')));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // The following X10 payload contains a live 'q' binding.
    ASSERT_TRUE(in.push(std::string("\033[M\x20\x71\x21", 6)));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
}

TEST(poll_event, a_skipped_csi_still_ends_at_its_own_final_byte)
{
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const int n = platform::detail::MAX_PENDING;
    ASSERT_TRUE(in.push("\033[" + std::string(static_cast<size_t>(n) * 2, '1')));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("~"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);
}

TEST(poll_event, a_terminator_split_across_the_buffer_boundary_still_ends_the_skip)
{
    // Preserve a trailing ESC that may begin an ST terminator.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const auto n = static_cast<size_t>(platform::detail::MAX_PENDING);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(n - 8, 'A') + "\033"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(in.push("\\"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
}

TEST(poll_event, an_over_long_csi_does_not_resume_as_a_plain_arrow)
{
    // A skipped CSI final must not decode as an arrow.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const auto n = static_cast<size_t>(platform::detail::MAX_PENDING);
    ASSERT_TRUE(in.push("\033[" + std::string(n - 2, '0')));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(in.push("A"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);

    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
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
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
}

TEST(poll_event, a_skipped_sequence_never_reports_an_event)
{
    // Never decode a tail after skip mode drops the sequence middle.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033[<0;" + std::string(70, '9'))); // past MAX_KEY_SEQUENCE, then stalls
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // This tail is a valid mouse report by itself.
    ASSERT_TRUE(in.push("32;40;12M"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
}

TEST(poll_event, the_rate_floor_charges_for_every_window_that_elapsed)
{
    // Charge every elapsed window so the rate floor is frame-rate independent.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'x')));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // One autorepeat burst cannot cover five elapsed windows.
    const int one_frame_of_typing = 30 * 5; // 30 B/s for 5 s
    ASSERT_TRUE(one_frame_of_typing > platform::detail::RATE_QUOTA);
    platform::detail::pending().meter.credit = one_frame_of_typing;
    platform::detail::pending().meter.window =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(5 * platform::detail::RATE_WINDOW_MS);
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
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
        ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
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
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
}

TEST(poll_event, a_spent_read_budget_does_not_abandon_a_live_partial)
{
    // A spent read budget is not evidence that a partial sequence is stale.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_EQ(platform::detail::pending().len, 1);

    platform::detail::pending().refills = platform::detail::MAX_REFILLS_PER_PASS;
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_EQ(platform::detail::pending().len, 1); // held, not discarded
    // Reaching the cap ends the pass and releases its budget.
    ASSERT_EQ(platform::detail::pending().refills, 0);

    // The next pass can judge the partial stale.
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_EQ(platform::detail::pending().len, 0);
}

TEST(poll_event, a_skip_promoted_from_a_stalled_partial_inherits_the_measured_rate)
{
    // Seed a promoted skip from the measured arrival rate.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(400, 'q'))); // below MAX_PENDING
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    platform::detail::pending().last_growth =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::PARTIAL_TIMEOUT_MS + 1);
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // The initial 400 bytes must carry the first rate window.
    platform::detail::pending().meter.window =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::RATE_WINDOW_MS + 1);
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
}

TEST(poll_event, the_rate_floor_measures_a_rate_not_presence_in_each_window)
{
    // Carry burst credit so the floor measures average rate.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'q')));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // Age two empty windows without sleeping.
    for (int window = 0; window < 2; window++)
    {
        platform::detail::pending().meter.window =
            std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::RATE_WINDOW_MS + 1);
        ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
        ASSERT_TRUE(platform::detail::pending().skipping);
    }

    ASSERT_TRUE(in.push("\a"));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
}

TEST(poll_event, an_unterminated_sequence_is_given_up_on_when_it_stops_arriving)
{
    // The rate floor must end an unterminated skip.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(static_cast<size_t>(platform::detail::MAX_PENDING), 'q')));
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(platform::detail::pending().skipping);

    // Age an empty window without sleeping.
    platform::detail::pending().meter.window =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(platform::detail::RATE_WINDOW_MS + 1);
    platform::detail::pending().meter.credit = 0;
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
}

TEST(poll_event, a_skip_advances_at_the_rate_bytes_arrive_not_one_buffer_per_call)
{
    // Consume multiple bufferfuls per drain pass.
    StdinFeed in;
    ASSERT_TRUE(in.ok);
    const auto n = static_cast<size_t>(platform::detail::MAX_PENDING);
    ASSERT_TRUE(in.push("\033]52;c;" + std::string(n * 8, 'q') + "\a"));
    // One call consumes all eight buffers and the terminator.
    ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    ASSERT_TRUE(!platform::detail::pending().skipping);

    ASSERT_TRUE(in.push("q"));
    const terminal_input::InputEvent ev = platform::poll_event();
    ASSERT_EQ(ev.type, terminal_input::InputEvent::Type::Key);
    ASSERT_EQ(ev.key, terminal_input::Key::Q);
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
        ASSERT_EQ(next_type(), terminal_input::InputEvent::Type::None);
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(ms < (CALLS * platform::detail::PARTIAL_TIMEOUT_MS) / 10);
}
#endif
