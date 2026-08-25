#pragma once

#include "tests/test.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

// Independent sixel decoder. Execute raster attributes, register definitions,
// repeats, motion, and data into a register plane while validating the wire grammar.

struct SixelRgb
{
    int r = -1;
    int g = -1;
    int b = -1;
};

struct SixelFrame
{
    int p1 = -1;
    int p2 = -1;
    int p3 = -1;
    int pan = 0;
    int pad = 0;
    int w = 0;
    int h = 0;
    std::array<SixelRgb, 256> palette{};
    std::array<bool, 256> defined{};
    // Register painted per pixel, -1 where no pass painted (P2=1 transparency).
    std::vector<int> plane;
};

inline SixelFrame sixel_decode(const std::string &s)
{
    SixelFrame f;
    size_t i = 0;
    const auto read_int = [&s, &i]() -> int
    {
        if (i >= s.size() || s[i] < '0' || s[i] > '9')
        {
            ASSERT_FAIL("expected a number in the sixel stream");
        }
        long v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        {
            v = (v * 10) + (s[i] - '0');
            ASSERT_TRUE(v <= 1000000000L);
            i++;
        }
        return static_cast<int>(v);
    };

    ASSERT_TRUE(s.size() > 4 && s[0] == '\033' && s[1] == 'P');
    i = 2;
    f.p1 = read_int();
    ASSERT_TRUE(i < s.size() && s[i] == ';');
    i++;
    f.p2 = read_int();
    ASSERT_TRUE(i < s.size() && s[i] == ';');
    i++;
    f.p3 = read_int();
    ASSERT_TRUE(i < s.size() && s[i] == 'q');
    i++;

    ASSERT_TRUE(i < s.size() && s[i] == '"'); // raster attributes are mandatory here
    i++;
    f.pan = read_int();
    for (int *dim : { &f.pad, &f.w, &f.h })
    {
        ASSERT_TRUE(i < s.size() && s[i] == ';');
        i++;
        *dim = read_int();
    }
    ASSERT_TRUE(f.w > 0 && f.h > 0);
    f.plane.assign(static_cast<size_t>(f.w) * static_cast<size_t>(f.h), -1);

    int reg = -1;
    int x = 0;
    int band = 0;
    const auto paint = [&](char ch, int count)
    {
        ASSERT_TRUE(ch >= 0x3F && ch <= 0x7E);
        const int bits = ch - 0x3F;
        for (int n = 0; n < count; n++)
        {
            ASSERT_TRUE(x < f.w); // painting past the declared width
            if (bits != 0)
            {
                ASSERT_TRUE(reg >= 0); // data before any register selection
                for (int dy = 0; dy < 6; dy++)
                {
                    if ((bits & (1 << dy)) != 0)
                    {
                        const int y = (band * 6) + dy;
                        ASSERT_TRUE(y < f.h); // painting past the declared height
                        f.plane[(static_cast<size_t>(y) * static_cast<size_t>(f.w)) + static_cast<size_t>(x)] = reg;
                    }
                }
            }
            x++;
        }
    };

    bool terminated = false;
    while (i < s.size())
    {
        const char c = s[i];
        if (c == '\033')
        {
            ASSERT_TRUE(i + 1 < s.size() && s[i + 1] == '\\');
            i += 2;
            terminated = true;
            break;
        }
        if (c == '#')
        {
            i++;
            const int n = read_int();
            ASSERT_TRUE(n >= 0 && n < 256);
            if (i < s.size() && s[i] == ';')
            {
                i++;
                const int model = read_int();
                ASSERT_EQ(model, 2); // RGB; the encoder never uses HLS
                SixelRgb c3;
                for (int *ch : { &c3.r, &c3.g, &c3.b })
                {
                    ASSERT_TRUE(i < s.size() && s[i] == ';');
                    i++;
                    *ch = read_int();
                    ASSERT_TRUE(*ch >= 0 && *ch <= 100);
                }
                f.palette[static_cast<size_t>(n)] = c3;
                f.defined[static_cast<size_t>(n)] = true;
            }
            else
            {
                reg = n;
            }
            continue;
        }
        if (c == '!')
        {
            i++;
            const int count = read_int();
            ASSERT_TRUE(count > 0 && i < s.size());
            paint(s[i], count);
            i++;
            continue;
        }
        if (c == '$')
        {
            x = 0;
            i++;
            continue;
        }
        if (c == '-')
        {
            x = 0;
            band++;
            i++;
            continue;
        }
        paint(c, 1);
        i++;
    }
    ASSERT_TRUE(terminated);
    ASSERT_EQ(i, s.size()); // nothing after ST
    return f;
}
