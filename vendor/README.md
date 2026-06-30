# Vendored libraries

Libraries are chosen for fit, not size, and vendored directly — a library may be a single header, a small source set, or a full vendored source subset, whatever the job warrants. Do not edit these files manually — they are formatting-disabled via `vendor/.clang-format` and diff-suppressed in `.gitattributes`. To update a library, follow the refresh recipe below.

| Library | Version | Commit | Upstream | License |
| --- | --- | --- | --- | --- |
| stb_image | v2.30 | `31c1ad37456438565541f4919958214b6e762fb4` | <https://github.com/nothings/stb> | MIT / Unlicense (dual) |
| cgltf | master (post-v1.15) | `85cd62382dfea638278962690cf515023f33ed00` | <https://github.com/jkuhlmann/cgltf> | MIT |
| tinyply | 3.0 | `c9bb690dfe5e9105961e9e28120c48c9ae084bc6` | <https://github.com/ddiakopoulos/tinyply> | public domain |
| tinyobjloader | v2.0.0rc13 | `2945a967c5303b2c8c14174117c45f3302591150` | <https://github.com/tinyobjloader/tinyobjloader> | MIT |
| stl_reader | v2.0 | `a130fe0b2ac15d7c2fd642bf1dcbdec600e69151` | <https://github.com/sreiter/stl_reader> | BSD-2-Clause |
| meshoptimizer | v1.1 | `dc9d09ed83e1004aef47a1c3c597e0ec64848a37` | <https://github.com/zeux/meshoptimizer> | MIT |
| draco | 1.5.7 | `8786740086a9f4d83f44aa83badfbea4dce7a1b5` | <https://github.com/google/draco> | Apache-2.0 |
| basis_universal (`vendor/basisu/`) | v2_1_0r | `e4f439fc9545b6a9e1fd26fc7ffd0c682c4b96d4` | <https://github.com/BinomialLLC/basis_universal> | Apache-2.0 |
| zstd (decode amalgam) | bundled with basis_universal `v2_1_0r` | `e4f439fc9545b6a9e1fd26fc7ffd0c682c4b96d4` | <https://github.com/BinomialLLC/basis_universal> (vendored copy of <https://github.com/facebook/zstd>) | BSD-3-Clause |
| libwebp (decode subset, `vendor/libwebp/`) | v1.6.0 | `4fa21912338357f89e4fd51cf2368325b59e9bd9` | <https://chromium.googlesource.com/webm/libwebp> | BSD-3-Clause + PATENTS |

## Refresh recipe

```sh
# Example: update cgltf to v1.16
curl -sL https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.16/cgltf.h -o vendor/cgltf/cgltf.h
curl -sL https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.16/LICENSE  -o vendor/cgltf/LICENSE
git ls-remote https://github.com/jkuhlmann/cgltf refs/tags/v1.16
# Update the commit and version in this table, update THIRD_PARTY_NOTICES if the license changed, then test: make clean && make && make test
```

For stb (no per-file tags), use `master` and record the resolved HEAD SHA:

```sh
curl -sL https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -o vendor/stb/stb_image.h
curl -sL https://raw.githubusercontent.com/nothings/stb/master/LICENSE      -o vendor/stb/LICENSE
git ls-remote https://github.com/nothings/stb HEAD
```

For stl_reader (header lives at `include/stl_reader/stl_reader.h` in the repo, but we flatten to `vendor/stl_reader/stl_reader.h`):

```sh
curl -sL https://raw.githubusercontent.com/sreiter/stl_reader/<tag>/include/stl_reader/stl_reader.h -o vendor/stl_reader/stl_reader.h
curl -sL https://raw.githubusercontent.com/sreiter/stl_reader/<tag>/LICENSE -o vendor/stl_reader/LICENSE
```

For meshoptimizer (header lives in `src/`, compiled via unity shim `meshoptimizer_impl.cpp`):

```sh
BASE="https://raw.githubusercontent.com/zeux/meshoptimizer/<tag>"
curl -sL "$BASE/src/meshoptimizer.h" -o vendor/meshoptimizer/src/meshoptimizer.h
curl -sL "$BASE/LICENSE.md"          -o vendor/meshoptimizer/LICENSE.md
for f in allocator clusterizer indexanalyzer indexcodec indexgenerator \
          meshletcodec meshletutils opacitymap overdrawoptimizer partition \
          quantization rasterizer simplifier spatialorder stripifier \
          vcacheoptimizer vertexcodec vertexfilter vfetchoptimizer; do
    curl -sL "$BASE/src/$f.cpp" -o "vendor/meshoptimizer/src/$f.cpp"
done
git ls-remote https://github.com/zeux/meshoptimizer refs/tags/<tag>
# Update the commit and version in this table, update THIRD_PARTY_NOTICES if the license changed, then test: make clean && make && make test
```

