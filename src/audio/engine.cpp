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
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

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
// Dynamic Media Foundation MP3 / WAV / FLAC / M4A / AAC Decoder -> 48k Stereo
// ============================================================================

bool loadCustomAudioFile48k(
    const std::filesystem::path& filePath,
    CustomAudioTrack48k& outTrack,
    std::string* outError,
    bool injectFault,
    const std::atomic<bool>* cancelRequested,
    bool compactPcm16) {
    outTrack = {};
    if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
        if (outError) *outError = "Audio decode cancelled";
        return false;
    }
    const std::string safeName = redactUserPath(filePath.string());
    if (injectFault) {
        if (outError) *outError = "Audio file fault injected (" + safeName + ")";
        return false;
    }
    if (filePath.empty()) {
        if (outError) *outError = "Empty audio file path";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec) || !std::filesystem::is_regular_file(filePath, ec)) {
        if (outError) *outError = "Audio file not found: " + safeName;
        return false;
    }
    const uintmax_t fBytes = std::filesystem::file_size(filePath, ec);
    if (ec || fBytes == 0 || fBytes > (512ULL * 1024ULL * 1024ULL)) {
        if (outError) *outError = "Invalid or oversized audio file: " + safeName;
        return false;
    }

    using PFN_MFCreateSourceReaderFromURL = HRESULT(STDAPICALLTYPE*)(
        LPCWSTR pwszURL, IMFAttributes* pAttributes, IMFSourceReader** ppSourceReader);

    HMODULE hMfRw = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hMfRw) {
        if (outError) *outError = "Windows Media Foundation mfreadwrite.dll unavailable";
        return false;
    }
    auto pfnCreateReader = reinterpret_cast<PFN_MFCreateSourceReaderFromURL>(
        GetProcAddress(hMfRw, "MFCreateSourceReaderFromURL"));
    if (!pfnCreateReader) {
        FreeLibrary(hMfRw);
        if (outError) *outError = "MFCreateSourceReaderFromURL entry point unavailable";
        return false;
    }

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needCoUninit = SUCCEEDED(hrCo);
    bool ok = false;

    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        ComPtr<IMFSourceReader> reader;
        const std::wstring wUrl = filePath.wstring();
        if (SUCCEEDED(pfnCreateReader(wUrl.c_str(), nullptr, &reader)) && reader) {
            (void)reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            if (SUCCEEDED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE))) {
                bool configured = false;

                // Attempt 1: Ask Media Foundation to decode & resample directly to 48 kHz stereo float32
                {
                    ComPtr<IMFMediaType> req48k;
                    if (SUCCEEDED(MFCreateMediaType(&req48k)) && req48k) {
                        req48k->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                        req48k->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
                        req48k->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2u);
                        req48k->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000u);
                        req48k->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32u);
                        req48k->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8u);
                        req48k->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000u * 8u);
                        if (SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, req48k.Get()))) {
                            configured = true;
                        }
                    }
                }
                // Attempt 2: Native rate/channel IEEE Float
                if (!configured) {
                    ComPtr<IMFMediaType> reqFlt;
                    if (SUCCEEDED(MFCreateMediaType(&reqFlt)) && reqFlt) {
                        reqFlt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                        reqFlt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
                        if (SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, reqFlt.Get()))) {
                            configured = true;
                        }
                    }
                }
                // Attempt 3: Native rate/channel 16-bit PCM
                if (!configured) {
                    ComPtr<IMFMediaType> reqPcm;
                    if (SUCCEEDED(MFCreateMediaType(&reqPcm)) && reqPcm) {
                        reqPcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                        reqPcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                        reqPcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16u);
                        if (SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, reqPcm.Get()))) {
                            configured = true;
                        }
                    }
                }

                ComPtr<IMFMediaType> curType;
                if (configured &&
                    SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &curType)) &&
                    curType) {
                    GUID subType = GUID_NULL;
                    UINT32 srcRate = 48000u;
                    UINT32 srcCh = 2u;
                    UINT32 srcBits = 32u;
                    (void)curType->GetGUID(MF_MT_SUBTYPE, &subType);
                    (void)curType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &srcRate);
                    (void)curType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &srcCh);
                    (void)curType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &srcBits);

                    const bool isFloat = (subType == MFAudioFormat_Float && srcBits == 32u);
                    const bool isPcm16 = (subType == MFAudioFormat_PCM && srcBits == 16u);
                    if ((isFloat || isPcm16) && srcRate >= 8000u && srcRate <= 192000u && srcCh >= 1u && srcCh <= 8u) {
                        const uint64_t maxSrcFrames = static_cast<uint64_t>(srcRate) * 20ULL * 60ULL; // 20-minute cap
                        std::vector<float> rawStereo;
                        rawStereo.reserve(static_cast<size_t>(srcRate) * 180u * 2u);

                        const size_t bytesPerSample = isFloat ? 4u : 2u;
                        const size_t frameStrideBytes = bytesPerSample * static_cast<size_t>(srcCh);

                        bool cancelled = false;
                        while ((rawStereo.size() / 2u) < maxSrcFrames) {
                            if (cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
                                cancelled = true;
                                break;
                            }
                            DWORD flags = 0;
                            ComPtr<IMFSample> sample;
                            const HRESULT hrRead = reader->ReadSample(
                                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                0, nullptr, &flags, nullptr, &sample);
                            if (FAILED(hrRead) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
                                break;
                            }
                            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 && !sample) {
                                break;
                            }
                            if (sample) {
                                ComPtr<IMFMediaBuffer> buf;
                                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
                                    BYTE* ptr = nullptr;
                                    DWORD curLen = 0;
                                    if (SUCCEEDED(buf->Lock(&ptr, nullptr, &curLen)) && ptr && curLen >= frameStrideBytes) {
                                        const size_t nFrames = std::min<size_t>(
                                            curLen / frameStrideBytes,
                                            static_cast<size_t>(maxSrcFrames - (rawStereo.size() / 2u)));
                                        for (size_t f = 0; f < nFrames; ++f) {
                                            float sL = 0.0f, sR = 0.0f;
                                            if (isFloat) {
                                                const float* fp = reinterpret_cast<const float*>(ptr + f * frameStrideBytes);
                                                sL = fp[0];
                                                sR = (srcCh >= 2u) ? fp[1] : fp[0];
                                            } else {
                                                const int16_t* ip = reinterpret_cast<const int16_t*>(ptr + f * frameStrideBytes);
                                                sL = static_cast<float>(ip[0]) * (1.0f / 32768.0f);
                                                sR = static_cast<float>((srcCh >= 2u) ? ip[1] : ip[0]) * (1.0f / 32768.0f);
                                            }
                                            if (!std::isfinite(sL)) sL = 0.0f;
                                            if (!std::isfinite(sR)) sR = 0.0f;
                                            rawStereo.push_back(std::clamp(sL, -1.25f, 1.25f));
                                            rawStereo.push_back(std::clamp(sR, -1.25f, 1.25f));
                                        }
                                        buf->Unlock();
                                    }
                                }
                            }
                            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                                break;
                            }
                        }

                        const uint64_t srcFrames = cancelled ? 0ULL : (rawStereo.size() / 2u);
                        if (srcFrames >= 480u) {
                            if (srcRate == 48000u) {
                                outTrack.interleavedStereo = std::move(rawStereo);
                                outTrack.frameCount = srcFrames;
                            } else {
                                const uint64_t dstFrames = std::max<uint64_t>(
                                    480u, (srcFrames * 48000ULL) / static_cast<uint64_t>(srcRate));
                                outTrack.interleavedStereo.resize(static_cast<size_t>(dstFrames) * 2u);
                                const double ratio = static_cast<double>(srcRate) / 48000.0;
                                auto atCh = [&](int64_t idx, size_t ch) noexcept -> float {
                                    const uint64_t cIdx = static_cast<uint64_t>(
                                        std::clamp<int64_t>(idx, 0, static_cast<int64_t>(srcFrames) - 1));
                                    return rawStereo[static_cast<size_t>(cIdx) * 2u + ch];
                                };
                                for (uint64_t d = 0; d < dstFrames; ++d) {
                                    if ((d & 4095ULL) == 0ULL && cancelRequested && cancelRequested->load(std::memory_order_relaxed)) {
                                        cancelled = true;
                                        break;
                                    }
                                    const double sp = static_cast<double>(d) * ratio;
                                    const int64_t i1 = static_cast<int64_t>(std::floor(sp));
                                    const float frac = static_cast<float>(sp - static_cast<double>(i1));
                                    for (size_t ch = 0; ch < 2u; ++ch) {
                                        const float p0 = atCh(i1 - 1, ch);
                                        const float p1 = atCh(i1 + 0, ch);
                                        const float p2 = atCh(i1 + 1, ch);
                                        const float p3 = atCh(i1 + 2, ch);
                                        const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
                                        const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
                                        const float c = -0.5f * p0 + 0.5f * p2;
                                        const float v = ((a * frac + b) * frac + c) * frac + p1;
                                        outTrack.interleavedStereo[static_cast<size_t>(d) * 2u + ch] = std::clamp(v, -1.0f, 1.0f);
                                    }
                                }
                                if (!cancelled) {
                                    outTrack.frameCount = dstFrames;
                                }
                            }

                            if (cancelled || (cancelRequested && cancelRequested->load(std::memory_order_relaxed))) {
                                outTrack = {};
                            } else if (outTrack.frameCount >= 480u) {
                                // Peak-normalize safely so mastered MP3s & quiet WAVs both sit cleanly at ~0.92 peak
                                float peak = 0.0f;
                                for (float s : outTrack.interleavedStereo) {
                                    peak = std::max(peak, std::abs(s));
                                }
                                if (peak > 1e-4f) {
                                    const float normScale = std::clamp(0.92f / peak, 0.35f, 3.5f);
                                    for (float& s : outTrack.interleavedStereo) {
                                        s = std::clamp(s * normScale, -0.98f, 0.98f);
                                    }
                                }
                                if (compactPcm16) {
                                    outTrack.interleavedPcm16.resize(outTrack.interleavedStereo.size());
                                    for (size_t k = 0; k < outTrack.interleavedStereo.size(); ++k) {
                                        const float clamped = std::clamp(outTrack.interleavedStereo[k], -0.999f, 0.999f);
                                        outTrack.interleavedPcm16[k] = static_cast<int16_t>(std::lrintf(clamped * 32767.0f));
                                    }
                                    std::vector<float>().swap(outTrack.interleavedStereo);
                                }
                                outTrack.label = filePath.filename().string();
                                ok = true;
                            }
                        }
                    }
                }
            }
        }
        MFShutdown();
    }

    if (needCoUninit) CoUninitialize();
    FreeLibrary(hMfRw);

    if (!ok && outError && outError->empty()) {
        *outError = "Failed to decode audio stream from " + safeName;
    }
    return ok;
}

