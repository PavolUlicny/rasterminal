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

TEST(graphics, da1_with_param_4_means_sixel)
{
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?62;4;22c") + DSR, tg).done);
    ASSERT_TRUE(tg.sixel);
    ASSERT_FALSE(tg.kitty); // DA1 says nothing about kitty
}

TEST(graphics, da1_without_param_4_means_no_sixel)
{
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?62;22c") + DSR, tg).done);
    ASSERT_FALSE(tg.sixel);
}

TEST(graphics, da1_single_param_4_means_sixel)
{
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?4c") + DSR, tg).done);
    ASSERT_TRUE(tg.sixel);
}

TEST(graphics, da1_param_match_is_whole_token)
{
    // 44 and 40 both contain a '4'; neither is the sixel parameter.
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?62;44;40c") + DSR, tg).done);
    ASSERT_FALSE(tg.sixel);
}

TEST(graphics, da1_oversized_param_is_bounded_and_ignored)
{
    // A digit run past the accumulation cap must neither overflow nor match.
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?9999999999999999999c") + DSR, tg).done);
    ASSERT_FALSE(tg.sixel);
}

TEST(graphics, da1_non_digit_taints_only_its_own_token)
{
    // A ':' sub-parameter or a stray intermediate byte must not discard the
    // whole reply; only the token carrying it stops counting as plain 4.
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?62:1;4;22c") + DSR, tg).done);
    ASSERT_TRUE(tg.sixel);
    TermGraphics tg2;
    ASSERT_TRUE(scan(std::string("\033[?1;4;62 c") + DSR, tg2).done);
    ASSERT_TRUE(tg2.sixel);
}

TEST(graphics, da1_subparametered_4_is_not_the_plain_capability)
{
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?4:1c") + DSR, tg).done);
    ASSERT_FALSE(tg.sixel);
    TermGraphics tg2;
    ASSERT_TRUE(scan(std::string("\033[?62;4:2;22c") + DSR, tg2).done);
    ASSERT_FALSE(tg2.sixel);
}

