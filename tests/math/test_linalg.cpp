#include "tests/test.h"
#include "src/linalg.h"
#include <cmath>

// ─── vec3 ─────────────────────────────────────────────────────────────────────

TEST(vec3, dot_product)
{
    vec3 a{ 1, 2, 3 }, b{ 4, -5, 6 };
    ASSERT_NEAR(dot(a, b), (1.0f * 4) + (2 * -5) + (3 * 6), 1e-6f);
}

TEST(vec3, cross_product_right_hand)
{
    vec3 x{ 1, 0, 0 }, y{ 0, 1, 0 };
    vec3 z = cross(x, y);
    ASSERT_NEAR(z.x, 0.0f, 1e-6f);
    ASSERT_NEAR(z.y, 0.0f, 1e-6f);
    ASSERT_NEAR(z.z, 1.0f, 1e-6f);
}

TEST(vec3, cross_anticommutative)
{
    vec3 a{ 1, 2, 3 }, b{ 4, 5, 6 };
    vec3 ab = cross(a, b);
    vec3 ba = cross(b, a);
    ASSERT_NEAR(ab.x, -ba.x, 1e-6f);
    ASSERT_NEAR(ab.y, -ba.y, 1e-6f);
    ASSERT_NEAR(ab.z, -ba.z, 1e-6f);
}

TEST(vec3, normalize_unit_length)
{
    vec3 n = normalize(vec3{ 3, 0, 4 });
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
    vec4 v{ 2, 3, 5, 1 };
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
    {
        for (int r = 0; r < 4; r++)
        {
            ASSERT_NEAR(II.m[c][r], c == r ? 1.0f : 0.0f, 1e-6f);
        }
    }
}

TEST(mat4, default_constructor_is_zero)
{
    mat4 m;
    for (auto &c : m.m)
    {
        for (float r : c)
        {
            ASSERT_NEAR(r, 0.0f, 0.0f);
        }
    }
}

TEST(mat4, multiplication_is_associative)
{
    // (A*B)*C == A*(B*C) for any matrices — use three well-conditioned view matrices.
    mat4 A = look_at(vec3{ 1.0f, 2.0f, 3.0f }, vec3{ 0.0f, 0.0f, 0.0f }, vec3{ 0.0f, 1.0f, 0.0f });
    mat4 B = look_at(vec3{ 0.0f, 0.0f, 5.0f }, vec3{ 1.0f, 0.0f, 0.0f }, vec3{ 0.0f, 1.0f, 0.0f });
    mat4 C = look_at(vec3{ -2.0f, 1.0f, 4.0f }, vec3{ 1.0f, 1.0f, 1.0f }, vec3{ 0.0f, 1.0f, 0.0f });
    vec4 p{ 1.0f, 1.0f, 1.0f, 1.0f };
    vec4 lhs = (A * B) * C * p;
    vec4 rhs = A * (B * C) * p;
    ASSERT_NEAR(lhs.x, rhs.x, 1e-4f);
    ASSERT_NEAR(lhs.y, rhs.y, 1e-4f);
    ASSERT_NEAR(lhs.z, rhs.z, 1e-4f);
    ASSERT_NEAR(lhs.w, rhs.w, 1e-4f);
}

TEST(mat4, multiplication_is_noncommutative)
{
    // T*R * origin != R*T * origin: T translates +x, R rotates 90° about y (built inline).
    mat4 T = mat4::identity();
    T.m[3][0] = 1.0f;
    mat4 R = mat4::identity();
    R.m[0][0] = 0.0f;
    R.m[2][0] = 1.0f;
    R.m[0][2] = -1.0f;
    R.m[2][2] = 0.0f;
    vec4 origin{ 0.0f, 0.0f, 0.0f, 1.0f };
    vec4 tr = T * R * origin; // rotate first (no-op on origin), then translate: (1,0,0)
    vec4 rt = R * T * origin; // translate first → (1,0,0), then rotate 90° about y → (0,0,-1)
    float diff = (tr - rt).xyz().length();
    ASSERT_TRUE(diff > 0.5f);
}

// ─── projection ───────────────────────────────────────────────────────────────

