#pragma once

#include "linalg.h"

#include <cmath>

inline float specular_pow(float ndh, float shininess)
{
    // Common MTL shininess values get an exact multiply chain that is much
    // cheaper than libm powf in the inner lighting loop.
    if (shininess == 32.0f)
    {
        float x2 = ndh * ndh;
        float x4 = x2 * x2;
        float x8 = x4 * x4;
        float x16 = x8 * x8;
        return x16 * x16;
    }
    if (shininess == 16.0f)
    {
        float x2 = ndh * ndh;
        float x4 = x2 * x2;
        float x8 = x4 * x4;
        return x8 * x8;
    }
    if (shininess == 8.0f)
    {
        float x2 = ndh * ndh;
        float x4 = x2 * x2;
        return x4 * x4;
    }
    return std::pow(ndh, shininess);
}

// Per-surface material properties (from MTL Kd/Ks/Ns/map_Kd or defaults).
struct Material
{
    vec3 diffuse = {1.0f, 1.0f, 1.0f};
    vec3 specular = {0.4f, 0.4f, 0.4f};
    float shininess = 32.0f;
    int diffuse_tex = -1; // index into Mesh::textures, or -1 if none
    int normal_tex = -1;  // index into Mesh::textures, or -1 if none
};

struct Light
{
    // Direction toward the light source (world space, unit vector).
    vec3 direction = {0.408f, 0.816f, 0.408f};
    vec3 color = {1.0f, 1.0f, 1.0f};
};

// Blinn-Phong illumination summed over an array of directional lights.
// ambient is a scene-level term added once (not per light).
// Returns RGB in [0, ~1+]; caller is responsible for clamping before display.
inline vec3 compute_lighting(vec3 pos, vec3 normal,
                             const vec3 &eye_pos,
                             const Light *lights, int n_lights,
                             const vec3 &ambient,
                             const Material &mat = {},
                             float ao = 1.0f)
{
    vec3 n = normalize(normal);
    vec3 v = normalize(eye_pos - pos);

    vec3 result = ambient * mat.diffuse * ao;

    for (int i = 0; i < n_lights; i++)
    {
        const vec3 &l = lights[i].direction;
        const vec3 &lc = lights[i].color;

        float diff = dot(n, l);
        if (diff > 0.0f)
            result += lc * mat.diffuse * diff;

        float ndh = dot(n, normalize(l + v));
        if (ndh > 0.0f)
            result += lc * mat.specular * specular_pow(ndh, mat.shininess);
    }

    return result;
}
