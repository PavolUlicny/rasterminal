#include "tests/test.h"
#include "src/render/camera.h"

#include <cmath>

// The orbit eye sits `distance` along its orientation's +Z from `target`.
// Yaw uses world Y with upside-down inversion; pitch uses camera-local right.

static constexpr float EPS = 1e-4f;

// eye()

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

// view() matrix

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

// projection()

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
    // dimension is non-positive: must not produce NaN/inf.
    Camera c;
    mat4 P = c.projection(0, 100);
    ASSERT_TRUE(std::isfinite(P.m[0][0]));
    ASSERT_TRUE(std::isfinite(P.m[1][1]));
}

// process_key() from identity orientation

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

// orbit(): turntable-specific

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
    // World-Y yaw makes yaw-then-pitch equal pitch-then-yaw here. Local-up yaw
    // would not satisfy this turntable identity.
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

TEST(camera, orbit_yaw_follows_drag_when_upside_down)
{
    // At 180° pitch, positive dx still moves the eye toward screen-right, eye.x < 0.
    Camera c;
    c.target = { 0.0f, 0.0f, 0.0f };
    c.distance = 5.0f;
    c.orientation = quat::from_axis_angle({ 1.0f, 0.0f, 0.0f }, to_radians(180.0f));
    c.orbit(0.25f, 0.0f);
    ASSERT_TRUE(c.eye().x < 0.0f);
}

TEST(camera, orbit_pitch_not_flipped_when_upside_down)
{
    // Pitch is around local right, which follows the screen in every
    // orientation: the upside-down inversion must not touch dy. Upside down,
    // a positive dy rotates the -Z eye toward +Y.
    Camera c;
    c.target = { 0.0f, 0.0f, 0.0f };
    c.distance = 5.0f;
    c.orientation = quat::from_axis_angle({ 1.0f, 0.0f, 0.0f }, to_radians(180.0f));
    c.orbit(0.0f, 0.25f);
    ASSERT_TRUE(c.eye().y > 0.0f);
}

TEST(camera, orbit_upside_down_keeps_eye_on_sphere)
{
    // Mixed orbit sequence crossing the pole (through the flip branch): eye
    // must stay at `distance` from target.
    Camera c;
    c.target = { 1.0f, 2.0f, 3.0f };
    c.distance = 6.0f;
    c.orientation = quat::identity();
    c.orbit(0.3f, 1.2f);
    c.orbit(-0.7f, 1.1f); // past the pole, now upside down
    c.orbit(0.5f, -0.2f);
    float r = (c.eye() - c.target).length();
    ASSERT_NEAR(r, 6.0f, 1e-4f);
}

// First-person look uses the orbit rotation around the eye instead of the target.

namespace
{
    Camera fp_camera()
    {
        Camera c;
        c.first_person = true;
        c.target = { 0.0f, 0.0f, -5.0f }; // eye at the origin, looking down -Z
        c.distance = 5.0f;
        c.orientation = quat::identity();
        return c;
    }
} // namespace

TEST(camera, look_in_orbit_mode_is_orbit)
{
    // first_person defaults false, so look() must be indistinguishable from orbit()
    // there, including the upside-down dx inversion, hence the pitched start.
    Camera c1, c2;
    c1.target = c2.target = { 1.0f, 2.0f, 3.0f };
    c1.distance = c2.distance = 6.0f;
    c1.orientation = c2.orientation = quat::from_axis_angle({ 1.0f, 0.0f, 0.0f }, to_radians(180.0f));
    c1.look(0.3f, 0.2f);
    c2.orbit(0.3f, 0.2f);
    const vec3 e1 = c1.eye(), e2 = c2.eye();
    ASSERT_NEAR(e1.x, e2.x, EPS);
    ASSERT_NEAR(e1.y, e2.y, EPS);
    ASSERT_NEAR(e1.z, e2.z, EPS);
    ASSERT_NEAR(c1.target.x, c2.target.x, EPS);
    ASSERT_NEAR(c1.target.z, c2.target.z, EPS);
}

