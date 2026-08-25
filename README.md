# rasterminal

[![CI](https://github.com/PavolUlicny/rasterminal/actions/workflows/ci.yml/badge.svg)](https://github.com/PavolUlicny/rasterminal/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/PavolUlicny/rasterminal?include_prereleases&sort=semver)](https://github.com/PavolUlicny/rasterminal/releases)
[![License: MIT](https://img.shields.io/github/license/PavolUlicny/rasterminal?color=blue)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
![Platforms](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)
![Dependencies](https://img.shields.io/badge/dependencies-none-brightgreen)
[![Last commit](https://img.shields.io/github/last-commit/PavolUlicny/rasterminal)](https://github.com/PavolUlicny/rasterminal/commits/main)

**A fast 3D model viewer for the terminal.**

![rasterminal spinning a model](assets/demo.gif)

rasterminal renders 3D models on the CPU and draws them in real time using the kitty graphics protocol, sixel, or Unicode half-blocks. Native loaders handle OBJ, PLY, STL and glTF. Assimp handles more than 40 other formats. It needs no display server or GPU and works over SSH.

## Quick start

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

Drag with the mouse to orbit, scroll to zoom, press `Space` to spin, `1` to `3` to switch shading modes, `Q` to quit.

More models are available in the [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) repository.

## Gallery

| Wireframe | Flat | Phong |
| --- | --- | --- |
| ![wireframe shading](assets/shading-wireframe.png) | ![flat shading](assets/shading-flat.png) | ![phong shading](assets/shading-phong.png) |

## How it works

rasterminal implements the graphics pipeline in C++ on the CPU: transforms, near-plane clipping, perspective-correct rasterization, depth testing, backface culling, texture sampling, baked ambient occlusion and Blinn-Phong lighting. Transparent fragments use an exact per-pixel A-buffer, so intersecting and double-sided transparent surfaces composite correctly. A worker pool shares rendering and frame encoding across CPU cores.

At startup, rasterminal queries the terminal and chooses the best available output:

| Backend | Output | Notes |
| --- | --- | --- |
| Kitty | Native-resolution 24-bit image | Uses shared memory locally and compressed inline data over SSH |
| Sixel | Native-resolution 240-color image | Supported by terminals such as foot, mlterm, xterm with sixel enabled, and Windows Terminal 1.22 or later |
| Half-blocks | Two vertical pixels per cell | Works in terminals with UTF-8 and ANSI color |

`--graphics` overrides automatic selection. Kitty and sixel are unavailable under tmux and GNU screen because those multiplexers do not pass the required protocols through. Busy sixel frames can also exceed xterm's default `maxStringParse` limit. Start xterm with `xterm -xrm '*maxStringParse: 10000000'` if frames disappear.

## Prebuilt binaries

Each [release](https://github.com/PavolUlicny/rasterminal/releases) includes portable binaries for Linux, macOS and Windows. Linux statically links libstdc++ and libgcc; Windows statically links the C runtime.

Download the archive for your platform, then:

```sh
# Linux / macOS
tar xzf rasterminal-<version>-<platform>.tar.gz
cd rasterminal-<version>-<platform>
chmod +x rasterminal
./rasterminal <model>
```

On Windows, extract the `.zip` and run `rasterminal.exe` in Windows Terminal or another terminal with UTF-8 and ANSI escape support. Legacy `cmd.exe` is unsupported. Each release also includes SHA-256 checksums for its archives.

## Build

CMake 3.22 or newer is required. GCC, Clang, AppleClang and MSVC are supported. The presets cover the common configurations:

```sh
cmake --preset release      # portable | dist | debug | reldbg | clang
cmake --build --preset release
ctest --preset release
```

`release` tunes the binary for the build machine. `portable` omits `-march=native` or `/arch:AVX2`. `dist` also links libstdc++ and libgcc statically on Linux, but glibc remains dynamic, so build release artifacts on the oldest Linux distribution you support.

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

# MSVC (Developer PowerShell or cmd with vcvars)
cmake -B build-msvc
cmake --build build-msvc --config Release -j
ctest --test-dir build-msvc -C Release --output-on-failure
```

`RASTERMINAL_PORTABLE`, `RASTERMINAL_STATIC_LIBSTDCXX` and `RASTERMINAL_BUILD_TESTS` expose the same choices for custom builds. The test executable is excluded from the default build.

### Install

The install target writes the binary, man page, license and notices using the GNU directory layout. The default prefix is `/usr/local`.

```sh
cmake --build build -j
sudo cmake --install build
sudo cmake --build build --target uninstall
```

```sh
cmake --install build --prefix ~/.local
DESTDIR=/tmp/pkg cmake --install build
```

`uninstall` removes the files listed in that build directory's `install_manifest.txt`. Pass the same `DESTDIR` used during installation.

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
| `--yaw` | none | `0` | Initial yaw in degrees `[-180, 180]`; positive turns the model left |
| `--pitch` | none | `-17.2` | Initial pitch in degrees `[-180, 180]`; negative looks down from above |
| `--zoom` | none | `1` | Initial apparent-size multiplier `[0.2, 100]`; `2` appears twice as large |
| `--cull` / `--no-cull` | none | `on` | Backface culling initial state |
| `--texture` / `--no-texture` | none | `on` | Texture rendering initial state |
| `--spin` / `--no-spin` | `-S` | `off` | Auto-rotation initial state |
| `--threads [N]` | `-j [N]` | per backend | Worker threads; all cores for loading and pixel backends, at most four for half-blocks; bare `-j` uses all cores |
| `--fps [N]` | `-f [N]` | `30` | Frame cap; bare `-f` uncaps |
| `--smooth-angle` | none | `60` | Crease angle `[0, 180]` for computed normals; ignored for OBJ smoothing groups |
| `--color` | none | `auto` | `truecolor`/`24bit`, `256`, `auto` |
| `--graphics` | none | `auto` | `kitty`, `sixel`, `blocks`, `auto` |
| `--spin-speed` | none | `45` | Auto-rotation speed in degrees per second (positive number); applies whenever spinning is active |
| `--spin-direction` | none | `left` | `left`, `right`; the direction the model's front face moves |
| `--bench [N]` | `-B [N]` | `200` | Headless benchmark over N frames; prints a startup/runtime report to stderr and exits |
| `--bench-size` | none | `200x120` | Bench framebuffer size in pixels (`WxH`); requires `--bench` |
| `--bench-warmup` | none | `20` | Warmup frames discarded before measurement; requires `--bench` |
| `--ao` / `--no-ao` | none | `on` | Baked ambient occlusion |
| `--hud` / `--no-hud` | none | `shown` | HUD status line |
| `--input` / `--no-input` | none | `on` | Keyboard and mouse controls; `Q` and Ctrl+C always quit |
| `--first-person` / `--no-first-person` | none | `off` | Free-flying camera with no gravity or collision |
| `--first-person-speed` | none | `1` | Initial speed multiplier `[0.05, 20]`; requires `--first-person` |
| `--help` | `-h` | none | Print usage and exit |
| `--version` | `-V` | none | Print version and exit |

String values are case-insensitive. Value flags accept `--flag value`, `--flag=value`, `-f value` or `-fvalue`. Boolean flags take no value. When both forms of a paired flag appear, the later one wins.

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

`--no-input` ignores every binding except `Q` and Ctrl+C. The viewer keeps mouse tracking active, so dragging does not select terminal text.

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

| Format | Loader | Notes |
| --- | --- | --- |
| OBJ / MTL | Native | Triangles, quads, n-gons; diffuse (`map_Kd`), specular (`map_Ks`), and normal (`map_Bump` / `norm`) maps |
| PLY | Native | ASCII and binary (LE/BE); vertex and face colors |
| STL | Native | ASCII and binary; Z-up coordinates are remapped to Y-up; ASCII lines are limited to 64 KB |
| glTF 2.0 | Native | External and embedded (GLB); PBR materials, vertex colors, double-sided, second UV set (`TEXCOORD_1`); `KHR_draco_mesh_compression`, `EXT_meshopt_compression`/`KHR_meshopt_compression`, `KHR_texture_basisu` (KTX2), `EXT_texture_webp`, `KHR_materials_unlit`, `KHR_texture_transform` |
| AMF, 3DS, AC, ASE, Assbin, B3D, BVH, Collada, DXF, CSM, HMP, IrrMesh, IQM, IRR, LWO/LWS, MD2/MD3/MD5/MDC/MDL, NFF/NDO/OFF, Ogre, OpenGEX, MS3D, COB, Blender, IFC, XGL, FBX, Q3D/Q3BSP, RAW, SIB, SMD, Terragen, Unreal 3D, DirectX X, X3D, 3MF, MMD | Assimp fallback | Static geometry, scene transforms, common materials and textures |

Native extensions do not fall back to Assimp after a parse failure. The Assimp loader flattens scene transforms and instances. It does not import animation, skinning, cameras, lights or format-specific metadata. It uses the first texture for each supported material role, and caps `--smooth-angle 180` to Assimp's 175-degree limit.

## Requirements

Interactive mode requires UTF-8, ANSI color and terminal input on both stdin and stdout. Mouse reporting is needed for drag and wheel controls. `TERM=dumb` and Windows consoles without ANSI escape processing are rejected. `--bench`, `--help` and `--version` work with redirected streams.

Color detection uses `COLORTERM`, `TERM`, `TMUX` and `STY`; `--color` overrides it. GNU screen defaults to 256 colors. Pixel graphics are optional and selected by a startup query, with kitty preferred over sixel. Stock xterm needs sixel enabled, for example with `xterm -ti vt340`.

## Project status

This is a pre-1.0 project. CLI flags and controls may change between releases.

## Contributing

Use the [issue forms](https://github.com/PavolUlicny/rasterminal/issues/new/choose) for bugs and feature requests. See [CONTRIBUTING.md](CONTRIBUTING.md) before sending code. Open an issue before starting anything larger than a small fix.

## Third-party libraries

Vendored under `vendor/`; see `THIRD_PARTY_NOTICES` for full license texts.

| Library | Version | License | Use |
| --- | --- | --- | --- |
| [Assimp](https://github.com/assimp/assimp) | v6.0.5 | BSD-3-Clause plus bundled-component licenses | Import-only fallback for formats without a native loader |
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

rasterminal uses the MIT License. Vendored libraries retain their own licenses. See [`LICENSE`](LICENSE) and [`THIRD_PARTY_NOTICES`](THIRD_PARTY_NOTICES).
