#pragma once

#include <cmath>

// ─── vec2 ────────────────────────────────────────────────────────────────────

struct vec2
{
    float x, y;

    constexpr vec2() : x(0), y(0) {}
    constexpr vec2(float x_, float y_) : x(x_), y(y_) {}

    constexpr vec2 operator+(const vec2 &o) const { return {x + o.x, y + o.y}; }
    constexpr vec2 operator-(const vec2 &o) const { return {x - o.x, y - o.y}; }
    constexpr vec2 operator*(float t) const { return {x * t, y * t}; }
    constexpr vec2 operator/(float t) const { return {x / t, y / t}; }
};

// ─── vec3 ────────────────────────────────────────────────────────────────────

struct vec3
{
    float x, y, z;

    constexpr vec3() : x(0), y(0), z(0) {}
    constexpr vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    constexpr vec3 operator+(const vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr vec3 operator-(const vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr vec3 operator*(float t) const { return {x * t, y * t, z * t}; }
    constexpr vec3 operator*(const vec3 &o) const { return {x * o.x, y * o.y, z * o.z}; } // component-wise
    constexpr vec3 operator/(float t) const { return {x / t, y / t, z / t}; }
    constexpr vec3 operator-() const { return {-x, -y, -z}; }

    constexpr vec3 &operator+=(const vec3 &o)
    {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    constexpr vec3 &operator*=(float t)
    {
        x *= t;
        y *= t;
        z *= t;
        return *this;
    }

    float length() const { return std::sqrt(x * x + y * y + z * z); }
    constexpr float length_sq() const { return x * x + y * y + z * z; }
};

constexpr float dot(const vec3 &a, const vec3 &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

constexpr vec3 cross(const vec3 &a, const vec3 &b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}

inline vec3 normalize(const vec3 &v)
{
    const float len_sq = v.length_sq();
    if (len_sq < 1e-16f)
        return {};
    const float inv_len = 1.0f / std::sqrt(len_sq);
    return {v.x * inv_len, v.y * inv_len, v.z * inv_len};
}

constexpr vec3 lerp(const vec3 &a, const vec3 &b, float t)
{
    return a + (b - a) * t;
}

constexpr vec3 reflect(const vec3 &incident, const vec3 &normal)
{
    return incident - normal * (2.0f * dot(incident, normal));
}

// ─── vec4 ────────────────────────────────────────────────────────────────────

struct vec4
{
    float x, y, z, w;

    constexpr vec4() : x(0), y(0), z(0), w(0) {}
    constexpr vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr vec4(const vec3 &v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}

    constexpr vec4 operator+(const vec4 &o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    constexpr vec4 operator-(const vec4 &o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    constexpr vec4 operator*(float t) const { return {x * t, y * t, z * t, w * t}; }

    // perspective divide: clip space → NDC
    constexpr vec3 xyz() const { return {x, y, z}; }
    constexpr vec3 perspective_divide() const
    {
        const float inv_w = 1.0f / w;
        return {x * inv_w, y * inv_w, z * inv_w};
    }
};

// ─── mat4 ────────────────────────────────────────────────────────────────────
// column-major: m[col][row]

struct mat4
{
    float m[4][4];

    constexpr mat4() : m{} {}

    static constexpr mat4 identity()
    {
        mat4 result;
        result.m[0][0] = result.m[1][1] = result.m[2][2] = result.m[3][3] = 1.0f;
        return result;
    }

    constexpr mat4 operator*(const mat4 &o) const
    {
        mat4 result;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++)
                    result.m[c][r] += m[k][r] * o.m[c][k];
        return result;
    }

    constexpr vec4 operator*(const vec4 &v) const
    {
        return {
            m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z + m[3][0] * v.w,
            m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z + m[3][1] * v.w,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z + m[3][2] * v.w,
            m[0][3] * v.x + m[1][3] * v.y + m[2][3] * v.z + m[3][3] * v.w};
    }

    constexpr mat4 transposed() const
    {
        mat4 result;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                result.m[r][c] = m[c][r];
        return result;
    }
};

// ─── transform factories ─────────────────────────────────────────────────────

constexpr mat4 translation(float tx, float ty, float tz)
{
    mat4 m = mat4::identity();
    m.m[3][0] = tx;
    m.m[3][1] = ty;
    m.m[3][2] = tz;
    return m;
}

constexpr mat4 translation(const vec3 &t)
{
    return translation(t.x, t.y, t.z);
}

constexpr mat4 scale(float sx, float sy, float sz)
{
    mat4 m = mat4::identity();
    m.m[0][0] = sx;
    m.m[1][1] = sy;
    m.m[2][2] = sz;
    return m;
}

constexpr mat4 scale(float s)
{
    return scale(s, s, s);
}

inline mat4 rotation_x(float radians)
{
    mat4 m = mat4::identity();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    m.m[1][1] = c;
    m.m[2][1] = -s;
    m.m[1][2] = s;
    m.m[2][2] = c;
    return m;
}

inline mat4 rotation_y(float radians)
{
    mat4 m = mat4::identity();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    m.m[0][0] = c;
    m.m[2][0] = s;
    m.m[0][2] = -s;
    m.m[2][2] = c;
    return m;
}

inline mat4 rotation_z(float radians)
{
    mat4 m = mat4::identity();
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    m.m[0][0] = c;
    m.m[1][0] = -s;
    m.m[0][1] = s;
    m.m[1][1] = c;
    return m;
}

// ─── view / projection ───────────────────────────────────────────────────────

inline mat4 look_at(const vec3 &eye, const vec3 &target, const vec3 &up)
{
    const vec3 f = normalize(target - eye); // forward
    const vec3 r = normalize(cross(f, up)); // right
    const vec3 u = cross(r, f);             // up (reorthogonalized)

    mat4 m = mat4::identity();
    m.m[0][0] = r.x;
    m.m[1][0] = r.y;
    m.m[2][0] = r.z;
    m.m[0][1] = u.x;
    m.m[1][1] = u.y;
    m.m[2][1] = u.z;
    m.m[0][2] = -f.x;
    m.m[1][2] = -f.y;
    m.m[2][2] = -f.z;
    m.m[3][0] = -dot(r, eye);
    m.m[3][1] = -dot(u, eye);
    m.m[3][2] = dot(f, eye);
    return m;
}

// fov_y in radians, near/far are positive distances
inline mat4 perspective(float fov_y, float aspect, float near, float far)
{
    const float tan_half = std::tan(fov_y / 2.0f);

    mat4 m;
    m.m[0][0] = 1.0f / (aspect * tan_half);
    m.m[1][1] = 1.0f / tan_half;
    m.m[2][2] = -(far + near) / (far - near);
    m.m[3][2] = -(2.0f * far * near) / (far - near);
    m.m[2][3] = -1.0f;
    return m;
}

// ─── quat ────────────────────────────────────────────────────────────────────

struct quat
{
    float x, y, z, w;

    constexpr quat() : x(0), y(0), z(0), w(1) {}
    constexpr quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static constexpr quat identity() { return {0, 0, 0, 1}; }

    static quat from_axis_angle(const vec3 &axis, float radians)
    {
        const float half = radians * 0.5f;
        const float s = std::sin(half);
        return {axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
    }

    constexpr quat operator*(const quat &o) const
    {
        return {
            w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w,
            w * o.w - x * o.x - y * o.y - z * o.z};
    }

    constexpr vec3 rotate(const vec3 &v) const
    {
        const vec3 qv{x, y, z};
        const vec3 t = cross(qv, v) * 2.0f;
        return v + t * w + cross(qv, t);
    }
};

inline quat normalize(const quat &q)
{
    const float len_sq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (len_sq < 1e-8f)
        return quat::identity();
    const float inv = 1.0f / std::sqrt(len_sq);
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// ─── scalar utilities ────────────────────────────────────────────────────────

constexpr float clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr float to_radians(float degrees)
{
    return degrees * (3.14159265358979323846f / 180.0f);
}
