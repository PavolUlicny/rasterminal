#include "src/terminal/sixel_palette.h"

#include "src/terminal/cielab.h"
#include "src/terminal/color.h"
#include "src/terminal/sixel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Lloyd passes after Wu. Five matched ten in measured quality at half the cost.
    constexpr int LLOYD_PASSES = 5;
    // A pin forms when this share of the sample, in percent, equals the colour exactly.
    constexpr std::size_t PIN_COVERAGE_PCT = 1;
    // Keep the current palette while its error is within 5% of a fresh fit's, or within
    // 0.05 dE when the fresh fit is nearly exact.
    constexpr double KEEP_RATIO = 1.05;
    constexpr double KEEP_SLACK = 0.05;

    constexpr std::size_t TABLE_CELLS = QUANT256_LUT_SIZE;

    constexpr Color unpack(uint32_t rgb) noexcept
    {
        return { static_cast<uint8_t>(rgb), static_cast<uint8_t>(rgb >> 8u), static_cast<uint8_t>(rgb >> 16u) };
    }

    float squared_distance(const sixel::detail::LabEntries &e, int j, float L, float a, float b) noexcept
    {
        const auto i = static_cast<std::size_t>(j);
        const float dl = e.L[i] - L;
        const float da = e.a[i] - a;
        const float db = e.b[i] - b;
        return (dl * dl) + (da * da) + (db * db);
    }

    // Nearest entry to each cell, written to `assignment` when given; returns the
    // count-weighted sum of distances and the total weight.
    std::pair<double, double> assign_cells(
        const sixel::detail::Cells &cells, const sixel::detail::LabEntries &e, std::vector<uint8_t> *assignment
    )
    {
        double sum = 0.0;
        double weight = 0.0;
        const std::size_t n = cells.size();
        // Blocks of cells keep the running minima in registers while the entry loop vectorizes.
        constexpr std::size_t BLOCK = 64;
        std::array<float, BLOCK> best_d{};
        std::array<int, BLOCK> best_j{};
        for (std::size_t i0 = 0; i0 < n; i0 += BLOCK)
        {
            const std::size_t m = std::min(BLOCK, n - i0);
            best_d.fill(std::numeric_limits<float>::max());
            best_j.fill(0);
            for (int j = 0; j < e.count; j++)
            {
                const auto ju = static_cast<std::size_t>(j);
                const float L = e.L[ju];
                const float a = e.a[ju];
                const float b = e.b[ju];
                for (std::size_t i = 0; i < m; i++)
                {
                    const float dl = L - cells.L[i0 + i];
                    const float da = a - cells.a[i0 + i];
                    const float db = b - cells.b[i0 + i];
                    const float d = (dl * dl) + (da * da) + (db * db);
                    const bool closer = d < best_d[i];
                    best_d[i] = closer ? d : best_d[i];
                    best_j[i] = closer ? j : best_j[i];
                }
            }
            for (std::size_t i = 0; i < m; i++)
            {
                if (assignment != nullptr)
                {
                    (*assignment)[i0 + i] = static_cast<uint8_t>(best_j[i]);
                }
                sum += static_cast<double>(cells.weight[i0 + i]) * static_cast<double>(std::sqrt(best_d[i]));
                weight += static_cast<double>(cells.weight[i0 + i]);
            }
        }
        return { sum, weight };
    }
} // namespace