TEST(camera, fp_look_keeps_the_eye_fixed)
{
    // The whole point of the mode: rotation pivots on the eye, so the eye must not
    // move no matter how the view is swung around.
    Camera c = fp_camera();
    const vec3 before = c.eye();
    c.look(0.7f, 0.3f);
    c.look(-1.2f, -0.4f);
    c.look(2.5f, 0.1f);
    const vec3 after = c.eye();
    ASSERT_NEAR(after.x, before.x, EPS);
    ASSERT_NEAR(after.y, before.y, EPS);
    ASSERT_NEAR(after.z, before.z, EPS);
}

TEST(camera, fp_look_target_stays_ahead_of_the_eye)
{
    // `target` is derived in this mode: it must stay `distance` ahead along forward,
    // which is what keeps view() looking where the orientation points.
    Camera c = fp_camera();
    c.look(0.6f, -0.35f);
    const vec3 expected = c.eye() + c.forward() * c.distance;
    ASSERT_NEAR(c.target.x, expected.x, EPS);
    ASSERT_NEAR(c.target.y, expected.y, EPS);
    ASSERT_NEAR(c.target.z, expected.z, EPS);
}

TEST(camera, fp_look_positive_dx_turns_the_view_right)
{
    // From identity, a rightward drag makes forward gain +X.
    Camera c = fp_camera();
    c.look(0.2f, 0.0f);
    ASSERT_TRUE(c.forward().x > 0.0f);
}

TEST(camera, fp_look_negative_dx_turns_the_view_left)
{
    Camera c = fp_camera();
    c.look(-0.2f, 0.0f);
    ASSERT_TRUE(c.forward().x < 0.0f);
}

TEST(camera, fp_look_positive_dy_looks_up)
{
    // main.cpp negates the screen delta before calling, so a downward drag arrives as
    // a negative dy and lowers the view; positive raises it.
    Camera c = fp_camera();
    c.look(0.0f, 0.3f);
    ASSERT_TRUE(c.forward().y > 0.0f);
}

TEST(camera, fp_look_negative_dy_looks_down)
{
    Camera c = fp_camera();
    c.look(0.0f, -0.3f);
    ASSERT_TRUE(c.forward().y < 0.0f);
}

TEST(camera, fp_look_pitch_clamps_looking_up)
{
    // Far past the pole in one step, then again: forward must stop at straight up and
    // stay there rather than looping over and inverting the horizon.
    Camera c = fp_camera();
    c.look(0.0f, 10.0f);
    ASSERT_NEAR(c.forward().y, 1.0f, 1e-4f);
    c.look(0.0f, 10.0f);
    ASSERT_NEAR(c.forward().y, 1.0f, 1e-4f);
}

TEST(camera, fp_look_pitch_clamps_looking_down)
{
    Camera c = fp_camera();
    c.look(0.0f, -10.0f);
    ASSERT_NEAR(c.forward().y, -1.0f, 1e-4f);
    c.look(0.0f, -10.0f);
    ASSERT_NEAR(c.forward().y, -1.0f, 1e-4f);
}

TEST(camera, fp_look_pitch_clamp_never_inverts_the_horizon)
{
    // Detect pole crossing through up.y; forward.y is symmetric around the pole and
    // cannot distinguish 89.99 from 90.01 degrees. Exercise pure and yaw-mixed pitch.
    const float patterns[4][2] = { { 0.0f, 0.05f }, { 0.05f, 0.05f }, { 0.2f, 0.05f }, { 0.05f, 10.0f } };
    for (const auto &p : patterns)
    {
        Camera c = fp_camera();
        for (int i = 0; i < 2000; i++)
        {
            c.look(p[0], p[1]);
            ASSERT_TRUE(c.orientation.rotate({ 0.0f, 1.0f, 0.0f }).y > 0.0f);
        }
    }
}

