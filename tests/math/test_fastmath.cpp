#include "tests/test.h"
#include "src/math/fastmath.h"

#include <cmath>
#include <cstdint>

// The polynomial exp2/log2 that replace libm in the batch shader's specular power and tonemap
// rolloff. What has to hold is not a tight mathematical bound but the one the renderer relies on:
// a shaded channel must not move by a visible amount, i.e. by anything near one 8-bit step
// (1/255 = 3.9e-3). The bounds below are the measured errors with room to spare, so a compiler or
// target that rounds differently does not flake the suite while a real regression still trips it.

TEST(fastmath, fast_round_is_exactly_floor_of_x_plus_half)
{
    // The batch shader's exp2 splits its argument on this, so a disagreement with floor(x+0.5)
    // would put the result on the wrong side of a power of two. Negative inputs matter: the
    // implementation truncates toward zero and corrects, which is where the two forms diverge.
    for (int i = -200000; i <= 200000; i++)
    {
        const float x = static_cast<float>(i) * 0.0013f;
        ASSERT_TRUE(fast_round(x) == std::floor(x + 0.5f));
    }
    ASSERT_TRUE(fast_round(-0.5f) == 0.0f); // ties round up, as floor(x+0.5) does
    ASSERT_TRUE(fast_round(0.5f) == 1.0f);
    ASSERT_TRUE(fast_round(-1.5f) == -1.0f);
}

TEST(fastmath, exp2_tracks_libm_across_its_whole_range)
{
    double worst = 0.0;
    for (int i = 0; i <= 200000; i++)
    {
        const auto x = static_cast<float>(-126.0 + (252.0 * i / 200000.0));
        const double ref = std::exp2(static_cast<double>(x));
        worst = std::max(worst, std::fabs(static_cast<double>(fast_exp2(x)) - ref) / ref);
    }
    ASSERT_TRUE(worst < 1e-6); // measured 2.4e-7
}

TEST(fastmath, log2_tracks_libm_over_the_range_the_shader_uses)
{
    // The one caller feeds it a clamped dot product, so (0, 1] is what matters. Over the full
    // normal range the error is dominated by what a float result can hold (about 3 ulps of the
    // answer near 2^-120), not by the polynomial.
    double worst = 0.0;
    for (int i = 1; i <= 200000; i++)
    {
        const auto y = static_cast<float>(i) / 200000.0f;
        worst = std::max(worst, std::fabs(static_cast<double>(fast_log2(y)) - std::log2(static_cast<double>(y))));
    }
    ASSERT_TRUE(worst < 5e-6); // measured 9.4e-7
}

TEST(fastmath, exp2_clamps_instead_of_overflowing)
{
    // The argument is a shininess times a log, so it can run far outside the exponent range.
    // Both ends must saturate to a finite float rather than produce inf or a wrapped exponent.
    ASSERT_TRUE(std::isfinite(fast_exp2(1e9f)));
    ASSERT_TRUE(std::isfinite(fast_exp2(-1e9f)));
    ASSERT_TRUE(fast_exp2(1e9f) == fast_exp2(126.0f));
    ASSERT_TRUE(fast_exp2(-1e9f) == fast_exp2(-126.0f));
    ASSERT_TRUE(fast_exp2(-126.0f) > 0.0f);
    ASSERT_TRUE(fast_exp2(0.0f) == 1.0f);
    ASSERT_TRUE(fast_exp2(1.0f) > 1.999f && fast_exp2(1.0f) < 2.001f);
}

TEST(fastmath, specular_power_stays_far_inside_one_8bit_step)
{
    // The composed form the shader actually spells: fast_exp2(y * fast_log2(x)) for pow(x, y).
    // This is the claim the two approximations exist to support.
    constexpr double step = 1.0 / 255.0;
    double worst = 0.0;
    for (int i = 1; i <= 20000; i++)
    {
        const auto q = static_cast<float>(i) / 20000.0f; // (n.h)^2, in (0, 1]
        for (const float sh : { 8.0f, 16.0f, 32.0f, 77.0f })
        {
            const auto got = static_cast<double>(fast_exp2(sh * 0.5f * fast_log2(q)));
            const double ref = std::pow(static_cast<double>(q), static_cast<double>(sh) * 0.5);
            worst = std::max(worst, std::fabs(got - ref));
        }
    }
    ASSERT_TRUE(worst < 1e-5); // measured 1.9e-7, i.e. 400x inside this bound and 20000x inside a step
    ASSERT_TRUE(worst < step / 100.0);
}

TEST(fastmath, exp_keeps_the_tonemap_rolloff_inside_one_8bit_step)
{
    // fast_exp's own relative error grows with |x|, because it scales its argument by 1/ln2 and
    // that multiply rounds: 4.6e-7 over [-5, 0] but 3.5e-6 by -50. That is not the quantity to
    // pin, since the tonemap divides the result by its knee range and subtracts it from one, so
    // the places where exp is least accurate are exactly the places where it contributes least.
    // What a pixel sees is the composed channel, measured at 4.1e-8 against a step of 3.9e-3.
    constexpr float knee = 0.7f;
    constexpr float range = 1.0f - knee;
    constexpr double step = 1.0 / 255.0;
    double worst_channel = 0.0;
    double worst_exp = 0.0;
    for (int i = 0; i <= 200000; i++)
    {
        const auto c = static_cast<float>(8.0 * i / 200000.0); // an HDR channel up to 8.0
        const float arg = -(c - knee) / range;
        const double ref_exp = std::exp(static_cast<double>(arg));
        worst_exp = std::max(worst_exp, std::fabs(static_cast<double>(fast_exp(arg)) - ref_exp) / ref_exp);
        if (c <= knee)
        {
            continue; // below the knee the rolloff is not taken at all
        }
        const auto dknee = static_cast<double>(knee);
        const auto drange = static_cast<double>(range);
        const double got = dknee + (drange * (1.0 - static_cast<double>(fast_exp(arg))));
        const double ref = dknee + (drange * (1.0 - ref_exp));
        worst_channel = std::max(worst_channel, std::fabs(got - ref));
    }
    ASSERT_TRUE(worst_exp < 1e-5);           // measured 2.5e-6 over these arguments
    ASSERT_TRUE(worst_channel < step / 100); // measured 4.1e-8, i.e. 96000x inside a step
}
