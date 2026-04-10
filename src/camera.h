#pragma once

#include "linalg.h"
#include "platform.h"

struct Camera
{
    vec3 target = {0.0f, 0.0f, 0.0f};
    float distance = 3.0f;
    float yaw = 0.0f;    // radians, horizontal rotation around Y
    float pitch = 0.3f;  // radians, elevation above XZ plane
    float fov = 1.0472f; // radians, 60 degrees

    // World-space position of the camera eye, derived from orbit parameters.
    vec3 eye() const;

    mat4 view() const;
    mat4 projection(int pixel_width, int pixel_height) const;

    // Update camera state from a single key event.
    // dt: seconds elapsed since last frame, for speed-independent movement.
    void process_key(platform::Key key, float dt);
};
