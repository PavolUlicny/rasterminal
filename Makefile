CXX ?= g++

# ─── Compiler detection (POSIX: GCC or Clang) ─────────────────────────────────
IS_CLANG := $(findstring clang,$(shell $(CXX) --version 2>&1))

# ─── 32-bit (ILP32) target detection ──────────────────────────────────────────
# Probed from $(CXX) so it follows a native 32-bit toolchain (i686, armhf) AND CI's
# CXX='g++ -m32'. Two adjustments, scoped differently:
#  - SSE math, x86-32 only ($(ARCH32)): i686 defaults to the x87 FPU (80-bit intermediates),
#    whose results diverge from the SSE/x86-64 path the renderer and tests assume.
#    -msse2 -mfpmath=sse restores IEEE parity (SSE2 is universal on x86 since 2001; pre-SSE2
#    is a non-target). ARM ILP32 already uses IEEE VFP and would reject -msse2, so this is
#    gated on __i386__, not on pointer size.
#  - Drop -Wuseless-cast, any ILP32: "useless" is word-size-dependent; casts that
#    legitimately widen/narrow at LP64 become no-ops where size_t is 32-bit. Kept on 64-bit.
IS_X86_32 := $(filter 1,$(shell printf '__i386__\n' | $(CXX) -P -E -x c++ - 2>/dev/null | tail -1))
IS_ILP32  := $(filter 4,$(shell printf '__SIZEOF_POINTER__\n' | $(CXX) -P -E -x c++ - 2>/dev/null | tail -1))
ifeq ($(IS_X86_32),1)
ARCH32 := -msse2 -mfpmath=sse
endif

# shm_open (the kitty graphics shared-memory transport) lives in librt on glibc
# older than 2.34, where it moved into libc proper; macOS and musl always ship it
# in libc and have no librt to link. Same -P -E probe idiom as the ILP32 checks:
# -include limits.h pulls in the glibc version macros, any non-glibc expands the
# condition false and gets no flag. On every link line, incl. tests (the shm
# helpers are referenced from framebuffer.o regardless of which transport runs).
# (\043 is '#': a literal one cannot be written inside $(shell), where make's \# escape
# does not apply and the backslash would reach the preprocessor as a non-directive.)
NEEDS_LIBRT := $(shell printf '\043if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 34))\nlibrt\n\043endif\n' | $(CXX) -P -E -include limits.h -x c++ - 2>/dev/null | grep -x librt)
LIBRT := $(if $(filter librt,$(NEEDS_LIBRT)),-lrt)

# ─── Linker dead-code GC (drops unreferenced sections from the final binary) ──
# Paired with -ffunction-sections/-fdata-sections below. LTO + -fvisibility=hidden
# already strip unused C++; this mainly reaps the non-LTO C decode TUs (libwebp/zstd).
# macOS ld64 doesn't understand --gc-sections; it spells the same thing -dead_strip.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
GC_LINK   = -Wl,-dead_strip
# macOS links the ABI-stable system libc++; no static-libstdc++ story (or flag) there.
DIST_LINK =
else
GC_LINK   = -Wl,--gc-sections
# `dist` link-only flags: drop the build host's libstdc++/libgcc ABI dependency so a release
# binary doesn't fail on older distros with "GLIBCXX_… not found". (glibc itself stays dynamic;
# full distro-portability still needs an old-glibc/-static build; out of scope here.)
DIST_LINK = -static-libstdc++ -static-libgcc
endif

# ─── Large-file support ───────────────────────────────────────────────────────
# On 32-bit glibc, fopen refuses files >= 2 GB (EOVERFLOW) and off_t/ftello are
# 32-bit without this. No-op where off_t is already 64-bit (LP64 Linux, macOS).
# Applied to every C++ AND C flag set (all variants incl. debug) so off_t has one
# size across all TUs (mixed LFS defines are a classic off_t ABI footgun).
LFS = -D_FILE_OFFSET_BITS=64

