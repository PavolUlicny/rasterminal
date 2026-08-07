#pragma once

#include "color.h" // Color, ColorMode

#include <string>
#include <string_view>

// HUD status-bar composer. Pure: turns the session state into one styled, full-width line
// (fg-only SGR escapes plus explicit padding) for Framebuffer::set_hud(); it never writes to
// the terminal. Its own module for the same reason text.h is: framebuffer renders the HUD
// string but composing and measuring one is not its business, and main.cpp should not know
// escape syntax.

// Plain strings and bools rather than the args.h enums, so the composer does not depend on the
// CLI layer and tests can drive it directly.
struct HudInfo
{
    // One absence convention throughout: an empty view means the field is not shown. (wf_name
    // used to be a nullptr-checked pointer while its three neighbours would have dereferenced
    // one, which is two rules for the same question.)
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

// Compose the bar for a terminal `cols` wide: identity left, render modes centred, toggles and
// live numbers right-aligned, padded with spaces to exactly `cols` columns. When the full set
// does not fit, fields drop by priority: background, lighting mode, toggle tags, wireframe
// colour, name shrunk twice, name, first-person speed. The ladder runs all the way down rather
// than letting the line overrun: the right zone is right-aligned, so an overrun is taken off
// the LAST field, the fps reading, which therefore survives to about ten columns; below that
// the line runs past the edge and the terminal clips it (the framebuffer draws with auto-wrap
// off), keeping its
// inter-field gap. `cols < 1` returns an empty string. Emits fg SGRs only, in `mode`'s depth;
// the bar background is set once by the framebuffer.
std::string compose_hud(const HudInfo &info, int cols, ColorMode mode);
