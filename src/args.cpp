#include "src/args.h"

#include "src/platform/console.h"
#include "src/render/camera.h" // FP_SPEED_{MIN,MAX}: --first-person-speed parses the interactive range
#include "src/shading.h"
#include "src/version.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

const char *program_name(const char *argv0)
{
    if (!argv0 || !*argv0)
    {
        return "rasterminal";
    }
    const char *base = argv0;
    for (const char *p = argv0; *p != '\0'; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            base = p + 1;
        }
    }
    return (*base != '\0') ? base : "rasterminal"; // trailing-separator guard
}

namespace
{

    // Measured frames for a bare --bench or -B.
    constexpr int BENCH_DEFAULT_FRAMES = 200;

    struct ArgumentCursor
    {
        int argc;
        const char *const *argv;
        int index;
        const char *prog;

        const char *require_value(const char *flag)
        {
            if (index + 1 >= argc)
            {
                std::fprintf(stderr, "%s: %s requires a value\n", prog, flag);
                return nullptr;
            }
            return argv[++index];
        }
    };

    // Cast through unsigned char because std::tolower is undefined for negative char values.
    std::string to_lower(const char *val)
    {
        std::string v = val;
        std::transform(
            v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
        );
        return v;
    }

    // Case-insensitive parser shared by named-value flags. Aliases are extra table entries.
    template <typename E>
    bool parse_enum(
        const char *prog,
        const char *flag,
        const char *val,
        std::initializer_list<std::pair<const char *, E>> names,
        const char *expected,
        E &out
    )
    {
        const std::string v = to_lower(val);
        const auto *hit = std::find_if(names.begin(), names.end(), [&v](const auto &name) { return v == name.first; });
        if (hit == names.end())
        {
            std::fprintf(stderr, "%s: %s: invalid value '%s' (expected %s)\n", prog, flag, val, expected);
            return false;
        }
        out = hit->second;
        return true;
    }

    bool parse_positive_int(const char *prog, const char *flag, const char *val, int &out)
    {
        char *end = nullptr;
        errno = 0;
        const long v = std::strtol(val, &end, 10);
        if (end == val || *end != '\0' || v <= 0 || v > INT_MAX || errno == ERANGE)
        {
            std::fprintf(stderr, "%s: %s requires a positive integer, got '%s'\n", prog, flag, val);
            return false;
        }
        out = static_cast<int>(v);
        return true;
    }

    bool parse_nonnegative_int(const char *prog, const char *flag, const char *val, int &out)
    {
        char *end = nullptr;
        errno = 0;
        const long v = std::strtol(val, &end, 10);
        if (end == val || *end != '\0' || v < 0 || v > INT_MAX || errno == ERANGE)
        {
            std::fprintf(stderr, "%s: %s requires a non-negative integer, got '%s'\n", prog, flag, val);
            return false;
        }
        out = static_cast<int>(v);
        return true;
    }

    bool parse_size(const char *prog, const char *flag, const char *val, int &w, int &h)
    {
        const auto err = [prog, flag, val]() -> bool
        {
            std::fprintf(stderr, "%s: %s: invalid value '%s' (expected WxH, e.g. 400x240)\n", prog, flag, val);
            return false;
        };
        const char *sep = std::strchr(val, 'x');
        if (!sep || sep == val)
        {
            return err();
        }
        char *end = nullptr;
        errno = 0;
        const long ww = std::strtol(val, &end, 10);
        if (end != sep || ww <= 0 || ww > INT_MAX || errno == ERANGE)
        {
            return err();
        }
        errno = 0;
        const long hh = std::strtol(sep + 1, &end, 10);
        if (end == sep + 1 || *end != '\0' || hh <= 0 || hh > INT_MAX || errno == ERANGE)
        {
            return err();
        }
        // Bound the product before the framebuffer multiplies in 32-bit size_t.
        if (static_cast<int64_t>(ww) * hh > INT_MAX)
        {
            return err();
        }
        w = static_cast<int>(ww);
        h = static_cast<int>(hh);
        return true;
    }

