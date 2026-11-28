#pragma once

#include "boo/api.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace boo {

struct ToastNotification {
    double filmTimeSeconds = 0.0;
    EngineKind engine = EngineKind::RT;
    BackendStatus status = BackendStatus::Substituted;
    std::string message;
};

// Section 3.5 & 8.1: Fault-aware effect orchestrator with runtime substitution and scene-boundary toast discipline.
class FallbackOrchestrator {
public:
    FallbackOrchestrator();

    void registerEngineState(const HardwareResult& initialResult);
    void setPrimaryAndFallbackEffects(
        std::unique_ptr<IEffect> primary,
        std::unique_ptr<IEffect> fallback);

    HardwareResult initializeEffects(bool injectPrimaryFailure = false);
    void onSceneBoundary(uint32_t newActIndex, double filmTimeSeconds, bool retryQuarantined = false);
    bool syncPresetBeforeFrame(const QualityPresetSpec& spec, bool allowImmediateResize, double filmTimeSeconds = 0.0);
    void setHudTelemetry(const HudTelemetrySnapshot& snapshot) noexcept;
    void setDisableRt(bool disable) noexcept;
    void setDisableCuda(bool disable) noexcept;
    void setDisableTensor(bool disable) noexcept;
    void setDisableCodec(bool disable) noexcept;
    void setCodecSignalHijackActive(bool active) noexcept;
    void setAbCompareEngine(AbCompareEngine engine) noexcept;
    void resetTo(double filmTimeSeconds);
    void renderFrame(const FrameContext& ctx, bool injectRenderFaultThisFrame = false);
    bool captureClean(const FrameContext& ctx, const std::filesystem::path& directory, std::string* error);
    void shutdown() noexcept;

    const HardwareResult& engineStatus(EngineKind kind) const noexcept;
    void updateEngineStatus(const HardwareResult& updated);
    bool activeEffectOwnsEngineStatus(EngineKind kind) const noexcept;
    bool activeEffectUsesEmbeddedFallbackShader() const noexcept;

    bool isUsingFallbackEffect() const noexcept { return usingFallback_; }
    // Why the main effect was replaced (empty while it runs).
    const std::string& primaryFailureReason() const noexcept { return primaryFailureReason_; }
    const char* activeEffectName() const noexcept;
    uint64_t faultCount() const noexcept { return faultCount_; }
    const std::vector<ToastNotification>& toasts() const noexcept { return toasts_; }

private:
    void syncLiveEngineStatuses() noexcept;

    std::array<HardwareResult, kEngineKindCount> engineStates_{};
    std::array<bool, kEngineKindCount> announcedAtBoundary_{};
    std::unique_ptr<IEffect> primaryEffect_;
    std::unique_ptr<IEffect> fallbackEffect_;
    bool usingFallback_ = false;
    bool primaryFaulted_ = false;
    std::string primaryFailureReason_;
    uint64_t faultCount_ = 0;
    std::vector<ToastNotification> toasts_;
};

struct FrametimeStats {
    double meanMs = 0.0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;
    size_t sampleCount = 0;
};

FrametimeStats computeFrametimeStats(const std::vector<double>& samplesMs);

// Section 6.5: Sliding-window frametime & VRAM adaptive quality controller with hysteresis
class QualityLadder {
public:
    explicit QualityLadder(
        QualityPresetId initialPreset = QualityPresetId::Balanced,
        size_t windowFrames = 60);

    void setPreset(QualityPresetId preset) noexcept;
    void setIntegratedGraphics(bool integrated) noexcept { integratedGraphics_ = integrated; }
    QualityPresetId currentPreset() const noexcept { return currentPreset_; }
    bool safetyResolutionActive() const noexcept { return safetyResolutionActive_; }
    QualityPresetSpec effectiveSpec(QualityPresetSpec spec) const noexcept;

    void pushFrameSample(
        double gpuFrameMs,
        uint64_t estimatedVramBytes,
        uint64_t softVramCapBytes,
        bool atSafeSceneCut = false,
        float elapsedSeconds = 1.0f / 60.0f);

    FrametimeStats recentStats() const;
    uint32_t downgradeCount() const noexcept { return downgradeCount_; }
    uint32_t upgradeCount() const noexcept { return upgradeCount_; }
    bool memoryPressureActive() const noexcept { return memoryPressure_; }

private:
    void stepDown() noexcept;
    void stepUp() noexcept;

    QualityPresetId currentPreset_ = QualityPresetId::Balanced;
    size_t windowFrames_ = 60;
    std::deque<double> samplesMs_;
    size_t framesSinceEval_ = 0;
    uint32_t consecutiveOverBudgetWindows_ = 0;
    uint32_t consecutiveHeadroomWindows_ = 0;
    uint32_t downgradeCount_ = 0;
    uint32_t upgradeCount_ = 0;
    bool memoryPressure_ = false;
    bool integratedGraphics_ = false;
    float fastSeconds_ = 0.0f;
    bool safetyResolutionActive_ = false;
    int lastAutoUpgradeRank_ = -1;
    int maxAutoUpgradeRank_ = 3;
};

} // namespace boo
