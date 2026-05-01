#include "args.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

ParseResult parse_args(int argc, char *argv[])
{
    ParseResult result;
    ParsedArgs &args = result.args;

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
            std::fprintf(stderr, "Error: %s requires a value\n", flag);
            return nullptr;
        }
        return argv[++i];
    };

    auto parse_threads = [](const char *flag, const char *val, int &out) -> bool
    {
        char *end = nullptr;
        errno = 0;
        long v = std::strtol(val, &end, 10);
        if (end == val || *end != '\0' || v <= 0 || v > INT_MAX || errno == ERANGE)
        {
            std::fprintf(stderr,
                         "Error: %s requires a positive integer, got '%s'\n",
                         flag, val);
            return false;
        }
        out = static_cast<int>(v);
        return true;
    };

    // Returns true if s is a non-empty string of ASCII digits only.
    auto is_all_digits = [](const char *s) -> bool
    {
        if (!s || !*s)
            return false;
        while (*s)
        {
            if (!std::isdigit(static_cast<unsigned char>(*s)))
                return false;
            ++s;
        }
        return true;
    };

    auto parse_shading = [](const char *flag, const char *val, int &out) -> bool
    {
        std::string v = val;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        if (v == "wireframe" || v == "1")
            out = 0;
        else if (v == "flat" || v == "2")
            out = 1;
        else if (v == "gouraud" || v == "3")
            out = 2;
        else if (v == "phong" || v == "4")
            out = 3;
        else
        {
            std::fprintf(stderr,
                         "Error: %s: invalid value '%s'"
                         " (expected wireframe|flat|gouraud|phong or 1-4)\n",
                         flag, val);
            return false;
        }
        return true;
    };

    auto parse_bg = [](const char *flag, const char *val, int &out) -> bool
    {
        std::string v = val;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        if (v == "black" || v == "1")
            out = 0;
        else if (v == "gray" || v == "grey" || v == "2")
            out = 1;
        else if (v == "white" || v == "3")
            out = 2;
        else
        {
            std::fprintf(stderr,
                         "Error: %s: invalid value '%s'"
                         " (expected black|gray|white or 1-3)\n",
                         flag, val);
            return false;
        }
        return true;
    };

    auto parse_lighting = [](const char *flag, const char *val, int &out) -> bool
    {
        std::string v = val;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        if (v == "dual" || v == "1")
            out = 0;
        else if (v == "single" || v == "2")
            out = 1;
        else if (v == "flat" || v == "3")
            out = 2;
        else
        {
            std::fprintf(stderr,
                         "Error: %s: invalid value '%s'"
                         " (expected dual|single|flat or 1-3)\n",
                         flag, val);
            return false;
        }
        return true;
    };

    auto parse_bool = [](const char *flag, const char *val, bool &out) -> bool
    {
        std::string v = val;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        if (v == "on" || v == "1" || v == "true" || v == "yes" || v == "y")
            out = true;
        else if (v == "off" || v == "0" || v == "false" || v == "no" || v == "n")
            out = false;
        else
        {
            std::fprintf(stderr,
                         "Error: %s: invalid value '%s'"
                         " (expected on|off, 1|0, true|false, yes|no, y|n)\n",
                         flag, val);
            return false;
        }
        return true;
    };

    auto parse_wireframe_color = [](const char *flag, const char *val, int &out) -> bool
    {
        std::string v = val;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        if (v == "white" || v == "1")
            out = 0;
        else if (v == "red" || v == "2")
            out = 1;
        else if (v == "green" || v == "3")
            out = 2;
        else if (v == "yellow" || v == "4")
            out = 3;
        else if (v == "cyan" || v == "5")
            out = 4;
        else if (v == "magenta" || v == "6")
            out = 5;
        else
        {
            std::fprintf(stderr,
                         "Error: %s: invalid value '%s'"
                         " (expected white|red|green|yellow|cyan|magenta or 1-6)\n",
                         flag, val);
            return false;
        }
        return true;
    };

    for (int i = 1; i < argc; i++)
    {
        // Split --flag=value → flag name + value pointer.
        // For --flag value or -f value forms, eq_val stays nullptr.
        const char *eq_val = nullptr;
        std::string arg = argv[i];
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-')
        {
            size_t eq = arg.find('=');
            if (eq != std::string::npos)
            {
                eq_val = argv[i] + eq + 1;
                arg = arg.substr(0, eq);
            }
        }
        const char *flag = arg.c_str();

        // Get value: inline =value, else consume next argv token.
        auto get_val = [&](int &arg_i) -> const char *
        {
            if (eq_val != nullptr)
                return eq_val;
            return require_val(arg_i, flag);
        };

        if (arg == "-j" || arg == "--threads")
        {
            // Bare form (no value, or next token is not a positive integer) = all threads.
            if (eq_val == nullptr && (i + 1 >= argc || !is_all_digits(argv[i + 1])))
                args.n_threads = 0;
            else
            {
                const char *val = get_val(i);
                if (!val || !parse_threads(flag, val, args.n_threads))
                    return fail(1);
            }
        }
        else if (std::strncmp(argv[i], "-j", 2) == 0 &&
                 argv[i][2] != '\0' && argv[i][2] != '=' && eq_val == nullptr)
        {
            if (!parse_threads("-j", argv[i] + 2, args.n_threads))
                return fail(1);
        }
        else if (arg == "-f" || arg == "--fps")
        {
            // Bare form (no value, or next token is not a positive integer) = uncapped.
            if (eq_val == nullptr && (i + 1 >= argc || !is_all_digits(argv[i + 1])))
                args.fps = 0;
            else
            {
                const char *val = get_val(i);
                if (!val || !parse_threads(flag, val, args.fps))
                    return fail(1);
            }
        }
        else if (std::strncmp(argv[i], "-f", 2) == 0 &&
                 argv[i][2] != '\0' && argv[i][2] != '=' && eq_val == nullptr)
        {
            if (!parse_threads("-f", argv[i] + 2, args.fps))
                return fail(1);
        }
        else if (arg == "-s" || arg == "--shading")
        {
            const char *val = get_val(i);
            if (!val || !parse_shading(flag, val, args.shading))
                return fail(1);
        }
        else if (std::strncmp(argv[i], "-s", 2) == 0 &&
                 argv[i][2] != '\0' && argv[i][2] != '=' && eq_val == nullptr)
        {
            if (!parse_shading("-s", argv[i] + 2, args.shading))
                return fail(1);
        }
        else if (arg == "-b" || arg == "--bg")
        {
            const char *val = get_val(i);
            if (!val || !parse_bg(flag, val, args.bg))
                return fail(1);
        }
        else if (std::strncmp(argv[i], "-b", 2) == 0 &&
                 argv[i][2] != '\0' && argv[i][2] != '=' && eq_val == nullptr)
        {
            if (!parse_bg("-b", argv[i] + 2, args.bg))
                return fail(1);
        }
        else if (arg == "-l" || arg == "--lighting")
        {
            const char *val = get_val(i);
            if (!val || !parse_lighting(flag, val, args.lighting))
                return fail(1);
        }
        else if (std::strncmp(argv[i], "-l", 2) == 0 &&
                 argv[i][2] != '\0' && argv[i][2] != '=' && eq_val == nullptr)
        {
            if (!parse_lighting("-l", argv[i] + 2, args.lighting))
                return fail(1);
        }
        else if (arg == "-h" || arg == "--help")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "Error: %s does not take a value\n", flag);
                return fail(1);
            }
            std::printf(
                "Usage: rasterminal [options] <model>\n"
                "\n"
                "Render a 3D model in the terminal using unicode half-block characters\n"
                "and 24-bit ANSI color.\n"
                "\n"
                "Supported formats:\n"
                "  .obj        Wavefront OBJ with optional .mtl (diffuse/specular/normal maps)\n"
                "  .ply        ASCII or binary (little/big-endian)\n"
                "  .stl        ASCII or binary\n"
                "  .gltf/.glb  glTF 2.0 (PBR materials, textures, node transforms)\n"
                "\n"
                "Options:\n"
                "  -s,     --shading <mode>       Initial shading mode (default: gouraud)\n"
                "                                  wireframe|flat|gouraud|phong  or  1-4\n"
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
                "  -j [N], --threads [N]          Worker threads: bare -j/--threads uses all, -j N uses N (default: min(hw,4))\n"
                "  -f [N], --fps [N]              Frame cap: bare -f/--fps uncapped, -f N caps at N fps (default: 60)\n"
                "          --no-shadow            Disable shadow map\n"
                "          --no-ao                Disable ambient occlusion\n"
                "          --no-hud               Hide the HUD status line\n"
                "  -h,     --help                 Show this message\n"
                "\n"
                "Controls:\n"
                "  1-4          Shading mode           B       Cycle background\n"
                "  Space        Toggle spin            L       Cycle lighting\n"
                "  WASD/arrows  Orbit camera           R       Reset view\n"
                "  +/-          Zoom                   C       Cycle wireframe color\n"
                "  Mouse drag   Orbit                  K       Toggle backface culling\n"
                "  Scroll       Zoom                   T       Toggle textures\n"
                "  Q/Escape     Quit\n");
            return fail(0);
        }
        else if (arg == "-S" || arg == "--spin")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "Error: %s does not take a value\n", flag);
                return fail(1);
            }
            args.spin = true;
        }
        else if (arg == "--no-ao")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "Error: %s does not take a value\n", flag);
                return fail(1);
            }
            args.ao = false;
        }
        else if (arg == "--no-shadow")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "Error: %s does not take a value\n", flag);
                return fail(1);
            }
            args.shadow = false;
        }
        else if (arg == "--no-hud")
        {
            if (eq_val != nullptr)
            {
                std::fprintf(stderr, "Error: %s does not take a value\n", flag);
                return fail(1);
            }
            args.hud = false;
        }
        else if (arg == "-c" || arg == "--cull")
        {
            const char *val = get_val(i);
            if (!val || !parse_bool(flag, val, args.cull))
                return fail(1);
        }
        else if (std::strncmp(argv[i], "-c", 2) == 0 &&
                 argv[i][2] != '\0' && argv[i][2] != '=' && eq_val == nullptr)
        {
            if (!parse_bool("-c", argv[i] + 2, args.cull))
                return fail(1);
        }
        else if (arg == "-t" || arg == "--texture")
        {
            const char *val = get_val(i);
            if (!val || !parse_bool(flag, val, args.texture))
                return fail(1);
        }
        else if (std::strncmp(argv[i], "-t", 2) == 0 &&
                 argv[i][2] != '\0' && argv[i][2] != '=' && eq_val == nullptr)
        {
            if (!parse_bool("-t", argv[i] + 2, args.texture))
                return fail(1);
        }
        else if (arg == "-w" || arg == "--wireframe-color")
        {
            const char *val = get_val(i);
            if (!val || !parse_wireframe_color(flag, val, args.wireframe_color))
                return fail(1);
        }
        else if (std::strncmp(argv[i], "-w", 2) == 0 &&
                 argv[i][2] != '\0' && argv[i][2] != '=' && eq_val == nullptr)
        {
            if (!parse_wireframe_color("-w", argv[i] + 2, args.wireframe_color))
                return fail(1);
        }
        else if (argv[i][0] == '-')
        {
            std::fprintf(stderr, "Error: unknown flag '%s'\n", argv[i]);
            return fail(1);
        }
        else if (!args.model_path.empty())
        {
            std::fprintf(stderr, "Error: unexpected argument '%s'\n", argv[i]);
            return fail(1);
        }
        else
            args.model_path = argv[i];
    }

    if (args.model_path.empty())
    {
        std::fprintf(stderr,
                     "Error: no model specified\n"
                     "       Usage: rasterminal [options] <model>\n"
                     "       Run 'rasterminal --help' for more information.\n");
        return fail(1);
    }

    return result;
}
