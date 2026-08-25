#pragma once

#include "src/math/linalg.h"
#include "src/platform/input.h"

struct Camera
{
    vec3 target = { 0.0f, 0.0f, 0.0f };
    float distance = 3.0f;
    quat orientation = quat::identity(); // camera's current rotation
    float fov = 1.0472f;                 // radians, 60 degrees
    float near_plane = 0.01f;
    float far_plane = 100.0f;

    // First-person rotation pivots on the eye; target becomes a derived look-at point.
    bool first_person = false;
    vec3 fp_centre = { 0.0f, 0.0f, 0.0f }; // model centre; the outer position bound is measured from here
    float fp_base_speed = 1.0f;            // world units/sec at fp_speed == 1; scaled to the model by main.cpp
    float fp_speed = 1.0f;                 // movement-speed multiplier, adjusted by +/- and the wheel

    // Shared by CLI validation and interactive controls.
    static constexpr float FP_SPEED_MIN = 0.05f;
    static constexpr float FP_SPEED_MAX = 20.0f;

    // Key and wheel speed changes derive from the same step and key-latch window.
    static constexpr float FP_SPEED_WHEEL_STEP = 1.08f;
    static constexpr float HELD_KEY_WINDOW = 0.1f; // seconds a key stays latched after its last byte

    // Stop short of the pole to keep rounding from flipping the horizon.
    static constexpr float FP_MAX_PITCH = 1.57079633f - 1e-3f; // rad; the margin keeps rounding off the pole

    // Shared outer bound for orbit zoom and first-person flight.
    [[nodiscard]] float max_eye_distance() const;

    // World-space eye derived from orbit parameters.
    [[nodiscard]] vec3 eye() const;

    // Unit look direction.
    [[nodiscard]] vec3 forward() const;

    // Unit camera up axis; a negative Y component means the turntable is upside down.
    [[nodiscard]] vec3 up() const;

    [[nodiscard]] mat4 view() const;
    [[nodiscard]] mat4 view(const vec3 &eye_pos) const;
    [[nodiscard]] mat4 projection(int pixel_width, int pixel_height) const;

    // Apply one key for `dt` seconds.
    void process_key(platform::Key key, float dt);

    // Rotate using the active mode's pivot and pitch rules.
    void look(float dx, float dy);

    // First-person translation. Forward and right follow the view; world_up is absolute.
    void move(float fwd, float right, float world_up, float dt);

    // Scale the movement-speed multiplier, clamped to [FP_SPEED_MIN, FP_SPEED_MAX].
    void adjust_speed(float factor);

    // Turntable orbit around world Y and local right.
    void orbit(float dx, float dy);
    void spin_world_y(float radians);
};
