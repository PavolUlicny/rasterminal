#pragma once

#include "src/shading.h"

#include <cstdint>
#include <string>

// CLI enums stay here to keep ParsedArgs independent of the renderer. *_COUNT values
// support runtime cycling in declaration order.

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

// Auto detects color support. Forced modes still obey the TERM=dumb and Windows VT checks.
enum class ColorChoice : std::uint8_t
{
    Auto,
    TrueColor,
    Palette256
};

// Auto prefers detected kitty, then sixel, then blocks. Forced pixel backends still
// require detection because unsupported terminals may swallow their escapes silently.
enum class GraphicsChoice : std::uint8_t
{
    Auto,
    Kitty,
    Sixel,
    Blocks
};

// Direction describes the model's on-screen movement. There is no runtime cycle.
enum class SpinDirection : std::uint8_t
{
    Left,
    Right
};

// Parsed arguments use plain types and do not depend on renderer headers.
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
    GraphicsChoice graphics = GraphicsChoice::Auto;
    int fps = 30;               // 0 = uncapped (set by bare -f), >0 = cap at this value
    int bench = -1;             // -1 = off; >=1 = run this many measured frames headlessly
    int bench_width = 200;      // headless framebuffer width in pixels
    int bench_height = 120;     // headless framebuffer height in pixels
    int bench_warmup = 20;      // warmup frames discarded before measurement (0 = none)
    float smooth_angle = 60.0f; // crease angle (deg) for computed normals; 0=faceted, 180=fully smooth
    float spin_speed = 45.0f;   // auto-rotation speed in degrees/sec; magnitude only, always > 0
    SpinDirection spin_direction = SpinDirection::Left;
    float yaw = 0.0f;     // initial camera yaw in degrees, [-180, 180]
    float pitch = -17.2f; // initial camera pitch in degrees, [-180, 180] (rounding of the old fixed -0.3 rad tilt)
    float zoom = 1.0f;    // initial zoom factor, [0.2, 100]: apparent-size multiplier of the auto-fit framing
    bool cull = true;
    bool texture = true;
    bool spin = false;
    bool ao = true;
    bool hud = true;
    bool input = true; // false = --no-input: keyboard and mouse bindings ignored (Q and Ctrl+C still quit)
    // Free-flying camera without gravity, collision or a ground plane.
    bool first_person = false;
    // Initial first-person speed, relative to the model-scaled default.
    float first_person_speed = 1.0f;
};

// A failed result carries the process exit code; a successful one carries valid arguments.
struct ParseResult
{
    bool ok = true;
    int exit_code = 0;
    ParsedArgs args;
};

[[nodiscard]] ParseResult parse_args(int argc, char *argv[]);

// Invoked basename for diagnostics. Accepts either path separator and defaults to rasterminal.
[[nodiscard]] const char *program_name(const char *argv0);