For draco (decode-only glTF-bitstream subset, compiled via unity shim `draco_impl.cpp`):
Draco ships no single-header form, so we vendor only the source files the decoder
pulls. The subset is *derived mechanically* from an upstream checkout rather than
hand-listed — regenerate it on every version bump:

```sh
TAG=<tag>; git clone --depth 1 --branch "$TAG" https://github.com/google/draco.git /tmp/draco && cd /tmp/draco
# 1. Build draco_decoder with the glTF-bitstream feature set; this also generates draco_features.h.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDRACO_GLTF_BITSTREAM=ON -DDRACO_TESTS=OFF
cmake --build build --target draco_decoder -j

# 2. Candidate decode sources = all .cc minus encoders/io/scene/tools/tests.
find src/draco -name '*.cc' \
  | grep -vE '_test\.cc$|/testing/|/tools/|/io/|/javascript/|/maya/|/unity/|/scene/|/texture/|/material/|/animation/' \
  | grep -viE 'encode|encoder|_enc' > /tmp/cand.txt

# 3. Compile candidates into an archive; link a minimal DecodeMeshFromBuffer program;
#    the linker pulls exactly the needed members. Read them from the map, then take the
#    `c++ -M` header closure of that .cc set. Copy both into vendor/draco/ preserving the
#    src/draco/... layout. (See the phase-1 spike notes for the exact extraction script.)

# 4. License + provenance, then hand-author the generated features header:
cp LICENSE AUTHORS /path/to/vendor/draco/
cp build/draco/draco_features.h /path/to/vendor/draco/src/draco/draco_features.h   # then strip the "GENERATED" banner
git rev-parse HEAD   # record the commit SHA in the table above
```

Update the commit and version in this table; verify `vendor/draco/src/draco/draco_features.h`
still matches what `DRACO_GLTF_BITSTREAM=ON` generates; update `THIRD_PARTY_NOTICES` if the
license changed; then test: `make clean && make && make test`.

For basis_universal (decode-only KTX2/Basis transcoder for `KHR_texture_basisu`): we vendor
the whole `transcoder/` directory plus the bundled zstd *decode* amalgam (zstd ships inside
the basis_universal repo). The transcoder is configured for decode in the
`vendor/basisu/basisu_impl.cpp` shim (only `BASISD_SUPPORT_KTX2` + `BASISD_SUPPORT_KTX2_ZSTD`
are set; the GPU block-format targets are left at upstream defaults — partial stripping does
not compile). The shim and the zstd C TU are built with blanket `-w` (they are large,
unaudited TUs), wired per-source in both the Makefile and CMakeLists.txt.

```sh
TAG=<tag>; git clone --depth 1 --branch "$TAG" https://github.com/BinomialLLC/basis_universal.git /tmp/bu && cd /tmp/bu
cp transcoder/*           /path/to/vendor/basisu/transcoder/
cp zstd/zstd.h zstd/zstd_errors.h zstd/zstddeclib.c zstd/LICENSE  /path/to/vendor/basisu/zstd/
cp LICENSE                /path/to/vendor/basisu/LICENSE          # Apache-2.0 (basisu)
cp NOTICE                 /path/to/vendor/basisu/NOTICE           # required by Apache 2.0 4(d) — do not drop
git rev-parse HEAD        # record the commit SHA in the table above (both basisu + zstd rows)
```

Re-verify the `basisu_impl.cpp` `BASISD_SUPPORT_*` defines still compile; if either
license **or the NOTICE text** changed, update `THIRD_PARTY_NOTICES` (it reproduces the
basis_universal NOTICE verbatim, so a NOTICE change there must be mirrored — it is not a
license change); then test: `make clean && make && make test`.

