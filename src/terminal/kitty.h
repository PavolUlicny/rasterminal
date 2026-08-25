#pragma once

// Kitty protocol builders. They append to caller-owned buffers and perform no I/O.

#include <cstddef>
#include <string>

namespace kitty
{
    // Reusing one image ID with a=T replaces frames atomically. Per-frame deletes strobe.
    constexpr int IMAGE_ID = 1;

    // A fixed placement ID prevents Ghostty from accumulating anonymous placements each frame.
    constexpr int PLACEMENT_ID = 1;

    // Echoed query IDs distinguish our capability replies from unrelated APCs.
    constexpr int QUERY_ID = 31;
    constexpr int SHM_QUERY_ID = 32;

    // A 1x1 query transmit. Unsupported terminals stay silent; the batch's DSR bounds the wait.
    inline constexpr char QUERY[] = "\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\";

    // Standard base64 (padded) of data appended to out.
    void append_base64(std::string &out, const unsigned char *data, size_t n);

    // Probe a real shared-memory object before frame replies are suppressed with q=2.
    void append_query_shm(std::string &out, const char *shm_name);

    // Append a chunked direct transmit. `deflated` selects RFC 1950 zlib payloads.
    // Pixel dimensions describe the source; rows and columns describe its placement.
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

    // Append a shared-memory transmit containing only the encoded object name.
    void append_transmit_shm(std::string &out, const char *shm_name, int width, int height, int cols, int rows);

    // Free the terminal-side image on exit.
    void append_delete(std::string &out);
} // namespace kitty
