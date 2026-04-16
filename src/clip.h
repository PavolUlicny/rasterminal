#pragma once

#include "linalg.h"

// Conservative frustum rejection: returns true if the triangle is definitely
// not rasterizable in clip space. This includes any vertex with non-positive w
// and triangles where all three vertices lie outside the same frustum half-space.
// Does not clip — just avoids processing triangles that are obviously invisible.
inline bool clip_reject(const vec4 &a, const vec4 &b, const vec4 &c)
{
    if (a.w <= 0.0f || b.w <= 0.0f || c.w <= 0.0f)
        return true;
    if (a.x > a.w && b.x > b.w && c.x > c.w)
        return true;
    if (a.x < -a.w && b.x < -b.w && c.x < -c.w)
        return true;
    if (a.y > a.w && b.y > b.w && c.y > c.w)
        return true;
    if (a.y < -a.w && b.y < -b.w && c.y < -c.w)
        return true;
    if (a.z > a.w && b.z > b.w && c.z > c.w)
        return true;
    if (a.z < -a.w && b.z < -b.w && c.z < -c.w)
        return true;
    return false;
}