# miniz configuration, one uniform set across every C and C++ TU that includes
# miniz.h (mixed config on a shared header is the same footgun as LFS):
# NO_ZLIB_COMPATIBLE_NAMES drops the zlib-name compatibility layer (Z_* macros
# plus, since miniz 3.1, static compress/deflate/crc32 wrapper definitions
# stamped into every including TU), NO_STDIO + NO_ARCHIVE_APIS drop the file-I/O
# and ZIP layers nothing calls (the stdio layer also carries a #pragma message
# note that -w cannot silence; since 3.1.0 it fires only where miniz lacks
# large-file support, e.g. 32-bit x86 Linux, the cross32 CI target).
# Inflate stays: the tests round-trip frames through mz_uncompress.
MINIZ = -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -DMINIZ_NO_STDIO -DMINIZ_NO_ARCHIVE_APIS

WARN_COMMON = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
              -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual \
              -Wnon-virtual-dtor -Wnull-dereference -Wdouble-promotion \
              -Wformat=2 -Wimplicit-fallthrough -Wmisleading-indentation

ifeq ($(IS_CLANG),clang)
# -Wno-invalid-feature-combination: clang's -march=native on AVX10-capable CPUs
# (e.g. 2026 GitHub runners) emits "+avx10.1-256; will be promoted to avx10.1-512"
# per TU (a frontend warning from X86 target-feature init, not the driver), fatal
# under -Werror. The promotion is benign on such CPUs (they carry full 512-bit
# AVX-512, which is what makes the combination "invalid"); suppress it rather than
# drop -march=native. The group only exists since clang 18, and clang makes an
# unknown -Wno- group itself fatal under -Werror (-Wunknown-warning-option), so
# probe support first. Clang-only: GCC has no such warning and silently ignores
# unknown -Wno- groups, so its branch needs neither the flag nor the probe.
HAS_WNO_IFC := $(shell $(CXX) -Werror -Wno-invalid-feature-combination -fsyntax-only -x c++ /dev/null >/dev/null 2>&1 && echo 1)
WARNINGS = $(WARN_COMMON) $(if $(HAS_WNO_IFC),-Wno-invalid-feature-combination)
LTO      = -flto=thin
GCC_OPTS =
else
WARNINGS = $(WARN_COMMON) -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast
# -fno-fat-lto-objects: emit thin (bytecode-only) LTO objects; faster LTO, smaller .o, no
# runtime/binary change. GCC-only here (Clang's -flto=thin has no fat-object concept).
LTO      = -flto=auto -fno-fat-lto-objects
GCC_OPTS = -fno-plt -fno-semantic-interposition -fno-stack-clash-protection \
           -fgcse-sm -fgcse-las -fipa-pta -Wno-alloc-size-larger-than
endif

# -Wuseless-cast (GCC-only, added above) is word-size-dependent: relax it on any ILP32 target.
ifeq ($(IS_ILP32),4)
WARNINGS := $(filter-out -Wuseless-cast,$(WARNINGS))
endif

# The repo root, so every include in src/ and tests/ is root-relative ("src/foo.h",
# "tests/foo.h") and no file's include lines depend on which subdirectory it sits in.
# C++ only: the vendored C TUs include no project headers.
ROOT_INC    = -I.
VENDOR_INC  = -isystem vendor/cgltf -isystem vendor/stb -isystem vendor/stl_reader \
              -isystem vendor/tinyobjloader -isystem vendor/tinyply \
              -isystem vendor/meshoptimizer/src -isystem vendor/draco/src \
              -isystem vendor/basisu/transcoder -isystem vendor/basisu/zstd \
              -isystem vendor/libwebp -isystem vendor/miniz
