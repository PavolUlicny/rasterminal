# Vendored libraries

Each library is vendored as a header, source subset or full tree according to the
project's needs. Do not edit vendored files by hand. `vendor/.clang-format` disables
formatting and `.gitattributes` suppresses their diffs. Use the recipes below for updates.

After any update, run a clean build and test:

```sh
rm -rf build
cmake -B build
cmake --build build --target check -j
```

| Library | Version | Commit | Upstream | License |
| --- | --- | --- | --- | --- |
| Assimp | v6.0.5 | `392a658f9c271be965271f45e7521a1b80ea4392` | <https://github.com/assimp/assimp> | BSD-3-Clause plus bundled-component licenses |
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
| miniz (release amalgamation) | 3.1.2 | `77d0dce8627735138c51770d1799a1ef48f2117d` | <https://github.com/richgel999/miniz> | MIT |

## Refresh recipe

Assimp comes from its tagged source archive. Copy only the build and legal files
plus `cmake-modules/`, `code/`, `contrib/` and `include/` shown below. Do not copy
the root `.clang-format` or `test/models-nonbsd/`; the latter has separate licenses.

```sh
TAG=v6.0.5
ARCHIVE=/tmp/assimp-${TAG}.tar.gz
SOURCE=/tmp/assimp-${TAG}
curl -fL "https://github.com/assimp/assimp/archive/refs/tags/${TAG}.tar.gz" -o "$ARCHIVE"
sha256sum "$ARCHIVE"
mkdir "$SOURCE"
tar -xzf "$ARCHIVE" --strip-components=1 -C "$SOURCE"
rm -rf vendor/assimp
mkdir vendor/assimp
cp -a "$SOURCE"/{CMakeLists.txt,Build.md,CHANGES.md,CREDITS,LICENSE,Readme.md,SECURITY.md,assimp.pc.in} \
      "$SOURCE"/{cmake-modules,code,contrib,include} vendor/assimp/
rm -rf vendor/assimp/contrib/{android-cmake,draco,googletest,meshlab,tinyusdz,zip} \
       vendor/assimp/contrib/zlib/contrib
git ls-remote https://github.com/assimp/assimp.git "refs/tags/${TAG}"
```

Record the resolved tag commit and archive SHA-256. The v6.0.5 archive hash is
`edf3749559c2b7d1f758ffb66fc5bec62186221e623b7f2e8969f17ee46ecb6f`.
Review the root and retained `contrib/` licenses, then update this table, the top-level
README and `THIRD_PARTY_NOTICES`. Retained files must match upstream byte for byte.
Recheck every `ASSIMP_*` option because a rename can restore disabled code or dependencies.
Keep tools, samples, tests, Draco, USD and VRML disabled; they depend on pruned or downloaded sources.

Assimp builds as a static, import-only library. Exporters, tools, samples, tests,
documentation, installation, Draco, M3D, USD, VRML and C4D are disabled. OBJ, PLY,
STL and glTF remain native. A clean configure must report:

```text
Enabled:  AMF 3DS AC ASE ASSBIN B3D BVH COLLADA DXF CSM HMP IRRMESH IQM IRR LWO LWS
          MD2 MD3 MD5 MDC MDL NFF NDO OFF OGRE OPENGEX MS3D COB BLEND IFC XGL FBX
          Q3D Q3BSP RAW SIB SMD TERRAGEN 3D X X3D 3MF MMD
Disabled: OBJ PLY STL USD GLTF
```

Inspect the final link graph. Assimp and `zlibstatic` must come from `vendor/assimp/`,
with no system or downloaded dependency. Keep the zlib 1.2.13 Darwin workaround in the
root CMake file until upstream removes its `TARGET_OS_MAC` check.

```sh
# Example: update cgltf to v1.16
curl -sL https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.16/cgltf.h -o vendor/cgltf/cgltf.h
curl -sL https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.16/LICENSE  -o vendor/cgltf/LICENSE
git ls-remote https://github.com/jkuhlmann/cgltf refs/tags/v1.16
# Update both version tables and any changed license notice.
```

stb has no per-file tags. Use `master` and record the resolved HEAD SHA:

```sh
curl -sL https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -o vendor/stb/stb_image.h
curl -sL https://raw.githubusercontent.com/nothings/stb/master/LICENSE      -o vendor/stb/LICENSE
git ls-remote https://github.com/nothings/stb HEAD
```

Flatten stl_reader's `include/stl_reader/stl_reader.h` to `vendor/stl_reader/stl_reader.h`:

```sh
curl -sL https://raw.githubusercontent.com/sreiter/stl_reader/<tag>/include/stl_reader/stl_reader.h -o vendor/stl_reader/stl_reader.h
curl -sL https://raw.githubusercontent.com/sreiter/stl_reader/<tag>/LICENSE -o vendor/stl_reader/LICENSE
```

miniz release assets contain the amalgamated `miniz.c` and `miniz.h` pair:

```sh
curl -sL https://github.com/richgel999/miniz/releases/download/<tag>/miniz-<tag>.zip -o /tmp/miniz.zip
unzip -o -j /tmp/miniz.zip miniz.c miniz.h LICENSE -d vendor/miniz
git ls-remote https://github.com/richgel999/miniz refs/tags/<tag>
```

