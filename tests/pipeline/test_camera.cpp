#include "tests/test.h"
#include "src/camera.h"

#include <cmath>

// Camera is a trackball orbit camera: eye sits on a sphere around `target`,
// at `distance` along the +Z axis of its `orientation` frame.  Orbit rotates
// around the camera's current local axes, not fixed world axes.

static constexpr float EPS = 1e-4f;

// ─── eye() ───────────────────────────────────────────────────────────────────

TEST(camera, eye_identity_orientation_puts_eye_on_plus_z)
{
    Camera c;
    c.target = { 0.0f, 0.0f, 0.0f };
    c.distance = 5.0f;
    c.orientation = quat::identity();
    vec3 e = c.eye();
    ASSERT_NEAR(e.x, 0.0f, EPS);
    ASSERT_NEAR(e.y, 0.0f, EPS);
    ASSERT_NEAR(e.z, 5.0f, EPS);
}

TEST(camera, eye_relative_to_target_offset)
{
    Camera c;
    c.target = { 10.0f, 20.0f, 30.0f };
    c.distance = 4.0f;
    c.orientation = quat::identity();
    vec3 e = c.eye();
    ASSERT_NEAR(e.x, 10.0f, EPS);
    ASSERT_NEAR(e.y, 20.0f, EPS);
    ASSERT_NEAR(e.z, 34.0f, EPS);
}

TEST(camera, eye_on_unit_sphere_scaled_by_distance)
{
    // For any orientation, |eye - target| must equal distance.
    Camera c;
    c.target = { 0.0f, 0.0f, 0.0f };
    c.distance = 7.5f;
    c.orientation = normalize(
        quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(37.0f)) *
        quat::from_axis_angle({ 1.0f, 0.0f, 0.0f }, to_radians(21.0f))
    );
    vec3 e = c.eye();
    float r = (e - c.target).length();
    ASSERT_NEAR(r, 7.5f, EPS);
}

// ─── view() matrix ───────────────────────────────────────────────────────────

TEST(camera, view_transforms_eye_to_origin)
{
    Camera c;
    c.target = { 1.0f, 2.0f, 3.0f };
    c.distance = 5.0f;
    c.orientation = normalize(
        quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(40.0f)) *
        quat::from_axis_angle({ 1.0f, 0.0f, 0.0f }, to_radians(15.0f))
    );
    mat4 V = c.view();
    vec3 e = c.eye();
    vec4 r = V * vec4{ e.x, e.y, e.z, 1.0f };
    ASSERT_NEAR(r.x, 0.0f, EPS);
    ASSERT_NEAR(r.y, 0.0f, EPS);
    ASSERT_NEAR(r.z, 0.0f, EPS);
}

TEST(camera, view_puts_target_on_negative_z_at_distance)
{
    // look_at makes forward (eye → target) the -Z axis in view space,
    // so the target in view space is (0, 0, -distance).
    Camera c;
    c.target = { -3.0f, 4.0f, 2.0f };
    c.distance = 6.0f;
    c.orientation = normalize(
        quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(20.0f)) *
        quat::from_axis_angle({ 1.0f, 0.0f, 0.0f }, to_radians(-10.0f))
    );
    mat4 V = c.view();
    vec4 r = V * vec4{ c.target.x, c.target.y, c.target.z, 1.0f };
    ASSERT_NEAR(r.x, 0.0f, EPS);
    ASSERT_NEAR(r.y, 0.0f, EPS);
    ASSERT_NEAR(r.z, -6.0f, EPS);
}

// ─── projection() ────────────────────────────────────────────────────────────

TEST(camera, projection_square_aspect)
{
    Camera c;
    c.fov = to_radians(60.0f);
    c.near_plane = 1.0f;
    c.far_plane = 100.0f;
    mat4 P = c.projection(100, 100); // aspect = 1

    // A point at z = -near on the axis should map to clip z = -near after
    // perspective divide (z_ndc = -1).
    vec4 clip = P * vec4{ 0, 0, -1, 1 };
    vec3 ndc = clip.perspective_divide();
    ASSERT_NEAR(ndc.z, -1.0f, 1e-5f);
}

