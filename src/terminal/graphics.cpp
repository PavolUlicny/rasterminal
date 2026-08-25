#include "src/terminal/graphics.h"

#include "src/platform/input.h" // the shared grammar: detail::is_string_introducer / scan_to_csi_final /
                                // parse_cell_size_body / parse_sixel_geometry_body / MAX_CSI_PARAM_VALUE
#include "src/terminal/kitty.h" // QUERY_ID, so the reply match cannot drift from the query

#include <cstddef>
#include <string>

ReplyScan parse_graphics_replies(const char *buf, int len, TermGraphics &out)
{
    ReplyScan r;
    int i = 0;
    while (i < len)
    {
        if (buf[i] != '\033')
        {
            // Drop keystrokes typed during the one-round-trip query window.
            i++;
            r.consumed = i;
            continue;
        }
        if (i + 1 >= len)
        {
            break; // a bare introducer may still grow
        }
        const char c1 = buf[i + 1];

        if (platform::detail::is_string_introducer(c1))
        {
            // BEL ends OSC; ST ends every string family. Kitty replies use APC.
            const bool bel_terminates = c1 == ']';
            int end = -1;      // one past the terminator
            int body = -1;     // last body byte + 1
            int boundary = -1; // embedded ESC that introduces the next sequence
            for (int j = i + 2; j < len; j++)
            {
                if (bel_terminates && buf[j] == '\a')
                {
                    end = j + 1;
                    body = j;
                    break;
                }
                if (buf[j] == '\033')
                {
                    if (j + 1 >= len)
                    {
                        break; // ST or a boundary: the next byte decides; wait
                    }
                    if (buf[j + 1] == '\\')
                    {
                        end = j + 2;
                        body = j;
                        break;
                    }
                    // Treat a non-ST ESC as the next sequence boundary. Unlike input parsing,
                    // this scanner dispatches no keys, so doing so cannot leak a string tail.
                    boundary = j;
                    break;
                }
            }
            if (boundary >= 0)
            {
                i = boundary;
                r.consumed = i;
                continue;
            }
            if (end < 0)
            {
                break; // incomplete; wait for the rest
            }
            // Accept only exact OK replies carrying one of our query IDs.
            if (c1 == '_' && i + 2 < body && buf[i + 2] == 'G')
            {
                const std::string s(buf + i + 3, buf + body);
                const size_t semi = s.find(';');
                if (semi != std::string::npos && s.compare(semi + 1, std::string::npos, "OK") == 0)
                {
                    auto has_id = [&s, semi](int id)
                    {
                        const std::string id_key = "i=" + std::to_string(id);
                        size_t pos = 0;
                        while ((pos = s.find(id_key, pos)) != std::string::npos && pos < semi)
                        {
                            const bool starts = pos == 0 || s[pos - 1] == ',';
                            const size_t kend = pos + id_key.size();
                            if (starts && (kend == semi || s[kend] == ','))
                            {
                                return true;
                            }
                            pos = kend;
                        }
                        return false;
                    };
                    if (has_id(kitty::QUERY_ID))
                    {
                        out.kitty = true;
                    }
                    else if (has_id(kitty::SHM_QUERY_ID))
                    {
                        out.kitty_shm = true;
                    }
                }
            }
            i = end;
            r.consumed = i;
            continue;
        }

        if (c1 == '[')
        {
            const platform::detail::Scan s = platform::detail::scan_to_csi_final(buf, len, i + 2);
            if (s.kind == platform::detail::Scan::Kind::Incomplete)
            {
                break;
            }
            if (s.kind == platform::detail::Scan::Kind::Boundary)
            {
                // Drop a truncated CSI but preserve the next introducer.
                i = s.index;
                r.consumed = i;
                continue;
            }
            const char fin = buf[s.index];
            if (fin == 'n' && buf[i + 2] != '?')
            {
                // Replies are ordered, so our non-private DSR ends detection. Ignore private
                // DECDSR replies that another program may have left in the input queue.
                r.done = true;
                r.consumed = s.index + 1;
                return r;
            }
            if (fin == 't')
            {
                // Reuse the mid-session XTWINOPS cell-size parser.
                int w = 0;
                int h = 0;
                if (platform::detail::parse_cell_size_body(buf, i + 2, s.index, w, h))
                {
                    out.cell_w = w;
                    out.cell_h = h;
                }
            }
            else if (fin == 'S')
            {
                // Reuse the mid-session XTSMGRAPHICS parser; ignore other items and failures.
                int w = 0;
                int h = 0;
                if (platform::detail::parse_sixel_geometry_body(buf, i + 2, s.index, w, h))
                {
                    out.sixel_max_w = w;
                    out.sixel_max_h = h;
                }
            }
            else if (fin == 'c' && buf[i + 2] == '?')
            {
                // Plain DA1 parameter 4 advertises sixel. A malformed token taints only
                // itself so later valid capabilities remain visible.
                bool tainted = false;
                int v = 0;
                for (int k = i + 3; k <= s.index; k++)
                {
                    // Treat the final as the last separator.
                    const char ch = (k == s.index) ? ';' : buf[k];
                    if (ch == ';')
                    {
                        if (!tainted && v == 4)
                        {
                            out.sixel = true;
                        }
                        v = 0;
                        tainted = false;
                        continue;
                    }
                    if (ch < '0' || ch > '9')
                    {
                        tainted = true;
                        continue;
                    }
                    if (v <= platform::detail::MAX_CSI_PARAM_VALUE)
                    {
                        v = (v * 10) + (ch - '0');
                    }
                }
            }
            i = s.index + 1;
            r.consumed = i;
            continue;
        }

        // Skip an Alt chord or SS3 fragment, but preserve a second ESC as a new introducer.
        i += (c1 == '\033') ? 1 : 2;
        r.consumed = i;
    }
    return r;
}
