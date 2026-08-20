#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// UTF-8-aware text measuring, sanitizing and fitting for fixed-width output (the HUD status
// line). Its own header rather than framebuffer's: framebuffer renders the HUD string, but
// nothing in it needs to measure or cut one, and framebuffer.h reaches the rasterizer and
// renderer through their headers.

namespace text_detail
{

    struct Decoded
    {
        int32_t cp; // code point, or -1 when the bytes at this position are not valid UTF-8
        size_t len; // bytes consumed; always 1 on invalid input so a caller resynchronises
    };

    // Decode one UTF-8 code point. Rejects the encodings a naive decoder accepts and a terminal
    // may then render as something else entirely: truncated sequences, overlong forms, surrogate
    // halves, and anything past U+10FFFF. A model path is an arbitrary byte string on POSIX, so
    // invalid input is expected here rather than exceptional.
    inline Decoded decode_utf8(const char *s, size_t n)
    {
        const auto b0 = static_cast<unsigned char>(s[0]);
        if (b0 < 0x80u)
        {
            return { static_cast<int32_t>(b0), 1 };
        }
        size_t len = 0;
        uint32_t cp = 0; // unsigned while the bits are being assembled; a shift into a sign bit
                         // would be the one arithmetic mistake this decoder cannot afford
        if ((b0 & 0xE0u) == 0xC0u)
        {
            len = 2;
            cp = b0 & 0x1Fu;
        }
        else if ((b0 & 0xF0u) == 0xE0u)
        {
            len = 3;
            cp = b0 & 0x0Fu;
        }
        else if ((b0 & 0xF8u) == 0xF0u)
        {
            len = 4;
            cp = b0 & 0x07u;
        }
        else
        {
            return { -1, 1 }; // continuation byte where a lead belongs, or a 5+ byte lead
        }
        if (len > n)
        {
            return { -1, 1 };
        }
        for (size_t k = 1; k < len; ++k)
        {
            const auto bk = static_cast<unsigned char>(s[k]);
            if ((bk & 0xC0u) != 0x80u)
            {
                return { -1, 1 };
            }
            cp = (cp << 6u) | (bk & 0x3Fu);
        }
        constexpr uint32_t MIN_FOR_LEN[5] = { 0u, 0u, 0x80u, 0x800u, 0x10000u };
        if (cp < MIN_FOR_LEN[len] || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu)
        {
            return { -1, 1 };
        }
        return { static_cast<int32_t>(cp), len };
    }

    struct CpRange
    {
        int32_t lo, hi;
    };

    inline bool in_ranges(int32_t cp, const CpRange *ranges, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
        {
            if (cp >= ranges[i].lo && cp <= ranges[i].hi)
            {
                return true;
            }
        }
        return false;
    }

