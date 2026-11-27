#pragma once

#include "boo/results_model.hpp"
#include <array>
#include <cstdint>
#include <type_traits>

namespace boo {
enum class BenchmarkMode : uint32_t { Raster, PathTracing, DlssQuality };
enum class BenchmarkStage : uint32_t { Preparing, Warming, Measuring, Finishing, Finished };
enum class BenchmarkReason : uint32_t {
    None, Busy, Cancelled, UnsupportedDevice, SoftwareRenderer, MemoryBudget, TimeLimit,
    GraphicsReset, RendererFailed, TimingUnavailable, WorkloadChanged, WorkerExited,
    AdapterMismatch, RequiresDxr, RequiresDlss, DlssInitFailed
};
enum class BenchmarkPower : uint32_t { Unknown, Mains, Battery, Changed };
enum class BenchmarkFeature : uint32_t { Film, Lighting, Particles, NeuralMaterial, Director, Codec, Upscaling, Count };
enum class BenchmarkEvidence : uint32_t {
    Unknown, FilmGpu, RasterGpu, PathTracingGpu, HlslGpu, ScriptedCpu, Off, NotExercised, DlssGpu, Failed
};

struct BenchmarkRequest {
    BenchmarkMode mode = BenchmarkMode::Raster;
    int32_t luidLow = 0, luidHigh = 0; // Internal only. Never copied to a result card.
    uint64_t viewerLocalUsage = 0;
};
struct BenchmarkSnapshot {
    BenchmarkMode mode = BenchmarkMode::Raster;
    BenchmarkStage stage = BenchmarkStage::Preparing;
    BenchmarkReason reason = BenchmarkReason::None;
    ResultsOutcome outcome = ResultsOutcome::Unavailable;
    uint32_t warmupCount = 0, measuredCount = 0;
    uint32_t width = 0, height = 0;
    uint32_t inputWidth = 0, inputHeight = 0;
    uint32_t vendorId = 0, deviceId = 0; // Public PCI chip codes, no device instance information.
    uint64_t driverVersion = 0; // Four packed 16-bit version fields. Zero means unknown.
    BenchmarkPower power = BenchmarkPower::Unknown;
    std::array<BenchmarkEvidence, static_cast<size_t>(BenchmarkFeature::Count)> evidence{};
    double medianMs = 0, worstMs = 0; // Defined only for a completed 180-sample result.
};

static_assert(std::is_trivially_copyable_v<BenchmarkRequest> && std::is_trivially_copyable_v<BenchmarkSnapshot>);

// Raised during command recording before a changed PT1 frame can be submitted.
struct BenchmarkWorkloadChanged {};

class BenchmarkReporter {
public:
    virtual ~BenchmarkReporter() = default;
    virtual bool publish(const BenchmarkSnapshot&) = 0;
    // Called only after the final GPU retirement attempt. The private process can exit here.
    virtual void finish(const BenchmarkSnapshot& result) { publish(result); }
    virtual bool cancelled() const = 0;
};
void runResultsBenchmark(const BenchmarkRequest&, BenchmarkReporter&);
bool resultsDlssInBuild() noexcept;
} // namespace boo
