#include "boo/clock.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace boo {

namespace {
int64_t scaleTicksToSamples(int64_t ticks, int64_t sampleRate, int64_t freq) noexcept {
    if (freq <= 0) return 0;
    const int64_t wholeSeconds = ticks / freq;
    const int64_t remTicks     = ticks % freq;
    return wholeSeconds * sampleRate + (remTicks * sampleRate) / freq;
}
} // namespace

const char* toString(ClockSource source) noexcept {
    switch (source) {
        case ClockSource::AudioSampleClock:     return "AudioSampleClock_48kHz";
        case ClockSource::SilentMonotonicClock: return "SilentMonotonicClock_QPC";
    }
    return "Unknown";
}

const char* toString(TransportState state) noexcept {
    switch (state) {
        case TransportState::Uninitialized: return "Uninitialized";
        case TransportState::Loading:       return "Loading";
        case TransportState::Playing:       return "Playing";
        case TransportState::Paused:        return "Paused";
        case TransportState::Seeking:       return "Seeking";
        case TransportState::Ended:         return "Ended";
        case TransportState::Loop:          return "Loop";
        case TransportState::Shutdown:      return "Shutdown";
    }
    return "Unknown";
}

TimelineClock::TimelineClock() {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    tickFrequency_ = freq.QuadPart > 0 ? freq.QuadPart : 10000000;
    tickReader_ = []() -> int64_t {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return counter.QuadPart;
    };
}

void TimelineClock::configure(int32_t sampleRate, int64_t durationSamples) {
    sampleRate_ = sampleRate > 0 ? sampleRate : kDefaultSampleRate;
    durationSamples_ = durationSamples > 0 ? durationSamples : kFullFilmSamples;
    if (!hasCustomLoopRange_) {
        loopStartSample_ = 0;
        loopEndSample_ = durationSamples_;
    } else {
        loopStartSample_ = std::clamp<int64_t>(loopStartSample_, 0, durationSamples_ - 1);
        loopEndSample_   = std::clamp<int64_t>(loopEndSample_, loopStartSample_ + 1, durationSamples_);
    }
    currentSample_ = std::clamp<int64_t>(currentSample_, 0, durationSamples_);
}

void TimelineClock::setAudioSampleReader(AudioSampleReader reader) {
    audioReader_ = std::move(reader);
}

void TimelineClock::setCustomMonotonicSource(MonotonicTickReader tickReader, int64_t tickFrequency) {
    tickReader_ = std::move(tickReader);
    tickFrequency_ = tickFrequency > 0 ? tickFrequency : 10000000;
}

int64_t TimelineClock::readMonotonicSamples() const noexcept {
    if (!tickReader_) return 0;
    return scaleTicksToSamples(tickReader_(), sampleRate_, tickFrequency_);
}

void TimelineClock::rebaseAnchors(int64_t timelineSample) {
    currentSample_ = std::clamp<int64_t>(timelineSample, 0, durationSamples_);
    anchorTimelineSample_ = currentSample_;

    int64_t audioPlayed = 0;
    if (source_ == ClockSource::AudioSampleClock && audioReader_ && audioReader_(audioPlayed)) {
        anchorDriverSample_ = audioPlayed;
    } else {
        source_ = ClockSource::SilentMonotonicClock;
        anchorDriverSample_ = readMonotonicSamples();
    }
}

void TimelineClock::start(bool preferAudioClock, bool loopEnabled) {
    loopEnabled_ = loopEnabled;
    loopCount_ = 0;
    int64_t initialSample = hasCustomLoopRange_ ? loopStartSample_ : 0;

    int64_t audioPlayed = 0;
    if (preferAudioClock && audioReader_ && audioReader_(audioPlayed)) {
        source_ = ClockSource::AudioSampleClock;
    } else {
        source_ = ClockSource::SilentMonotonicClock;
    }

    rebaseAnchors(initialSample);
    lastWallMonotonicSamples_ = readMonotonicSamples();
    hasLastWallSample_ = true;
    seekFlagPending_ = false;
    state_ = loopEnabled_ ? TransportState::Loop : TransportState::Playing;
}

