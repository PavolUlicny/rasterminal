#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

// Polynomial exp2 / log2 for the per-pixel specular power and tonemap rolloff, replacing the
// libm calls (~100 instructions each; log2f + exp2f were ~10% of a Phong frame on Sponza).
// Written as branch-free arithmetic and bit casts so the batch shader's loops vectorize them.
// Accuracy, measured against libm: exp2 within 2.4e-7 relative over [-126, 126], log2 within
// 9.4e-7 absolute on (0, 1], the range the one caller uses (over the whole normal range its
// error is 3 ulps of its own result, which is what a float answer can hold, not polynomial
// error). fast_exp's relative error grows with |x| to 3.5e-6 by -50, since scaling by 1/ln2
// rounds. None of that reaches a pixel: composed as the shader spells them, the specular power
// lands 1.9e-7 from libm's and the tonemapped channel 4.1e-8, against 3.9e-3 for one 8-bit
// step. Pinned by tests/math/test_fastmath.cpp. Inputs are the shader's own (finite, log2
// argument > 0); nothing here handles NaN, inf or a non-positive log2 argument.

// Round to nearest as floor(x + 0.5), the floor spelled as truncate-then-adjust: that form
// vectorizes on every target (std::floor needs SSE4.1's roundps to vectorize, a
// sign-dependent half step blocked the lighting loop's if-conversion, and the magic-constant
// trick is folded away under -ffast-math). Valid for |x| < 2^31.
inline float fast_round(float x) noexcept
{
    const float y = x + 0.5f;
    const auto t = static_cast<float>(static_cast<int32_t>(y));
    return t > y ? t - 1.0f : t;
}

inline float fast_exp2(float x) noexcept
{
    // std::min/max, not hand-written conditionals: under -fno-finite-math-only GCC does not
    // if-convert the `x < c ? c : x` spelling, which blocked the vectorization of every loop
    // this sits in.
    x = std::max(-126.0f, std::min(126.0f, x));
    const float n = fast_round(x);
    const float f = x - n; // [-0.5, 0.5]
    // Degree-6 Taylor expansion of 2^f = e^(f ln 2); the truncated term is ln2^7/7! * 0.5^7 ~ 1.2e-7.
    float p = 1.5403530e-4f;
    p = (p * f) + 1.3333558e-3f;
    p = (p * f) + 9.6181291e-3f;
    p = (p * f) + 5.5504109e-2f;
    p = (p * f) + 2.4022651e-1f;
    p = (p * f) + 6.9314718e-1f;
    p = (p * f) + 1.0f;
    // n is in [-126, 126] after the clamp, so the biased exponent is a small positive integer.
    const uint32_t bits = static_cast<uint32_t>(static_cast<int32_t>(n) + 127) << 23u;
    float scale; // NOLINT(cppcoreguidelines-init-variables) — memcpy fills it
    std::memcpy(&scale, &bits, sizeof scale);
    return p * scale;
}

inline float fast_log2(float x) noexcept
{
    uint32_t bits; // NOLINT(cppcoreguidelines-init-variables) — memcpy fills it
    std::memcpy(&bits, &x, sizeof bits);
    // Split into 2^e * m with m in [1, 2), then fold m above sqrt(2) down so the series argument
    // t = (m - 1) / (m + 1) stays within +-0.172.
    int32_t e = static_cast<int32_t>(bits >> 23u) - 127;
    const uint32_t mbits = (bits & 0x7FFFFFu) | 0x3F800000u;
    float m; // NOLINT(cppcoreguidelines-init-variables) — memcpy fills it
    std::memcpy(&m, &mbits, sizeof m);
    const bool fold = m > 1.41421356f;
    m = fold ? m * 0.5f : m;
    e = fold ? e + 1 : e;
    const float t = (m - 1.0f) / (m + 1.0f);
    const float t2 = t * t;
    // log2(m) = 2/ln2 * (t + t^3/3 + t^5/5 + t^7/7 + t^9/9); the next term is below 1e-9.
    float p = 1.0f / 9.0f;
    p = (p * t2) + (1.0f / 7.0f);
    p = (p * t2) + (1.0f / 5.0f);
    p = (p * t2) + (1.0f / 3.0f);
    p = (p * t2) + 1.0f;
    return static_cast<float>(e) + (t * p * 2.8853900817779268f);
}

inline float fast_exp(float x) noexcept
{
    return fast_exp2(x * 1.4426950408889634f);
}
