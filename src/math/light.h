#pragma once

#include "src/math/linalg.h"

#include <cmath>
#include <cstdint>

// Compute ndh^shininess from ndh squared, avoiding half-vector normalization.
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

// Shared conversion for scalar and per-texel glTF roughness.
inline float roughness_to_shininess(float roughness) noexcept
{
    return ((1.0f - roughness) * 126.0f) + 2.0f;
}

// Texture index, UV set and optional baked 2x3 UV transform. Loaders clamp UV sets to 0 or 1.
// glTF loaders bake the project's flipped-v convention into the transform.
struct TexSlot
{
    int tex = -1;
    uint8_t uv_set = 0;
    bool has_transform = false;
    float t[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
};

// Apply a baked texture transform. Callers skip this when has_transform is false.
inline vec2 apply_tex_transform(const TexSlot &s, vec2 uv) noexcept
{
    return { (s.t[0] * uv.x) + (s.t[1] * uv.y) + s.t[2], (s.t[3] * uv.x) + (s.t[4] * uv.y) + s.t[5] };
}

// True when two bindings can reuse one sample. Image deduplication alone is insufficient
// because bindings may select different UV sets or transforms.
inline bool same_uv_mapping(const TexSlot &a, const TexSlot &b) noexcept
{
    if (a.uv_set != b.uv_set || a.has_transform != b.has_transform)
    {
        return false;
    }
    if (a.has_transform)
    {
        for (int i = 0; i < 6; i++)
        {
            if (a.t[i] != b.t[i])
            {
                return false;
            }
        }
    }
    return true;
}

// When adding a texture slot, also update decode_textures()'s explicit remap list.
struct Material
{
    vec3 diffuse = { 1.0f, 1.0f, 1.0f };
    vec3 ambient = { 1.0f, 1.0f, 1.0f }; // Ka; defaults to Kd when Ka absent in MTL
    vec3 specular = { 0.4f, 0.4f, 0.4f };
    float shininess = 32.0f;
    // Added after lighting. glTF loaders bake emissive strength into this factor.
    vec3 emissive = { 0.0f, 0.0f, 0.0f };
    TexSlot diffuse_map;
    TexSlot specular_map;
    TexSlot normal_map;
    TexSlot emissive_map;
    // Scales tangent-space X/Y before TBN transformation.
    float normal_scale = 1.0f;
    // Metallic-roughness inputs used by Phong shading.
    float metallic = 0.0f;  // metallicFactor; >0 enables the Phong specular-tint metallic remap
    float roughness = 1.0f; // roughnessFactor; baked into shininess at load, re-read per-texel only with an MR texture
    TexSlot mr_map;         // metallic-roughness (G=roughness, B=metallic)
    // The occlusion map's R channel replaces baked vertex AO to avoid double-darkening.
    TexSlot occlusion_map;
    float occlusion_strength = 1.0f;
    bool double_sided = false;
    float alpha_cutoff = 0.0f; // 0 = disabled; >0 = discard pixels with diffuse-tex alpha below this
    // Blend routes the material through alpha-over compositing; alpha_cutoff remains opaque.
    bool blend = false;
    float alpha = 1.0f;
    // Unlit materials bypass lighting but still apply alpha cutout.
    bool unlit = false;
};

struct Light
{
    // Direction toward the light source (world space, unit vector).
    vec3 direction = { 0.408f, 0.816f, 0.408f };
    vec3 color = { 1.0f, 1.0f, 1.0f };
};

// Tag for callers that guarantee a unit normal.
struct assume_unit_t
{
};
inline constexpr assume_unit_t assume_unit{};

// Compute ndh squared directly to avoid normalizing the half-vector.
inline void apply_light(vec3 &result, const vec3 &n, const vec3 &v, const Light &light, const Material &mat) noexcept
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

// Avoid copying a full Material in the Phong inner loop.
inline void apply_light(
    vec3 &result,
    const vec3 &n,
    const vec3 &v,
    const Light &light,
    const vec3 &diffuse,
    const vec3 &specular,
    float shininess
) noexcept
{
    const float diff = dot(n, light.direction);
    if (diff >= 0.0f)
    {
        result += light.color * diffuse * diff;

        const vec3 h = light.direction + v;
        const float ndh_raw = dot(n, h);
        if (ndh_raw > 0.0f)
        {
            const float hh = dot(h, h);
            const float ndh_sq = (ndh_raw * ndh_raw) / hh;
            result += light.color * specular * specular_pow_sq(ndh_sq, shininess);
        }
    }
}

// Sum directional Blinn-Phong lighting without copying Material. `v` must be unit length;
// this function normalizes `normal`. The caller clamps the result for display.
inline vec3 compute_lighting(
    vec3 normal,
    const vec3 &v,
    const Light *lights,
    int n_lights,
    const vec3 &ambient_scene,
    const vec3 &diffuse,
    const vec3 &ambient_mat,
    const vec3 &specular,
    float shininess,
    float ao = 1.0f
) noexcept
{
    const vec3 n = normalize(normal);
    vec3 result = ambient_scene * ambient_mat * ao;
    for (int i = 0; i < n_lights; i++)
    {
        apply_light(result, n, v, lights[i], diffuse, specular, shininess);
    }
    return result;
}

// Flat and loaded vertex normals are already unit length, so these overloads skip a sqrt.
inline vec3 compute_lighting(
    [[maybe_unused]] assume_unit_t tag,
    const vec3 &n,
    const vec3 &v,
    const Light *lights,
    int n_lights,
    const vec3 &ambient,
    const Material &mat = {},
    float ao = 1.0f
) noexcept
{
    vec3 result = ambient * mat.ambient * ao;
    for (int i = 0; i < n_lights; i++)
    {
        apply_light(result, n, v, lights[i], mat);
    }
    return result;
}

inline vec3 compute_lighting(
    [[maybe_unused]] assume_unit_t tag,
    vec3 pos,
    const vec3 &normal,
    const vec3 &eye_pos,
    const Light *lights,
    int n_lights,
    const vec3 &ambient,
    const Material &mat = {},
    float ao = 1.0f
) noexcept
{
    return compute_lighting(assume_unit, normal, normalize(eye_pos - pos), lights, n_lights, ambient, mat, ao);
}
