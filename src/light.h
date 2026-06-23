#pragma once

#include "linalg.h"

#include <cmath>
#include <cstdint>

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

// One texture binding: the slot index into Mesh::textures (-1 = none), the UV set it
// samples (glTF textureInfo.texCoord: 0 = TEXCOORD_0, 1 = TEXCOORD_1), and an optional
// KHR_texture_transform. uv_set is always 0 for non-glTF loaders and is clamped to {0,1}
// at load (a reference to an absent set degrades to 0 — see mesh_gltf.cpp).
//
// has_transform/t[] carry KHR_texture_transform. t is a 2x3 affine applied to the
// interpolated UV before sampling: feed.x = t0*u + t1*v + t2; feed.y = t3*u + t4*v + t5.
// The glTF spec defines the transform on v-down UVs, but we store UVs v-flipped (and
// sample re-flips), so the loader folds flip∘transform∘flip into t (see bake_transform in
// mesh_gltf.cpp). Identity by default; callers gate the per-pixel apply on has_transform.
struct TexSlot
{
    int tex = -1;
    uint8_t uv_set = 0;
    bool has_transform = false;
    float t[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
};

// Apply a TexSlot's baked KHR_texture_transform affine to an interpolated UV. Identity
// when the slot has no transform, but callers gate on has_transform to skip it entirely.
inline vec2 apply_tex_transform(const TexSlot &s, vec2 uv) noexcept
{
    return { (s.t[0] * uv.x) + (s.t[1] * uv.y) + s.t[2], (s.t[3] * uv.x) + (s.t[4] * uv.y) + s.t[5] };
}

// Two slots address the texture identically — same UV set and same KHR_texture_transform — so a
// sample taken for one can be reused for the other. Used by the ORM fast path: the texture cache
// dedups by image only (not texCoord/transform), so two bindings sharing a Texture* may still
// differ in either. The baked t[] is bit-identical for equal authored transforms (same arithmetic
// on the same inputs), so exact comparison is correct here.
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

// Per-surface material properties (from MTL Ka/Kd/Ks/Ns/map_Kd or defaults).
// NOTE: when adding a new *_map TexSlot, also update the remap loop in
// decode_textures() (mesh_loader.h) — it enumerates each one explicitly.
struct Material
{
    vec3 diffuse = { 1.0f, 1.0f, 1.0f };
    vec3 ambient = { 1.0f, 1.0f, 1.0f }; // Ka; defaults to Kd when Ka absent in MTL
    vec3 specular = { 0.4f, 0.4f, 0.4f };
    float shininess = 32.0f;
    // Self-illumination added post-lighting (after shadow lerp) so shaded areas still glow.
    // Modulated by emissive_map when present. A zero factor skips the per-pixel add and the
    // emissive_map sample (per glTF spec: emissive = factor * texture, so factor 0 ⇒ 0). For
    // glTF, mesh_gltf bakes KHR_materials_emissive_strength into this factor at load.
    vec3 emissive = { 0.0f, 0.0f, 0.0f };
    // Texture bindings (slot index into Mesh::textures + per-slot UV set; -1 tex = none).
    TexSlot diffuse_map;
    TexSlot specular_map;
    TexSlot normal_map;
    TexSlot emissive_map;
    // glTF normalTexture.scale: scales the X/Y components of the sampled tangent-space normal
    // before TBN transformation (spec: scaledNormal = normalize((sample*2-1) * vec3(scale,scale,1))).
    // Default 1.0 = no-op; non-glTF loaders never touch this. Mesh::has_normal_scale gates the
    // per-pixel multiply so the common case (scale==1 or no normal map) pays zero.
    float normal_scale = 1.0f;
    // glTF metallic-roughness (Phong path only; 0/-1 defaults = dielectric, no
    // per-pixel metallic work — non-glTF loaders leave these untouched).
    float metallic = 0.0f;  // metallicFactor; >0 enables the Phong specular-tint metallic remap
    float roughness = 1.0f; // roughnessFactor; baked into shininess at load, re-read per-texel only with an MR texture
    TexSlot mr_map;         // metallic-roughness (G=roughness, B=metallic)
    // glTF occlusionTexture (Phong path only). The R channel is authored ambient occlusion; it
    // REPLACES the baked per-vertex AO per-pixel (both target the same scale — multiplying would
    // double-darken). occlusion_strength is occlusionTexture.strength: ao = 1 + strength*(R-1).
    // Mesh::has_occlusion gates the per-pixel sample; non-glTF loaders leave these untouched.
    TexSlot occlusion_map;
    float occlusion_strength = 1.0f;
    bool double_sided = false;
    float alpha_cutoff = 0.0f; // 0 = disabled; >0 = discard pixels with diffuse-tex alpha below this
    // Alpha blending (distinct from alpha_cutoff / MASK, which is a binary-discard opaque path).
    // blend = true routes the material's triangles to the transparent pass (alpha-OVER compositing);
    // alpha is the base opacity (glTF baseColorFactor.a / MTL d). Per-fragment opacity is
    // alpha * diffuse-texture.a * vertex-color.a. Mesh::has_transparent gates the whole path.
    bool blend = false;
    float alpha = 1.0f;
    // KHR_materials_unlit: bypass lighting/shadow/emissive/normal/occlusion and output
    // baseColor * diffuse texture * vertex color directly (alpha cutout still applies).
    bool unlit = false;
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
// ~92 B Material copy the Phong inner loop would otherwise pay per pixel.
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
