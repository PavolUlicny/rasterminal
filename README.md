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

The finished framebuffer is then written to the terminal. Each character cell represents two vertically stacked pixels, drawn as a `▀` half-block glyph whose foreground color is the top pixel and background color is the bottom, both in 24-bit ANSI color. The whole frame is assembled in a single buffer and flushed in one write.

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
| `--shading` | `-s` | `phong` | `wireframe`/`1`, `flat`/`2`, `phong`/`3` |
| `--bg` | `-b` | `black` | `black`/`1`, `gray`/`2`, `white`/`3` |
| `--lighting` | `-l` | `dual` | `dual`/`1`, `single`/`2`, `flat`/`3` |
| `--wireframe-color` | `-w` | `white` | `white`, `red`, `green`, `yellow`, `cyan`, `magenta` |
| `--cull` | `-c` | `on` | `on`/`off` |
| `--texture` | `-t` | `on` | `on`/`off` |
| `--spin` | `-S` | `off` | Start with auto-rotation enabled |
| `--threads [N]` | `-j [N]` | `min(cores, 4)` | Worker threads; bare `-j` uses all cores |
| `--fps [N]` | `-f [N]` | `60` | Frame cap; bare `-f` uncaps |
| `--smooth-angle` | none | `60` | Crease angle in degrees `[0, 180]` for computed normals; `0` = faceted, `180` = fully smooth (ignored when an OBJ authors smoothing groups) |
| `--bench [N]` | `-B [N]` | `200` | Headless benchmark over N frames; prints a startup/runtime report to stderr and exits |
| `--bench-size` | none | `200x120` | Bench framebuffer size in pixels (`WxH`); requires `--bench` |
| `--bench-warmup` | none | `20` | Warmup frames discarded before measurement; requires `--bench` |
| `--no-ao` | none | `on` | Disable baked ambient occlusion |
| `--no-hud` | none | `shown` | Hide the status bar |
| `--help` | `-h` | none | Print usage and exit |
| `--version` | `-V` | none | Print version and exit |

String values are case-insensitive. Long flags accept `--flag value` or `--flag=value`; short flags accept `-f value` or `-fvalue`.

## Controls

| Key | Action |
| --- | --- |
| `W` `A` `S` `D` / arrow keys | Orbit camera |
| `+` / `-` | Zoom in / out |
| Left-button drag | Orbit camera |
| Scroll wheel | Zoom |
| `1` `2` `3` | Wireframe / flat / Phong shading |
| `L` | Cycle lighting (dual → single → flat) |
| `B` | Cycle background (black → gray → white) |
| `C` | Cycle wireframe color |
| `T` | Toggle texture rendering |
| `K` | Toggle backface culling |
| `Space` | Toggle auto-rotation |
| `R` | Reset all runtime state |
| `Q` / `Esc` | Quit |

## Supported formats

| Format | Notes |
| --- | --- |
| OBJ / MTL | Triangles, quads, n-gons; diffuse (`map_Kd`), specular (`map_Ks`), and normal (`map_Bump` / `norm`) maps |
| PLY | ASCII and binary (LE/BE); vertex and face colors |
| STL | ASCII and binary |
| glTF 2.0 | External and embedded (GLB); PBR materials, vertex colors, double-sided, second UV set (`TEXCOORD_1`); `KHR_draco_mesh_compression`, `EXT_meshopt_compression`, `KHR_texture_basisu` (KTX2), `EXT_texture_webp`, `KHR_materials_unlit`, `KHR_texture_transform` |

## Requirements

Any terminal with:

- UTF-8 support
- 24-bit (truecolor) ANSI color
- Mouse reporting (for drag-to-orbit and scroll-to-zoom)

Works well with: iTerm2, kitty, WezTerm, Windows Terminal, and most modern Linux terminals.

## Project status

Pre-1.0 and under active development. rasterminal works today, but it is still maturing: expect rough edges, and CLI flags or controls may change before 1.0.

## Third-party libraries

Vendored under `vendor/`; see `THIRD_PARTY_NOTICES` for full license texts.

| Library | Version | License | Use |
| --- | --- | --- | --- |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.15 | MIT | glTF / GLB parsing |
| [stb_image](https://github.com/nothings/stb) | 2.30 | MIT / Unlicense | Image loading |
| [stl_reader](https://github.com/sreiter/stl_reader) | 2.0 | BSD-2 | STL parsing |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) | 2.0.0rc13 | MIT | OBJ / MTL parsing |
| [tinyply](https://github.com/ddiakopoulos/tinyply) | 3.0 | Public domain | PLY parsing |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | 1.1 | MIT | Vertex cache / overdraw / fetch optimization |
| [draco](https://github.com/google/draco) | 1.5.7 | Apache-2.0 | Draco mesh decompression (`KHR_draco_mesh_compression`) |
| [basis_universal](https://github.com/BinomialLLC/basis_universal) | v2_1_0r | Apache-2.0 | KTX2 / Basis Universal texture transcoding (`KHR_texture_basisu`) |
| [zstd](https://github.com/facebook/zstd) | bundled w/ basis_universal | BSD-3 | Zstd decompression for KTX2 UASTC payloads |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) | 1.6.0 | BSD-3 + PATENTS | WebP texture decoding (`EXT_texture_webp`) |

## License

rasterminal is released under the MIT License; see [`LICENSE`](LICENSE). Vendored third-party libraries retain their own licenses, reproduced in full in [`THIRD_PARTY_NOTICES`](THIRD_PARTY_NOTICES).
