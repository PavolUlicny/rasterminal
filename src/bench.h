#pragma once

#include "src/args.h"
#include "src/loaders/mesh.h"

#include <cstdint>
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

// Render args.bench headless frames after args.bench_warmup and report timings on stderr.
void run_bench(const Mesh &mesh, const ParsedArgs &args, int n_threads, double load_ms);
