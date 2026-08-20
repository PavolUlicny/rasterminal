#include "tests/test.h"
#include "tests/webp_fixtures.h"
#include "src/render/texture.h"
#include "src/loaders/webp_decode.h"

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

// decode_webp_rgba: lossy happy path

TEST(webp_decode, lossy_solid_decodes_to_expected_color)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    ASSERT_TRUE(decode_webp_rgba(k_webp_lossy_solid, k_webp_lossy_solid_len, rgba, w, h));
    ASSERT_EQ(w, 4);
    ASSERT_EQ(h, 4);
    ASSERT_EQ(rgba.size(), size_t{ 4 } * 4 * 4);

    // Source (200,90,40,255); VP8 lossy reference-decodes to ~(201,90,41,255). Loose tolerance
    // for the lossy codec; every texel is the same colour.
    for (int y = 0; y < 4; y++)
    {
        for (int x = 0; x < 4; x++)
        {
            ASSERT_NEAR(static_cast<float>(px(rgba, w, x, y, 0)), 201.0f, 6.0f);
            ASSERT_NEAR(static_cast<float>(px(rgba, w, x, y, 1)), 90.0f, 6.0f);
            ASSERT_NEAR(static_cast<float>(px(rgba, w, x, y, 2)), 41.0f, 6.0f);
            ASSERT_EQ(px(rgba, w, x, y, 3), 255);
        }
    }
}

// decode_webp_rgba: lossless layout (catches wrong pitch/orientation)

TEST(webp_decode, lossless_quad_decodes_with_correct_layout)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    ASSERT_TRUE(decode_webp_rgba(k_webp_lossless_quad, k_webp_lossless_quad_len, rgba, w, h));
    ASSERT_EQ(w, 4);
    ASSERT_EQ(h, 4);
    ASSERT_EQ(rgba.size(), size_t{ 4 } * 4 * 4);

    // Lossless: exact corner colours. Distinct quadrants catch a wrong row pitch or a
    // flipped/transposed decode, which a solid fixture could not.
    // TL=red, TR=green, BL=blue, BR=white.
    ASSERT_EQ(px(rgba, w, 0, 0, 0), 255); // TL red
    ASSERT_EQ(px(rgba, w, 0, 0, 1), 0);
    ASSERT_EQ(px(rgba, w, 0, 0, 2), 0);

    ASSERT_EQ(px(rgba, w, 3, 0, 0), 0); // TR green
    ASSERT_EQ(px(rgba, w, 3, 0, 1), 255);
    ASSERT_EQ(px(rgba, w, 3, 0, 2), 0);

    ASSERT_EQ(px(rgba, w, 0, 3, 0), 0); // BL blue
    ASSERT_EQ(px(rgba, w, 0, 3, 1), 0);
    ASSERT_EQ(px(rgba, w, 0, 3, 2), 255);

    ASSERT_EQ(px(rgba, w, 3, 3, 0), 255); // BR white
    ASSERT_EQ(px(rgba, w, 3, 3, 1), 255);
    ASSERT_EQ(px(rgba, w, 3, 3, 2), 255);

    // All opaque.
    for (int y = 0; y < 4; y++)
    {
        for (int x = 0; x < 4; x++)
        {
            ASSERT_EQ(px(rgba, w, x, y, 3), 255);
        }
    }
}

TEST(webp_decode, alpha_decodes_transparency)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    ASSERT_TRUE(decode_webp_rgba(k_webp_alpha, k_webp_alpha_len, rgba, w, h));
    ASSERT_EQ(w, 4);
    ASSERT_EQ(h, 4);

    // Fully transparent everywhere except the (1,1) texel = (255,0,0,128).
    ASSERT_EQ(px(rgba, w, 0, 0, 3), 0);
    ASSERT_EQ(px(rgba, w, 1, 1, 0), 255);
    ASSERT_EQ(px(rgba, w, 1, 1, 1), 0);
    ASSERT_EQ(px(rgba, w, 1, 1, 2), 0);
    ASSERT_EQ(px(rgba, w, 1, 1, 3), 128);
}

// decode_webp_rgba: minimum dimensions

