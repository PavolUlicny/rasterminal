#include "tests/gltf_test_util.h"

#include <cmath>
#include <vector>

// Group H: COLOR_0 vertex colors

TEST(gltf_valid, color0_sets_has_vertex_colors_and_loads_rgb)
{
    // COLOR_0 VEC4 float: red/green/blue per vertex.
    // has_vertex_colors=true; alpha channel discarded, only RGB stored.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0,\"COLOR_0\":1}}]}],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0},"
                             "{\"buffer\":0,\"byteLength\":48,\"byteOffset\":36}"
                             "],"
                             "\"buffers\":[{\"byteLength\":84}]}";
    std::string bin;
    emit_tri_verts(bin);
    // vertex 0: red, vertex 1: green, vertex 2: blue  (alpha=1 discarded)
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 1.0f);
    TmpFile f(tmp_path("rast_color0.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), static_cast<size_t>(3));
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].y, 0.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[1].y, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[2].z, 1.0f, 1e-5f);
}

TEST(gltf_valid, partial_color0_split_inherits_color)
{
    // P0 has COLOR_0 and a crease split; following P1 has no colors or normals.
    // Split copies must inherit color and keep vertex_colors parallel to vertices.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0,\"COLOR_0\":1},\"indices\":2},"
        "{\"attributes\":{\"POSITION\":3}}"
        "]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,1]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC4\"},"
        "{\"bufferView\":2,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[5,5,5],\"max\":[6,6,5]}"
        "],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteLength\":48,\"byteOffset\":0},"
        "{\"buffer\":0,\"byteLength\":64,\"byteOffset\":48},"
        "{\"buffer\":0,\"byteLength\":12,\"byteOffset\":112},"
        "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":124}"
        "],"
        "\"buffers\":[{\"byteLength\":160}]}";
    std::string bin;
    // P0 positions: origin, +X, +Y, +Z (two tris fold 90 deg across edge 0-1)
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    // P0 colors VEC4: vertex 0 red, 1 green, 2 blue, 3 yellow (alpha discarded)
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    // P0 indices: tri A (0,1,2), tri B (1,0,3), both contain the origin vertex 0
    emit_u16_le(bin, 0);
    emit_u16_le(bin, 1);
    emit_u16_le(bin, 2);
    emit_u16_le(bin, 1);
    emit_u16_le(bin, 0);
    emit_u16_le(bin, 3);
    // P1 positions: a far, uncolored triangle
    emit_f32_le(bin, 5.0f);
    emit_f32_le(bin, 5.0f);
    emit_f32_le(bin, 5.0f);
    emit_f32_le(bin, 6.0f);
    emit_f32_le(bin, 5.0f);
    emit_f32_le(bin, 5.0f);
    emit_f32_le(bin, 5.0f);
    emit_f32_le(bin, 6.0f);
    emit_f32_le(bin, 5.0f);
    TmpFile f(tmp_path("rast_partial_color0.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), m.vertices.size()); // parallel array stays in sync
    int origin_count = 0;
    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        const Vertex &v = m.vertices[i];
        if (v.pos.x == 0.0f && v.pos.y == 0.0f && v.pos.z == 0.0f)
        {
            origin_count++;
            ASSERT_NEAR(m.vertex_colors[i].x, 1.0f, 1e-5f); // red, not white
            ASSERT_NEAR(m.vertex_colors[i].y, 0.0f, 1e-5f);
            ASSERT_NEAR(m.vertex_colors[i].z, 0.0f, 1e-5f);
        }
    }
    ASSERT_EQ(origin_count, 2); // origin vertex split across the hard edge
}

