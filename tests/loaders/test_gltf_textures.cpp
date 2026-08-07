#include "tests/inline_bmp.h"
#include "tests/gltf_test_util.h"

#include <cmath>
#include <vector>

// Group N: load_tex path coverage

TEST(gltf_valid, normal_tex_via_buffer_view_sets_normal_tex_index)
{
    // image[0] has bufferView=1 (4 garbage bytes) and no URI.
    // load_tex takes the else-if(img->buffer_view) branch; load_from_memory fails
    // on the garbage data → load_tex returns -1 → mat.normal_map.tex = -1.
    // Covers both the buffer_view branch in load_tex AND the mat.normal_map.tex
    // assignment in map_mat.  Mesh geometry still loads.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"normalTexture\":{\"index\":0}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":4}"
                             "],"
                             "\"buffers\":[{\"byteLength\":40}]}";
    std::string bin;
    emit_tri_verts(bin); // 36 bytes geometry
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0); // 4 bytes garbage image data

    TmpFile f(tmp_path("rast_nmap_bv.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].normal_map.tex < 0);
}

TEST(gltf_valid, zero_emissive_factor_with_texture_skips_decode)
{
    // Spec-literal: emissive = factor × texture. A zero factor means no fragment can sample
    // the texture (do_emissive is gated on factor>0), so the loader skips load_tex entirely.
    // Saves a stb_image_load + permanent RAM footprint for a texture nothing will read.
    // Uses a valid 1×1 BMP so the texture WOULD have decoded successfully if we'd asked.
    std::string bin;
    emit_tri_verts(bin); // 36 bytes geometry
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));
    const auto img_len = static_cast<uint32_t>(sizeof(k1x1_red_bmp));

    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                       "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                       "{\"POSITION\":0},\"material\":0}]}],"
                       "\"materials\":[{\"emissiveTexture\":{\"index\":0}}]," // no emissiveFactor → default {0,0,0}
                       "\"textures\":[{\"source\":0}],"
                       "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                       "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                       "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                       "\"bufferViews\":["
                       "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                       "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":" +
                       std::to_string(img_len) +
                       "}"
                       "],"
                       "\"buffers\":[{\"byteLength\":" +
                       std::to_string(36u + img_len) + "}]}";

    TmpFile f(tmp_path("rast_emissive_skip_decode.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].emissive_map.tex, -1);
    ASSERT_TRUE(m.textures.empty());
    ASSERT_FALSE(m.has_emissive);
}

// Group N+: glTF normalTexture.scale parsing + has_normal_scale gate

TEST(gltf_valid, normal_scale_read_and_sets_has_flag)
{
    // normalTexture.scale=0.5 with a valid embedded BMP → normal_tex >= 0 and the
    // scale is read into mat.normal_scale; the != 1.0 + normal_tex>=0 predicate
    // arms Mesh::has_normal_scale.
    std::string bin;
    emit_tri_verts(bin); // 36 bytes geometry at offset 0
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"normalTexture\":{\"index\":0,\"scale\":0.5}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_nscale.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].normal_scale, 0.5f, 1e-4f);
    ASSERT_TRUE(m.materials[1].normal_map.tex >= 0);
    ASSERT_TRUE(m.has_normal_scale);
}

TEST(gltf_valid, normal_scale_default_one_no_has_flag)
{
    // normalTexture without an explicit scale → cgltf default 1.0 → mat.normal_scale
    // stays 1.0 and has_normal_scale stays false (predicate filters scale != 1.0).
    std::string bin;
    emit_tri_verts(bin);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"normalTexture\":{\"index\":0}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_nscale_default.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].normal_scale, 1.0f, 1e-4f);
    ASSERT_FALSE(m.has_normal_scale);
}

TEST(gltf_valid, normal_scale_with_failed_decode_no_has_flag)
{
    // scale=0.5 is parsed (assignment lives inside if(normal_texture.texture),
    // which is truthy whenever the JSON binds a texture — independent of decode),
    // but the 4 garbage image bytes make load_tex return -1. The has_normal_scale
    // predicate's normal_tex>=0 guard must keep the gate disarmed despite the
    // non-unit scale, so a failed normal-map decode never costs the per-pixel path.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"normalTexture\":{\"index\":0,\"scale\":0.5}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":4}"
                             "],"
                             "\"buffers\":[{\"byteLength\":40}]}";
    std::string bin;
    emit_tri_verts(bin);
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0); // 4 bytes garbage image data

    TmpFile f(tmp_path("rast_nscale_faildecode.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].normal_scale, 0.5f, 1e-4f);
    ASSERT_TRUE(m.materials[1].normal_map.tex < 0);
    ASSERT_FALSE(m.has_normal_scale);
}

// glTF occlusionTexture parsing + has_occlusion gate

TEST(gltf_valid, occlusion_strength_read_and_sets_has_flag)
{
    // occlusionTexture.strength=0.5 with a valid embedded BMP → occlusion_tex >= 0,
    // strength read into mat.occlusion_strength, and Mesh::has_occlusion armed
    // (the gate is occlusion_tex>=0 only — strength does not factor in).
    std::string bin;
    emit_tri_verts(bin);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"occlusionTexture\":{\"index\":0,\"strength\":0.5}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_occl.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].occlusion_strength, 0.5f, 1e-4f);
    ASSERT_TRUE(m.materials[1].occlusion_map.tex >= 0);
    ASSERT_TRUE(m.has_occlusion);
}

TEST(gltf_valid, occlusion_default_strength_one_still_sets_flag)
{
    // occlusionTexture without an explicit strength → cgltf default 1.0. Unlike
    // normalScale, has_occlusion gates on occlusion_tex>=0 alone, so the flag still
    // arms at default strength.
    std::string bin;
    emit_tri_verts(bin);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"occlusionTexture\":{\"index\":0}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_occl_default.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].occlusion_strength, 1.0f, 1e-4f);
    ASSERT_TRUE(m.materials[1].occlusion_map.tex >= 0);
    ASSERT_TRUE(m.has_occlusion);
}

TEST(gltf_valid, occlusion_failed_decode_no_has_flag)
{
    // strength=0.5 is parsed (assignment lives inside if(occlusion_texture.texture)),
    // but 4 garbage image bytes make load_tex return -1. The occlusion_tex>=0 gate
    // must keep has_occlusion disarmed so a failed decode never costs the per-pixel path.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"occlusionTexture\":{\"index\":0,\"strength\":0.5}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":4}"
                             "],"
                             "\"buffers\":[{\"byteLength\":40}]}";
    std::string bin;
    emit_tri_verts(bin);
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0);

    TmpFile f(tmp_path("rast_occl_faildecode.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].occlusion_strength, 0.5f, 1e-4f);
    ASSERT_TRUE(m.materials[1].occlusion_map.tex < 0);
    ASSERT_FALSE(m.has_occlusion);
}

TEST(gltf_valid, no_occlusion_texture_keeps_flag_false)
{
    // A material with no occlusionTexture → occlusion_tex stays -1, strength stays
    // 1.0, has_occlusion stays false.
    std::string bin;
    emit_tri_verts(bin);

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";

    TmpFile f(tmp_path("rast_occl_none.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].occlusion_map.tex, -1);
    ASSERT_NEAR(m.materials[1].occlusion_strength, 1.0f, 1e-4f);
    ASSERT_FALSE(m.has_occlusion);
}

TEST(gltf_valid, occlusion_strength_above_one_clamped)
{
    // glTF caps occlusionTexture.strength at 1.0; cgltf does not enforce. An out-of-range
    // strength=2.0 must clamp to 1.0 at load so it cannot drive the per-pixel ao negative.
    std::string bin;
    emit_tri_verts(bin);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"occlusionTexture\":{\"index\":0,\"strength\":2.0}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_occl_clamp.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_NEAR(m.materials[1].occlusion_strength, 1.0f, 1e-4f);
}

TEST(gltf_valid, diffuse_tex_via_external_uri_load_failure_sets_diffuse_tex_neg)
{
    // image[0] has uri="does_not_exist.tga" (no bufferView).
    // load_tex takes the URI branch; tex.load(dir + uri) fails because the file
    // does not exist → load_tex returns -1 → mat.diffuse_map.tex = -1.
    // Covers the if(img->uri) branch in load_tex.  Mesh geometry still loads.
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"uri\":\"does_not_exist.tga\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);

    TmpFile f(tmp_path("rast_tex_uri.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].diffuse_map.tex < 0);
}

// Group V: embedded texture load success

TEST(gltf_valid, diffuse_tex_via_embedded_buffer_view_loads_successfully)
{
    // image[0] has bufferView=1 pointing to a valid 1×1 BMP embedded in the GLB
    // binary buffer.  load_tex takes the buffer_view branch and load_from_memory
    // succeeds → ok=true → lines 62-64 in mesh_gltf.cpp (store idx, push texture,
    // return idx) are executed.  mat.diffuse_map.tex is set to a valid (≥0) index.
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin); // 36 bytes geometry at offset 0
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size);

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_embed_tex.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(1));
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_TRUE(m.materials[1].diffuse_map.tex >= 0);
    ASSERT_FALSE(m.textures.empty());
}

// Group V+: shared image deduplicates texture

TEST(gltf_valid, shared_image_deduplicates_texture)
{
    // Two primitives both reference material 0, whose baseColorTexture resolves
    // to image 0 (a valid embedded BMP).  map_mat runs once per primitive, so
    // load_tex is invoked twice with the same cgltf_image; dedup must collapse
    // them into a single texture slot rather than decoding twice.
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin); // 36 bytes geometry at offset 0
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size);

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_tex_dedup.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.triangles.size(), static_cast<size_t>(2));
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    // Every material that carries a diffuse texture points at the single slot 0.
    bool found = false;
    for (const auto &mat : m.materials)
    {
        if (mat.diffuse_map.tex >= 0)
        {
            ASSERT_EQ(mat.diffuse_map.tex, 0);
            found = true;
        }
    }
    ASSERT_TRUE(found);
}

// Group V++: parallel texture decode (n_threads > 1)

TEST(gltf_valid, parallel_decode_two_distinct_textures)
{
    // Two images (two bufferViews over the same BMP bytes → two distinct
    // cgltf_image), two materials, two primitives. Loaded with n_threads=4 so the
    // two decodes run on the parallel path. Both must land in distinct valid slots.
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // 36 bytes at offset 0
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // 58 bytes at offset 36

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":0},\"material\":1}]}],"
                             "\"materials\":["
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}},"
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":1}}}],"
                             "\"textures\":[{\"source\":0},{\"source\":1}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"},"
                             "{\"bufferView\":2,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}"
                             "],"
                             "\"buffers\":[{\"byteLength\":94}]}";

    TmpFile f(tmp_path("rast_par_tex.glb"), make_glb(json, bin));
    Mesh m;
    const bool ok = m.load_model(f.path, /*ao=*/false, /*n_threads=*/4);
    ASSERT_TRUE(ok);
    ASSERT_EQ(m.textures.size(), size_t{ 2 });
    ASSERT_TRUE(m.textures[0].valid());
    ASSERT_TRUE(m.textures[1].valid());
    ASSERT_TRUE(m.materials.size() >= 3);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, 0);
    ASSERT_EQ(m.materials[2].diffuse_map.tex, 1);
}

