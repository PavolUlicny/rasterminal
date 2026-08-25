#include "tests/test.h"
#include "src/terminal/text.h"

#include <cstddef>
#include <string>

namespace
{
    // Structural UTF-8 check: every leading byte is followed by exactly the continuation bytes
    // its length announces, and the string does not end mid-sequence. Enough to catch a split
    // landing inside a code point, which is the only way truncate_middle can produce bad bytes.
    bool is_valid_utf8(const std::string &s)
    {
        size_t i = 0;
        while (i < s.size())
        {
            const auto b = static_cast<unsigned char>(s[i]);
            size_t len = 0;
            if (b < 0x80u)
            {
                len = 1;
            }
            else if ((b & 0xE0u) == 0xC0u)
            {
                len = 2;
            }
            else if ((b & 0xF0u) == 0xE0u)
            {
                len = 3;
            }
            else if ((b & 0xF8u) == 0xF0u)
            {
                len = 4;
            }
            else
            {
                return false; // continuation byte where a lead byte belongs, or an invalid lead
            }
            if (i + len > s.size())
            {
                return false;
            }
            for (size_t k = 1; k < len; ++k)
            {
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0u) != 0x80u)
                {
                    return false;
                }
            }
            i += len;
        }
        return true;
    }

    // U+2605 BLACK STAR, 3 bytes. Repeated, it puts a code point boundary every 3 bytes, so an
    // even-ish split lands inside a sequence and both boundary walks have to fire.
    const std::string STAR = "\xe2\x98\x85";

    std::string repeat_star(size_t n)
    {
        std::string s;
        for (size_t i = 0; i < n; ++i)
        {
            s += STAR;
        }
        return s;
    }
} // namespace

TEST(text, truncate_middle_shorter_than_budget_is_unchanged)
{
    ASSERT_TRUE(truncate_middle("suzanne.obj", 24) == "suzanne.obj");
}

TEST(text, truncate_middle_exactly_budget_is_unchanged)
{
    const std::string name(24, 'a');
    ASSERT_TRUE(truncate_middle(name, 24) == name);
}

TEST(text, truncate_middle_one_over_budget_fits_exactly)
{
    // The first size that truncates. ASCII, so the result must fill the budget exactly: no
    // boundary walk shrinks it.
    const std::string name(25, 'a');
    const std::string out = truncate_middle(name, 24);
    ASSERT_EQ(out.size(), static_cast<size_t>(24));
    ASSERT_TRUE(out.find("...") != std::string::npos);
}

TEST(text, truncate_middle_keeps_head_and_extension)
{
    // The whole point of splitting the middle rather than the tail: the extension survives.
    const std::string out = truncate_middle("a_very_long_scanned_model_name.stl", 24);
    ASSERT_EQ(out.size(), static_cast<size_t>(24));
    ASSERT_TRUE(out == "a_very_long...l_name.stl");
}

TEST(text, truncate_middle_splits_on_utf8_boundaries)
{
    // 10 stars = 30 bytes. At budget 24 the raw splits land at byte 11 (inside the 4th star)
    // and byte 20 (inside the 7th), so both walks must move off the continuation bytes.
    const std::string out = truncate_middle(repeat_star(10), 24);
    ASSERT_TRUE(is_valid_utf8(out));
    ASSERT_TRUE(out.size() <= 24u);
    ASSERT_TRUE(out.find("...") != std::string::npos);
    // Walks shrink, never grow: 3 stars + "..." + 3 stars.
    ASSERT_TRUE(out == repeat_star(3) + "..." + repeat_star(3));
}

TEST(text, truncate_middle_name_without_extension)
{
    const std::string out = truncate_middle(std::string(40, 'x'), 24);
    ASSERT_EQ(out.size(), static_cast<size_t>(24));
    ASSERT_TRUE(out == std::string(11, 'x') + "..." + std::string(10, 'x'));
}

TEST(text, sanitize_controls_replaces_c0_and_del)
{
    ASSERT_TRUE(sanitize_controls("a\x1b[31mb") == "a?[31mb");
    ASSERT_TRUE(sanitize_controls(std::string("a\0b", 3)) == "a?b");
    ASSERT_TRUE(sanitize_controls("\x7f") == "?");
    ASSERT_TRUE(sanitize_controls("tab\there\nnl\rcr") == "tab?here?nl?cr");
}

