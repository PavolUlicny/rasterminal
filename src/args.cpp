#include "args.h"

#include "platform.h"
#include "version.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

    auto parse_threads = [prog](const char *flag, const char *val, int &out) -> bool
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

    // Lowercase-copy for case-insensitive value matching. unsigned char avoids the
    // std::tolower UB on a negative char; the flag values it sees are plain ASCII.
    auto to_lower = [](const char *val) -> std::string
    {
        std::string v = val;
        std::transform(
            v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }
        );
        return v;
    };

    auto parse_shading = [prog, to_lower](const char *flag, const char *val, int &out) -> bool
    {
        const std::string v = to_lower(val);
        if (v == "wireframe" || v == "1")
        {
            out = 0;
        }
        else if (v == "flat" || v == "2")
        {
            out = 1;
        }
        else if (v == "phong" || v == "3")
        {
            out = 2;
        }
        else
        {
            std::fprintf(
                stderr,
                "%s: %s: invalid value '%s'"
                " (expected wireframe|flat|phong or 1-3)\n",
                prog, flag, val
            );
            return false;
        }
        return true;
    };

    auto parse_bg = [prog, to_lower](const char *flag, const char *val, int &out) -> bool
    {
        const std::string v = to_lower(val);
        if (v == "black" || v == "1")
        {
            out = 0;
        }
        else if (v == "gray" || v == "grey" || v == "2")
        {
            out = 1;
        }
        else if (v == "white" || v == "3")
        {
            out = 2;
        }
        else
        {
            std::fprintf(
                stderr,
                "%s: %s: invalid value '%s'"
                " (expected black|gray|white or 1-3)\n",
                prog, flag, val
            );
            return false;
        }
        return true;
    };

    auto parse_lighting = [prog, to_lower](const char *flag, const char *val, int &out) -> bool
    {
        const std::string v = to_lower(val);
        if (v == "dual" || v == "1")
        {
            out = 0;
        }
        else if (v == "single" || v == "2")
        {
            out = 1;
        }
        else if (v == "flat" || v == "3")
        {
            out = 2;
        }
        else
        {
            std::fprintf(
                stderr,
                "%s: %s: invalid value '%s'"
                " (expected dual|single|flat or 1-3)\n",
                prog, flag, val
            );
            return false;
        }
        return true;
    };

    auto parse_bool = [prog, to_lower](const char *flag, const char *val, bool &out) -> bool
    {
        const std::string v = to_lower(val);
        if (v == "on" || v == "1" || v == "true" || v == "yes" || v == "y")
        {
            out = true;
        }
        else if (v == "off" || v == "0" || v == "false" || v == "no" || v == "n")
        {
            out = false;
        }
        else
        {
            std::fprintf(
                stderr,
                "%s: %s: invalid value '%s'"
                " (expected on|off, 1|0, true|false, yes|no, y|n)\n",
                prog, flag, val
            );
            return false;
        }
        return true;
    };

    auto parse_wireframe_color = [prog, to_lower](const char *flag, const char *val, int &out) -> bool
    {
        const std::string v = to_lower(val);
        if (v == "white" || v == "1")
        {
            out = 0;
        }
        else if (v == "red" || v == "2")
        {
            out = 1;
        }
        else if (v == "green" || v == "3")
        {
            out = 2;
        }
        else if (v == "yellow" || v == "4")
        {
            out = 3;
        }
        else if (v == "cyan" || v == "5")
        {
            out = 4;
        }
        else if (v == "magenta" || v == "6")
        {
            out = 5;
        }
        else
        {
            std::fprintf(
                stderr,
                "%s: %s: invalid value '%s'"
                " (expected white|red|green|yellow|cyan|magenta or 1-6)\n",
                prog, flag, val
            );
            return false;
        }
        return true;
    };

    // No 1-indexed numeric aliases here: there is no runtime keybinding to mirror,
    // and "256" is itself numeric.
    auto parse_color = [prog, to_lower](const char *flag, const char *val, int &out) -> bool
    {
        const std::string v = to_lower(val);
        if (v == "auto")
        {
            out = 0;
        }
        else if (v == "truecolor" || v == "24bit")
        {
            out = 1;
        }
        else if (v == "256")
        {
            out = 2;
        }
        else
        {
            std::fprintf(
                stderr,
                "%s: %s: invalid value '%s'"
                " (expected truecolor|24bit|256|auto)\n",
                prog, flag, val
            );
            return false;
        }
        return true;
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
            "                                  wireframe|flat|phong  or  1-3\n"
            "  -b,     --bg <color>           Initial background color (default: black)\n"
            "                                  black|gray|white  or  1-3\n"
            "  -l,     --lighting <mode>      Initial lighting mode (default: dual)\n"
            "                                  dual|single|flat  or  1-3\n"
            "  -w,     --wireframe-color <c>  Initial wireframe color (default: white)\n"
            "                                  white|red|green|yellow|cyan|magenta  or  1-6\n"
            "  -c,     --cull <on|off>        Backface culling initial state (default: on)\n"
            "                                  on|off, 1|0, true|false, yes|no, y|n\n"
            "  -t,     --texture <on|off>     Texture rendering initial state (default: on)\n"
            "                                  on|off, 1|0, true|false, yes|no, y|n\n"
            "  -S,     --spin                 Start with auto-rotation enabled\n"
            "  -j [N], --threads [N]          Worker threads (default: min(hw,4))\n"
            "                                  bare -j/--threads uses all cores, -j N uses N\n"
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

        if (arg == "--threads")
        {
            // Bare form (no value, or next token is not a positive integer) = all threads.
            if (eq_val == nullptr && (i + 1 >= argc || !is_all_digits(argv[i + 1])))
            {
                args.n_threads = 0;
            }
            else
            {
                const char *val = get_val(i);
                if (!val || !parse_threads(flag, val, args.n_threads))
                {
                    return fail(1);
                }
            }
        }
        else if (arg == "--fps")
        {
            // Bare form (no value, or next token is not a positive integer) = uncapped.
            if (eq_val == nullptr && (i + 1 >= argc || !is_all_digits(argv[i + 1])))
            {
                args.fps = 0;
            }
            else
            {
                const char *val = get_val(i);
                if (!val || !parse_threads(flag, val, args.fps))
                {
                    return fail(1);
                }
            }
        }
        else if (arg == "--bench")
        {
            // Bare form (no value, or next token is not a positive integer) = 200 frames.
            if (eq_val == nullptr && (i + 1 >= argc || !is_all_digits(argv[i + 1])))
            {
                args.bench = 200;
            }
            else
            {
                const char *val = get_val(i);
                if (!val || !parse_threads(flag, val, args.bench))
                {
                    return fail(1);
                }
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
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "%s: %s does not take a value\n", prog, flag);
                return fail(1);
            }
            print_help();
            return fail(0);
        }
        else if (arg == "--version")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "%s: %s does not take a value\n", prog, flag);
                return fail(1);
            }
            print_version();
            return fail(0);
        }
        else if (arg == "--spin")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "%s: %s does not take a value\n", prog, flag);
                return fail(1);
            }
            args.spin = true;
        }
        else if (arg == "--no-ao")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "%s: %s does not take a value\n", prog, flag);
                return fail(1);
            }
            args.ao = false;
        }
        else if (arg == "--no-hud")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "%s: %s does not take a value\n", prog, flag);
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

                if (c == 'S')
                {
                    args.spin = true;
                }
                else if (c == 'h')
                {
                    print_help();
                    return fail(0);
                }
                else if (c == 'V')
                {
                    print_version();
                    return fail(0);
                }
                else if (c == 's' || c == 'b' || c == 'l' || c == 'w' || c == 'c' || c == 't')
                {
                    const char *val = (*rest != '\0') ? rest : require_val(i, short_flag);
                    if (!val)
                    {
                        return fail(1);
                    }
                    bool valid = false;
                    if (c == 's')
                    {
                        valid = parse_shading(short_flag, val, args.shading);
                    }
                    else if (c == 'b')
                    {
                        valid = parse_bg(short_flag, val, args.bg);
                    }
                    else if (c == 'l')
                    {
                        valid = parse_lighting(short_flag, val, args.lighting);
                    }
                    else if (c == 'w')
                    {
                        valid = parse_wireframe_color(short_flag, val, args.wireframe_color);
                    }
                    else if (c == 'c')
                    {
                        valid = parse_bool(short_flag, val, args.cull);
                    }
                    else
                    {
                        valid = parse_bool(short_flag, val, args.texture);
                    }
                    if (!valid)
                    {
                        return fail(1);
                    }
                    value_consumed = true;
                }
                else if (c == 'j' || c == 'f' || c == 'B')
                {
                    int &out = (c == 'j') ? args.n_threads : (c == 'f') ? args.fps : args.bench;
                    const int bare = (c == 'B') ? 200 : 0;
                    if (*rest != '\0')
                    {
                        if (!parse_threads(short_flag, rest, out))
                        {
                            return fail(1);
                        }
                    }
                    else if (i + 1 < argc && is_all_digits(argv[i + 1]))
                    {
                        if (!parse_threads(short_flag, argv[++i], out))
                        {
                            return fail(1);
                        }
                    }
                    else
                    {
                        out = bare;
                    }
                    value_consumed = true;
                }
                else
                {
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
