#include "boo/results_reference.hpp"

#include <windows.h>
#include <array>
#include <cmath>
#include <string_view>

namespace boo {
namespace {
using E = BenchmarkEvidence;
using F = BenchmarkFeature;
constexpr std::string_view referenceBuild = "2.1.0.0";
// Builds whose benchmark protocols match the recorded references (fixed-shot tests unchanged since 2.1.0.0;
// the render test and film tour were measured on the 2.2 code).
bool referenceAppliesToThisBuild() noexcept {
    const std::string_view build(BOO_RELEASE_VERSION);
    return build == "2.1.0.0" || build == "2.1.1.0" || build == "2.2.0.0";
}
constexpr double referenceColdStartupSeconds = 36.7; // 35.8 and 37.7 s with --cold-shader-cache, 2026-11-28
constexpr double referenceWarmStartupSeconds = 7.4;  // 7.25 and 7.52 s, 2026-11-28
constexpr uint64_t referenceDriver = (32ull << 48) | (16ull << 16) | 2140ull;

// First completed mains-powered measurement per protocol, documented in
// docs/BENCHMARK_REFERENCE.md. These data are public hardware and timing fields.
constexpr ResultsReference reference(BenchmarkMode mode, unsigned inputWidth, unsigned inputHeight,
                                     double median, double maximum, std::array<E, 7> evidence) {
    BenchmarkSnapshot s{};
    s.mode = mode;
    s.stage = BenchmarkStage::Finished;
    s.outcome = ResultsOutcome::Completed;
    s.warmupCount = kResultsWarmupCount;
    s.measuredCount = kResultsMeasuredCount;
    s.width = 1280;
    s.height = 720;
    s.inputWidth = inputWidth;
    s.inputHeight = inputHeight;
    s.vendorId = 0x10de;
    s.deviceId = 0x2c59;
    s.driverVersion = referenceDriver;
    s.power = BenchmarkPower::Mains;
    s.evidence = evidence;
    s.medianMs = median;
    s.worstMs = maximum;
    return {referenceBuild.data(), s, {ResultsCpuModel::IntelCoreUltra9_275HX, ResultsNpuModel::IntelAiBoost},
            referenceColdStartupSeconds, referenceWarmStartupSeconds};
}
constexpr auto references = std::to_array<ResultsReference>({
    reference(BenchmarkMode::Raster, 1280, 720, 0.5819, 0.6006,
              {E::FilmGpu, E::RasterGpu, E::HlslGpu, E::Unknown, E::ScriptedCpu, E::Off, E::Off}),
    reference(BenchmarkMode::PathTracing, 1280, 720, 4.0999, 5.0287,
              {E::FilmGpu, E::PathTracingGpu, E::HlslGpu, E::Unknown, E::ScriptedCpu, E::Off, E::Off}),
    reference(BenchmarkMode::DlssQuality, 853, 480, 0.9440, 1.3090,
              {E::FilmGpu, E::RasterGpu, E::HlslGpu, E::Unknown, E::ScriptedCpu, E::Off, E::DlssGpu}),
});
// Median of three path-traced runs on 2026-11-28: 25.0, 23.3, 23.0 s.
constexpr ResultsReference renderReference() {
    BenchmarkSnapshot s{};
    s.mode = BenchmarkMode::RenderTest;
    s.stage = BenchmarkStage::Finished;
    s.outcome = ResultsOutcome::Completed;
    s.width = s.inputWidth = 1920;
    s.height = s.inputHeight = 1080;
    s.vendorId = 0x10de;
    s.deviceId = 0x2c59;
    s.driverVersion = referenceDriver;
    s.power = BenchmarkPower::Mains;
    s.tilesDone = s.tilesTotal = 510;
    s.samplesPerPixel = 24;
    s.renderPathTraced = true;
    s.renderSeconds = 23.3;
    s.megaSamplesPerSecond = 2.1;
    return {"2.1.1.0", s, {ResultsCpuModel::IntelCoreUltra9_275HX, ResultsNpuModel::IntelAiBoost},
            referenceColdStartupSeconds, referenceWarmStartupSeconds};
}
constexpr auto renderReferenceData = renderReference();
// RTX 5080 laptop, 2026-11-28, build 2.2.0.0: middle value of three runs (overall 0.5445 / 0.5461 / 0.5493 ms).
constexpr ResultsReference tourReference() {
    BenchmarkSnapshot s{};
    s.mode = BenchmarkMode::FilmTour;
    s.stage = BenchmarkStage::Finished;
    s.outcome = ResultsOutcome::Completed;
    s.width = s.inputWidth = 1280;
    s.height = s.inputHeight = 720;
    s.vendorId = 0x10de;
    s.deviceId = 0x2c59;
    s.driverVersion = referenceDriver;
    s.power = BenchmarkPower::Mains;
    s.evidence = {E::FilmGpu, E::RasterGpu, E::HlslGpu, E::Unknown, E::ScriptedCpu, E::Off, E::Off};
    s.tourFramesDone = kFilmTourSceneCount * (kFilmTourWarmupCount + kFilmTourMeasuredCount);
    s.medianMs = 0.5461;
    s.tourMediansMs = {0.6928, 1.1889, 0.8103, 0.5055, 0.1405, 0.5590};
    s.tourWorstMs = {1.0172, 1.5664, 0.8580, 0.5749, 0.1761, 0.6084};
    return {"2.2.0.0", s, {ResultsCpuModel::IntelCoreUltra9_275HX, ResultsNpuModel::IntelAiBoost},
            referenceColdStartupSeconds, referenceWarmStartupSeconds};
}
constexpr auto tourReferenceData = tourReference();
} // namespace

const ResultsReference* resultsReferenceFor(const BenchmarkSnapshot& s) noexcept {
    if (s.outcome != ResultsOutcome::Completed ||
        s.reason != BenchmarkReason::None || s.stage != BenchmarkStage::Finished)
        return nullptr;
    USHORT processMachine{}, nativeMachine{};
    if (!IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine) ||
        nativeMachine != IMAGE_FILE_MACHINE_AMD64 || processMachine != IMAGE_FILE_MACHINE_UNKNOWN)
        return nullptr;
    if (s.mode == BenchmarkMode::FilmTour) {
        const auto& r = tourReferenceData.snapshot;
        if (!referenceAppliesToThisBuild() ||
            s.width != r.width || s.height != r.height || s.inputWidth != r.inputWidth ||
            s.inputHeight != r.inputHeight || s.tourFramesDone != r.tourFramesDone ||
            !std::isfinite(s.medianMs) || s.medianMs <= 0 ||
            s.evidence[static_cast<size_t>(F::Film)] != E::FilmGpu ||
            s.evidence[static_cast<size_t>(F::Lighting)] != E::RasterGpu ||
            s.evidence[static_cast<size_t>(F::Upscaling)] != E::Off) return nullptr;
        for (size_t i = 0; i < kFilmTourSceneCount; ++i)
            if (!std::isfinite(s.tourMediansMs[i]) || s.tourMediansMs[i] <= 0 ||
                !std::isfinite(s.tourWorstMs[i]) || s.tourWorstMs[i] < s.tourMediansMs[i]) return nullptr;
        return &tourReferenceData;
    }
    if (!referenceAppliesToThisBuild()) return nullptr;
    if (s.mode == BenchmarkMode::RenderTest) {
        const auto& r = renderReferenceData.snapshot;
        if (s.renderPathTraced &&
            s.width == r.width && s.height == r.height && s.inputWidth == r.inputWidth &&
            s.inputHeight == r.inputHeight && s.power == r.power && s.tilesTotal == r.tilesTotal &&
            s.tilesDone == s.tilesTotal && s.samplesPerPixel == r.samplesPerPixel &&
            std::isfinite(s.renderSeconds) && s.renderSeconds > 0 &&
            std::isfinite(s.megaSamplesPerSecond) && s.megaSamplesPerSecond > 0)
            return &renderReferenceData;
        return nullptr;
    }
    if (s.warmupCount != kResultsWarmupCount || s.measuredCount != kResultsMeasuredCount ||
        !std::isfinite(s.medianMs) || !std::isfinite(s.worstMs) || s.medianMs < 0 || s.worstMs < s.medianMs)
        return nullptr;
    for (const auto& measured : references) {
        const auto& r = measured.snapshot;
        if (s.mode == r.mode && s.width == r.width && s.height == r.height &&
            s.inputWidth == r.inputWidth && s.inputHeight == r.inputHeight && s.power == r.power &&
            s.evidence[static_cast<size_t>(F::Film)] == r.evidence[static_cast<size_t>(F::Film)] &&
            s.evidence[static_cast<size_t>(F::Lighting)] == r.evidence[static_cast<size_t>(F::Lighting)] &&
            s.evidence[static_cast<size_t>(F::Director)] == r.evidence[static_cast<size_t>(F::Director)] &&
            s.evidence[static_cast<size_t>(F::Upscaling)] == r.evidence[static_cast<size_t>(F::Upscaling)])
            return &measured;
    }
    return nullptr;
}
} // namespace boo
