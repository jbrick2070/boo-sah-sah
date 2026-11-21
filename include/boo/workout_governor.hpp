#pragma once

// The workout dial's frame-rate floor.
//
// The dial asks for a number of birds. They are released in batches. After each batch the governor
// lets the frame rate settle, measures it, and only then decides on the next. As it goes it learns
// what a bird costs on this machine, so it never releases a batch it expects to take the frame rate
// under the floor, and it sheds birds at once if the frame rate falls there anyway (a heavier shot,
// another program taking the GPU). The default floor is film speed: 24 frames a second. To stay
// ahead of it the governor aims a sixth higher, 28 frames a second, and holds there.
//
// Pure arithmetic on the numbers it is given, so it can be tested against a simulated machine.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace boo {

struct WorkoutGovernorInput {
    double frameSeconds = 0.0; // wall-clock time from the previous rendered frame to this one
    double gpuSeconds = 0.0;   // GPU time of the most recently measured frame; 0 when unknown
    uint32_t requested = 0;    // birds the dial asks for
    uint32_t limit = 0;        // most birds the renderer can hold
    float floorFps = 24.f;     // 0 disables the floor: the request is released as asked
    bool flockVisible = true;  // false while the flock is not being drawn at all
};

class WorkoutGovernor {
public:
    void update(const WorkoutGovernorInput& in) noexcept {
        const double dt = std::clamp(in.frameSeconds, 0.0, 0.25);
        const uint32_t want = std::min(in.requested, in.limit);
        if (want == 0u) { reset(); return; }  // dial at zero: the authored film
        if (target_ > want) { retarget(want); atFloor_ = false; } // the dial came down: let them go at once
        const bool governed = in.floorFps > 0.f;
        if (!governed) {
            atFloor_ = false;
            if (target_ < want) release(want);
        }

        // Sweep the arrival front through the newest batch.
        if (arriving()) {
            front_ = std::min(front_ + rate_ * dt, double(target_) + band_);
            if (!arriving()) { grown_ = target_; beginSettling(); }
        }
        if (!governed) return;

        cooldown_ = std::max(0.0, cooldown_ - dt);
        if (ceiling_ > 0.0 && (ceilingAge_ += dt) > kCeilingSeconds) ceiling_ = 0.0;
        if (!in.flockVisible || in.frameSeconds <= 0.0) { clearWindow(); recentCount_ = 0u; return; }
        if (graceFrames_ > 0u || graceSeconds_ > 0.0) {
            if (graceFrames_ > 0u) --graceFrames_;
            graceSeconds_ = std::max(0.0, graceSeconds_ - in.frameSeconds);
            clearWindow();
            recentCount_ = 0u;
            return;
        }

        // A sudden overload must not wait for a full measurement.
        recent_[recentAt_] = in.frameSeconds;
        recentAt_ = (recentAt_ + 1u) % kRecent;
        recentCount_ = std::min(recentCount_ + 1u, kRecent);
        if (target_ > 0u && recentCount_ == kRecent) {
            auto sorted = recent_;
            std::sort(sorted.begin(), sorted.end());
            const double typical = sorted[kRecent / 2u];
            if (typical > 1.0 / (double(in.floorFps) * kPanicBelow)) {
                fps_ = static_cast<float>(1.0 / typical);
                shed(typical, 1.0 / (double(in.floorFps) * kAim));
                return;
            }
        }
        if (arriving()) return; // the batch is still coming in
        if (settleSeconds_ > 0.0 || settleFrames_ > 0u) {
            settleSeconds_ = std::max(0.0, settleSeconds_ - in.frameSeconds);
            if (settleFrames_ > 0u) --settleFrames_;
            return;
        }

        frames_[windowCount_] = in.frameSeconds;
        gpu_[windowCount_] = in.gpuSeconds;
        ++windowCount_;
        windowSeconds_ += in.frameSeconds;
        if (windowCount_ < kWindow && (windowSeconds_ < kMeasureSeconds || windowCount_ < kMeasureFrames)) return;
        const double frame = typical(frames_, windowCount_);
        const double gpu = typical(gpu_, windowCount_);
        clearWindow();
        decide(frame, gpu, want, in.floorFps);
    }

    // After a pause of the app's own making (buffers reallocated): the frames around it say
    // nothing about the flock. On real hardware they stay slow for about a second afterwards,
    // while the new memory is touched for the first time and frame pacing recovers.
    void restartMeasurement() noexcept {
        clearWindow();
        recentCount_ = 0u;
        beginSettling();
        graceFrames_ = kGraceFrames;
        graceSeconds_ = kGraceSeconds;
    }

    void reset() noexcept {
        const uint32_t peak = peak_, steadyBirds = steadyBirds_;
        const float steadyFps = steadyFps_;
        *this = WorkoutGovernor{};
        peak_ = peak; steadyBirds_ = steadyBirds; steadyFps_ = steadyFps;
    }

