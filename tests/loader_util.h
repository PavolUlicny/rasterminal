#pragma once

// Shared helpers for the format-specific loader test files
// (test_obj.cpp, test_ply.cpp, test_stl.cpp, test_dispatch.cpp).
// Header-only: each translation unit gets its own `static` copy, which is
// fine given these are tiny functions only used by tests.

#include "test.h"
#include "../src/mesh.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

static std::string tmp_path(const char *name)
{
    return (std::filesystem::temp_directory_path() / name).string();
}

static void write_bytes(const std::string &path, const void *data, size_t n)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    ASSERT_TRUE(f != nullptr);
    size_t w = std::fwrite(data, 1, n, f);
    std::fclose(f);
    ASSERT_EQ(w, n);
}

static void write_str(const std::string &path, const std::string &s)
{
    write_bytes(path, s.data(), s.size());
}

// Scoped temp file: writes on construction, removes on destruction. Ensures
// the file is cleaned up even if an assertion throws mid-test.
struct TmpFile
{
    std::string path;
    TmpFile(const std::string &p, const std::string &contents) : path(p) { write_str(path, contents); }
    TmpFile(const std::string &p, const void *data, size_t n) : path(p) { write_bytes(path, data, n); }
    ~TmpFile() { std::remove(path.c_str()); }
    TmpFile(const TmpFile &) = delete;
    TmpFile &operator=(const TmpFile &) = delete;
};

// Load a file expected to succeed; sanity-check that the mesh is non-empty.
[[maybe_unused]] static Mesh load_ok(const std::string &path)
{
    Mesh m;
    // ao=false: skip AO bake (expensive on large meshes, irrelevant to loader
    // correctness). compute_tangents/normals still run via load_model.
    bool ok = m.load_model(path, /*ao=*/false);
    if (!ok)
        ASSERT_FAIL("load_model(\"" + path + "\") returned false");
    if (m.vertices.empty())
        ASSERT_FAIL("load_model(\"" + path + "\") produced zero vertices");
    if (m.triangles.empty())
        ASSERT_FAIL("load_model(\"" + path + "\") produced zero triangles");
    return m;
}

[[maybe_unused]] static void assert_rejects(const std::string &path)
{
    Mesh m;
    bool ok = m.load_model(path, /*ao=*/false);
    if (ok)
        ASSERT_FAIL("load_model(\"" + path + "\") should have rejected but returned true");
    // After a failed load, load_model clears the mesh — verify.
    if (!m.vertices.empty() || !m.triangles.empty())
        ASSERT_FAIL("rejected load left residual mesh state");
}

// Little/big-endian byte emitters for binary file construction.
static void emit_u32_le(std::string &s, uint32_t v)
{
    s.push_back(static_cast<char>(v & 0xFFu));
    s.push_back(static_cast<char>((v >> 8) & 0xFFu));
    s.push_back(static_cast<char>((v >> 16) & 0xFFu));
    s.push_back(static_cast<char>((v >> 24) & 0xFFu));
}

static void emit_u32_be(std::string &s, uint32_t v)
{
    s.push_back(static_cast<char>((v >> 24) & 0xFFu));
    s.push_back(static_cast<char>((v >> 16) & 0xFFu));
    s.push_back(static_cast<char>((v >> 8) & 0xFFu));
    s.push_back(static_cast<char>(v & 0xFFu));
}

[[maybe_unused]] static void emit_f32_le(std::string &s, float v)
{
    uint32_t u;
    std::memcpy(&u, &v, 4);
    emit_u32_le(s, u);
}

[[maybe_unused]] static void emit_f32_be(std::string &s, float v)
{
    uint32_t u;
    std::memcpy(&u, &v, 4);
    emit_u32_be(s, u);
}