TEST(camera, fp_launch_pitch_limit_is_a_pose_look_can_hold)
{
    // At FP_MAX_PITCH, pure yaw must remain pure yaw. A round 90-degree launch clamp
    // starts beyond look()'s limit and forces the first input away from the pole.
    Camera c = fp_camera();
    c.orientation = quat::from_axis_angle({ 1.0f, 0.0f, 0.0f }, Camera::FP_MAX_PITCH);
    c.target = c.eye() + c.forward() * c.distance;
    const float elevation_before = c.forward().y;
    c.look(0.3f, 0.0f); // pure yaw
    ASSERT_NEAR(c.forward().y, elevation_before, 1e-5f);
}

TEST(camera, fp_look_pitch_clamp_recovers_from_an_inverted_start)
{
    // Defence in depth for the same evenness: handed an already-inverted orientation,
    // the clamp must pull back toward the pole rather than push further past it.
    Camera c = fp_camera();
    c.orientation = quat::from_axis_angle({ 1.0f, 0.0f, 0.0f }, to_radians(100.0f));
    c.target = c.eye() + c.forward() * c.distance;
    ASSERT_TRUE(c.orientation.rotate({ 0.0f, 1.0f, 0.0f }).y < 0.0f); // starts inverted
    for (int i = 0; i < 50; i++)
    {
        c.look(0.0f, 0.05f); // keep asking to look further up
    }
    ASSERT_TRUE(c.orientation.rotate({ 0.0f, 1.0f, 0.0f }).y > 0.0f); // pulled back upright
}

TEST(camera, fp_look_pitch_clamp_is_reversible)
{
    // Pinned at the top, a downward step must still come back: the clamp bounds the
    // delta, it does not latch the orientation.
    Camera c = fp_camera();
    c.look(0.0f, 10.0f);
    c.look(0.0f, -0.5f);
    const float y = c.forward().y;
    ASSERT_TRUE(y < 1.0f);
    ASSERT_TRUE(y > 0.0f);
}

TEST(camera, fp_look_view_is_finite_at_the_pitch_limit)
{
    // Looking straight up, forward is world +Y. look_at's up vector comes from the
    // orientation quat rather than world Y, so it stays perpendicular and the cross
    // product does not degenerate, so no epsilon backs the clamp off the pole.
    Camera c = fp_camera();
    c.look(0.0f, 10.0f);
    const mat4 v = c.view();
    for (const auto &row : v.m)
    {
        for (float val : row)
        {
            ASSERT_TRUE(std::isfinite(val));
        }
    }
}

TEST(camera, fp_look_yaw_still_works_at_the_pitch_limit)
{
    // Pinned straight up, yaw must still rotate the frame (it is about world Y, which
    // the clamp never touches).
    Camera c = fp_camera();
    c.look(0.0f, 10.0f);
    const vec3 right_before = c.orientation.rotate({ 1.0f, 0.0f, 0.0f });
    c.look(0.8f, 0.0f);
    const vec3 right_after = c.orientation.rotate({ 1.0f, 0.0f, 0.0f });
    const float moved = (right_after - right_before).length();
    ASSERT_TRUE(moved > 1e-3f);
}

TEST(camera, fp_look_never_rolls)
{
    // A fly camera must keep the horizon level: the right axis stays horizontal for
    // any sequence of looks, which is also what makes the pitch clamp exact.
    Camera c = fp_camera();
    c.look(0.9f, 0.4f);
    c.look(-1.7f, -0.9f);
    c.look(0.3f, 12.0f);
    c.look(2.2f, -0.6f);
    const vec3 right = c.orientation.rotate({ 1.0f, 0.0f, 0.0f });
    ASSERT_NEAR(right.y, 0.0f, 1e-4f);
}

TEST(camera, fp_look_zero_is_noop)
{
    Camera c = fp_camera();
    const vec3 e_before = c.eye();
    const vec3 f_before = c.forward();
    c.look(0.0f, 0.0f);
    ASSERT_NEAR(c.eye().x, e_before.x, EPS);
    ASSERT_NEAR(c.eye().y, e_before.y, EPS);
    ASSERT_NEAR(c.eye().z, e_before.z, EPS);
    ASSERT_NEAR(c.forward().y, f_before.y, EPS);
}