namespace sixel
{
    namespace detail
    {
        int sample_positions(int width, int height, std::vector<std::size_t> &out)
        {
            if (width <= 0 || height <= 0)
            {
                return 1;
            }
            const auto w = static_cast<uint64_t>(width);
            const auto h = static_cast<uint64_t>(height);
            const auto limit = static_cast<uint64_t>(MAX_SAMPLES);
            // The square root is only a starting point; the grid product is the bound.
            auto s =
                static_cast<uint64_t>(std::ceil(std::sqrt(static_cast<double>(w * h) / static_cast<double>(limit))));
            s = std::max<uint64_t>(s, 1u);
            while (((h + s - 1) / s) * ((w + s - 1) / s) > limit)
            {
                s++;
            }
            const auto stride = static_cast<std::size_t>(s);
            const auto row_px = static_cast<std::size_t>(width);
            const auto rows = static_cast<std::size_t>(height);
            for (std::size_t y0 = 0; y0 < rows; y0 += stride)
            {
                const std::size_t block_h = std::min(stride, rows - y0);
                const uint32_t row_hash = static_cast<uint32_t>(y0 / stride) * 0x9E3779B1u;
                for (std::size_t x0 = 0; x0 < row_px; x0 += stride)
                {
                    const std::size_t block_w = std::min(stride, row_px - x0);
                    // Hash the position within the block (lowbias32), so periodic patterns such as
                    // 1-pixel stripes cannot line up with the sample.
                    uint32_t v = row_hash ^ (static_cast<uint32_t>(x0 / stride) * 0x85EBCA77u);
                    v ^= v >> 16u;
                    v *= 0x7FEB352Du;
                    v ^= v >> 15u;
                    v *= 0x846CA68Bu;
                    v ^= v >> 16u;
                    const std::size_t y = y0 + ((v & 0xFFFFu) % block_h);
                    const std::size_t x = x0 + ((v >> 16u) % block_w);
                    out.push_back((y * row_px) + x);
                }
            }
            return static_cast<int>(s);
        }

        void read_sample(
            const std::atomic<uint64_t> *px, const std::vector<std::size_t> &positions, std::vector<uint32_t> &out
        )
        {
            out.resize(positions.size());
            std::transform(
                positions.begin(), positions.end(), out.begin(), [px](std::size_t i)
                { return static_cast<uint32_t>(px[i].load(std::memory_order_relaxed)) & 0x00FFFFFFu; }
            );
        }

        Histogram::Histogram() : m_count(CELLS), m_sum_r(CELLS), m_sum_g(CELLS), m_sum_b(CELLS), m_sum_sq(CELLS)
        {
            m_occupied.reserve(static_cast<std::size_t>(MAX_SAMPLES));
        }

        void Histogram::clear() noexcept
        {
            for (const uint16_t k : m_occupied)
            {
                m_count[k] = 0;
                m_sum_r[k] = 0;
                m_sum_g[k] = 0;
                m_sum_b[k] = 0;
                m_sum_sq[k] = 0;
            }
            m_occupied.clear();
        }

        void Histogram::add(uint32_t rgb, uint32_t n) noexcept
        {
            const uint32_t r = rgb & 0xFFu;
            const uint32_t g = (rgb >> 8u) & 0xFFu;
            const uint32_t b = (rgb >> 16u) & 0xFFu;
            const auto k = static_cast<uint16_t>(((r >> 3u) << 10u) | ((g >> 3u) << 5u) | (b >> 3u));
            if (m_count[k] == 0)
            {
                m_occupied.push_back(k);
            }
            m_count[k] += n;
            m_sum_r[k] += n * r;
            m_sum_g[k] += n * g;
            m_sum_b[k] += n * b;
            m_sum_sq[k] += static_cast<uint64_t>(n) * ((r * r) + (g * g) + (b * b));
        }

        void Histogram::add_all(const std::vector<uint32_t> &sample) noexcept
        {
            std::size_t i = 0;
            while (i < sample.size())
            {
                const uint32_t c = sample[i];
                std::size_t j = i + 1;
                while (j < sample.size() && sample[j] == c)
                {
                    j++;
                }
                add(c, static_cast<uint32_t>(j - i));
                i = j;
            }
        }

        Wu::Wu()
        {
            const std::size_t n = static_cast<std::size_t>(N) * N * N;
            m_wt.resize(n);
            m_r.resize(n);
            m_g.resize(n);
            m_b.resize(n);
            m_sq.resize(n);
        }