void TimelineClock::pause() {
    if (state_ == TransportState::Playing || state_ == TransportState::Loop ||
        state_ == TransportState::Seeking || state_ == TransportState::Ended) {
        state_ = TransportState::Paused;
    }
}

void TimelineClock::resume() {
    if (state_ == TransportState::Paused || state_ == TransportState::Ended) {
        if (state_ == TransportState::Ended) {
            currentSample_ = hasCustomLoopRange_ ? loopStartSample_ : 0;
        }
        rebaseAnchors(currentSample_);
        lastWallMonotonicSamples_ = readMonotonicSamples();
        hasLastWallSample_ = true;
        state_ = loopEnabled_ ? TransportState::Loop : TransportState::Playing;
    }
}

void TimelineClock::togglePause() {
    if (state_ == TransportState::Paused || state_ == TransportState::Ended) {
        resume();
    } else if (state_ == TransportState::Playing || state_ == TransportState::Loop) {
        pause();
    }
}

void TimelineClock::seekToSample(int64_t targetSample) {
    const int64_t clamped = std::clamp<int64_t>(targetSample, 0, durationSamples_);
    rebaseAnchors(clamped);
    seekFlagPending_ = true;
    if (state_ == TransportState::Ended) {
        state_ = loopEnabled_ ? TransportState::Loop : TransportState::Playing;
    }
}

void TimelineClock::stepFrame(int direction) {
    if (state_ == TransportState::Ended) {
        state_ = TransportState::Paused;
    }
    if (state_ != TransportState::Paused) return;
    seekToSample(currentSample_ + (direction < 0 ? -1 : 1) * (sampleRate_ / 60));
}

void TimelineClock::seekRelativeSeconds(int seconds) {
    seekToSample(currentSample_ + static_cast<int64_t>(seconds) * sampleRate_);
}

void TimelineClock::seekToSeconds(double seconds) {
    seekToSample(secondsToSamples(seconds, sampleRate_));
}

void TimelineClock::setLoopEnabled(bool enabled) {
    loopEnabled_ = enabled;
    if (state_ == TransportState::Playing && enabled) {
        state_ = TransportState::Loop;
    } else if (state_ == TransportState::Loop && !enabled) {
        state_ = TransportState::Playing;
    }
}

void TimelineClock::setLoopRange(int64_t startSample, int64_t endSample) {
    if (endSample > startSample && startSample >= 0 && endSample <= durationSamples_) {
        hasCustomLoopRange_ = true;
        loopStartSample_ = startSample;
        loopEndSample_ = endSample;
        if (currentSample_ < loopStartSample_ || currentSample_ >= loopEndSample_) {
            seekToSample(loopStartSample_);
        }
    }
}

void TimelineClock::clearLoopRange() {
    hasCustomLoopRange_ = false;
    loopStartSample_ = 0;
    loopEndSample_ = durationSamples_;
}

void TimelineClock::switchToSilentFallback() {
    if (source_ != ClockSource::SilentMonotonicClock) {
        source_ = ClockSource::SilentMonotonicClock;
        anchorTimelineSample_ = currentSample_;
        anchorDriverSample_ = readMonotonicSamples();
    }
}

bool TimelineClock::switchToAudioClock() {
    int64_t audioPlayed = 0;
    if (!audioReader_ || !audioReader_(audioPlayed)) {
        return false;
    }
    source_ = ClockSource::AudioSampleClock;
    anchorTimelineSample_ = currentSample_;
    anchorDriverSample_ = audioPlayed;
    return true;
}

void TimelineClock::shutdown() noexcept {
    state_ = TransportState::Shutdown;
}

