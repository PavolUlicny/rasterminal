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
       src/camera.h \
       src/light.h \
       src/shadow.h \
       src/rasterize.h \
       src/renderer.h \
       src/platform.h \
       src/texture.h

$(TARGET): $(SRCS) $(HDRS) $(VENDOR_HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

release: $(TARGET)

portable: CXXFLAGS = -std=c++17 $(WARNINGS) -Werror $(OPT_COMMON) $(VENDOR_INC)
portable: $(TARGET)

debug: CXXFLAGS = -std=c++17 $(WARNINGS) -Werror -O0 -g -pthread $(VENDOR_INC)
debug: $(TARGET)

# ─── tests ────────────────────────────────────────────────────────────────────
# Only links sources the tests actually exercise — no renderer/main.
# Run from repo root so models/ paths in test_loaders resolve.

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
              src/texture.cpp \
              src/camera.cpp \
              src/rasterize.cpp \
              src/framebuffer.cpp \
              src/shadow.cpp \
              vendor/meshoptimizer/meshoptimizer_impl.cpp \
              vendor/draco/draco_impl.cpp

$(TEST_TARGET): $(TEST_SRCS) $(HDRS) $(VENDOR_HDRS) tests/test.h tests/loader_util.h tests/rasterize_test_util.h
	$(CXX) $(TEST_CXXFLAGS) -o $@ $(TEST_SRCS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(TARGET) $(TEST_TARGET)

.PHONY: release portable debug test clean
