# rasterminal

[![CI](https://github.com/PavolUlicny/rasterminal/actions/workflows/ci.yml/badge.svg)](https://github.com/PavolUlicny/rasterminal/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/PavolUlicny/rasterminal?include_prereleases&sort=semver)](https://github.com/PavolUlicny/rasterminal/releases)
[![License: MIT](https://img.shields.io/github/license/PavolUlicny/rasterminal?color=blue)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
![Platforms](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)
![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen)
[![Last commit](https://img.shields.io/github/last-commit/PavolUlicny/rasterminal)](https://github.com/PavolUlicny/rasterminal/commits/main)

**A fast 3D model viewer in the terminal.**

![rasterminal spinning a model](assets/demo.gif)

rasterminal renders 3D models on the CPU and displays them in your terminal in real time, so you can view OBJ, PLY, STL, and glTF files anywhere you have a terminal, including over SSH with no display or GPU.

## Contents

- [Quick start](#quick-start)
- [Gallery](#gallery)
- [How it works](#how-it-works)
- [Prebuilt binaries](#prebuilt-binaries)
- [Build](#build)
- [Usage](#usage)
- [Controls](#controls)
- [Supported formats](#supported-formats)
- [Requirements](#requirements)
- [Project status](#project-status)
- [Contributing](#contributing)
- [Third-party libraries](#third-party-libraries)
- [License](#license)

## Quick start

```sh
# 1. Clone and build (release build, GCC or Clang)
git clone https://github.com/PavolUlicny/rasterminal.git
cd rasterminal
make

# 2. Grab a model to look at
curl -fsSL -o Duck.glb \
  https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Duck/glTF-Binary/Duck.glb

# 3. Render it
./rasterminal Duck.glb
```

Drag with the mouse to orbit, scroll to zoom, press `Space` to spin, `1` to `3` to switch shading modes, `Q` to quit.

For a denser model, try the Stanford bunny:

```sh
curl -fsSL -o bunny.stl \
  https://raw.githubusercontent.com/reprap-io/reprapio_stanford_bunny/master/bunny.stl
./rasterminal bunny.stl
```

More test assets live in the [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) repository; any `.glb`, `.gltf`, `.obj`, `.ply`, or `.stl` file works.

## Gallery

| Wireframe | Flat | Phong |
| --- | --- | --- |
| ![wireframe shading](assets/shading-wireframe.png) | ![flat shading](assets/shading-flat.png) | ![phong shading](assets/shading-phong.png) |

## How it works

rasterminal implements the same rasterization pipeline a GPU runs, entirely in CPU code. Each frame, every triangle is transformed from model space through the view and projection matrices into clip space. A world-space backface test rejects roughly half of all triangles before any projection work, with double-sided materials opting out of the test and flipping their normals instead. Triangles that cross the near plane are clipped so nothing renders behind the camera, and the remainder are conservatively rejected against the view frustum.

Surviving triangles go through the perspective divide and are scan-converted into fragments, with color, texture coordinates, world position, and normals all interpolated in a perspective-correct way across the triangle. A z-buffer keeps the nearest fragment at each pixel. Shading runs per fragment: flat shading evaluates Blinn-Phong lighting once per face, Phong shading evaluates it per pixel, and both are modulated by texture sampling and baked ambient occlusion.

Transparent surfaces (glTF `BLEND` materials, MTL `d`/`Tr`, or per-vertex alpha) take a separate path. Their fragments are gathered into a per-pixel list, sorted back to front, and composited over the finished opaque image, so the result is correct even where transparent geometry interpenetrates or is double-sided. Fully opaque models skip this path entirely.

The finished framebuffer is then written to the terminal. On terminals that implement the kitty graphics protocol (kitty, Ghostty, WezTerm), detected by a capability query at startup, the frame is transmitted as real pixels at the window's native resolution: locally through a shared-memory object, so only a tiny escape sequence crosses the terminal pipe per frame, and over ssh as zlib-compressed inline data. Everywhere else each character cell represents two vertically stacked pixels, drawn as a `▀` half-block glyph whose foreground color is the top pixel and background color is the bottom, both in 24-bit ANSI color (perceptually quantized to the xterm-256 palette on terminals without truecolor support). Either way the whole frame is assembled in a single buffer and flushed in one write; `--graphics` overrides the choice.

Rendering is multi-threaded with a work-stealing scheduler. Each worker claims a chunk of triangles and rasterizes it end to end, committing opaque fragments through a per-pixel 64-bit atomic that packs depth and color into one slot, with no separate depth pre-pass. Transparency adds two further work-stealing phases, accumulate then resolve, but only for models that actually use blended materials.

## Prebuilt binaries

Prebuilt binaries for Linux, macOS, and Windows are attached to each [release](https://github.com/PavolUlicny/rasterminal/releases). They use portable codegen (no `-march=native`, so they run on any CPU of the target architecture) and are self-contained: the Linux build statically links libstdc++/libgcc, and the Windows build statically links the C runtime, so neither needs extra runtime packages installed.

Download the archive for your platform, then:

```sh
# Linux / macOS
tar xzf rasterminal-<version>-<platform>.tar.gz
cd rasterminal-<version>-<platform>
chmod +x rasterminal
./rasterminal <model>
```

On Windows, extract the `.zip` and run `rasterminal.exe` from a terminal that supports ANSI escapes and UTF-8 (Windows Terminal is recommended; legacy `cmd.exe` is not supported).

Each release includes a `checksums.txt`; verify your download with `sha256sum -c checksums.txt` (Linux), `shasum -a 256 -c checksums.txt` (macOS), or `Get-FileHash` (Windows). `checksums.txt` lists all platforms, so check the line for the archive you downloaded; `-c` reports the other archives as missing, which is expected. To build from source instead, see [Build](#build).

## Build

Two build systems are provided. Each has a **release** variant (`-march=native`, fastest on the build machine) and a **portable** variant (no `-march=native`, runs on any CPU of the target architecture). All other speed flags (`-O3 -ffast-math -funroll-loops`, LTO, and so on) apply to both.

### Linux / macOS: Make (GCC or Clang)

```sh
make                     # release: -O3 -march=native + all speed flags
make CXX=clang++         # same with Clang
make portable            # distributable binary, no -march=native
make dist                # release artifact: portable + static libstdc++/libgcc (Linux)
make debug               # -O0 -g
make test                # build and run test suite
```

`dist` is the binary to ship: same portable codegen, but it statically links libstdc++/libgcc so it runs on older distros without a matching `GLIBCXX` symbol. glibc stays dynamic, so build on the oldest distro you need to support.

### All platforms: CMake

The configurations below are also available as presets (CMake ≥ 3.21):

```sh
cmake --preset release      # or: portable | dist | debug | reldbg | clang
cmake --build --preset release
ctest --preset release      # release and dist define test presets
```

Equivalent explicit invocations:

```sh
# Release
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Debug
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j

# Portable (runs on any x86-64, not just the build machine)
cmake -B build-portable -DCMAKE_BUILD_TYPE=Release -DRASTERMINAL_PORTABLE=ON
cmake --build build-portable -j

# Dist (portable + static libstdc++/libgcc, the release artifact)
cmake -B build-dist -DCMAKE_BUILD_TYPE=Release -DRASTERMINAL_PORTABLE=ON -DRASTERMINAL_STATIC_LIBSTDCXX=ON
cmake --build build-dist -j

# Tests
cmake --build build --target rasterminal_tests -j
ctest --test-dir build --output-on-failure

# Clang
cmake -B build -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# MSVC (Developer PowerShell or cmd with vcvars)
cmake -B build-msvc
cmake --build build-msvc --config Release -j
ctest --test-dir build-msvc -C Release --output-on-failure
```

### Install

Both build systems install the binary, the man page, and the license/notices following the GNU directory layout (defaulting to `/usr/local`). Build first, then install:

```sh
make                     # build whichever variant you want to ship (or: make dist)
sudo make install        # binary -> /usr/local/bin, man page -> .../share/man/man1, docs -> .../share/doc/rasterminal
sudo make uninstall      # remove everything install added

# CMake equivalent (after configuring/building a build dir):
sudo cmake --install build
```

Override the prefix to install without root, or stage into a fakeroot for packaging:

```sh
make install PREFIX=~/.local              # no sudo; ensure ~/.local/bin is on PATH
make install DESTDIR=/tmp/pkg PREFIX=/usr # staged install for packagers
cmake --install build --prefix ~/.local   # CMake prefix override
```

## Usage

```sh
rasterminal [options] <model>
```

| Flag | Short | Default | Description |
| --- | --- | --- | --- |
| `--shading` | `-s` | `phong` | `wireframe`, `flat`, `phong` |
| `--bg` | `-b` | `black` | `black`, `gray`, `white` |
| `--lighting` | `-l` | `dual` | `dual`, `single`, `flat` |
| `--wireframe-color` | `-w` | `white` | `white`, `red`, `green`, `yellow`, `cyan`, `magenta` |
| `--yaw` | none | `0` | Initial camera yaw in degrees `[-180, 180]`; positive turns the model left on screen |
| `--pitch` | none | `-17.2` | Initial camera pitch in degrees `[-180, 180]`; negative looks down from above; clamped to just inside straight up and straight down under `--first-person`, the same limit the mode holds while you look |
| `--zoom` | none | `1` | Initial zoom `[0.2, 100]` as a size multiplier of the auto-fit framing; `2` = twice as close; the bounds equal the range the scroll wheel reaches in orbit mode. Under `--first-person` it sets how far back you start, and the wheel sets movement speed instead |
| `--cull` / `--no-cull` | none | `on` | Backface culling initial state |
| `--texture` / `--no-texture` | none | `on` | Texture rendering initial state |
| `--spin` / `--no-spin` | `-S` | `off` | Auto-rotation initial state |
| `--threads [N]` | `-j [N]` | `min(cores, 4)` | Worker threads; bare `-j` uses all cores; `N` above the CPU thread count is clamped |
| `--fps [N]` | `-f [N]` | `60` | Frame cap; bare `-f` uncaps |
| `--smooth-angle` | none | `60` | Crease angle in degrees `[0, 180]` for computed normals; `0` = faceted, `180` = fully smooth (ignored when an OBJ authors smoothing groups) |
| `--color` | none | `auto` | `truecolor`/`24bit`, `256`, `auto`; with the kitty graphics backend the image is always 24-bit and this affects only the HUD line |
| `--graphics` | none | `auto` | `kitty`, `blocks`, `auto`: rendering backend; `auto` draws real pixels via the kitty graphics protocol where the terminal supports it, else unicode half-blocks |
| `--spin-speed` | none | `45` | Auto-rotation speed in degrees per second (positive number); applies whenever spinning is active |
| `--spin-direction` | none | `left` | `left`, `right`: the way the model's front face moves on screen; under `--first-person`, where nothing is being orbited, it is the way the view itself turns |
| `--bench [N]` | `-B [N]` | `200` | Headless benchmark over N frames; prints a startup/runtime report to stderr and exits |
| `--bench-size` | none | `200x120` | Bench framebuffer size in pixels (`WxH`); requires `--bench` |
| `--bench-warmup` | none | `20` | Warmup frames discarded before measurement; requires `--bench` |
| `--ao` / `--no-ao` | none | `on` | Baked ambient occlusion |
| `--hud` / `--no-hud` | none | `shown` | HUD status line |
| `--input` / `--no-input` | none | `on` | Keyboard and mouse controls; `--no-input` ignores every binding except `Q` (and Ctrl+C) |
| `--first-person` / `--no-first-person` | none | `off` | Free-flying first-person camera instead of the turntable (no gravity, collision or ground plane) |
| `--first-person-speed` | none | `1` | Initial movement speed `[0.05, 20]` as a multiplier of the model-scaled default; the bounds equal the range `+`/`-` and the wheel move within; requires `--first-person` |
| `--help` | `-h` | none | Print usage and exit |
| `--version` | `-V` | none | Print version and exit |

String values are case-insensitive. Long flags that take a value accept `--flag value` or `--flag=value`; short flags accept `-f value` or `-fvalue`. Boolean flags take no value: `--cull=on` is an error, not a synonym for `--cull`. The paired flags above (for example `--cull` and `--no-cull`) select the two states directly, and where both appear the later one on the command line wins.

## Controls

| Key | Action |
| --- | --- |
| `W` `A` `S` `D` / arrow keys | Orbit camera |
| `+` / `-` | Zoom in / out |
| Mouse drag | Orbit camera |
| Scroll wheel | Zoom |
| `1` `2` `3` | Wireframe / flat / Phong shading |
| `L` | Cycle lighting (dual → single → flat) |
| `B` | Cycle background (black → gray → white) |
| `C` | Cycle wireframe color |
| `T` | Toggle texture rendering |
| `K` | Toggle backface culling |
| `Space` | Toggle auto-rotation |
| `R` | Reset to the state set by the command-line flags (their defaults when not passed) |
| `Q` / `Ctrl+C` | Quit |

`--no-input` ignores every binding in this table except the last: `Q` and Ctrl+C still quit. The mouse stays claimed by the viewer, so a drag neither orbits nor selects text.

### First-person controls

`--first-person` swaps the turntable for a free-flying camera. There is no gravity, collision or ground plane: it flies, and passes through geometry. Only the camera bindings change: the shading, lighting, background, wireframe-color, texture and culling keys, and `R` and `Q`, all behave exactly as they do above. `Space` still starts and stops auto-rotation, but see below for what it does here.

| Key | Action |
| --- | --- |
| `W` `A` `S` `D` | Move forward / left / back / right, following where you are looking |
| `E` / `V` | Move up / down along world up, whatever the view is pitched to |
| Arrow keys | Look |
| Mouse drag | Look |
| `+` / `-`, scroll wheel | Movement speed |

The camera flies through geometry, and since the near plane is scaled to the model rather than to the mode, a surface can pass inside it and vanish just before you reach it. It cannot be flown off into empty space (the outer limit is the same distance the scroll wheel reaches in orbit mode), though it can be pointed away from the model; `R` returns to the launch view. At `--zoom 0.2` you start at that outer limit, so flying backwards does nothing until you move in.

Only one key moves you at a time, so there is no diagonal flight: you cannot hold forward and strafe together, or move and turn together with the keyboard. That is a limitation of terminal input rather than a choice, since a terminal sends no key releases and the operating system repeats only the most recently pressed key. Drag the mouse to look while you move.

Movement speed is scaled to the model, so the keys feel the same on a tiny model and a huge one, and `--first-person-speed` sets where it starts. Pitch stops at straight up and straight down, so the horizon never inverts, and the camera cannot fly further out than the scroll wheel can already zoom in orbit mode. `Space` still works and becomes a slow panorama from wherever the camera is standing, which is worth trying from inside a large scene. `R` returns to the launch view and speed.

## Supported formats

| Format | Notes |
| --- | --- |
| OBJ / MTL | Triangles, quads, n-gons; diffuse (`map_Kd`), specular (`map_Ks`), and normal (`map_Bump` / `norm`) maps |
| PLY | ASCII and binary (LE/BE); vertex and face colors |
| STL | ASCII and binary; the format's conventional Z-up orientation is remapped so models load upright. ASCII files are rejected if any single line exceeds 64 KB (a malformed-input guard; the format puts one short statement per line) |
| glTF 2.0 | External and embedded (GLB); PBR materials, vertex colors, double-sided, second UV set (`TEXCOORD_1`); `KHR_draco_mesh_compression`, `EXT_meshopt_compression`/`KHR_meshopt_compression`, `KHR_texture_basisu` (KTX2), `EXT_texture_webp`, `KHR_materials_unlit`, `KHR_texture_transform` |

## Requirements

Any terminal with:

- UTF-8 support
- ANSI color: 24-bit (truecolor) where supported, with an automatic 256-color fallback elsewhere, detected from `COLORTERM`/`TERM` and overridable with `--color` (a `dumb` terminal, or a Windows console that cannot enable ANSI escape processing, is rejected outright); kitty graphics frames are always 24-bit pixels, so there the mode affects only the HUD line
- Pixel-perfect rendering on terminals implementing the kitty graphics protocol, detected by a startup query and overridable with `--graphics`; not available under tmux or GNU screen (no passthrough support)
- Mouse reporting (for drag-to-orbit and scroll-to-zoom)

Interactive rendering also requires that both standard input and standard output be the terminal: if either is piped or redirected, rasterminal exits with an error instead of emitting escape sequences into the stream. The headless `--bench` mode is exempt.

Works well with: iTerm2, kitty, WezTerm, Windows Terminal, and most modern Linux terminals.

## Project status

Pre-1.0 and under active development. rasterminal works today, but it is still maturing: expect rough edges, and CLI flags or controls may change before 1.0.

## Contributing

Bug reports and feature requests are welcome through the [issue forms](https://github.com/PavolUlicny/rasterminal/issues/new/choose). Code contributions are welcome too: see [CONTRIBUTING.md](CONTRIBUTING.md) for how to build, test, and format your changes, and for anything larger than a small fix, please open an issue first to discuss it.

## Third-party libraries

Vendored under `vendor/`; see `THIRD_PARTY_NOTICES` for full license texts.

| Library | Version | License | Use |
| --- | --- | --- | --- |
| [cgltf](https://github.com/jkuhlmann/cgltf) | master (post-v1.15) | MIT | glTF / GLB parsing |
| [stb_image](https://github.com/nothings/stb) | v2.30 | MIT / Unlicense | Image loading |
| [stl_reader](https://github.com/sreiter/stl_reader) | v2.0 | BSD-2 | STL parsing |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) | v2.0.0rc13 | MIT | OBJ / MTL parsing |
| [tinyply](https://github.com/ddiakopoulos/tinyply) | 3.0 | Public domain | PLY parsing |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | v1.1 | MIT | Vertex cache / overdraw / fetch optimization |
| [draco](https://github.com/google/draco) | 1.5.7 | Apache-2.0 | Draco mesh decompression (`KHR_draco_mesh_compression`) |
| [basis_universal](https://github.com/BinomialLLC/basis_universal) | v2_1_0r | Apache-2.0 | KTX2 / Basis Universal texture transcoding (`KHR_texture_basisu`) |
| [zstd](https://github.com/facebook/zstd) | bundled w/ basis_universal v2_1_0r | BSD-3 | Zstd decompression for KTX2 UASTC payloads |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) | v1.6.0 | BSD-3 + PATENTS | WebP texture decoding (`EXT_texture_webp`) |
| [miniz](https://github.com/richgel999/miniz) | 3.1.2 | MIT | zlib deflate for the kitty graphics direct transport |

## License

rasterminal is released under the MIT License; see [`LICENSE`](LICENSE). Vendored third-party libraries retain their own licenses, reproduced in full in [`THIRD_PARTY_NOTICES`](THIRD_PARTY_NOTICES).
