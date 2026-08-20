#include "tests/test.h"
#include "src/args.h"

#include <cstdio>
#include <cstring>
#include <vector>

// Build a fake argv and call parse_args with stdout/stderr redirected to
// /dev/null so help text and error messages don't pollute the test output.
static ParseResult run(std::initializer_list<const char *> tokens)
{
    std::vector<const char *> argv{ "rasterminal" };
    for (const auto *t : tokens)
    {
        argv.push_back(t);
    }

    // Flush any pending buffered output before touching the fds.
    std::fflush(stdout);
    std::fflush(stderr);
    int saved_out = test_dup(TEST_STDOUT);
    int saved_err = test_dup(TEST_STDERR);
    int devnull = test_devnull();
    if (devnull >= 0)
    {
        test_dup2(devnull, TEST_STDOUT);
        test_dup2(devnull, TEST_STDERR);
        test_close(devnull);
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): parse_args takes char** (argv style)
    ParseResult result = parse_args(static_cast<int>(argv.size()), const_cast<char **>(argv.data()));

    // Drain any buffered output while fds still point to /dev/null, then restore.
    std::fflush(stdout);
    std::fflush(stderr);
    if (saved_out >= 0)
    {
        test_dup2(saved_out, TEST_STDOUT);
        test_close(saved_out);
    }
    if (saved_err >= 0)
    {
        test_dup2(saved_err, TEST_STDERR);
        test_close(saved_err);
    }

    return result;
}

// program_name (diagnostic prefix)

TEST(args, program_name_strips_unix_path)
{
    ASSERT_TRUE(std::strcmp(program_name("/usr/local/bin/rasterminal"), "rasterminal") == 0);
}

TEST(args, program_name_bare_is_unchanged)
{
    ASSERT_TRUE(std::strcmp(program_name("rasterminal"), "rasterminal") == 0);
}

TEST(args, program_name_strips_relative_prefix)
{
    ASSERT_TRUE(std::strcmp(program_name("./rasterminal"), "rasterminal") == 0);
}

TEST(args, program_name_strips_windows_path_keeps_exe)
{
    ASSERT_TRUE(std::strcmp(program_name("C:\\tools\\rasterminal.exe"), "rasterminal.exe") == 0);
}

TEST(args, program_name_strips_windows_mixed_separators)
{
    ASSERT_TRUE(std::strcmp(program_name("C:/Program Files\\app\\rasterminal.exe"), "rasterminal.exe") == 0);
}

TEST(args, program_name_strips_windows_unc_path)
{
    ASSERT_TRUE(std::strcmp(program_name("\\\\server\\share\\rasterminal.exe"), "rasterminal.exe") == 0);
}

TEST(args, program_name_windows_trailing_backslash_falls_back)
{
    ASSERT_TRUE(std::strcmp(program_name("C:\\tools\\"), "rasterminal") == 0);
}

TEST(args, program_name_null_falls_back)
{
    ASSERT_TRUE(std::strcmp(program_name(nullptr), "rasterminal") == 0);
}

TEST(args, program_name_empty_falls_back)
{
    ASSERT_TRUE(std::strcmp(program_name(""), "rasterminal") == 0);
}

TEST(args, program_name_trailing_separator_falls_back)
{
    ASSERT_TRUE(std::strcmp(program_name("/usr/bin/"), "rasterminal") == 0);
}

// defaults

