#include "boo/results_worker.hpp"
#include "boo/capabilities.hpp"
#include "boo/render.hpp"
#include "boo/results_card.hpp"
#include "boo/results_export.hpp"
#include <windows.h>
#include <objbase.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>
#include <thread>

namespace boo {
namespace {
// Compare the isolated PT1 render configuration with normal playback on the very same film frame.
// Each renderer has independent temporal history. Both use the same device and shader bytecode.
bool openingImage(D3D12Context& device, const BenchmarkRequest& request, bool benchmark,
                  std::unique_ptr<M1TaipeiSliceEffect>& renderer, std::vector<uint8_t>& pixels) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::printf("Preparing %s film renderer for image comparison...\n", benchmark ? "PT1" : "reference");
    std::fflush(stdout);
    if (benchmark) device.selectBenchmarkAdapter(request);
    auto safe = findQualityPresetSpec(loadQualityPresets(resolveResourcePath("config/quality_presets.json")),
                                      QualityPresetId::Safe);
    safe.allowDxr = true;
    renderer = std::make_unique<M1TaipeiSliceEffect>(device, safe, false, false, true, true, true,
                                                     false, true, false, ShowVisualMode::FullFilm360s);
    auto& film = *renderer;
    if (film.probeAndInitialize().status == BackendStatus::Faulted || !film.hasAllAuthoredSceneAssets()) return false;
    const auto prepareSeconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("%s scene prepared in %.2f s.\n", benchmark ? "PT1" : "Reference", prepareSeconds);
    std::fflush(stdout);
    film.resetTo(8.0);
    FrameContext frame{};
    frame.samplePosition = 384000;
    frame.filmTimeSeconds = 8.0;
    frame.dtSeconds = 1.0 / 30;
    frame.randomSeed = 266203;
    frame.qualityPreset = QualityPresetId::Safe;
    frame.effectsDetail = 0;
    frame.pathTracing = PathTracingLevel::High;
    frame.dlssMode = DlssMode::Off;
    frame.rayReconstruction = false;
    frame.frameGeneration = FrameGenerationMode::Off;
    frame.reflex = ReflexMode::Off;
    frame.showOverlay = false;
    uint32_t w{}, h{};
    const bool drawn = film.renderHeadlessCanvasForTest(frame, pixels, w, h) && w == 1280 && h == 720 &&
                       film.pathTracingState() == RendererFeatureState::Active && film.lastVerifiedRayHits() > 0;
    std::printf("%s pixels retired, verified=%d.\n", benchmark ? "PT1" : "Reference", int(drawn));
    std::fflush(stdout);
    return drawn;
}

int compareOpening(const BenchmarkRequest& request) {
    D3D12Context device;
    device.setRayTracingAllowed(true);
    if (!device.initialize(nullptr, 1280, 720, false, false) || device.isWarp() ||
        device.adapterLuidLow() != request.luidLow || device.adapterLuidHigh() != request.luidHigh) return 9;
    std::unique_ptr<M1TaipeiSliceEffect> viewer, worker;
    std::vector<uint8_t> generic, measured;
    if (!openingImage(device, request, false, viewer, generic) || !openingImage(device, request, true, worker, measured) ||
        generic.empty() || generic.size() != measured.size()) return 9;
    uint64_t totalError = 0, different = 0;
    unsigned maxError = 0;
    for (size_t i = 0; i < generic.size(); ++i) {
        const auto delta = static_cast<unsigned>(std::abs(static_cast<int>(generic[i]) - measured[i]));
        totalError += delta;
        maxError = (std::max)(maxError, delta);
        if (delta) ++different;
    }
    const double meanError = static_cast<double>(totalError) / generic.size();
    std::printf("PT1 / live film RGBA8: different channels=%llu, mean error=%.6f, max error=%u\n",
        static_cast<unsigned long long>(different), meanError, maxError);
    std::fflush(stdout);
    // Allow ordinary floating-point reordering at the 8-bit output quantization boundary.
    const bool drained = device.retireGpuForShutdown();
    ExitProcess(drained && meanError <= 0.02 && maxError <= 3 ? 0 : 10);
}
} // namespace