TEST(webp_decode, one_pixel_decodes)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    ASSERT_TRUE(decode_webp_rgba(k_webp_one_pixel, k_webp_one_pixel_len, rgba, w, h));
    ASSERT_EQ(w, 1);
    ASSERT_EQ(h, 1);
    ASSERT_EQ(rgba.size(), size_t{ 4 });
    ASSERT_EQ(px(rgba, w, 0, 0, 0), 123);
    ASSERT_EQ(px(rgba, w, 0, 0, 1), 45);
    ASSERT_EQ(px(rgba, w, 0, 0, 2), 67);
    ASSERT_EQ(px(rgba, w, 0, 0, 3), 255);
}

// decode_webp_rgba: non-square dimensions (catches transpose / wrong pitch)

TEST(webp_decode, non_square_decodes_with_correct_dimensions_and_layout)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    ASSERT_TRUE(decode_webp_rgba(k_webp_rect, k_webp_rect_len, rgba, w, h));
    // A transposed decode would report 2x6; the asymmetric size pins width vs height.
    ASSERT_EQ(w, 6);
    ASSERT_EQ(h, 2);
    ASSERT_EQ(rgba.size(), size_t{ 6 } * 2 * 4);

    // row0 px x = (x*40,0,0); row1 px x = (0,x*40,0). Lossless, so exact.
    ASSERT_EQ(px(rgba, w, 0, 0, 0), 0);   // (0,0) black
    ASSERT_EQ(px(rgba, w, 5, 0, 0), 200); // (5,0) row0 -> red channel
    ASSERT_EQ(px(rgba, w, 5, 0, 1), 0);
    ASSERT_EQ(px(rgba, w, 5, 1, 0), 0); // (5,1) row1 -> green channel
    ASSERT_EQ(px(rgba, w, 5, 1, 1), 200);
}

// decode_webp_rgba: lossy colour + alpha plane (ALPH chunk)

TEST(webp_decode, lossy_alpha_decodes_color_and_alpha)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    ASSERT_TRUE(decode_webp_rgba(k_webp_lossy_alpha, k_webp_lossy_alpha_len, rgba, w, h));
    ASSERT_EQ(w, 4);
    ASSERT_EQ(h, 4);

    // RGB is lossy (~201,100,50); alpha = (x+y)*30 is stored losslessly, so it is exact.
    ASSERT_NEAR(static_cast<float>(px(rgba, w, 0, 0, 0)), 201.0f, 6.0f);
    ASSERT_NEAR(static_cast<float>(px(rgba, w, 0, 0, 1)), 100.0f, 6.0f);
    ASSERT_NEAR(static_cast<float>(px(rgba, w, 0, 0, 2)), 50.0f, 6.0f);
    ASSERT_EQ(px(rgba, w, 0, 0, 3), 0);   // (0+0)*30
    ASSERT_EQ(px(rgba, w, 3, 0, 3), 90);  // (3+0)*30
    ASSERT_EQ(px(rgba, w, 3, 3, 3), 180); // (3+3)*30
}

// decode_webp_rgba: fail-loud paths

TEST(webp_decode, null_data_returns_false)
{
    std::vector<uint8_t> rgba;
    int w = -1;
    int h = -1;
    ASSERT_FALSE(decode_webp_rgba(nullptr, 16, rgba, w, h));
}

TEST(webp_decode, zero_size_returns_false)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    const uint8_t dummy = 0;
    ASSERT_FALSE(decode_webp_rgba(&dummy, 0, rgba, w, h));
}

TEST(webp_decode, garbage_returns_false)
{
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    const uint8_t garbage[] = { 0x00, 0x01, 0x02, 0x03, 0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44 };
    ASSERT_FALSE(decode_webp_rgba(garbage, sizeof(garbage), rgba, w, h));
}

TEST(webp_decode, valid_magic_truncated_body_returns_false)
{
    // Keep the RIFF/WEBP header + a little of the VP8L stream, drop the rest: WebPGetFeatures or
    // WebPDecodeRGBAInto must reject it rather than read past the buffer.
    std::vector<uint8_t> truncated(k_webp_lossless_quad, k_webp_lossless_quad + 24);
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    ASSERT_FALSE(decode_webp_rgba(truncated.data(), truncated.size(), rgba, w, h));
}