# Every vendored file a TU can include, found rather than listed, for the same reason
# as HDRS/TEST_HDRS below: an incomplete prerequisite list does not fail the build, it
# silently leaves stale objects behind. The list this replaces named 8 headers and none
# at all from libwebp, so a vendor refresh could relink without recompiling. `find`
# because the trees nest arbitrarily deep (draco/src/draco/...).
#
# Not just headers: the unity shims #include vendored SOURCES (basisu_impl.cpp pulls in
# basisu_transcoder.cpp, which pulls in ten .inc tables; meshoptimizer_impl.cpp pulls in
# its whole src/). Globbing only .h/.hpp left those out, so a basisu refresh that touched
# only a transcode table relinked the stale object and silently transcoded KTX2 with the
# previous tables. Naming the extensions rather than every file keeps a README edit from
# rebuilding the world.
#
# `:=` on all three of these, not `=`: a recursively-expanded variable re-runs its
# $(shell) at EVERY reference, and these are referenced by many pattern rules each
# (VENDOR_HDRS by eight, since the .c rules below take it too), so `=` paid for the
# find once per reference rather than once per invocation.
VENDOR_HDRS := $(shell find vendor -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.inc' \
                    -o -name '*.inl' -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' \) 2>/dev/null)

# Tier 1: fast flags safe for any CPU (both release and portable).
# -DNDEBUG strips assert()/library DCHECKs (in src + every vendored C/C++ lib) from all
# optimized builds, never set for debug, which keeps asserts live.
OPT_COMMON = -O3 $(LTO) -funroll-loops -ffast-math -fno-finite-math-only -DNDEBUG \
             -ffunction-sections -fdata-sections \
             -fno-rtti -fomit-frame-pointer -fstrict-aliasing \
             -fmerge-all-constants -fvisibility=hidden -fvisibility-inlines-hidden \
             -fno-stack-protector -fno-asynchronous-unwind-tables \
             -pipe -pthread $(GCC_OPTS) $(ARCH32) $(LFS) $(MINIZ)

# Tier 2: machine-specific (release only).
ARCH_NATIVE = -march=native

CXXFLAGS      = -std=c++17 $(WARNINGS) -Werror $(OPT_COMMON) $(ARCH_NATIVE) $(ROOT_INC) $(VENDOR_INC)
TEST_CXXFLAGS = -std=c++17 $(WARNINGS) -Werror -O3 $(LTO) $(ARCH_NATIVE) -funroll-loops \
                -ffast-math -fno-finite-math-only -DNDEBUG -ffunction-sections -fdata-sections \
                -fomit-frame-pointer -fstrict-aliasing \
                -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
                -pipe -pthread $(ROOT_INC) $(VENDOR_INC) $(ARCH32) $(LFS) $(MINIZ)
TARGET   = rasterminal

SRCS = src/main.cpp \
       src/args.cpp \
       src/terminal/color.cpp \
       src/terminal/framebuffer.cpp \
       src/terminal/hud.cpp \
       src/terminal/graphics.cpp \
       src/terminal/kitty.cpp \
       src/terminal/sixel.cpp \
       src/loaders/mesh.cpp \
       src/loaders/mesh_obj.cpp \
       src/loaders/mesh_ply.cpp \
       src/loaders/mesh_stl.cpp \
       src/loaders/mesh_gltf.cpp \
       src/loaders/draco_decode.cpp \
       src/render/camera.cpp \
       src/render/rasterize.cpp \
       src/render/renderer.cpp \
       src/render/texture.cpp \
       src/loaders/ktx2_decode.cpp \
       src/loaders/webp_decode.cpp \
       vendor/meshoptimizer/meshoptimizer_impl.cpp \
       vendor/draco/draco_impl.cpp \
       vendor/basisu/basisu_impl.cpp

