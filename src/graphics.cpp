#include "graphics.h"

#include "input.h" // detail::is_string_introducer / scan_to_csi_final / parse_cell_size_body: the shared grammar
#include "kitty.h" // QUERY_ID, so the reply match cannot drift from the query

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
            // A loose byte is a keystroke typed inside the query window; it is
            // dropped with everything else here (accepted: the window is ~one
            // round trip, and the batch ends in a DSR precisely to keep it short).
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
            // Locate the terminator first (BEL ends OSC only, ST ends all five;
            // the same family rule as parse_input). The kitty reply is an APC.
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
                    // An embedded ESC that does not start ST ends this sequence
                    // (stop short of it). Deliberately the OPPOSITE of
                    // parse_input's string arm: its payload rule exists because
                    // ending a string at an ESC hands the tail to the KEY
                    // dispatch, and this scanner dispatches nothing, so that risk
                    // does not exist here; the payload rule instead lets a stray
                    // typed introducer swallow a real reply and take the reply's
                    // ST as its own terminator. Our replies are short single
                    // terminal writes, so nothing can interleave inside one.
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
            // \033_G...: a kitty graphics reply. Ours carry our query ids and
            // the payload "OK" exactly; an error reply (or someone else's id)
            // leaves the queried capability unsupported.
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
                // Truncated CSI chased by another sequence: drop what we have,
                // leave the chasing introducer for the next iteration.
                i = s.index;
                r.consumed = i;
                continue;
            }
            const char fin = buf[s.index];
            if (fin == 'n')
            {
                // The DSR sentinel. The terminal answers requests in order, so
                // every reply it will send is already in the buffer: done.
                r.done = true;
                r.consumed = s.index + 1;
                return r;
            }
            if (fin == 't')
            {
                // \033[6;<height>;<width>t, the cell-size report (XTWINOPS 16),
                // validated by the same parse_cell_size_body as the mid-session
                // input arm so the two cannot drift.
                int w = 0;
                int h = 0;
                if (platform::detail::parse_cell_size_body(buf, i + 2, s.index, w, h))
                {
                    out.cell_w = w;
                    out.cell_h = h;
                }
            }
            i = s.index + 1;
            r.consumed = i;
            continue;
        }

        // ESC + anything else: an alt chord or SS3 fragment typed into the
        // window; skip both bytes. A second ESC is the NEXT sequence's
        // introducer (the boundary rule), so only the stray one is skipped.
        i += (c1 == '\033') ? 1 : 2;
        r.consumed = i;
    }
    return r;
}
