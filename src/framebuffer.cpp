#include "framebuffer.h"

#include "color.h"    // Color, ColorMode
#include "graphics.h" // GraphicsBackend
#include "kitty.h"    // escape composition for the kitty backend
#include "platform.h" // shm frame helpers (POSIX)
#include "sixel.h"    // escape composition for the sixel backend

#include "miniz.h" // zlib deflate for the kitty direct transport; config macros come from the build systems

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
    // Top pixel → foreground color, bottom pixel → background color.
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
    // Ring of shm object names, one per in-flight frame. The terminal unlinks an
    // object after reading it, so by the time a slot comes around again its old
    // object is normally gone; the pre-create unlink in shm_frame_open reclaims
    // one that never got read. The ring is slack, not backpressure: the pty
    // carries only tiny escapes, so a stalled terminal can have more than four of
    // them queued, and a slot then wraps while its escape is unread. The terminal,
    // reaching the stale escape, opens the slot's newer object (at worst mid-write:
    // one torn or dropped frame, replaced by the very next transmit). If a RESIZE
    // came between, the new object is a different inode of a different size, so the
    // stale escape's s=/v= describe more pixels than it holds; a terminal that sizes
    // the object before reading (kitty does) drops that frame rather than reading
    // past the end, and q=2 suppresses the error reply it would send back. A create
    // that fails AFTER its pre-unlink costs the unread predecessor the same way,
    // and the same-frame direct fallback repaints. Accepted:
    // real backpressure would need reading per-frame replies, which q=2 suppresses
    // on purpose so they cannot interleave with interactive input.
    constexpr unsigned SHM_RING = 4;

    // Keyed on the pid alone, which assumes ONE kitty Framebuffer per process: a second live one
    // would recreate the very names the first has named in unread escapes, and its destructor
    // would unlink all four of the first's slots. The app builds one; only the tests construct
    // more, and they do so one at a time.
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

    // Run fn(lo, hi) over an even split of [0, total) across the borrowed pool, then redo
    // serially any range whose worker did not report back.
    //
    // The bookkeeping is not paranoia: both callers below fill a buffer that something else
    // then reads as pixels, and neither loop allocates, so a worker that never ran leaves no
    // trace of itself. A skipped range would ship uninitialized bytes, which for the sixel
    // plane is a memory-safety hole rather than merely wrong colours (see quantize_to_palette).
    // fn by const reference, not a forwarding reference: it is called once per worker and again
    // for any range the pool did not cover, so it must not be consumed by the first call.
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
                // Sit the round out unless the pool is the size the flags were made for. The
                // flags say which ranges the SERIAL redo below can skip, and it derives those
                // ranges from its own count, so a worker splitting by a different one would
                // mark a flag for a range it did not fill: a silent hole rather than a redo.
                // Bailing here instead leaves every flag clear and the redo covers the frame.
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

    // The adler32 of a concatenation, from the two halves' own adler32s and the length of
    // the second (zlib's adler32_combine, which miniz does not export). It lets the
    // parallel deflate fold in the checksum of each chunk as that chunk is compressed
    // instead of making a second serial pass over the whole frame, which measured 1.40 ms
    // at 1080p and 5.53 ms at 4K, a quarter of the direct transport's remaining cost.
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
    int pixel_width, int pixel_height, bool headless, ColorMode mode, const GraphicsConfig &gfx, bool adopt_alt_screen
)
    : m_width(pixel_width), m_height(pixel_height),
      m_pixel(static_cast<size_t>(pixel_width) * static_cast<size_t>(pixel_height)), m_headless(headless), m_mode(mode),
      m_gfx(gfx)
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
        if (!adopt_alt_screen)
        {
            std::fputs("\033[?1049h", stdout); // enter alternate screen buffer
        }
        std::fputs("\033[?25l", stdout); // hide cursor
        std::fflush(stdout);
    }
}

