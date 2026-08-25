#pragma once

#include "src/terminal/color.h" // Color, ColorMode

#include <string>
#include <string_view>

// Pure HUD composer. It emits a padded, foreground-styled line without terminal I/O or CLI types.
struct HudInfo
{
    // Empty views omit optional fields.
    std::string_view model_name; // basename, control bytes already sanitized; composer truncates
    std::string_view shading_name;
    std::string_view light_name;
    std::string_view bg_name;
    std::string_view wf_name; // non-empty only in wireframe shading
    Color wf_color;           // the wf name renders in its own edge colour
    bool spinning = false;
    bool culling = false;
    bool texturing = false;
    bool has_textures = false; // false omits the tex tag entirely (nothing to toggle)
    bool first_person = false;
    float fp_speed = 1.0f;
    int fps = 0;
};

// Compose a full-width bar with identity left, modes centered and live values right.
// Narrow layouts drop fields by priority, preserving fps longest. Non-positive width is empty.
std::string compose_hud(const HudInfo &info, int cols, ColorMode mode);