# C sources, compiled as C with $(CC): must not go through the C++ flags.
#  - miniz amalgam: zlib deflate for the kitty graphics direct transport; configured
#    by the uniform $(MINIZ) define set above.
#  - zstd decode amalgam: used by the basisu transcoder for KTX2 Zstd supercompression.
#  - libwebp decode subset: EXT_texture_webp image decode. Listed individually (a unity
#    #include shim fails on duplicate file-local statics, e.g. clip_8b). The dsp SIMD
#    variants (sse2/sse41/avx2/neon) self-gate on arch macros and carry no per-file SIMD
#    flags: release (-march=native) activates all of them; portable activates only the
#    x86-64 SSE2 baseline (sse41/avx2 compile to empty stubs). libwebp ships CPUID runtime
#    dispatch that COULD safely carry sse41/avx2 in a portable binary via per-file
#    -msse4.1/-mavx2 (it picks kernels by actual CPU, so no SIGILL risk): we deliberately
#    forgo that to keep "portable = no arch-specific codegen" uniform across every TU.
#    Portable WebP decode is correct, just SSE2-only (decode is load-time, not hot-path).
#    The C flags' blanket -w covers their warnings, so no per-TU suppression needed here.
CSRCS = vendor/miniz/miniz.c \
        vendor/basisu/zstd/zstddeclib.c \
        vendor/libwebp/src/dec/alpha_dec.c \
        vendor/libwebp/src/dec/buffer_dec.c \
        vendor/libwebp/src/dec/frame_dec.c \
        vendor/libwebp/src/dec/idec_dec.c \
        vendor/libwebp/src/dec/io_dec.c \
        vendor/libwebp/src/dec/quant_dec.c \
        vendor/libwebp/src/dec/tree_dec.c \
        vendor/libwebp/src/dec/vp8_dec.c \
        vendor/libwebp/src/dec/vp8l_dec.c \
        vendor/libwebp/src/dec/webp_dec.c \
        vendor/libwebp/src/dsp/alpha_processing.c \
        vendor/libwebp/src/dsp/cpu.c \
        vendor/libwebp/src/dsp/dec.c \
        vendor/libwebp/src/dsp/dec_clip_tables.c \
        vendor/libwebp/src/dsp/filters.c \
        vendor/libwebp/src/dsp/lossless.c \
        vendor/libwebp/src/dsp/rescaler.c \
        vendor/libwebp/src/dsp/upsampling.c \
        vendor/libwebp/src/dsp/yuv.c \
        vendor/libwebp/src/dsp/alpha_processing_sse2.c \
        vendor/libwebp/src/dsp/dec_sse2.c \
        vendor/libwebp/src/dsp/filters_sse2.c \
        vendor/libwebp/src/dsp/lossless_sse2.c \
        vendor/libwebp/src/dsp/rescaler_sse2.c \
        vendor/libwebp/src/dsp/upsampling_sse2.c \
        vendor/libwebp/src/dsp/yuv_sse2.c \
        vendor/libwebp/src/dsp/alpha_processing_sse41.c \
        vendor/libwebp/src/dsp/dec_sse41.c \
        vendor/libwebp/src/dsp/lossless_sse41.c \
        vendor/libwebp/src/dsp/upsampling_sse41.c \
        vendor/libwebp/src/dsp/yuv_sse41.c \
        vendor/libwebp/src/dsp/lossless_avx2.c \
        vendor/libwebp/src/dsp/alpha_processing_neon.c \
        vendor/libwebp/src/dsp/dec_neon.c \
        vendor/libwebp/src/dsp/filters_neon.c \
        vendor/libwebp/src/dsp/lossless_neon.c \
        vendor/libwebp/src/dsp/rescaler_neon.c \
        vendor/libwebp/src/dsp/upsampling_neon.c \
        vendor/libwebp/src/dsp/yuv_neon.c \
        vendor/libwebp/src/utils/bit_reader_utils.c \
        vendor/libwebp/src/utils/color_cache_utils.c \
        vendor/libwebp/src/utils/filters_utils.c \
        vendor/libwebp/src/utils/huffman_utils.c \
        vendor/libwebp/src/utils/palette.c \
        vendor/libwebp/src/utils/quant_levels_dec_utils.c \
        vendor/libwebp/src/utils/rescaler_utils.c \
        vendor/libwebp/src/utils/random_utils.c \
        vendor/libwebp/src/utils/thread_utils.c \
        vendor/libwebp/src/utils/utils.c

