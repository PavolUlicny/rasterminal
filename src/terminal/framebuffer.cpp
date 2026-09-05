#include "src/terminal/framebuffer.h"

#include "src/platform/platform.h" // shm frame helpers (POSIX)
#include "src/terminal/color.h"    // Color, ColorMode
#include "src/terminal/graphics.h" // GraphicsBackend
#include "src/terminal/kitty.h"    // escape composition for the kitty backend
#include "src/terminal/sixel.h"    // escape composition for the sixel backend

#include "miniz.h" // zlib deflate for the kitty direct transport; config macros come from the build

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace
{

    // UTF-8 encoding of ▀ (U+2580 UPPER HALF BLOCK)
    // Top pixel is foreground; bottom pixel is background.
    const char UPPER_HALF[] = "\xe2\x96\x80";

    struct ByteLut
    {
        char s[256][3];
        uint8_t len[256];
    };
    constexpr ByteLut make_byte_lut() noexcept
    {
        ByteLut t{};
        for (int i = 0; i < 256; ++i)
        {
            if (i < 10)
            {
                t.s[i][0] = static_cast<char>('0' + i);
                t.len[i] = 1;
            }
            else if (i < 100)
            {
                t.s[i][0] = static_cast<char>('0' + (i / 10));
                t.s[i][1] = static_cast<char>('0' + (i % 10));
                t.len[i] = 2;
            }
            else
            {
                t.s[i][0] = static_cast<char>('0' + (i / 100));
                t.s[i][1] = static_cast<char>('0' + ((i / 10) % 10));
                t.s[i][2] = static_cast<char>('0' + (i % 10));
                t.len[i] = 3;
            }
        }
        return t;
    }
    constexpr ByteLut byte_lut = make_byte_lut();

    // Always writes 3 bytes (single store); caller advances by the returned length.
    // 1-2 slop bytes past len are within tmp[48] and overwritten by the next write.
    int write_byte(char *buf, uint8_t v)
    {
        buf[0] = byte_lut.s[v][0];
        buf[1] = byte_lut.s[v][1];
        buf[2] = byte_lut.s[v][2];
        return byte_lut.len[v];
    }

    // Fast integer-to-string: writes decimal digits of v into buf, returns length.
    int write_int(char *buf, int v)
    {
        if (v == 0)
        {
            buf[0] = '0';
            return 1;
        }
        char tmp[12]; // 10 digits max for INT_MAX (2147483647) + headroom
        int len = 0;
        while (v > 0)
        {
            tmp[len++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
        for (int i = 0; i < len; i++)
        {
            buf[i] = tmp[len - 1 - i];
        }
        return len;
    }

} // namespace

namespace
{
    // Rotate shm names to avoid overwriting normally in-flight frames. This is slack,
    // not backpressure; a stalled terminal may drop one frame before the next repaint.
    constexpr unsigned SHM_RING = 4;

    // Names assume one live kitty Framebuffer per process.
    void shm_ring_name(char *buf, size_t cap, unsigned slot)
    {
        std::snprintf(buf, cap, "/rasterminal-%lu-%u", platform::process_id(), slot);
    }

    // Grow-only capacity for the staging arrays; the fresh allocation is left
    // uninitialized on purpose (see the member comment).
    void ensure_capacity(std::unique_ptr<unsigned char[]> &buf, size_t &cap, size_t need)
    {
        if (need > cap)
        {
            // make_unique value-initializes and reset() takes ownership on this same line
            // NOLINTNEXTLINE(modernize-make-unique,cppcoreguidelines-owning-memory)
            buf.reset(new unsigned char[need]);
            cap = need;
        }
    }

    // Synchronized-output open bracket (mode 2026); end_frame keys its empty-frame
    // check on this literal's length.
    constexpr char SYNC_BEGIN[] = "\033[?2026h";

    // Split a range across borrowed workers, then redo any unreported range serially.
    template <typename F>
    void split_ranges(const Framebuffer::ParallelRunner &par, std::vector<uint8_t> &covered, size_t total, const F &fn)
    {
        // Start of worker w's range; w == n_workers gives total, so the end of one range is the
        // start of the next and the split covers [0, total) exactly.
        const auto start_of = [total](size_t w, size_t n_workers)
        { return std::min(total, ((total + n_workers - 1) / n_workers) * w); };

        covered.assign(static_cast<size_t>(par.n_workers), 0u);
        std::vector<uint8_t> *cov = &covered;
        par.run(
            [&fn, &start_of, cov](int worker_id, int n_workers)
            {
                // A mismatched pool size invalidates the range map; let serial redo cover it.
                const auto w = static_cast<size_t>(worker_id);
                if (static_cast<size_t>(n_workers) != cov->size() || w >= cov->size())
                {
                    return;
                }
                const auto n = static_cast<size_t>(n_workers);
                fn(start_of(w, n), start_of(w + 1, n));
                (*cov)[w] = 1u;
            }
        );
        for (size_t w = 0; w < covered.size(); w++)
        {
            if (covered[w] == 0u)
            {
                fn(start_of(w, covered.size()), start_of(w + 1, covered.size()));
            }
        }
    }

    // zlib's adler32_combine, absent from miniz, avoids a second full-frame pass.
    constexpr uint32_t ADLER_BASE = 65521u;
    // uint64_t throughout: rem * sum1 reaches 65520 * 65535, which overflows 32 bits.
    // No cast on the modulo, which would be useless at LP64 (size_t is unsigned long
    // there) and narrowing at LLP64; the assignment converts on both.
    uint32_t adler_combine(uint32_t a1, uint32_t a2, size_t len2) noexcept
    {
        const uint64_t rem = len2 % ADLER_BASE;
        uint64_t sum1 = a1 & 0xFFFFu;
        uint64_t sum2 = (rem * sum1) % ADLER_BASE;
        sum1 += (a2 & 0xFFFFu) + ADLER_BASE - 1u;
        sum2 += ((a1 >> 16u) & 0xFFFFu) + ((a2 >> 16u) & 0xFFFFu) + ADLER_BASE - rem;
        if (sum1 >= ADLER_BASE)
        {
            sum1 -= ADLER_BASE;
        }
        if (sum1 >= ADLER_BASE)
        {
            sum1 -= ADLER_BASE;
        }
        if (sum2 >= (uint64_t{ ADLER_BASE } << 1u))
        {
            sum2 -= (uint64_t{ ADLER_BASE } << 1u);
        }
        if (sum2 >= ADLER_BASE)
        {
            sum2 -= ADLER_BASE;
        }
        return static_cast<uint32_t>(sum1 | (sum2 << 16u));
    }
} // namespace

Framebuffer::Framebuffer(
    int pixel_width,
    int pixel_height,
    bool headless,
    ColorMode mode,
    const GraphicsConfig &gfx,
    bool adopt_alt_screen,
    WriteFrameFn write_frame,
    bool defer_terminal_setup
)
    : m_width(pixel_width), m_height(pixel_height),
      m_pixel(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height)), m_headless(headless), m_mode(mode),
      m_gfx(gfx), m_write_frame(write_frame != nullptr ? write_frame : platform::detail::write_terminal_bytes)
{
    // Sized here rather than in the init list so the predicate has one
    // spelling (is_pixel_backend needs m_gfx, which member order initializes
    // after m_prev_color).
    if (!is_pixel_backend())
    {
        m_prev_color.assign(m_pixel.size(), 0u);
    }
    fill_cleared(0u);
    if (!m_headless)
    {
        // Precondition: stdout is a terminal (main.cpp enforces the tty check
        // before constructing us); this path and present() write ANSI to it.
        // Preallocate a mode-dependent per-cell upper bound (see buf_reserve_bytes).
        m_buf.reserve(buf_reserve_bytes());
        if (defer_terminal_setup)
        {
            m_adopted_alt_screen = adopt_alt_screen;
            m_terminal_release_pending = adopt_alt_screen;
            return;
        }
        if (!adopt_alt_screen)
        {
            std::fputs("\033[?1049h", stdout); // enter alternate screen buffer
        }
        std::fputs("\033[?25l", stdout); // hide cursor
        std::fflush(stdout);
        m_terminal_active = true;
    }
}

void Framebuffer::suspend_terminal(WriteCleanupFn write_cleanup, bool *mouse_cleanup_pending) noexcept
{
    if (write_cleanup == nullptr)
    {
        write_cleanup = platform::write_terminal_cleanup;
    }
    if (!m_terminal_active && !m_terminal_release_pending)
    {
        return;
    }
    m_adopted_alt_screen = false;
    m_mouse_cleanup_pending = m_mouse_cleanup_pending || (mouse_cleanup_pending != nullptr && *mouse_cleanup_pending);

    constexpr char reset[] = "\033\\\033[?2026l\033[?7h";
    constexpr char mouse[] = "\033[?1002l\033[?1006l";
    constexpr char release[] = "\033[?25h\033[0m\033[?1049l";
    char cleanup
        [sizeof reset + sizeof mouse + sizeof release + sizeof kitty::FINISH_TRANSMISSION + sizeof kitty::DELETE_IMAGE];
    char *out = cleanup;
    const auto append = [&out](const char *text)
    {
        const size_t size = std::strlen(text);
        std::memcpy(out, text, size);
        out += size;
    };
    // One bounded write preserves cleanup order across partial writes. A failed
    // release retains every reset for the next attempt, including image deletion.
    append(reset);
    if (m_gfx.backend == GraphicsBackend::Kitty)
    {
        if (m_output_recovery_pending)
        {
            append(kitty::FINISH_TRANSMISSION);
        }
        append(kitty::DELETE_IMAGE);
    }
    if (m_mouse_cleanup_pending)
    {
        append(mouse);
    }
    append(release);
    *out = '\0';
    m_terminal_release_pending = !write_cleanup(cleanup);
    m_output_recovery_pending = m_terminal_release_pending;
    if (!m_terminal_release_pending)
    {
        m_image_shown = false;
        m_mouse_cleanup_pending = false;
        if (mouse_cleanup_pending != nullptr)
        {
            *mouse_cleanup_pending = false;
        }
    }
    m_terminal_active = false;
}

bool Framebuffer::resume_terminal(bool *canceled) noexcept
{
    if (canceled != nullptr)
    {
        *canceled = false;
    }
    if (!m_headless && !m_terminal_active)
    {
        const char *setup = m_output_recovery_pending ? "\033\\\033[?1049h\033[?25l"
                            : m_adopted_alt_screen    ? "\033[?25l"
                                                      : "\033[?1049h\033[?25l";
        // Even a partial reacquisition owns cleanup. Do not render until the
        // complete setup has reached the terminal.
        m_terminal_release_pending = true;
        if (!platform::write_terminal(setup, std::strlen(setup), true, m_write_frame, canceled))
        {
            m_output_recovery_pending = true;
            return false;
        }
        m_terminal_active = true;
        m_adopted_alt_screen = false;
        m_force_redraw = true;
        m_image_dirty = true;
        m_pending_clear = true;
    }
    return true;
}

Framebuffer::~Framebuffer() noexcept
{
    suspend_terminal();
    // Headless mode suppresses escapes, not cleanup of shared-memory frames.
    if (m_gfx.backend == GraphicsBackend::Kitty)
    {
        // Reclaim unread frames and any stale entries left by this pid.
        for (unsigned slot = 0; slot < SHM_RING; slot++)
        {
            char name[64];
            shm_ring_name(name, sizeof name, slot);
            platform::shm_frame_remove(name);
        }
    }
}

void Framebuffer::resize(int pixel_width, int pixel_height)
{
    m_width = pixel_width;
    m_height = pixel_height;
    const size_t npx = static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height);
    m_pixel = std::vector<std::atomic<uint64_t>>(npx);
    m_prev_color = std::vector<uint32_t>(is_pixel_backend() ? 0u : npx, 0u);
    fill_cleared(0u);
    // Release frame-sized buffers here so shrinking the window also returns
    // their memory. reserve() alone would retain the old capacity.
    m_buf.clear();
    m_buf.shrink_to_fit();
    m_buf.reserve(buf_reserve_bytes());
    m_rgb.reset();
    m_rgb_cap = 0;
    m_z.reset();
    m_z_cap = 0;
    m_idx.reset();
    m_idx_cap = 0;
    m_sixel_scratch = {};
    // Parallel staging is also frame-sized and must shrink with the window.
    m_zchunk.reset();
    m_zchunk_cap = 0;
    m_zchunk_len = std::vector<size_t>();
    m_zchunk_adler = std::vector<uint32_t>();
    m_sixel_parts = std::vector<std::string>();
    m_sixel_par_scratch = std::vector<sixel::Scratch>();
    m_par_covered = std::vector<uint8_t>();
    m_force_redraw = true;
    m_image_dirty = true;
    // Clear content from the previous dimensions in the next synchronized frame.
    m_pending_clear = true;
}

