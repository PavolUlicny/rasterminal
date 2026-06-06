# rasterminal

[![CI](https://github.com/PavolUlicny/rasterminal/actions/workflows/ci.yml/badge.svg)](https://github.com/PavolUlicny/rasterminal/actions/workflows/ci.yml)

A software 3D rasterizer that renders entirely in your terminal. No GPU, no windowing system, no runtime dependencies.

![rasterminal spinning a model](assets/demo.gif)

Rasterminal reimplements the full GPU rasterization pipeline on the CPU — model/view/projection transforms, perspective-correct triangle rasterization, z-buffer depth testing, backface culling, Blinn-Phong lighting, and hard shadow maps — and paints the result with Unicode half-block characters (`▀`) and 24-bit ANSI colour. Each terminal cell carries two vertical pixels (one as the glyph's foreground, one as its background), so a plain text grid becomes a framebuffer.

## Contents

- [Quick start](#quick-start)
- [Gallery](#gallery)
- [How it works](#how-it-works)
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

Drag with the mouse to orbit, scroll to zoom, press `Space` to spin, `1`–`4` to switch shading modes, `Q` to quit.

Want something denser? The Stanford bunny is a good stress test:

```sh
curl -fsSL -o bunny.stl \
  https://raw.githubusercontent.com/reprap-io/reprapio_stanford_bunny/master/bunny.stl
./rasterminal bunny.stl
```

More test assets live in the [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) repository — any `.glb`, `.gltf`, `.obj`, `.ply`, or `.stl` file works.

## Gallery

| Wireframe | Flat |
| --- | --- |
| ![wireframe shading](assets/shading-wireframe.png) | ![flat shading](assets/shading-flat.png) |

| Gouraud | Phong |
| --- | --- |
| ![gouraud shading](assets/shading-gouraud.png) | ![phong shading](assets/shading-phong.png) |

## How it works

Every frame, for every triangle, rasterminal runs the same pipeline a GPU would — just on the CPU:

1. **Transform** — vertices go through model, view, and projection matrices into clip space.
2. **Cull** — a world-space backface test rejects roughly half the triangles before any projection work. Double-sided materials opt out and flip their normals instead.
3. **Clip** — triangles crossing the near plane are split so nothing renders behind the camera; the rest are conservatively frustum-rejected.
4. **Rasterize** — after the perspective divide, triangles are scan-converted with perspective-correct interpolation of colour, UVs, world position, and normals.
5. **Depth test** — a z-buffer keeps the nearest fragment per pixel.
6. **Shade** — Blinn-Phong lighting (flat per-face, Gouraud per-vertex, or Phong per-pixel), modulated by texture sampling, baked ambient occlusion, and a hard shadow map built from the key light.
7. **Resolve** — each pair of vertically stacked pixels becomes one terminal cell: a `▀` glyph whose foreground is the top pixel and background is the bottom, in 24-bit colour.

The work is spread across threads with a single-pass work-stealing loop: each worker claims a chunk of triangles and rasterizes it end-to-end, committing fragments through a per-pixel 64-bit atomic that packs depth and colour together — no separate depth pass, no band buffers, no inter-phase barrier.

## Build

Two build systems are provided. Each has a **release** variant (`-march=native`, fastest on the build machine) and a **portable** variant (no `-march=native`, runs on any CPU of the target architecture). All other speed flags (`-O3 -ffast-math -funroll-loops`, LTO, and so on) apply to both.

### Linux / macOS — Make (GCC or Clang)

```sh
make                     # release: -O3 -march=native + all speed flags
make CXX=clang++         # same with Clang
make portable            # distributable binary, no -march=native
make dist                # release artifact: portable + static libstdc++/libgcc (Linux)
make debug               # -O0 -g
make test                # build and run test suite
```

`dist` is the binary to ship: same codegen as `portable` (any CPU of the target arch) but it
statically links libstdc++/libgcc, so it does not fail on older distros with a missing
`GLIBCXX_…` symbol. (glibc itself stays dynamic, so the build host's glibc version is still the
floor — build on the oldest distro you need to support, or use a fully static toolchain.)

### All platforms — CMake

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

# Dist (portable + static libstdc++/libgcc — the release artifact)
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

## Usage

```sh
rasterminal [options] <model>
```

| Flag | Short | Default | Description |
| --- | --- | --- | --- |
| `--shading` | `-s` | `gouraud` | `wireframe`/`1`, `flat`/`2`, `gouraud`/`3`, `phong`/`4` |
| `--bg` | `-b` | `black` | `black`/`1`, `gray`/`2`, `white`/`3` |
| `--lighting` | `-l` | `dual` | `dual`/`1`, `single`/`2`, `flat`/`3` |
| `--wireframe-color` | `-w` | `white` | `white`, `red`, `green`, `yellow`, `cyan`, `magenta` |
| `--cull` | `-c` | `on` | `on`/`off` |
| `--texture` | `-t` | `on` | `on`/`off` |
| `--spin` | `-S` | off | Start with auto-rotation enabled |
| `--threads [N]` | `-j [N]` | `min(cores, 4)` | Worker threads; bare `-j` uses all cores |
| `--fps [N]` | `-f [N]` | `60` | Frame cap; bare `-f` uncaps |
| `--smooth-angle` | | `60` | Crease angle in degrees `[0, 180]` for computed normals; `0` = faceted, `180` = fully smooth (ignored when an OBJ authors smoothing groups) |
| `--bench [N]` | `-B [N]` | `200` | Headless benchmark over N frames; prints a startup/runtime report to stderr and exits |
| `--bench-size` | | `200x120` | Bench framebuffer size in pixels (`WxH`); requires `--bench` |
| `--bench-warmup` | | `20` | Warmup frames discarded before measurement; requires `--bench` |
| `--no-shadow` | | | Disable shadow map (faster startup on large meshes) |
| `--no-ao` | | | Disable baked ambient occlusion |
| `--no-hud` | | | Hide the status bar |
| `--help` | `-h` | | Print usage and exit |

String values are case-insensitive. Long flags accept `--flag value` or `--flag=value`; short flags accept `-f value` or `-fvalue`.

## Controls

| Key | Action |
| --- | --- |
| `W` `A` `S` `D` / arrow keys | Orbit camera |
| `+` / `-` | Zoom in / out |
| Left-button drag | Orbit camera |
| Scroll wheel | Zoom |
| `1` `2` `3` `4` | Wireframe / flat / Gouraud / Phong shading |
| `L` | Cycle lighting (dual → single → flat) |
| `B` | Cycle background (black → gray → white) |
| `C` | Cycle wireframe colour |
| `T` | Toggle texture rendering |
| `K` | Toggle backface culling |
| `Space` | Toggle auto-rotation |
| `R` | Reset all runtime state |
| `Q` / `Esc` | Quit |

## Supported formats

| Format | Notes |
| --- | --- |
| OBJ / MTL | Triangles, quads, n-gons; `map_Kd` / `map_Ks` textures |
| PLY | ASCII and binary (LE/BE); vertex and face colours |
| STL | ASCII and binary |
| glTF 2.0 | External and embedded (GLB); PBR materials, vertex colours, double-sided; `KHR_draco_mesh_compression`, `EXT_meshopt_compression`, `KHR_texture_basisu` (KTX2), `EXT_texture_webp`, `KHR_materials_unlit` |

## Requirements

Any terminal with:

- UTF-8 support
- 24-bit (truecolour) ANSI colour
- Mouse reporting (for drag-to-orbit and scroll-to-zoom)

Works well with: iTerm2, kitty, WezTerm, Windows Terminal, and most modern Linux terminals.

## Project status

**Pre-v1.0.0 — under active development.** Rasterminal works and is fun to play with today, but it is still maturing: expect some rough edges, and CLI flags or controls may change before v1.0.0. For a stable, polished experience, keep an eye out for the v1.0.0 release.

## Third-party libraries

Vendored under `vendor/` — see `THIRD_PARTY_NOTICES` for full licence texts.

| Library | Version | Licence | Use |
| --- | --- | --- | --- |
| [cgltf](https://github.com/jkuhlmann/cgltf) | 1.15 | MIT | glTF / GLB parsing |
| [stb_image](https://github.com/nothings/stb) | 2.30 | MIT / Unlicense | Image loading |
| [stl_reader](https://github.com/sreiter/stl_reader) | 2.0 | BSD-2 | STL parsing |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) | 2.0.0rc13 | MIT | OBJ / MTL parsing |
| [tinyply](https://github.com/ddiakopoulos/tinyply) | 3.0 | Public domain | PLY parsing |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | 1.1 | MIT | Vertex cache / overdraw / fetch optimisation |
| [draco](https://github.com/google/draco) | 1.5.7 | Apache-2.0 | Draco mesh decompression (`KHR_draco_mesh_compression`) |
| [basis_universal](https://github.com/BinomialLLC/basis_universal) | v2_1_0r | Apache-2.0 | KTX2 / Basis Universal texture transcoding (`KHR_texture_basisu`) |
| [zstd](https://github.com/facebook/zstd) | bundled w/ basis_universal | BSD-3 | Zstd decompression for KTX2 UASTC payloads |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) | 1.6.0 | BSD-3 + PATENTS | WebP texture decoding (`EXT_texture_webp`) |

## License

Rasterminal is released under the MIT License — see [`LICENSE`](LICENSE). Vendored third-party libraries retain their own licenses, reproduced in full in [`THIRD_PARTY_NOTICES`](THIRD_PARTY_NOTICES).
