# rasterminal

[![CI](https://github.com/PavolUlicny/rasterminal/actions/workflows/ci.yml/badge.svg)](https://github.com/PavolUlicny/rasterminal/actions/workflows/ci.yml)

> **Pre-v1.0.0 — under active development.** Rasterminal works and is fun to play with today,
> but it's still maturing: expect some rough edges, and CLI flags or controls may change before
> v1.0.0. For a stable, polished experience, keep an eye out for the v1.0.0 release.

A software 3D rasterizer that renders entirely in the terminal. No GPU, no windowing system — just Unicode half-block characters (▀) and 24-bit ANSI colour, one cell per two vertical pixels.

## Features

- **Shading modes** — wireframe, flat, Gouraud, and Phong (per-pixel)
- **Textures** — diffuse maps with perspective-correct UV interpolation and bilinear filtering
- **Lighting** — Blinn-Phong with dual (warm key + cool fill) or single light; baked ambient occlusion
- **Shadow maps** — hard shadows from the key light
- **Orbit camera** — yaw, pitch, zoom; left-button drag; scroll to zoom
- **File formats** — OBJ/MTL, PLY (ASCII + binary), STL (ASCII + binary), glTF 2.0, GLB
- **Vertex colours** — PLY and glTF COLOR_0; per-face colours on PLY and STL
- **Double-sided materials** — glTF `doubleSided` flag respected with correct back-face normals
- **Near-plane clipping** — no pop-in artefacts when the camera gets close
- **Multithreaded** — single-pass work-stealing pipeline; workers run geometry and rasterization end-to-end on each triangle chunk and commit fragments through a 64-bit atomic depth+colour slot (no intermediate band buffer, no inter-phase barrier). Thread count configurable.
- **Terminal resize** — framebuffer adapts each frame
- **Clean exit** — restores terminal state on quit, Ctrl+C, or SIGTERM

## Requirements

Any terminal with:

- UTF-8 support
- 24-bit (truecolour) ANSI colour
- Mouse reporting (for drag-to-orbit and scroll-to-zoom)

Works well with: iTerm2, kitty, WezTerm, Windows Terminal, most modern Linux terminals.

## Build

### Linux / macOS — Make (GCC or Clang)

```sh
make                     # release: -O3 -march=native + all speed flags
make CXX=clang++         # same with Clang
make portable            # distributable binary, no -march=native
make debug               # -O0 -g
make test                # build and run test suite
```

### All platforms — CMake

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

```bash
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
| glTF 2.0 | External and embedded (GLB); PBR materials, vertex colours, double-sided; `KHR_draco_mesh_compression` |

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
