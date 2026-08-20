#include "tests/test.h"
#include "src/math/light.h"

#include <cmath>

// All tests use unit-length directions. compute_lighting normalizes its normal
// argument internally but expects light directions to already be unit. The lit
// math is driven through the production shading-params overload
// compute_lighting(normal, v, …, diffuse, ambient, specular, shininess, ao): the
// same one rasterize_phong uses; a Material's fields are passed out explicitly.

static constexpr float EPS = 1e-5f;

// ambient-only behaviour

TEST(light, no_lights_returns_ambient_times_diffuse)
{
    Material mat;
    mat.diffuse = { 0.5f, 0.5f, 0.5f };
    mat.ambient = { 0.5f, 0.5f, 0.5f }; // Ka == Kd (the common case when Ka is absent in MTL)
    vec3 ambient{ 0.2f, 0.2f, 0.2f };
    vec3 v{ 0, 0, 1 };

    vec3 r = compute_lighting(
        vec3{ 0, 1, 0 }, v, nullptr, 0, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess, 1.0f
    );
    ASSERT_NEAR(r.x, 0.1f, EPS); // 0.2 * 0.5 * 1.0
    ASSERT_NEAR(r.y, 0.1f, EPS);
    ASSERT_NEAR(r.z, 0.1f, EPS);
}

TEST(light, ao_scales_ambient)
{
    Material mat;
    vec3 ambient{ 0.4f, 0.4f, 0.4f };
    vec3 v{ 0, 0, 1 };

    vec3 full = compute_lighting(
        vec3{ 0, 1, 0 }, v, nullptr, 0, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess, 1.0f
    );
    vec3 half = compute_lighting(
        vec3{ 0, 1, 0 }, v, nullptr, 0, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess, 0.5f
    );

    ASSERT_NEAR(half.x, full.x * 0.5f, EPS);
    ASSERT_NEAR(half.y, full.y * 0.5f, EPS);
    ASSERT_NEAR(half.z, full.z * 0.5f, EPS);
}

TEST(light, ao_does_not_affect_direct_diffuse)
{
    // A light aligned with the normal: diffuse contribution = 1.0.
    // AO is ambient-only, so changing AO must leave the direct term unchanged.
    Material mat;
    mat.diffuse = { 1, 1, 1 };
    mat.specular = { 0, 0, 0 }; // isolate diffuse
    Light l;
    l.direction = { 0, 1, 0 };
    l.color = { 1, 1, 1 };
    vec3 ambient{ 0, 0, 0 }; // kill ambient to expose direct only
    vec3 v{ 0, 0, 1 };
    vec3 n{ 0, 1, 0 };

    vec3 full = compute_lighting(n, v, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess, 1.0f);
    vec3 zero_ao = compute_lighting(n, v, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess, 0.0f);

    // With ambient=0, full and zero_ao should be identical.
    ASSERT_NEAR(full.x, zero_ao.x, EPS);
    ASSERT_NEAR(full.y, zero_ao.y, EPS);
    ASSERT_NEAR(full.z, zero_ao.z, EPS);
}

// diffuse (Lambert)

TEST(light, perpendicular_light_gives_max_diffuse)
{
    Material mat;
    mat.diffuse = { 1, 1, 1 };
    mat.specular = { 0, 0, 0 };
    Light l;
    l.direction = { 0, 1, 0 };
    l.color = { 1, 1, 1 };
    vec3 ambient{ 0, 0, 0 };
    vec3 v{ 0, 1, 0 }; // view along same direction → no specular peak issues
    vec3 n{ 0, 1, 0 }; // aligned with light → dot=1

    vec3 r = compute_lighting(n, v, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);
    // Diffuse = diffuse_color * light_color * dot = 1*1*1 = 1 per channel,
    // plus specular_pow(1, 32) with specular=0 → 0. Result = (1,1,1).
    ASSERT_NEAR(r.x, 1.0f, EPS);
    ASSERT_NEAR(r.y, 1.0f, EPS);
    ASSERT_NEAR(r.z, 1.0f, EPS);
}

