CXX ?= g++

# ─── Compiler detection (POSIX: GCC or Clang) ─────────────────────────────────
IS_CLANG := $(findstring clang,$(shell $(CXX) --version 2>&1))

WARN_COMMON = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
              -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual \
              -Wnon-virtual-dtor -Wnull-dereference -Wdouble-promotion \
              -Wformat=2 -Wimplicit-fallthrough -Wmisleading-indentation

ifeq ($(IS_CLANG),clang)
WARNINGS = $(WARN_COMMON)
LTO      = -flto=thin
GCC_OPTS =
else
WARNINGS = $(WARN_COMMON) -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast
LTO      = -flto=auto
GCC_OPTS = -fno-plt -fno-semantic-interposition -fno-stack-clash-protection \
           -fgcse-sm -fgcse-las -fipa-pta -Wno-alloc-size-larger-than
endif

VENDOR_INC  = -isystem vendor/cgltf -isystem vendor/stb -isystem vendor/stl_reader \
              -isystem vendor/tinyobjloader -isystem vendor/tinyply \
              -isystem vendor/meshoptimizer/src -isystem vendor/draco/src
VENDOR_HDRS = vendor/cgltf/cgltf.h vendor/stb/stb_image.h vendor/stl_reader/stl_reader.h \
              vendor/tinyobjloader/tiny_obj_loader.h vendor/tinyply/tinyply.h \
              vendor/meshoptimizer/src/meshoptimizer.h vendor/draco/src/draco/draco_features.h

# Tier 1: fast flags safe for any CPU (both release and portable).
OPT_COMMON = -O3 $(LTO) -funroll-loops -ffast-math -fno-finite-math-only \
             -fno-rtti -fomit-frame-pointer -fstrict-aliasing \
             -fmerge-all-constants -fvisibility=hidden \
             -fno-stack-protector -fno-asynchronous-unwind-tables \
             -pipe -pthread $(GCC_OPTS)

# Tier 2: machine-specific (release only).
ARCH_NATIVE = -march=native

CXXFLAGS      = -std=c++17 $(WARNINGS) -Werror $(OPT_COMMON) $(ARCH_NATIVE) $(VENDOR_INC)
TEST_CXXFLAGS = -std=c++17 $(WARNINGS) -Werror -O3 $(LTO) $(ARCH_NATIVE) -funroll-loops \
                -ffast-math -fno-finite-math-only -fomit-frame-pointer -fstrict-aliasing \
                -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
                -pipe -pthread $(VENDOR_INC)
TARGET   = rasterminal

SRCS = src/main.cpp \
       src/args.cpp \
       src/framebuffer.cpp \
       src/mesh.cpp \
       src/mesh_obj.cpp \
       src/mesh_ply.cpp \
       src/mesh_stl.cpp \
       src/mesh_gltf.cpp \
       src/draco_decode.cpp \
       src/camera.cpp \
       src/shadow.cpp \
       src/rasterize.cpp \
       src/renderer.cpp \
       src/texture.cpp \
       vendor/meshoptimizer/meshoptimizer_impl.cpp \
       vendor/draco/draco_impl.cpp

HDRS = src/args.h \
       src/clip.h \
       src/linalg.h \
       src/framebuffer.h \
       src/mesh.h \
       src/mesh_loader.h \
       src/draco_decode.h \
       src/camera.h \
       src/light.h \
       src/shadow.h \
       src/rasterize.h \
       src/renderer.h \
       src/platform.h \
       src/texture.h

# LTO_SUPPRESS: GCC's LTO re-emits -Wmaybe-uninitialized from Draco's edgebreaker
# templates during the link step's recompile, and per-TU pragma context (in
# src/draco_decode.cpp + vendor/draco/draco_impl.cpp) doesn't survive that re-emit.
# The suppression is needed only at link time — keeping compile-time warnings active
# for every other TU so real uninitialized-read bugs are still caught by -Werror.
# Clang has no equivalent warning name; leave empty there.
ifeq ($(IS_CLANG),clang)
LTO_SUPPRESS =
else
LTO_SUPPRESS = -Wno-maybe-uninitialized
endif