# Header prerequisites, found rather than listed, unlike the source lists (which
# are enumerated so the Makefile and CMake stay comparable, and where an omission
# fails loudly at link). A header missing from a prerequisite list fails SILENTLY:
# the build succeeds and leaves stale objects behind. All three of these lists had
# already gone stale by hand, HDRS twice.
HDRS := $(shell find src -name '*.h' 2>/dev/null)
# Shared test helpers and binary fixtures. Found at any depth, not globbed at the
# two levels that happen to exist today: a prerequisite list that depends on a
# layout convention being followed is the same silent failure again.
TEST_HDRS := $(shell find tests -name '*.h' 2>/dev/null)

# LTO_SUPPRESS: per-compiler warning suppression needed only at the link step.
#  - GCC: LTO re-emits -Wmaybe-uninitialized from Draco's edgebreaker templates during
#    the link step's recompile, and per-TU pragma context (in src/loaders/draco_decode.cpp +
#    vendor/draco/draco_impl.cpp) doesn't survive that re-emit.
#  - Clang: the link rules pass the full CXXFLAGS (incl. compile-only math flags like
#    -fno-finite-math-only) to a link-only clang++ invocation; under ThinLTO AppleClang
#    intermittently reports those as -Wunused-command-line-argument, which -Werror makes
#    fatal. Suppress that one warning at link time only.
# Applied only at link: compile-time warnings stay active for every TU so real bugs
# are still caught by -Werror.
ifeq ($(IS_CLANG),clang)
LTO_SUPPRESS = -Wno-unused-command-line-argument
else
LTO_SUPPRESS = -Wno-maybe-uninitialized
endif

TEST_TARGET = rasterminal_tests
TEST_SRCS   = tests/test_main.cpp \
              tests/test_dispatch.cpp \
              tests/test_args.cpp \
              tests/test_platform.cpp \
              tests/test_text.cpp \
              tests/test_hud.cpp \
              tests/terminal/test_framebuffer.cpp \
              tests/terminal/test_graphics.cpp \
              tests/terminal/test_kitty.cpp \
              tests/terminal/test_sixel.cpp \
              tests/loaders/test_obj.cpp \
              tests/loaders/test_ply.cpp \
              tests/loaders/test_stl.cpp \
              tests/loaders/test_gltf.cpp \
              tests/loaders/test_gltf_topology.cpp \
              tests/loaders/test_gltf_vertex_colors.cpp \
              tests/loaders/test_gltf_textures.cpp \
              tests/loaders/test_gltf_texcoord1.cpp \
              tests/loaders/test_gltf_texture_transform.cpp \
              tests/loaders/test_gltf_draco.cpp \
              tests/loaders/test_gltf_meshopt.cpp \
              tests/loaders/test_gltf_ktx2.cpp \
              tests/loaders/test_gltf_webp.cpp \
              tests/loaders/test_mesh_geometry.cpp \
              tests/loaders/test_mesh_vcache.cpp \
              tests/texture/test_texture.cpp \
              tests/texture/test_ktx2.cpp \
              tests/texture/test_webp.cpp \
              tests/rasterize/test_rasterize.cpp \
              tests/rasterize/test_rasterize_texture.cpp \
              tests/rasterize/test_rasterize_vcol.cpp \
              tests/rasterize/test_rasterize_alpha.cpp \
              tests/rasterize/test_rasterize_emissive.cpp \
              tests/rasterize/test_rasterize_tonemap.cpp \
              tests/renderer/test_renderer.cpp \
              tests/renderer/test_renderer_ao_clip.cpp \
              tests/renderer/test_renderer_vcol.cpp \
              tests/renderer/test_renderer_misc.cpp \
              tests/renderer/test_renderer_unlit.cpp \
              tests/renderer/test_renderer_tiled.cpp \
              tests/renderer/test_transparency.cpp \
              tests/pipeline/test_camera.cpp \
              tests/pipeline/test_clip.cpp \
              tests/pipeline/test_clip_near.cpp \
              tests/math/test_fastmath.cpp \
              tests/math/test_linalg.cpp \
              tests/math/test_light.cpp \
              src/args.cpp \
              src/terminal/color.cpp \
              src/render/renderer.cpp \
              src/loaders/mesh.cpp \
              src/loaders/mesh_obj.cpp \
              src/loaders/mesh_ply.cpp \
              src/loaders/mesh_stl.cpp \
              src/loaders/mesh_gltf.cpp \
              src/loaders/draco_decode.cpp \
              src/render/texture.cpp \
              src/loaders/ktx2_decode.cpp \
              src/loaders/webp_decode.cpp \
              src/render/camera.cpp \
              src/render/rasterize.cpp \
              src/terminal/framebuffer.cpp \
              src/terminal/hud.cpp \
              src/terminal/graphics.cpp \
              src/terminal/kitty.cpp \
              src/terminal/sixel.cpp \
              vendor/meshoptimizer/meshoptimizer_impl.cpp \
              vendor/draco/draco_impl.cpp \
              vendor/basisu/basisu_impl.cpp

