#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// UTF-8 measurement, sanitizing, and fitting for fixed-width terminal text.

namespace text_detail
{

    struct Decoded
    {
        int32_t cp; // code point, or -1 when the bytes at this position are not valid UTF-8
        size_t len; // bytes consumed; always 1 on invalid input so a caller resynchronises
    };

    // Strictly decode one code point; invalid POSIX filename bytes consume one byte.
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

    // Approximate terminal width. Ambiguous cases round up because over-counting only
    // wastes padding, while under-counting clips the line's trailing fields.
    inline int cp_width(int32_t cp)
    {
        // Nonspacing Mn/Me ranges only. VS16 is zero here and handled with its base below.
        static constexpr CpRange ZERO[] = {
            { 0x0300, 0x036F }, // combining diacriticals
            { 0x0483, 0x0489 }, { 0x0591, 0x05BD }, { 0x05BF, 0x05BF }, { 0x05C1, 0x05C2 }, { 0x05C4, 0x05C5 },
            { 0x05C7, 0x05C7 }, { 0x0610, 0x061A }, { 0x064B, 0x065F }, { 0x0670, 0x0670 }, { 0x06D6, 0x06DC },
            { 0x0900, 0x0902 }, { 0x093A, 0x093A }, { 0x093C, 0x093C }, { 0x0941, 0x0948 }, { 0x094D, 0x094D },
            { 0x0951, 0x0957 }, { 0x0962, 0x0963 }, { 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A }, { 0x0E47, 0x0E4E },
            { 0x1AB0, 0x1AFF }, { 0x1DC0, 0x1DFF }, { 0x20D0, 0x20F0 }, { 0x302A, 0x302D }, { 0x3099, 0x309A },
            { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F },
        };
        // Coarse BMP wide/fullwidth ranges; neutral symbols round up where terminals vary.
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
        // ASCII lies below both sorted tables and cannot match. Return early instead
        // of scanning every ordinary name; keep the tables sorted when adding ranges.
        if (cp < ZERO[0].lo)
        {
            return 1;
        }
        if (in_ranges(cp, ZERO, sizeof(ZERO) / sizeof(ZERO[0])))
        {
            return 0;
        }
        // Supplementary-plane characters round up to two columns.
        if (cp >= 0x10000)
        {
            return 2;
        }
        return in_ranges(cp, WIDE, sizeof(WIDE) / sizeof(WIDE[0])) ? 2 : 1;
    }

} // namespace text_detail

// Rendered terminal width. VS16 promotes its preceding base to two columns;
// regional indicators remain conservatively unclustered.
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
            // Charge only the width needed to promote the preceding base to two columns.
            cw = 2 - prev_w;
        }
        w += static_cast<size_t>(cw);
        prev_w = cw;
        i += d.len;
    }
    return w;
}

// Replace controls, invalid UTF-8, and invisible formatting with one '?' per input byte.
inline std::string sanitize_controls(std::string_view text)
{
    // Unicode 15.1 Cf plus line and paragraph separators.
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

// Replace the middle with "..." within a byte budget, preserving the filename suffix.
// Split only at code-point boundaries; grapheme clustering would require Unicode tables.
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