Framebuffer::~Framebuffer()
{
    if (!m_headless)
    {
        if (m_gfx.backend == GraphicsBackend::Kitty)
        {
            // Free the resident image terminal-side before leaving; without this
            // the last frame's data stays in the terminal for the session.
            std::string cleanup;
            kitty::append_delete(cleanup);
            std::fputs(cleanup.c_str(), stdout);
        }
        // Restore cursor, reset colours, then leave the alternate screen buffer —
        // this restores the terminal to exactly the state it was in before launch.
        std::fputs("\033[?25h\033[0m\033[?1049l", stdout);
        std::fflush(stdout);
    }
    // Outside the headless guard on purpose: headless only suppresses terminal
    // escapes, but a headless kitty framebuffer that called present() (tests)
    // still created ring objects, and unlinking is not terminal output.
    if (m_gfx.backend == GraphicsBackend::Kitty)
    {
        // The terminal unlinks what it reads, but frames it never read (and
        // anything left by an earlier crash of this pid) are ours to reclaim.
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
    // The output and staging buffers are sized to the frame; a resize is where
    // releasing them is free (everything reallocates to the new size either
    // way), so a shrunk window gives their memory back: shrink m_buf before
    // re-reserving (reserve alone never shrinks, and in kitty direct mode the
    // old capacity, grown by the first transmit, is the largest allocation in
    // the process). The staging buffers' session-long retention at a CONSTANT
    // size after a transient shm fallback stays deliberate (see buf_reserve_bytes).
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
    // The parallel staging is frame-sized too and must follow the same policy: the
    // deflate chunk blocks are ~1.1x the frame, and the sixel parts and per-worker
    // scratch together hold a whole encoded frame plus a mask per worker. Without
    // this a shrunk 4K window keeps tens of megabytes for the rest of the session.
    m_zchunk.reset();
    m_zchunk_cap = 0;
    m_zchunk_len = std::vector<size_t>();
    m_zchunk_adler = std::vector<uint32_t>();
    m_sixel_parts = std::vector<std::string>();
    m_sixel_par_scratch = std::vector<sixel::Scratch>();
    m_par_covered = std::vector<uint8_t>();
    m_force_redraw = true;
    m_image_dirty = true;
    // Leftover content from the previous (possibly larger) terminal is wiped
    // by the next present, inside its synchronized bracket (see the member).
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
    // Nothing composed past the opening bracket: write zero bytes, so an idle
    // present on any backend stays literally silent (no bare 2026 pairs at the
    // idle pacing rate; blocks composes nothing when every cell is clean).
    // Compared against the bare bracket, NOT a base captured after
    // begin_frame(): that base would include a deferred \033[2J, and an
    // erase-only frame (sixel shrunk to zero rows) must still be written.
    if (m_buf.size() == sizeof(SYNC_BEGIN) - 1)
    {
        return;
    }
    m_buf += "\033[?2026l";
    // Return values intentionally ignored: a write failure to a terminal means
    // the session is already broken; there is no meaningful recovery path here.
    (void)std::fwrite(m_buf.data(), 1, m_buf.size(), stdout);
    (void)std::fflush(stdout);
}

void Framebuffer::present()
{
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

// Whole-frame serialization into a plain buffer, split across the borrowed pool. This is
// the DIRECT transport's staging fill, which is ordinary user-space work and does scale;
// the shm path deliberately does NOT come through here (it streams per chunk through
// write_rgb_range, and parallelizing that was measured useless because the kernel
// serializes the tmpfs write behind page allocation and the inode lock).
void Framebuffer::write_rgb(unsigned char *out)
{
    const size_t npx = m_pixel.size();
    // Below this the dispatch round trip costs more than the loop it saves, the same
    // threshold quantize_to_palette uses.
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

    // Converted and handed over a chunk at a time rather than as one frame-sized
    // buffer: the chunk stays L2-resident across the unpack and the append, and
    // nothing frame-sized has to be allocated (256 KB against 24 MB at 4K). The
    // size is flat across 64 KB - 1 MB (2.25 / 2.21 / 2.24 ms at 1080p), so it is
    // picked for the cache residency rather than tuned.
    //
    // Reserved BEFORE the object is opened, and deliberately so: ShmFrame holds a raw
    // fd with no destructor, so a throw between open and close would leak both the fd
    // and a frame-sized /dev/shm object (the ftruncate has already reserved it). This
    // is the only allocating call in the body, so hoisting it keeps the open..close
    // span nothrow, which is what the old mapping form got for free.
    constexpr size_t CHUNK_BYTES = size_t{ 256 } * 1024;
    constexpr size_t CHUNK_PX = CHUNK_BYTES / 3u;
    ensure_capacity(m_rgb, m_rgb_cap, CHUNK_PX * 3u);

    platform::ShmFrame frame = platform::shm_frame_open(name, npx * 3u);
    // cppcheck suppression: in the _WIN32 configuration the open is the stub
    // that always returns an invalid frame, so the multi-config scan reads this
    // condition as constant. Same false-positive class as present_kitty's.
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
        // A partially written object would be transmitted as a torn frame, so it
        // is reclaimed and this frame takes the direct transport instead. Reached
        // when /dev/shm fills mid-frame; shm is retried next frame, since the
        // medium itself was verified at startup.
        platform::shm_frame_remove(name);
        return false;
    }
    kitty::append_transmit_shm(m_buf, name, m_width, m_height, m_gfx.cols, m_gfx.rows);
    return true;
}

// Deflate one frame across the borrowed pool, as pigz does it: each chunk is compressed
// independently into raw DEFLATE and closed with TDEFL_SYNC_FLUSH, which emits an empty
// stored block and so leaves the chunk byte-aligned with no BFINAL bit set. The pieces
// therefore concatenate into one legal DEFLATE stream, and a zlib header, a final empty
// block and the adler32 of the WHOLE input make it the ordinary zlib stream the terminal
// already knows how to inflate (o=z). Returns the stream length, or 0 if any chunk failed.
//
// Splitting costs ratio in principle, because each chunk starts with an empty
// back-reference window, but a chunk here is hundreds of KB against deflate's 32 KB
// window: measured on real frames the output moves under 0.4% and in the cases tried it
// came out slightly SMALLER than the one-shot form, so there is no size paid for the speed.
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

    // One tdefl_compressor is ~300 KB, too large for the stack of a worker; each worker
    // allocates its own, which is why the callback can throw bad_alloc (run_on_workers
    // swallows it per worker and the zero length left behind fails the frame cleanly).
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
                // out_n < stride as well as the status: tdefl reports OKAY with all
                // input consumed even when the output buffer filled, parking the tail
                // (the 00 00 FF FF sync marker among it) in its internal flush
                // remainder. Such a chunk would splice a truncated deflate block into
                // the middle of the stream, which the terminal's inflate cannot
                // recover from, whereas a rejected chunk degrades cleanly to raw.
                // Not reachable at today's ~10% stride margin against a measured 0.02%
                // worst-case expansion, but the margin is an undocumented miniz
                // property that a version bump could change.
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
    // Folded from the per-chunk checksums in band order; equals the whole-frame adler32
    // exactly (pinned by test, and by the terminal's own inflate rejecting it otherwise).
    // Seeded with the adler32 of the empty input, which combine treats as an identity, so a
    // zero-length frame still emits a legal stream rather than a trailer of four zero bytes.
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
        // Four chunks per worker, not one. The split is uniform in INPUT size but not in
        // work: how long a chunk takes depends on how compressible it is, so with exactly
        // one chunk each the frame waits on the least compressible one. Oversubscribing
        // lets the steal loop even that out, worth 1.20x at 1080p and 1.14x at 4K over one
        // chunk per worker, for 0.35% more wire bytes (each chunk starts with an empty
        // 32 KB back-reference window). Measured 8 per worker as the turn: 0.98x the time
        // of 4 AND 0.83% more bytes, both directions worse.
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
    // zlib level 1: rendered frames (large flat background, smooth gradients)
    // compress several-fold even at the fastest level, and the levels above it buy
    // ~2% of size for 3x the time. It is worth doing at all because it removes ~5.4 MB
    // from a typical 1080p frame, which any real link takes far longer to carry than
    // the deflate takes to run. It is not cheap, though: 16 ms of a 1080p frame and
    // 72 ms of a 4K one, measured, which is why deflate_frame splits it across the
    // renderer's idle pool when one is wired in (3.2x, and marginally SMALLER output).
    // A compression failure falls back to the raw form (identical pixels, bigger
    // payload), which is also what a partly-failed parallel deflate degrades to.
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

    // A one-row terminal with the HUD shown leaves zero image rows, and nothing to show is
    // nothing to transmit. This guard is the only thing enforcing that now: the shm transport
    // used to refuse a zero-length frame by itself, since the mapping could not be created,
    // but the Linux fill streams to an fd and a zero-length object opens perfectly well.
    // The resident frame from before the shrink is still displayed (a retransmit no longer
    // replaces it, and nothing else touches it), covering the HUD row, so it is deleted
    // terminal-side once.
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
        // A failed shm frame falls back to the direct transport for THIS frame
        // only and shm is retried next frame: the medium itself was verified end
        // to end by the startup probe, so a failure here is transient (EMFILE, a
        // momentarily full /dev/shm) and must not demote the transport for the
        // rest of the session. The retry costs one failed syscall on a frame
        // that is about to spend milliseconds rendering.
        //
        // cppcheck suppression: transmit_shm() is constantly false in the _WIN32
        // configuration (shm_frame_open is the invalid-frame stub there), so the
        // multi-config scan reads !sent as always true. Same class of false
        // positive as main.cpp's VT-gate suppression.
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

// Map the frame's colour halves onto the fixed 240-entry palette, into m_idx.
// One hoisted-LUT load per pixel, and every pixel independent of every other, so it
// splits across the borrowed pool by range. Unlike the shm fill (which is bounded by
// in-kernel page allocation and did not benefit), this is plain user-space work.
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
    // An uncovered range leaves m_idx uninitialized, and that is a MEMORY-SAFETY hole, not
    // merely wrong pixels: the encoder derives a colour register as `index - 16` and uses it to
    // subscript a 240-entry stack array, so any stale byte below 16 indexes negatively
    // (reproduced under ASan). Hence split_ranges' covered bookkeeping rather than a bare split.
    split_ranges(m_par, m_par_covered, npx, quantize_range);
}