TEST(gltf_valid, parallel_decode_failure_compacts_and_remaps)
{
    // image0 valid (BMP), image1 bad (4 garbage bytes). Loaded with n_threads=4.
    // The failed slot must be compacted out and the referencing material remapped
    // to -1, while the survivor occupies slot 0.
    constexpr size_t bmp_size = sizeof(k1x1_red_bmp);
    std::string bin;
    emit_tri_verts(bin);                                                // 36 at offset 0
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), bmp_size); // 58 at offset 36
    // 4 zero bytes at offset 94: too short for any stb_image format probe to
    // accept (PNG needs 8-byte sig, BMP/PSD/GIF need magic, TGA needs ≥18-byte
    // header). Decode reliably fails — exercises the failure-compaction path.
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0);
    bin.push_back(0);

    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":0},\"material\":1}]}],"
                             "\"materials\":["
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}},"
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":1}}}],"
                             "\"textures\":[{\"source\":0},{\"source\":1}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"},"
                             "{\"bufferView\":2,\"mimeType\":\"image/bmp\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":["
                             "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58},"
                             "{\"buffer\":0,\"byteOffset\":94,\"byteLength\":4}"
                             "],"
                             "\"buffers\":[{\"byteLength\":98}]}";

    TmpFile f(tmp_path("rast_par_fail.glb"), make_glb(json, bin));
    Mesh m;
    const bool ok = m.load_model(f.path, /*ao=*/false, /*n_threads=*/4);
    ASSERT_TRUE(ok);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].valid());
    ASSERT_TRUE(m.materials.size() >= 3);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, 0);
    ASSERT_EQ(m.materials[2].diffuse_map.tex, -1);
}