TEST(text, sanitize_controls_replaces_c1)
{
    // U+009B is CSI: a terminal not in UTF-8 mode reads the raw byte as an escape introducer,
    // and the UTF-8 encoding of the same code point is no more welcome in a displayed name.
    ASSERT_TRUE(sanitize_controls("a\xc2\x9b_31m") == "a??_31m");
    ASSERT_TRUE(sanitize_controls("\x9b") == "?"); // raw C1 byte: invalid UTF-8 as well
}

TEST(text, sanitize_controls_replaces_invalid_utf8)
{
    // A POSIX filename is an arbitrary byte string, so every malformed shape has to be covered:
    // a lone continuation byte, a truncated sequence, an overlong form, and a surrogate half.
    ASSERT_TRUE(sanitize_controls("\xff\xfe") == "??");
    ASSERT_TRUE(
        sanitize_controls("a\x98"
                          "b") == "a?b"
    );
    ASSERT_TRUE(sanitize_controls("\xe2\x98") == "??");           // truncated 3-byte lead
    ASSERT_TRUE(sanitize_controls("\xc0\xaf") == "??");           // overlong '/'
    ASSERT_TRUE(sanitize_controls("\xed\xa0\x80") == "???");      // UTF-16 surrogate half
    ASSERT_TRUE(sanitize_controls("\xf5\x80\x80\x80") == "????"); // past U+10FFFF
}

TEST(text, sanitize_controls_replaces_every_listed_format_control)
{
    // The documented invisible and direction-changing character set must be exhaustive.
    ASSERT_TRUE(sanitize_controls("\xc2\xad") == "??");           // U+00AD soft hyphen
    ASSERT_TRUE(sanitize_controls("\xd8\x9c") == "??");           // U+061C Arabic letter mark
    ASSERT_TRUE(sanitize_controls("\xe1\xa0\x8e") == "???");      // U+180E Mongolian vowel sep
    ASSERT_TRUE(sanitize_controls("\xe2\x80\xa8") == "???");      // U+2028 line separator
    ASSERT_TRUE(sanitize_controls("\xef\xbf\xb9") == "???");      // U+FFF9 interlinear anchor
    ASSERT_TRUE(sanitize_controls("\xf3\xa0\x80\x81") == "????"); // U+E0001 language tag
    ASSERT_TRUE(sanitize_controls("\xf3\xa0\x81\xa1") == "????"); // U+E0061 tag latin a
    // Category Cf reaches well past the characters that come to mind; these are the rest of it.
    ASSERT_TRUE(sanitize_controls("\xd8\x80") == "??");           // U+0600 Arabic number sign
    ASSERT_TRUE(sanitize_controls("\xdb\x9d") == "??");           // U+06DD Arabic end of ayah
    ASSERT_TRUE(sanitize_controls("\xdc\x8f") == "??");           // U+070F Syriac abbreviation
    ASSERT_TRUE(sanitize_controls("\xe2\x81\xaa") == "???");      // U+206A inhibit symmetric swap
    ASSERT_TRUE(sanitize_controls("\xf0\x91\x81\xbf") == "????"); // U+1107F Brahmi number joiner
    ASSERT_TRUE(sanitize_controls("\xf0\x91\x82\xbd") == "????"); // U+110BD Kaithi number sign
    ASSERT_TRUE(sanitize_controls("\xf0\x93\x90\xb0") == "????"); // U+13430 Egyptian vertical join
    ASSERT_TRUE(sanitize_controls("\xf0\x9d\x85\xb3") == "????"); // U+1D173 musical begin beam
}

TEST(text, sanitize_controls_replaces_bidi_and_zero_width)
{
    // Strip invisible U+200B and spoofing U+202E. Build U+202E at runtime so this
    // source file cannot itself display misleadingly in an editor.
    const std::string rlo = std::string("a") + "\xe2" + "\x80" + "\xae" + "b";
    ASSERT_TRUE(sanitize_controls(rlo) == "a???b");
    ASSERT_TRUE(
        sanitize_controls("a\xe2\x80\x8b"
                          "b") == "a???b"
    );
    ASSERT_TRUE(
        sanitize_controls("a\xef\xbb\xbf"
                          "b") == "a???b"
    ); // U+FEFF
}

TEST(text, sanitize_controls_preserves_byte_length)
{
    // One '?' per replaced byte: the byte budget truncate_middle applies, and the width bound
    // derived from it, both have to survive the pass.
    const std::string dirty = "a\x1b\xc2\x9b\xff\xed\xa0\x80z";
    ASSERT_EQ(sanitize_controls(dirty).size(), dirty.size());
}

