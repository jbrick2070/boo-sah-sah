#pragma once

#include "boo/api.hpp"
#include "boo/health.hpp"

#include <cstdint>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace boo {

enum class BenchCategory : uint8_t {
    RT     = 0,
    CUDA   = 1,
    Tensor = 2,
    NVENC  = 3
};

const char* toString(BenchCategory cat) noexcept;

struct BenchResultEntry {
    BenchCategory category = BenchCategory::RT;
    std::string id;
    std::string title;
    std::string backendName;
    BackendStatus status = BackendStatus::Unavailable;
    bool isFallbackComparison = false;
    bool executed = false;
    bool outputVerified = false;

    double coldInitMs = 0.0;
    FrametimeStats timingMs{};

    double primaryScore = 0.0;
    std::string primaryUnit;     // e.g. "MRays/s", "MParticles/s", "TFLOPS", "FPS"
    double secondaryScore = 0.0;
    std::string secondaryUnit;   // e.g. "1080p FPS", "GFLOPS", "GB/s", "Mbps"
    double referenceTarget = 1.0; // Nominal scale target for creative progress bar normalization

    std::string verificationDetail;
    std::string notes;
};

struct BenchmarkProgressEvent {
    size_t stepIndex = 0;
    size_t totalSteps = 1;
    float overallProgress01 = 0.0f;
    float stageProgress01 = 0.0f;
    std::string activeStageTitle;
    std::string phaseLabel; // "INIT", "WARMUP", "SAMPLING", "VERIFY", "DONE"
    double liveMetricValue = 0.0;
    std::string liveMetricUnit;
};

struct BenchmarkConfig {
    const std::atomic<bool>* cancelRequested = nullptr;
    bool cancelled() const noexcept { return cancelRequested && cancelRequested->load(std::memory_order_acquire); }
    uint32_t timedIterations = 40;
    uint32_t warmupIterations = 6;
    bool disableRT = false;
    bool disableCUDA = false;
    bool disableTensor = false;
    bool disableNVENC = false;
    bool includeFallbacks = true;
};

struct BenchmarkSuiteReport {
    std::string timestampIso;
    std::string adapterName;
    std::string cpuName;
    uint32_t timedIterations = 0;
    double totalElapsedSeconds = 0.0;
    std::vector<BenchResultEntry> entries;
};

using BenchmarkProgressCallback = std::function<void(const BenchmarkProgressEvent&)>;

// Runs ad-hoc numerical benchmarks for RT (DXR 1.1), CUDA (Driver PTX), Tensor (PTX mma.sync FP16),
// and NVENC (Hardware H.264 MFT + nvEncodeAPI probe), alongside D3D12 HLSL substitutes.
BenchmarkSuiteReport runAdHocBenchmarks(
    void* d3d12DevicePtr,
    void* d3d12CommandQueuePtr,
    int32_t adapterLuidLow,
    int32_t adapterLuidHigh,
    const std::string& adapterName,
    const std::string& cpuName,
    const BenchmarkConfig& config,
    BenchmarkProgressCallback onProgress = nullptr);

void printCreativeBenchmarkDashboard(
    const BenchmarkSuiteReport& report,
    bool enableAnsiColors = true);

} // namespace boo
