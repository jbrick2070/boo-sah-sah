#pragma once

// RC1's fixed, sample-clock measurement contract. This model contains no machine
// identifiers, free-form diagnostics, rendering, file access, or export code.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace boo {

inline constexpr std::size_t kResultsWarmupCount = 30;
inline constexpr std::size_t kResultsMeasuredCount = 180;
inline constexpr std::size_t kResultsSampleCount = kResultsWarmupCount + kResultsMeasuredCount;

enum class ResultsSampleKind : uint8_t { Warmup, Measured };
struct ResultsSample {
    uint32_t tag;          // Unique within the fixed RC1 sequence, 0 through 209.
    int64_t sample48k;     // Film sample position, not a wall-clock timestamp.
    ResultsSampleKind kind;
};

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
