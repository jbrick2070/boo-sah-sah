#include "boo/audio.hpp"
#include "boo/logging.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

namespace boo {

using Microsoft::WRL::ComPtr;

namespace {

constexpr double kPi    = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kInvSr = 1.0 / 48000.0;

std::string propWideToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), len, nullptr, nullptr);
    return out;
}

int64_t queryQpcTicksNow() noexcept {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

int64_t scaleTicksToFrames48k(int64_t ticks, int64_t freq) noexcept {
    if (freq <= 0 || ticks <= 0) return 0;
    const int64_t wholeSec = ticks / freq;
    const int64_t remTicks = ticks % freq;
    return wholeSec * AudioEngine::kTargetSampleRate +
           (remTicks * AudioEngine::kTargetSampleRate) / freq;
}

// Read-only IMMNotificationClient observer that detects user/OS default-endpoint changes
// without ever altering system audio configuration.
class EndpointNotificationClient final : public IMMNotificationClient {
public:
    explicit EndpointNotificationClient(AudioEngine* owner) noexcept : owner_(owner) {}

    void detachOwner() noexcept {
        owner_.store(nullptr, std::memory_order_release);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG next = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (next == 0) {
            delete this;
        }
        return next;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppvObject = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD /*dwNewState*/) override {
        if (auto* eng = owner_.load(std::memory_order_acquire)) {
            eng->notifyDefaultEndpointChangedFromCallback(pwstrDeviceId);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override {
        if (auto* eng = owner_.load(std::memory_order_acquire)) {
            eng->notifyDefaultEndpointChangedFromCallback(pwstrDeviceId);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
        EDataFlow flow,
        ERole role,
        LPCWSTR pwstrDefaultDeviceId) override {
        if (flow == eRender && role == eConsole) {
            if (auto* eng = owner_.load(std::memory_order_acquire)) {
                eng->notifyDefaultEndpointChangedFromCallback(pwstrDefaultDeviceId);
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/) override {
        return S_OK;
    }

private:
    std::atomic<ULONG> refCount_{1};
    std::atomic<AudioEngine*> owner_{nullptr};
};

} // namespace

AudioBandSnapshot computeAnalyticalAudioBands(
    int64_t samplePosition,
    int32_t sampleRate,
    int64_t durationSamples,
    Edition edition) noexcept {
    const double sr = static_cast<double>(std::max<int32_t>(1, sampleRate));
    const double t = std::max(0.0, static_cast<double>(samplePosition) / sr);
    const double t60 = std::fmod(t, 60.0);

    if (durationSamples == 17280000) {
        const double u = std::clamp(std::fmod(t, 360.0), 0.0, 360.0);
        auto fade = [](double x) noexcept { return 0.5 - 0.5 * std::cos(kPi * std::clamp(x, 0.0, 1.0)); };
        const double midShift = (edition == Edition::Midnight) ? 0.08 : 0.0;

        // 7-act smooth crossfade envelopes across [0..45..100..150..195..245..275..360]
        const double c1 = fade((u - 43.0) / 4.0);
        const double c2 = fade((u - 98.0) / 4.0);
        const double c3 = fade((u - 148.0) / 4.0);
        const double c4 = fade((u - 193.0) / 4.0);
        const double c5 = fade((u - 243.0) / 4.0);
        const double c6 = fade((u - 273.0) / 4.0);
        const double dawnCalm = fade((u - 338.0) / 8.0);

        const double rev0 = std::exp(-std::pow((u - 36.0) / 4.5, 2.0));
        const double rev1 = std::exp(-std::pow((u - 175.0) / 5.0, 2.0));
        const double rev2 = std::exp(-std::pow((u - 342.0) / 5.0, 2.0));

        const double pulse2Hz = 0.5 + 0.5 * std::sin(kTwoPi * 2.0 * t);
        const double pulse3Hz = 0.5 + 0.5 * std::sin(kTwoPi * 3.0 * t + 0.6);

        double lowEnv  = 0.28 * (1.0 - c1) + 0.42 * c1 * (1.0 - c2) + 0.36 * c2 * (1.0 - c3)
                       + 0.44 * c3 * (1.0 - c4) + 0.68 * c4 * (1.0 - c5) + 0.58 * c5 * (1.0 - c6)
                       + (0.64 * (1.0 - 0.55 * dawnCalm)) * c6 + midShift;
        double midEnv  = 0.46 * (1.0 - c1) + 0.38 * c1 * (1.0 - c2) + 0.62 * c2 * (1.0 - c3)
                       + (0.52 + 0.16 * rev1) * c3 * (1.0 - c4) + 0.58 * c4 * (1.0 - c5)
                       + 0.44 * c5 * (1.0 - c6) + (0.60 * (1.0 - 0.35 * dawnCalm) + 0.14 * rev2) * c6;
        double highEnv = (0.26 + 0.28 * rev0) * (1.0 - c1) + 0.64 * c1 * (1.0 - c2)
                       + 0.42 * c2 * (1.0 - c3) + (0.34 + 0.24 * rev1) * c3 * (1.0 - c4)
                       + 0.52 * c4 * (1.0 - c5) + 0.62 * c5 * (1.0 - c6)
                       + (0.66 * (1.0 - 0.45 * dawnCalm) + 0.18 * rev2) * c6;

        AudioBandSnapshot out{};
        out.low  = std::clamp(static_cast<float>(lowEnv  * (0.62 + 0.38 * pulse2Hz)), 0.0f, 1.0f);
        out.mid  = std::clamp(static_cast<float>(midEnv  * (0.72 + 0.28 * std::sin(kTwoPi * 0.5 * t + 0.4))), 0.0f, 1.0f);
        out.high = std::clamp(static_cast<float>(highEnv * (0.60 + 0.40 * pulse3Hz)), 0.0f, 1.0f);
        out.rms  = std::clamp(0.45f * out.low + 0.35f * out.mid + 0.20f * out.high, 0.0f, 1.0f);
        return out;
    }

    if(durationSamples == 8640000) {
        const double u=std::fmod(t,180.0),a=.5-.5*std::cos(kPi*std::clamp((u-58)/4.,0.,1.)),b=.5-.5*std::cos(kPi*std::clamp((u-118)/4.,0.,1.));
        AudioBandSnapshot out;out.low=float(.35+.25*(1-a)*(.5+.5*std::sin(kTwoPi*2*t)));out.mid=float(.35+.25*a);out.high=float(.15+.2*a+.3*b*std::exp(-std::pow((u-168)/8,2)));out.rms=.45f*out.low+.35f*out.mid+.2f*out.high;return out;
    }
    const float sec1 = (t60 >= 20.0 && t60 < 40.0) ? 1.0f : 0.35f;
    const float sec2 = (t60 >= 40.0) ? 1.0f : 0.30f;

    AudioBandSnapshot out{};
    out.low  = std::clamp(static_cast<float>((0.25 + 0.45 * sec1) * (0.55 + 0.45 * std::sin(kTwoPi * 1.0 * t))), 0.0f, 1.0f);
    out.mid  = std::clamp(static_cast<float>(0.42 + 0.28 * std::sin(kTwoPi * 0.5 * t + 0.7)), 0.0f, 1.0f);
    out.high = std::clamp(static_cast<float>((0.20 + 0.55 * sec2) * (0.50 + 0.50 * std::sin(kTwoPi * 1.5 * t + 1.4))), 0.0f, 1.0f);
    out.rms  = std::clamp(0.45f * out.low + 0.35f * out.mid + 0.20f * out.high, 0.0f, 1.0f);
    return out;
}

// ============================================================================
// AudioSynthesizer48k — Deterministic Multi-Voice C1 Crossfade Synthesizer
// ============================================================================

AudioSynthesizer48k::AudioSynthesizer48k() noexcept {
    reset(0, 0.0f);
}

uint32_t AudioSynthesizer48k::seedLfsrForSample(uint64_t sample) noexcept {
    uint32_t h = static_cast<uint32_t>(sample ^ (sample >> 32)) * 2654435761u;
    h ^= 0xACE12663u;
    return (h != 0u) ? h : 0xACE12663u;
}

void AudioSynthesizer48k::setEdition(Edition edition) noexcept {
    targetEdition_ = edition;
    if (!hasRenderedAnyAudio_ || currentGain_ <= 1e-6f) {
        editionMidnightMix_ = (edition == Edition::Midnight) ? 1.0f : 0.0f;
    }
}

void AudioSynthesizer48k::reset(uint64_t seedSample, float initialGain) noexcept {
    voices_ = {};
    primaryVoiceIndex_ = 0;
    voices_[0].active = true;
    voices_[0].fadingOut = false;
    voices_[0].phaseSample = seedSample;
    voices_[0].lfsr = seedLfsrForSample(seedSample);
    voices_[0].rainLp = 0.0f;
    voices_[0].currentWeight = 1.0f;
    voices_[0].fadeStartWeight = 1.0f;
    voices_[0].fadePos = kCrossfadeSamples;

    hasRenderedAnyAudio_ = (initialGain > 1e-6f);
    editionMidnightMix_ = (targetEdition_ == Edition::Midnight) ? 1.0f : 0.0f;
    currentGain_ = std::clamp(initialGain, 0.0f, 1.0f);
    rampStartGain_ = currentGain_;
    rampTargetGain_ = currentGain_;
    gainRampPos_ = kGainRampSamples;

    residualLeft_ = 0.0f;
    residualRight_ = 0.0f;
    residualMono_ = 0.0f;
    residualPos_ = 64;

    lp180_ = 0.0f;
    lp2400_ = 0.0f;
}

uint64_t AudioSynthesizer48k::primarySamplePosition() const noexcept {
    return voices_[primaryVoiceIndex_].phaseSample;
}

void AudioSynthesizer48k::seekTo(uint64_t targetSample) noexcept {
    if (!hasRenderedAnyAudio_ || currentGain_ <= 1e-6f) {
        voices_ = {};
        primaryVoiceIndex_ = 0;
        voices_[0].active = true;
        voices_[0].fadingOut = false;
        voices_[0].phaseSample = targetSample;
        voices_[0].lfsr = seedLfsrForSample(targetSample);
        voices_[0].rainLp = 0.0f;
        voices_[0].currentWeight = 1.0f;
        voices_[0].fadeStartWeight = 1.0f;
        voices_[0].fadePos = kCrossfadeSamples;
        return;
    }

    const VoiceSlot& curPri = voices_[primaryVoiceIndex_];
    if (curPri.active && !curPri.fadingOut && curPri.phaseSample == targetSample) {
        return;
    }

    // Transition all non-fading voices into smooth C1 raised-cosine fade-out from their exact current weight.
    for (auto& v : voices_) {
        if (v.active && !v.fadingOut) {
            if (v.currentWeight <= 1e-6f) {
                v.active = false;
                v.currentWeight = 0.0f;
            } else {
                v.fadingOut = true;
                v.fadeStartWeight = v.currentWeight;
                v.fadePos = 0;
            }
        }
    }

    // Allocate a voice slot for the incoming target sample.
    size_t chosen = kMaxVoices;
    for (size_t i = 0; i < kMaxVoices; ++i) {
        if (!voices_[i].active) {
            chosen = i;
            break;
        }
    }
    if (chosen == kMaxVoices) {
        // Extreme stress (>8 seeks inside a single 16 ms window): recycle the quietest fading voice
        // and fold its instantaneous output into the 64-sample raised-cosine residual damper.
        size_t minIdx = 0;
        float minW = voices_[0].currentWeight;
        for (size_t i = 1; i < kMaxVoices; ++i) {
            if (voices_[i].currentWeight < minW) {
                minW = voices_[i].currentWeight;
                minIdx = i;
            }
        }
        float evL = 0.0f, evR = 0.0f, evM = 0.0f;
        evaluateVoiceSample(voices_[minIdx], evL, evR, evM);
        residualLeft_  += evL * minW;
        residualRight_ += evR * minW;
        residualMono_  += evM * minW;
        residualPos_ = 0;
        chosen = minIdx;
    }

    VoiceSlot& incoming = voices_[chosen];
    incoming = {};
    incoming.active = true;
    incoming.fadingOut = false;
    incoming.phaseSample = targetSample;
    incoming.lfsr = seedLfsrForSample(targetSample);
    incoming.rainLp = 0.0f;
    incoming.currentWeight = 0.0f;
    incoming.fadeStartWeight = 0.0f;
    incoming.fadePos = 0;
    primaryVoiceIndex_ = chosen;
}

void AudioSynthesizer48k::evaluateVoiceSample(
    VoiceSlot& v,
    float& outLeft,
    float& outRight,
    float& outMono) noexcept {
    const double t = static_cast<double>(v.phaseSample) * kInvSr;
    if (durationSamples_ == 17280000 || durationSamples_ == 2640000 ||
        durationSamples_ == 2400000  || durationSamples_ == 2160000) {
        const double durSec = static_cast<double>(durationSamples_) * kInvSr;
        const double localU = std::fmod(t, durSec);
        auto fade = [](double x) noexcept { return 0.5 - 0.5 * std::cos(kPi * std::clamp(x, 0.0, 1.0)); };
        auto sine = [&](double hz) noexcept { return std::sin(kTwoPi * hz * t); };

        // Map standalone solo loops directly into their matching movement in the 360.0s 7-act score
        double u = localU;
        if (durationSamples_ == 2640000)      u = 45.0  + localU; // SoloRT [45..100s)
        else if (durationSamples_ == 2400000) u = 195.0 + localU; // SoloCUDA / SoloTensor 50s loop
        else if (durationSamples_ == 2160000) u = 150.0 + localU; // SoloNPU [150..195s)

        const double seam = fade(localU / 0.050) * fade((durSec - localU) / 0.050);
        const double midMix = std::clamp(static_cast<double>(editionMidnightMix_), 0.0, 1.0);
        const double famMix = 1.0 - midMix;

        // Smooth 4.0s crossfades across all 7 act boundaries
        const double c1 = fade((u - 43.0) / 4.0);
        const double c2 = fade((u - 98.0) / 4.0);
        const double c3 = fade((u - 148.0) / 4.0);
        const double c4 = fade((u - 193.0) / 4.0);
        const double c5 = fade((u - 243.0) / 4.0);
        const double c6 = fade((u - 273.0) / 4.0);

        const double w0 = 1.0 - c1;
        const double w1 = c1 * (1.0 - c2);
        const double w2 = c2 * (1.0 - c3);
        const double w3 = c3 * (1.0 - c4);
        const double w4 = c4 * (1.0 - c5);
        const double w5 = c5 * (1.0 - c6);
        const double w6 = c6;

        // Filtered rain texture for Act 0 Taipei opening
        v.lfsr = (v.lfsr >> 1u) ^ (((v.lfsr & 1u) != 0u) ? 0xB400u : 0u);
        const float white = static_cast<float>((static_cast<double>(v.lfsr & 0xFFFFu) / 32768.0) - 1.0);
        v.rainLp += 0.16f * (white - v.rainLp);

        // Act 0 [0..45s): Synth-noir city-pop (Dmaj9 in Family vs sultry Bm9 in Midnight) + 36.0s Enamel Scale Reveal swell
        const double rev0 = std::exp(-std::pow((u - 36.0) / 4.2, 2.0));
        const double padFam = 0.095 * sine(146.832) + 0.068 * sine(184.997) + 0.062 * sine(220.000) + 0.048 * sine(277.183) + 0.040 * sine(329.628);
        const double padMid = 0.098 * sine(123.471) + 0.072 * sine(146.832) + 0.060 * sine(184.997) + 0.050 * sine(220.000) + 0.042 * sine(277.183);
        const double act0 = (padFam * famMix + padMid * midMix) * (0.72 + 0.28 * sine(0.5))
                          + 0.055 * (sine(73.416) * famMix + sine(61.735) * midMix) * (0.55 + 0.45 * sine(1.5))
                          + rev0 * (0.045 * sine(587.330) + 0.032 * sine(880.000))
                          + static_cast<double>(v.rainLp) * 0.014 * (1.0 - fade((u - 32.0) / 8.0));

        // Act 1 [45..100s): Crystal percussion & optical mirror pulses
        const double bellGate = std::pow(0.5 + 0.5 * sine(3.0), 4.0);
        const double pulseGate = std::pow(0.5 + 0.5 * sine(2.0), 3.0);
        const double act1 = pulseGate * (0.085 * sine(110.000) + 0.045 * sine(220.000))
                          + bellGate * (0.048 * sine(880.000) + 0.036 * (sine(1108.731) * famMix + sine(1046.502) * midMix) + 0.024 * sine(1318.510))
                          + 0.042 * (sine(277.183) * famMix + sine(261.626) * midMix);

        // Act 2 [100..150s): Neural Velvet lush harmonic electronica
        const double swell2 = 0.65 + 0.35 * sine(0.25);
        const double act2 = (0.090 * sine(97.999) + 0.072 * sine(195.998) + 0.058 * (sine(246.942) * famMix + sine(233.082) * midMix)
                           + 0.048 * sine(293.665) + 0.036 * sine(369.994) + 0.025 * sine(587.330)) * swell2;

        // Act 3 [150..195s): Madame N suspense -> warm jazz-lounge bloom across 175.0s midpoint_ecology_reveal
        const double jazzMorph = fade((u - 169.0) / 8.0);
        const double rev1 = std::exp(-std::pow((u - 175.0) / 4.5, 2.0));
        // Integrate sinusoidal frequency modulation analytically. Multiplying a
        // time-varying frequency by absolute film time makes late seeks produce
        // ultrasonic phase slopes, and edition interpolation produces clicks.
        const auto fm = [&](double carrier, double depth, double rate) noexcept {
            return std::sin(kTwoPi * carrier * t - (depth / rate) * std::cos(kTwoPi * rate * t));
        };
        const double susp3 = 0.088 * sine(82.407) + 0.052 * sine(123.471) + 0.036 * fm(246.942, 0.8, 3.5);
        const double jazzFam = 0.092 * fm(103.999, 6.0, 1.75) + 0.068 * sine(195.998) + 0.055 * sine(246.942) + 0.046 * sine(369.994) + 0.034 * sine(440.000);
        const double jazzMid = 0.094 * fm(97.499, 5.0, 1.75) + 0.070 * sine(184.997) + 0.056 * sine(220.000) + 0.048 * sine(329.628) + 0.036 * sine(415.305);
        const double act3 = susp3 * (1.0 - jazzMorph)
                          + (jazzFam * famMix + jazzMid * midMix) * jazzMorph
                          + rev1 * (0.038 * sine(739.989) + 0.026 * sine(987.767));

        // Act 4 [195..245s): Particle Mutiny — Jazz-Funk -> Highlife guitar-bells -> Demoscene Techno
        const double hlMorph = fade((u - 210.0) / 5.0);
        const double tcMorph = fade((u - 227.0) / 5.0);
        const double funkGate = std::pow(0.5 + 0.5 * sine(2.5), 3.0);
        const double polyGate = std::pow(0.5 + 0.5 * std::sin(kTwoPi * 3.75 * t + 0.8), 4.0);
        const double techKick = std::pow(0.5 + 0.5 * sine(2.2), 6.0);
        const double funk4 = funkGate * (0.105 * sine(73.416) + 0.062 * sine(146.832)) + 0.048 * (sine(293.665) * famMix + sine(261.626) * midMix);
        const double high4 = 0.085 * sine(97.999) + polyGate * (0.056 * sine(587.330) + 0.042 * sine(783.991) + 0.030 * sine(987.767));
        const double tech4 = techKick * 0.125 * sine(65.406) + 0.064 * sine(130.813) + polyGate * 0.046 * sine(523.251);
        const double act4 = funk4 * (1.0 - hlMorph) + high4 * hlMorph * (1.0 - tcMorph) + tech4 * tcMorph;

        // Act 5 [245..275s): Signal Hijack broken-beat carrier interference (strictly C1 continuous)
        const double brkGate = std::pow(0.5 + 0.5 * std::sin(kTwoPi * 2.75 * t + 0.5 * sine(0.5)), 4.0);
        const double fmMod = 14.0 * sine(55.0);
        const double act5 = brkGate * 0.105 * sine(73.416)
                          + 0.052 * std::sin(kTwoPi * 220.0 * t + fmMod * 0.08)
                          + 0.034 * sine(659.255);

        // Act 6 [275..360s): Silicon Rebellion grand tutti -> 342.0s serene dawn mirror reveal -> warm tonic end-card hold
        const double dawnMorph = fade((u - 337.0) / 7.5);
        const double cardHold  = fade((u - 350.5) / 3.5);
        const double rev2 = std::exp(-std::pow((u - 342.0) / 4.2, 2.0));
        const double tuttiDrive = std::pow(0.5 + 0.5 * sine(2.0), 3.0);
        const double tutti6 = 0.105 * sine(73.416) * (0.65 + 0.35 * tuttiDrive)
                            + 0.074 * sine(146.832) + 0.062 * sine(220.000)
                            + 0.054 * (sine(277.183) * famMix + sine(261.626) * midMix)
                            + 0.046 * sine(440.000) + bellGate * 0.038 * sine(880.000);
        const double dawn6  = (0.088 * sine(146.832) + 0.066 * sine(184.997) + 0.058 * sine(220.000) + 0.046 * sine(293.665))
                            * (1.0 - 0.32 * cardHold)
                            + rev2 * (0.042 * sine(880.000) + 0.028 * sine(1174.659));
        const double act6 = tutti6 * (1.0 - dawnMorph) + dawn6 * dawnMorph;

        const double total = (act0 * w0 + act1 * w1 + act2 * w2 + act3 * w3 + act4 * w4 + act5 * w5 + act6 * w6) * seam;
        outMono = static_cast<float>(total);
        const float pan = static_cast<float>(0.14 * sine(0.14));
        outLeft  = outMono * (1.0f - pan);
        outRight = outMono * (1.0f + pan);
        return;
    }
    if(durationSamples_ == 8640000) {
        const double u=std::fmod(t,180.0);
        auto fade=[](double x){return .5-.5*std::cos(kPi*std::clamp(x,0.,1.));};
        auto sine=[&](double hz){return std::sin(kTwoPi*hz*t);};
        const double a=fade((u-58)/4),b=fade((u-118)/4),finale=fade((u-169)/3);
        const double rail=std::pow(.5+.5*sine(2.0),8),arp=std::pow(.5+.5*sine(3.0),4);
        const double minor=.10*sine(73.416)+.055*sine(174.614)+.04*sine(261.626)+.025*sine(311.127);
        const double metal=rail*(.025*sine(587.33)+.012*sine(831.0));
        const double solar=(.09*sine(146.832)+.07*sine(184.997)+.06*sine(220)+.04*sine(329.628))*(.75+.25*sine(.125));
        const double aerial=arp*(.035*sine(440)+.025*sine(659.255));
        const double sanctuary=.10*sine(146.832)+.07*sine(184.997)+.065*sine(277.183)+.045*sine(329.628);
        const double bloom=std::exp(-std::pow((u-167.5)/5,2))*(.03*sine(880)+.018*sine(1318.51));
        const double chord=finale*(.07*sine(73.416)+.035*sine(293.665)+.025*sine(440));
        const double seam=fade(u/.05)*fade((180-u)/.05);
        outMono=float(((minor+metal)*(1-a)+(solar+aerial)*a*(1-b)+(sanctuary+bloom+chord)*b)*seam);
        const float pan=float(.12*sine(.1));outLeft=outMono*(1-pan);outRight=outMono*(1+pan);return;
    }
    const double t60 = std::fmod(t, 60.0);

    // 50 ms raised-cosine seam across the 60.0s slice loop boundary
    double loopSeam = 1.0;
    if (t60 < 0.050) {
        loopSeam = 0.5 - 0.5 * std::cos(kPi * (t60 / 0.050));
    } else if (t60 > 59.950) {
        loopSeam = 0.5 - 0.5 * std::cos(kPi * ((60.0 - t60) / 0.050));
    }

    // Smooth crossfade weights across the 3 M1 Taipei shots
    const double w1 = std::clamp((t60 - 18.5) / 3.0, 0.0, 1.0);
    const double w2 = std::clamp((t60 - 38.5) / 3.0, 0.0, 1.0);
    const double sec0Weight = 1.0 - w1 * 0.45;
    const double sec1Weight = w1 * (1.0 - w2 * 0.35);
    const double sec2Weight = w2;

    // Warm Dmaj9 electric piano pad (D3, F#3, A3, C#4, E4)
    const double pulse = 0.62 + 0.38 * std::sin(kTwoPi * 0.5 * t);
    const double rhodes =
        (std::sin(kTwoPi * 146.832 * t) * 0.34 +
         std::sin(kTwoPi * 184.997 * t) * 0.22 +
         std::sin(kTwoPi * 220.000 * t) * 0.24 +
         std::sin(kTwoPi * 277.183 * t) * 0.18 +
         std::sin(kTwoPi * 329.628 * t) * 0.16) * pulse;

    // Low-pass filtered Taipei rain shimmer in Shot 0 (prevents harsh white-noise sample steps)
    v.lfsr = (v.lfsr >> 1u) ^ (((v.lfsr & 1u) != 0u) ? 0xB400u : 0u);
    const float white = static_cast<float>((static_cast<double>(v.lfsr & 0xFFFFu) / 32768.0) - 1.0);
    v.rainLp += 0.18f * (white - v.rainLp);
    const double rainTex = static_cast<double>(v.rainLp) * 0.032 * (1.0 - w1);

    // Pulsing D2 (73.416 Hz) / A2 (110.0 Hz) architectural bass ostinato in Shot 1+
    const double bassGate = 0.50 + 0.50 * std::max(0.0, std::sin(kTwoPi * 2.0 * t));
    const double bass =
        (std::sin(kTwoPi * 73.416 * t) * 0.52 +
         std::sin(kTwoPi * 110.000 * t) * 0.28) * bassGate;

    // Crystalline Glass Orchid high bell shimmer (A5, C#6, E6, F#6) peaking across 45s reveal
    const double crystalEnv = 0.55 + 0.45 * std::sin(kTwoPi * 1.0 * t);
    const double crystal =
        (std::sin(kTwoPi * 880.000 * t) * 0.22 +
         std::sin(kTwoPi * 1108.731 * t) * 0.18 +
         std::sin(kTwoPi * 1318.510 * t) * 0.16 +
         std::sin(kTwoPi * 1479.978 * t) * 0.12) * crystalEnv;

    const float mono = static_cast<float>(
        (rhodes * sec0Weight + rainTex + bass * sec1Weight + crystal * sec2Weight) * 0.48 * loopSeam);
    const float pan = static_cast<float>(0.16 * std::sin(kTwoPi * 0.18 * t));

    outMono  = mono;
    outLeft  = mono * (1.0f - pan);
    outRight = mono * (1.0f + pan);
}

uint32_t AudioSynthesizer48k::renderFrames(
    float* outInterleavedStereo,
    uint32_t frameCount,
    bool paused,
    bool muted,
    float masterVolume01,
    AudioBandSnapshot* outBands) noexcept {
    if (!outInterleavedStereo || frameCount == 0) {
        if (outBands) *outBands = {};
        return 0;
    }

    constexpr float kAlpha180  = 0.023288f; // 180 Hz 1-pole LPF @ 48 kHz
    constexpr float kAlpha2400 = 0.269605f; // 2400 Hz 1-pole LPF @ 48 kHz

    const float clampedVol = std::isfinite(masterVolume01) ? std::clamp(masterVolume01, 0.0f, 1.0f) : 0.0f;
    const float desiredGain = (paused || muted) ? 0.0f : (clampedVol * 0.14f);
    if ( std::fabs(desiredGain - rampTargetGain_) > 1e-6f) {
        rampStartGain_ = currentGain_;
        rampTargetGain_ = desiredGain;
        gainRampPos_ = 0;
    }

    float sumSqAll  = 0.0f;
    float sumSqLow  = 0.0f;
    float sumSqMid  = 0.0f;
    float sumSqHigh = 0.0f;

    for (uint32_t i = 0; i < frameCount; ++i) {
        // C1 raised-cosine master gain envelope (clickless pause, resume, mute, and volume transitions)
        if (gainRampPos_ < kGainRampSamples) {
            const float u = static_cast<float>(gainRampPos_ + 1u) / static_cast<float>(kGainRampSamples);
            const float s = 0.5f * (1.0f - std::cos(static_cast<float>(kPi) * u));
            currentGain_ = rampStartGain_ + (rampTargetGain_ - rampStartGain_) * s;
            ++gainRampPos_;
        } else {
            currentGain_ = rampTargetGain_;
        }

        // Smooth 12 ms harmonic blend when switching Family <-> Midnight mid-playback
        const float targetMid = (targetEdition_ == Edition::Midnight) ? 1.0f : 0.0f;
        editionMidnightMix_ += (targetMid - editionMidnightMix_) * (1.0f / static_cast<float>(kGainRampSamples));

        // Advance voices while unpaused OR while still audibly ramping down to zero on pause!
        const bool shouldAdvanceVoices = (!paused) || (currentGain_ > 1e-6f);

        float mixL = 0.0f;
        float mixR = 0.0f;
        float mixMono = 0.0f;

        if (shouldAdvanceVoices) {
            for (auto& v : voices_) {
                if (!v.active) continue;

                float w = v.currentWeight;
                if (v.fadingOut) {
                    if (v.fadePos >= kCrossfadeSamples) {
                        v.active = false;
                        v.currentWeight = 0.0f;
                        continue;
                    }
                    const float u = static_cast<float>(v.fadePos) / static_cast<float>(kCrossfadeSamples);
                    const float s = 0.5f * (1.0f - std::cos(static_cast<float>(kPi) * u));
                    w = v.fadeStartWeight * (1.0f - s);
                    v.currentWeight = w;
                    ++v.fadePos;
                } else if (v.fadePos < kCrossfadeSamples) {
                    const float u = static_cast<float>(v.fadePos) / static_cast<float>(kCrossfadeSamples);
                    const float s = 0.5f * (1.0f - std::cos(static_cast<float>(kPi) * u));
                    w = s;
                    v.currentWeight = w;
                    ++v.fadePos;
                } else {
                    w = 1.0f;
                    v.currentWeight = 1.0f;
                }

                float vL = 0.0f, vR = 0.0f, vM = 0.0f;
                evaluateVoiceSample(v, vL, vR, vM);
                mixL    += vL * w;
                mixR    += vR * w;
                mixMono += vM * w;
                ++v.phaseSample;
            }
        }

        if (residualPos_ < 64u) {
            const float u = static_cast<float>(residualPos_) / 64.0f;
            const float s = 0.5f * (1.0f + std::cos(static_cast<float>(kPi) * u));
            mixL    += residualLeft_ * s;
            mixR    += residualRight_ * s;
            mixMono += residualMono_ * s;
            ++residualPos_;
            if (residualPos_ >= 64u) {
                residualLeft_ = residualRight_ = residualMono_ = 0.0f;
            }
        }

        outInterleavedStereo[i * 2 + 0] = mixL * currentGain_;
        outInterleavedStereo[i * 2 + 1] = mixR * currentGain_;
        if (currentGain_ > 1e-6f) {
            hasRenderedAnyAudio_ = true;
        }

        const float envMono = paused ? (mixMono * ((clampedVol > 1e-5f) ? (currentGain_ / (clampedVol * 0.14f)) : 0.0f)) : mixMono;
        lp180_  += kAlpha180 * (envMono - lp180_);
        lp2400_ += kAlpha2400 * (envMono - lp2400_);
        const float lowBand  = lp180_;
        const float midBand  = lp2400_ - lp180_;
        const float highBand = envMono - lp2400_;

        sumSqAll  += envMono * envMono;
        sumSqLow  += lowBand * lowBand;
        sumSqMid  += midBand * midBand;
        sumSqHigh += highBand * highBand;
    }

    if (outBands) {
        const float invN = 1.0f / static_cast<float>(frameCount);
        outBands->rms  = std::clamp(std::sqrt(sumSqAll  * invN) * 2.2f, 0.0f, 1.0f);
        outBands->low  = std::clamp(std::sqrt(sumSqLow  * invN) * 3.2f, 0.0f, 1.0f);
        outBands->mid  = std::clamp(std::sqrt(sumSqMid  * invN) * 2.8f, 0.0f, 1.0f);
        outBands->high = std::clamp(std::sqrt(sumSqHigh * invN) * 4.5f, 0.0f, 1.0f);
    }

    return paused ? 0u : frameCount;
}

// ============================================================================
// AudioEngine — WASAPI Presentation Clock + Endpoint Migration
// ============================================================================

AudioEngine::AudioEngine() {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = (freq.QuadPart > 0) ? freq.QuadPart : 10000000;
}

AudioEngine::~AudioEngine() {
    shutdown();
}

void AudioEngine::notifyDefaultEndpointChangedFromCallback(const wchar_t* /*newDeviceId*/) noexcept {
    migrationPending_.store(true, std::memory_order_release);
}

bool AudioEngine::initialize(bool forceFail) {
    shutdown();
    forceFailed_ = forceFail;
    migrationCount_.store(0, std::memory_order_relaxed);
    consecutiveMigrationFailures_ = 0;
    lastMigrationAttemptQpc_ = 0;
    submittedFrames48k_.store(0, std::memory_order_relaxed);
    monotonicPresented48k_.store(0, std::memory_order_relaxed);

    if (forceFail) {
        fallbackReason_ = "Audio unavailable (forced silent fallback via CLI/fault injection)";
        healthy_.store(false, std::memory_order_release);
        return false;
    }

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comValid = SUCCEEDED(hrCo) || hrCo == RPC_E_CHANGED_MODE;
    if (!comValid) {
        fallbackReason_ = "COM initialization failed for WASAPI";
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    const HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        fallbackReason_ = "WASAPI MMDeviceEnumerator unavailable";
        return false;
    }

    auto* notif = new EndpointNotificationClient(this);
    if (FAILED(enumerator->RegisterEndpointNotificationCallback(notif))) {
        notif->detachOwner();
        notif->Release();
        notif = nullptr;
    }
    enumeratorPtr_ = enumerator.Detach();
    notifClientPtr_ = notif;

    return openDefaultEndpoint(0, 0);
}

bool AudioEngine::openDefaultEndpoint(uint64_t initialSeedSample, int64_t initialPlayedCounter) {
    stopStreamOnly();

    auto* enumerator = static_cast<IMMDeviceEnumerator*>(enumeratorPtr_);
    if (!enumerator) {
        fallbackReason_ = "WASAPI MMDeviceEnumerator unavailable";
        return false;
    }

    ComPtr<IMMDevice> device;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        fallbackReason_ = "No active default audio render endpoint available";
        return false;
    }

    LPWSTR rawId = nullptr;
    activeEndpointId_.clear();
    if (SUCCEEDED(device->GetId(&rawId)) && rawId) {
        activeEndpointId_ = rawId;
        CoTaskMemFree(rawId);
    }

    endpointName_.clear();
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR) {
            endpointName_ = propWideToUtf8(varName.pwszVal);
        }
        PropVariantClear(&varName);
    }
    if (endpointName_.empty()) {
        endpointName_ = "Default Windows Audio Endpoint";
    }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr) || !client) {
        fallbackReason_ = "Failed to activate WASAPI IAudioClient";
        return false;
    }

    WAVEFORMATEXTENSIBLE wfx{};
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = static_cast<WORD>(kChannels);
    wfx.Format.nSamplesPerSec = static_cast<DWORD>(kTargetSampleRate);
    wfx.Format.wBitsPerSample = 32;
    wfx.Format.nBlockAlign = static_cast<WORD>((wfx.Format.nChannels * wfx.Format.wBitsPerSample) / 8);
    wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
    wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    wfx.Samples.wValidBitsPerSample = 32;
    wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wfx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    const DWORD streamFlags =
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    const REFERENCE_TIME bufferDuration100ns = 200000; // 20 ms shared-mode buffer
    hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        bufferDuration100ns,
        0,
        &wfx.Format,
        nullptr);
    if (FAILED(hr)) {
        fallbackReason_ = "IAudioClient::Initialize (48 kHz float stereo) failed";
        return false;
    }

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ev) {
        fallbackReason_ = "Failed to create WASAPI event handle";
        return false;
    }

    if (FAILED(client->SetEventHandle(ev))) {
        CloseHandle(ev);
        fallbackReason_ = "IAudioClient::SetEventHandle failed";
        return false;
    }

    UINT32 bufFrames = 0;
    if (FAILED(client->GetBufferSize(&bufFrames)) || bufFrames == 0) {
        CloseHandle(ev);
        fallbackReason_ = "IAudioClient::GetBufferSize failed";
        return false;
    }

    ComPtr<IAudioRenderClient> renderClient;
    if (FAILED(client->GetService(IID_PPV_ARGS(&renderClient))) || !renderClient) {
        CloseHandle(ev);
        fallbackReason_ = "IAudioClient::GetService(IAudioRenderClient) failed";
        return false;
    }

    ComPtr<IAudioClock> audioClock;
    UINT64 clockFreq = 0;
    if (SUCCEEDED(client->GetService(IID_PPV_ARGS(&audioClock))) && audioClock) {
        if (FAILED(audioClock->GetFrequency(&clockFreq)) || clockFreq == 0) {
            audioClock.Reset();
            clockFreq = 0;
        }
    }

    // Prime initial buffer with silence before Start(); chunk ring tracks these primed frames
    // so readPlayedSamples48k only begins advancing once sample 0 actually hits the speaker.
    BYTE* initData = nullptr;
    if (SUCCEEDED(renderClient->GetBuffer(bufFrames, &initData)) && initData) {
        renderClient->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    if (FAILED(client->Start())) {
        CloseHandle(ev);
        fallbackReason_ = "IAudioClient::Start failed";
        return false;
    }

    audioEventHandle_ = ev;
    bufferFrameCount_ = bufFrames;
    mixSampleRate_ = kTargetSampleRate;
    audioClockFreq_ = clockFreq;
    usesHwClock_.store(audioClock != nullptr && clockFreq > 0, std::memory_order_relaxed);
    devicePtr_ = device.Detach();
    clientPtr_ = client.Detach();
    renderClientPtr_ = renderClient.Detach();
    audioClockPtr_ = audioClock.Detach();

    initialPlayedCounter_ = initialPlayedCounter;
    submittedFrames48k_.store(initialPlayedCounter, std::memory_order_relaxed);
    monotonicPresented48k_.store(initialPlayedCounter, std::memory_order_relaxed);

    clockSeq_.store(0, std::memory_order_relaxed);
    clockBasePresented48k_.store(initialPlayedCounter, std::memory_order_relaxed);
    clockBaseQpcTicks_.store(queryQpcTicksNow(), std::memory_order_relaxed);
    clockMaxSubmitted48k_.store(initialPlayedCounter, std::memory_order_relaxed);
    clockAdvancing_.store(false, std::memory_order_relaxed);

    pendingPhaseSample_.store(initialSeedSample, std::memory_order_relaxed);
    phaseResetGen_.store(0, std::memory_order_relaxed);
    bandSeq_.store(0, std::memory_order_relaxed);
    bandRms_.store(0.0f, std::memory_order_relaxed);
    bandLow_.store(0.0f, std::memory_order_relaxed);
    bandMid_.store(0.0f, std::memory_order_relaxed);
    bandHigh_.store(0.0f, std::memory_order_relaxed);
    cachedReaderBands_ = {};
    migrationPending_.store(false, std::memory_order_release);
    stopRequested_.store(false, std::memory_order_release);
    healthy_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    worker_ = std::thread(&AudioEngine::audioThreadMain, this);
    return true;
}