void Framebuffer::resize(
    int pixel_width, int pixel_height, int image_cols, int image_rows, int origin_col, int origin_row
)
{
    m_gfx.cols = image_cols;
    m_gfx.rows = image_rows;
    m_gfx.origin_col = origin_col;
    m_gfx.origin_row = origin_row;
    resize(pixel_width, pixel_height);
}

void Framebuffer::clear(Color bg)
{
    fill_cleared(pack_color(bg));
    m_image_dirty = true;
}

void Framebuffer::begin_frame()
{
    m_buf.clear();
    m_buf += SYNC_BEGIN;
    if (m_pending_clear)
    {
        m_buf += "\033[2J"; // the erase a resize deferred here (see the member)
        m_pending_clear = false;
    }
}

void Framebuffer::end_frame()
{
    // Skip an empty synchronized frame, but still write a deferred erase-only
    // frame after resize.
    if (m_buf.size() == sizeof(SYNC_BEGIN) - 1)
    {
        return;
    }
    m_buf += "\033[?2026l";
    if (!platform::write_terminal(m_buf.data(), m_buf.size(), /*cancel_on_control=*/true, m_write_frame))
    {
        m_output_recovery_pending = !recover_terminal_output();
        // SIGCONT can cancel a stop after output was abandoned. The next frame
        // must repaint even if the main loop no longer sees a suspend request.
        m_force_redraw = true;
        m_image_dirty = true;
        m_pending_clear = true;
    }
}