    // Birds to draw this frame, and how the newest of them are arriving (see workoutBirdArrival).
    uint32_t birds() const noexcept {
        const double shown = std::min(double(target_), std::ceil(front_));
        return std::max(grown_, static_cast<uint32_t>(std::max(shown, 0.0)));
    }
    uint32_t grown() const noexcept { return grown_; }
    float front() const noexcept { return static_cast<float>(front_); }
    float band() const noexcept { return static_cast<float>(band_); }
    // birds() as a number that changes smoothly while a batch arrives.
    float flockSize() const noexcept {
        return static_cast<float>(std::clamp(front_, double(grown_), double(std::max(target_, grown_))));
    }

    uint32_t released() const noexcept { return target_; }
    bool atFloor() const noexcept { return atFloor_; }   // the floor, not the dial, is holding the flock back
    bool releasing() const noexcept { return arriving(); }
    float measuredFps() const noexcept { return fps_; }  // frame rate at the last decision
    // The last flock that was measured holding the floor, and the largest so far: a score for the machine.
    uint32_t steadyBirds() const noexcept { return steadyBirds_; }
    float steadyFps() const noexcept { return steadyFps_; }
    uint32_t peakBirds() const noexcept { return peak_; }
    double secondsPerBird() const noexcept { return std::max(slopeFrame_, slopeGpu_); }

private:
    static constexpr uint32_t kWindow = 64u;          // most frames in one measurement
    static constexpr uint32_t kRecent = 5u;           // frames watched for a sudden overload
    static constexpr uint32_t kMeasureFrames = 8u;
    static constexpr uint32_t kSettleFrames = 4u;
    static constexpr uint32_t kGraceFrames = 10u;
    static constexpr double kGraceSeconds = 1.5;
    static constexpr double kMeasureSeconds = 0.40;
    static constexpr double kSettleSeconds = 0.20;
    static constexpr double kRampSeconds = 0.45;      // time for one batch to arrive
    static constexpr double kAim = 28.0 / 24.0;       // aim well clear of the floor: 28 fps for a floor of 24
    static constexpr double kShedBelow = 1.06;        // measured this close to the floor: shed back to the aim
    static constexpr double kPanicBelow = 1.0;        // several frames under the floor itself: shed now
    static constexpr double kHoldBand = 0.025;        // less headroom than this share of a frame: hold
    static constexpr double kStepShare = 0.7;         // of the measured headroom spent by one batch
    static constexpr double kProportionalShare = 0.9;
    static constexpr double kFarBelow = 0.35;         // load under this share of the limit: bigger batches
    static constexpr double kFirstBatch = 100.0;
    static constexpr double kShedExtra = 1.15;
    static constexpr double kCooldownSeconds = 1.5;   // after shedding, before growing again
    static constexpr double kCeilingSeconds = 15.0;   // how long a flock that broke the floor is remembered
    static constexpr double kCeilingShare = 0.97;

    bool arriving() const noexcept { return grown_ < target_ && front_ < double(target_) + band_; }

    void clearWindow() noexcept { windowCount_ = 0u; windowSeconds_ = 0.0; }
    void beginSettling() noexcept { settleSeconds_ = kSettleSeconds; settleFrames_ = kSettleFrames; clearWindow(); }

    // The upper quartile, not the middle: the floor is for the slower frames too.
    static double typical(std::array<double, kWindow> values, uint32_t count) noexcept {
        std::sort(values.begin(), values.begin() + count);
        return values[(count * 3u) / 4u];
    }

    void release(uint32_t birds) noexcept {
        if (birds <= target_) return;
        if (!arriving()) { grown_ = target_; front_ = double(target_); }
        target_ = birds;
        band_ = std::max(4.0, 0.5 * double(target_ - grown_));
        rate_ = (double(target_) + band_ - front_) / kRampSeconds;
        recentCount_ = 0u;
        clearWindow();
    }

    void retarget(uint32_t birds) noexcept {
        if (birds >= target_) return;
        target_ = birds;
        grown_ = std::min(grown_, target_);
        front_ = std::min(front_, double(target_) + band_);
        recentCount_ = 0u;
        if (!arriving()) beginSettling(); else clearWindow();
    }

    static void blend(double& slope, double sample) noexcept {
        // Believe a higher cost at once and a lower one slowly: an underestimate breaks the floor.
        // More birds never make a frame faster, so a sample far under the estimate is noise.
        sample = std::max(sample, 0.5 * slope);
        slope = sample >= slope ? sample : 0.5 * (slope + sample);
    }

    // What one more bird costs: the steeper of the last step's slope and the average over the
    // whole flock, in frame time and in GPU time.
    double birdCost(double frame, double gpu) const noexcept {
        double cost = std::max(slopeFrame_, slopeGpu_);
        if (haveBase_ && target_ > 0u) {
            cost = std::max(cost, (frame - baseFrame_) / double(target_));
            if (gpu > 0.0 && baseGpu_ > 0.0) cost = std::max(cost, (gpu - baseGpu_) / double(target_));
        }
        return cost;
    }

