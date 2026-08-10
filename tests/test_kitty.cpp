#include "src/kitty.h"
#include "tests/b64_test_util.h"
#include "tests/test.h"

#include "miniz.h" // independent inflate for the deflated-transmit round trip

#include <cstddef>
#include <string>
#include <vector>

namespace
{

    struct Apc
    {
        std::string keys;    // control data between \033_G and ';' (or ST)
        std::string payload; // after ';', before ST; empty if no ';'
    };

    // Splits a byte stream into its APC records, asserting the framing is exact:
    // every record starts \033_G and ends with ST, nothing between records.
    std::vector<Apc> split_apcs(const std::string &s)
    {
        std::vector<Apc> out;
        size_t i = 0;
        while (i < s.size())
        {
            ASSERT_EQ(s.compare(i, 3, "\033_G"), 0);
            i += 3;
            const size_t st = s.find("\033\\", i);
            ASSERT_TRUE(st != std::string::npos);
            const std::string body = s.substr(i, st - i);
            const size_t semi = body.find(';');
            Apc a;
            if (semi == std::string::npos)
            {
                a.keys = body;
            }
            else
            {
                a.keys = body.substr(0, semi);
                a.payload = body.substr(semi + 1);
            }
            out.push_back(a);
            i = st + 2;
        }
        return out;
    }

    // Whole-key match: "m=1" must not match inside "m=10".
    bool has_key(const Apc &a, const std::string &kv)
    {
        size_t pos = 0;
        while ((pos = a.keys.find(kv, pos)) != std::string::npos)
        {
            const bool starts = pos == 0 || a.keys[pos - 1] == ',';
            const size_t end = pos + kv.size();
            const bool ends = end == a.keys.size() || a.keys[end] == ',';
            if (starts && ends)
            {
                return true;
            }
            pos = end;
        }
        return false;
    }

