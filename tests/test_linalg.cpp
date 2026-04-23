#include "test.h"
#include "../src/linalg.h"

// ─── vec3 ─────────────────────────────────────────────────────────────────────

TEST(vec3, dot_product)
{
    vec3 a{1, 2, 3}, b{4, -5, 6};
    ASSERT_NEAR(dot(a, b), 1.0f * 4 + 2 * -5 + 3 * 6, 1e-6f);
}

TEST(vec3, cross_product_right_hand)
{
    vec3 x{1, 0, 0}, y{0, 1, 0};
    vec3 z = cross(x, y);
    ASSERT_NEAR(z.x, 0.0f, 1e-6f);
    ASSERT_NEAR(z.y, 0.0f, 1e-6f);
    ASSERT_NEAR(z.z, 1.0f, 1e-6f);
}

TEST(vec3, cross_anticommutative)
{
    vec3 a{1, 2, 3}, b{4, 5, 6};
    vec3 ab = cross(a, b);
    vec3 ba = cross(b, a);
    ASSERT_NEAR(ab.x, -ba.x, 1e-6f);
    ASSERT_NEAR(ab.y, -ba.y, 1e-6f);
    ASSERT_NEAR(ab.z, -ba.z, 1e-6f);
}

TEST(vec3, normalize_unit_length)
{
    vec3 n = normalize(vec3{3, 0, 4});
    ASSERT_NEAR(n.length(), 1.0f, 1e-6f);
    ASSERT_NEAR(n.x, 0.6f, 1e-6f);
    ASSERT_NEAR(n.z, 0.8f, 1e-6f);
}

TEST(vec3, length_pythagorean)
{
    ASSERT_NEAR(vec3(3, 4, 0).length(), 5.0f, 1e-6f);
}

// ─── mat4 ─────────────────────────────────────────────────────────────────────

TEST(mat4, identity_times_vec_is_vec)
{
    mat4 I = mat4::identity();
    vec4 v{2, 3, 5, 1};
    vec4 r = I * v;
    ASSERT_NEAR(r.x, 2.0f, 1e-6f);
    ASSERT_NEAR(r.y, 3.0f, 1e-6f);
    ASSERT_NEAR(r.z, 5.0f, 1e-6f);
    ASSERT_NEAR(r.w, 1.0f, 1e-6f);
}

TEST(mat4, identity_times_identity_is_identity)
{
    mat4 I = mat4::identity();
    mat4 II = I * I;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            ASSERT_NEAR(II.m[c][r], c == r ? 1.0f : 0.0f, 1e-6f);
}

TEST(mat4, translation_applies_to_point)
{
    mat4 T = translation(1, 2, 3);
    vec4 p = T * vec4{10, 20, 30, 1};
    ASSERT_NEAR(p.x, 11.0f, 1e-6f);
    ASSERT_NEAR(p.y, 22.0f, 1e-6f);
    ASSERT_NEAR(p.z, 33.0f, 1e-6f);
}

TEST(mat4, translation_does_not_apply_to_direction)
{
    // Directions have w=0, so translation should leave them untouched.
    mat4 T = translation(1, 2, 3);
    vec4 d = T * vec4{10, 20, 30, 0};
    ASSERT_NEAR(d.x, 10.0f, 1e-6f);
    ASSERT_NEAR(d.y, 20.0f, 1e-6f);
    ASSERT_NEAR(d.z, 30.0f, 1e-6f);
}

TEST(mat4, scale_applies_componentwise)
{
    mat4 S = scale(2, 3, 4);
    vec4 p = S * vec4{1, 1, 1, 1};
    ASSERT_NEAR(p.x, 2.0f, 1e-6f);
    ASSERT_NEAR(p.y, 3.0f, 1e-6f);
    ASSERT_NEAR(p.z, 4.0f, 1e-6f);
}

TEST(mat4, rotation_y_90deg_maps_x_to_neg_z)
{
    // rotation_y(90°) rotates the x axis onto the -z axis (right-handed).
    mat4 R = rotation_y(to_radians(90.0f));
    vec4 r = R * vec4{1, 0, 0, 1};
    ASSERT_NEAR(r.x, 0.0f, 1e-5f);
    ASSERT_NEAR(r.y, 0.0f, 1e-5f);
    ASSERT_NEAR(r.z, -1.0f, 1e-5f);
}

TEST(mat4, compose_translation_then_scale)
{
    // Column-major: M = S * T means T is applied first, then S.
    mat4 T = translation(1, 0, 0);
    mat4 S = scale(2);
    mat4 M = S * T;
    vec4 p = M * vec4{0, 0, 0, 1};
    ASSERT_NEAR(p.x, 2.0f, 1e-6f); // (0,0,0) → translate → (1,0,0) → scale×2 → (2,0,0)
}

