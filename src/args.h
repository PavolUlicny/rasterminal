#pragma once

#include "shading.h"

#include <cstdint>
#include <string>

// CLI-level enumerations for the value flags. Defined here (not in the renderer
// headers) so ParsedArgs stays free of renderer dependencies; main.cpp maps them
// to colours/lights/framebuffer modes. The *_COUNT constants back the runtime
// cycling keybindings (B/L/C wrap through the enumerators in declaration order).

enum class Background : std::uint8_t
{
    Black,
    Gray,
    White
};
constexpr int BACKGROUND_COUNT = static_cast<int>(Background::White) + 1;

enum class LightingMode : std::uint8_t
{
    Dual,
    Single,
    Flat
};
constexpr int LIGHTING_MODE_COUNT = static_cast<int>(LightingMode::Flat) + 1;

enum class WireframeColor : std::uint8_t
{
    White,
    Red,
    Green,
    Yellow,
    Cyan,
    Magenta
};
constexpr int WIREFRAME_COLOR_COUNT = static_cast<int>(WireframeColor::Magenta) + 1;

// --color choice: Auto defers to platform::detect_term_color(); the other two
// force the framebuffer mode (the TERM=dumb fatal and Windows VT gate still apply).
enum class ColorChoice : std::uint8_t
{
    Auto,
    TrueColor,
    Palette256
};

// --spin-direction: the way the model's front face moves on screen while
// spinning. Left is the historical direction (no COUNT constant: like
// ColorChoice, there is no cycling keybinding for it).
enum class SpinDirection : std::uint8_t
{
    Left,
    Right
};

// Parsed command-line arguments.  All values are plain types — no renderer
// dependencies — so this header can be included by the test binary cheaply.
struct ParsedArgs
{
    std::string model_path; // required positional
    int n_threads = -1;     // raw tri-state (-1 = auto, 0 = all cores, >0 = N); never use directly,
                            // resolve via Renderer::resolve_thread_count (clamps N to the hw count)
    ShadingMode shading = ShadingMode::Phong;
    Background bg = Background::Black;
    LightingMode lighting = LightingMode::Dual;
    WireframeColor wireframe_color = WireframeColor::White;
    ColorChoice color = ColorChoice::Auto;
    int fps = 60;               // 0 = uncapped (set by bare -f), >0 = cap at this value
    int bench = -1;             // -1 = off; >=1 = run this many measured frames headlessly
    int bench_width = 200;      // headless framebuffer width in pixels
    int bench_height = 120;     // headless framebuffer height in pixels
    int bench_warmup = 20;      // warmup frames discarded before measurement (0 = none)
    float smooth_angle = 60.0f; // crease angle (deg) for computed normals; 0=faceted, 180=fully smooth
    float spin_speed = 45.0f;   // auto-rotation speed in degrees/sec; magnitude only, always > 0
    SpinDirection spin_direction = SpinDirection::Left;
    bool cull = true;
    bool texture = true;
    bool spin = false;
    bool ao = true;
    bool hud = true;
};

// Result of parse_args().
// ok=false → caller should return exit_code immediately (error or --help).
// ok=true  → args is fully populated and valid.
struct ParseResult
{
    bool ok = true;
    int exit_code = 0;
    ParsedArgs args;
};

[[nodiscard]] ParseResult parse_args(int argc, char *argv[]);

// Basename of argv[0] — the name the program was invoked as — for use as the
// diagnostic prefix ("progname: message", the standard Unix convention).
// Splits on '/' or '\\' (Windows paths); falls back to "rasterminal" when
// argv0 is null, empty, or ends in a separator.
[[nodiscard]] const char *program_name(const char *argv0);
