#pragma once

#include <cstdint>

// Shading mode shared by the renderer (runtime state, template dispatch) and the
// CLI layer (args.h holds the parsed --shading value). Lives in its own header so
// args.h can use the real enum without depending on the renderer headers.
enum class ShadingMode : std::uint8_t
{
    Wireframe,
    Flat,
    Phong
};
