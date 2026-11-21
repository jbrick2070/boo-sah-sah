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
    RendererFeatureState pathTracing = RendererFeatureState::Unavailable;
    RendererFeatureState superResolution = RendererFeatureState::Unavailable;
    RendererFeatureState rayReconstruction = RendererFeatureState::Unavailable;
    uint32_t dlssInputWidth = 0;
    uint32_t dlssInputHeight = 0;
    FrameGenerationStats presentation{};
    uint32_t maxFramesPresentedPerRender = 0;
    NeuralRenderingStats neural{};
    std::string gpuSensorsDevice;
    std::string gpuSensorsReason;
    std::string gpuSensorsMatch;
    uint32_t gpuSensorsNvidiaCount = 0;
    GpuSensorReading gpuSensorsLast{};
    GpuSensorSummary gpuSensorsRun{};
    uint32_t gpuWorkout = 0;
    uint32_t flockFormation = 0;
    uint32_t flockSpecies = 0;
    bool birdCam = false;
    uint32_t birdCount = 0;
    // The workout's frame-rate floor: what the dial asked for and what the machine could fly.
    uint32_t workoutRequested = 0;     // birds the level asks for
    uint32_t workoutReleased = 0;      // workout birds flying at the end of the run
    float workoutFloorFps = 0.0f;      // 0 = no floor
    bool workoutAtFloor = false;       // the floor, not the dial, was limiting the flock
    uint32_t workoutSteadyBirds = 0;   // last flock measured holding the floor
    float workoutSteadyFps = 0.0f;     // and the frame rate it held
    uint32_t workoutPeakBirds = 0;     // largest flock measured holding the floor
    double workoutMicrosecondsPerBird = 0.0; // frame time one more bird costs on this machine
    uint32_t workoutCapacity = 0;      // birds the buffers had grown to hold
    uint32_t physxGenre = 0;           // PhysX suite genre at the end of the run
    PhysxStats physx{};                // the active genre's figures
    PhysxRuntimeInfo physxRuntime{};   // the shared PhysX runtime
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
