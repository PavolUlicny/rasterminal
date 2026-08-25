#pragma once

// Stateless terminal escape-sequence grammar. platform.h owns buffering and timing.

#include <cstdint>

namespace platform
{

    enum class Key : std::uint8_t
    {
        None = 0,
        W,
        A,
        S,
        D, // camera orbit
        Q, // quit
        Num1,
        Num2,
        Num3, // shading modes
        Up,
        Down, // pitch
        Left,
        Right, // yaw
        Plus,
        Minus, // zoom
        Space, // toggle auto-rotation
        B,     // toggle background colour
        L,     // cycle lighting preset
        R,     // reset to the flag-specified launch state
        C,     // cycle wireframe colour
        K,     // toggle backface culling
        T,     // toggle texture rendering
        E,     // first-person: move up
        V,     // first-person: move down
    };

    struct InputEvent
    {
        enum class Type : std::uint8_t
        {
            None,
            Key,
            ScrollUp,
            ScrollDown,
            // Button identity is irrelevant to camera dragging.
            MousePress,
            MouseRelease,
            MouseMove, // a button is held and the pointer moved
            // XTWINOPS cell size; x/y carry pixel width/height.
            CellSize,
            // XTSMGRAPHICS sixel limit; x/y carry maximum width/height.
            SixelGeometry,
        } type = Type::None;

        Key key = Key::None; // valid when type == Key
        int x = 0, y = 0;    // terminal cell position (1-based); px sizes for CellSize/SixelGeometry
    };

    namespace detail
    {
        inline Key key_from_char(char c)
        {
            switch (c)
            {
            case 'w':
            case 'W':
                return Key::W;
            case 'a':
            case 'A':
                return Key::A;
            case 's':
            case 'S':
                return Key::S;
            case 'd':
            case 'D':
                return Key::D;
            case 'q':
            case 'Q':
                return Key::Q;
            case '1':
                return Key::Num1;
            case '2':
                return Key::Num2;
            case '3':
                return Key::Num3;
            case '+':
                return Key::Plus;
            case '-':
                return Key::Minus;
            case ' ':
                return Key::Space;
            case 'b':
            case 'B':
                return Key::B;
            case 'l':
            case 'L':
                return Key::L;
            case 'r':
            case 'R':
                return Key::R;
            case 'c':
            case 'C':
                return Key::C;
            case 'k':
            case 'K':
                return Key::K;
            case 't':
            case 'T':
                return Key::T;
            case 'e':
            case 'E':
                return Key::E;
            case 'v':
            case 'V':
                return Key::V;
            default:
                return Key::None;
            }
        }

        // ECMA-48 CSI final byte.
        constexpr bool is_csi_final(char c)
        {
            const auto u = static_cast<unsigned char>(c);
            return u >= 0x40 && u <= 0x7E;
        }

        // Numeric parameters and separators; private markers are handled at the prefix.
        constexpr bool is_csi_param(char c)
        {
            return (c >= '0' && c <= '9') || c == ';';
        }

        // Decode the shared CSI and SS3 arrow final.
        constexpr Key arrow_from_final(char c)
        {
            switch (c)
            {
            case 'A':
                return Key::Up;
            case 'B':
                return Key::Down;
            case 'C':
                return Key::Right;
            case 'D':
                return Key::Left;
            default:
                return Key::None;
            }
        }

        // CSI scan result: terminator, embedded-ESC boundary, or incomplete input.
        struct Scan
        {
            enum class Kind : std::uint8_t
            {
                Final,
                Boundary,
                Incomplete,
            } kind = Kind::Incomplete;
            int index = 0;
        };

        // Centralize the boundary rule so truncated CSI cannot consume the next sequence.
        inline Scan scan_to_csi_final(const char *buf, int len, int from)
        {
            for (int i = from; i < len; i++)
            {
                if (buf[i] == '\033')
                {
                    return Scan{ Scan::Kind::Boundary, i };
                }
                if (is_csi_final(buf[i]))
                {
                    return Scan{ Scan::Kind::Final, i };
                }
            }
            return Scan{};
        }

        // OSC, DCS, SOS, PM, and APC end at BEL or ST, not a CSI final.
        constexpr bool is_string_introducer(char c)
        {
            return c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_';
        }

        // Parser sanity ceiling; main.cpp validates against the actual grid.
        constexpr int MAX_MOUSE_COORD = 10000;

        // SGR mouse encodes the button and flags in one byte.
        constexpr int MAX_SGR_BUTTON = 255;

        // Low bits 3 mean no button only when the extended-button bit is clear.
        constexpr bool sgr_no_button(unsigned int flags)
        {
            return (flags & 3U) == 3U && (flags & 128U) == 0U;
        }