// Group: sampler wrap modes

// One-triangle GLB: single material whose baseColorTexture is image 0 (embedded BMP),
// with caller-supplied `textures` and `samplers` JSON segments. `samplers` is the full
// "samplers":[...], segment or "" for none.
static std::string sampler_glb(const std::string &textures, const std::string &samplers)
{
    std::string bin;
    emit_tri_verts(bin);                                                            // 36 @ 0
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp)); // 58 @ 36
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
        "\"textures\":" +
        textures +
        ","
        "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}]," +
        samplers +
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}],"
        "\"buffers\":[{\"byteLength\":94}]}";
    return make_glb(json, bin);
}

TEST(gltf_sampler, wrap_clamp_repeat_per_axis)
{
    // wrapS=33071 (CLAMP_TO_EDGE), wrapT=10497 (REPEAT) — carried independently per axis.
    TmpFile f(
        tmp_path("rast_wrap_cr.glb"),
        sampler_glb(R"([{"source":0,"sampler":0}])", R"("samplers":[{"wrapS":33071,"wrapT":10497}],)")
    );
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].wrap_s == WrapMode::Clamp);
    ASSERT_TRUE(m.textures[0].wrap_t == WrapMode::Repeat);
}

TEST(gltf_sampler, wrap_mirror)
{
    // wrapS=33648 (MIRRORED_REPEAT).
    TmpFile f(
        tmp_path("rast_wrap_mirror.glb"),
        sampler_glb(R"([{"source":0,"sampler":0}])", R"("samplers":[{"wrapS":33648,"wrapT":33648}],)")
    );
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].wrap_s == WrapMode::Mirror);
    ASSERT_TRUE(m.textures[0].wrap_t == WrapMode::Mirror);
}

