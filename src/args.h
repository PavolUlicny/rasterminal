#pragma once

#include <string>

// Parsed command-line arguments.  All values are plain types — no renderer
// dependencies — so this header can be included by the test binary cheaply.
struct ParsedArgs
{
    std::string model_path;  // required positional
    int n_threads = -1;      // -1 = auto (min(hw_concurrency, 4))
    int shading = 2;         // 0=wireframe  1=flat  2=gouraud  3=phong
    int bg = 0;              // 0=black  1=gray  2=white
    int lighting = 0;        // 0=dual  1=single  2=flat
    int wireframe_color = 0; // 0=white, 1=red, 2=green, 3=yellow, 4=cyan, 5=magenta
    int fps = 60;            // 0 = uncapped (set by bare -f), >0 = cap at this value
    int bench = -1;          // -1 = off; >=1 = run this many measured frames headlessly
    bool cull = true;
    bool texture = true;
    bool spin = false;
    bool shadow = true;
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

ParseResult parse_args(int argc, char *argv[]);