void AudioEngine::stopStreamOnly() noexcept {
    stopRequested_.store(true, std::memory_order_release);
    if (audioEventHandle_) {
        SetEvent(static_cast<HANDLE>(audioEventHandle_));
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false, std::memory_order_release);
    healthy_.store(false, std::memory_order_release);

    if (clientPtr_) {
        static_cast<IAudioClient*>(clientPtr_)->Stop();
    }
    if (audioClockPtr_) {
        static_cast<IAudioClock*>(audioClockPtr_)->Release();
        audioClockPtr_ = nullptr;
    }
    if (renderClientPtr_) {
        static_cast<IAudioRenderClient*>(renderClientPtr_)->Release();
        renderClientPtr_ = nullptr;
    }
    if (clientPtr_) {
        static_cast<IAudioClient*>(clientPtr_)->Release();
        clientPtr_ = nullptr;
    }
    if (devicePtr_) {
        static_cast<IMMDevice*>(devicePtr_)->Release();
        devicePtr_ = nullptr;
    }
    if (audioEventHandle_) {
        CloseHandle(static_cast<HANDLE>(audioEventHandle_));
        audioEventHandle_ = nullptr;
    }
}

void AudioEngine::shutdown() noexcept {
    stopStreamOnly();

    auto* enumerator = static_cast<IMMDeviceEnumerator*>(enumeratorPtr_);
    auto* notif = static_cast<EndpointNotificationClient*>(notifClientPtr_);
    if (notif) {
        notif->detachOwner();
        if (enumerator) {
            enumerator->UnregisterEndpointNotificationCallback(notif);
        }
        notif->Release();
        notifClientPtr_ = nullptr;
    }
    if (enumerator) {
        enumerator->Release();
        enumeratorPtr_ = nullptr;
    }
    activeEndpointId_.clear();
}

