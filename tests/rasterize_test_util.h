#pragma once

#include "tests/test.h"
#include "src/terminal/framebuffer.h"

#include <cstdio>
#include <limits>
#include <string>

// Redirects stdout to /dev/null for the duration of its lifetime.
// Use for tests that call Framebuffer::resize() or Framebuffer::present(),
// which write to stdout regardless of headless mode.  Tests that only
// construct, draw, and read back a Framebuffer should use headless=true instead.
struct FdRedirect
{
    int saved_out;
    FdRedirect() : saved_out(test_dup(TEST_STDOUT))
    {
        std::fflush(stdout);

        int dn = test_devnull();
        if (dn < 0)
        {
            return; // /dev/null unavailable: leave stdout alone rather than corrupt it
        }
        test_dup2(dn, TEST_STDOUT);
        test_close(dn);
    }
    ~FdRedirect()
    {
        std::fflush(stdout);
        if (saved_out >= 0)
        {
            test_dup2(saved_out, TEST_STDOUT);
            test_close(saved_out);
        }
    }
    FdRedirect(const FdRedirect &) = delete;
    FdRedirect &operator=(const FdRedirect &) = delete;
    FdRedirect(FdRedirect &&) = delete;
    FdRedirect &operator=(FdRedirect &&) = delete;
};

// Returns true iff pixel (x,y) was drawn (stored depth < +inf). Read-only.
static inline bool was_drawn(Framebuffer &fb, int x, int y)
{
    return fb.depth_at(x, y) < std::numeric_limits<float>::infinity();
}

// Assert stored depth at (x,y) is within eps of D (i.e. D-eps < depth <= D+eps).
// Read-only; call after all drawing is done.
static inline void assert_depth_near(Framebuffer &fb, int x, int y, float D, float eps)
{
    const float d = fb.depth_at(x, y);
    if (d > D + eps)
    {
        ASSERT_FAIL("depth > " + std::to_string(D + eps) + " at (" + std::to_string(x) + "," + std::to_string(y) + ")");
    }
    if (d <= D - eps)
    {
        ASSERT_FAIL(
            "depth <= " + std::to_string(D - eps) + " at (" + std::to_string(x) + "," + std::to_string(y) + ")"
        );
    }
}

// Assert pixel (x,y) is channel-wise within ±tol of expected.
static inline void assert_pixel_near(Framebuffer &fb, int x, int y, Color expected, int tol)
{
    Color got = fb.get_pixel(x, y);
    int dr = std::abs(static_cast<int>(got.r) - static_cast<int>(expected.r));
    int dg = std::abs(static_cast<int>(got.g) - static_cast<int>(expected.g));
    int db = std::abs(static_cast<int>(got.b) - static_cast<int>(expected.b));
    if (dr > tol || dg > tol || db > tol)
    {
        ASSERT_FAIL(
            "pixel(" + std::to_string(x) + "," + std::to_string(y) +
            ")"
            " expected~(" +
            std::to_string(static_cast<int>(expected.r)) + "," + std::to_string(static_cast<int>(expected.g)) + "," +
            std::to_string(static_cast<int>(expected.b)) +
            ")"
            " got(" +
            std::to_string(static_cast<int>(got.r)) + "," + std::to_string(static_cast<int>(got.g)) + "," +
            std::to_string(static_cast<int>(got.b)) +
            ")"
            " tol=" +
            std::to_string(tol)
        );
    }
}
