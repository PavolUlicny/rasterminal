#include "camera.h"
#include "input.h"
#include "linalg.h"

#include <cmath>

namespace
{
    // The turntable rotation composition, shared by orbit() and look() so the two
    // cannot drift: yaw about world Y, pitch about the camera's own right axis.
    // Composition order is load-bearing.
    quat rotated(const quat &o, float dx, float dy)
    {
        const vec3 local_right = o.rotate({ 1.0f, 0.0f, 0.0f });
        const quat yaw = quat::from_axis_angle({ 0.0f, 1.0f, 0.0f }, -dx);
        const quat pitch = quat::from_axis_angle(local_right, dy);
        return normalize(yaw * pitch * o);
    }

    // First-person's pivot rule, shared by every rotation that reaches this camera:
    // adopt the new orientation with the eye held still, re-deriving `target` ahead of
    // it. Only `target` is stored, so a rotation that forgets this step swings the eye
    // around a point `distance` in front of the camera instead of turning the view.
    void reorient_about_eye(Camera &c, const quat &o)
    {
        const vec3 eye_before = c.eye();
        c.orientation = o;
        c.target = eye_before + c.forward() * c.distance;
    }

    // Growth applied to the first-person speed multiplier for one frame of a held
    // +/- key, derived from Camera's two constants rather than hand-computed from them
    // so neither can drift away from the other.
    //
    // Fed dts summing to HELD_KEY_WINDOW the factors multiply to exactly one wheel
    // notch, but a real tap only lands within a frame of it, in EITHER direction.
    // main.cpp observes the latch at frame boundaries and hands over whole frame
    // intervals, and the first of those measures the interval that ended BEFORE the
    // key arrived, so the total is a whole number of frames offset from the window by
    // where in a frame the byte fell. Under even pacing that only ever overshoots
    // (+1.3% at 60 fps, +3.9% at 20); a short frame at the press followed by longer
    // ones undershoots instead (measured -0.38%). Bounded by one frame either way,
    // which is why the dt cap exists. Held for a second the multiplier moves about
    // 2.2x, crossing the whole speed range in about eight.
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
    // Turntable yaw is about world Y; when the camera is upside down (up vector's
    // world-Y component negative) that rotation sweeps the screen opposite to the
    // drag, so invert dx to keep the model following the mouse (Blender behaviour).
    // up().y is the same quantity the closed form 1 - 2(x^2 + z^2) gives for a unit
    // quat (it is R[1][1]); orientation is always unit, normalized after every
    // rotation. Shared with look()'s past-the-pole test rather than written twice.
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

    // Both modes apply the same rotation to `orientation`; only the pivot differs,
    // and that alone gives each mode its convention. The eye and the look direction
    // are opposite vectors, so one rotation reads as "the model follows the drag"
    // while swinging the eye, and as "the view follows the drag" while swinging the
    // look direction. Negating an axis here would invert first-person, not fix it.

    // Pitch clamps to straight up / straight down: a fly camera must never roll, and
    // looping past a pole would invert the horizon. The rotation is about the camera's
    // right axis, which stays horizontal for as long as there is no roll, so the
    // elevation change equals the rotation angle exactly and the limit applies to the
    // delta rather than being corrected for afterwards.
    //
    // asin() alone cannot express that limit, because it is even about the pole: at
    // 90+d it returns 90-d, so an overshoot reads as room for exactly as much again and
    // the excess DOUBLES every call. Rounding in the rotation is enough to start it,
    // and the horizon then inverts within about 31 calls, half a second of holding a
    // key (measured, and equally from pure pitch: dx is not involved). Two things stop
    // it. The up vector's Y component carries the sign asin drops, so folding it in
    // makes an overshoot pull back instead of pushing on; and stopping a hair short of
    // the pole keeps ordinary rounding from crossing at all. A test that watches only
    // forward.y cannot see any of this, since that value is identical either side.
    constexpr float PI = 3.14159265f;
    const vec3 fwd = forward();
    float pitch_now = std::asin(clamp(fwd.y, -1.0f, 1.0f));
    if (up().y < 0.0f)
    {
        pitch_now = (fwd.y >= 0.0f) ? (PI - pitch_now) : (-PI - pitch_now);
    }
    dy = clamp(dy, -FP_MAX_PITCH - pitch_now, FP_MAX_PITCH - pitch_now);

    // orbit()'s upside-down dx inversion has no counterpart here: the pitch clamp puts
    // upside down out of reach.
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
    // Only `target` is stored; the eye is derived from it, so it comes along.
    target = target + step * (fp_base_speed * fp_speed * dt);

    // Outer bound: the distance orbit's zoom-out already stops at, measured from the
    // model centre, so both modes reach the same positions radially. Past far_plane
    // the model is clipped away entirely and nothing on screen says which way back is.
    // No inner bound, since flying into the geometry is the point of the mode.
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
        // Spin is a rotation like any other and takes the same pivot, which is what
        // makes it a panorama from where the camera stands rather than a turntable.
        // Pivoting on `target` instead would fly the eye around a circle of radius
        // `distance`, and past move()'s far bound, which nothing else enforces.
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
        // WASD leaves the rotation group to translate, so the arrows stop being an
        // alias for it and become the only keyboard look. +/- retune the movement
        // speed rather than zooming: `distance` is the lever arm `target` is derived
        // through, so scaling it does move the eye, but it moves it straight along the
        // view axis, which is what W and S already do. The wheel would be duplicating
        // a key instead of adding a control, and speed is what freelook puts there.
        //
        // Looking is slower than orbiting deliberately. Orbiting keeps the model
        // centred and only turns it, while looking sweeps the whole scene across the
        // frame, so the same angular rate reads much faster here. The anchor is one
        // VERTICAL field of view per second, `fov` being the vertical one: a pitch
        // sweeps the frame's height in about a second, and a yaw sweeps that same
        // angle, which on a wide terminal is less than the frame's width (roughly
        // two thirds of it at 80x24). Deliberate: the alternative is scaling yaw by
        // the aspect, which would change how fast you turn when the terminal is
        // resized, and a turn rate that moves under you is worse than a slow one.
        //
        // One key acts per frame, so there is no diagonal flight (no forward+strafe,
        // no moving while turning). That is not a choice: a terminal delivers no key
        // releases, so a held key is inferred from autorepeat, and the operating system
        // repeats only the most recently pressed one. The others stop sending anything
        // at all, which nothing here can observe. Drag the mouse to look while moving.
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

    distance = clamp(distance, near_plane * 2.0f, max_eye_distance());
}