        // Bound parameter accumulation before signed overflow.
        constexpr int MAX_CSI_PARAM_VALUE = 1000000;

        // Shared sanity bound for startup and resize cell-size parsing.
        constexpr int MAX_CELL_REPORT_PX = 1000;

        // X10 has three opaque payload bytes; consume by count, even across ESC.
        constexpr int X10_PAYLOAD_BYTES = 3;

        // One front-of-buffer decision: event, ignored input, or incomplete prefix.
        struct ParseResult
        {
            enum class Kind : std::uint8_t
            {
                Complete,
                Drop,
                Incomplete,
            } kind = Kind::Incomplete;
            int consumed = 0;
            InputEvent event;
        };

        inline ParseResult parse_complete(int consumed, const InputEvent &ev)
        {
            ParseResult r;
            r.kind = ParseResult::Kind::Complete;
            r.consumed = consumed;
            r.event = ev;
            return r;
        }

        inline ParseResult parse_drop(int consumed)
        {
            ParseResult r;
            r.kind = ParseResult::Kind::Drop;
            r.consumed = consumed;
            return r;
        }

        inline ParseResult parse_incomplete()
        {
            return ParseResult{};
        }

        // SGR mouse: locate the terminator before validating the body, so malformed
        // reports are dropped whole instead of leaking bytes as keypresses.
        inline ParseResult parse_sgr_mouse(const char *buf, int len)
        {
            const Scan s = scan_to_csi_final(buf, len, 3);
            if (s.kind == Scan::Kind::Incomplete)
            {
                return parse_incomplete();
            }
            if (s.kind == Scan::Kind::Boundary)
            {
                return parse_drop(s.index);
            }
            const int consumed = s.index + 1;
            const char fin = buf[s.index];
            if (fin != 'M' && fin != 'm')
            {
                return parse_drop(consumed); // a CSI, but not a mouse report
            }

            // Require exactly button, x, and y; never repair malformed drag origins.
            int nums[3] = {};
            bool given[3] = {};
            int ni = 0;
            for (int i = 3; i < s.index; i++)
            {
                const char d = buf[i];
                if (d == ';')
                {
                    if (ni >= 2)
                    {
                        return parse_drop(consumed); // a fourth parameter
                    }
                    ni++;
                    continue;
                }
                if (!is_csi_param(d)) // the ';' case is already handled, so: a digit
                {
                    return parse_drop(consumed);
                }
                // The previous value is bounded, so this multiply cannot overflow.
                nums[ni] = (nums[ni] * 10) + (d - '0');
                given[ni] = true;
                if (nums[ni] > MAX_CSI_PARAM_VALUE)
                {
                    return parse_drop(consumed);
                }
            }
            if (ni != 2 || !given[0] || !given[1] || !given[2])
            {
                return parse_drop(consumed); // short of its three parameters
            }
            // SGR coordinates are 1-based; reject impossible drag origins.
            if (nums[1] < 1 || nums[2] < 1 || nums[1] > MAX_MOUSE_COORD || nums[2] > MAX_MOUSE_COORD ||
                nums[0] > MAX_SGR_BUTTON)
            {
                return parse_drop(consumed);
            }

            InputEvent ev;
            ev.x = nums[1];
            ev.y = nums[2];

            // Decode flags independently so modified wheel events remain wheel events.
            const auto flags = static_cast<unsigned int>(nums[0]);
            if ((flags & 64U) != 0U)
            {
                // Wheel events have no release form.
                if (fin != 'M')
                {
                    return parse_drop(consumed);
                }
                // Only wheel buttons 4/5 are vertical; ignore horizontal 6/7.
                const unsigned int axis = flags & 3U;
                if (axis > 1U)
                {
                    return parse_drop(consumed); // horizontal wheel: nothing bound
                }
                ev.type = axis == 1U ? InputEvent::Type::ScrollDown : InputEvent::Type::ScrollUp;
            }
            else if ((flags & 32U) != 0U)
            {
                // Orbit only for press-form motion with a button held.
                if (sgr_no_button(flags) || fin != 'M')
                {
                    return parse_drop(consumed);
                }
                ev.type = InputEvent::Type::MouseMove; // motion + button held
            }
            else if (fin == 'M')
            {
                // A press must name a button.
                if (sgr_no_button(flags))
                {
                    return parse_drop(consumed);
                }
                ev.type = InputEvent::Type::MousePress;
            }
            else
            {
                ev.type = InputEvent::Type::MouseRelease;
            }
            return parse_complete(consumed, ev);
        }