TEST(perspective, point_on_near_plane_has_z_ndc_minus_one)
{
    mat4 P = perspective(to_radians(60.0f), 1.0f, 1.0f, 100.0f);
    vec4 clip = P * vec4{ 0, 0, -1, 1 }; // point at z=-near
    vec3 ndc = clip.perspective_divide();
    ASSERT_NEAR(ndc.z, -1.0f, 1e-5f);
}

TEST(perspective, point_on_far_plane_has_z_ndc_plus_one)
{
    mat4 P = perspective(to_radians(60.0f), 1.0f, 1.0f, 100.0f);
    vec4 clip = P * vec4{ 0, 0, -100, 1 };
    vec3 ndc = clip.perspective_divide();
    ASSERT_NEAR(ndc.z, 1.0f, 1e-5f);
}

TEST(perspective, off_axis_point_within_frustum_projects_inside_ndc)
{
    mat4 P = perspective(to_radians(60.0f), 1.0f, 1.0f, 100.0f);
    vec4 clip = P * vec4{ 0.5f, 0.3f, -10.0f, 1.0f };
    vec3 ndc = clip.perspective_divide();
    ASSERT_TRUE(ndc.x > -1.0f && ndc.x < 1.0f);
    ASSERT_TRUE(ndc.y > -1.0f && ndc.y < 1.0f);
    ASSERT_TRUE(ndc.z > -1.0f && ndc.z < 1.0f);
}

TEST(perspective, wide_aspect_squashes_x_relative_to_y)
{
    // aspect=2: the x extent is twice the y extent, so equal scene coords produce smaller ndc.x.
    mat4 P = perspective(to_radians(60.0f), 2.0f, 1.0f, 100.0f);
    vec4 clip = P * vec4{ 1.0f, 1.0f, -5.0f, 1.0f };
    vec3 ndc = clip.perspective_divide();
    ASSERT_TRUE(std::abs(ndc.x) < std::abs(ndc.y));
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
    vec3 r = normalize(vec3{ 0, 0, 0 });
    ASSERT_NEAR(r.x, 0.0f, 1e-6f);
    ASSERT_NEAR(r.y, 0.0f, 1e-6f);
    ASSERT_NEAR(r.z, 0.0f, 1e-6f);
}

TEST(vec3, lerp_at_t0_returns_a)
{
    vec3 a{ 1, 2, 3 }, b{ 4, 5, 6 };
    vec3 r = lerp(a, b, 0.0f);
    ASSERT_NEAR(r.x, 1.0f, 1e-6f);
    ASSERT_NEAR(r.y, 2.0f, 1e-6f);
    ASSERT_NEAR(r.z, 3.0f, 1e-6f);
}

TEST(vec3, lerp_at_t1_returns_b)
{
    vec3 a{ 1, 2, 3 }, b{ 4, 5, 6 };
    vec3 r = lerp(a, b, 1.0f);
    ASSERT_NEAR(r.x, 4.0f, 1e-6f);
    ASSERT_NEAR(r.y, 5.0f, 1e-6f);
    ASSERT_NEAR(r.z, 6.0f, 1e-6f);
}

TEST(vec3, lerp_midpoint)
{
    vec3 a{ 0, 0, 0 }, b{ 2, 4, 6 };
    vec3 r = lerp(a, b, 0.5f);
    ASSERT_NEAR(r.x, 1.0f, 1e-6f);
    ASSERT_NEAR(r.y, 2.0f, 1e-6f);
    ASSERT_NEAR(r.z, 3.0f, 1e-6f);
}

TEST(vec3, normalize_unit_vector_is_idempotent)
{
    vec3 n = normalize(vec3{ 1.0f, 0.0f, 0.0f });
    ASSERT_NEAR(n.x, 1.0f, 1e-6f);
    ASSERT_NEAR(n.y, 0.0f, 1e-6f);
    ASSERT_NEAR(n.z, 0.0f, 1e-6f);
}

TEST(vec3, normalize_just_above_threshold)
{
    // length_sq = 1e-6, well above the 1e-16 guard; must normalize to unit.
    vec3 n = normalize(vec3{ 1e-3f, 0.0f, 0.0f });
    ASSERT_NEAR(n.length(), 1.0f, 1e-5f);
}

