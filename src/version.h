#pragma once

// Single source of truth for the program identity (printed by --version/-V).
inline constexpr const char *RASTERMINAL_VERSION = "0.1.0";
// UTF-8 bytes for "Pavol Uličný" (č = U+010D, ý = U+00FD) written as escapes so the
// literal stays pure ASCII — MSVC builds without /utf-8 would otherwise re-encode raw
// UTF-8 source bytes per the active code page (mojibake) or fail under /W4 /WX (C4819).
inline constexpr const char *RASTERMINAL_AUTHOR = "Pavol Uli\xc4\x8dn\xc3\xbd";
inline constexpr const char *RASTERMINAL_COPYRIGHT_YEAR = "2026";
