CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
TARGET   = rasterminal

SRCS = src/main.cpp \
       src/framebuffer.cpp

HDRS = src/linalg.h \
       src/framebuffer.h \
       src/platform.h

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

debug: CXXFLAGS = -std=c++17 -Wall -Wextra -O0 -g
debug: $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: debug clean
