cmake_minimum_required(VERSION 3.22)
project(rasterminal CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RASTERMINAL_PORTABLE
       "Build for distribution: omit machine-specific opts (-march=native / /arch:AVX2)"
       OFF)
option(RASTERMINAL_BUILD_TESTS "Build the test suite" ON)
# Static libstdc++ and libgcc remove their host ABI requirements, but glibc stays dynamic.
# The dist preset combines this option with portable code generation.
option(RASTERMINAL_STATIC_LIBSTDCXX
       "Statically link libstdc++/libgcc into the binary (Linux release artifacts)"
       OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
# Modern glibc may make Threads::Threads empty. Pass -pthread explicitly so its compile and
# link semantics remain paired on GNU-compatible compilers.
add_library(rasterminal_threads INTERFACE)
target_link_libraries(rasterminal_threads INTERFACE Threads::Threads)
target_compile_options(rasterminal_threads INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-pthread>)
target_link_options(rasterminal_threads INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-pthread>)

# shm_open may live in libc or librt. Link-test both forms because macOS and musl have no librt.
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
      message(FATAL_ERROR "shm_open could not be linked from libc or librt")
    endif()
  endif()
endif()

# 32-bit targets
# CMAKE_SIZEOF_VOID_P reflects native and cross-compiled target width.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(RASTERMINAL_ILP32 ON) # gates -Wuseless-cast off below (word-size-dependent warning)
  # x86-32 defaults to x87 arithmetic, whose extended precision changes renderer results.
  # Gate SSE2 on __i386__ because ARM ILP32 rejects these flags. MSVC already uses SSE2.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("#ifndef __i386__\n#error not i386\n#endif\nint main(){return 0;}"
                              RASTERMINAL_X86_32)
    if(RASTERMINAL_X86_32)
      add_compile_options(-msse2 -mfpmath=sse)
    endif()
  endif()
endif()

# Use one off_t ABI across every translation unit. This also permits files of 2 GB or more on
# 32-bit glibc. The define is inert where off_t is already 64-bit and on MSVC.
add_compile_definitions(_FILE_OFFSET_BITS=64)

# Vendored libraries
add_library(rasterminal_vendor INTERFACE)
foreach(lib cgltf stb stl_reader tinyobjloader tinyply meshoptimizer/src draco/src
            basisu/transcoder basisu/zstd libwebp miniz)
  target_include_directories(rasterminal_vendor SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/${lib})
endforeach()

