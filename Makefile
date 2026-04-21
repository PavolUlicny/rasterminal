CXX      = g++
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
           -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual \
           -Wnon-virtual-dtor -Wnull-dereference -Wdouble-promotion \
           -Wformat=2 -Wimplicit-fallthrough -Wmisleading-indentation \
           -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast
CXXFLAGS = -std=c++17 $(WARNINGS) -O3 -march=native -pthread
TARGET   = rasterminal

SRCS = src/main.cpp \
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

HDRS = src/linalg.h \
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

clean:
	rm -f $(TARGET)

.PHONY: debug clean
