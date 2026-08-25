#pragma once

#include <assimp/material.h>
#include <assimp/texture.h>

#include <string>

namespace assimp_detail
{
    // Some importers leave the ninth, terminator byte uninitialized.
    std::string embedded_format_hint(const aiTexture &source);

    // Prefix lookup can match "$mat.blend.*" and overflow this one-int destination.
    bool get_blend_func(const aiMaterial &source, int &out);
} // namespace assimp_detail