    // Rendered columns for one code point: 0 for a nonspacing mark, 2 for a double-width glyph,
    // 1 otherwise. A short range list rather than a full Unicode width table, so the rule that
    // decides every doubtful case is which way the error is safe: the caller pads a line to
    // exactly the terminal width, so UNDER-counting runs the line past the right edge, where
    // the terminal clips whatever the layout protected (the fps reading), while over-counting
    // only leaves an invisible pad column unused. Every judgement call below therefore rounds
    // up, and anything not positively known to be zero-width is one.
    inline int cp_width(int32_t cp)
    {
        // Nonspacing (Unicode Mn/Me) only. The SPACING combining marks that share these blocks
        // (Mc: U+0903, U+093B, U+093E-0940, U+0949-094C, U+094E-094F in Devanagari, and their
        // analogues elsewhere) occupy a column of their own and are deliberately absent, so a
        // block range is never used where the block mixes the two classes; when in doubt leave
        // a mark out (one column costs pad, zero pushes the line off the edge). Two of these
        // ranges (U+302A-302D, U+3099-309A) sit INSIDE the coarse Wide ranges further down;
        // ZERO is consulted first, so they win, which is the point (U+302E-302F, the spacing
        // Hangul tone marks in the same block, are deliberately absent). U+FE0F is zero HERE
        // and handled with context in display_width: it promotes the base it follows to emoji
        // presentation (two columns), so its cost depends on what the base already measured,
        // and a context-free answer is wrong either way round (one double-counts a wide base,
        // zero under-counts a narrow one).
        static constexpr CpRange ZERO[] = {
            { 0x0300, 0x036F }, // combining diacriticals
            { 0x0483, 0x0489 }, { 0x0591, 0x05BD }, { 0x05BF, 0x05BF }, { 0x05C1, 0x05C2 }, { 0x05C4, 0x05C5 },
            { 0x05C7, 0x05C7 }, { 0x0610, 0x061A }, { 0x064B, 0x065F }, { 0x0670, 0x0670 }, { 0x06D6, 0x06DC },
            { 0x0900, 0x0902 }, { 0x093A, 0x093A }, { 0x093C, 0x093C }, { 0x0941, 0x0948 }, { 0x094D, 0x094D },
            { 0x0951, 0x0957 }, { 0x0962, 0x0963 }, { 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A }, { 0x0E47, 0x0E4E },
            { 0x1AB0, 0x1AFF }, { 0x1DC0, 0x1DFF }, { 0x20D0, 0x20F0 }, { 0x302A, 0x302D }, { 0x3099, 0x309A },
            { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F },
        };
        // East Asian Wide and Fullwidth within the BMP (above it, see the blanket rule below).
        // Several entries are deliberately coarser than the Wide code points they cover: the
        // symbol and emoji areas interleave Wide and Neutral so finely that an exact list is
        // both long and a standing invitation to omit one, and over-counting a Neutral symbol
        // spends a column of invisible pad while omitting a Wide one clips a field. The Hangul
        // Jamo Extended-B and Yijing hexagram ranges are Neutral by the letter of the standard
        // and drawn wide by many terminals, so they round up on the same principle.
        static constexpr CpRange WIDE[] = {
            { 0x1100, 0x115F },                                         // Hangul Jamo
            { 0x231A, 0x232A },                                         // watch/hourglass and the Wide angle brackets
            { 0x23E9, 0x23F3 },                                         // media and clock symbols
            { 0x25FD, 0x25FE }, { 0x2614, 0x2615 }, { 0x2648, 0x2653 }, // zodiac
            { 0x267F, 0x26FD },                                         // miscellaneous symbols
            { 0x2705, 0x27BF },                                         // dingbats
            { 0x2B1B, 0x2B55 }, { 0x2E80, 0x303E }, { 0x3041, 0x33FF },
            { 0x3400, 0x4DBF }, { 0x4DC0, 0x4DFF }, // Yijing hexagrams
            { 0x4E00, 0x9FFF }, { 0xA000, 0xA4CF }, { 0xA960, 0xA97F },
            { 0xAC00, 0xD7A3 }, { 0xD7B0, 0xD7FF }, // Hangul Jamo Extended-B
            { 0xF900, 0xFAFF }, { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE6F },
            { 0xFF00, 0xFF60 }, { 0xFFE0, 0xFFE6 },
        };
        // Below the first table entry nothing is special, and that is every ASCII character:
        // without this the scans below run their full length on every ordinary name and cannot
        // match. Both tables are kept in ascending order and ZERO's first entry is the lowest of
        // either, so the bound is exact rather than a guess. Keep them sorted if a range is added.
        if (cp < ZERO[0].lo)
        {
            return 1;
        }
        if (in_ranges(cp, ZERO, sizeof(ZERO) / sizeof(ZERO[0])))
        {
            return 0;
        }
        // Everything above the BMP counts as two columns. What actually turns up in a filename
        // up there is emoji and the CJK extensions, all double-width and spread across blocks
        // that keep being added to (transport, regional indicators, geometric shapes, symbols
        // and pictographs extended-A); enumerating them invites exactly the omission this
        // replaces, and being wrong for the narrow outliers (mathematical and musical
        // alphanumerics) costs a column of pad rather than a clipped field.
        if (cp >= 0x10000)
        {
            return 2;
        }
        return in_ranges(cp, WIDE, sizeof(WIDE) / sizeof(WIDE[0])) ? 2 : 1;
    }

} // namespace text_detail

// Rendered width of a UTF-8 string in terminal columns; an invalid byte counts as one, which is
// what sanitize_controls leaves behind for it ('?'). Fixed-width layout measures with this
// rather than the byte count, a safe but loose upper bound (three bytes for a one-column glyph)
// that stops a padded bar visibly short of the right edge. This is the layer that knows about
// SEQUENCES; cp_width answers only in isolation. Exactly one sequence rule exists, for U+FE0F
// (at the branch below). A regional-indicator PAIR deliberately gets none: a clustering
// terminal draws one flag in two columns, a non-clustering one two boxed letters in four, both
// are common, and charging each indicator its full two columns is exact for the second and
// generous for the first, the direction this file rounds. A rule exact on only some terminals
// under-measures on the rest, and under-measuring clips the field the bar protects.
inline size_t display_width(std::string_view text)
{
    constexpr int32_t VS16 = 0xFE0F; // emoji presentation selector
    size_t w = 0;
    size_t i = 0;
    int prev_w = 0;
    while (i < text.size())
    {
        const text_detail::Decoded d = text_detail::decode_utf8(text.data() + i, text.size() - i);
        int cw = (d.cp < 0) ? 1 : text_detail::cp_width(d.cp);
        if (d.cp == VS16)
        {
            // Emoji presentation makes the PRECEDING character two columns wide, so this
            // selector costs whatever that character has not already been charged: nothing after
            // a base that measured wide, one column after a narrow one. Leading with no base at
            // all is malformed, and prev_w = 0 then rounds it up to two, the safe direction.
            cw = 2 - prev_w;
        }
        w += static_cast<size_t>(cw);
        prev_w = cw;
        i += d.len;
    }
    return w;
}

