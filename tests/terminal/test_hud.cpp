#include "tests/test.h"
#include "src/terminal/hud.h"
#include "src/terminal/text.h"

#include <cstddef>
#include <string>

// compose_hud is pure (state in, styled string out), so the whole layout is testable here with
// no framebuffer and no fd redirection. What no byte-level test can decide (last-column erase
// order, wrap state) lives in the framebuffer tests and the tmux recipe in CLAUDE.md.

namespace
{
    // Remove SGR escapes (\033[...m), leaving the visible text.
    std::string strip_sgr(const std::string &s)
    {
        std::string out;
        size_t i = 0;
        while (i < s.size())
        {
            if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[')
            {
                i += 2;
                while (i < s.size() && s[i] != 'm')
                {
                    ++i;
                }
                if (i < s.size())
                {
                    ++i; // consume the final 'm'
                }
                continue;
            }
            out += s[i++];
        }
        return out;
    }

    // Rendered width of the bar's visible content. Measured with the same display_width() the
    // composer lays out with, which is not circular: that function's own answers are pinned
    // against hand-counted columns in the text tests.
    size_t visible_cols(const std::string &styled)
    {
        return display_width(strip_sgr(styled));
    }

    // True when the first visible glyph of the line is preceded by an SGR. Padding runs are
    // exempt: they deliberately carry no escape, since a space shows only the bar background.
    bool first_glyph_is_styled(const std::string &styled)
    {
        bool seen_sgr = false;
        size_t i = 0;
        while (i < styled.size())
        {
            if (styled[i] == '\033')
            {
                seen_sgr = true;
                while (i < styled.size() && styled[i] != 'm')
                {
                    ++i;
                }
                if (i < styled.size())
                {
                    ++i;
                }
                continue;
            }
            if (styled[i] != ' ')
            {
                return seen_sgr;
            }
            ++i;
        }
        return true; // nothing but padding
    }

    bool starts_with(const std::string &s, const std::string &prefix)
    {
        return s.compare(0, prefix.size(), prefix) == 0;
    }

    bool ends_with(const std::string &s, const std::string &suffix)
    {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // Ordinary orbit-mode session: textured model, culling and texturing on, not spinning.
    HudInfo base_info()
    {
        HudInfo info;
        info.model_name = "suzanne.obj";
        info.shading_name = "phong";
        info.light_name = "dual";
        info.bg_name = "black";
        info.culling = true;
        info.texturing = true;
        info.has_textures = true;
        info.fps = 60;
        return info;
    }
} // namespace

TEST(hud, full_bar_at_80_cols)
{
    const std::string out = compose_hud(base_info(), 80, ColorMode::TrueColor);
    const std::string text = strip_sgr(out);
    ASSERT_EQ(visible_cols(out), static_cast<size_t>(80));
    ASSERT_TRUE(starts_with(text, " suzanne.obj"));
    ASSERT_TRUE(ends_with(text, "60 fps "));
    for (const char *field : { "phong", "dual", "black", "spin", "cull", "tex" })
    {
        ASSERT_TRUE(text.find(field) != std::string::npos);
    }
}

TEST(hud, pads_to_exact_width_across_sizes)
{
    // Whatever the ladder drops, the line comes out exactly cols wide: the padding is what makes
    // the bar a bar rather than a ragged line. An ASCII name, so the cut is measured in
    // characters and these numbers are readable; the non-ASCII cases are pinned separately.
    for (const int cols : { 120, 100, 80, 70, 60, 50, 40, 30 })
    {
        const std::string out = compose_hud(base_info(), cols, ColorMode::TrueColor);
        ASSERT_EQ(visible_cols(out), static_cast<size_t>(cols));
    }
}

TEST(hud, drop_progression_narrowing)
{
    // Field widths for base_info put the full bar at 58 columns; each width below exercises one
    // more cumulative drop: bg, then light, then the toggle tags, then the whole centre.
    const HudInfo info = base_info();

    const std::string full = strip_sgr(compose_hud(info, 80, ColorMode::TrueColor));
    ASSERT_TRUE(full.find("black") != std::string::npos);

    const std::string no_bg = strip_sgr(compose_hud(info, 55, ColorMode::TrueColor));
    ASSERT_TRUE(no_bg.find("black") == std::string::npos);
    ASSERT_TRUE(no_bg.find("dual") != std::string::npos);

    const std::string no_light = strip_sgr(compose_hud(info, 45, ColorMode::TrueColor));
    ASSERT_TRUE(no_light.find("dual") == std::string::npos);
    ASSERT_TRUE(no_light.find("phong") != std::string::npos);

    const std::string no_tags = strip_sgr(compose_hud(info, 35, ColorMode::TrueColor));
    ASSERT_TRUE(no_tags.find("spin") == std::string::npos);
    ASSERT_TRUE(no_tags.find("phong") != std::string::npos);

    // The centre is gone entirely, and at this width the name and the reading both still fit.
    // (Narrower still, the name gives way too; the_fps_reading_outlives_every_other_field
    // covers that end of the ladder.)
    const std::string floor = strip_sgr(compose_hud(info, 25, ColorMode::TrueColor));
    ASSERT_TRUE(floor.find("phong") == std::string::npos);
    ASSERT_TRUE(floor.find("suzanne.obj") != std::string::npos);
    ASSERT_TRUE(floor.find("60 fps") != std::string::npos);
    ASSERT_EQ(visible_cols(compose_hud(info, 25, ColorMode::TrueColor)), static_cast<size_t>(25));
}

namespace
{
    // Wireframe shading plus an over-budget name, the only configuration that reaches the two
    // deepest levels above the floor: the wireframe colour name exists to be dropped, and the
    // name is long enough for the shrunk budget to change it.
    HudInfo wireframe_info()
    {
        HudInfo info = base_info();
        info.model_name = "a_very_long_scanned_model_name.stl";
        info.shading_name = "wireframe";
        info.wf_name = "white";
        info.wf_color = { 200, 200, 200 };
        return info;
    }
} // namespace

TEST(hud, drop_wf_before_shrinking_the_name)
{
    // 48 columns: the wireframe colour name is gone, but the shading mode stays and the name
    // still gets the full budget (its 24-column cut).
    const HudInfo info = wireframe_info();
    const std::string out = compose_hud(info, 48, ColorMode::TrueColor);
    const std::string text = strip_sgr(out);
    ASSERT_TRUE(text.find("white") == std::string::npos);
    ASSERT_TRUE(text.find("wireframe") != std::string::npos);
    ASSERT_TRUE(text.find("a_very_long...l_name.stl") != std::string::npos);
    ASSERT_EQ(visible_cols(out), static_cast<size_t>(48));
}

TEST(hud, shrinks_the_name_before_dropping_the_centre)
{
    // 36 columns: one level deeper, the name budget halves to 12 bytes and the centre survives.
    const HudInfo info = wireframe_info();
    const std::string out = compose_hud(info, 36, ColorMode::TrueColor);
    const std::string text = strip_sgr(out);
    ASSERT_TRUE(text.find("a_very_long...l_name.stl") == std::string::npos);
    ASSERT_TRUE(text.find("a_ver....stl") != std::string::npos);
    ASSERT_TRUE(text.find("wireframe") != std::string::npos);
    ASSERT_EQ(visible_cols(out), static_cast<size_t>(36));
}

TEST(hud, first_person_speed_outlives_the_lighting_mode)
{
    // A deliberate ordering decision, not an accident of the field widths: the lighting mode is
    // legible in the render itself, while the first-person speed has no display but this one, so
    // lighting is dropped first and the speed survives to the floor.
    HudInfo info = base_info();
    info.first_person = true;
    info.fp_speed = 1.0f;
    const std::string text = strip_sgr(compose_hud(info, 55, ColorMode::TrueColor));
    ASSERT_TRUE(text.find("dual") == std::string::npos);
    ASSERT_TRUE(text.find("fp 1.00x") != std::string::npos);

    // And at the floor, where the centre is gone entirely, it is still there beside the fps.
    const std::string floor = strip_sgr(compose_hud(info, 34, ColorMode::TrueColor));
    ASSERT_TRUE(floor.find("phong") == std::string::npos);
    ASSERT_TRUE(floor.find("fp 1.00x") != std::string::npos);
    ASSERT_TRUE(floor.find("60 fps") != std::string::npos);
}

TEST(hud, every_drop_level_styles_its_first_glyph)
{
    // The first retained segment must emit its style instead of inheriting the
    // framebuffer default. Check every drop level in both colour modes.
    HudInfo wf = wireframe_info();
    HudInfo fp = base_info();
    fp.first_person = true;
    for (const int cols : { 120, 80, 60, 48, 44, 36, 30, 25, 15, 8, 1 })
    {
        ASSERT_TRUE(first_glyph_is_styled(compose_hud(base_info(), cols, ColorMode::TrueColor)));
        ASSERT_TRUE(first_glyph_is_styled(compose_hud(base_info(), cols, ColorMode::Palette256)));
        ASSERT_TRUE(first_glyph_is_styled(compose_hud(wf, cols, ColorMode::TrueColor)));
        ASSERT_TRUE(first_glyph_is_styled(compose_hud(fp, cols, ColorMode::TrueColor)));
    }
    // An empty model name empties the left zone entirely, so the first glyph comes from a later
    // one; it must still be styled.
    HudInfo nameless = base_info();
    nameless.model_name = "";
    ASSERT_TRUE(first_glyph_is_styled(compose_hud(nameless, 80, ColorMode::TrueColor)));
    ASSERT_TRUE(first_glyph_is_styled(compose_hud(nameless, 20, ColorMode::TrueColor)));
}

TEST(hud, the_fps_reading_outlives_every_other_field)
{
    // The right-aligned FPS field must survive every supported width. The drop ladder
    // removes name budget, name, and speed before allowing the line to overrun.
    for (const int cols : { 80, 40, 30, 24, 20, 16, 12, 10 })
    {
        const std::string out = compose_hud(base_info(), cols, ColorMode::TrueColor);
        ASSERT_TRUE(visible_cols(out) <= static_cast<size_t>(cols));
        ASSERT_TRUE(strip_sgr(out).find("60 fps") != std::string::npos);
    }
    // The gap survives with it: wherever the name is still shown it never touches the digits.
    for (const int cols : { 30, 24, 20, 18 })
    {
        ASSERT_TRUE(
            strip_sgr(compose_hud(base_info(), cols, ColorMode::TrueColor)).find("  60 fps") != std::string::npos
        );
    }
}

TEST(hud, first_person_speed_yields_only_to_the_fps_reading)
{
    // It outlives the lighting mode and the model name, and gives way to nothing but the
    // reading itself, at the last width where the two numbers cannot both fit.
    HudInfo info = base_info();
    info.first_person = true;
    info.fp_speed = 1.0f;
    const std::string mid = strip_sgr(compose_hud(info, 24, ColorMode::TrueColor));
    ASSERT_TRUE(mid.find("fp 1.00x") != std::string::npos);
    ASSERT_TRUE(mid.find("suzanne") == std::string::npos); // the name went first
    const std::string floor = strip_sgr(compose_hud(info, 12, ColorMode::TrueColor));
    ASSERT_TRUE(floor.find("fp ") == std::string::npos);
    ASSERT_TRUE(floor.find("60 fps") != std::string::npos);
    ASSERT_TRUE(visible_cols(compose_hud(info, 12, ColorMode::TrueColor)) <= static_cast<size_t>(12));
}

TEST(hud, tex_tag_only_for_textured_models)
{
    HudInfo info = base_info();
    ASSERT_TRUE(strip_sgr(compose_hud(info, 80, ColorMode::TrueColor)).find("tex") != std::string::npos);
    info.has_textures = false;
    ASSERT_TRUE(strip_sgr(compose_hud(info, 80, ColorMode::TrueColor)).find("tex") == std::string::npos);
}

TEST(hud, toggle_tags_accent_on_dim_off)
{
    // base_info: spinning off (dim), culling on (accent). The accent run then continues through
    // " tex" with no repeated SGR (same colour), so exactly one accent escape covers both tags.
    const std::string out = compose_hud(base_info(), 80, ColorMode::TrueColor);
    ASSERT_TRUE(out.find("\033[38;2;90;90;90mspin") != std::string::npos);
    ASSERT_TRUE(out.find("\033[38;2;110;190;220m cull tex") != std::string::npos);
}

TEST(hud, background_field_is_dimmer_than_the_fields_before_it)
{
    // Without captions, wireframe and background may both read "white". Give the
    // background label styling so the two fields remain distinguishable.
    HudInfo info = base_info();
    info.shading_name = "wireframe";
    info.wf_name = "white";
    info.wf_color = { 200, 200, 200 };
    info.bg_name = "white";
    const std::string out = compose_hud(info, 80, ColorMode::TrueColor);
    ASSERT_TRUE(out.find("\033[38;2;200;200;200mwhite") != std::string::npos); // wireframe colour
    // The background now shares the label grey with the separator that precedes it, so the SGR
    // dedup emits one escape covering both and the word follows the separator directly.
    ASSERT_TRUE(out.find("\033[38;2;140;140;140m \xc2\xb7 white") != std::string::npos);
    // The background label must not use the brighter grey reserved for values such as lighting.
    ASSERT_TRUE(out.find("\033[38;2;220;220;220mwhite") == std::string::npos);
    ASSERT_TRUE(out.find("\033[38;2;220;220;220mdual") != std::string::npos);
}

TEST(hud, wireframe_name_in_its_own_color)
{
    HudInfo info = base_info();
    info.shading_name = "wireframe";
    info.wf_name = "red";
    info.wf_color = { 220, 80, 80 };
    const std::string out = compose_hud(info, 80, ColorMode::TrueColor);
    ASSERT_TRUE(out.find("\033[38;2;220;80;80mred") != std::string::npos);
}

TEST(hud, first_person_speed_field)
{
    HudInfo info = base_info();
    info.first_person = true;
    info.fp_speed = 1.25f;
    const std::string text = strip_sgr(compose_hud(info, 80, ColorMode::TrueColor));
    ASSERT_TRUE(text.find("fp 1.25x") != std::string::npos);
    ASSERT_TRUE(strip_sgr(compose_hud(base_info(), 80, ColorMode::TrueColor)).find("fp ") == std::string::npos);
}

TEST(hud, palette256_emits_indexed_sgr_only)
{
    const std::string out = compose_hud(base_info(), 80, ColorMode::Palette256);
    ASSERT_TRUE(out.find("38;2;") == std::string::npos);
    ASSERT_TRUE(out.find("\033[38;5;") != std::string::npos);
    ASSERT_EQ(visible_cols(out), static_cast<size_t>(80));
}

TEST(hud, long_name_truncated)
{
    HudInfo info = base_info();
    info.model_name = "a_very_long_scanned_model_name_indeed.stl";
    const std::string text = strip_sgr(compose_hud(info, 80, ColorMode::TrueColor));
    ASSERT_TRUE(text.find("...") != std::string::npos);
    ASSERT_TRUE(text.find(".stl") != std::string::npos); // middle cut keeps the extension
    ASSERT_EQ(visible_cols(compose_hud(info, 80, ColorMode::TrueColor)), static_cast<size_t>(80));
}

TEST(hud, nonascii_name_fills_the_bar_exactly)
{
    // U+2605 is 3 bytes for one column, so the name's byte count is nearly three times its
    // width. The layout measures columns, not bytes: padding from the byte count would leave
    // the right-aligned fields ~12 columns short of the edge on any such filename.
    HudInfo info = base_info();
    std::string stars;
    for (int i = 0; i < 12; ++i)
    {
        stars += "\xe2\x98\x85";
    }
    info.model_name = stars;
    ASSERT_EQ(visible_cols(compose_hud(info, 80, ColorMode::TrueColor)), static_cast<size_t>(80));
}

TEST(hud, name_starting_with_an_orphan_selector_fills_the_bar_exactly)
{
    // Drop a leading emoji presentation selector because its width depends on the
    // preceding segment, while the name is measured independently.
    HudInfo info = base_info();
    const std::string name = std::string("\xef\xb8\x8f") + "model.obj";
    info.model_name = name;
    const std::string out = compose_hud(info, 80, ColorMode::TrueColor);
    ASSERT_EQ(visible_cols(out), static_cast<size_t>(80));
    ASSERT_TRUE(strip_sgr(out).find("model.obj") != std::string::npos);
}

TEST(hud, flag_emoji_name_fills_the_bar_exactly)
{
    HudInfo info = base_info();
    std::string flags;
    for (int i = 0; i < 4; ++i)
    {
        flags += "\xf0\x9f\x87\xa6\xf0\x9f\x87\xba"; // U+1F1E6 U+1F1FA
    }
    info.model_name = flags;
    ASSERT_EQ(visible_cols(compose_hud(info, 80, ColorMode::TrueColor)), static_cast<size_t>(80));
}

TEST(hud, east_asian_name_fills_the_bar_exactly)
{
    // The other direction: a CJK name is two columns per 3-byte code point, so a width measured
    // as one-per-code-point would overflow the line instead.
    HudInfo info = base_info();
    std::string cjk;
    for (int i = 0; i < 8; ++i)
    {
        cjk += "\xe6\xa8\xa1"; // U+6A21
    }
    info.model_name = cjk;
    ASSERT_EQ(visible_cols(compose_hud(info, 80, ColorMode::TrueColor)), static_cast<size_t>(80));
}

TEST(hud, degenerate_widths_no_crash)
{
    ASSERT_TRUE(compose_hud(base_info(), 0, ColorMode::TrueColor).empty());
    ASSERT_TRUE(compose_hud(base_info(), -3, ColorMode::TrueColor).empty());
    for (const int cols : { 1, 2, 5, 10, 15 })
    {
        // Under about ten columns not even the reading fits, so the line runs past the edge and
        // the terminal clips it. Nothing to assert about the layout there; it must not blow up.
        const std::string out = compose_hud(base_info(), cols, ColorMode::TrueColor);
        ASSERT_TRUE(!out.empty());
    }
}
