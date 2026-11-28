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
} // namespace

const ResultsReference* resultsReferenceFor(const BenchmarkSnapshot& s) noexcept {
    if ((std::string_view(BOO_RELEASE_VERSION) != referenceBuild &&
         std::string_view(BOO_RELEASE_VERSION) != "2.1.1.0") || s.outcome != ResultsOutcome::Completed ||
        s.reason != BenchmarkReason::None || s.stage != BenchmarkStage::Finished ||
        s.warmupCount != kResultsWarmupCount || s.measuredCount != kResultsMeasuredCount ||
        !std::isfinite(s.medianMs) || !std::isfinite(s.worstMs) || s.medianMs < 0 || s.worstMs < s.medianMs)
        return nullptr;
    USHORT processMachine{}, nativeMachine{};
    if (!IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine) ||
        nativeMachine != IMAGE_FILE_MACHINE_AMD64 || processMachine != IMAGE_FILE_MACHINE_UNKNOWN)
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
