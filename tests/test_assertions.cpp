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
}