TEST(light, back_facing_light_contributes_no_diffuse)
{
    Material mat;
    mat.diffuse = { 1, 1, 1 };
    mat.specular = { 0, 0, 0 };
    Light l;
    l.direction = { 0, 1, 0 };
    l.color = { 1, 1, 1 };
    vec3 ambient{ 0, 0, 0 };
    vec3 v{ 0, 0, 1 };
    vec3 n{ 0, -1, 0 }; // faces away from light → dot = -1

    vec3 r = compute_lighting(n, v, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);
    ASSERT_NEAR(r.x, 0.0f, EPS);
    ASSERT_NEAR(r.y, 0.0f, EPS);
    ASSERT_NEAR(r.z, 0.0f, EPS);
}

TEST(light, tangential_light_contributes_no_diffuse)
{
    Material mat;
    mat.diffuse = { 1, 1, 1 };
    mat.specular = { 0, 0, 0 };
    Light l;
    l.direction = { 1, 0, 0 };
    l.color = { 1, 1, 1 };
    vec3 ambient{ 0, 0, 0 };
    vec3 v{ 0, 0, 1 };
    vec3 n{ 0, 1, 0 }; // perpendicular to light → dot = 0, no contribution

    vec3 r = compute_lighting(n, v, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);
    ASSERT_NEAR(r.x, 0.0f, EPS);
}

TEST(light, light_color_tints_diffuse)
{
    Material mat;
    mat.diffuse = { 1, 1, 1 };
    mat.specular = { 0, 0, 0 };
    Light l;
    l.direction = { 0, 1, 0 };
    l.color = { 1, 0, 0 }; // pure red
    vec3 ambient{ 0, 0, 0 };
    vec3 v{ 0, 1, 0 };
    vec3 n{ 0, 1, 0 };

    vec3 r = compute_lighting(n, v, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);
    ASSERT_NEAR(r.x, 1.0f, EPS);
    ASSERT_NEAR(r.y, 0.0f, EPS);
    ASSERT_NEAR(r.z, 0.0f, EPS);
}

// specular (Blinn-Phong)

TEST(light, specular_peaks_when_half_vector_equals_normal)
{
    // If view direction == light direction, then half-vector == light == normal,
    // so dot(n, h) = 1 and specular term = mat.specular * 1 ^ shininess.
    Material mat;
    mat.diffuse = { 0, 0, 0 }; // isolate specular
    mat.specular = { 1, 1, 1 };
    mat.shininess = 32.0f;
    Light l;
    l.direction = { 0, 1, 0 };
    l.color = { 1, 1, 1 };
    vec3 ambient{ 0, 0, 0 };
    vec3 v{ 0, 1, 0 };
    vec3 n{ 0, 1, 0 };

    vec3 r = compute_lighting(n, v, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);
    ASSERT_NEAR(r.x, 1.0f, EPS);
    ASSERT_NEAR(r.y, 1.0f, EPS);
    ASSERT_NEAR(r.z, 1.0f, EPS);
}

TEST(light, specular_is_strictly_smaller_off_peak)
{
    Material mat;
    mat.diffuse = { 0, 0, 0 };
    mat.specular = { 1, 1, 1 };
    mat.shininess = 32.0f;
    Light l;
    l.direction = { 0, 1, 0 };
    l.color = { 1, 1, 1 };
    vec3 ambient{ 0, 0, 0 };
    vec3 n{ 0, 1, 0 };

    vec3 peak =
        compute_lighting(n, vec3{ 0, 1, 0 }, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);
    // View rotated 45° away from the light: half-vector no longer aligns
    // with the normal, so specular contribution drops.
    vec3 off = compute_lighting(
        n, normalize(vec3{ 1, 1, 0 }), &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess
    );
    ASSERT_TRUE(off.x < peak.x);
    ASSERT_TRUE(off.x >= 0.0f);
}

