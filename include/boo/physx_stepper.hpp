#pragma once

// Fixed-step timing for the PhysX suite. Physics advances in equal steps whatever the frame rate:
// frame time goes into an accumulator, whole steps come out, at most a few a frame, and anything
// beyond that is dropped so a long stall cannot snowball. Pure arithmetic, testable alone.

#include <algorithm>
#include <cstdint>

namespace boo {

class PhysxStepper {
public:
    explicit PhysxStepper(double stepSeconds = 1.0 / 120.0, uint32_t maxStepsPerFrame = 4u) noexcept
        : step_(stepSeconds > 0.0 ? stepSeconds : 1.0 / 120.0), maxSteps_(std::max(1u, maxStepsPerFrame)) {}

    // How many steps to take for a frame that lasted frameSeconds. Paused frames take none and keep
    // nothing; a reset (a cut to another place) clears what was owed.
    uint32_t advance(double frameSeconds, bool paused, bool reset) noexcept {
        if (reset) { owed_ = 0.0; ++resets_; }
        if (paused || !(frameSeconds > 0.0)) return 0u;
        owed_ += std::min(frameSeconds, 0.25);
        auto steps = static_cast<uint32_t>(owed_ / step_);
        if (steps > maxSteps_) {
            dropped_ += owed_ - double(maxSteps_) * step_;
            steps = maxSteps_;
            owed_ = 0.0;
        } else {
            owed_ -= double(steps) * step_;
        }
        total_ += steps;
        return steps;
    }

    double stepSeconds() const noexcept { return step_; }
    double owedSeconds() const noexcept { return owed_; }
    double droppedSeconds() const noexcept { return dropped_; } // simulated time given up to stalls
    uint64_t totalSteps() const noexcept { return total_; }
    uint64_t resets() const noexcept { return resets_; }

private:
    double step_;
    uint32_t maxSteps_;
    double owed_ = 0.0;
    double dropped_ = 0.0;
    uint64_t total_ = 0;
    uint64_t resets_ = 0;
};

} // namespace boo
