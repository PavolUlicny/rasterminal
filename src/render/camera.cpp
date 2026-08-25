#include "src/render/camera.h"
#include "src/math/linalg.h"
#include "src/platform/input.h"

#include <cmath>

namespace
{
    // Yaw around world Y, then pitch around camera right. Order matters.
    quat rotated(const quat &o, float dx, float dy)
    {
        const vec3 local_right = o.rotate({ 1.0f, 0.0f, 0.0f });
        const quat yaw = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, -dx);
        const quat pitch = quat::from_axis_angle(local_right, dy);
        return normalize(yaw * pitch * o);
    }

    // First-person rotations hold the eye fixed and rebuild the stored look-at target.
    void reorient_about_eye(Camera &c, const quat &o)
    {
        const vec3 eye_before = c.eye();
        c.orientation = o;
        c.target = eye_before + c.forward() * c.distance;
    }

    // A held key changes speed by one wheel step per latch window.
    float speed_key_factor(float dt)
    {
        return std::pow(Camera::FP_SPEED_WHEEL_STEP, dt / Camera::HELD_KEY_WINDOW);
    }
} // namespace

float Camera::max_eye_distance() const
{
    return far_plane * 0.5f;
}

vec3 Camera::eye() const
{
    return target + orientation.rotate({ 0.0f, 0.0f, 1.0f }) * distance;
}

vec3 Camera::forward() const
{
    return orientation.rotate({ 0.0f, 0.0f, -1.0f });
}

vec3 Camera::up() const
{
    return orientation.rotate({ 0.0f, 1.0f, 0.0f });
}

mat4 Camera::view(const vec3 &eye_pos) const
{
    return look_at(eye_pos, target, up());
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
    // World-Y yaw reverses on screen when upside down; invert it to preserve drag direction.
    if (up().y < 0.0f)
    {
        dx = -dx;
    }

    orientation = rotated(orientation, dx, dy);
}

void Camera::look(float dx, float dy)
{
    if (!first_person)
    {
        orbit(dx, dy);
        return;
    }

    // Recover pitch past a pole with up().y because asin alone cannot distinguish 90+d
    // from 90-d. The clamp keeps first-person upright and prevents horizon inversion.
    constexpr float PI = 3.14159265f;
    const vec3 fwd = forward();
    float pitch_now = std::asin(clamp(fwd.y, -1.0f, 1.0f));
    if (up().y < 0.0f)
    {
        pitch_now = (fwd.y >= 0.0f) ? (PI - pitch_now) : (-PI - pitch_now);
    }
    dy = clamp(dy, -FP_MAX_PITCH - pitch_now, FP_MAX_PITCH - pitch_now);

    // The pitch clamp makes orbit()'s upside-down yaw correction unnecessary.
    reorient_about_eye(*this, rotated(orientation, dx, dy));
}

void Camera::move(float fwd, float right, float world_up, float dt)
{
    if (!first_person)
    {
        return;
    }

    vec3 step = forward() * fwd + orientation.rotate({ 1.0f, 0.0f, 0.0f }) * right;
    step.y += world_up; // world-absolute, not view-relative: E/V must still rise and fall while pitched
    // The derived eye moves with the stored target.
    target = target + step * (fp_base_speed * fp_speed * dt);

    // Match orbit's outer bound. There is no inner bound because flight may enter geometry.
    const vec3 offset = eye() - fp_centre;
    const float dist = offset.length();
    const float max_dist = max_eye_distance();
    if (dist > max_dist)
    {
        target = fp_centre + offset * (max_dist / dist) + forward() * distance;
    }
}

void Camera::adjust_speed(float factor)
{
    fp_speed = clamp(fp_speed * factor, FP_SPEED_MIN, FP_SPEED_MAX);
}

void Camera::spin_world_y(float radians)
{
    const quat r = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, radians);
    if (first_person)
    {
        // Pivot around the eye so spin pans in place instead of orbiting the target.
        reorient_about_eye(*this, normalize(r * orientation));
        return;
    }
    orientation = normalize(r * orientation);
}

void Camera::process_key(platform::Key key, float dt)
{
    const float orbit_speed = 2.5f;

    if (first_person)
    {
        // Look at one vertical field of view per second. One movement key acts per frame
        // because terminals report repeats, not key releases, so chords cannot be tracked.
        const float look_speed = fov;
        switch (key)
        {
        case platform::Key::W:
            move(1.0f, 0.0f, 0.0f, dt);
            break;
        case platform::Key::S:
            move(-1.0f, 0.0f, 0.0f, dt);
            break;
        case platform::Key::D:
            move(0.0f, 1.0f, 0.0f, dt);
            break;
        case platform::Key::A:
            move(0.0f, -1.0f, 0.0f, dt);
            break;
        case platform::Key::E:
            move(0.0f, 0.0f, 1.0f, dt);
            break;
        case platform::Key::V:
            move(0.0f, 0.0f, -1.0f, dt);
            break;
        case platform::Key::Left:
            look(-look_speed * dt, 0.0f);
            break;
        case platform::Key::Right:
            look(look_speed * dt, 0.0f);
            break;
        case platform::Key::Up:
            look(0.0f, look_speed * dt);
            break;
        case platform::Key::Down:
            look(0.0f, -look_speed * dt);
            break;
        case platform::Key::Plus:
            adjust_speed(speed_key_factor(dt));
            break;
        case platform::Key::Minus:
            adjust_speed(1.0f / speed_key_factor(dt));
            break;
        default:
            break;
        }
        return;
    }

    const float zoom_speed = distance * 1.5f;

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

    distance = clamp(distance, near_plane * 2.0f, max_eye_distance());
}
