#pragma once

#include "boo/api.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace boo {

struct AudioBandSnapshot {
    float rms = 0.0f;
    float low = 0.0f;
    float mid = 0.0f;
    float high = 0.0f;
};

struct CustomAudioTrack48k {
    std::vector<float> interleavedStereo;  // Interleaved [L0, R0, L1, R1, ...] float at 48,000 Hz
    std::vector<int16_t> interleavedPcm16; // Optional compact normalized 16-bit PCM stereo at 48,000 Hz
    uint64_t frameCount = 0;
    std::string label;
};

// Decodes MP3 / WAV / FLAC / M4A / AAC / OGG audio via dynamic Media Foundation (zero static PE import)
// and resamples cleanly to 48,000 Hz stereo. Supports cooperative cancellation for instant shutdown.
bool loadCustomAudioFile48k(
    const std::filesystem::path& filePath,
    CustomAudioTrack48k& outTrack,
    std::string* outError = nullptr,
    bool injectFault = false,
    const std::atomic<bool>* cancelRequested = nullptr,
    bool compactPcm16 = false);

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
    void setCustomTrack(const CustomAudioTrack48k* track) noexcept { customTrack_ = track; }
    const CustomAudioTrack48k* customTrack() const noexcept { return customTrack_; }
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
        const CustomAudioTrack48k* voiceTrack = nullptr;
    };

    static uint32_t seedLfsrForSample(uint64_t sample) noexcept;
    int64_t durationSamples_ = 2880000;
    Edition targetEdition_ = Edition::Family;
    float editionMidnightMix_ = 0.0f;
    void evaluateVoiceSample(VoiceSlot& v, float& outLeft, float& outRight) noexcept;

    std::array<VoiceSlot, kMaxVoices> voices_{};
    size_t primaryVoiceIndex_ = 0;
    bool hasRenderedAnyAudio_ = false;

    float currentGain_ = 0.0f;
    float rampStartGain_ = 0.0f;
    float rampTargetGain_ = 0.0f;
    uint32_t gainRampPos_ = kGainRampSamples;

    float residualLeft_ = 0.0f;
    float residualRight_ = 0.0f;
    float residualMusicLeft_ = 0.0f;
    float residualMusicRight_ = 0.0f;
    float residualSynthWeight_ = 0.0f;
    uint32_t residualPos_ = 64;

    float lp180_ = 0.0f;
    float lp2400_ = 0.0f;
    const CustomAudioTrack48k* customTrack_ = nullptr;
    struct StereoHallState;
    std::shared_ptr<StereoHallState> hall_;
};

// Section 2.4: 48,000 Hz stereo floating-point WASAPI audio engine with
// IAudioClock hardware presentation tracking, buffer-offset compensation,
// C1 raised-cosine seek crossfading, and read-only default-endpoint migration.
// Audio callback never allocates, locks mutexes, logs, or touches GPU/inference work.
class AudioEngine {
public:
    static constexpr int32_t kTargetSampleRate = 48000;
    static constexpr int32_t kChannels = 2;
    static constexpr size_t  kBundledTrackCount = 4;

    static const char* bundledSoundtrackTitle(uint32_t slot1To4) noexcept;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    void setDurationSamples(int64_t samples) noexcept { durationSamples_ = samples; }
    void setEdition(Edition edition) noexcept { edition_.store(static_cast<uint8_t>(edition), std::memory_order_relaxed); }
    bool initialize(bool forceFail = false);
    void shutdown() noexcept;

    void setPaused(bool paused) noexcept;
    void setExploreAmbient(bool active) noexcept { exploreAmbient_.store(active, std::memory_order_relaxed); }
    void setMuted(bool muted) noexcept;
    bool isMuted() const noexcept { return muted_.load(std::memory_order_relaxed); }
    void setMasterVolume(float volume01) noexcept;
    void resetPhase(uint64_t seedSample) noexcept;

    // Built-in 4-track original soundtrack (0=Procedural Synth, 1..4=Bundled OST Tracks 1..4, 5=External MP3)
    void startBundledSoundtrack(
        const std::array<std::filesystem::path, kBundledTrackCount>& trackPaths,
        uint32_t initialSlot1To4 = 1u);
    void selectSoundtrackSlot(uint32_t slot0To5) noexcept;
    uint32_t cycleSoundtrackForward() noexcept;
    uint32_t cycleSoundtrackBackward() noexcept;
    uint32_t activeSoundtrackSlot() const noexcept { return selectedTrackSlot_.load(std::memory_order_acquire); }
    bool hasBundledSoundtrack() const noexcept { return hasBundledSoundtrack_; }
    std::string activeSoundtrackTitle() const;

    bool loadExternalAudioFile(const std::filesystem::path& filePath, std::string* outError = nullptr, bool injectFault = false);
    void clearExternalAudioFile() noexcept;
    bool hasExternalAudioFile() const noexcept {
        return selectedTrackSlot_.load(std::memory_order_acquire) == 5u &&
               activeCustomTrack_.load(std::memory_order_acquire) != nullptr;
    }
    const std::string& externalAudioLabel() const noexcept { return customAudioLabel_; }

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
    void stopEndpointAndEnumerator() noexcept;
    void stopPreloadWorker() noexcept;
    void audioThreadMain() noexcept;

    int64_t durationSamples_ = 2880000;
    std::atomic<uint8_t> edition_{0};
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> healthy_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> exploreAmbient_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> usesHwClock_{false};
    std::atomic<bool> migrationPending_{false};
    std::atomic<float> masterVolume_{0.75f};

    bool hasBundledSoundtrack_ = false;
    std::atomic<uint32_t> selectedTrackSlot_{0}; // 0=Procedural Synth, 1..4=OST 1..4, 5=External MP3
    std::atomic<uint64_t> trackSwitchGen_{0};
    std::atomic<uint64_t> lastSongSamplePos_{0};
    std::atomic<bool> preloadInProgress_{false};
    std::atomic<bool> cancelPreload_{false};
    std::thread preloadWorker_;
    std::array<std::atomic<const CustomAudioTrack48k*>, kBundledTrackCount> bundledTracks_{nullptr, nullptr, nullptr, nullptr};
    std::array<std::shared_ptr<const CustomAudioTrack48k>, kBundledTrackCount> bundledTrackStorage_{};

    std::atomic<const CustomAudioTrack48k*> activeCustomTrack_{nullptr};
    std::vector<std::shared_ptr<const CustomAudioTrack48k>> retainedTracks_;
    std::string customAudioLabel_;

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
