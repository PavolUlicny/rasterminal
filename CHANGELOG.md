# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `--yaw`, `--pitch`, and `--zoom` flags setting the initial camera pose. `--yaw` takes degrees in `[-180, 180]` (default `0`); positive turns the model left on screen, the same direction as `--spin-direction left`. `--pitch` takes degrees in `[-180, 180]` (default `-17.2`, a rounding of the previous fixed `-0.3` rad tilt); negative looks down from above, `-90` is top-down, and values past 90 in either direction view the model upside down. `--zoom` takes an apparent-size multiplier of the auto-fit framing in `[0.2, 100]` (default `1`); `2` starts twice as close, and the bounds equal the range reachable with the scroll wheel. Together the three flags can express any camera view reachable interactively. They also set the `--bench` camera (the per-frame bench spin step is unchanged), and the `R` reset key returns to the flag-specified view.

- 256-color fallback: terminals without 24-bit color support now automatically receive xterm-256 palette output, quantized per pixel from the same truecolor rendering pipeline by perceptual (CIELAB) nearest-color matching, so dark and muted colors keep their hue instead of collapsing to the gray ramp (the palette's color cube has no chromatic entry with a nonzero channel below 95, and a plain RGB-distance match would send nearly all dark colors to gray). Exact palette colors, the default black background among them, always map to themselves. Capability is detected at startup from the `COLORTERM` and `TERM` environment variables (`COLORTERM=truecolor`/`24bit`, a `TERM` truecolor hint such as the `-direct` terminfo family, or a `TERM` naming a known truecolor terminal (kitty, WezTerm, Alacritty, Ghostty, foot, Contour) selects 24-bit output; any other non-empty `TERM` except `dumb` gets 256 colors; when `TERM` is unset or empty and `COLORTERM` carries no truecolor signal, the default is 256 colors, except on native Windows consoles which default to 24-bit). Terminals detected as truecolor keep byte-identical output. The known-terminal names keep ssh sessions (where `COLORTERM` is not forwarded) on 24-bit for those terminals; a truecolor terminal not identifiable from `TERM` is still detected as 256-color over ssh (force 24-bit with `--color truecolor`).
- `--color truecolor|256|auto` flag (with `24bit` accepted as an alias of `truecolor`) overriding the automatic color-capability detection; `auto` (the default) keeps detection. Forcing a mode does not bypass the `TERM=dumb` rejection or the Windows VT-capability gate.
- `--spin-speed` and `--spin-direction left|right` flags controlling the auto-rotation rate (degrees per second, any positive number) and the way the model's front face moves on screen. Both apply whenever spinning is active, whether started with `--spin` or toggled with Space, and neither implies `--spin`. The defaults match the previous fixed behavior: `left`, and 45 degrees per second (a rounding of the old hard-coded 0.8 rad/s, about 45.8). The `--bench` camera path is unaffected.
- `--no-spin`, `--ao`, and `--hud` flags, completing the paired-boolean convention: all five boolean state flags (`--cull`, `--texture`, `--spin`, `--ao`, `--hud`) now come as `--flag`/`--no-flag` pairs with the later flag winning, so the default state is always spellable and an earlier flag can be overridden. The existing `--spin`/`-S`, `--no-ao`, and `--no-hud` behave exactly as before.
- `install`/`uninstall` targets for both Make and CMake (GNU directory layout, `PREFIX`/`DESTDIR` overrides) installing the binary, man page, and license/notices
- Man page (`man/rasterminal.1`)

### Changed

- `--cull` and `--texture` no longer take an `on|off` value; they are now paired boolean flags (`--cull`/`--no-cull` and `--texture`/`--no-texture`, with the later flag winning), in the same plain-boolean style as `--spin` and `--no-ao`. The `-c`/`-t` short flags and the boolean value spellings (`on`/`off`, `1`/`0`, `true`/`false`, `yes`/`no`, `y`/`n`) are removed with the value form: the `=` form (for example `--cull=on`) is now a does-not-take-a-value error, `-c`/`-t` are unknown-flag errors, and a space-separated token after the flag is no longer consumed as a value but read as the model operand.
- Interactive rendering now fails with a clear error, instead of emitting escape garbage, on any terminal it cannot draw to: stdin or stdout not being a terminal (piped or redirected, which previously also set raw mode on a non-terminal stdin), `TERM=dumb`, and Windows consoles that cannot enable VT escape-sequence processing. `--bench`, `--help`, and `--version` are unaffected and keep working with redirected stdio (with one Windows-only nuance: `--version` now switches the console to UTF-8 only when stdout is a VT-capable console, so when it is redirected, piped, or a legacy console, the code page is left untouched. The emitted bytes are UTF-8 either way; only display through a non-UTF-8 console shows the accented author name as mojibake).
- Lit surfaces now pass through a soft-knee highlight rolloff (tonemap) before display, so bright, untextured, flat-lit, or strongly-emissive areas no longer clip to flat white but keep their shading gradient. The curve is identity below the knee (0.7) and rolls off above it, so any channel lit past the knee (including a full-white lit surface) is pulled down somewhat while darker midtones and shadows are unchanged; unlit materials and UI (wireframe, HUD, background) are unaffected.
- Slightly smaller per-frame terminal output: the incremental redraw no longer writes a cursor-advance escape when a row's changed pixels are followed by an unchanged run reaching the right edge. The rendered result is identical; only the redundant escape (which the next absolute cursor move made a no-op) is dropped.

### Removed

- The 1-indexed numeric value aliases for `--shading`/`--bg`/`--lighting`/`--wireframe-color` (for example `-s 3` for `-s phong`, or `--bg 2` for `--bg gray`). Use the named values instead; a numeric value is now an invalid-value error.
- The shadow map and the `--no-shadow` flag. A single auto-fit model with no ground plane has nothing to receive a cast shadow, so the feature added load and per-frame cost without value to a model viewer. Lit surfaces now use all configured lights uniformly; baked ambient occlusion still provides crevice darkening. The default look is slightly flatter than before. Passing `--no-shadow` is now an unknown-flag error.

### Fixed

- STL models now load upright instead of pitched 90 degrees sideways. STL carries no orientation metadata and its ecosystem convention (CAD, 3D printing) is Z-up, while the renderer is Y-up; the loader now remaps STL coordinates to Y-up at load, matching the fixed per-format up-axis mainstream viewers use. OBJ and glTF are Y-up by specification and PLY is treated as Y-up, so those formats are unchanged.
- A `--threads`/`-j` value above the hardware thread count is now clamped to it everywhere. Previously only the render worker pool clamped: the `--bench` report header printed the unclamped request (for example `threads=999` while 16 workers actually ran), and model loading (ambient-occlusion baking, texture decode) could spawn the full requested number of threads.
- `--bench-size WxH` now rejects dimensions whose pixel count (`W * H`) would exceed the addressable range, instead of accepting them. Such a value previously crashed a 32-bit build (the pixel count wrapped a 32-bit `size_t`, producing a tiny framebuffer that rasterization then wrote past); 64-bit builds already failed on the oversized allocation. Each axis was, and still is, capped individually.
- Release builds (`-march=native`) no longer fail with newer Clang on AVX10-capable CPUs: Clang's benign "avx10.1-256 will be promoted to avx10.1-512" compiler warning was fatal under `-Werror`; it is now suppressed in both build systems (Clang only).
- Model files of 2 GB or more now load on Windows (file sizing used the C library's 32-bit `ftell` there, so a binary STL that large was always rejected at its file-size validation and a glTF external image that large failed to read; 64-bit Linux and macOS were unaffected). 32-bit builds, whose address space can never fit such a model, now report the normal load failure everywhere (previously 32-bit Windows misdetected the file at its size validation, and 32-bit Linux failed at opening it, since large-file `fopen` support was not enabled).
- Horizontal orbit no longer reverses when the model is viewed upside down: after pitching the camera past the top or bottom pole, dragging the mouse left/right (and the A/D and arrow keys) moved the model opposite to the input. The yaw input is now inverted while the view is upside down (matching Blender's turntable behaviour), so the model always follows the drag, including continuously through a drag that crosses a pole.
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
