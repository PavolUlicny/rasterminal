#pragma once

#include "src/args.h"
#include "src/terminal/color.h"

namespace viewer
{
    inline constexpr Color BG_BLACK = { 0, 0, 0 };
    inline constexpr Color BG_GRAY = { 128, 128, 128 };
    inline constexpr Color BG_WHITE = { 240, 240, 240 };

    // "_of" rather than "_color" to avoid reading like the Renderer::wireframe_color member.
    constexpr Color wireframe_color_of(WireframeColor c) noexcept
    {
        switch (c)
        {
        case WireframeColor::White:
            return { 200, 200, 200 };
        case WireframeColor::Red:
            return { 220, 80, 80 };
        case WireframeColor::Green:
            return { 80, 200, 120 };
        case WireframeColor::Yellow:
            return { 230, 200, 80 };
        case WireframeColor::Cyan:
            return { 100, 200, 220 };
        case WireframeColor::Magenta:
            return { 220, 120, 200 };
        }
        return { 200, 200, 200 };
    }

    constexpr Color background_color(Background b) noexcept
    {
        switch (b)
        {
        case Background::Gray:
            return BG_GRAY;
        case Background::White:
            return BG_WHITE;
        case Background::Black:
            return BG_BLACK;
        }
        return BG_BLACK;
    }
} // namespace viewer