TEST(camera, projection_wider_aspect_scales_x_less)
{
    // Wider viewport (aspect > 1) → same world x maps to smaller ndc.x,
    // because the horizontal FOV effectively increases to match the width.
    Camera c;
    c.fov = to_radians(60.0f);
    c.near_plane = 1.0f;
    c.far_plane = 100.0f;
    mat4 P_sq = c.projection(100, 100);
    mat4 P_wide = c.projection(200, 100);

    vec4 csq = P_sq * vec4{ 0.5f, 0, -2, 1 };
    vec4 cwide = P_wide * vec4{ 0.5f, 0, -2, 1 };
    vec3 nsq = csq.perspective_divide();
    vec3 nwide = cwide.perspective_divide();
    ASSERT_TRUE(std::fabs(nwide.x) < std::fabs(nsq.x));
}

TEST(camera, projection_degenerate_size_does_not_divide_by_zero)
{
    // Guard clause in Camera::projection falls back to aspect=1 when either
    // dimension is non-positive — must not produce NaN/inf.
    Camera c;
    mat4 P = c.projection(0, 100);
    ASSERT_TRUE(std::isfinite(P.m[0][0]));
    ASSERT_TRUE(std::isfinite(P.m[1][1]));
}

// ─── process_key() ───────────────────────────────────────────────────────────
// Tested behaviourally: verify that eye() moves in the expected world direction
// after each key, starting from identity orientation.

TEST(camera, process_key_A_model_moves_left)
{
    // A makes model appear to move left: camera orbits to +X side.
    Camera c;
    c.distance = 5.0f;
    c.orientation = quat::identity();
    c.process_key(platform::Key::A, 0.1f);
    vec3 e = c.eye();
    ASSERT_TRUE(e.x > 0.0f);
}

TEST(camera, process_key_D_model_moves_right)
{
    Camera c;
    c.distance = 5.0f;
    c.orientation = quat::identity();
    c.process_key(platform::Key::D, 0.1f);
    vec3 e = c.eye();
    ASSERT_TRUE(e.x < 0.0f);
}

TEST(camera, process_key_W_model_moves_up)
{
    // W makes model appear to move up (top toward camera): camera orbits downward.
    Camera c;
    c.distance = 5.0f;
    c.orientation = quat::identity();
    c.process_key(platform::Key::W, 0.1f);
    vec3 e = c.eye();
    ASSERT_TRUE(e.y < 0.0f);
}

TEST(camera, process_key_S_model_moves_down)
{
    Camera c;
    c.distance = 5.0f;
    c.orientation = quat::identity();
    c.process_key(platform::Key::S, 0.1f);
    vec3 e = c.eye();
    ASSERT_TRUE(e.y > 0.0f);
}

TEST(camera, process_key_LEFT_same_as_A)
{
    Camera c1, c2;
    c1.distance = c2.distance = 5.0f;
    c1.orientation = c2.orientation = quat::identity();
    c1.process_key(platform::Key::Left, 0.1f);
    c2.process_key(platform::Key::A, 0.1f);
    vec3 e1 = c1.eye(), e2 = c2.eye();
    ASSERT_NEAR(e1.x, e2.x, EPS);
    ASSERT_NEAR(e1.y, e2.y, EPS);
    ASSERT_NEAR(e1.z, e2.z, EPS);
}

TEST(camera, process_key_RIGHT_same_as_D)
{
    Camera c1, c2;
    c1.distance = c2.distance = 5.0f;
    c1.orientation = c2.orientation = quat::identity();
    c1.process_key(platform::Key::Right, 0.1f);
    c2.process_key(platform::Key::D, 0.1f);
    vec3 e1 = c1.eye(), e2 = c2.eye();
    ASSERT_NEAR(e1.x, e2.x, EPS);
    ASSERT_NEAR(e1.y, e2.y, EPS);
    ASSERT_NEAR(e1.z, e2.z, EPS);
}

TEST(camera, process_key_UP_same_as_W)
{
    Camera c1, c2;
    c1.distance = c2.distance = 5.0f;
    c1.orientation = c2.orientation = quat::identity();
    c1.process_key(platform::Key::Up, 0.1f);
    c2.process_key(platform::Key::W, 0.1f);
    vec3 e1 = c1.eye(), e2 = c2.eye();
    ASSERT_NEAR(e1.x, e2.x, EPS);
    ASSERT_NEAR(e1.y, e2.y, EPS);
    ASSERT_NEAR(e1.z, e2.z, EPS);
}

