#pragma once

#include "boo/benchmark.hpp"
#include "boo/capabilities.hpp"
#include "boo/clock.hpp"
#include "boo/cues.hpp"
#include "boo/health.hpp"
#include "boo/render.hpp"
#include "boo/settings.hpp"

#include <filesystem>
#include <string>

namespace boo {

struct RuntimeDiagnosticsSummary {
    AppSettings settings;
    SystemCapabilities capabilities;
    ClockSource activeClockSource = ClockSource::SilentMonotonicClock;
    std::string audioEndpoint;
    std::string audioFallbackReason;
    bool audioUsesHardwarePresentationClock = false;
    int64_t audioSubmittedSamples48k = 0;
    int64_t audioPresentedSamples48k = 0;
    double audioBufferLatencyMs = 0.0;
    uint32_t audioEndpointMigrations = 0;

    uint64_t renderedFrames = 0;
    uint64_t presentStalls = 0;
    uint32_t deviceRecoveryAttempts = 0;
    uint32_t deviceRecoverySuccesses = 0;
    bool deviceLostInjected = false;
    bool graphicsUnavailable = false;
    bool d3dDebugLayerActive = false;
    uint32_t d3dDebugErrors = 0;
    uint32_t d3dDebugWarnings = 0;
    int64_t startSample = 0;
    int64_t finalSample = 0;
    double wallElapsedSeconds = 0.0;

    FrametimeStats cpuFrameStats{};
    FrametimeStats gpuFrameStats{};
    GpuPassTimingsMs lastGpuPassTimings{};
    std::array<LivePillarTelemetry, 4> lastLivePillars{};
    float lastNpuInferMs = 0.0f;
    float lastNpuResultAgeMs = 0.0f;
    uint64_t npuCompletedInferences = 0;
    uint32_t swapchainBits = 8;
    uint64_t peakEstimatedVramBytes = 0;
    QualityPresetId finalQualityPreset = QualityPresetId::Balanced;
    uint32_t qualityDowngrades = 0;
    uint64_t effectFaultCount = 0;
    bool usedEmbeddedFallbackShader = false;
    bool spoutActive = false;
    std::string spoutSenderName;
    uint64_t spoutPublishedFrames = 0;
    bool mediaInputActive = false;
    bool liveCameraActive = false;
    uint64_t mediaDecodedFrames = 0;
    std::string mediaDiagnosticCode;
    std::string mediaStatusReason;

    bool hasBenchmarkReport = false;
    BenchmarkSuiteReport benchmarkReport{};
};

std::string formatHardwareOrchestraTable(
    const SystemCapabilities& caps,
    const FallbackOrchestrator& orchestrator,
    ClockSource clockSrc,
    const std::string& audioDesc);

bool exportDiagnosticsJson(
    const std::filesystem::path& path,
    const RuntimeDiagnosticsSummary& summary,
    const FallbackOrchestrator& orchestrator,
    std::string* outError = nullptr);

bool exportBenchmarkJson(
    const std::filesystem::path& path,
    const BenchmarkSuiteReport& report,
    std::string* outError = nullptr);

bool exportBenchmarkCsv(
    const std::filesystem::path& path,
    const BenchmarkSuiteReport& report,
    std::string* outError = nullptr);

} // namespace boo
