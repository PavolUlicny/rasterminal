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
    // Flip the up vector when the camera passes over the top or bottom pole
    // so full vertical rotation works without gimbal lock.
    vec3 up = (std::cos(pitch) >= 0.0f) ? vec3{0.0f, 1.0f, 0.0f}
                                        : vec3{0.0f, -1.0f, 0.0f};
    return look_at(eye(), target, up);
}

mat4 Camera::projection(int pixel_width, int pixel_height) const
{
    float aspect = (pixel_width > 0 && pixel_height > 0) ? (float)pixel_width / (float)pixel_height : 1.0f;
    return perspective(fov, aspect, near_plane, far_plane);
}

void Camera::process_key(platform::Key key, float dt)
{
    const float orbit_speed = 2.5f;           // radians/sec
    const float zoom_speed = distance * 1.5f; // scales with distance so zoom feels consistent

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

    distance = clamp(distance, 0.1f, 500.0f);
}