TEST(camera, process_key_DOWN_same_as_S)
{
    Camera c1, c2;
    c1.distance = c2.distance = 5.0f;
    c1.orientation = c2.orientation = quat::identity();
    c1.process_key(platform::Key::Down, 0.1f);
    c2.process_key(platform::Key::S, 0.1f);
    vec3 e1 = c1.eye(), e2 = c2.eye();
    ASSERT_NEAR(e1.x, e2.x, EPS);
    ASSERT_NEAR(e1.y, e2.y, EPS);
    ASSERT_NEAR(e1.z, e2.z, EPS);
}

TEST(camera, process_key_PLUS_decreases_distance)
{
    Camera c;
    c.distance = 3.0f;
    c.process_key(platform::Key::Plus, 0.1f);
    // zoom_speed = 3.0 * 1.5 = 4.5; distance -= 4.5 * 0.1 = 0.45
    ASSERT_NEAR(c.distance, 2.55f, 1e-3f);
}

TEST(camera, process_key_MINUS_increases_distance)
{
    Camera c;
    c.distance = 3.0f;
    c.process_key(platform::Key::Minus, 0.1f);
    ASSERT_NEAR(c.distance, 3.45f, 1e-3f);
}

TEST(camera, process_key_distance_clamped_at_near)
{
    Camera c;
    c.distance = 3.0f;
    c.near_plane = 0.01f;
    c.far_plane = 100.0f;
    c.process_key(platform::Key::Plus, 100.0f);
    ASSERT_NEAR(c.distance, c.near_plane * 2.0f, EPS);
}

TEST(camera, process_key_distance_clamped_at_far)
{
    Camera c;
    c.distance = 3.0f;
    c.near_plane = 0.01f;
    c.far_plane = 100.0f;
    c.process_key(platform::Key::Minus, 100.0f);
    ASSERT_NEAR(c.distance, c.far_plane * 0.5f, EPS);
}

TEST(camera, process_key_unknown_does_not_change_state)
{
    Camera c;
    c.distance = 5.0f;
    c.orientation = quat::identity();
    vec3 e_before = c.eye();
    float d_before = c.distance;
    c.process_key(platform::Key::Space, 1.0f);
    ASSERT_NEAR(c.distance, d_before, EPS);
    vec3 e_after = c.eye();
    ASSERT_NEAR(e_after.x, e_before.x, EPS);
    ASSERT_NEAR(e_after.y, e_before.y, EPS);
    ASSERT_NEAR(e_after.z, e_before.z, EPS);
}

// ─── orbit() — trackball-specific ────────────────────────────────────────────

TEST(camera, orbit_keeps_eye_on_sphere)
{
    // After arbitrary sequence of orbits, eye must remain at `distance` from target.
    Camera c;
    c.target = { 1.0f, 2.0f, 3.0f };
    c.distance = 6.0f;
    c.orientation = quat::identity();
    c.orbit(0.7f, 0.3f);
    c.orbit(-0.4f, 1.2f);
    c.orbit(2.1f, -0.9f);
    float r = (c.eye() - c.target).length();
    ASSERT_NEAR(r, 6.0f, 1e-4f);
}

TEST(camera, orbit_pure_yaw_and_pitch_are_commutative)
{
    // With world-Y yaw, a pure-yaw step followed by a pure-pitch step is
    // mathematically equivalent to pitch-then-yaw (the conjugation identity
    // cancels the intermediate axis rotation). This pins the turntable
    // behaviour that distinguishes world-Y yaw from the old local-up yaw.
    Camera c1, c2;
    c1.distance = c2.distance = 5.0f;
    c1.orientation = c2.orientation = quat::identity();

    c1.orbit(to_radians(45.0f), 0.0f);
    c1.orbit(0.0f, to_radians(30.0f));

    c2.orbit(0.0f, to_radians(30.0f));
    c2.orbit(to_radians(45.0f), 0.0f);

    float diff = (c1.eye() - c2.eye()).length();
    ASSERT_TRUE(diff < 1e-4f);
}