TEST(light, multiple_lights_sum_their_contributions)
{
    Material mat;
    mat.diffuse = { 1, 1, 1 };
    mat.specular = { 0, 0, 0 };
    Light ls[2];
    ls[0].direction = { 0, 1, 0 };
    ls[0].color = { 0.3f, 0, 0 };
    ls[1].direction = { 0, 1, 0 };
    ls[1].color = { 0, 0.5f, 0 };
    vec3 ambient{ 0, 0, 0 };
    vec3 v{ 0, 1, 0 };
    vec3 n{ 0, 1, 0 };

    vec3 r = compute_lighting(n, v, ls, 2, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);
    ASSERT_NEAR(r.x, 0.3f, EPS);
    ASSERT_NEAR(r.y, 0.5f, EPS);
    ASSERT_NEAR(r.z, 0.0f, EPS);
}

// specular_pow_sq fast paths
// specular_pow_sq(ndh², s) computes ndh^s = (ndh²)^(s/2).
// The fast paths for shininess ∈ {8, 16, 32} must match the general exp2/log2 form.

TEST(specular_pow_sq, shininess_32_matches_power)
{
    float x = 0.8f;
    float got = specular_pow_sq(x * x, 32.0f);
    float expected = std::pow(x, 32.0f);
    ASSERT_NEAR(got, expected, 1e-5f);
}

TEST(specular_pow_sq, shininess_16_matches_power)
{
    float x = 0.7f;
    float got = specular_pow_sq(x * x, 16.0f);
    float expected = std::pow(x, 16.0f);
    ASSERT_NEAR(got, expected, 1e-5f);
}

TEST(specular_pow_sq, shininess_8_matches_power)
{
    float x = 0.6f;
    float got = specular_pow_sq(x * x, 8.0f);
    float expected = std::pow(x, 8.0f);
    ASSERT_NEAR(got, expected, 1e-5f);
}

TEST(specular_pow_sq, shininess_1_returns_x)
{
    // (x²)^(1/2) = x; the exp2f/log2f general path handles this.
    ASSERT_NEAR(specular_pow_sq(0.5f * 0.5f, 1.0f), 0.5f, 1e-5f);
}

TEST(specular_pow_sq, shininess_zero_returns_one)
{
    // (x²)^0 = 1 for any x; exp2f(0) = 1.
    ASSERT_NEAR(specular_pow_sq(0.5f * 0.5f, 0.0f), 1.0f, 1e-5f);
    ASSERT_NEAR(specular_pow_sq(0.9f * 0.9f, 0.0f), 1.0f, 1e-5f);
}

TEST(specular_pow_sq, zero_base_returns_zero)
{
    // (0²)^(n/2) = 0 for n > 0; log2f(0) = -inf, exp2f(-inf) = 0.
    ASSERT_NEAR(specular_pow_sq(0.0f, 32.0f), 0.0f, 1e-5f);
    ASSERT_NEAR(specular_pow_sq(0.0f, 1.0f), 0.0f, 1e-5f);
}

// robustness

TEST(light, normal_is_normalized_internally)
{
    // Passing a non-unit normal (scaled by 5) must give identical results to
    // the pre-normalized form since compute_lighting normalizes internally.
    Material mat;
    mat.diffuse = { 1, 1, 1 };
    mat.specular = { 1, 1, 1 };
    mat.shininess = 32.0f;
    Light l;
    l.direction = { 0, 1, 0 };
    l.color = { 1, 1, 1 };
    vec3 ambient{ 0.2f, 0.2f, 0.2f };
    vec3 v{ 0, 1, 0 };
    vec3 n_unit{ 0, 1, 0 };
    vec3 n_scaled{ 0, 5, 0 };

    vec3 r_unit = compute_lighting(n_unit, v, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);
    vec3 r_scaled =
        compute_lighting(n_scaled, v, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);

    ASSERT_NEAR(r_unit.x, r_scaled.x, EPS);
    ASSERT_NEAR(r_unit.y, r_scaled.y, EPS);
    ASSERT_NEAR(r_unit.z, r_scaled.z, EPS);
}

