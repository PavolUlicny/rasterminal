#pragma once

#include <cstddef>
#include <string>

// UTF-8-aware text fitting for fixed-width output (the HUD status line). Its own header rather
// than framebuffer's: framebuffer renders the HUD string, but nothing in it needs to measure or
// cut one, and framebuffer.h reaches the rasterizer and renderer through their headers.

// Shorten text to at most max_bytes bytes by replacing its middle with "...", so a long field
// cannot push the fields after it past the terminal edge (the HUD is drawn with auto-wrap off,
// so an overlong line is clipped rather than wrapped). The middle is cut rather than the tail so
// that a filename keeps its extension.
//
// The budget is in bytes, not columns, because in UTF-8 a byte count is always an upper bound on
// the rendered width: every double-width code point encodes to at least 3 bytes (the East Asian
// Wide and Fullwidth ranges all start at U+1100), and no 1- or 2-byte code point is wider than
// one column. That keeps the fit safe with no wcwidth dependency, at the cost of a non-ASCII
// string using fewer visible characters than its budget would allow.
//
// Splits land on a code point boundary but NOT a grapheme cluster boundary, so a cut between a
// base character and its combining marks separates them, and a leading orphan mark on the tail
// renders against the ellipsis's last dot. Accepted, not fixed: cluster boundaries need Unicode
// tables nothing else here wants, the output stays well-formed UTF-8 either way (only the
// rendering is odd), and it takes a filename with combining marks landing on one of two exact
// byte offsets.
inline std::string truncate_middle(const std::string &text, size_t max_bytes)
{
    // ASCII, so the marker's own width is fixed and independent of the terminal's encoding.
    constexpr char ELLIPSIS[] = "...";
    constexpr size_t ELLIPSIS_LEN = sizeof(ELLIPSIS) - 1;

    // A UTF-8 continuation byte (0b10xxxxxx) is never the start of a code point.
    const auto is_continuation = [&text](size_t i) { return (static_cast<unsigned char>(text[i]) & 0xC0u) == 0x80u; };

    if (text.size() <= max_bytes)
    {
        return text;
    }
    // No room for the marker plus content. Degenerate (no caller asks for this), but the split
    // arithmetic below would underflow.
    if (max_bytes <= ELLIPSIS_LEN)
    {
        size_t head = max_bytes;
        while (head > 0 && is_continuation(head))
        {
            --head;
        }
        return text.substr(0, head);
    }

    const size_t keep = max_bytes - ELLIPSIS_LEN;
    // The head takes the odd byte: a filename's start is usually what distinguishes it, while the
    // tail is kept mainly for the extension.
    size_t head = (keep + 1) / 2;
    size_t tail = text.size() - (keep - head);
    // Both walks shrink the result, never grow it, so the byte budget still holds afterwards.
    while (head > 0 && is_continuation(head))
    {
        --head;
    }
    while (tail < text.size() && is_continuation(tail))
    {
        ++tail;
    }
    return text.substr(0, head) + ELLIPSIS + text.substr(tail);
}
