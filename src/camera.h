#pragma once

#include "input.h"
#include "linalg.h"

struct Camera
{
    vec3 target = { 0.0f, 0.0f, 0.0f };
    float distance = 3.0f;
    quat orientation = quat::identity(); // camera's current rotation
    float fov = 1.0472f;                 // radians, 60 degrees
    float near_plane = 0.01f;
    float far_plane = 100.0f;

    // First-person mode: view rotation pivots on the eye instead of on `target`, so
    // `target` becomes a derived look-at point rather than the model centre.
    bool first_person = false;
    vec3 fp_centre = { 0.0f, 0.0f, 0.0f }; // model centre; the outer position bound is measured from here
    float fp_base_speed = 1.0f;            // world units/sec at fp_speed == 1; scaled to the model by main.cpp
    float fp_speed = 1.0f;                 // movement-speed multiplier, adjusted by +/- and the wheel

    // Interactive range of fp_speed. Public because --first-person-speed parses
    // against exactly this range, the way --zoom's bounds equal the scroll clamp.
    static constexpr float FP_SPEED_MIN = 0.05f;
    static constexpr float FP_SPEED_MAX = 20.0f;

    // One wheel notch of movement speed, and how long main.cpp holds a tapped key.
    // Both live here so process_key() can derive its per-frame factor from them rather
    // than restate a number hand-computed from the pair, which is what keeps the key
    // and the wheel from drifting apart. A tap lands WITHIN A FRAME of one notch, in
    // EITHER direction: see the derivation at speed_key_factor() in camera.cpp.
    // main.cpp consumes both, and caps its frame dt at the window to bound the error.
    static constexpr float FP_SPEED_WHEEL_STEP = 1.08f;
    static constexpr float HELD_KEY_WINDOW = 0.1f; // seconds a key stays latched after its last byte

    // Pitch limit in first-person, a hair under straight up/down. look() clamps every
    // rotation to it, and main.cpp clamps the --pitch launch pose to the same value:
    // starting outside it would put the camera in a pose the mode forbids, and the
    // first look of any direction (a pure yaw included) would be forced to pitch back.
    static constexpr float FP_MAX_PITCH = 1.57079633f - 1e-3f; // rad; the margin keeps rounding off the pole

    // How far the eye may get from what it is looking at: orbit's zoom-out stop, and
    // the outer bound first-person flight is held to, so both modes reach the same
    // positions radially. One definition, since three sites need the same number.
    [[nodiscard]] float max_eye_distance() const;

    // World-space position of the camera eye, derived from orbit parameters.
    [[nodiscard]] vec3 eye() const;

    // Unit direction the camera looks along. eye()'s sibling: both fall out of
    // `orientation`, and first-person maintains target == eye() + forward() * distance.
    [[nodiscard]] vec3 forward() const;

    // Unit up axis of the camera frame. Its Y component is the "upside down" test both
    // modes need, so there is one definition of which way is up rather than a closed
    // form in one place and a rotate() in another.
    [[nodiscard]] vec3 up() const;

    [[nodiscard]] mat4 view() const;
    [[nodiscard]] mat4 view(const vec3 &eye_pos) const;
    [[nodiscard]] mat4 projection(int pixel_width, int pixel_height) const;

    // Update camera state from a single key event.
    // dt: seconds elapsed since last frame, for speed-independent movement.
    void process_key(platform::Key key, float dt);

    // Single entry point for view rotation, in either mode: orbit() when the camera
    // is a turntable, first-person look (pivoting on the eye, pitch clamped to
    // straight up/down) otherwise. Argument convention is the same in both.
    void look(float dx, float dy);

    // First-person translation, one unit per argument at the current speed: forward
    // and right follow the look direction, world_up is absolute (which is what makes
    // E/V useful while pitched). No-op in orbit mode.
    void move(float fwd, float right, float world_up, float dt);

    // Scale the movement-speed multiplier, clamped to [FP_SPEED_MIN, FP_SPEED_MAX].
    void adjust_speed(float factor);

    // Turntable orbit: dx yaws around world Y (inverted while upside down so the
    // drag direction is preserved), dy pitches around local right.
    void orbit(float dx, float dy);
    void spin_world_y(float radians);
};