bool Framebuffer::recover_terminal_output() noexcept
{
    if (!platform::end_terminal_frame())
    {
        return false;
    }
    if (m_gfx.backend == GraphicsBackend::Kitty)
    {
        // Ghostty keeps partial uploads across deletes. Finish quietly before
        // deleting so the next frame cannot append to an abandoned upload.
        if (!platform::write_terminal_cleanup(kitty::FINISH_TRANSMISSION) ||
            !platform::write_terminal_cleanup(kitty::DELETE_IMAGE))
        {
            return false;
        }
        m_image_shown = false;
    }
    return true;
}

void Framebuffer::present()
{
    if (!m_headless && !m_terminal_active)
    {
        return;
    }
    if (m_output_recovery_pending)
    {
        if (!recover_terminal_output())
        {
            return;
        }
        m_output_recovery_pending = false;
    }
    if (m_gfx.backend == GraphicsBackend::Kitty)
    {
        present_kitty();
        return;
    }
    if (m_gfx.backend == GraphicsBackend::Sixel)
    {
        present_sixel();
        return;
    }
    if (m_mode == ColorMode::TrueColor)
    {
        present_impl<true>();
    }
    else
    {
        present_impl<false>();
    }
}

// Serialize a direct-transport frame across the borrowed worker pool. The shm
// path streams chunks separately and gains nothing from this parallel stage.
void Framebuffer::write_rgb(unsigned char *out)
{
    const size_t npx = m_pixel.size();
    // Below this, dispatch costs more than the work it saves.
    constexpr size_t MIN_PARALLEL_PIXELS = size_t{ 1 } << 18u;
    if (!m_par.usable() || npx < MIN_PARALLEL_PIXELS)
    {
        write_rgb_range(out, 0, npx);
        return;
    }
    split_ranges(
        m_par, m_par_covered, npx, [this, out](size_t lo, size_t hi) { write_rgb_range(out + (lo * 3u), lo, hi - lo); }
    );
}

