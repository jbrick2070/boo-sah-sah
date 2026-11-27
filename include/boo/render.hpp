#pragma once

#include "boo/api.hpp"
#include "boo/audio.hpp"
#include "boo/benchmark.hpp"
#include "boo/capabilities.hpp"
#include "boo/city_draw_stats.hpp"
#include "boo/director.hpp"
#include "boo/health.hpp"
#include "boo/live_accelerators.hpp"
#include "boo/settings.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct HWND__;
using HWND = HWND__*;

namespace boo {

struct GpuPassTimingsMs {
    double totalMs = 0.0;
    double sceneMs = 0.0;
    double bloomMs = 0.0;
    double compositeMs = 0.0;
    double presentPassMs = 0.0;
};

enum class DirectorHudBackend : uint32_t { Off, Warming, Npu, Cpu, Scripted, Faulted };

inline DirectorHudBackend classifyDirectorHudBackend(const HardwareResult& result) noexcept {
    if (result.health == HealthState::Probing) return DirectorHudBackend::Warming;
    if (result.status == BackendStatus::Faulted) return DirectorHudBackend::Faulted;
    if (result.status == BackendStatus::Unavailable || result.diagnosticCode == "DISABLED_BY_FLAG")
        return DirectorHudBackend::Off;
    if (result.successfullyExecutedTestWork) {
        if (result.status == BackendStatus::Accelerated) return DirectorHudBackend::Npu;
        if (result.activeBackend.starts_with("OpenVINO_CPU")) return DirectorHudBackend::Cpu;
    }
    return DirectorHudBackend::Scripted;
}

// Telemetry packet uploaded each frame to the procedural D3D12 HUD shader
// to render crisp on-screen numbers, progress bars, engine badges, audio spectrum, and benchmark gauges.
struct HudTelemetrySnapshot {
    double filmTimeSeconds = 0.0;
    double durationSeconds = 60.0;
    uint32_t actIndex = 0;
    uint32_t totalActs = 3;
    float actProgress01 = 0.0f;
    float fps = 60.0f;
    float gpuFrameMs = 0.5f;
    float p95FrameMs = 1.0f;
    std::array<float, 16> recentGpuMs{};
    float volumePercent = -1.0f;
    float vramUsedMb = 128.0f;
    float vramBudgetMb = 12288.0f;
    bool audioClockActive = false;
    bool muted = false;
    bool paused = false;
    bool loopEnabled = false;
    QualityPresetId preset = QualityPresetId::Balanced;
    Edition edition = Edition::Family;
    PlaybackMode playbackMode = PlaybackMode::FullFilm;
    DirectorMoodState mood{};
    AudioBandSnapshot audioBands{};
    uint32_t canvasWidth = 1920;
    uint32_t canvasHeight = 1080;
    uint32_t swapchainBits = 8;

    // Engine status codes: 0=Accelerated, 1=Substituted, 2=Degraded, 3=Unavailable, 4=Faulted
    std::array<BackendStatus, kEngineKindCount> engineStatus{};

    // Live-scene hardware attribution telemetry (0=RT, 1=CUDA, 2=Tensor, 3=Codec) — strictly separate from `B` bench!
    AbCompareEngine abCompareEngine = AbCompareEngine::RT;
    bool codecSignalHijackActive = false;
    bool liveCameraActive = false;
    std::array<LivePillarTelemetry, 4> livePillars{};
    DirectorHudBackend directorBackend = DirectorHudBackend::Off;
    float directorRateHz = 0.0f;
    float npuInferMs = 0.0f;
    float npuResultAgeMs = 0.0f;
    uint64_t npuCompletedInferences = 0;

