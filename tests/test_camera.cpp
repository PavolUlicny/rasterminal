#include "test.h"
#include "../src/camera.h"

// Camera is an orbit camera: eye sits on a sphere around `target`, parameter-
// ised by (yaw, pitch, distance). yaw rotates horizontally around +Y, pitch
// elevates above/below the XZ plane. At yaw=0, pitch=0 the eye is at
// target + (0, 0, distance) (on the +Z side looking toward -Z).

static constexpr float EPS = 1e-5f;

// ─── eye() orbit ─────────────────────────────────────────────────────────────

TEST(camera, eye_default_orientation)
{
    Camera c;
    c.target = {0, 0, 0};
    c.distance = 5.0f;
    c.yaw = 0;
    c.pitch = 0;
    vec3 e = c.eye();
    ASSERT_NEAR(e.x, 0.0f, EPS);
    ASSERT_NEAR(e.y, 0.0f, EPS);
    ASSERT_NEAR(e.z, 5.0f, EPS);
}

TEST(camera, eye_yaw_90_puts_eye_on_plus_x)
{
    Camera c;
    c.target = {0, 0, 0};
    c.distance = 5.0f;
    c.yaw = to_radians(90.0f);
    c.pitch = 0;
    vec3 e = c.eye();
    ASSERT_NEAR(e.x, 5.0f, EPS);
    ASSERT_NEAR(e.y, 0.0f, EPS);
    ASSERT_NEAR(e.z, 0.0f, EPS);
}

TEST(camera, eye_pitch_90_puts_eye_directly_above)
{
    Camera c;
    c.target = {0, 0, 0};
    c.distance = 5.0f;
    c.yaw = 0;
    c.pitch = to_radians(90.0f);
    vec3 e = c.eye();
    ASSERT_NEAR(e.x, 0.0f, EPS);
    ASSERT_NEAR(e.y, 5.0f, EPS);
    ASSERT_NEAR(e.z, 0.0f, EPS);
}

TEST(camera, eye_relative_to_target_offset)
{
    Camera c;
    c.target = {10, 20, 30};
    c.distance = 4.0f;
    c.yaw = 0;
    c.pitch = 0;
    vec3 e = c.eye();
    ASSERT_NEAR(e.x, 10.0f, EPS);
    ASSERT_NEAR(e.y, 20.0f, EPS);
    ASSERT_NEAR(e.z, 34.0f, EPS);
}

TEST(camera, eye_on_unit_sphere_scaled_by_distance)
{
    // For any yaw/pitch, |eye - target| must equal distance.
    Camera c;
    c.target = {0, 0, 0};
    c.distance = 7.5f;
    c.yaw = to_radians(37.0f);
    c.pitch = to_radians(21.0f);
    vec3 e = c.eye();
    float r = (e - c.target).length();
    ASSERT_NEAR(r, 7.5f, 1e-5f);
}

// ─── view() matrix ───────────────────────────────────────────────────────────

TEST(camera, view_transforms_eye_to_origin)
{
    Camera c;
    c.target = {1, 2, 3};
    c.distance = 5.0f;
    c.yaw = to_radians(40.0f);
    c.pitch = to_radians(15.0f);

    mat4 V = c.view();
    vec3 e = c.eye();
    vec4 r = V * vec4{e.x, e.y, e.z, 1.0f};
    ASSERT_NEAR(r.x, 0.0f, 1e-4f);
    ASSERT_NEAR(r.y, 0.0f, 1e-4f);
    ASSERT_NEAR(r.z, 0.0f, 1e-4f);
}

TEST(camera, view_puts_target_on_negative_z_at_distance)
{
    // look_at makes forward (eye → target) the -Z axis in view space,
    // so the target in view space is (0, 0, -distance).
    Camera c;
    c.target = {-3, 4, 2};
    c.distance = 6.0f;
    c.yaw = to_radians(20.0f);
    c.pitch = to_radians(-10.0f);

    mat4 V = c.view();
    vec4 r = V * vec4{c.target.x, c.target.y, c.target.z, 1.0f};
    ASSERT_NEAR(r.x, 0.0f, 1e-4f);
    ASSERT_NEAR(r.y, 0.0f, 1e-4f);
    ASSERT_NEAR(r.z, -6.0f, 1e-4f);
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
    vec4 clip = P * vec4{0, 0, -1, 1};
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

    vec4 csq = P_sq * vec4{0.5f, 0, -2, 1};
    vec4 cwide = P_wide * vec4{0.5f, 0, -2, 1};
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
    // m[0][0] = 1/(aspect*tan_half) should be finite.
    ASSERT_TRUE(std::isfinite(P.m[0][0]));
    ASSERT_TRUE(std::isfinite(P.m[1][1]));
}
