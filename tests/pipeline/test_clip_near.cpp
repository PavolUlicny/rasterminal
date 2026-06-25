#include "tests/test.h"
#include "src/rasterize.h"

static ClipVert
make_cv(float w, float u = 0.0f, float v_coord = 0.0f, float px = 0.0f, float py = 0.0f, float pz = 0.0f)
{
    ClipVert r{};
    r.c = { 0.0f, 0.0f, 0.0f, w };
    r.uv = { u, v_coord };
    r.pos = { px, py, pz };
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
    for (auto &t : out)
    {
        for (const ClipVert &cv : t)
        {
            ASSERT_TRUE(cv.c.w >= NEAR - 1e-5f);
        }
    }
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
    const int n = clip_near(a, b, c, out, NEAR);
    ASSERT_EQ(n, 1);
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

TEST(clip_near, rotation_puts_c_as_inside_vertex_first)
{
    // When c is the only inside vertex, clip_near rotates it to out[0][0].
    ClipVert a = make_cv(0.05f), b = make_cv(0.05f), c = make_cv(1.5f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 1);
    ASSERT_NEAR(out[0][0].c.w, 1.5f, 1e-6f);
}

TEST(clip_near, two_inside_a_outside_correct_output)
{
    // a outside, b and c inside — the !ia rotation puts b/c into aa/bb slots.
    // out[0][0]=b(w=1.0), out[0][1]=c(w=2.0); both clipped edges land at NEAR.
    ClipVert a = make_cv(0.05f), b = make_cv(1.0f), c = make_cv(2.0f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 2);
    ASSERT_NEAR(out[0][0].c.w, 1.0f, 1e-5f); // b preserved
    ASSERT_NEAR(out[0][1].c.w, 2.0f, 1e-5f); // c preserved
    ASSERT_NEAR(out[0][2].c.w, NEAR, 1e-5f); // clipped edge
    ASSERT_NEAR(out[1][2].c.w, NEAR, 1e-5f); // clipped edge
}

TEST(clip_near, two_inside_b_outside_correct_output)
{
    // a and c inside, b outside — the else rotation puts c/a into aa/bb slots.
    // out[0][0]=c(w=2.0), out[0][1]=a(w=1.0); both clipped edges land at NEAR.
    ClipVert a = make_cv(1.0f), b = make_cv(0.05f), c = make_cv(2.0f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 2);
    ASSERT_NEAR(out[0][0].c.w, 2.0f, 1e-5f); // c preserved
    ASSERT_NEAR(out[0][1].c.w, 1.0f, 1e-5f); // a preserved
    ASSERT_NEAR(out[0][2].c.w, NEAR, 1e-5f); // clipped edge
    ASSERT_NEAR(out[1][2].c.w, NEAR, 1e-5f); // clipped edge
}

TEST(clip_near, two_inside_c_outside_correct_output)
{
    // a and b inside, c outside — the !ic case performs no rotation.
    // out[0] = {a, b, cross(b,c)};  out[1] = {a, cross(b,c), cross(a,c)}.
    // Mirrors the !ia and !ib tests to ensure all three rotation branches are verified.
    ClipVert a = make_cv(1.0f), b = make_cv(2.0f), c = make_cv(0.05f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 2);
    ASSERT_NEAR(out[0][0].c.w, 1.0f, 1e-5f); // a preserved
    ASSERT_NEAR(out[0][1].c.w, 2.0f, 1e-5f); // b preserved
    ASSERT_NEAR(out[0][2].c.w, NEAR, 1e-5f); // cross(b,c) at near plane
    ASSERT_NEAR(out[1][2].c.w, NEAR, 1e-5f); // cross(a,c) at near plane
}

TEST(clip_near, two_inside_uv_interpolated_at_both_crossings)
{
    // Verifies cross_edge UV interpolation in the n=2 path (two separate crossings).
    // a inside: w=2, uv=(0,0).  b inside: w=2, uv=(1,0).  c outside: w=0, uv=(1,1).
    // t = (NEAR - 2) / (0 - 2) = (0.1 - 2) / (0 - 2) = 0.95 for both edges.
    // cross(b,c): uv = (1,0) + ((1,1)-(1,0))*0.95 = (1, 0.95)
    // cross(a,c): uv = (0,0) + ((1,1)-(0,0))*0.95 = (0.95, 0.95)
    ClipVert a = make_cv(2.0f, 0.0f, 0.0f);
    ClipVert b = make_cv(2.0f, 1.0f, 0.0f);
    ClipVert c = make_cv(0.0f, 1.0f, 1.0f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 2);
    // out[0][2] = cross(b, c)
    ASSERT_NEAR(out[0][2].uv.x, 1.0f, 1e-4f);
    ASSERT_NEAR(out[0][2].uv.y, 0.95f, 1e-4f);
    // out[1][2] = cross(a, c)
    ASSERT_NEAR(out[1][2].uv.x, 0.95f, 1e-4f);
    ASSERT_NEAR(out[1][2].uv.y, 0.95f, 1e-4f);
}

// ─── varying near_w ───────────────────────────────────────────────────────────

TEST(clip_near, alternate_near_w_clips_correctly)
{
    // near_w = 0.5: a inside (w=2.0), b/c outside (w=0.0).
    // t = (0.5 - 2.0) / (0.0 - 2.0) = 0.75 → clipped w = 0.5.
    const float NEAR2 = 0.5f;
    ClipVert a = make_cv(2.0f), b = make_cv(0.0f), c = make_cv(0.0f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR2), 1);
    ASSERT_NEAR(out[0][1].c.w, NEAR2, 1e-5f);
    ASSERT_NEAR(out[0][2].c.w, NEAR2, 1e-5f);
}

// ─── grazing clip precision ───────────────────────────────────────────────────

TEST(clip_near, grazing_clip_t_near_zero)
{
    // Inside vertex w = NEAR + 0.001 (just barely inside).
    // t = (NEAR - w_in) / (0 - w_in) ≈ 0.0099 — very close to 0.
    // Clipped vertices must still land at NEAR.
    const float w_in = NEAR + 0.001f;
    ClipVert a = make_cv(w_in), b = make_cv(0.0f), c = make_cv(0.0f);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 1);
    ASSERT_NEAR(out[0][1].c.w, NEAR, 1e-5f);
    ASSERT_NEAR(out[0][2].c.w, NEAR, 1e-5f);
}

TEST(clip_near, grazing_clip_t_near_one)
{
    // Outside vertex w = NEAR - 0.001 (just barely outside).
    // t = (NEAR - 2.0) / (w_out - 2.0) ≈ 0.9995 — very close to 1.
    // Clipped vertices must still land at NEAR.
    const float w_out = NEAR - 0.001f;
    ClipVert a = make_cv(2.0f), b = make_cv(w_out), c = make_cv(w_out);
    ClipVert out[2][3];
    ASSERT_EQ(clip_near(a, b, c, out, NEAR), 1);
    ASSERT_NEAR(out[0][1].c.w, NEAR, 1e-5f);
    ASSERT_NEAR(out[0][2].c.w, NEAR, 1e-5f);
}