TEST(camera, orbit_does_not_gimbal_lock)
{
    // Drive many full pitch revolutions via small increments. The view matrix
    // must stay finite and the upper 3×3 rotation rows must stay unit-length.
    Camera c;
    c.target = { 0.0f, 0.0f, 0.0f };
    c.distance = 5.0f;
    c.orientation = quat::identity();

    static constexpr float step = 3.14159265f / 4.0f; // 45° steps
    for (int i = 0; i < 64; i++)                      // 8 full revolutions
    {
        c.orbit(0.0f, step);
    }

    mat4 v = c.view();
    for (auto &row : v.m)
    {
        for (float val : row)
        {
            ASSERT_TRUE(std::isfinite(val));
        }
    }

    for (int r = 0; r < 3; r++)
    {
        vec3 row{ v.m[0][r], v.m[1][r], v.m[2][r] };
        ASSERT_NEAR(row.length(), 1.0f, 1e-3f);
    }
}

// ─── spin_world_y() ──────────────────────────────────────────────────────────

TEST(camera, spin_world_y_rotates_around_world_up)
{
    // Spinning around world Y must not change the Y component of (eye - target).
    Camera c;
    c.target = { 0.0f, 0.0f, 0.0f };
    c.distance = 5.0f;
    // Start with some arbitrary pitch so the eye is not already on the XZ plane.
    c.orientation = quat::from_axis_angle({ 1.0f, 0.0f, 0.0f }, to_radians(-30.0f));
    float y_before = c.eye().y;

    c.spin_world_y(to_radians(90.0f));
    c.spin_world_y(to_radians(137.0f));

    ASSERT_NEAR(c.eye().y, y_before, 1e-3f);
}

TEST(camera, spin_world_y_rotates_xz_correctly)
{
    // Identity orientation → eye on +Z. A +90° world-Y spin rotates +Z toward +X
    // (right-hand rule around +Y: +Z → +X). Both distance and Y must be unchanged.
    Camera c;
    c.target = { 0.0f, 0.0f, 0.0f };
    c.distance = 5.0f;
    c.orientation = quat::identity();
    c.spin_world_y(to_radians(90.0f));
    vec3 e = c.eye();
    ASSERT_NEAR(e.x, 5.0f, EPS);
    ASSERT_NEAR(e.y, 0.0f, EPS);
    ASSERT_NEAR(e.z, 0.0f, EPS);
}

TEST(camera, spin_world_y_zero_is_noop)
{
    Camera c;
    c.target = { 0.0f, 0.0f, 0.0f };
    c.distance = 5.0f;
    c.orientation = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(37.0f));
    vec3 e_before = c.eye();
    c.spin_world_y(0.0f);
    vec3 e_after = c.eye();
    ASSERT_NEAR(e_after.x, e_before.x, EPS);
    ASSERT_NEAR(e_after.y, e_before.y, EPS);
    ASSERT_NEAR(e_after.z, e_before.z, EPS);
}

// ─── projection() additional degenerate inputs ────────────────────────────────

TEST(camera, projection_zero_height_fallback)
{
    // pixel_height = 0 fails the guard → aspect = 1, no division by zero.
    Camera c;
    mat4 P = c.projection(100, 0);
    ASSERT_TRUE(std::isfinite(P.m[0][0]));
    ASSERT_TRUE(std::isfinite(P.m[1][1]));
}

TEST(camera, projection_negative_dimension_fallback)
{
    // Negative width fails pixel_width > 0 → aspect = 1 fallback.
    Camera c;
    mat4 P = c.projection(-1, 100);
    ASSERT_TRUE(std::isfinite(P.m[0][0]));
    ASSERT_TRUE(std::isfinite(P.m[1][1]));
}

TEST(camera, projection_both_zero_fallback)
{
    // Both dimensions zero: guard (pixel_width > 0 && pixel_height > 0) is false → aspect=1.
    Camera c;
    mat4 P_sq = c.projection(100, 100);
    mat4 P_00 = c.projection(0, 0);
    ASSERT_NEAR(P_00.m[0][0], P_sq.m[0][0], EPS); // x-scale encodes aspect
}

// ─── view(eye_pos) overload ───────────────────────────────────────────────────

TEST(camera, view_explicit_eye_pos_maps_to_origin)
{
    // The two-arg overload must use the supplied position, not recompute eye().
    // We pass a position offset from the normal eye() — it must still map to origin.
    Camera c;
    c.target = { 2.0f, 3.0f, 4.0f };
    c.distance = 5.0f;
    c.orientation = normalize(quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(50.0f)));
    const vec3 custom_eye = c.target + vec3{ 0.0f, 2.0f, 6.0f };
    mat4 V = c.view(custom_eye);
    vec4 r = V * vec4{ custom_eye.x, custom_eye.y, custom_eye.z, 1.0f };
    ASSERT_NEAR(r.x, 0.0f, EPS);
    ASSERT_NEAR(r.y, 0.0f, EPS);
    ASSERT_NEAR(r.z, 0.0f, EPS);
}

