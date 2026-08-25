#pragma once

#include <cstdint>

// Shared here so CLI parsing does not depend on renderer headers.
enum class ShadingMode : std::uint8_t
{
    Wireframe,
    Flat,
    Phong
};
