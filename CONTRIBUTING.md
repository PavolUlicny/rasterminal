# Contributing to rasterminal

Thanks for your interest in contributing. rasterminal is pre-1.0 and under active development, so before starting anything larger than a small fix, please [open an issue](https://github.com/PavolUlicny/rasterminal/issues/new/choose) to discuss it first. That avoids work on something that is already planned, already declined, or better solved another way. For design or approach discussion, use the feature request form: it is the intended channel for anything that is not a bug report.

## Bugs and feature requests

Use the [issue forms](https://github.com/PavolUlicny/rasterminal/issues/new/choose). The bug form asks for your OS, terminal emulator, and version because terminal rendering bugs rarely reproduce without those details.

## Building and testing

```sh
make            # build
make test       # build and run the test suite
```

Or with CMake: `cmake --preset release && cmake --build --preset release && ctest --preset release`.

The project is C++17 with no external dependencies (everything third-party is vendored under `vendor/`). You need a C++17 compiler (GCC, Clang, and MSVC are all supported) plus a C compiler for the vendored C sources (the zstd and miniz amalgams and the libwebp decode subset; any system `cc`, selected by the Makefile's `CC` variable), and the preset commands need CMake 3.21 or newer (plain `cmake -B build` works on older versions). See the [Build section of the README](README.md#build) for the full set of build variants and platforms.

### Adding a test file

Test sources are listed by hand in both build systems, with no globbing: add your new file to `TEST_SRCS` in the `Makefile` and to the `rasterminal_tests` source list in `CMakeLists.txt`, or it silently won't be built by the one you skipped. Includes in tests are root-relative (`#include "src/foo.h"`, `#include "tests/foo.h"`), never `../`-relative, because the repo root is on the test include path.

## Code style

Formatting is enforced by CI with `clang-format --dry-run --Werror` over `src/` and `tests/`, so run `clang-format -i` on every C++ file you touch in those two trees (the repo's `.clang-format` supplies the rules; `vendor/` is never formatted). CI also runs clang-tidy, cppcheck, sanitizers, and 32-bit builds on every pull request, and all jobs must pass.

## Pull requests

Fork the repo, create a branch, and open the pull request against `main`. All CI jobs must be green before merge; there are no other process requirements.

## Commits and changelog

Commit messages are one line in conventional-commit form: `type(scope): message` (for example `fix(stl): reject truncated binary headers`). User-facing changes also need an entry in `CHANGELOG.md` under `## [Unreleased]`, following the existing Keep a Changelog format.

## Vendored libraries

Never edit anything under `vendor/` by hand. Those libraries are refreshed from upstream; see `vendor/README.md` for the version and source of each.

## License

By contributing, you agree that your contributions are licensed under the project's [MIT license](LICENSE).