    // Standalone offscreen numerical benchmark bar readouts (`B` sweep: RT, CUDA, Tensor, NVENC)
    bool benchRunning = false;
    bool benchHasResults = false;
    float benchOverallProgress01 = 0.0f;
    uint32_t benchActiveCategory = 0;
    std::array<float, 4> benchPrimaryValues{0.0f, 0.0f, 0.0f, 0.0f};   // RT MRays/s, CUDA MPart/s, Tensor TFLOPS, NVENC FPS
    std::array<float, 4> benchFallbackValues{0.0f, 0.0f, 0.0f, 0.0f};  // HLSL substitute comparison values
    std::array<float, 4> benchBarFill01{0.0f, 0.0f, 0.0f, 0.0f};       // Normalized [0..1] progress/meter fill
    std::array<uint32_t, 4> benchVerifiedFlags{0, 0, 0, 0};
};

class D3D12Context {
public:
    static constexpr uint32_t kFrameCount = 3; // Triple-buffered flip-model swap chain (Section 3.1)

    D3D12Context();
    ~D3D12Context();

    D3D12Context(const D3D12Context&) = delete;
    D3D12Context& operator=(const D3D12Context&) = delete;

    bool initialize(
        HWND hwnd,
        uint32_t width,
        uint32_t height,
        bool forceWarp,
        bool enableDebugLayer,
        std::string* outError = nullptr);

    void shutdown() noexcept;

    bool resize(uint32_t width, uint32_t height);
    bool beginFrame();
    void bindSwapchainRenderTarget() noexcept;
    void writePassTimestamp(uint32_t passSlot1To3) noexcept;
    bool endFrameAndPresent(bool vsync, double& outLastCompletedGpuMs);
    bool executeSingleCommandListAndWait(
        const std::function<void(void* cmdList)>& recordFn,
        std::string* outError = nullptr);
    // A failed/expired fence wait quarantines this context until initialize().
    bool waitForGpu() noexcept;
    // Quarantine without reusing allocators, then drain submitted work even after
    // a synchronization failure. If draining fails, remove this D3D12 device
    // before callers release its resources. True means drained; false means removed.
    bool retireGpuForShutdown() noexcept;

    // Deterministic lifecycle fault; drains submitted work and simulates removal
    // without resetting the physical GPU or affecting other applications.
    bool injectDeviceLostForTest() noexcept;
    bool injectSynchronizationFailureForTest() noexcept;

    bool checkDeviceHealthy(std::string* outReason = nullptr) const;
    uint64_t queryEstimatedAppVramBytes() const noexcept;

    void* deviceRaw() const noexcept;
    void* commandQueueRaw() const noexcept;
    void* commandListRaw() const noexcept;

    uint32_t width() const noexcept { return width_; }
    uint32_t height() const noexcept { return height_; }
    uint32_t backBufferDxgiFormat() const noexcept;
    uint32_t swapchainChannelBits() const noexcept;
    uint32_t currentBackBufferIndex() const noexcept;
    GpuPassTimingsMs lastPassTimings() const noexcept;
    uint32_t debugErrorCount() const;
    uint32_t debugWarningCount() const;
    bool debugLayerActive() const noexcept;

    const std::string& adapterName() const noexcept { return adapterName_; }
    int32_t adapterLuidLow() const noexcept { return luidLow_; }
    int32_t adapterLuidHigh() const noexcept { return luidHigh_; }
    bool isWarp() const noexcept { return isWarp_; }
    uint64_t presentCount() const noexcept { return presentCount_; }
    uint64_t presentStallCount() const noexcept { return presentStallCount_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::string adapterName_;
    int32_t luidLow_ = 0;
    int32_t luidHigh_ = 0;
    bool isWarp_ = false;
    uint64_t presentCount_ = 0;
    uint64_t presentStallCount_ = 0;
};

class M0TestVisualEffect final : public IEffect {
public:
    M0TestVisualEffect(D3D12Context& d3d, bool forceEmbeddedFallbackShader, bool simulateInitFailure = false);
    ~M0TestVisualEffect() override;