int verifyFilmBenchmarkIfRequested(int argc, char** argv) {
    using namespace std::chrono_literals;
    using Microsoft::WRL::ComPtr;
    if (argc < 2 || std::string_view(argv[1]) != "--verify-film-benchmark") return -1;
    const bool saveExample = argc == 7 && std::string_view(argv[5]) == "--save-example";
    if ((argc != 5 && !saveExample) || std::string_view(argv[3]) != "--no-activate" || std::string_view(argv[4]) != "--mute") return 2;
    const std::string_view test = argv[2];
    BenchmarkRequest request{};
    BenchmarkReason expected = BenchmarkReason::None;
    if (test == "raster" || test == "integrated") request.mode = BenchmarkMode::Raster;
    else if (test == "path" || test == "fidelity" || test == "viewer-path") request.mode = BenchmarkMode::PathTracing;
    else if (test == "dlss") request.mode = BenchmarkMode::DlssQuality;
    else if (test == "warp") expected = BenchmarkReason::SoftwareRenderer;
    else if (test == "wrong-adapter") expected = BenchmarkReason::AdapterMismatch;
    else if (test == "low-budget") {
        request.viewerLocalUsage = (std::numeric_limits<uint64_t>::max)();
        expected = BenchmarkReason::MemoryBudget;
    } else if (test != "viewer-budget") return 2;

    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    if (test == "warp") {
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) || FAILED(adapter->GetDesc1(&desc))) return 3;
    } else {
        const auto preference = test == "integrated" ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
        for (UINT i = 0; ; ++i) {
            if (FAILED(factory->EnumAdapterByGpuPreference(i, preference,
                                                           IID_PPV_ARGS(&adapter)))) return 3;
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
            adapter.Reset();
        }
    }
    request.luidLow = static_cast<int32_t>(desc.AdapterLuid.LowPart);
    request.luidHigh = desc.AdapterLuid.HighPart;
    if (test == "fidelity") {
        try { return compareOpening(request); }
        catch (...) { return 9; }
    }
    if (test == "wrong-adapter") request.luidLow = request.luidHigh = -1;
    // Keep an actual Safe film device, meshes and render targets resident while the child measures.
    D3D12Context viewer;
    std::unique_ptr<M1TaipeiSliceEffect> pausedFilm;
    if (test == "viewer-budget" || test == "viewer-path") {
        const bool rt = test == "viewer-path";
        viewer.setRayTracingAllowed(rt);
        if (!viewer.initialize(nullptr, 1280, 720, false, false) ||
            viewer.adapterLuidLow() != request.luidLow || viewer.adapterLuidHigh() != request.luidHigh) return 3;
        auto safe = findQualityPresetSpec(loadQualityPresets(resolveResourcePath("config/quality_presets.json")),
                                                 QualityPresetId::Safe);
        safe.allowDxr = rt;
        pausedFilm = std::make_unique<M1TaipeiSliceEffect>(viewer, safe, !rt, false, true, true, true,
                                                           false, true, false, ShowVisualMode::FullFilm360s);
        if (pausedFilm->probeAndInitialize().status == BackendStatus::Faulted || !pausedFilm->hasAllAuthoredSceneAssets())
            return 3;
        request.viewerLocalUsage = viewer.queryEstimatedAppVramBytes();
        std::printf("Paused viewer holds %llu bytes on this adapter.\n", static_cast<unsigned long long>(request.viewerLocalUsage));
        if (!request.viewerLocalUsage) return 3;
    }
    ResultsWorker worker;
    if (!worker.start(request)) return 4;
    ResultsWorkerState terminal = ResultsWorkerState::idle;
    const auto started = std::chrono::steady_clock::now();
    auto stage = BenchmarkStage::Preparing;
    do {
        terminal = worker.poll();
        const auto observed = worker.snapshot().stage;
        if (stage != observed) {
            stage = observed;
            const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::printf("Stage %u at %.2f s.\n", unsigned(stage), seconds);
            std::fflush(stdout);
        }
        if (terminal != ResultsWorkerState::starting && terminal != ResultsWorkerState::running) break;
        std::this_thread::sleep_for(10ms);
    } while (true);
    const auto s = worker.snapshot();
    std::printf("%s state=%u reason=%u outcome=%u warmup=%u measured=%u vendor=%04x device=%04x "
                "driver=%llu canvas=%ux%u input=%ux%u median_ms=%.4f worst_ms=%.4f evidence=",
        argv[2], unsigned(terminal), unsigned(s.reason), unsigned(s.outcome), s.warmupCount, s.measuredCount,
        s.vendorId, s.deviceId, static_cast<unsigned long long>(s.driverVersion), s.width, s.height,
        s.inputWidth, s.inputHeight, s.medianMs, s.worstMs);
    for (const auto row : s.evidence) std::printf(" %u", unsigned(row));
    std::puts("");
    std::fflush(stdout);
    if (terminal != ResultsWorkerState::completed || s.stage != BenchmarkStage::Finished || s.reason != expected) return 5;
    if (expected != BenchmarkReason::None)
        return s.outcome == ResultsOutcome::Unavailable && !s.warmupCount && !s.measuredCount &&
               !s.medianMs && !s.worstMs ? 0 : 6;
    const auto light = request.mode == BenchmarkMode::PathTracing ? BenchmarkEvidence::PathTracingGpu : BenchmarkEvidence::RasterGpu;
    const auto upscale = request.mode == BenchmarkMode::DlssQuality ? BenchmarkEvidence::DlssGpu : BenchmarkEvidence::Off;
    const auto materials = s.evidence[static_cast<size_t>(BenchmarkFeature::NeuralMaterial)];
    // The field dispatch alone cannot establish a material's visible contribution. The shader may
    // observe water or silk on this adapter, or leave its attribution unknown on this opening shot.
    if (materials != BenchmarkEvidence::Unknown && materials != BenchmarkEvidence::HlslGpu) return 7;
    const decltype(s.evidence) wanted = {BenchmarkEvidence::FilmGpu, light, BenchmarkEvidence::HlslGpu,
        materials, BenchmarkEvidence::ScriptedCpu, BenchmarkEvidence::Off, upscale};
    if (s.mode != request.mode || s.outcome != ResultsOutcome::Completed || s.warmupCount != kResultsWarmupCount ||
        s.measuredCount != kResultsMeasuredCount || s.vendorId != desc.VendorId || s.deviceId != desc.DeviceId ||
        s.width != 1280 || s.height != 720 || !std::isfinite(s.medianMs) || !std::isfinite(s.worstMs) ||
        s.medianMs < 0.0 || s.worstMs < s.medianMs || s.evidence != wanted) return 7;
    const bool dimensionsMatch = request.mode == BenchmarkMode::DlssQuality ?
        s.inputWidth > 0 && s.inputHeight > 0 && s.inputWidth < s.width && s.inputHeight < s.height :
        s.inputWidth == s.width && s.inputHeight == s.height;
    if (!dimensionsMatch) return 8;
    if (saveExample) {
        ResultsDialogContext context;
        context.hardware = probeResultsHostHardware();
        // This headless test does not present a viewer frame. Startup stays unavailable.
        const auto card = makeResultsCard(context, s);
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 9;
        const auto destination = std::filesystem::path(argv[6]);
        const bool saved = writeResultsFile(destination / L"BOO-SAH-SAH-results.png", card, L"en-US", false, ResultsSaveKind::Picture) &&
            writeResultsFile(destination / L"BOO-SAH-SAH-results.txt", card, L"en-US", false, ResultsSaveKind::Text);
        CoUninitialize();
        if (!saved) return 9;
    }
    // The retained viewer is only the memory/ordinary-pipeline fixture for this test. Both the
    // worker result and export have already been verified; retire that independent fixture too.
    if (pausedFilm) ExitProcess(viewer.retireGpuForShutdown() ? 0 : 10);
    return 0;
}
} // namespace boo
