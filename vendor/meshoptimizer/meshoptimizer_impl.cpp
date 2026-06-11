// Suppress warnings from vendored code on GCC/Clang.
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
// GCC's optimizer reports a false null-dereference in simplifier.cpp's hash probe at ILP32 -O3.
#pragma GCC diagnostic ignored "-Wnull-dereference"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wuseless-cast"
#endif
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4245 4267)
#endif

#include "src/allocator.cpp"
#include "src/clusterizer.cpp"
#include "src/indexanalyzer.cpp"
#include "src/indexcodec.cpp"
#include "src/indexgenerator.cpp"
#include "src/meshletcodec.cpp"
#include "src/meshletutils.cpp"
#include "src/opacitymap.cpp"
#include "src/overdrawoptimizer.cpp"
#include "src/partition.cpp"
#include "src/quantization.cpp"
#include "src/rasterizer.cpp"
#include "src/simplifier.cpp"
#include "src/spatialorder.cpp"
#include "src/stripifier.cpp"
#include "src/vcacheoptimizer.cpp"
#include "src/vertexcodec.cpp"
#include "src/vertexfilter.cpp"
#include "src/vfetchoptimizer.cpp"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