        // Parse strict XTWINOPS `6;<height>;<width>` for startup and resize paths.
        inline bool parse_cell_size_body(const char *buf, int from, int to, int &w, int &h)
        {
            int nums[3] = {};
            bool given[3] = {};
            int ni = 0;
            for (int i = from; i < to; i++)
            {
                const char d = buf[i];
                if (d == ';')
                {
                    if (ni >= 2)
                    {
                        return false; // a fourth parameter
                    }
                    ni++;
                    continue;
                }
                if (!is_csi_param(d))
                {
                    return false;
                }
                nums[ni] = (nums[ni] * 10) + (d - '0');
                given[ni] = true;
                if (nums[ni] > MAX_CSI_PARAM_VALUE)
                {
                    return false; // bounds the multiply, like the SGR arm
                }
            }
            if (ni != 2 || !given[0] || !given[1] || !given[2] || nums[0] != 6)
            {
                return false;
            }
            if (nums[1] < 1 || nums[1] > MAX_CELL_REPORT_PX || nums[2] < 1 || nums[2] > MAX_CELL_REPORT_PX)
            {
                return false;
            }
            w = nums[2];
            h = nums[1];
            return true;
        }

        // Decode the XTWINOPS reply to a resize-time cell-size request.
        inline ParseResult parse_cell_size_report(const char *buf, int fin)
        {
            const int consumed = fin + 1;
            int w = 0;
            int h = 0;
            if (!parse_cell_size_body(buf, 2, fin, w, h))
            {
                return parse_drop(consumed);
            }
            InputEvent ev;
            ev.type = InputEvent::Type::CellSize;
            ev.x = w;
            ev.y = h;
            return parse_complete(consumed, ev);
        }

        // Parse strict successful XTSMGRAPHICS `?2;0;<width>;<height>` replies.
        inline bool parse_sixel_geometry_body(const char *buf, int from, int to, int &w, int &h)
        {
            if (from >= to || buf[from] != '?')
            {
                return false;
            }
            int nums[4] = {};
            bool given[4] = {};
            int ni = 0;
            for (int i = from + 1; i < to; i++)
            {
                const char d = buf[i];
                if (d == ';')
                {
                    if (ni >= 3)
                    {
                        return false; // a fifth parameter
                    }
                    ni++;
                    continue;
                }
                if (!is_csi_param(d))
                {
                    return false;
                }
                nums[ni] = (nums[ni] * 10) + (d - '0');
                given[ni] = true;
                if (nums[ni] > MAX_CSI_PARAM_VALUE)
                {
                    return false; // bounds the multiply, like the SGR arm
                }
            }
            if (ni != 3 || !given[0] || !given[1] || !given[2] || !given[3] || nums[0] != 2 || nums[1] != 0)
            {
                return false;
            }
            if (nums[2] < 1 || nums[3] < 1)
            {
                return false;
            }
            w = nums[2];
            h = nums[3];
            return true;
        }

        // Decode the resize-time XTSMGRAPHICS geometry reply.
        inline ParseResult parse_sixel_geometry_report(const char *buf, int fin)
        {
            const int consumed = fin + 1;
            int w = 0;
            int h = 0;
            if (!parse_sixel_geometry_body(buf, 2, fin, w, h))
            {
                return parse_drop(consumed);
            }
            InputEvent ev;
            ev.type = InputEvent::Type::SixelGeometry;
            ev.x = w;
            ev.y = h;
            return parse_complete(consumed, ev);
        }

        // Find the end of an overlong sequence without decoding it. String payload ESCs
        // are not boundaries: swallowing interleaved input is safer than dispatching payload.
        inline int skip_scan(const char *buf, int len)
        {
            if (len < 2)
            {
                return 0; // no introducer to read the family from
            }
            // The retained introducer identifies the terminator family.
            const bool string_seq = is_string_introducer(buf[1]);
            const bool bel_terminates = buf[1] == ']';
            for (int i = 2; i < len; i++)
            {
                if (!string_seq)
                {
                    // ESC cannot be a CSI/SS3 parameter; leave it for the next parse.
                    if (buf[i] == '\033')
                    {
                        return i;
                    }
                    if (is_csi_final(buf[i]))
                    {
                        return i + 1;
                    }
                    continue;
                }
                if (bel_terminates && buf[i] == '\a')
                {
                    return i + 1;
                }
                if (buf[i] == '\033')
                {
                    if (i + 1 >= len)
                    {
                        return 0; // ST's second byte has not arrived yet
                    }
                    if (buf[i + 1] == '\\')
                    {
                        return i + 2; // ST: both bytes are ours
                    }
                    // An ESC not followed by '\\' remains string payload.
                }
            }
            return 0;
        }