        void Wu::build(const Histogram &hist)
        {
            for (std::vector<int64_t> *m : { &m_wt, &m_r, &m_g, &m_b, &m_sq })
            {
                std::fill(m->begin(), m->end(), int64_t{ 0 });
            }
            for (const uint16_t k : hist.occupied())
            {
                const unsigned cell = k;
                const std::size_t i =
                    at(static_cast<int>(cell >> 10u) + 1, static_cast<int>((cell >> 5u) & 31u) + 1,
                       static_cast<int>(cell & 31u) + 1);
                m_wt[i] = hist.count(k);
                m_r[i] = hist.sum_r(k);
                m_g[i] = hist.sum_g(k);
                m_b[i] = hist.sum_b(k);
                m_sq[i] = static_cast<int64_t>(hist.sum_sq(k));
            }
            // Summing along each axis in turn makes every array a 3D prefix sum.
            for (std::vector<int64_t> *pm : { &m_wt, &m_r, &m_g, &m_b, &m_sq })
            {
                std::vector<int64_t> &m = *pm;
                for (int r = 1; r < N; r++)
                {
                    for (int g = 1; g < N; g++)
                    {
                        for (int b = 2; b < N; b++)
                        {
                            m[at(r, g, b)] += m[at(r, g, b - 1)];
                        }
                    }
                }
                for (int r = 1; r < N; r++)
                {
                    for (int g = 2; g < N; g++)
                    {
                        for (int b = 1; b < N; b++)
                        {
                            m[at(r, g, b)] += m[at(r, g - 1, b)];
                        }
                    }
                }
                for (int r = 2; r < N; r++)
                {
                    for (int g = 1; g < N; g++)
                    {
                        for (int b = 1; b < N; b++)
                        {
                            m[at(r, g, b)] += m[at(r - 1, g, b)];
                        }
                    }
                }
            }
            m_boxes.assign(1, Box{ 0, N - 1, 0, N - 1, 0, N - 1 });
            m_var.assign(1, variance(m_boxes[0]));
        }

        int64_t Wu::volume(const std::vector<int64_t> &m, const Box &x) noexcept
        {
            return m[at(x.r1, x.g1, x.b1)] - m[at(x.r1, x.g1, x.b0)] - m[at(x.r1, x.g0, x.b1)] +
                   m[at(x.r1, x.g0, x.b0)] - m[at(x.r0, x.g1, x.b1)] + m[at(x.r0, x.g1, x.b0)] +
                   m[at(x.r0, x.g0, x.b1)] - m[at(x.r0, x.g0, x.b0)];
        }

        double Wu::variance(const Box &x) const noexcept
        {
            const auto w = static_cast<double>(volume(m_wt, x));
            if (w <= 0.0)
            {
                return 0.0;
            }
            const auto r = static_cast<double>(volume(m_r, x));
            const auto g = static_cast<double>(volume(m_g, x));
            const auto b = static_cast<double>(volume(m_b, x));
            return static_cast<double>(volume(m_sq, x)) - (((r * r) + (g * g) + (b * b)) / w);
        }

        void Wu::split_to(int count)
        {
            while (static_cast<int>(m_boxes.size()) < count)
            {
                const auto i = static_cast<std::size_t>(std::max_element(m_var.begin(), m_var.end()) - m_var.begin());
                if (m_var[i] <= 0.0)
                {
                    return;
                }
                const Box box = m_boxes[i];
                const auto total_w = static_cast<double>(volume(m_wt, box));
                const auto total_r = static_cast<double>(volume(m_r, box));
                const auto total_g = static_cast<double>(volume(m_g, box));
                const auto total_b = static_cast<double>(volume(m_b, box));
                double best = -1.0;
                int best_axis = -1;
                int best_cut = 0;
                for (int axis = 0; axis < 3; axis++)
                {
                    const int lo = axis == 0 ? box.r0 : (axis == 1 ? box.g0 : box.b0);
                    const int hi = axis == 0 ? box.r1 : (axis == 1 ? box.g1 : box.b1);
                    for (int cut = lo + 1; cut < hi; cut++)
                    {
                        Box half = box;
                        (axis == 0 ? half.r1 : (axis == 1 ? half.g1 : half.b1)) = cut;
                        const auto w = static_cast<double>(volume(m_wt, half));
                        if (w <= 0.0 || w >= total_w)
                        {
                            continue;
                        }
                        const auto r = static_cast<double>(volume(m_r, half));
                        const auto g = static_cast<double>(volume(m_g, half));
                        const auto b = static_cast<double>(volume(m_b, half));
                        const double rest_r = total_r - r;
                        const double rest_g = total_g - g;
                        const double rest_b = total_b - b;
                        const double score =
                            (((r * r) + (g * g) + (b * b)) / w) +
                            (((rest_r * rest_r) + (rest_g * rest_g) + (rest_b * rest_b)) / (total_w - w));
                        if (score > best)
                        {
                            best = score;
                            best_axis = axis;
                            best_cut = cut;
                        }
                    }
                }
                if (best_axis < 0)
                {
                    // All of the box's weight sits in one cell: it cannot be split.
                    m_var[i] = 0.0;
                    continue;
                }
                Box low = box;
                Box high = box;
                (best_axis == 0 ? low.r1 : (best_axis == 1 ? low.g1 : low.b1)) = best_cut;
                (best_axis == 0 ? high.r0 : (best_axis == 1 ? high.g0 : high.b0)) = best_cut;
                m_boxes[i] = low;
                m_var[i] = variance(low);
                m_boxes.push_back(high);
                m_var.push_back(variance(high));
            }
        }

