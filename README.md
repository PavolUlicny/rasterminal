# rasterminal

[![CI](https://github.com/PavolUlicny/rasterminal/actions/workflows/ci.yml/badge.svg)](https://github.com/PavolUlicny/rasterminal/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/PavolUlicny/rasterminal?include_prereleases&sort=semver)](https://github.com/PavolUlicny/rasterminal/releases)
[![License: MIT](https://img.shields.io/github/license/PavolUlicny/rasterminal?color=blue)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
![Platforms](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)
![Dependencies](https://img.shields.io/badge/dependencies-vendored-brightgreen)
[![Last commit](https://img.shields.io/github/last-commit/PavolUlicny/rasterminal)](https://github.com/PavolUlicny/rasterminal/commits/main)

**A 3D model viewer that renders entirely in your terminal.**

rasterminal renders on the CPU in real time, with no GPU or display server. It runs on Linux, macOS and Windows, and works over SSH. Native loaders handle OBJ, PLY, STL and glTF; Assimp handles more than 40 other formats.

![rasterminal spinning a model](assets/demo.gif)

[Quick start](#quick-start) · [Downloads](#prebuilt-binaries) · [Build](#build) · [Usage](#usage) · [Controls](#controls) · [Formats](#supported-formats) · [Terminal support](#requirements)

## Quick start

Download a [prebuilt binary](#prebuilt-binaries), or build from source with CMake 3.22 or newer and a C++17 toolchain. Third-party libraries are included in the repository.

```sh
# Clone and build
git clone https://github.com/PavolUlicny/rasterminal.git
cd rasterminal
cmake -B build
cmake --build build -j

# Download a model
curl -fsSL -o Duck.glb \
  https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Duck/glTF-Binary/Duck.glb

# Open it
./build/rasterminal Duck.glb
```

Drag to orbit and scroll to zoom. Press `Space` to spin, `1` to `3` to switch shading modes, and `Q` to quit.

More models are available in the [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) repository.

## Gallery

| Wireframe | Flat | Phong |
| --- | --- | --- |
| ![wireframe shading](assets/shading-wireframe.png) | ![flat shading](assets/shading-flat.png) | ![phong shading](assets/shading-phong.png) |

## How it works

The C++17 renderer handles transforms, near-plane clipping, perspective-correct rasterization, depth testing and backface culling. Texture sampling, baked ambient occlusion and Blinn-Phong lighting give models their color and shading.

An exact per-pixel A-buffer composites transparent fragments, including intersecting and double-sided surfaces. A persistent worker pool shares rendering and frame encoding across CPU cores.

At startup, rasterminal queries the terminal. It prefers kitty graphics, then sixel, then Unicode half-blocks:

| Backend | Output | Notes |
| --- | --- | --- |
| Kitty | Native-resolution 24-bit image | Uses shared memory locally and compressed inline data over SSH |
| Sixel | Native-resolution 240-color image | Supported by terminals such as foot, mlterm, xterm with sixel enabled, and Windows Terminal 1.22 or later |
| Half-blocks | Two vertical pixels per cell | Works in terminals with UTF-8 and ANSI color |

Use `--graphics` to choose a backend. Kitty and sixel are unavailable under tmux and GNU screen because rasterminal does not implement protocol pass-through for those multiplexers.

Busy sixel frames can exceed xterm's default `maxStringParse` limit. If frames disappear, start xterm with:

```sh
xterm -xrm '*maxStringParse: 10000000'
```

## Prebuilt binaries

Each [release](https://github.com/PavolUlicny/rasterminal/releases) includes portable binaries and SHA-256 checksums.

| Platform | Architecture | Archive |
| --- | --- | --- |
| Linux | x86_64 | `.tar.gz` |
| macOS | arm64 | `.tar.gz` |
| Windows | x86_64 | `.zip` |

Linux binaries statically link libstdc++ and libgcc. Windows binaries statically link the C runtime.

### Linux and macOS

Download the archive for your platform. Replace `<version>` with its release tag, including the `v` prefix, and `<platform>` with `linux-x86_64` or `macos-arm64`:

```sh
tar xzf rasterminal-<version>-<platform>.tar.gz
cd rasterminal-<version>-<platform>
chmod +x rasterminal
./rasterminal <model>
```

The macOS binary is not signed or notarized. If Gatekeeper blocks it, try to
open it once, then go to **System Settings → Privacy & Security** and click
**Open Anyway**. Verify the archive's SHA-256 checksum before overriding the
warning. See [Apple's instructions](https://support.apple.com/guide/mac-help/open-a-mac-app-from-an-unknown-developer-mh40616/mac).

### Windows

Extract the `.zip` and run `rasterminal.exe` in Windows Terminal or another terminal with UTF-8 and ANSI escape support. Legacy `cmd.exe` is unsupported.

## Build

Building requires CMake 3.22 or newer, a C++17 compiler and a C compiler. GCC, Clang, AppleClang and MSVC are supported. Use a preset to configure, build and test:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Each preset has matching configure, build and test commands.

| Preset | Configuration |
| --- | --- |
| `release` | Release build tuned for the build machine |
| `portable` | Release build without `-march=native` or `/arch:AVX2` |
| `dist` | Portable release with static libstdc++ and libgcc on Linux |
| `debug` | Debug build |
| `reldbg` | Optimized build with debug symbols |
| `clang` | Release build with Clang |

Linux builds still link glibc dynamically, including with `dist`. Build release artifacts on the oldest Linux distribution you support.

### Manual configuration

Without presets, configure the options directly:

```sh
# Machine-tuned release
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Portable release
cmake -B build-portable -DCMAKE_BUILD_TYPE=Release -DRASTERMINAL_PORTABLE=ON
cmake --build build-portable -j

# Build and run tests
cmake --build build --target check -j
```

For MSVC, use Developer PowerShell or a command prompt initialized with `vcvars`:

```powershell
cmake -B build-msvc
cmake --build build-msvc --config Release -j --target rasterminal rasterminal_tests
ctest --test-dir build-msvc -C Release --output-on-failure
```

`RASTERMINAL_PORTABLE`, `RASTERMINAL_STATIC_LIBSTDCXX` and `RASTERMINAL_BUILD_TESTS` expose the same choices for custom builds. The test executable is excluded from the default build.

### Install

The install target writes the binary, man page, README, license and notices using the GNU directory layout. The default prefix is `/usr/local`.

```sh
cmake --build build -j
sudo cmake --install build
sudo cmake --build build --target uninstall
```

For a user-local install or a staged package:

```sh
cmake --install build --prefix ~/.local
DESTDIR=/tmp/pkg cmake --install build
```

`uninstall` removes the files listed in that build directory's `install_manifest.txt`. Pass the same `DESTDIR` used during installation.

## Usage

```sh
rasterminal [options] <model>
```

String values are case-insensitive. Value flags accept `--flag value`, `--flag=value`, `-f value` or `-fvalue`. Boolean flags take no value. When both forms of a paired flag appear, the later one wins.

### Appearance

| Flag | Short | Default | Description |
| --- | --- | --- | --- |
| `--shading` | `-s` | `phong` | `wireframe`, `flat`, `phong` |
| `--bg` | `-b` | `black` | `black`, `gray`, `white` |
| `--lighting` | `-l` | `dual` | `dual`, `single`, `flat` |
| `--wireframe-color` | `-w` | `white` | `white`, `red`, `green`, `yellow`, `cyan`, `magenta` |
| `--cull` / `--no-cull` | none | `on` | Backface culling initial state |
| `--texture` / `--no-texture` | none | `on` | Texture rendering initial state |
| `--ao` / `--no-ao` | none | `on` | Baked ambient occlusion |
| `--smooth-angle` | none | `60` | Crease angle `[0, 180]` for computed normals; ignored for OBJ smoothing groups |

### Camera and motion

| Flag | Short | Default | Description |
| --- | --- | --- | --- |
| `--yaw` | none | `0` | Initial yaw in degrees `[-180, 180]`; positive turns the model left |
| `--pitch` | none | `-17.2` | Initial pitch in degrees `[-180, 180]`; negative looks down from above |
| `--zoom` | none | `1` | Initial apparent-size multiplier `[0.2, 100]`; `2` appears twice as large |
| `--spin` / `--no-spin` | `-S` | `off` | Auto-rotation initial state |
| `--spin-speed` | none | `45` | Positive auto-rotation speed in degrees per second; applies whenever spinning is active |
| `--spin-direction` | none | `left` | `left`, `right`; the direction the model's front face moves |
| `--first-person` / `--no-first-person` | none | `off` | Free-flying camera with no gravity or collision |
| `--first-person-speed` | none | `1` | Initial speed multiplier `[0.05, 20]`; requires `--first-person` |

### Output and interaction

| Flag | Short | Default | Description |
| --- | --- | --- | --- |
| `--graphics` | none | `auto` | `kitty`, `sixel`, `blocks`, `auto` |
| `--color` | none | `auto` | `truecolor`/`24bit`, `256`, `auto` |
| `--threads [N]` | `-j [N]` | hardware concurrency | Worker threads for loading and rendering; bare `-j` uses the default, and `N` is clamped to the CPU thread count |
| `--fps [N]` | `-f [N]` | `30` | Frame cap; bare `-f` uncaps |
| `--hud` / `--no-hud` | none | `shown` | HUD status line |
| `--input` / `--no-input` | none | `on` | Keyboard and mouse controls; `Q` and Ctrl+C always quit |

### Benchmark and information

| Flag | Short | Default | Description |
| --- | --- | --- | --- |
| `--bench [N]` | `-B [N]` | `200` | Headless benchmark over N frames; prints a startup/runtime report to stderr and exits |
| `--bench-size` | none | `200x120` | Bench framebuffer size in pixels as `WxH`; requires `--bench` |
| `--bench-warmup` | none | `20` | Warmup frames discarded before measurement; requires `--bench` |
| `--help` | `-h` | none | Print usage and exit |
| `--version` | `-V` | none | Print version and exit |

## Controls

| Key | Action |
| --- | --- |
| `W` `A` `S` `D` / arrow keys | Orbit camera |
| `+` / `-` | Zoom in / out |
| Mouse drag | Orbit camera |
| Scroll wheel | Zoom |
| `1` `2` `3` | Wireframe / flat / Phong shading |
| `L` | Cycle lighting: dual → single → flat |
| `B` | Cycle background: black → gray → white |
| `C` | Cycle wireframe color |
| `T` | Toggle texture rendering |
| `K` | Toggle backface culling |
| `Space` | Toggle auto-rotation |
| `R` | Reset to the launch state, including command-line settings and defaults |
| `Q` / `Ctrl+C` | Quit |

`--no-input` ignores the viewer's keyboard and mouse controls except `Q`. Ctrl+C still quits. The viewer keeps mouse tracking active, so dragging does not select terminal text.

On POSIX, Ctrl+Z restores the terminal and suspends the viewer, including with `--no-input`. Use `fg` to resume with a full redraw. A viewer resumed with `bg` stays idle until it returns to the foreground.

### First-person controls

`--first-person` replaces the turntable with a camera that can fly through geometry. Rendering controls, `R`, `Q` and `Space` keep their usual bindings.

| Key | Action |
| --- | --- |
| `W` `A` `S` `D` | Move forward / left / back / right |
| `E` / `V` | Move along world up / down |
| Arrow keys | Look |
| Mouse drag | Look |
| `+` / `-`, scroll wheel | Movement speed |

Distance is limited to the turntable's zoom range, and pitch stops at straight up or down. `R` restores the launch view and speed. Movement speed scales with model size.

Terminals do not report key releases, so only the most recently pressed movement key repeats. Use mouse drag to look while moving.

## Supported formats

### Native loaders

| Format | Support |
| --- | --- |
| OBJ / MTL | Triangles, quads and n-gons; diffuse `map_Kd`, specular `map_Ks`, and normal `map_Bump` / `norm` maps |
| PLY | ASCII and binary, little-endian and big-endian; vertex and face colors |
| STL | ASCII and binary; Z-up coordinates remapped to Y-up; ASCII lines limited to 64 KB |
| glTF 2.0 / GLB | External and embedded data; PBR materials, vertex colors, double-sided materials and a second UV set, `TEXCOORD_1` |

The glTF loader supports these extensions:

| Extension | Support |
| --- | --- |
| `KHR_draco_mesh_compression` | Draco mesh compression |
| `EXT_meshopt_compression`, `KHR_meshopt_compression` | meshopt compression |
| `KHR_texture_basisu` | KTX2 / Basis Universal textures |
| `EXT_texture_webp` | WebP textures |
| `KHR_materials_unlit` | Unlit materials |
| `KHR_texture_transform` | Texture transforms |

OBJ, PLY, STL and glTF/GLB always use their native loaders. A parsing failure reports an error without retrying through Assimp.

### Assimp formats

Assimp imports static geometry, scene transforms, common materials and textures for these formats:

- AMF, 3DS, AC, ASE, Assbin, B3D, Collada, DXF, HMP, IrrMesh, IQM, IRR
- LWO/LWS, MD2/MD3/MD5/MDC/MDL, NFF/NDO/OFF, Ogre, OpenGEX, MS3D, COB
- Blender, IFC, XGL, FBX, Q3D/Q3BSP, RAW, SIB, SMD, Terragen, Unreal 3D
- DirectX X, X3D, 3MF, MMD

The loader flattens scene transforms and instances. It uses the first texture for each supported material role and caps `--smooth-angle 180` to Assimp's 175-degree limit.

Animation, skinning, cameras, lights and format-specific metadata are not imported. BVH and CSM provide motion-capture data without model geometry and are unsupported.

## Requirements

Interactive mode requires UTF-8, ANSI color and terminal input on both stdin and stdout. Mouse reporting is needed for drag and wheel controls. `TERM=dumb` and Windows consoles without ANSI escape processing are rejected. `--bench`, `--help` and `--version` work with redirected streams.

Color detection uses `COLORTERM`, `TERM`, `TMUX` and `STY`; `--color` overrides it. GNU screen defaults to 256 colors. Pixel graphics are optional and selected by a startup query, with kitty preferred over sixel. Stock xterm needs sixel enabled, for example with `xterm -ti vt340`.

## Project status

This is a pre-1.0 project. CLI flags and controls may change between releases.

## Contributing

Use the [issue forms](https://github.com/PavolUlicny/rasterminal/issues/new/choose) for bugs and feature requests. See [CONTRIBUTING.md](CONTRIBUTING.md) before sending code. Open an issue before starting anything larger than a small fix.

## Third-party libraries

All third-party libraries are included under [`vendor/`](vendor/). See the [vendor README](vendor/README.md) for source revisions and update instructions, and [`THIRD_PARTY_NOTICES`](THIRD_PARTY_NOTICES) for full license texts.

| Library | Version | License | Use |
| --- | --- | --- | --- |
| [Assimp](https://github.com/assimp/assimp) | v6.0.5 | BSD-3-Clause plus bundled-component licenses | Import-only loader for formats without a native loader |
| [cgltf](https://github.com/jkuhlmann/cgltf) | master, post-v1.15 | MIT | glTF / GLB parsing |
| [stb_image](https://github.com/nothings/stb) | v2.30 | MIT / Unlicense | Image loading |
| [stl_reader](https://github.com/sreiter/stl_reader) | v2.0 | BSD-2 | STL parsing |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) | v2.0.0rc13 | MIT | OBJ / MTL parsing |
| [tinyply](https://github.com/ddiakopoulos/tinyply) | 3.0 | Public domain | PLY parsing |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | v1.1 | MIT | Vertex cache / overdraw / fetch optimization |
| [draco](https://github.com/google/draco) | 1.5.7 | Apache-2.0 | Draco mesh decompression for `KHR_draco_mesh_compression` |
| [basis_universal](https://github.com/BinomialLLC/basis_universal) | v2_1_0r | Apache-2.0 | KTX2 / Basis Universal texture transcoding for `KHR_texture_basisu` |
| [zstd](https://github.com/facebook/zstd) | bundled with basis_universal v2_1_0r | BSD-3 | Zstd decompression for KTX2 UASTC payloads |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) | v1.6.0 | BSD-3 + PATENTS | WebP texture decoding for `EXT_texture_webp` |
| [miniz](https://github.com/richgel999/miniz) | 3.1.2 | MIT | zlib deflate for the kitty graphics direct transport |

## License

rasterminal uses the MIT License. Vendored libraries retain their own licenses. See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES`](THIRD_PARTY_NOTICES).