bool AudioEngine::pollEndpointMigration(uint64_t resumeTimelineSample) {
    if (forceFailed_ || !enumeratorPtr_) {
        return false;
    }

    const bool pending = migrationPending_.load(std::memory_order_acquire);
    const bool currentlyHealthy = healthy_.load(std::memory_order_acquire);
    if (!pending && currentlyHealthy) {
        return false;
    }

    // Check whether default endpoint actually differs if current stream is still healthy
    auto* enumerator = static_cast<IMMDeviceEnumerator*>(enumeratorPtr_);
    if (currentlyHealthy && pending) {
        ComPtr<IMMDevice> defaultDev;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDev)) && defaultDev) {
            LPWSTR rawId = nullptr;
            std::wstring newId;
            if (SUCCEEDED(defaultDev->GetId(&rawId)) && rawId) {
                newId = rawId;
                CoTaskMemFree(rawId);
            }
            if (!newId.empty() && newId == activeEndpointId_) {
                migrationPending_.store(false, std::memory_order_release);
                return false;
            }
        }
    }

    if (consecutiveMigrationFailures_ >= 8u) {
        return false;
    }

    const int64_t nowQpc = queryQpcTicksNow();
    if (lastMigrationAttemptQpc_ != 0) {
        const double sinceLastSec =
            static_cast<double>(nowQpc - lastMigrationAttemptQpc_) / static_cast<double>(std::max<int64_t>(1, qpcFrequency_));
        if (sinceLastSec < 0.50) {
            return false;
        }
    }
    lastMigrationAttemptQpc_ = nowQpc;

    const int64_t carryCounter = monotonicPresented48k_.load(std::memory_order_acquire);
    if (openDefaultEndpoint(resumeTimelineSample, carryCounter)) {
        consecutiveMigrationFailures_ = 0;
        migrationCount_.fetch_add(1, std::memory_order_relaxed);
        BOO_LOG_INFO("Audio", "Migrated WASAPI playback cleanly to '" + endpointName_ + "'.");
        return true;
    }

    ++consecutiveMigrationFailures_;
    return false;
}