        void Wu::box_means(std::vector<std::array<float, 3>> &out) const
        {
            out.clear();
            for (const Box &box : m_boxes)
            {
                const auto w = static_cast<double>(volume(m_wt, box));
                if (w > 0.0)
                {
                    out.push_back({ static_cast<float>(static_cast<double>(volume(m_r, box)) / w),
                                    static_cast<float>(static_cast<double>(volume(m_g, box)) / w),
                                    static_cast<float>(static_cast<double>(volume(m_b, box)) / w) });
                }
            }
        }

        void Cells::assign(const Histogram &hist)
        {
            const std::size_t n = hist.occupied().size();
            L.resize(n);
            a.resize(n);
            b.resize(n);
            weight.resize(n);
            for (std::size_t i = 0; i < n; i++)
            {
                const uint16_t k = hist.occupied()[i];
                const auto count = static_cast<float>(hist.count(k));
                const Lab lab = cielab_from_linear(
                    srgb_to_linear(static_cast<float>(hist.sum_r(k)) / count / 255.0f),
                    srgb_to_linear(static_cast<float>(hist.sum_g(k)) / count / 255.0f),
                    srgb_to_linear(static_cast<float>(hist.sum_b(k)) / count / 255.0f)
                );
                L[i] = lab.L;
                a[i] = lab.a;
                b[i] = lab.b;
                weight[i] = count;
            }
        }

        void LabEntries::set(const std::vector<Color> &entries)
        {
            count = static_cast<int>(entries.size());
            for (std::size_t j = 0; j < entries.size(); j++)
            {
                const Lab lab = cielab_from_srgb8(entries[j]);
                L[j] = lab.L;
                a[j] = lab.a;
                b[j] = lab.b;
            }
        }

        double mean_error(const Cells &cells, const LabEntries &entries)
        {
            const auto [sum, weight] = assign_cells(cells, entries, nullptr);
            return weight > 0.0 ? sum / weight : 0.0;
        }

        void lloyd(const Cells &cells, LabEntries &centroids, int passes)
        {
            const std::size_t n = cells.size();
            const auto k = static_cast<std::size_t>(centroids.count);
            std::vector<uint8_t> assignment(n);
            std::vector<uint8_t> previous;
            std::array<double, 256> sum_l{};
            std::array<double, 256> sum_a{};
            std::array<double, 256> sum_b{};
            std::array<double, 256> sum_w{};
            for (int pass = 0; pass < passes; pass++)
            {
                (void)assign_cells(cells, centroids, &assignment);
                if (pass > 0 && assignment == previous)
                {
                    return;
                }
                std::fill(sum_l.begin(), sum_l.begin() + static_cast<std::ptrdiff_t>(k), 0.0);
                std::fill(sum_a.begin(), sum_a.begin() + static_cast<std::ptrdiff_t>(k), 0.0);
                std::fill(sum_b.begin(), sum_b.begin() + static_cast<std::ptrdiff_t>(k), 0.0);
                std::fill(sum_w.begin(), sum_w.begin() + static_cast<std::ptrdiff_t>(k), 0.0);
                for (std::size_t i = 0; i < n; i++)
                {
                    const std::size_t j = assignment[i];
                    const auto w = static_cast<double>(cells.weight[i]);
                    sum_l[j] += w * static_cast<double>(cells.L[i]);
                    sum_a[j] += w * static_cast<double>(cells.a[i]);
                    sum_b[j] += w * static_cast<double>(cells.b[i]);
                    sum_w[j] += w;
                }
                for (std::size_t j = 0; j < k; j++)
                {
                    if (sum_w[j] > 0.0)
                    {
                        centroids.L[j] = static_cast<float>(sum_l[j] / sum_w[j]);
                        centroids.a[j] = static_cast<float>(sum_a[j] / sum_w[j]);
                        centroids.b[j] = static_cast<float>(sum_b[j] / sum_w[j]);
                    }
                }
                previous.swap(assignment);
                assignment.resize(n);
            }
        }

