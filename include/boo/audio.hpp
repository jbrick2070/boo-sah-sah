#pragma once

#include "boo/api.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace boo {

struct AudioBandSnapshot {
    float rms = 0.0f;
    float low = 0.0f;
    float mid = 0.0f;
    float high = 0.0f;
};

AudioBandSnapshot computeAnalyticalAudioBands(
    int64_t samplePosition,
    int32_t sampleRate = 48000,
    int64_t durationSamples = 2880000,
    Edition edition = Edition::Family) noexcept;

// Deterministic, allocation-free 48 kHz stereo floating-point synthesizer with
// multi-voice C1 raised-cosine seek/loop crossfading and clickless pause/resume ramps.
// Exposed for direct waveform-continuity verification in unit tests as well as real-time WASAPI mixing.
class AudioSynthesizer48k {
public:
    static constexpr int32_t  kSampleRate       = 48000;
    static constexpr int32_t  kChannels         = 2;
    static constexpr uint32_t kCrossfadeSamples = 768; // 16.0 ms C1 raised-cosine seek/loop crossfade
    static constexpr uint32_t kGainRampSamples  = 576; // 12.0 ms C1 raised-cosine pause/mute/volume ramp
    static constexpr size_t   kMaxVoices        = 8;   // Supports rapid overlapping seeks without allocation

    AudioSynthesizer48k() noexcept;

    void setDurationSamples(int64_t samples) noexcept { durationSamples_ = samples; }
    void setEdition(Edition edition) noexcept;
    Edition edition() const noexcept { return targetEdition_; }
    void reset(uint64_t seedSample, float initialGain = 0.0f) noexcept;
    void seekTo(uint64_t targetSample) noexcept;

    // Synthesizes `frameCount` stereo float frames into `outInterleavedStereo` (size = frameCount * 2).
    // Returns the number of timeline frames advanced (frameCount when !paused, 0 when paused).
    uint32_t renderFrames(
        float* outInterleavedStereo,
        uint32_t frameCount,
        bool paused,
        bool muted,
        float masterVolume01,
        AudioBandSnapshot* outBands = nullptr) noexcept;

    uint64_t primarySamplePosition() const noexcept;
    float currentGain() const noexcept { return currentGain_; }

private:
    struct VoiceSlot {
        bool active = false;
        bool fadingOut = false;
        uint64_t phaseSample = 0;
        uint32_t lfsr = 0xACE12663u;
        float rainLp = 0.0f;
        float currentWeight = 0.0f;
        float fadeStartWeight = 0.0f;
        uint32_t fadePos = 0;
    };

    static uint32_t seedLfsrForSample(uint64_t sample) noexcept;
    int64_t durationSamples_ = 2880000;
    Edition targetEdition_ = Edition::Family;
    float editionMidnightMix_ = 0.0f;
    void evaluateVoiceSample(VoiceSlot& v, float& outLeft, float& outRight, float& outMono) noexcept;

    std::array<VoiceSlot, kMaxVoices> voices_{};
    size_t primaryVoiceIndex_ = 0;
    bool hasRenderedAnyAudio_ = false;

    float currentGain_ = 0.0f;
    float rampStartGain_ = 0.0f;
    float rampTargetGain_ = 0.0f;
    uint32_t gainRampPos_ = kGainRampSamples;

    float residualLeft_ = 0.0f;
    float residualRight_ = 0.0f;
    float residualMono_ = 0.0f;
    uint32_t residualPos_ = 64;

    float lp180_ = 0.0f;
    float lp2400_ = 0.0f;
};

// Section 2.4: 48,000 Hz stereo floating-point WASAPI audio engine with
// IAudioClock hardware presentation tracking, buffer-offset compensation,
// C1 raised-cosine seek crossfading, and read-only default-endpoint migration.
// Audio callback never allocates, locks mutexes, logs, or touches GPU/inference work.
class AudioEngine {
public:
    static constexpr int32_t kTargetSampleRate = 48000;
    static constexpr int32_t kChannels = 2;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    void setDurationSamples(int64_t samples) noexcept { durationSamples_ = samples; }
    void setEdition(Edition edition) noexcept { edition_.store(static_cast<uint8_t>(edition), std::memory_order_relaxed); }
    bool initialize(bool forceFail = false);
    void shutdown() noexcept;

    void setPaused(bool paused) noexcept;
    void setMuted(bool muted) noexcept;
    bool isMuted() const noexcept { return muted_.load(std::memory_order_relaxed); }
    void setMasterVolume(float volume01) noexcept;
    void resetPhase(uint64_t seedSample) noexcept;