void AudioEngine::setPaused(bool paused) noexcept {
    paused_.store(paused, std::memory_order_relaxed);
}

void AudioEngine::setMuted(bool muted) noexcept {
    muted_.store(muted, std::memory_order_relaxed);
}

void AudioEngine::setMasterVolume(float volume01) noexcept {
    masterVolume_.store((std::isfinite(volume01) ? std::clamp(volume01, 0.0f, 1.0f) : 0.0f), std::memory_order_relaxed);
}

void AudioEngine::resetPhase(uint64_t seedSample) noexcept {
    pendingPhaseSample_.store(seedSample, std::memory_order_relaxed);
    phaseResetGen_.fetch_add(1, std::memory_order_release);
}

bool AudioEngine::readPlayedSamples48k(int64_t& outSamples48k) const noexcept {
    if (!healthy_.load(std::memory_order_acquire)) {
        return false;
    }

    int64_t basePresented = 0;
    int64_t baseQpc = 0;
    int64_t maxSubmitted = 0;
    bool advancing = false;
    bool gotSnap = false;

    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t seq0 = clockSeq_.load(std::memory_order_acquire);
        if ((seq0 & 1u) != 0u) {
            continue;
        }
        basePresented = clockBasePresented48k_.load(std::memory_order_relaxed);
        baseQpc       = clockBaseQpcTicks_.load(std::memory_order_relaxed);
        maxSubmitted  = clockMaxSubmitted48k_.load(std::memory_order_relaxed);
        advancing     = clockAdvancing_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t seq1 = clockSeq_.load(std::memory_order_relaxed);
        if (seq0 == seq1) {
            gotSnap = true;
            break;
        }
    }

    int64_t candidate = monotonicPresented48k_.load(std::memory_order_acquire);
    if (gotSnap) {
        int64_t interp = basePresented;
        if (advancing) {
            const int64_t elapsedTicks = std::max<int64_t>(0, queryQpcTicksNow() - baseQpc);
            // Cap inter-callback QPC interpolation to 25 ms (1,200 frames) and never exceed submitted frames
            const int64_t deltaFrames = std::min<int64_t>(1200, scaleTicksToFrames48k(elapsedTicks, qpcFrequency_));
            interp = std::min<int64_t>(maxSubmitted, basePresented + deltaFrames);
        }
        candidate = std::max<int64_t>(candidate, interp);
    }

    int64_t prev = monotonicPresented48k_.load(std::memory_order_relaxed);
    while (candidate > prev &&
           !monotonicPresented48k_.compare_exchange_weak(
               prev, candidate, std::memory_order_release, std::memory_order_relaxed)) {
    }
    outSamples48k = std::max<int64_t>(prev, candidate);
    return true;
}

