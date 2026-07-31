#include "tests/test.h"
#include "src/text.h"

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