TEST(gltf_valid, partial_color0_uncolored_primitive_first)
{
    // Put the uncolored primitive first. Absolute resize must backfill its leading
    // gap with white without shifting the following primitive's colors.
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0}},"
        "{\"attributes\":{\"POSITION\":1,\"COLOR_0\":2}}"
        "]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[10,10,10],\"max\":[11,11,10]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}"
        "],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":0},"
        "{\"buffer\":0,\"byteLength\":36,\"byteOffset\":36},"
        "{\"buffer\":0,\"byteLength\":48,\"byteOffset\":72}"
        "],"
        "\"buffers\":[{\"byteLength\":120}]}";
    std::string bin;
    // P0 (uncolored) positions: a far triangle at ~(10,10,10)
    emit_f32_le(bin, 10.0f);
    emit_f32_le(bin, 10.0f);
    emit_f32_le(bin, 10.0f);
    emit_f32_le(bin, 11.0f);
    emit_f32_le(bin, 10.0f);
    emit_f32_le(bin, 10.0f);
    emit_f32_le(bin, 10.0f);
    emit_f32_le(bin, 11.0f);
    emit_f32_le(bin, 10.0f);
    // P1 (colored) positions: triangle at the origin
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    // P1 colors VEC4: all green (alpha discarded)
    for (int i = 0; i < 3; i++)
    {
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 1.0f);
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 1.0f);
    }
    TmpFile f(tmp_path("rast_partial_color0_rev.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), m.vertices.size());
    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        const Vertex &v = m.vertices[i];
        const vec3 &c = m.vertex_colors[i];
        if (v.pos.z == 0.0f) // P1 (origin triangle) → green
        {
            ASSERT_NEAR(c.x, 0.0f, 1e-5f);
            ASSERT_NEAR(c.y, 1.0f, 1e-5f);
            ASSERT_NEAR(c.z, 0.0f, 1e-5f);
        }
        else // P0 (far triangle, z=10) → white back-fill
        {
            ASSERT_NEAR(c.x, 1.0f, 1e-5f);
            ASSERT_NEAR(c.y, 1.0f, 1e-5f);
            ASSERT_NEAR(c.z, 1.0f, 1e-5f);
        }
    }
}

// Group P: vertex_colors white-fill on second primitive

TEST(gltf_valid, second_primitive_color0_white_fills_first_primitive_verts)
{
    // The second primitive introduces COLOR_0 at vert_base 3. Resize must backfill
    // the first primitive white, then write red/green/blue at indices 3..5.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0}},"
                             "{\"attributes\":{\"POSITION\":1,\"COLOR_0\":2}}"
                             "]}],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":48}"
                             "],"
                             "\"buffers\":[{\"byteLength\":120}]}";

    std::string bin;
    emit_tri_verts(bin); // prim 0 positions (36 bytes)
    emit_tri_verts(bin); // prim 1 positions (36 bytes)
    // prim 1 COLOR_0: red, green, blue (VEC4 FLOAT, alpha discarded)
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 1.0f);

    TmpFile f(tmp_path("rast_vcol_whitefill.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    ASSERT_EQ(m.vertex_colors.size(), static_cast<size_t>(6));
    // Prim 0 vertices: white-filled because prim 0 had no COLOR_0.
    ASSERT_NEAR(m.vertex_colors[0].x, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].y, 1.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[0].z, 1.0f, 1e-5f);
    // Prim 1 vertices: actual COLOR_0 values.
    ASSERT_NEAR(m.vertex_colors[3].x, 1.0f, 1e-5f); // red
    ASSERT_NEAR(m.vertex_colors[3].y, 0.0f, 1e-5f);
    ASSERT_NEAR(m.vertex_colors[4].y, 1.0f, 1e-5f); // green
    ASSERT_NEAR(m.vertex_colors[5].z, 1.0f, 1e-5f); // blue
}

// Group T: first primitive COLOR_0, second has no color

TEST(gltf_valid, first_primitive_color0_second_has_no_color)
{
    // Prim 0: COLOR_0 (red/green/blue). Prim 1: no COLOR_0.
    // vertex_colors filled for prim 0 only (size=3); vertices.size()=6.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0,\"COLOR_0\":2}},"
                             "{\"attributes\":{\"POSITION\":1}}"
                             "]}],"
                             "\"accessors\":["
                             "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]},"
                             "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}"
                             "],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":48}"
                             "],"
                             "\"buffers\":[{\"byteLength\":120}]}";
    std::string bin;
    emit_tri_verts(bin); // prim 0 positions
    emit_tri_verts(bin); // prim 1 positions
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 1.0f);
    TmpFile f(tmp_path("rast_vcol_fwd.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_vertex_colors);
    // optimize_vertex_cache pads partially-initialised vertex_colors to vertices.size()
    // and then remaps, so size == vertices.size() and order may change.
    ASSERT_EQ(m.vertex_colors.size(), m.vertices.size());
    bool found_red = false, found_green = false, found_blue = false;
    for (const auto &c : m.vertex_colors)
    {
        if (c.x > 0.9f && c.y < 0.1f && c.z < 0.1f)
        {
            found_red = true;
        }
        if (c.x < 0.1f && c.y > 0.9f && c.z < 0.1f)
        {
            found_green = true;
        }
        if (c.x < 0.1f && c.y < 0.1f && c.z > 0.9f)
        {
            found_blue = true;
        }
    }
    ASSERT_TRUE(found_red);
    ASSERT_TRUE(found_green);
    ASSERT_TRUE(found_blue);
}
