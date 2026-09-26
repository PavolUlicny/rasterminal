# Contributing to rasterminal

## Issues

Use the [issue forms](https://github.com/PavolUlicny/rasterminal/issues/new/choose). Include the
requested OS, terminal and version details with bug reports.

Before starting anything larger than a small fix, open a feature request to discuss the design or implementation.

## Building and testing

```sh
cmake -B build                            # configure
cmake --build build -j 8                  # build the viewer
cmake --build build --target check -j 8   # build and run the test suite
```

Give `-j` a job count. Without one, the Makefiles generator starts an unlimited number of
compiler processes.

The default build omits the test binary. Building the `check` target compiles it and runs
the suite.

You need CMake 3.22 or newer, a C++17 compiler and a C compiler. GCC, Clang,
AppleClang and MSVC are supported. Third-party code is vendored under `vendor/`. See the README's
[build section](README.md#build) for other configurations and platforms.

### Code organization

`CMakeLists.txt` lists files explicitly. Add a `src/` file to `rasterminal`, and to
`rasterminal_tests` if the tests link it; test files go only in `rasterminal_tests`.
Vendored C files go in the `rasterminal_c` object library, which has its own flags and no LTO.

Includes in `src/` and `tests/` are root-relative, such as
`#include "src/render/renderer.h"` and `#include "tests/foo.h"`. Do not use `../` paths or
bare neighboring filenames. Put test sources under `tests/<subsystem>/`; shared helpers
and fixtures stay at `tests/` root.

Only `src/platform/` includes OS headers (`<windows.h>`, `<unistd.h>`, `<termios.h>`,
`<sys/...>`) or branches on the target OS. Other code calls the interfaces in
`src/platform/`. Compiler- and CPU-specific code, such as intrinsics, diagnostic
pragmas and inlining attributes, stays next to the code that uses it.

Headers use `.h`.

Keep implementation details near the code and user-facing behavior in the README,
man page and CLI help.

## Code style

Run `clang-format -i` on every C or C++ source and header you change under `src/`
or `tests/`. Do not format `vendor/`. CI also runs clang-tidy, cppcheck, sanitizers and 32-bit builds.

## Pull requests

Open pull requests against `main`. All CI jobs must pass before merge.

## Commits and changelog

Use a one-line conventional commit message, such as
`fix(stl): reject truncated binary headers`. Add user-visible changes to `CHANGELOG.md`
under `## [Unreleased]`.

## Vendored libraries

Do not edit vendored sources by hand. Refresh libraries from upstream using the
[vendor update instructions](vendor/README.md). Record version, source, configuration
and license changes in that file and in `THIRD_PARTY_NOTICES`.

## License

By contributing, you agree to license your contributions under the project's
[MIT license](LICENSE).