    // Polled on the main thread (never on the real-time audio thread) to cleanly
    // migrate to a newly selected default endpoint or recover from endpoint invalidation
    // without modifying OS audio settings. Returns true if a new endpoint became active.
    bool pollEndpointMigration(uint64_t resumeTimelineSample);

    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    bool deviceHealthy() const noexcept { return healthy_.load(std::memory_order_acquire); }
    bool migrationPending() const noexcept { return migrationPending_.load(std::memory_order_acquire); }
    bool usesHardwarePresentationClock() const noexcept { return usesHwClock_.load(std::memory_order_relaxed); }

    // Lock-free 48 kHz speaker-presented sample position reader for TimelineClock
    bool readPlayedSamples48k(int64_t& outSamples48k) const noexcept;
    int64_t submittedFrames48k() const noexcept { return submittedFrames48k_.load(std::memory_order_acquire); }
    int64_t presentedFrames48k() const noexcept { return monotonicPresented48k_.load(std::memory_order_acquire); }
    double estimatedBufferLatencyMs() const noexcept;
    uint32_t endpointMigrationCount() const noexcept { return migrationCount_.load(std::memory_order_relaxed); }

    float currentRmsEnvelope() const noexcept { return rmsEnvelope_.load(std::memory_order_relaxed); }
    AudioBandSnapshot currentBandSnapshot() const noexcept;

    const std::string& endpointDescription() const noexcept { return endpointName_; }
    const std::string& fallbackReason() const noexcept { return fallbackReason_; }
    int32_t mixSampleRate() const noexcept { return mixSampleRate_; }

    // Internal notification hook invoked by read-only IMMNotificationClient observer
    void notifyDefaultEndpointChangedFromCallback(const wchar_t* newDeviceId) noexcept;

private:
    bool openDefaultEndpoint(uint64_t initialSeedSample, int64_t initialPlayedCounter);
    void stopStreamOnly() noexcept;
    void audioThreadMain() noexcept;

    int64_t durationSamples_ = 2880000;
    std::atomic<uint8_t> edition_{0};
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> healthy_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> usesHwClock_{false};
    std::atomic<bool> migrationPending_{false};
    std::atomic<float> masterVolume_{0.25f};

    std::atomic<int64_t> submittedFrames48k_{0};
    mutable std::atomic<int64_t> monotonicPresented48k_{0};

    // Lock-free presentation clock seqlock (updated by audioThreadMain via IAudioClock::GetPosition)
    std::atomic<uint32_t> clockSeq_{0};
    std::atomic<int64_t> clockBasePresented48k_{0};
    std::atomic<int64_t> clockBaseQpcTicks_{0};
    std::atomic<int64_t> clockMaxSubmitted48k_{0};
    std::atomic<bool> clockAdvancing_{false};
    int64_t qpcFrequency_ = 10000000;
    uint64_t audioClockFreq_ = 0;
    int64_t initialPlayedCounter_ = 0;

    // Generation-tagged lock-free phase seek request (never clobbered by audio thread)
    std::atomic<uint64_t> pendingPhaseSample_{0};
    std::atomic<uint64_t> phaseResetGen_{0};

    std::atomic<float> rmsEnvelope_{0.0f};
    std::atomic<uint32_t> bandSeq_{0};
    std::atomic<float> bandRms_{0.0f};
    std::atomic<float> bandLow_{0.0f};
    std::atomic<float> bandMid_{0.0f};
    std::atomic<float> bandHigh_{0.0f};
    mutable AudioBandSnapshot cachedReaderBands_{};

    int32_t mixSampleRate_ = kTargetSampleRate;
    std::string endpointName_;
    std::wstring activeEndpointId_;
    std::string fallbackReason_;
    bool forceFailed_ = false;
    int64_t lastMigrationAttemptQpc_ = 0;
    uint32_t consecutiveMigrationFailures_ = 0;
    std::atomic<uint32_t> migrationCount_{0};

    void* audioEventHandle_ = nullptr;
    void* enumeratorPtr_ = nullptr;
    void* notifClientPtr_ = nullptr;
    void* devicePtr_ = nullptr;
    void* clientPtr_ = nullptr;
    void* renderClientPtr_ = nullptr;
    void* audioClockPtr_ = nullptr;
    uint32_t bufferFrameCount_ = 0;
};

} // namespace boo