TEST(vec3, normalize_below_threshold_returns_zero)
{
    // length_sq ≈ 1e-20, below the 1e-16 guard → returns {0,0,0}.
    vec3 n = normalize(vec3{ 1e-10f, 0.0f, 0.0f });
    ASSERT_NEAR(n.x, 0.0f, 1e-6f);
    ASSERT_NEAR(n.y, 0.0f, 1e-6f);
    ASSERT_NEAR(n.z, 0.0f, 1e-6f);
}

TEST(vec3, cross_parallel_returns_zero)
{
    // {1,2,3} and {2,4,6} are parallel (scalar multiple); cross product is zero.
    vec3 r = cross(vec3{ 1, 2, 3 }, vec3{ 2, 4, 6 });
    ASSERT_NEAR(r.x, 0.0f, 1e-5f);
    ASSERT_NEAR(r.y, 0.0f, 1e-5f);
    ASSERT_NEAR(r.z, 0.0f, 1e-5f);
}

TEST(vec3, dot_orthogonal_is_zero)
{
    ASSERT_NEAR(dot(vec3{ 1, 0, 0 }, vec3{ 0, 1, 0 }), 0.0f, 1e-6f);
    ASSERT_NEAR(dot(vec3{ 0, 1, 0 }, vec3{ 0, 0, 1 }), 0.0f, 1e-6f);
    ASSERT_NEAR(dot(vec3{ 1, 0, 0 }, vec3{ 0, 0, 1 }), 0.0f, 1e-6f);
}

TEST(vec3, dot_self_is_length_sq)
{
    vec3 v{ 3.0f, 4.0f, 5.0f };
    ASSERT_NEAR(dot(v, v), v.length_sq(), 1e-5f);
}

TEST(vec3, lerp_extrapolates_past_one)
{
    vec3 a{ 0, 0, 0 }, b{ 1, 1, 1 };
    vec3 r = lerp(a, b, 2.0f);
    ASSERT_NEAR(r.x, 2.0f, 1e-6f);
    ASSERT_NEAR(r.y, 2.0f, 1e-6f);
    ASSERT_NEAR(r.z, 2.0f, 1e-6f);
}

TEST(vec3, lerp_negative_t)
{
    vec3 a{ 1, 1, 1 }, b{ 3, 3, 3 };
    vec3 r = lerp(a, b, -1.0f);
    ASSERT_NEAR(r.x, -1.0f, 1e-6f);
    ASSERT_NEAR(r.y, -1.0f, 1e-6f);
    ASSERT_NEAR(r.z, -1.0f, 1e-6f);
}

TEST(vec3, arithmetic_operators_componentwise)
{
    vec3 a{ 1.0f, 2.0f, 3.0f };
    a += vec3{ 4.0f, 5.0f, 6.0f };
    ASSERT_NEAR(a.x, 5.0f, 1e-6f);
    ASSERT_NEAR(a.y, 7.0f, 1e-6f);
    ASSERT_NEAR(a.z, 9.0f, 1e-6f);

    vec3 b{ 2.0f, 3.0f, 4.0f };
    b *= 2.0f;
    ASSERT_NEAR(b.x, 4.0f, 1e-6f);
    ASSERT_NEAR(b.y, 6.0f, 1e-6f);
    ASSERT_NEAR(b.z, 8.0f, 1e-6f);

    vec3 neg = -vec3{ 1.0f, -2.0f, 3.0f };
    ASSERT_NEAR(neg.x, -1.0f, 1e-6f);
    ASSERT_NEAR(neg.y, 2.0f, 1e-6f);
    ASSERT_NEAR(neg.z, -3.0f, 1e-6f);

    vec3 cw = vec3{ 1.0f, 2.0f, 3.0f } * vec3{ 4.0f, 5.0f, 6.0f };
    ASSERT_NEAR(cw.x, 4.0f, 1e-6f);
    ASSERT_NEAR(cw.y, 10.0f, 1e-6f);
    ASSERT_NEAR(cw.z, 18.0f, 1e-6f);

    vec3 dv = vec3{ 4.0f, 6.0f, 8.0f } / 2.0f;
    ASSERT_NEAR(dv.x, 2.0f, 1e-6f);
    ASSERT_NEAR(dv.y, 3.0f, 1e-6f);
    ASSERT_NEAR(dv.z, 4.0f, 1e-6f);
}