TEST(gltf_sampler, no_sampler_defaults_to_repeat)
{
    // Texture with no sampler → both axes Repeat (glTF spec default).
    TmpFile f(tmp_path("rast_wrap_nosamp.glb"), sampler_glb(R"([{"source":0}])", ""));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].wrap_s == WrapMode::Repeat);
    ASSERT_TRUE(m.textures[0].wrap_t == WrapMode::Repeat);
}

TEST(gltf_sampler, omitted_wrap_fields_default_to_repeat)
{
    // Sampler present but wrapS/wrapT omitted → cgltf reports default repeat.
    TmpFile f(tmp_path("rast_wrap_empty.glb"), sampler_glb(R"([{"source":0,"sampler":0}])", R"("samplers":[{}],)"));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].wrap_s == WrapMode::Repeat);
    ASSERT_TRUE(m.textures[0].wrap_t == WrapMode::Repeat);
}

TEST(gltf_sampler, dedup_splits_on_differing_sampler)
{
    // Two textures share image 0 but reference different samplers (CLAMP vs REPEAT). Wrap
    // belongs to the (image, sampler) pair, so they must NOT collapse — two distinct slots.
    std::string bin;
    emit_tri_verts(bin);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":0},\"material\":1}]}],"
                             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}},"
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":1}}}],"
                             "\"textures\":[{\"source\":0,\"sampler\":0},{\"source\":0,\"sampler\":1}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"samplers\":[{\"wrapS\":33071,\"wrapT\":33071},{\"wrapS\":10497,\"wrapT\":10497}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}],"
                             "\"buffers\":[{\"byteLength\":94}]}";
    TmpFile f(tmp_path("rast_wrap_split.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.textures.size(), size_t{ 2 });
    ASSERT_TRUE(m.materials.size() >= 3);
    const Texture &a = m.textures[static_cast<size_t>(m.materials[1].diffuse_map.tex)];
    const Texture &b = m.textures[static_cast<size_t>(m.materials[2].diffuse_map.tex)];
    ASSERT_TRUE(a.wrap_s == WrapMode::Clamp);
    ASSERT_TRUE(b.wrap_s == WrapMode::Repeat);
}

TEST(gltf_sampler, dedup_keeps_identical_sampler)
{
    // Two textures, same image, same sampler → still one decoded slot (no dedup regression).
    std::string bin;
    emit_tri_verts(bin);
    bin.append(reinterpret_cast<const char *>(k1x1_red_bmp), sizeof(k1x1_red_bmp));
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":0},\"material\":1}]}],"
                             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}},"
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":1}}}],"
                             "\"textures\":[{\"source\":0,\"sampler\":0},{\"source\":0,\"sampler\":0}],"
                             "\"images\":[{\"bufferView\":1,\"mimeType\":\"image/bmp\"}],"
                             "\"samplers\":[{\"wrapS\":33071,\"wrapT\":33071}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
                             "\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                             "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":58}],"
                             "\"buffers\":[{\"byteLength\":94}]}";
    TmpFile f(tmp_path("rast_wrap_keep.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_EQ(m.textures.size(), size_t{ 1 });
    ASSERT_TRUE(m.textures[0].wrap_s == WrapMode::Clamp);
}

// Group: inline data: URI images

// One-triangle GLB whose single material's baseColorTexture is image 0 carrying `uri`.
static std::string data_uri_tex_glb(const std::string &uri)
{
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":"
                             "{\"POSITION\":0},\"material\":0}]}],"
                             "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],"
                             "\"textures\":[{\"source\":0}],"
                             "\"images\":[{\"uri\":\"" +
                             uri +
                             "\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    return make_glb(json, bin);
}

// Build the data: URI for the given bytes with the given mediatype, base64-encoded.
static std::string data_uri(const std::string &mediatype, const uint8_t *bytes, size_t n)
{
    return "data:" + mediatype + ";base64," + b64encode(bytes, n);
}

TEST(gltf_data_uri, base64_bmp_decodes)
{
    // Happy path: a padded-base64 BMP data URI decodes through the stb route. Proves the
    // base64 round-trip (k1x1_red_bmp is 58 bytes -> padded encoding) and content-sniff.
    const std::string uri = data_uri("image/bmp", k1x1_red_bmp, sizeof(k1x1_red_bmp));
    TmpFile f(tmp_path("rast_datauri_bmp.glb"), data_uri_tex_glb(uri));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    const int idx = m.materials[1].diffuse_map.tex;
    ASSERT_TRUE(idx >= 0);
    ASSERT_TRUE(idx < static_cast<int>(m.textures.size()));
    const Texture &t = m.textures[static_cast<size_t>(idx)];
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(t.width, 1);
    ASSERT_EQ(t.height, 1);
}

TEST(gltf_data_uri, no_base64_marker_dropped)
{
    // data:<mediatype>,<raw> without ";base64" is not handled -> slot dropped, load ok.
    TmpFile f(tmp_path("rast_datauri_nomarker.glb"), data_uri_tex_glb("data:image/bmp,Qk0"));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1);
}