For libwebp (decode-only subset for `EXT_texture_webp`): libwebp ships no single-header
form, so we vendor only the source the decoder pulls. The set is *derived mechanically*
from an upstream checkout — do NOT hand-maintain it; regenerate on every version bump.
The `.c` files are listed individually in both build systems (a unity `#include` shim
fails on duplicate file-local statics, e.g. `clip_8b`); the dsp SIMD variants self-gate on
arch macros (`WEBP_USE_SSE2`/`SSE41`/`AVX2`/`NEON`), so no per-file SIMD flags are used.
They are C TUs, built with blanket `-w` (Makefile `C_OPT`; CMake `set_source_files_properties`
on `WEBP_DEC_SRCS`). Internal includes are repo-rooted (`"src/dec/..."`), so the layout
must keep the `src/` prefix and the include path is `-isystem vendor/libwebp`.

```sh
TAG=v1.6.0; git clone --depth 1 --branch "$TAG" https://chromium.googlesource.com/webm/libwebp /tmp/webp && cd /tmp/webp

# 1. Decode .c set = libwebp's libwebpdecoder groups (see src/{dec,dsp,utils}/Makefile.am:
#    dec + dsp COMMON + the *_decode_ SIMD variants + utils COMMON), minus mips/msa (no target).
CFILES=$(cat <<'EOF'
src/dec/alpha_dec.c src/dec/buffer_dec.c src/dec/frame_dec.c src/dec/idec_dec.c src/dec/io_dec.c src/dec/quant_dec.c src/dec/tree_dec.c src/dec/vp8_dec.c src/dec/vp8l_dec.c src/dec/webp_dec.c
src/dsp/alpha_processing.c src/dsp/cpu.c src/dsp/dec.c src/dsp/dec_clip_tables.c src/dsp/filters.c src/dsp/lossless.c src/dsp/rescaler.c src/dsp/upsampling.c src/dsp/yuv.c
src/dsp/alpha_processing_sse2.c src/dsp/dec_sse2.c src/dsp/filters_sse2.c src/dsp/lossless_sse2.c src/dsp/rescaler_sse2.c src/dsp/upsampling_sse2.c src/dsp/yuv_sse2.c
src/dsp/alpha_processing_sse41.c src/dsp/dec_sse41.c src/dsp/lossless_sse41.c src/dsp/upsampling_sse41.c src/dsp/yuv_sse41.c
src/dsp/lossless_avx2.c
src/dsp/alpha_processing_neon.c src/dsp/dec_neon.c src/dsp/filters_neon.c src/dsp/lossless_neon.c src/dsp/rescaler_neon.c src/dsp/upsampling_neon.c src/dsp/yuv_neon.c
src/utils/bit_reader_utils.c src/utils/color_cache_utils.c src/utils/filters_utils.c src/utils/huffman_utils.c src/utils/palette.c src/utils/quant_levels_dec_utils.c src/utils/rescaler_utils.c src/utils/random_utils.c src/utils/thread_utils.c src/utils/utils.c
EOF
)
# 2. Header set = the -M closure of those .c. The two x86 passes (native + portable) capture the
#    SSE2/SSE41/AVX2-guarded includes; a THIRD pass forces WEBP_USE_NEON (with a stub <arm_neon.h>)
#    so the ARM-only include src/dsp/neon.h is captured too. Toggling -march on x86 never enters the
#    `#if defined(WEBP_USE_NEON)` block, so without this pass neon.h is silently dropped and the
#    macOS-ARM build later fails with "src/dsp/neon.h file not found".
mkdir -p /tmp/armstub && : > /tmp/armstub/arm_neon.h
HDRS=$({ for m in "-march=native" ""; do for f in $CFILES; do gcc $m -DNDEBUG -I. -MM -MG $f 2>/dev/null; done; done
         for f in $CFILES; do gcc -DWEBP_USE_NEON -DNDEBUG -I. -isystem /tmp/armstub -MM -MG $f 2>/dev/null; done; } \
       | tr ' \\' '\n\n' | grep '^src/.*\.h$' | sed 's#/\./#/#' | sort -u)
# 3. Copy .c + .h preserving the src/ layout, plus the three root files every source
#    header points to: COPYING (license), PATENTS (patent grant), AUTHORS (contributors).
for f in $CFILES $HDRS; do mkdir -p /path/to/vendor/libwebp/$(dirname $f); cp $f /path/to/vendor/libwebp/$f; done
cp COPYING PATENTS AUTHORS /path/to/vendor/libwebp/
git rev-parse HEAD   # record the commit SHA in the table above
```

If the `.c` closure changed (new/removed files), update `CSRCS` (Makefile) and
`WEBP_DEC_SRCS` (CMakeLists.txt) to match; if the license or PATENTS text changed, update
`THIRD_PARTY_NOTICES`; then test: `make clean && make && make test`.