TEST(camera, view_eye_at_target_does_not_crash)
{
    // eye == target → look_at forward vector is zero → normalize(zero) → NaN.
    // No guard; test documents the call completes without aborting.
    Camera c;
    c.target = { 1.0f, 2.0f, 3.0f };
    mat4 V = c.view(c.target);
    (void)V;
}

// ─── orbit() zero step ────────────────────────────────────────────────────────

TEST(camera, orbit_zero_is_noop)
{
    Camera c;
    c.target = { 0.0f, 0.0f, 0.0f };
    c.distance = 5.0f;
    c.orientation = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, to_radians(20.0f));
    vec3 e_before = c.eye();
    c.orbit(0.0f, 0.0f);
    vec3 e_after = c.eye();
    ASSERT_NEAR(e_after.x, e_before.x, EPS);
    ASSERT_NEAR(e_after.y, e_before.y, EPS);
    ASSERT_NEAR(e_after.z, e_before.z, EPS);
}

TEST(camera, orbit_compound_nonzero_matches_formula)
{
    // Single orbit(dx, dy) with both non-zero: local_right is from the original orientation,
    // composition order is normalize(yaw * pitch * orientation).
    Camera c;
    c.target = { 0.0f, 0.0f, 0.0f };
    c.distance = 5.0f;
    c.orientation = quat::identity();

    const float dx = 0.5f, dy = 0.3f;
    const vec3 local_right = c.orientation.rotate({ 1.0f, 0.0f, 0.0f });
    const quat yaw = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, -dx);
    const quat pitch = quat::from_axis_angle(local_right, dy);
    Camera c_exp;
    c_exp.target = { 0.0f, 0.0f, 0.0f };
    c_exp.distance = 5.0f;
    c_exp.orientation = normalize(yaw * pitch * c.orientation);
    const vec3 exp = c_exp.eye();

    c.orbit(dx, dy);
    const vec3 act = c.eye();
    ASSERT_NEAR(act.x, exp.x, EPS);
    ASSERT_NEAR(act.y, exp.y, EPS);
    ASSERT_NEAR(act.z, exp.z, EPS);
}

// ─── process_key() dt=0 ───────────────────────────────────────────────────────

TEST(camera, eye_zero_distance_returns_target)
{
    Camera c;
    c.target = { 1.0f, 2.0f, 3.0f };
    c.distance = 0.0f;
    c.orientation = quat::identity();
    vec3 e = c.eye();
    ASSERT_NEAR(e.x, c.target.x, EPS);
    ASSERT_NEAR(e.y, c.target.y, EPS);
    ASSERT_NEAR(e.z, c.target.z, EPS);
}

TEST(camera, projection_near_equals_far_produces_nonfinite)
{
    // perspective() denominator is (far - near); near == far → divide by zero.
    // No guard exists; documents that the result contains non-finite values.
    // volatile prevents MSVC from constant-folding (far - near) to 0 at
    // compile time and raising C4723 (potential divide by 0) as an error.
    Camera c;
    c.fov = to_radians(60.0f);
    volatile float same = 1.0f;
    c.near_plane = same;
    c.far_plane = same;
    mat4 P = c.projection(100, 100);
    ASSERT_FALSE(std::isfinite(P.m[2][2]));
}

TEST(camera, process_key_dt_zero_is_noop)
{
    // With dt=0 both orbit and zoom steps compute zero delta → state unchanged.
    Camera c;
    c.distance = 3.0f;
    c.orientation = quat::identity();
    const vec3 e_before = c.eye();
    const float d_before = c.distance;
    for (auto key : { platform::Key::A, platform::Key::D, platform::Key::W, platform::Key::S, platform::Key::Plus,
                      platform::Key::Minus })
    {
        c.process_key(key, 0.0f);
    }
    ASSERT_NEAR(c.distance, d_before, EPS);
    vec3 e_after = c.eye();
    ASSERT_NEAR(e_after.x, e_before.x, EPS);
    ASSERT_NEAR(e_after.y, e_before.y, EPS);
    ASSERT_NEAR(e_after.z, e_before.z, EPS);
}