        std::vector<int> place_pins(std::vector<Color> &entries, const std::vector<Color> &pins, int budget)
        {
            std::vector<int> pin_entries;
            pin_entries.reserve(pins.size());
            for (const Color pin : pins)
            {
                const auto same = std::find(entries.begin(), entries.end(), pin);
                if (same != entries.end())
                {
                    pin_entries.push_back(static_cast<int>(same - entries.begin()));
                    continue;
                }
                if (static_cast<int>(entries.size()) < budget)
                {
                    entries.push_back(pin);
                    pin_entries.push_back(static_cast<int>(entries.size()) - 1);
                    continue;
                }
                const Lab target = cielab_from_srgb8(pin);
                int nearest = -1;
                float nearest_d = std::numeric_limits<float>::max();
                for (std::size_t j = 0; j < entries.size(); j++)
                {
                    if (std::find(pin_entries.begin(), pin_entries.end(), static_cast<int>(j)) != pin_entries.end())
                    {
                        continue;
                    }
                    const Lab lab = cielab_from_srgb8(entries[j]);
                    const float dl = lab.L - target.L;
                    const float da = lab.a - target.a;
                    const float db = lab.b - target.b;
                    const float d = (dl * dl) + (da * da) + (db * db);
                    if (d < nearest_d)
                    {
                        nearest_d = d;
                        nearest = static_cast<int>(j);
                    }
                }
                // The caller passes at most `budget` pins, so an unheld entry always remains.
                entries[static_cast<std::size_t>(nearest)] = pin;
                pin_entries.push_back(nearest);
            }
            return pin_entries;
        }

        uint32_t table_entry(Lab cell, const LabEntries &entries) noexcept
        {
            std::array<float, 256> d{};
            int best = 0;
            float best_d = std::numeric_limits<float>::max();
            for (int j = 0; j < entries.count; j++)
            {
                d[static_cast<std::size_t>(j)] = squared_distance(entries, j, cell.L, cell.a, cell.b);
            }
            for (int j = 0; j < entries.count; j++)
            {
                if (d[static_cast<std::size_t>(j)] < best_d)
                {
                    best_d = d[static_cast<std::size_t>(j)];
                    best = j;
                }
            }
            const float reach = std::sqrt(best_d) + 1.0f;
            const float limit = reach * reach;
            std::array<int, 3> alt = { best, best, best };
            std::array<float, 3> alt_d = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                                           std::numeric_limits<float>::max() };
            for (int j = 0; j < entries.count; j++)
            {
                const float dj = d[static_cast<std::size_t>(j)];
                if (j == best || dj > limit)
                {
                    continue;
                }
                for (std::size_t s = 0; s < 3; s++)
                {
                    if (dj < alt_d[s])
                    {
                        for (std::size_t t = 2; t > s; t--)
                        {
                            alt[t] = alt[t - 1];
                            alt_d[t] = alt_d[t - 1];
                        }
                        alt[s] = j;
                        alt_d[s] = dj;
                        break;
                    }
                }
            }
            return static_cast<uint32_t>(best) | (static_cast<uint32_t>(alt[0]) << 8u) |
                   (static_cast<uint32_t>(alt[1]) << 16u) | (static_cast<uint32_t>(alt[2]) << 24u);
        }

        Lab cell_centre_lab(std::size_t cell) noexcept
        {
            // Linear-light value at the centre of each 6-bit level, so a fill costs three cbrt calls.
            static const std::array<float, 64> centre_lin = []
            {
                std::array<float, 64> lin{};
                for (std::size_t i = 0; i < lin.size(); i++)
                {
                    lin[i] = srgb_to_linear(((4.0f * static_cast<float>(i)) + 1.5f) / 255.0f);
                }
                return lin;
            }();
            return cielab_from_linear(
                centre_lin[(cell >> 12u) & 63u], centre_lin[(cell >> 6u) & 63u], centre_lin[cell & 63u]
            );
        }

