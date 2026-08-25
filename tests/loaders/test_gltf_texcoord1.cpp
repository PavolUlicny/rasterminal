#include "tests/inline_bmp.h"
#include "tests/gltf_test_util.h"

#include <cmath>
#include <vector>

// A baseColorTexture bound to texCoord:1 with a TEXCOORD_1 accessor present →
// has_uv1 set, uv1 length-matched, the binding records uv_set 1, and the stored
// uv1 carries the real (v-flipped) second-set values rather than a uv0 copy.
TEST(gltf_valid, texcoord1_referenced_populates_uv1)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // POSITION  36 @ 0
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);                  // TEXCOORD_0 24 @ 36
    emit_uvs(bin, 0.2f, 0.3f, 0.8f, 0.3f, 0.5f, 0.9f);                  // TEXCOORD_1 24 @ 60
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 84

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1,\"TEXCOORD_1\":2},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":1}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":3,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":84,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":142}]}";

    TmpFile f(tmp_path("rast_texcoord1.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_uv1);
    ASSERT_EQ(m.uv1.size(), m.vertices.size());
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(static_cast<int>(m.materials[1].diffuse_map.uv_set), 1);
    // The single triangle is not reordered (optimize_vertex_cache skips <2 tris, no creasing),
    // so vertex 0 keeps its input attributes. uv1 input (0.2,0.3) is stored v-flipped → (0.2,0.7),
    // distinct from uv0 (0.0, flipped 1.0), proving the real second set was read, not a uv0 copy.
    ASSERT_TRUE(std::fabs(m.uv1[0].x - 0.2f) < 0.01f);
    ASSERT_TRUE(std::fabs(m.uv1[0].y - 0.7f) < 0.01f);
}

// TEXCOORD_1 present but no texture references it (texCoord defaults to 0) → uv1 is
// dropped (read-then-drop) and has_uv1 stays false; the binding records set 0.
TEST(gltf_valid, texcoord1_present_but_unreferenced_is_dropped)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
    emit_uvs(bin, 0.2f, 0.3f, 0.8f, 0.3f, 0.5f, 0.9f);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size);

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1,\"TEXCOORD_1\":2},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":3,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":84,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":142}]}";

    TmpFile f(tmp_path("rast_texcoord1_unused.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.has_uv1);
    ASSERT_TRUE(m.uv1.empty());
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(static_cast<int>(m.materials[1].diffuse_map.uv_set), 0);
}

// texCoord:1 referenced but the primitive provides no TEXCOORD_1 → degrade to set 0
// (runtime-loader convention): load still succeeds, has_uv1 false, uv_set forced to 0.
TEST(gltf_valid, texcoord1_referenced_but_absent_degrades_to_zero)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // POSITION  36 @ 0
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);                  // TEXCOORD_0 24 @ 36
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 60

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":1}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":2,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":118}]}";

    TmpFile f(tmp_path("rast_texcoord1_absent.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.has_uv1);
    ASSERT_TRUE(m.uv1.empty());
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(static_cast<int>(m.materials[1].diffuse_map.uv_set), 0);
}

// texCoord:2 (>= 2) is unsupported and capped to set 0, even with TEXCOORD_1 present.
// Nothing then references set 1, so uv1 is dropped and the binding records set 0.
TEST(gltf_valid, texcoord_ge_2_caps_to_zero)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
    emit_uvs(bin, 0.2f, 0.3f, 0.8f, 0.3f, 0.5f, 0.9f);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size);

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1,\"TEXCOORD_1\":2},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":2}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":3,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":84,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":142}]}";

    TmpFile f(tmp_path("rast_texcoord2.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_FALSE(m.has_uv1);
    ASSERT_TRUE(m.uv1.empty());
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(static_cast<int>(m.materials[1].diffuse_map.uv_set), 0);
}

// A two-triangle quad triggers cache optimization. UV1 must follow its vertex through remapping.
TEST(gltf_valid, texcoord1_survives_vertex_cache_remap)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    // 4 positions (quad corners): VEC3 ×4 = 48 bytes @ 0
    const float quad[4][3] = { { -1, -1, 0 }, { 1, -1, 0 }, { 1, 1, 0 }, { -1, 1, 0 } };
    for (const auto &p : quad)
    {
        emit_f32_le(bin, p[0]);
        emit_f32_le(bin, p[1]);
        emit_f32_le(bin, p[2]);
    }
    // uv0 ×4 = 32 bytes @ 48
    const float uv0[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (const auto &t : uv0)
    {
        emit_f32_le(bin, t[0]);
        emit_f32_le(bin, t[1]);
    }
    // uv1 ×4 = 32 bytes @ 80 (distinct from uv0 so a mix-up is visible)
    const float uv1[4][2] = { { 0.25f, 0.0f }, { 0.75f, 0.0f }, { 0.75f, 0.5f }, { 0.25f, 0.5f } };
    for (const auto &t : uv1)
    {
        emit_f32_le(bin, t[0]);
        emit_f32_le(bin, t[1]);
    }
    // indices: two tris (0,1,2)(0,2,3), 6 × uint16 = 12 bytes @ 112
    for (int i : { 0, 1, 2, 0, 2, 3 })
    {
        emit_u16_le(bin, static_cast<uint16_t>(i));
    }
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // 58 @ 124

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1,\"TEXCOORD_1\":2},\"indices\":3,\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":1}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":32},"
        "{\"buffer\":0,\"byteOffset\":80,\"byteLength\":32},"
        "{\"buffer\":0,\"byteOffset\":112,\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":124,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":182}]}";

    TmpFile f(tmp_path("rast_texcoord1_quad.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_uv1);
    ASSERT_EQ(m.uv1.size(), m.vertices.size());
    // Find the corner at (1,-1,0): its uv1 was file (0.75,0.0) → stored v-flipped (0.75,1.0).
    bool found = false;
    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        const vec3 &p = m.vertices[i].pos;
        if (std::fabs(p.x - 1.0f) < 0.01f && std::fabs(p.y + 1.0f) < 0.01f)
        {
            ASSERT_TRUE(std::fabs(m.uv1[i].x - 0.75f) < 0.01f);
            ASSERT_TRUE(std::fabs(m.uv1[i].y - 1.0f) < 0.01f);
            found = true;
        }
    }
    ASSERT_TRUE(found);
}

// Only the middle of three primitives has TEXCOORD_1. Lazy creation must backfill
// and pad the others from uv0 while preserving the middle's distinct values.
TEST(gltf_valid, texcoord1_partial_coverage_backfills_and_pads)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // prim0 POS  36 @ 0
    emit_uvs(bin, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f);                  // prim0 uv0  24 @ 36
    emit_tri_verts(bin);                                                // prim1 POS  36 @ 60
    emit_uvs(bin, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f);                  // prim1 uv0  24 @ 96
    emit_uvs(bin, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f);                  // prim1 uv1  24 @ 120
    emit_tri_verts(bin);                                                // prim2 POS  36 @ 144
    emit_uvs(bin, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f);                  // prim2 uv0  24 @ 180
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 204

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},\"material\":0},"
        "{\"attributes\":{\"POSITION\":2,\"TEXCOORD_0\":3,\"TEXCOORD_1\":4},\"material\":0},"
        "{\"attributes\":{\"POSITION\":5,\"TEXCOORD_0\":6},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":1}}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":7,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":5,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]},"
        "{\"bufferView\":6,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":60,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":120,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":144,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":180,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":204,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":262}]}";

    TmpFile f(tmp_path("rast_texcoord1_partial.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_uv1);
    ASSERT_EQ(m.uv1.size(), m.vertices.size());
    // 9 vertices: 6 degrade (uv1 == own uv0), 3 carry the real second set (uv1 != uv0).
    int n_equal = 0;
    int n_diff = 0;
    for (size_t i = 0; i < m.vertices.size(); i++)
    {
        const vec2 d{ m.uv1[i].x - m.vertices[i].uv.x, m.uv1[i].y - m.vertices[i].uv.y };
        if (std::fabs(d.x) < 0.01f && std::fabs(d.y) < 0.01f)
        {
            n_equal++;
        }
        else
        {
            n_diff++;
        }
    }
    ASSERT_EQ(n_equal, 6);
    ASSERT_EQ(n_diff, 3);
}

// Tangents must be built from the UV set the normal map samples (glTF spec). Geometry: a
// triangle in the XY plane (normal +Z) with uv0 running U along +X but uv1 running U along
// +Y. With the normalTexture bound to texCoord:1, compute_tangents must use uv1, yielding a
// +Y-dominant tangent (a uv0-derived tangent would be +X-dominant).
TEST(gltf_valid, normal_map_texcoord1_drives_tangents)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    // POSITION (0,0,0)(1,0,0)(0,1,0), 36 @ 0
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 0.0f);
    emit_f32_le(bin, 1.0f);
    emit_f32_le(bin, 0.0f);
    // NORMAL (0,0,1)×3, 36 @ 36
    for (int i = 0; i < 3; i++)
    {
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 0.0f);
        emit_f32_le(bin, 1.0f);
    }
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);                  // uv0: U along +X, 24 @ 72
    emit_uvs(bin, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f);                  // uv1: U along +Y, 24 @ 96
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 120

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2,\"TEXCOORD_1\":3},\"material\":0}]}],"
        "\"materials\":[{\"normalTexture\":{\"index\":0,\"texCoord\":1}}],"
        "\"textures\":[{\"source\":0}],"
        "\"images\":[{\"bufferView\":4,\"mimeType\":\"image/bmp\"}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":120,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":178}]}";

    TmpFile f(tmp_path("rast_texcoord1_tangent.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_uv1);
    ASSERT_EQ(static_cast<int>(m.materials[1].normal_map.uv_set), 1);
    ASSERT_EQ(m.tangents.size(), m.vertices.size());
    // uv1-derived tangent is +Y; a uv0-derived one would be +X. Assert +Y dominance.
    for (const vec3 &t : m.tangents)
    {
        if (std::fabs(t.y) <= std::fabs(t.x))
        {
            ASSERT_FAIL("tangent should be +Y-dominant (built from TEXCOORD_1), got X-dominant");
        }
    }
}
