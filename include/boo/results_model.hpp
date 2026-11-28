#pragma once

// RC1's fixed, sample-clock measurement contract. This model contains no machine
// identifiers, free-form diagnostics, rendering, file access, or export code.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace boo {

struct RenderTile {
    uint32_t x, y, width, height;
};

// Sort by distance from the centre, with row/column as deterministic tie-breakers.
// Edge tiles are clipped so the returned rectangles partition the entire image.
inline std::vector<RenderTile> renderTileOrder(uint32_t width, uint32_t height, uint32_t tileSize) {
    std::vector<RenderTile> tiles;
    if (!width || !height || !tileSize) return tiles;
    for (uint64_t y = 0; y < height; y += tileSize) {
        for (uint64_t x = 0; x < width; x += tileSize) {
            tiles.push_back({static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                static_cast<uint32_t>((std::min)(uint64_t(tileSize), uint64_t(width) - x)),
                static_cast<uint32_t>((std::min)(uint64_t(tileSize), uint64_t(height) - y))});
        }
    }
    const auto distance = [=](const RenderTile& t) {
        const double dx = 2.0 * t.x + t.width - width;
        const double dy = 2.0 * t.y + t.height - height;
        return dx * dx + dy * dy;
    };
    std::sort(tiles.begin(), tiles.end(), [&](const RenderTile& a, const RenderTile& b) {
        const auto da = distance(a), db = distance(b);
        if (da != db) return da < db;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    return tiles;
}

inline std::optional<double> renderMegaSamplesPerSecond(
    uint32_t width, uint32_t height, uint32_t samplesPerPixel, double seconds) noexcept {
    if (!width || !height || !samplesPerPixel || !std::isfinite(seconds) || seconds <= 0.0)
        return std::nullopt;
    const double result = static_cast<double>(width) * height * samplesPerPixel / seconds / 1000000.0;
    return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
}

inline constexpr std::size_t kResultsWarmupCount = 30;
inline constexpr std::size_t kResultsMeasuredCount = 180;
inline constexpr std::size_t kResultsSampleCount = kResultsWarmupCount + kResultsMeasuredCount;
inline constexpr std::size_t kFilmTourSceneCount = 6;
inline constexpr uint32_t kFilmTourWarmupCount = 15;
inline constexpr uint32_t kFilmTourMeasuredCount = 90;
inline constexpr uint32_t kFilmTourFramesPerScene = kFilmTourWarmupCount + kFilmTourMeasuredCount;
inline constexpr std::array<double, kFilmTourSceneCount> kFilmTourTimesSeconds{3.5, 47.0, 225.0, 86.0, 298.0, 343.5};

enum class ResultsSampleKind : uint8_t { Warmup, Measured };
struct ResultsSample {
    uint32_t tag;          // Unique within the fixed RC1 sequence, 0 through 209.
    int64_t sample48k;     // Film sample position, not a wall-clock timestamp.
    ResultsSampleKind kind;
};

// Reuse the fixed-shot timestamp slots; film time stays fixed for the entire scene.
constexpr std::optional<ResultsSample> filmTourSampleAt(std::size_t scene, uint32_t index) noexcept {
    if (scene >= kFilmTourSceneCount || index >= kFilmTourFramesPerScene) return std::nullopt;
    const bool warm = index < kFilmTourWarmupCount;
    return ResultsSample{warm ? index : static_cast<uint32_t>(kResultsWarmupCount) + index - kFilmTourWarmupCount,
        static_cast<int64_t>(kFilmTourTimesSeconds[scene] * 48000.0),
        warm ? ResultsSampleKind::Warmup : ResultsSampleKind::Measured};
}

inline std::optional<double> filmTourGeometricMean(const std::array<double, kFilmTourSceneCount>& medians) noexcept {
    double sum = 0;
    for (const double value : medians) {
        if (!std::isfinite(value) || value <= 0) return std::nullopt;
        sum += std::log(value) / static_cast<double>(kFilmTourSceneCount);
    }
    const double score = std::exp(sum);
    return std::isfinite(score) && score > 0 ? std::optional<double>{score} : std::nullopt;
}

constexpr std::optional<ResultsSample> resultsSampleAt(std::size_t index) noexcept {
    if (index >= kResultsSampleCount) return std::nullopt;
    if (index < kResultsWarmupCount)
        return ResultsSample{static_cast<uint32_t>(index), 336000 + 1600 * static_cast<int64_t>(index), ResultsSampleKind::Warmup};
    const auto measuredIndex = index - kResultsWarmupCount;
    return ResultsSample{static_cast<uint32_t>(index), 384000 + 1600 * static_cast<int64_t>(measuredIndex), ResultsSampleKind::Measured};
}

enum class ResultsOutcome : uint8_t { Completed, Interrupted, Cancelled, Unavailable };
enum class ResultsInvalidation : uint8_t { MissingQuery, CanvasChanged, EmergencyImage };
struct ResultsSummary {
    double medianMs;
    double worstMs;
};
struct ResultsRun {
    ResultsOutcome outcome;
    std::optional<ResultsSummary> summary;
};

class ResultsMeasurements {
public:
    // Each measured tag may be recorded exactly once. Zero is a valid time.
    // A bad, duplicate, or warmup tag and an invalid value invalidate the set.
    bool record(uint32_t tag, double milliseconds) noexcept {
        if (finished_) return false;
        if (tag < kResultsWarmupCount || tag >= kResultsSampleCount ||
            !std::isfinite(milliseconds) || milliseconds < 0.0) {
            valid_ = false;
            return false;
        }
        const auto index = static_cast<std::size_t>(tag) - kResultsWarmupCount;
        if (present_[index]) { valid_ = false; return false; }
        values_[index] = milliseconds;
        present_[index] = true;
        ++count_;
        return true;
    }

    void invalidate(ResultsInvalidation reason) noexcept {
        if (finished_) return;
        valid_ = false;
        if (reason == ResultsInvalidation::CanvasChanged || reason == ResultsInvalidation::EmergencyImage)
            interrupted_ = true;
    }

    std::size_t validSampleCount() const noexcept { return count_; }

    // A changed canvas or emergency image interrupts the run. Missing, invalid,
    // or incomplete measurements are unavailable, never a partial success.
    ResultsRun finish(ResultsOutcome requested) noexcept {
        if (finished_) return result_;
        finished_ = true;
        result_ = {requested, std::nullopt};
        if (requested != ResultsOutcome::Completed) return result_;
        if (!valid_ || count_ != kResultsMeasuredCount) {
            result_.outcome = interrupted_ ? ResultsOutcome::Interrupted : ResultsOutcome::Unavailable;
            return result_;
        }
        auto sorted = values_;
        std::sort(sorted.begin(), sorted.end());
        // Split the addition to avoid overflow when both finite inputs are large.
        result_.summary = ResultsSummary{sorted[89] * 0.5 + sorted[90] * 0.5, sorted.back()};
        return result_;
    }

private:
    std::array<double, kResultsMeasuredCount> values_{};
    std::array<bool, kResultsMeasuredCount> present_{};
    std::size_t count_ = 0;
    bool valid_ = true;
    bool interrupted_ = false;
    bool finished_ = false;
    ResultsRun result_{ResultsOutcome::Unavailable, std::nullopt};
};

} // namespace boo