TEST(gltf_data_uri, invalid_alphabet_dropped)
{
    // Payload has no base64-alphabet chars -> out_size 0 -> slot dropped, load ok.
    TmpFile f(tmp_path("rast_datauri_badalpha.glb"), data_uri_tex_glb("data:image/bmp;base64,@@@@"));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1);
}

TEST(gltf_data_uri, empty_payload_dropped)
{
    // Comma is the last char -> empty payload -> out_size 0 -> slot dropped, load ok.
    TmpFile f(tmp_path("rast_datauri_empty.glb"), data_uri_tex_glb("data:image/bmp;base64,"));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1);
}

TEST(gltf_data_uri, valid_base64_non_image_dropped)
{
    // Well-formed base64 that decodes to non-image bytes: the base64 step succeeds but the
    // content-sniff / stb decode rejects it -> slot dropped, load ok.
    const uint8_t hello[] = { 'h', 'e', 'l', 'l', 'o' };
    const std::string uri = data_uri("application/octet-stream", hello, sizeof(hello));
    TmpFile f(tmp_path("rast_datauri_nonimage.glb"), data_uri_tex_glb(uri));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1);
}

TEST(gltf_data_uri, no_comma_dropped)
{
    // No comma at all -> strchr returns null -> slot dropped without UB, load ok.
    TmpFile f(tmp_path("rast_datauri_nocomma.glb"), data_uri_tex_glb("data:image/bmp;base64"));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1);
}