void Framebuffer::write_rgb_range(unsigned char *out, size_t first, size_t count) const
{
    for (size_t i = first; i < first + count; i++)
    {
        const uint32_t c = unpack_color_bits(m_pixel[i].load(std::memory_order_relaxed));
        out[0] = static_cast<unsigned char>(c);
        out[1] = static_cast<unsigned char>(c >> 8u);
        out[2] = static_cast<unsigned char>(c >> 16u);
        out += 3;
    }
}

bool Framebuffer::transmit_shm()
{
    // On Windows platform.h's shm stubs make the open fail, so this reads as
    // a per-frame fallback to direct; m_gfx.shm can never be true there anyway
    // (the startup probe can never verify a transport whose open always fails).
    const size_t npx = m_pixel.size();
    char name[64];
    shm_ring_name(name, sizeof name, m_shm_seq % SHM_RING);
    m_shm_seq++;

    // Convert in an L2-sized chunk. Allocate before opening the non-RAII ShmFrame so
    // the open-to-close span cannot throw and leak the object.
    constexpr size_t CHUNK_BYTES = size_t{ 256 } * 1024;
    constexpr size_t CHUNK_PX = CHUNK_BYTES / 3u;
    ensure_capacity(m_rgb, m_rgb_cap, CHUNK_PX * 3u);

    platform::ShmFrame frame = platform::shm_frame_open(name, npx * 3u);
    // Windows uses an always-invalid open stub, so cppcheck's multi-config scan
    // treats this condition as constant.
    // cppcheck-suppress knownConditionTrueFalse
    if (!frame.valid())
    {
        return false;
    }
    bool ok = true;
    for (size_t done = 0; done < npx && ok; done += CHUNK_PX)
    {
        const size_t take = (npx - done < CHUNK_PX) ? npx - done : CHUNK_PX;
        write_rgb_range(m_rgb.get(), done, take);
        ok = platform::shm_frame_append(frame, m_rgb.get(), take * 3u);
    }
    platform::shm_frame_close(frame);
    if (!ok)
    {
        // Reclaim a partial object instead of transmitting a torn frame. Use direct
        // transport now and retry the startup-verified shm path next frame.
        platform::shm_frame_remove(name);
        return false;
    }
    kitty::append_transmit_shm(m_buf, name, m_width, m_height, m_gfx.cols, m_gfx.rows);
    return true;
}

