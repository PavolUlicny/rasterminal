// Unity shim that compiles the basis_universal transcoder (decode-only).
//
// Like vendor/meshoptimizer/meshoptimizer_impl.cpp and vendor/draco/draco_impl.cpp,
// the transcoder .cpp is compiled directly rather than reached through -isystem.
// Unlike those, this TU is large (~1.5 MB) and trips a long, compiler-specific set of
// warnings that would change on every upstream bump, so instead of a pragma list it is
// built with blanket warning suppression (-w / /w) applied per-TU in the build files
// (Makefile, CMakeLists.txt) — the same treatment the vendored zstd C TU gets. We do
// not audit vendored code; refresh from upstream instead (see vendor/README.md).
//
// Configuration: upstream-default format set. rasterminal only ever transcodes to
// cTFRGBA32 for the CPU rasterizer, so the GPU block-format targets (and their lookup
// tables) are dead code at runtime — but they are NOT compiled out. Disabling them is
// entangled: ASTC/HDR helper code is referenced unconditionally, so a partial strip
// fails to compile, and a minimal safe subset is fragile across compilers and brittle
// on every version bump. The only required flags are KTX2 + Zstd, which #error if unset.

#define BASISD_SUPPORT_KTX2 1
#define BASISD_SUPPORT_KTX2_ZSTD 1

#include "transcoder/basisu_transcoder.cpp"
