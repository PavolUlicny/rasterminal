cmake_minimum_required(VERSION 3.16)
# C is enabled for the vendored zstd decode amalgam (vendor/basisu/zstd/zstddeclib.c),
# used by the basisu transcoder for KTX2 Zstd supercompression.
project(rasterminal CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RASTERMINAL_PORTABLE
       "Build for distribution: omit machine-specific opts (-march=native / /arch:AVX2)"
       OFF)
option(RASTERMINAL_BUILD_TESTS "Build the test suite" ON)
# Release-artifact link: statically link libstdc++/libgcc so the binary drops the build host's
# libstdc++ ABI dependency (no "GLIBCXX_… not found" on older distros). glibc stays dynamic;
# full distro-portability still needs an old-glibc/-static build. Linux GNU/Clang only (macOS
# links the ABI-stable system libc++). Pair with RASTERMINAL_PORTABLE=ON for the `dist` preset.
option(RASTERMINAL_STATIC_LIBSTDCXX
       "Statically link libstdc++/libgcc into the binary (Linux release artifacts)"
       OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Prefer the -pthread flag (compile + link) over a bare -lpthread, so Threads::Threads
# matches the Makefile's -pthread and stays correct where the compile-time flag matters
# (e.g. BSD); on modern glibc it is effectively a no-op.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# shm_open (the kitty graphics shared-memory transport) lives in librt on glibc
# older than 2.34 (it moved into libc proper there); macOS and musl never split it
# out, and neither ships a librt to link. A true link check rather than a version
# probe, so any libc arrangement resolves itself: plain first, then with rt, and
# fail loud if neither links. POSIX-only: the Windows build compiles no shm code.
set(RASTERMINAL_SHM_LIB "")
if(NOT WIN32)
  include(CheckCXXSourceCompiles)
  set(RASTERMINAL_SHM_PROBE_SRC
      "#include <sys/mman.h>\n#include <fcntl.h>\nint main() { return shm_open(\"/rasterminal-probe\", O_RDONLY, 0) == -1; }")
  check_cxx_source_compiles("${RASTERMINAL_SHM_PROBE_SRC}" RASTERMINAL_SHM_PLAIN)
  if(NOT RASTERMINAL_SHM_PLAIN)
    set(CMAKE_REQUIRED_LIBRARIES rt)
    check_cxx_source_compiles("${RASTERMINAL_SHM_PROBE_SRC}" RASTERMINAL_SHM_NEEDS_RT)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(RASTERMINAL_SHM_NEEDS_RT)
      set(RASTERMINAL_SHM_LIB rt)
    else()
      message(FATAL_ERROR "shm_open links neither plainly nor with librt")
    endif()
  endif()
endif()

# ─── 32-bit (ILP32) target adjustments ────────────────────────────────────────
# CMAKE_SIZEOF_VOID_P reflects the effective target: a -m32 flag (Linux i686), -A Win32
# (MSVC), or a native 32-bit toolchain. Mirrors the Makefile's 32-bit handling, applied
# at directory scope so both targets, all configs (incl. Debug), and C *and* C++ sources
# (the vendored zstd/libwebp decode TUs) inherit it.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(RASTERMINAL_ILP32 ON) # gates -Wuseless-cast off below (word-size-dependent warning)
  # Pin SSE math on x86-32 (GNU/Clang): i686 defaults to the x87 FPU (80-bit intermediates),
  # whose results diverge from the SSE/x86-64 path the renderer and tests assume. ARM ILP32
  # is already IEEE (VFP) and would reject -msse2, so gate on __i386__, not pointer size.
  # MSVC Win32 is SSE2 by default and needs neither flag.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("#ifndef __i386__\n#error not i386\n#endif\nint main(){return 0;}"
                              RASTERMINAL_X86_32)
    if(RASTERMINAL_X86_32)
      add_compile_options(-msse2 -mfpmath=sse)
    endif()
  endif()
endif()

# ─── Large-file support ───────────────────────────────────────────────────────
# On 32-bit glibc, fopen refuses files >= 2 GB (EOVERFLOW) and off_t/ftello are 32-bit
# without this. Directory scope so both targets, all configs, and C *and* C++ TUs share
# one off_t size (mixed LFS defines are a classic ABI footgun). No-op where off_t is
# already 64-bit (LP64 Linux, macOS); MSVC ignores the macro (it uses _ftelli64 instead).
add_compile_definitions(_FILE_OFFSET_BITS=64)