# Per-build-type object caches. Mtime alone can't tell which variant produced
# $(TARGET), so each variant has its own subdir of objects and is phony: the
# resulting $(TARGET) always matches the variant just invoked. Without this,
# `make` after `make portable` (or any flag-changing variant) silently mixes a
# portable/debug binary with stale release objects. Trade-off: the link runs on
# every `make` invocation (not cheap under -flto=auto + -fipa-pta, typically a
# few seconds on no-change rebuilds); the per-variant .o cache still avoids
# unnecessary recompiles, so source edits stay incremental.
OBJDIR             = obj
PORTABLE_CXXFLAGS  = -std=c++17 $(WARNINGS) -Werror $(OPT_COMMON) $(ROOT_INC) $(VENDOR_INC)
DEBUG_CXXFLAGS     = -std=c++17 $(WARNINGS) -Werror -O0 -g -pthread $(ROOT_INC) $(VENDOR_INC) $(ARCH32) $(LFS) $(MINIZ)

# ─── C flags (vendored zstd + miniz amalgams + libwebp decode subset) ─────────
# Third-party C; compile with $(CC), warnings off (-w), never via the strict C++
# flag set. The speed tier mirrors the C++ OPT_COMMON (-funroll-loops/-ffast-math/
# -DNDEBUG) so these TUs aren't left at a bare -O3, but LTO is deliberately omitted:
# $(LTO) is derived from $(CXX) (so -flto=thin would reach a gcc $(CC) under `make
# CXX=clang++`), and cross-family C/C++ LTO objects can't link anyway. The lost
# cross-TU inlining is marginal in every case: the webp/zstd decode TUs are load-time
# and isolated behind webp_decode.cpp/ktx2_decode.cpp, and miniz's mz_compress2 runs
# per transmitted kitty frame but as one self-contained call from framebuffer.cpp.
# -march follows the variant (native for release/test only). libwebp's
# internal includes are repo-rooted ("src/dec/..."), so it needs its root on the
# include path. WEBP_USE_THREAD makes libwebp's lazy SIMD function-pointer init
# mutex-guarded (cpu.h): texture decode runs on worker threads, and without it the
# first concurrent WebP decode races on the global DSP tables. Both flags are shared
# with the self-contained zstd and miniz amalgams, which ignore them harmlessly.
CC ?= cc
C_INC           = -isystem vendor/libwebp -DWEBP_USE_THREAD
C_OPT           = -std=c11 -O3 -funroll-loops -ffast-math -fno-finite-math-only -DNDEBUG \
                  -ffunction-sections -fdata-sections -w -pipe $(C_INC) $(ARCH32) $(LFS) $(MINIZ)
RELEASE_CFLAGS  = $(C_OPT) $(ARCH_NATIVE)
PORTABLE_CFLAGS = $(C_OPT)
DEBUG_CFLAGS    = -std=c11 -O0 -g -w -pipe $(C_INC) $(ARCH32) $(LFS) $(MINIZ)
TEST_CFLAGS     = $(C_OPT) $(ARCH_NATIVE)