TEST(light, multiple_lights_sum_specular)
{
    // Two lights, specular isolated (diffuse = 0), distinct colors.
    // Result must equal the per-light sum.
    Material mat;
    mat.diffuse = { 0, 0, 0 };
    mat.specular = { 1, 1, 1 };
    mat.shininess = 32.0f;
    Light ls[2];
    ls[0].direction = { 0, 1, 0 };
    ls[0].color = { 0.4f, 0, 0 };
    ls[1].direction = { 0, 1, 0 };
    ls[1].color = { 0, 0.6f, 0 };
    vec3 ambient{ 0, 0, 0 };
    vec3 v{ 0, 1, 0 }; // half-vector == normal → ndh = 1 for both lights
    vec3 n{ 0, 1, 0 };

    // Individual contributions
    vec3 r0 = compute_lighting(n, v, &ls[0], 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);
    vec3 r1 = compute_lighting(n, v, &ls[1], 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);

    // Combined
    vec3 r = compute_lighting(n, v, ls, 2, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);

    ASSERT_NEAR(r.x, r0.x + r1.x, EPS);
    ASSERT_NEAR(r.y, r0.y + r1.y, EPS);
    ASSERT_NEAR(r.z, r0.z + r1.z, EPS);
}

TEST(light, ao_zero_zeroes_ambient)
{
    // ao=0 must zero the ambient term even when ambient and mat.ambient are non-zero.
    Material mat;
    mat.ambient = { 1.0f, 1.0f, 1.0f };
    vec3 ambient{ 0.5f, 0.5f, 0.5f };
    vec3 v{ 0, 0, 1 };
    vec3 r = compute_lighting(
        vec3{ 0, 1, 0 }, v, nullptr, 0, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess, 0.0f
    );
    ASSERT_NEAR(r.x, 0.0f, EPS);
    ASSERT_NEAR(r.y, 0.0f, EPS);
    ASSERT_NEAR(r.z, 0.0f, EPS);
}

TEST(light, ndh_zero_gates_specular)
{
    // v = -l → h = l + v = {0,0,0}: ndh_raw = 0, gate fires before hh division.
    // Without the gate, hh = 0 → 0/0 = NaN. Gate must suppress specular cleanly.
    Material mat;
    mat.diffuse = { 0.0f, 0.0f, 0.0f };
    mat.specular = { 1.0f, 1.0f, 1.0f };
    mat.shininess = 32.0f;
    Light l;
    l.direction = { 0.0f, 1.0f, 0.0f };
    l.color = { 1.0f, 1.0f, 1.0f };
    vec3 ambient{ 0, 0, 0 };
    vec3 n{ 0.0f, 1.0f, 0.0f };

    vec3 r = compute_lighting(
        n, vec3{ 0.0f, -1.0f, 0.0f }, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess
    );

    ASSERT_NEAR(r.x, 0.0f, EPS);
    ASSERT_NEAR(r.y, 0.0f, EPS);
    ASSERT_NEAR(r.z, 0.0f, EPS);
}

TEST(light, specular_independent_of_diffuse_branch)
{
    // n={0,1,0}, l={1,0,0}: diff=dot(n,l)=0 so the diffuse branch (diff>0) is
    // not entered. v=normalize({-1,1,0}): h=normalize(l+v) has h.y≈0.924>0 so
    // the specular branch still fires. Proves the two branches are independent.
    Material mat;
    mat.diffuse = { 1, 1, 1 };
    mat.specular = { 1, 1, 1 };
    mat.shininess = 32.0f;
    Light l;
    l.direction = { 1, 0, 0 }; // tangential to n={0,1,0}: diff = 0, branch skipped
    l.color = { 1, 1, 1 };
    vec3 ambient{ 0, 0, 0 };
    vec3 n{ 0, 1, 0 };
    vec3 v = normalize(vec3{ -1, 1, 0 }); // half-vector tilted toward n → ndh > 0

    vec3 r = compute_lighting(n, v, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);
    ASSERT_TRUE(r.x > 0.0f);
}