// ============================================================================
// AudioSynthesizer48k — Deterministic Multi-Voice C1 Crossfade Synthesizer
// ============================================================================

struct AudioSynthesizer48k::StereoHallState {
    static constexpr size_t kComb0L = 1597, kComb0R = 1621;
    static constexpr size_t kComb1L = 1823, kComb1R = 1847;
    static constexpr size_t kComb2L = 2089, kComb2R = 2113;
    static constexpr size_t kComb3L = 2357, kComb3R = 2381;
    static constexpr size_t kAp0L   = 347,  kAp0R   = 359;
    static constexpr size_t kAp1L   = 113,  kAp1R   = 127;

    std::array<float, kComb0L> c0L{}; std::array<float, kComb0R> c0R{};
    std::array<float, kComb1L> c1L{}; std::array<float, kComb1R> c1R{};
    std::array<float, kComb2L> c2L{}; std::array<float, kComb2R> c2R{};
    std::array<float, kComb3L> c3L{}; std::array<float, kComb3R> c3R{};
    std::array<float, kAp0L>   ap0L{}; std::array<float, kAp0R>  ap0R{};
    std::array<float, kAp1L>   ap1L{}; std::array<float, kAp1R>  ap1R{};
    size_t i0L = 0, i0R = 0, i1L = 0, i1R = 0, i2L = 0, i2R = 0, i3L = 0, i3R = 0;
    size_t ia0L = 0, ia0R = 0, ia1L = 0, ia1R = 0;
    float damp0L = 0.0f, damp0R = 0.0f;
    float damp1L = 0.0f, damp1R = 0.0f;
    float damp2L = 0.0f, damp2R = 0.0f;
    float damp3L = 0.0f, damp3R = 0.0f;
    float silkL = 0.0f, silkR = 0.0f;