# ─── Install locations (GNU Coding Standards) ─────────────────────────────────
# Standard `?=` vars so packagers can override; DESTDIR is left unset (empty = real
# install, packagers stage into a fakeroot by setting it on the command line).
PREFIX      ?= /usr/local
exec_prefix ?= $(PREFIX)
bindir      ?= $(exec_prefix)/bin
datarootdir ?= $(PREFIX)/share
mandir      ?= $(datarootdir)/man
man1dir     ?= $(mandir)/man1
docdir      ?= $(datarootdir)/doc/$(TARGET)

INSTALL         ?= install
INSTALL_PROGRAM ?= $(INSTALL) -m 755
INSTALL_DATA    ?= $(INSTALL) -m 644

# Terse output by default (one short line per compile/link); `make V=1` echoes the
# full g++ commands. Q silences the recipe-line echo; E prints the short progress line
# (and becomes a shell no-op under V=1 so the full command isn't doubled).
V ?= 0
ifeq ($(V),0)
Q = @
E = @printf '  %-5s %s\n'
else
Q =
E = @:
endif

RELEASE_OBJS  = $(patsubst %.cpp,$(OBJDIR)/release/%.o,$(SRCS))
PORTABLE_OBJS = $(patsubst %.cpp,$(OBJDIR)/portable/%.o,$(SRCS))
DEBUG_OBJS    = $(patsubst %.cpp,$(OBJDIR)/debug/%.o,$(SRCS))
TEST_OBJS     = $(patsubst %.cpp,$(OBJDIR)/test/%.o,$(TEST_SRCS))

RELEASE_COBJS  = $(patsubst %.c,$(OBJDIR)/release/%.o,$(CSRCS))
PORTABLE_COBJS = $(patsubst %.c,$(OBJDIR)/portable/%.o,$(CSRCS))
DEBUG_COBJS    = $(patsubst %.c,$(OBJDIR)/debug/%.o,$(CSRCS))
TEST_COBJS     = $(patsubst %.c,$(OBJDIR)/test/%.o,$(CSRCS))

$(OBJDIR)/release/%.o: %.cpp $(HDRS) $(VENDOR_HDRS)
	@mkdir -p $(@D)
	$(E) CXX $<
	$(Q)$(CXX) -c $(CXXFLAGS) -o $@ $<

$(OBJDIR)/portable/%.o: %.cpp $(HDRS) $(VENDOR_HDRS)
	@mkdir -p $(@D)
	$(E) CXX $<
	$(Q)$(CXX) -c $(PORTABLE_CXXFLAGS) -o $@ $<

$(OBJDIR)/debug/%.o: %.cpp $(HDRS) $(VENDOR_HDRS)
	@mkdir -p $(@D)
	$(E) CXX $<
	$(Q)$(CXX) -c $(DEBUG_CXXFLAGS) -o $@ $<

$(OBJDIR)/test/%.o: %.cpp $(HDRS) $(VENDOR_HDRS) $(TEST_HDRS)
	@mkdir -p $(@D)
	$(E) CXX $<
	$(Q)$(CXX) -c $(TEST_CXXFLAGS) -o $@ $<

$(OBJDIR)/release/%.o: %.c $(VENDOR_HDRS)
	@mkdir -p $(@D)
	$(E) CC $<
	$(Q)$(CC) -c $(RELEASE_CFLAGS) -o $@ $<

$(OBJDIR)/portable/%.o: %.c $(VENDOR_HDRS)
	@mkdir -p $(@D)
	$(E) CC $<
	$(Q)$(CC) -c $(PORTABLE_CFLAGS) -o $@ $<

$(OBJDIR)/debug/%.o: %.c $(VENDOR_HDRS)
	@mkdir -p $(@D)
	$(E) CC $<
	$(Q)$(CC) -c $(DEBUG_CFLAGS) -o $@ $<

$(OBJDIR)/test/%.o: %.c $(VENDOR_HDRS)
	@mkdir -p $(@D)
	$(E) CC $<
	$(Q)$(CC) -c $(TEST_CFLAGS) -o $@ $<