TEST(light, negative_ndh_suppresses_specular)
{
    // diff=1 (enter outer if); v={0,-3,0} → h={0,-2,0} → ndh_raw=-2 < 0 → inner
    // if skipped. Result must be diffuse-only; specular={1,1,1} would inflate it.
    Material mat;
    mat.diffuse = { 0.8f, 0.6f, 0.4f };
    mat.specular = { 1.0f, 1.0f, 1.0f };
    mat.shininess = 32.0f;
    Light l;
    l.direction = { 0.0f, 1.0f, 0.0f };
    l.color = { 1.0f, 1.0f, 1.0f };
    vec3 ambient{ 0.0f, 0.0f, 0.0f };
    vec3 n{ 0.0f, 1.0f, 0.0f };
    vec3 v{ 0.0f, -3.0f, 0.0f }; // h = l+v = {0,-2,0}, ndh_raw = -2

    vec3 r = compute_lighting(n, v, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);
    ASSERT_NEAR(r.x, 0.8f, EPS);
    ASSERT_NEAR(r.y, 0.6f, EPS);
    ASSERT_NEAR(r.z, 0.4f, EPS);
}

// specular_pow_sq intermediate shininess

TEST(specular_pow_sq, shininess_4_matches_power)
{
    const float x = 0.75f;
    ASSERT_NEAR(specular_pow_sq(x * x, 4.0f), std::pow(x, 4.0f), 1e-5f);
}

TEST(specular_pow_sq, shininess_12_matches_power)
{
    const float x = 0.9f;
    ASSERT_NEAR(specular_pow_sq(x * x, 12.0f), std::pow(x, 12.0f), 1e-5f);
}

// assume_unit overloads

TEST(light, assume_unit_precomputed_v_matches_regular)
{
    // The assume_unit (no internal normalize) overload must match the normalizing
    // shading-params overload for a unit normal.
    Material mat;
    mat.diffuse = { 0.8f, 0.6f, 0.4f };
    mat.specular = { 0.3f, 0.3f, 0.3f };
    mat.shininess = 16.0f;
    Light l;
    l.direction = { 0.0f, 1.0f, 0.0f };
    l.color = { 1.0f, 1.0f, 1.0f };
    vec3 ambient{ 0.1f, 0.1f, 0.1f };
    vec3 n{ 0.0f, 1.0f, 0.0f };
    vec3 v = normalize(vec3{ 0.0f, 1.0f, 1.0f });

    vec3 r_reg = compute_lighting(n, v, &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess);
    vec3 r_au = compute_lighting(assume_unit, n, v, &l, 1, ambient, mat);

    ASSERT_NEAR(r_au.x, r_reg.x, EPS);
    ASSERT_NEAR(r_au.y, r_reg.y, EPS);
    ASSERT_NEAR(r_au.z, r_reg.z, EPS);
}

TEST(light, assume_unit_pos_eye_derives_v_correctly)
{
    // The pos/eye_pos assume_unit overload must derive v == normalize(eye_pos - pos)
    // and match the normalizing reference fed that same v.
    Material mat;
    mat.diffuse = { 1.0f, 1.0f, 1.0f };
    mat.specular = { 0.5f, 0.5f, 0.5f };
    mat.shininess = 32.0f;
    Light l;
    l.direction = { 0.0f, 1.0f, 0.0f };
    l.color = { 1.0f, 1.0f, 1.0f };
    vec3 ambient{ 0.2f, 0.2f, 0.2f };
    const float ao = 0.7f;
    vec3 normal{ 0.0f, 1.0f, 0.0f };
    vec3 pos{ 0.0f, 0.0f, 0.0f };
    vec3 eye_pos{ 0.0f, 2.0f, 3.0f };

    vec3 r_reg = compute_lighting(
        normal, normalize(eye_pos - pos), &l, 1, ambient, mat.diffuse, mat.ambient, mat.specular, mat.shininess, ao
    );
    vec3 r_au = compute_lighting(assume_unit, pos, normal, eye_pos, &l, 1, ambient, mat, ao);

    ASSERT_NEAR(r_au.x, r_reg.x, EPS);
    ASSERT_NEAR(r_au.y, r_reg.y, EPS);
    ASSERT_NEAR(r_au.z, r_reg.z, EPS);
}
