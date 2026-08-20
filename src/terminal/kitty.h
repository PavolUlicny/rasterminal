#pragma once

// kitty graphics protocol escape composition: pure string builders, zero I/O
// (the hud.cpp pattern: this file composes, framebuffer writes). Everything a
// frame transmission needs is appended into a caller-owned string so the
// per-frame buffer is reused, never reallocated.

#include <cstddef>
#include <string>

namespace kitty
{
    // Every frame retransmits under this one id with a=T, which replaces the
    // previous image atomically. Ping-pong ids with delete-before-transmit were
    // tried in the wild (ProteinView) and strobe: the delete opens a visible
    // no-image gap. Do not add per-frame deletes.
    constexpr int IMAGE_ID = 1;

    // Every transmit also names this fixed placement id (p=1). Without p= a
    // placement is anonymous, and the spec makes each one NEW: kitty happens to
    // free them with the replaced image data, but ghostty keeps every anonymous
    // placement (plus a tracked pin each) until a resize or delete, growing its
    // render cost by one placement per frame: measured 60 fps decaying to ~42
    // within 30 s of -S, dragging the whole compositor down. An explicit id
    // makes the placement itself replace on both (verified in both sources).
    constexpr int PLACEMENT_ID = 1;

    // Ids the capability queries are sent under; replies echo them, which is how
    // the detection scanner tells our replies from any other APC in the stream
    // and from each other.
    constexpr int QUERY_ID = 31;
    constexpr int SHM_QUERY_ID = 32;

    // Capability probe: a 1x1 RGB direct transmit with a=q. A terminal that
    // supports the protocol must answer immediately ("\033_Gi=31;OK\033\\" or an
    // error); one that does not stays silent, which is why the detection batch
    // ends in a DSR the terminal will answer either way. Deliberately no q= key:
    // the reply is the point here, everywhere else it is suppressed.
    inline constexpr char QUERY[] = "\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\";

    // Standard base64 (padded) of data appended to out.
    void append_base64(std::string &out, const unsigned char *data, size_t n);

    // Probe the shared-memory medium END TO END: a=q with t=s naming a real 1x1
    // object the caller created. A terminal that cannot open the object (it runs
    // on the other end of an ssh session, or in a sandbox with its own /dev/shm)
    // answers with an error rather than OK, so the transport is verified before
    // the first frame; frames themselves carry q=2, which would suppress that
    // error into a silent blank screen.
    void append_query_shm(std::string &out, const char *shm_name);

    // Full transmit-and-display of a frame via the direct medium (t=d): the
    // payload (raw 3-bytes-per-pixel RGB, or its RFC 1950 zlib stream when
    // `deflated`, which adds o=z) is base64-encoded and chunked per the
    // protocol, all control keys included. width/height are the image's pixel
    // dimensions either way; cols and rows are the cell rectangle the terminal
    // scales the image to (c=/r=), so cell pixel geometry never has to be exact
    // for the image to fill it. Whether and how to compress is the caller's
    // policy; this only spells the wire format.
    void append_transmit_direct(
        std::string &out,
        const unsigned char *data,
        size_t data_len,
        int width,
        int height,
        int cols,
        int rows,
        bool deflated
    );

    // Transmit-and-display via the shared-memory medium (t=s): the payload is
    // only the base64 of the POSIX shm object name, so this is a single
    // escape of ~60 bytes regardless of frame size. The terminal reads the
    // object and then unlinks and closes it.
    void append_transmit_shm(std::string &out, const char *shm_name, int width, int height, int cols, int rows);

    // Delete the image and free its data terminal-side (a=d,d=I). Emitted once
    // on exit; without it the last frame stays resident in the terminal.
    void append_delete(std::string &out);
} // namespace kitty