TEST(graphics, csi_c_without_private_marker_is_not_da1)
{
    // A plain 4 token behind no '?': only the private-marker guard rejects it,
    // so this fails if the guard goes. Bare \033[c beside it is the one input
    // where that guard reads the final byte itself (a bounds tripwire).
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[62;4c") + DSR, tg).done);
    ASSERT_FALSE(tg.sixel);
    TermGraphics tg2;
    ASSERT_TRUE(scan(std::string("\033[c") + DSR, tg2).done);
    ASSERT_FALSE(tg2.sixel);
}

TEST(graphics, da1_intermediate_before_the_final_drops_the_last_token)
{
    // The taint rule's accepted conservative direction: an intermediate walked
    // past by the final-scan lands in the trailing token, so its 4 stops
    // counting as plain.
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?62;4 c") + DSR, tg).done);
    ASSERT_FALSE(tg.sixel);
}

TEST(graphics, da1_capped_token_does_not_poison_the_next)
{
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?9999999999;4c") + DSR, tg).done);
    ASSERT_TRUE(tg.sixel);
}

TEST(graphics, da1_empty_params_mean_no_sixel)
{
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?c") + DSR, tg).done);
    ASSERT_FALSE(tg.sixel);
}

TEST(graphics, truncated_da1_is_dropped_at_the_boundary)
{
    // The chasing reply's ESC ends the unterminated DA1 (the boundary rule);
    // nothing from it is honoured and the rest of the batch still lands.
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?62;4") + DSR, tg).done);
    ASSERT_FALSE(tg.sixel);
    TermGraphics tg2;
    ASSERT_TRUE(scan(std::string("\033[?4") + KITTY_OK + DSR, tg2).done);
    ASSERT_TRUE(tg2.kitty);
    ASSERT_FALSE(tg2.sixel);
}

TEST(graphics, da2_style_reply_is_not_da1)
{
    // '>'-led (DA2) and '='-led (DA3) c-final replies are located and skipped.
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[>1;4;0c") + "\033[=4c" + DSR, tg).done);
    ASSERT_FALSE(tg.sixel);
}

TEST(graphics, da1_split_across_reads)
{
    bool done = false;
    const TermGraphics tg = drip_feed(std::string("\033[?62;4c") + DSR, done);
    ASSERT_TRUE(done);
    ASSERT_TRUE(tg.sixel);
}

TEST(graphics, sixel_geometry_reply_records_the_max)
{
    // XTSMGRAPHICS item-2 read reply: width;height in px, status 0 = success.
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?2;0;1000;1000S") + DSR, tg).done);
    ASSERT_EQ(tg.sixel_max_w, 1000);
    ASSERT_EQ(tg.sixel_max_h, 1000);
}

TEST(graphics, sixel_geometry_failure_or_other_item_is_ignored)
{
    // A failure status and the colour-registers item (1) are located and
    // skipped; neither may write the max. The four-parameter forms pin the
    // item and status guards themselves, which the short forms cannot reach
    // (arity rejects them first).
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?2;3;0S") + "\033[?1;0;1024S" + DSR, tg).done);
    ASSERT_EQ(tg.sixel_max_w, 0);
    ASSERT_EQ(tg.sixel_max_h, 0);
    TermGraphics tg2;
    ASSERT_TRUE(scan(std::string("\033[?1;0;100;100S") + "\033[?2;3;10;10S" + DSR, tg2).done);
    ASSERT_EQ(tg2.sixel_max_w, 0);
    ASSERT_EQ(tg2.sixel_max_h, 0);
}

TEST(graphics, sixel_geometry_empty_status_is_ignored)
{
    // An empty status token accumulates to 0, which must not read as the
    // success status (the given[] arity guard is what rejects it).
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?2;;480;312S") + DSR, tg).done);
    ASSERT_EQ(tg.sixel_max_w, 0);
    ASSERT_EQ(tg.sixel_max_h, 0);
}

TEST(graphics, private_dsr_reply_is_not_the_sentinel)
{
    // A stale '?'-led DECDSR reply in the queue must not end detection before
    // the capability replies arrive; only the plain DSR form is the sentinel.
    TermGraphics tg;
    const ReplyScan r = scan(std::string("\033[?13n") + KITTY_OK + DSR, tg);
    ASSERT_TRUE(r.done);
    ASSERT_TRUE(tg.kitty);
}

TEST(graphics, sixel_geometry_invalid_values_are_ignored)
{
    TermGraphics tg;
    ASSERT_TRUE(scan(std::string("\033[?2;0;0;100S") + DSR, tg).done);
    ASSERT_EQ(tg.sixel_max_w, 0);
    TermGraphics tg2;
    ASSERT_TRUE(scan(std::string("\033[?2;0;1000001;10S") + DSR, tg2).done);
    ASSERT_EQ(tg2.sixel_max_w, 0);
}

TEST(graphics, sixel_geometry_split_across_reads)
{
    bool done = false;
    const TermGraphics tg = drip_feed(std::string("\033[?2;0;480;312S") + DSR, done);
    ASSERT_TRUE(done);
    ASSERT_EQ(tg.sixel_max_w, 480);
    ASSERT_EQ(tg.sixel_max_h, 312);
}

TEST(graphics, full_batch_with_da1)
{
    // The real reply order of the full query batch: kitty OK, cell size, DA1,
    // sixel geometry, DSR.
    TermGraphics tg;
    const ReplyScan r = scan(std::string(KITTY_OK) + CELL_SIZE + "\033[?62;4;22c" + "\033[?2;0;1000;1000S" + DSR, tg);
    ASSERT_TRUE(r.done);
    ASSERT_TRUE(tg.kitty);
    ASSERT_TRUE(tg.sixel);
    ASSERT_EQ(tg.cell_w, 15);
    ASSERT_EQ(tg.cell_h, 33);
    ASSERT_EQ(tg.sixel_max_w, 1000);
    ASSERT_EQ(tg.sixel_max_h, 1000);
}