    const char* name() const noexcept override {
        if (d3d_.isWarp() && !fallbackMode_) return "WARP_ProceduralDiagnostic";
        return fallbackMode_ ? "M0_SafeFallbackVisual" : "M0_GlassOrchidTestVisual";
    }
    HardwareResult probeAndInitialize() override;
    void preload(double atFilmTime) override;
    void resetTo(double filmTime) override;
    void render(const FrameContext& ctx) override;
    void shutdown() noexcept override;

    void setHudTelemetry(const HudTelemetrySnapshot& snapshot) noexcept override { hud_ = snapshot; }
    bool isUsingEmbeddedFallbackShader() const noexcept override { return usingEmbeddedBytecode_; }
    bool ownsLiveEngineStatus(EngineKind kind) const noexcept override { return kind == EngineKind::RT && d3d_.isWarp(); }
    bool queryLiveEngineStatus(EngineKind kind, HardwareResult& outStatus) const noexcept override;

private:
    struct Impl;
    D3D12Context& d3d_;
    bool fallbackMode_ = false;
    bool simulateInitFailure_ = false;
    bool usingEmbeddedBytecode_ = false;
    HudTelemetrySnapshot hud_{};
    std::unique_ptr<Impl> impl_;
};

// Milestone M1 60-Second Taipei -> Enamel Monolith -> Glass Orchid Vertical Slice Renderer:
// 4-Pass Architecture:
//   Pass 1: 96-instance Dynamic Multi-Mesh TLAS + DXR 1.1 Inline RayQuery (or SM 6.0 Analytic Substitute) -> SceneHDR (FP16)
//   Pass 2: 3-Mip Dual-Kawase HDR Bloom (Downsample + Additive Tent Upsample) -> BloomMip0 (FP16)
//   Pass 3: Chroma-Preserving Filmic Tonemap + IEC sRGB + Sample-Deterministic IGN Dither -> CanvasClean (16:9 R10G10B10A2_UNORM)
//   Pass 4: 16:9 Aspect-Preserving Catmull-Rom Blit (with isolated A/B compare buffer) + 10/8-bit Dither + Live HUD -> Swapchain RTV
class M1TaipeiSliceEffect final : public IEffect {
public:
    M1TaipeiSliceEffect(
        D3D12Context& d3d,
        const QualityPresetSpec& initialSpec,
        bool disableRT,
        bool simulateInitFailure = false,
        bool disableCUDA = false,
        bool disableTensor = false,
        bool disableCodec = false,
        bool enableCodecHijack = false,
        bool trilogy = false,
        bool injectMeshFailure = false,
        ShowVisualMode visualMode = ShowVisualMode::M1Slice60s);
    ~M1TaipeiSliceEffect() override;

    ShowVisualMode visualMode() const noexcept;

    const char* name() const noexcept override { return "M1_TaipeiGlassOrchidSlice"; }
    HardwareResult probeAndInitialize() override;
    void preload(double atFilmTime) override;
    void resetTo(double filmTime) override;
    void render(const FrameContext& ctx) override;
    void shutdown() noexcept override;

    void onSceneBoundary(uint32_t actIndex, double filmTimeSeconds) noexcept override;
    bool syncPresetBeforeFrame(const QualityPresetSpec& spec, bool allowImmediateResize) override;
    void setHudTelemetry(const HudTelemetrySnapshot& snapshot) noexcept override { hud_ = snapshot; }
    void setDisableRt(bool disable) noexcept override;
    void setDisableCuda(bool disable) noexcept override;
    void setDisableTensor(bool disable) noexcept override;
    void setDisableCodec(bool disable) noexcept override;
    void setCodecSignalHijackActive(bool active) noexcept override;
    void setAbCompareEngine(AbCompareEngine engine) noexcept override;

    bool isRtDisabled() const noexcept { return disableRT_; }
    bool isCudaDisabled() const noexcept { return disableCUDA_; }
    bool isTensorDisabled() const noexcept { return disableTensor_; }
    bool isCodecDisabled() const noexcept { return disableCodec_; }
    bool isCodecSignalHijackActive() const noexcept { return codecHijackActive_; }
    AbCompareEngine abCompareEngine() const noexcept { return abCompareEngine_; }
    LivePillarTelemetry livePillarTelemetry(size_t pillarIndex) const noexcept;