// move() / adjust_speed(): first-person

TEST(camera, fp_move_forward_follows_the_look_direction)
{
    // Identity orientation looks down -Z, so forward movement must go there.
    Camera c = fp_camera();
    c.fp_base_speed = 2.0f;
    c.move(1.0f, 0.0f, 0.0f, 0.5f);
    const vec3 e = c.eye();
    ASSERT_NEAR(e.x, 0.0f, EPS);
    ASSERT_NEAR(e.y, 0.0f, EPS);
    ASSERT_NEAR(e.z, -1.0f, EPS); // 2.0 * 1.0 * 0.5
}

TEST(camera, fp_move_forward_includes_pitch)
{
    // The fly convention: look down, move forward, descend. This is what makes E/V
    // meaningful as a separate absolute-vertical control.
    Camera c = fp_camera();
    c.look(0.0f, -0.6f); // pitch down
    c.move(1.0f, 0.0f, 0.0f, 1.0f);
    ASSERT_TRUE(c.eye().y < 0.0f);
}

TEST(camera, fp_move_back_is_the_negation_of_forward)
{
    Camera c = fp_camera();
    c.look(0.4f, -0.3f);
    const vec3 start = c.eye();
    c.move(1.0f, 0.0f, 0.0f, 0.7f);
    c.move(-1.0f, 0.0f, 0.0f, 0.7f);
    const vec3 e = c.eye();
    ASSERT_NEAR(e.x, start.x, EPS);
    ASSERT_NEAR(e.y, start.y, EPS);
    ASSERT_NEAR(e.z, start.z, EPS);
}

TEST(camera, fp_move_right_strafes_along_view_right)
{
    Camera c = fp_camera();
    c.move(0.0f, 1.0f, 0.0f, 1.0f);
    ASSERT_TRUE(c.eye().x > 0.0f);
    ASSERT_NEAR(c.eye().y, 0.0f, EPS);
}

TEST(camera, fp_move_left_strafes_the_other_way)
{
    Camera c = fp_camera();
    c.move(0.0f, -1.0f, 0.0f, 1.0f);
    ASSERT_TRUE(c.eye().x < 0.0f);
}

TEST(camera, fp_move_up_is_world_absolute_while_pitched)
{
    // E/V follow world Y, not the view basis: pitched steeply, rising must still be
    // straight up and must not drift horizontally.
    Camera c = fp_camera();
    c.look(0.0f, -1.2f); // steep pitch down
    const vec3 start = c.eye();
    c.move(0.0f, 0.0f, 1.0f, 1.0f);
    const vec3 e = c.eye();
    ASSERT_NEAR(e.x, start.x, EPS);
    ASSERT_NEAR(e.z, start.z, EPS);
    ASSERT_NEAR(e.y, start.y + 1.0f, EPS);
}

TEST(camera, fp_move_down_is_world_absolute_while_pitched)
{
    Camera c = fp_camera();
    c.look(0.0f, 1.1f); // pitched up
    const vec3 start = c.eye();
    c.move(0.0f, 0.0f, -1.0f, 1.0f);
    const vec3 e = c.eye();
    ASSERT_NEAR(e.x, start.x, EPS);
    ASSERT_NEAR(e.z, start.z, EPS);
    ASSERT_NEAR(e.y, start.y - 1.0f, EPS);
}

TEST(camera, fp_move_scales_with_base_speed_and_multiplier)
{
    // Distance travelled is fp_base_speed * fp_speed * dt: the multiplier is what
    // --first-person-speed and the +/- keys drive, so it must scale linearly.
    Camera c1 = fp_camera(), c2 = fp_camera();
    c1.fp_base_speed = c2.fp_base_speed = 3.0f;
    c1.fp_speed = 1.0f;
    c2.fp_speed = 2.0f;
    c1.move(1.0f, 0.0f, 0.0f, 0.25f);
    c2.move(1.0f, 0.0f, 0.0f, 0.25f);
    ASSERT_NEAR(c1.eye().z, -0.75f, EPS);
    ASSERT_NEAR(c2.eye().z, -1.5f, EPS);
}

