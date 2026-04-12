CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
TARGET   = rasterminal

SRCS = src/main.cpp \
       src/framebuffer.cpp \
       src/mesh.cpp \
       src/camera.cpp \
       src/renderer.cpp \
       src/texture.cpp

HDRS = src/linalg.h \
       src/framebuffer.h \
       src/mesh.h \
       src/camera.h \
       src/light.h \
       src/renderer.h \
       src/platform.h \
       src/texture.h \
       src/stb_image.h

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

debug: CXXFLAGS = -std=c++17 -Wall -Wextra -O0 -g
debug: $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: debug clean