        Color percent_round_trip(Color c) noexcept
        {
            return { channel_from_pct(channel_pct(c.r)), channel_from_pct(channel_pct(c.g)),
                     channel_from_pct(channel_pct(c.b)) };
        }
    } // namespace detail

    FittedPalette::FittedPalette(int colors)
        : m_budget(std::clamp(colors, 1, MAX_REGISTERS)),
          m_table(std::make_unique<std::atomic<uint32_t>[]>(TABLE_CELLS))
    {
        m_sample.reserve(static_cast<std::size_t>(detail::MAX_SAMPLES));
        for (std::size_t i = 0; i < TABLE_CELLS; i++)
        {
            m_table[i].store(detail::EMPTY_ENTRY, std::memory_order_relaxed);
        }
    }

    FittedPalette::~FittedPalette() = default;

    void FittedPalette::collect_pins(uint32_t clear_rgb, std::optional<uint32_t> required)
    {
        m_prev_pins.swap(m_pins);
        m_pins.clear();
        const auto add = [this](Color c)
        {
            if (std::find(m_pins.begin(), m_pins.end(), c) == m_pins.end())
            {
                m_pins.push_back(c);
            }
        };
        if (required)
        {
            add(unpack(*required & 0x00FFFFFFu));
        }
        // Count after `required` so a colour pinned for both reasons keeps the higher priority.
        const auto covers = [this](uint32_t rgb)
        {
            const auto n = static_cast<std::size_t>(std::count(m_sample.begin(), m_sample.end(), rgb));
            return n * 100u >= m_sample.size() * PIN_COVERAGE_PCT;
        };
        clear_rgb &= 0x00FFFFFFu;
        if (covers(clear_rgb))
        {
            add(unpack(clear_rgb));
        }
        if (covers(0u))
        {
            add(Color{ 0, 0, 0 });
        }
        if (static_cast<int>(m_pins.size()) > m_budget)
        {
            m_pins.resize(static_cast<std::size_t>(m_budget));
        }
    }

    void FittedPalette::fit(int budget, Fit &out)
    {
        m_wu.split_to(budget);
        m_wu.box_means(m_means);
        std::vector<Color> &palette = out.entries;
        palette.clear();
        detail::LabEntries &lab = out.lab;
        lab.count = static_cast<int>(m_means.size());
        for (std::size_t j = 0; j < m_means.size(); j++)
        {
            const Lab c = cielab_from_linear(
                srgb_to_linear(m_means[j][0] / 255.0f), srgb_to_linear(m_means[j][1] / 255.0f),
                srgb_to_linear(m_means[j][2] / 255.0f)
            );
            lab.L[j] = c.L;
            lab.a[j] = c.a;
            lab.b[j] = c.b;
        }
        detail::lloyd(m_cells, lab, LLOYD_PASSES);
        for (int j = 0; j < lab.count; j++)
        {
            const auto i = static_cast<std::size_t>(j);
            // Round now so the table and the error measure use what the terminal shows.
            const Color c = detail::percent_round_trip(srgb8_from_cielab({ lab.L[i], lab.a[i], lab.b[i] }));
            if (std::find(palette.begin(), palette.end(), c) == palette.end())
            {
                palette.push_back(c);
            }
        }
        std::vector<Color> rounded_pins(m_pins.size());
        std::transform(m_pins.begin(), m_pins.end(), rounded_pins.begin(), detail::percent_round_trip);
        // The table pins cells from the current pin set each frame, so the indices are not kept.
        (void)detail::place_pins(palette, rounded_pins, budget);
        lab.set(palette);
        out.error = detail::mean_error(m_cells, lab);
    }

    bool FittedPalette::has_exact_entry(Color pin) const noexcept
    {
        return std::find(m_entries.begin(), m_entries.end(), detail::percent_round_trip(pin)) != m_entries.end();
    }

    void FittedPalette::install(Fit &fit)
    {
        m_entries.swap(fit.entries);
        m_lab = fit.lab;
        // Every register is redefined each frame because the palette is shared terminal state.
        m_block.clear();
        for (std::size_t j = 0; j < m_entries.size(); j++)
        {
            append_register(m_block, static_cast<int>(j), m_entries[j]);
        }
        for (std::size_t i = 0; i < TABLE_CELLS; i++)
        {
            m_table[i].store(detail::EMPTY_ENTRY, std::memory_order_relaxed);
        }
        m_pinned_cells.clear();
        m_generation++;
    }

    void FittedPalette::pin_table_cells()
    {
        for (const std::size_t cell : m_pinned_cells)
        {
            m_table[cell].store(detail::EMPTY_ENTRY, std::memory_order_relaxed);
        }
        m_pinned_cells.clear();
        // Lowest priority first, so an earlier pin sharing a 6-bit cell overwrites a later one.
        for (auto it = m_pins.rbegin(); it != m_pins.rend(); ++it)
        {
            const auto entry = std::find(m_entries.begin(), m_entries.end(), detail::percent_round_trip(*it));
            if (entry == m_entries.end())
            {
                continue;
            }
            const std::size_t cell = quant256_idx(*it);
            m_table[cell].store(
                detail::pinned_entry(static_cast<int>(entry - m_entries.begin())), std::memory_order_relaxed
            );
            m_pinned_cells.push_back(cell);
        }
    }

    bool FittedPalette::update(
        const std::atomic<uint64_t> *px, int width, int height, uint32_t clear_rgb, std::optional<uint32_t> required
    )
    {
        if (width <= 0 || height <= 0)
        {
            return false;
        }
        if (width != m_positions_width || height != m_positions_height)
        {
            m_positions.clear();
            (void)detail::sample_positions(width, height, m_positions);
            m_positions_width = width;
            m_positions_height = height;
        }
        detail::read_sample(px, m_positions, m_sample);
        m_hist.clear();
        m_hist.add_all(m_sample);
        m_cells.assign(m_hist);
        m_wu.build(m_hist);
        collect_pins(clear_rgb, required);

        fit(m_budget, m_fresh);
        bool refit = m_entries.empty();
        if (!refit)
        {
            const double current = detail::mean_error(m_cells, m_lab);
            const double fresh = m_fresh.error;
            refit = current > std::max(KEEP_RATIO * fresh, fresh + KEEP_SLACK);
        }
        if (!refit)
        {
            // A new pin without an exact entry refits now, so a background or wireframe colour
            // change is exact on its first frame. A returning pin whose entry survived changes
            // nothing, so coverage hovering at the threshold cannot refit every frame.
            for (const Color pin : m_pins)
            {
                const bool is_new = std::find(m_prev_pins.begin(), m_prev_pins.end(), pin) == m_prev_pins.end();
                if (is_new && !has_exact_entry(pin))
                {
                    refit = true;
                    break;
                }
            }
        }
        if (refit)
        {
            install(m_fresh);
        }
        pin_table_cells();
        return refit;
    }

    uint32_t FittedPalette::fill(std::size_t cell) const noexcept
    {
        const uint32_t value = detail::table_entry(detail::cell_centre_lab(cell), m_lab);
        // Racing fills of one cell compute and store the same value.
        m_table[cell].store(value, std::memory_order_relaxed);
        return value;
    }

    void FittedPalette::map_rows(const std::atomic<uint64_t> *px, int width, int y0, int y1, unsigned char *out)
        const noexcept
    {
        const auto w = static_cast<std::size_t>(width);
        const std::atomic<uint32_t> *table = m_table.get();
        for (int y = y0; y < y1; y++)
        {
            const std::atomic<uint64_t> *row = px + (static_cast<std::size_t>(y) * w);
            unsigned char *dst = out + (static_cast<std::size_t>(y) * w);
            // No register matches 256, so a row's first pixel takes its cell's nearest entry.
            uint32_t prev = 256u;
            for (std::size_t x = 0; x < w; x++)
            {
                const std::size_t cell =
                    quant256_idx_packed(static_cast<uint32_t>(row[x].load(std::memory_order_relaxed)));
                uint32_t e = table[cell].load(std::memory_order_relaxed);
                if (e == detail::EMPTY_ENTRY)
                {
                    e = fill(cell);
                }
                // Keep the left neighbour's register when it is one of the cell's alternates:
                // longer runs encode shorter at no visible cost.
                const bool keep = prev == ((e >> 8u) & 0xFFu) || prev == ((e >> 16u) & 0xFFu) || prev == (e >> 24u);
                prev = keep ? prev : (e & 0xFFu);
                dst[x] = static_cast<unsigned char>(prev);
            }
        }
    }
} // namespace sixel
