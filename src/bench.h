#pragma once

#include "src/args.h"
#include "src/loaders/mesh.h"
#include "src/terminal/framebuffer.h"

#include <cstdint>
#include <optional>
#include <vector>

struct BenchStats
{
    int64_t min_ns = 0;
    int64_t median_ns = 0;
    int64_t p95_ns = 0;
    int64_t max_ns = 0;
    double stddev_ms = 0.0;
};

// frame_ns must be non-empty. Selection reorders it.
BenchStats summarize_frames(std::vector<int64_t> &frame_ns);

// Presentation samples from the measured frames, for the --bench-graphics report.
struct PresentSamples
{
    std::vector<int64_t> encode_ns;
    std::vector<int64_t> bytes;
    // Presents that composed nothing, such as a blocks frame with no changed cell.
    // They are not samples.
    int empty = 0;

    void add(const Framebuffer::PresentStats &stats);
};

// The colours the viewer would draw a frame with, so --bench-graphics encodes what
// a terminal receives. required is the colour the sixel palette must hold exactly.
struct BenchColors
{
    Color background;
    Color wireframe;
    std::optional<Color> required;
};

[[nodiscard]] BenchColors bench_colors(const ParsedArgs &args);

// Backend the bench presents through, or nullopt to measure rendering only
// (no --bench-graphics). Kitty uses direct transport, the path remote sessions take.
[[nodiscard]] std::optional<GraphicsConfig> bench_graphics(const ParsedArgs &args);

// Render args.bench headless frames after args.bench_warmup and report timings on stderr.
void run_bench(const Mesh &mesh, const ParsedArgs &args, int n_threads, double load_ms);
