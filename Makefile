CXX      = g++
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
           -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual \
           -Wnon-virtual-dtor -Wnull-dereference -Wdouble-promotion \
           -Wformat=2 -Wimplicit-fallthrough -Wmisleading-indentation \
           -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast
CXXFLAGS      = -std=c++17 $(WARNINGS) -Werror -O3 -march=native -flto=auto -funroll-loops -ffast-math -fno-finite-math-only \
                -fno-exceptions -fno-rtti -fomit-frame-pointer -fstrict-aliasing \
                -fno-plt -fno-semantic-interposition \
                -fno-stack-protector -fno-stack-clash-protection -fno-asynchronous-unwind-tables \
                -fmerge-all-constants -fvisibility=hidden \
                -falign-functions=32 -falign-loops=32 \
                -fgcse-sm -fgcse-las -fipa-pta \
                -Wno-alloc-size-larger-than -pipe -pthread
TEST_CXXFLAGS = -std=c++17 $(WARNINGS) -Werror -O3 -march=native -flto=auto -funroll-loops -ffast-math -fno-finite-math-only \
                -fomit-frame-pointer -fstrict-aliasing \
                -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
                -pipe -pthread
TARGET   = rasterminal

SRCS = src/main.cpp \
       src/args.cpp \
       src/framebuffer.cpp \
       src/mesh.cpp \
       src/mesh_obj.cpp \
       src/mesh_ply.cpp \
       src/mesh_stl.cpp \
       src/camera.cpp \
       src/shadow.cpp \
       src/rasterize.cpp \
       src/renderer.cpp \
       src/texture.cpp

HDRS = src/args.h \
       src/linalg.h \
       src/framebuffer.h \
       src/mesh.h \
       src/camera.h \
       src/light.h \
       src/shadow.h \
       src/rasterize.h \
       src/renderer.h \
       src/platform.h \
       src/texture.h \
       src/stb_image.h

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

debug: CXXFLAGS = -std=c++17 $(WARNINGS) -O0 -g -pthread
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
              tests/test_mesh_geometry.cpp \
              tests/test_args.cpp \
              tests/test_shadow.cpp \
              tests/test_rasterize.cpp \
              src/args.cpp \
              src/mesh.cpp \
              src/mesh_obj.cpp \
              src/mesh_ply.cpp \
              src/mesh_stl.cpp \
              src/texture.cpp \
              src/camera.cpp \
              src/rasterize.cpp \
              src/framebuffer.cpp \
              src/shadow.cpp

$(TEST_TARGET): $(TEST_SRCS) $(HDRS) tests/test.h
	$(CXX) $(TEST_CXXFLAGS) -o $@ $(TEST_SRCS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

clean:
	rm -f $(TARGET) $(TEST_TARGET)

.PHONY: debug test clean