ClockSnapshot TimelineClock::tickFrame() {
    ClockSnapshot snap{};
    const int64_t nowWallSamples = readMonotonicSamples();
    int64_t wallDeltaSamples = 0;
    if (hasLastWallSample_) {
        wallDeltaSamples = std::max<int64_t>(0, nowWallSamples - lastWallMonotonicSamples_);
    }
    lastWallMonotonicSamples_ = nowWallSamples;
    hasLastWallSample_ = true;

    snap.unclampedDtSeconds = samplesToSeconds(wallDeltaSamples, sampleRate_);
    snap.dtSeconds = std::min(snap.unclampedDtSeconds, kMaxClampedDtSec);

    if (state_ == TransportState::Playing || state_ == TransportState::Loop) {
        int64_t driverNow = 0;
        if (source_ == ClockSource::AudioSampleClock) {
            if (!audioReader_ || !audioReader_(driverNow)) {
                // Seamless fallback to monotonic clock if audio device dropped mid-show
                switchToSilentFallback();
                driverNow = anchorDriverSample_;
            }
        } else {
            driverNow = nowWallSamples;
        }

        const int64_t elapsedSamples = std::max<int64_t>(0, driverNow - anchorDriverSample_);
        int64_t rawPos = anchorTimelineSample_ + elapsedSamples;

        const int64_t activeEnd   = hasCustomLoopRange_ ? loopEndSample_ : durationSamples_;
        const int64_t activeStart = hasCustomLoopRange_ ? loopStartSample_ : 0;
        const int64_t span        = std::max<int64_t>(1, activeEnd - activeStart);

        if (rawPos >= activeEnd) {
            if (loopEnabled_) {
                const int64_t over = rawPos - activeStart;
                const uint64_t wraps = static_cast<uint64_t>(over / span);
                loopCount_ += wraps;
                rawPos = activeStart + (over % span);
                anchorTimelineSample_ = rawPos;
                anchorDriverSample_ = driverNow;
                snap.loopedThisTick = true;
            } else {
                rawPos = activeEnd;
                state_ = TransportState::Ended;
                snap.endedThisTick = true;
                snap.dtSeconds = 0.0;
            }
        }
        currentSample_ = rawPos;
    } else {
        snap.dtSeconds = 0.0;
    }

    snap.samplePosition = currentSample_;
    snap.filmTimeSeconds = samplesToSeconds(currentSample_, sampleRate_);
    snap.source = source_;
    snap.state = seekFlagPending_ ? TransportState::Seeking : state_;
    snap.loopCount = loopCount_;
    seekFlagPending_ = false;
    return snap;
}

double TimelineClock::samplesToSeconds(int64_t samples, int32_t sampleRate) noexcept {
    if (sampleRate <= 0) sampleRate = kDefaultSampleRate;
    return static_cast<double>(samples) / static_cast<double>(sampleRate);
}

int64_t TimelineClock::secondsToSamples(double seconds, int32_t sampleRate) noexcept {
    if (sampleRate <= 0) sampleRate = kDefaultSampleRate;
    if (!std::isfinite(seconds) || seconds <= 0.0) return 0;
    return static_cast<int64_t>(std::llround(seconds * static_cast<double>(sampleRate)));
}

std::string TimelineClock::formatTimecode(int64_t samples, int32_t sampleRate) {
    if (sampleRate <= 0) sampleRate = kDefaultSampleRate;
    const int64_t clamped = std::max<int64_t>(0, samples);
    const int64_t totalMs = (clamped * 1000) / sampleRate;
    const int64_t mins    = totalMs / 60000;
    const int64_t secs    = (totalMs / 1000) % 60;
    const int64_t ms      = totalMs % 1000;
    char buf[32]{};
    std::snprintf(
        buf,
        sizeof(buf),
        "%02lld:%02lld.%03lld",
        static_cast<long long>(mins),
        static_cast<long long>(secs),
        static_cast<long long>(ms));
    return std::string(buf);
}

} // namespace boo
