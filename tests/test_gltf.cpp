#include "loader_util.h"

// ═══════════════════════════════════════════════════════════════════════════
//  SHIPPED GLTF/GLB MODELS
// ═══════════════════════════════════════════════════════════════════════════

TEST(shipped, gltf_duck)
{
    Mesh m = load_ok("models/gltf/Duck.gltf");
    ASSERT_TRUE(m.triangles.size() > 0);
    ASSERT_TRUE(!m.textures.empty());
    ASSERT_TRUE(m.materials.size() >= 2);
}

TEST(shipped, glb_duck)
{
    Mesh m = load_ok("models/glb/Duck.glb");
    ASSERT_TRUE(m.triangles.size() > 0);
    ASSERT_TRUE(!m.textures.empty());
    ASSERT_TRUE(m.materials.size() >= 2);
}

// ═══════════════════════════════════════════════════════════════════════════
//  REJECTIONS
// ═══════════════════════════════════════════════════════════════════════════

TEST(reject, empty_file_gltf)
{
    TmpFile t("/tmp/rast_empty.gltf", "");
    assert_rejects(t.path);
}

TEST(reject, empty_file_glb)
{
    TmpFile t("/tmp/rast_empty.glb", "");
    assert_rejects(t.path);
}
