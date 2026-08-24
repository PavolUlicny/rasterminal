#pragma once

#include <assimp/material.h>
#include <assimp/texture.h>

#include <string>

namespace assimp_detail
{
    // Read at most the eight-byte descriptor. Some importers do not initialize the
    // ninth byte reserved for the terminator.
    std::string embedded_format_hint(const aiTexture &source);

    // Assimp's prefix lookup can match Blender's longer "$mat.blend.*" keys, then write
    // several floats through this one-int destination. Match AI_MATKEY_BLEND_FUNC exactly.
    bool get_blend_func(const aiMaterial &source, int &out);
} // namespace assimp_detail