    bool is_all_digits(const char *s)
    {
        if (!s || !*s)
        {
            return false;
        }
        while (*s)
        {
            if (!std::isdigit(static_cast<unsigned char>(*s)))
            {
                return false;
            }
            ++s;
        }
        return true;
    }

    // An attached value is always parsed. A separate token is consumed only when every character is an ASCII digit.
    bool parse_optional_positive_int(
        ArgumentCursor &cursor, const char *flag, const char *attached, int &out, int bare_value
    )
    {
        if (attached != nullptr)
        {
            return parse_positive_int(cursor.prog, flag, attached, out);
        }
        if (cursor.index + 1 < cursor.argc && is_all_digits(cursor.argv[cursor.index + 1]))
        {
            return parse_positive_int(cursor.prog, flag, cursor.argv[++cursor.index], out);
        }
        out = bare_value;
        return true;
    }

    bool parse_shading(const char *prog, const char *flag, const char *val, ShadingMode &out)
    {
        return parse_enum(
            prog, flag, val,
            { { "wireframe", ShadingMode::Wireframe }, { "flat", ShadingMode::Flat }, { "phong", ShadingMode::Phong } },
            "wireframe|flat|phong", out
        );
    }

    bool parse_background(const char *prog, const char *flag, const char *val, Background &out)
    {
        return parse_enum(
            prog, flag, val,
            { { "black", Background::Black },
              { "gray", Background::Gray },
              { "grey", Background::Gray },
              { "white", Background::White } },
            "black|gray|white", out
        );
    }

    bool parse_lighting(const char *prog, const char *flag, const char *val, LightingMode &out)
    {
        return parse_enum(
            prog, flag, val,
            { { "dual", LightingMode::Dual }, { "single", LightingMode::Single }, { "flat", LightingMode::Flat } },
            "dual|single|flat", out
        );
    }

    bool parse_wireframe_color(const char *prog, const char *flag, const char *val, WireframeColor &out)
    {
        return parse_enum(
            prog, flag, val,
            { { "white", WireframeColor::White },
              { "red", WireframeColor::Red },
              { "green", WireframeColor::Green },
              { "yellow", WireframeColor::Yellow },
              { "cyan", WireframeColor::Cyan },
              { "magenta", WireframeColor::Magenta } },
            "white|red|green|yellow|cyan|magenta", out
        );
    }

    bool parse_color(const char *prog, const char *flag, const char *val, ColorChoice &out)
    {
        return parse_enum(
            prog, flag, val,
            { { "auto", ColorChoice::Auto },
              { "truecolor", ColorChoice::TrueColor },
              { "24bit", ColorChoice::TrueColor },
              { "256", ColorChoice::Palette256 } },
            "truecolor|24bit|256|auto", out
        );
    }

    bool parse_graphics(const char *prog, const char *flag, const char *val, GraphicsChoice &out)
    {
        return parse_enum(
            prog, flag, val,
            { { "auto", GraphicsChoice::Auto },
              { "kitty", GraphicsChoice::Kitty },
              { "sixel", GraphicsChoice::Sixel },
              { "blocks", GraphicsChoice::Blocks } },
            "kitty|sixel|blocks|auto", out
        );
    }

    // ERANGE also rejects subnormal values, which are useless for these flags.
    bool parse_float(
        const char *prog, const char *flag, const char *val, bool (*valid)(float), const char *expected, float &out
    )
    {
        char *end = nullptr;
        errno = 0;
        const float v = std::strtof(val, &end);
        if (end == val || *end != '\0' || errno == ERANGE || !std::isfinite(v) || !valid(v))
        {
            std::fprintf(stderr, "%s: %s: invalid value '%s' (expected %s)\n", prog, flag, val, expected);
            return false;
        }
        out = v;
        return true;
    }

    bool parse_angle(const char *prog, const char *flag, const char *val, float &out)
    {
        return parse_float(
            prog, flag, val, [](float v) { return v >= 0.0f && v <= 180.0f; }, "a number in [0, 180]", out
        );
    }