// ─── projection ───────────────────────────────────────────────────────────────

TEST(perspective, point_on_near_plane_has_z_ndc_minus_one)
{
    mat4 P = perspective(to_radians(60.0f), 1.0f, 1.0f, 100.0f);
    vec4 clip = P * vec4{0, 0, -1, 1}; // point at z=-near
    vec3 ndc = clip.perspective_divide();
    ASSERT_NEAR(ndc.z, -1.0f, 1e-5f);
}

TEST(perspective, point_on_far_plane_has_z_ndc_plus_one)
{
    mat4 P = perspective(to_radians(60.0f), 1.0f, 1.0f, 100.0f);
    vec4 clip = P * vec4{0, 0, -100, 1};
    vec3 ndc = clip.perspective_divide();
    ASSERT_NEAR(ndc.z, 1.0f, 1e-5f);
}

// ─── utilities ────────────────────────────────────────────────────────────────

TEST(util, clamp_bounds)
{
    ASSERT_NEAR(clamp(-5.0f, 0.0f, 10.0f), 0.0f, 0.0f);
    ASSERT_NEAR(clamp(5.0f, 0.0f, 10.0f), 5.0f, 0.0f);
    ASSERT_NEAR(clamp(15.0f, 0.0f, 10.0f), 10.0f, 0.0f);
}

TEST(util, to_radians)
{
    ASSERT_NEAR(to_radians(180.0f), 3.14159265f, 1e-5f);
    ASSERT_NEAR(to_radians(0.0f), 0.0f, 1e-6f);
}

// ─── vec3 additional ──────────────────────────────────────────────────────────

TEST(vec3, normalize_zero_vector_returns_zero)
{
    // length_sq < 1e-16f → returns default-constructed vec3{0,0,0}
    vec3 r = normalize(vec3{0, 0, 0});
    ASSERT_NEAR(r.x, 0.0f, 1e-6f);
    ASSERT_NEAR(r.y, 0.0f, 1e-6f);
    ASSERT_NEAR(r.z, 0.0f, 1e-6f);
}

TEST(vec3, lerp_at_t0_returns_a)
{
    vec3 a{1, 2, 3}, b{4, 5, 6};
    vec3 r = lerp(a, b, 0.0f);
    ASSERT_NEAR(r.x, 1.0f, 1e-6f);
    ASSERT_NEAR(r.y, 2.0f, 1e-6f);
    ASSERT_NEAR(r.z, 3.0f, 1e-6f);
}

TEST(vec3, lerp_at_t1_returns_b)
{
    vec3 a{1, 2, 3}, b{4, 5, 6};
    vec3 r = lerp(a, b, 1.0f);
    ASSERT_NEAR(r.x, 4.0f, 1e-6f);
    ASSERT_NEAR(r.y, 5.0f, 1e-6f);
    ASSERT_NEAR(r.z, 6.0f, 1e-6f);
}

TEST(vec3, lerp_midpoint)
{
    vec3 a{0, 0, 0}, b{2, 4, 6};
    vec3 r = lerp(a, b, 0.5f);
    ASSERT_NEAR(r.x, 1.0f, 1e-6f);
    ASSERT_NEAR(r.y, 2.0f, 1e-6f);
    ASSERT_NEAR(r.z, 3.0f, 1e-6f);
}

TEST(vec3, reflect_normal_incidence_inverts)
{
    // reflect((0,0,-1), (0,0,1)) = (0,0,1): straight reflection off Z+ surface.
    vec3 r = reflect(vec3{0, 0, -1}, vec3{0, 0, 1});
    ASSERT_NEAR(r.x, 0.0f, 1e-6f);
    ASSERT_NEAR(r.y, 0.0f, 1e-6f);
    ASSERT_NEAR(r.z, 1.0f, 1e-6f);
}

TEST(vec3, reflect_45_degree_flips_y)
{
    // reflect((1,-1,0)/√2, (0,1,0)) → (1,1,0)/√2: angle of incidence = angle of reflection.
    vec3 incident = normalize(vec3{1, -1, 0});
    vec3 r = reflect(incident, vec3{0, 1, 0});
    ASSERT_NEAR(r.x, incident.x, 1e-5f);
    ASSERT_NEAR(r.y, -incident.y, 1e-5f);
    ASSERT_NEAR(r.z, 0.0f, 1e-5f);
}

// ─── vec4 ─────────────────────────────────────────────────────────────────────

