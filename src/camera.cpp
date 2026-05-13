#include "camera.h"

#include <cmath>

vec3 Camera::eye() const
{
    return target + orientation.rotate({0.0f, 0.0f, 1.0f}) * distance;
}

mat4 Camera::view(const vec3 &eye_pos) const
{
    return look_at(eye_pos, target, orientation.rotate({0.0f, 1.0f, 0.0f}));
}

mat4 Camera::view() const { return view(eye()); }

mat4 Camera::projection(int pixel_width, int pixel_height) const
{
    const float aspect = (pixel_width > 0 && pixel_height > 0) ? static_cast<float>(pixel_width) / static_cast<float>(pixel_height) : 1.0f;
    return perspective(fov, aspect, near_plane, far_plane);
}

void Camera::orbit(float dx, float dy)
{
    const vec3 local_right = orientation.rotate({1.0f, 0.0f, 0.0f});
    const quat yaw = quat::from_axis_angle({0.0f, 1.0f, 0.0f}, -dx);
    const quat pitch = quat::from_axis_angle(local_right, dy);
    orientation = normalize(yaw * pitch * orientation);
}

void Camera::spin_world_y(float radians)
{
    const quat r = quat::from_axis_angle({0.0f, 1.0f, 0.0f}, radians);
    orientation = normalize(r * orientation);
}

void Camera::process_key(platform::Key key, float dt)
{
    const float orbit_speed = 2.5f;
    const float zoom_speed = distance * 1.5f; // scales with distance so zoom feels consistent

    switch (key)
    {
    case platform::KEY_A:
    case platform::KEY_LEFT:
        orbit(-orbit_speed * dt, 0.0f);
        break;
    case platform::KEY_D:
    case platform::KEY_RIGHT:
        orbit(orbit_speed * dt, 0.0f);
        break;
    case platform::KEY_W:
    case platform::KEY_UP:
        orbit(0.0f, orbit_speed * dt);
        break;
    case platform::KEY_S:
    case platform::KEY_DOWN:
        orbit(0.0f, -orbit_speed * dt);
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

    distance = clamp(distance, near_plane * 2.0f, far_plane * 0.5f);
}