    bool parse_spin_speed(const char *prog, const char *flag, const char *val, float &out)
    {
        return parse_float(prog, flag, val, [](float v) { return v > 0.0f; }, "a positive number", out);
    }

    bool parse_orbit_angle(const char *prog, const char *flag, const char *val, float &out)
    {
        return parse_float(
            prog, flag, val, [](float v) { return v >= -180.0f && v <= 180.0f; }, "a number in [-180, 180]", out
        );
    }

    bool parse_zoom(const char *prog, const char *flag, const char *val, float &out)
    {
        // These bounds keep the initial camera distance inside the interactive zoom range.
        return parse_float(
            prog, flag, val, [](float v) { return v >= 0.2f && v <= 100.0f; }, "a number in [0.2, 100]", out
        );
    }

    bool parse_first_person_speed(const char *prog, const char *flag, const char *val, float &out)
    {
        return parse_float(
            prog, flag, val, [](float v) { return v >= Camera::FP_SPEED_MIN && v <= Camera::FP_SPEED_MAX; },
            "a number in [0.05, 20]", out
        );
    }

    bool parse_spin_direction(const char *prog, const char *flag, const char *val, SpinDirection &out)
    {
        return parse_enum(
            prog, flag, val, { { "left", SpinDirection::Left }, { "right", SpinDirection::Right } }, "left|right", out
        );
    }

    struct BooleanOption
    {
        bool ParsedArgs::*field;
        std::string_view long_name;
        char short_name;
        bool value;
    };

    constexpr BooleanOption BOOLEAN_OPTIONS[] = {
        { &ParsedArgs::spin, "--spin", 'S', true },
        { &ParsedArgs::spin, "--no-spin", '\0', false },
        { &ParsedArgs::ao, "--ao", '\0', true },
        { &ParsedArgs::ao, "--no-ao", '\0', false },
        { &ParsedArgs::hud, "--hud", '\0', true },
        { &ParsedArgs::hud, "--no-hud", '\0', false },
        { &ParsedArgs::input, "--input", '\0', true },
        { &ParsedArgs::input, "--no-input", '\0', false },
        { &ParsedArgs::first_person, "--first-person", '\0', true },
        { &ParsedArgs::first_person, "--no-first-person", '\0', false },
        { &ParsedArgs::cull, "--cull", '\0', true },
        { &ParsedArgs::cull, "--no-cull", '\0', false },
        { &ParsedArgs::texture, "--texture", '\0', true },
        { &ParsedArgs::texture, "--no-texture", '\0', false },
    };

    struct SeenOptions
    {
        bool bench_size = false;
        bool first_person_speed = false;
        bool bench_warmup = false;
    };

    using ValueHandler =
        bool (*)(const char *prog, const char *flag, const char *value, ParsedArgs &args, SeenOptions &seen);

    template <auto Field, auto Parser>
    // cppcheck-suppress unusedFunction -- instantiated as VALUE_OPTIONS function-pointer handlers.
    bool parse_member_value(
        const char *prog, const char *flag, const char *value, ParsedArgs &args, [[maybe_unused]] SeenOptions &seen
    )
    {
        return Parser(prog, flag, value, args.*Field);
    }

    struct ValueOptionSpec
    {
        std::string_view long_name;
        char short_name;
        ValueHandler handler;
    };