// Compress independent raw-DEFLATE chunks ending in sync flushes, then wrap their
// concatenation as one zlib stream. Return zero if any chunk fails.
size_t Framebuffer::deflate_frame_parallel(size_t len, int chunks)
{
    const size_t per = (len + static_cast<size_t>(chunks) - 1) / static_cast<size_t>(chunks);
    // Worst case a chunk expands: stored blocks cost 5 bytes per 65535, plus the sync
    // flush marker and slack. mz_compressBound covers the expansion; the rest is framing.
    const size_t stride = mz_compressBound(static_cast<mz_ulong>(per)) + 64u;
    const size_t need = stride * static_cast<size_t>(chunks);
    ensure_capacity(m_zchunk, m_zchunk_cap, need);
    m_zchunk_len.assign(static_cast<size_t>(chunks), 0u);

    m_zchunk_adler.assign(static_cast<size_t>(chunks), 0u);

    const unsigned char *src = m_rgb.get();
    unsigned char *blocks = m_zchunk.get();
    std::vector<size_t> *lens = &m_zchunk_len;
    std::vector<uint32_t> *adlers = &m_zchunk_adler;

    // A compressor is about 300 KB, so allocate it off the worker stack.
    m_par.run(
        [src, blocks, lens, adlers, len, per, stride, chunks](int worker_id, int n_workers)
        {
            for (int c = worker_id; c < chunks; c += n_workers)
            {
                const size_t lo = std::min(len, per * static_cast<size_t>(c));
                const size_t hi = std::min(len, lo + per);
                if (lo >= hi)
                {
                    continue;
                }
                // Folded in here rather than in a second pass over the whole frame: this
                // chunk's bytes are already being read, so the checksum rides along.
                (*adlers)[static_cast<size_t>(c)] =
                    static_cast<uint32_t>(mz_adler32(MZ_ADLER32_INIT, src + lo, hi - lo));
                auto z = std::make_unique<tdefl_compressor>();
                // -15 == raw deflate: the zlib container is written once, below.
                const auto flags =
                    static_cast<int>(tdefl_create_comp_flags_from_zip_params(1, -15, MZ_DEFAULT_STRATEGY));
                if (tdefl_init(z.get(), nullptr, nullptr, flags) != TDEFL_STATUS_OKAY)
                {
                    continue;
                }
                size_t in_n = hi - lo;
                size_t out_n = stride;
                const tdefl_status st = tdefl_compress(
                    z.get(), src + lo, &in_n, blocks + (stride * static_cast<size_t>(c)), &out_n, TDEFL_SYNC_FLUSH
                );
                // miniz may report OKAY with a pending flush tail when output fills.
                // Reject that truncated chunk and send the frame raw.
                if (st == TDEFL_STATUS_OKAY && in_n == hi - lo && out_n < stride)
                {
                    (*lens)[static_cast<size_t>(c)] = out_n;
                }
            }
        }
    );

    size_t total = 2u; // zlib header
    for (int c = 0; c < chunks; c++)
    {
        const size_t lo = std::min(len, per * static_cast<size_t>(c));
        const size_t got = m_zchunk_len[static_cast<size_t>(c)];
        if (got == 0 && lo < len)
        {
            return 0; // a worker gave up (bad_alloc); send the frame raw instead
        }
        total += got;
    }
    total += 6u; // final empty block + adler32

    ensure_capacity(m_z, m_z_cap, total);
    unsigned char *out = m_z.get();
    size_t at = 0;
    // CMF/FLG for deflate with a 32 KB window; (0x78 << 8 | 0x01) % 31 == 0, as zlib requires.
    out[at++] = 0x78;
    out[at++] = 0x01;
    for (int c = 0; c < chunks; c++)
    {
        const size_t got = m_zchunk_len[static_cast<size_t>(c)];
        std::memcpy(out + at, blocks + (stride * static_cast<size_t>(c)), got);
        at += got;
    }
    // BFINAL=1, BTYPE=fixed, immediately the end-of-block symbol, zero-padded.
    out[at++] = 0x03;
    out[at++] = 0x00;
    // Fold per-chunk checksums in band order to reproduce whole-frame Adler-32.
    // The empty-input seed is the combine identity and keeps zero-length streams valid.
    uint32_t adler = 1;
    for (int c = 0; c < chunks; c++)
    {
        const size_t lo = std::min(len, per * static_cast<size_t>(c));
        const size_t hi = std::min(len, lo + per);
        if (lo >= hi)
        {
            continue;
        }
        adler = adler_combine(adler, m_zchunk_adler[static_cast<size_t>(c)], hi - lo);
    }
    out[at++] = static_cast<unsigned char>(adler >> 24u);
    out[at++] = static_cast<unsigned char>(adler >> 16u);
    out[at++] = static_cast<unsigned char>(adler >> 8u);
    out[at++] = static_cast<unsigned char>(adler);
    return at;
}

size_t Framebuffer::deflate_frame(size_t len)
{
    // Below this the split is not worth its framing: each chunk adds a sync-flush marker
    // and loses its back-reference window, and the whole deflate is already sub-millisecond.
    constexpr size_t MIN_PARALLEL_BYTES = size_t{ 1 } << 20u;
    if (m_par.usable() && len >= MIN_PARALLEL_BYTES)
    {
        // Four chunks per worker balance variable compressibility with little ratio loss.
        constexpr size_t MIN_CHUNK_BYTES = size_t{ 64 } << 10u;
        const int chunks = std::min(m_par.n_workers * 4, static_cast<int>(len / MIN_CHUNK_BYTES));
        if (chunks > 1)
        {
            const size_t n = deflate_frame_parallel(len, chunks);
            if (n != 0)
            {
                return n;
            }
        }
    }

    // unsigned int, not mz_ulong: mz_ulong is unsigned long, which IS size_t at LP64 (a direct
    // cast trips -Wuseless-cast there) but 32-bit on LLP64 (where the narrowing must be explicit
    // for /W4). A frame's byte count is far below either bound.
    const auto src_len = static_cast<unsigned int>(len);
    mz_ulong z_len = mz_compressBound(src_len);
    ensure_capacity(m_z, m_z_cap, z_len);
    if (mz_compress2(m_z.get(), &z_len, m_rgb.get(), src_len, 1) == MZ_OK)
    {
        return z_len;
    }
    return 0;
}

void Framebuffer::transmit_direct()
{
    const size_t rgb_len = m_pixel.size() * 3u;
    ensure_capacity(m_rgb, m_rgb_cap, rgb_len);
    write_rgb(m_rgb.get());
    // Level 1 captures most frame redundancy; failures fall back to identical raw pixels.
    const size_t z_len = deflate_frame(rgb_len);
    if (z_len != 0)
    {
        kitty::append_transmit_direct(m_buf, m_z.get(), z_len, m_width, m_height, m_gfx.cols, m_gfx.rows, true);
        return;
    }
    kitty::append_transmit_direct(m_buf, m_rgb.get(), rgb_len, m_width, m_height, m_gfx.cols, m_gfx.rows, false);
}

