#pragma once

#include "linalg.h"

#include <cmath>

// ndh^shininess given ndh² as input — lets the caller skip a sqrt in the
// half-vector normalize. For the squaring-chain cases (32/16/8) ndh^N = (ndh²)^(N/2),
// so we start one step further along the chain with no precision loss.
inline float specular_pow_sq(float ndh_sq, float shininess) noexcept
{
    if (shininess == 32.0f)
    {
        const float x4 = ndh_sq * ndh_sq;
        const float x8 = x4 * x4;
        const float x16 = x8 * x8;
        return x16 * x16;
    }
    if (shininess == 16.0f)
    {
        const float x4 = ndh_sq * ndh_sq;
        const float x8 = x4 * x4;
        return x8 * x8;
    }
    if (shininess == 8.0f)
    {
        const float x4 = ndh_sq * ndh_sq;
        return x4 * x4;
    }
    return std::exp2f(shininess * 0.5f * std::log2f(ndh_sq));
}

// Per-surface material properties (from MTL Ka/Kd/Ks/Ns/map_Kd or defaults).
struct Material
{
    vec3 diffuse = {1.0f, 1.0f, 1.0f};
    vec3 ambient = {1.0f, 1.0f, 1.0f}; // Ka; defaults to Kd when Ka absent in MTL
    vec3 specular = {0.4f, 0.4f, 0.4f};
    float shininess = 32.0f;
    int diffuse_tex = -1;  // index into Mesh::textures, or -1 if none
    int specular_tex = -1; // index into Mesh::textures, or -1 if none
    int normal_tex = -1;   // index into Mesh::textures, or -1 if none
    bool double_sided = false;
    float alpha_cutoff = 0.0f; // 0 = disabled; >0 = discard pixels with diffuse-tex alpha below this
};

struct Light
{
    // Direction toward the light source (world space, unit vector).
    vec3 direction = {0.408f, 0.816f, 0.408f};
    vec3 color = {1.0f, 1.0f, 1.0f};
};

// Tag for callers that guarantee the normal is already unit-length.
// Skips the internal normalize() — saves one sqrt per call.
struct assume_unit_t
{
};
inline constexpr assume_unit_t assume_unit{};

// Half-vector normalize skipped: ndh² = (n·h)² / (h·h), saves one sqrt per light.
inline void apply_light(vec3 &result, const vec3 &n, const vec3 &v,
                        const Light &light, const Material &mat) noexcept
{
    const float diff = dot(n, light.direction);
    if (diff >= 0.0f)
    {
        result += light.color * mat.diffuse * diff;

        const vec3 h = light.direction + v;
        const float ndh_raw = dot(n, h);
        if (ndh_raw > 0.0f)
        {
            const float hh = dot(h, h);
            const float ndh_sq = (ndh_raw * ndh_raw) / hh;
            result += light.color * mat.specular * specular_pow_sq(ndh_sq, mat.shininess);
        }
    }
}

// Blinn-Phong illumination summed over an array of directional lights.
// ambient is a scene-level term added once (not per light).
// v must be the unit view vector (normalize(eye - pos)) — precomputed by caller.
// Returns RGB in [0, ~1+]; caller is responsible for clamping before display.
inline vec3 compute_lighting(vec3 normal, const vec3 &v,
                             const Light *lights, int n_lights,
                             const vec3 &ambient,
                             const Material &mat = {},
                             float ao = 1.0f) noexcept
{
    const vec3 n = normalize(normal);
    vec3 result = ambient * mat.ambient * ao;
    for (int i = 0; i < n_lights; i++)
        apply_light(result, n, v, lights[i], mat);
    return result;
}

// Convenience overload: derives v from pos and eye_pos.
// Used by Flat/Gouraud paths where v is not precomputed.
inline vec3 compute_lighting(vec3 pos, vec3 normal,
                             const vec3 &eye_pos,
                             const Light *lights, int n_lights,
                             const vec3 &ambient,
                             const Material &mat = {},
                             float ao = 1.0f) noexcept
{
    return compute_lighting(normal, normalize(eye_pos - pos),
                            lights, n_lights, ambient, mat, ao);
}

// assume_unit overloads: caller guarantees normal is already unit-length.
// Flat face normals and Gouraud vertex normals are always unit after load;
// skipping normalize() saves one sqrt per Flat triangle and per Gouraud vertex.
inline vec3 compute_lighting([[maybe_unused]] assume_unit_t tag, const vec3 &n, const vec3 &v,
                             const Light *lights, int n_lights,
                             const vec3 &ambient,
                             const Material &mat = {},
                             float ao = 1.0f) noexcept
{
    vec3 result = ambient * mat.ambient * ao;
    for (int i = 0; i < n_lights; i++)
        apply_light(result, n, v, lights[i], mat);
    return result;
}

inline vec3 compute_lighting([[maybe_unused]] assume_unit_t tag, vec3 pos, const vec3 &normal,
                             const vec3 &eye_pos,
                             const Light *lights, int n_lights,
                             const vec3 &ambient,
                             const Material &mat = {},
                             float ao = 1.0f) noexcept
{
    return compute_lighting(assume_unit, normal, normalize(eye_pos - pos),
                            lights, n_lights, ambient, mat, ao);
}