// Replace everything that could make a terminal do something other than draw a character with
// '?'. A model path is an arbitrary byte string on POSIX (need not be UTF-8 at all), so this
// covers more than the C0 controls: C1 (0x80-0x9F, escape-sequence introducers to a terminal
// not in UTF-8 mode, 0x9B being CSI), any byte sequence that is not valid UTF-8 (its rendering,
// and therefore its width, is the terminal's guess), and the bidi and zero-width formatting
// controls, which can silently reorder or hide the rest of the name. One '?' per replaced BYTE,
// so the result is exactly as long as the input: both truncate_middle's byte budget and the
// width bound the layout derives from it survive the pass.
inline std::string sanitize_controls(std::string_view text)
{
    // Unicode general category Cf in full (15.1), plus the line and paragraph separators. The
    // whole category rather than its memorable members, because the comment above and a test
    // both call this exhaustive and a from-memory list has already had to be extended once:
    // soft hyphen, the Arabic number and letter marks, the Syriac abbreviation mark, the Arabic
    // and Kashmiri marks, the Mongolian vowel separator, the zero-width space and joiners with
    // LRM and RLM, the separators, the bidi embeddings, overrides, isolates and the deprecated
    // formatting characters, the word joiner and invisible operators, the byte-order mark, the
    // interlinear annotation marks, the Kaithi and Brahmi number signs, the Egyptian and
    // shorthand format controls, the musical beams and slurs, and the tag characters (an
    // invisible alphabet in its own right).
    static constexpr text_detail::CpRange FORMAT[] = {
        { 0x00AD, 0x00AD },   { 0x0600, 0x0605 },   { 0x061C, 0x061C },   { 0x06DD, 0x06DD },   { 0x070F, 0x070F },
        { 0x0890, 0x0891 },   { 0x08E2, 0x08E2 },   { 0x180E, 0x180E },   { 0x200B, 0x200F },   { 0x2028, 0x2029 },
        { 0x202A, 0x202E },   { 0x2060, 0x2064 },   { 0x2066, 0x206F },   { 0xFEFF, 0xFEFF },   { 0xFFF9, 0xFFFB },
        { 0x1107F, 0x1107F }, { 0x110BD, 0x110BD }, { 0x110CD, 0x110CD }, { 0x13430, 0x1343F }, { 0x1BCA0, 0x1BCA3 },
        { 0x1D173, 0x1D17A }, { 0xE0000, 0xE007F },
    };
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size())
    {
        const text_detail::Decoded d = text_detail::decode_utf8(text.data() + i, text.size() - i);
        // An invalid encoding reports cp = -1, so the first test catches it alongside C0.
        const bool bad = d.cp < 0x20 || d.cp == 0x7F || (d.cp >= 0x80 && d.cp <= 0x9F) ||
                         text_detail::in_ranges(d.cp, FORMAT, sizeof(FORMAT) / sizeof(FORMAT[0]));
        if (bad)
        {
            out.append(d.len, '?');
        }
        else
        {
            out.append(text.substr(i, d.len));
        }
        i += d.len;
    }
    return out;
}

// Shorten text to at most max_bytes bytes by replacing its middle with "...", so a long field
// cannot push the fields after it past the terminal edge; the middle rather than the tail so a
// filename keeps its extension. The budget is in BYTES, not columns, and stays that way now
// that display_width() exists: bytes bound the storage as well as the width (no 1-byte code
// point exceeds one column and every 2-column code point costs at least 3 bytes, so byte count
// >= column count always), and a column-walked split would buy a slightly longer name only for
// a non-Latin-script name, where the extra characters are least likely to be what
// distinguishes it; the caller measures the RESULT with display_width, so a loose budget costs
// the layout nothing. Splits land on a code point boundary but NOT a grapheme cluster
// boundary, so a cut can separate a base from its combining marks and render an orphan mark
// against the ellipsis. Accepted, not fixed: cluster boundaries need Unicode tables nothing
// else here wants, the output stays well-formed UTF-8 either way, and it takes combining marks
// landing on one of two exact byte offsets.
inline std::string truncate_middle(std::string_view text, size_t max_bytes)
{
    // ASCII, so the marker's own width is fixed and independent of the terminal's encoding.
    constexpr char ELLIPSIS[] = "...";
    constexpr size_t ELLIPSIS_LEN = sizeof(ELLIPSIS) - 1;

    // A UTF-8 continuation byte (0b10xxxxxx) is never the start of a code point.
    const auto is_continuation = [&text](size_t i) { return (static_cast<unsigned char>(text[i]) & 0xC0u) == 0x80u; };

    if (text.size() <= max_bytes)
    {
        return std::string(text);
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
        return std::string(text.substr(0, head));
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
    std::string out;
    out.reserve(max_bytes);
    out += text.substr(0, head);
    out += ELLIPSIS;
    out += text.substr(tail);
    return out;
}