void Framebuffer::present_kitty()
{
    begin_frame();
    const bool full_redraw = m_force_redraw;
    m_force_redraw = false;

    // A one-row terminal with a HUD has no image rows. Delete the previously
    // displayed image once; a zero-length shm frame would not replace it.
    if (m_pixel.empty())
    {
        if (m_image_shown)
        {
            kitty::append_delete(m_buf);
            m_image_shown = false;
        }
    }
    else if (m_image_dirty || full_redraw)
    {
        // The placement renders at the cursor cell; home it first. C=1 in the
        // transmit keys then leaves the cursor untouched for the HUD block.
        m_buf += "\033[1;1H";
        // Use direct transport for this frame, then retry startup-verified shm.
        // On Windows, the shm stub makes !sent statically true in cppcheck's multi-config scan.
        bool sent = false;
        if (m_gfx.shm)
        {
            sent = transmit_shm();
        }
        // cppcheck-suppress knownConditionTrueFalse
        if (!sent)
        {
            transmit_direct();
        }
        m_image_dirty = false;
        m_image_shown = true;
    }

    append_hud_line(full_redraw);
    end_frame();
}

// Map pixels onto the fixed 240-entry palette, splitting independent ranges
// across the borrowed worker pool.
void Framebuffer::quantize_to_palette(size_t npx)
{
    const uint8_t *lut = quant256_lut().data();
    unsigned char *idx = m_idx.get();
    const std::atomic<uint64_t> *px = m_pixel.data();

    const auto quantize_range = [lut, idx, px](size_t lo, size_t hi) noexcept
    {
        for (size_t i = lo; i < hi; i++)
        {
            // Packed-word indexing: quant256_idx_packed ignores bits 24+, so
            // neither COLOR_MASK nor the Color round trip is needed.
            idx[i] = lut[quant256_idx_packed(static_cast<uint32_t>(px[i].load(std::memory_order_relaxed)))];
        }
    };

    // Below this the dispatch round trip costs more than the loop it saves.
    constexpr size_t MIN_PARALLEL_PIXELS = size_t{ 1 } << 18u;
    if (!m_par.usable() || npx < MIN_PARALLEL_PIXELS)
    {
        quantize_range(0, npx);
        return;
    }
    // An uncovered range leaves indices uninitialized; values below 16 would
    // underflow the encoder's `index - 16` register lookup.
    split_ranges(m_par, m_par_covered, npx, quantize_range);
}

// Encode independent sixel band ranges in parallel, then concatenate them in
// order to reproduce the serial frame.
void Framebuffer::encode_sixel_frame()
{
    const int bands = sixel::band_count(m_height);
    const int workers = m_par.n_workers;
    // Require two bands per worker. Degenerate widths also use the serial path
    // so header, bands, and footer agree on whether a frame exists.
    if (m_width <= 0 || !m_par.usable() || bands < workers * 2)
    {
        sixel::append_frame(m_buf, idx_plane(), m_width, m_height, m_sixel_scratch);
        return;
    }

    m_sixel_parts.resize(static_cast<size_t>(workers));
    m_sixel_par_scratch.resize(static_cast<size_t>(workers));
    // Clear before dispatch so a worker that never runs cannot contribute stale bytes.
    for (std::string &part : m_sixel_parts)
    {
        part.clear();
    }
    m_par_covered.assign(static_cast<size_t>(workers), 0u);

    const unsigned char *idx = idx_plane();
    const int px_w = m_width;
    const int px_h = m_height;
    std::vector<std::string> *parts = &m_sixel_parts;
    std::vector<sixel::Scratch> *scratch = &m_sixel_par_scratch;
    std::vector<uint8_t> *covered = &m_par_covered;

    const size_t before_header = m_buf.size();
    sixel::append_header(m_buf, px_w, px_h);
    m_par.run(
        [parts, scratch, covered, idx, px_w, px_h, bands](int worker_id, int n_workers)
        {
            // A pool-size mismatch invalidates the range split; leave the worker
            // uncovered so the completeness check selects serial encoding.
            const auto w = static_cast<size_t>(worker_id);
            if (static_cast<size_t>(n_workers) != covered->size() || w >= covered->size())
            {
                return;
            }
            const int per = (bands + n_workers - 1) / n_workers;
            const int lo = per * worker_id;
            if (lo < bands)
            {
                sixel::append_bands((*parts)[w], idx, px_w, px_h, lo, lo + per, (*scratch)[w]);
            }
            (*covered)[w] = 1u;
        }
    );

    // A worker allocation failure is swallowed by run_on_workers. Any uncovered
    // range therefore discards the parallel result and re-encodes serially.
    const bool complete = std::all_of(m_par_covered.begin(), m_par_covered.end(), [](uint8_t c) { return c != 0u; });
    if (!complete)
    {
        m_buf.resize(before_header);
        sixel::append_frame(m_buf, idx, px_w, px_h, m_sixel_scratch);
        return;
    }
    for (const std::string &part : m_sixel_parts)
    {
        m_buf += part;
    }
    sixel::append_footer(m_buf);
}