TEST(vec3, length_sq_matches_length_squared)
{
    vec3 v{ 3.0f, 4.0f, 5.0f };
    float l = v.length();
    ASSERT_NEAR(v.length_sq(), l * l, 1e-4f);
}

// ─── vec4 ─────────────────────────────────────────────────────────────────────

TEST(vec4, perspective_divide_divides_by_w)
{
    vec4 v{ 2.0f, 4.0f, 6.0f, 2.0f };
    vec3 r = v.perspective_divide();
    ASSERT_NEAR(r.x, 1.0f, 1e-6f);
    ASSERT_NEAR(r.y, 2.0f, 1e-6f);
    ASSERT_NEAR(r.z, 3.0f, 1e-6f);
}

TEST(vec4, perspective_divide_w_one_is_identity)
{
    vec4 v{ 3.0f, 5.0f, 7.0f, 1.0f };
    vec3 r = v.perspective_divide();
    ASSERT_NEAR(r.x, 3.0f, 1e-6f);
    ASSERT_NEAR(r.y, 5.0f, 1e-6f);
    ASSERT_NEAR(r.z, 7.0f, 1e-6f);
}

TEST(vec4, perspective_divide_negative_w)
{
    // Negative w is well-defined: divides all components by w.
    vec3 r = vec4{ 2.0f, 4.0f, 6.0f, -2.0f }.perspective_divide();
    ASSERT_NEAR(r.x, -1.0f, 1e-6f);
    ASSERT_NEAR(r.y, -2.0f, 1e-6f);
    ASSERT_NEAR(r.z, -3.0f, 1e-6f);
}

TEST(vec4, arithmetic_operators)
{
    vec4 a{ 1.0f, 2.0f, 3.0f, 4.0f };
    vec4 b{ 5.0f, 6.0f, 7.0f, 8.0f };
    vec4 s = a + b;
    ASSERT_NEAR(s.x, 6.0f, 1e-6f);
    ASSERT_NEAR(s.y, 8.0f, 1e-6f);
    ASSERT_NEAR(s.z, 10.0f, 1e-6f);
    ASSERT_NEAR(s.w, 12.0f, 1e-6f);

    vec4 d = b - a;
    ASSERT_NEAR(d.x, 4.0f, 1e-6f);
    ASSERT_NEAR(d.y, 4.0f, 1e-6f);
    ASSERT_NEAR(d.z, 4.0f, 1e-6f);
    ASSERT_NEAR(d.w, 4.0f, 1e-6f);

    vec4 m = a * 3.0f;
    ASSERT_NEAR(m.x, 3.0f, 1e-6f);
    ASSERT_NEAR(m.y, 6.0f, 1e-6f);
    ASSERT_NEAR(m.z, 9.0f, 1e-6f);
    ASSERT_NEAR(m.w, 12.0f, 1e-6f);
}

TEST(vec4, vec3_constructor_copies_xyz)
{
    vec3 v{ 1.0f, 2.0f, 3.0f };
    vec4 r{ v, 5.0f };
    ASSERT_NEAR(r.x, 1.0f, 1e-6f);
    ASSERT_NEAR(r.y, 2.0f, 1e-6f);
    ASSERT_NEAR(r.z, 3.0f, 1e-6f);
    ASSERT_NEAR(r.w, 5.0f, 1e-6f);
}

// ─── vec2 ─────────────────────────────────────────────────────────────────────