TEST(camera, fp_move_dt_zero_is_noop)
{
    Camera c = fp_camera();
    const vec3 start = c.eye();
    c.move(1.0f, 1.0f, 1.0f, 0.0f);
    ASSERT_NEAR(c.eye().x, start.x, EPS);
    ASSERT_NEAR(c.eye().y, start.y, EPS);
    ASSERT_NEAR(c.eye().z, start.z, EPS);
}

TEST(camera, fp_move_is_a_noop_in_orbit_mode)
{
    Camera c;
    c.target = { 0.0f, 0.0f, 0.0f };
    c.distance = 5.0f;
    const vec3 start = c.eye();
    c.move(1.0f, 1.0f, 1.0f, 1.0f);
    ASSERT_NEAR(c.eye().x, start.x, EPS);
    ASSERT_NEAR(c.eye().y, start.y, EPS);
    ASSERT_NEAR(c.eye().z, start.z, EPS);
}

TEST(camera, fp_move_clamps_at_the_far_limit)
{
    // Same outer bound as orbit's zoom-out stop, measured from the model centre.
    Camera c = fp_camera();
    c.fp_centre = { 0.0f, 0.0f, 0.0f };
    c.near_plane = 0.01f;
    c.far_plane = 100.0f;
    c.fp_base_speed = 1000.0f;
    c.move(1.0f, 0.0f, 0.0f, 10.0f);
    ASSERT_NEAR((c.eye() - c.fp_centre).length(), c.max_eye_distance(), 1e-3f);
}

TEST(camera, fp_move_clamp_does_not_change_the_look_direction)
{
    // Hitting the wall stops the position and nothing else: the view must not swing.
    Camera c = fp_camera();
    c.fp_base_speed = 1000.0f;
    c.look(0.5f, -0.2f);
    const vec3 fwd_before = c.forward();
    c.move(1.0f, 0.0f, 0.0f, 10.0f);
    const vec3 fwd_after = c.forward();
    ASSERT_NEAR(fwd_after.x, fwd_before.x, EPS);
    ASSERT_NEAR(fwd_after.y, fwd_before.y, EPS);
    ASSERT_NEAR(fwd_after.z, fwd_before.z, EPS);
}

TEST(camera, fp_move_target_stays_derived_after_a_clamp)
{
    // The clamp rewrites `target`, which is the stored field, so the eye-ahead
    // invariant has to survive it or view() would look somewhere else.
    Camera c = fp_camera();
    c.fp_base_speed = 1000.0f;
    c.move(1.0f, 0.0f, 0.0f, 10.0f);
    const vec3 expected = c.eye() + c.forward() * c.distance;
    ASSERT_NEAR(c.target.x, expected.x, 1e-3f);
    ASSERT_NEAR(c.target.y, expected.y, 1e-3f);
    ASSERT_NEAR(c.target.z, expected.z, 1e-3f);
}

TEST(camera, fp_move_has_no_inner_limit)
{
    // Orbit's near clamp keeps the eye off the target; first-person must be able to
    // fly right through the model centre, which is the whole point of the mode.
    Camera c = fp_camera();
    c.fp_centre = { 0.0f, 0.0f, -10.0f }; // 10 ahead of the eye
    c.fp_base_speed = 10.0f;
    c.move(1.0f, 0.0f, 0.0f, 1.5f); // 15 forward: past the centre
    ASSERT_TRUE(c.eye().z < -10.0f);
}

