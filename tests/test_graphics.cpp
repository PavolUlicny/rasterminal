#include "src/graphics.h"
#include "tests/test.h"

#include <string>

namespace
{

    constexpr char KITTY_OK[] = "\033_Gi=31;OK\033\\";
    constexpr char CELL_SIZE[] = "\033[6;33;15t"; // 33 px tall, 15 px wide
    constexpr char DSR[] = "\033[0n";

    // One full scan of a byte string, the way the startup reader uses it when
    // everything arrives in one read.
    ReplyScan scan(const std::string &s, TermGraphics &out)
    {
        return parse_graphics_replies(s.data(), static_cast<int>(s.size()), out);
    }

    // Feeds the stream one byte at a time through the compacting loop the real
    // reader runs: append, scan, drop the consumed prefix, repeat.
    TermGraphics drip_feed(const std::string &s, bool &done)
    {
        TermGraphics tg;
        std::string buf;
        done = false;
        for (const char c : s)
        {
            buf += c;
            const ReplyScan r = parse_graphics_replies(buf.data(), static_cast<int>(buf.size()), tg);
            if (r.done)
            {
                done = true;
                break;
            }
            buf.erase(0, static_cast<size_t>(r.consumed));
        }
        return tg;
    }

} // namespace

TEST(graphics, full_reply_batch)
{
    TermGraphics tg;
    const ReplyScan r = scan(std::string(KITTY_OK) + CELL_SIZE + DSR, tg);
    ASSERT_TRUE(r.done);
    ASSERT_TRUE(tg.kitty);
    ASSERT_EQ(tg.cell_w, 15);
    ASSERT_EQ(tg.cell_h, 33);
}

TEST(graphics, error_reply_means_no_kitty)
{
    TermGraphics tg;
    const ReplyScan r = scan(std::string("\033_Gi=31;ENOTSUPPORTED:whatever\033\\") + DSR, tg);
    ASSERT_TRUE(r.done);
    ASSERT_FALSE(tg.kitty);
}

TEST(graphics, dsr_only_means_no_graphics_support)
{
    TermGraphics tg;
    const ReplyScan r = scan(DSR, tg);
    ASSERT_TRUE(r.done);
    ASSERT_FALSE(tg.kitty);
    ASSERT_EQ(tg.cell_w, 0);
    ASSERT_EQ(tg.cell_h, 0);
}

TEST(graphics, missing_cell_size_reply)
{
    TermGraphics tg;
    const ReplyScan r = scan(std::string(KITTY_OK) + DSR, tg);
    ASSERT_TRUE(r.done);
    ASSERT_TRUE(tg.kitty);
    ASSERT_EQ(tg.cell_w, 0);
}

TEST(graphics, byte_by_byte_delivery)
{
    bool done = false;
    const TermGraphics tg = drip_feed(std::string(KITTY_OK) + CELL_SIZE + DSR, done);
    ASSERT_TRUE(done);
    ASSERT_TRUE(tg.kitty);
    ASSERT_EQ(tg.cell_w, 15);
    ASSERT_EQ(tg.cell_h, 33);
}

TEST(graphics, junk_between_replies_is_skipped)
{
    // Keystrokes, an arrow, an alt chord, and an OSC reply landing between ours.
    const std::string s =
        std::string("qq") + KITTY_OK + "w\033[A\033x" + "\033]0;title\a" + CELL_SIZE + "  " + DSR + "trailing";
    TermGraphics tg;
    const ReplyScan r = scan(s, tg);
    ASSERT_TRUE(r.done);
    ASSERT_TRUE(tg.kitty);
    ASSERT_EQ(tg.cell_w, 15);
    ASSERT_EQ(tg.cell_h, 33);
}

TEST(graphics, stray_esc_before_reply_is_a_boundary)
{
    // A bare typed ESC chased by a real reply: the second ESC is the reply's
    // introducer, not the chord's second byte, so the reply must survive.
    const std::string s = std::string("\033") + KITTY_OK + DSR;
    TermGraphics tg;
    const ReplyScan r = scan(s, tg);
    ASSERT_TRUE(r.done);
    ASSERT_TRUE(tg.kitty);
}

