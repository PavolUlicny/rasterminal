#include "src/terminal/hud.h"

#include "src/terminal/color.h" // Color, ColorMode, append_fg_sgr
#include "src/terminal/text.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace
{

    // One brightness hierarchy for both truecolor and quantized output.
    constexpr Color FG_VALUE = HUD_BAR_FG;         // primary values: name, numbers, mode names
    constexpr Color FG_LABEL = { 140, 140, 140 };  // units and separators
    constexpr Color FG_ACCENT = { 110, 190, 220 }; // active states: shading mode, lit toggles
    constexpr Color FG_DIM = { 90, 90, 90 };       // inactive toggles: visible but receding

    // A non-owning styled run with its rendered column width.
    struct Seg
    {
        std::string_view text;
        Color fg;
        int cols;
    };

    // Middle dot is East Asian Ambiguous, as is the renderer's half-block. Both require
    // terminals to display ambiguous characters narrowly.
    constexpr std::string_view SEPARATOR = " \xc2\xb7 ";
    constexpr int SEPARATOR_COLS = 3;

    // left 2 (margin, name) + centre 7 (mode, then up to three separator+field pairs) +
    // right 9 (three toggle tags, gap, "fp ", value, gap, fps, unit), with headroom.
    constexpr size_t MAX_SEGS = 24;

} // namespace