TEST(gltf_data_uri, short_metadata_no_underread)
{
    // Comma at index 5: the `comma - uri >= 7` guard rejects before the strncmp would read
    // before the start of the string -> slot dropped, load ok.
    TmpFile f(tmp_path("rast_datauri_short.glb"), data_uri_tex_glb("data:,x"));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 2);
    ASSERT_EQ(m.materials[1].diffuse_map.tex, -1);
}

TEST(gltf_data_uri, shared_data_uri_dedup)
{
    // Two materials, two textures, both pointing at image 0 (one data: URI). Both load_tex
    // calls resolve the same cgltf_image* -> TexKey dedup -> one decode, one texture slot,
    // and both materials reference it.
    const std::string uri = data_uri("image/bmp", k1x1_red_bmp, sizeof(k1x1_red_bmp));
    const std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                             "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":["
                             "{\"attributes\":{\"POSITION\":0},\"material\":0},"
                             "{\"attributes\":{\"POSITION\":0},\"material\":1}]}],"
                             "\"materials\":["
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}},"
                             "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":1}}}],"
                             "\"textures\":[{\"source\":0},{\"source\":0}],"
                             "\"images\":[{\"uri\":\"" +
                             uri +
                             "\"}],"
                             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
                             "\"type\":\"VEC3\",\"min\":[-1,-1,0],\"max\":[1,1,0]}],"
                             "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
                             "\"buffers\":[{\"byteLength\":36}]}";
    std::string bin;
    emit_tri_verts(bin);
    TmpFile f(tmp_path("rast_datauri_dedup.glb"), make_glb(json, bin));
    Mesh m = load_ok(f.path);
    ASSERT_TRUE(m.materials.size() >= 3);
    const int a = m.materials[1].diffuse_map.tex;
    const int b = m.materials[2].diffuse_map.tex;
    ASSERT_TRUE(a >= 0);
    ASSERT_EQ(a, b);
    ASSERT_EQ(m.textures.size(), static_cast<size_t>(1));
}
