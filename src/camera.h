#pragma once

#include "linalg.h"
#include "platform.h"

struct Camera
{
    vec3 target = { 0.0f, 0.0f, 0.0f };
    float distance = 3.0f;
    quat orientation = quat::identity(); // camera's current rotation
    float fov = 1.0472f;                 // radians, 60 degrees
    float near_plane = 0.01f;
    float far_plane = 100.0f;

    // World-space position of the camera eye, derived from orbit parameters.
    [[nodiscard]] vec3 eye() const;

    [[nodiscard]] mat4 view() const;
    [[nodiscard]] mat4 view(const vec3 &eye_pos) const;
    [[nodiscard]] mat4 projection(int pixel_width, int pixel_height) const;

    // Update camera state from a single key event.
    // dt: seconds elapsed since last frame, for speed-independent movement.
    void process_key(platform::Key key, float dt);

    void orbit(float dx, float dy); // dx: around local up, dy: around local right
    void spin_world_y(float radians);
};