TEST(graphics, stray_esc_before_dsr_does_not_eat_the_sentinel)
{
    // The scenario the boundary rule protects at the end of the batch: a typed
    // ESC just ahead of the DSR must not consume its introducer, which would
    // stall detection for the full deadline.
    const std::string s = KITTY_OK + std::string("\033") + DSR;
    TermGraphics tg;
    const ReplyScan r = scan(s, tg);
    ASSERT_TRUE(r.done);
    ASSERT_TRUE(tg.kitty);
}

TEST(graphics, stray_string_introducer_does_not_swallow_the_reply)
{
    // A typed alt+] chord opens an OSC that never terminates; the reply's own ST
    // must not be taken as that junk's terminator (the embedded-ESC boundary
    // rule): the junk is dropped short of the reply and the reply parses whole.
    const std::string s = std::string("\033]junk") + KITTY_OK + CELL_SIZE + DSR;
    TermGraphics tg;
    const ReplyScan r = scan(s, tg);
    ASSERT_TRUE(r.done);
    ASSERT_TRUE(tg.kitty);
    ASSERT_EQ(tg.cell_w, 15);
    ASSERT_EQ(tg.cell_h, 33);
}

TEST(graphics, stray_apc_introducer_does_not_swallow_the_reply)
{
    // Same boundary rule in the ST-only family: an unterminated APC fragment
    // ahead of the batch must not consume the replies behind it.
    const std::string s = std::string("\033_x") + KITTY_OK + DSR;
    TermGraphics tg;
    const ReplyScan r = scan(s, tg);
    ASSERT_TRUE(r.done);
    ASSERT_TRUE(tg.kitty);
}

TEST(graphics, shm_probe_ok_sets_the_transport)
{
    TermGraphics tg;
    const ReplyScan r = scan(std::string(KITTY_OK) + "\033_Gi=32;OK\033\\" + CELL_SIZE + DSR, tg);
    ASSERT_TRUE(r.done);
    ASSERT_TRUE(tg.kitty);
    ASSERT_TRUE(tg.kitty_shm);
}

TEST(graphics, shm_probe_error_means_direct_transport)
{
    // The far end of an ssh session (or a sandboxed terminal) cannot open the
    // probe object and says so; only the shm transport is ruled out.
    TermGraphics tg;
    const ReplyScan r = scan(std::string(KITTY_OK) + "\033_Gi=32;ENOENT:no such object\033\\" + DSR, tg);
    ASSERT_TRUE(r.done);
    ASSERT_TRUE(tg.kitty);
    ASSERT_FALSE(tg.kitty_shm);
}

TEST(graphics, shm_probe_unanswered_means_direct_transport)
{
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string(KITTY_OK) + DSR, tg).done);
    ASSERT_TRUE(tg.kitty);
    ASSERT_FALSE(tg.kitty_shm);
}

TEST(graphics, wrong_query_id_is_not_ours)
{
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033_Gi=99;OK\033\\") + DSR, tg).done);
    ASSERT_FALSE(tg.kitty);
}

TEST(graphics, id_key_match_is_whole_key)
{
    // i=310 must not read as i=31.
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033_Gi=310;OK\033\\") + DSR, tg).done);
    ASSERT_FALSE(tg.kitty);
}

TEST(graphics, out_of_range_cell_size_is_ignored)
{
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[6;0;15t") + "\033[6;33;5000t" + DSR, tg).done);
    ASSERT_EQ(tg.cell_w, 0);
    ASSERT_EQ(tg.cell_h, 0);
}

TEST(graphics, wrong_first_param_is_not_a_cell_size)
{
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[4;33;15t") + DSR, tg).done);
    ASSERT_EQ(tg.cell_w, 0);
}

TEST(graphics, incomplete_tail_is_left_unconsumed)
{
    // Buffer ends inside the APC reply: nothing of it may be consumed, and the
    // scan must not report done.
    const std::string s = std::string("x") + "\033_Gi=31;O";
    TermGraphics tg;
    const ReplyScan r = scan(s, tg);
    ASSERT_FALSE(r.done);
    ASSERT_EQ(r.consumed, 1); // only the loose keystroke
    ASSERT_FALSE(tg.kitty);
}

TEST(graphics, st_split_across_reads)
{
    // The ST's backslash arrives in a later read than its ESC.
    bool done = false;
    const TermGraphics tg = drip_feed(std::string(KITTY_OK) + DSR, done);
    ASSERT_TRUE(done);
    ASSERT_TRUE(tg.kitty);
}
