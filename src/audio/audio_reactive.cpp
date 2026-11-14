#include "boo/audio_reactive.hpp"

#include <algorithm>
#include <cmath>

namespace boo {
namespace {

float clamp01(float v) noexcept { return std::min(1.0f, std::max(0.0f, v)); }

// One-pole coefficient for a time constant tau at step dt.
float onePole(float dt, float tau) noexcept {
    return 1.0f - std::exp(-dt / std::max(tau, 1e-4f));
}

float follow(float current, float target, float dt, float attack, float release) noexcept {
    const float k = onePole(dt, target > current ? attack : release);
    return current + (target - current) * k;
}

} // namespace

void AudioReactiveFollower::reset() noexcept {
    state_ = {};
    peak_[0] = peak_[1] = peak_[2] = 0.25f;
    lowAverage_ = 0.0f;
    previousLow_ = 0.0f;
    sinceBeat_ = 1.0;
}

AudioReactiveState AudioReactiveFollower::update(float low, float mid, float high, float rms,
                                                 double dtSeconds, bool reducedFlash, bool hold) noexcept {
    const float dt = static_cast<float>(std::min(0.1, std::max(0.0, dtSeconds)));
    if (!(dt > 0.0f)) {
        return state_;
    }
    const float raw[3] = {
        hold ? 0.0f : clamp01(std::isfinite(low) ? low : 0.0f),
        hold ? 0.0f : clamp01(std::isfinite(mid) ? mid : 0.0f),
        hold ? 0.0f : clamp01(std::isfinite(high) ? high : 0.0f)};
    const float level = hold ? 0.0f : clamp01(std::isfinite(rms) ? rms : 0.0f);

    // Automatic gain: running peak with an ~8 s decay and a floor so silence never amplifies noise.
    float norm[3];
    const float peakDecay = std::exp(-dt / 8.0f);
    for (int b = 0; b < 3; ++b) {
        if (!hold) {
            peak_[b] = std::max(raw[b], std::max(0.08f, peak_[b] * peakDecay));
        }
        norm[b] = clamp01(raw[b] / peak_[b]);
    }

    // Beat: low-band rise above its ~0.4 s average, rate-limited below 3 Hz.
    sinceBeat_ += dt;
    lowAverage_ += (norm[0] - lowAverage_) * onePole(dt, 0.40f);
    const float rise = norm[0] - previousLow_;
    previousLow_ = norm[0];
    const float onset = norm[0] - lowAverage_ * 1.12f - 0.05f;
    float beat = state_.beat * std::exp(-dt / kBeatDecaySeconds);
    if (!reducedFlash && !hold && onset > 0.0f && rise > 0.0f && sinceBeat_ >= kMinBeatIntervalSeconds) {
        beat = std::max(beat, clamp01(0.35f + onset * 3.2f));
        sinceBeat_ = 0.0;
    }
    if (reducedFlash) {
        beat = 0.0f;
    }

    const float scale = reducedFlash ? 0.5f : 1.0f;
    const float slow = reducedFlash ? 3.0f : 1.0f;
    state_.low   = follow(state_.low,   norm[0] * scale, dt, 0.030f * slow, 0.22f * slow);
    state_.mid   = follow(state_.mid,   norm[1] * scale, dt, 0.035f * slow, 0.16f * slow);
    state_.high  = follow(state_.high,  norm[2] * scale, dt, 0.025f * slow, 0.11f * slow);
    state_.level = follow(state_.level, level * scale,   dt, 0.050f * slow, 0.30f * slow);
    state_.beat  = clamp01(beat);
    return state_;
}

uint32_t AudioReactiveFollower::pack(const AudioReactiveState& s) noexcept {
    auto q = [](float v) noexcept { return static_cast<uint32_t>(clamp01(v) * 255.0f + 0.5f); };
    return q(s.low) | (q(s.mid) << 8u) | (q(s.high) << 16u) | (q(s.beat) << 24u);
}

} // namespace boo
