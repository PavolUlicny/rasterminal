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

# Prefer the -pthread flag over a bare -lpthread where Threads has to name a library at all.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
# On modern glibc Threads finds pthread inside libc and then contributes NO flag, so the
# preference above never fires and -pthread reaches neither the compile nor the link, so spell
# it out and let the two travel together. Compile-side it is close to a no-op on glibc (it
# defines _REENTRANT), but not everywhere. C++ only: the vendored C TUs are built without it.
add_library(rasterminal_threads INTERFACE)
target_link_libraries(rasterminal_threads INTERFACE Threads::Threads)
target_compile_options(rasterminal_threads INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-pthread>)
target_link_options(rasterminal_threads INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-pthread>)

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
# (MSVC), or a native 32-bit toolchain. Applied
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

# The basisu transcoder shim is a large, unaudited vendored TU that trips a long,
# compiler-specific warning set; suppress all its warnings per-source rather than
# maintaining a fragile pragma list. The vendored C
# TUs get the same treatment at target level, on rasterminal_c below.
set_source_files_properties(
  vendor/basisu/basisu_impl.cpp
  PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
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
# decode stays correct, just SSE2-only (decode is load-time, not hot-path).
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

# ─── Vendored C sources ───────────────────────────────────────────────────────
# Compiled once here and linked into both executables, rather than listed as sources of
# each. Their flag set is deliberately NOT the C++ one: no LTO, no C++-only options, and
# none of the aggressive tier src/ opts into. Three reasons.
#
# LTO is the load-bearing one. INTERPROCEDURAL_OPTIMIZATION applies per LANGUAGE, so a
# target carrying both would hand these TUs the C compiler's LTO form while C++ gets the
# C++ compiler's. `cmake -DCMAKE_CXX_COMPILER=clang++` alone (CMAKE_C_COMPILER stays at
# `cc`, gcc on most distros) then produced GIMPLE C objects that a clang link cannot read:
# "plugin needed to handle lto object", then undefined references to every C symbol. Off
# on MSVC too, for uniformity; the lost /GL costs close to nothing, since webp and zstd
# decode at load time and miniz's deflate feeds only the kitty transport.
#
# Second, the C++-only options (-fno-rtti and friends) draw a "valid for C++ but not for C"
# diagnostic per TU that only the blanket -w was hiding. Third, an OBJECT library compiles
# these 51 files once for the whole build instead of once per executable. OBJECT rather
# than STATIC so every object still reaches the link: an archive would let the linker
# decide which members survive.
add_library(rasterminal_c OBJECT
  vendor/basisu/zstd/zstddeclib.c
  vendor/miniz/miniz.c
  ${WEBP_DEC_SRCS})
# MSVC gets no C standard: nothing has ever set one there, so leaving it at the default
# keeps Windows codegen where it is.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
  set_target_properties(rasterminal_c PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endif()
# libwebp's internal includes are repo-rooted ("src/dsp/dsp.h"), so it needs its own root
# on the path. Deliberately just that one: zstd and miniz are self-contained, and handing
# the C TUs the whole vendor set would hide what they actually depend on.
target_include_directories(rasterminal_c SYSTEM PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libwebp)
# WEBP_USE_THREAD mutex-guards libwebp's lazy SIMD function-pointer init (cpu.h): texture
# decode runs on worker threads, so without it the first concurrent WebP decode races on the
# global DSP tables. cpu.h gates the mutex on `WEBP_USE_THREAD && !_WIN32`, so it engages
# everywhere except MSVC/_WIN32: there the unsynchronized path (and thus the race) remains,
# matching upstream's own posture. That residual race is benign: idempotent writes of the same
# SIMD impl, and aligned-pointer stores are atomic on the targeted ISAs. zstd and miniz are
# self-contained and ignore it.
target_compile_definitions(rasterminal_c PRIVATE WEBP_USE_THREAD)
# The speed tier mirrors the C++ one's CPU-agnostic half so these TUs are not left at a bare
# -O3, and stops there: everything below that line is either C++-only or an aggressive option
# src/ opts into for its own code and these unaudited amalgams do not.
target_compile_options(rasterminal_c PRIVATE
  $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:
    -O3;-funroll-loops;-ffast-math;-fno-finite-math-only;
    -ffunction-sections;-fdata-sections;-pipe>
  $<$<AND:$<CONFIG:Debug>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:-O0;-pipe>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<C_COMPILER_ID:MSVC>>:/O2;/fp:fast;/GS-;/Gy>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:-march=native>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<C_COMPILER_ID:MSVC>>:/arch:AVX2>)

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
# Only the two C++ executables link this. The vendored C TUs build in rasterminal_c, which
# does not, so none of these C++-only -W flags can reach a C compile and trip the driver's
# "valid for C++ but not for C" diagnostic under -Werror.
target_compile_options(rasterminal_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
    -Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wsign-conversion;
    -Wold-style-cast;-Wcast-align;-Wunused;-Woverloaded-virtual;
    -Wnon-virtual-dtor;-Wnull-dereference;-Wdouble-promotion;
    -Wformat=2;-Wimplicit-fallthrough;-Wmisleading-indentation;-Werror>
  $<$<CXX_COMPILER_ID:GNU>:
    -Wlogical-op;-Wduplicated-cond;-Wduplicated-branches>
  # Clang's -march=native on AVX10-capable CPUs (e.g. 2026 GitHub runners) emits
  # "+avx10.1-256; will be promoted to avx10.1-512" per TU (a frontend warning from X86
  # target-feature init, not the driver), fatal under -Werror. The promotion is benign
  # on such CPUs (they carry full 512-bit AVX-512, which is what makes the combination
  # "invalid"); suppress it rather than drop -march=native. Gated on the probed
  # RASTERMINAL_HAS_WNO_IFC (see above): the group only exists since clang 18, and clang
  # makes an unknown -Wno- group itself fatal under -Werror (-Wunknown-warning-option).
  $<$<BOOL:${RASTERMINAL_HAS_WNO_IFC}>:-Wno-invalid-feature-combination>
  # -Wuseless-cast is word-size-dependent (a cast that narrows/widens at LP64 is a no-op at
  # ILP32); keep it on 64-bit, drop it on 32-bit. See the ILP32 block near the top.
  $<$<AND:$<CXX_COMPILER_ID:GNU>,$<NOT:$<BOOL:${RASTERMINAL_ILP32}>>>:-Wuseless-cast>
  $<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive->
  $<$<CXX_COMPILER_ID:MSVC>:/D_CRT_SECURE_NO_WARNINGS>)

# ─── Release optimisation flags ───────────────────────────────────────────────
# Tier 1: CPU-agnostic speed flags, applied in all non-Debug configs (Release, Portable,
# RelWithDebInfo, MinSizeRel via the NOT:Debug gate). The appended -O3 overrides the
# config default for RelWithDebInfo (-O2) and MinSizeRel (-Os): both end up at -O3, by
# design (RelWithDebInfo is then -O3 -g, the accurate target for opt-dependent bug repro).
# -DNDEBUG comes from the config's own *_FLAGS (every non-Debug config defines it).
set(RASTERMINAL_RELEASE_OPTS
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
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:MSVC>>:/arch:AVX2>
  # Debug's -O0 is what CMake's bare -g config flags already imply; spelled out so a reader
  # comparing the two build systems does not have to know that.
  $<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-O0>)

add_library(rasterminal_release_opts INTERFACE)
target_compile_options(rasterminal_release_opts INTERFACE ${RASTERMINAL_RELEASE_OPTS})

# Linker dead-code GC: drops the sections emitted by -ffunction-sections/-fdata-sections
# (and /Gy) that nothing references. LTO + -fvisibility=hidden already strip unused C++;
# this mainly reaps the non-LTO C decode TUs (libwebp/zstd). macOS ld64 spells --gc-sections
# as -dead_strip; MSVC's /OPT:REF removes unreferenced, /OPT:ICF folds identical functions.
# Applied (non-Debug) to both opt tiers so rasterminal and rasterminal_tests inherit it.
set(RASTERMINAL_GC_LINK_OPTS
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>,$<NOT:$<PLATFORM_ID:Darwin>>>:-Wl,--gc-sections>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<PLATFORM_ID:Darwin>>:-Wl,-dead_strip>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:MSVC>>:/OPT:REF;/OPT:ICF>)

# The optimisation flags go on the LINK line as well, because under LTO that is where code
# generation happens, and GCC's manual asks for the same options there. Without this the
# compile-time tier does not reach the code the linker actually emits. Verified against the
# Makefile before it was removed: the two produced the same code, function for function.
# Clang then reports the compile-only members of the set as unused command-line arguments,
# harmless but noisy, so silence that one group at link. GCC does not warn.
set(RASTERMINAL_LTO_LINK_OPTS
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:Clang,AppleClang>>:-Wno-unused-command-line-argument>)
target_link_options(rasterminal_release_opts INTERFACE
  ${RASTERMINAL_GC_LINK_OPTS} ${RASTERMINAL_RELEASE_OPTS} ${RASTERMINAL_LTO_LINK_OPTS})

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
  $<TARGET_OBJECTS:rasterminal_c>)
# Repo root on the include path so every include in src/ is root-relative
# ("src/render/renderer.h"), independent of which subdirectory the including file is in.
# C++ only: libwebp's internal includes are repo-rooted
# ("src/dsp/dsp.h"), so a src/ subdirectory would shadow one. Nothing gates it here because
# the C TUs are no longer sources of this target; they build in rasterminal_c.
target_include_directories(rasterminal PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(rasterminal PRIVATE
  rasterminal_vendor
  rasterminal_warnings
  rasterminal_release_opts
  rasterminal_threads
  ${RASTERMINAL_SHM_LIB})
# GCC's LTO re-emits -Wmaybe-uninitialized from Draco's edgebreaker templates during
# the link-step recompile, where per-TU pragma context (src/loaders/draco_decode.cpp +
# vendor/draco/draco_impl.cpp) doesn't reach. Suppress only at link time so other
# TUs keep -Wmaybe-uninitialized active at compile.
target_link_options(rasterminal PRIVATE
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU>>:-Wno-maybe-uninitialized>
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
  set(RASTERMINAL_TEST_OPTS
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:
      -O3;-funroll-loops;-ffast-math;-fno-finite-math-only;
      -ffunction-sections;-fdata-sections;
      -fomit-frame-pointer;-fstrict-aliasing;
      -fstack-protector-strong;-D_FORTIFY_SOURCE=2;-pipe>
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:MSVC>>:/O2;/fp:fast;/GS;/Gy>
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-march=native>
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:MSVC>>:/arch:AVX2>
    $<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-O0>)

  add_library(rasterminal_test_opts INTERFACE)
  target_compile_options(rasterminal_test_opts INTERFACE ${RASTERMINAL_TEST_OPTS})
  # Same at link as for rasterminal: LTO does codegen there.
  target_link_options(rasterminal_test_opts INTERFACE
    ${RASTERMINAL_GC_LINK_OPTS} ${RASTERMINAL_TEST_OPTS} ${RASTERMINAL_LTO_LINK_OPTS})

  # EXCLUDE_FROM_ALL so a plain `cmake --build` produces the viewer and nothing else, the way
  # a plain `make` did. Building the suite means a second copy of every src/ and vendored C++
  # TU on top of its own 45, which is most of the build under LTO and pure waste for anyone
  # who only wants the binary. Build it by name, or through the `check` target below.
  add_executable(rasterminal_tests EXCLUDE_FROM_ALL
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
    $<TARGET_OBJECTS:rasterminal_c>)
  # Repo root on the include path so every test include is root-relative:
  # "src/foo.h" for code under test, "tests/foo.h" for shared helpers/fixtures
  # (which stay at tests/ root while sources nest under tests/<subsystem>/).
  target_include_directories(rasterminal_tests PRIVATE ${PROJECT_SOURCE_DIR})
  target_link_libraries(rasterminal_tests PRIVATE
    rasterminal_vendor
    rasterminal_warnings
    rasterminal_test_opts
    rasterminal_threads
    ${RASTERMINAL_SHM_LIB})
  target_link_options(rasterminal_tests PRIVATE
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU>>:-Wno-maybe-uninitialized>)
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

  # `cmake --build <dir> --target check` builds the suite and runs it in one command.
  # $<CONFIG> rather than a fixed name so it also works under the
  # multi-config generators, where ctest needs to be told which config to run.
  add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL)
  add_dependencies(check rasterminal_tests)
endif()

# ─── Install ──────────────────────────────────────────────────────────────────
# GNUInstallDirs supplies the standard,
# overridable CMAKE_INSTALL_* paths (DESTDIR-aware for fakeroot staging). The man page
# and THIRD_PARTY_NOTICES ship per the vendored licenses; docs land in a package-specific
# subdir (CMAKE_INSTALL_DOCDIR = share/doc/rasterminal).
include(GNUInstallDirs)
install(TARGETS rasterminal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES man/rasterminal.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES THIRD_PARTY_NOTICES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# CMake ships no uninstall target, so this walks the install_manifest.txt the install step
# writes. That removes exactly what THIS build directory installed, wherever it went, which
# is stricter than a fixed list of paths: a prefix change between install and uninstall
# cannot leave files behind or delete someone else's.
# DESTDIR is read from the environment by the script, so a staged install is undone the way
# it was made: DESTDIR=/tmp/pkg cmake --build <dir> --target uninstall.
add_custom_target(uninstall
  COMMAND ${CMAKE_COMMAND}
          -DRASTERMINAL_DOCDIR=${CMAKE_INSTALL_FULL_DOCDIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  USES_TERMINAL)