    constexpr ValueOptionSpec VALUE_OPTIONS[] = {
        { "--bench-size", '\0',
          [](const char *prog, const char *flag, const char *value, ParsedArgs &args, SeenOptions &seen)
          {
              seen.bench_size = true;
              return parse_size(prog, flag, value, args.bench_width, args.bench_height);
          } },
        { "--bench-warmup", '\0',
          [](const char *prog, const char *flag, const char *value, ParsedArgs &args, SeenOptions &seen)
          {
              seen.bench_warmup = true;
              return parse_nonnegative_int(prog, flag, value, args.bench_warmup);
          } },
        { "--smooth-angle", '\0', parse_member_value<&ParsedArgs::smooth_angle, parse_angle> },
        { "--color", '\0', parse_member_value<&ParsedArgs::color, parse_color> },
        { "--graphics", '\0', parse_member_value<&ParsedArgs::graphics, parse_graphics> },
        { "--spin-speed", '\0', parse_member_value<&ParsedArgs::spin_speed, parse_spin_speed> },
        { "--spin-direction", '\0', parse_member_value<&ParsedArgs::spin_direction, parse_spin_direction> },
        { "--yaw", '\0', parse_member_value<&ParsedArgs::yaw, parse_orbit_angle> },
        { "--pitch", '\0', parse_member_value<&ParsedArgs::pitch, parse_orbit_angle> },
        { "--zoom", '\0', parse_member_value<&ParsedArgs::zoom, parse_zoom> },
        { "--shading", 's', parse_member_value<&ParsedArgs::shading, parse_shading> },
        { "--bg", 'b', parse_member_value<&ParsedArgs::bg, parse_background> },
        { "--lighting", 'l', parse_member_value<&ParsedArgs::lighting, parse_lighting> },
        { "--first-person-speed", '\0',
          [](const char *prog, const char *flag, const char *value, ParsedArgs &args, SeenOptions &seen)
          {
              seen.first_person_speed = true;
              return parse_first_person_speed(prog, flag, value, args.first_person_speed);
          } },
        { "--wireframe-color", 'w', parse_member_value<&ParsedArgs::wireframe_color, parse_wireframe_color> },
    };

    struct OptionalIntegerSpec
    {
        std::string_view long_name;
        char short_name;
        int bare_value;
        int ParsedArgs::*field;
    };

    constexpr int ALL_THREADS = 0;
    constexpr int UNCAPPED_FPS = 0;

    constexpr OptionalIntegerSpec OPTIONAL_INTEGER_OPTIONS[] = {
        { "--threads", 'j', ALL_THREADS, &ParsedArgs::n_threads },
        { "--fps", 'f', UNCAPPED_FPS, &ParsedArgs::fps },
        { "--bench", 'B', BENCH_DEFAULT_FRAMES, &ParsedArgs::bench },
    };

    void print_help(const char *prog);
    void print_version();

    using ActionHandler = void (*)(const char *prog);

    struct ActionOption
    {
        std::string_view long_name;
        char short_name;
        ActionHandler handler;
    };

    constexpr ActionOption ACTION_OPTIONS[] = {
        { "--help", 'h', print_help },
        { "--version", 'V', [](const char *) { print_version(); } },
    };

    template <typename Option, size_t N>
    const Option *find_long_option(const Option (&options)[N], std::string_view name)
    {
        const auto *hit = std::find_if(
            std::begin(options), std::end(options), [name](const Option &option) { return option.long_name == name; }
        );
        return hit == std::end(options) ? nullptr : hit;
    }

    template <typename Option, size_t N> const Option *find_short_option(const Option (&options)[N], char name)
    {
        const auto *hit = std::find_if(
            std::begin(options), std::end(options), [name](const Option &option) { return option.short_name == name; }
        );
        return hit == std::end(options) ? nullptr : hit;
    }

    template <typename Option, size_t N> constexpr bool option_names_are_unique(const Option (&options)[N])
    {
        for (size_t i = 0; i < N; ++i)
        {
            for (size_t j = i + 1; j < N; ++j)
            {
                if (options[i].long_name == options[j].long_name ||
                    (options[i].short_name != '\0' && options[i].short_name == options[j].short_name))
                {
                    return false;
                }
            }
        }
        return true;
    }

    template <typename Left, size_t L, typename Right, size_t R>
    constexpr bool option_names_are_disjoint(const Left (&left)[L], const Right (&right)[R])
    {
        for (size_t i = 0; i < L; ++i)
        {
            for (size_t j = 0; j < R; ++j)
            {
                if (left[i].long_name == right[j].long_name ||
                    (left[i].short_name != '\0' && left[i].short_name == right[j].short_name))
                {
                    return false;
                }
            }
        }
        return true;
    }

    template <typename Option, size_t N> constexpr bool option_tables_are_unique(const Option (&options)[N])
    {
        return option_names_are_unique(options);
    }

