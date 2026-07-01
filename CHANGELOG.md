# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `install`/`uninstall` targets for both Make and CMake (GNU directory layout, `PREFIX`/`DESTDIR` overrides) installing the binary, man page, and license/notices
- Man page (`man/rasterminal.1`)

### Changed

- Lit surfaces now pass through a soft-knee highlight rolloff (tonemap) before display, so bright, untextured, flat-lit, or strongly-emissive areas no longer clip to flat white but keep their shading gradient. The curve is identity below the knee (0.7) and rolls off above it, so any channel lit past the knee (including a full-white lit surface) is pulled down somewhat while darker midtones and shadows are unchanged; unlit materials and UI (wireframe, HUD, background) are unaffected.

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