double AudioEngine::estimatedBufferLatencyMs() const noexcept {
    const int64_t sub = submittedFrames48k_.load(std::memory_order_acquire);
    const int64_t pres = monotonicPresented48k_.load(std::memory_order_acquire);
    const int64_t lagFrames = std::max<int64_t>(0, sub - pres);
    return (static_cast<double>(lagFrames) * 1000.0) / static_cast<double>(kTargetSampleRate);
}

AudioBandSnapshot AudioEngine::currentBandSnapshot() const noexcept {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t seq0 = bandSeq_.load(std::memory_order_acquire);
        if ((seq0 & 1u) != 0u) {
            continue;
        }
        AudioBandSnapshot snap{};
        snap.rms  = bandRms_.load(std::memory_order_relaxed);
        snap.low  = bandLow_.load(std::memory_order_relaxed);
        snap.mid  = bandMid_.load(std::memory_order_relaxed);
        snap.high = bandHigh_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t seq1 = bandSeq_.load(std::memory_order_relaxed);
        if (seq0 == seq1) {
            cachedReaderBands_ = snap;
            return snap;
        }
    }
    return cachedReaderBands_;
}

void AudioEngine::audioThreadMain() noexcept {
    // Section 2.4 & 3.3: Real-time audio callback loop.
    // Strictly NO heap allocations, NO mutexes, NO logging, NO GPU/inference waits.
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    auto* client       = static_cast<IAudioClient*>(clientPtr_);
    auto* renderClient = static_cast<IAudioRenderClient*>(renderClientPtr_);
    auto* audioClock   = static_cast<IAudioClock*>(audioClockPtr_);
    HANDLE waitEvent   = static_cast<HANDLE>(audioEventHandle_);

    struct ChunkRecord {
        int64_t streamStart = 0;
        int64_t streamEnd   = 0;
        int64_t playedStart = 0;
        int64_t playedEnd   = 0;
    };
    constexpr uint32_t kRingCap = 64u;
    std::array<ChunkRecord, kRingCap> chunkRing{};
    uint32_t chunkWriteIdx = 0;
    uint32_t chunkCount = 0;

    // Record the initial primed silent buffer so sample 0 aligns with physical speaker output
    int64_t streamWriteCursor = static_cast<int64_t>(bufferFrameCount_);
    int64_t playedWriteCursor = initialPlayedCounter_;
    chunkRing[0] = {0, streamWriteCursor, playedWriteCursor, playedWriteCursor};
    chunkWriteIdx = 1u;
    chunkCount = 1u;

    auto mapHwStreamToPlayed = [&](int64_t hwStreamFrames48k) noexcept -> int64_t {
        if (chunkCount == 0u) return playedWriteCursor;
        const uint32_t inspect = std::min<uint32_t>(chunkCount, kRingCap);
        for (uint32_t k = 0; k < inspect; ++k) {
            const uint32_t idx = (chunkWriteIdx + kRingCap - 1u - k) & (kRingCap - 1u);
            const ChunkRecord& c = chunkRing[idx];
            if (hwStreamFrames48k >= c.streamEnd) {
                return c.playedEnd;
            }
            if (hwStreamFrames48k >= c.streamStart) {
                const int64_t streamSpan = std::max<int64_t>(1, c.streamEnd - c.streamStart);
                const int64_t playedSpan = c.playedEnd - c.playedStart;
                return c.playedStart + ((hwStreamFrames48k - c.streamStart) * playedSpan) / streamSpan;
            }
        }
        const uint32_t oldestIdx = (chunkWriteIdx + kRingCap - inspect) & (kRingCap - 1u);
        return chunkRing[oldestIdx].playedStart;
    };

    auto publishPresentationClock = [&](bool isPausedNow, UINT32 fallbackPadding) noexcept {
        int64_t hwStreamFrames48k = std::max<int64_t>(0, streamWriteCursor - static_cast<int64_t>(fallbackPadding));
        int64_t sampleQpc = queryQpcTicksNow();
        if (audioClock && audioClockFreq_ > 0) {
            UINT64 devPos = 0;
            UINT64 clockQpc100ns = 0;
            const HRESULT hrPos = audioClock->GetPosition(&devPos, &clockQpc100ns);
            if (SUCCEEDED(hrPos)) {
                const uint64_t wholeSec = devPos / audioClockFreq_;
                const uint64_t rem      = devPos % audioClockFreq_;
                hwStreamFrames48k = static_cast<int64_t>(
                    wholeSec * static_cast<uint64_t>(kTargetSampleRate) +
                    (rem * static_cast<uint64_t>(kTargetSampleRate)) / audioClockFreq_);
            } else if (hrPos == AUDCLNT_E_DEVICE_INVALIDATED) {
                migrationPending_.store(true, std::memory_order_release);
            }
        }
        const int64_t presentedPlayed = std::clamp<int64_t>(
            mapHwStreamToPlayed(hwStreamFrames48k),
            initialPlayedCounter_,
            playedWriteCursor);
        const bool advancing = (!isPausedNow) && (presentedPlayed < playedWriteCursor);

        clockSeq_.fetch_add(1u, std::memory_order_release);
        clockBasePresented48k_.store(presentedPlayed, std::memory_order_relaxed);
        clockBaseQpcTicks_.store(sampleQpc, std::memory_order_relaxed);
        clockMaxSubmitted48k_.store(playedWriteCursor, std::memory_order_relaxed);
        clockAdvancing_.store(advancing, std::memory_order_relaxed);
        clockSeq_.fetch_add(1u, std::memory_order_release);
    };

    AudioSynthesizer48k synth;
    synth.setDurationSamples(durationSamples_);
    synth.setEdition(static_cast<Edition>(edition_.load(std::memory_order_relaxed)));
    synth.reset(pendingPhaseSample_.load(std::memory_order_relaxed), 0.0f);
    uint64_t appliedPhaseGen = phaseResetGen_.load(std::memory_order_acquire);

    while (!stopRequested_.load(std::memory_order_acquire)) {
        synth.setEdition(static_cast<Edition>(edition_.load(std::memory_order_relaxed)));
        const DWORD waitRes = WaitForSingleObject(waitEvent, 20);
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }
        if (waitRes != WAIT_OBJECT_0 && waitRes != WAIT_TIMEOUT) {
            migrationPending_.store(true, std::memory_order_release);
            healthy_.store(false, std::memory_order_release);
            break;
        }

        UINT32 padding = 0;
        const HRESULT hrPad = client->GetCurrentPadding(&padding);
        if (FAILED(hrPad)) {
            migrationPending_.store(true, std::memory_order_release);
            healthy_.store(false, std::memory_order_release);
            break;
        }

        const bool isPaused = paused_.load(std::memory_order_relaxed);
        const UINT32 availableFrames = (bufferFrameCount_ > padding) ? (bufferFrameCount_ - padding) : 0;
        if (availableFrames == 0) {
            publishPresentationClock(isPaused, padding);
            continue;
        }

        BYTE* rawBuf = nullptr;
        if (FAILED(renderClient->GetBuffer(availableFrames, &rawBuf)) || !rawBuf) {
            migrationPending_.store(true, std::memory_order_release);
            healthy_.store(false, std::memory_order_release);
            break;
        }

        // Consume any pending transport seek/loop phase reset atomically via C1 crossfader
        const uint64_t reqGen = phaseResetGen_.load(std::memory_order_acquire);
        if (reqGen != appliedPhaseGen) {
            const uint64_t targetSample = pendingPhaseSample_.load(std::memory_order_relaxed);
            appliedPhaseGen = reqGen;
            synth.seekTo(targetSample);
        }

        const bool isMuted = muted_.load(std::memory_order_relaxed);
        const float masterVol = masterVolume_.load(std::memory_order_relaxed);

        AudioBandSnapshot bands{};
        const uint32_t timelineAdvanced = synth.renderFrames(
            reinterpret_cast<float*>(rawBuf),
            availableFrames,
            isPaused,
            isMuted,
            masterVol,
            &bands);

        rmsEnvelope_.store(bands.rms, std::memory_order_relaxed);

        bandSeq_.fetch_add(1u, std::memory_order_release);
        bandRms_.store(bands.rms, std::memory_order_relaxed);
        bandLow_.store(bands.low, std::memory_order_relaxed);
        bandMid_.store(bands.mid, std::memory_order_relaxed);
        bandHigh_.store(bands.high, std::memory_order_relaxed);
        bandSeq_.fetch_add(1u, std::memory_order_release);

        if (FAILED(renderClient->ReleaseBuffer(availableFrames, 0))) {
            migrationPending_.store(true, std::memory_order_release);
            healthy_.store(false, std::memory_order_release);
            break;
        }

        const int64_t nextStreamCursor = streamWriteCursor + static_cast<int64_t>(availableFrames);
        const int64_t nextPlayedCursor = playedWriteCursor + static_cast<int64_t>(timelineAdvanced);
        chunkRing[chunkWriteIdx & (kRingCap - 1u)] = {
            streamWriteCursor,
            nextStreamCursor,
            playedWriteCursor,
            nextPlayedCursor
        };
        ++chunkWriteIdx;
        if (chunkCount < kRingCap) {
            ++chunkCount;
        }
        streamWriteCursor = nextStreamCursor;
        playedWriteCursor = nextPlayedCursor;
        submittedFrames48k_.store(playedWriteCursor, std::memory_order_release);

        publishPresentationClock(isPaused, padding + availableFrames);
    }

    if (mmcssHandle) {
        AvRevertMmThreadCharacteristics(mmcssHandle);
    }
    running_.store(false, std::memory_order_release);
}

} // namespace boo