TEST(vec2, arithmetic_operators)
{
    vec2 a{ 3.0f, 7.0f };
    vec2 b{ 1.0f, 2.0f };

    vec2 s = a + b;
    ASSERT_NEAR(s.x, 4.0f, 1e-6f);
    ASSERT_NEAR(s.y, 9.0f, 1e-6f);

    vec2 d = a - b;
    ASSERT_NEAR(d.x, 2.0f, 1e-6f);
    ASSERT_NEAR(d.y, 5.0f, 1e-6f);

    vec2 m = a * 2.0f;
    ASSERT_NEAR(m.x, 6.0f, 1e-6f);
    ASSERT_NEAR(m.y, 14.0f, 1e-6f);

    vec2 q = a / 2.0f;
    ASSERT_NEAR(q.x, 1.5f, 1e-6f);
    ASSERT_NEAR(q.y, 3.5f, 1e-6f);
}

// ─── look_at ──────────────────────────────────────────────────────────────────

TEST(look_at, eye_transforms_to_origin)
{
    vec3 eye{ 0, 0, 5 }, target{ 0, 0, 0 }, up{ 0, 1, 0 };
    mat4 V = look_at(eye, target, up);
    vec4 e = V * vec4{ eye, 1.0f };
    ASSERT_NEAR(e.x, 0.0f, 1e-5f);
    ASSERT_NEAR(e.y, 0.0f, 1e-5f);
    ASSERT_NEAR(e.z, 0.0f, 1e-5f);
}

TEST(look_at, target_lands_on_negative_z_at_distance)
{
    vec3 eye{ 0, 0, 5 }, target{ 0, 0, 0 }, up{ 0, 1, 0 };
    mat4 V = look_at(eye, target, up);
    vec4 t = V * vec4{ target, 1.0f };
    ASSERT_NEAR(t.x, 0.0f, 1e-5f);
    ASSERT_NEAR(t.y, 0.0f, 1e-5f);
    ASSERT_NEAR(t.z, -5.0f, 1e-5f);
}

TEST(look_at, upper_left_3x3_is_orthonormal)
{
    // Rows of the rotation part must be unit-length and mutually orthogonal.
    vec3 eye{ 1, 2, 3 }, target{ -1, 0, 1 }, up{ 0, 1, 0 };
    mat4 V = look_at(eye, target, up);
    vec3 row0{ V.m[0][0], V.m[1][0], V.m[2][0] };
    vec3 row1{ V.m[0][1], V.m[1][1], V.m[2][1] };
    vec3 row2{ V.m[0][2], V.m[1][2], V.m[2][2] };
    ASSERT_NEAR(row0.length(), 1.0f, 1e-5f);
    ASSERT_NEAR(row1.length(), 1.0f, 1e-5f);
    ASSERT_NEAR(row2.length(), 1.0f, 1e-5f);
    ASSERT_NEAR(dot(row0, row1), 0.0f, 1e-5f);
    ASSERT_NEAR(dot(row0, row2), 0.0f, 1e-5f);
    ASSERT_NEAR(dot(row1, row2), 0.0f, 1e-5f);
}

TEST(look_at, non_unit_up_is_reorthogonalised)
{
    // up is neither unit-length nor perpendicular to forward; cross+normalize fixes it.
    vec3 eye{ 0, 0, 5 }, target{ 0, 0, 0 }, up{ 0.5f, 1.0f, 0.0f };
    mat4 V = look_at(eye, target, up);
    vec3 row0{ V.m[0][0], V.m[1][0], V.m[2][0] };
    vec3 row1{ V.m[0][1], V.m[1][1], V.m[2][1] };
    vec3 row2{ V.m[0][2], V.m[1][2], V.m[2][2] };
    ASSERT_NEAR(row0.length(), 1.0f, 1e-5f);
    ASSERT_NEAR(row1.length(), 1.0f, 1e-5f);
    ASSERT_NEAR(row2.length(), 1.0f, 1e-5f);
    ASSERT_NEAR(dot(row0, row1), 0.0f, 1e-5f);
}

TEST(look_at, up_parallel_to_forward_degenerates_right_row)
{
    // forward = (0,0,-1), up = (0,0,1) → cross = 0 → normalize returns 0 → r row is zero.
    vec3 eye{ 0, 0, 5 }, target{ 0, 0, 0 }, up{ 0, 0, 1 };
    mat4 V = look_at(eye, target, up);
    ASSERT_NEAR(V.m[0][0], 0.0f, 1e-6f);
    ASSERT_NEAR(V.m[1][0], 0.0f, 1e-6f);
    ASSERT_NEAR(V.m[2][0], 0.0f, 1e-6f);
}

