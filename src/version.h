#pragma once

// Single source of truth for --version and release tags.
inline constexpr const char *RASTERMINAL_VERSION = "0.1.0-alpha.1";
// ASCII escapes prevent MSVC without /utf-8 from re-encoding the author's name.
inline constexpr const char *RASTERMINAL_AUTHOR = "Pavol Uli\xc4\x8dn\xc3\xbd";
inline constexpr const char *RASTERMINAL_COPYRIGHT_YEAR = "2026";
inline constexpr const char *RASTERMINAL_HOMEPAGE = "https://github.com/PavolUlicny/rasterminal";
