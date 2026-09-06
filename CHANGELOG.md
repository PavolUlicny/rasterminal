# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Native-resolution graphics through the kitty and sixel protocols. `--graphics kitty|sixel|blocks|auto` selects the backend; `auto` prefers kitty, then sixel, and falls back to half-blocks. Local kitty sessions use shared memory, remote sessions send compressed frames, and sixel uses its fixed 240-color palette. Capability detection prevents a forced pixel backend from failing silently. Pixel graphics are unavailable under tmux and GNU screen.
- Synchronized output on terminals that support it, so complete frames appear at once.
- Built-in support through Assimp 6.0.5 for more than 40 additional model formats. It imports static geometry, scene transforms, common materials and textures. OBJ, PLY, STL and glTF/GLB continue to use only their native loaders.
- A free-flying camera selected with `--first-person`. WASD moves, `E` and `V` move along world up, arrows and mouse drag look around, and `+`, `-` and the wheel adjust speed. `--first-person-speed` sets the initial speed. The camera has no gravity or collision and can pass through geometry.
- `--yaw`, `--pitch` and `--zoom` for the initial camera pose. They also set the benchmark camera and the state restored by `R`.
- Automatic 24-bit or 256-color output, with perceptual palette matching for 256-color terminals. `--color truecolor|256|auto` overrides detection. Kitty images remain 24-bit and sixel images use 240 colors, so the option affects only the HUD with those backends.
- `--spin-speed` and `--spin-direction left|right` for auto-rotation.
- `--input` and `--no-input`. The latter ignores every keyboard and mouse binding except `Q` and Ctrl+C while leaving resize handling and auto-rotation active.
- Positive and negative forms for every state flag, including `--spin`/`--no-spin`, `--ao`/`--no-ao` and `--hud`/`--no-hud`. When both forms appear, the later one wins.
- CMake install and uninstall targets, plus a `rasterminal(1)` man page.

### Changed

- Rendering is faster, especially when large triangles fill a high-resolution frame. On a 16-thread laptop, Phong rendering at 1920x1080 fell from 61 ms to 9 ms for Sponza and from 202 ms to 94 ms for a dense jungle scene. Alpha-blended scenes improved by two to eight times. Wireframe rendering is unchanged.
- glTF and GLB files with many mesh primitives load faster. A 645,000-triangle, 113-primitive model fell from 1.9 seconds to 0.6 seconds on a 16-thread laptop. Single-primitive models are unaffected.
- Model loading now uses every core by default. Half-block rendering still uses at most four. `--threads` overrides both defaults.
- Idle half-block sessions no longer redraw the model. Rendering resumes immediately after input, resize or auto-rotation.
- The default frame cap is now 30 fps instead of 60. Use `-f 60` to restore the previous cap.
- The HUD is now a full-width status bar that drops less useful fields as the terminal narrows. It measures display columns correctly and safely shortens or sanitizes problematic filenames.
- `Q` and Ctrl+C are now the only quit controls. Escape sequences are buffered and parsed as complete units, so unsupported keys and partial terminal replies no longer quit the viewer or trigger unrelated bindings.
- `R` restores the launch state selected by command-line flags, including camera pose, shading, lighting, background, wireframe color, spin, culling and texturing.
- `--cull` and `--texture` are now paired boolean flags with `--no-cull` and `--no-texture`. Their former values and the `-c` and `-t` aliases have been removed.
- Interactive mode now reports an error when stdin or stdout is not a terminal, `TERM=dumb`, or the Windows console cannot process ANSI escapes. `--bench`, `--help` and `--version` still work with redirected streams.
- Bright lit surfaces use a soft highlight rolloff instead of clipping to flat white. Midtones, shadows, unlit materials, wireframe, HUD and background are unchanged.
- Building now requires CMake 3.22 or newer.

### Removed

- The Makefile. CMake is now the only build system. The binary is written to the build directory, and the `portable`, `dist` and `debug` presets replace their former Make targets.
- Numeric aliases for `--shading`, `--bg`, `--lighting` and `--wireframe-color`. Use the named values.
- Shadow mapping and `--no-shadow`. A single model with no ground plane has little use for cast shadows, while the shadow map cost load and frame time. Baked ambient occlusion remains available.

### Fixed