TEST(look_at, eye_equals_target_degenerates_rotation)
{
    // forward = normalize(0) = 0 → all rotation rows are zero.
    vec3 pos{ 1, 2, 3 };
    mat4 V = look_at(pos, pos, vec3{ 0, 1, 0 });
    vec3 row0{ V.m[0][0], V.m[1][0], V.m[2][0] };
    vec3 row1{ V.m[0][1], V.m[1][1], V.m[2][1] };
    ASSERT_NEAR(row0.length(), 0.0f, 1e-6f);
    ASSERT_NEAR(row1.length(), 0.0f, 1e-6f);
}

TEST(look_at, non_axis_aligned_eye_maps_to_origin)
{
    vec3 eye{ 3, 4, 5 }, target{ 1, 1, 1 }, up{ 0, 1, 0 };
    mat4 V = look_at(eye, target, up);
    vec4 e = V * vec4{ eye, 1.0f };
    ASSERT_NEAR(e.x, 0.0f, 1e-4f);
    ASSERT_NEAR(e.y, 0.0f, 1e-4f);
    ASSERT_NEAR(e.z, 0.0f, 1e-4f);
}

// ─── quat ─────────────────────────────────────────────────────────────────────

TEST(quat, identity_rotate_is_no_op)
{
    vec3 v{ 1.0f, 2.0f, 3.0f };
    vec3 r = quat::identity().rotate(v);
    ASSERT_NEAR(r.x, 1.0f, 1e-5f);
    ASSERT_NEAR(r.y, 2.0f, 1e-5f);
    ASSERT_NEAR(r.z, 3.0f, 1e-5f);
}

TEST(quat, rotate_90_around_y_maps_x_to_neg_z)
{
    // 90° rotation about +Y maps +X → -Z (right-handed).
    vec3 r = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(90.0f)).rotate({ 1, 0, 0 });
    ASSERT_NEAR(r.x, 0.0f, 1e-5f);
    ASSERT_NEAR(r.y, 0.0f, 1e-5f);
    ASSERT_NEAR(r.z, -1.0f, 1e-5f);
}

TEST(quat, rotate_90_around_y_maps_z_to_pos_x)
{
    vec3 r = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(90.0f)).rotate({ 0, 0, 1 });
    ASSERT_NEAR(r.x, 1.0f, 1e-5f);
    ASSERT_NEAR(r.y, 0.0f, 1e-5f);
    ASSERT_NEAR(r.z, 0.0f, 1e-5f);
}

TEST(quat, rotate_90_around_x_maps_y_to_pos_z)
{
    // 90° rotation about +X maps +Y → +Z.
    vec3 r = quat::from_axis_angle({ 1.0f, 0.0f, 0.0f }, to_radians(90.0f)).rotate({ 0, 1, 0 });
    ASSERT_NEAR(r.x, 0.0f, 1e-5f);
    ASSERT_NEAR(r.y, 0.0f, 1e-5f);
    ASSERT_NEAR(r.z, 1.0f, 1e-5f);
}

TEST(quat, rotate_preserves_vector_length)
{
    vec3 v{ 3.0f, 4.0f, 0.0f };
    quat q = quat::from_axis_angle(normalize(vec3{ 1.0f, 1.0f, 0.0f }), to_radians(73.0f));
    vec3 r = q.rotate(v);
    ASSERT_NEAR(r.length(), v.length(), 1e-5f);
}

TEST(quat, normalize_produces_unit_length)
{
    quat q{ 1.0f, 2.0f, 3.0f, 4.0f };
    quat n = normalize(q);
    float len_sq = (n.x * n.x) + (n.y * n.y) + (n.z * n.z) + (n.w * n.w);
    ASSERT_NEAR(len_sq, 1.0f, 1e-5f);
}

TEST(quat, normalize_zero_returns_identity)
{
    quat n = normalize(quat{ 0.0f, 0.0f, 0.0f, 0.0f });
    ASSERT_NEAR(n.x, 0.0f, 1e-6f);
    ASSERT_NEAR(n.y, 0.0f, 1e-6f);
    ASSERT_NEAR(n.z, 0.0f, 1e-6f);
    ASSERT_NEAR(n.w, 1.0f, 1e-6f);
}

