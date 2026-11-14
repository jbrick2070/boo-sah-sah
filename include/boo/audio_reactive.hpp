#pragma once

#include "boo/api.hpp"

#include <cstdint>

namespace boo {

// Turns the audio engine's raw low / mid / high band RMS (measured on the actual mixed output: procedural
// score, bundled OST or an imported track) into smooth, auto-levelled, flash-safe visual drive signals.
//
//   * Per-band automatic gain: a slowly decaying running peak normalises quiet and loud masters alike.
//   * Asymmetric smoothing: fast attack, slower per-band release, so motion reads as musical, not jittery.
//   * Beat: low-band onset detection against a short running average, with a refractory interval that
//     caps retriggers below 3 Hz, and an exponential decay envelope.
//   * Reduced-flash presentation disables the beat envelope and halves / slows the band signals.
//
// Portable and allocation-free; call once per presented frame on the main thread.
class AudioReactiveFollower {
public:
    static constexpr double kMinBeatIntervalSeconds = 0.36; // < 3 retriggers per second
    static constexpr float  kBeatDecaySeconds       = 0.20f;

    void reset() noexcept;

    // low/mid/high/rms are the raw [0,1] band values for the audio block nearest this frame.
    // dtSeconds is the presented frame delta (clamped internally). When `hold` is true (paused or
    // seeking) the signals glide to rest instead of following the input.
    AudioReactiveState update(float low, float mid, float high, float rms,
                              double dtSeconds, bool reducedFlash, bool hold = false) noexcept;

    const AudioReactiveState& state() const noexcept { return state_; }

    // Packs a state into RGBA8 (low | mid << 8 | high << 16 | beat << 24) for a single shader DWORD.
    static uint32_t pack(const AudioReactiveState& s) noexcept;

private:
    AudioReactiveState state_{};
    float peak_[3]{0.25f, 0.25f, 0.25f};
    float lowAverage_ = 0.0f;
    float previousLow_ = 0.0f;
    double sinceBeat_ = 1.0;
};

} // namespace boo
