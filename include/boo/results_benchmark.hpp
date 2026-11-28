#pragma once

#include "boo/results_model.hpp"
#include "boo/render_test_image.hpp"
#include <array>
#include <cstdint>
#include <type_traits>

namespace boo {
enum class BenchmarkMode : uint32_t { Raster, PathTracing, DlssQuality, RenderTest, FilmTour };
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
    std::uintptr_t ownerWindow = 0; // Used only to select the progress window's monitor.
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
    // Render-test fields; zero for the fixed-shot modes.
    uint32_t tilesDone = 0, tilesTotal = 0, samplesPerPixel = 0;
    bool renderPathTraced = false;
    double renderSeconds = 0, gpuSeconds = 0, megaSamplesPerSecond = 0;
    uint32_t tourScene = 0, tourWarmupCount = 0, tourMeasuredCount = 0, tourFramesDone = 0;
    std::array<double, kFilmTourSceneCount> tourMediansMs{}, tourWorstMs{};
};

// At most a 1920 x 1080 image divided into 64-pixel tiles.
inline constexpr uint32_t kResultsMaxRenderTiles = 30 * 17;
struct ResultsProgress {
    bool marquee;
    uint32_t position; // 0 through 1000, independently for each stage.
};
constexpr ResultsProgress resultsProgress(const BenchmarkSnapshot& state) noexcept {
    const auto fraction = [](uint32_t count, uint32_t total) constexpr {
        return total ? static_cast<uint32_t>(uint64_t{(std::min)(count, total)} * 1000 / total) : 0u;
    };
    if (state.mode == BenchmarkMode::FilmTour) {
        if (state.stage == BenchmarkStage::Preparing) return {true, 0};
        return {false, fraction(state.tourFramesDone, static_cast<uint32_t>(kFilmTourSceneCount) * kFilmTourFramesPerScene)};
    }
    switch (state.stage) {
    case BenchmarkStage::Preparing: return {true, 0};
    // Fixed-shot modes: one monotonic bar across warm-up and measuring (210 frames in all).
    case BenchmarkStage::Warming:
        return {false, fraction((std::min)(state.warmupCount, static_cast<uint32_t>(kResultsWarmupCount)),
                                static_cast<uint32_t>(kResultsWarmupCount + kResultsMeasuredCount))};
    case BenchmarkStage::Measuring:
        if (state.mode == BenchmarkMode::RenderTest)
            return {false, state.tilesTotal <= kResultsMaxRenderTiles ? fraction(state.tilesDone, state.tilesTotal) : 0u};
        return {false, fraction(static_cast<uint32_t>(kResultsWarmupCount) + (std::min)(state.measuredCount, static_cast<uint32_t>(kResultsMeasuredCount)),
                                static_cast<uint32_t>(kResultsWarmupCount + kResultsMeasuredCount))};
    case BenchmarkStage::Finishing:
    case BenchmarkStage::Finished: return {false, 1000};
    }
    return {false, 0};
}

static_assert(std::is_trivially_copyable_v<BenchmarkRequest> && std::is_trivially_copyable_v<BenchmarkSnapshot>);

// Raised during command recording before a changed PT1 frame can be submitted.
struct BenchmarkWorkloadChanged {};

class BenchmarkReporter {
public:
    virtual ~BenchmarkReporter() = default;
    virtual bool publish(const BenchmarkSnapshot&) = 0;
    virtual bool publishImage(const RenderTestImage&) { return false; }
    // Called only after the final GPU retirement attempt. The private process can exit here.
    virtual void finish(const BenchmarkSnapshot& result) { publish(result); }
    virtual bool cancelled() const = 0;
};
void runResultsBenchmark(const BenchmarkRequest&, BenchmarkReporter&);
bool resultsDlssInBuild() noexcept;
} // namespace boo