TEST(webp_decode, failure_leaves_outputs_untouched)
{
    std::vector<uint8_t> rgba = { 1, 2, 3 };
    int w = 7;
    int h = 9;
    const uint8_t garbage[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    ASSERT_FALSE(decode_webp_rgba(garbage, sizeof(garbage), rgba, w, h));
    // out params must be untouched on failure.
    ASSERT_EQ(rgba.size(), size_t{ 3 });
    ASSERT_EQ(w, 7);
    ASSERT_EQ(h, 9);
}

TEST(webp_decode, animated_webp_is_rejected)
{
    // EXT_texture_webp requires a still image. WebPGetFeatures reports the canvas + has_animation
    // for an animated file, so decode_webp_rgba must reject it (rather than allocate a buffer for
    // a decode the simple API cannot perform).
    std::vector<uint8_t> rgba;
    int w = -1;
    int h = -1;
    ASSERT_FALSE(decode_webp_rgba(k_webp_animated, k_webp_animated_len, rgba, w, h));
    ASSERT_TRUE(rgba.empty());
    ASSERT_EQ(w, -1);
    ASSERT_EQ(h, -1);
}

TEST(webp_decode, valid_header_corrupt_body_returns_false)
{
    // WebPGetFeatures succeeds on this fixture (RIFF + VP8L header intact) but the compressed
    // body is damaged, so the failure surfaces from WebPDecodeRGBAInto, not the header parse.
    std::vector<uint8_t> rgba;
    int w = 0;
    int h = 0;
    ASSERT_FALSE(decode_webp_rgba(k_webp_corrupt_body, k_webp_corrupt_body_len, rgba, w, h));
}

// Texture::load_webp_from_memory

TEST(webp_texture, load_success_populates_texture)
{
    Texture t;
    ASSERT_TRUE(t.load_webp_from_memory(k_webp_lossy_solid, k_webp_lossy_solid_len));
    ASSERT_TRUE(t.valid());
    ASSERT_EQ(t.width, 4);
    ASSERT_EQ(t.height, 4);
    ASSERT_EQ(t.pixels.size(), size_t{ 4 } * 4 * 4);
    // Sampling the loaded solid texture returns the decoded colour.
    vec3 c = t.sample_rgb(0.5f, 0.5f);
    ASSERT_NEAR(c.x, 201.0f / 255.0f, 0.04f);
    ASSERT_NEAR(c.y, 90.0f / 255.0f, 0.04f);
    ASSERT_NEAR(c.z, 41.0f / 255.0f, 0.04f);
}

TEST(webp_texture, load_failure_preserves_previous_data)
{
    Texture t;
    ASSERT_TRUE(t.load_webp_from_memory(k_webp_lossy_solid, k_webp_lossy_solid_len));
    const int old_w = t.width;
    const int old_h = t.height;
    const std::vector<uint8_t> old_pixels = t.pixels;

    const uint8_t garbage[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    ASSERT_FALSE(t.load_webp_from_memory(garbage, sizeof(garbage)));
    ASSERT_EQ(t.width, old_w);
    ASSERT_EQ(t.height, old_h);
    if (t.pixels != old_pixels)
    {
        ASSERT_FAIL("load_webp_from_memory() should leave pixels unchanged on failure");
    }
}

// Thread safety: parallel decode (decode_textures runs in parallel)

TEST(webp_decode, concurrent_decode_is_thread_safe)
{
    // decode_textures() decodes in parallel. libwebp DOES have global init to guard: it lazily
    // populates its SIMD dispatch tables on the first decode. WEBP_USE_THREAD (set for the
    // libwebp TUs in both build systems) makes that init mutex-guarded; without it, the first
    // concurrent decodes race on those tables. That race is what enforces the invariant, not
    // this test: the init writes are idempotent (same impl pointers), so output matches with or
    // without the mutex and only the CI tsan job actually observes the difference. This test is
    // therefore a reentrancy smoke check (every worker succeeds and yields identical bytes); do
    // not read a green run here as proof the guard is present; keep WEBP_USE_THREAD.
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
                    decode_webp_rgba(
                        k_webp_lossless_quad, k_webp_lossless_quad_len, results[static_cast<size_t>(i)], w, h
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