TEST(vec4, perspective_divide_divides_by_w)
{
    vec4 v{2.0f, 4.0f, 6.0f, 2.0f};
    vec3 r = v.perspective_divide();
    ASSERT_NEAR(r.x, 1.0f, 1e-6f);
    ASSERT_NEAR(r.y, 2.0f, 1e-6f);
    ASSERT_NEAR(r.z, 3.0f, 1e-6f);
}

TEST(vec4, perspective_divide_w_one_is_identity)
{
    vec4 v{3.0f, 5.0f, 7.0f, 1.0f};
    vec3 r = v.perspective_divide();
    ASSERT_NEAR(r.x, 3.0f, 1e-6f);
    ASSERT_NEAR(r.y, 5.0f, 1e-6f);
    ASSERT_NEAR(r.z, 7.0f, 1e-6f);
}

// ─── rotation_x / rotation_z ─────────────────────────────────────────────────

TEST(rotation_x, 90deg_maps_y_axis_to_z)
{
    mat4 R = rotation_x(to_radians(90.0f));
    vec4 r = R * vec4{0, 1, 0, 1};
    ASSERT_NEAR(r.x, 0.0f, 1e-5f);
    ASSERT_NEAR(r.y, 0.0f, 1e-5f);
    ASSERT_NEAR(r.z, 1.0f, 1e-5f);
}

TEST(rotation_x, 90deg_maps_z_axis_to_neg_y)
{
    mat4 R = rotation_x(to_radians(90.0f));
    vec4 r = R * vec4{0, 0, 1, 1};
    ASSERT_NEAR(r.x, 0.0f, 1e-5f);
    ASSERT_NEAR(r.y, -1.0f, 1e-5f);
    ASSERT_NEAR(r.z, 0.0f, 1e-5f);
}

TEST(rotation_z, 90deg_maps_x_axis_to_y)
{
    mat4 R = rotation_z(to_radians(90.0f));
    vec4 r = R * vec4{1, 0, 0, 1};
    ASSERT_NEAR(r.x, 0.0f, 1e-5f);
    ASSERT_NEAR(r.y, 1.0f, 1e-5f);
    ASSERT_NEAR(r.z, 0.0f, 1e-5f);
}

TEST(rotation_z, 90deg_maps_y_axis_to_neg_x)
{
    mat4 R = rotation_z(to_radians(90.0f));
    vec4 r = R * vec4{0, 1, 0, 1};
    ASSERT_NEAR(r.x, -1.0f, 1e-5f);
    ASSERT_NEAR(r.y, 0.0f, 1e-5f);
    ASSERT_NEAR(r.z, 0.0f, 1e-5f);
}

// ─── look_at ──────────────────────────────────────────────────────────────────

TEST(look_at, eye_transforms_to_origin)
{
    vec3 eye{0, 0, 5}, target{0, 0, 0}, up{0, 1, 0};
    mat4 V = look_at(eye, target, up);
    vec4 e = V * vec4{eye, 1.0f};
    ASSERT_NEAR(e.x, 0.0f, 1e-5f);
    ASSERT_NEAR(e.y, 0.0f, 1e-5f);
    ASSERT_NEAR(e.z, 0.0f, 1e-5f);
}

TEST(look_at, target_lands_on_negative_z_at_distance)
{
    vec3 eye{0, 0, 5}, target{0, 0, 0}, up{0, 1, 0};
    mat4 V = look_at(eye, target, up);
    vec4 t = V * vec4{target, 1.0f};
    ASSERT_NEAR(t.x, 0.0f, 1e-5f);
    ASSERT_NEAR(t.y, 0.0f, 1e-5f);
    ASSERT_NEAR(t.z, -5.0f, 1e-5f);
}

TEST(look_at, upper_left_3x3_is_orthonormal)
{
    // Rows of the rotation part must be unit-length and mutually orthogonal.
    vec3 eye{1, 2, 3}, target{-1, 0, 1}, up{0, 1, 0};
    mat4 V = look_at(eye, target, up);
    vec3 row0{V.m[0][0], V.m[1][0], V.m[2][0]};
    vec3 row1{V.m[0][1], V.m[1][1], V.m[2][1]};
    vec3 row2{V.m[0][2], V.m[1][2], V.m[2][2]};
    ASSERT_NEAR(row0.length(), 1.0f, 1e-5f);
    ASSERT_NEAR(row1.length(), 1.0f, 1e-5f);
    ASSERT_NEAR(row2.length(), 1.0f, 1e-5f);
    ASSERT_NEAR(dot(row0, row1), 0.0f, 1e-5f);
    ASSERT_NEAR(dot(row0, row2), 0.0f, 1e-5f);
    ASSERT_NEAR(dot(row1, row2), 0.0f, 1e-5f);
}
