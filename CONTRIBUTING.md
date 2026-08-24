# Contributing to rasterminal

Thanks for your interest in contributing. rasterminal is pre-1.0 and under active development, so before starting anything larger than a small fix, please [open an issue](https://github.com/PavolUlicny/rasterminal/issues/new/choose) to discuss it first. That avoids work on something that is already planned, already declined, or better solved another way. For design or approach discussion, use the feature request form: it is the intended channel for anything that is not a bug report.

## Bugs and feature requests

Use the [issue forms](https://github.com/PavolUlicny/rasterminal/issues/new/choose). The bug form asks for your OS, terminal emulator, and version because terminal rendering bugs rarely reproduce without those details.

## Building and testing

```sh
cmake -B build                            # configure
cmake --build build -j                    # build the viewer
cmake --build build --target check -j     # build and run the test suite
```

The test binary is not part of the default build, so `cmake --build build` on its own gives you the viewer alone.

The project is C++17 with no external dependencies (everything third-party is vendored under `vendor/`). You need CMake 3.22 or newer, a C++17 compiler (GCC, Clang and MSVC are all supported), and a C compiler for the vendored C sources (the Assimp zlib copy, zstd and miniz amalgams, and the libwebp decode subset), which CMake picks up as `CMAKE_C_COMPILER`. See the [Build section of the README](README.md#build) for the full set of build variants and platforms.

### Adding a source or test file

Both source lists in `CMakeLists.txt` are written by hand, with no globbing. A new `src/` file goes in `add_executable(rasterminal ...)`, and in the `rasterminal_tests` list too if the tests link it; a new test file goes in the `rasterminal_tests` list only. Skip one and the file silently won't be built. A new vendored **C** file goes in the `rasterminal_c` object library instead, which both executables pick up: those sources are built with their own flags and without LTO, and must not be added to either executable directly.

Includes are root-relative throughout `src/` and `tests/` (`#include "src/render/renderer.h"`, `#include "tests/foo.h"`), never `../`-relative and never a bare neighbouring filename, because the repo root is on the include path. `src/` is grouped into `loaders/`, `render/`, `math/`, `terminal/` and `platform/`, with the entry point and CLI at its root; test sources nest under `tests/<subsystem>/` while shared helpers and fixtures stay at `tests/` root.

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
