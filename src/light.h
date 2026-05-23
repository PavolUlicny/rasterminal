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

// Map glTF roughness [0,1] to a Blinn-Phong shininess exponent. Used by the loader
// (scalar roughnessFactor) and the Phong rasterizer (per-texel roughness from the MR
// texture), so the mapping is defined once.
inline float roughness_to_shininess(float roughness) noexcept
{
    return ((1.0f - roughness) * 126.0f) + 2.0f;
}

// Per-surface material properties (from MTL Ka/Kd/Ks/Ns/map_Kd or defaults).
// NOTE: when adding a new *_tex field, also update the remap loop in
// decode_textures() (mesh_loader.h) — it enumerates each one explicitly.
struct Material
{
    vec3 diffuse = { 1.0f, 1.0f, 1.0f };
    vec3 ambient = { 1.0f, 1.0f, 1.0f }; // Ka; defaults to Kd when Ka absent in MTL
    vec3 specular = { 0.4f, 0.4f, 0.4f };
    float shininess = 32.0f;
    // Self-illumination added post-lighting (after shadow lerp) so shaded areas still glow.
    // Modulated by emissive_tex when present. A zero factor skips the per-pixel add and any
    // emissive_tex sample (per glTF spec: emissive = factor * texture, so factor 0 ⇒ 0).
    // NOTE: Mesh::load_model promotes a zero factor to {1,1,1} when emissive_tex >= 0 after
    // decode (industry convention for "author bound a texture but forgot the factor"); a
    // legitimate explicit-zero-with-texture material cannot be distinguished from default-zero.
    // emissive_was_promoted (declared below alongside double_sided to pack the two bools into
    // a single slot) tags promoted materials so the renderer can suppress the factor under
    // the texture toggle without affecting authored factors.
    vec3 emissive = { 0.0f, 0.0f, 0.0f };
    // Texture slot indices into Mesh::textures (-1 = none).
    int diffuse_tex = -1;
    int specular_tex = -1;
    int normal_tex = -1;
    int emissive_tex = -1;
    // glTF metallic-roughness (Phong path only; 0/-1 defaults = dielectric, no
    // per-pixel metallic work — non-glTF loaders leave these untouched).
    float metallic = 0.0f;  // metallicFactor; >0 enables the Phong specular-tint metallic remap
    float roughness = 1.0f; // roughnessFactor; baked into shininess at load, re-read per-texel only with an MR texture
    int metallic_roughness_tex = -1; // index into Mesh::textures (G=roughness, B=metallic), or -1 if none
    bool double_sided = false;
    bool emissive_was_promoted = false; // packs into the same alignment slot as double_sided
    float alpha_cutoff = 0.0f;          // 0 = disabled; >0 = discard pixels with diffuse-tex alpha below this

    // Emissive factor to feed the rasterizer given the current texture toggle:
    // authored factors always pass; loader-promoted {1,1,1} factors are zeroed when textures
    // are off (otherwise toggling textures off on a BoomBox/DamagedHelmet-style asset would
    // render solid white from the leftover factor add). Encapsulated here so the gate stays
    // in one place across the Flat/Gouraud and Phong call sites in renderer.cpp.
    [[nodiscard]] vec3 effective_emissive(bool show_emissive) const noexcept
    {
        return (!emissive_was_promoted || show_emissive) ? emissive : vec3{ 0.0f, 0.0f, 0.0f };
    }
};

struct Light
{
    // Direction toward the light source (world space, unit vector).
    vec3 direction = { 0.408f, 0.816f, 0.408f };
    vec3 color = { 1.0f, 1.0f, 1.0f };
};

// Tag for callers that guarantee the normal is already unit-length.
// Skips the internal normalize() — saves one sqrt per call.
struct assume_unit_t
{
};
inline constexpr assume_unit_t assume_unit{};

// Half-vector normalize skipped: ndh² = (n·h)² / (h·h), saves one sqrt per light.
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

// Shading-params overload: takes only the four fields used by lighting, avoiding the
// ~88 B Material copy the Phong inner loop would otherwise pay per pixel.
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

// Blinn-Phong illumination summed over an array of directional lights.
// ambient is a scene-level term added once (not per light).
// v must be the unit view vector (normalize(eye - pos)) — precomputed by caller.
// Returns RGB in [0, ~1+]; caller is responsible for clamping before display.
inline vec3 compute_lighting(
    vec3 normal,
    const vec3 &v,
    const Light *lights,
    int n_lights,
    const vec3 &ambient,
    const Material &mat = {},
    float ao = 1.0f
) noexcept
{
    const vec3 n = normalize(normal);
    vec3 result = ambient * mat.ambient * ao;
    for (int i = 0; i < n_lights; i++)
    {
        apply_light(result, n, v, lights[i], mat);
    }
    return result;
}

// Shading-params overload: mirrors the (normal, v, …, Material) variant above but takes
// only the four fields lighting actually consumes. Used by rasterize_phong to skip the
// per-pixel Material struct copy. Behaviour is bit-identical to the Material version
// when called with the same diffuse/ambient/specular/shininess values.
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

// Convenience overload: derives v from pos and eye_pos.
// Used by Flat/Gouraud paths where v is not precomputed.
inline vec3 compute_lighting(
    vec3 pos,
    vec3 normal,
    const vec3 &eye_pos,
    const Light *lights,
    int n_lights,
    const vec3 &ambient,
    const Material &mat = {},
    float ao = 1.0f
) noexcept
{
    return compute_lighting(normal, normalize(eye_pos - pos), lights, n_lights, ambient, mat, ao);
}

// assume_unit overloads: caller guarantees normal is already unit-length.
// Flat face normals and Gouraud vertex normals are always unit after load;
// skipping normalize() saves one sqrt per Flat triangle and per Gouraud vertex.
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