TEST(text, sanitize_controls_passes_printable_and_utf8)
{
    ASSERT_TRUE(sanitize_controls("normal-name.obj") == "normal-name.obj");
    const std::string stars = repeat_star(3);
    ASSERT_TRUE(sanitize_controls(stars) == stars);
    // Combining marks are ordinary content (Vietnamese, Greek polytonic, and so on).
    ASSERT_TRUE(sanitize_controls("e\xcc\x81") == "e\xcc\x81");
    ASSERT_TRUE(sanitize_controls("").empty());
}

TEST(text, display_width_ascii_equals_byte_count)
{
    ASSERT_EQ(display_width("suzanne.obj"), static_cast<size_t>(11));
    ASSERT_EQ(display_width(""), static_cast<size_t>(0));
}

TEST(text, display_width_narrow_multibyte_is_one_column)
{
    // The HUD separator (U+00B7, 2 bytes) and a 3-byte symbol are each one column: this is the
    // gap that a byte count over-counts and that the bar's padding must not inherit.
    ASSERT_EQ(display_width("\xc2\xb7"), static_cast<size_t>(1));
    ASSERT_EQ(display_width(STAR), static_cast<size_t>(1));
    ASSERT_EQ(display_width(repeat_star(12)), static_cast<size_t>(12));
}

TEST(text, display_width_east_asian_is_two_columns)
{
    ASSERT_EQ(display_width("\xe4\xb8\x80"), static_cast<size_t>(2));     // U+4E00
    ASSERT_EQ(display_width("\xef\xbc\xa1"), static_cast<size_t>(2));     // U+FF21 fullwidth A
    ASSERT_EQ(display_width("\xf0\x9f\x98\x80"), static_cast<size_t>(2)); // U+1F600 emoji
}

TEST(text, display_width_emoji_outside_the_common_blocks_are_two_columns)
{
    // Emoji are spread over blocks that keep being added to, and under-counting one pushes the
    // bar past the terminal edge, so everything above the BMP counts as two columns rather than
    // being enumerated. These four all sat outside an earlier hand-written block list.
    ASSERT_EQ(display_width("\xf0\x9f\x9a\x80"), static_cast<size_t>(2)); // U+1F680 rocket
    ASSERT_EQ(display_width("\xf0\x9f\x87\xa6"), static_cast<size_t>(2)); // U+1F1E6 regional indicator
    ASSERT_EQ(display_width("\xf0\x9f\x9f\xa0"), static_cast<size_t>(2)); // U+1F7E0 orange circle
    ASSERT_EQ(display_width("\xf0\x9f\xa9\xb0"), static_cast<size_t>(2)); // U+1FA70 ballet shoes
}

TEST(text, display_width_bmp_wide_symbols_are_two_columns)
{
    // These wide BMP code points sit outside the blanket rule for code points above the BMP.
    // Under-measuring them pushes the fps field past the right edge.
    ASSERT_EQ(display_width("\xe2\x8c\x9a"), static_cast<size_t>(2)); // U+231A watch
    ASSERT_EQ(display_width("\xe2\x8c\xa9"), static_cast<size_t>(2)); // U+2329 angle bracket
    ASSERT_EQ(display_width("\xe2\x9a\xa1"), static_cast<size_t>(2)); // U+26A1 high voltage
    ASSERT_EQ(display_width("\xe2\x9c\x85"), static_cast<size_t>(2)); // U+2705 check mark button
    ASSERT_EQ(display_width("\xe2\xad\x90"), static_cast<size_t>(2)); // U+2B50 star
    ASSERT_EQ(display_width("\xe4\xb7\x80"), static_cast<size_t>(2)); // U+4DC0 hexagram
    ASSERT_EQ(display_width("\xed\x9e\xb0"), static_cast<size_t>(2)); // U+D7B0 Hangul Jamo Ext-B
}

TEST(text, display_width_emoji_presentation_selector_adds_a_column)
{
    // U+FE0F asks for emoji presentation, and the terminal then draws the base character two
    // columns wide, so the selector carries whatever the base was not already charged.
    // U+2122 is Neutral and outside every wide range, so the pair is exactly the promotion.
    ASSERT_EQ(display_width("\xe2\x84\xa2\xef\xb8\x8f"), static_cast<size_t>(2)); // U+2122 U+FE0F
    // Its text-presentation sibling U+FE0E promotes nothing and stays zero-width.
    ASSERT_EQ(display_width("\xe2\x84\xa2\xef\xb8\x8e"), static_cast<size_t>(1)); // U+2122 U+FE0E
    // A base already measured wide must not be charged twice: still two columns, not three.
    ASSERT_EQ(display_width("\xe2\x9d\xa4\xef\xb8\x8f"), static_cast<size_t>(2));     // U+2764 U+FE0F
    ASSERT_EQ(display_width("\xf0\x9f\x98\x80\xef\xb8\x8f"), static_cast<size_t>(2)); // U+1F600 U+FE0F
    // No base at all is malformed; it rounds up rather than measuring short.
    ASSERT_EQ(display_width("\xef\xb8\x8f"), static_cast<size_t>(2)); // bare U+FE0F
}