    template <typename First, size_t N, typename... Rest>
    constexpr bool option_tables_are_unique(const First (&first)[N], const Rest &...rest)
    {
        return option_names_are_unique(first) && (option_names_are_disjoint(first, rest) && ...) &&
               option_tables_are_unique(rest...);
    }

    static_assert(
        option_tables_are_unique(BOOLEAN_OPTIONS, ACTION_OPTIONS, OPTIONAL_INTEGER_OPTIONS, VALUE_OPTIONS),
        "CLI option names must be unique"
    );

    bool reject_attached_value(const char *prog, const char *flag, const char *attached)
    {
        if (attached == nullptr)
        {
            return true;
        }
        std::fprintf(stderr, "%s: %s does not take a value\n", prog, flag);
        return false;
    }

    void print_version()
    {
        // GNU-style version block. Program identity is canonical, not argv[0].
        char text[512];
        std::snprintf(
            text, sizeof text,
            "rasterminal %s\n"
            "Copyright (C) %s %s\n"
            "License MIT: <https://opensource.org/license/mit>\n"
            "This is free software: you are free to change and redistribute it.\n"
            "There is NO WARRANTY, to the extent permitted by law.\n"
            "\n"
            "Written by %s.\n",
            RASTERMINAL_VERSION, RASTERMINAL_COPYRIGHT_YEAR, RASTERMINAL_AUTHOR, RASTERMINAL_AUTHOR
        );
        platform::write_utf8_stdout(text);
    }

    // As in git, a short flag beside --[no-]name means only the positive form.
    void print_help(const char *prog)
    {
        std::printf(
            "Usage: %s [options] <model>\n"
            "\n"
            "Render a 3D model as a kitty or sixel image where supported, or as\n"
            "Unicode half-blocks elsewhere. See --graphics and --color.\n"
            "\n"
            "Supported formats:\n"
            "  .obj        Wavefront OBJ with optional .mtl (diffuse/specular/normal maps)\n"
            "  .ply        ASCII or binary (little/big-endian)\n"
            "  .stl        ASCII or binary\n"
            "  .gltf/.glb  glTF 2.0 (PBR materials, textures, node transforms)\n"
            "  others      Through Assimp (FBX, Collada, 3DS, Blender, IFC, OFF, X, ...)\n"
            "\n"
            "Options:\n"
            "  -s,     --shading <mode>       Initial shading mode (default: phong)\n"
            "                                  wireframe|flat|phong\n"
            "  -b,     --bg <color>           Initial background color (default: black)\n"
            "                                  black|gray|white\n"
            "  -l,     --lighting <mode>      Initial lighting mode (default: dual)\n"
            "                                  dual|single|flat\n"
            "  -w,     --wireframe-color <c>  Initial wireframe color (default: white)\n"
            "                                  white|red|green|yellow|cyan|magenta\n"
            "          --yaw DEG              Initial camera yaw in [-180, 180] (default: 0)\n"
            "                                  positive turns the model left on screen\n"
            "          --pitch DEG            Initial pitch in [-180, 180] (default: -17.2)\n"
            "                                  negative looks down from above\n"
            "                                  first-person mode clamps it just inside +-90\n"
            "          --zoom FACTOR          Initial zoom in [0.2, 100] (default: 1)\n"
            "                                  2 appears twice as large\n"
            "                                  sets starting distance in first-person mode\n"
            "          --first-person-speed F Initial speed in [0.05, 20] (default: 1)\n"
            "                                  model-scaled; requires --first-person\n"
            "          --[no-]cull            Backface culling initial state (default: on)\n"
            "          --[no-]texture         Texture rendering initial state (default: on)\n"
            "  -S,     --[no-]spin            Auto-rotation initial state (default: off)\n"
            "  -j [N], --threads [N]          Threads (default: hardware concurrency)\n"
            "                                  bare uses the default; N is CPU-count clamped\n"
            "  -f [N], --fps [N]              Frame cap (default: 30)\n"
            "                                  bare -f/--fps uncapped, -f N caps at N fps\n"
            "  -B [N], --bench [N]            Headless benchmark: N frames (default: 200)\n"
            "                                  reports timing, fps and throughput to stderr\n"
            "          --bench-size WxH       Framebuffer size in pixels (default: 200x120)\n"
            "          --bench-warmup N       Bench warmup frames discarded (default: 20)\n"
            "          --smooth-angle DEG     Crease angle for computed normals (default: 60)\n"
            "                                  0=faceted, 180=smooth\n"
            "                                  ignored when an OBJ authors smoothing groups\n"
            "          --color <mode>         Color output (default: auto)\n"
            "                                  truecolor|24bit|256|auto\n"
            "                                  auto detects from COLORTERM/TERM/TMUX/STY\n"
            "                                  affects only the HUD under kitty or sixel\n"
            "          --graphics <mode>      Rendering backend (default: auto)\n"
            "                                  kitty|sixel|blocks|auto\n"
            "                                  auto prefers kitty, then sixel, then blocks\n"
            "          --spin-speed DEG/S     Speed in degrees/sec (default: 45)\n"
            "          --spin-direction <d>   Auto-rotation direction (default: left)\n"
            "                                  left|right: model movement on screen\n"
            "          --[no-]ao              Baked ambient occlusion (default: on)\n"
            "          --[no-]hud             HUD status line (default: shown)\n"
            "          --[no-]input           Keyboard and mouse controls (default: on)\n"
            "                                  --no-input keeps only Q; Ctrl+C also quits\n"
            "          --[no-]first-person    Free-flying camera; no gravity or collision\n"
            "                                  (default: off)\n"
            "  -h,     --help                 Show this message\n"
            "  -V,     --version              Show version and exit\n"
            "\n"
            "Controls:\n"
            "  1-3          Shading mode           B       Cycle background\n"
            "  Space        Toggle spin            L       Cycle lighting\n"
            "  WASD/arrows  Orbit camera           R       Reset to launch state\n"
            "  +/-          Zoom                   C       Cycle wireframe color\n"
            "  Mouse drag   Orbit                  K       Toggle backface culling\n"
            "  Scroll       Zoom                   T       Toggle textures\n"
            "  Q, Ctrl+C    Quit\n"
            "\n"
            "First-person controls (--first-person, replaces orbit and zoom above):\n"
            "  WASD         Move                   E / V   Move up / down\n"
            "  Arrows       Look                   +/-     Movement speed\n"
            "  Mouse drag   Look                   Scroll  Movement speed\n"
            "\n"
            "Report bugs to: <%s/issues>\n"
            "Home page: <%s>\n",
            prog, RASTERMINAL_HOMEPAGE, RASTERMINAL_HOMEPAGE
        );
    }

} // namespace