    void learn(double frame, double gpu) noexcept {
        if (target_ == 0u) { haveBase_ = true; baseFrame_ = frame; baseGpu_ = gpu; }
        if (haveAnchor_) {
            const double moved = double(target_) - double(anchorBirds_);
            if (std::abs(moved) >= std::max(24.0, 0.02 * double(std::max(target_, anchorBirds_)))) {
                blend(slopeFrame_, (frame - anchorFrame_) / moved);
                if (gpu > 0.0 && anchorGpu_ > 0.0) blend(slopeGpu_, (gpu - anchorGpu_) / moved);
            }
        }
        haveAnchor_ = true; anchorFrame_ = frame; anchorGpu_ = gpu; anchorBirds_ = target_;
    }

    void shed(double frame, double limit) noexcept {
        const double proportional = double(target_) * (1.0 - limit / frame);
        const double modelled = slopeFrame_ > 0.0 ? (frame - limit) / slopeFrame_ : 0.0;
        const double least = std::max(4.0, 0.04 * double(target_));
        const double most = std::max(least, 0.6 * double(target_));
        const double drop = std::clamp(kShedExtra * std::max(proportional, modelled), least, most);
        ceiling_ = double(target_); ceilingAge_ = 0.0;
        cooldown_ = kCooldownSeconds;
        atFloor_ = true;
        haveAnchor_ = false; // the next measurement starts the model afresh from where we land
        retarget(static_cast<uint32_t>(std::max(0.0, double(target_) - drop)));
    }

    void decide(double frame, double gpu, uint32_t want, float floorFps) noexcept {
        fps_ = static_cast<float>(1.0 / frame);
        const double limit = 1.0 / (double(floorFps) * kAim);
        if (frame > 1.0 / (double(floorFps) * kShedBelow)) {
            if (target_ > 0u) shed(frame, limit); else atFloor_ = true; // the film alone is under the floor
            return;
        }
        learn(frame, gpu);
        steadyBirds_ = target_; steadyFps_ = fps_;
        peak_ = std::max(peak_, target_);
        if (target_ >= want) { atFloor_ = false; return; }
        if (cooldown_ > 0.0) { atFloor_ = true; return; }
        // GPU time shows the real load when something else (vertical sync, a frame cap) sets the pace.
        const double load = std::max(frame, gpu);
        const double headroom = limit - load;
        if (headroom <= kHoldBand * limit) { atFloor_ = true; return; }
        const double cost = birdCost(frame, gpu);
        double step = std::max(double(target_), kFirstBatch) * (load < kFarBelow * limit ? 3.0 : 1.0);
        if (cost > 0.0) step = std::min(step, kStepShare * headroom / cost);
        // With no measurement of the film alone to judge by, never more than the flock would need
        // if the whole frame were its doing. The film takes part of every frame, so this falls short.
        if (target_ > 0u && !haveBase_) step = std::min(step, kProportionalShare * double(target_) * headroom / load);
        if (ceiling_ > 0.0) step = std::min(step, ceiling_ * kCeilingShare - double(target_));
        step = std::min(step, double(want - target_));
        if (step < std::max(4.0, 0.004 * double(target_))) { atFloor_ = true; return; }
        atFloor_ = false;
        release(target_ + static_cast<uint32_t>(step));
    }

    uint32_t target_ = 0u;   // birds released
    uint32_t grown_ = 0u;    // birds below this index are full size
    double front_ = 0.0;     // arrival front
    double band_ = 1.0;
    double rate_ = 0.0;      // birds a second the front advances

    std::array<double, kWindow> frames_{}, gpu_{};
    uint32_t windowCount_ = 0u;
    double windowSeconds_ = 0.0;
    double settleSeconds_ = 0.0;
    uint32_t settleFrames_ = 0u;
    uint32_t graceFrames_ = 0u;
    double graceSeconds_ = 0.0;
    bool haveBase_ = false; // the film alone, measured with no workout birds flying
    double baseFrame_ = 0.0, baseGpu_ = 0.0;
    std::array<double, kRecent> recent_{};
    uint32_t recentCount_ = 0u, recentAt_ = 0u;

    bool haveAnchor_ = false; // last steady measurement, for the cost of a bird
    double anchorFrame_ = 0.0, anchorGpu_ = 0.0;
    uint32_t anchorBirds_ = 0u;
    double slopeFrame_ = 0.0, slopeGpu_ = 0.0; // seconds a bird
    double ceiling_ = 0.0, ceilingAge_ = 0.0;
    double cooldown_ = 0.0;
    bool atFloor_ = false;
    float fps_ = 0.f;
    uint32_t steadyBirds_ = 0u, peak_ = 0u;
    float steadyFps_ = 0.f;
};

} // namespace boo
