#pragma once

#include "src/terminal/color.h"

#include <cmath>
#include <cstdint>

// sRGB (D65) to CIELAB and back, shared by the blocks LUT and the fitted sixel palette.
//
// CIELAB preserves dark model hues better than OKLab here: 69.8% of measured pixels
// mapped to gray, compared with 90.9% under OKLab.

struct Lab
{
    float L, a, b;
};

// sRGB EOTF (IEC 61966-2-1): non-linear [0,1] -> linear-light [0,1].
inline float srgb_to_linear(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

// Inverse EOTF. Input outside [0,1] clips, so pow never sees a negative base.
inline float linear_to_srgb(float v)
{
    if (!(v > 0.0f))
    {
        return 0.0f;
    }
    if (v >= 1.0f)
    {
        return 1.0f;
    }
    return v <= 0.0031308f ? v * 12.92f : (1.055f * std::pow(v, 1.0f / 2.4f)) - 0.055f;
}

inline Lab cielab_from_linear(float r, float g, float b)
{
    // sRGB -> XYZ (D65); X and Z rows pre-divided by the white point (Y's is 1).
    const float x = ((0.4124564f / 0.95047f) * r) + ((0.3575761f / 0.95047f) * g) + ((0.1804375f / 0.95047f) * b);
    const float y = (0.2126729f * r) + (0.7151522f * g) + (0.0721750f * b);
    const float z = ((0.0193339f / 1.08883f) * r) + ((0.1191920f / 1.08883f) * g) + ((0.9503041f / 1.08883f) * b);
    const auto f = [](float t)
    { return t > 216.0f / 24389.0f ? std::cbrt(t) : (((24389.0f / 27.0f) * t) + 16.0f) / 116.0f; };
    const float fx = f(x);
    const float fy = f(y);
    const float fz = f(z);
    return { (116.0f * fy) - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz) };
}

inline Lab cielab_from_srgb8(Color c)
{
    return cielab_from_linear(
        srgb_to_linear(static_cast<float>(c.r) / 255.0f), srgb_to_linear(static_cast<float>(c.g) / 255.0f),
        srgb_to_linear(static_cast<float>(c.b) / 255.0f)
    );
}

// Out-of-gamut input, such as a centroid of saturated colours, clips per channel.
inline Color srgb8_from_cielab(Lab lab)
{
    const float fy = (lab.L + 16.0f) / 116.0f;
    const float fx = fy + (lab.a / 500.0f);
    const float fz = fy - (lab.b / 200.0f);
    const auto finv = [](float t)
    { return t > 6.0f / 29.0f ? t * t * t : ((116.0f * t) - 16.0f) * (27.0f / 24389.0f); };
    const float x = finv(fx) * 0.95047f;
    const float y = finv(fy);
    const float z = finv(fz) * 1.08883f;
    const float r = (3.2404542f * x) - (1.5371385f * y) - (0.4985314f * z);
    const float g = (-0.9692660f * x) + (1.8760108f * y) + (0.0415560f * z);
    const float b = (0.0556434f * x) - (0.2040259f * y) + (1.0572252f * z);
    const auto to8 = [](float v) { return static_cast<uint8_t>((linear_to_srgb(v) * 255.0f) + 0.5f); };
    return { to8(r), to8(g), to8(b) };
}