TEST(quat, composition_identity_is_neutral)
{
    quat q = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(45.0f));
    vec3 via_q = q.rotate({ 1, 0, 0 });
    vec3 via_qi = (q * quat::identity()).rotate({ 1, 0, 0 });
    ASSERT_NEAR(via_qi.x, via_q.x, 1e-5f);
    ASSERT_NEAR(via_qi.y, via_q.y, 1e-5f);
    ASSERT_NEAR(via_qi.z, via_q.z, 1e-5f);
}

TEST(quat, two_90deg_rotations_equal_180deg)
{
    // Two 90° rotations around Y should map +X to -X (same as a 180° rotation).
    quat q90 = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(90.0f));
    vec3 r = (q90 * q90).rotate({ 1, 0, 0 });
    ASSERT_NEAR(r.x, -1.0f, 1e-5f);
    ASSERT_NEAR(r.y, 0.0f, 1e-5f);
    ASSERT_NEAR(r.z, 0.0f, 1e-5f);
}

TEST(quat, composition_is_noncommutative)
{
    // Rotating around X then Y produces a different result than Y then X.
    quat rx = quat::from_axis_angle({ 1.0f, 0.0f, 0.0f }, to_radians(90.0f));
    quat ry = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(90.0f));
    vec3 rxy = (rx * ry).rotate({ 0, 1, 0 });
    vec3 ryx = (ry * rx).rotate({ 0, 1, 0 });
    float diff = (rxy - ryx).length();
    ASSERT_TRUE(diff > 0.1f);
}

TEST(quat, normalize_unit_is_idempotent)
{
    quat q = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(45.0f));
    quat n = normalize(q);
    float len_sq = (n.x * n.x) + (n.y * n.y) + (n.z * n.z) + (n.w * n.w);
    ASSERT_NEAR(len_sq, 1.0f, 1e-5f);
    // Normalizing again should leave it unchanged.
    quat nn = normalize(n);
    ASSERT_NEAR(nn.x, n.x, 1e-6f);
    ASSERT_NEAR(nn.y, n.y, 1e-6f);
    ASSERT_NEAR(nn.z, n.z, 1e-6f);
    ASSERT_NEAR(nn.w, n.w, 1e-6f);
}

TEST(quat, normalize_below_threshold_returns_identity)
{
    // length_sq = 1e-10 < 1e-8 threshold → returns identity.
    quat n = normalize(quat{ 1e-5f, 0.0f, 0.0f, 0.0f });
    ASSERT_NEAR(n.x, 0.0f, 1e-6f);
    ASSERT_NEAR(n.y, 0.0f, 1e-6f);
    ASSERT_NEAR(n.z, 0.0f, 1e-6f);
    ASSERT_NEAR(n.w, 1.0f, 1e-6f);
}

TEST(quat, from_axis_angle_full_turn_is_no_op)
{
    vec3 v{ 1.0f, 2.0f, 3.0f };
    quat q = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(360.0f));
    vec3 r = q.rotate(v);
    ASSERT_NEAR(r.x, v.x, 1e-4f);
    ASSERT_NEAR(r.y, v.y, 1e-4f);
    ASSERT_NEAR(r.z, v.z, 1e-4f);
}

TEST(quat, from_axis_angle_zero_angle_is_identity)
{
    vec3 v{ 1.0f, 2.0f, 3.0f };
    quat q = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, 0.0f);
    vec3 r = q.rotate(v);
    ASSERT_NEAR(r.x, v.x, 1e-6f);
    ASSERT_NEAR(r.y, v.y, 1e-6f);
    ASSERT_NEAR(r.z, v.z, 1e-6f);
}