# Suppress warnings for the vendored basisu implementation as a unit.
set_source_files_properties(
  vendor/basisu/basisu_impl.cpp
  PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")

# Assimp is static and import-only. Native formats remain exclusive to their loaders.
# Function scope contains Assimp's variables and must precede the global miniz definitions.
function(rasterminal_add_assimp)
  set(BUILD_SHARED_LIBS OFF)
  set(ASSIMP_HUNTER_ENABLED OFF)
  set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT ON)
  set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF)
  set(ASSIMP_DOUBLE_PRECISION OFF)
  set(ASSIMP_NO_EXPORT ON)
  set(ASSIMP_BUILD_ASSIMP_TOOLS OFF)
  set(ASSIMP_BUILD_SAMPLES OFF)
  set(ASSIMP_BUILD_TESTS OFF)
  set(ASSIMP_BUILD_DOCS OFF)
  set(ASSIMP_INSTALL OFF)
  set(ASSIMP_WARNINGS_AS_ERRORS OFF)
  set(ASSIMP_BUILD_USE_CCACHE OFF)
  set(ASSIMP_INJECT_DEBUG_POSTFIX OFF)
  set(ASSIMP_IGNORE_GIT_HASH ON)
  set(ASSIMP_BUILD_ZLIB ON)
  set(ZLIB_FOUND FALSE)
  set(ASSIMP_BUILD_DRACO OFF)
  set(ASSIMP_BUILD_NONFREE_C4D_IMPORTER OFF)
  set(ASSIMP_BUILD_M3D_IMPORTER OFF)
  set(ASSIMP_BUILD_USD_IMPORTER OFF)
  set(ASSIMP_BUILD_VRML_IMPORTER OFF)
  set(ASSIMP_BUILD_OBJ_IMPORTER OFF)
  set(ASSIMP_BUILD_PLY_IMPORTER OFF)
  set(ASSIMP_BUILD_STL_IMPORTER OFF)
  set(ASSIMP_BUILD_GLTF_IMPORTER OFF)
  add_subdirectory(vendor/assimp EXCLUDE_FROM_ALL)
  # Keep project IPO settings out of vendored code.
  set_property(TARGET assimp PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
  set_property(TARGET zlibstatic PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
  # zlib 1.2.13 mistakes Clang's TARGET_OS_MAC builtin for legacy Mac OS and breaks fdopen.
  if(APPLE AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    include(CheckCCompilerFlag)
    check_c_compiler_flag(-fno-define-target-os-macros RASTERMINAL_HAS_NO_TARGET_OS_MACROS)
    if(RASTERMINAL_HAS_NO_TARGET_OS_MACROS)
      target_compile_options(zlibstatic PRIVATE -fno-define-target-os-macros)
    endif()
  endif()
endfunction()
rasterminal_add_assimp()
# Every miniz consumer must see the same API configuration. The project uses only direct
# deflate and inflate, including mz_uncompress in tests.
add_compile_definitions(MINIZ_NO_ZLIB_COMPATIBLE_NAMES MINIZ_NO_STDIO MINIZ_NO_ARCHIVE_APIS)

# List libwebp sources separately because a unity include collides on file-local symbols.
# SIMD implementations self-gate on architecture macros. Portable builds keep the SSE2
# baseline and omit per-file SSE4.1 or AVX2 flags to preserve CPU-independent code generation.
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

# Vendored C sources
# Build these once with C-only flags and no LTO. Mixed GCC C and Clang C++ builds cannot link
# GCC LTO objects, and an object library ensures every decoder object reaches both executables.
add_library(rasterminal_c OBJECT
  vendor/basisu/zstd/zstddeclib.c
  vendor/miniz/miniz.c
  ${WEBP_DEC_SRCS})
# Preserve MSVC's default C mode.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
  set_target_properties(rasterminal_c PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endif()
# libwebp resolves internal includes from its own root.
target_include_directories(rasterminal_c SYSTEM PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libwebp)
# WEBP_USE_THREAD guards libwebp's lazy DSP initialization on POSIX. cpu.h excludes Windows,
# where concurrent initialization remains unsynchronized and writes the same function pointers.
target_compile_definitions(rasterminal_c PRIVATE WEBP_USE_THREAD)
# Vendored C code gets the common speed flags but none of the aggressive C++ flags.
target_compile_options(rasterminal_c PRIVATE
  $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:
    -O3;-funroll-loops;-ffast-math;-fno-finite-math-only;
    -ffunction-sections;-fdata-sections;-pipe>
  $<$<AND:$<CONFIG:Debug>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:-O0;-pipe>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<C_COMPILER_ID:MSVC>>:/O2;/fp:fast;/GS-;/Gy>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<C_COMPILER_ID:GNU,Clang,AppleClang>>:-march=native>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<C_COMPILER_ID:MSVC>>:/arch:AVX2>)

# Warning flags
# Clang added -Wno-invalid-feature-combination in version 18. Probe it before use.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-Wno-invalid-feature-combination RASTERMINAL_HAS_WNO_IFC)
endif()

add_library(rasterminal_warnings INTERFACE)
# Only C++ targets consume these flags.
target_compile_options(rasterminal_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
    -Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wsign-conversion;
    -Wold-style-cast;-Wcast-align;-Wunused;-Woverloaded-virtual;
    -Wnon-virtual-dtor;-Wnull-dereference;-Wdouble-promotion;
    -Wformat=2;-Wimplicit-fallthrough;-Wmisleading-indentation;-Werror>
  $<$<CXX_COMPILER_ID:GNU>:
    -Wlogical-op;-Wduplicated-cond;-Wduplicated-branches>
  # AVX10-capable hosts trigger a benign feature-promotion warning under -march=native.
  # An unknown warning option is itself fatal under -Werror, so use the probed flag.
  $<$<BOOL:${RASTERMINAL_HAS_WNO_IFC}>:-Wno-invalid-feature-combination>
  # The same cast can be meaningful on LP64 and redundant on ILP32.
  $<$<AND:$<CXX_COMPILER_ID:GNU>,$<NOT:$<BOOL:${RASTERMINAL_ILP32}>>>:-Wuseless-cast>
  $<$<CXX_COMPILER_ID:MSVC>:/W4;/WX;/permissive->
  $<$<CXX_COMPILER_ID:MSVC>:/D_CRT_SECURE_NO_WARNINGS>)

# Release optimization flags
# Every non-Debug configuration uses the same CPU-independent speed tier. Explicit -O3
# overrides the RelWithDebInfo and MinSizeRel defaults.
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
  # Portable builds omit the machine-specific tier.
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-march=native>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${RASTERMINAL_PORTABLE}>>,$<CXX_COMPILER_ID:MSVC>>:/arch:AVX2>
  $<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-O0>)

add_library(rasterminal_release_opts INTERFACE)
target_compile_options(rasterminal_release_opts INTERFACE ${RASTERMINAL_RELEASE_OPTS})

# Drop unused sections, especially from the non-LTO decoder objects.
set(RASTERMINAL_GC_LINK_OPTS
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>,$<NOT:$<PLATFORM_ID:Darwin>>>:-Wl,--gc-sections>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<PLATFORM_ID:Darwin>>:-Wl,-dead_strip>
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:MSVC>>:/OPT:REF;/OPT:ICF>)

# LTO performs code generation at link time, so the optimization tier must reach the linker.
# Clang warns about compile-only members there; suppress that warning at link time.
set(RASTERMINAL_LTO_LINK_OPTS
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:Clang,AppleClang>>:-Wno-unused-command-line-argument>)
target_link_options(rasterminal_release_opts INTERFACE
  ${RASTERMINAL_GC_LINK_OPTS} ${RASTERMINAL_RELEASE_OPTS} ${RASTERMINAL_LTO_LINK_OPTS})

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
if(NOT ipo_ok)
  message(STATUS "IPO/LTO not supported: ${ipo_msg}")
endif()

# Main binary
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
  src/loaders/mesh_assimp.cpp
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
# The repository root keeps project includes stable across source subdirectories.
target_include_directories(rasterminal PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(rasterminal PRIVATE
  assimp::assimp
  rasterminal_vendor
  rasterminal_warnings
  rasterminal_release_opts
  rasterminal_threads
  ${RASTERMINAL_SHM_LIB})
# GCC LTO loses Draco's per-file suppression during link-time recompilation. Keep the
# suppression on the link step so project compilation still checks -Wmaybe-uninitialized.
target_link_options(rasterminal PRIVATE
  $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU>>:-Wno-maybe-uninitialized>
  $<$<AND:$<BOOL:${RASTERMINAL_STATIC_LIBSTDCXX}>,$<CXX_COMPILER_ID:GNU,Clang>,$<NOT:$<PLATFORM_ID:Darwin>>>:-static-libstdc++;-static-libgcc>)
if(ipo_ok)
  set_property(TARGET rasterminal PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  set_property(TARGET rasterminal PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  set_property(TARGET rasterminal PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
endif()

# Test binary
if(RASTERMINAL_BUILD_TESTS)
  # The test framework needs RTTI. Tests also keep stack and fortify checks.
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
  target_link_options(rasterminal_test_opts INTERFACE
    ${RASTERMINAL_GC_LINK_OPTS} ${RASTERMINAL_TEST_OPTS} ${RASTERMINAL_LTO_LINK_OPTS})

  # The test target recompiles project and vendored C++ sources. Keep it out of viewer-only builds.
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
    tests/loaders/test_assimp.cpp
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
    src/loaders/mesh_assimp.cpp
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
  # Tests use repository-root includes for project code and shared fixtures.
  target_include_directories(rasterminal_tests PRIVATE ${PROJECT_SOURCE_DIR})
  target_link_libraries(rasterminal_tests PRIVATE
    assimp::assimp
    rasterminal_vendor
    rasterminal_warnings
    rasterminal_test_opts
    rasterminal_threads
    ${RASTERMINAL_SHM_LIB})
  target_link_options(rasterminal_tests PRIVATE
    $<$<AND:$<NOT:$<CONFIG:Debug>>,$<CXX_COMPILER_ID:GNU>>:-Wno-maybe-uninitialized>)
  if(NOT WIN32)
    # POSIX terminal integration tests launch the production executable in a pty.
    add_dependencies(rasterminal_tests rasterminal)
    target_compile_definitions(rasterminal_tests PRIVATE
      RASTERMINAL_TEST_BINARY="$<TARGET_FILE:rasterminal>")
  endif()
  if(ipo_ok)
    set_property(TARGET rasterminal_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET rasterminal_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    set_property(TARGET rasterminal_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
  endif()

  enable_testing()
  # A fixed working directory keeps relative test files independent of the build tree.
  add_test(NAME rasterminal_tests
           COMMAND rasterminal_tests
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  # $<CONFIG> makes the check target work with single-config and multi-config generators.
  add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL)
  add_dependencies(check rasterminal_tests)
endif()

# Install
include(GNUInstallDirs)
install(TARGETS rasterminal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES man/rasterminal.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES THIRD_PARTY_NOTICES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# The uninstall script follows install_manifest.txt and reads DESTDIR from the environment.
# Pass only the docdir name because --prefix may replace the configured install prefix.
get_filename_component(RASTERMINAL_DOCDIR_NAME "${CMAKE_INSTALL_DOCDIR}" NAME)
add_custom_target(uninstall
  COMMAND ${CMAKE_COMMAND}
          -DRASTERMINAL_DOCDIR_NAME=${RASTERMINAL_DOCDIR_NAME}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  USES_TERMINAL)
