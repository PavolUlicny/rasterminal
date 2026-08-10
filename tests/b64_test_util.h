#pragma once

#include "tests/test.h"

#include <string>
#include <vector>

// Independent base64 decoder so kitty.cpp's encoder is not checking itself.
// Shared by test_kitty.cpp and the framebuffer kitty-present tests; '=' padding
// is skipped, any other non-alphabet byte fails the calling test.
inline std::vector<unsigned char> b64_decode(const std::string &s)
{
    auto val = [](char c) -> int
    {
        if (c >= 'A' && c <= 'Z')
        {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z')
        {
            return (c - 'a') + 26;
        }
        if (c >= '0' && c <= '9')
        {
            return (c - '0') + 52;
        }
        if (c == '+')
        {
            return 62;
        }
        if (c == '/')
        {
            return 63;
        }
        return -1; // '=' padding or an invalid byte; both end the data
    };
    std::vector<unsigned char> out;
    unsigned int acc = 0; // unsigned: the signed shift overflows (UB) once 26+ bits accumulate
    int bits = 0;
    for (const char c : s)
    {
        const int v = val(c);
        if (v < 0)
        {
            if (c == '=')
            {
                continue;
            }
            ASSERT_FAIL("invalid base64 byte in payload");
        }
        acc = (acc << 6u) | static_cast<unsigned int>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}
