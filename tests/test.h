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
#include <vector>

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
        Registrar(const char *suite, const char *name, void (*fn)())
        {
            registry().push_back({suite, name, fn});
        }
    };

    class AssertionError : public std::runtime_error
    {
    public:
        explicit AssertionError(const std::string &msg) : std::runtime_error(msg) {}
    };

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
                std::printf("  \x1b[31mFAIL\x1b[0m  %s.%s\n        %s\n",
                            t.suite, t.name, e.what());
                failed++;
            }
            catch (const std::exception &e)
            {
                std::printf("  \x1b[31mFAIL\x1b[0m  %s.%s (unexpected exception)\n        %s\n",
                            t.suite, t.name, e.what());
                failed++;
            }
        }
        std::printf("\n%d passed, %d failed (%zu total)\n",
                    passed, failed, registry().size());
        return failed == 0 ? 0 : 1;
    }
} // namespace testing

#define TEST(suite, name)                                 \
    static void test_##suite##_##name();                  \
    static const testing::Registrar reg_##suite##_##name( \
        #suite, #name, &test_##suite##_##name);           \
    static void test_##suite##_##name()

#define ASSERT_FAIL(msg)                                             \
    do                                                               \
    {                                                                \
        throw testing::AssertionError(                               \
            std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
            ": " + (msg));                                           \
    } while (0)

#define ASSERT_TRUE(expr)                                 \
    do                                                    \
    {                                                     \
        if (!(expr))                                      \
            ASSERT_FAIL("ASSERT_TRUE(" #expr ") failed"); \
    } while (0)

#define ASSERT_FALSE(expr)                                 \
    do                                                     \
    {                                                      \
        if ((expr))                                        \
            ASSERT_FAIL("ASSERT_FALSE(" #expr ") failed"); \
    } while (0)

#define ASSERT_EQ(a, b)                                                      \
    do                                                                       \
    {                                                                        \
        auto _va = (a);                                                      \
        auto _vb = (b);                                                      \
        if (!(_va == _vb))                                                   \
            ASSERT_FAIL("ASSERT_EQ(" #a ", " #b ") failed: " +               \
                        std::to_string(_va) + " != " + std::to_string(_vb)); \
    } while (0)

#define ASSERT_NEAR(a, b, eps)                                              \
    do                                                                      \
    {                                                                       \
        auto _va = (a);                                                     \
        auto _vb = (b);                                                     \
        auto _d = std::fabs(_va - _vb);                                     \
        if (_d > (eps))                                                     \
            ASSERT_FAIL("ASSERT_NEAR(" #a ", " #b ", " #eps ") failed: |" + \
                        std::to_string(_va) + " - " + std::to_string(_vb) + \
                        "| = " + std::to_string(_d));                       \
    } while (0)