# The basisu transcoder shim is a large, unaudited vendored TU that trips a long,
# compiler-specific warning set; suppress all of its warnings per-TU (-w, mirroring the
# zstd C TU) instead of maintaining a fragile pragma list that breaks on version bumps.
$(OBJDIR)/release/vendor/basisu/basisu_impl.o:  CXXFLAGS          += -w
$(OBJDIR)/portable/vendor/basisu/basisu_impl.o: PORTABLE_CXXFLAGS += -w
$(OBJDIR)/debug/vendor/basisu/basisu_impl.o:    DEBUG_CXXFLAGS     += -w
$(OBJDIR)/test/vendor/basisu/basisu_impl.o:     TEST_CXXFLAGS      += -w

.DEFAULT_GOAL := release

release: $(RELEASE_OBJS) $(RELEASE_COBJS)
	$(E) LINK $(TARGET)
	$(Q)$(CXX) $(CXXFLAGS) $(LTO_SUPPRESS) $(GC_LINK) -o $(TARGET) $^ $(LIBRT)

portable: $(PORTABLE_OBJS) $(PORTABLE_COBJS)
	$(E) LINK $(TARGET)
	$(Q)$(CXX) $(PORTABLE_CXXFLAGS) $(LTO_SUPPRESS) $(GC_LINK) -o $(TARGET) $^ $(LIBRT)

# Release artifact: portable codegen (link-only static flags reuse the portable objects).
dist: $(PORTABLE_OBJS) $(PORTABLE_COBJS)
	$(E) LINK $(TARGET)
	$(Q)$(CXX) $(PORTABLE_CXXFLAGS) $(LTO_SUPPRESS) $(GC_LINK) $(DIST_LINK) -o $(TARGET) $^ $(LIBRT)

debug: $(DEBUG_OBJS) $(DEBUG_COBJS)
	$(E) LINK $(TARGET)
	$(Q)$(CXX) $(DEBUG_CXXFLAGS) -o $(TARGET) $^ $(LIBRT)

$(TEST_TARGET): $(TEST_OBJS) $(TEST_COBJS)
	$(E) LINK $@
	$(Q)$(CXX) $(TEST_CXXFLAGS) $(LTO_SUPPRESS) $(GC_LINK) -o $@ $^ $(LIBRT)

test: $(TEST_TARGET)
	$(Q)./$(TEST_TARGET)

# Install the just-built binary, man page, and docs. The build variants are all phony
# and each links $(TARGET), so there's no file rule to depend on and no single variant
# install should force (a packager may have just run `make dist`). Require the binary to
# exist and fail loud otherwise rather than rebuilding. `install -d` creates the dirs
# (separate step: BSD/macOS install lacks GNU's -D). THIRD_PARTY_NOTICES ships beside the
# executable per the vendored licenses; the man page is left uncompressed for packaging.
install:
	@test -x ./$(TARGET) || { printf '%s\n' "$(TARGET) not built; run 'make' (or 'make dist') first" >&2; exit 1; }
	$(E) INST $(DESTDIR)$(bindir)/$(TARGET)
	$(Q)$(INSTALL) -d $(DESTDIR)$(bindir) $(DESTDIR)$(man1dir) $(DESTDIR)$(docdir)
	$(Q)$(INSTALL_PROGRAM) $(TARGET) $(DESTDIR)$(bindir)/$(TARGET)
	$(Q)$(INSTALL_DATA) man/rasterminal.1 $(DESTDIR)$(man1dir)/rasterminal.1
	$(Q)$(INSTALL_DATA) THIRD_PARTY_NOTICES LICENSE README.md $(DESTDIR)$(docdir)/

uninstall:
	$(Q)rm -f $(DESTDIR)$(bindir)/$(TARGET)
	$(Q)rm -f $(DESTDIR)$(man1dir)/rasterminal.1
	$(Q)rm -rf $(DESTDIR)$(docdir)

clean:
	rm -rf $(TARGET) $(TEST_TARGET) $(OBJDIR)

.PHONY: release portable dist debug test clean install uninstall