Update both version tables and `THIRD_PARTY_NOTICES` if the license changed. Verify that
`MINIZ_NO_ZLIB_COMPATIBLE_NAMES`, `MINIZ_NO_STDIO` and `MINIZ_NO_ARCHIVE_APIS` still
exist upstream. A renamed or removed macro silently restores unwanted code. Never set
`MINIZ_NO_INFLATE_APIS`; tests use `mz_uncompress`.

meshoptimizer is compiled through `meshoptimizer_impl.cpp`:

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
# Update both version tables and any changed license notice.
```

Draco is a decode-only glTF bitstream subset compiled through `draco_impl.cpp`.
Regenerate its source closure from upstream on every update:

```sh
TAG=<tag>; git clone --depth 1 --branch "$TAG" https://github.com/google/draco.git /tmp/draco && cd /tmp/draco
# 1. Build the decoder and generate draco_features.h.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDRACO_GLTF_BITSTREAM=ON -DDRACO_TESTS=OFF
cmake --build build --target draco_decoder -j

# 2. List decoder candidates, excluding encoders, I/O, scenes, tools and tests.
find src/draco -name '*.cc' \
  | grep -vE '_test\.cc$|/testing/|/tools/|/io/|/javascript/|/maya/|/unity/|/scene/|/texture/|/material/|/animation/' \
  | grep -viE 'encode|encoder|_enc' > /tmp/cand.txt

# 3. Archive the candidates and link a minimal DecodeMeshFromBuffer program. Read the
#    selected members from the link map, calculate their `c++ -M` header closure, and copy
#    both sets to vendor/draco/ without changing the src/draco/ layout.

# 4. Copy provenance files and the generated feature header.
cp LICENSE AUTHORS /path/to/vendor/draco/
cp build/draco/draco_features.h /path/to/vendor/draco/src/draco/draco_features.h   # then strip the "GENERATED" banner
git rev-parse HEAD   # record the commit SHA in the table above
```

Update both version tables. Verify that `vendor/draco/src/draco/draco_features.h` matches
`DRACO_GLTF_BITSTREAM=ON`, and update changed license notices.

basis_universal provides the decode-only KTX2/Basis transcoder for `KHR_texture_basisu`.
Vendor its `transcoder/` directory and bundled zstd decode amalgam. `basisu_impl.cpp`
defines `BASISD_SUPPORT_KTX2` and `BASISD_SUPPORT_KTX2_ZSTD`; leave GPU targets at their
upstream defaults because partial stripping does not compile. CMake applies `-w` to the
shim and builds the zstd C file through `rasterminal_c`.

```sh
TAG=<tag>; git clone --depth 1 --branch "$TAG" https://github.com/BinomialLLC/basis_universal.git /tmp/bu && cd /tmp/bu
cp transcoder/*           /path/to/vendor/basisu/transcoder/
cp zstd/zstd.h zstd/zstd_errors.h zstd/zstddeclib.c zstd/LICENSE  /path/to/vendor/basisu/zstd/
cp LICENSE                /path/to/vendor/basisu/LICENSE          # Apache-2.0 (basisu)
cp NOTICE                 /path/to/vendor/basisu/NOTICE           # required by Apache 2.0 4(d), do not drop
git rev-parse HEAD        # record the commit SHA in the table above (both basisu + zstd rows)
```

Verify that the `BASISD_SUPPORT_*` defines still compile. Update `THIRD_PARTY_NOTICES`
when either the license or NOTICE changes; it reproduces the NOTICE verbatim.

libwebp is a decode-only subset for `EXT_texture_webp`. Regenerate its source closure from
upstream on every update. `WEBP_DEC_SRCS` lists each C file because a unity build collides
on file-local statics. SIMD files select themselves through architecture macros and need no
per-file flags. `rasterminal_c` compiles them with `-w` and without LTO or C++ options.
Preserve the `src/` layout for repo-rooted internal includes.

```sh
TAG=v1.6.0; git clone --depth 1 --branch "$TAG" https://chromium.googlesource.com/webm/libwebp /tmp/webp && cd /tmp/webp

# 1. List libwebpdecoder's dec, dsp and utils groups, excluding MIPS and MSA.
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
# 2. Calculate the header closure. Native and portable x86 passes cover x86 SIMD. A third
#    pass forces WEBP_USE_NEON with a stub arm_neon.h so src/dsp/neon.h is included for ARM.
mkdir -p /tmp/armstub && : > /tmp/armstub/arm_neon.h
HDRS=$({ for m in "-march=native" ""; do for f in $CFILES; do gcc $m -DNDEBUG -I. -MM -MG $f 2>/dev/null; done; done
         for f in $CFILES; do gcc -DWEBP_USE_NEON -DNDEBUG -I. -isystem /tmp/armstub -MM -MG $f 2>/dev/null; done; } \
       | tr ' \\' '\n\n' | grep '^src/.*\.h$' | sed 's#/\./#/#' | sort -u)
# 3. Preserve the src/ layout and copy the license, patent grant and author list.
for f in $CFILES $HDRS; do mkdir -p /path/to/vendor/libwebp/$(dirname $f); cp $f /path/to/vendor/libwebp/$f; done
cp COPYING PATENTS AUTHORS /path/to/vendor/libwebp/
git rev-parse HEAD   # record the commit SHA in the table above
```

Keep `WEBP_DEC_SRCS` synchronized with the C closure. Update `THIRD_PARTY_NOTICES` when
the license or PATENTS changes.
