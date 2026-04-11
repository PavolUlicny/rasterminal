#pragma once

#include "linalg.h"

#include <cmath>

// Per-surface material properties (from MTL Kd/Ks/Ns or defaults).
struct Material
{
    vec3 diffuse = {1.0f, 1.0f, 1.0f};
    vec3 specular = {0.4f, 0.4f, 0.4f};
    float shininess = 32.0f;
};

struct Light
{
    // Direction toward the light source (world space, unit vector).
    // Default: normalize(1, 2, 1) — upper-right-forward.
    vec3 direction = {0.408f, 0.816f, 0.408f};
    vec3 color = {1.0f, 1.0f, 1.0f};
    float ambient_intensity = 0.15f;
};

// Blinn-Phong illumination at a world-space surface point.
// Returns RGB in [0, ~1+]; caller is responsible for clamping before display.
// mat defaults to a neutral grey-specular white surface, matching the pre-MTL look.
inline vec3 compute_lighting(vec3 pos, vec3 normal,
                             const vec3 &eye_pos, const Light &light,
                             const Material &mat = {})
{
    vec3 n = normalize(normal);
    vec3 v = normalize(eye_pos - pos);
    vec3 l = light.direction; // already normalised

    // Ambient
    vec3 result = light.color * light.ambient_intensity * mat.diffuse;

    // Diffuse (Lambertian)
    float diff = dot(n, l);
    if (diff > 0.0f)
        result += light.color * mat.diffuse * diff;

    // Specular (Blinn-Phong half-vector)
    float ndh = dot(n, normalize(l + v));
    if (ndh > 0.0f)
        result += light.color * mat.specular * std::pow(ndh, mat.shininess);

    return result;
}
