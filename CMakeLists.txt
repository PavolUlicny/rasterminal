cmake_minimum_required(VERSION 3.16)
project(rasterminal CXX)

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
foreach(lib cgltf stb stl_reader tinyobjloader tinyply)
  target_include_directories(rasterminal_vendor SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/${lib})
endforeach()

# ─── Warning flags ────────────────────────────────────────────────────────────
add_library(rasterminal_warnings INTERFACE)
target_compile_options(rasterminal_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
    -Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wsign-conversion;
    -Wold-style-cast;-Wcast-align;-Wunused;-Woverloaded-virtual;
    -Wnon-virtual-dtor;-Wnull-dereference;-Wdouble-promotion;
    -Wformat=2;-Wimplicit-fallthrough;-Wmisleading-indentation;-Werror>
  $<$<CXX_COMPILER_ID:GNU>:
    -Wlogical-op;-Wduplicated-cond;-Wduplicated-branches;-Wuseless-cast>
  $<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive-;/D_CRT_SECURE_NO_WARNINGS>)

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
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:MSVC>>:/O2;/Oi;/Ot;/fp:fast;/GR->
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
  src/camera.cpp
  src/shadow.cpp
  src/rasterize.cpp
  src/renderer.cpp
  src/texture.cpp)
target_link_libraries(rasterminal PRIVATE
  rasterminal_vendor
  rasterminal_warnings
  rasterminal_release_opts
  Threads::Threads)
if(ipo_ok)
  set_property(TARGET rasterminal PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  set_property(TARGET rasterminal PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  set_property(TARGET rasterminal PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
endif()

# ─── Test binary ──────────────────────────────────────────────────────────────
if(RASTERMINAL_BUILD_TESTS)
  # Tests drop -fno-rtti (test framework uses try/catch) and the aggressive
  # GCC-only opts, but keep safety flags (-fstack-protector-strong).
  add_library(rasterminal_test_opts INTERFACE)
  target_compile_options(rasterminal_test_opts INTERFACE
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:
      -O3;-funroll-loops;-ffast-math;-fno-finite-math-only;
      -fomit-frame-pointer;-fstrict-aliasing;
      -fstack-protector-strong;-D_FORTIFY_SOURCE=2;-pipe>
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:MSVC>>:/O2;/Oi;/Ot;/fp:fast;/GS>
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
    tests/test_mesh_geometry.cpp
    tests/test_args.cpp
    tests/test_shadow.cpp
    tests/test_rasterize.cpp
    tests/test_gltf.cpp
    src/args.cpp
    src/mesh.cpp
    src/mesh_obj.cpp
    src/mesh_ply.cpp
    src/mesh_stl.cpp
    src/mesh_gltf.cpp
    src/texture.cpp
    src/camera.cpp
    src/rasterize.cpp
    src/framebuffer.cpp
    src/shadow.cpp)
  target_link_libraries(rasterminal_tests PRIVATE
    rasterminal_vendor
    rasterminal_warnings
    rasterminal_test_opts
    Threads::Threads)
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