    void clear() noexcept {
        c0L.fill(0.0f); c0R.fill(0.0f);
        c1L.fill(0.0f); c1R.fill(0.0f);
        c2L.fill(0.0f); c2R.fill(0.0f);
        c3L.fill(0.0f); c3R.fill(0.0f);
        ap0L.fill(0.0f); ap0R.fill(0.0f);
        ap1L.fill(0.0f); ap1R.fill(0.0f);
        i0L = i0R = i1L = i1R = i2L = i2R = i3L = i3R = 0;
        ia0L = ia0R = ia1L = ia1R = 0;
        damp0L = damp0R = damp1L = damp1R = damp2L = damp2R = damp3L = damp3R = 0.0f;
        silkL = silkR = 0.0f;
    }

    void process(float inL, float inR, float& outWetL, float& outWetR) noexcept {
        constexpr float kFb   = 0.79f;
        constexpr float kDamp = 0.34f;
        const float sendL = (inL * 0.78f + inR * 0.22f) * 0.24f;
        const float sendR = (inR * 0.78f + inL * 0.22f) * 0.24f;

        const float y0L = c0L[i0L], y0R = c0R[i0R];
        damp0L = y0L * (1.0f - kDamp) + damp0L * kDamp;
        damp0R = y0R * (1.0f - kDamp) + damp0R * kDamp;
        c0L[i0L] = sendL + damp0L * kFb;
        c0R[i0R] = sendR + damp0R * kFb;

        const float y1L = c1L[i1L], y1R = c1R[i1R];
        damp1L = y1L * (1.0f - kDamp) + damp1L * kDamp;
        damp1R = y1R * (1.0f - kDamp) + damp1R * kDamp;
        c1L[i1L] = sendL + damp1L * kFb;
        c1R[i1R] = sendR + damp1R * kFb;

        const float y2L = c2L[i2L], y2R = c2R[i2R];
        damp2L = y2L * (1.0f - kDamp) + damp2L * kDamp;
        damp2R = y2R * (1.0f - kDamp) + damp2R * kDamp;
        c2L[i2L] = sendL + damp2L * kFb;
        c2R[i2R] = sendR + damp2R * kFb;

        const float y3L = c3L[i3L], y3R = c3R[i3R];
        damp3L = y3L * (1.0f - kDamp) + damp3L * kDamp;
        damp3R = y3R * (1.0f - kDamp) + damp3R * kDamp;
        c3L[i3L] = sendL + damp3L * kFb;
        c3R[i3R] = sendR + damp3R * kFb;

        if (++i0L >= kComb0L) i0L = 0;
        if (++i0R >= kComb0R) i0R = 0;
        if (++i1L >= kComb1L) i1L = 0;
        if (++i1R >= kComb1R) i1R = 0;
        if (++i2L >= kComb2L) i2L = 0;
        if (++i2R >= kComb2R) i2R = 0;
        if (++i3L >= kComb3L) i3L = 0;
        if (++i3R >= kComb3R) i3R = 0;

        float sumL = y0L + y1L + y2L + y3L;
        float sumR = y0R + y1R + y2R + y3R;

        // 2-stage stereo-decorrelated Schroeder allpass diffusion
        constexpr float kApG = 0.5f;
        const float b0L = ap0L[ia0L], b0R = ap0R[ia0R];
        ap0L[ia0L] = sumL + b0L * kApG;
        ap0R[ia0R] = sumR + b0R * kApG;
        sumL = b0L - sumL * kApG;
        sumR = b0R - sumR * kApG;
        if (++ia0L >= kAp0L) ia0L = 0;
        if (++ia0R >= kAp0R) ia0R = 0;

        const float b1L = ap1L[ia1L], b1R = ap1R[ia1R];
        ap1L[ia1L] = sumL + b1L * kApG;
        ap1R[ia1R] = sumR + b1R * kApG;
        outWetL = b1L - sumL * kApG;
        outWetR = b1R - sumR * kApG;
        if (++ia1L >= kAp1L) ia1L = 0;
        if (++ia1R >= kAp1R) ia1R = 0;
    }
};

AudioSynthesizer48k::AudioSynthesizer48k() noexcept
    : hall_(std::shared_ptr<StereoHallState>(new (std::nothrow) StereoHallState())) {
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
    voices_[0].voiceTrack = customTrack_;

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
    if (hall_) {
        hall_->clear();
    }
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
        voices_[0].voiceTrack = customTrack_;
        return;
    }

    const VoiceSlot& curPri = voices_[primaryVoiceIndex_];
    if (curPri.active && !curPri.fadingOut &&
        curPri.phaseSample == targetSample && curPri.voiceTrack == customTrack_) {
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
    incoming.voiceTrack = customTrack_;
    primaryVoiceIndex_ = chosen;
}

