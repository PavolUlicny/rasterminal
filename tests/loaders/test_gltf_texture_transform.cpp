#include "tests/inline_bmp.h"
#include "tests/gltf_test_util.h"

#include <cmath>
#include <vector>

// KHR_texture_transform

// baseColorTexture with KHR_texture_transform (offset + scale, no rotation) → the slot bakes
// the flip-folded 2x3 affine. With rotation 0: c=1,s=0, so for offset (ox,oy) and scale
// (sx,sy) the coefficients are {sx,0,ox, 0,sy,1-oy-sy} — exercising the v-flip terms (a02 has
// no rotation contribution here, a12 = 1 - oy - sy carries the fold).
TEST(gltf_valid, texture_transform_bakes_affine)
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
        "\"extensionsUsed\":[\"KHR_texture_transform\"],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{\"offset\":[0.1,0.2],\"scale\":[2.0,3.0]}}}}}],"
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

    TmpFile f(tmp_path("rast_tex_transform.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const TexSlot &d = m.materials[1].diffuse_map;
    ASSERT_TRUE(d.has_transform);
    const float expect[6] = { 2.0f, 0.0f, 0.1f, 0.0f, 3.0f, 1.0f - 0.2f - 3.0f };
    for (int i = 0; i < 6; i++)
    {
        if (std::fabs(d.t[i] - expect[i]) > 1e-5f)
        {
            ASSERT_FAIL("baked KHR_texture_transform affine coefficient mismatch");
        }
    }
}

// KHR_texture_transform's own texCoord overrides textureInfo.texCoord (spec). baseColorTexture
// declares texCoord 0 but the transform declares texCoord 1, and TEXCOORD_1 is present → the
// slot resolves to set 1.
TEST(gltf_valid, texture_transform_texcoord_override)
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
        "\"extensionsUsed\":[\"KHR_texture_transform\"],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"texCoord\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{\"texCoord\":1}}}}}],"
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

    TmpFile f(tmp_path("rast_tex_transform_override.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.has_uv1);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].diffuse_map.has_transform);
    ASSERT_EQ(static_cast<int>(m.materials[1].diffuse_map.uv_set), 1);
}

// A baseColorTexture with no KHR_texture_transform → has_transform false and t stays identity,
// so apply_tex_transform is a no-op even if (incorrectly) invoked.
TEST(gltf_valid, texture_transform_absent_is_identity)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size);

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
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

    TmpFile f(tmp_path("rast_tex_transform_absent.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const TexSlot &d = m.materials[1].diffuse_map;
    ASSERT_FALSE(d.has_transform);
    const float identity[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    for (int i = 0; i < 6; i++)
    {
        if (std::fabs(d.t[i] - identity[i]) > 1e-6f)
        {
            ASSERT_FAIL("no-transform slot must keep an identity affine");
        }
    }
}

// Keystone correctness test for the v-flip handling, independent of bake_transform's own
// derivation. Author a non-trivial offset+rotation+scale, load it (running the real bake), then
// for several UV points assert that our full pipeline — store v-flip (uv.y = 1 - v_gltf) → baked
// affine → the sampler's internal v-flip — lands on the EXACT image pixel the glTF transform with
// NEGATED rotation (Tr·R(-θ)·S, the flipY convention for v-flipped textures) intends. The expected
// value encodes that spec-with-negated-rotation directly; the implementation reaches it by a
// different route, so a wrong rotation sign (or any sign slip in the affine) makes expected !=
// actual. This is the check that catches a bad derivation — the exact-coefficient test above only
// pins the formula against itself, and would happily accept the un-negated (red-marker) bake.
TEST(gltf_valid, texture_transform_matches_gltf_spec_sampling)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // POSITION  36 @ 0
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);                  // TEXCOORD_0 24 @ 36
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // BMP 58 @ 60

    const float ox = 0.1f, oy = 0.2f, rot = 0.5f, sx = 1.5f, sy = 2.0f;
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1},\"material\":0}]}],"
        "\"extensionsUsed\":[\"KHR_texture_transform\"],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{\"offset\":[0.1,0.2],\"rotation\":0.5,\"scale\":[1.5,2.0]}}}}}],"
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

    TmpFile f(tmp_path("rast_tex_transform_spec.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const TexSlot &d = m.materials[1].diffuse_map;
    ASSERT_TRUE(d.has_transform);

    const float c = std::cos(rot), s = std::sin(rot);
    const vec2 pts[] = { { 0.0f, 0.0f }, { 0.3f, 0.2f }, { 1.0f, 1.0f }, { 0.7f, 0.4f } };
    for (const vec2 &p : pts)
    {
        const float u_g = p.x, v_g = p.y;
        // The image texel our pipeline reads (sampler does v = 1 - feed.y, so the v-down sampling
        // coordinate is (feed.x, 1 - feed.y)) must equal the glTF transform with the rotation angle
        // NEGATED: Tr · R(-θ) · S · g. That -θ is the flipY convention every glTF reference renderer
        // uses for v-flipped textures; the naive +θ (R(+θ)) points the TextureTransformTest arrow at
        // the red "opposite direction" marker. Only the sin terms flip sign, so offset and
        // axis-aligned scale are identical either way — this isolates the rotation handedness.
        const float u_exp = (c * sx * u_g) + (s * sy * v_g) + ox;
        const float v_exp = (-s * sx * u_g) + (c * sy * v_g) + oy;
        const vec2 feed = apply_tex_transform(d, vec2{ u_g, 1.0f - v_g });
        const float u_act = feed.x;
        const float v_act = 1.0f - feed.y;
        if (std::fabs(u_act - u_exp) > 1e-4f || std::fabs(v_act - v_exp) > 1e-4f)
        {
            ASSERT_FAIL("baked transform does not reproduce glTF Tr·R(-rot)·S sampling");
        }
    }
}

// End-to-end rotation-handedness guard. The keystone above only checks apply_tex_transform's
// coordinates against a 1x1 texture; it cannot see which way a rotation actually turns the image.
// This drives the REAL pipeline: load a pure +rotation (real bake_transform) → apply the baked
// affine → sample a real multi-texel texture through the REAL Texture::sample, and assert the
// rotated brightness gradient runs the direction the official Khronos TextureTransformTest render
// shows. A regression that drops the v-flip rotation negation flips the cross term (t[3]) sign and
// reverses this inequality — the exact failure that pointed the arrow at the "opposite direction"
// marker. Robust by construction: a vertical gradient + an inequality assertion (no bilinear/wrap
// edge fragility), and the sampled colours come from this test's own texture, not the loaded glTF's.
TEST(gltf_valid, texture_transform_rotation_handedness_end_to_end)
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
        "\"extensionsUsed\":[\"KHR_texture_transform\"],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{\"rotation\":0.5}}}}}],"
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

    TmpFile f(tmp_path("rast_tex_transform_handed.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const TexSlot &d = m.materials[1].diffuse_map;
    ASSERT_TRUE(d.has_transform);

    // Vertical gradient: top row black, bottom row white. Sampling depends only on the feed's v.
    Texture grad;
    grad.width = 1;
    grad.height = 2;
    grad.pixels = { 0, 0, 0, 255, 255, 255, 255, 255 };

    // Two stored UVs differing only in u. Under the authored +rotation the cross term t[3] couples
    // u into the sampled row, so the brightness must change monotonically with u. Both feeds land
    // inside [0,1] (no wrap), so the only thing under test is the rotation's direction.
    auto brightness = [&](float su)
    {
        const vec2 feed = apply_tex_transform(d, vec2{ su, 0.5f });
        return grad.sample_rgb(feed.x, feed.y).x;
    };
    const float lo_u = brightness(0.2f);
    const float hi_u = brightness(0.8f);

    // Khronos-correct sense (t[3] = +sin θ): larger u samples a lower image row → darker. The
    // pre-fix bug negated t[3], making larger u brighter. Margin guards against noise, not sign.
    if (hi_u >= lo_u - 0.15f)
    {
        ASSERT_FAIL("rotation handedness reversed: +rotation must darken with increasing u "
                    "(v-flip rotation negation dropped — see bake_transform)");
    }
}

// A transform that specifies only offset → cgltf defaults scale to [1,1] (not [0,0]), so the
// texture is shifted, not collapsed. Guards the cgltf default our bake depends on across bumps.
TEST(gltf_valid, texture_transform_defaults_scale_to_one)
{
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);
    emit_uvs(bin, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size);

    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
        "{\"POSITION\":0,\"TEXCOORD_0\":1},\"material\":0}]}],"
        "\"extensionsUsed\":[\"KHR_texture_transform\"],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,"
        "\"extensions\":{\"KHR_texture_transform\":{\"offset\":[0.25,0.0]}}}}}],"
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

    TmpFile f(tmp_path("rast_tex_transform_defscale.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const TexSlot &d = m.materials[1].diffuse_map;
    ASSERT_TRUE(d.has_transform);
    // scale 1, rotation 0, offset.u 0.25 → {1,0,0.25, 0,1,0} (a12 = 1 - 0 - 1 = 0).
    const float expect[6] = { 1.0f, 0.0f, 0.25f, 0.0f, 1.0f, 0.0f };
    for (int i = 0; i < 6; i++)
    {
        if (std::fabs(d.t[i] - expect[i]) > 1e-5f)
        {
            ASSERT_FAIL("offset-only transform must keep unit scale (cgltf default), not collapse");
        }
    }
}
