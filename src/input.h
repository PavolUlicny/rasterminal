#pragma once

// Terminal input decoding: the event vocabulary and the escape-sequence grammar.
//
// Deliberately free of platform code and of state. parse_input is a pure function
// over a byte span (no file descriptors, no clock, no blocking, nothing carried
// between calls), so the whole grammar is unit-testable on every platform, and
// platform.h stays the thin #ifdef layer it claims to be. platform.h owns the
// reading half: it holds the buffer, decides how long to wait for the rest of a
// sequence and what to do with one too long to hold, feeds bytes here, and turns
// the results into events. None of that policy belongs to the grammar.

#include <cstdint>

namespace platform
{
    // ─── input events ────────────────────────────────────────────────────────────

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
    };

    struct InputEvent
    {
        enum class Type : std::uint8_t
        {
            None,
            Key,
            ScrollUp,
            ScrollDown,
            // The button number is not decoded, since nothing reads it: any button
            // presses, releases, and drags the camera alike.
            MousePress,
            MouseRelease,
            MouseMove, // a button is held and the pointer moved
        } type = Type::None;

        Key key = Key::None; // valid when type == Key
        int x = 0, y = 0;    // terminal cell position (1-based)
    };

    // ─── input parsing helpers ───────────────────────────────────────────────────

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
            default:
                return Key::None;
            }
        }

        // ECMA-48 CSI final byte range (0x40..0x7E): the byte that terminates a
        // \033[... sequence.
        constexpr bool is_csi_final(char c)
        {
            const auto u = static_cast<unsigned char>(c);
            return u >= 0x40 && u <= 0x7E;
        }

        // The bytes that may appear between an introducer and a final: the numeric
        // parameters and the ';' that separates them. Not the whole ECMA-48 parameter
        // range (0x30..0x3F), which also holds the private markers '<'..'?'; those
        // only ever lead, and the arms that accept them test for them by position.
        constexpr bool is_csi_param(char c)
        {
            return (c >= '0' && c <= '9') || c == ';';
        }

        // Arrow-key final byte, shared by the CSI (\033[A) and SS3/DECCKM (\033OA)
        // forms. Returns Key::None for any non-arrow final.
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

        // Outcome of scanning for a CSI terminator from `from`.
        //   Final      - `index` is the terminating byte
        //   Boundary   - `index` is an embedded ESC: the sequence was truncated and
        //                the next one has already started
        //   Incomplete - neither seen yet; more bytes are needed
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

        // Single home for the ESC-boundary rule on the CSI-final arms. Keeping it in
        // one place matters: the rule was twice added to some arms and forgotten on
        // others, and each time the omission let a truncated sequence swallow the
        // next one's introducer and dispatch its body as keypresses.
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

        // String-sequence introducer: OSC, DCS, SOS, PM, APC. These carry a text
        // payload terminated by BEL or ST, not a single final byte, so they need
        // their own consume arm (falling through to the alt+key drop would consume
        // two bytes and stream the payload into the dispatch as keypresses).
        constexpr bool is_string_introducer(char c)
        {
            return c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_';
        }

        // Upper sanity bound on a mouse coordinate, in terminal cells. Far beyond any
        // real terminal (the widest run to the low thousands), so it rejects only
        // garbage: without it a report can name a cell a million columns out.
        //
        // A ceiling and nothing more, because the grammar cannot know the terminal's
        // size. What lies between this and the real size is not the parser's to judge
        // and is caught where that size is known, by main.cpp treating an impossible
        // drag delta as a discontinuity rather than a movement.
        constexpr int MAX_MOUSE_COORD = 10000;

        // Largest meaningful SGR button parameter. The encoding is eight flag bits
        // (button, shift, meta, ctrl, motion, wheel, extended), so anything above
        // this is not a button description at all and the report is malformed. The
        // coordinates are range-checked for the same reason; without this the button
        // is bit-decoded regardless, and a garbage value still selects an arm.
        constexpr int MAX_SGR_BUTTON = 255;

        // In the SGR button byte, bit 7 selects the extended buttons 8-11, so the low
        // two bits mean "no button held" only when it is clear: with it set, 3 is
        // button 11 and a drag with it is ordinary input.
        constexpr bool sgr_no_button(unsigned int flags)
        {
            return (flags & 3U) == 3U && (flags & 128U) == 0U;
        }

        // Ceiling for CSI parameter accumulation. Real coordinates are four digits;
        // this only stops a long digit run from overflowing the int (signed
        // overflow is UB, and the value reaches main.cpp as a mouse coordinate).
        constexpr int MAX_CSI_PARAM_VALUE = 1000000;

        // X10 mouse report payload: exactly three bytes after \033[M. Each is
        // biased by 32 and wraps past column 223, so any byte value is legitimate
        // payload, including 0x00 and ESC. It is consumed by count, never scanned.
        constexpr int X10_PAYLOAD_BYTES = 3;

        // What one parse step decided about the front of the buffer.
        //   Complete   - `consumed` bytes are an event to report. A Key event always
        //                names a key: a byte with no binding is a Drop, so no caller
        //                has to filter Key::None out of the dispatch.
        //   Drop       - `consumed` bytes are a valid sequence with no binding
        //   Incomplete - the bytes so far are a prefix; more are needed
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

        // SGR mouse: \033[<btn;x;yM (press/motion/scroll) or ...m (release).
        // `len` is the whole buffer; the parameters start after the '<'.
        //
        // Locate the terminator first, exactly like the other CSI arms, then decide.
        // Deciding byte-by-byte instead would abandon the sequence at the first
        // unexpected character and leave the remainder to dispatch: a stray byte in
        // "\033[<0 1;2M" would surface the "1" and "2" as shading-mode keys.
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

            // Everything between '<' and the final must be exactly three numeric
            // parameters. Anything else is malformed, and a malformed report is
            // dropped rather than repaired: main.cpp takes a press as the drag
            // origin, so a wrong coordinate snaps the camera on the next motion.
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
                // Tested after the accumulate, so the ceiling means what it says: a
                // value past it is meaningless as a coordinate and the report is
                // dropped rather than handed to main.cpp. Testing after also bounds
                // the multiply, since the running value is at most the ceiling on
                // entry and so cannot overflow (UB).
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
            // SGR coordinates are 1-based cell positions, so a zero is not a position
            // and a value past any terminal's width is not one either. Dropping both
            // matters because main.cpp takes a press as the drag origin: accepting one
            // reintroduces exactly the snap the parameter check prevents.
            if (nums[1] < 1 || nums[2] < 1 || nums[1] > MAX_MOUSE_COORD || nums[2] > MAX_MOUSE_COORD ||
                nums[0] > MAX_SGR_BUTTON)
            {
                return parse_drop(consumed);
            }

            InputEvent ev;
            ev.x = nums[1];
            ev.y = nums[2];

            // Flag bits, not exact codes: modifiers ride in bits 2-4, so ctrl+wheel
            // (80/81) and shift+wheel (68/69) would otherwise miss an equality test
            // and fall through to the motion arm, snapping the camera by the delta
            // from a stale drag position.
            const auto flags = static_cast<unsigned int>(nums[0]);
            if ((flags & 64U) != 0U)
            {
                // A wheel notch is a press and has no release, so the 'm' form is
                // malformed; reporting it would zoom a second time per notch.
                if (fin != 'M')
                {
                    return parse_drop(consumed);
                }
                // The low two bits select the wheel axis, so only 0 and 1 are the
                // vertical pair. Horizontal wheels report 2 and 3 (buttons 6/7);
                // reading bit 0 alone would decode those as vertical and zoom.
                const unsigned int axis = flags & 3U;
                if (axis > 1U)
                {
                    return parse_drop(consumed); // horizontal wheel: nothing bound
                }
                ev.type = axis == 1U ? InputEvent::Type::ScrollDown : InputEvent::Type::ScrollUp;
            }
            else if ((flags & 32U) != 0U)
            {
                // Motion counts only while a button is actually held and only in the
                // press form: no-button motion is what a terminal in mode 1003 sends
                // for a bare pointer move, and a motion-flagged release is malformed.
                // Either would otherwise orbit the camera without a drag.
                if (sgr_no_button(flags) || fin != 'M')
                {
                    return parse_drop(consumed);
                }
                ev.type = InputEvent::Type::MouseMove; // motion + button held
            }
            else if (fin == 'M')
            {
                // Same no-button test as the motion arm: there is no such thing as
                // pressing "no button", and letting it through would seed the drag
                // origin from a report that never named a button.
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

        // Where the sequence at the front ends, when it is being skipped because it
        // is too long to hold. Returns the count through its terminator, or 0 while
        // that terminator has not arrived. Nothing is decoded: a skipped sequence is
        // missing its middle, so the only question left about it is where it stops.
        //
        // Deliberately does NOT treat an embedded ESC as a boundary, which is the one
        // way it differs from parse_input, and the reason it exists separately. While
        // a sequence is being skipped, an ESC is either payload of a reply that is not
        // conforming, or the start of an ordinary sequence the terminal wrote between
        // two chunks of this one. In both cases THIS sequence has not ended, and
        // treating the ESC as its end hands the rest of the reply to the dispatch as
        // keypresses: measured, a clipboard reply with a scroll notch delivered inside
        // it dispatched its tail as keys, quit key included. Whatever is interleaved is
        // swallowed instead, which is the direction this design always errs in.
        //
        // The cost is that a reply the terminal really did abandon mid-string no longer
        // yields to the next keypress; it runs to the rate floor. Measured at 1.9 s to
        // recover once whatever else was arriving stops. While it does not stop, the
        // floor cannot fire, so a never-terminating reply plus continuous input (a
        // mouse drag clears 256 B/s comfortably) swallows that input for as long as it
        // lasts. Accepted: it needs a terminal that opens a string sequence and never
        // closes it, which a conforming one does not do, and Ctrl+C still quits
        // throughout, because raw mode leaves ISIG set and main.cpp checks the signal
        // flag outside the input drain. The alternative is the leak this rule exists to
        // stop, which ends the session outright rather than delaying it.
        inline int skip_scan(const char *buf, int len)
        {
            if (len < 2)
            {
                return 0; // no introducer to read the family from
            }
            // The introducer is kept precisely so the family, and therefore the
            // terminator to look for, is still known here.
            const bool string_seq = is_string_introducer(buf[1]);
            const bool bel_terminates = buf[1] == ']';
            for (int i = 2; i < len; i++)
            {
                if (!string_seq)
                {
                    // CSI and SS3 carry parameters and then a final, an alphabet an ESC
                    // can never belong to, so an ESC really does end this one: stop
                    // SHORT of it, leaving whatever it introduces to parse whole. That
                    // matters because '[' is itself a legal final (0x5B), so without
                    // this an interleaved sequence's own introducer would end the skip
                    // and strand the rest of it for the dispatch (measured: an X10
                    // report delivered inside a skipped CSI surfaced Space and Q).
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
                    // Not ST, so ordinary payload as far as the skip is concerned.
                }
            }
            return 0;
        }

        // Decides the front of the buffer. Pure: no file descriptors, no clock, no
        // blocking, so it is directly unit-testable on every platform.
        //
        // Every sequence family is consumed in full or not at all. A family whose
        // terminator is not a plain CSI final needs its own arm, because the generic
        // scan stops at the first byte in 0x40-0x7E: SS3 is single-shot, string
        // payloads run to BEL or ST, X10's payload is opaque and counted, and the
        // Linux console's F1-F5 put a second '[' (0x5B, itself a legal final) where
        // parameters would go.
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
                    // A byte with no binding is consumed and reported as nothing, the
                    // same as a sequence with no binding. Reporting it as a keypress
                    // carrying Key::None only moved the question to the dispatch,
                    // which then had to remember that a Key event may name no key: the
                    // one place that forgot cancelled an in-progress camera movement
                    // on every unrelated keystroke.
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

            // SS3: \033O + exactly one byte. A-D are the arrows in DECCKM
            // application cursor mode. Single-shot by definition, so the normal form
            // is never scanned for a final: doing so would let a stray \033O (which
            // is also the alt+shift+O chord) swallow real typing, since every letter
            // key is a legal final.
            //
            // The exception is a parameter byte, which no single-shot SS3 can carry:
            // that means a non-standard parameterized form, and consuming only three
            // bytes there would leave its tail to dispatch as keypresses ('Q' quits).
            // Scanning is bounded here because the alphabet is constrained to
            // parameter bytes, unlike the single-shot form. The cost is that after an
            // alt+shift+O chord and a typed digit, the scan also consumes the next
            // key if it lands in the final range, which is every letter including the
            // quit key. That needs the chord, then a digit, then the key, all inside
            // the reassembly window.
            if (buf[1] == 'O')
            {
                if (len < 3)
                {
                    return parse_incomplete();
                }
                if (buf[2] == '\033')
                {
                    // Truncated: the next sequence has started. Counted arms need the
                    // same boundary rule as the scanning ones, or the count eats that
                    // sequence's introducer and its body dispatches as keypresses.
                    return parse_drop(2);
                }
                if (is_csi_param(buf[2]))
                {
                    // Only parameter bytes may precede the final. Scanning for any
                    // final instead would swallow every following key whose byte is
                    // below 0x40 (space, '+', '-'), not just the digits this form can
                    // legitimately carry.
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

            // String sequences (OSC/DCS/SOS/PM/APC): payload terminated by BEL or by
            // ST (ESC backslash). Terminal replies arrive in this form, including a
            // reply to a query some earlier program made whose answer lands in our
            // stdin. Both terminator bytes are consumed, so nothing trails.
            if (is_string_introducer(buf[1]))
            {
                // BEL terminates OSC only. It is an xterm convenience for that one
                // family; for DCS/SOS/PM/APC a 0x07 is ordinary payload, and treating
                // it as the end would cut the reply short and dispatch the rest.
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
                        // Not ST, so ordinary payload as far as this sequence is
                        // concerned, and the scan goes on. Unlike a CSI, whose
                        // alphabet an ESC can never belong to, a string payload is
                        // arbitrary text delivered in pieces, so an ESC inside one is
                        // either payload or a sequence the terminal wrote between two
                        // of those pieces. Either way THIS sequence continues, and
                        // ending it here hands its tail to the dispatch: measured, a
                        // 307-byte clipboard reply with a scroll notch inside it
                        // surfaced 102 keypresses including two quits. Whatever is
                        // interleaved is swallowed instead. One that never terminates
                        // fills the buffer and becomes a skip, which the rate floor
                        // ends; see skip_scan, which applies the same rule.
                    }
                }
                return parse_incomplete();
            }

            if (buf[1] == '\033')
            {
                // altSendsEscape prefixes a whole sequence with a second ESC, so
                // alt+Up is ESC ESC [ A. Drop only the prefix: the second ESC then
                // starts a fresh parse and the arrow is reported normally, where
                // consuming both bytes would leave "[A" to dispatch as a literal
                // bracket and an orbit. alt+Escape (ESC ESC alone) leaves a bare ESC
                // that ages out.
                return parse_drop(1);
            }
            if (buf[1] != '[')
            {
                // alt+key: altSendsEscape prefixes the key byte with ESC, so both
                // bytes are consumed and the chord does nothing.
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

            // X10 mouse: \033[M + 3 payload bytes, sent by terminals that take
            // \033[?1002h but not the SGR extension. 'M' is a legal CSI final, so the
            // payload must be counted rather than scanned; every byte value is
            // legitimate (a coordinate past column 223 wraps into the control range).
            // Dropped rather than decoded: rasterminal negotiates the SGR form.
            if (buf[2] == 'M')
            {
                // The deliberate exception to the ESC-boundary rule that every other
                // arm follows. Each payload byte is a coordinate biased by 32, and
                // past column 223 it wraps into the control range, so ESC (and 0x00)
                // are legitimate payload: a click at column 251 sends one. Treating
                // ESC as a boundary here truncates that report and leaks its
                // remaining coordinate bytes as keypresses, which are printable and
                // can be the quit key. The payload is therefore consumed by count,
                // never by value.
                //
                // The cost is that a truncated report chased by another sequence
                // swallows three bytes of it. That needs a terminal to emit a
                // malformed X10 report, since a merely slow one is held by the
                // buffer until it is whole, whereas the wide-column click is
                // ordinary input. Both directions were probe-checked before choosing.
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
                // Same boundary rule as every other arm: a truncated form must not
                // let its byte count swallow the next sequence's introducer. Any
                // other trailing byte is still consumed, since \033[[ begins nothing
                // else and swallowing beats leaving a stray byte to dispatch.
                return parse_drop(buf[3] == '\033' ? 3 : 4);
            }

            // Generic CSI: consume through the final byte. An unparameterized arrow
            // is the one binding here; everything else (F5-F12, Home/End, Delete,
            // modified arrows, shift-tab, focus events) is dropped.
            const Scan s = scan_to_csi_final(buf, len, 2);
            if (s.kind == Scan::Kind::Incomplete)
            {
                return parse_incomplete();
            }
            if (s.kind == Scan::Kind::Boundary)
            {
                return parse_drop(s.index);
            }
            // Only the unparameterized form is an arrow: a final at any later index
            // means parameters were present, which is a modified arrow, not one.
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
