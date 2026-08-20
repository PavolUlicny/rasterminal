#pragma once

// This is a test-prelude header: the renderer test TUs include it and rely on
// it to pull in the test framework and Renderer. IWYU pragma: export marks those
// as intentionally re-exported so clangd's include-cleaner doesn't flag them as
// unused here.
#include "tests/test.h" // IWYU pragma: export
#include "tests/rasterize_test_util.h"
#include "src/render/renderer.h" // IWYU pragma: export
#include "src/render/texture.h"

// Camera at (0,0,5) looking at origin.  Identity orientation so the projection
// is symmetric and pixel (20,10) on a 40×20 framebuffer maps exactly to world (0,0,0).
// fov = π/2, aspect 2:1 → x_ndc = x_world/(2*(5-z)), y_ndc = y_world/(5-z).
inline Camera make_test_camera()
{
    Camera c;
    c.target = { 0.0f, 0.0f, 0.0f };
    c.distance = 5.0f;
    c.orientation = quat::identity();
    c.fov = 3.14159265f / 2.0f;
    c.near_plane = 0.1f;
    c.far_plane = 100.0f;
    return c;
}

// Single-triangle mesh at z=0 facing the camera (+Z).
// Front face: v0=(-1,-1,0), v1=(1,-1,0), v2=(0,1,0).  Normal = (0,0,1).
// With the test camera, projects to sa≈(18,12), sb≈(22,12), sc≈(20,8) on 40×20,
// covering centre pixel (20,10).
// flip_winding=true swaps v1/v2 so the triangle faces -Z (back-face from camera).
// double_sided=true marks the single material as double-sided.
inline Mesh make_unit_triangle(bool flip_winding = false, bool double_sided = false)
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = { 0.0f, 0.0f, 1.0f };
    v.uv = { 0.5f, 0.5f };

    v.pos = { -1.0f, -1.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 1.0f, -1.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 0.0f, 1.0f, 0.0f };
    m.vertices.push_back(v);

    m.tangents.resize(3, { 1.0f, 0.0f, 0.0f });

    Triangle tri{};
    if (flip_winding)
    {
        tri.v[0] = 0;
        tri.v[1] = 2; // swap v1/v2 → face normal points -Z
        tri.v[2] = 1;
    }
    else
    {
        tri.v[0] = 0;
        tri.v[1] = 1;
        tri.v[2] = 2;
    }
    tri.material_idx = 0;
    m.triangles.push_back(tri);

    Material mat;
    mat.double_sided = double_sided;
    m.materials.push_back(mat);

    if (double_sided)
    {
        m.has_double_sided = true;
    }

    return m;
}

// Large screen-filling triangle: z=0, v0=(-4,-4,0), v1=(4,-4,0), v2=(0,4,0).
// With the test camera, projects to sa≈(12,18), sb≈(28,18), sc≈(20,2) on 40×20,
// spanning all 4 worker bands.  Face normal = +Z (front-facing from +Z camera).
inline Mesh make_large_triangle()
{
    Mesh m;
    Vertex v{};
    v.ao = 1.0f;
    v.normal = { 0.0f, 0.0f, 1.0f };
    v.uv = { 0.5f, 0.5f };

    v.pos = { -4.0f, -4.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 4.0f, -4.0f, 0.0f };
    m.vertices.push_back(v);
    v.pos = { 0.0f, 4.0f, 0.0f };
    m.vertices.push_back(v);

    m.tangents.resize(3, { 1.0f, 0.0f, 0.0f });

    Triangle tri{};
    tri.v[0] = 0;
    tri.v[1] = 1;
    tri.v[2] = 2;
    tri.material_idx = 0;
    m.triangles.push_back(tri);
    m.materials.push_back(Material{});
    return m;
}

inline Light make_key_light_z(vec3 color = { 1.0f, 0.0f, 0.0f })
{
    Light l{};
    l.direction = { 0.0f, 0.0f, 1.0f };
    l.color = color;
    return l;
}

// Count pixels with stored depth < +inf (i.e. something was drawn). Read-only.
inline int count_drawn_pixels(Framebuffer &fb)
{
    int n = 0;
    for (int y = 0; y < fb.height(); y++)
    {
        for (int x = 0; x < fb.width(); x++)
        {
            if (was_drawn(fb, x, y))
            {
                n++;
            }
        }
    }
    return n;
}

// Build an in-memory RGBA texture, same helper pattern as test_rasterize_texture.cpp.
inline Texture make_solid_tex_rgba(int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    Texture t;
    t.width = w;
    t.height = h;
    t.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (int i = 0; i < w * h; i++)
    {
        const size_t base = static_cast<size_t>(i) * 4;
        t.pixels[base + 0] = r;
        t.pixels[base + 1] = g;
        t.pixels[base + 2] = b;
        t.pixels[base + 3] = 255;
    }
    return t;
}