ParseResult parse_args(int argc, char *argv[])
{
    ParseResult result;
    ParsedArgs &args = result.args;

    // Use the invoked basename in diagnostics.
    const char *prog = program_name(argc > 0 ? argv[0] : nullptr);

    auto fail = [&](int code) -> ParseResult &
    {
        result.ok = false;
        result.exit_code = code;
        return result;
    };

    SeenOptions seen;
    bool end_of_options = false;

    ArgumentCursor cursor{ argc, argv, 1, prog };
    for (; cursor.index < argc; ++cursor.index)
    {
        const char *current = argv[cursor.index];
        // POSIX Guideline 10: "--" ends option parsing.
        if (!end_of_options && std::strcmp(current, "--") == 0)
        {
            end_of_options = true;
            continue;
        }
        if (end_of_options)
        {
            if (!args.model_path.empty())
            {
                std::fprintf(stderr, "%s: unexpected argument '%s'\n", prog, current);
                return fail(1);
            }
            args.model_path = current;
            continue;
        }

        // Split --flag=value. Other forms leave eq_val null.
        const char *eq_val = nullptr;
        std::string arg = current;
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-')
        {
            const size_t eq = arg.find('=');
            if (eq != std::string::npos)
            {
                eq_val = current + eq + 1;
                arg.resize(eq);
            }
        }
        const char *flag = arg.c_str();

        // Prefer =value, otherwise consume the next token.
        auto get_val = [&]() -> const char *
        {
            if (eq_val != nullptr)
            {
                return eq_val;
            }
            return cursor.require_value(flag);
        };

        if (const auto *option = find_long_option(BOOLEAN_OPTIONS, arg))
        {
            if (!reject_attached_value(prog, flag, eq_val))
            {
                return fail(1);
            }
            args.*(option->field) = option->value;
            continue;
        }

        if (const auto *option = find_long_option(ACTION_OPTIONS, arg))
        {
            if (!reject_attached_value(prog, flag, eq_val))
            {
                return fail(1);
            }
            option->handler(prog);
            return fail(0);
        }

        if (const auto *option = find_long_option(OPTIONAL_INTEGER_OPTIONS, arg))
        {
            if (!parse_optional_positive_int(cursor, flag, eq_val, args.*(option->field), option->bare_value))
            {
                return fail(1);
            }
            continue;
        }

        if (const auto *option = find_long_option(VALUE_OPTIONS, arg))
        {
            const char *value = get_val();
            if (!value || !option->handler(prog, flag, value, args, seen))
            {
                return fail(1);
            }
            continue;
        }
        // POSIX Guideline 5: cluster boolean options and put a value-taking option last.
        // Like getopt, -s=phong passes "=phong" as the value.
        if (current[0] == '-' && current[1] != '\0' && current[1] != '-')
        {
            bool value_consumed = false; // a value flag ate the rest of the token
            for (int k = 1; current[k] != '\0' && !value_consumed; k++)
            {
                const char c = current[k];
                const char short_flag[3] = { '-', c, '\0' };
                const char *rest = current + k + 1;

                if (const auto *option = find_short_option(BOOLEAN_OPTIONS, c))
                {
                    args.*(option->field) = option->value;
                    continue;
                }

                if (const auto *option = find_short_option(ACTION_OPTIONS, c))
                {
                    option->handler(prog);
                    return fail(0);
                }

                if (const auto *option = find_short_option(OPTIONAL_INTEGER_OPTIONS, c))
                {
                    if (!parse_optional_positive_int(
                            cursor, short_flag, (*rest != '\0') ? rest : nullptr, args.*(option->field),
                            option->bare_value
                        ))
                    {
                        return fail(1);
                    }
                    value_consumed = true;
                    continue;
                }

                if (const auto *option = find_short_option(VALUE_OPTIONS, c))
                {
                    const char *value = (*rest != '\0') ? rest : cursor.require_value(short_flag);
                    if (!value || !option->handler(prog, short_flag, value, args, seen))
                    {
                        return fail(1);
                    }
                    value_consumed = true;
                    continue;
                }

                std::fprintf(stderr, "%s: unknown flag '-%c'\n", prog, c);
                return fail(1);
            }
        }
        // A lone "-" is an operand. Any other leading dash here is an unknown flag.
        else if (current[0] == '-' && current[1] != '\0')
        {
            std::fprintf(stderr, "%s: unknown flag '%s'\n", prog, current);
            return fail(1);
        }
        else if (!args.model_path.empty())
        {
            std::fprintf(stderr, "%s: unexpected argument '%s'\n", prog, current);
            return fail(1);
        }
        else
        {
            args.model_path = current;
        }
    }

    if (seen.bench_size && args.bench < 1)
    {
        std::fprintf(stderr, "%s: --bench-size requires --bench\n", prog);
        return fail(1);
    }
    // First-person mode is session-fixed, so reject a speed that could never take effect.
    if (seen.first_person_speed && !args.first_person)
    {
        std::fprintf(stderr, "%s: --first-person-speed requires --first-person\n", prog);
        return fail(1);
    }
    if (seen.bench_warmup && args.bench < 1)
    {
        std::fprintf(stderr, "%s: --bench-warmup requires --bench\n", prog);
        return fail(1);
    }

    if (args.model_path.empty())
    {
        std::fprintf(
            stderr,
            "%s: no model specified\n"
            "Usage: %s [options] <model>\n"
            "Run '%s --help' for more information.\n",
            prog, prog, prog
        );
        return fail(1);
    }

    return result;
}