TEST(quat, from_axis_angle_negative_is_inverse)
{
    vec3 v{ 1.0f, 0.0f, 0.0f };
    vec3 axis{ 0.0f, 1.0f, 0.0f };
    float angle = to_radians(73.0f);
    quat q_fwd = quat::from_axis_angle(axis, angle);
    quat q_bwd = quat::from_axis_angle(axis, -angle);
    vec3 r = q_bwd.rotate(q_fwd.rotate(v));
    ASSERT_NEAR(r.x, v.x, 1e-4f);
    ASSERT_NEAR(r.y, v.y, 1e-4f);
    ASSERT_NEAR(r.z, v.z, 1e-4f);
}

TEST(quat, from_axis_angle_non_unit_axis_scales_imaginary)
{
    // Precondition: axis must be unit. With axis=(2,0,0), imaginary part = axis*sin(half).
    // At 90°, sin(45°) ≈ 0.7071, so q.x ≈ 2*0.7071 ≈ 1.414 > 1.
    quat q = quat::from_axis_angle({ 2.0f, 0.0f, 0.0f }, to_radians(90.0f));
    ASSERT_TRUE(q.x > 1.0f);
}

// ─── util edge cases ─────────────────────────────────────────────────────────

TEST(util, clamp_lo_equals_hi)
{
    ASSERT_NEAR(clamp(0.0f, 5.0f, 5.0f), 5.0f, 0.0f);
    ASSERT_NEAR(clamp(5.0f, 5.0f, 5.0f), 5.0f, 0.0f);
    ASSERT_NEAR(clamp(10.0f, 5.0f, 5.0f), 5.0f, 0.0f);
}

TEST(util, clamp_negative_range)
{
    ASSERT_NEAR(clamp(-15.0f, -10.0f, -1.0f), -10.0f, 0.0f);
    ASSERT_NEAR(clamp(-5.0f, -10.0f, -1.0f), -5.0f, 0.0f);
    ASSERT_NEAR(clamp(5.0f, -10.0f, -1.0f), -1.0f, 0.0f);
}

TEST(util, to_radians_negative)
{
    ASSERT_NEAR(to_radians(-180.0f), -3.14159265f, 1e-5f);
}

TEST(util, to_radians_360)
{
    ASSERT_NEAR(to_radians(360.0f), 2.0f * 3.14159265f, 1e-5f);
}

// ─── perspective() degenerate planes ─────────────────────────────────────────

TEST(perspective, near_equals_far_produces_nonfinite)
{
    // (far - near) = 0 → divide by zero → non-finite m[2][2] and m[3][2].
    // volatile prevents MSVC from constant-folding at compile time (C4723).
    volatile float same = 1.0f;
    mat4 P = perspective(to_radians(60.0f), 1.0f, static_cast<float>(same), static_cast<float>(same));
    ASSERT_FALSE(std::isfinite(P.m[2][2]));
}

TEST(perspective, near_greater_than_far_produces_finite)
{
    // far < near: denominator (far - near) is negative but non-zero → finite result.
    // Depth range is inverted but no singularity occurs.
    mat4 P = perspective(to_radians(60.0f), 1.0f, 10.0f, 1.0f);
    ASSERT_TRUE(std::isfinite(P.m[2][2]));
    ASSERT_TRUE(std::isfinite(P.m[3][2]));
}

// ─── normalize(quat) 1e-8 threshold boundary ─────────────────────────────────

TEST(quat, normalize_just_below_threshold_returns_identity)
{
    // len_sq = (sqrt(9.9e-9))^2 = 9.9e-9 < 1e-8 → guard fires → identity.
    const float mag = std::sqrt(9.9e-9f);
    quat n = normalize(quat{ mag, 0.0f, 0.0f, 0.0f });
    ASSERT_NEAR(n.w, 1.0f, 1e-6f);
    ASSERT_NEAR(n.x, 0.0f, 1e-6f);
}

TEST(quat, normalize_just_above_threshold_normalizes)
{
    // len_sq = (sqrt(1.01e-8))^2 = 1.01e-8 > 1e-8 → guard does not fire → unit quat.
    const float mag = std::sqrt(1.01e-8f);
    quat n = normalize(quat{ mag, 0.0f, 0.0f, 0.0f });
    const float len_sq = (n.x * n.x) + (n.y * n.y) + (n.z * n.z) + (n.w * n.w);
    ASSERT_NEAR(len_sq, 1.0f, 1e-5f);
}