std::string compose_hud(const HudInfo &info, int cols, ColorMode mode)
{
    if (cols < 1)
    {
        return {};
    }

    // Cumulative drop levels remove visually redundant fields first and preserve live numbers.
    constexpr int DROP_BG = 1;
    constexpr int DROP_LIGHT = 2;
    constexpr int DROP_TAGS = 3;
    constexpr int DROP_WF = 4;
    constexpr int SHRINK_NAME = 5;
    constexpr int DROP_CENTRE = 6;
    // Keep dropping fields after the center disappears so right-edge clipping does not eat fps.
    constexpr int SHRINK_NAME_TINY = 7;
    constexpr int DROP_NAME = 8;
    constexpr int DROP_FP = 9;
    // Avoid POSIX's NAME_MAX macro.
    constexpr size_t NAME_BUDGET_MAX = 24;
    constexpr size_t NAME_BUDGET_MIN = 12;
    constexpr size_t NAME_BUDGET_TINY = 8;
    constexpr int ZONE_GAP = 2;

    // These values do not change across fit attempts.
    char fp_buf[24] = "";
    if (info.first_person)
    {
        std::snprintf(fp_buf, sizeof(fp_buf), "%.2fx", static_cast<double>(info.fp_speed));
    }
    char fps_buf[16];
    std::snprintf(fps_buf, sizeof(fps_buf), "%d", info.fps);

    // Recut the name only when its budget changes; short names remain non-owning views.
    std::string name_store;
    std::string_view name = info.model_name;
    int name_cols = 0;
    size_t name_budget = 0;

    // One fixed array holds three contiguous zones without per-zone allocations.
    std::array<Seg, MAX_SEGS> segs{};
    size_t n_seg = 0;
    size_t left_end = 0;
    size_t centre_end = 0;

    const auto push = [&](std::string_view text, Color fg, int seg_cols) { segs[n_seg++] = Seg{ text, fg, seg_cols }; };
    // Every field but the model name is ASCII, where one byte is exactly one column.
    const auto push_ascii = [&](std::string_view text, Color fg) { push(text, fg, static_cast<int>(text.size())); };
    const auto width_of = [&](size_t begin, size_t end)
    {
        int w = 0;
        for (size_t i = begin; i < end; ++i)
        {
            w += segs[i].cols;
        }
        return w;
    };

    int w_left = 0;
    int w_centre = 0;
    int w_right = 0;

    for (int level = 0; level <= DROP_FP; ++level)
    {
        size_t budget = NAME_BUDGET_MAX;
        if (level >= SHRINK_NAME_TINY)
        {
            budget = NAME_BUDGET_TINY;
        }
        else if (level >= SHRINK_NAME)
        {
            budget = NAME_BUDGET_MIN;
        }
        if (budget != name_budget)
        {
            name_budget = budget;
            if (info.model_name.size() > budget)
            {
                name_store = truncate_middle(info.model_name, budget);
                name = name_store;
            }
            // A detached VS16 has context-dependent width and no base, so remove it before measuring.
            constexpr std::string_view VS16 = "\xef\xb8\x8f";
            while (name.size() >= VS16.size() && name.compare(0, VS16.size(), VS16) == 0)
            {
                name.remove_prefix(VS16.size());
            }
            name_cols = static_cast<int>(display_width(name));
        }

        n_seg = 0;

        // Left zone: margin + model name.
        push_ascii(" ", FG_VALUE);
        if (level < DROP_NAME)
        {
            push(name, FG_VALUE, name_cols);
        }
        left_end = n_seg;

        // Centre zone: shading [· wf colour] [· light] [· bg].
        if (level < DROP_CENTRE)
        {
            push_ascii(info.shading_name, FG_ACCENT);
            if (!info.wf_name.empty() && level < DROP_WF)
            {
                push(SEPARATOR, FG_LABEL, SEPARATOR_COLS);
                push_ascii(info.wf_name, info.wf_color);
            }
            if (level < DROP_LIGHT)
            {
                push(SEPARATOR, FG_LABEL, SEPARATOR_COLS);
                push_ascii(info.light_name, FG_VALUE);
            }
            if (level < DROP_BG)
            {
                push(SEPARATOR, FG_LABEL, SEPARATOR_COLS);
                // Dim the first dropped field and distinguish it from a same-named wireframe color.
                push_ascii(info.bg_name, FG_LABEL);
            }
        }
        centre_end = n_seg;

        // Right zone: [toggle tags] [fp speed] fps + margin.
        if (level < DROP_TAGS)
        {
            const auto tag_fg = [](bool on) { return on ? FG_ACCENT : FG_DIM; };
            push_ascii("spin", tag_fg(info.spinning));
            push_ascii(" cull", tag_fg(info.culling));
            if (info.has_textures)
            {
                push_ascii(" tex", tag_fg(info.texturing));
            }
            push_ascii("  ", FG_LABEL);
        }
        if (info.first_person && level < DROP_FP)
        {
            push_ascii("fp ", FG_LABEL);
            push_ascii(fp_buf, FG_VALUE);
            push_ascii("  ", FG_LABEL);
        }
        push_ascii(fps_buf, FG_VALUE);
        push_ascii(" fps ", FG_LABEL);

        w_left = width_of(0, left_end);
        w_centre = width_of(left_end, centre_end);
        w_right = width_of(centre_end, n_seg);
        const int need =
            (centre_end == left_end) ? w_left + ZONE_GAP + w_right : w_left + ZONE_GAP + w_centre + ZONE_GAP + w_right;
        if (need <= cols)
        {
            break;
        }
    }

    // Center the middle zone when possible, otherwise keep both gaps at ZONE_GAP.
    int pad1 = 0;
    int pad2 = 0;
    if (centre_end == left_end)
    {
        // Preserve a separator even below the minimum fit width; auto-wrap is disabled.
        pad1 = std::max(cols - w_left - w_right, ZONE_GAP);
    }
    else
    {
        int start = (cols - w_centre) / 2;
        start = std::min(start, cols - w_right - ZONE_GAP - w_centre);
        start = std::max(start, w_left + ZONE_GAP);
        pad1 = start - w_left;
        pad2 = cols - w_right - start - w_centre;
    }
    // Prevent a negative pad from becoming a huge size_t.
    pad1 = std::max(pad1, 0);
    pad2 = std::max(pad2, 0);

    std::string out;
    out.reserve(static_cast<size_t>(cols) + (24u * n_seg));

    Color cur{};
    bool cur_known = false;
    const auto emit = [&](size_t begin, size_t end)
    {
        for (size_t i = begin; i < end; ++i)
        {
            const Seg &s = segs[i];
            // Padding needs no foreground escape.
            if (s.text.find_first_not_of(' ') == std::string_view::npos)
            {
                out += s.text;
                continue;
            }
            if (!cur_known || s.fg != cur)
            {
                append_fg_sgr(out, s.fg, mode);
                cur = s.fg;
                cur_known = true;
            }
            out += s.text;
        }
    };

    emit(0, left_end);
    out.append(static_cast<size_t>(pad1), ' ');
    emit(left_end, centre_end);
    out.append(static_cast<size_t>(pad2), ' ');
    emit(centre_end, n_seg);
    return out;
}