void AudioSynthesizer48k::evaluateVoiceSample(
    VoiceSlot& v,
    float& outLeft,
    float& outRight,
    float& outMono) noexcept {
    // 1. Bundled 4-Track Original Soundtrack or User-Imported MP3 / WAV / FLAC / M4A
    const CustomAudioTrack48k* trk = v.voiceTrack ? v.voiceTrack : customTrack_;
    const bool hasPcm16 = (trk != nullptr) && (trk->interleavedPcm16.size()  >= trk->frameCount * 2u);
    const bool hasFlt32 = (trk != nullptr) && (trk->interleavedStereo.size() >= trk->frameCount * 2u);
    if (v.voiceTrack != nullptr && trk->frameCount > 0u && (hasPcm16 || hasFlt32)) {
        const uint64_t N = trk->frameCount;
        const uint64_t idx = v.phaseSample % N;
        float seam = 1.0f;
        if (N > 6000u) {
            constexpr uint64_t kSeamFrames = 2400u; // 50 ms C1 loop crossfade seam
            if (idx < kSeamFrames) {
                const float u = static_cast<float>(idx) / static_cast<float>(kSeamFrames);
                seam = 0.5f - 0.5f * std::cos(static_cast<float>(kPi) * u);
            } else if (N - idx < kSeamFrames) {
                const float u = static_cast<float>(N - idx) / static_cast<float>(kSeamFrames);
                seam = 0.5f - 0.5f * std::cos(static_cast<float>(kPi) * u);
            }
        }
        constexpr float kCustomGain = 4.2f;
        float rawL = 0.0f;
        float rawR = 0.0f;
        if (hasPcm16) {
            rawL = static_cast<float>(trk->interleavedPcm16[static_cast<size_t>(idx) * 2u + 0u]) * (1.0f / 32767.0f);
            rawR = static_cast<float>(trk->interleavedPcm16[static_cast<size_t>(idx) * 2u + 1u]) * (1.0f / 32767.0f);
        } else {
            rawL = trk->interleavedStereo[static_cast<size_t>(idx) * 2u + 0u];
            rawR = trk->interleavedStereo[static_cast<size_t>(idx) * 2u + 1u];
        }
        const float sL = rawL * seam * kCustomGain;
        const float sR = rawR * seam * kCustomGain;
        outLeft  = sL;
        outRight = sR;
        outMono  = 0.5f * (sL + sR);
        return;
    }

    // 2. Built-In NVIDIA RTX Showcase Polyphonic Score (CS-80 Stereo Ensemble + DX7 Glassy Rhodes +
    //    Crystal Bell Arpeggiator + Cantabile Lead + Analog Sub-Bass & Heartbeat Pulse)
    const double t = static_cast<double>(v.phaseSample) * kInvSr;
    auto fade = [](double x) noexcept {
        return 0.5 - 0.5 * std::cos(kPi * std::clamp(x, 0.0, 1.0));
    };
    auto osc = [](double hz, double timeSec) noexcept {
        const double cyc = timeSec * hz;
        return std::sin(kTwoPi * (cyc - std::floor(cyc)));
    };

    const double durSec = std::max(1.0, static_cast<double>(durationSamples_) * kInvSr);
    const double localU = std::fmod(t, durSec);
    const double seam = fade(localU / 0.050) * fade((durSec - localU) / 0.050);

    const double midMix = std::clamp(static_cast<double>(editionMidnightMix_), 0.0, 1.0);
    const double famMix = 1.0 - midMix;

    // Map M1 60s, M2 180s, Solo loops, and FullFilm 360s onto unified 360s 7-act orchestration envelope
    double u = localU;
    if (durationSamples_ == 2640000) {
        u = 45.0 + localU;  // SoloRT [45..100s)
    } else if (durationSamples_ == 2400000) {
        u = 195.0 + localU; // SoloCUDA / SoloTensor 50s loop
    } else if (durationSamples_ == 2160000) {
        u = 150.0 + localU; // SoloNPU [150..195s)
    } else if (durationSamples_ == 8640000) {
        // M2 180s Trilogy -> Shanghai (Act 0..1), Shenzhen (Act 2..4), Taipei Sanctuary (Act 6)
        u = localU * 2.0;
    } else if (durationSamples_ != 17280000) {
        // M1 60s Taipei Slice -> Rain Noir -> Architectural Bass -> Glass Orchid Dawn Finale
        const double m1a = fade((localU - 18.0) / 4.0);
        const double m1b = fade((localU - 38.0) / 4.0);
        u = localU * (1.0 - m1a) + (55.0 + localU) * m1a * (1.0 - m1b) + (300.0 + localU) * m1b;
    }

    // Smooth 4.0s raised-cosine crossfades across all 7 film acts
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

    const double rev0 = std::exp(-std::pow((u - 36.0) / 4.5, 2.0));
    const double rev1 = std::exp(-std::pow((u - 175.0) / 5.0, 2.0));
    const double rev2 = std::exp(-std::pow((u - 342.0) / 5.5, 2.0));
    const double jazzMorph = fade((u - 166.0) / 8.0);
    const double dawnMorph = fade((u - 335.0) / 7.0);

    // Per-act dynamic orchestration balances (ensuring 343.5s Dawn Bathroom Studio is immediately lush!)
    const double padAmt =
        0.94 * w0 + 0.78 * w1 + 1.04 * w2 + 0.92 * w3 + 0.80 * w4 + 0.72 * w5 + 0.98 * w6;
    const double rhodesAmt =
        0.82 * w0 + 0.92 * w1 + 0.86 * w2 + (0.68 + 0.30 * jazzMorph) * w3 + 0.94 * w4 + 0.78 * w5 + 0.92 * w6;
    const double crystalAmt =
        (0.28 + 0.48 * rev0) * w0 + 0.88 * w1 + 0.56 * w2 + (0.24 + 0.52 * jazzMorph) * w3 +
        0.92 * w4 + 0.68 * w5 + (0.84 * (1.0 - dawnMorph) + 0.58 * dawnMorph) * w6;
    const double pulseAmt =
        0.48 * w0 + 0.74 * w1 + 0.58 * w2 + (0.42 + 0.34 * jazzMorph) * w3 +
        1.00 * w4 + 0.92 * w5 + (0.94 * (1.0 - dawnMorph) + 0.46 * dawnMorph) * w6;
    const double leadAmt =
        (0.22 + 0.68 * rev0) * w0 + 0.35 * w1 + 0.68 * w2 + (0.30 + 0.52 * jazzMorph + 0.25 * rev1) * w3 +
        0.64 * w4 + 0.42 * w5 + (0.78 + 0.22 * rev2) * w6;

    // 4-Chord Harmonic Progression (32.0s cycle = 8.0s per chord, smooth 1.44s C1 voice-leading)
    // Family:   Dmaj9 -> Bm11 -> Gmaj9(#11) -> A13sus4 (Warm Lydian / Marbles RTX Dawn)
    // Midnight: Bm9   -> Gmaj7(#11) -> Em9  -> F#7sus4(b9) (Sultry Neo-Noir Taipei Rain)
    static constexpr double kFamBass[4] = {73.416, 61.735, 48.999, 55.000};
    static constexpr double kMidBass[4] = {61.735, 48.999, 41.203, 46.249};

    static constexpr double kFamPad[4][5] = {
        {146.832, 184.997, 220.000, 277.183, 329.628}, // Dmaj9
        {123.471, 184.997, 220.000, 277.183, 369.994}, // Bm11
        {146.832, 184.997, 220.000, 246.942, 329.628}, // Gmaj9#11
        {146.832, 164.814, 195.998, 246.942, 369.994}  // A13sus4
    };
    static constexpr double kMidPad[4][5] = {
        {123.471, 146.832, 184.997, 220.000, 277.183}, // Bm9
        {97.999,  146.832, 184.997, 220.000, 293.665}, // Gmaj7#11
        {110.000, 146.832, 184.997, 195.998, 246.942}, // Em9
        {138.591, 164.814, 195.998, 220.000, 277.183}  // F#7sus4
    };

    static constexpr double kFamArp[4][6] = {
        {293.665, 369.994, 440.000, 554.365, 659.255, 739.989},
        {246.942, 369.994, 440.000, 554.365, 659.255, 739.989},
        {293.665, 369.994, 440.000, 493.883, 659.255, 739.989},
        {293.665, 329.628, 391.995, 493.883, 659.255, 739.989}
    };
    static constexpr double kMidArp[4][6] = {
        {246.942, 293.665, 369.994, 440.000, 554.365, 659.255},
        {246.942, 293.665, 369.994, 440.000, 554.365, 587.330},
        {220.000, 293.665, 329.628, 391.995, 493.883, 659.255},
        {277.183, 329.628, 391.995, 440.000, 554.365, 659.255}
    };

    const double progPos = std::fmod(t, 32.0) * 0.125; // [0.0, 4.0)
    const int cIdx0 = static_cast<int>(progPos) & 3;
    const int cIdx1 = (cIdx0 + 1) & 3;
    const double cFrac = progPos - std::floor(progPos);
    const double cXfade = (cFrac <= 0.82) ? 0.0 : fade((cFrac - 0.82) / 0.18);
    const double cw0 = 1.0 - cXfade;
    const double cw1 = cXfade;

    // A. Cinematic 120 BPM Quarter-Note Sub-Kick Heartbeat & Warm Analog Sub-Bass
    const uint64_t beatStep = static_cast<uint64_t>(t * 2.0);
    const double beatT = t - static_cast<double>(beatStep) * 0.50;
    const double kickEnv = fade(beatT / 0.012) * std::exp(-beatT * 9.5) * fade((0.50 - beatT) / 0.024);
    const double kickPh = kTwoPi * (38.0 * beatT + (24.0 / 20.0) * (1.0 - std::exp(-20.0 * beatT)));
    const double subKick = 0.155 * pulseAmt * kickEnv * std::sin(kickPh);

    auto evalBassChord = [&](int ci) noexcept -> double {
        double b = 0.0;
        if (famMix > 1e-5) {
            const double bf = kFamBass[ci];
            b += famMix * (0.68 * osc(bf, t) + 0.24 * osc(bf * 2.0, t) + 0.08 * osc(bf * 3.0, t));
        }
        if (midMix > 1e-5) {
            const double bm = kMidBass[ci];
            b += midMix * (0.68 * osc(bm, t) + 0.24 * osc(bm * 2.0, t) + 0.08 * osc(bm * 3.0, t));
        }
        return b;
    };
    const double bassWave = cw0 * evalBassChord(cIdx0) + ((cw1 > 1e-6) ? (cw1 * evalBassChord(cIdx1)) : 0.0);
    const double bassGroove = 0.72 + 0.28 * (0.5 + 0.5 * osc(2.0, t));
    const double bassSig = 0.145 * (0.65 + 0.35 * pulseAmt) * bassGroove * bassWave + subKick;

    // B. Wide Stereo Detuned CS-80 Analog Brass/Strings Pad (with gentle sidechain breath)
    static constexpr double kVoiceGains[5] = {0.26, 0.22, 0.20, 0.17, 0.15};
    auto evalPadChord = [&](int ci, double& pL, double& pR) noexcept {
        double accL = 0.0, accR = 0.0;
        if (famMix > 1e-5) {
            double fl = 0.0, fr = 0.0;
            for (int n = 0; n < 5; ++n) {
                const double f = kFamPad[ci][n];
                const double g = kVoiceGains[n];
                fl += g * (osc(f - 0.21, t) + 0.26 * osc(f * 2.0 - 0.33, t));
                fr += g * (osc(f + 0.21, t) + 0.26 * osc(f * 2.0 + 0.33, t));
            }
            accL += famMix * fl;
            accR += famMix * fr;
        }
        if (midMix > 1e-5) {
            double ml = 0.0, mr = 0.0;
            for (int n = 0; n < 5; ++n) {
                const double f = kMidPad[ci][n];
                const double g = kVoiceGains[n];
                ml += g * (osc(f - 0.21, t) + 0.26 * osc(f * 2.0 - 0.33, t));
                mr += g * (osc(f + 0.21, t) + 0.26 * osc(f * 2.0 + 0.33, t));
            }
            accL += midMix * ml;
            accR += midMix * mr;
        }
        pL = accL;
        pR = accR;
    };

    double padL0 = 0.0, padR0 = 0.0;
    evalPadChord(cIdx0, padL0, padR0);
    double padL = cw0 * padL0;
    double padR = cw0 * padR0;
    if (cw1 > 1e-6) {
        double padL1 = 0.0, padR1 = 0.0;
        evalPadChord(cIdx1, padL1, padR1);
        padL += cw1 * padL1;
        padR += cw1 * padR1;
    }
    const double padBreath = (0.82 + 0.18 * osc(0.18, t)) * (1.0 - 0.24 * kickEnv * pulseAmt);
    padL *= 0.21 * padAmt * padBreath;
    padR *= 0.21 * padAmt * padBreath;

    // C. 8th-Note DX7 / Fender Rhodes Glassy Electric Piano Arpeggiator (0.250s notes, C1 zeroed boundaries)
    static constexpr uint8_t kArpPatFam[16] = {0, 2, 4, 3, 1, 4, 5, 4, 0, 2, 3, 5, 4, 2, 3, 1};
    static constexpr uint8_t kArpPatMid[16] = {0, 1, 3, 4, 2, 5, 4, 2, 0, 2, 4, 5, 3, 4, 2, 1};
    const uint64_t step8 = static_cast<uint64_t>(t * 4.0);
    const double local8 = t - static_cast<double>(step8) * 0.25;
    const int arpChord = static_cast<int>(std::fmod(static_cast<double>(step8) * 0.25, 32.0) * 0.125) & 3;
    const double env8 = fade(local8 / 0.015) * std::exp(-local8 * 8.2) * fade((0.25 - local8) / 0.020);

    auto evalRhodes = [&](double fHz) noexcept -> double {
        const double ph = kTwoPi * fHz * local8;
        const double tine = 0.48 * std::exp(-local8 * 14.5) * std::sin(2.0 * ph);
        return std::sin(ph + tine) + 0.14 * std::exp(-local8 * 18.0) * std::sin(3.0 * ph);
    };
    const double fRhodesFam = kFamArp[arpChord][kArpPatFam[step8 & 15u]];
    const double fRhodesMid = kMidArp[arpChord][kArpPatMid[step8 & 15u]];
    const double rhodesSample =
        0.135 * rhodesAmt * env8 *
        (famMix * evalRhodes(fRhodesFam) + midMix * evalRhodes(fRhodesMid));
    const double arpPan = ((step8 & 1u) != 0u) ? 0.36 : -0.36;

    // D. 16th-Note Crystal Celesta Shimmer (0.125s notes, gentle sparkling upper harmonic halo)
    static constexpr uint8_t kCrysPat[8] = {5, 3, 4, 2, 5, 4, 3, 1};
    const uint64_t step16 = static_cast<uint64_t>(t * 8.0);
    const double local16 = t - static_cast<double>(step16) * 0.125;
    const int crysChord = static_cast<int>(std::fmod(static_cast<double>(step16) * 0.125, 32.0) * 0.125) & 3;
    const double env16 = fade(local16 / 0.012) * std::exp(-local16 * 15.0) * fade((0.125 - local16) / 0.016);
    const double fCrysFam = kFamArp[crysChord][kCrysPat[step16 & 7u]] * 2.0;
    const double fCrysMid = kMidArp[crysChord][kCrysPat[(step16 + 3u) & 7u]] * 2.0;
    const double crystalSample =
        0.048 * crystalAmt * env16 *
        (famMix * std::sin(kTwoPi * fCrysFam * local16) + midMix * std::sin(kTwoPi * fCrysMid * local16));
    const double crysPan = ((step16 & 1u) != 0u) ? -0.42 : 0.42;

    // E. Expressive Cantabile Synth Lead (2.0s lyrical phrasing with delayed ribbon vibrato)
    static constexpr double kLeadFam[16] = {
        587.330, 659.255, 739.989, 880.000,
        739.989, 659.255, 554.365, 440.000,
        493.883, 587.330, 739.989, 659.255,
        659.255, 587.330, 493.883, 440.000
    };
    static constexpr double kLeadMid[16] = {
        493.883, 587.330, 659.255, 739.989,
        587.330, 554.365, 493.883, 369.994,
        391.995, 493.883, 659.255, 587.330,
        554.365, 493.883, 440.000, 369.994
    };
    const uint64_t leadStep = static_cast<uint64_t>(t * 0.5);
    const double localLead = t - static_cast<double>(leadStep) * 2.0;
    const double leadEnv = fade(localLead / 0.18) * (0.92 - 0.22 * (localLead * 0.5)) * fade((2.0 - localLead) / 0.24);
    const double leadVib = 0.22 * fade(localLead / 0.50) * std::sin(kTwoPi * 5.2 * localLead);
    auto evalLead = [&](double fHz) noexcept -> double {
        const double ph = kTwoPi * fHz * localLead + leadVib;
        return 0.76 * std::sin(ph) + 0.24 * std::sin(2.0 * ph);
    };
    const double leadSample =
        0.088 * leadAmt * leadEnv *
        (famMix * evalLead(kLeadFam[leadStep & 15u]) + midMix * evalLead(kLeadMid[leadStep & 15u]));
    const double leadPan = 0.18 * osc(0.12, t);

    // F. Silky Filtered Taipei Rain Atmosphere in Act 0 Opening
    v.lfsr = (v.lfsr >> 1u) ^ (((v.lfsr & 1u) != 0u) ? 0xB400u : 0u);
    const float white = static_cast<float>((static_cast<double>(v.lfsr & 0xFFFFu) / 32768.0) - 1.0);
    v.rainLp += 0.14f * (white - v.rainLp);
    const double rainSample = static_cast<double>(v.rainLp) * 0.016 * w0 * (1.0 - fade((u - 34.0) / 8.0));

    const double chL = (bassSig + padL +
                        rhodesSample * (1.0 - arpPan) +
                        crystalSample * (1.0 - crysPan) +
                        leadSample * (1.0 - leadPan) +
                        rainSample) * seam;
    const double chR = (bassSig + padR +
                        rhodesSample * (1.0 + arpPan) +
                        crystalSample * (1.0 + crysPan) +
                        leadSample * (1.0 + leadPan) +
                        rainSample) * seam;

    outLeft  = static_cast<float>(chL);
    outRight = static_cast<float>(chR);
    outMono  = static_cast<float>(0.5 * (chL + chR));
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
    if (std::fabs(desiredGain - rampTargetGain_) > 1e-6f) {
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

        float finalL = mixL;
        float finalR = mixR;
        if (customTrack_ != nullptr) {
            // Preserve full 20 kHz studio master clarity for imported MP3/WAV/FLAC tracks
            finalL = 0.96f * std::tanh(mixL * 0.85f);
            finalR = 0.96f * std::tanh(mixR * 0.85f);
            if (hall_) {
                hall_->silkL = finalL;
                hall_->silkR = finalR;
            }
        } else if (hall_) {
            if (shouldAdvanceVoices) {
                float wetL = 0.0f, wetR = 0.0f;
                hall_->process(mixL, mixR, wetL, wetR);
                const float revL = 0.84f * std::tanh((mixL * 0.82f + wetL * 0.34f) * 1.45f);
                const float revR = 0.84f * std::tanh((mixR * 0.82f + wetR * 0.34f) * 1.45f);
                hall_->silkL += 0.34f * (revL - hall_->silkL);
                hall_->silkR += 0.34f * (revR - hall_->silkR);
            }
            finalL = hall_->silkL;
            finalR = hall_->silkR;
            mixMono = 0.5f * (finalL + finalR);
        }

        outInterleavedStereo[i * 2 + 0] = finalL * currentGain_;
        outInterleavedStereo[i * 2 + 1] = finalR * currentGain_;
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

const char* AudioEngine::bundledSoundtrackTitle(uint32_t slot1To4) noexcept {
    switch (slot1To4) {
        case 1u: return "1. Neon Orchid - Taiwanese Spy Jazz";
        case 2u: return "2. Acid Lotus - Taiwanese Detroit Techno";
        case 3u: return "3. Velvet Cipher - Cinematic Taiwanese Synth-Funk";
        case 4u: return "4. Glass Dragon - Progressive Demoscene Electronica";
        default: return "Procedural RTX Synth Score";
    }
}

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
    stopEndpointAndEnumerator();
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

void AudioEngine::stopEndpointAndEnumerator() noexcept {
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

void AudioEngine::stopPreloadWorker() noexcept {
    cancelPreload_.store(true, std::memory_order_release);
    if (preloadWorker_.joinable()) {
        preloadWorker_.join();
    }
    preloadInProgress_.store(false, std::memory_order_release);
}

void AudioEngine::shutdown() noexcept {
    stopPreloadWorker();
    stopEndpointAndEnumerator();

    hasBundledSoundtrack_ = false;
    selectedTrackSlot_.store(0u, std::memory_order_release);
    lastSongSamplePos_.store(0ULL, std::memory_order_relaxed);
    for (size_t i = 0; i < kBundledTrackCount; ++i) {
        bundledTracks_[i].store(nullptr, std::memory_order_release);
        bundledTrackStorage_[i].reset();
    }
    activeCustomTrack_.store(nullptr, std::memory_order_release);
    retainedTracks_.clear();
    customAudioLabel_.clear();
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

void AudioEngine::startBundledSoundtrack(
    const std::array<std::filesystem::path, kBundledTrackCount>& trackPaths,
    uint32_t initialSlot1To4) {
    stopPreloadWorker();

    bool anyExists = false;
    std::error_code ec;
    for (const auto& p : trackPaths) {
        if (!p.empty() && std::filesystem::exists(p, ec)) {
            anyExists = true;
            break;
        }
    }
    if (!anyExists) {
        return;
    }

    const uint32_t clampedSlot = std::clamp<uint32_t>(initialSlot1To4, 1u, static_cast<uint32_t>(kBundledTrackCount));
    hasBundledSoundtrack_ = true;
    cancelPreload_.store(false, std::memory_order_release);
    preloadInProgress_.store(true, std::memory_order_release);
    selectedTrackSlot_.store(clampedSlot, std::memory_order_release);
    trackSwitchGen_.fetch_add(1u, std::memory_order_release);

    const size_t firstIdx = static_cast<size_t>(clampedSlot - 1u);
    preloadWorker_ = std::thread([this, trackPaths, firstIdx]() {
        for (size_t step = 0; step < kBundledTrackCount; ++step) {
            if (cancelPreload_.load(std::memory_order_acquire)) {
                break;
            }
            const size_t idx = (firstIdx + step) % kBundledTrackCount;
            if (bundledTracks_[idx].load(std::memory_order_acquire) != nullptr) {
                continue;
            }
            CustomAudioTrack48k decoded{};
            std::string err;
            if (loadCustomAudioFile48k(trackPaths[idx], decoded, &err, false, &cancelPreload_, true)) {
                decoded.label = bundledSoundtrackTitle(static_cast<uint32_t>(idx + 1u));
                auto sharedPtr = std::make_shared<const CustomAudioTrack48k>(std::move(decoded));
                bundledTrackStorage_[idx] = sharedPtr;
                bundledTracks_[idx].store(sharedPtr.get(), std::memory_order_release);
            }
        }
        preloadInProgress_.store(false, std::memory_order_release);
    });
}

void AudioEngine::selectSoundtrackSlot(uint32_t slot0To5) noexcept {
    uint32_t clamped = std::min<uint32_t>(slot0To5, 5u);
    if (clamped == 5u && activeCustomTrack_.load(std::memory_order_acquire) == nullptr) {
        clamped = hasBundledSoundtrack_ ? 1u : 0u;
    }
    if (clamped >= 1u && clamped <= kBundledTrackCount && !hasBundledSoundtrack_) {
        clamped = 0u;
    }
    selectedTrackSlot_.store(clamped, std::memory_order_release);
    lastSongSamplePos_.store(0ULL, std::memory_order_relaxed);
    trackSwitchGen_.fetch_add(1u, std::memory_order_release);
}

uint32_t AudioEngine::cycleSoundtrackForward() noexcept {
    const uint32_t cur = selectedTrackSlot_.load(std::memory_order_acquire);
    uint32_t next = 0u;
    if (hasBundledSoundtrack_) {
        if (cur >= 1u && cur < kBundledTrackCount) {
            next = cur + 1u;
        } else if (cur == kBundledTrackCount) {
            next = 0u; // After Track 4 -> Built-in Procedural Synth
        } else {
            next = 1u; // From Synth (0) or External MP3 (5) -> Track 1
        }
    } else if (activeCustomTrack_.load(std::memory_order_acquire) != nullptr) {
        next = (cur == 5u) ? 0u : 5u;
    }
    selectSoundtrackSlot(next);
    return selectedTrackSlot_.load(std::memory_order_acquire);
}

uint32_t AudioEngine::cycleSoundtrackBackward() noexcept {
    const uint32_t cur = selectedTrackSlot_.load(std::memory_order_acquire);
    uint32_t prev = 0u;
    if (hasBundledSoundtrack_) {
        if (cur == 5u) {
            prev = 1u;
        } else if (cur == 1u) {
            prev = 0u;
        } else if (cur == 0u) {
            prev = static_cast<uint32_t>(kBundledTrackCount);
        } else {
            prev = cur - 1u;
        }
    } else if (activeCustomTrack_.load(std::memory_order_acquire) != nullptr) {
        prev = (cur == 5u) ? 0u : 5u;
    }
    selectSoundtrackSlot(prev);
    return selectedTrackSlot_.load(std::memory_order_acquire);
}

std::string AudioEngine::activeSoundtrackTitle() const {
    const uint32_t slot = selectedTrackSlot_.load(std::memory_order_acquire);
    if (slot >= 1u && slot <= kBundledTrackCount) {
        return bundledSoundtrackTitle(slot);
    }
    if (slot == 5u && activeCustomTrack_.load(std::memory_order_acquire) != nullptr) {
        return customAudioLabel_;
    }
    return "Procedural RTX Synth Score";
}

bool AudioEngine::loadExternalAudioFile(
    const std::filesystem::path& filePath,
    std::string* outError,
    bool injectFault) {
    CustomAudioTrack48k decoded{};
    if (!loadCustomAudioFile48k(filePath, decoded, outError, injectFault)) {
        return false;
    }
    auto sharedTrack = std::make_shared<const CustomAudioTrack48k>(std::move(decoded));
    retainedTracks_.push_back(sharedTrack);
    customAudioLabel_ = sharedTrack->label;
    activeCustomTrack_.store(sharedTrack.get(), std::memory_order_release);
    selectedTrackSlot_.store(5u, std::memory_order_release);
    lastSongSamplePos_.store(0ULL, std::memory_order_relaxed);
    trackSwitchGen_.fetch_add(1u, std::memory_order_release);
    return true;
}

void AudioEngine::clearExternalAudioFile() noexcept {
    activeCustomTrack_.store(nullptr, std::memory_order_release);
    customAudioLabel_.clear();
    selectedTrackSlot_.store(hasBundledSoundtrack_ ? 1u : 0u, std::memory_order_release);
    lastSongSamplePos_.store(0ULL, std::memory_order_relaxed);
    trackSwitchGen_.fetch_add(1u, std::memory_order_release);
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

    auto resolveSlotTrack = [&](uint32_t slot, bool& outWaitingForPreload) noexcept -> const CustomAudioTrack48k* {
        outWaitingForPreload = false;
        if (slot == 5u) {
            return activeCustomTrack_.load(std::memory_order_acquire);
        }
        if (slot >= 1u && slot <= kBundledTrackCount) {
            const auto* t = bundledTracks_[slot - 1u].load(std::memory_order_acquire);
            if (t) return t;
            if (preloadInProgress_.load(std::memory_order_acquire)) {
                outWaitingForPreload = true;
                return nullptr;
            }
            for (size_t k = 0; k < kBundledTrackCount; ++k) {
                if (const auto* alt = bundledTracks_[k].load(std::memory_order_acquire)) {
                    return alt;
                }
            }
        }
        return nullptr;
    };

    AudioSynthesizer48k synth;
    synth.setDurationSamples(durationSamples_);
    synth.setEdition(static_cast<Edition>(edition_.load(std::memory_order_relaxed)));
    bool initWaiting = false;
    const uint32_t initSlot = selectedTrackSlot_.load(std::memory_order_acquire);
    const CustomAudioTrack48k* appliedTrack = resolveSlotTrack(initSlot, initWaiting);
    synth.setCustomTrack(appliedTrack);
    const uint64_t initSample = appliedTrack
        ? ((initialPlayedCounter_ > 0) ? lastSongSamplePos_.load(std::memory_order_relaxed) : 0ULL)
        : pendingPhaseSample_.load(std::memory_order_relaxed);
    synth.reset(initSample, 0.0f);
    uint64_t appliedSwitchGen = trackSwitchGen_.load(std::memory_order_acquire);
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

        // 1. Auto-advance across the 4-track bundled soundtrack album (1 -> 2 -> 3 -> 4 -> 1) when a song finishes!
        uint32_t curSlot = selectedTrackSlot_.load(std::memory_order_acquire);
        const uint64_t curSwitchGen = trackSwitchGen_.load(std::memory_order_acquire);
        if (curSwitchGen == appliedSwitchGen &&
            curSlot >= 1u && curSlot <= kBundledTrackCount &&
            appliedTrack != nullptr && appliedTrack->frameCount > 4800ULL) {
            if (synth.primarySamplePosition() + 1200ULL >= appliedTrack->frameCount) {
                uint32_t nextSlot = curSlot;
                for (uint32_t step = 1u; step <= kBundledTrackCount; ++step) {
                    const uint32_t cand = ((curSlot - 1u + step) % static_cast<uint32_t>(kBundledTrackCount)) + 1u;
                    if (bundledTracks_[cand - 1u].load(std::memory_order_acquire) != nullptr) {
                        nextSlot = cand;
                        break;
                    }
                }
                if (selectedTrackSlot_.compare_exchange_strong(curSlot, nextSlot, std::memory_order_acq_rel)) {
                    curSlot = nextSlot;
                    appliedTrack = bundledTracks_[curSlot - 1u].load(std::memory_order_acquire);
                    synth.setCustomTrack(appliedTrack);
                    synth.seekTo(0ULL);
                    lastSongSamplePos_.store(0ULL, std::memory_order_relaxed);
                }
            }
        }

        // 2. Seamless C1 crossfade when switching soundtrack tracks, completing initial preload, or importing an MP3!
        bool waitingForPreload = false;
        const CustomAudioTrack48k* desiredTrack = resolveSlotTrack(curSlot, waitingForPreload);
        if (desiredTrack != appliedTrack || curSwitchGen != appliedSwitchGen) {
            appliedTrack = desiredTrack;
            appliedSwitchGen = curSwitchGen;
            synth.setCustomTrack(appliedTrack);
            synth.seekTo(appliedTrack ? 0ULL : pendingPhaseSample_.load(std::memory_order_relaxed));
            if (appliedTrack) {
                lastSongSamplePos_.store(0ULL, std::memory_order_relaxed);
            }
        }

        // 3. Consume transport seek/loop phase resets on the procedural synth while keeping song tracks continuous across scene cuts!
        const uint64_t reqGen = phaseResetGen_.load(std::memory_order_acquire);
        if (reqGen != appliedPhaseGen) {
            const uint64_t targetSample = pendingPhaseSample_.load(std::memory_order_relaxed);
            appliedPhaseGen = reqGen;
            if (appliedTrack == nullptr && !waitingForPreload) {
                synth.seekTo(targetSample);
            }
        }

        const bool isMuted = muted_.load(std::memory_order_relaxed) || waitingForPreload;
        const float masterVol = masterVolume_.load(std::memory_order_relaxed);
        const bool ambientPlay = isPaused && exploreAmbient_.load(std::memory_order_relaxed);

        AudioBandSnapshot bands{};
        const uint32_t synthAdvanced = synth.renderFrames(
            reinterpret_cast<float*>(rawBuf),
            availableFrames,
            isPaused && !ambientPlay,
            isMuted,
            masterVol,
            &bands);
        if (appliedTrack != nullptr) {
            lastSongSamplePos_.store(synth.primarySamplePosition(), std::memory_order_relaxed);
        }
        const uint32_t timelineAdvanced = isPaused ? 0u : synthAdvanced;

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