# ─── Vendored libraries ───────────────────────────────────────────────────────
# SYSTEM suppresses warnings from these headers on all compilers.
add_library(rasterminal_vendor INTERFACE)
foreach(lib cgltf stb stl_reader tinyobjloader tinyply meshoptimizer/src draco/src
            basisu/transcoder basisu/zstd libwebp miniz)
  target_include_directories(rasterminal_vendor SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/${lib})
endforeach()

# The basisu transcoder shim and the zstd C amalgam are large, unaudited vendored TUs
# that trip a long, compiler-specific warning set; suppress all their warnings per-source
# (mirrors the Makefile's per-TU -w) rather than maintaining a fragile pragma list.
set_source_files_properties(
  vendor/basisu/basisu_impl.cpp
  PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
# zstd is a C TU: use C_COMPILER_ID so the right flag is chosen on mixed-compiler
# builds (e.g. Clang CXX + MSVC cl.exe C, a valid CMake configuration on Windows).
set_source_files_properties(
  vendor/basisu/zstd/zstddeclib.c
  PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
# miniz (zlib deflate for the kitty graphics direct transport): a C TU like zstd.
set_source_files_properties(
  vendor/miniz/miniz.c
  PROPERTIES COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
# miniz configuration at directory scope so every TU that includes miniz.h sees one
# uniform set (mixed config on a shared header is the same footgun as _FILE_OFFSET_BITS):
# NO_ZLIB_COMPATIBLE_NAMES drops the zlib-name compatibility layer (Z_* macros plus,
# since miniz 3.1, static compress/deflate/crc32 wrapper definitions stamped into every
# including TU), NO_STDIO + NO_ARCHIVE_APIS drop the file-I/O and ZIP layers nothing
# calls (the stdio layer also carries a #pragma message note that blanket -w cannot
# silence; since 3.1.0 it fires only where miniz lacks large-file support, e.g. 32-bit
# x86 Linux, the cross32 CI target). Inflate stays: tests round-trip via mz_uncompress.
add_compile_definitions(MINIZ_NO_ZLIB_COMPATIBLE_NAMES MINIZ_NO_STDIO MINIZ_NO_ARCHIVE_APIS)

# libwebp decode subset (EXT_texture_webp). Listed individually: a unity #include shim
# fails on duplicate file-local statics (e.g. clip_8b). The dsp SIMD variants self-gate on
# arch macros and carry no per-file SIMD flags: release (-march=native / /arch:AVX2) activates
# all of them; portable activates only the SSE2 baseline (sse41/avx2 become empty stubs).
# libwebp ships CPUID runtime dispatch that COULD safely carry sse41/avx2 in a portable binary
# via per-file -msse4.1/-mavx2 (kernels are selected by actual CPU, so no SIGILL risk): we
# deliberately forgo that to keep "portable = no arch-specific codegen" uniform; portable WebP
# decode stays correct, just SSE2-only (decode is load-time, not hot-path). These are C TUs;
# suppress warnings per-source like zstd (GNU/Clang C already gets no -W via the
# COMPILE_LANGUAGE:CXX gate on rasterminal_warnings, but this also covers MSVC's /WX C warnings).
set(WEBP_DEC_SRCS
  vendor/libwebp/src/dec/alpha_dec.c
  vendor/libwebp/src/dec/buffer_dec.c
  vendor/libwebp/src/dec/frame_dec.c
  vendor/libwebp/src/dec/idec_dec.c
  vendor/libwebp/src/dec/io_dec.c
  vendor/libwebp/src/dec/quant_dec.c
  vendor/libwebp/src/dec/tree_dec.c
  vendor/libwebp/src/dec/vp8_dec.c
  vendor/libwebp/src/dec/vp8l_dec.c
  vendor/libwebp/src/dec/webp_dec.c
  vendor/libwebp/src/dsp/alpha_processing.c
  vendor/libwebp/src/dsp/cpu.c
  vendor/libwebp/src/dsp/dec.c
  vendor/libwebp/src/dsp/dec_clip_tables.c
  vendor/libwebp/src/dsp/filters.c
  vendor/libwebp/src/dsp/lossless.c
  vendor/libwebp/src/dsp/rescaler.c
  vendor/libwebp/src/dsp/upsampling.c
  vendor/libwebp/src/dsp/yuv.c
  vendor/libwebp/src/dsp/alpha_processing_sse2.c
  vendor/libwebp/src/dsp/dec_sse2.c
  vendor/libwebp/src/dsp/filters_sse2.c
  vendor/libwebp/src/dsp/lossless_sse2.c
  vendor/libwebp/src/dsp/rescaler_sse2.c
  vendor/libwebp/src/dsp/upsampling_sse2.c
  vendor/libwebp/src/dsp/yuv_sse2.c
  vendor/libwebp/src/dsp/alpha_processing_sse41.c
  vendor/libwebp/src/dsp/dec_sse41.c
  vendor/libwebp/src/dsp/lossless_sse41.c
  vendor/libwebp/src/dsp/upsampling_sse41.c
  vendor/libwebp/src/dsp/yuv_sse41.c
  vendor/libwebp/src/dsp/lossless_avx2.c
  vendor/libwebp/src/dsp/alpha_processing_neon.c
  vendor/libwebp/src/dsp/dec_neon.c
  vendor/libwebp/src/dsp/filters_neon.c
  vendor/libwebp/src/dsp/lossless_neon.c
  vendor/libwebp/src/dsp/rescaler_neon.c
  vendor/libwebp/src/dsp/upsampling_neon.c
  vendor/libwebp/src/dsp/yuv_neon.c
  vendor/libwebp/src/utils/bit_reader_utils.c
  vendor/libwebp/src/utils/color_cache_utils.c
  vendor/libwebp/src/utils/filters_utils.c
  vendor/libwebp/src/utils/huffman_utils.c
  vendor/libwebp/src/utils/palette.c
  vendor/libwebp/src/utils/quant_levels_dec_utils.c
  vendor/libwebp/src/utils/rescaler_utils.c
  vendor/libwebp/src/utils/random_utils.c
  vendor/libwebp/src/utils/thread_utils.c
  vendor/libwebp/src/utils/utils.c)
# WEBP_USE_THREAD mutex-guards libwebp's lazy SIMD function-pointer init (cpu.h): texture
# decode runs on worker threads, so without it the first concurrent WebP decode races on the
# global DSP tables. cpu.h gates the mutex on `WEBP_USE_THREAD && !_WIN32`, so it engages
# everywhere except MSVC/_WIN32: there the unsynchronized path (and thus the race) remains,
# matching upstream's own posture. That residual race is benign: idempotent writes of the same
# SIMD impl, and aligned-pointer stores are atomic on the targeted ISAs.
set_source_files_properties(
  ${WEBP_DEC_SRCS}
  PROPERTIES
    COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>"
    COMPILE_DEFINITIONS WEBP_USE_THREAD)

# ─── Warning flags ────────────────────────────────────────────────────────────
# Probe -Wno-invalid-feature-combination (used in the warnings target below): the group
# only exists since clang 18. check_cxx_compiler_flag's fail-regex catches the unknown
# -Wno- group diagnostic on older clangs, leaving the variable false there. Clang-family
# only: GCC never needs the flag.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-Wno-invalid-feature-combination RASTERMINAL_HAS_WNO_IFC)
endif()

add_library(rasterminal_warnings INTERFACE)
# Warning flags are gated to C++ sources (COMPILE_LANGUAGE:CXX): the C sources (the
# vendored zstd and miniz amalgams and the libwebp decode subset) must not receive the
# C++-only -W flags (they would trip the driver's "valid for C++ but not C" diagnostics
# under -Werror). They are suppressed via the per-source -w sets above.
target_compile_options(rasterminal_warnings INTERFACE
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:
    -Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wsign-conversion;
    -Wold-style-cast;-Wcast-align;-Wunused;-Woverloaded-virtual;
    -Wnon-virtual-dtor;-Wnull-dereference;-Wdouble-promotion;
    -Wformat=2;-Wimplicit-fallthrough;-Wmisleading-indentation;-Werror>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:
    -Wlogical-op;-Wduplicated-cond;-Wduplicated-branches>
  # Clang's -march=native on AVX10-capable CPUs (e.g. 2026 GitHub runners) emits
  # "+avx10.1-256; will be promoted to avx10.1-512" per TU (a frontend warning from X86
  # target-feature init, not the driver), fatal under -Werror. The promotion is benign
  # on such CPUs (they carry full 512-bit AVX-512, which is what makes the combination
  # "invalid"); suppress it rather than drop -march=native. Gated on the probed
  # RASTERMINAL_HAS_WNO_IFC (see above): the group only exists since clang 18, and clang
  # makes an unknown -Wno- group itself fatal under -Werror (-Wunknown-warning-option).
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<BOOL:${RASTERMINAL_HAS_WNO_IFC}>>:-Wno-invalid-feature-combination>
  # -Wuseless-cast is word-size-dependent (a cast that narrows/widens at LP64 is a no-op at
  # ILP32); keep it on 64-bit, drop it on 32-bit. See the ILP32 block near the top.
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>,$<NOT:$<BOOL:${RASTERMINAL_ILP32}>>>:
    -Wuseless-cast>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/W4;/WX;/permissive->
  $<$<CXX_COMPILER_ID:MSVC>:/D_CRT_SECURE_NO_WARNINGS>)

# ─── Release optimisation flags ───────────────────────────────────────────────
# Tier 1: CPU-agnostic speed flags, applied in all non-Debug configs (Release, Portable,
# RelWithDebInfo, MinSizeRel via the NOT:Debug gate). The appended -O3 overrides the
# config default for RelWithDebInfo (-O2) and MinSizeRel (-Os): both end up at -O3, by
# design (RelWithDebInfo is then -O3 -g, the accurate target for opt-dependent bug repro).
# -DNDEBUG comes from the config's own *_FLAGS (every non-Debug config defines it).
add_library(rasterminal_release_opts INTERFACE)
target_compile_options(rasterminal_release_opts INTERFACE
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:
    -O3;-funroll-loops;-ffast-math;-fno-finite-math-only;
    -ffunction-sections;-fdata-sections;
    -fno-rtti;-fomit-frame-pointer;-fstrict-aliasing;
    -fmerge-all-constants;-fvisibility=hidden;-fvisibility-inlines-hidden;
    -fno-stack-protector;-fno-asynchronous-unwind-tables;-pipe>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU>>:
    -fno-plt;-fno-semantic-interposition;-fno-stack-clash-protection;
    -fgcse-sm;-fgcse-las;-fipa-pta;-Wno-alloc-size-larger-than>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:MSVC>>:/O2;/fp:fast;/GR-;/GS-;/Gy>
  # Tier 2: machine-specific, omitted when RASTERMINAL_PORTABLE=ON.
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-march=native>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:MSVC>>:/arch:AVX2>)

# Linker dead-code GC: drops the sections emitted by -ffunction-sections/-fdata-sections
# (and /Gy) that nothing references. LTO + -fvisibility=hidden already strip unused C++;
# this mainly reaps the non-LTO C decode TUs (libwebp/zstd). macOS ld64 spells --gc-sections
# as -dead_strip; MSVC's /OPT:REF removes unreferenced, /OPT:ICF folds identical functions.
# Applied (non-Debug) to both opt tiers so rasterminal and rasterminal_tests inherit it.
set(RASTERMINAL_GC_LINK_OPTS
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>,$<NOT:$<PLATFORM_ID:Darwin>>>:-Wl,--gc-sections>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<PLATFORM_ID:Darwin>>:-Wl,-dead_strip>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:MSVC>>:/OPT:REF;/OPT:ICF>)
target_link_options(rasterminal_release_opts INTERFACE ${RASTERMINAL_GC_LINK_OPTS})

# LTO: CMake picks the right form per compiler (-flto=auto / -flto=thin / /GL+/LTCG).
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
if(NOT ipo_ok)
  message(STATUS "IPO/LTO not supported: ${ipo_msg}")
endif()

# ─── Main binary ──────────────────────────────────────────────────────────────
add_executable(rasterminal
  src/main.cpp
  src/args.cpp
  src/terminal/color.cpp
  src/terminal/framebuffer.cpp
  src/terminal/hud.cpp
  src/terminal/graphics.cpp
  src/terminal/kitty.cpp
  src/terminal/sixel.cpp
  src/loaders/mesh.cpp
  src/loaders/mesh_obj.cpp
  src/loaders/mesh_ply.cpp
  src/loaders/mesh_stl.cpp
  src/loaders/mesh_gltf.cpp
  src/loaders/draco_decode.cpp
  src/render/camera.cpp
  src/render/rasterize.cpp
  src/render/renderer.cpp
  src/render/texture.cpp
  src/loaders/ktx2_decode.cpp
  src/loaders/webp_decode.cpp
  vendor/meshoptimizer/meshoptimizer_impl.cpp
  vendor/draco/draco_impl.cpp
  vendor/basisu/basisu_impl.cpp
  vendor/basisu/zstd/zstddeclib.c
  vendor/miniz/miniz.c
  ${WEBP_DEC_SRCS})
# Repo root on the include path so every include in src/ is root-relative
# ("src/render/renderer.h"), independent of which subdirectory the including file is in.
# Gated to C++ because target scope does NOT separate languages: the vendored C TUs are
# sources of this same target, and libwebp's internal includes are repo-rooted
# ("src/dsp/dsp.h"), so handing them the repo root lets a src/ subdirectory shadow one.
# Matches the Makefile, where $(ROOT_INC) is in the C++ flag sets only.
target_include_directories(rasterminal PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:${PROJECT_SOURCE_DIR}>)
target_link_libraries(rasterminal PRIVATE
  rasterminal_vendor
  rasterminal_warnings
  rasterminal_release_opts
  Threads::Threads
  ${RASTERMINAL_SHM_LIB})
# GCC's LTO re-emits -Wmaybe-uninitialized from Draco's edgebreaker templates during
# the link-step recompile, where per-TU pragma context (src/loaders/draco_decode.cpp +
# vendor/draco/draco_impl.cpp) doesn't reach. Suppress only at link time so other
# TUs keep -Wmaybe-uninitialized active at compile.
target_link_options(rasterminal PRIVATE
  $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>
  # Statically link libstdc++/libgcc for release artifacts (Linux GNU/Clang only).
  $<$<AND:$<BOOL:${RASTERMINAL_STATIC_LIBSTDCXX}>,$<CXX_COMPILER_ID:GNU,Clang>,$<NOT:$<PLATFORM_ID:Darwin>>>:-static-libstdc++;-static-libgcc>)
if(ipo_ok)
  set_property(TARGET rasterminal PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  set_property(TARGET rasterminal PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  set_property(TARGET rasterminal PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
endif()

# ─── Test binary ──────────────────────────────────────────────────────────────
if(RASTERMINAL_BUILD_TESTS)
  # Tests drop -fno-rtti (test framework needs RTTI) and the aggressive
  # GCC-only opts, but keep safety flags (-fstack-protector-strong).
  add_library(rasterminal_test_opts INTERFACE)
  target_compile_options(rasterminal_test_opts INTERFACE
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:
      -O3;-funroll-loops;-ffast-math;-fno-finite-math-only;
      -ffunction-sections;-fdata-sections;
      -fomit-frame-pointer;-fstrict-aliasing;
      -fstack-protector-strong;-D_FORTIFY_SOURCE=2;-pipe>
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:MSVC>>:/O2;/fp:fast;/GS;/Gy>
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-march=native>
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:MSVC>>:/arch:AVX2>)
  target_link_options(rasterminal_test_opts INTERFACE ${RASTERMINAL_GC_LINK_OPTS})

  add_executable(rasterminal_tests
    tests/test_main.cpp
    tests/test_dispatch.cpp
    tests/test_args.cpp
    tests/test_platform.cpp
    tests/terminal/test_framebuffer.cpp
    tests/terminal/test_graphics.cpp
    tests/terminal/test_kitty.cpp
    tests/terminal/test_sixel.cpp
    tests/terminal/test_text.cpp
    tests/terminal/test_hud.cpp
    tests/loaders/test_obj.cpp
    tests/loaders/test_ply.cpp
    tests/loaders/test_stl.cpp
    tests/loaders/test_gltf.cpp
    tests/loaders/test_gltf_topology.cpp
    tests/loaders/test_gltf_vertex_colors.cpp
    tests/loaders/test_gltf_textures.cpp
    tests/loaders/test_gltf_texcoord1.cpp
    tests/loaders/test_gltf_texture_transform.cpp
    tests/loaders/test_gltf_draco.cpp
    tests/loaders/test_gltf_meshopt.cpp
    tests/loaders/test_gltf_ktx2.cpp
    tests/loaders/test_gltf_webp.cpp
    tests/loaders/test_mesh_geometry.cpp
    tests/loaders/test_mesh_vcache.cpp
    tests/texture/test_texture.cpp
    tests/texture/test_ktx2.cpp
    tests/texture/test_webp.cpp
    tests/rasterize/test_rasterize.cpp
    tests/rasterize/test_rasterize_texture.cpp
    tests/rasterize/test_rasterize_vcol.cpp
    tests/rasterize/test_rasterize_alpha.cpp
    tests/rasterize/test_rasterize_emissive.cpp
    tests/rasterize/test_rasterize_tonemap.cpp
    tests/renderer/test_renderer.cpp
    tests/renderer/test_renderer_ao_clip.cpp
    tests/renderer/test_renderer_vcol.cpp
    tests/renderer/test_renderer_misc.cpp
    tests/renderer/test_renderer_unlit.cpp
    tests/renderer/test_renderer_tiled.cpp
    tests/renderer/test_transparency.cpp
    tests/pipeline/test_camera.cpp
    tests/pipeline/test_clip.cpp
    tests/pipeline/test_clip_near.cpp
    tests/math/test_fastmath.cpp
    tests/math/test_linalg.cpp
    tests/math/test_light.cpp
    src/args.cpp
    src/terminal/color.cpp
    src/render/renderer.cpp
    src/loaders/mesh.cpp
    src/loaders/mesh_obj.cpp
    src/loaders/mesh_ply.cpp
    src/loaders/mesh_stl.cpp
    src/loaders/mesh_gltf.cpp
    src/loaders/draco_decode.cpp
    src/render/texture.cpp
    src/loaders/ktx2_decode.cpp
    src/loaders/webp_decode.cpp
    src/render/camera.cpp
    src/render/rasterize.cpp
    src/terminal/framebuffer.cpp
    src/terminal/hud.cpp
    src/terminal/graphics.cpp
    src/terminal/kitty.cpp
    src/terminal/sixel.cpp
    vendor/meshoptimizer/meshoptimizer_impl.cpp
    vendor/draco/draco_impl.cpp
    vendor/basisu/basisu_impl.cpp
    vendor/basisu/zstd/zstddeclib.c
    vendor/miniz/miniz.c
    ${WEBP_DEC_SRCS})
  # Repo root on the include path so every test include is root-relative:
  # "src/foo.h" for code under test, "tests/foo.h" for shared helpers/fixtures
  # (which stay at tests/ root while sources nest under tests/<subsystem>/).
  # C++-gated for the same reason as the rasterminal target above.
  target_include_directories(rasterminal_tests PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${PROJECT_SOURCE_DIR}>)
  target_link_libraries(rasterminal_tests PRIVATE
    rasterminal_vendor
    rasterminal_warnings
    rasterminal_test_opts
    Threads::Threads
    ${RASTERMINAL_SHM_LIB})
  target_link_options(rasterminal_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>)
  if(ipo_ok)
    set_property(TARGET rasterminal_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET rasterminal_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    set_property(TARGET rasterminal_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
  endif()

  enable_testing()
  # Run from the repo root so relative model paths (models/…) resolve correctly.
  add_test(NAME rasterminal_tests
           COMMAND rasterminal_tests
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ─── Install ──────────────────────────────────────────────────────────────────
# Parity with the Makefile's `install` target. GNUInstallDirs supplies the standard,
# overridable CMAKE_INSTALL_* paths (DESTDIR-aware for fakeroot staging). The man page
# and THIRD_PARTY_NOTICES ship per the vendored licenses; docs land in a package-specific
# subdir (CMAKE_INSTALL_DOCDIR = share/doc/rasterminal). No uninstall target: CMake has
# no native one (the Makefile keeps the convenience `make uninstall`).
include(GNUInstallDirs)
install(TARGETS rasterminal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES man/rasterminal.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES THIRD_PARTY_NOTICES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
