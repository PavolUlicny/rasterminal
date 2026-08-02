#include "hud.h"

#include "color.h" // Color, ColorMode, append_fg_sgr
#include "text.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace
{

    // Brightness hierarchy on the {18,18,18} bar, plus one accent. In Palette256 mode each fg
    // maps through quantize_256(), so the two depths agree by construction and no second colour
    // table exists to drift.
    constexpr Color FG_VALUE = HUD_BAR_FG;         // primary values: name, numbers, mode names
    constexpr Color FG_LABEL = { 140, 140, 140 };  // units and separators
    constexpr Color FG_ACCENT = { 110, 190, 220 }; // active states: shading mode, lit toggles
    constexpr Color FG_DIM = { 90, 90, 90 };       // inactive toggles: visible but receding

    // A styled run of text. `cols` is the rendered width in terminal columns, which is not the
    // byte count: the separator is one column in two bytes, and a model name in a non-Latin
    // script routinely differs in both directions (display_width measures it). The view points
    // either at a string literal or at one of compose_hud's own buffers, both of which outlive
    // the segment array, so nothing here owns or copies a string.
    struct Seg
    {
        std::string_view text;
        Color fg;
        int cols;
    };

    // " · " (U+00B7 MIDDLE DOT): 4 bytes, counted as 3 columns.
    //
    // U+00B7 is East Asian Ambiguous, so a terminal configured for a CJK locale may render it
    // two columns wide and the bar would then over-run by one column per separator. That is not
    // a new assumption: ▀ (U+2580), which every rendered pixel cell is made of, is Ambiguous
    // too, so ambiguous-as-narrow is a precondition of the whole viewer rather than of this
    // line. A terminal that breaks it turns the render itself into stripes long before the
    // status bar's alignment matters.
    //
    // Stated generally, because it licenses more than this one glyph: any character the bar
    // draws may be East Asian Ambiguous and counted narrow, and needs no separate argument for
    // it. What a new glyph does still have to clear is cp_width's own rule, that a doubtful
    // width rounds UP, and the older bar for a terminal font: nothing beyond the ASCII range
    // and the handful of characters (▀ and this dot) that predate every font a terminal ships.
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

    // Cumulative drop levels, tried in order until the line fits, so a narrow terminal loses
    // fields deliberately instead of clipping them at the edge. Ordered by how redundant each
    // field is with the picture itself: the background is visible behind the model, the lighting
    // mode shows in the shading, the toggles all have visible effects, the wireframe colour is
    // literally on screen. The fps reading is last because it has no visual stand-in at all,
    // and the first-person speed second-to-last for the same reason (the HUD is its only
    // display); the model name outranks neither, being static identity the user just typed.
    constexpr int DROP_BG = 1;
    constexpr int DROP_LIGHT = 2;
    constexpr int DROP_TAGS = 3;
    constexpr int DROP_WF = 4;
    constexpr int SHRINK_NAME = 5;
    constexpr int DROP_CENTRE = 6;
    // Past the centred group the ladder keeps going, because the right zone is right-aligned:
    // stopping here and letting the line run off the edge means the TERMINAL chooses what to
    // drop, and what it clips is the rightmost field, which is the fps reading this floor exists
    // to protect. So the name gives way instead, and last of all the first-person speed.
    constexpr int SHRINK_NAME_TINY = 7;
    constexpr int DROP_NAME = 8;
    constexpr int DROP_FP = 9;
    // Not NAME_MAX/NAME_MIN: POSIX <limits.h> defines NAME_MAX as a macro, and any toolchain
    // whose C++ headers pull that in transitively would rewrite the declaration below into a
    // numeric literal and fail to compile. Reached this file's TU on no platform we build for,
    // which is exactly why it would surface as someone else's broken build.
    constexpr size_t NAME_BUDGET_MAX = 24;
    constexpr size_t NAME_BUDGET_MIN = 12;
    constexpr size_t NAME_BUDGET_TINY = 8;
    constexpr int ZONE_GAP = 2;

    // Formatted once: neither depends on the drop level, and the loop below may rebuild the
    // segment list several times before one fits.
    char fp_buf[24] = "";
    if (info.first_person)
    {
        std::snprintf(fp_buf, sizeof(fp_buf), "%.2fx", static_cast<double>(info.fp_speed));
    }
    char fps_buf[16];
    std::snprintf(fps_buf, sizeof(fps_buf), "%d", info.fps);

    // The name is re-cut only when the budget actually changes (at most twice on the way down,
    // at SHRINK_NAME and SHRINK_NAME_TINY), and a name already within budget is viewed in place
    // rather than copied, so the common call builds no string at all. A name over budget costs
    // one cut result per budget it passes through. (That is why truncate_middle takes a
    // string_view; taking a std::string made every one of those two allocations.)
    std::string name_store;
    std::string_view name = info.model_name;
    int name_cols = 0;
    size_t name_budget = 0;

    // One fixed array rather than three vectors: the zones are built in order, so two boundary
    // indices describe them, and a compose then allocates only its output string (plus the cut
    // name above, when the name is over budget).
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
            // A leading emoji presentation selector has to go before the name is measured. Its
            // width is the one that depends on the character BEFORE it, and each segment here is
            // measured alone and the widths summed, so a segment that starts with it resolves to
            // a different number than the assembled line renders. It is meaningless anyway: it
            // either lost its base to the cut above or never had one.
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
                // Dimmer than the fields before it, for two reasons that agree. It is the first
                // field dropped when the bar runs out of room, so the styling matches the
                // priority the layout already assigns it; and with the captions gone, the only
                // pair of fields that can render the same word is the wireframe colour and this
                // one (both can be "white", two fields apart in near-identical greys), which
                // the brightness difference separates without bringing a caption back.
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

    // Padding. The centre zone sits truly centred when the slack allows and is nudged inward
    // otherwise; the fit check above passed whenever the zone exists, which is exactly the
    // condition making both clamps satisfiable, so both pads land at ZONE_GAP or wider.
    int pad1 = 0;
    int pad2 = 0;
    if (centre_end == left_end)
    {
        // Never narrower than the gap. Below the floor width the line runs past the right edge
        // and the terminal clips it (auto-wrap is off), and losing the tail of the fps reading
        // is far better than the model name running straight into the digits with no separator.
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
    // Defensive only, per the argument above: a negative count would convert to a huge size_t
    // at the append below rather than simply producing a short line.
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
            // A run of spaces shows only the bar background; emitting no SGR for it (and leaving
            // the dedup state alone) keeps padding free of escape bytes.
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
