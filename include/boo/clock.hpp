#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace boo {

enum class ClockSource : uint8_t {
    AudioSampleClock     = 0, // Master 48 kHz audio sample position
    SilentMonotonicClock = 1  // High-resolution QPC fallback when audio unavailable/muted-clock
};

// Section 7.1: Explicit transport states
enum class TransportState : uint8_t {
    Uninitialized = 0,
    Loading       = 1,
    Playing       = 2,
    Paused        = 3,
    Seeking       = 4,
    Ended         = 5,
    Loop          = 6,
    Shutdown      = 7
};

const char* toString(ClockSource source) noexcept;
const char* toString(TransportState state) noexcept;

struct ClockSnapshot {
    int64_t samplePosition = 0;
    double filmTimeSeconds = 0.0;
    double dtSeconds = 0.0;
    double unclampedDtSeconds = 0.0;
    ClockSource source = ClockSource::SilentMonotonicClock;
    TransportState state = TransportState::Uninitialized;
    bool loopedThisTick = false;
    bool endedThisTick = false;
    uint64_t loopCount = 0;
};

class TimelineClock {
public:
    static constexpr int32_t kDefaultSampleRate = 48000;
    static constexpr int64_t kFullFilmSamples   = 17280000; // 360.000 s * 48,000 Hz
    static constexpr double  kMaxClampedDtSec   = 0.100;    // 100 ms stall clamp

    using AudioSampleReader = std::function<bool(int64_t& outTotalPlayedSamples)>;
    using MonotonicTickReader = std::function<int64_t()>;

    TimelineClock();

    void configure(int32_t sampleRate, int64_t durationSamples);

    // Optional injection points for deterministic unit testing and real WASAPI binding
    void setAudioSampleReader(AudioSampleReader reader);
    void setCustomMonotonicSource(MonotonicTickReader tickReader, int64_t tickFrequency);

    void start(bool preferAudioClock, bool loopEnabled);
    void pause();
    void resume();
    void togglePause();
    void seekToSample(int64_t targetSample);
    void seekToSeconds(double seconds);
    void stepFrame(int direction);
    void seekRelativeSeconds(int seconds);

    void setLoopEnabled(bool enabled);
    bool loopEnabled() const noexcept { return loopEnabled_; }
    bool getLoopRange(int64_t& startSample, int64_t& endSample) const noexcept {
        startSample = loopStartSample_;
        endSample = loopEndSample_;
        return hasCustomLoopRange_;
    }
    void setLoopRange(int64_t startSample, int64_t endSample);
    void clearLoopRange();

    // Seamlessly switches between audio clock and monotonic clock without jumping timeline position.
    void switchToSilentFallback();
    bool switchToAudioClock();
    void shutdown() noexcept;

    ClockSnapshot tickFrame();

    int32_t sampleRate() const noexcept { return sampleRate_; }
    int64_t durationSamples() const noexcept { return durationSamples_; }
    int64_t currentSample() const noexcept { return currentSample_; }
    double currentSeconds() const noexcept { return samplesToSeconds(currentSample_, sampleRate_); }
    ClockSource source() const noexcept { return source_; }
    TransportState state() const noexcept { return state_; }
    uint64_t loopCount() const noexcept { return loopCount_; }

    static double samplesToSeconds(int64_t samples, int32_t sampleRate = kDefaultSampleRate) noexcept;
    static int64_t secondsToSamples(double seconds, int32_t sampleRate = kDefaultSampleRate) noexcept;
    static std::string formatTimecode(int64_t samples, int32_t sampleRate = kDefaultSampleRate);

private:
    int64_t readMonotonicSamples() const noexcept;
    void rebaseAnchors(int64_t timelineSample);

    int32_t sampleRate_ = kDefaultSampleRate;
    int64_t durationSamples_ = kFullFilmSamples;

    ClockSource source_ = ClockSource::SilentMonotonicClock;
    TransportState state_ = TransportState::Uninitialized;

    bool loopEnabled_ = false;
    bool hasCustomLoopRange_ = false;
    int64_t loopStartSample_ = 0;
    int64_t loopEndSample_ = kFullFilmSamples;

    int64_t currentSample_ = 0;
    int64_t anchorTimelineSample_ = 0;
    int64_t anchorDriverSample_ = 0;
    int64_t lastWallMonotonicSamples_ = 0;
    bool hasLastWallSample_ = false;
    bool seekFlagPending_ = false;
    uint64_t loopCount_ = 0;

    AudioSampleReader audioReader_;
    MonotonicTickReader tickReader_;
    int64_t tickFrequency_ = 10000000;
};

} // namespace boo
