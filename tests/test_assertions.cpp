#include "tests/test.h"

#include <limits>

namespace
{
    template <typename Function> void assert_throws_assertion_error(const Function &function)
    {
        try
        {
            function();
        }
        catch (const testing::AssertionError &)
        {
            return;
        }
        ASSERT_FAIL("expected testing::AssertionError");
    }
} // namespace

TEST(Assertions, NearRejectsNaN)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();

    assert_throws_assertion_error([nan]() { ASSERT_NEAR(nan, 1.0f, 0.001f); });
    assert_throws_assertion_error([nan]() { ASSERT_NEAR(1.0f, nan, 0.001f); });
    assert_throws_assertion_error([nan]() { ASSERT_NEAR(1.0f, 1.0f, nan); });
    assert_throws_assertion_error([nan]() { ASSERT_NEAR(nan, nan, 0.001f); });
}

TEST(Assertions, NearRejectsDoubleNaN)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();

    assert_throws_assertion_error([nan]() { ASSERT_NEAR(nan, 1.0, 0.001); });
    assert_throws_assertion_error([nan]() { ASSERT_NEAR(1.0, nan, 0.001); });
    assert_throws_assertion_error([nan]() { ASSERT_NEAR(1.0, 1.0, nan); });
    assert_throws_assertion_error([nan]() { ASSERT_NEAR(nan, nan, 0.001); });
}

TEST(Assertions, NearChecksTolerance)
{
    ASSERT_NEAR(1.0f, 1.25f, 0.25f);
    ASSERT_NEAR(1.25, 1.0, 0.25);
    ASSERT_NEAR(1.0, 1.0, 0.0);
    ASSERT_NEAR(1.0f, 1.25, 0.25f);
    ASSERT_NEAR(1u, 2u, 1.0);
    assert_throws_assertion_error([]() { ASSERT_NEAR(1.0, 1.5f, 0.25); });
    assert_throws_assertion_error([]() { ASSERT_NEAR(1.0f, 1.5f, 0.25f); });
    assert_throws_assertion_error([]() { ASSERT_NEAR(1.0, 1.0, -0.25); });
}

TEST(Assertions, NearRejectsIndeterminateDifference)
{
    const double inf = std::numeric_limits<double>::infinity();

    assert_throws_assertion_error([inf]() { ASSERT_NEAR(inf, inf, 0.001); });
    assert_throws_assertion_error([inf]() { ASSERT_NEAR(-inf, -inf, 0.001); });
}
