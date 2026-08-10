#include "kitty.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace
{

    constexpr char B64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    // Chunk granularity in INPUT bytes. 3072 bytes encode to exactly 4096 base64
    // chars, the protocol's per-chunk ceiling; being a multiple of 3 it never
    // produces padding mid-stream, so every chunk but the last is a multiple of 4
    // as the protocol requires and the encoder needs no cross-chunk carry state.
    constexpr size_t BYTES_PER_CHUNK = 3072;

    void append_uint(std::string &out, unsigned int v)
    {
        out += std::to_string(v);
    }

    // Control keys shared by both transmit mediums. q=2 suppresses the OK and
    // error responses, which would otherwise land in stdin for the input parser
    // to swallow. C=1 keeps the cursor where it is: the placement spans the whole
    // render area, and the default cursor advance would land it off-screen at the
    // bottom of a full-height image.
    void append_transmit_keys(std::string &out, char medium, int width, int height, int cols, int rows)
    {
        out += "q=2,i=";
        append_uint(out, kitty::IMAGE_ID);
        out += ",p=";
        append_uint(out, kitty::PLACEMENT_ID);
        out += ",a=T,t=";
        out += medium;
        out += ",f=24,s=";
        append_uint(out, static_cast<unsigned int>(width));
        out += ",v=";
        append_uint(out, static_cast<unsigned int>(height));
        out += ",c=";
        append_uint(out, static_cast<unsigned int>(cols));
        out += ",r=";
        append_uint(out, static_cast<unsigned int>(rows));
        out += ",C=1";
    }

    void append_base64_str(std::string &out, const char *s)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): char to byte view of the same array
        kitty::append_base64(out, reinterpret_cast<const unsigned char *>(s), std::strlen(s));
    }

} // namespace

namespace kitty
{

    void append_base64(std::string &out, const unsigned char *data, size_t n)
    {
        // One reserve and quad-at-a-time appends: per-character += on the
        // direct-transport hot path pays the append machinery four times per
        // group, and the exact output size is known up front. Guarded, because
        // before C++20 (P0966) reserve below the current capacity was allowed to
        // SHRINK it: unguarded, each per-chunk call under append_transmit_direct's
        // whole-frame reservation could shrink and regrow on such a library.
        const size_t need = out.size() + (((n + 2) / 3) * 4);
        if (need > out.capacity())
        {
            out.reserve(need);
        }
        char quad[4];
        size_t i = 0;
        for (; i + 3 <= n; i += 3)
        {
            const unsigned int v = (static_cast<unsigned int>(data[i]) << 16u) |
                                   (static_cast<unsigned int>(data[i + 1]) << 8u) |
                                   static_cast<unsigned int>(data[i + 2]);
            quad[0] = B64_ALPHABET[(v >> 18u) & 63u];
            quad[1] = B64_ALPHABET[(v >> 12u) & 63u];
            quad[2] = B64_ALPHABET[(v >> 6u) & 63u];
            quad[3] = B64_ALPHABET[v & 63u];
            out.append(quad, 4);
        }
        const size_t rem = n - i;
        if (rem == 1)
        {
            const unsigned int v = static_cast<unsigned int>(data[i]) << 16u;
            quad[0] = B64_ALPHABET[(v >> 18u) & 63u];
            quad[1] = B64_ALPHABET[(v >> 12u) & 63u];
            quad[2] = '=';
            quad[3] = '=';
            out.append(quad, 4);
        }
        else if (rem == 2)
        {
            const unsigned int v =
                (static_cast<unsigned int>(data[i]) << 16u) | (static_cast<unsigned int>(data[i + 1]) << 8u);
            quad[0] = B64_ALPHABET[(v >> 18u) & 63u];
            quad[1] = B64_ALPHABET[(v >> 12u) & 63u];
            quad[2] = B64_ALPHABET[(v >> 6u) & 63u];
            quad[3] = '=';
            out.append(quad, 4);
        }
    }

    void append_transmit_direct(
        std::string &out,
        const unsigned char *data,
        size_t data_len,
        int width,
        int height,
        int cols,
        int rows,
        bool deflated
    )
    {
        const size_t chunks = std::max<size_t>(1, (data_len + BYTES_PER_CHUNK - 1) / BYTES_PER_CHUNK);
        // One reservation for the whole run, keys included: the per-chunk
        // append_base64 reserves only its own output, which would leave the
        // frame's total growth to the library's policy (a doubling chain on
        // libstdc++, measured 11 reallocations at 8 MB; possibly one exact
        // reallocation PER CHUNK on a library that honors reserve literally).
        // 32 B per chunk + 128 B flat covers the framing (a continuation header
        // is ~13 B, the first chunk's key run ~90 B). Guarded like append_base64:
        // from the second frame on, the persisting capacity already covers the
        // need, and an unguarded pre-C++20 reserve may SHRINK to it (P0966).
        const size_t need = out.size() + (((data_len + 2) / 3) * 4) + (chunks * 32) + 128;
        if (need > out.capacity())
        {
            out.reserve(need);
        }
        // The whole image must go out as one uninterrupted run of chunks: the
        // protocol forbids any other graphics escape between them.
        for (size_t c = 0; c < chunks; c++)
        {
            const size_t off = c * BYTES_PER_CHUNK;
            const size_t take = std::min(BYTES_PER_CHUNK, data_len - off);
            const bool last = c + 1 == chunks;
            out += "\033_G";
            if (c == 0)
            {
                append_transmit_keys(out, 'd', width, height, cols, rows);
                if (deflated)
                {
                    out += ",o=z";
                }
                if (chunks > 1)
                {
                    out += ",m=1";
                }
            }
            else
            {
                out += last ? "q=2,m=0" : "q=2,m=1";
            }
            out += ';';
            append_base64(out, data + off, take);
            out += "\033\\";
        }
    }

    void append_transmit_shm(std::string &out, const char *shm_name, int width, int height, int cols, int rows)
    {
        out += "\033_G";
        append_transmit_keys(out, 's', width, height, cols, rows);
        out += ';';
        append_base64_str(out, shm_name);
        out += "\033\\";
    }

    void append_query_shm(std::string &out, const char *shm_name)
    {
        out += "\033_Gi=";
        append_uint(out, SHM_QUERY_ID);
        out += ",s=1,v=1,a=q,t=s,f=24;";
        append_base64_str(out, shm_name);
        out += "\033\\";
    }

    void append_delete(std::string &out)
    {
        out += "\033_Gq=2,a=d,d=I,i=";
        append_uint(out, IMAGE_ID);
        out += "\033\\";
    }

} // namespace kitty
