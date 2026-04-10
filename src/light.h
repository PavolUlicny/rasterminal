#pragma once

#include "linalg.h"

#include <cmath>

struct Light
{
    // Direction toward the light source (world space, unit vector).
    // Default: normalize(1, 2, 1) — upper-right-forward.
    vec3 direction = {0.408f, 0.816f, 0.408f};
    vec3 color = {1.0f, 1.0f, 1.0f};
    float ambient_intensity = 0.15f;
    float specular_strength = 0.4f;
    int shininess = 32;
};

// Blinn-Phong illumination at a world-space surface point.
// Returns RGB in [0, ~1+]; caller is responsible for clamping before display.
inline vec3 compute_lighting(vec3 pos, vec3 normal,
                             const vec3 &eye_pos, const Light &light)
{
    vec3 n = normalize(normal);
    vec3 v = normalize(eye_pos - pos);
    vec3 l = light.direction; // already normalised

    // Ambient
    vec3 result = light.color * light.ambient_intensity;

    // Diffuse (Lambertian)
    float diff = dot(n, l);
    if (diff > 0.0f)
        result += light.color * diff;

    // Specular (Blinn-Phong half-vector)
    float ndh = dot(n, normalize(l + v));
    if (ndh > 0.0f)
        result += light.color * (std::pow(ndh, (float)light.shininess) * light.specular_strength);

    return result;
}
