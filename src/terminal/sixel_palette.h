#pragma once

// Per-frame sixel palette fitted to the image, and the table that maps pixels onto it.

#include "src/terminal/cielab.h"
#include "src/terminal/color.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sixel
{
    namespace detail
    {
        // The fit reads at most this many pixels per frame, whatever the resolution.
        inline constexpr int MAX_SAMPLES = 32768;

        // The pixels the fit reads: one per s x s block, partial blocks included, at a position
        // hashed from the block, so an unchanged frame gives the same sample. s is the smallest
        // stride with at most MAX_SAMPLES blocks. Appends indices y * width + x to `out` in
        // row-major block order; returns s.
        int sample_positions(int width, int height, std::vector<std::size_t> &out);

        // The packed 0x00BBGGRR colours of the pixels at `positions`, replacing `out`.
        void read_sample(
            const std::atomic<uint64_t> *px, const std::vector<std::size_t> &positions, std::vector<uint32_t> &out
        );

        // Sample counts per 5-bit-per-channel cell, with channel sums and the sum of squares.
        class Histogram
        {
          public:
            static constexpr int SIDE = 32;
            static constexpr std::size_t CELLS = std::size_t{ SIDE } * SIDE * SIDE;

            Histogram();
            // Clears only the cells the last fill occupied.
            void clear() noexcept;
            // Add `n` samples of one colour.
            void add(uint32_t rgb, uint32_t n = 1) noexcept;
            // Add a whole sample, each run of one colour, such as a background, in one step.
            void add_all(const std::vector<uint32_t> &sample) noexcept;

            // Occupied cells, `(r << 10) | (g << 5) | b` over the high 5 bits of each channel.
            [[nodiscard]] const std::vector<uint16_t> &occupied() const noexcept { return m_occupied; }
            [[nodiscard]] uint32_t count(uint16_t cell) const noexcept { return m_count[cell]; }
            [[nodiscard]] uint32_t sum_r(uint16_t cell) const noexcept { return m_sum_r[cell]; }
            [[nodiscard]] uint32_t sum_g(uint16_t cell) const noexcept { return m_sum_g[cell]; }
            [[nodiscard]] uint32_t sum_b(uint16_t cell) const noexcept { return m_sum_b[cell]; }
            [[nodiscard]] uint64_t sum_sq(uint16_t cell) const noexcept { return m_sum_sq[cell]; }

          private:
            std::vector<uint32_t> m_count, m_sum_r, m_sum_g, m_sum_b;
            std::vector<uint64_t> m_sum_sq;
            std::vector<uint16_t> m_occupied;
        };

        // Wu's quantizer: greedy maximum-variance box splits over int64_t cumulative moments.
        // Being greedy, split_to(n) gives the same boxes whether or not split_to(m < n) ran first.
        class Wu
        {
          public:
            Wu();
            void build(const Histogram &hist);
            // Split until there are `count` boxes or no box can be split.
            void split_to(int count);
            // Mean sRGB colour of each box, 0..255 per channel.
            void box_means(std::vector<std::array<float, 3>> &out) const;

          private:
            struct Box
            {
                int r0, r1, g0, g1, b0, b1;
            };
            static constexpr int N = Histogram::SIDE + 1;
            [[nodiscard]] static std::size_t at(int r, int g, int b) noexcept
            {
                return (((static_cast<std::size_t>(r) * N) + static_cast<std::size_t>(g)) * N) +
                       static_cast<std::size_t>(b);
            }
            [[nodiscard]] static int64_t volume(const std::vector<int64_t> &m, const Box &x) noexcept;
            [[nodiscard]] double variance(const Box &x) const noexcept;

            std::vector<int64_t> m_wt, m_r, m_g, m_b, m_sq;
            std::vector<Box> m_boxes;
            std::vector<double> m_var;
        };

        // Occupied histogram cells as CIELAB colours of their mean, weighted by sample count.
        struct Cells
        {
            std::vector<float> L, a, b, weight;
            void assign(const Histogram &hist);
            [[nodiscard]] std::size_t size() const noexcept { return weight.size(); }
        };

        // Palette entries in CIELAB, structure of arrays so the distance scans vectorize.
        struct LabEntries
        {
            std::array<float, 256> L{}, a{}, b{};
            int count = 0;
            void set(const std::vector<Color> &entries);
        };

        // Count-weighted mean distance from each cell to its nearest entry, in dE.
        [[nodiscard]] double mean_error(const Cells &cells, const LabEntries &entries);

        // Lloyd's k-means in CIELAB over the cells. An empty cluster keeps its centroid. Stops
        // early once no cell changes cluster, since every further pass would repeat it.
        void lloyd(const Cells &cells, LabEntries &centroids, int passes);

        // Give each pin an entry, in priority order: an entry already equal to it, else a new
        // entry while there are fewer than `budget`, else the nearest entry no earlier pin
        // holds. Pins are percent-rounded colours, at most `budget` of them. Returns each
        // pin's entry index.
        std::vector<int> place_pins(std::vector<Color> &entries, const std::vector<Color> &pins, int budget);

        // Table value for a cell centre: the nearest entry in bits 0..7 and up to 3 others within
        // its distance + 1 dE, closest first, in bits 8..31. A missing alternate repeats the
        // nearest entry, so EMPTY_ENTRY (nearest 255, alternate 3 = 0) is never produced.
        inline constexpr uint32_t EMPTY_ENTRY = 0x00FFFFFFu;
        [[nodiscard]] uint32_t table_entry(Lab cell, const LabEntries &entries) noexcept;

        // CIELAB colour at the centre of a 64^3 table cell (quant256_idx layout).
        [[nodiscard]] Lab cell_centre_lab(std::size_t cell) noexcept;

        // Value of a pinned cell: the pin's entry and no alternates.
        [[nodiscard]] constexpr uint32_t pinned_entry(int entry) noexcept
        {
            return static_cast<uint32_t>(entry) * 0x01010101u;
        }

        // The colour a register shows after the percent encoding.
        [[nodiscard]] Color percent_round_trip(Color c) noexcept;
    } // namespace detail

    // Owns the fitted palette, its register block and the lazy 64^3 lookup table. Pixels are
    // framebuffer slots: colour 0x00BBGGRR in the low 24 bits, other bits ignored.
    class FittedPalette
    {
      public:
        explicit FittedPalette(int colors);

        FittedPalette(const FittedPalette &) = delete;
        FittedPalette &operator=(const FittedPalette &) = delete;
        FittedPalette(FittedPalette &&) = delete;
        FittedPalette &operator=(FittedPalette &&) = delete;
        ~FittedPalette();

        // Sample the frame and refit when the current palette's error exceeds a fresh fit's by
        // more than 5% and more than 0.05 dE, or when a new pin has no exact entry. `required`
        // must be an exact entry whether or not the sample saw it. Returns true when it installed
        // a new palette. Calling thread only, before any map_rows call for this frame.
        bool update(
            const std::atomic<uint64_t> *px, int width, int height, uint32_t clear_rgb, std::optional<uint32_t> required
        );

        // Map rows [y0, y1) of the frame to register numbers in `out`, the frame-sized plane.
        // Safe concurrently on disjoint row ranges.
        void map_rows(const std::atomic<uint64_t> *px, int width, int y0, int y1, unsigned char *out) const noexcept;

        [[nodiscard]] const std::string &register_block() const noexcept { return m_block; }
        // Registers emitted, at most budget().
        // cppcheck-suppress unusedFunction -- tests read it.
        [[nodiscard]] int entry_count() const noexcept { return static_cast<int>(m_entries.size()); }
        // Size the fresh fits use.
        // cppcheck-suppress unusedFunction -- tests read it.
        [[nodiscard]] int budget() const noexcept { return m_budget; }
        // Fits installed so far, the first frame's included, even those identical to the last.
        [[nodiscard]] unsigned generation() const noexcept { return m_generation; }
        // The installed colours, in register order, as the terminal shows them.
        // cppcheck-suppress unusedFunction -- tests read it.
        [[nodiscard]] const std::vector<Color> &entries() const noexcept { return m_entries; }

      private:
        struct Fit
        {
            std::vector<Color> entries;
            detail::LabEntries lab;
            double error = 0.0;
        };

        void collect_pins(uint32_t clear_rgb, std::optional<uint32_t> required);
        void fit(int budget, Fit &out);
        void install(Fit &fit);
        void pin_table_cells();
        [[nodiscard]] bool has_exact_entry(Color pin) const noexcept;
        [[nodiscard]] uint32_t fill(std::size_t cell) const noexcept;

        int m_budget;
        unsigned m_generation = 0;

        // Pixel indices the fit reads in frames of the recorded size. Hashing the positions costs
        // about four times as much as reading the pixels, so they are computed once per size.
        std::vector<std::size_t> m_positions;
        int m_positions_width = 0;
        int m_positions_height = 0;
        std::vector<uint32_t> m_sample;
        detail::Histogram m_hist;
        detail::Wu m_wu;
        detail::Cells m_cells;
        std::vector<std::array<float, 3>> m_means;

        // Pins of the current frame as source colours, highest priority first, and of the last.
        std::vector<Color> m_pins;
        std::vector<Color> m_prev_pins;

        std::vector<Color> m_entries;
        detail::LabEntries m_lab;
        std::string m_block;
        Fit m_fresh;

        // Cells currently holding a pinned value, so a pin that goes away can be refilled.
        std::vector<std::size_t> m_pinned_cells;
        // Lazily filled 64^3 table; see detail::table_entry for the value layout.
        std::unique_ptr<std::atomic<uint32_t>[]> m_table;
    };
} // namespace sixel