    std::vector<unsigned char> test_rgb(int width, int height)
    {
        std::vector<unsigned char> rgb(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
        for (size_t i = 0; i < rgb.size(); i++)
        {
            rgb[i] = static_cast<unsigned char>(((i * 7u) + 3u) & 0xFF);
        }
        return rgb;
    }

    // Reassembles the chunked payload and asserts the framing rules the protocol
    // states: only the first record carries the control keys, every record but
    // the last says m=1, the last m=0 (or no m at all when there is only one),
    // and every non-final chunk is a multiple of 4 base64 chars within the 4096
    // ceiling.
    std::vector<unsigned char> decode_direct(const std::vector<Apc> &apcs, int width, int height)
    {
        ASSERT_TRUE(!apcs.empty());
        const Apc &head = apcs[0];
        ASSERT_TRUE(has_key(head, "a=T"));
        ASSERT_TRUE(has_key(head, "t=d"));
        ASSERT_TRUE(has_key(head, "f=24"));
        ASSERT_TRUE(has_key(head, "q=2"));
        ASSERT_TRUE(has_key(head, "C=1"));
        ASSERT_TRUE(has_key(head, "i=" + std::to_string(kitty::IMAGE_ID)));
        // p= pins the one placement that a=T replaces each frame; an anonymous
        // placement would be a NEW one per frame, which ghostty accumulates.
        ASSERT_TRUE(has_key(head, "p=" + std::to_string(kitty::PLACEMENT_ID)));
        ASSERT_TRUE(has_key(head, "s=" + std::to_string(width)));
        ASSERT_TRUE(has_key(head, "v=" + std::to_string(height)));
        std::string b64;
        for (size_t i = 0; i < apcs.size(); i++)
        {
            const Apc &a = apcs[i];
            const bool last = i + 1 == apcs.size();
            if (i > 0)
            {
                ASSERT_TRUE(has_key(a, "q=2"));
                ASSERT_FALSE(has_key(a, "a=T")); // control keys only on the first chunk
            }
            if (apcs.size() == 1)
            {
                ASSERT_FALSE(has_key(a, "m=1"));
                ASSERT_FALSE(has_key(a, "m=0"));
            }
            else
            {
                ASSERT_TRUE(has_key(a, last ? "m=0" : "m=1"));
            }
            ASSERT_TRUE(a.payload.size() <= 4096u);
            if (!last)
            {
                ASSERT_EQ(a.payload.size() % 4u, 0u);
            }
            b64 += a.payload;
        }
        return b64_decode(b64);
    }

} // namespace

TEST(kitty, base64_known_vectors)
{
    auto enc = [](const std::string &in)
    {
        std::string out;
        kitty::append_base64(out, reinterpret_cast<const unsigned char *>(in.data()), in.size());
        return out;
    };
    ASSERT_TRUE(enc("").empty());
    ASSERT_TRUE(enc("f") == "Zg==");
    ASSERT_TRUE(enc("fo") == "Zm8=");
    ASSERT_TRUE(enc("foo") == "Zm9v");
    ASSERT_TRUE(enc("foobar") == "Zm9vYmFy");
}

TEST(kitty, single_chunk_roundtrip)
{
    const int w = 2;
    const int h = 2;
    const auto rgb = test_rgb(w, h);
    std::string out;
    kitty::append_transmit_direct(out, rgb.data(), rgb.size(), w, h, 80, 23, /*deflated=*/false);
    const auto apcs = split_apcs(out);
    ASSERT_EQ(apcs.size(), 1u);
    ASSERT_TRUE(has_key(apcs[0], "c=80"));
    ASSERT_TRUE(has_key(apcs[0], "r=23"));
    const auto decoded = decode_direct(apcs, w, h);
    ASSERT_TRUE(decoded == rgb);
    ASSERT_FALSE(has_key(apcs[0], "o=z")); // raw form carries no compression key
}

TEST(kitty, deflated_transmit_roundtrip)
{
    // The caller compresses; the emitter only frames. Chunking still holds for
    // an arbitrary-length zlib stream because padding can only ever land on the
    // final chunk (the chunk size is a multiple of 3).
    const int w = 40;
    const int h = 30;
    const auto rgb = test_rgb(w, h);
    // unsigned int intermediary for the same -Wuseless-cast/LLP64 reason as
    // framebuffer.cpp's transmit_direct.
    const auto src_len = static_cast<unsigned int>(rgb.size());
    mz_ulong z_len = mz_compressBound(src_len);
    std::vector<unsigned char> z(z_len);
    ASSERT_EQ(mz_compress2(z.data(), &z_len, rgb.data(), src_len, 1), MZ_OK);
    std::string out;
    kitty::append_transmit_direct(out, z.data(), z_len, w, h, 10, 10, /*deflated=*/true);
    const auto apcs = split_apcs(out);
    ASSERT_TRUE(has_key(apcs[0], "o=z"));
    const auto payload = decode_direct(apcs, w, h);
    mz_ulong inflated_len = src_len;
    std::vector<unsigned char> inflated(inflated_len);
    ASSERT_EQ(
        mz_uncompress(inflated.data(), &inflated_len, payload.data(), static_cast<unsigned int>(payload.size())), MZ_OK
    );
    ASSERT_EQ(inflated_len, src_len);
    ASSERT_TRUE(inflated == rgb);
}

TEST(kitty, exact_chunk_boundary_is_one_chunk)
{
    // 32x32 = 3072 bytes = exactly 4096 base64 chars, the per-chunk ceiling.
    const int w = 32;
    const int h = 32;
    const auto rgb = test_rgb(w, h);
    std::string out;
    kitty::append_transmit_direct(out, rgb.data(), rgb.size(), w, h, 10, 10, /*deflated=*/false);
    const auto apcs = split_apcs(out);
    ASSERT_EQ(apcs.size(), 1u);
    ASSERT_EQ(apcs[0].payload.size(), 4096u);
    const auto decoded = decode_direct(apcs, w, h);
    ASSERT_TRUE(decoded == rgb);
}

TEST(kitty, just_past_boundary_is_two_chunks)
{
    // 33x32 = 3168 bytes -> 4224 base64 chars -> a full chunk and a short tail.
    const int w = 33;
    const int h = 32;
    const auto rgb = test_rgb(w, h);
    std::string out;
    kitty::append_transmit_direct(out, rgb.data(), rgb.size(), w, h, 10, 10, /*deflated=*/false);
    const auto apcs = split_apcs(out);
    ASSERT_EQ(apcs.size(), 2u);
    ASSERT_EQ(apcs[0].payload.size(), 4096u);
    const auto decoded = decode_direct(apcs, w, h);
    ASSERT_TRUE(decoded == rgb);
}

TEST(kitty, many_chunk_roundtrip)
{
    const int w = 100;
    const int h = 60; // 18000 bytes -> 24000 chars -> 6 chunks
    const auto rgb = test_rgb(w, h);
    std::string out;
    kitty::append_transmit_direct(out, rgb.data(), rgb.size(), w, h, 120, 40, /*deflated=*/false);
    const auto apcs = split_apcs(out);
    ASSERT_EQ(apcs.size(), 6u);
    const auto decoded = decode_direct(apcs, w, h);
    ASSERT_TRUE(decoded == rgb);
}

TEST(kitty, shm_transmit_names_the_object)
{
    std::string out;
    kitty::append_transmit_shm(out, "/rasterminal-123-0", 640, 480, 80, 23);
    const auto apcs = split_apcs(out);
    ASSERT_EQ(apcs.size(), 1u);
    ASSERT_TRUE(has_key(apcs[0], "a=T"));
    ASSERT_TRUE(has_key(apcs[0], "t=s"));
    ASSERT_TRUE(has_key(apcs[0], "f=24"));
    ASSERT_TRUE(has_key(apcs[0], "q=2"));
    ASSERT_TRUE(has_key(apcs[0], "C=1"));
    ASSERT_TRUE(has_key(apcs[0], "p=" + std::to_string(kitty::PLACEMENT_ID)));
    ASSERT_TRUE(has_key(apcs[0], "s=640"));
    ASSERT_TRUE(has_key(apcs[0], "v=480"));
    ASSERT_TRUE(has_key(apcs[0], "c=80"));
    ASSERT_TRUE(has_key(apcs[0], "r=23"));
    ASSERT_FALSE(has_key(apcs[0], "m=1")); // never chunked: the payload is only a name
    const auto decoded = b64_decode(apcs[0].payload);
    ASSERT_TRUE(std::string(decoded.begin(), decoded.end()) == "/rasterminal-123-0");
}

TEST(kitty, delete_frees_terminal_side_data)
{
    std::string out;
    kitty::append_delete(out);
    ASSERT_TRUE(out == "\033_Gq=2,a=d,d=I,i=1\033\\");
}

TEST(kitty, query_shape)
{
    const std::string q = kitty::QUERY;
    const auto apcs = split_apcs(q);
    ASSERT_EQ(apcs.size(), 1u);
    ASSERT_TRUE(has_key(apcs[0], "a=q"));
    ASSERT_TRUE(has_key(apcs[0], "i=" + std::to_string(kitty::QUERY_ID)));
    // 1x1 RGB probe: the payload must decode to exactly 3 bytes.
    ASSERT_EQ(b64_decode(apcs[0].payload).size(), 3u);
}