TEST_TARGET = rasterminal_tests
TEST_SRCS   = tests/test_main.cpp \
              tests/test_dispatch.cpp \
              tests/test_obj.cpp \
              tests/test_ply.cpp \
              tests/test_stl.cpp \
              tests/test_linalg.cpp \
              tests/test_light.cpp \
              tests/test_clip.cpp \
              tests/test_camera.cpp \
              tests/test_clip_near.cpp \
              tests/test_texture.cpp \
              tests/test_framebuffer.cpp \
              tests/test_mesh_geometry.cpp \
              tests/test_mesh_vcache.cpp \
              tests/test_args.cpp \
              tests/test_shadow.cpp \
              tests/test_rasterize.cpp \
              tests/test_rasterize_texture.cpp \
              tests/test_rasterize_vcol.cpp \
              tests/test_rasterize_shadow.cpp \
              tests/test_rasterize_alpha.cpp \
              tests/test_rasterize_emissive.cpp \
              tests/test_gltf.cpp \
              tests/test_gltf_draco.cpp \
              tests/test_renderer.cpp \
              tests/test_renderer_ao_clip.cpp \
              tests/test_renderer_shadow_depth.cpp \
              tests/test_renderer_vcol.cpp \
              tests/test_renderer_misc.cpp \
              src/args.cpp \
              src/renderer.cpp \
              src/mesh.cpp \
              src/mesh_obj.cpp \
              src/mesh_ply.cpp \
              src/mesh_stl.cpp \
              src/mesh_gltf.cpp \
              src/draco_decode.cpp \
              src/texture.cpp \
              src/camera.cpp \
              src/rasterize.cpp \
              src/framebuffer.cpp \
              src/shadow.cpp \
              vendor/meshoptimizer/meshoptimizer_impl.cpp \
              vendor/draco/draco_impl.cpp

# Per-build-type object caches. Mtime alone can't tell which variant produced
# $(TARGET), so each variant has its own subdir of objects and is phony — the
# resulting $(TARGET) always matches the variant just invoked. Without this,
# `make` after `make portable` (or any flag-changing variant) silently mixes a
# portable/debug binary with stale release objects. Trade-off: the link runs on
# every `make` invocation (not cheap under -flto=auto + -fipa-pta — typically a
# few seconds on no-change rebuilds); the per-variant .o cache still avoids
# unnecessary recompiles, so source edits stay incremental.
OBJDIR             = obj
PORTABLE_CXXFLAGS  = -std=c++17 $(WARNINGS) -Werror $(OPT_COMMON) $(VENDOR_INC)
DEBUG_CXXFLAGS     = -std=c++17 $(WARNINGS) -Werror -O0 -g -pthread $(VENDOR_INC)

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

$(OBJDIR)/test/%.o: %.cpp $(HDRS) $(VENDOR_HDRS) tests/test.h tests/loader_util.h tests/rasterize_test_util.h \
                    tests/draco_cube_bitstream.h tests/draco_cube_color.h
	@mkdir -p $(@D)
	$(E) CXX $<
	$(Q)$(CXX) -c $(TEST_CXXFLAGS) -o $@ $<

.DEFAULT_GOAL := release

release: $(RELEASE_OBJS)
	$(E) LINK $(TARGET)
	$(Q)$(CXX) $(CXXFLAGS) $(LTO_SUPPRESS) -o $(TARGET) $^

portable: $(PORTABLE_OBJS)
	$(E) LINK $(TARGET)
	$(Q)$(CXX) $(PORTABLE_CXXFLAGS) $(LTO_SUPPRESS) -o $(TARGET) $^

debug: $(DEBUG_OBJS)
	$(E) LINK $(TARGET)
	$(Q)$(CXX) $(DEBUG_CXXFLAGS) -o $(TARGET) $^

$(TEST_TARGET): $(TEST_OBJS)
	$(E) LINK $@
	$(Q)$(CXX) $(TEST_CXXFLAGS) $(LTO_SUPPRESS) -o $@ $^

test: $(TEST_TARGET)
	$(Q)./$(TEST_TARGET)

clean:
	rm -rf $(TARGET) $(TEST_TARGET) $(OBJDIR)

.PHONY: release portable debug test clean
