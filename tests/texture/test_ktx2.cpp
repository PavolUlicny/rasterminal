#include "tests/test.h"
#include "tests/ktx2_fixtures.h"
#include "src/loaders/ktx2_decode.h"
#include "src/render/texture.h"

#include <cstdint>
#include <thread>
#include <vector>

namespace
{
    // RGBA channel of pixel (x, y) in a w-wide row-major top-left-first buffer.
    int px(const std::vector<uint8_t> &rgba, int w, int x, int y, int c)
    {
        return rgba
            [(((static_cast<size_t>(y) * static_cast<size_t>(w)) + static_cast<size_t>(x)) * 4) +
             static_cast<size_t>(c)];
    }
} // namespace

// decode_ktx2_rgba: ETC1S happy path

TEST(ktx2_decode, etc1s_solid_decodes_to_expected_color)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    ASSERT_TRUE(decode_ktx2_rgba(k_ktx2_etc1s_solid, k_ktx2_etc1s_solid_len, rgba, w, h));
    ASSERT_EQ(w, 4);
    ASSERT_EQ(h, 4);
    ASSERT_EQ(rgba.size(), size_t{ 4 } * 4 * 4);

    // Solid source (200,90,40,255); ETC1S reference-decodes to ~(198,91,41,255). Loose
    // tolerance for the lossy codec; every texel is the same colour.
    for (int y = 0; y < 4; y++)
    {
        for (int x = 0; x < 4; x++)
        {
            ASSERT_NEAR(static_cast<float>(px(rgba, w, x, y, 0)), 198.0f, 8.0f);
            ASSERT_NEAR(static_cast<float>(px(rgba, w, x, y, 1)), 91.0f, 8.0f);
            ASSERT_NEAR(static_cast<float>(px(rgba, w, x, y, 2)), 41.0f, 8.0f);
            ASSERT_EQ(px(rgba, w, x, y, 3), 255);
        }
    }
}

// decode_ktx2_rgba: UASTC + Zstd happy path (also proves the zstd link path)

TEST(ktx2_decode, uastc_zstd_quad_decodes_with_correct_layout)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    // A successful decode of this fixture proves BASISD_SUPPORT_KTX2_ZSTD is wired and
    // the zstd amalgam linked: the payload is Zstd-supercompressed.
    ASSERT_TRUE(decode_ktx2_rgba(k_ktx2_uastc_zstd_quad, k_ktx2_uastc_zstd_quad_len, rgba, w, h));
    ASSERT_EQ(w, 4);
    ASSERT_EQ(h, 4);
    ASSERT_EQ(rgba.size(), size_t{ 4 } * 4 * 4);

    // Quadrant layout (UASTC is near-lossless here). Distinct corners catch a wrong row
    // pitch or a flipped/transposed decode, which a solid fixture could not.
    // TL=red, TR=green, BL=blue, BR=white.
    ASSERT_NEAR(static_cast<float>(px(rgba, w, 0, 0, 0)), 223.0f, 12.0f); // TL red
    ASSERT_TRUE(px(rgba, w, 0, 0, 0) > px(rgba, w, 0, 0, 1));             // R > G
    ASSERT_TRUE(px(rgba, w, 0, 0, 0) > px(rgba, w, 0, 0, 2));             // R > B

    ASSERT_NEAR(static_cast<float>(px(rgba, w, 3, 0, 1)), 197.0f, 12.0f); // TR green
    ASSERT_TRUE(px(rgba, w, 3, 0, 1) > px(rgba, w, 3, 0, 0));             // G > R

    ASSERT_NEAR(static_cast<float>(px(rgba, w, 0, 3, 2)), 210.0f, 12.0f); // BL blue
    ASSERT_TRUE(px(rgba, w, 0, 3, 2) > px(rgba, w, 0, 3, 0));             // B > R

    // BR white: all channels high.
    ASSERT_TRUE(px(rgba, w, 3, 3, 0) > 220);
    ASSERT_TRUE(px(rgba, w, 3, 3, 1) > 220);
    ASSERT_TRUE(px(rgba, w, 3, 3, 2) > 220);
}

// decode_ktx2_rgba: fail-loud paths

TEST(ktx2_decode, null_data_returns_false)
{
    std::vector<uint8_t> rgba;
    int w = -1;
    int h = -1;
    ASSERT_FALSE(decode_ktx2_rgba(nullptr, 16, rgba, w, h));
}

TEST(ktx2_decode, zero_size_returns_false)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    const uint8_t dummy = 0;
    ASSERT_FALSE(decode_ktx2_rgba(&dummy, 0, rgba, w, h));
}

