#include "camera.h"

#include <cmath>

vec3 Camera::eye() const
{
    // Spherical → Cartesian, relative to target.
    //   x = cos(pitch) * sin(yaw)
    //   y = sin(pitch)
    //   z = cos(pitch) * cos(yaw)
    // At yaw=0, pitch=0 the eye sits at target + (0, 0, distance).
    float cp = std::cos(pitch);
    return target + vec3{cp * std::sin(yaw),
                         std::sin(pitch),
                         cp * std::cos(yaw)} *
                        distance;
}

mat4 Camera::view() const
{
    return look_at(eye(), target, {0.0f, 1.0f, 0.0f});
}

mat4 Camera::projection(int pixel_width, int pixel_height) const
{
    float aspect = (float)pixel_width / (float)pixel_height;
    return perspective(fov, aspect, 0.1f, 100.0f);
}

void Camera::process_key(platform::Key key, float dt)
{
    const float orbit_speed = 2.5f; // radians/sec
    const float zoom_speed = 5.0f;  // units/sec

    switch (key)
    {
    case platform::KEY_A:
    case platform::KEY_LEFT:
        yaw -= orbit_speed * dt;
        break;
    case platform::KEY_D:
    case platform::KEY_RIGHT:
        yaw += orbit_speed * dt;
        break;
    case platform::KEY_W:
    case platform::KEY_UP:
        pitch += orbit_speed * dt;
        break;
    case platform::KEY_S:
    case platform::KEY_DOWN:
        pitch -= orbit_speed * dt;
        break;
    case platform::KEY_PLUS:
        distance -= zoom_speed * dt;
        break;
    case platform::KEY_MINUS:
        distance += zoom_speed * dt;
        break;
    default:
        break;
    }

    // Clamp pitch to ~±89° to prevent gimbal lock when the eye is directly
    // above or below the target (where up={0,1,0} becomes parallel to forward).
    pitch = clamp(pitch, -1.553f, 1.553f);
    distance = clamp(distance, 0.1f, 500.0f);
}