TEST(camera, adjust_speed_multiplies)
{
    Camera c = fp_camera();
    c.fp_speed = 1.0f;
    c.adjust_speed(2.0f);
    ASSERT_NEAR(c.fp_speed, 2.0f, EPS);
    c.adjust_speed(0.5f);
    ASSERT_NEAR(c.fp_speed, 1.0f, EPS);
}

TEST(camera, adjust_speed_clamps_at_both_ends)
{
    Camera c = fp_camera();
    c.fp_speed = 1.0f;
    c.adjust_speed(1e6f);
    ASSERT_NEAR(c.fp_speed, Camera::FP_SPEED_MAX, EPS);
    c.adjust_speed(1e-9f);
    ASSERT_NEAR(c.fp_speed, Camera::FP_SPEED_MIN, EPS);
}

// process_key(): first-person routing

TEST(camera, fp_process_key_WASD_moves_without_rotating)
{
    Camera c = fp_camera();
    c.fp_base_speed = 4.0f;
    const vec3 fwd_before = c.forward();
    c.process_key(platform::Key::W, 0.1f);
    ASSERT_TRUE(c.eye().z < -0.01f); // advanced along -Z
    const vec3 fwd_after = c.forward();
    ASSERT_NEAR(fwd_after.z, fwd_before.z, EPS);
}

TEST(camera, fp_process_key_A_and_D_strafe)
{
    Camera c1 = fp_camera(), c2 = fp_camera();
    c1.fp_base_speed = c2.fp_base_speed = 4.0f;
    c1.process_key(platform::Key::A, 0.1f);
    c2.process_key(platform::Key::D, 0.1f);
    ASSERT_TRUE(c1.eye().x < 0.0f);
    ASSERT_TRUE(c2.eye().x > 0.0f);
}

TEST(camera, fp_process_key_E_and_V_move_vertically)
{
    Camera c1 = fp_camera(), c2 = fp_camera();
    c1.fp_base_speed = c2.fp_base_speed = 4.0f;
    c1.process_key(platform::Key::E, 0.1f);
    c2.process_key(platform::Key::V, 0.1f);
    ASSERT_TRUE(c1.eye().y > 0.0f);
    ASSERT_TRUE(c2.eye().y < 0.0f);
}

TEST(camera, fp_process_key_arrows_look_without_moving)
{
    // The arrows are the keyboard look here, so the eye must stay put.
    Camera c = fp_camera();
    const vec3 e_before = c.eye();
    c.process_key(platform::Key::Left, 0.1f);
    ASSERT_NEAR(c.eye().x, e_before.x, EPS);
    ASSERT_NEAR(c.eye().z, e_before.z, EPS);
    ASSERT_TRUE(c.forward().x < 0.0f); // Left turns the view left
}

TEST(camera, fp_process_key_arrow_directions)
{
    Camera right = fp_camera(), up = fp_camera(), down = fp_camera();
    right.process_key(platform::Key::Right, 0.1f);
    up.process_key(platform::Key::Up, 0.1f);
    down.process_key(platform::Key::Down, 0.1f);
    ASSERT_TRUE(right.forward().x > 0.0f);
    ASSERT_TRUE(up.forward().y > 0.0f);
    ASSERT_TRUE(down.forward().y < 0.0f);
}

TEST(camera, fp_process_key_arrow_look_rate_is_one_fov_per_second)
{
    // Looking is anchored to the field of view: a second of a held arrow sweeps one
    // frame's worth of content past. Deliberately slower than orbit's 2.5 rad/s, since
    // looking moves the whole scene rather than turning a model that stays centred.
    Camera c = fp_camera();
    const vec3 before = c.forward();
    c.process_key(platform::Key::Right, 1.0f);
    const float swept = std::acos(clamp(dot(before, c.forward()), -1.0f, 1.0f));
    ASSERT_NEAR(swept, c.fov, 1e-3f);
}