void Framebuffer::present_sixel()
{
    begin_frame();
    const bool full_redraw = m_force_redraw;
    m_force_redraw = false;

    // Sixel reserves the final terminal row, leaving a one-row terminal no image.
    // Nothing is resident to delete; begin_frame's deferred erase clears old cells.
    if (!m_pixel.empty() && (m_image_dirty || full_redraw))
    {
        // Position the cursor before sixel paints, accounting for centered capped
        // images. Its final cursor position is irrelevant because the HUD uses absolute positioning.
        append_cursor_pos(m_gfx.origin_row, m_gfx.origin_col);
        const size_t npx = m_pixel.size();
        ensure_capacity(m_idx, m_idx_cap, npx);
        quantize_to_palette(npx);
        encode_sixel_frame();
        m_image_dirty = false;
    }

    append_hud_line(full_redraw);
    end_frame();
}

template <bool TC> void Framebuffer::present_impl()
{
    begin_frame();
    // Everything past this point in m_buf is the pixel section's own output;
    // the trailing SGR reset below keys on it.
    const size_t pixel_base = m_buf.size();

    // Capture before the pixel section consumes the flag; a full redraw must
    // also restore the HUD erased by \033[2J.
    const bool full_redraw = m_force_redraw;

    const int term_rows = m_height / 2;

    char tmp[48]; // 36 bytes worst case for combined fg+bg SGR sequence
    int n = 0;

    // Hoist the lookup table and its static-init guard out of the pixel loop.
    const uint8_t *qlut = nullptr;
    if constexpr (!TC)
    {
        qlut = quant256_lut().data();
    }

    // The previous frame ended with \033[0m, so the terminal's initial colours
    // are unknown. Values are packed RGB or palette indices according to mode.
    uint32_t prev_fg = 0;
    uint32_t prev_bg = 0;
    bool fg_known = false;
    bool bg_known = false;

    // \033[NC: advance the cursor N columns; the bare form is 1 byte shorter for N == 1.
    auto append_cursor_advance = [&](int cols)
    {
        if (cols == 1)
        {
            m_buf.append("\033[C", 3);
            return;
        }
        tmp[0] = '\033';
        tmp[1] = '[';
        n = 2;
        n += write_int(tmp + n, cols);
        tmp[n++] = 'C';
        m_buf.append(tmp, static_cast<size_t>(n));
    };

    // Append an SGR color body so changed foreground and background share one escape.
    // Compile-time color mode keeps this per-cell path branch-free.
    auto write_color = [&](char lead, uint32_t raw)
    {
        tmp[n++] = lead;
        tmp[n++] = '8';
        tmp[n++] = ';';
        if constexpr (TC)
        {
            const Color c = unpack_color(raw);
            tmp[n++] = '2';
            tmp[n++] = ';';
            n += write_byte(tmp + n, c.r);
            tmp[n++] = ';';
            n += write_byte(tmp + n, c.g);
            tmp[n++] = ';';
            n += write_byte(tmp + n, c.b);
        }
        else
        {
            tmp[n++] = '5';
            tmp[n++] = ';';
            n += write_byte(tmp + n, static_cast<uint8_t>(raw));
        }
    };

    // Equal halves use a background-only space instead of ▀ with matching colours.
    // Otherwise emit ▀ with one combined SGR for changed colours. Raw equality matches
    // colour or palette-index equality in both modes.
    auto emit_cell = [&](uint32_t top, uint32_t bot)
    {
        if (top == bot)
        {
            const bool bg_change = !bg_known || bot != prev_bg;
            if (bg_change)
            {
                tmp[0] = '\033';
                tmp[1] = '[';
                n = 2;
                write_color('4', bot);
                tmp[n++] = 'm';
                m_buf.append(tmp, static_cast<size_t>(n));
                prev_bg = bot;
                bg_known = true;
                // fg_known intentionally unchanged: terminal fg is unaffected.
            }
            m_buf += ' ';
        }
        else
        {
            const bool fg_change = !fg_known || top != prev_fg;
            const bool bg_change = !bg_known || bot != prev_bg;
            if (fg_change || bg_change)
            {
                tmp[0] = '\033';
                tmp[1] = '[';
                n = 2;
                if (fg_change)
                {
                    write_color('3', top);
                    prev_fg = top;
                    fg_known = true;
                    if (bg_change)
                    {
                        tmp[n++] = ';';
                    }
                }
                if (bg_change)
                {
                    write_color('4', bot);
                    prev_bg = bot;
                    bg_known = true;
                }
                tmp[n++] = 'm';
                m_buf.append(tmp, static_cast<size_t>(n));
            }
            m_buf.append(UPPER_HALF, 3);
        }
    };

    // The value stored per cell and compared against m_prev_color: packed RGB in truecolor (byte-for-byte
    // as before), or the xterm-256 index in 256 mode. Quantizing here means two distinct RGB values that
    // collapse to the same index read as unchanged, so the incremental skip path coalesces them.
    auto load_color = [&](size_t i) -> uint32_t
    {
        const uint32_t rgb = unpack_color_bits(m_pixel[i].load(std::memory_order_relaxed));
        if constexpr (TC)
        {
            return rgb;
        }
        else
        {
            return qlut[quant256_idx(unpack_color(rgb))];
        }
    };

    if (m_force_redraw)
    {
        for (int row = 0; row < term_rows; row++)
        {
            append_cursor_pos(row + 1, 1);

            const int prow = row * 2;
            const size_t top_base = pixel_idx(0, prow);
            // Guard against odd pixel height: reuse top pixel for bottom row.
            const size_t bot_base = pixel_idx(0, prow + 1 < m_height ? prow + 1 : prow);

            for (int col = 0; col < m_width; col++)
            {
                const size_t ti = top_base + static_cast<size_t>(col);
                const size_t bi = bot_base + static_cast<size_t>(col);
                const uint32_t tc = load_color(ti);
                const uint32_t bc = load_color(bi);
                emit_cell(tc, bc);
                m_prev_color[ti] = tc;
                m_prev_color[bi] = bc;
            }
        }
        m_force_redraw = false;
    }
    else
    {
        for (int row = 0; row < term_rows; row++)
        {
            const int prow = row * 2;
            const size_t top_base = pixel_idx(0, prow);
            const size_t bot_base = pixel_idx(0, prow + 1 < m_height ? prow + 1 : prow);

            bool row_started = false;
            int col = 0;
            while (col < m_width)
            {
                const size_t ti = top_base + static_cast<size_t>(col);
                const size_t bi = bot_base + static_cast<size_t>(col);
                const uint32_t top_cur = load_color(ti);
                const uint32_t bot_cur = load_color(bi);
                if (top_cur != m_prev_color[ti] || bot_cur != m_prev_color[bi])
                {
                    if (!row_started)
                    {
                        append_cursor_pos(row + 1, col + 1);
                        row_started = true;
                    }
                    emit_cell(top_cur, bot_cur);
                    m_prev_color[ti] = top_cur;
                    m_prev_color[bi] = bot_cur;
                    col++;
                }
                else if (!row_started)
                {
                    col++;
                }
                else
                {
                    // col+0 is already known unchanged (dirty check above); start at 1.
                    int skip = 1;
                    while (col + skip < m_width &&
                           load_color(top_base + static_cast<size_t>(col + skip)) ==
                               m_prev_color[top_base + static_cast<size_t>(col + skip)] &&
                           load_color(bot_base + static_cast<size_t>(col + skip)) ==
                               m_prev_color[bot_base + static_cast<size_t>(col + skip)])
                    {
                        skip++;
                    }
                    col += skip;
                    // Do not advance past a clean row tail; all later output uses
                    // absolute positioning.
                    if (col < m_width)
                    {
                        append_cursor_advance(skip);
                    }
                }
            }
        }
    }

    // Reset SGR after emitted pixels, but keep a fully clean frame empty so
    // end_frame can skip it.
    if (m_buf.size() > pixel_base)
    {
        m_buf += "\033[0m";
    }

    append_hud_line(full_redraw);
    end_frame();
}