TEST(ktx2_decode, garbage_returns_false)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    const uint8_t garbage[] = { 0x00, 0x01, 0x02, 0x03, 0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44 };
    ASSERT_FALSE(decode_ktx2_rgba(garbage, sizeof(garbage), rgba, w, h));
}

TEST(ktx2_decode, valid_magic_truncated_body_returns_false)
{
    // Keep the 12-byte KTX2 identifier + a little of the header, drop the rest: init()
    // or start_transcoding() must reject it rather than read past the buffer.
    std::vector<uint8_t> truncated(k_ktx2_etc1s_solid, k_ktx2_etc1s_solid + 40);
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    ASSERT_FALSE(decode_ktx2_rgba(truncated.data(), truncated.size(), rgba, w, h));
}

TEST(ktx2_decode, oversized_declared_dimension_fails_loud)
{
    // Set pixelWidth at byte 20 to 65536, beyond the transcoder's 16384 cap.
    // Decoding must reject it before allocation.
    std::vector<uint8_t> bad(k_ktx2_etc1s_solid, k_ktx2_etc1s_solid + k_ktx2_etc1s_solid_len);
    bad[20] = 0x00;
    bad[21] = 0x00;
    bad[22] = 0x01; // 0x00010000 = 65536
    bad[23] = 0x00;
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    ASSERT_FALSE(decode_ktx2_rgba(bad.data(), bad.size(), rgba, w, h));
}

TEST(ktx2_decode, failure_leaves_outputs_untouched)
{
    std::vector<uint8_t> rgba = { 1, 2, 3 };
    int w = 7;
    int h = 9;
    const uint8_t garbage[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    ASSERT_FALSE(decode_ktx2_rgba(garbage, sizeof(garbage), rgba, w, h));
    // out params must be untouched on failure.
    ASSERT_EQ(rgba.size(), size_t{ 3 });
    ASSERT_EQ(w, 7);
    ASSERT_EQ(h, 9);
}

// Texture::load_ktx2_from_memory

TEST(ktx2_texture, load_success_populates_texture)
{
    Texture t;
    ASSERT_TRUE(t.load_ktx2_from_memory(k_ktx2_etc1s_solid, k_ktx2_etc1s_solid_len));
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(t.width, 4);
    ASSERT_EQ(t.height, 4);
    ASSERT_EQ(t.pixels.size(), size_t{ 4 } * 4 * 4);
    // Sampling the loaded texture returns the solid colour.
    vec3 c = t.sample_rgb(0.5f, 0.5f);
    ASSERT_NEAR(c.x, 198.0f / 255.0f, 0.04f);
    ASSERT_NEAR(c.y, 91.0f / 255.0f, 0.04f);
    ASSERT_NEAR(c.z, 41.0f / 255.0f, 0.04f);
}

TEST(ktx2_texture, load_failure_preserves_previous_data)
{
    Texture t;
    ASSERT_TRUE(t.load_ktx2_from_memory(k_ktx2_etc1s_solid, k_ktx2_etc1s_solid_len));
    const int old_w = t.width;
    const int old_h = t.height;
    const std::vector<uint8_t> old_pixels = t.pixels;

    const uint8_t garbage[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    ASSERT_FALSE(t.load_ktx2_from_memory(garbage, sizeof(garbage)));
    ASSERT_EQ(t.width, old_w);
    ASSERT_EQ(t.height, old_h);
    if (t.pixels != old_pixels)
    {
        ASSERT_FAIL("load_ktx2_from_memory() should leave pixels unchanged on failure");
    }
}

// Thread safety: the one-time basisu_transcoder_init() under contention

TEST(ktx2_decode, concurrent_decode_is_thread_safe)
{
    // decode_textures() runs decodes in parallel; the std::call_once init must hold up
    // under contention and every worker must produce the same bytes.
    constexpr int kThreads = 8;
    std::vector<std::vector<uint8_t>> results(kThreads);
    std::vector<int> oks(kThreads, 0);
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; i++)
    {
        workers.emplace_back(
            [&, i]()
            {
                int w = 0;
                int h = 0;
                oks[static_cast<size_t>(i)] =
                    decode_ktx2_rgba(
                        k_ktx2_uastc_zstd_quad, k_ktx2_uastc_zstd_quad_len, results[static_cast<size_t>(i)], w, h
                    )
                        ? 1
                        : 0;
            }
        );
    }
    for (auto &t : workers)
    {
        t.join();
    }
    for (int i = 0; i < kThreads; i++)
    {
        ASSERT_EQ(oks[static_cast<size_t>(i)], 1);
        if (results[static_cast<size_t>(i)] != results[0])
        {
            ASSERT_FAIL("concurrent decodes produced differing output");
        }
    }
}
