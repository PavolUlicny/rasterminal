#include "args.h"

#include "platform.h"
#include "shading.h"
#include "version.h"

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
#include <string>
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

    // Frames a bare --bench/-B runs (both the long-form and cluster paths read this).
    constexpr int BENCH_DEFAULT_FRAMES = 200;

    // Lowercase-copy for case-insensitive value matching. unsigned char avoids the
    // std::tolower UB on a negative char; the flag values it sees are plain ASCII.
    // Deliberately NOT platform.h's ascii_lower/ieq: those live in platform::detail
    // (private to the terminal classifier), and a cold parse path doesn't justify
    // the first cross-file use of that namespace.
    std::string to_lower(const char *val)
    {
        std::string v = val;
        std::transform(
            v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
        );
        return v;
    }

    // Shared matcher for the named-value flags (enum values and boolean spellings):
    // case-insensitive lookup of val in names (aliases are just extra entries); on
    // miss prints the standard invalid-value diagnostic with the flag's
    // expected-values string.
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

} // namespace

ParseResult parse_args(int argc, char *argv[])
{
    ParseResult result;
    ParsedArgs &args = result.args;

    // Diagnostic prefix: the name the program was invoked as (basename of argv[0]).
    const char *prog = program_name(argc > 0 ? argv[0] : nullptr);

    auto fail = [&](int code) -> ParseResult &
    {
        result.ok = false;
        result.exit_code = code;
        return result;
    };

    // Returns the next argv token for a flag that requires a value.
    // Prints an error and returns nullptr if there is none.
    auto require_val = [&](int &i, const char *flag) -> const char *
    {
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "%s: %s requires a value\n", prog, flag);
            return nullptr;
        }
        return argv[++i];
    };

    // Positive-integer value shared by --threads/--fps/--bench (and their short forms).
    auto parse_pos_int = [prog](const char *flag, const char *val, int &out) -> bool
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
    };

    auto parse_nonneg_int = [prog](const char *flag, const char *val, int &out) -> bool
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
    };

    auto parse_size = [prog](const char *flag, const char *val, int &w, int &h) -> bool
    {
        auto err = [prog, flag, val]() -> bool
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
        // Cap the pixel count so w*h can't overflow size_t in the framebuffer (on ILP32
        // size_t is 32-bit and the product would silently wrap to a tiny buffer, then
        // rasterization writes out of bounds). Multiply in int64 because long is itself
        // 32-bit on ILP32. INT_MAX pixels is already absurd for a bench framebuffer; a
        // valid but large value just fails loud in the allocation instead.
        if (static_cast<int64_t>(ww) * hh > INT_MAX)
        {
            return err();
        }
        w = static_cast<int>(ww);
        h = static_cast<int>(hh);
        return true;
    };

    // Returns true if s is a non-empty string of ASCII digits only.
    auto is_all_digits = [](const char *s) -> bool
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
    };

    // Optional-integer flags (--threads/--fps/--bench and -j/-f/-B), one decision for
    // both spellings: attached is the =value (long form; present-but-empty still
    // parses, so "--fps=" errors) or the rest of the cluster token (nullptr when
    // none). With no attached value, the next argv token is consumed only if it is a
    // positive integer; otherwise the flag is bare and means bare_value.
    auto parse_opt_int = [&](int &arg_i, const char *flag, const char *attached, int &out, int bare_value) -> bool
    {
        if (attached != nullptr)
        {
            return parse_pos_int(flag, attached, out);
        }
        if (arg_i + 1 < argc && is_all_digits(argv[arg_i + 1]))
        {
            return parse_pos_int(flag, argv[++arg_i], out);
        }
        out = bare_value;
        return true;
    };

    auto parse_shading = [prog](const char *flag, const char *val, ShadingMode &out) -> bool
    {
        return parse_enum(
            prog, flag, val,
            { { "wireframe", ShadingMode::Wireframe }, { "flat", ShadingMode::Flat }, { "phong", ShadingMode::Phong } },
            "wireframe|flat|phong", out
        );
    };

    auto parse_bg = [prog](const char *flag, const char *val, Background &out) -> bool
    {
        return parse_enum(
            prog, flag, val,
            { { "black", Background::Black },
              { "gray", Background::Gray },
              { "grey", Background::Gray },
              { "white", Background::White } },
            "black|gray|white", out
        );
    };

    auto parse_lighting = [prog](const char *flag, const char *val, LightingMode &out) -> bool
    {
        return parse_enum(
            prog, flag, val,
            { { "dual", LightingMode::Dual }, { "single", LightingMode::Single }, { "flat", LightingMode::Flat } },
            "dual|single|flat", out
        );
    };

    auto parse_bool = [prog](const char *flag, const char *val, bool &out) -> bool
    {
        return parse_enum(
            prog, flag, val,
            { { "on", true },
              { "1", true },
              { "true", true },
              { "yes", true },
              { "y", true },
              { "off", false },
              { "0", false },
              { "false", false },
              { "no", false },
              { "n", false } },
            "on|off, 1|0, true|false, yes|no, y|n", out
        );
    };

    auto parse_wireframe_color = [prog](const char *flag, const char *val, WireframeColor &out) -> bool
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
    };

    auto parse_color = [prog](const char *flag, const char *val, ColorChoice &out) -> bool
    {
        return parse_enum(
            prog, flag, val,
            { { "auto", ColorChoice::Auto },
              { "truecolor", ColorChoice::TrueColor },
              { "24bit", ColorChoice::TrueColor },
              { "256", ColorChoice::Palette256 } },
            "truecolor|24bit|256|auto", out
        );
    };

    auto parse_angle = [prog](const char *flag, const char *val, float &out) -> bool
    {
        char *end = nullptr;
        errno = 0;
        const float v = std::strtof(val, &end);
        if (end == val || *end != '\0' || errno == ERANGE || !std::isfinite(v) || v < 0.0f || v > 180.0f)
        {
            std::fprintf(stderr, "%s: %s: invalid value '%s' (expected a number in [0, 180])\n", prog, flag, val);
            return false;
        }
        out = v;
        return true;
    };

    auto print_version = []()
    {
        // UTF-8 author bytes need the console CP set before printing — this path exits
        // before the main loop's enable_raw_mode would do it. Other early-exit paths
        // (--help, --bench, errors) print pure ASCII, so they leave console state alone.
        // The return (VT support) is ignored: --version wants only the code page. But
        // init_console_output sets the CP solely when stdout is a VT-capable console, so for
        // redirected/piped stdout or a VT-incapable console the CP is left as-is. That is
        // harmless for a file or a UTF-8-aware consumer (the raw bytes are UTF-8 regardless);
        // only display through a non-UTF-8 console (a legacy console directly, or a downstream
        // console pager like `... | more`) shows the accented author name as mojibake.
        // Acceptable: the code page is a persistent console-wide side effect not worth forcing
        // for --version.
        platform::init_console_output();
        // GNU-standard --version block (cf. gnulib version-etc): canonical name +
        // version, copyright, license, generic free-software/no-warranty blurb,
        // then a blank line and the author. Name is a constant, never argv[0].
        std::printf(
            "rasterminal %s\n"
            "Copyright (C) %s %s\n"
            "License MIT: <https://opensource.org/license/mit>\n"
            "This is free software: you are free to change and redistribute it.\n"
            "There is NO WARRANTY, to the extent permitted by law.\n"
            "\n"
            "Written by %s.\n",
            RASTERMINAL_VERSION, RASTERMINAL_COPYRIGHT_YEAR, RASTERMINAL_AUTHOR, RASTERMINAL_AUTHOR
        );
    };

    auto print_help = [prog]()
    {
        std::printf(
            "Usage: %s [options] <model>\n"
            "\n"
            "Render a 3D model in the terminal using unicode half-block characters\n"
            "and 24-bit ANSI color (256-color fallback).\n"
            "\n"
            "Supported formats:\n"
            "  .obj        Wavefront OBJ with optional .mtl (diffuse/specular/normal maps)\n"
            "  .ply        ASCII or binary (little/big-endian)\n"
            "  .stl        ASCII or binary\n"
            "  .gltf/.glb  glTF 2.0 (PBR materials, textures, node transforms)\n"
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
            "  -c,     --cull <on|off>        Backface culling initial state (default: on)\n"
            "                                  on|off, 1|0, true|false, yes|no, y|n\n"
            "  -t,     --texture <on|off>     Texture rendering initial state (default: on)\n"
            "                                  on|off, 1|0, true|false, yes|no, y|n\n"
            "  -S,     --spin                 Start with auto-rotation enabled\n"
            "  -j [N], --threads [N]          Worker threads (default: min(hw,4))\n"
            "                                  bare -j/--threads uses all cores, -j N uses N\n"
            "                                  N above the CPU thread count is clamped\n"
            "  -f [N], --fps [N]              Frame cap (default: 60)\n"
            "                                  bare -f/--fps uncapped, -f N caps at N fps\n"
            "  -B [N], --bench [N]            Headless benchmark: N frames (default: 200)\n"
            "                                  prints timing + fps + throughput to stderr\n"
            "          --bench-size WxH       Bench framebuffer size in pixels (default: 200x120)\n"
            "          --bench-warmup N       Bench warmup frames discarded (default: 20)\n"
            "          --smooth-angle DEG     Crease angle for computed normals (default: 60)\n"
            "                                  0=faceted, 180=smooth\n"
            "                                  ignored when an OBJ authors smoothing groups\n"
            "          --color <mode>         Color output (default: auto)\n"
            "                                  truecolor|24bit|256|auto\n"
            "                                  auto detects from COLORTERM/TERM\n"
            "          --no-ao                Disable ambient occlusion\n"
            "          --no-hud               Hide the HUD status line\n"
            "  -h,     --help                 Show this message\n"
            "  -V,     --version              Show version and exit\n"
            "\n"
            "Controls:\n"
            "  1-3          Shading mode           B       Cycle background\n"
            "  Space        Toggle spin            L       Cycle lighting\n"
            "  WASD/arrows  Orbit camera           R       Reset view\n"
            "  +/-          Zoom                   C       Cycle wireframe color\n"
            "  Mouse drag   Orbit                  K       Toggle backface culling\n"
            "  Scroll       Zoom                   T       Toggle textures\n"
            "  Q/Escape     Quit\n"
            "\n"
            "Report bugs to: <%s/issues>\n"
            "Home page: <%s>\n",
            prog, RASTERMINAL_HOMEPAGE, RASTERMINAL_HOMEPAGE
        );
    };

    bool saw_bench_size = false;
    bool saw_bench_warmup = false;
    bool end_of_options = false;

    for (int i = 1; i < argc; i++)
    {
        // POSIX Guideline 10: the first "--" terminates option parsing; every
        // token after it is a positional operand, even one beginning with '-'.
        if (!end_of_options && std::strcmp(argv[i], "--") == 0)
        {
            end_of_options = true;
            continue;
        }
        if (end_of_options)
        {
            if (!args.model_path.empty())
            {
                std::fprintf(stderr, "%s: unexpected argument '%s'\n", prog, argv[i]);
                return fail(1);
            }
            args.model_path = argv[i];
            continue;
        }

        // Split --flag=value → flag name + value pointer.
        // For --flag value or -f value forms, eq_val stays nullptr.
        const char *eq_val = nullptr;
        std::string arg = argv[i];
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-')
        {
            const size_t eq = arg.find('=');
            if (eq != std::string::npos)
            {
                eq_val = argv[i] + eq + 1;
                arg.resize(eq);
            }
        }
        const char *flag = arg.c_str();

        // Get value: inline =value, else consume next argv token.
        auto get_val = [&](int &arg_i) -> const char *
        {
            if (eq_val != nullptr)
            {
                return eq_val;
            }
            return require_val(arg_i, flag);
        };

        // Boolean long flags take no value: "--flag=anything" is an error.
        auto no_value = [&]() -> bool
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "%s: %s does not take a value\n", prog, flag);
                return false;
            }
            return true;
        };

        if (arg == "--threads")
        {
            // Bare = all threads (0).
            if (!parse_opt_int(i, flag, eq_val, args.n_threads, 0))
            {
                return fail(1);
            }
        }
        else if (arg == "--fps")
        {
            // Bare = uncapped (0).
            if (!parse_opt_int(i, flag, eq_val, args.fps, 0))
            {
                return fail(1);
            }
        }
        else if (arg == "--bench")
        {
            if (!parse_opt_int(i, flag, eq_val, args.bench, BENCH_DEFAULT_FRAMES))
            {
                return fail(1);
            }
        }
        else if (arg == "--bench-size")
        {
            const char *val = get_val(i);
            if (!val || !parse_size(flag, val, args.bench_width, args.bench_height))
            {
                return fail(1);
            }
            saw_bench_size = true;
        }
        else if (arg == "--bench-warmup")
        {
            const char *val = get_val(i);
            if (!val || !parse_nonneg_int(flag, val, args.bench_warmup))
            {
                return fail(1);
            }
            saw_bench_warmup = true;
        }
        else if (arg == "--smooth-angle")
        {
            const char *val = get_val(i);
            if (!val || !parse_angle(flag, val, args.smooth_angle))
            {
                return fail(1);
            }
        }
        else if (arg == "--color")
        {
            const char *val = get_val(i);
            if (!val || !parse_color(flag, val, args.color))
            {
                return fail(1);
            }
        }
        else if (arg == "--shading")
        {
            const char *val = get_val(i);
            if (!val || !parse_shading(flag, val, args.shading))
            {
                return fail(1);
            }
        }
        else if (arg == "--bg")
        {
            const char *val = get_val(i);
            if (!val || !parse_bg(flag, val, args.bg))
            {
                return fail(1);
            }
        }
        else if (arg == "--lighting")
        {
            const char *val = get_val(i);
            if (!val || !parse_lighting(flag, val, args.lighting))
            {
                return fail(1);
            }
        }
        else if (arg == "--help")
        {
            if (!no_value())
            {
                return fail(1);
            }
            print_help();
            return fail(0);
        }
        else if (arg == "--version")
        {
            if (!no_value())
            {
                return fail(1);
            }
            print_version();
            return fail(0);
        }
        else if (arg == "--spin")
        {
            if (!no_value())
            {
                return fail(1);
            }
            args.spin = true;
        }
        else if (arg == "--no-ao")
        {
            if (!no_value())
            {
                return fail(1);
            }
            args.ao = false;
        }
        else if (arg == "--no-hud")
        {
            if (!no_value())
            {
                return fail(1);
            }
            args.hud = false;
        }
        else if (arg == "--cull")
        {
            const char *val = get_val(i);
            if (!val || !parse_bool(flag, val, args.cull))
            {
                return fail(1);
            }
        }
        else if (arg == "--texture")
        {
            const char *val = get_val(i);
            if (!val || !parse_bool(flag, val, args.texture))
            {
                return fail(1);
            }
        }
        else if (arg == "--wireframe-color")
        {
            const char *val = get_val(i);
            if (!val || !parse_wireframe_color(flag, val, args.wireframe_color))
            {
                return fail(1);
            }
        }
        // Short-option cluster (POSIX Guideline 5): a run of no-argument options
        // bundled behind one '-', with at most one value-taking option last. A
        // single short flag ("-s", "-j8") is just a length-one cluster. The value
        // flag takes the rest of the token, or the next argv token if none follows
        // (getopt-style: "-s=phong" passes "=phong" as the value).
        else if (argv[i][0] == '-' && argv[i][1] != '\0' && argv[i][1] != '-')
        {
            const char *tok = argv[i];
            bool value_consumed = false; // a value flag ate the rest of the token
            for (int k = 1; tok[k] != '\0' && !value_consumed; k++)
            {
                const char c = tok[k];
                const char short_flag[3] = { '-', c, '\0' };
                const char *rest = tok + k + 1;

                switch (c)
                {
                case 'S':
                    args.spin = true;
                    break;
                case 'h':
                    print_help();
                    return fail(0);
                case 'V':
                    print_version();
                    return fail(0);
                case 's':
                case 'b':
                case 'l':
                case 'w':
                case 'c':
                case 't':
                {
                    const char *val = (*rest != '\0') ? rest : require_val(i, short_flag);
                    if (!val)
                    {
                        return fail(1);
                    }
                    bool valid = false;
                    switch (c)
                    {
                    case 's':
                        valid = parse_shading(short_flag, val, args.shading);
                        break;
                    case 'b':
                        valid = parse_bg(short_flag, val, args.bg);
                        break;
                    case 'l':
                        valid = parse_lighting(short_flag, val, args.lighting);
                        break;
                    case 'w':
                        valid = parse_wireframe_color(short_flag, val, args.wireframe_color);
                        break;
                    case 'c':
                        valid = parse_bool(short_flag, val, args.cull);
                        break;
                    case 't':
                        valid = parse_bool(short_flag, val, args.texture);
                        break;
                    default:
                        break; // unreachable (outer case labels); valid stays false -> fail
                    }
                    if (!valid)
                    {
                        return fail(1);
                    }
                    value_consumed = true;
                    break;
                }
                case 'j':
                case 'f':
                case 'B':
                {
                    int &out = (c == 'j') ? args.n_threads : (c == 'f') ? args.fps : args.bench;
                    const int bare = (c == 'B') ? BENCH_DEFAULT_FRAMES : 0;
                    if (!parse_opt_int(i, short_flag, (*rest != '\0') ? rest : nullptr, out, bare))
                    {
                        return fail(1);
                    }
                    value_consumed = true;
                    break;
                }
                default:
                    std::fprintf(stderr, "%s: unknown flag '-%c'\n", prog, c);
                    return fail(1);
                }
            }
        }
        // A lone "-" is an operand (POSIX stdin sentinel), not a flag: it has no
        // char after the dash, so it falls through to positional handling. What
        // reaches here with a leading '-' is an unknown long flag ("--foo").
        else if (argv[i][0] == '-' && argv[i][1] != '\0')
        {
            std::fprintf(stderr, "%s: unknown flag '%s'\n", prog, argv[i]);
            return fail(1);
        }
        else if (!args.model_path.empty())
        {
            std::fprintf(stderr, "%s: unexpected argument '%s'\n", prog, argv[i]);
            return fail(1);
        }
        else
        {
            args.model_path = argv[i];
        }
    }

    if (saw_bench_size && args.bench < 1)
    {
        std::fprintf(stderr, "%s: --bench-size requires --bench\n", prog);
        return fail(1);
    }
    if (saw_bench_warmup && args.bench < 1)
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
