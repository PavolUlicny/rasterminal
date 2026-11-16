#pragma once

#include "src/args.h"
#include "src/shading.h"
#include "src/terminal/hud.h"
#include "src/terminal/text.h"
#include "src/viewer/scene_colors.h"
#include "src/viewer/state.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace viewer
{
    // The HUD's fields are uniformly lowercase.
    constexpr const char *shading_name(ShadingMode mode) noexcept
    {
        switch (mode)
        {
        case ShadingMode::Wireframe:
            return "wireframe";
        case ShadingMode::Flat:
            return "flat";
        case ShadingMode::Phong:
            return "phong";
        }
        return "phong";
    }

    constexpr const char *lighting_name(LightingMode m) noexcept
    {
        switch (m)
        {
        case LightingMode::Single:
            return "single";
        case LightingMode::Flat:
            return "flat";
        case LightingMode::Dual:
            return "dual";
        }
        return "dual";
    }

    constexpr const char *background_name(Background b) noexcept
    {
        switch (b)
        {
        case Background::Gray:
            return "gray";
        case Background::White:
            return "white";
        case Background::Black:
            return "black";
        }
        return "black";
    }

    constexpr const char *wireframe_name(WireframeColor c) noexcept
    {
        switch (c)
        {
        case WireframeColor::White:
            return "white";
        case WireframeColor::Red:
            return "red";
        case WireframeColor::Green:
            return "green";
        case WireframeColor::Yellow:
            return "yellow";
        case WireframeColor::Cyan:
            return "cyan";
        case WireframeColor::Magenta:
            return "magenta";
        }
        return "white";
    }

    // A control-safe basename; the HUD composer handles truncation.
    inline std::string hud_model_name(std::string_view path)
    {
        const size_t slash = path.find_last_of("/\\");
        if (slash != std::string_view::npos)
        {
            path.remove_prefix(slash + 1);
        }
        return sanitize_controls(path);
    }

    // The result borrows model_name.
    inline HudInfo make_hud_info(const ViewerState &state, std::string_view model_name, bool has_textures, int fps)
    {
        HudInfo info;
        info.model_name = model_name;
        info.shading_name = shading_name(state.settings.shading);
        info.light_name = lighting_name(state.settings.lighting);
        info.bg_name = background_name(state.settings.background);
        if (state.settings.shading == ShadingMode::Wireframe)
        {
            info.wf_name = wireframe_name(state.settings.wireframe_color);
            info.wf_color = wireframe_color_of(state.settings.wireframe_color);
        }
        info.spinning = state.settings.spinning;
        info.culling = state.settings.culling;
        info.texturing = state.settings.texturing;
        info.has_textures = has_textures;
        info.first_person = state.camera.first_person;
        info.fp_speed = state.camera.fp_speed;
        info.fps = fps;
        return info;
    }
} // namespace viewer
