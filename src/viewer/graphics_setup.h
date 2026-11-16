#pragma once

#include "src/args.h"
#include "src/terminal/graphics.h"

#include <cstring>

namespace viewer
{
    // Interrupted queries must be retried. Detection errors are reported only
    // after the caller releases the terminal state the query changed.
    struct GraphicsSetup
    {
        GraphicsBackend backend = GraphicsBackend::Blocks;
        bool shm_ok = false;
        bool interrupted = false;
        int cell_w = 0;
        int cell_h = 0;
        // The terminal's max sixel image size (0 = unreported); see TermGraphics.
        int sixel_max_w = 0;
        int sixel_max_h = 0;
        // The terminal's sixel colour register count (0 = unreported).
        int sixel_registers = 0;
        // Set with exit_code 1; main prints it after restoring the terminal.
        const char *error = nullptr;
        int exit_code = -1;
    };

    // TMUX marks tmux itself; a TERM of screen* or tmux* also catches GNU screen.
    inline bool under_multiplexer(const char *tmux_env, const char *term_env) noexcept
    {
        return tmux_env != nullptr || (term_env != nullptr && (std::strncmp(term_env, "screen", 6) == 0 ||
                                                               std::strncmp(term_env, "tmux", 4) == 0));
    }

    // A null reply means the query was skipped because a multiplexer needs protocol
    // passthrough this build does not implement, so the session uses blocks.
    // control_requested is sampled after the query.
    inline GraphicsSetup resolve_graphics(GraphicsChoice choice, const TermGraphics *reply, bool control_requested)
    {
        GraphicsSetup gfx;
        if (choice == GraphicsChoice::Blocks)
        {
            return gfx;
        }
        if (reply != nullptr)
        {
            gfx.interrupted = reply->interrupted;
            if (reply->failed)
            {
                gfx.error = "failed to query terminal graphics";
                gfx.exit_code = 1;
                return gfx;
            }
            // Outside the backend selection: a sixel-only terminal's cell-size
            // reply must not be thrown away with the kitty verdict.
            gfx.cell_w = reply->cell_w;
            gfx.cell_h = reply->cell_h;
            gfx.sixel_max_w = reply->sixel_max_w;
            gfx.sixel_max_h = reply->sixel_max_h;
            gfx.sixel_registers = reply->sixel_registers;
            // Auto prefers kitty to sixel. A forced choice masks the other pixel
            // backend, so --graphics sixel exercises sixel on terminals with both.
            if (reply->kitty && choice != GraphicsChoice::Sixel)
            {
                gfx.backend = GraphicsBackend::Kitty;
                gfx.shm_ok = reply->kitty_shm;
            }
            else if (reply->sixel && choice != GraphicsChoice::Kitty)
            {
                gfx.backend = GraphicsBackend::Sixel;
            }
        }
        // Suspension restarts detection after continuation. Termination unwinds
        // the terminal guards before finish_termination re-raises the signal.
        if (gfx.interrupted || control_requested)
        {
            gfx.interrupted = true;
            return gfx;
        }
        // Forced pixel modes still require detection; unsupported escapes would
        // otherwise produce a blank screen with no diagnostic.
        if (gfx.backend == GraphicsBackend::Blocks &&
            (choice == GraphicsChoice::Kitty || choice == GraphicsChoice::Sixel))
        {
            const bool kitty_choice = choice == GraphicsChoice::Kitty;
            if (reply == nullptr)
            {
                // The multiplexer gate also covers GNU screen, so the message must not blame tmux alone.
                gfx.error = kitty_choice ? "kitty graphics is not supported under tmux or GNU screen"
                                         : "sixel graphics is not supported under tmux or GNU screen";
            }
            else
            {
                gfx.error = kitty_choice ? "terminal does not answer the kitty graphics query"
                                         : "terminal does not report sixel support";
            }
            gfx.exit_code = 1;
        }
        return gfx;
    }
} // namespace viewer