TEST(args, defaults_when_only_model_given)
{
    ParseResult r = run({ "model.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.model_path == "model.obj");
    ASSERT_EQ(r.args.n_threads, -1);
    ASSERT_EQ(r.args.shading, ShadingMode::Phong);
    ASSERT_EQ(r.args.bg, Background::Black);
    ASSERT_EQ(r.args.lighting, LightingMode::Dual);
    ASSERT_EQ(r.args.wireframe_color, WireframeColor::White);
    ASSERT_EQ(r.args.fps, 30);
    ASSERT_TRUE(r.args.cull);
    ASSERT_TRUE(r.args.texture);
    ASSERT_FALSE(r.args.spin);
    ASSERT_TRUE(r.args.ao);
    ASSERT_TRUE(r.args.hud);
    ASSERT_TRUE(r.args.input);
    ASSERT_FALSE(r.args.first_person);
    ASSERT_NEAR(r.args.smooth_angle, 60.0f, 1e-6f);
    ASSERT_NEAR(r.args.spin_speed, 45.0f, 1e-6f);
    ASSERT_EQ(r.args.spin_direction, SpinDirection::Left);
    ASSERT_NEAR(r.args.yaw, 0.0f, 1e-6f);
    ASSERT_NEAR(r.args.pitch, -17.2f, 1e-6f);
    ASSERT_NEAR(r.args.zoom, 1.0f, 1e-6f);
}

// --help

TEST(args, help_short_exits_zero)
{
    ParseResult r = run({ "-h" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

TEST(args, help_long_exits_zero)
{
    ParseResult r = run({ "--help" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

// --version

TEST(args, version_short_exits_zero)
{
    ParseResult r = run({ "-V" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

TEST(args, version_long_exits_zero)
{
    ParseResult r = run({ "--version" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

TEST(args, version_with_equals_value_is_error)
{
    ParseResult r = run({ "--version=1", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, version_clusters_after_boolean)
{
    ParseResult r = run({ "-SV" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

TEST(args, no_model_is_error)
{
    ParseResult r = run({});
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// error: unknown flag

TEST(args, unknown_long_flag_is_error)
{
    ParseResult r = run({ "--foo", "model.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, unknown_short_flag_is_error)
{
    ParseResult r = run({ "-z", "model.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// lone "-" operand (POSIX stdin sentinel)

TEST(args, lone_dash_is_model_path)
{
    ParseResult r = run({ "-" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.model_path == "-");
}

TEST(args, lone_dash_after_flag)
{
    ParseResult r = run({ "-s", "flat", "-" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.shading, ShadingMode::Flat);
    ASSERT_TRUE(r.args.model_path == "-");
}

TEST(args, lone_dash_then_operand_is_error)
{
    // '-' is the (only) operand; a second operand is one too many.
    ParseResult r = run({ "-", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, operand_then_lone_dash_is_error)
{
    ParseResult r = run({ "m.obj", "-" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// error: extra positional

TEST(args, two_positional_args_is_error)
{
    ParseResult r = run({ "model.obj", "extra" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// "--" end-of-options terminator (POSIX Guideline 10)

TEST(args, double_dash_allows_dash_prefixed_model)
{
    ParseResult r = run({ "--", "-file.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.model_path == "-file.obj");
}

TEST(args, double_dash_with_normal_model)
{
    ParseResult r = run({ "--", "model.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.model_path == "model.obj");
}

TEST(args, double_dash_after_flags)
{
    ParseResult r = run({ "-s", "flat", "--", "-m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.shading, ShadingMode::Flat);
    ASSERT_TRUE(r.args.model_path == "-m.obj");
}

TEST(args, double_dash_alone_is_no_model_error)
{
    ParseResult r = run({ "--" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, double_dash_two_operands_is_error)
{
    ParseResult r = run({ "--", "a.obj", "b.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, double_dash_does_not_trigger_help)
{
    ParseResult r = run({ "--", "--help" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.model_path == "--help");
}

TEST(args, double_dash_then_lone_dash_is_model)
{
    // A '-' after "--" is an ordinary operand, just like any other token.
    ParseResult r = run({ "--", "-" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.model_path == "-");
}

// error: missing value

TEST(args, missing_value_for_shading_is_error)
{
    ParseResult r = run({ "--shading" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, missing_value_for_bg_is_error)
{
    ParseResult r = run({ "--bg" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, missing_value_for_lighting_is_error)
{
    ParseResult r = run({ "--lighting" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// error: short flags reject = form

TEST(args, short_flag_equals_form_is_rejected)
{
    ASSERT_FALSE(run({ "-s=phong", "m.obj" }).ok);
    ASSERT_FALSE(run({ "-b=white", "m.obj" }).ok);
    ASSERT_FALSE(run({ "-l=dual", "m.obj" }).ok);
    ASSERT_FALSE(run({ "-w=red", "m.obj" }).ok);
    ASSERT_FALSE(run({ "-f=30", "m.obj" }).ok);
}

TEST(args, bare_threads_long_form_uses_all)
{
    ParseResult r = run({ "--threads", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.n_threads, 0);
    ASSERT_TRUE(r.args.model_path == "m.obj");
}

// --shading

TEST(args, shading_wireframe_by_name)
{
    ASSERT_EQ(run({ "--shading", "wireframe", "m.obj" }).args.shading, ShadingMode::Wireframe);
}

TEST(args, shading_flat_by_name)
{
    ASSERT_EQ(run({ "--shading", "flat", "m.obj" }).args.shading, ShadingMode::Flat);
}

TEST(args, shading_phong_by_name)
{
    ASSERT_EQ(run({ "--shading", "phong", "m.obj" }).args.shading, ShadingMode::Phong);
}

TEST(args, shading_numeric_is_invalid)
{
    // The 1-indexed numeric aliases were removed; only the named values are accepted.
    ASSERT_FALSE(run({ "-s", "1", "m.obj" }).ok);
    ASSERT_FALSE(run({ "-s", "3", "m.obj" }).ok);
    ASSERT_FALSE(run({ "-s", "4", "m.obj" }).ok);
}

TEST(args, shading_unknown_value_is_invalid)
{
    // An unrecognised shading name is rejected with the invalid-value exit code.
    ParseResult r = run({ "--shading", "spline", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, shading_case_insensitive)
{
    ASSERT_EQ(run({ "--shading", "PHONG", "m.obj" }).args.shading, ShadingMode::Phong);
    ASSERT_EQ(run({ "--shading", "Flat", "m.obj" }).args.shading, ShadingMode::Flat);
}

TEST(args, shading_invalid_value_is_error)
{
    ParseResult r = run({ "--shading", "raytraced", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --shading compact and equals forms

TEST(args, shading_compact_short_form)
{
    ASSERT_EQ(run({ "-sphong", "m.obj" }).args.shading, ShadingMode::Phong);
    ASSERT_EQ(run({ "-swireframe", "m.obj" }).args.shading, ShadingMode::Wireframe);
}

TEST(args, shading_equals_long_form)
{
    ASSERT_EQ(run({ "--shading=flat", "m.obj" }).args.shading, ShadingMode::Flat);
}

TEST(args, shading_compact_case_insensitive)
{
    ASSERT_EQ(run({ "-sPHONG", "m.obj" }).args.shading, ShadingMode::Phong);
    ASSERT_EQ(run({ "-sFLAT", "m.obj" }).args.shading, ShadingMode::Flat);
}

TEST(args, shading_compact_invalid_value_is_error)
{
    ASSERT_FALSE(run({ "-sbad", "m.obj" }).ok);
}

// --bg

TEST(args, bg_black)
{
    ASSERT_EQ(run({ "--bg", "black", "m.obj" }).args.bg, Background::Black);
}

TEST(args, bg_gray)
{
    ASSERT_EQ(run({ "--bg", "gray", "m.obj" }).args.bg, Background::Gray);
}

TEST(args, bg_grey_alias)
{
    ASSERT_EQ(run({ "--bg", "grey", "m.obj" }).args.bg, Background::Gray);
}

TEST(args, bg_white)
{
    ASSERT_EQ(run({ "--bg", "white", "m.obj" }).args.bg, Background::White);
}

TEST(args, bg_numeric_is_invalid)
{
    ASSERT_FALSE(run({ "-b", "1", "m.obj" }).ok);
    ASSERT_FALSE(run({ "-b", "2", "m.obj" }).ok);
    ASSERT_FALSE(run({ "-b", "3", "m.obj" }).ok);
}

TEST(args, bg_case_insensitive)
{
    ASSERT_EQ(run({ "--bg", "WHITE", "m.obj" }).args.bg, Background::White);
    ASSERT_EQ(run({ "--bg", "GrAy", "m.obj" }).args.bg, Background::Gray);
}

TEST(args, bg_compact_short_form)
{
    ASSERT_EQ(run({ "-bwhite", "m.obj" }).args.bg, Background::White);
}

TEST(args, bg_equals_long_form)
{
    ASSERT_EQ(run({ "--bg=gray", "m.obj" }).args.bg, Background::Gray);
}

TEST(args, bg_compact_case_insensitive)
{
    ASSERT_EQ(run({ "-bWHITE", "m.obj" }).args.bg, Background::White);
    ASSERT_EQ(run({ "-bGrAy", "m.obj" }).args.bg, Background::Gray);
}

TEST(args, bg_compact_invalid_value_is_error)
{
    ASSERT_FALSE(run({ "-bbad", "m.obj" }).ok);
}

TEST(args, bg_invalid_value_is_error)
{
    ParseResult r = run({ "--bg", "red", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --lighting

TEST(args, lighting_dual)
{
    ASSERT_EQ(run({ "--lighting", "dual", "m.obj" }).args.lighting, LightingMode::Dual);
}

TEST(args, lighting_single)
{
    ASSERT_EQ(run({ "--lighting", "single", "m.obj" }).args.lighting, LightingMode::Single);
}

TEST(args, lighting_flat)
{
    ASSERT_EQ(run({ "--lighting", "flat", "m.obj" }).args.lighting, LightingMode::Flat);
}

TEST(args, lighting_numeric_is_invalid)
{
    ASSERT_FALSE(run({ "-l", "1", "m.obj" }).ok);
    ASSERT_FALSE(run({ "-l", "2", "m.obj" }).ok);
    ASSERT_FALSE(run({ "-l", "3", "m.obj" }).ok);
}

TEST(args, lighting_case_insensitive)
{
    ASSERT_EQ(run({ "--lighting", "SINGLE", "m.obj" }).args.lighting, LightingMode::Single);
    ASSERT_EQ(run({ "--lighting", "FlAt", "m.obj" }).args.lighting, LightingMode::Flat);
}

TEST(args, lighting_compact_short_form)
{
    ASSERT_EQ(run({ "-lsingle", "m.obj" }).args.lighting, LightingMode::Single);
}

TEST(args, lighting_equals_long_form)
{
    ASSERT_EQ(run({ "--lighting=flat", "m.obj" }).args.lighting, LightingMode::Flat);
}

TEST(args, lighting_compact_case_insensitive)
{
    ASSERT_EQ(run({ "-lSINGLE", "m.obj" }).args.lighting, LightingMode::Single);
    ASSERT_EQ(run({ "-lFlAt", "m.obj" }).args.lighting, LightingMode::Flat);
}

TEST(args, lighting_compact_invalid_value_is_error)
{
    ASSERT_FALSE(run({ "-lbad", "m.obj" }).ok);
}

TEST(args, lighting_invalid_value_is_error)
{
    ParseResult r = run({ "--lighting", "ambient", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --threads

TEST(args, threads_bare_j_at_end)
{
    ParseResult r = run({ "m.obj", "-j" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.n_threads, 0);
}

TEST(args, threads_bare_j_before_model)
{
    ParseResult r = run({ "-j", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.n_threads, 0);
}

TEST(args, threads_bare_j_with_flag_after)
{
    ParseResult r = run({ "-j", "--spin", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.n_threads, 0);
    ASSERT_TRUE(r.args.spin);
}

TEST(args, threads_positive_value)
{
    ParseResult r = run({ "-j", "4", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.n_threads, 4);
    ASSERT_TRUE(r.args.model_path == "m.obj");
}

TEST(args, threads_compact_short_form)
{
    ASSERT_EQ(run({ "-j8", "m.obj" }).args.n_threads, 8);
}

TEST(args, threads_equals_long_form)
{
    ASSERT_EQ(run({ "--threads=2", "m.obj" }).args.n_threads, 2);
}

TEST(args, threads_zero_is_error)
{
    ParseResult r = run({ "--threads", "0", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, threads_negative_is_error)
{
    ParseResult r = run({ "--threads=-1", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, threads_non_integer_is_error)
{
    ParseResult r = run({ "--threads", "fast", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --spin / --no-spin

TEST(args, spin_default_is_off)
{
    ASSERT_FALSE(run({ "m.obj" }).args.spin);
}

TEST(args, spin_flag_enables_spin)
{
    ASSERT_TRUE(run({ "--spin", "m.obj" }).args.spin);
    ASSERT_TRUE(run({ "-S", "m.obj" }).args.spin);
}

TEST(args, no_spin_disables)
{
    ParseResult r = run({ "--no-spin", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_FALSE(r.args.spin);
}

TEST(args, spin_last_flag_wins)
{
    // Asserting ok/model_path too: neither form takes a value, so a regression
    // that consumed the following token would otherwise pass here vacuously.
    // The off order uses -S so the cluster-path assignment is covered too.
    ParseResult off = run({ "-S", "--no-spin", "m.obj" });
    ASSERT_TRUE(off.ok);
    ASSERT_TRUE(off.args.model_path == "m.obj");
    ASSERT_FALSE(off.args.spin);

    ParseResult on = run({ "--no-spin", "--spin", "m.obj" });
    ASSERT_TRUE(on.ok);
    ASSERT_TRUE(on.args.model_path == "m.obj");
    ASSERT_TRUE(on.args.spin);
}

TEST(args, spin_rejects_value)
{
    ParseResult r = run({ "--spin=yes", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, no_spin_rejects_value)
{
    ParseResult r = run({ "--no-spin=off", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --ao / --no-ao

TEST(args, ao_default_is_on)
{
    ASSERT_TRUE(run({ "m.obj" }).args.ao);
}

TEST(args, no_ao_disables)
{
    ParseResult r = run({ "--no-ao", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_FALSE(r.args.ao);
}

TEST(args, ao_last_flag_wins)
{
    // Asserting ok/model_path too: neither form takes a value, so a regression
    // that consumed the following token would otherwise pass here vacuously.
    ParseResult on = run({ "--no-ao", "--ao", "m.obj" });
    ASSERT_TRUE(on.ok);
    ASSERT_TRUE(on.args.model_path == "m.obj");
    ASSERT_TRUE(on.args.ao);

    ParseResult off = run({ "--ao", "--no-ao", "m.obj" });
    ASSERT_TRUE(off.ok);
    ASSERT_TRUE(off.args.model_path == "m.obj");
    ASSERT_FALSE(off.args.ao);
}

TEST(args, ao_rejects_value)
{
    ParseResult r = run({ "--ao=on", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, no_ao_rejects_value)
{
    ParseResult r = run({ "--no-ao=on", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --hud / --no-hud

TEST(args, hud_default_is_on)
{
    ASSERT_TRUE(run({ "m.obj" }).args.hud);
}

TEST(args, no_hud_disables)
{
    ParseResult r = run({ "--no-hud", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_FALSE(r.args.hud);
}

TEST(args, hud_last_flag_wins)
{
    // Asserting ok/model_path too: neither form takes a value, so a regression
    // that consumed the following token would otherwise pass here vacuously.
    ParseResult on = run({ "--no-hud", "--hud", "m.obj" });
    ASSERT_TRUE(on.ok);
    ASSERT_TRUE(on.args.model_path == "m.obj");
    ASSERT_TRUE(on.args.hud);

    ParseResult off = run({ "--hud", "--no-hud", "m.obj" });
    ASSERT_TRUE(off.ok);
    ASSERT_TRUE(off.args.model_path == "m.obj");
    ASSERT_FALSE(off.args.hud);
}

TEST(args, hud_rejects_value)
{
    ParseResult r = run({ "--hud=on", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, no_hud_rejects_value)
{
    ParseResult r = run({ "--no-hud=on", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --input / --no-input

TEST(args, input_default_is_on)
{
    ASSERT_TRUE(run({ "m.obj" }).args.input);
}

TEST(args, no_input_disables)
{
    ParseResult r = run({ "--no-input", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_FALSE(r.args.input);
}

TEST(args, input_last_flag_wins)
{
    // Asserting ok/model_path too: neither form takes a value, so a regression
    // that consumed the following token would otherwise pass here vacuously.
    ParseResult on = run({ "--no-input", "--input", "m.obj" });
    ASSERT_TRUE(on.ok);
    ASSERT_TRUE(on.args.model_path == "m.obj");
    ASSERT_TRUE(on.args.input);

    ParseResult off = run({ "--input", "--no-input", "m.obj" });
    ASSERT_TRUE(off.ok);
    ASSERT_TRUE(off.args.model_path == "m.obj");
    ASSERT_FALSE(off.args.input);
}

TEST(args, input_rejects_value)
{
    ParseResult r = run({ "--input=on", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, no_input_rejects_value)
{
    ParseResult r = run({ "--no-input=on", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --first-person / --no-first-person

TEST(args, first_person_default_is_off)
{
    ASSERT_FALSE(run({ "m.obj" }).args.first_person);
}

TEST(args, first_person_enables)
{
    ParseResult r = run({ "--first-person", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.first_person);
}

TEST(args, no_first_person_disables)
{
    ParseResult r = run({ "--no-first-person", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_FALSE(r.args.first_person);
}

TEST(args, first_person_last_flag_wins)
{
    ParseResult on = run({ "--no-first-person", "--first-person", "m.obj" });
    ASSERT_TRUE(on.ok);
    ASSERT_TRUE(on.args.first_person);

    ParseResult off = run({ "--first-person", "--no-first-person", "m.obj" });
    ASSERT_TRUE(off.ok);
    ASSERT_FALSE(off.args.first_person);
}

TEST(args, first_person_rejects_value)
{
    ParseResult r = run({ "--first-person=on", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, no_first_person_rejects_value)
{
    ParseResult r = run({ "--no-first-person=on", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --first-person-speed

TEST(args, first_person_speed_default_is_one)
{
    ASSERT_NEAR(run({ "m.obj" }).args.first_person_speed, 1.0f, 1e-6f);
}

TEST(args, first_person_speed_sets_value)
{
    ParseResult r = run({ "--first-person", "--first-person-speed", "3.5", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.first_person_speed, 3.5f, 1e-6f);
}

TEST(args, first_person_speed_equals_form)
{
    ParseResult r = run({ "--first-person", "--first-person-speed=0.25", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.first_person_speed, 0.25f, 1e-6f);
}

TEST(args, first_person_speed_accepts_range_bounds)
{
    ParseResult lo = run({ "--first-person", "--first-person-speed=0.05", "m.obj" });
    ASSERT_TRUE(lo.ok);
    ParseResult hi = run({ "--first-person", "--first-person-speed=20", "m.obj" });
    ASSERT_TRUE(hi.ok);
}

TEST(args, first_person_speed_below_min_is_error)
{
    ParseResult r = run({ "--first-person", "--first-person-speed=0.04", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, first_person_speed_above_max_is_error)
{
    ParseResult r = run({ "--first-person", "--first-person-speed=20.1", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, first_person_speed_rejects_non_numeric)
{
    ParseResult r = run({ "--first-person", "--first-person-speed=fast", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, first_person_speed_rejects_non_finite)
{
    ASSERT_FALSE(run({ "--first-person", "--first-person-speed=nan", "m.obj" }).ok);
    ASSERT_FALSE(run({ "--first-person", "--first-person-speed=inf", "m.obj" }).ok);
}

TEST(args, first_person_speed_missing_value_is_error)
{
    ParseResult r = run({ "--first-person", "--first-person-speed" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, first_person_speed_requires_first_person)
{
    // --first-person is session-fixed, so the value could never come into play; the
    // flag is rejected rather than silently ignored.
    ParseResult r = run({ "--first-person-speed=2", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, first_person_speed_rejected_when_first_person_is_turned_back_off)
{
    // The later flag wins for the pair, so this ends with first-person off.
    ParseResult r = run({ "--first-person", "--first-person-speed=2", "--no-first-person", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, first_person_speed_order_does_not_matter)
{
    // The requires-check runs after the whole command line is parsed, so the flags
    // may appear in either order.
    ParseResult r = run({ "--first-person-speed=2", "--first-person", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.first_person_speed, 2.0f, 1e-6f);
}

// combined flags

TEST(args, multiple_flags_all_applied)
{
    ParseResult r = run({ "--shading=phong", "--bg=white", "--lighting=single", "--wireframe-color=magenta",
                          "--fps=120", "--no-cull", "--no-texture", "--spin", "--no-ao", "--no-hud", "--no-input",
                          "--first-person", "-j2", "scene.ply" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.model_path == "scene.ply");
    ASSERT_EQ(r.args.shading, ShadingMode::Phong);
    ASSERT_EQ(r.args.bg, Background::White);
    ASSERT_EQ(r.args.lighting, LightingMode::Single);
    ASSERT_EQ(r.args.wireframe_color, WireframeColor::Magenta);
    ASSERT_EQ(r.args.fps, 120);
    ASSERT_FALSE(r.args.cull);
    ASSERT_FALSE(r.args.texture);
    ASSERT_TRUE(r.args.spin);
    ASSERT_FALSE(r.args.ao);
    ASSERT_FALSE(r.args.hud);
    ASSERT_FALSE(r.args.input);
    ASSERT_TRUE(r.args.first_person);
    ASSERT_EQ(r.args.n_threads, 2);
}

TEST(args, flags_before_and_after_model_path)
{
    // The model path can appear anywhere among the flags.
    ParseResult r = run({ "--shading=flat", "my.stl", "--bg=gray" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.model_path == "my.stl");
    ASSERT_EQ(r.args.shading, ShadingMode::Flat);
    ASSERT_EQ(r.args.bg, Background::Gray);
}

// short-option clustering (POSIX Guideline 5)

TEST(args, cluster_spin_help_exits_zero)
{
    ParseResult r = run({ "-Sh" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

TEST(args, cluster_help_first_exits_zero)
{
    // 'h' exits regardless of its position in the cluster.
    ParseResult r = run({ "-hS" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

TEST(args, cluster_spin_threads_compact)
{
    ParseResult r = run({ "-Sj4", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.n_threads, 4);
}

TEST(args, cluster_spin_threads_next_token)
{
    // Bare 'j' last in the cluster consumes the following positive integer.
    ParseResult r = run({ "-Sj", "4", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.n_threads, 4);
}

TEST(args, cluster_spin_shading_value)
{
    ParseResult r = run({ "-Ssphong", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.shading, ShadingMode::Phong);
}

TEST(args, cluster_spin_bare_bench)
{
    // Bare 'B' last in the cluster, no integer follows → default 200.
    ParseResult r = run({ "-SB", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.bench, 200);
}

TEST(args, cluster_value_flag_next_token)
{
    // A mandatory-value flag last in the cluster pulls the next argv token.
    ParseResult r = run({ "-Sb", "gray", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.bg, Background::Gray);
}

TEST(args, cluster_unknown_char_is_error)
{
    ParseResult r = run({ "-Sz", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, cluster_unknown_char_first_is_error)
{
    // Order does not matter: an unknown leading char errors before the valid 'S'.
    ParseResult r = run({ "-zS", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, single_short_flag_is_length_one_cluster)
{
    // A lone short flag is just a length-one cluster; the refactor must not
    // regress solo short flags.
    ParseResult r = run({ "-S", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.n_threads, -1);
}

TEST(args, cluster_repeated_spin)
{
    // 'S' is a no-arg flag and keeps the cluster going; repeats are idempotent.
    ParseResult r = run({ "-SS", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
}

TEST(args, cluster_many_spins_then_value_flag)
{
    ParseResult r = run({ "-SSSj4", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.n_threads, 4);
}

// Exit flags ('h'/'V') short-circuit the whole parse from anywhere in a cluster.

TEST(args, cluster_version_first_short_circuits)
{
    ParseResult r = run({ "-VS" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

TEST(args, cluster_help_short_circuits_before_unknown)
{
    // 'h' exits 0 before the unknown 'z' would error.
    ParseResult r = run({ "-hz" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

TEST(args, cluster_version_short_circuits_before_unknown)
{
    ParseResult r = run({ "-Vz" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 0);
}

// Optional-int flags ('j'/'f'/'B') last in a cluster: compact value, next-token
// value, or bare (default).

TEST(args, cluster_spin_threads_bare_no_digit)
{
    // 'j' ends the cluster and the next token is not an integer → all cores (0).
    ParseResult r = run({ "-Sj", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.n_threads, 0);
    ASSERT_TRUE(r.args.model_path == "m.obj");
}

TEST(args, cluster_spin_fps_compact)
{
    ParseResult r = run({ "-Sf120", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.fps, 120);
}

TEST(args, cluster_spin_fps_bare)
{
    // Bare 'f' → uncapped (0).
    ParseResult r = run({ "-Sf", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.fps, 0);
}

TEST(args, cluster_spin_fps_next_token)
{
    ParseResult r = run({ "-Sf", "120", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.fps, 120);
}

TEST(args, cluster_spin_bench_compact)
{
    ParseResult r = run({ "-SB50", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.bench, 50);
}

TEST(args, cluster_spin_bench_next_token)
{
    ParseResult r = run({ "-SB", "50", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.bench, 50);
}

// Mandatory-value flags ('s'/'b'/'l'/'w') in a cluster: compact value.

TEST(args, cluster_spin_bg_compact)
{
    // "white" contains 'w' and 'h' (themselves flags): proof the value is taken
    // verbatim and its chars are not re-parsed as further cluster flags.
    ParseResult r = run({ "-Sbwhite", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.bg, Background::White);
}

TEST(args, cluster_spin_lighting_compact)
{
    ParseResult r = run({ "-Slsingle", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.lighting, LightingMode::Single);
}

TEST(args, cluster_spin_wireframe_compact)
{
    ParseResult r = run({ "-Swred", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.wireframe_color, WireframeColor::Red);
}

// Mandatory-value flags last in a cluster pull the next argv token.

TEST(args, cluster_spin_shading_next_token)
{
    ParseResult r = run({ "-Ss", "phong", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_EQ(r.args.shading, ShadingMode::Phong);
}

// A value flag greedily consumes the rest of the token, so a later flag char is
// treated as that flag's value (and validated as such), never as its own flag.

TEST(args, cluster_value_flag_consumes_following_flag_char)
{
    // 'l' after the bg flag is "-b"'s value → invalid background → error.
    ParseResult r = run({ "-Sbl", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, cluster_optint_consumes_following_flag_char)
{
    // 'S' after the fps flag is "-f"'s value → not an integer → error.
    ParseResult r = run({ "-SfS", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// Error edges inside a cluster.

TEST(args, cluster_value_flag_missing_value_is_error)
{
    // 'b' ends the cluster with no following token → missing value.
    ParseResult r = run({ "-Sb" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, cluster_value_flag_equals_is_rejected)
{
    // Same getopt-style rule as a solo short flag: "=phong" is the value verbatim.
    ParseResult r = run({ "-Ss=phong", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, cluster_threads_compact_nondigit_is_error)
{
    ParseResult r = run({ "-Sjx", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, cluster_then_double_dash_dash_model)
{
    // A cluster, then "--", then a '-'-prefixed operand: all three cooperate.
    ParseResult r = run({ "-S", "--", "-m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.args.spin);
    ASSERT_TRUE(r.args.model_path == "-m.obj");
}

// --wireframe-color

TEST(args, wireframe_color_default)
{
    ASSERT_EQ(run({ "m.obj" }).args.wireframe_color, WireframeColor::White);
}

TEST(args, wireframe_color_name)
{
    ASSERT_EQ(run({ "--wireframe-color", "red", "m.obj" }).args.wireframe_color, WireframeColor::Red);
    ASSERT_EQ(run({ "--wireframe-color", "green", "m.obj" }).args.wireframe_color, WireframeColor::Green);
    ASSERT_EQ(run({ "--wireframe-color", "yellow", "m.obj" }).args.wireframe_color, WireframeColor::Yellow);
    ASSERT_EQ(run({ "--wireframe-color", "cyan", "m.obj" }).args.wireframe_color, WireframeColor::Cyan);
    ASSERT_EQ(run({ "--wireframe-color", "magenta", "m.obj" }).args.wireframe_color, WireframeColor::Magenta);
    ASSERT_EQ(run({ "--wireframe-color", "white", "m.obj" }).args.wireframe_color, WireframeColor::White);
}

TEST(args, wireframe_color_numeric_is_invalid)
{
    ASSERT_FALSE(run({ "--wireframe-color", "1", "m.obj" }).ok);
    ASSERT_FALSE(run({ "--wireframe-color", "6", "m.obj" }).ok);
}

TEST(args, wireframe_color_case_insensitive)
{
    ASSERT_EQ(run({ "--wireframe-color", "YELLOW", "m.obj" }).args.wireframe_color, WireframeColor::Yellow);
    ASSERT_EQ(run({ "--wireframe-color", "Cyan", "m.obj" }).args.wireframe_color, WireframeColor::Cyan);
}

TEST(args, wireframe_color_short_form)
{
    ASSERT_EQ(run({ "-w", "red", "m.obj" }).args.wireframe_color, WireframeColor::Red);
}

TEST(args, wireframe_color_compact_short_form)
{
    ASSERT_EQ(run({ "-wmagenta", "m.obj" }).args.wireframe_color, WireframeColor::Magenta);
}

TEST(args, wireframe_color_compact_case_insensitive)
{
    ASSERT_EQ(run({ "-wYELLOW", "m.obj" }).args.wireframe_color, WireframeColor::Yellow);
    ASSERT_EQ(run({ "-wCyAn", "m.obj" }).args.wireframe_color, WireframeColor::Cyan);
}

TEST(args, wireframe_color_compact_invalid_value_is_error)
{
    ASSERT_FALSE(run({ "-wbad", "m.obj" }).ok);
}

TEST(args, wireframe_color_equals_form)
{
    ASSERT_EQ(run({ "--wireframe-color=cyan", "m.obj" }).args.wireframe_color, WireframeColor::Cyan);
}

TEST(args, wireframe_color_invalid_is_error)
{
    ParseResult r = run({ "--wireframe-color", "blue", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, wireframe_color_missing_value_is_error)
{
    ParseResult r = run({ "--wireframe-color" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --color

TEST(args, color_default_is_auto)
{
    ASSERT_EQ(run({ "m.obj" }).args.color, ColorChoice::Auto);
}

TEST(args, color_values)
{
    ASSERT_EQ(run({ "--color", "auto", "m.obj" }).args.color, ColorChoice::Auto);
    ASSERT_EQ(run({ "--color", "truecolor", "m.obj" }).args.color, ColorChoice::TrueColor);
    ASSERT_EQ(run({ "--color", "24bit", "m.obj" }).args.color, ColorChoice::TrueColor);
    ASSERT_EQ(run({ "--color", "256", "m.obj" }).args.color, ColorChoice::Palette256);
}

TEST(args, color_case_insensitive)
{
    ASSERT_EQ(run({ "--color", "TRUECOLOR", "m.obj" }).args.color, ColorChoice::TrueColor);
    ASSERT_EQ(run({ "--color", "24Bit", "m.obj" }).args.color, ColorChoice::TrueColor);
    ASSERT_EQ(run({ "--color", "Auto", "m.obj" }).args.color, ColorChoice::Auto);
}

TEST(args, color_equals_form)
{
    ASSERT_EQ(run({ "--color=256", "m.obj" }).args.color, ColorChoice::Palette256);
    ASSERT_EQ(run({ "--color=truecolor", "m.obj" }).args.color, ColorChoice::TrueColor);
}

TEST(args, color_invalid_value_is_error)
{
    // 16-color output is unsupported, so "16" is invalid like any other junk value.
    ParseResult r = run({ "--color", "16", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
    ASSERT_FALSE(run({ "--color", "always", "m.obj" }).ok);
}

TEST(args, color_has_no_numeric_aliases)
{
    // --color has no numeric aliases: "256" is itself a value, so a numeric "1"/"2"
    // must be rejected, not mapped to the internal 1=truecolor/2=256 encoding.
    ASSERT_FALSE(run({ "--color", "1", "m.obj" }).ok);
    ASSERT_FALSE(run({ "--color", "2", "m.obj" }).ok);
    ASSERT_FALSE(run({ "--color", "0", "m.obj" }).ok);
}

TEST(args, color_missing_value_is_error)
{
    ParseResult r = run({ "--color" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --graphics

TEST(args, graphics_default_is_auto)
{
    ASSERT_EQ(run({ "m.obj" }).args.graphics, GraphicsChoice::Auto);
}

TEST(args, graphics_values)
{
    ASSERT_EQ(run({ "--graphics", "auto", "m.obj" }).args.graphics, GraphicsChoice::Auto);
    ASSERT_EQ(run({ "--graphics", "kitty", "m.obj" }).args.graphics, GraphicsChoice::Kitty);
    ASSERT_EQ(run({ "--graphics", "sixel", "m.obj" }).args.graphics, GraphicsChoice::Sixel);
    ASSERT_EQ(run({ "--graphics", "blocks", "m.obj" }).args.graphics, GraphicsChoice::Blocks);
}

TEST(args, graphics_case_insensitive)
{
    ASSERT_EQ(run({ "--graphics", "KITTY", "m.obj" }).args.graphics, GraphicsChoice::Kitty);
    ASSERT_EQ(run({ "--graphics", "Sixel", "m.obj" }).args.graphics, GraphicsChoice::Sixel);
    ASSERT_EQ(run({ "--graphics", "Blocks", "m.obj" }).args.graphics, GraphicsChoice::Blocks);
}

TEST(args, graphics_equals_form)
{
    ASSERT_EQ(run({ "--graphics=kitty", "m.obj" }).args.graphics, GraphicsChoice::Kitty);
    ASSERT_EQ(run({ "--graphics=sixel", "m.obj" }).args.graphics, GraphicsChoice::Sixel);
    ASSERT_EQ(run({ "--graphics=blocks", "m.obj" }).args.graphics, GraphicsChoice::Blocks);
}

TEST(args, graphics_invalid_value_is_error)
{
    ParseResult r = run({ "--graphics", "iterm2", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
    ASSERT_FALSE(run({ "--graphics", "1", "m.obj" }).ok);
}

TEST(args, graphics_missing_value_is_error)
{
    ParseResult r = run({ "--graphics" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --fps

TEST(args, fps_default)
{
    ASSERT_EQ(run({ "m.obj" }).args.fps, 30);
}

TEST(args, fps_bare_long_at_end)
{
    ParseResult r = run({ "m.obj", "--fps" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.fps, 0);
}

TEST(args, fps_bare_short_before_model)
{
    ParseResult r = run({ "-f", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.fps, 0);
}

TEST(args, fps_bare_with_flag_after)
{
    ParseResult r = run({ "-f", "--spin", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.fps, 0);
    ASSERT_TRUE(r.args.spin);
}

TEST(args, fps_positive_value)
{
    ParseResult r = run({ "-f", "45", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.fps, 45);
}

TEST(args, fps_compact_short_form)
{
    ASSERT_EQ(run({ "-f120", "m.obj" }).args.fps, 120);
}

TEST(args, fps_equals_long_form)
{
    ASSERT_EQ(run({ "--fps=144", "m.obj" }).args.fps, 144);
}

TEST(args, fps_zero_is_error)
{
    ParseResult r = run({ "--fps", "0", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, fps_negative_is_error)
{
    ParseResult r = run({ "--fps=-1", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, fps_non_integer_is_error)
{
    ParseResult r = run({ "--fps", "fast", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --bench

TEST(args, bench_default_is_off)
{
    ASSERT_EQ(run({ "m.obj" }).args.bench, -1);
}

TEST(args, bench_bare_long_at_end)
{
    ParseResult r = run({ "m.obj", "--bench" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.bench, 200);
}

TEST(args, bench_bare_short_before_model)
{
    ParseResult r = run({ "-B", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.bench, 200);
}

TEST(args, bench_bare_with_flag_after)
{
    ParseResult r = run({ "-B", "--spin", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.bench, 200);
    ASSERT_TRUE(r.args.spin);
}

TEST(args, bench_positive_value)
{
    ParseResult r = run({ "-B", "50", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.bench, 50);
}

TEST(args, bench_compact_short_form)
{
    ASSERT_EQ(run({ "-B100", "m.obj" }).args.bench, 100);
}

TEST(args, bench_equals_long_form)
{
    ASSERT_EQ(run({ "--bench=500", "m.obj" }).args.bench, 500);
}

TEST(args, bench_zero_is_error)
{
    ParseResult r = run({ "--bench", "0", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_negative_is_error)
{
    ParseResult r = run({ "--bench=-1", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_non_integer_is_error)
{
    ParseResult r = run({ "--bench", "fast", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --cull / --no-cull

TEST(args, cull_default_is_on)
{
    ASSERT_TRUE(run({ "m.obj" }).args.cull);
}

TEST(args, no_cull_disables)
{
    ParseResult r = run({ "--no-cull", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_FALSE(r.args.cull);
}

TEST(args, cull_last_flag_wins)
{
    // Asserting ok/model_path too: neither form takes a value, so a regression
    // that consumed the following token would otherwise pass here vacuously.
    ParseResult on = run({ "--no-cull", "--cull", "m.obj" });
    ASSERT_TRUE(on.ok);
    ASSERT_TRUE(on.args.model_path == "m.obj");
    ASSERT_TRUE(on.args.cull);

    ParseResult off = run({ "--cull", "--no-cull", "m.obj" });
    ASSERT_TRUE(off.ok);
    ASSERT_TRUE(off.args.model_path == "m.obj");
    ASSERT_FALSE(off.args.cull);
}

TEST(args, cull_rejects_value)
{
    ParseResult r = run({ "--cull=on", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, no_cull_rejects_value)
{
    ParseResult r = run({ "--no-cull=off", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, cull_short_flag_is_unknown)
{
    // The -c short flag was removed along with the on|off value form.
    ParseResult r = run({ "-coff", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
    ASSERT_FALSE(run({ "-c", "off", "m.obj" }).ok);
}

// --texture / --no-texture

TEST(args, texture_default_is_on)
{
    ASSERT_TRUE(run({ "m.obj" }).args.texture);
}

TEST(args, no_texture_disables)
{
    ParseResult r = run({ "--no-texture", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_FALSE(r.args.texture);
}

TEST(args, texture_last_flag_wins)
{
    ParseResult on = run({ "--no-texture", "--texture", "m.obj" });
    ASSERT_TRUE(on.ok);
    ASSERT_TRUE(on.args.model_path == "m.obj");
    ASSERT_TRUE(on.args.texture);

    ParseResult off = run({ "--texture", "--no-texture", "m.obj" });
    ASSERT_TRUE(off.ok);
    ASSERT_TRUE(off.args.model_path == "m.obj");
    ASSERT_FALSE(off.args.texture);
}

TEST(args, texture_rejects_value)
{
    ParseResult r = run({ "--texture=on", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, no_texture_rejects_value)
{
    ParseResult r = run({ "--no-texture=off", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, texture_short_flag_is_unknown)
{
    // The -t short flag was removed along with the on|off value form.
    ParseResult r = run({ "-toff", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
    ASSERT_FALSE(run({ "-t", "off", "m.obj" }).ok);
}

// error: integer overflow / compact-form numeric errors

TEST(args, threads_long_overflow_is_error)
{
    // 20-digit value exceeds LONG_MAX on any platform → errno == ERANGE.
    ParseResult r = run({ "--threads", "99999999999999999999", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, threads_compact_overflow_is_error)
{
    ParseResult r = run({ "-j99999999999999999999", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, fps_long_overflow_is_error)
{
    ParseResult r = run({ "--fps", "99999999999999999999", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, fps_compact_overflow_is_error)
{
    ParseResult r = run({ "-f99999999999999999999", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_long_overflow_is_error)
{
    ParseResult r = run({ "--bench", "99999999999999999999", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_compact_overflow_is_error)
{
    ParseResult r = run({ "-B99999999999999999999", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_compact_zero_is_error)
{
    ParseResult r = run({ "-B0", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_compact_non_digit_is_error)
{
    ParseResult r = run({ "-Babc", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_short_explicit_zero_is_error)
{
    ParseResult r = run({ "-B", "0", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, threads_compact_zero_is_error)
{
    ParseResult r = run({ "-j0", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, fps_compact_zero_is_error)
{
    ParseResult r = run({ "-f0", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, threads_compact_non_digit_is_error)
{
    ParseResult r = run({ "-jabc", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, fps_compact_non_digit_is_error)
{
    ParseResult r = run({ "-fabc", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, threads_short_explicit_zero_is_error)
{
    // -j 0 (space-separated explicit zero) is an error, same as --threads 0.
    ParseResult r = run({ "-j", "0", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, fps_short_explicit_zero_is_error)
{
    ParseResult r = run({ "-f", "0", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, help_with_equals_value_is_error)
{
    ParseResult r = run({ "--help=foo", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// error: empty value after =

TEST(args, empty_value_after_equals_is_error)
{
    ASSERT_FALSE(run({ "--shading=", "m.obj" }).ok);
    ASSERT_FALSE(run({ "--bg=", "m.obj" }).ok);
    ASSERT_FALSE(run({ "--threads=", "m.obj" }).ok);
    ASSERT_FALSE(run({ "--wireframe-color=", "m.obj" }).ok);
    ASSERT_FALSE(run({ "--lighting=", "m.obj" }).ok);
    ASSERT_FALSE(run({ "--fps=", "m.obj" }).ok);
    ASSERT_FALSE(run({ "--bench=", "m.obj" }).ok);
    ASSERT_FALSE(run({ "--color=", "m.obj" }).ok);
}

// --bench-size / --bench-warmup

TEST(args, bench_size_default)
{
    ParseResult r = run({ "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.bench_width, 200);
    ASSERT_EQ(r.args.bench_height, 120);
}

TEST(args, bench_size_valid)
{
    ParseResult r = run({ "--bench", "50", "--bench-size", "400x240", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.bench_width, 400);
    ASSERT_EQ(r.args.bench_height, 240);
}

TEST(args, bench_size_equals_form)
{
    ParseResult r = run({ "--bench", "50", "--bench-size=800x600", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.bench_width, 800);
    ASSERT_EQ(r.args.bench_height, 600);
}

TEST(args, bench_size_no_separator_is_error)
{
    ParseResult r = run({ "--bench", "50", "--bench-size", "400", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_size_zero_width_is_error)
{
    ParseResult r = run({ "--bench", "50", "--bench-size", "0x100", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_size_zero_height_is_error)
{
    ParseResult r = run({ "--bench", "50", "--bench-size", "100x0", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_size_non_numeric_is_error)
{
    ParseResult r = run({ "--bench", "50", "--bench-size", "axb", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_size_trailing_garbage_is_error)
{
    ParseResult r = run({ "--bench", "50", "--bench-size", "100x100x", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_size_overflowing_product_is_error)
{
    // Each axis is <= INT_MAX and parses fine, but the pixel count would overflow
    // size_t on a 32-bit build (wrapping to a tiny framebuffer, then out-of-bounds
    // rasterization). The product guard rejects it before that.
    ParseResult r = run({ "--bench", "50", "--bench-size", "65536x65537", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_size_missing_value_is_error)
{
    ParseResult r = run({ "--bench", "50", "--bench-size", "m.obj" });
    // "m.obj" consumed as value, then model_path is empty → error
    ASSERT_FALSE(r.ok);
}

TEST(args, bench_size_without_bench_is_error)
{
    ParseResult r = run({ "--bench-size", "400x240", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_warmup_default)
{
    ParseResult r = run({ "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.bench_warmup, 20);
}

TEST(args, bench_warmup_valid)
{
    ParseResult r = run({ "--bench", "50", "--bench-warmup", "100", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.bench_warmup, 100);
}

TEST(args, bench_warmup_zero_is_valid)
{
    ParseResult r = run({ "--bench", "50", "--bench-warmup", "0", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.bench_warmup, 0);
}

TEST(args, bench_warmup_negative_is_error)
{
    ParseResult r = run({ "--bench", "50", "--bench-warmup", "-5", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_warmup_non_integer_is_error)
{
    ParseResult r = run({ "--bench", "50", "--bench-warmup", "foo", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_warmup_missing_value_is_error)
{
    ParseResult r = run({ "--bench", "50", "--bench-warmup", "m.obj" });
    // "m.obj" consumed as value, then model_path is empty → error
    ASSERT_FALSE(r.ok);
}

TEST(args, bench_warmup_without_bench_is_error)
{
    ParseResult r = run({ "--bench-warmup", "50", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_size_x_leading_is_error)
{
    // "x100" → sep == val (string starts with 'x') → error
    ParseResult r = run({ "--bench", "50", "--bench-size", "x100", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_size_negative_width_is_error)
{
    // "-10x100" → ww < 0 → ww <= 0 guard fires
    ParseResult r = run({ "--bench", "50", "--bench-size", "-10x100", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_size_negative_height_is_error)
{
    // "100x-10" → hh < 0 → hh <= 0 guard fires
    ParseResult r = run({ "--bench", "50", "--bench-size", "100x-10", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_size_overflow_width_is_error)
{
    ParseResult r = run({ "--bench", "50", "--bench-size", "99999999999999999999x100", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_size_overflow_height_is_error)
{
    ParseResult r = run({ "--bench", "50", "--bench-size", "100x99999999999999999999", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_warmup_overflow_is_error)
{
    ParseResult r = run({ "--bench", "50", "--bench-warmup", "99999999999999999999", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, bench_warmup_equals_form)
{
    ParseResult r = run({ "--bench", "50", "--bench-warmup=10", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.bench_warmup, 10);
}

// --smooth-angle

TEST(args, smooth_angle_valid)
{
    ParseResult r = run({ "--smooth-angle", "30", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.smooth_angle, 30.0f, 1e-6f);
}

TEST(args, smooth_angle_equals_form)
{
    ParseResult r = run({ "--smooth-angle=45.5", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.smooth_angle, 45.5f, 1e-6f);
}

TEST(args, smooth_angle_zero_is_valid)
{
    ParseResult r = run({ "--smooth-angle", "0", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.smooth_angle, 0.0f, 1e-6f);
}

TEST(args, smooth_angle_max_is_valid)
{
    ParseResult r = run({ "--smooth-angle", "180", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.smooth_angle, 180.0f, 1e-6f);
}

TEST(args, smooth_angle_negative_is_error)
{
    ParseResult r = run({ "--smooth-angle", "-1", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, smooth_angle_above_max_is_error)
{
    ParseResult r = run({ "--smooth-angle", "181", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, smooth_angle_non_numeric_is_error)
{
    ParseResult r = run({ "--smooth-angle", "foo", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, smooth_angle_trailing_garbage_is_error)
{
    ParseResult r = run({ "--smooth-angle", "30deg", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, smooth_angle_missing_value_is_error)
{
    ParseResult r = run({ "--smooth-angle" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, smooth_angle_nan_is_error)
{
    ParseResult r = run({ "--smooth-angle", "nan", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, smooth_angle_inf_is_error)
{
    ParseResult r = run({ "--smooth-angle", "inf", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --spin-speed

TEST(args, spin_speed_valid)
{
    ParseResult r = run({ "--spin-speed", "90", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.spin_speed, 90.0f, 1e-6f);
}

TEST(args, spin_speed_equals_form)
{
    ParseResult r = run({ "--spin-speed=12.5", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.spin_speed, 12.5f, 1e-6f);
}

TEST(args, spin_speed_empty_value_is_error)
{
    ParseResult r = run({ "--spin-speed=", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, spin_speed_zero_is_error)
{
    ParseResult r = run({ "--spin-speed", "0", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, spin_speed_negative_is_error)
{
    ParseResult r = run({ "--spin-speed", "-45", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, spin_speed_non_numeric_is_error)
{
    ParseResult r = run({ "--spin-speed", "fast", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, spin_speed_trailing_garbage_is_error)
{
    ParseResult r = run({ "--spin-speed", "45deg", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, spin_speed_missing_value_is_error)
{
    ParseResult r = run({ "--spin-speed" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, spin_speed_nan_is_error)
{
    ParseResult r = run({ "--spin-speed", "nan", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, spin_speed_inf_is_error)
{
    ParseResult r = run({ "--spin-speed", "inf", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --spin-direction

TEST(args, spin_direction_left)
{
    ParseResult r = run({ "--spin-direction", "left", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.spin_direction, SpinDirection::Left);
}

TEST(args, spin_direction_right)
{
    ParseResult r = run({ "--spin-direction", "right", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.spin_direction, SpinDirection::Right);
}

TEST(args, spin_direction_case_insensitive)
{
    ParseResult r = run({ "--spin-direction", "RIGHT", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.spin_direction, SpinDirection::Right);
}

TEST(args, spin_direction_equals_form)
{
    ParseResult r = run({ "--spin-direction=right", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.args.spin_direction, SpinDirection::Right);
}

TEST(args, spin_direction_invalid_value_is_error)
{
    ParseResult r = run({ "--spin-direction", "up", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, spin_direction_empty_value_is_error)
{
    ParseResult r = run({ "--spin-direction=", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, spin_direction_missing_value_is_error)
{
    ParseResult r = run({ "--spin-direction" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, spin_flags_do_not_imply_spin)
{
    ParseResult r = run({ "--spin-speed", "90", "--spin-direction", "right", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_FALSE(r.args.spin);
}

// --yaw

TEST(args, yaw_valid)
{
    ParseResult r = run({ "--yaw", "90", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.yaw, 90.0f, 1e-6f);
}

TEST(args, yaw_equals_form)
{
    ParseResult r = run({ "--yaw=45.5", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.yaw, 45.5f, 1e-6f);
}

TEST(args, yaw_negative_space_form)
{
    // A leading-dash value token is consumed verbatim, not read as a flag.
    ParseResult r = run({ "--yaw", "-45", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.yaw, -45.0f, 1e-6f);
}

TEST(args, yaw_min_is_valid)
{
    ParseResult r = run({ "--yaw", "-180", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.yaw, -180.0f, 1e-6f);
}

TEST(args, yaw_max_is_valid)
{
    ParseResult r = run({ "--yaw", "180", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.yaw, 180.0f, 1e-6f);
}

TEST(args, yaw_below_min_is_error)
{
    ParseResult r = run({ "--yaw", "-180.5", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, yaw_above_max_is_error)
{
    ParseResult r = run({ "--yaw", "180.5", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, yaw_non_numeric_is_error)
{
    ParseResult r = run({ "--yaw", "left", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, yaw_trailing_garbage_is_error)
{
    ParseResult r = run({ "--yaw", "90deg", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, yaw_missing_value_is_error)
{
    ParseResult r = run({ "--yaw" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, yaw_empty_value_is_error)
{
    ParseResult r = run({ "--yaw=", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, yaw_nan_is_error)
{
    ParseResult r = run({ "--yaw", "nan", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, yaw_inf_is_error)
{
    ParseResult r = run({ "--yaw", "inf", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --pitch
// --pitch shares parse_orbit_angle with --yaw, whose suite owns the wrapper's
// error paths (range, malformed, nan/inf, missing/empty value); these pin only
// the branch wiring, the target field, and one fail-loud rejection.

TEST(args, pitch_valid)
{
    ParseResult r = run({ "--pitch", "30", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.pitch, 30.0f, 1e-6f);
}

TEST(args, pitch_equals_form)
{
    // Not the -17.2 default: the assignment must be observable.
    ParseResult r = run({ "--pitch=-45.5", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.pitch, -45.5f, 1e-6f);
}

TEST(args, pitch_top_down_is_valid)
{
    ParseResult r = run({ "--pitch", "-90", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.pitch, -90.0f, 1e-6f);
}

TEST(args, pitch_upside_down_is_valid)
{
    ParseResult r = run({ "--pitch", "180", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.pitch, 180.0f, 1e-6f);
}

TEST(args, pitch_above_max_is_error)
{
    ParseResult r = run({ "--pitch", "181", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

// --zoom

TEST(args, zoom_valid)
{
    ParseResult r = run({ "--zoom", "2.5", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.zoom, 2.5f, 1e-6f);
}

TEST(args, zoom_equals_form)
{
    ParseResult r = run({ "--zoom=0.5", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.zoom, 0.5f, 1e-6f);
}

TEST(args, zoom_min_is_valid)
{
    ParseResult r = run({ "--zoom", "0.2", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.zoom, 0.2f, 1e-6f);
}

TEST(args, zoom_max_is_valid)
{
    ParseResult r = run({ "--zoom", "100", "m.obj" });
    ASSERT_TRUE(r.ok);
    ASSERT_NEAR(r.args.zoom, 100.0f, 1e-6f);
}

TEST(args, zoom_below_min_is_error)
{
    ParseResult r = run({ "--zoom", "0.19", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, zoom_above_max_is_error)
{
    ParseResult r = run({ "--zoom", "100.5", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, zoom_zero_is_error)
{
    ParseResult r = run({ "--zoom", "0", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, zoom_negative_is_error)
{
    ParseResult r = run({ "--zoom", "-1", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, zoom_non_numeric_is_error)
{
    ParseResult r = run({ "--zoom", "close", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, zoom_trailing_garbage_is_error)
{
    ParseResult r = run({ "--zoom", "2x", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, zoom_missing_value_is_error)
{
    ParseResult r = run({ "--zoom" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, zoom_empty_value_is_error)
{
    ParseResult r = run({ "--zoom=", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, zoom_nan_is_error)
{
    ParseResult r = run({ "--zoom", "nan", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}

TEST(args, zoom_inf_is_error)
{
    ParseResult r = run({ "--zoom", "inf", "m.obj" });
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.exit_code, 1);
}
