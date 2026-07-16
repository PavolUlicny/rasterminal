#include "camera.h"
#include "linalg.h"
#include "platform.h"

#include <cmath>

vec3 Camera::eye() const
{
    return target + orientation.rotate({ 0.0f, 0.0f, 1.0f }) * distance;
}

mat4 Camera::view(const vec3 &eye_pos) const
{
    return look_at(eye_pos, target, orientation.rotate({ 0.0f, 1.0f, 0.0f }));
}

mat4 Camera::view() const
{
    return view(eye());
}

mat4 Camera::projection(int pixel_width, int pixel_height) const
{
    const float aspect = (pixel_width > 0 && pixel_height > 0)
                             ? static_cast<float>(pixel_width) / static_cast<float>(pixel_height)
                             : 1.0f;
    return perspective(fov, aspect, near_plane, far_plane);
}

void Camera::orbit(float dx, float dy)
{
    // Turntable yaw is about world Y; when the camera is upside down (up vector's
    // world-Y component negative) that rotation sweeps the screen opposite to the
    // drag, so invert dx to keep the model following the mouse (Blender behaviour).
    // For a unit quat, up_y = R[1][1] = 1 - 2(x^2 + z^2); orientation is always
    // unit (normalized after every orbit/spin).
    const float up_y = 1.0f - (2.0f * ((orientation.x * orientation.x) + (orientation.z * orientation.z)));
    if (up_y < 0.0f)
    {
        dx = -dx;
    }

    const vec3 local_right = orientation.rotate({ 1.0f, 0.0f, 0.0f });
    const quat yaw = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, -dx);
    const quat pitch = quat::from_axis_angle(local_right, dy);
    orientation = normalize(yaw * pitch * orientation);
}

void Camera::spin_world_y(float radians)
{
    const quat r = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, radians);
    orientation = normalize(r * orientation);
}

void Camera::process_key(platform::Key key, float dt)
{
    const float orbit_speed = 2.5f;
    const float zoom_speed = distance * 1.5f; // scales with distance so zoom feels consistent

    switch (key)
    {
    case platform::Key::A:
    case platform::Key::Left:
        orbit(-orbit_speed * dt, 0.0f);
        break;
    case platform::Key::D:
    case platform::Key::Right:
        orbit(orbit_speed * dt, 0.0f);
        break;
    case platform::Key::W:
    case platform::Key::Up:
        orbit(0.0f, orbit_speed * dt);
        break;
    case platform::Key::S:
    case platform::Key::Down:
        orbit(0.0f, -orbit_speed * dt);
        break;
    case platform::Key::Plus:
        distance -= zoom_speed * dt;
        break;
    case platform::Key::Minus:
        distance += zoom_speed * dt;
        break;
    default:
        break;
    }

    distance = clamp(distance, near_plane * 2.0f, far_plane * 0.5f);
}
