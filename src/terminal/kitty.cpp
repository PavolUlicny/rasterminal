#include "src/terminal/kitty.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace
{

    constexpr char B64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    // 3072 input bytes encode to kitty's 4096-character chunk limit without padding.
    constexpr size_t BYTES_PER_CHUNK = 3072;

    void append_uint(std::string &out, unsigned int v)
    {
        out += std::to_string(v);
    }

    // Suppress frame replies and keep full-height placements from advancing the cursor.
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
        // Resize once and write the exact output length. This measured 3x faster than append().
        const size_t base = out.size();
        out.resize(base + (((n + 2) / 3) * 4));
        char *o = out.data() + base;
        size_t i = 0;
        for (; i + 3 <= n; i += 3)
        {
            const unsigned int v = (static_cast<unsigned int>(data[i]) << 16u) |
                                   (static_cast<unsigned int>(data[i + 1]) << 8u) |
                                   static_cast<unsigned int>(data[i + 2]);
            o[0] = B64_ALPHABET[(v >> 18u) & 63u];
            o[1] = B64_ALPHABET[(v >> 12u) & 63u];
            o[2] = B64_ALPHABET[(v >> 6u) & 63u];
            o[3] = B64_ALPHABET[v & 63u];
            o += 4;
        }
        const size_t rem = n - i;
        if (rem == 1)
        {
            const unsigned int v = static_cast<unsigned int>(data[i]) << 16u;
            o[0] = B64_ALPHABET[(v >> 18u) & 63u];
            o[1] = B64_ALPHABET[(v >> 12u) & 63u];
            o[2] = '=';
            o[3] = '=';
        }
        else if (rem == 2)
        {
            const unsigned int v =
                (static_cast<unsigned int>(data[i]) << 16u) | (static_cast<unsigned int>(data[i + 1]) << 8u);
            o[0] = B64_ALPHABET[(v >> 18u) & 63u];
            o[1] = B64_ALPHABET[(v >> 12u) & 63u];
            o[2] = B64_ALPHABET[(v >> 6u) & 63u];
            o[3] = '=';
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
        // Reserve the full chunk run once. Guard it because pre-C++20 reserve may shrink.
        const size_t need = out.size() + (((data_len + 2) / 3) * 4) + (chunks * 32) + 128;
        if (need > out.capacity())
        {
            out.reserve(need);
        }
        // Kitty forbids interleaving graphics commands within a chunk run.
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
        out += DELETE_IMAGE;
    }

} // namespace kitty
