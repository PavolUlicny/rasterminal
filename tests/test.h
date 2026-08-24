#pragma once

// Minimal test framework, self-contained and header-only.
//
// Usage:
//     TEST(linalg, identity) { ASSERT_TRUE(...); ASSERT_NEAR(x, y, 1e-6f); }
//
// Tests register themselves via static initializers into a process-wide
// registry. run_all_tests() iterates, runs each in a try/catch, and prints
// a one-line pass/fail summary. Assertion macros throw on failure so a
// failing assert inside a helper still aborts the test cleanly.

#include <cmath>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// Portable fd helpers (used across several test files for stdio capture)
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
static inline int test_dup(int fd)
{
    return _dup(fd);
}
static inline int test_dup2(int fd, int dst)
{
    return _dup2(fd, dst);
}
static inline void test_close(int fd)
{
    _close(fd);
}
static inline int test_devnull()
{
    return _open("nul", _O_WRONLY);
}
static inline int test_fileno(std::FILE *f)
{
    return _fileno(f);
}
static inline int test_seek(int fd, long offset, int whence)
{
    return _lseek(fd, offset, whence);
}
static const int TEST_STDOUT = 1;
static const int TEST_STDERR = 2;
#else
#include <fcntl.h>
#include <unistd.h>
static inline int test_dup(int fd)
{
    return dup(fd);
}
static inline int test_dup2(int fd, int dst)
{
    return dup2(fd, dst);
}
static inline void test_close(int fd)
{
    close(fd);
}
static inline int test_devnull()
{
    return open("/dev/null", O_WRONLY);
}
static inline int test_seek(int fd, long offset, int whence)
{
    return static_cast<int>(lseek(fd, offset, whence));
}
static inline int test_fileno(std::FILE *f)
{
    return fileno(f);
}
static const int TEST_STDOUT = STDOUT_FILENO;
static const int TEST_STDERR = STDERR_FILENO;
#endif

namespace testing
{
    struct TestCase
    {
        const char *suite;
        const char *name;
        void (*fn)();
    };

    inline std::vector<TestCase> &registry()
    {
        static std::vector<TestCase> r;
        return r;
    }

    struct Registrar
    {
        Registrar(const char *suite, const char *name, void (*fn)()) { registry().push_back({ suite, name, fn }); }
    };

    class AssertionError : public std::runtime_error
    {
      public:
        explicit AssertionError(const std::string &msg) : std::runtime_error(msg) {}
    };

    // Stringify a value for assertion messages; enum classes print as their
    // underlying integer (std::to_string cannot take them directly).
    template <typename T> inline std::string assert_str(const T &v)
    {
        if constexpr (std::is_enum_v<T>)
        {
            return std::to_string(static_cast<long long>(v));
        }
        else
        {
            return std::to_string(v);
        }
    }

    inline int run_all_tests()
    {
        int passed = 0, failed = 0;
        for (const auto &t : registry())
        {
            try
            {
                t.fn();
                std::printf("  \x1b[32mPASS\x1b[0m  %s.%s\n", t.suite, t.name);
                passed++;
            }
            catch (const AssertionError &e)
            {
                std::printf("  \x1b[31mFAIL\x1b[0m  %s.%s\n        %s\n", t.suite, t.name, e.what());
                failed++;
            }
            catch (const std::exception &e)
            {
                std::printf(
                    "  \x1b[31mFAIL\x1b[0m  %s.%s (unexpected exception)\n        %s\n", t.suite, t.name, e.what()
                );
                failed++;
            }
        }
        std::printf("\n%d passed, %d failed (%zu total)\n", passed, failed, registry().size());
        return failed == 0 ? 0 : 1;
    }
} // namespace testing

#define TEST(suite, name)                                                                                              \
    static void test_##suite##_##name();                                                                               \
    static const testing::Registrar reg_##suite##_##name(#suite, #name, &test_##suite##_##name);                       \
    static void test_##suite##_##name()

#define ASSERT_FAIL(msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        throw testing::AssertionError(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + (msg));          \
    } while (0)

#define ASSERT_TRUE(expr)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
            ASSERT_FAIL("ASSERT_TRUE(" #expr ") failed");                                                              \
    } while (0)

#define ASSERT_FALSE(expr)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((expr))                                                                                                    \
            ASSERT_FAIL("ASSERT_FALSE(" #expr ") failed");                                                             \
    } while (0)

#define ASSERT_EQ(a, b)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _va = (a);                                                                                                \
        auto _vb = (b);                                                                                                \
        if (!(_va == _vb))                                                                                             \
            ASSERT_FAIL(                                                                                               \
                "ASSERT_EQ(" #a ", " #b ") failed: " + testing::assert_str(_va) + " != " + testing::assert_str(_vb)    \
            );                                                                                                         \
    } while (0)

#define ASSERT_NEAR(a, b, eps)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _va = (a);                                                                                                \
        auto _vb = (b);                                                                                                \
        auto _d = std::fabs(_va - _vb);                                                                                \
        if (_d > (eps))                                                                                                \
            ASSERT_FAIL(                                                                                               \
                "ASSERT_NEAR(" #a ", " #b ", " #eps ") failed: |" + std::to_string(_va) + " - " +                      \
                std::to_string(_vb) + "| = " + std::to_string(_d)                                                      \
            );                                                                                                         \
    } while (0)