- Terminal teardown on POSIX now releases a Ctrl+S output pause before sending cleanup escapes, then restores the original flow-control setting. Quitting under XOFF no longer leaves the alternate screen, hidden cursor, or mouse tracking active.
- Rasterminal restores Windows console modes and the output code page after normal exits, Ctrl+C, and Ctrl+Break. It clears queued mouse reports and handles Ctrl+C when processed input was inherited disabled, so the shell receives no stale escapes and keeps its original input behavior.
- POSIX interactive startup now stops safely if raw input mode cannot be enabled. Terminal restoration retries interrupted or transient failures instead of leaving the shell in raw mode.
- POSIX sessions now restore terminal state before terminating from `SIGINT`, `SIGTERM`, `SIGQUIT`, or `SIGHUP`, and preserve the signal termination status. `SIGINT` and `SIGTERM` therefore report their usual shell statuses, typically 130 and 143, instead of 0.
- POSIX job control restores the terminal before Ctrl+Z suspends the viewer. `fg` resumes rendering with a full redraw; `bg` leaves the viewer idle without reclaiming the terminal. Suspension also works during startup queries and with `--no-input`.
- Triangle coverage is more accurate. Long, nearly horizontal edges no longer bleed beyond their triangles, and shared edges have fewer pinholes.
- Malformed models with non-finite texture coordinates or undersized glTF vertex attributes are rejected instead of reading uninitialized or out-of-bounds memory.
- Valid STL files no longer fail when their opening text confuses the ASCII/binary heuristic. STL coordinates are also remapped from the format's usual Z-up convention so models load upright. ASCII STL lines longer than 64 KB are rejected as malformed.
- PLY rejects missing or unknown format declarations instead of treating them as ASCII.
- Input parsing no longer waits for incomplete escape sequences on POSIX. Modified wheel events, horizontal scrolling, malformed mouse reports, interrupted drags and unbound keys no longer cause jumps or cancel held movement.
- Windows falls back to 80x24 when the console cannot report a valid size. Files of 2 GB or more now use 64-bit file sizing.
- `--threads` values above the hardware thread count are clamped consistently, and `--bench-size` rejects dimensions whose pixel count overflows the addressable range.
- Horizontal orbit follows the input direction when the view is upside down. The HUD frame-rate counter now includes sub-millisecond frames and no longer starts at zero on light models.
- glTF now supports the ratified `KHR_meshopt_compression` name, its `COLOR` filter, and sparse accessors.
- CMake handles mixed C and C++ compiler configurations correctly and no longer applies C++ or LTO flags to vendored C sources.
- Release builds no longer fail on AVX10-capable CPUs because of Clang's promotion warning.

## [v0.1.0-alpha.1] - 2026-06-27

First public prerelease.

### Added

- CPU rasterization pipeline: model/view/projection transforms, perspective-correct triangle rasterization, z-buffer depth testing, backface culling, Blinn-Phong lighting, and order-independent alpha-blended transparency (exact per-pixel A-buffer)
- Shading modes: wireframe, flat, and Phong
- Shadow mapping (alpha-cutout aware) and baked ambient occlusion
- Texturing: bilinear sampling with per-texture wrap modes (Repeat/Clamp/Mirror), alpha cutout, and normal/metallic/emissive/occlusion maps
- Compressed texture and geometry support: KTX2/Basis (ETC1S/UASTC), WebP, Draco mesh compression, and meshopt-compressed buffer views
- Model formats: OBJ/MTL, PLY (ASCII and binary LE/BE), STL (ASCII and binary), and glTF 2.0 / GLB
- Multithreaded worker pool driving the opaque, transparent-accumulate, and resolve phases
- Unicode half-block (`▀`) output with 24-bit ANSI color; two vertical pixels per terminal cell
- Interactive controls: keyboard plus mouse drag-orbit and scroll-zoom, auto-rotation, and a HUD status line
- CLI flags for initial shading/background/lighting/wireframe color, culling, texturing, threads, frame cap, headless benchmarking, crease angle, and the `--no-shadow`/`--no-ao`/`--no-hud` toggles
- Cross-platform support: Linux, macOS, and Windows, including 32-bit (ILP32) builds
- Two build systems (Make and CMake) each with release, portable, and dist (self-contained) variants

[Unreleased]: https://github.com/PavolUlicny/rasterminal/compare/v0.1.0-alpha.1...HEAD
[v0.1.0-alpha.1]: https://github.com/PavolUlicny/rasterminal/releases/tag/v0.1.0-alpha.1