TEST(camera, fp_process_key_look_is_slower_than_orbit)
{
    // The two rates are set independently; this pins that the first-person one stays
    // the slower of the pair, which is the whole point of separating them.
    Camera fp = fp_camera();
    Camera orb;
    orb.distance = 5.0f;
    orb.orientation = quat::identity();
    const vec3 fp_before = fp.forward();
    const vec3 orb_before = orb.orientation.rotate({ 0.0f, 0.0f, -1.0f });
    fp.process_key(platform::Key::Right, 0.5f);
    orb.process_key(platform::Key::D, 0.5f);
    const float fp_swept = std::acos(clamp(dot(fp_before, fp.forward()), -1.0f, 1.0f));
    const float orb_swept =
        std::acos(clamp(dot(orb_before, orb.orientation.rotate({ 0.0f, 0.0f, -1.0f })), -1.0f, 1.0f));
    ASSERT_TRUE(fp_swept < orb_swept);
}

TEST(camera, fp_process_key_plus_and_minus_retune_speed)
{
    Camera up = fp_camera(), down = fp_camera();
    up.process_key(platform::Key::Plus, 0.1f);
    down.process_key(platform::Key::Minus, 0.1f);
    ASSERT_TRUE(up.fp_speed > 1.0f);
    ASSERT_TRUE(down.fp_speed < 1.0f);
}

TEST(camera, fp_speed_key_factor_integrates_to_one_wheel_notch)
{
    // Deltas summing to the latch window multiply to exactly one wheel notch.
    // The end-to-end tap below allows one frame of timing error.
    for (int frames : { 1, 6, 60 }) // one slow frame, 60 fps, 600 fps: same total
    {
        Camera c = fp_camera();
        const float dt = Camera::HELD_KEY_WINDOW / static_cast<float>(frames);
        for (int i = 0; i < frames; i++)
        {
            c.process_key(platform::Key::Plus, dt);
        }
        ASSERT_NEAR(c.fp_speed, Camera::FP_SPEED_WHEEL_STEP, 1e-4f);
    }
}

TEST(camera, fp_key_tap_lands_within_a_frame_of_one_wheel_notch)
{
    // Replay main.cpp's latch using real time since the key byte. The first applied
    // dt began before that byte, making the one-frame error two-sided; uneven pacing
    // is required to expose undershoot.
    struct Pacing
    {
        float press_frame, rest;
    };
    const Pacing pacings[] = {
        { 1.0f / 60.0f, 1.0f / 60.0f }, // even, 60 fps
        { 0.05f, 0.05f },               // even, 20 fps
        { 0.005f, 0.030f },             // short frame at the press, then longer
        { 0.001f, 0.049f },             // the same, more extreme
        { 0.050f, 0.005f },             // long frame at the press, then shorter
    };
    for (const Pacing &p : pacings)
    {
        const float widest = (p.press_frame > p.rest) ? p.press_frame : p.rest;
        const float slack = std::pow(Camera::FP_SPEED_WHEEL_STEP, widest / Camera::HELD_KEY_WINDOW);
        for (int i = 0; i <= 16; i++)
        {
            Camera c = fp_camera();
            float since = p.press_frame * (static_cast<float>(i) / 16.0f); // where the byte fell
            bool first = true;
            while (since <= Camera::HELD_KEY_WINDOW)
            {
                c.process_key(platform::Key::Plus, first ? p.press_frame : p.rest);
                first = false;
                since += p.rest;
            }
            ASSERT_TRUE(c.fp_speed >= (Camera::FP_SPEED_WHEEL_STEP / slack) - 1e-4f);
            ASSERT_TRUE(c.fp_speed <= (Camera::FP_SPEED_WHEEL_STEP * slack) + 1e-4f);
        }
    }
}

TEST(camera, fp_key_speed_steps_are_exact_inverses)
{
    // A tap up and a tap down must return to where it started, as the wheel's exact
    // reciprocal does; the HUD shows this number, so any drift is visible.
    Camera c = fp_camera();
    c.process_key(platform::Key::Plus, Camera::HELD_KEY_WINDOW);
    c.process_key(platform::Key::Minus, Camera::HELD_KEY_WINDOW);
    ASSERT_NEAR(c.fp_speed, 1.0f, 1e-5f);
}