    bool ownsLiveEngineStatus(EngineKind kind) const noexcept override {
        return kind == EngineKind::RT || kind == EngineKind::CUDA ||
               kind == EngineKind::Tensor || kind == EngineKind::Codec;
    }
    bool queryLiveEngineStatus(EngineKind kind, HardwareResult& outStatus) const noexcept override;

    bool captureClean(const FrameContext& ctx, const std::filesystem::path& directory, std::string* error) override;
    void setExternalNv12Frame(const uint8_t* nv12Bytes, size_t byteCount, uint64_t frameSeq) noexcept;
    void clearExternalNv12Frame() noexcept;
    bool hasExternalMediaFrame() const noexcept;
    void* canvasCleanResourceRaw() const noexcept;
    uint32_t canvasWidth() const noexcept;
    uint32_t canvasHeight() const noexcept;
    uint32_t lastVerifiedRayHits() const noexcept;
    uint32_t activeDenoiseMode() const noexcept;
    bool hasAllAuthoredSceneAssets() const noexcept;
    CityDrawStats lastCityDrawStats() const noexcept;

    // Deterministic offscreen Passes 1-3 execution + CanvasClean RGBA8/RGB10 readback for headless regression testing.
    bool renderHeadlessCanvasForTest(
        const FrameContext& ctx,
        std::vector<uint8_t>& outRgba,
        uint32_t& outWidth,
        uint32_t& outHeight,
        std::string* outError = nullptr,
        std::vector<uint32_t>* outRgb10 = nullptr);

    // Reads back the canonical CanvasClean texture produced by the most recent live swapchain render() without re-rendering.
    // Tests inspect linear FP16 output before UNORM conversion can hide NaN/Inf.
    bool readSceneHdrFiniteForTest(uint64_t& nonFiniteComponents, std::string* error = nullptr);
    bool readLastRenderedCanvasForTest(
        std::vector<uint8_t>& outRgba,
        uint32_t& outWidth,
        uint32_t& outHeight,
        std::string* outError = nullptr,
        std::vector<uint32_t>* outRgb10 = nullptr);

    // Executes canonical CanvasClean + isolated A/B comparison buffer for headless A/B architecture verification.
    bool renderHeadlessAbCompareForTest(
        const FrameContext& ctx,
        std::vector<uint8_t>& outCleanRgba,
        std::vector<uint8_t>& outAbCompareRgba,
        uint32_t& outWidth,
        uint32_t& outHeight,
        std::string* outError = nullptr,
        std::vector<uint32_t>* outCleanRgb10 = nullptr,
        std::vector<uint32_t>* outAbCompareRgb10 = nullptr);

    // Executes the complete 4-pass live pipeline (Passes 1-3 + optional A/B or Codec round-trip + Pass 4 HUD/A-B Present)
    // into an offscreen RGBA8 presentation target for deterministic live-HUD and 4-pillar A/B verification & captures.
    bool renderHeadlessPresentedFrameForTest(
        const FrameContext& ctx,
        const HudTelemetrySnapshot& hud,
        uint32_t presentWidth,
        uint32_t presentHeight,
        std::vector<uint8_t>& outPresentedRgba,
        std::string* outError = nullptr);

private:
    struct Impl;
    D3D12Context& d3d_;
    QualityPresetSpec activeSpec_{};
    bool disableRT_ = false;
    bool disableCUDA_ = false;
    bool disableTensor_ = false;
    bool disableCodec_ = false;
    bool codecHijackActive_ = false;
    AbCompareEngine abCompareEngine_ = AbCompareEngine::RT;
    bool simulateInitFailure_ = false;
    HudTelemetrySnapshot hud_{};
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