void Framebuffer::append_cursor_pos(int row, int col)
{
    char tmp[24]; // exact worst case: ESC [ + two 10-digit int coordinates + ; + H
    tmp[0] = '\033';
    tmp[1] = '[';
    int n = 2;
    n += write_int(tmp + n, row);
    tmp[n++] = ';';
    n += write_int(tmp + n, col);
    tmp[n++] = 'H';
    m_buf.append(tmp, static_cast<size_t>(n));
}

void Framebuffer::append_hud_line(bool full_redraw)
{
    // Skip an unchanged HUD unless a full redraw erased its row.
    if (m_hud.empty())
    {
        // A full redraw may erase the remembered line; an empty HUD must forget it
        // so restoring the same text later is not skipped.
        m_prev_hud.clear();
        return;
    }
    if (!full_redraw && m_hud == m_prev_hud)
    {
        return;
    }

    // The row just below the pixel/image rows: the image's cell rows for the
    // pixel backends, half the pixel height in cell rows for blocks.
    const int hud_row = (is_pixel_backend() ? m_gfx.rows : m_height / 2) + 1;
    append_cursor_pos(hud_row, 1);

    // Disable auto-wrap so a long HUD string clips at the terminal edge
    // instead of wrapping onto the next line and corrupting the display.
    m_buf += "\033[?7l";
    if (m_mode == ColorMode::TrueColor)
    {
        // Keep these literals, the Palette256 branch, and its quantizer tests in
        // sync. The default foreground also styles raw set_hud() strings.
        static_assert(
            HUD_BAR_BG.r == 18 && HUD_BAR_BG.g == 18 && HUD_BAR_BG.b == 18 && HUD_BAR_FG.r == 220 &&
                HUD_BAR_FG.g == 220 && HUD_BAR_FG.b == 220,
            "the escape literal below spells these out; change both together"
        );
        m_buf += "\033[48;2;18;18;18m\033[38;2;220;220;220m";
    }
    else
    {
        m_buf += "\033[48;5;233m\033[38;5;253m";
    }
    // Erase before drawing: with wrapping off, a trailing EL0 would erase the final
    // column. This also clears residue from a previously wider HUD line.
    m_buf += "\033[K";
    m_buf += m_hud;
    m_buf += "\033[0m";
    m_buf += "\033[?7h"; // re-enable auto-wrap
    m_prev_hud = m_hud;
}
