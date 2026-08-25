#pragma once

#include "src/math/linalg.h"

// Conservative clip-space rejection. This never clips a partly visible triangle.
[[nodiscard]] constexpr bool clip_reject(const vec4 &a, const vec4 &b, const vec4 &c) noexcept
{
    if (a.w <= 0.0f || b.w <= 0.0f || c.w <= 0.0f)
    {
        return true;
    }
    if (a.x > a.w && b.x > b.w && c.x > c.w)
    {
        return true;
    }
    if (a.x < -a.w && b.x < -b.w && c.x < -c.w)
    {
        return true;
    }
    if (a.y > a.w && b.y > b.w && c.y > c.w)
    {
        return true;
    }
    if (a.y < -a.w && b.y < -b.w && c.y < -c.w)
    {
        return true;
    }
    if (a.z > a.w && b.z > b.w && c.z > c.w)
    {
        return true;
    }
    if (a.z < -a.w && b.z < -b.w && c.z < -c.w)
    {
        return true;
    }
    return false;
}
