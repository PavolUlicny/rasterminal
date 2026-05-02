#include "test.h"
#include "../src/args.h"

#include <cstdio>
#include <vector>

// Build a fake argv and call parse_args with stdout/stderr redirected to
// /dev/null so help text and error messages don't pollute the test output.
static ParseResult run(std::initializer_list<const char *> tokens)
{
    std::vector<const char *> argv{"rasterminal"};
    for (auto t : tokens)
        argv.push_back(t);

    // Flush any pending buffered output before touching the fds.
    std::fflush(stdout);
    std::fflush(stderr);
    int saved_out = test_dup(TEST_STDOUT);
    int saved_err = test_dup(TEST_STDERR);
    int devnull = test_devnull();
    test_dup2(devnull, TEST_STDOUT);
    test_dup2(devnull, TEST_STDERR);
    test_close(devnull);

    ParseResult result = parse_args(static_cast<int>(argv.size()),
                                    const_cast<char **>(argv.data()));

    // Drain any buffered output while fds still point to /dev/null, then restore.
    std::fflush(stdout);
    std::fflush(stderr);
    test_dup2(saved_out, TEST_STDOUT);
    test_dup2(saved_err, TEST_STDERR);
    test_close(saved_out);
    test_close(saved_err);

    return result;
}

// ─── defaults ─────────────────────────────────────────────────────────────────

TEST(args, defaults_when_only_model_given)
{
    ParseResult r = run({"model.obj"});
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.model_path == "model.obj");
    ASSERT_EQ(r.args.n_threads, -1);
    ASSERT_EQ(r.args.shading, 2);         // gouraud
    ASSERT_EQ(r.args.bg, 0);              // black
    ASSERT_EQ(r.args.lighting, 0);        // dual
    ASSERT_EQ(r.args.wireframe_color, 0); // white
    ASSERT_EQ(r.args.fps, 60);
    ASSERT_TRUE(r.args.cull);
    ASSERT_TRUE(r.args.texture);
    ASSERT_FALSE(r.args.spin);
    ASSERT_TRUE(r.args.shadow);
    ASSERT_TRUE(r.args.ao);
    ASSERT_TRUE(r.args.hud);
}

// ─── --help ───────────────────────────────────────────────────────────────────

