#pragma once

#include <cmath>

// ─── vec2 ────────────────────────────────────────────────────────────────────

struct vec2
{
    float x, y;

    vec2() : x(0), y(0) {}
    vec2(float x, float y) : x(x), y(y) {}

    vec2 operator+(const vec2 &o) const { return {x + o.x, y + o.y}; }
    vec2 operator-(const vec2 &o) const { return {x - o.x, y - o.y}; }
    vec2 operator*(float t) const { return {x * t, y * t}; }
    vec2 operator/(float t) const { return {x / t, y / t}; }
};

// ─── vec3 ────────────────────────────────────────────────────────────────────

struct vec3
{
    float x, y, z;

    vec3() : x(0), y(0), z(0) {}
    vec3(float x, float y, float z) : x(x), y(y), z(z) {}

    vec3 operator+(const vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    vec3 operator-(const vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    vec3 operator*(float t) const { return {x * t, y * t, z * t}; }
    vec3 operator/(float t) const { return {x / t, y / t, z / t}; }
    vec3 operator-() const { return {-x, -y, -z}; }

    vec3 &operator+=(const vec3 &o)
    {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    vec3 &operator*=(float t)
    {
        x *= t;
        y *= t;
        z *= t;
        return *this;
    }

    float length() const { return std::sqrt(x * x + y * y + z * z); }
    float length_sq() const { return x * x + y * y + z * z; }
};

inline float dot(const vec3 &a, const vec3 &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline vec3 cross(const vec3 &a, const vec3 &b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}

inline vec3 normalize(const vec3 &v)
{
    return v / v.length();
}

inline vec3 lerp(const vec3 &a, const vec3 &b, float t)
{
    return a + (b - a) * t;
}

inline vec3 reflect(const vec3 &incident, const vec3 &normal)
{
    return incident - normal * (2.0f * dot(incident, normal));
}

// ─── vec4 ────────────────────────────────────────────────────────────────────

struct vec4
{
    float x, y, z, w;

    vec4() : x(0), y(0), z(0), w(0) {}
    vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    vec4(const vec3 &v, float w) : x(v.x), y(v.y), z(v.z), w(w) {}

    vec4 operator+(const vec4 &o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    vec4 operator-(const vec4 &o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    vec4 operator*(float t) const { return {x * t, y * t, z * t, w * t}; }

    // perspective divide: clip space → NDC
    vec3 xyz() const { return {x, y, z}; }
    vec3 perspective_divide() const { return {x / w, y / w, z / w}; }
};

// ─── mat4 ────────────────────────────────────────────────────────────────────
// column-major: m[col][row]

struct mat4
{
    float m[4][4];

    mat4()
    {
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                m[c][r] = 0.0f;
    }

    static mat4 identity()
    {
        mat4 result;
        result.m[0][0] = result.m[1][1] = result.m[2][2] = result.m[3][3] = 1.0f;
        return result;
    }

    mat4 operator*(const mat4 &o) const
    {
        mat4 result;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++)
                    result.m[c][r] += m[k][r] * o.m[c][k];
        return result;
    }

    vec4 operator*(const vec4 &v) const
    {
        return {
            m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z + m[3][0] * v.w,
            m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z + m[3][1] * v.w,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z + m[3][2] * v.w,
            m[0][3] * v.x + m[1][3] * v.y + m[2][3] * v.z + m[3][3] * v.w};
    }

    mat4 transposed() const
    {
        mat4 result;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                result.m[r][c] = m[c][r];
        return result;
    }
};

// ─── transform factories ─────────────────────────────────────────────────────

inline mat4 translation(float tx, float ty, float tz)
{
    mat4 m = mat4::identity();
    m.m[3][0] = tx;
    m.m[3][1] = ty;
    m.m[3][2] = tz;
    return m;
}

inline mat4 translation(const vec3 &t)
{
    return translation(t.x, t.y, t.z);
}

inline mat4 scale(float sx, float sy, float sz)
{
    mat4 m = mat4::identity();
    m.m[0][0] = sx;
    m.m[1][1] = sy;
    m.m[2][2] = sz;
    return m;
}

inline mat4 scale(float s)
{
    return scale(s, s, s);
}

inline mat4 rotation_x(float radians)
{
    mat4 m = mat4::identity();
    float c = std::cos(radians), s = std::sin(radians);
    m.m[1][1] = c;
    m.m[2][1] = -s;
    m.m[1][2] = s;
    m.m[2][2] = c;
    return m;
}

inline mat4 rotation_y(float radians)
{
    mat4 m = mat4::identity();
    float c = std::cos(radians), s = std::sin(radians);
    m.m[0][0] = c;
    m.m[2][0] = s;
    m.m[0][2] = -s;
    m.m[2][2] = c;
    return m;
}

inline mat4 rotation_z(float radians)
{
    mat4 m = mat4::identity();
    float c = std::cos(radians), s = std::sin(radians);
    m.m[0][0] = c;
    m.m[1][0] = -s;
    m.m[0][1] = s;
    m.m[1][1] = c;
    return m;
}

// ─── view / projection ───────────────────────────────────────────────────────

inline mat4 look_at(const vec3 &eye, const vec3 &target, const vec3 &up)
{
    vec3 f = normalize(target - eye); // forward
    vec3 r = normalize(cross(f, up)); // right
    vec3 u = cross(r, f);             // up (reorthogonalized)

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
    float tan_half = std::tan(fov_y / 2.0f);

    mat4 m;
    m.m[0][0] = 1.0f / (aspect * tan_half);
    m.m[1][1] = 1.0f / tan_half;
    m.m[2][2] = -(far + near) / (far - near);
    m.m[3][2] = -(2.0f * far * near) / (far - near);
    m.m[2][3] = -1.0f;
    return m;
}

// ─── scalar utilities ────────────────────────────────────────────────────────

inline float clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float to_radians(float degrees)
{
    return degrees * (3.14159265358979323846f / 180.0f);
}
