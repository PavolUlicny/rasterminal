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

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

# ─── Vendored libraries ───────────────────────────────────────────────────────
# SYSTEM suppresses warnings from these headers on all compilers.
add_library(rasterminal_vendor INTERFACE)
foreach(lib cgltf stb stl_reader tinyobjloader tinyply meshoptimizer/src draco/src
            basisu/transcoder basisu/zstd)
  target_include_directories(rasterminal_vendor SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/${lib})
endforeach()

# The basisu transcoder shim and the zstd C amalgam are large, unaudited vendored TUs
# that trip a long, compiler-specific warning set; suppress all their warnings per-source
# (mirrors the Makefile's per-TU -w) rather than maintaining a fragile pragma list.
set_source_files_properties(
  vendor/basisu/basisu_impl.cpp
  vendor/basisu/zstd/zstddeclib.c
  PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")

# ─── Warning flags ────────────────────────────────────────────────────────────
add_library(rasterminal_warnings INTERFACE)
# Warning flags are gated to C++ sources (COMPILE_LANGUAGE:CXX): the only C source is the
# vendored zstd amalgam, which must not receive the C++-only -W flags (they would trip the
# driver's "valid for C++ but not C" diagnostics under -Werror). It is suppressed via the
# per-source -w set above.
target_compile_options(rasterminal_warnings INTERFACE
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:
    -Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wsign-conversion;
    -Wold-style-cast;-Wcast-align;-Wunused;-Woverloaded-virtual;
    -Wnon-virtual-dtor;-Wnull-dereference;-Wdouble-promotion;
    -Wformat=2;-Wimplicit-fallthrough;-Wmisleading-indentation;-Werror>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:
    -Wlogical-op;-Wduplicated-cond;-Wduplicated-branches;-Wuseless-cast>
  $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/W4;/WX;/permissive->
  $<$<CXX_COMPILER_ID:MSVC>:/D_CRT_SECURE_NO_WARNINGS>)

# ─── Release optimisation flags ───────────────────────────────────────────────
# Tier 1: CPU-agnostic speed flags — applied in both release and portable builds.
add_library(rasterminal_release_opts INTERFACE)
target_compile_options(rasterminal_release_opts INTERFACE
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:
    -O3;-funroll-loops;-ffast-math;-fno-finite-math-only;
    -fno-rtti;-fomit-frame-pointer;-fstrict-aliasing;
    -fmerge-all-constants;-fvisibility=hidden;
    -fno-stack-protector;-fno-asynchronous-unwind-tables;-pipe>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU>>:
    -fno-plt;-fno-semantic-interposition;-fno-stack-clash-protection;
    -fgcse-sm;-fgcse-las;-fipa-pta;-Wno-alloc-size-larger-than>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:MSVC>>:/O2;/fp:fast;/GR->
  # Tier 2: machine-specific — omitted when RASTERMINAL_PORTABLE=ON.
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-march=native>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:MSVC>>:/arch:AVX2>)

# LTO — CMake picks the right form per compiler (-flto=auto / -flto=thin / /GL+/LTCG).
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
if(NOT ipo_ok)
  message(STATUS "IPO/LTO not supported: ${ipo_msg}")
endif()

# ─── Main binary ──────────────────────────────────────────────────────────────
add_executable(rasterminal
  src/main.cpp
  src/args.cpp
  src/framebuffer.cpp
  src/mesh.cpp
  src/mesh_obj.cpp
  src/mesh_ply.cpp
  src/mesh_stl.cpp
  src/mesh_gltf.cpp
  src/draco_decode.cpp
  src/camera.cpp
  src/shadow.cpp
  src/rasterize.cpp
  src/renderer.cpp
  src/texture.cpp
  src/ktx2_decode.cpp
  vendor/meshoptimizer/meshoptimizer_impl.cpp
  vendor/draco/draco_impl.cpp
  vendor/basisu/basisu_impl.cpp
  vendor/basisu/zstd/zstddeclib.c)
target_link_libraries(rasterminal PRIVATE
  rasterminal_vendor
  rasterminal_warnings
  rasterminal_release_opts
  Threads::Threads)
# GCC's LTO re-emits -Wmaybe-uninitialized from Draco's edgebreaker templates during
# the link-step recompile, where per-TU pragma context (src/draco_decode.cpp +
# vendor/draco/draco_impl.cpp) doesn't reach. Suppress only at link time so other
# TUs keep -Wmaybe-uninitialized active at compile.
target_link_options(rasterminal PRIVATE
  $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>)
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
      -fomit-frame-pointer;-fstrict-aliasing;
      -fstack-protector-strong;-D_FORTIFY_SOURCE=2;-pipe>
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:MSVC>>:/O2;/fp:fast;/GS>
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-march=native>
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:MSVC>>:/arch:AVX2>)

  add_executable(rasterminal_tests
    tests/test_main.cpp
    tests/test_dispatch.cpp
    tests/test_obj.cpp
    tests/test_ply.cpp
    tests/test_stl.cpp
    tests/test_linalg.cpp
    tests/test_light.cpp
    tests/test_clip.cpp
    tests/test_camera.cpp
    tests/test_clip_near.cpp
    tests/test_texture.cpp
    tests/test_ktx2.cpp
    tests/test_framebuffer.cpp
    tests/test_mesh_geometry.cpp
    tests/test_mesh_vcache.cpp
    tests/test_args.cpp
    tests/test_shadow.cpp
    tests/test_rasterize.cpp
    tests/test_rasterize_texture.cpp
    tests/test_rasterize_vcol.cpp
    tests/test_rasterize_shadow.cpp
    tests/test_rasterize_alpha.cpp
    tests/test_rasterize_emissive.cpp
    tests/test_gltf.cpp
    tests/test_gltf_draco.cpp
    tests/test_gltf_meshopt.cpp
    tests/test_gltf_ktx2.cpp
    tests/test_renderer.cpp
    tests/test_renderer_ao_clip.cpp
    tests/test_renderer_shadow_depth.cpp
    tests/test_renderer_vcol.cpp
    tests/test_renderer_misc.cpp
    src/args.cpp
    src/renderer.cpp
    src/mesh.cpp
    src/mesh_obj.cpp
    src/mesh_ply.cpp
    src/mesh_stl.cpp
    src/mesh_gltf.cpp
    src/draco_decode.cpp
    src/texture.cpp
    src/ktx2_decode.cpp
    src/camera.cpp
    src/rasterize.cpp
    src/framebuffer.cpp
    src/shadow.cpp
    vendor/meshoptimizer/meshoptimizer_impl.cpp
    vendor/draco/draco_impl.cpp
    vendor/basisu/basisu_impl.cpp
    vendor/basisu/zstd/zstddeclib.c)
  target_link_libraries(rasterminal_tests PRIVATE
    rasterminal_vendor
    rasterminal_warnings
    rasterminal_test_opts
    Threads::Threads)
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