TEST(args, help_short_exits_zero)
{
    ParseResult r = run({"-h"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

TEST(args, help_long_exits_zero)
{
    ParseResult r = run({"--help"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

TEST(args, help_does_not_require_model)
{
    // --help should exit 0 even with no model argument.
    ParseResult r = run({"--help"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

// ─── error: no model ─────────────────────────────────────────────────────────

TEST(args, no_model_is_error)
{
    ParseResult r = run({});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// ─── error: unknown flag ──────────────────────────────────────────────────────

TEST(args, unknown_long_flag_is_error)
{
    ParseResult r = run({"--foo", "model.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, unknown_short_flag_is_error)
{
    ParseResult r = run({"-z", "model.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// ─── error: extra positional ──────────────────────────────────────────────────

TEST(args, two_positional_args_is_error)
{
    ParseResult r = run({"model.obj", "extra"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// ─── error: missing value ─────────────────────────────────────────────────────

TEST(args, missing_value_for_shading_is_error)
{
    ParseResult r = run({"--shading"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, missing_value_for_bg_is_error)
{
    ParseResult r = run({"--bg"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, missing_value_for_lighting_is_error)
{
    ParseResult r = run({"--lighting"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// ─── error: short flags reject = form ────────────────────────────────────────

TEST(args, short_flag_equals_form_is_unknown_flag)
{
    ASSERT_FALSE(run({"-s=phong", "m.obj"}).ok);
    ASSERT_FALSE(run({"-b=white", "m.obj"}).ok);
    ASSERT_FALSE(run({"-l=dual", "m.obj"}).ok);
    ASSERT_FALSE(run({"-w=red", "m.obj"}).ok);
    ASSERT_FALSE(run({"-f=30", "m.obj"}).ok);
    ASSERT_FALSE(run({"-c=on", "m.obj"}).ok);
}

TEST(args, bare_threads_long_form_uses_all)
{
    ParseResult r = run({"--threads", "m.obj"});
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.n_threads, 0);
    ASSERT_TRUE(r.args.model_path == "m.obj");
}

// ─── error: boolean flags reject =value ──────────────────────────────────────

TEST(args, spin_with_equals_value_is_error)
{
    ParseResult r = run({"--spin=yes", "model.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, no_shadow_with_equals_value_is_error)
{
    ParseResult r = run({"--no-shadow=1", "model.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// ─── --shading ────────────────────────────────────────────────────────────────

TEST(args, shading_wireframe_by_name)
{
    ASSERT_EQ(run({"--shading", "wireframe", "m.obj"}).args.shading, 0);
}

TEST(args, shading_flat_by_name)
{
    ASSERT_EQ(run({"--shading", "flat", "m.obj"}).args.shading, 1);
}

TEST(args, shading_gouraud_by_name)
{
    ASSERT_EQ(run({"--shading", "gouraud", "m.obj"}).args.shading, 2);
}

TEST(args, shading_phong_by_name)
{
    ASSERT_EQ(run({"--shading", "phong", "m.obj"}).args.shading, 3);
}

TEST(args, shading_numeric_alias_1)
{
    ASSERT_EQ(run({"-s", "1", "m.obj"}).args.shading, 0);
}

TEST(args, shading_numeric_alias_4)
{
    ASSERT_EQ(run({"-s", "4", "m.obj"}).args.shading, 3);
}

TEST(args, shading_case_insensitive)
{
    ASSERT_EQ(run({"--shading", "PHONG", "m.obj"}).args.shading, 3);
    ASSERT_EQ(run({"--shading", "Gouraud", "m.obj"}).args.shading, 2);
}

TEST(args, shading_invalid_value_is_error)
{
    ParseResult r = run({"--shading", "raytraced", "m.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// ─── --shading compact and equals forms ──────────────────────────────────────

TEST(args, shading_compact_short_form)
{
    ASSERT_EQ(run({"-sphong", "m.obj"}).args.shading, 3);
    ASSERT_EQ(run({"-swireframe", "m.obj"}).args.shading, 0);
}

TEST(args, shading_equals_long_form)
{
    ASSERT_EQ(run({"--shading=flat", "m.obj"}).args.shading, 1);
}

TEST(args, shading_compact_case_insensitive)
{
    ASSERT_EQ(run({"-sPHONG", "m.obj"}).args.shading, 3);
    ASSERT_EQ(run({"-sFLAT", "m.obj"}).args.shading, 1);
}

// ─── --bg ─────────────────────────────────────────────────────────────────────

TEST(args, bg_black)
{
    ASSERT_EQ(run({"--bg", "black", "m.obj"}).args.bg, 0);
}

TEST(args, bg_gray)
{
    ASSERT_EQ(run({"--bg", "gray", "m.obj"}).args.bg, 1);
}

TEST(args, bg_grey_alias)
{
    ASSERT_EQ(run({"--bg", "grey", "m.obj"}).args.bg, 1);
}

TEST(args, bg_white)
{
    ASSERT_EQ(run({"--bg", "white", "m.obj"}).args.bg, 2);
}

TEST(args, bg_numeric_aliases)
{
    ASSERT_EQ(run({"-b", "1", "m.obj"}).args.bg, 0);
    ASSERT_EQ(run({"-b", "2", "m.obj"}).args.bg, 1);
    ASSERT_EQ(run({"-b", "3", "m.obj"}).args.bg, 2);
}

TEST(args, bg_compact_short_form)
{
    ASSERT_EQ(run({"-bwhite", "m.obj"}).args.bg, 2);
}

TEST(args, bg_equals_long_form)
{
    ASSERT_EQ(run({"--bg=gray", "m.obj"}).args.bg, 1);
}

TEST(args, bg_compact_case_insensitive)
{
    ASSERT_EQ(run({"-bWHITE", "m.obj"}).args.bg, 2);
    ASSERT_EQ(run({"-bGrAy", "m.obj"}).args.bg, 1);
}

TEST(args, bg_invalid_value_is_error)
{
    ParseResult r = run({"--bg", "red", "m.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// ─── --lighting ───────────────────────────────────────────────────────────────

TEST(args, lighting_dual)
{
    ASSERT_EQ(run({"--lighting", "dual", "m.obj"}).args.lighting, 0);
}

TEST(args, lighting_single)
{
    ASSERT_EQ(run({"--lighting", "single", "m.obj"}).args.lighting, 1);
}

TEST(args, lighting_flat)
{
    ASSERT_EQ(run({"--lighting", "flat", "m.obj"}).args.lighting, 2);
}

TEST(args, lighting_numeric_aliases)
{
    ASSERT_EQ(run({"-l", "1", "m.obj"}).args.lighting, 0);
    ASSERT_EQ(run({"-l", "2", "m.obj"}).args.lighting, 1);
    ASSERT_EQ(run({"-l", "3", "m.obj"}).args.lighting, 2);
}

TEST(args, lighting_compact_short_form)
{
    ASSERT_EQ(run({"-lsingle", "m.obj"}).args.lighting, 1);
}

TEST(args, lighting_equals_long_form)
{
    ASSERT_EQ(run({"--lighting=flat", "m.obj"}).args.lighting, 2);
}

TEST(args, lighting_compact_case_insensitive)
{
    ASSERT_EQ(run({"-lSINGLE", "m.obj"}).args.lighting, 1);
    ASSERT_EQ(run({"-lFlAt", "m.obj"}).args.lighting, 2);
}

TEST(args, lighting_invalid_value_is_error)
{
    ParseResult r = run({"--lighting", "ambient", "m.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// ─── --threads ────────────────────────────────────────────────────────────────

TEST(args, threads_bare_j_at_end)
{
    ParseResult r = run({"m.obj", "-j"});
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.n_threads, 0);
}

TEST(args, threads_bare_j_before_model)
{
    ParseResult r = run({"-j", "m.obj"});
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.n_threads, 0);
}

TEST(args, threads_bare_j_with_flag_after)
{
    ParseResult r = run({"-j", "--spin", "m.obj"});
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.n_threads, 0);
    ASSERT_TRUE(r.args.spin);
}

TEST(args, threads_positive_value)
{
    ParseResult r = run({"-j", "4", "m.obj"});
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.n_threads, 4);
    ASSERT_TRUE(r.args.model_path == "m.obj");
}

TEST(args, threads_compact_short_form)
{
    ASSERT_EQ(run({"-j8", "m.obj"}).args.n_threads, 8);
}

TEST(args, threads_equals_long_form)
{
    ASSERT_EQ(run({"--threads=2", "m.obj"}).args.n_threads, 2);
}

TEST(args, threads_zero_is_error)
{
    ParseResult r = run({"--threads", "0", "m.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, threads_negative_is_error)
{
    ParseResult r = run({"--threads=-1", "m.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, threads_non_integer_is_error)
{
    ParseResult r = run({"--threads", "fast", "m.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// ─── boolean flags ────────────────────────────────────────────────────────────

TEST(args, spin_flag_enables_spin)
{
    ASSERT_TRUE(run({"--spin", "m.obj"}).args.spin);
    ASSERT_TRUE(run({"-S", "m.obj"}).args.spin);
}

TEST(args, no_shadow_disables_shadow)
{
    ASSERT_FALSE(run({"--no-shadow", "m.obj"}).args.shadow);
}

TEST(args, no_ao_disables_ao)
{
    ASSERT_FALSE(run({"--no-ao", "m.obj"}).args.ao);
}

TEST(args, no_hud_disables_hud)
{
    ASSERT_FALSE(run({"--no-hud", "m.obj"}).args.hud);
}

// ─── combined flags ───────────────────────────────────────────────────────────

TEST(args, multiple_flags_all_applied)
{
    ParseResult r = run({"--shading=phong", "--bg=white", "--lighting=single",
                         "--wireframe-color=magenta", "--fps=120", "--cull=off",
                         "--texture=off", "--spin", "--no-shadow", "--no-ao", "--no-hud",
                         "-j2", "scene.ply"});
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.model_path == "scene.ply");
    ASSERT_EQ(r.args.shading, 3);
    ASSERT_EQ(r.args.bg, 2);
    ASSERT_EQ(r.args.lighting, 1);
    ASSERT_EQ(r.args.wireframe_color, 5);
    ASSERT_EQ(r.args.fps, 120);
    ASSERT_FALSE(r.args.cull);
    ASSERT_FALSE(r.args.texture);
    ASSERT_TRUE(r.args.spin);
    ASSERT_FALSE(r.args.shadow);
    ASSERT_FALSE(r.args.ao);
    ASSERT_FALSE(r.args.hud);
    ASSERT_EQ(r.args.n_threads, 2);
}

TEST(args, flags_before_and_after_model_path)
{
    // The model path can appear anywhere among the flags.
    ParseResult r = run({"--shading=flat", "my.stl", "--bg=gray"});
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.model_path == "my.stl");
    ASSERT_EQ(r.args.shading, 1);
    ASSERT_EQ(r.args.bg, 1);
}

// ─── --wireframe-color ────────────────────────────────────────────────────────

TEST(args, wireframe_color_default)
{
    ASSERT_EQ(run({"m.obj"}).args.wireframe_color, 0);
}

TEST(args, wireframe_color_name)
{
    ASSERT_EQ(run({"--wireframe-color", "red", "m.obj"}).args.wireframe_color, 1);
    ASSERT_EQ(run({"--wireframe-color", "green", "m.obj"}).args.wireframe_color, 2);
    ASSERT_EQ(run({"--wireframe-color", "yellow", "m.obj"}).args.wireframe_color, 3);
    ASSERT_EQ(run({"--wireframe-color", "cyan", "m.obj"}).args.wireframe_color, 4);
    ASSERT_EQ(run({"--wireframe-color", "magenta", "m.obj"}).args.wireframe_color, 5);
    ASSERT_EQ(run({"--wireframe-color", "white", "m.obj"}).args.wireframe_color, 0);
}

TEST(args, wireframe_color_numeric)
{
    ASSERT_EQ(run({"--wireframe-color", "1", "m.obj"}).args.wireframe_color, 0);
    ASSERT_EQ(run({"--wireframe-color", "6", "m.obj"}).args.wireframe_color, 5);
}

TEST(args, wireframe_color_case_insensitive)
{
    ASSERT_EQ(run({"--wireframe-color", "YELLOW", "m.obj"}).args.wireframe_color, 3);
    ASSERT_EQ(run({"--wireframe-color", "Cyan", "m.obj"}).args.wireframe_color, 4);
}

TEST(args, wireframe_color_short_form)
{
    ASSERT_EQ(run({"-w", "red", "m.obj"}).args.wireframe_color, 1);
}

TEST(args, wireframe_color_compact_short_form)
{
    ASSERT_EQ(run({"-wmagenta", "m.obj"}).args.wireframe_color, 5);
}

TEST(args, wireframe_color_compact_case_insensitive)
{
    ASSERT_EQ(run({"-wYELLOW", "m.obj"}).args.wireframe_color, 3);
    ASSERT_EQ(run({"-wCyAn", "m.obj"}).args.wireframe_color, 4);
}

TEST(args, wireframe_color_equals_form)
{
    ASSERT_EQ(run({"--wireframe-color=cyan", "m.obj"}).args.wireframe_color, 4);
}

TEST(args, wireframe_color_invalid_is_error)
{
    ParseResult r = run({"--wireframe-color", "blue", "m.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, wireframe_color_missing_value_is_error)
{
    ParseResult r = run({"--wireframe-color"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// ─── --fps ────────────────────────────────────────────────────────────────────

TEST(args, fps_default)
{
    ASSERT_EQ(run({"m.obj"}).args.fps, 60);
}

TEST(args, fps_bare_long_at_end)
{
    ParseResult r = run({"m.obj", "--fps"});
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.fps, 0);
}

TEST(args, fps_bare_short_before_model)
{
    ParseResult r = run({"-f", "m.obj"});
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.fps, 0);
}

TEST(args, fps_bare_with_flag_after)
{
    ParseResult r = run({"-f", "--spin", "m.obj"});
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.fps, 0);
    ASSERT_TRUE(r.args.spin);
}

TEST(args, fps_positive_value)
{
    ParseResult r = run({"-f", "30", "m.obj"});
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.fps, 30);
}

TEST(args, fps_compact_short_form)
{
    ASSERT_EQ(run({"-f120", "m.obj"}).args.fps, 120);
}

TEST(args, fps_equals_long_form)
{
    ASSERT_EQ(run({"--fps=144", "m.obj"}).args.fps, 144);
}

TEST(args, fps_zero_is_error)
{
    ParseResult r = run({"--fps", "0", "m.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, fps_negative_is_error)
{
    ParseResult r = run({"--fps=-1", "m.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, fps_non_integer_is_error)
{
    ParseResult r = run({"--fps", "fast", "m.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// ─── --cull ───────────────────────────────────────────────────────────────────

TEST(args, cull_default_is_on)
{
    ASSERT_TRUE(run({"m.obj"}).args.cull);
}

TEST(args, cull_on_values)
{
    ASSERT_TRUE(run({"--cull", "on", "m.obj"}).args.cull);
    ASSERT_TRUE(run({"--cull", "1", "m.obj"}).args.cull);
    ASSERT_TRUE(run({"--cull", "true", "m.obj"}).args.cull);
    ASSERT_TRUE(run({"--cull", "yes", "m.obj"}).args.cull);
    ASSERT_TRUE(run({"--cull", "y", "m.obj"}).args.cull);
}

TEST(args, cull_off_values)
{
    ASSERT_FALSE(run({"--cull", "off", "m.obj"}).args.cull);
    ASSERT_FALSE(run({"--cull", "0", "m.obj"}).args.cull);
    ASSERT_FALSE(run({"--cull", "false", "m.obj"}).args.cull);
    ASSERT_FALSE(run({"--cull", "no", "m.obj"}).args.cull);
    ASSERT_FALSE(run({"--cull", "n", "m.obj"}).args.cull);
}

TEST(args, cull_case_insensitive)
{
    ASSERT_TRUE(run({"--cull", "ON", "m.obj"}).args.cull);
    ASSERT_FALSE(run({"--cull", "OFF", "m.obj"}).args.cull);
    ASSERT_TRUE(run({"--cull", "True", "m.obj"}).args.cull);
    ASSERT_FALSE(run({"--cull", "FALSE", "m.obj"}).args.cull);
}

TEST(args, cull_short_form)
{
    ASSERT_FALSE(run({"-c", "off", "m.obj"}).args.cull);
    ASSERT_TRUE(run({"-c", "on", "m.obj"}).args.cull);
}

TEST(args, cull_compact_short_form)
{
    ASSERT_FALSE(run({"-coff", "m.obj"}).args.cull);
    ASSERT_TRUE(run({"-con", "m.obj"}).args.cull);
}

TEST(args, cull_compact_case_insensitive)
{
    ASSERT_FALSE(run({"-cOFF", "m.obj"}).args.cull);
    ASSERT_TRUE(run({"-cYES", "m.obj"}).args.cull);
}

TEST(args, cull_equals_long_form)
{
    ASSERT_FALSE(run({"--cull=off", "m.obj"}).args.cull);
    ASSERT_TRUE(run({"--cull=on", "m.obj"}).args.cull);
}

TEST(args, cull_missing_value_is_error)
{
    ParseResult r = run({"--cull"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, cull_invalid_value_is_error)
{
    ParseResult r = run({"--cull", "maybe", "m.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// ─── --texture ────────────────────────────────────────────────────────────────

TEST(args, texture_default_is_on)
{
    ASSERT_TRUE(run({"m.obj"}).args.texture);
}

TEST(args, texture_on_values)
{
    ASSERT_TRUE(run({"--texture", "on", "m.obj"}).args.texture);
    ASSERT_TRUE(run({"--texture", "1", "m.obj"}).args.texture);
    ASSERT_TRUE(run({"--texture", "true", "m.obj"}).args.texture);
    ASSERT_TRUE(run({"--texture", "yes", "m.obj"}).args.texture);
    ASSERT_TRUE(run({"--texture", "y", "m.obj"}).args.texture);
}

TEST(args, texture_off_values)
{
    ASSERT_FALSE(run({"--texture", "off", "m.obj"}).args.texture);
    ASSERT_FALSE(run({"--texture", "0", "m.obj"}).args.texture);
    ASSERT_FALSE(run({"--texture", "false", "m.obj"}).args.texture);
    ASSERT_FALSE(run({"--texture", "no", "m.obj"}).args.texture);
    ASSERT_FALSE(run({"--texture", "n", "m.obj"}).args.texture);
}

TEST(args, texture_case_insensitive)
{
    ASSERT_TRUE(run({"--texture", "ON", "m.obj"}).args.texture);
    ASSERT_FALSE(run({"--texture", "OFF", "m.obj"}).args.texture);
    ASSERT_TRUE(run({"--texture", "True", "m.obj"}).args.texture);
    ASSERT_FALSE(run({"--texture", "FALSE", "m.obj"}).args.texture);
}

TEST(args, texture_short_form)
{
    ASSERT_FALSE(run({"-t", "off", "m.obj"}).args.texture);
    ASSERT_TRUE(run({"-t", "on", "m.obj"}).args.texture);
}

TEST(args, texture_compact_short_form)
{
    ASSERT_FALSE(run({"-toff", "m.obj"}).args.texture);
    ASSERT_TRUE(run({"-ton", "m.obj"}).args.texture);
}

TEST(args, texture_compact_case_insensitive)
{
    ASSERT_FALSE(run({"-tOFF", "m.obj"}).args.texture);
    ASSERT_TRUE(run({"-tYES", "m.obj"}).args.texture);
}

TEST(args, texture_equals_long_form)
{
    ASSERT_FALSE(run({"--texture=off", "m.obj"}).args.texture);
    ASSERT_TRUE(run({"--texture=on", "m.obj"}).args.texture);
}

TEST(args, texture_missing_value_is_error)
{
    ParseResult r = run({"--texture"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, texture_invalid_value_is_error)
{
    ParseResult r = run({"--texture", "maybe", "m.obj"});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}
