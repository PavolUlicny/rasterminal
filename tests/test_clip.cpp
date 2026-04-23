#include "test.h"
#include "../src/clip.h"

// clip_reject is a conservative frustum-rejection test: it MUST return true
// when the triangle is provably invisible, and is allowed to return false even
// for triangles that turn out to be off-screen after fuller clipping. So tests
// come in two shapes:
//   - REJECT: clip_reject must return true (an obviously-invisible triangle).
//   - ACCEPT: clip_reject must return false (visible, or borderline cases the
//     function is not allowed to reject because that would drop real pixels).

// Build a clip-space vertex from NDC-like (x,y,z) with an explicit w.
// After perspective divide these would give NDC = (x/w, y/w, z/w).
static vec4 clip_v(float x, float y, float z, float w)
{
    return {x, y, z, w};
}

// ─── inside the frustum ──────────────────────────────────────────────────────

TEST(clip_reject, triangle_fully_inside_is_accepted)
{
    vec4 a = clip_v(0, 0, 0, 1);
    vec4 b = clip_v(0.5f, 0, 0, 1);
    vec4 c = clip_v(0, 0.5f, 0, 1);
    ASSERT_FALSE(clip_reject(a, b, c));
}

// ─── behind-camera rejection (w ≤ 0) ────────────────────────────────────────

TEST(clip_reject, any_vertex_with_zero_w_rejects)
{
    // A zero-w vertex would divide by zero in perspective_divide.
    vec4 a = clip_v(0, 0, 0, 0);
    vec4 b = clip_v(0.5f, 0, 0, 1);
    vec4 c = clip_v(0, 0.5f, 0, 1);
    ASSERT_TRUE(clip_reject(a, b, c));
}

TEST(clip_reject, any_vertex_with_negative_w_rejects)
{
    vec4 a = clip_v(0, 0, 0, -0.5f);
    vec4 b = clip_v(0.5f, 0, 0, 1);
    vec4 c = clip_v(0, 0.5f, 0, 1);
    ASSERT_TRUE(clip_reject(a, b, c));
}

// ─── per-plane rejection: all vertices outside the same half-space ──────────

TEST(clip_reject, all_outside_right_plane_rejects)
{
    // x > w on every vertex → triangle entirely to the right of the view frustum.
    vec4 a = clip_v(2.0f, 0, 0, 1);
    vec4 b = clip_v(3.0f, 0, 0, 1);
    vec4 c = clip_v(2.5f, 1, 0, 1);
    ASSERT_TRUE(clip_reject(a, b, c));
}

TEST(clip_reject, all_outside_left_plane_rejects)
{
    vec4 a = clip_v(-2.0f, 0, 0, 1);
    vec4 b = clip_v(-3.0f, 0, 0, 1);
    vec4 c = clip_v(-2.5f, 1, 0, 1);
    ASSERT_TRUE(clip_reject(a, b, c));
}

TEST(clip_reject, all_outside_top_plane_rejects)
{
    vec4 a = clip_v(0, 2.0f, 0, 1);
    vec4 b = clip_v(0, 3.0f, 0, 1);
    vec4 c = clip_v(0.5f, 2.5f, 0, 1);
    ASSERT_TRUE(clip_reject(a, b, c));
}

TEST(clip_reject, all_outside_bottom_plane_rejects)
{
    vec4 a = clip_v(0, -2.0f, 0, 1);
    vec4 b = clip_v(0, -3.0f, 0, 1);
    vec4 c = clip_v(0.5f, -2.5f, 0, 1);
    ASSERT_TRUE(clip_reject(a, b, c));
}

TEST(clip_reject, all_behind_far_plane_rejects)
{
    // z > w on every vertex → triangle beyond the far plane.
    vec4 a = clip_v(0, 0, 2.0f, 1);
    vec4 b = clip_v(0, 0, 3.0f, 1);
    vec4 c = clip_v(0.5f, 0, 2.5f, 1);
    ASSERT_TRUE(clip_reject(a, b, c));
}

TEST(clip_reject, all_in_front_of_near_plane_rejects)
{
    // z < -w on every vertex.
    vec4 a = clip_v(0, 0, -2.0f, 1);
    vec4 b = clip_v(0, 0, -3.0f, 1);
    vec4 c = clip_v(0.5f, 0, -2.5f, 1);
    ASSERT_TRUE(clip_reject(a, b, c));
}

// ─── must NOT reject borderline cases ───────────────────────────────────────
// Dropping these would lose visible pixels — the whole point of "conservative".

TEST(clip_reject, straddling_right_plane_is_accepted)
{
    // One vertex inside, two outside on the right → crosses the plane, visible.
    vec4 a = clip_v(0, 0, 0, 1);       // inside
    vec4 b = clip_v(2.0f, 0, 0, 1);    // outside right
    vec4 c = clip_v(2.0f, 0.5f, 0, 1); // outside right
    ASSERT_FALSE(clip_reject(a, b, c));
}

TEST(clip_reject, mixed_sides_not_rejected)
{
    // Vertices outside different half-spaces (one left, one right, one top) —
    // no single plane has all three on its outside, so the triangle still
    // intersects the frustum somewhere.
    vec4 a = clip_v(-2.0f, 0, 0, 1);
    vec4 b = clip_v(2.0f, 0, 0, 1);
    vec4 c = clip_v(0, 2.0f, 0, 1);
    ASSERT_FALSE(clip_reject(a, b, c));
}

TEST(clip_reject, vertex_exactly_on_boundary_is_accepted)
{
    // x == w is on the boundary, inside the closed half-space — the strict
    // > test means a vertex with x==w is treated as inside.
    vec4 a = clip_v(1.0f, 0, 0, 1); // on right boundary
    vec4 b = clip_v(0, 0, 0, 1);
    vec4 c = clip_v(0, 0.5f, 0, 1);
    ASSERT_FALSE(clip_reject(a, b, c));
}