// Encode the quantized plane as one sixel frame. The bands are independent, so with a
// borrowed pool each worker encodes a contiguous range into its own buffer and the pieces
// are concatenated in band order, which is byte-for-byte what the serial encoder emits.
// Unlike the shm fill this is pure user-space work with no kernel serialization, so it
// scales; the encode was 2.6 ms of a 1080p frame and ~10 ms of a 4K one.
void Framebuffer::encode_sixel_frame()
{
    const int bands = sixel::band_count(m_height);
    const int workers = m_par.n_workers;
    // Two bands per worker minimum: below that the split cannot balance and the extra
    // per-worker staging (one register-by-width mask each) is not worth allocating.
    // A degenerate width goes serial as well: append_header and append_bands both decline
    // it, but append_footer does not, so the split would emit a bare ST with no DCS to end
    // (a zero height cannot reach that, having no bands). Unreachable from present_sixel,
    // which runs only on a non-empty frame; kept so the two paths agree for any caller.
    if (m_width <= 0 || !m_par.usable() || bands < workers * 2)
    {
        sixel::append_frame(m_buf, idx_plane(), m_width, m_height, m_sixel_scratch);
        return;
    }

    m_sixel_parts.resize(static_cast<size_t>(workers));
    m_sixel_par_scratch.resize(static_cast<size_t>(workers));
    // Cleared HERE, not inside the worker: a worker that never runs would otherwise
    // leave the PREVIOUS frame's bytes in its buffer, and the concatenation below would
    // splice them into the middle of this frame.
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
            // Same rule as split_ranges: a pool of a different size than the buffers were made
            // for would band up the frame differently, and marking a flag for it would let the
            // completeness check below pass over the bands nobody encoded, splicing a truncated
            // (but perfectly well-formed) frame. Sitting the round out forces the serial encode.
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

    // Unlike the quantize and the staging fill, this worker CAN fail: append_bands grows
    // a per-worker mask and a per-worker string that reaches megabytes on a 4K frame, so
    // bad_alloc is reachable and run_on_workers swallows it per worker. A partial range
    // would splice a truncated band (plausibly a half-written RLE token) into the middle
    // of the DCS string, which no length check downstream would notice, so any gap throws
    // the whole parallel attempt away and re-encodes serially.
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

    // Zero image rows (a one-row terminal; sixel reserves the last row even
    // without the HUD): nothing to paint, and unlike kitty nothing resident
    // terminal-side to delete; the erase the shrinking resize deferred into
    // this frame wipes the painted cells (\033[2J in begin_frame).
    if (!m_pixel.empty() && (m_image_dirty || full_redraw))
    {
        // Sixel paints at the cursor; position it first ((1, 1) unless a
        // geometry-capped image is being centered, see GraphicsConfig). Where
        // the terminal leaves the cursor afterwards varies, which is fine:
        // the HUD block positions absolutely.
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

    // Captured before the pixel section consumes the flag: the HUD must also be re-emitted on
    // any frame that repaints every cell (first frame, resize), since \033[2J wiped its row.
    const bool full_redraw = m_force_redraw;

    const int term_rows = m_height / 2;

    char tmp[48]; // 36 bytes worst case for combined fg+bg SGR sequence
    int n = 0;

    // Table pointer hoisted out of the pixel loops so quant256_lut()'s magic-static init guard is
    // paid once per frame, not per load_color call. Never read in the truecolor instantiation.
    const uint8_t *qlut = nullptr;
    if constexpr (!TC)
    {
        qlut = quant256_lut().data();
    }

    // fg_known / bg_known track whether the terminal's current fg/bg are
    // reflected by prev_fg / prev_bg.  Both start false because \033[0m at
    // the end of the previous frame reset SGR to an unknown terminal default.
    // prev_fg / prev_bg hold the raw cell value (packed RGB in truecolor, palette
    // index in 256; see load_color), compared directly against the incoming raw.
    uint32_t prev_fg = 0;
    uint32_t prev_bg = 0;
    bool fg_known = false;
    bool bg_known = false;

    // \033[NC — advance the cursor N columns; the bare form is 1 byte shorter for N == 1.
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

    // Append one SGR colour body (no leading ESC[ or trailing m) into tmp at n. Deliberately not
    // color.h's append_fg_sgr (the shared spelling for styling a string): this one emits a BODY so
    // a changed fg and bg combine into one escape, writes into a fixed buffer, and uses
    // write_byte's LUT to avoid a division; it runs per cell per frame, where those differences
    // are the point, while append_fg_sgr runs a dozen times a frame, where clarity is. The two
    // colour modes differ only here (38;2;r;g;b vs 38;5;idx, 48;... for bg); `if constexpr` gives
    // each present_impl instantiation only its own body, keeping the truecolor bytes exactly the
    // historical output. `raw` is load_color's cell value (packed RGB in truecolor, palette index
    // in the low byte in 256); `lead` is '3' (fg) or '4' (bg).
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

    // Cell emission, shared across modes. When top == bot a space with bg-only SGR is visually
    // identical to ▀ with fg==bg, saving the fg SGR and 2+ bytes; otherwise emit ▀ with one combined
    // SGR covering whichever of fg/bg changed (combining avoids a redundant ESC[ header and closing m).
    // top/bot are raw cell values compared directly (raw-equal == colour/index-equal in both modes).
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
                    // A clean run reaching the row end needs no advance: the cursor is hidden
                    // and never read back, and anything written after it (a later dirty row,
                    // the HUD, or the next frame) positions absolutely. With --no-hud on the
                    // last row nothing follows at all. Emitting one would be dead bytes on
                    // every row that ends in background.
                    if (col < m_width)
                    {
                        append_cursor_advance(skip);
                    }
                }
            }
        }
    }

    // Reset SGR once at the end of the pixel section — keeps the terminal clean
    // when the HUD is empty and prevents bleed into HUD's own colour escapes.
    // Only when the section emitted anything: an entirely-clean frame must
    // compose nothing, so end_frame's zero-byte skip covers blocks idle frames
    // too (the terminal's SGR state is already reset from the last frame that
    // did emit, and the HUD block sets its own colours in full).
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
    // The HUD changes at the fps latch rate or on a keypress, far below the frame rate, so an
    // unchanged line is skipped outright (most frames emit zero HUD bytes); a full redraw always
    // re-emits, since \033[2J wiped the row. This deliberately gives up the old incidental
    // self-healing of re-writing the row every frame: the pixel rows above never had that
    // property (diffed against m_prev_color the same way), so unconditional HUD output could
    // only restore one row of a screen corrupted as a whole, and the recovery for both is the
    // same full redraw any resize already triggers.
    if (m_hud.empty())
    {
        // Nothing is drawn, so nothing is on the row that a later frame could match against.
        // Without this the pair (empty HUD, full redraw) would leave m_prev_hud naming a line
        // the \033[2J just wiped, and restoring that same line afterwards would be skipped as
        // unchanged, forever. Not reachable from main.cpp, which sets a non-empty line every
        // frame or none at all, but the skip must not depend on that.
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
        // bg {18,18,18} and a default fg {220,220,220}; keep both in sync with the Palette256
        // branch and the test pins (quantize_256_grey_ramp_exact ties 233 and 253 below to
        // the quantizer of these same RGBs). A composed line overrides the fg immediately
        // with its own per-segment SGRs, so this exists for the other kind of caller: set_hud
        // takes an arbitrary string, and an unstyled one would otherwise draw in whatever
        // foreground the terminal happened to be left with. {220,220,220} is hud.cpp's
        // FG_VALUE, so an unstyled line matches the styled line's primary text.
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
    // Erase BEFORE drawing, never after: the line runs the full width, and with auto-wrap
    // off the cursor finishes ON the last column, so a trailing EL0 would erase from that
    // column inclusive and eat the final character (invisible to byte-level tests and to
    // pyte; only a real terminal shows it, see the tmux recipe in CLAUDE.md). Erasing first
    // also clears residue from a previously wider line, and on BCE terminals paints the row
    // in the bar background, covering the few columns the composer can underfill when it
    // rounds a doubtful glyph width up; non-BCE terminals get the bar from its own padding.
    m_buf += "\033[K";
    m_buf += m_hud;
    m_buf += "\033[0m";
    m_buf += "\033[?7h"; // re-enable auto-wrap
    m_prev_hud = m_hud;
}
