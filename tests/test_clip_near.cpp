#include "test.h"
#include "../src/rasterize.h"

static ClipVert make_cv(float w, float u = 0.0f, float v_coord = 0.0f,
                        float px = 0.0f, float py = 0.0f, float pz = 0.0f)
{
    ClipVert r{};
    r.c = {0.0f, 0.0f, 0.0f, w};
    r.uv = {u, v_coord};
    r.pos = {px, py, pz};
    return r;
}

static constexpr float NEAR = 0.1f;

// ─── count tests ──────────────────────────────────────────────────────────────

TEST(clip_near, all_inside_returns_one_triangle)
{
    ClipVert a = make_cv(1.0f), b = make_cv(2.0f), c = make_cv(3.0f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 1);
    ASSERT_NEAR(out[0][0].c.w, 1.0f, 1e-6f);
    ASSERT_NEAR(out[0][1].c.w, 2.0f, 1e-6f);
    ASSERT_NEAR(out[0][2].c.w, 3.0f, 1e-6f);
}

TEST(clip_near, all_outside_returns_zero)
{
    ClipVert a = make_cv(0.05f), b = make_cv(0.03f), c = make_cv(0.01f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 0);
}

TEST(clip_near, all_exactly_on_near_plane_returns_zero)
{
    // Condition is strict w > near_w, so w == near_w is outside.
    ClipVert a = make_cv(NEAR), b = make_cv(NEAR), c = make_cv(NEAR);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 0);
}

TEST(clip_near, one_inside_returns_one_triangle)
{
    ClipVert a = make_cv(1.0f), b = make_cv(0.05f), c = make_cv(0.05f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 1);
}

TEST(clip_near, two_inside_returns_two_triangles)
{
    ClipVert a = make_cv(1.0f), b = make_cv(2.0f), c = make_cv(0.05f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 2);
}

TEST(clip_near, exactly_on_plane_treated_as_outside)
{
    // w == near_w fails the strict > test → outside. Two inside + one on-plane → n=2 → 2 tris.
    ClipVert a = make_cv(1.0f), b = make_cv(1.0f), c = make_cv(NEAR);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 2);
}

// ─── clipped vertex position ──────────────────────────────────────────────────

TEST(clip_near, clipped_vertices_land_at_near_w)
{
    // a inside (w=2), b and c outside (w=0). Both clipped edges must land at NEAR.
    ClipVert a = make_cv(2.0f), b = make_cv(0.0f), c = make_cv(0.0f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 1);
    // out[0][0] = a (inside vert), [1] and [2] are the clipped intersections.
    ASSERT_NEAR(out[0][1].c.w, NEAR, 1e-5f);
    ASSERT_NEAR(out[0][2].c.w, NEAR, 1e-5f);
}

TEST(clip_near, two_inside_all_output_verts_at_or_beyond_near)
{
    ClipVert a = make_cv(1.0f), b = make_cv(2.0f), c = make_cv(0.05f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 2);
    for (int t = 0; t < 2; t++)
        for (int v = 0; v < 3; v++)
            ASSERT_TRUE(out[t][v].c.w >= NEAR - 1e-5f);
}

// ─── attribute interpolation ──────────────────────────────────────────────────

TEST(clip_near, uv_is_linearly_interpolated)
{
    // a inside: w=2, uv=(0,0).  b outside: w=0, uv=(1,0).  c outside: w=0, uv=(0,1).
    // t = (0.1 - 2.0) / (0.0 - 2.0) = 0.95
    // clipped b uv → (0.95, 0);  clipped c uv → (0, 0.95)
    ClipVert a = make_cv(2.0f, 0.0f, 0.0f);
    ClipVert b = make_cv(0.0f, 1.0f, 0.0f);
    ClipVert c = make_cv(0.0f, 0.0f, 1.0f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 1);
    ASSERT_NEAR(out[0][1].uv.x, 0.95f, 1e-5f);
    ASSERT_NEAR(out[0][1].uv.y, 0.0f, 1e-5f);
    ASSERT_NEAR(out[0][2].uv.x, 0.0f, 1e-5f);
    ASSERT_NEAR(out[0][2].uv.y, 0.95f, 1e-5f);
}

TEST(clip_near, world_pos_is_linearly_interpolated)
{
    // a inside: w=2, pos=(0,0,0). b outside: w=0, pos=(10,0,0). c outside: w=0, pos=(0,10,0).
    // t=0.95 → clipped b pos=(9.5,0,0), clipped c pos=(0,9.5,0).
    ClipVert a = make_cv(2.0f, 0, 0, 0, 0, 0);
    ClipVert b = make_cv(0.0f, 0, 0, 10, 0, 0);
    ClipVert c = make_cv(0.0f, 0, 0, 0, 10, 0);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 1);
    ASSERT_NEAR(out[0][1].pos.x, 9.5f, 1e-4f);
    ASSERT_NEAR(out[0][2].pos.y, 9.5f, 1e-4f);
}

// ─── inside vertex preservation ───────────────────────────────────────────────

TEST(clip_near, inside_vertex_is_copied_verbatim)
{
    // When a is the only inside vertex, out[0][0] must be an exact copy of a.
    ClipVert a = make_cv(5.0f, 0.3f, 0.7f, 1.0f, 2.0f, 3.0f);
    a.ao = 0.8f;
    ClipVert b = make_cv(0.0f), c = make_cv(0.0f);
    ClipVert out[2][3];
    clip_near(a, b, c, out, NEAR);
    ASSERT_NEAR(out[0][0].c.w, 5.0f, 1e-6f);
    ASSERT_NEAR(out[0][0].uv.x, 0.3f, 1e-6f);
    ASSERT_NEAR(out[0][0].uv.y, 0.7f, 1e-6f);
    ASSERT_NEAR(out[0][0].pos.x, 1.0f, 1e-6f);
    ASSERT_NEAR(out[0][0].ao, 0.8f, 1e-6f);
}

TEST(clip_near, rotation_puts_b_as_inside_vertex_first)
{
    // When b is the only inside vertex, clip_near rotates it to out[0][0].
    ClipVert a = make_cv(0.05f), b = make_cv(1.5f), c = make_cv(0.05f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 1);
    ASSERT_NEAR(out[0][0].c.w, 1.5f, 1e-6f);
}