TEST(camera, fp_process_key_leaves_distance_alone)
{
    // `distance` is only the lever arm deriving `target` in this mode; the zoom clamp
    // that orbit applies at the end of process_key must not run.
    Camera c = fp_camera();
    c.distance = 5.0f;
    c.near_plane = 0.01f;
    c.far_plane = 100.0f;
    for (auto key : { platform::Key::Plus, platform::Key::Minus, platform::Key::W, platform::Key::Up })
    {
        c.process_key(key, 0.5f);
    }
    ASSERT_NEAR(c.distance, 5.0f, EPS);
}

// spin_world_y()

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

TEST(camera, fp_spin_keeps_the_eye_fixed)
{
    // Spin takes the same pivot as every other rotation in this mode, so it pans the
    // view in place. Pivoting on `target` instead would fly the eye around a circle of
    // radius `distance` (measured: a quarter turn moved it 7.07 units at distance 5).
    Camera c = fp_camera();
    const vec3 before = c.eye();
    c.spin_world_y(to_radians(90.0f));
    c.spin_world_y(to_radians(137.0f));
    const vec3 after = c.eye();
    ASSERT_NEAR(after.x, before.x, EPS);
    ASSERT_NEAR(after.y, before.y, EPS);
    ASSERT_NEAR(after.z, before.z, EPS);
}

TEST(camera, fp_spin_rotates_the_view)
{
    // Fixing the eye must not make spin a no-op: the look direction still sweeps.
    Camera c = fp_camera();
    const vec3 before = c.forward();
    c.spin_world_y(to_radians(90.0f));
    const vec3 after = c.forward();
    ASSERT_TRUE((after - before).length() > 1e-3f);
    ASSERT_NEAR(after.y, before.y, EPS); // world-Y spin does not change elevation
}

TEST(camera, fp_spin_left_turns_the_view_left)
{
    // In first person, the positive angle used for `left` turns the view left.
    Camera c = fp_camera();
    c.spin_world_y(0.2f);
    ASSERT_TRUE(c.forward().x < 0.0f);
}

TEST(camera, fp_spin_keeps_target_derived)
{
    // `target` is the stored field, so spin has to leave it the eye-ahead point or
    // view() would look somewhere else on the next frame.
    Camera c = fp_camera();
    c.look(0.4f, -0.25f);
    c.spin_world_y(to_radians(70.0f));
    const vec3 expected = c.eye() + c.forward() * c.distance;
    ASSERT_NEAR(c.target.x, expected.x, EPS);
    ASSERT_NEAR(c.target.y, expected.y, EPS);
    ASSERT_NEAR(c.target.z, expected.z, EPS);
}

TEST(camera, fp_spin_cannot_escape_the_far_bound)
{
    // move() is the only place the outer bound is enforced, so a rotation that moved
    // the eye could carry it outside with nothing to pull it back.
    Camera c = fp_camera();
    c.fp_centre = { 0.0f, 0.0f, 0.0f };
    c.near_plane = 0.01f;
    c.far_plane = 20.0f; // max_dist = 10
    c.fp_base_speed = 100.0f;
    c.move(1.0f, 0.0f, 0.0f, 1.0f); // pin against the bound
    for (int i = 0; i < 16; i++)
    {
        c.spin_world_y(to_radians(45.0f));
    }
    ASSERT_TRUE((c.eye() - c.fp_centre).length() <= c.max_eye_distance() + 1e-3f);
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

// projection() additional degenerate inputs

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

// view(eye_pos) overload

TEST(camera, view_explicit_eye_pos_maps_to_origin)
{
    // The two-arg overload must use the supplied position, not recompute eye().
    // We pass a position offset from the normal eye(); it must still map to origin.
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

// orbit() zero step

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

// process_key() dt=0

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
    // Equal planes divide by zero and intentionally produce non-finite output.
    // volatile prevents MSVC from diagnosing a constant division during compilation.
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