        // Parse one complete front-of-buffer sequence or request more bytes.
        inline ParseResult parse_input(const char *buf, int len)
        {
            if (len <= 0)
            {
                return parse_incomplete();
            }
            if (buf[0] != '\033')
            {
                const Key k = key_from_char(buf[0]);
                if (k == Key::None)
                {
                    // Consume unbound bytes as Drop, never Key::None.
                    return parse_drop(1);
                }
                InputEvent ev;
                ev.type = InputEvent::Type::Key;
                ev.key = k;
                return parse_complete(1, ev);
            }
            if (len < 2)
            {
                return parse_incomplete(); // bare ESC so far; may still grow
            }

            // SS3 is normally one final byte. Parameter bytes signal a non-standard
            // extended form, which must be consumed whole to avoid leaking its tail.
            if (buf[1] == 'O')
            {
                if (len < 3)
                {
                    return parse_incomplete();
                }
                if (buf[2] == '\033')
                {
                    // Preserve the introducer of a following sequence.
                    return parse_drop(2);
                }
                if (is_csi_param(buf[2]))
                {
                    // Extended SS3 accepts only parameter bytes before its final.
                    for (int i = 3; i < len; i++)
                    {
                        if (is_csi_param(buf[i]))
                        {
                            continue;
                        }
                        return parse_drop(is_csi_final(buf[i]) ? i + 1 : i);
                    }
                    return parse_incomplete();
                }
                const Key k = arrow_from_final(buf[2]);
                if (k == Key::None)
                {
                    return parse_drop(3);
                }
                InputEvent ev;
                ev.type = InputEvent::Type::Key;
                ev.key = k;
                return parse_complete(3, ev);
            }

            // String sequences end at ST; OSC also accepts BEL.
            if (is_string_introducer(buf[1]))
            {
                // BEL is payload outside OSC.
                const bool bel_terminates = buf[1] == ']';
                for (int i = 2; i < len; i++)
                {
                    if (bel_terminates && buf[i] == '\a')
                    {
                        return parse_drop(i + 1);
                    }
                    if (buf[i] == '\033')
                    {
                        if (i + 1 >= len)
                        {
                            return parse_incomplete(); // wait for ST's second byte
                        }
                        if (buf[i + 1] == '\\')
                        {
                            return parse_drop(i + 2); // ST: both bytes are ours
                        }
                        // An ESC not followed by '\\' remains payload; ending here could
                        // dispatch the rest of a terminal reply as user input.
                    }
                }
                return parse_incomplete();
            }

            if (buf[1] == '\033')
            {
                // Drop only the alt prefix so ESC-prefixed arrows remain intact.
                return parse_drop(1);
            }
            if (buf[1] != '[')
            {
                // Ignore an ESC-prefixed ordinary key as an Alt chord.
                return parse_drop(2);
            }
            if (len < 3)
            {
                return parse_incomplete();
            }

            if (buf[2] == '<')
            {
                return parse_sgr_mouse(buf, len);
            }

            // Drop legacy X10 mouse reports by their fixed three-byte payload.
            if (buf[2] == 'M')
            {
                // ESC is a valid wrapped X10 coordinate, so no boundary rule applies.
                const int total = 3 + X10_PAYLOAD_BYTES;
                return len < total ? parse_incomplete() : parse_drop(total);
            }

            // Linux virtual console F1-F5: \033[[A .. \033[[E.
            if (buf[2] == '[')
            {
                if (len < 4)
                {
                    return parse_incomplete();
                }
                // A truncated form must not consume the next sequence introducer.
                // Consume any other trailing byte because \033[[ starts no valid sequence.
                return parse_drop(buf[3] == '\033' ? 3 : 4);
            }

            // Generic CSI: decode supported reports and plain arrows; drop the rest.
            const Scan s = scan_to_csi_final(buf, len, 2);
            if (s.kind == Scan::Kind::Incomplete)
            {
                return parse_incomplete();
            }
            if (s.kind == Scan::Kind::Boundary)
            {
                return parse_drop(s.index);
            }
            if (buf[s.index] == 't')
            {
                return parse_cell_size_report(buf, s.index);
            }
            if (buf[s.index] == 'S')
            {
                return parse_sixel_geometry_report(buf, s.index);
            }
            // Parameterized arrows are modified keys and remain unbound.
            const Key k = (s.index == 2) ? arrow_from_final(buf[2]) : Key::None;
            if (k == Key::None)
            {
                return parse_drop(s.index + 1);
            }
            InputEvent ev;
            ev.type = InputEvent::Type::Key;
            ev.key = k;
            return parse_complete(s.index + 1, ev);
        }
    } // namespace detail
} // namespace platform