TEST(text, display_width_marks_inside_wide_ranges_are_zero)
{
    // ZERO must override coarse Wide ranges for nonspacing marks that add no column.
    ASSERT_EQ(display_width("\xe3\x82\x9a"), static_cast<size_t>(0)); // U+309A kana semi-voiced
    ASSERT_EQ(display_width("\xe3\x80\xaa"), static_cast<size_t>(0)); // U+302A CJK tone mark
    // A kana syllable plus its voiced-sound mark is one 2-column glyph, not two.
    ASSERT_EQ(display_width("\xe3\x81\x8b\xe3\x82\x99"), static_cast<size_t>(2)); // U+304B U+3099
}

TEST(text, display_width_regional_indicators_round_up_to_two_each)
{
    // Terminals render two regional indicators as either one two-column flag or two
    // two-column letters. Charge four columns, preferring harmless overestimation.
    const std::string ri_a = "\xf0\x9f\x87\xa6"; // U+1F1E6
    const std::string ri_u = "\xf0\x9f\x87\xba"; // U+1F1FA
    ASSERT_EQ(display_width(ri_a), static_cast<size_t>(2));
    ASSERT_EQ(display_width(ri_a + ri_u), static_cast<size_t>(4));
    ASSERT_EQ(display_width(ri_a + ri_u + ri_a + ri_u), static_cast<size_t>(8));
}

TEST(text, display_width_combining_marks_are_zero)
{
    ASSERT_EQ(display_width("e\xcc\x81"), static_cast<size_t>(1)); // e + combining acute
}

TEST(text, display_width_spacing_combining_marks_are_one_column)
{
    // Devanagari mixes nonspacing marks with SPACING ones (Unicode Mc) in the same blocks. The
    // spacing ones take a column of their own; counting them zero (as a whole-block range would)
    // under-measures the name and runs the bar off the edge.
    ASSERT_EQ(display_width("\xe0\xa4\x83"), static_cast<size_t>(1)); // U+0903 visarga (Mc)
    ASSERT_EQ(display_width("\xe0\xa5\x8b"), static_cast<size_t>(1)); // U+094B vowel sign o (Mc)
    ASSERT_EQ(display_width("\xe0\xa4\xbe"), static_cast<size_t>(1)); // U+093E vowel sign aa (Mc)
    // ...while the nonspacing neighbours in the same blocks stay zero.
    ASSERT_EQ(display_width("\xe0\xa4\x81"), static_cast<size_t>(0)); // U+0901 candrabindu (Mn)
    ASSERT_EQ(display_width("\xe0\xa5\x8d"), static_cast<size_t>(0)); // U+094D virama (Mn)
    // A whole syllable: base + spacing vowel sign = 2 columns.
    ASSERT_EQ(display_width("\xe0\xa4\x95\xe0\xa4\xbe"), static_cast<size_t>(2)); // U+0915 U+093E
}

TEST(text, display_width_invalid_byte_counts_one)
{
    // Matches what sanitize_controls leaves in its place, so measuring before or after the
    // sanitize pass gives the same answer.
    ASSERT_EQ(display_width("\xff"), static_cast<size_t>(1));
    ASSERT_EQ(display_width("a\xe2\x98"), static_cast<size_t>(3));
}

TEST(text, truncate_middle_degenerate_budgets)
{
    // No caller asks for these, but the split arithmetic must not underflow and the result must
    // still be valid UTF-8 within budget.
    const std::string stars = repeat_star(10);
    for (size_t budget : { size_t{ 0 }, size_t{ 1 }, size_t{ 2 }, size_t{ 3 }, size_t{ 4 }, size_t{ 5 } })
    {
        const std::string out = truncate_middle(stars, budget);
        ASSERT_TRUE(out.size() <= budget);
        ASSERT_TRUE(is_valid_utf8(out));
    }
    // Budget 3 holds one star and no marker; budget 4 has room only for the marker.
    ASSERT_TRUE(truncate_middle(stars, 3) == STAR);
    ASSERT_TRUE(truncate_middle(stars, 4) == "...");
    ASSERT_TRUE(truncate_middle(stars, 0).empty());
}
