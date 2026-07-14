# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- 256-color fallback: terminals without 24-bit color support now automatically receive xterm-256 palette output, quantized per pixel from the same truecolor rendering pipeline. Capability is detected at startup from the `COLORTERM` and `TERM` environment variables (`COLORTERM=truecolor`/`24bit`, a `TERM` truecolor hint such as the `-direct` terminfo family, or a `TERM` naming a known truecolor terminal (kitty, WezTerm, Alacritty, Ghostty, foot, Contour) selects 24-bit output; any other non-empty `TERM` except `dumb` gets 256 colors; when `TERM` is unset or empty and `COLORTERM` carries no truecolor signal, the default is 256 colors, except on native Windows consoles which default to 24-bit). Terminals detected as truecolor keep byte-identical output. The known-terminal names keep ssh sessions (where `COLORTERM` is not forwarded) on 24-bit for those terminals; a truecolor terminal not identifiable from `TERM` is still detected as 256-color over ssh.
- `install`/`uninstall` targets for both Make and CMake (GNU directory layout, `PREFIX`/`DESTDIR` overrides) installing the binary, man page, and license/notices
- Man page (`man/rasterminal.1`)

### Changed

- Interactive rendering now fails with a clear error, instead of emitting escape garbage, on any terminal it cannot draw to: stdin or stdout not being a terminal (piped or redirected, which previously also set raw mode on a non-terminal stdin), `TERM=dumb`, and Windows consoles that cannot enable VT escape-sequence processing. `--bench`, `--help`, and `--version` are unaffected and keep working with redirected stdio (with one Windows-only nuance: `--version` now switches the console to UTF-8 only when stdout is a VT-capable console, so when it is redirected, piped, or a legacy console, the code page is left untouched. The emitted bytes are UTF-8 either way; only display through a non-UTF-8 console shows the accented author name as mojibake).
- Lit surfaces now pass through a soft-knee highlight rolloff (tonemap) before display, so bright, untextured, flat-lit, or strongly-emissive areas no longer clip to flat white but keep their shading gradient. The curve is identity below the knee (0.7) and rolls off above it, so any channel lit past the knee (including a full-white lit surface) is pulled down somewhat while darker midtones and shadows are unchanged; unlit materials and UI (wireframe, HUD, background) are unaffected.
- Slightly smaller per-frame terminal output: the incremental redraw no longer writes a cursor-advance escape when a row's changed pixels are followed by an unchanged run reaching the right edge. The rendered result is identical; only the redundant escape (which the next absolute cursor move made a no-op) is dropped.

### Removed

- The shadow map and the `--no-shadow` flag. A single auto-fit model with no ground plane has nothing to receive a cast shadow, so the feature added load and per-frame cost without value to a model viewer. Lit surfaces now use all configured lights uniformly; baked ambient occlusion still provides crevice darkening. The default look is slightly flatter than before. Passing `--no-shadow` is now an unknown-flag error.

### Fixed

- The interactive HUD FPS counter read `0 fps` at startup (until the model was moved) and updated erratically, reading too low, at uncapped or very high frame caps. It discarded every frame faster than 1 ms, so on light models where nearly every frame is sub-millisecond the reading was never seeded. The smoothing threshold is fixed so sub-millisecond frames now count, and the displayed value is refreshed at a fixed ~10 Hz so it stays readable at high frame rates.
- glTF models using the ratified `KHR_meshopt_compression` extension (as opposed to the older draft `EXT_meshopt_compression` name) failed to load any real geometry, since the vendored `cgltf` only recognized the draft name. Refreshed `cgltf` to a current commit that parses both, and added decoding for the extension's `COLOR` vertex filter.
- glTF sparse accessors (`POSITION`/`NORMAL`/`TEXCOORD`/indices using the `accessor.sparse` override) previously read as zero/empty instead of resolving the sparse data, a latent bug in the same `cgltf` refresh above.

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
