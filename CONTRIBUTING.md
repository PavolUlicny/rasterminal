# Contributing to rasterminal

## Issues

Use the [issue forms](https://github.com/PavolUlicny/rasterminal/issues/new/choose). Include the
requested OS, terminal and version details with bug reports.

Before starting anything larger than a small fix, open a feature request to discuss the design or implementation.

## Building and testing

```sh
cmake -B build                            # configure
cmake --build build -j                    # build the viewer
cmake --build build --target check -j     # build and run the test suite
```

The default build omits the test binary. Building the `check` target compiles it and runs
the suite.

You need CMake 3.22 or newer, a C++17 compiler and a C compiler. GCC, Clang and MSVC
are supported. Third-party code is vendored under `vendor/`. See the README's
[build section](README.md#build) for other configurations and platforms.

### Adding a source or test file

`CMakeLists.txt` lists source files explicitly. Add a new `src/` file to `rasterminal`,
and to `rasterminal_tests` if the tests link it. Add test files only to
`rasterminal_tests`. Vendored C files belong in the shared `rasterminal_c` object library,
which uses separate flags and disables LTO.

Includes in `src/` and `tests/` are root-relative, such as
`#include "src/render/renderer.h"` and `#include "tests/foo.h"`. Do not use `../` paths or
bare neighboring filenames. Put test sources under `tests/<subsystem>/`; shared helpers
and fixtures stay at `tests/` root.

## Code style

Run `clang-format -i` on every C++ file you change under `src/` or `tests/`. Do not format
`vendor/`. CI also runs clang-tidy, cppcheck, sanitizers and 32-bit builds.

## Pull requests

Open pull requests against `main`. All CI jobs must pass before merge.

## Commits and changelog

Use a one-line conventional commit message, such as
`fix(stl): reject truncated binary headers`. Add user-visible changes to `CHANGELOG.md`
under `## [Unreleased]`.

## Vendored libraries

Do not edit `vendor/` by hand. Refresh libraries from upstream as described in `vendor/README.md`.

## License

By contributing, you agree that your contributions are licensed under the project's [MIT license](LICENSE).
