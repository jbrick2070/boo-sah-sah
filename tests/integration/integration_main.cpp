#include "boo/audio.hpp"
#include "boo/benchmark.hpp"
#include "boo/capabilities.hpp"
#include "boo/clock.hpp"
#include "boo/cues.hpp"
#include "boo/director.hpp"
#include "boo/health.hpp"
#include "boo/logging.hpp"
#include "boo/m1_scene.hpp"
#include "boo/media_input.hpp"
#include "boo/render.hpp"
#include "boo/settings.hpp"
#include "boo/spout_sender.hpp"
#include "boo/window.hpp"
#include "boo/json.hpp"
#include <fstream>
#include <thread>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <psapi.h>
#include <charconv>
#include <iomanip>
#include <sstream>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

int gTestsRun = 0;
int gTestsFailed = 0;

#define BOO_CHECK(cond)                                                                                  \
    do {                                                                                                 \
        if (!(cond)) {                                                                                   \
            std::cerr << "  [FAIL] " << __FILE__ << ":" << __LINE__ << " -> " << #cond << "\n";          \
            return false;                                                                                \
        }                                                                                                \
    } while (0)

void runTest(const char* name, bool (*fn)()) {
    ++gTestsRun;
    if (fn()) {
        std::cout << "[PASS] " << name << "\n";
    } else {
        ++gTestsFailed;
        std::cerr << "[FAIL] " << name << "\n";
    }
}

bool testCapabilityProbeHonestyAndPermutations() {
    using namespace boo;

    // 1. Default baseline probe: NVENC must be not_loaded, and NO optional engine may claim untested work!
    ProbeFlags normal{};
    const auto caps = probeSystemCapabilities(normal);
    BOO_CHECK(!caps.osVersion.empty());
    BOO_CHECK(!caps.cpuBrand.empty());
    BOO_CHECK(caps.d3d12Available);
    const auto cached = resultsHostHardware(caps);
    const auto headless = probeResultsHostHardware();
    BOO_CHECK(cached.cpu == headless.cpu && cached.cpuFamily == headless.cpuFamily && cached.cpuModel == headless.cpuModel);
    BOO_CHECK(cached.npu == headless.npu && cached.acceleratorVendor == headless.acceleratorVendor &&
              cached.acceleratorDevice == headless.acceleratorDevice);
    SystemCapabilities missing;
    BOO_CHECK(resultsHostHardware(missing).npu == ResultsNpuModel::Unknown);
    missing.acceleratorEnumerationSucceeded = true;
    BOO_CHECK(resultsHostHardware(missing).npu == ResultsNpuModel::NoneDetected);
    missing.npuDevices.emplace_back();
    BOO_CHECK(resultsHostHardware(missing).npu == ResultsNpuModel::Unknown);

    for (size_t i = 0; i < kEngineKindCount; ++i) {
        const auto& r = caps.engines[i];
        BOO_CHECK(r.successfullyExecutedTestWork == false);
    }
    BOO_CHECK(caps.engines[static_cast<size_t>(EngineKind::Codec)].diagnosticCode == "NOT_LOADED_BY_DEFAULT");

    // 2. All-disabled fault-injection permutation
    ProbeFlags allDisabled{};
    allDisabled.disableRT = true;
    allDisabled.disableCUDA = true;
    allDisabled.disableTensor = true;
    allDisabled.disableNPU = true;
    allDisabled.disableNVENC = true;
    const auto disCaps = probeSystemCapabilities(allDisabled);
    BOO_CHECK(resultsHostHardware(disCaps).npu == ResultsNpuModel::Unknown);
    for (size_t i = 0; i < kEngineKindCount; ++i) {
        BOO_CHECK(disCaps.engines[i].diagnosticCode == "DISABLED_BY_FLAG");
        BOO_CHECK(disCaps.engines[i].status == BackendStatus::Substituted);
    }
    return true;
}

bool testD3D12HeadlessAndShaderFallback() {
    using namespace boo;
    D3D12Context d3d;
    std::string err;
    BOO_CHECK(d3d.initialize(nullptr, 1280, 720, false, false, &err));
    BOO_CHECK(d3d.checkDeviceHealthy());

    // Normal shader initialization + injected shader failure fallback verification
    {
        FallbackOrchestrator orch;
        orch.setPrimaryAndFallbackEffects(
            std::make_unique<M0TestVisualEffect>(d3d, false, false),
            std::make_unique<M0TestVisualEffect>(d3d, true, false));
        const auto res = orch.initializeEffects(false);
        BOO_CHECK(res.successfullyExecutedTestWork);
        BOO_CHECK(!orch.isUsingFallbackEffect());
        orch.shutdown();
    }
    {
        // --inject-shader-fail simulation -> must fall back cleanly to embedded safe DXIL shader!
        FallbackOrchestrator orch;
        orch.setPrimaryAndFallbackEffects(
            std::make_unique<M0TestVisualEffect>(d3d, false, true),
            std::make_unique<M0TestVisualEffect>(d3d, true, false));
        const auto res = orch.initializeEffects(false);
        BOO_CHECK(res.successfullyExecutedTestWork);
        BOO_CHECK(orch.isUsingFallbackEffect());
        BOO_CHECK(std::string(orch.activeEffectName()) == "M0_SafeFallbackVisual");
        orch.shutdown();
    }

    d3d.shutdown();
    return true;
}

bool testAudioAndSilentFallbackClockIntegration() {
    using namespace boo;

    // 1. Injected audio failure -> silent monotonic clock must advance smoothly
    {
        AudioEngine audio;
        BOO_CHECK(!audio.initialize(true));
        TimelineClock clk;
        clk.configure(48000, 17280000);
        clk.start(false, false);
        BOO_CHECK(clk.source() == ClockSource::SilentMonotonicClock);
        Sleep(35);
        const auto snap = clk.tickFrame();
        BOO_CHECK(snap.samplePosition > 500);
        clk.shutdown();
    }

    // 2. Real audio engine attempt (succeeds if endpoint exists, or cleanly degrades without crash)
    {
        AudioEngine audio;
        audio.setMuted(true);
        const bool ok = audio.initialize(false);
        TimelineClock clk;
        clk.configure(48000, 17280000);
        if (ok) {
            clk.setAudioSampleReader([&](int64_t& out) { return audio.readPlayedSamples48k(out); });
        }
        clk.start(ok, false);
        Sleep(60);
        const auto snap = clk.tickFrame();
        BOO_CHECK(snap.samplePosition > 0);
        audio.shutdown();
        const auto dropped = clk.tickFrame();
        BOO_CHECK(dropped.source == ClockSource::SilentMonotonicClock);
        BOO_CHECK(dropped.samplePosition >= snap.samplePosition);
        Sleep(20);
        BOO_CHECK(clk.tickFrame().samplePosition > dropped.samplePosition);
        clk.pause();
        const auto paused = clk.currentSample();
        clk.switchToSilentFallback();
        Sleep(10);
        BOO_CHECK(clk.tickFrame().samplePosition == paused);
        clk.shutdown();
    }
    return true;
}

bool testAdHocNumericalBenchmarksVerification() {
    using namespace boo;
    ProbeFlags pf{};
    const auto caps = probeSystemCapabilities(pf);

    D3D12Context d3d;
    BOO_CHECK(d3d.initialize(nullptr, 1920, 1080, false, false));

    BenchmarkConfig cfg{};
    cfg.warmupIterations = 2;
    cfg.timedIterations = 5;
    cfg.includeFallbacks = true;

    const auto rep = runAdHocBenchmarks(
        d3d.deviceRaw(),
        d3d.commandQueueRaw(),
        d3d.adapterLuidLow(),
        d3d.adapterLuidHigh(),
        d3d.adapterName(),
        caps.cpuBrand,
        cfg);

    BOO_CHECK(!rep.entries.empty());
    size_t verifiedFallbacks = 0;
    for (const auto& e : rep.entries) {
        if (e.executed && e.outputVerified) {
            BOO_CHECK(std::isfinite(e.primaryScore) && e.primaryScore > 0.0);
            BOO_CHECK(e.timingMs.p50Ms > 0.0);
            if (e.isFallbackComparison) ++verifiedFallbacks;
        }
    }
    // All 4 D3D12 HLSL fallback benchmarks must execute and verify on any D3D12 device
    BOO_CHECK(verifiedFallbacks == 4);

    d3d.shutdown();
    return true;
}

struct ImageLumaMetrics {
    double meanLuma = 0.0;
    double p10Luma = 0.0;
    double p99Luma = 0.0;
};

ImageLumaMetrics computeRgba8LumaMetrics(const std::vector<uint8_t>& rgba) {
    ImageLumaMetrics out{};
    const size_t pixelCount = rgba.size() / 4;
    if (pixelCount == 0) return out;

    std::vector<float> lumas;
    lumas.reserve(pixelCount);
    double sum = 0.0;
    for (size_t i = 0; i < pixelCount; ++i) {
        const float r = static_cast<float>(rgba[i * 4 + 0]) / 255.0f;
        const float g = static_cast<float>(rgba[i * 4 + 1]) / 255.0f;
        const float b = static_cast<float>(rgba[i * 4 + 2]) / 255.0f;
        const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        lumas.push_back(y);
        sum += static_cast<double>(y);
    }
    std::sort(lumas.begin(), lumas.end());
    out.meanLuma = sum / static_cast<double>(pixelCount);
    out.p10Luma = static_cast<double>(lumas[static_cast<size_t>(0.10 * static_cast<double>(pixelCount - 1))]);
    out.p99Luma = static_cast<double>(lumas[static_cast<size_t>(0.99 * static_cast<double>(pixelCount - 1))]);
    return out;
}

double computeMeanAbsDiffRgb01(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 1.0;
    const size_t pixelCount = a.size() / 4;
    double sum = 0.0;
    for (size_t i = 0; i < pixelCount; ++i) {
        sum += std::abs(static_cast<int>(a[i * 4 + 0]) - static_cast<int>(b[i * 4 + 0])) / 255.0;
        sum += std::abs(static_cast<int>(a[i * 4 + 1]) - static_cast<int>(b[i * 4 + 1])) / 255.0;
        sum += std::abs(static_cast<int>(a[i * 4 + 2]) - static_cast<int>(b[i * 4 + 2])) / 255.0;
    }
    return sum / static_cast<double>(pixelCount * 3);
}

boo::FrameContext makeM1TestContext(int64_t samplePos, uint64_t presentationFrame, bool reducedFlash = false) {
    boo::FrameContext ctx{};
    ctx.presentationFrame = presentationFrame;
    ctx.samplePosition = samplePos;
    ctx.sampleRate = 48000;
    ctx.filmTimeSeconds = static_cast<double>(samplePos) / 48000.0;
    ctx.dtSeconds = 1.0 / 60.0;
    ctx.actIndex = samplePos < 960000 ? 0u : (samplePos < 1920000 ? 1u : 2u);
    ctx.actProgress = 0.5f;
    ctx.audioEnvelope = boo::computeAnalyticalAudioBands(samplePos, 48000).rms;
    ctx.moodIrisAngle = 0.5f;
    ctx.moodIntensity = 0.5f;
    ctx.edition = boo::Edition::Family;
    ctx.qualityPreset = boo::QualityPresetId::Balanced;
    ctx.showOverlay = true;
    ctx.reducedFlash = reducedFlash;
    return ctx;
}

// Byte-exact A/B fixtures isolate one input (audio, media, hardware or HUD).
// Their reference must not advance an unrelated temporal reconstruction history.
boo::FrameContext makeNativeReferenceContext(int64_t samplePos, uint64_t frame) {
    auto ctx = makeM1TestContext(samplePos, frame);
    ctx.dlssMode = boo::DlssMode::Off;
    ctx.rayReconstruction = false;
    return ctx;
}

// Freeze driver specialization before creating PSOs in bit-exact fixtures.
// Retail D3D12 may decline. Pixel assertions remain identical in that case.
bool freezeDriverProfilingForTest(boo::D3D12Context& d3d) {
    Microsoft::WRL::ComPtr<ID3D12Device6> device;
    if (FAILED(static_cast<ID3D12Device*>(d3d.deviceRaw())->QueryInterface(IID_PPV_ARGS(&device)))) return false;
    return SUCCEEDED(device->SetBackgroundProcessingMode(
        D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_PROFILING_BY_SYSTEM,
        D3D12_MEASUREMENTS_ACTION_DISCARD_PREVIOUS, nullptr, nullptr));
}

bool testM1HeadlessDeterminismAndDxrVerification() {
    using namespace boo;

    D3D12Context d3d;
    std::string err;
    BOO_CHECK(d3d.initialize(nullptr, 1280, 720, false, false, &err));

    QualityPresetSpec safeSpec{};
    safeSpec.id = QualityPresetId::Safe;
    safeSpec.label = "Safe";
    safeSpec.renderWidth = 1280;
    safeSpec.renderHeight = 720;
    safeSpec.allowDxr = false;
    safeSpec.rtBounces = 0;

    // 1. Mandatory Raster Verification (runs on every D3D12 adapter including WARP)
    std::vector<uint8_t> img5s, img30s, img48Fresh;
    {
        M1TaipeiSliceEffect rasterFx(d3d, safeSpec, /*disableRT=*/true, false);
        const auto initRes = rasterFx.probeAndInitialize();
        BOO_CHECK(initRes.status == BackendStatus::Substituted);
        BOO_CHECK(!initRes.successfullyExecutedTestWork);

        uint32_t w = 0, h = 0;
        BOO_CHECK(rasterFx.renderHeadlessCanvasForTest(makeM1TestContext(240000, 0), img5s, w, h, &err));
        BOO_CHECK(w == 1280 && h == 720 && img5s.size() == 1280u * 720u * 4u);

        BOO_CHECK(rasterFx.renderHeadlessCanvasForTest(makeM1TestContext(1440000, 1), img30s, w, h, &err));
        BOO_CHECK(rasterFx.renderHeadlessCanvasForTest(makeM1TestContext(2304000, 0), img48Fresh, w, h, &err));

        const auto m5 = computeRgba8LumaMetrics(img5s);
        const auto m30 = computeRgba8LumaMetrics(img30s);
        const auto m48 = computeRgba8LumaMetrics(img48Fresh);

        BOO_CHECK(m5.meanLuma > 0.02 && m5.meanLuma < 0.70 && (m5.p99Luma - m5.p10Luma) > 0.08);
        BOO_CHECK(m30.meanLuma > 0.02 && m30.meanLuma < 0.70 && (m30.p99Luma - m30.p10Luma) > 0.08);
        BOO_CHECK(m48.meanLuma > 0.02 && m48.meanLuma < 0.70 && (m48.p99Luma - m48.p10Luma) > 0.08);

        // Distinct 3-shot visual progression
        BOO_CHECK(computeMeanAbsDiffRgb01(img5s, img30s) > 0.015);
        BOO_CHECK(computeMeanAbsDiffRgb01(img30s, img48Fresh) > 0.015);

        // Flash safety across the 45.0s (sample 2,160,000) Glass Orchid reveal
        std::vector<uint8_t> rA, rB, rC;
        BOO_CHECK(rasterFx.renderHeadlessCanvasForTest(makeM1TestContext(2155200, 10, false), rA, w, h, &err));
        BOO_CHECK(rasterFx.renderHeadlessCanvasForTest(makeM1TestContext(2160000, 11, false), rB, w, h, &err));
        BOO_CHECK(rasterFx.renderHeadlessCanvasForTest(makeM1TestContext(2164800, 12, false), rC, w, h, &err));
        const double lA = computeRgba8LumaMetrics(rA).meanLuma;
        const double lB = computeRgba8LumaMetrics(rB).meanLuma;
        const double lC = computeRgba8LumaMetrics(rC).meanLuma;
        BOO_CHECK(std::abs(lB - lA) < 0.02 && std::abs(lC - lB) < 0.02);

        // Adjacent 60 Hz frames under --reduced-flash must have < 0.005 luma delta
        BOO_CHECK(rasterFx.renderHeadlessCanvasForTest(makeM1TestContext(2159200, 20, true), rA, w, h, &err));
        BOO_CHECK(rasterFx.renderHeadlessCanvasForTest(makeM1TestContext(2160000, 21, true), rB, w, h, &err));
        BOO_CHECK(rasterFx.renderHeadlessCanvasForTest(makeM1TestContext(2160800, 22, true), rC, w, h, &err));
        const double rfA = computeRgba8LumaMetrics(rA).meanLuma;
        const double rfB = computeRgba8LumaMetrics(rB).meanLuma;
        const double rfC = computeRgba8LumaMetrics(rC).meanLuma;
        BOO_CHECK(std::abs(rfB - rfA) < 0.005 && std::abs(rfC - rfB) < 0.005);

        rasterFx.shutdown();
    }

    // 2. Exact Byte-for-Byte Seek Determinism (independent of presentationFrame & prior history!)
    {
        M1TaipeiSliceEffect seekFx(d3d, safeSpec, /*disableRT=*/true, false);
        BOO_CHECK(seekFx.probeAndInitialize().status == BackendStatus::Substituted);

        std::vector<uint8_t> scratch, img48AfterSeek;
        uint32_t w = 0, h = 0;
        BOO_CHECK(seekFx.renderHeadlessCanvasForTest(makeM1TestContext(96000, 3), scratch, w, h, &err));
        BOO_CHECK(seekFx.renderHeadlessCanvasForTest(makeM1TestContext(1728000, 44), scratch, w, h, &err));
        seekFx.resetTo(48.0);
        auto seekCtx = makeM1TestContext(2304000, 999);
        seekCtx.seeking = true;
        BOO_CHECK(seekFx.renderHeadlessCanvasForTest(seekCtx, img48AfterSeek, w, h, &err));
        BOO_CHECK(img48AfterSeek == img48Fresh);
        seekFx.shutdown();
    }

    // 3. M1 Fault-to-M0-SafeFallback Orchestrator Honesty Check
    {
        FallbackOrchestrator orch;
        orch.setPrimaryAndFallbackEffects(
            std::make_unique<M1TaipeiSliceEffect>(d3d, safeSpec, true, /*simulateInitFailure=*/true),
            std::make_unique<M0TestVisualEffect>(d3d, true, false));
        orch.initializeEffects(false);
        BOO_CHECK(orch.isUsingFallbackEffect());
        const auto& rtState = orch.engineStatus(EngineKind::RT);
        BOO_CHECK(rtState.status == BackendStatus::Degraded);
        BOO_CHECK(!rtState.successfullyExecutedTestWork);
        orch.shutdown();
    }

    // 4. Conditional Hardware DXR 1.1 Inline RayQuery Verification
    ProbeFlags pf{};
    const auto caps = probeSystemCapabilities(pf);
    const bool hasDxr11Hw = !d3d.isWarp() &&
                            caps.dxrTierString != "None" &&
                            caps.dxrTierString != "Tier_1_0";
    if (!hasDxr11Hw) {
        std::cout << "  [SKIPPED: Hardware DXR Tier 1.1+ adapter not present; raster path verified]\n";
        d3d.shutdown();
        return true;
    }

    {
        QualityPresetSpec dxrSpec = safeSpec;
        dxrSpec.id = QualityPresetId::Balanced;
        dxrSpec.label = "Balanced720Test";
        dxrSpec.allowDxr = true;
        dxrSpec.rtBounces = 1;

        M1TaipeiSliceEffect dxrFx(d3d, dxrSpec, /*disableRT=*/false, false);
        const auto initRes = dxrFx.probeAndInitialize();
        BOO_CHECK(initRes.capabilityDetected);

        std::vector<uint8_t> dxrImg48s;
        uint32_t w = 0, h = 0;
        BOO_CHECK(dxrFx.renderHeadlessCanvasForTest(makeM1TestContext(2304000, 0), dxrImg48s, w, h, &err));

        HardwareResult liveRt{};
        BOO_CHECK(dxrFx.queryLiveEngineStatus(EngineKind::RT, liveRt));
        BOO_CHECK(liveRt.status == BackendStatus::Accelerated);
        BOO_CHECK(liveRt.successfullyExecutedTestWork);
        BOO_CHECK(dxrFx.lastVerifiedRayHits() > 0);

        const double dxrVsRasterDiff = computeMeanAbsDiffRgb01(dxrImg48s, img48Fresh);
        BOO_CHECK(dxrVsRasterDiff > 0.003 && dxrVsRasterDiff < 0.35);

        // Live RT toggle ('X') must immediately drop to Substituted & match raster byte-for-byte
        dxrFx.setDisableRt(true);
        std::vector<uint8_t> dxrDisabledImg48s;
        BOO_CHECK(dxrFx.renderHeadlessCanvasForTest(makeM1TestContext(2304000, 5), dxrDisabledImg48s, w, h, &err));

        HardwareResult disabledRt{};
        BOO_CHECK(dxrFx.queryLiveEngineStatus(EngineKind::RT, disabledRt));
        BOO_CHECK(disabledRt.status == BackendStatus::Substituted);
        BOO_CHECK(!disabledRt.successfullyExecutedTestWork);
        BOO_CHECK(dxrDisabledImg48s == img48Fresh);

        dxrFx.shutdown();
    }

    d3d.shutdown();
    return true;
}

bool testNativeCanvasModesAndCapture() {
    using namespace boo;
    D3D12Context d3d;
    std::string error;
    BOO_CHECK(d3d.initialize(nullptr, 640, 360, false, true, &error));
    auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    auto spec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    M1TaipeiSliceEffect fx(d3d, spec, false);
    BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);
    BOO_CHECK(fx.canvasWidth() == 2560 && fx.canvasHeight() == 1440);
    std::vector<uint8_t> full, compact, split, splitAgain, off;
    std::vector<uint32_t> packedFresh, packedSeek;
    uint32_t w = 0, h = 0;
    auto ctx = makeM1TestContext(2304000, 0);
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx, full, w, h, &error, &packedFresh));
    ctx.hudMode = 1;
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx, compact, w, h, &error));
    BOO_CHECK(full == compact);
    ctx.hudMode = 2;
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx, split, w, h, &error));
    ctx.presentationFrame = 999;
    fx.resetTo(48);
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx, splitAgain, w, h, &error));
    BOO_CHECK(split == splitAgain);
    HardwareResult rt;
    fx.queryLiveEngineStatus(EngineKind::RT, rt);
    if (rt.successfullyExecutedTestWork) BOO_CHECK(split != full);
    ctx.hudMode = 3;
    ctx.showOverlay = false;
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx, off, w, h, &error, &packedSeek));
    BOO_CHECK(full == off);
    BOO_CHECK(packedFresh == packedSeek);
    BOO_CHECK(std::any_of(packedFresh.begin(), packedFresh.end(), [](uint32_t pixel) { return (pixel & 3u) == 1u || (pixel & 3u) == 2u; }));
    spec = findQualityPresetSpec(presets, QualityPresetId::Experimental4K);
    BOO_CHECK(fx.syncPresetBeforeFrame(spec, true));
    BOO_CHECK(fx.canvasWidth() == 3840 && fx.canvasHeight() == 2160);
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx, off, w, h, &error));
    BOO_CHECK(off.size() == 3840u * 2160u * 4u);
    const auto dir = std::filesystem::path("diagnostics/test_captures") / std::to_string(GetTickCount64());
    ctx.hudMode = 2;
    BOO_CHECK(fx.captureClean(ctx, dir, &error));
    size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        ++files;
        if (entry.path().extension() == ".json") {
            auto parsed = parseJsonFile(entry.path());
            BOO_CHECK(parsed.ok && parsed.value["samplePosition"].asInt64() == 2304000);
            BOO_CHECK(parsed.value["width"].asInt64() == 3840);
        } else {
            std::ifstream in(entry.path(), std::ios::binary);
            BITMAPFILEHEADER header{}; BITMAPINFOHEADER info{};
            in.read(reinterpret_cast<char*>(&header), sizeof(header));
            in.read(reinterpret_cast<char*>(&info), sizeof(info));
            BOO_CHECK(header.bfType == 0x4D42 && info.biWidth == 3840 && info.biHeight == -2160);
            std::vector<uint8_t> pixels(off.size());
            in.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
            for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
            BOO_CHECK(pixels == off);
        }
    }
    BOO_CHECK(files == 2);
    std::atomic<bool> cancel{true};
    BenchmarkConfig config;
    config.cancelRequested = &cancel;
    auto report = runAdHocBenchmarks(d3d.deviceRaw(), d3d.commandQueueRaw(), d3d.adapterLuidLow(), d3d.adapterLuidHigh(), d3d.adapterName(), "test", config);
    BOO_CHECK(report.entries.empty());
    fx.shutdown();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    return true;
}

bool testNoActivateWindowTransitions() {
    using namespace boo;
    const HWND foreground = GetForegroundWindow();
    AppWindow window;
    BOO_CHECK(window.create("BOO lifecycle test", 640, 360, false, true));
    const std::pair<WPARAM, TransportCommand> keys[] = {
        {VK_OEM_COMMA, TransportCommand::StepBack}, {VK_OEM_PERIOD, TransportCommand::StepForward},
        {VK_OEM_4, TransportCommand::StepBack}, {VK_OEM_6, TransportCommand::StepForward},
        {'G', TransportCommand::RevealPreroll}, {VK_OEM_MINUS, TransportCommand::VolumeDown},
        {VK_OEM_PLUS, TransportCommand::VolumeUp}, {VK_UP, TransportCommand::VolumeUp},
        {VK_DOWN, TransportCommand::VolumeDown}, {VK_F12, TransportCommand::CaptureClean},
        {VK_TAB, TransportCommand::ToggleOverlay}, {'B', TransportCommand::TriggerBenchSweep},
        {'R', TransportCommand::Restart}, {'N', TransportCommand::NextAct}, {'P', TransportCommand::PrevAct},
        {'X', TransportCommand::ToggleRtRuntime}, {'C', TransportCommand::ToggleCudaRuntime},
        {'T', TransportCommand::ToggleTensorRuntime}, {'V', TransportCommand::ToggleCodecRuntime},
        {'A', TransportCommand::CycleAbCompareEngine},
        {'F', TransportCommand::ToggleExploreMode}, {'J', TransportCommand::TogglePracticalLamps},
        {'H', TransportCommand::ToggleRtxSplitWipe}, {VK_F8, TransportCommand::ResetInteractiveLighting},
        {VK_F2, TransportCommand::HeroPreset1}, {VK_F3, TransportCommand::HeroPreset2},
        {VK_F4, TransportCommand::HeroPreset3}, {VK_F5, TransportCommand::HeroPreset4},
        {VK_F6, TransportCommand::HeroPreset5}, {VK_F7, TransportCommand::HeroPreset6}
    };
    for (const auto& [key, expected] : keys) {
        SendMessageW(window.hwnd(), WM_KEYDOWN, key, 0);
        TransportCommand actual;
        BOO_CHECK(window.popCommand(actual) && actual == expected);
        SendMessageW(window.hwnd(), WM_KEYDOWN, key, 0x40000000);
        BOO_CHECK(!window.popCommand(actual));
    }
    D3D12Context d3d;
    std::string error;
    BOO_CHECK(d3d.initialize(window.hwnd(), window.clientWidth(), window.clientHeight(), false, true, &error));
    QualityPresetSpec spec;
    spec.renderWidth = 640; spec.renderHeight = 360;
    M1TaipeiSliceEffect fx(d3d, spec, false);
    BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> benchQueue;
    D3D12_COMMAND_QUEUE_DESC qd{};
    BOO_CHECK(SUCCEEDED(static_cast<ID3D12Device*>(d3d.deviceRaw())->CreateCommandQueue(&qd, IID_PPV_ARGS(&benchQueue))));
    std::atomic<bool> cancel{false};
    BenchmarkConfig benchConfig;
    benchConfig.cancelRequested = &cancel;
    benchConfig.timedIterations = 500;
    benchConfig.disableCUDA = benchConfig.disableTensor = benchConfig.disableNVENC = true;
    std::jthread worker([&] {
        runAdHocBenchmarks(d3d.deviceRaw(), benchQueue.Get(), d3d.adapterLuidLow(), d3d.adapterLuidHigh(), d3d.adapterName(), "test", benchConfig);
    });
    for (uint32_t mode = 0; mode < 4; ++mode) {
        BOO_CHECK(d3d.resize(640 + mode * 16, 360 + mode * 9));
        BOO_CHECK(d3d.beginFrame());
        auto ctx = makeM1TestContext(2304000, mode);
        ctx.hudMode = mode;
        fx.render(ctx);
        double ms = 0;
        BOO_CHECK(d3d.endFrameAndPresent(false, ms));
    }
    cancel.store(true, std::memory_order_release);
    worker.join();
    SendMessageW(window.hwnd(), WM_SIZE, SIZE_MINIMIZED, 0);
    BOO_CHECK(window.isMinimized());
    BOO_CHECK(d3d.resize(0, 0));
    SendMessageW(window.hwnd(), WM_SIZE, SIZE_RESTORED, MAKELPARAM(640, 360));
    BOO_CHECK(!window.isMinimized());
    window.toggleBorderlessFullscreen();
    window.toggleBorderlessFullscreen();
    BOO_CHECK(GetForegroundWindow() == foreground);
    d3d.waitForGpu();
    fx.shutdown();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    d3d.shutdown();
    window.destroy();
    MSG msg{}; while (PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    return true;
}

bool testWasapiPresentationClockAndMigrationLifecycle() {
    using namespace boo;
    AudioEngine audio;
    audio.setMuted(true); // Never emit audible disruption during automated testing
    if (!audio.initialize(false)) {
        std::cout << "  [SKIPPED: No active WASAPI output endpoint available]\n";
        return true;
    }

    TimelineClock clk;
    clk.configure(48000, 2880000);
    clk.setAudioSampleReader([&](int64_t& out) { return audio.readPlayedSamples48k(out); });
    clk.start(true, false);
    BOO_CHECK(clk.source() == ClockSource::AudioSampleClock);
    BOO_CHECK(audio.usesHardwarePresentationClock());

    int64_t prevPresented = 0;
    BOO_CHECK(audio.readPlayedSamples48k(prevPresented));
    for (int step = 0; step < 6; ++step) {
        Sleep(18);
        int64_t curPresented = 0;
        BOO_CHECK(audio.readPlayedSamples48k(curPresented));
        BOO_CHECK(curPresented >= prevPresented); // Strict monotonic speaker presentation position
        const int64_t curSubmitted = audio.submittedFrames48k();
        BOO_CHECK(curPresented <= curSubmitted);  // Speaker cannot present frames not yet submitted
        prevPresented = curPresented;
    }
    BOO_CHECK(prevPresented > 0);
    const double bufLatMs = audio.estimatedBufferLatencyMs();
    BOO_CHECK(bufLatMs >= 0.0 && bufLatMs < 250.0);

    // Pause -> repeated seek -> resume -> harmless migration poll
    clk.pause();
    audio.setPaused(true);
    Sleep(25);
    const int64_t pausedSample = clk.tickFrame().samplePosition;
    Sleep(20);
    BOO_CHECK(clk.tickFrame().samplePosition == pausedSample);

    clk.seekToSample(2160000);
    audio.resetPhase(2160000);
    clk.seekToSample(2304000);
    audio.resetPhase(2304000);
    BOO_CHECK(!audio.pollEndpointMigration(2304000));

    audio.setPaused(false);
    clk.resume();
    Sleep(35);
    const auto resumedSnap = clk.tickFrame();
    BOO_CHECK(resumedSnap.source == ClockSource::AudioSampleClock);
    BOO_CHECK(resumedSnap.samplePosition > 2304000);

    audio.shutdown();
    clk.shutdown();
    return true;
}

bool testLiveFourPillarHardwareAttributionAndAbSplits() {
    using namespace boo;
    D3D12Context d3d;
    std::string err;
    BOO_CHECK(d3d.initialize(nullptr, 960, 540, false, true, &err));

    ProbeFlags pf{};
    const auto caps = probeSystemCapabilities(pf);

    QualityPresetSpec spec{};
    spec.id = QualityPresetId::Showcase;
    spec.label = "ShowcaseTest";
    spec.renderWidth = 960;
    spec.renderHeight = 540;
    spec.allowDxr = true;
    spec.allowCuda = true;
    spec.allowTensor = true;
    spec.rtBounces = 1;

    M1TaipeiSliceEffect fx(d3d, spec, false, false, false, false, false, false);
    BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);

    uint32_t w = 0, h = 0;
    std::vector<uint8_t> imgAllLive, imgNoCuda, imgNoTensor, imgFinale56s;
    auto ctx48 = makeM1TestContext(2304000, 0);
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx48, imgAllLive, w, h, &err));
    BOO_CHECK(w == 960 && h == 540);

    // Verify Taipei 101 56.0s Finale framing produces distinct, rich exposure
    auto ctx56 = makeM1TestContext(2688000, 1);
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx56, imgFinale56s, w, h, &err));
    const auto m56 = computeRgba8LumaMetrics(imgFinale56s);
    BOO_CHECK(m56.meanLuma > 0.02 && m56.meanLuma < 0.72 && (m56.p99Luma - m56.p10Luma) > 0.08);
    BOO_CHECK(computeMeanAbsDiffRgb01(imgAllLive, imgFinale56s) > 0.02);

    // Re-render 48.0s canonical to check live 4-pillar statuses
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx48, imgAllLive, w, h, &err));

    HardwareResult rtSt{}, cudaSt{}, tensorSt{}, codecSt{};
    BOO_CHECK(fx.queryLiveEngineStatus(EngineKind::RT, rtSt));
    BOO_CHECK(fx.queryLiveEngineStatus(EngineKind::CUDA, cudaSt));
    BOO_CHECK(fx.queryLiveEngineStatus(EngineKind::Tensor, tensorSt));
    BOO_CHECK(fx.queryLiveEngineStatus(EngineKind::Codec, codecSt));
    // Codec is OFF/standby until 'V' or Codec A/B is toggled!
    BOO_CHECK(codecSt.status == BackendStatus::Unavailable);

    if (!d3d.isWarp() && caps.cudaMatchesD3D12Luid) {
        const auto cTel = fx.livePillarTelemetry(1);
        BOO_CHECK(cudaSt.status == BackendStatus::Accelerated);
        BOO_CHECK(cTel.hardwareVerified);
        BOO_CHECK(cTel.workloadItems == 65536u && cTel.workloadSteps == 16u);
        BOO_CHECK(cTel.executionMs > 0.0f && cTel.throughput > 0.0f);

        // Toggling CUDA off ('C') switches to 8.2K HLSL substitute and visibly changes rendered 3D particles!
        fx.setDisableCuda(true);
        BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx48, imgNoCuda, w, h, &err));
        HardwareResult cudaSub{};
        BOO_CHECK(fx.queryLiveEngineStatus(EngineKind::CUDA, cudaSub));
        BOO_CHECK(cudaSub.status == BackendStatus::Substituted);
        BOO_CHECK(!fx.livePillarTelemetry(1).hardwareVerified);
        BOO_CHECK(imgNoCuda != imgAllLive);
        fx.setDisableCuda(false);

        const auto tTel = fx.livePillarTelemetry(2);
        if (tTel.hardwareVerified) {
            BOO_CHECK(tensorSt.status == BackendStatus::Accelerated);
            BOO_CHECK(tTel.workloadSteps == 512u && tTel.executionMs > 0.0f && tTel.throughput > 0.0f);

            // Toggling Tensor off ('T') switches to scalar HLSL substitute and visibly changes caustic/petal light!
            fx.setDisableTensor(true);
            BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx48, imgNoTensor, w, h, &err));
            HardwareResult tensorSub{};
            BOO_CHECK(fx.queryLiveEngineStatus(EngineKind::Tensor, tensorSub));
            BOO_CHECK(tensorSub.status == BackendStatus::Substituted);
            BOO_CHECK(!fx.livePillarTelemetry(2).hardwareVerified);
            BOO_CHECK(imgNoTensor != imgAllLive);
            fx.setDisableTensor(false);
        }
    }

    // Verify HUD Mode 0 (LIVE GPU + BENCH [B] separation) and all 4 A/B Split-Screen pillars (RT, CUDA, Tensor, Codec)
    HudTelemetrySnapshot hud{};
    hud.fps = 60.0f;
    hud.gpuFrameMs = 2.4f;
    hud.p95FrameMs = 2.8f;
    hud.durationSeconds = 60.0;
    hud.audioClockActive = true;
    hud.benchRunning = false;
    hud.benchHasResults = true; // Simulate completed standalone benchmark sweep
    hud.benchPrimaryValues = {999.0f, 888.0f, 777.0f, 666.0f};
    hud.benchBarFill01 = {0.9f, 0.8f, 0.7f, 0.6f};

    std::vector<uint8_t> presHud0, presCodecHijack;
    ctx48.hudMode = 0;
    ctx48.showOverlay = true;
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx48, hud, 960, 540, presHud0, &err));
    BOO_CHECK(presHud0.size() == 960u * 540u * 4u);
    // Standalone bench values in HUD must NEVER overwrite live pillar telemetry!
    BOO_CHECK(std::abs(fx.livePillarTelemetry(0).throughput - 999.0f) > 0.01f);

    // Activate 'V' Live Codec Signal Hijack -> verify real H.264 MFT round-trip telemetry & visible compression difference
    fx.setCodecSignalHijackActive(true);
    ctx48.hudMode = 3;
    ctx48.showOverlay = false;
    std::vector<uint8_t> presCleanCinema;
    fx.setCodecSignalHijackActive(false);
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx48, hud, 960, 540, presCleanCinema, &err));
    fx.setCodecSignalHijackActive(true);
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx48, hud, 960, 540, presCodecHijack, &err));
    HardwareResult codecLive{};
    BOO_CHECK(fx.queryLiveEngineStatus(EngineKind::Codec, codecLive));
    if (!d3d.isWarp() && codecLive.status == BackendStatus::Accelerated) {
        const auto vTel = fx.livePillarTelemetry(3);
        BOO_CHECK(vTel.hardwareVerified);
        BOO_CHECK(vTel.executionMs > 0.0f && vTel.qualityOrBitrate > 18.0f);
        BOO_CHECK(computeMeanAbsDiffRgb01(presCleanCinema, presCodecHijack) > 0.0015);
    }
    fx.setCodecSignalHijackActive(false);

    // Exercise all 4 single-variable A/B split modes in HUD Mode 2
    ctx48.hudMode = 2;
    ctx48.showOverlay = true;
    const AbCompareEngine abModes[4] = {
        AbCompareEngine::RT, AbCompareEngine::CUDA, AbCompareEngine::Tensor, AbCompareEngine::Codec
    };
    for (AbCompareEngine ab : abModes) {
        fx.setAbCompareEngine(ab);
        std::vector<uint8_t> presAb;
        BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx48, hud, 960, 540, presAb, &err));
        BOO_CHECK(presAb.size() == 960u * 540u * 4u);
    }

    fx.shutdown();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    d3d.shutdown();
    return true;
}

bool testLiveNoActivateSwapchainDeterminism() {
    using namespace boo;
    const HWND foreground = GetForegroundWindow();
    AppWindow window;
    BOO_CHECK(window.create("BOO live swapchain determinism test", 960, 540, false, true));

    D3D12Context d3d;
    std::string err;
    BOO_CHECK(d3d.initialize(window.hwnd(), window.clientWidth(), window.clientHeight(), false, true, &err));

    CueSheet cueSheet{};
    ValidationReport rep{};
    BOO_CHECK(loadCueSheetFromFile(resolveResourcePath("assets/cues/m1_taipei_60s.json"), cueSheet, rep));

    auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    QualityPresetSpec showcaseSpec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    showcaseSpec.renderWidth = 960;
    showcaseSpec.renderHeight = 540;
    QualityPresetSpec safeSpec = findQualityPresetSpec(presets, QualityPresetId::Safe);
    safeSpec.renderWidth = 640;
    safeSpec.renderHeight = 360;

    M1TaipeiSliceEffect fx(d3d, showcaseSpec, false);
    BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);

    auto buildLiveCtx = [&](int64_t samplePos, uint64_t frameNo, uint32_t hudMode, bool overlay) {
        const size_t actIdx = findSceneIndexAtSample(cueSheet, samplePos);
        const auto& cue = cueSheet.scenes[actIdx];
        FrameContext ctx{};
        ctx.presentationFrame = frameNo;
        ctx.samplePosition = samplePos;
        ctx.sampleRate = cueSheet.sampleRate;
        ctx.filmTimeSeconds = static_cast<double>(samplePos) / static_cast<double>(cueSheet.sampleRate);
        ctx.dtSeconds = 1.0 / 60.0;
        ctx.randomSeed = cue.deterministicSeed;
        ctx.actIndex = static_cast<uint32_t>(actIdx);
        const double span = std::max(1.0, static_cast<double>(cue.endSample - cue.startSample));
        ctx.actProgress = static_cast<float>((static_cast<double>(samplePos - cue.startSample)) / span);
        ctx.audioEnvelope = computeAnalyticalAudioBands(samplePos, cueSheet.sampleRate).rms;
        ctx.edition = Edition::Family;
        ctx.qualityPreset = QualityPresetId::Showcase;
        ctx.showOverlay = overlay;
        ctx.hudMode = hudMode;
        ScriptedDirector sampleDirector;
        sampleDirector.reset(cueSheet.seed);
        const auto mood = sampleDirector.evaluate(ctx, &cue);
        ctx.moodIrisAngle = mood.irisAngle;
        ctx.moodIntensity = mood.lightTension;
        return ctx;
    };

    auto presentLiveFrame = [&](const FrameContext& ctx) -> bool {
        if (!d3d.beginFrame()) return false;
        fx.render(ctx);
        double gpuMs = 0.0;
        return d3d.endFrameAndPresent(false, gpuMs);
    };

    constexpr int64_t kTargetSample = 2304000; // 48.000 s
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgbaRef, rgbaScratch;
    std::vector<uint32_t> rgb10Path1, rgb10Path2, rgb10Path3, rgb10Path4, rgb10Path5;

    // Path 1: Direct seek to 48.0s in HUD Mode 0
    fx.resetTo(48.0);
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample, 0, 0, true)));
    BOO_CHECK(fx.readLastRenderedCanvasForTest(rgbaRef, w, h, &err, &rgb10Path1));
    BOO_CHECK(w == 960 && h == 540 && !rgb10Path1.empty());

    // Path 2: Continuous playback stepping across prior samples in HUD Mode 1 with active benchmark telemetry
    HudTelemetrySnapshot noisyHud{};
    noisyHud.benchRunning = true;
    noisyHud.benchOverallProgress01 = 0.65f;
    fx.setHudTelemetry(noisyHud);
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample - 2400, 10, 1, true)));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample - 1600, 11, 1, true)));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample - 800,  12, 1, true)));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample,        13, 1, true)));
    BOO_CHECK(fx.readLastRenderedCanvasForTest(rgbaScratch, w, h, &err, &rgb10Path2));
    BOO_CHECK(rgb10Path2 == rgb10Path1);

    // Path 3: Overshoot to 2,305,600, pause, backward-step (-800, -800) in HUD Mode 2 (CUDA A/B Split active!)
    fx.setAbCompareEngine(AbCompareEngine::CUDA);
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample + 1600, 30, 2, true)));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample + 800,  31, 2, true)));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample,        32, 2, true)));
    BOO_CHECK(fx.readLastRenderedCanvasForTest(rgbaScratch, w, h, &err, &rgb10Path3));
    BOO_CHECK(rgb10Path3 == rgb10Path1);

    // Path 4: Toggle all 4 pillars OFF on intermediate frame, then restore canonical in HUD Mode 2 (Tensor A/B Split)
    fx.setDisableRt(true);
    fx.setDisableCuda(true);
    fx.setDisableTensor(true);
    fx.setCodecSignalHijackActive(true);
    BOO_CHECK(presentLiveFrame(buildLiveCtx(960000, 50, 0, true)));
    fx.setDisableRt(false);
    fx.setDisableCuda(false);
    fx.setDisableTensor(false);
    fx.setCodecSignalHijackActive(false);
    fx.setAbCompareEngine(AbCompareEngine::Tensor);
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample, 51, 2, true)));
    BOO_CHECK(fx.readLastRenderedCanvasForTest(rgbaScratch, w, h, &err, &rgb10Path4));
    BOO_CHECK(rgb10Path4 == rgb10Path1);

    // Path 5: Preset transition Showcase -> Safe -> Showcase, then render clean cinema (HUD Mode 3)
    BOO_CHECK(fx.syncPresetBeforeFrame(safeSpec, true));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(240000, 70, 3, false)));
    BOO_CHECK(fx.syncPresetBeforeFrame(showcaseSpec, true));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample, 71, 3, false)));
    BOO_CHECK(fx.readLastRenderedCanvasForTest(rgbaScratch, w, h, &err, &rgb10Path5));
    BOO_CHECK(rgb10Path5 == rgb10Path1);

    BOO_CHECK(GetForegroundWindow() == foreground);
    d3d.waitForGpu();
    fx.shutdown();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    d3d.shutdown();
    window.destroy();
    MSG msg{}; while (PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    return true;
}

bool testM2LiveNoActivateSwapchainDeterminism() {
    using namespace boo;
    const HWND foreground = GetForegroundWindow();
    AppWindow window;
    BOO_CHECK(window.create("BOO live swapchain determinism test", 960, 540, false, true));

    D3D12Context d3d;
    std::string err;
    BOO_CHECK(d3d.initialize(window.hwnd(), window.clientWidth(), window.clientHeight(), false, true, &err));
    freezeDriverProfilingForTest(d3d);

    CueSheet cueSheet{};
    ValidationReport rep{};
    BOO_CHECK(loadCueSheetFromFile(resolveResourcePath("assets/cues/m2_trilogy_180s.json"), cueSheet, rep));

    auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    QualityPresetSpec showcaseSpec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    showcaseSpec.renderWidth = 960;
    showcaseSpec.renderHeight = 540;
    QualityPresetSpec safeSpec = findQualityPresetSpec(presets, QualityPresetId::Safe);
    safeSpec.renderWidth = 640;
    safeSpec.renderHeight = 360;

    M1TaipeiSliceEffect fx(d3d, showcaseSpec, false, false, false, false, false, false, true);
    BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);

    auto buildLiveCtx = [&](int64_t samplePos, uint64_t frameNo, uint32_t hudMode, bool overlay) {
        const size_t actIdx = findSceneIndexAtSample(cueSheet, samplePos);
        const auto& cue = cueSheet.scenes[actIdx];
        FrameContext ctx{};
        // Compare the same native path across direct seeks, continuous playback and split A/B.
        ctx.dlssMode = DlssMode::Off;
        ctx.rayReconstruction = false;
        ctx.presentationFrame = frameNo;
        ctx.samplePosition = samplePos;
        ctx.sampleRate = cueSheet.sampleRate;
        ctx.filmTimeSeconds = static_cast<double>(samplePos) / static_cast<double>(cueSheet.sampleRate);
        ctx.dtSeconds = 1.0 / 60.0;
        ctx.randomSeed = cue.deterministicSeed;
        ctx.actIndex = static_cast<uint32_t>(actIdx);
        const double span = std::max(1.0, static_cast<double>(cue.endSample - cue.startSample));
        ctx.actProgress = static_cast<float>((static_cast<double>(samplePos - cue.startSample)) / span);
        ctx.audioEnvelope = computeAnalyticalAudioBands(samplePos, cueSheet.sampleRate).rms;
        ctx.edition = Edition::Family;
        ctx.qualityPreset = QualityPresetId::Showcase;
        ctx.showOverlay = overlay;
        ctx.hudMode = hudMode;
        ScriptedDirector sampleDirector;
        sampleDirector.reset(cueSheet.seed);
        const auto mood = sampleDirector.evaluate(ctx, &cue);
        ctx.moodIrisAngle = mood.irisAngle;
        ctx.moodIntensity = mood.lightTension;
        return ctx;
    };

    auto presentLiveFrame = [&](const FrameContext& ctx) -> bool {
        if (!d3d.beginFrame()) return false;
        fx.render(ctx);
        double gpuMs = 0.0;
        return d3d.endFrameAndPresent(false, gpuMs);
    };

    for(const int64_t kTargetSample : {720000LL,2160000LL,3600000LL,5184000LL,8064000LL,8448000LL}) {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgbaRef, rgbaScratch;
    std::vector<uint32_t> rgb10Path1, rgb10Path2, rgb10Path3, rgb10Path4, rgb10Path5;

    // Path 1: Direct seek to 48.0s in HUD Mode 0
    fx.resetTo(48.0);
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample, 0, 0, true)));
    BOO_CHECK(fx.readLastRenderedCanvasForTest(rgbaRef, w, h, &err, &rgb10Path1));
    BOO_CHECK(w == 960 && h == 540 && !rgb10Path1.empty());

    // Path 2: Continuous playback stepping across prior samples in HUD Mode 1 with active benchmark telemetry
    HudTelemetrySnapshot noisyHud{};
    noisyHud.benchRunning = true;
    noisyHud.benchOverallProgress01 = 0.65f;
    fx.setHudTelemetry(noisyHud);
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample - 2400, 10, 1, true)));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample - 1600, 11, 1, true)));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample - 800,  12, 1, true)));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample,        13, 1, true)));
    BOO_CHECK(fx.readLastRenderedCanvasForTest(rgbaScratch, w, h, &err, &rgb10Path2));
    BOO_CHECK(rgb10Path2 == rgb10Path1);

    // Path 3: Overshoot to 2,305,600, pause, backward-step (-800, -800) in HUD Mode 2 (CUDA A/B Split active!)
    fx.setAbCompareEngine(AbCompareEngine::CUDA);
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample + 1600, 30, 2, true)));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample + 800,  31, 2, true)));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample,        32, 2, true)));
    BOO_CHECK(fx.readLastRenderedCanvasForTest(rgbaScratch, w, h, &err, &rgb10Path3));
    BOO_CHECK(rgb10Path3 == rgb10Path1);

    // Path 4: Toggle all 4 pillars OFF on intermediate frame, then restore canonical in HUD Mode 2 (Tensor A/B Split)
    fx.setDisableRt(true);
    fx.setDisableCuda(true);
    fx.setDisableTensor(true);
    fx.setCodecSignalHijackActive(true);
    BOO_CHECK(presentLiveFrame(buildLiveCtx(960000, 50, 0, true)));
    fx.setDisableRt(false);
    fx.setDisableCuda(false);
    fx.setDisableTensor(false);
    fx.setCodecSignalHijackActive(false);
    fx.setAbCompareEngine(AbCompareEngine::Tensor);
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample, 51, 2, true)));
    BOO_CHECK(fx.readLastRenderedCanvasForTest(rgbaScratch, w, h, &err, &rgb10Path4));
    BOO_CHECK(rgb10Path4 == rgb10Path1);

    // Path 5: Preset transition Showcase -> Safe -> Showcase, then render clean cinema (HUD Mode 3)
    BOO_CHECK(fx.syncPresetBeforeFrame(safeSpec, true));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(240000, 70, 3, false)));
    BOO_CHECK(fx.syncPresetBeforeFrame(showcaseSpec, true));
    BOO_CHECK(presentLiveFrame(buildLiveCtx(kTargetSample, 71, 3, false)));
    BOO_CHECK(fx.readLastRenderedCanvasForTest(rgbaScratch, w, h, &err, &rgb10Path5));
    BOO_CHECK(rgb10Path5 == rgb10Path1);

    }
    BOO_CHECK(GetForegroundWindow() == foreground);
    d3d.waitForGpu();
    fx.shutdown();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    d3d.shutdown();
    window.destroy();
    MSG msg{}; while (PeekMessageW(&msg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    return true;
}

bool testM2CapturesAndDeterminism() {
    using namespace boo; D3D12Context d3d;std::string err;
    BOO_CHECK(d3d.initialize(nullptr,2560,1440,false,true,&err));
    freezeDriverProfilingForTest(d3d);
    auto presets=loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    auto spec=findQualityPresetSpec(presets,QualityPresetId::Showcase);
    M1TaipeiSliceEffect fx(d3d,spec,false,false,false,false,false,false,true);
    BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);
    auto dir=std::filesystem::path("build/trilogy_captures");std::filesystem::create_directories(dir);
    auto save=[&](const std::string& name,const std::vector<uint8_t>& rgba,uint32_t w,uint32_t h){
        std::ofstream out(dir/(name+".ppm"),std::ios::binary);out<<"P6\n"<<w<<" "<<h<<"\n255\n";
        for(size_t i=0;i<rgba.size();i+=4)out.write(reinterpret_cast<const char*>(rgba.data()+i),3);
    };
    uint32_t w=0,h=0;std::vector<uint8_t> rgba,again;std::vector<uint32_t> ref,actual;
    for(int sec:{15,45,75,108,168,176}) {
        auto ctx=makeNativeReferenceContext(int64_t(sec)*48000,0);ctx.showOverlay=false;ctx.hudMode=3;
        BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx,rgba,w,h,&err,&ref));save(std::to_string(sec),rgba,w,h);
        BOO_CHECK(w==2560&&h==1440);auto metrics=computeRgba8LumaMetrics(rgba);
        std::cout<<"M2 "<<sec<<"s mean="<<metrics.meanLuma<<" RT hits="<<fx.lastVerifiedRayHits()<<"\n";
        fx.resetTo(0);BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx,again,w,h,&err,&actual));BOO_CHECK(ref==actual);
    }
    HudTelemetrySnapshot hud{};hud.durationSeconds=180;hud.fps=0;hud.gpuFrameMs=0;hud.p95FrameMs=0;hud.vramUsedMb=0;hud.vramBudgetMb=0;
    // Display measured standalone results only when a prior smoke benchmark receipt exists.
    auto bench=parseJsonFile("build/x64-release/diagnostics/benchmarks_latest.json");
    if(bench.ok)for(const auto& entry:bench.value["benchmarks"].asArray()) {
        if(!entry["executed"].asBool()||!entry["output_verified"].asBool()||entry["is_fallback_comparison"].asBool())continue;
        const auto category=entry["category"].asString();const size_t index=category=="RT"?0:category=="CUDA"?1:category=="Tensor"?2:3;
        hud.benchHasResults=true;hud.benchPrimaryValues[index]=static_cast<float>(entry["primary_score"].asNumber());
    }
    auto ctx=makeM1TestContext(8064000,0);ctx.showOverlay=true;ctx.hudMode=0;
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx,hud,2560,1440,rgba,&err));save("hud",rgba,2560,1440);
    ctx.hudMode=2;
    for(auto mode:{AbCompareEngine::RT,AbCompareEngine::CUDA,AbCompareEngine::Tensor,AbCompareEngine::Codec}) {
        fx.setAbCompareEngine(mode);BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx,hud,2560,1440,rgba,&err));save("ab_"+std::to_string(static_cast<int>(mode)),rgba,2560,1440);
    }
    fx.shutdown();
    spec.renderWidth=640;spec.renderHeight=360;
    M1TaipeiSliceEffect fallback(d3d,spec,false,false,true,true,true,false,true,true);
    BOO_CHECK(fallback.probeAndInitialize().status!=BackendStatus::Faulted);
    BOO_CHECK(fallback.renderHeadlessCanvasForTest(ctx,rgba,w,h,&err));fallback.shutdown();
    BOO_CHECK(d3d.debugErrorCount()==0);return true;
}

bool testStandaloneHardwareSolosRtCudaTensor() {
    using namespace boo;

    D3D12Context d3d;
    std::string err;
    BOO_CHECK(d3d.initialize(nullptr, 960, 540, false, true, &err));
    freezeDriverProfilingForTest(d3d);

    ProbeFlags pf{};
    const auto caps = probeSystemCapabilities(pf);

    auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    QualityPresetSpec showcaseSpec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    showcaseSpec.renderWidth = 960;
    showcaseSpec.renderHeight = 540;
    QualityPresetSpec balancedSpec = findQualityPresetSpec(presets, QualityPresetId::Balanced);
    balancedSpec.renderWidth = 960;
    balancedSpec.renderHeight = 540;

    uint32_t w = 0, h = 0;

    // 1. M2 Standalone SoloRT (Hall of Mirrors)
    {
        M1TaipeiSliceEffect fxRt(d3d, showcaseSpec, false, false, false, false, false, false, false, false, ShowVisualMode::SoloRT);
        const auto st = fxRt.probeAndInitialize();
        BOO_CHECK(st.status != BackendStatus::Faulted);

        std::vector<uint8_t> imgRtOn, imgRtAgain, imgRtOff;
        std::vector<uint32_t> rgb10A, rgb10B;
        auto ctxRt = makeNativeReferenceContext(1152000, 0); // 24.0s
        ctxRt.randomSeed = 302001u;
        BOO_CHECK(fxRt.renderHeadlessCanvasForTest(ctxRt, imgRtOn, w, h, &err, &rgb10A));
        BOO_CHECK(w == 960 && h == 540);
        const auto mRt = computeRgba8LumaMetrics(imgRtOn);
        BOO_CHECK(mRt.meanLuma > 0.01 && (mRt.p99Luma - mRt.p10Luma) > 0.05);
        fxRt.resetTo(0.0);
        BOO_CHECK(fxRt.renderHeadlessCanvasForTest(ctxRt, imgRtAgain, w, h, &err, &rgb10B));
        BOO_CHECK(rgb10A == rgb10B);

        HardwareResult rtLive{};
        BOO_CHECK(fxRt.queryLiveEngineStatus(EngineKind::RT, rtLive));
        if (!d3d.isWarp() && rtLive.status == BackendStatus::Accelerated) {
            BOO_CHECK(fxRt.lastVerifiedRayHits() > 0);
            fxRt.setDisableRt(true);
            BOO_CHECK(fxRt.renderHeadlessCanvasForTest(ctxRt, imgRtOff, w, h, &err));
            HardwareResult rtSub{};
            BOO_CHECK(fxRt.queryLiveEngineStatus(EngineKind::RT, rtSub));
            BOO_CHECK(rtSub.status == BackendStatus::Substituted);
            BOO_CHECK(imgRtOff != imgRtOn);
            fxRt.setDisableRt(false);
        }
        fxRt.shutdown();
    }

    // 2. M3 Standalone SoloCUDA (Particle Mutiny >= 100k GPU-resident particles + seeded morph + HLSL fallback)
    {
        M1TaipeiSliceEffect fxCuda(d3d, showcaseSpec, false, false, false, false, false, false, false, false, ShowVisualMode::SoloCUDA);
        const auto st = fxCuda.probeAndInitialize();
        BOO_CHECK(st.status != BackendStatus::Faulted);

        std::vector<uint8_t> imgCudaOn, imgCudaAgain, imgCudaBalanced, imgCudaOff;
        std::vector<uint32_t> rgb10A, rgb10B;
        auto ctxCuda = makeNativeReferenceContext(1200000, 0); // 25.0s mid-morph formation
        ctxCuda.randomSeed = 305001u;
        BOO_CHECK(fxCuda.renderHeadlessCanvasForTest(ctxCuda, imgCudaOn, w, h, &err, &rgb10A));
        const auto mCuda = computeRgba8LumaMetrics(imgCudaOn);
        BOO_CHECK(mCuda.meanLuma > 0.01 && (mCuda.p99Luma - mCuda.p10Luma) > 0.05);

        fxCuda.resetTo(0.0);
        BOO_CHECK(fxCuda.renderHeadlessCanvasForTest(ctxCuda, imgCudaAgain, w, h, &err, &rgb10B));
        BOO_CHECK(rgb10A == rgb10B);

        if (!d3d.isWarp() && caps.cudaMatchesD3D12Luid) {
            HardwareResult cudaLive{};
            BOO_CHECK(fxCuda.queryLiveEngineStatus(EngineKind::CUDA, cudaLive));
            const auto cTel = fxCuda.livePillarTelemetry(1);
            BOO_CHECK(cudaLive.status == BackendStatus::Accelerated);
            BOO_CHECK(cTel.hardwareVerified);
            BOO_CHECK(cTel.workloadItems >= 100000u && cTel.workloadSteps == 16u);
            BOO_CHECK(cTel.workloadItems == 262144u);

            // Balanced preset in SoloCUDA still honors the >= 100k M3 hardware gate (102,400 particles)
            BOO_CHECK(fxCuda.syncPresetBeforeFrame(balancedSpec, true));
            BOO_CHECK(fxCuda.renderHeadlessCanvasForTest(ctxCuda, imgCudaBalanced, w, h, &err));
            BOO_CHECK(fxCuda.livePillarTelemetry(1).hardwareVerified);
            BOO_CHECK(fxCuda.livePillarTelemetry(1).workloadItems == 102400u);
            BOO_CHECK(fxCuda.syncPresetBeforeFrame(showcaseSpec, true));
            BOO_CHECK(fxCuda.renderHeadlessCanvasForTest(ctxCuda, imgCudaOn, w, h, &err));

            fxCuda.setDisableCuda(true);
            BOO_CHECK(fxCuda.renderHeadlessCanvasForTest(ctxCuda, imgCudaOff, w, h, &err));
            HardwareResult cudaSub{};
            BOO_CHECK(fxCuda.queryLiveEngineStatus(EngineKind::CUDA, cudaSub));
            BOO_CHECK(cudaSub.status == BackendStatus::Substituted);
            BOO_CHECK(!fxCuda.livePillarTelemetry(1).hardwareVerified);
            BOO_CHECK(fxCuda.livePillarTelemetry(1).workloadItems == 8192u);
            BOO_CHECK(imgCudaOff != imgCudaOn);
            fxCuda.setDisableCuda(false);
        }
        fxCuda.shutdown();
    }

    // 3. M4 Standalone SoloTensor (Neural Velvet mma.sync 64-layer SIREN field + HLSL procedural fallback)
    {
        M1TaipeiSliceEffect fxTensor(d3d, showcaseSpec, false, false, false, false, false, false, false, false, ShowVisualMode::SoloTensor);
        const auto st = fxTensor.probeAndInitialize();
        BOO_CHECK(st.status != BackendStatus::Faulted);

        std::vector<uint8_t> imgTensorOn, imgTensorAgain, imgTensorOff;
        std::vector<uint32_t> rgb10A, rgb10B;
        auto ctxTensor = makeNativeReferenceContext(960000, 0); // 20.0s
        ctxTensor.randomSeed = 303001u;
        BOO_CHECK(fxTensor.renderHeadlessCanvasForTest(ctxTensor, imgTensorOn, w, h, &err, &rgb10A));
        const auto mTensor = computeRgba8LumaMetrics(imgTensorOn);
        BOO_CHECK(mTensor.meanLuma > 0.01 && (mTensor.p99Luma - mTensor.p10Luma) > 0.05);

        fxTensor.resetTo(0.0);
        BOO_CHECK(fxTensor.renderHeadlessCanvasForTest(ctxTensor, imgTensorAgain, w, h, &err, &rgb10B));
        BOO_CHECK(rgb10A == rgb10B);

        if (!d3d.isWarp() && caps.cudaMatchesD3D12Luid && fxTensor.livePillarTelemetry(2).hardwareVerified) {
            HardwareResult tensorLive{};
            BOO_CHECK(fxTensor.queryLiveEngineStatus(EngineKind::Tensor, tensorLive));
            const auto tTel = fxTensor.livePillarTelemetry(2);
            BOO_CHECK(tensorLive.status == BackendStatus::Accelerated);
            BOO_CHECK(tTel.hardwareVerified && tTel.workloadSteps == 512u && tTel.throughput > 0.0f);

            fxTensor.setDisableTensor(true);
            BOO_CHECK(fxTensor.renderHeadlessCanvasForTest(ctxTensor, imgTensorOff, w, h, &err));
            HardwareResult tensorSub{};
            BOO_CHECK(fxTensor.queryLiveEngineStatus(EngineKind::Tensor, tensorSub));
            BOO_CHECK(tensorSub.status == BackendStatus::Substituted);
            BOO_CHECK(!fxTensor.livePillarTelemetry(2).hardwareVerified);
            BOO_CHECK(imgTensorOff != imgTensorOn);
            fxTensor.setDisableTensor(false);
        }
        fxTensor.shutdown();
    }

    d3d.waitForGpu();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    d3d.shutdown();
    return true;
}

void reportDirectorFrameDifference(const char* label, const std::vector<uint32_t>& live10,
                                   const std::vector<uint32_t>& script10, uint32_t w, uint32_t h) {
    std::array<uint32_t, 1024> channelDifference{};
    uint64_t totalDifference = 0;
    uint32_t changedPixels = 0;
    for (size_t i = 0; i < live10.size(); ++i) {
        changedPixels += (live10[i] & 0x3fffffffu) != (script10[i] & 0x3fffffffu);
        for (int shift : {0, 10, 20}) {
            const int delta = std::abs(int((live10[i] >> shift) & 1023u) - int((script10[i] >> shift) & 1023u));
            ++channelDifference[delta];
            totalDifference += delta;
        }
    }
    const double channels = 3.0 * live10.size();
    uint32_t below = 0;
    size_t p95 = 0;
    while (p95 + 1 < channelDifference.size() && below + channelDifference[p95] < channels * 0.95)
        below += channelDifference[p95++];
    std::cout << "  " << label << ", " << w << "x" << h << ": "
              << 100.0 * changedPixels / live10.size() << "% pixels changed, mean absolute RGB delta "
              << totalDifference / channels << "/1023, p95 " << p95 << "/1023.\n";
}

bool testMadameNNpuOpenVinoAndSoloNpuFailover() {
    using namespace boo;

    ProbeFlags pf{};
    const auto caps = probeSystemCapabilities(pf);
    const auto xmlPath = resolveResourcePath("assets/models/madame_n_director.xml");
    auto sanctuaryFrame = makeM1TestContext(1056000, 0);
    sanctuaryFrame.actIndex = 3;
    sanctuaryFrame.actProgress = 22.0f / 45.0f;
    ScriptedDirector sanctuaryDirector;
    sanctuaryDirector.reset(266203);
    const auto sanctuaryBaseline = sanctuaryDirector.evaluate(sanctuaryFrame, nullptr);
    std::optional<NeuralInferenceSample> sanctuaryInference;

    if (isOpenVinoRuntimeAvailable(true)) {
        // 1. Explicit OpenVINO NPU compile + actual inference verification (or CPU if host has no NPU)
        {
            OpenVinoDirector dir;
            OpenVinoDirectorConfig cfg{};
            cfg.modelXmlPath = xmlPath;
            cfg.allowNpu = true;
            cfg.allowCpuFallback = true;
            const HardwareResult res = dir.initialize(cfg);
            BOO_CHECK(dir.isReady());
            BOO_CHECK(res.successfullyExecutedTestWork);

            if (!caps.npuDevices.empty()) {
                BOO_CHECK(dir.isUsingHardwareNpu());
                BOO_CHECK(dir.activeDeviceName() == "NPU");
                BOO_CHECK(res.status == BackendStatus::Accelerated);
                BOO_CHECK(res.health == HealthState::ActiveAccelerated);
                BOO_CHECK(res.diagnosticCode == "NPU_OPENVINO_LIVE_VERIFIED");
            }

            // Evaluate Act 1 Suspense vs Act 3 Comic/Lounge feature vectors
            DirectorFeatureVector fvSuspense{};
            fvSuspense.values = {0.60f, 0.50f, 0.55f, 0.45f, 0.40f, 0.30f, 1.0f / 6.0f, 1.0f, 0.0f, 0.0f, 0.30f, 0.32f, 0.78f, 0.42f, 0.15f, 0.25f};
            DirectorFeatureVector fvComic{};
            fvComic.values = {0.50f, 0.50f, 0.45f, 0.55f, 0.35f, 0.78f, 3.0f / 6.0f, 0.0f, 1.0f, 0.0f, 0.48f, 0.68f, 0.18f, 0.32f, 0.12f, 0.75f};

            NeuralInferenceSample sSusp{}, sComic{};
            BOO_CHECK(dir.infer(fvSuspense, sSusp, false));
            BOO_CHECK(dir.infer(fvComic, sComic, false));
            BOO_CHECK(sSusp.latencyMs > 0.0f && sComic.latencyMs > 0.0f);
            BOO_CHECK(sSusp.category == DirectorCategory::Suspense);
            BOO_CHECK(sComic.category == DirectorCategory::Comic);
            for (size_t i = 0; i < 5u; ++i) {
                BOO_CHECK(sSusp.controls[i] >= 0.0f && sSusp.controls[i] <= 1.0f);
                BOO_CHECK(sComic.controls[i] >= 0.0f && sComic.controls[i] <= 1.0f);
            }
            sanctuaryInference.emplace();
            BOO_CHECK(dir.infer(sanctuaryDirector.buildFeatures(sanctuaryFrame, nullptr), *sanctuaryInference));
            dir.shutdown();
        }

        // 2. Explicit OpenVINO CPU substitute verification (never labeled Accelerated!)
        {
            OpenVinoDirector cpuDir;
            OpenVinoDirectorConfig cpuCfg{};
            cpuCfg.modelXmlPath = xmlPath;
            cpuCfg.allowNpu = false;
            cpuCfg.allowCpuFallback = true;
            const HardwareResult cpuRes = cpuDir.initialize(cpuCfg);
            BOO_CHECK(cpuDir.isReady());
            BOO_CHECK(!cpuDir.isUsingHardwareNpu());
            BOO_CHECK(cpuDir.activeDeviceName() == "CPU");
            BOO_CHECK(cpuRes.status == BackendStatus::Substituted);
            BOO_CHECK(cpuRes.health == HealthState::ActiveSubstitute);
            BOO_CHECK(cpuRes.successfullyExecutedTestWork);

            DirectorFeatureVector fv{};
            fv.values = {0.75f, 0.80f, 0.65f, 0.60f, 0.55f, 0.82f, 4.0f / 6.0f, 0.0f, 0.0f, 0.0f, 0.78f, 0.82f, 0.42f, 0.68f, 0.25f, 0.40f};
            NeuralInferenceSample sCpu{};
            BOO_CHECK(cpuDir.infer(fv, sCpu, false));
            BOO_CHECK(sCpu.category == DirectorCategory::Bloom);
            cpuDir.shutdown();
        }

        // 3. Non-stalling async worker startup + live 5 Hz inference + mid-playback timeout failover
        {
            AsyncDirectorController asyncCtrl;
            const auto tStart0 = std::chrono::steady_clock::now();
            asyncCtrl.start(266203, xmlPath, false, false, true, false);
            const double startMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tStart0).count();
            // Worker thread launch must never stall caller
            BOO_CHECK(startMs < 80.0);

            BOO_CHECK(asyncCtrl.waitForInitialCompile(8000));
            // Allow first 5 Hz inference tick to complete
            for (int retry = 0; retry < 30 && asyncCtrl.telemetry().completedInferences == 0; ++retry) {
                Sleep(25);
            }
            const auto liveTel = asyncCtrl.telemetry();
            BOO_CHECK(liveTel.completedInferences >= 1);
            BOO_CHECK(liveTel.completedRateHz >= 1.0f && liveTel.completedRateHz <= 5.0f);
            BOO_CHECK(!asyncCtrl.retryIfDegraded());
            if (!caps.npuDevices.empty()) {
                BOO_CHECK(liveTel.hardwareResult.status == BackendStatus::Accelerated);
            }

            FrameContext fctx{};
            fctx.samplePosition = 960000;
            fctx.sampleRate = 48000;
            fctx.filmTimeSeconds = 20.0;
            fctx.dtSeconds = 1.0 / 60.0;
            fctx.actIndex = 3;
            fctx.actProgress = 0.72f;
            fctx.audioEnvelope = 0.52f;

            DirectorMoodState baseline{};
            const DirectorMoodState neuralMood = asyncCtrl.evaluateFrame(fctx, nullptr, &baseline);
            BOO_CHECK(!neuralMood.isScriptedFallback);
            ScriptedDirector reference;
            reference.reset(266203);
            const auto expected = reference.evaluate(fctx, nullptr);
            BOO_CHECK(baseline.isScriptedFallback && baseline.category == expected.category);
            BOO_CHECK(baseline.irisAngle == expected.irisAngle && baseline.spectralShift == expected.spectralShift);

            // A seek must draw the current scripted baseline before fresh model output arrives.
            fctx.samplePosition = 8064000;
            fctx.filmTimeSeconds = 168.0;
            fctx.seeking = true;
            const auto atSeek = asyncCtrl.evaluateFrame(fctx, nullptr, &baseline);
            BOO_CHECK(atSeek.isScriptedFallback && atSeek.irisAngle == baseline.irisAngle);
            fctx.seeking = false;
            bool resumedNeural = false;
            for (int retry = 0; retry < 120; ++retry) {
                if (!asyncCtrl.evaluateFrame(fctx, nullptr).isScriptedFallback) {
                    resumedNeural = true;
                    break;
                }
                Sleep(25);
            }
            BOO_CHECK(resumedNeural);
            BOO_CHECK(asyncCtrl.telemetry().completedInferences > liveTel.completedInferences);

            // Inject mid-film NPU inference timeout -> verify smooth continuation and Degraded failover
            asyncCtrl.setInjectTimeout(true);
            for (int step = 0; step < 25; ++step) {
                fctx.samplePosition += 800;
                fctx.filmTimeSeconds = static_cast<double>(fctx.samplePosition) / 48000.0;
                const auto m = asyncCtrl.evaluateFrame(fctx, nullptr);
                BOO_CHECK(std::isfinite(m.irisAngle) && m.irisAngle >= 0.0f && m.irisAngle <= 1.0f);
                if (asyncCtrl.telemetry().timeoutCount > 0) break;
                Sleep(20);
            }
            const auto failTel = asyncCtrl.telemetry();
            BOO_CHECK(failTel.timeoutCount >= 1);
            BOO_CHECK(failTel.hardwareResult.status == BackendStatus::Degraded);
            BOO_CHECK(failTel.hardwareResult.diagnosticCode == "NPU_INFERENCE_TIMEOUT_FAILOVER");
            BOO_CHECK(failTel.hardwareResult.humanReadableReason.find("Injected inference timeout") != std::string::npos);
            BOO_CHECK(failTel.completedInferences == 0 && failTel.completedRateHz == 0.0f && failTel.lastInferMs == 0.0f);
            const DirectorMoodState fallbackMood = asyncCtrl.evaluateFrame(fctx, nullptr);
            BOO_CHECK(fallbackMood.isScriptedFallback);
            BOO_CHECK(!asyncCtrl.retryIfDegraded());
            asyncCtrl.setInjectTimeout(false);
            BOO_CHECK(!asyncCtrl.retryIfDegraded());

            // A real bad-input failure must report its cause and leave the watchdog count alone.
            // A rapid disable/enable pair starts a new route even between two worker ticks.
            asyncCtrl.setDisableNpu(true);
            BOO_CHECK(!asyncCtrl.retryIfDegraded());
            asyncCtrl.setDisableNpu(false);
            for (int retry = 0; retry < 160 && asyncCtrl.telemetry().completedInferences == 0; ++retry) Sleep(25);
            BOO_CHECK(asyncCtrl.telemetry().completedInferences > 0);
            auto invalid = fctx;
            invalid.audioEnvelope = std::nanf("");
            asyncCtrl.evaluateFrame(invalid, nullptr);
            for (int retry = 0; retry < 80 && asyncCtrl.telemetry().hardwareResult.status != BackendStatus::Degraded; ++retry) Sleep(25);
            const auto badInput = asyncCtrl.telemetry();
            BOO_CHECK(badInput.hardwareResult.diagnosticCode == "NPU_INFERENCE_ERROR_FAILOVER");
            BOO_CHECK(badInput.hardwareResult.humanReadableReason.find("non-finite feature") != std::string::npos);
            BOO_CHECK(badInput.timeoutCount == failTel.timeoutCount);
            BOO_CHECK(badInput.completedRateHz == 0.0f && badInput.completedInferences == 0);
            asyncCtrl.reset(266203);
            asyncCtrl.evaluateFrame(fctx, nullptr);
            const bool hardwareNpu = liveTel.hardwareResult.status == BackendStatus::Accelerated;
            BOO_CHECK(asyncCtrl.retryIfDegraded() == hardwareNpu);
            BOO_CHECK(!asyncCtrl.retryIfDegraded());
            if (hardwareNpu) {
                for (int retry = 0; retry < 160 && asyncCtrl.telemetry().completedInferences == 0; ++retry) Sleep(25);
                const auto recovered = asyncCtrl.telemetry();
                BOO_CHECK(recovered.hardwareResult.status == BackendStatus::Accelerated);
                BOO_CHECK(recovered.completedInferences > 0 && recovered.completedRateHz > 0.0f);
            }
            asyncCtrl.shutdown();
        }
    }

    // 4. 3D SoloNPU visual stage coherence & director modulation sensitivity
    {
        D3D12Context d3d;
        std::string err;
        BOO_CHECK(d3d.initialize(nullptr, 960, 540, false, true, &err));
        auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
        QualityPresetSpec spec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
        spec.renderWidth = 960;
        spec.renderHeight = 540;

        M1TaipeiSliceEffect fxNpu(d3d, spec, false, false, false, false, false, false, false, false, ShowVisualMode::SoloNPU);
        BOO_CHECK(fxNpu.probeAndInitialize().status != BackendStatus::Faulted);

        uint32_t w = 0, h = 0;
        std::vector<uint8_t> imgCalm, imgBloom;
        auto ctxA = makeM1TestContext(1056000, 0); // 22.0s on Madame N 3D stage
        ctxA.randomSeed = 304001u;
        ctxA.moodIrisAngle = 0.22f;
        ctxA.moodPaletteAccent = 0.18f;
        BOO_CHECK(fxNpu.renderHeadlessCanvasForTest(ctxA, imgCalm, w, h, &err));
        const auto mCalm = computeRgba8LumaMetrics(imgCalm);
        BOO_CHECK(mCalm.meanLuma > 0.01 && (mCalm.p99Luma - mCalm.p10Luma) > 0.05);

        auto ctxB = ctxA;
        ctxB.moodIrisAngle = 0.88f;
        ctxB.moodPaletteAccent = 0.85f;
        BOO_CHECK(fxNpu.renderHeadlessCanvasForTest(ctxB, imgBloom, w, h, &err));
        BOO_CHECK(imgCalm != imgBloom);

        // Compare two complete sanctuary renders at the same sample. With matching lighting
        // the two 10-bit canvases must agree, even when the scripted iris would move the camera.
        fxNpu.setAbCompareEngine(AbCompareEngine::NPU);
        ctxB.scriptedDirectorComparison = {
            ctxB.moodBloomStrength, 0.1f, ctxB.moodIntensity, ctxB.moodPaletteAccent, ctxB.moodCutAccent};
        std::vector<uint32_t> live10, script10;
        BOO_CHECK(fxNpu.renderHeadlessAbCompareForTest(ctxB, imgBloom, imgCalm, w, h, &err, &live10, &script10));
        BOO_CHECK(live10 == script10);
        const auto sameFrame = live10;
        ctxB.scriptedDirectorComparison.lightTension = 0.02f;
        ctxB.scriptedDirectorComparison.bloomStrength = 0.04f;
        ctxB.scriptedDirectorComparison.spectralShift = 0.05f;
        BOO_CHECK(fxNpu.renderHeadlessAbCompareForTest(ctxB, imgBloom, imgCalm, w, h, &err, &live10, &script10));
        BOO_CHECK(live10 == sameFrame);
        BOO_CHECK(live10 != script10);

        if (sanctuaryInference) {
            const auto& controls = sanctuaryInference->controls;
            sanctuaryFrame.moodBloomStrength = controls[0];
            sanctuaryFrame.moodIrisAngle = controls[1];
            sanctuaryFrame.moodIntensity = controls[2];
            sanctuaryFrame.moodPaletteAccent = controls[3];
            sanctuaryFrame.moodCutAccent = controls[4];
            sanctuaryFrame.scriptedDirectorComparison = {
                sanctuaryBaseline.bloomStrength, sanctuaryBaseline.irisAngle, sanctuaryBaseline.lightTension,
                sanctuaryBaseline.spectralShift, sanctuaryBaseline.cutAccent};
            BOO_CHECK(fxNpu.renderHeadlessAbCompareForTest(sanctuaryFrame, imgBloom, imgCalm, w, h, &err, &live10, &script10));
            BOO_CHECK(live10 != script10);
            reportDirectorFrameDifference("Sanctuary at 22 s", live10, script10, w, h);
        }

        fxNpu.shutdown();
        d3d.waitForGpu();
        BOO_CHECK(d3d.debugErrorCount() == 0);
        d3d.shutdown();
    }

    return true;
}

bool testFullFilm360sRevealsEditionsAndM7CombinedFailover() {
    using namespace boo;

    D3D12Context d3d;
    std::string err;
    BOO_CHECK(d3d.initialize(nullptr, 960, 540, false, true, &err));
    freezeDriverProfilingForTest(d3d);

    auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    QualityPresetSpec spec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    spec.renderWidth = 960;
    spec.renderHeight = 540;

    M1TaipeiSliceEffect fxFilm(d3d, spec, false, false, false, false, false, false, false, false, ShowVisualMode::FullFilm360s);
    const HardwareResult initRes = fxFilm.probeAndInitialize();
    BOO_CHECK(initRes.status != BackendStatus::Faulted);

    // 1. Verify all 7 acts + exact narrative reveals (36.0s, 175.0s, 342.0s) + closing hold (355.0s)
    //    render valid, non-black scenes and deterministic RGB10A2 CanvasClean output.
    struct FilmCheckpoint {
        int64_t sample;
        uint32_t actIdx;
        uint64_t seed;
    };
    static constexpr FilmCheckpoint kPoints[] = {
        {  1728000LL, 0u, 301001ULL }, // Act 0: 00:36.000 opening_scale_reveal
        {  3360000LL, 1u, 302001ULL }, // Act 1: 01:10.000 mirror_intelligence (DXR)
        {  5760000LL, 2u, 303001ULL }, // Act 2: 02:00.000 neural_velvet (Tensor)
        {  8400000LL, 3u, 304001ULL }, // Act 3: 02:55.000 midpoint_ecology_reveal (NPU)
        { 10560000LL, 4u, 305001ULL }, // Act 4: 03:40.000 particle_mutiny (CUDA)
        { 12240000LL, 5u, 306001ULL }, // Act 5: 04:15.000 signal_hijack (Codec)
        { 14400000LL, 6u, 307001ULL }, // Act 6: 05:00.000 silicon_rebellion (Combined Finale)
        { 16416000LL, 6u, 307001ULL }, // Act 6: 05:42.000 final_everyday_care_reveal
        { 17040000LL, 6u, 307001ULL }, // Act 6: 05:55.000 non-medical end card hold
    };

    uint32_t w = 0, h = 0;
    for (const auto& cp : kPoints) {
        std::vector<uint8_t> rgbaA;
        std::vector<uint32_t> rgb10A;
        auto ctx = makeNativeReferenceContext(cp.sample, cp.actIdx);
        ctx.randomSeed = cp.seed;
        ctx.edition = Edition::Family;
        BOO_CHECK(fxFilm.renderHeadlessCanvasForTest(ctx, rgbaA, w, h, &err, &rgb10A));
        BOO_CHECK(w == 960 && h == 540);
        const auto m = computeRgba8LumaMetrics(rgbaA);
        BOO_CHECK(m.meanLuma > 0.008 && (m.p99Luma - m.p10Luma) > 0.04);

        // Verify exact seek determinism on key narrative reveals
        if (cp.sample == 1728000LL || cp.sample == 8400000LL || cp.sample == 16416000LL) {
            std::vector<uint8_t> rgbaB;
            std::vector<uint32_t> rgb10B;
            BOO_CHECK(fxFilm.renderHeadlessCanvasForTest(ctx, rgbaB, w, h, &err, &rgb10B));
            BOO_CHECK(rgb10A == rgb10B);
        }
    }

    // 2. Verify Family vs Midnight visual differentiation + Pass-4 caption & end-card clean-feed isolation
    {
        std::vector<uint8_t> cleanFam, cleanMid;
        auto ctxFam = makeNativeReferenceContext(1728000LL, 0u);
        ctxFam.randomSeed = 301001ULL;
        ctxFam.edition = Edition::Family;
        auto ctxMid = ctxFam;
        ctxMid.edition = Edition::Midnight;
        BOO_CHECK(fxFilm.renderHeadlessCanvasForTest(ctxFam, cleanFam, w, h, &err));
        BOO_CHECK(fxFilm.renderHeadlessCanvasForTest(ctxMid, cleanMid, w, h, &err));
        BOO_CHECK(cleanFam != cleanMid);

        // Captions ON vs OFF on Pass 4 swapchain present must alter presented lower-third pixels
        // while keeping CanvasClean 100% bit-identical!
        HudTelemetrySnapshot hud{};
        hud.filmTimeSeconds = 36.0;
        hud.durationSeconds = 360.0;
        hud.fps = 60.0f;
        hud.canvasWidth = 960;
        hud.canvasHeight = 540;
        hud.swapchainBits = 8;

        auto ctxNoCap = ctxFam;
        ctxNoCap.showOverlay = false;
        ctxNoCap.hudMode = 3u;
        ctxNoCap.showCaptions = false;
        std::vector<uint8_t> presNoCap, cleanFromNoCap;
        std::vector<uint32_t> clean10NoCap;
        BOO_CHECK(fxFilm.renderHeadlessPresentedFrameForTest(ctxNoCap, hud, 960, 540, presNoCap, &err));
        BOO_CHECK(fxFilm.readLastRenderedCanvasForTest(cleanFromNoCap, w, h, &err, &clean10NoCap));

        auto ctxWithCap = ctxNoCap;
        ctxWithCap.showCaptions = true;
        std::vector<uint8_t> presWithCap, cleanFromWithCap;
        std::vector<uint32_t> clean10WithCap;
        BOO_CHECK(fxFilm.renderHeadlessPresentedFrameForTest(ctxWithCap, hud, 960, 540, presWithCap, &err));
        BOO_CHECK(fxFilm.readLastRenderedCanvasForTest(cleanFromWithCap, w, h, &err, &clean10WithCap));

        BOO_CHECK(presWithCap != presNoCap);
        BOO_CHECK(clean10WithCap == clean10NoCap);

        // Closing non-medical end card at 355.0s appears on Pass 4 present while CanvasClean stays clean
        auto ctxEnd = makeM1TestContext(17040000LL, 6u);
        ctxEnd.randomSeed = 307001ULL;
        ctxEnd.showOverlay = false;
        ctxEnd.hudMode = 3u;
        ctxEnd.showCaptions = false;
        std::vector<uint8_t> presEnd, cleanAtEnd, directCleanEnd;
        BOO_CHECK(fxFilm.renderHeadlessPresentedFrameForTest(ctxEnd, hud, 960, 540, presEnd, &err));
        BOO_CHECK(fxFilm.readLastRenderedCanvasForTest(cleanAtEnd, w, h, &err));
        BOO_CHECK(presEnd != cleanAtEnd);
    }

    // The scripted Shenzhen broadcast runs the codec but keeps Clean Cinema native.
    // Only the telemetry corner gets a monitor. V still owns a full-screen comparison.
    {
        auto ctx = makeNativeReferenceContext(12480000, 5u);
        ctx.actIndex = 5u;
        ctx.showOverlay = false;
        ctx.showCaptions = false;
        ctx.hudMode = 3u;
        HudTelemetrySnapshot hud{};
        hud.durationSeconds = 360;
        fxFilm.setDisableCodec(true); // same deterministic shader substitute on any adapter
        std::vector<uint8_t> cinema, clean, inset, manual;
        BOO_CHECK(fxFilm.renderHeadlessPresentedFrameForTest(ctx, hud, 960, 540, cinema, &err));
        BOO_CHECK(fxFilm.readLastRenderedCanvasForTest(clean, w, h, &err));
        // Pass 4 applies triangular display dither at eight bits; the native image
        // can move by one output code per component (allow two for sampler precision).
        for (size_t i = 0; i < clean.size(); ++i) {
            BOO_CHECK(std::abs(int(cinema[i]) - int(clean[i])) <= 2);
        }
        ctx.showOverlay = true;
        ctx.hudMode = 1u;
        BOO_CHECK(fxFilm.renderHeadlessPresentedFrameForTest(ctx, hud, 960, 540, inset, &err));
        const auto row = 70u * 960u * 4u;
        BOO_CHECK(std::equal(cinema.begin()+row+400u*4u, cinema.begin()+row+700u*4u, inset.begin()+row+400u*4u));
        BOO_CHECK(!std::equal(cinema.begin()+row+800u*4u, cinema.begin()+row+900u*4u, inset.begin()+row+800u*4u));
        ctx.showOverlay = false;
        ctx.hudMode = 3u;
        fxFilm.setCodecSignalHijackActive(true);
        BOO_CHECK(fxFilm.renderHeadlessPresentedFrameForTest(ctx, hud, 960, 540, manual, &err));
        BOO_CHECK(computeMeanAbsDiffRgb01(cinema, manual) > 0.003);
        fxFilm.setCodecSignalHijackActive(false);
        fxFilm.setDisableCodec(false);
    }

    // 3. M7 Combined Finale (Act 6 @ 300.0s = 14,400,000) multi-engine execution & mid-playback failover
    {
        AsyncDirectorController asyncDir;
        asyncDir.start(266203, resolveResourcePath("assets/models/madame_n_director.xml"), false, false, true, false);
        (void)asyncDir.waitForInitialCompile(5000);

        auto ctxComb = makeM1TestContext(14400000LL, 6u);
        ctxComb.randomSeed = 307001ULL;
        const auto m0 = asyncDir.evaluateFrame(ctxComb, nullptr);
        ctxComb.moodIrisAngle = m0.irisAngle;
        ctxComb.moodIntensity = m0.lightTension;
        ctxComb.moodPaletteAccent = m0.spectralShift;

        std::vector<uint8_t> imgAllHw, imgNoRt, imgNoCuda, imgNoTensor, imgAllSub;
        BOO_CHECK(fxFilm.renderHeadlessCanvasForTest(ctxComb, imgAllHw, w, h, &err));
        BOO_CHECK(computeRgba8LumaMetrics(imgAllHw).meanLuma > 0.01);

        // Single-accelerator failovers mid-playback
        fxFilm.setDisableRt(true);
        BOO_CHECK(fxFilm.renderHeadlessCanvasForTest(ctxComb, imgNoRt, w, h, &err));
        HardwareResult rtSt{};
        BOO_CHECK(fxFilm.queryLiveEngineStatus(EngineKind::RT, rtSt) && rtSt.status == BackendStatus::Substituted);
        BOO_CHECK(computeRgba8LumaMetrics(imgNoRt).meanLuma > 0.01);

        fxFilm.setDisableCuda(true);
        BOO_CHECK(fxFilm.renderHeadlessCanvasForTest(ctxComb, imgNoCuda, w, h, &err));
        HardwareResult cudaSt{};
        BOO_CHECK(fxFilm.queryLiveEngineStatus(EngineKind::CUDA, cudaSt) && cudaSt.status == BackendStatus::Substituted);

        fxFilm.setDisableTensor(true);
        asyncDir.setDisableNpu(true);
        const auto mSub = asyncDir.evaluateFrame(ctxComb, nullptr);
        ctxComb.moodIrisAngle = mSub.irisAngle;
        BOO_CHECK(fxFilm.renderHeadlessCanvasForTest(ctxComb, imgAllSub, w, h, &err));
        HardwareResult tenSt{};
        BOO_CHECK(fxFilm.queryLiveEngineStatus(EngineKind::Tensor, tenSt) && tenSt.status == BackendStatus::Substituted);
        BOO_CHECK(computeRgba8LumaMetrics(imgAllSub).meanLuma > 0.01);

        // Restore all accelerators mid-playback
        fxFilm.setDisableRt(false);
        fxFilm.setDisableCuda(false);
        fxFilm.setDisableTensor(false);
        std::vector<uint8_t> imgRestored;
        BOO_CHECK(fxFilm.renderHeadlessCanvasForTest(ctxComb, imgRestored, w, h, &err));
        BOO_CHECK(computeRgba8LumaMetrics(imgRestored).meanLuma > 0.01);

        asyncDir.shutdown();
    }

    fxFilm.shutdown();
    d3d.waitForGpu();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    d3d.shutdown();
    return true;
}

bool testSpout2CleanSenderAndLiveD3d11Receiver() {
    using namespace boo;
    using Microsoft::WRL::ComPtr;

    // 1. WARP software adapter must cleanly report unavailable without crashing or stalling
    {
        D3D12Context warpD3d;
        std::string wErr;
        if (warpD3d.initialize(nullptr, 640, 360, true, false, &wErr)) {
            SpoutSender warpSpout;
            std::string wReason;
            BOO_CHECK(!warpSpout.initialize(warpD3d, "BooSahSah_WarpSkip", 640, 360, &wReason));
            BOO_CHECK(!warpSpout.isActive());
            warpD3d.shutdown();
        }
    }

    // 2. Live hardware D3D12 -> Spout2 shared texture -> independent D3D11 Spout2 receiver verification
    D3D12Context d3d;
    std::string err;
    BOO_CHECK(d3d.initialize(nullptr, 960, 540, false, true, &err));
    freezeDriverProfilingForTest(d3d);
    if (d3d.isWarp()) {
        d3d.shutdown();
        return true;
    }

    auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    QualityPresetSpec spec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    spec.renderWidth = 960;
    spec.renderHeight = 540;

    M1TaipeiSliceEffect fx(d3d, spec, false, false, false, false, false, false, false, false, ShowVisualMode::FullFilm360s);
    BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);

    const std::string testName = "BooSahSah_Clean_Test";
    SpoutSender spout;
    std::string spoutReason;
    BOO_CHECK(spout.initialize(d3d, testName, 960, 540, &spoutReason));
    BOO_CHECK(spout.isActive());

    SpoutSharedTextureInfo info{};
    bool inList = false;
    BOO_CHECK(querySpoutSenderInfo(testName, info, &inList));
    BOO_CHECK(inList);
    BOO_CHECK(info.shareHandle != 0u);
    BOO_CHECK(info.width == 960u && info.height == 540u);
    BOO_CHECK(info.format == 87u); // DXGI_FORMAT_B8G8R8A8_UNORM

    // Publish frame 1 with NO receiver attached (proves receiver-absent non-blocking resilience)
    HudTelemetrySnapshot hud{};
    hud.filmTimeSeconds = 36.0;
    hud.durationSeconds = 360.0;
    hud.fps = 60.0f;
    hud.canvasWidth = 960;
    hud.canvasHeight = 540;
    hud.swapchainBits = 8;

    auto ctxHudOn = makeNativeReferenceContext(1728000LL, 0u); // 36.0s opening_scale_reveal (caption active!)
    ctxHudOn.randomSeed = 301001ULL;
    ctxHudOn.showOverlay = true;
    ctxHudOn.hudMode = 0u;
    ctxHudOn.showCaptions = true;

    std::vector<uint8_t> presHudOn;
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctxHudOn, hud, 960, 540, presHudOn, &err));
    BOO_CHECK(d3d.executeSingleCommandListAndWait([&](void* cmdRaw) {
        spout.recordCleanPublish(d3d, cmdRaw, fx.canvasCleanResourceRaw(), fx.canvasWidth(), fx.canvasHeight());
    }, &err));
    BOO_CHECK(spout.completePublish());
    BOO_CHECK(spout.publishedFrameCount() == 1ULL);

    // Open independent D3D11 receiver device on matching LUID & read shared Spout2 texture
    HMODULE hD3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    BOO_CHECK(hD3d11 != nullptr);
    auto pfnCreate11 = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(hD3d11, "D3D11CreateDevice"));
    BOO_CHECK(pfnCreate11 != nullptr);

    ComPtr<IDXGIFactory4> fac4;
    BOO_CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac4))) && fac4);
    LUID luid{};
    luid.LowPart = static_cast<DWORD>(d3d.adapterLuidLow());
    luid.HighPart = d3d.adapterLuidHigh();
    ComPtr<IDXGIAdapter1> rxAdapter;
    BOO_CHECK(SUCCEEDED(fac4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&rxAdapter))) && rxAdapter);

    ComPtr<ID3D11Device> rxDev;
    ComPtr<ID3D11DeviceContext> rxCtx;
    const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL rxFl = D3D_FEATURE_LEVEL_11_0;
    BOO_CHECK(SUCCEEDED(pfnCreate11(
        rxAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        fls, 2, D3D11_SDK_VERSION, &rxDev, &rxFl, &rxCtx)) && rxDev && rxCtx);

    const HANDLE rxHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(info.shareHandle));
    ComPtr<ID3D11Texture2D> rxSharedTex;
    BOO_CHECK(SUCCEEDED(rxDev->OpenSharedResource(rxHandle, IID_PPV_ARGS(&rxSharedTex))) && rxSharedTex);

    D3D11_TEXTURE2D_DESC stDesc{};
    rxSharedTex->GetDesc(&stDesc);
    BOO_CHECK(stDesc.Width == 960u && stDesc.Height == 540u && stDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    stDesc.Usage = D3D11_USAGE_STAGING;
    stDesc.BindFlags = 0;
    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> rxStaging;
    BOO_CHECK(SUCCEEDED(rxDev->CreateTexture2D(&stDesc, nullptr, &rxStaging)) && rxStaging);

    auto readReceiverRgba = [&](std::vector<uint8_t>& outRgba) -> bool {
        rxCtx->CopyResource(rxStaging.Get(), rxSharedTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(rxCtx->Map(rxStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
            return false;
        }
        outRgba.resize(960u * 540u * 4u);
        for (uint32_t y = 0; y < 540u; ++y) {
            const auto* srcRow = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
            uint8_t* dstRow = outRgba.data() + static_cast<size_t>(y) * 960u * 4u;
            for (uint32_t x = 0; x < 960u; ++x) {
                dstRow[x * 4u + 0u] = srcRow[x * 4u + 2u]; // R from BGRA
                dstRow[x * 4u + 1u] = srcRow[x * 4u + 1u]; // G
                dstRow[x * 4u + 2u] = srcRow[x * 4u + 0u]; // B
                dstRow[x * 4u + 3u] = srcRow[x * 4u + 3u]; // A
            }
        }
        rxCtx->Unmap(rxStaging.Get(), 0);
        return true;
    };

    std::vector<uint8_t> rxFromHudOn;
    BOO_CHECK(readReceiverRgba(rxFromHudOn));
    const auto mRx1 = computeRgba8LumaMetrics(rxFromHudOn);
    BOO_CHECK(mRx1.meanLuma > 0.01 && (mRx1.p99Luma - mRx1.p10Luma) > 0.04);

    // Re-render exact same timestamp (36.0s) with HUD & captions completely OFF:
    // Presented frame differs (no HUD bar, no caption), while Spout2 received frame MUST BE BIT-IDENTICAL!
    auto ctxHudOff = ctxHudOn;
    ctxHudOff.showOverlay = false;
    ctxHudOff.hudMode = 3u;
    ctxHudOff.showCaptions = false;
    std::vector<uint8_t> presHudOff, rxFromHudOff;
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctxHudOff, hud, 960, 540, presHudOff, &err));
    BOO_CHECK(presHudOn != presHudOff);
    BOO_CHECK(d3d.executeSingleCommandListAndWait([&](void* cmdRaw) {
        spout.recordCleanPublish(d3d, cmdRaw, fx.canvasCleanResourceRaw(), fx.canvasWidth(), fx.canvasHeight());
    }, &err));
    BOO_CHECK(spout.completePublish());
    BOO_CHECK(readReceiverRgba(rxFromHudOff));
    BOO_CHECK(rxFromHudOn == rxFromHudOff);

    // Advance timeline to Act 3 (175.0s = 8,400,000) -> verify live motion across Spout2 receiver frames
    auto ctxAct3 = makeM1TestContext(8400000LL, 3u);
    ctxAct3.randomSeed = 304001ULL;
    std::vector<uint8_t> dummyClean, rxFromAct3;
    uint32_t cw = 0, ch = 0;
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctxAct3, dummyClean, cw, ch, &err));
    BOO_CHECK(d3d.executeSingleCommandListAndWait([&](void* cmdRaw) {
        spout.recordCleanPublish(d3d, cmdRaw, fx.canvasCleanResourceRaw(), fx.canvasWidth(), fx.canvasHeight());
    }, &err));
    // Force a real publication fence timeout without hanging/resetting hardware.
    // Its GPU queue wait is released from the CPU once receiver exclusion is checked.
    Microsoft::WRL::ComPtr<ID3D12Fence> publicationGate;
    BOO_CHECK(SUCCEEDED(static_cast<ID3D12Device*>(d3d.deviceRaw())->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&publicationGate))));
    HANDLE receiverMutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
        (testName + "_SpoutAccessMutex").c_str());
    BOO_CHECK(receiverMutex != nullptr);
    BOO_CHECK(SUCCEEDED(static_cast<ID3D12CommandQueue*>(d3d.commandQueueRaw())->Wait(publicationGate.Get(), 1)));
    const bool timedOut = !spout.completePublish();
    const uint64_t countAfterTimeout = spout.publishedFrameCount();
    DWORD receiverWait = WAIT_FAILED;
    std::thread receiverAttempt([&] {
        receiverWait = WaitForSingleObject(receiverMutex, 20);
        if (receiverWait == WAIT_OBJECT_0 || receiverWait == WAIT_ABANDONED) ReleaseMutex(receiverMutex);
    });
    receiverAttempt.join();
    const HRESULT released = publicationGate->Signal(1);
    CloseHandle(receiverMutex);
    BOO_CHECK(SUCCEEDED(released));
    BOO_CHECK(timedOut && countAfterTimeout == 2 && receiverWait == WAIT_TIMEOUT);
    BOO_CHECK(spout.completePublish());
    BOO_CHECK(readReceiverRgba(rxFromAct3));
    BOO_CHECK(rxFromAct3 != rxFromHudOn);
    BOO_CHECK(spout.publishedFrameCount() == 3ULL);

    rxStaging.Reset();
    rxSharedTex.Reset();
    rxCtx.Reset();
    rxDev.Reset();
    rxAdapter.Reset();
    fac4.Reset();
    FreeLibrary(hD3d11);

    // Verify clean SpoutSenderNames shared-memory unregistration on shutdown
    spout.shutdown();
    SpoutSharedTextureInfo afterShutdown{};
    bool stillInList = true;
    BOO_CHECK(!querySpoutSenderInfo(testName, afterShutdown, &stillInList));
    BOO_CHECK(!stillInList);

    fx.shutdown();
    d3d.waitForGpu();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    d3d.shutdown();
    return true;
}

bool generateSyntheticTestMp4(const std::filesystem::path& outMp4Path) {
    using Microsoft::WRL::ComPtr;
    using PFN_MFCreateSinkWriterFromURL = HRESULT(STDAPICALLTYPE*)(
        LPCWSTR pwszOutputURL,
        IMFByteStream* pByteStream,
        IMFAttributes* pAttributes,
        IMFSinkWriter** ppSinkWriter);

    HMODULE hMfRw = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hMfRw) return false;
    auto pfnCreateSink = reinterpret_cast<PFN_MFCreateSinkWriterFromURL>(
        GetProcAddress(hMfRw, "MFCreateSinkWriterFromURL"));
    if (!pfnCreateSink) {
        FreeLibrary(hMfRw);
        return false;
    }

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hrCo);
    bool ok = false;

    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        std::error_code ec;
        if (outMp4Path.has_parent_path()) {
            std::filesystem::create_directories(outMp4Path.parent_path(), ec);
        }
        std::filesystem::remove(outMp4Path, ec);

        constexpr uint32_t W = 320;
        constexpr uint32_t H = 240;
        constexpr uint32_t kFps = 30;
        constexpr LONGLONG kFrameDur100ns = 10000000LL / kFps;

        ComPtr<IMFSinkWriter> writer;
        if (SUCCEEDED(pfnCreateSink(outMp4Path.wstring().c_str(), nullptr, nullptr, &writer)) && writer) {
            ComPtr<IMFMediaType> outType;
            MFCreateMediaType(&outType);
            outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            outType->SetUINT32(MF_MT_AVG_BITRATE, 900000);
            outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, W, H);
            MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, kFps, 1);
            MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

            DWORD streamIdx = 0;
            if (SUCCEEDED(writer->AddStream(outType.Get(), &streamIdx))) {
                ComPtr<IMFMediaType> inType;
                MFCreateMediaType(&inType);
                inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, W, H);
                MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, kFps, 1);
                MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

                if (SUCCEEDED(writer->SetInputMediaType(streamIdx, inType.Get(), nullptr)) &&
                    SUCCEEDED(writer->BeginWriting())) {
                    bool allFramesOk = true;
                    for (uint32_t f = 0; f < 18u; ++f) {
                        ComPtr<IMFMediaBuffer> buf;
                        if (FAILED(MFCreateMemoryBuffer(W * H * 4u, &buf)) || !buf) {
                            allFramesOk = false;
                            break;
                        }
                        BYTE* dst = nullptr;
                        if (FAILED(buf->Lock(&dst, nullptr, nullptr)) || !dst) {
                            allFramesOk = false;
                            break;
                        }
                        for (uint32_t y = 0; y < H; ++y) {
                            for (uint32_t x = 0; x < W; ++x) {
                                const size_t o = (static_cast<size_t>(y) * W + x) * 4u;
                                const bool checker = (((x / 32u) + (y / 32u) + f) & 1u) != 0u;
                                dst[o + 0u] = checker ? 220u : 28u;  // B
                                dst[o + 1u] = checker ? 40u  : 235u; // G
                                dst[o + 2u] = checker ? 245u : 35u;  // R
                                dst[o + 3u] = 255u;
                            }
                        }
                        buf->Unlock();
                        buf->SetCurrentLength(W * H * 4u);

                        ComPtr<IMFSample> sample;
                        MFCreateSample(&sample);
                        sample->AddBuffer(buf.Get());
                        sample->SetSampleTime(static_cast<LONGLONG>(f) * kFrameDur100ns);
                        sample->SetSampleDuration(kFrameDur100ns);
                        if (FAILED(writer->WriteSample(streamIdx, sample.Get()))) {
                            allFramesOk = false;
                            break;
                        }
                    }
                    if (allFramesOk && SUCCEEDED(writer->Finalize())) {
                        ok = true;
                    }
                }
            }
        }
        MFShutdown();
    }

    if (needUninit) CoUninitialize();
    FreeLibrary(hMfRw);
    return ok;
}

bool testSignalHijackMediaIngestCameraPrivacyAndFallback() {
    using namespace boo;

    // 1. Default opt-in guarantee + corrupt MP4 + injected media fault
    MediaIngestSource media;
    BOO_CHECK(!media.isActive() && !media.isLiveCameraActive());
    BOO_CHECK(!media.openLocalFile(resolveResourcePath("tests/fixtures/corrupt_sample.mp4"), false));
    BOO_CHECK(!media.isActive() && !media.isLiveCameraActive());
    BOO_CHECK(!media.openLiveCamera(true));
    BOO_CHECK(!media.isActive() && !media.isLiveCameraActive());

    // 2. Synthesize real 320x240 H.264 MP4 clip & decode via MediaIngestSource into bounded 1280x720 NV12
    const auto clipPath = std::filesystem::path("build/m9_test_clip.mp4");
    std::vector<uint8_t> decodedNv12;
    uint64_t frameSeq = 0;
    if (generateSyntheticTestMp4(clipPath)) {
        BOO_CHECK(media.openLocalFile(clipPath, false));
        BOO_CHECK(media.isActive());
        BOO_CHECK(!media.isLiveCameraActive());
        BOO_CHECK(media.mode() == MediaSourceMode::LocalVideoFile);
        BOO_CHECK(media.decodedFrameCount() >= 1ULL);
        BOO_CHECK(media.copyLatestNv12Frame(decodedNv12, frameSeq));
        BOO_CHECK(decodedNv12.size() == MediaIngestSource::kNv12ByteSize);
        BOO_CHECK(frameSeq >= 1ULL);
        media.shutdown();
        BOO_CHECK(!media.isActive());
    } else {
        // Fallback synthetic NV12 buffer if host lacks H.264 SinkWriter (e.g. Windows Server Core / N)
        decodedNv12.resize(MediaIngestSource::kNv12ByteSize, 128u);
        for (size_t i = 0; i < 1280u * 720u; ++i) {
            decodedNv12[i] = ((i / 64u) & 1u) ? 215u : 35u;
        }
        frameSeq = 1ULL;
    }

    // 3. Verify Signal Hijack external media in both Live HW H.264 Round-Trip AND SHADER SIGNAL (--disable-nvenc),
    //    plus "LIVE CAMERA" privacy indicator on Pass 4 swapchain (even with HUD overlay hidden) while CanvasClean stays clean!
    D3D12Context d3d;
    std::string err;
    BOO_CHECK(d3d.initialize(nullptr, 960, 540, false, true, &err));
    freezeDriverProfilingForTest(d3d);

    auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    QualityPresetSpec spec = findQualityPresetSpec(presets, QualityPresetId::Balanced);
    spec.renderWidth = 960;
    spec.renderHeight = 540;

    M1TaipeiSliceEffect fx(
        d3d, spec,
        false, false, false, false, false,
        true,  // enableCodecHijack
        true,  // trilogy
        false,
        ShowVisualMode::FullFilm360s);
    const auto initRes = fx.probeAndInitialize();
    BOO_CHECK(initRes.status != BackendStatus::Faulted);

    auto ctx = makeNativeReferenceContext(10560000LL, 4u); // 220.0s Act 5 Signal Hijack
    ctx.randomSeed = 305001ULL;
    ctx.showOverlay = false; // Hide HUD so we isolate pure Signal Hijack & LIVE CAMERA privacy pill!
    ctx.hudMode = 3u;
    ctx.showCaptions = false;

    HudTelemetrySnapshot hud{};
    hud.filmTimeSeconds = 220.0;
    hud.durationSeconds = 360.0;
    hud.canvasWidth = 960;
    hud.canvasHeight = 540;
    hud.swapchainBits = 8;
    hud.codecSignalHijackActive = true;
    hud.liveCameraActive = false;

    std::vector<uint8_t> cleanBefore, cleanWithMedia, presProcOnly, presExtHw, presExtSub, presCamBadge;
    uint32_t cw = 0, ch = 0;
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx, cleanBefore, cw, ch, &err));
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx, hud, 960, 540, presProcOnly, &err));

    fx.setExternalNv12Frame(decodedNv12.data(), decodedNv12.size(), frameSeq);
    BOO_CHECK(fx.hasExternalMediaFrame());
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx, cleanWithMedia, cw, ch, &err));
    // Canonical Pass-3 CanvasClean (and Spout2 clean feed) must remain untouched by Signal Hijack external media!
    BOO_CHECK(cleanBefore == cleanWithMedia);

    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx, hud, 960, 540, presExtHw, &err));
    BOO_CHECK(presExtHw != presProcOnly);
    if (!d3d.isWarp() && fx.livePillarTelemetry(3).hardwareVerified) {
        HardwareResult codecHw{};
        BOO_CHECK(fx.queryLiveEngineStatus(EngineKind::Codec, codecHw));
        BOO_CHECK(codecHw.status == BackendStatus::Accelerated);
    }

    // Disable NVENC/HW Codec -> verify external media still renders via honest SHADER SIGNAL substitute (never Accelerated!)
    fx.setDisableCodec(true);
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx, hud, 960, 540, presExtSub, &err));
    HardwareResult codecSub{};
    BOO_CHECK(fx.queryLiveEngineStatus(EngineKind::Codec, codecSub));
    BOO_CHECK(codecSub.status == BackendStatus::Substituted);
    BOO_CHECK(!fx.livePillarTelemetry(3).hardwareVerified);
    BOO_CHECK(presExtSub != presProcOnly);

    // Enable liveCameraActive -> verify crimson "LIVE CAMERA" privacy pill renders at top-center even when HUD is OFF!
    hud.liveCameraActive = true;
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx, hud, 960, 540, presCamBadge, &err));
    BOO_CHECK(presCamBadge != presExtSub);

    uint32_t redPillDiffPixels = 0;
    for (uint32_t y = 10u; y < 36u; ++y) {
        for (uint32_t x = 410u; x < 550u; ++x) {
            const size_t idx = (static_cast<size_t>(y) * 960u + x) * 4u;
            if (presCamBadge[idx + 0u] != presExtSub[idx + 0u] ||
                presCamBadge[idx + 1u] != presExtSub[idx + 1u] ||
                presCamBadge[idx + 2u] != presExtSub[idx + 2u]) {
                ++redPillDiffPixels;
            }
        }
    }
    BOO_CHECK(redPillDiffPixels > 200u);

    // Clear external media:
    //  (a) Under stateless SHADER SIGNAL (disableCodec=true), recovers procedural substitute bit-identically on frame 1!
    fx.clearExternalNv12Frame();
    BOO_CHECK(!fx.hasExternalMediaFrame());
    hud.liveCameraActive = false;
    fx.resetTo(220.0);
    std::vector<uint8_t> presSubRecovered, presSubProcBaseline;
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx, hud, 960, 540, presSubRecovered, &err));
    fx.resetTo(220.0);
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx, hud, 960, 540, presSubProcBaseline, &err));
    BOO_CHECK(presSubRecovered == presSubProcBaseline);
    BOO_CHECK(presSubRecovered != presExtSub);

    //  (b) Re-enable 0.95 Mbps HW H.264 round-trip and pump past P-frame scene-cut convergence
    fx.setDisableCodec(false);
    std::vector<uint8_t> presRecovered;
    for (int settle = 0; settle < 8; ++settle) {
        fx.resetTo(220.0);
        BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx, hud, 960, 540, presRecovered, &err));
    }

    auto computeMeanAbsRgbDiff = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) -> double {
        if (a.size() != b.size() || a.empty()) return 999.0;
        uint64_t sum = 0;
        const size_t nPix = a.size() / 4u;
        for (size_t i = 0; i < nPix; ++i) {
            sum += static_cast<uint64_t>(std::abs(static_cast<int>(a[i * 4u + 0u]) - static_cast<int>(b[i * 4u + 0u])));
            sum += static_cast<uint64_t>(std::abs(static_cast<int>(a[i * 4u + 1u]) - static_cast<int>(b[i * 4u + 1u])));
            sum += static_cast<uint64_t>(std::abs(static_cast<int>(a[i * 4u + 2u]) - static_cast<int>(b[i * 4u + 2u])));
        }
        return static_cast<double>(sum) / static_cast<double>(nPix * 3u);
    };
    const double diffWithExtMedia = computeMeanAbsRgbDiff(presExtHw, presProcOnly);
    const double diffAfterClear   = computeMeanAbsRgbDiff(presRecovered, presProcOnly);
    BOO_CHECK(diffWithExtMedia > 10.0);
    BOO_CHECK(diffAfterClear < diffWithExtMedia * 0.35);

    std::error_code ec;
    std::filesystem::remove(clipPath, ec);

    fx.shutdown();
    d3d.waitForGpu();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    d3d.shutdown();
    return true;
}

bool testInteractiveExploreLightingAndNrdConvergence() {
    using namespace boo;

    D3D12Context d3d;
    std::string err;
    BOO_CHECK(d3d.initialize(nullptr, 960, 540, false, true, &err));

    CueSheet cue;
    ValidationReport validation;
    BOO_CHECK(loadCueSheetFromFile(resolveResourcePath("assets/cues/film_360s.json"), cue, validation) && validation.valid);

    const auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    QualityPresetSpec spec = findQualityPresetSpec(presets, QualityPresetId::Balanced);
    spec.renderWidth = 960;
    spec.renderHeight = 540;

    M1TaipeiSliceEffect fx(
        d3d, spec,
        false, false, false, false, false, false, true, false,
        ShowVisualMode::FullFilm360s);
    BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);
    fx.setDisableRt(false);

    const int64_t bathroomSample = 346LL * 48000LL;
    auto makeBaseBathroomCtx = [&]() {
        auto ctx = makeNativeReferenceContext(bathroomSample, 0u); // NRD REBLUR and progressive rendering
        const size_t sceneIdx = findSceneIndexAtSample(cue, bathroomSample);
        const auto& scene = cue.scenes.at(sceneIdx);
        ctx.sampleRate = cue.sampleRate;
        ctx.actIndex = static_cast<uint32_t>(sceneIdx);
        ctx.actProgress = float(double(bathroomSample - scene.startSample) /
                                double(std::max<int64_t>(1, scene.endSample - scene.startSample)));
        ctx.randomSeed = scene.deterministicSeed;
        ctx.edition = Edition::Family;
        ctx.qualityPreset = QualityPresetId::Balanced;
        ctx.showOverlay = false;
        ctx.hudMode = 3u;
        ctx.showCaptions = false;
        ScriptedDirector dir;
        dir.reset(cue.seed);
        const auto mood = dir.evaluate(ctx, &scene);
        ctx.moodIrisAngle = mood.irisAngle;
        ctx.moodIntensity = mood.lightTension;
        ctx.moodPaletteAccent = mood.spectralShift;
        ctx.moodBloomStrength = mood.bloomStrength;
        ctx.moodCutAccent = mood.cutAccent;
        return ctx;
    };

    const FrameContext canonicalCtx = makeBaseBathroomCtx();
    std::vector<uint8_t> baseA, baseB;
    std::vector<uint32_t> rgb10A, rgb10B;
    uint32_t cw = 0, ch = 0;

    // 1. Canonical baseline render at 346.0 s (Dawn Bathroom)
    fx.resetTo(346.0);
    BOO_CHECK(fx.renderHeadlessCanvasForTest(canonicalCtx, baseA, cw, ch, &err, &rgb10A));
    BOO_CHECK(cw == 960u && ch == 540u);
    uint64_t nonFinite = 0;
    BOO_CHECK(fx.readSceneHdrFiniteForTest(nonFinite, &err) && nonFinite == 0);

    // 2. Interactive 6-DOF Hero Preset 2 (CNC Faucet Macro) + NRD / Progressive PT accumulation (8 SPP)
    FrameContext macroCtx = canonicalCtx;
    macroCtx.interactive.exploreActive = true;
    macroCtx.interactive.blendWeight = 1.0f;
    macroCtx.interactive.activePreset = 2u;
    macroCtx.interactive.eye[0] = 0.05f;    macroCtx.interactive.eye[1] = 3.83f;    macroCtx.interactive.eye[2] = 5.76f;
    macroCtx.interactive.target[0] = -0.18f; macroCtx.interactive.target[1] = 3.67f; macroCtx.interactive.target[2] = 6.34f;
    macroCtx.interactive.fovYRadians = 0.44f;
    macroCtx.interactive.accumulate = true;

    std::vector<uint8_t> macroSpp1, macroSpp8;
    std::vector<uint32_t> macroRgb10Spp8;
    for (uint32_t s = 0; s < 8u; ++s) {
        macroCtx.presentationFrame = s;
        macroCtx.interactive.accumFrame = s;
        std::vector<uint8_t> cur;
        BOO_CHECK(fx.renderHeadlessCanvasForTest(
            macroCtx, cur, cw, ch, &err, (s == 7u) ? &macroRgb10Spp8 : nullptr));
        BOO_CHECK(fx.readSceneHdrFiniteForTest(nonFinite, &err) && nonFinite == 0);
        if (s == 0u) macroSpp1 = std::move(cur);
        if (s == 7u) macroSpp8 = std::move(cur);
    }
    BOO_CHECK(computeMeanAbsDiffRgb01(macroSpp1, baseA) > 0.025);
    BOO_CHECK(macroSpp8 != macroSpp1);
    BOO_CHECK(computeMeanAbsDiffRgb01(macroSpp8, macroSpp1) < 0.085);

    // 3. Pass-4 Studio Bar + Draggable Split Curtain isolation (must never contaminate Pass-3 CanvasClean!)
    HudTelemetrySnapshot hud{};
    hud.filmTimeSeconds = 346.0;
    hud.durationSeconds = 360.0;
    hud.fps = 60.0f;
    hud.canvasWidth = 960;
    hud.canvasHeight = 540;
    hud.swapchainBits = 8;

    FrameContext wipeLeft = macroCtx;
    wipeLeft.showOverlay = true;
    wipeLeft.hudMode = 2u;
    wipeLeft.interactive.abSplitRatio = 0.26f;
    wipeLeft.interactive.accumFrame = 7u;
    fx.setAbCompareEngine(AbCompareEngine::RT);

    std::vector<uint8_t> presWipe26, presWipe74, verifyClean;
    std::vector<uint32_t> verifyRgb10;
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(wipeLeft, hud, 960, 540, presWipe26, &err));
    BOO_CHECK(fx.readLastRenderedCanvasForTest(verifyClean, cw, ch, &err, &verifyRgb10));

    FrameContext wipeRight = wipeLeft;
    wipeRight.interactive.abSplitRatio = 0.74f;
    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(wipeRight, hud, 960, 540, presWipe74, &err));
    if (!d3d.isWarp()) {
        BOO_CHECK(presWipe26 != presWipe74);
    }

    // 4. Live Sun Time-of-Day Override + RTXDI ReSTIR Practical Lamps Toggle at Midnight
    FrameContext sunGolden = canonicalCtx;
    sunGolden.interactive.exploreActive = true;
    sunGolden.interactive.blendWeight = 1.0f;
    sunGolden.interactive.eye[0] = 0.34f;    sunGolden.interactive.eye[1] = 4.02f;    sunGolden.interactive.eye[2] = 5.12f;
    sunGolden.interactive.target[0] = -0.18f; sunGolden.interactive.target[1] = 3.88f; sunGolden.interactive.target[2] = 6.32f;
    sunGolden.interactive.fovYRadians = 0.56f;
    sunGolden.interactive.sunOverride = true;
    sunGolden.interactive.sunDir[0] = 0.80f; sunGolden.interactive.sunDir[1] = 0.34f; sunGolden.interactive.sunDir[2] = -0.49f;
    sunGolden.interactive.sunScale = 1.05f;

    FrameContext sunNightLampsOn = sunGolden;
    sunNightLampsOn.interactive.sunDir[0] = 0.80f; sunNightLampsOn.interactive.sunDir[1] = -0.28f; sunNightLampsOn.interactive.sunDir[2] = -0.49f;
    sunNightLampsOn.interactive.sunScale = 1.0f;
    sunNightLampsOn.interactive.lampsOff = false;

    FrameContext sunNightLampsOff = sunNightLampsOn;
    sunNightLampsOff.interactive.lampsOff = true;

    std::vector<uint8_t> imgGolden, imgNightOn, imgNightOff;
    BOO_CHECK(fx.renderHeadlessCanvasForTest(sunGolden, imgGolden, cw, ch, &err));
    BOO_CHECK(fx.renderHeadlessCanvasForTest(sunNightLampsOn, imgNightOn, cw, ch, &err));
    BOO_CHECK(fx.renderHeadlessCanvasForTest(sunNightLampsOff, imgNightOff, cw, ch, &err));

    const auto mGolden = computeRgba8LumaMetrics(imgGolden);
    const auto mNightOn = computeRgba8LumaMetrics(imgNightOn);
    const auto mNightOff = computeRgba8LumaMetrics(imgNightOff);
    BOO_CHECK(computeMeanAbsDiffRgb01(imgGolden, imgNightOn) > 0.015);
    BOO_CHECK(mGolden.meanLuma > mNightOn.meanLuma);
    if (!d3d.isWarp()) {
        BOO_CHECK(mNightOn.meanLuma > mNightOff.meanLuma + 0.002);
        BOO_CHECK(computeMeanAbsDiffRgb01(imgNightOn, imgNightOff) > 0.003);
    }

    // 5. Live Motion NRD::REBLUR_DIFFUSE_SPECULAR + Dynamic Ray-Traced Marbles RTX Physics Spheres
    {
        const bool hadNrdStationary = (!d3d.isWarp() && fx.activeDenoiseMode() == 2u);

        FrameContext noMarblesCtx = macroCtx;
        noMarblesCtx.interactive.accumulate = false;
        noMarblesCtx.interactive.accumFrame = 0u;
        noMarblesCtx.interactive.marbleCount = 0u;
        std::vector<uint8_t> imgNoMarbles;
        BOO_CHECK(fx.renderHeadlessCanvasForTest(noMarblesCtx, imgNoMarbles, cw, ch, &err));
        BOO_CHECK(fx.readSceneHdrFiniteForTest(nonFinite, &err) && nonFinite == 0);
        if (hadNrdStationary) {
            BOO_CHECK(fx.activeDenoiseMode() == 1u); // Seamless REBLUR motion denoiser engagement!
        }

        FrameContext marblesMotionCtx = noMarblesCtx;
        marblesMotionCtx.interactive.marbleCount = 6u;
        static constexpr uint32_t kTestMarbleMats[6] = {3u, 4u, 35u, 8u, 23u, 31u};
        for (uint32_t mIdx = 0; mIdx < 6u; ++mIdx) {
            const float ang = static_cast<float>(mIdx) * 1.0471976f;
            const float rad = 0.021f;
            const float px = -0.180f + std::cos(ang) * 0.145f;
            const float pz =  6.245f + std::sin(ang) * 0.105f;
            const float py =  3.620f + rad;
            marblesMotionCtx.interactive.marblePosRad[mIdx][0] = px;
            marblesMotionCtx.interactive.marblePosRad[mIdx][1] = py;
            marblesMotionCtx.interactive.marblePosRad[mIdx][2] = pz;
            marblesMotionCtx.interactive.marblePosRad[mIdx][3] = rad;
            marblesMotionCtx.interactive.marblePrevPos[mIdx][0] = px - 0.015f;
            marblesMotionCtx.interactive.marblePrevPos[mIdx][1] = py;
            marblesMotionCtx.interactive.marblePrevPos[mIdx][2] = pz - 0.010f;
            marblesMotionCtx.interactive.marblePrevPos[mIdx][3] = 0.0f;
            marblesMotionCtx.interactive.marbleRotQuat[mIdx][3] = 1.0f;
            marblesMotionCtx.interactive.marbleMat[mIdx] = kTestMarbleMats[mIdx];
        }

        std::vector<uint8_t> imgMarblesMotion;
        for (uint32_t f = 0; f < 4u; ++f) {
            marblesMotionCtx.presentationFrame = 20u + f;
            marblesMotionCtx.interactive.accumFrame = f;
            marblesMotionCtx.interactive.eye[0] = 0.05f + 0.018f * static_cast<float>(f);
            for (uint32_t mIdx = 0; mIdx < 6u; ++mIdx) {
                marblesMotionCtx.interactive.marblePrevPos[mIdx][0] = marblesMotionCtx.interactive.marblePosRad[mIdx][0];
                marblesMotionCtx.interactive.marblePosRad[mIdx][0] += 0.012f;
            }
            BOO_CHECK(fx.renderHeadlessCanvasForTest(marblesMotionCtx, imgMarblesMotion, cw, ch, &err));
            BOO_CHECK(fx.readSceneHdrFiniteForTest(nonFinite, &err) && nonFinite == 0);
            if (hadNrdStationary) {
                BOO_CHECK(fx.activeDenoiseMode() == 1u);
            }
        }
        BOO_CHECK(computeMeanAbsDiffRgb01(imgMarblesMotion, imgNoMarbles) > 0.004);

        // Seamless hand-off back to NRD::REFERENCE when camera & marbles settle
        FrameContext marblesSettled = marblesMotionCtx;
        marblesSettled.interactive.accumulate = true;
        marblesSettled.interactive.accumFrame = 0u;
        for (uint32_t mIdx = 0; mIdx < 6u; ++mIdx) {
            marblesSettled.interactive.marblePrevPos[mIdx][0] = marblesSettled.interactive.marblePosRad[mIdx][0];
        }
        std::vector<uint8_t> imgMarblesRef;
        BOO_CHECK(fx.renderHeadlessCanvasForTest(marblesSettled, imgMarblesRef, cw, ch, &err));
        if (hadNrdStationary) {
            BOO_CHECK(fx.activeDenoiseMode() == 2u);
        }
    }

    // 6. 8-Mode Live Buffer / AOV Inspection (Base Color, Normal, ORM, Direct, GI, Refl/Refr, Ray Heatmap)
    {
        FrameContext aovCtx = macroCtx;
        aovCtx.interactive.accumulate = false;
        aovCtx.interactive.accumFrame = 0u;

        std::array<std::vector<uint8_t>, 8> aovCanvases{};
        for (uint32_t mode = 1u; mode <= 7u; ++mode) {
            aovCtx.interactive.aovMode = mode;
            BOO_CHECK(fx.renderHeadlessCanvasForTest(aovCtx, aovCanvases[mode], cw, ch, &err));
            BOO_CHECK(fx.readSceneHdrFiniteForTest(nonFinite, &err) && nonFinite == 0);
            const auto mAov = computeRgba8LumaMetrics(aovCanvases[mode]);
            BOO_CHECK(mAov.meanLuma > 0.008);
        }
        BOO_CHECK(computeMeanAbsDiffRgb01(aovCanvases[1], aovCanvases[2]) > 0.03);
        BOO_CHECK(computeMeanAbsDiffRgb01(aovCanvases[2], aovCanvases[3]) > 0.03);
        BOO_CHECK(computeMeanAbsDiffRgb01(aovCanvases[5], aovCanvases[6]) > 0.015);
        BOO_CHECK(computeMeanAbsDiffRgb01(aovCanvases[7], aovCanvases[1]) > 0.03);

        // Verify Pass-4 AOV banner renders on presented output while keeping Pass-3 CanvasClean pure
        aovCtx.showOverlay = true;
        aovCtx.hudMode = 1u;
        aovCtx.interactive.aovMode = 2u;
        std::vector<uint8_t> aovPres, aovCleanAfterPres;
        BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(aovCtx, hud, 960, 540, aovPres, &err));
        BOO_CHECK(fx.readLastRenderedCanvasForTest(aovCleanAfterPres, cw, ch, &err));
        BOO_CHECK(aovCleanAfterPres == aovCanvases[2]);
        BOO_CHECK(aovPres != aovCleanAfterPres);
    }

    // 7. Return to Canonical Film Mode -> MUST match baseline frame bit-for-bit (`rgb10A == rgb10B`)!
    fx.resetTo(346.0);
    BOO_CHECK(fx.renderHeadlessCanvasForTest(canonicalCtx, baseB, cw, ch, &err, &rgb10B));
    BOO_CHECK(rgb10A == rgb10B && baseA == baseB);

    fx.shutdown();
    d3d.waitForGpu();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    d3d.shutdown();
    return true;
}

// Live audio drive must reach the image (mesh emissives via the packed scene DWORD, composite lens pump
// via gAudioKick), stay deterministic for a given drive, and leave no state behind that could change a
// later zero-drive render (seek determinism). Captures and the other gates always run with zero drive.
bool testAudioReactiveDriveReachesCanvasDeterministically() {
    using namespace boo;
    D3D12Context d3d;
    std::string err;
    BOO_CHECK(d3d.initialize(nullptr, 960, 540, false, true, &err));
    freezeDriverProfilingForTest(d3d);
    auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    QualityPresetSpec spec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    spec.renderWidth = 960;
    spec.renderHeight = 540;
    M1TaipeiSliceEffect fx(d3d, spec, false, false, false, false, false, false, false, false, ShowVisualMode::FullFilm360s);
    BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);

    // 05:20 night skyline: lit curtain-wall windows, jade accents and light shafts are all in frame.
    auto ctxZero = makeNativeReferenceContext(15360000LL, 6u);
    ctxZero.randomSeed = 307001ULL;
    ctxZero.edition = Edition::Family;
    ctxZero.showOverlay = false;
    ctxZero.hudMode = 3;
    auto ctxFull = ctxZero;
    ctxFull.audioReactive.low = 1.0f;
    ctxFull.audioReactive.mid = 1.0f;
    ctxFull.audioReactive.high = 1.0f;
    ctxFull.audioReactive.beat = 1.0f;
    ctxFull.audioReactive.level = 1.0f;
    auto ctxBeat = ctxZero;
    ctxBeat.audioReactive.beat = 1.0f;

    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    std::vector<uint32_t> zero10, full10, fullRepeat10, beat10, zeroAgain10;
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctxZero, rgba, w, h, &err, &zero10));
    BOO_CHECK(w == 960 && h == 540);
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctxFull, rgba, w, h, &err, &full10));
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctxFull, rgba, w, h, &err, &fullRepeat10));
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctxBeat, rgba, w, h, &err, &beat10));
    fx.resetTo(0.0);
    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctxZero, rgba, w, h, &err, &zeroAgain10));

    BOO_CHECK(full10 != zero10);        // band + beat drive reaches the canvas
    BOO_CHECK(beat10 != zero10);        // beat alone reaches the canvas (composite lens pump)
    BOO_CHECK(full10 == fullRepeat10);  // deterministic for a given drive
    BOO_CHECK(zeroAgain10 == zero10);   // no residual state leaks into later zero-drive renders

    fx.shutdown();
    d3d.waitForGpu();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    d3d.shutdown();
    return true;
}

#include "film_capture_gate.hpp"
#include "city_baseline.hpp"
#include "film_stress_gate.hpp"

// Uses the same cue, features, frame and director implementation as film playback.
bool testDirectorComparisonCaptures() {
    using namespace boo;
    CaptureComScope com;
    BOO_CHECK(SUCCEEDED(com.result) || com.result == RPC_E_CHANGED_MODE);
    std::filesystem::create_directories(gFilmCaptureDirectory);
    OpenVinoDirector director;
    OpenVinoDirectorConfig config;
    config.modelXmlPath = resolveResourcePath("assets/models/madame_n_director.xml");
    const auto backend = director.initialize(config);
    BOO_CHECK(director.isReady());
    std::cout << "  " << backend.humanReadableReason << "\n";
    D3D12Context d3d;
    std::string error;
    BOO_CHECK(d3d.initialize(nullptr, 960, 540, false, false, &error));
    auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    auto spec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    spec.renderWidth = 960;
    spec.renderHeight = 540;
    for (bool fullFilm : {false, true}) {
        CueSheet cue;
        ValidationReport report;
        BOO_CHECK(loadCueSheetFromFile(resolveResourcePath(fullFilm ? "assets/cues/film_360s.json" : "assets/cues/solo_npu.json"), cue, report));
        const uint32_t sceneIndex = fullFilm ? 3u : 0u;
        const auto& scene = cue.scenes.at(sceneIndex);
        const char* name = fullFilm ? "film168" : "solo22";
        const auto mode = fullFilm ? ShowVisualMode::FullFilm360s : ShowVisualMode::SoloNPU;
        M1TaipeiSliceEffect effect(d3d, spec, false, false, false, false, false, false, false, false, mode);
        BOO_CHECK(effect.probeAndInitialize().status != BackendStatus::Faulted);
        effect.setAbCompareEngine(AbCompareEngine::NPU);
        auto ctx = makeM1TestContext((fullFilm ? 168LL : 22LL) * cue.sampleRate, 0);
        ctx.randomSeed = cue.seed;
        ctx.qualityPreset = spec.id;
        ctx.actIndex = sceneIndex;
        ctx.actProgress = float(ctx.samplePosition - scene.startSample) / float(scene.endSample - scene.startSample);
        ctx.audioEnvelope = computeAnalyticalAudioBands(ctx.samplePosition, cue.sampleRate, cue.durationSamples, ctx.edition).rms;
        ctx.hudMode = 2;
        ScriptedDirector script;
        script.reset(cue.seed);
        const auto baseline = script.evaluate(ctx, &scene);
        NeuralInferenceSample neural;
        BOO_CHECK(director.infer(script.buildFeatures(ctx, &scene), neural));
        ctx.moodBloomStrength = neural.controls[0];
        ctx.moodIrisAngle = neural.controls[1];
        ctx.moodIntensity = neural.controls[2];
        ctx.moodPaletteAccent = neural.controls[3];
        ctx.moodCutAccent = neural.controls[4];
        ctx.scriptedDirectorComparison = {baseline.bloomStrength, baseline.irisAngle, baseline.lightTension,
                                         baseline.spectralShift, baseline.cutAccent};
        uint32_t w = 0, h = 0;
        std::vector<uint32_t> live10, script10;
        std::vector<uint8_t> live8, script8, presented;
        BOO_CHECK(effect.renderHeadlessAbCompareForTest(ctx, live8, script8, w, h, &error, &live10, &script10));
        BOO_CHECK(live10 != script10);
        reportDirectorFrameDifference(name, live10, script10, w, h);
        const auto path = [&](const char* suffix) { return gFilmCaptureDirectory / (std::string(name) + suffix + ".png"); };
        BOO_CHECK(writeCapturePng(path("_model"), live8, w, h));
        BOO_CHECK(writeCapturePng(path("_script"), script8, w, h));
        HudTelemetrySnapshot hud{};
        hud.directorBackend = classifyDirectorHudBackend(backend);
        hud.engineStatus[static_cast<size_t>(EngineKind::NPU)] = backend.status;
        hud.npuCompletedInferences = 1;
        hud.npuInferMs = neural.latencyMs;
        hud.mood.isScriptedFallback = false;
        BOO_CHECK(effect.renderHeadlessPresentedFrameForTest(ctx, hud, w, h, presented, &error));
        BOO_CHECK(writeCapturePng(path("_comparison"), presented, w, h));
        // Switching between two identical light programs must leave every 10-bit pixel intact.
        ctx.scriptedDirectorComparison = {ctx.moodBloomStrength, ctx.moodIrisAngle, ctx.moodIntensity,
                                         ctx.moodPaletteAccent, ctx.moodCutAccent};
        BOO_CHECK(effect.renderHeadlessAbCompareForTest(ctx, live8, script8, w, h, &error, &live10, &script10));
        BOO_CHECK(live10 == script10);
        effect.shutdown();
    }
    d3d.waitForGpu();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    return true;
}


} // namespace

bool testQuarantinedGpuRetirement() {
    using Microsoft::WRL::ComPtr;
    boo::D3D12Context d3d;
    std::string error;
    BOO_CHECK(d3d.initialize(nullptr, 64, 64, true, true, &error));
    auto* device = static_cast<ID3D12Device*>(d3d.deviceRaw());
    auto* queue = static_cast<ID3D12CommandQueue*>(d3d.commandQueueRaw());
    ComPtr<ID3D12Fence> gate, marker;
    BOO_CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))));
    BOO_CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker))));
    BOO_CHECK(SUCCEEDED(queue->Wait(gate.Get(), 1)));
    BOO_CHECK(SUCCEEDED(queue->Signal(marker.Get(), 1)));
    BOO_CHECK(d3d.injectSynchronizationFailureForTest());
    BOO_CHECK(!d3d.checkDeviceHealthy() && !d3d.waitForGpu());
    BOO_CHECK(marker->GetCompletedValue() == 0);
    // A healthy queue is deliberately blocked. Teardown must drain it despite
    // quarantine, and must not mistake the failure flag for retirement.
    std::thread releaseGate([gate] { Sleep(50); gate->Signal(1); });
    const bool drained = d3d.retireGpuForShutdown();
    releaseGate.join();
    BOO_CHECK(drained && marker->GetCompletedValue() == 1);
    BOO_CHECK(SUCCEEDED(device->GetDeviceRemovedReason()));
    BOO_CHECK(!d3d.checkDeviceHealthy()); // Drain never un-quarantines allocators.
    BOO_CHECK(d3d.retireGpuForShutdown()); // Idempotent.
    BOO_CHECK(d3d.debugErrorCount() == 0);
    d3d.shutdown();
    return true;
}

int main(int argc, char** argv) {
    boo::Logger::instance().setConsoleEnabled(false);

    bool cityBaseline = false;
    bool noActivate = false, trilogyCaptures = false, filmCaptures = false, heroVideoCapture = false, interactiveCaptures = false, validationCaptures = false, filmStress = false, lifecycleTests = false;
    bool effectsCaptures = false, directorCaptures = false;
    for (int arg = 1; arg < argc; ++arg) {
        const std::string option = argv[arg];
        if (option == "--no-activate") noActivate = true;
        else if (option == "--require-debug-layer") gRequireDebugLayer = true;
        else if (option == "--trilogy-captures") trilogyCaptures = true;
        else if (option == "--city-baseline") cityBaseline = true;
        else if (option == "--graphics-features") { cityBaseline = true; gGraphicsFeatures = true; }
        else if (option == "--require-ngx") { cityBaseline = true; gGraphicsFeatures = true; gRequireNgx = true; }
        else if (option == "--room-baseline" || option == "--room-baseline-raster") {
            cityBaseline = true;
            gRoomBaseline = true;
            gCityForceRaster = option == "--room-baseline-raster";
        }
        else if (option == "--show-baseline" || option == "--show-baseline-raster") {
            cityBaseline = true;
            gWholeShowBaseline = true;
            gCityForceRaster = option == "--show-baseline-raster";
        }
        else if (option == "--baseline-4k") gBaseline4K = true;
        else if (option == "--city-baseline-raster") { cityBaseline = true; gCityForceRaster = true; }
        else if (option == "--film-captures") filmCaptures = true;
        else if (option == "--hero-video-capture") heroVideoCapture = true;
        else if (option == "--interactive-captures") interactiveCaptures = true;
        else if (option == "--validation-captures") validationCaptures = true;
        else if (option == "--director-captures") directorCaptures = true;
        else if (option == "--effects-captures") effectsCaptures = true;
        else if (option == "--film-stress") filmStress = true;
        else if (option == "--lifecycle-tests") lifecycleTests = true;
        else if (option == "--capture-dir" && arg + 1 < argc) gFilmCaptureDirectory = argv[++arg];
        else if (option == "--capture-start-seconds" && arg + 1 < argc) {
            const std::string value = argv[++arg];
            uint32_t seconds = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || seconds > 350) {
                std::cerr << "--capture-start-seconds must be an integer in [0, 350].\n"; return 2;
            }
            gHeroCaptureStartSeconds = seconds;
        }
        else if (option == "--film-capture-seconds" && arg + 1 < argc) {
            // Comma-separated whole seconds inside the six-minute film, e.g. 0,8,18,24,30.
            const std::string list = argv[++arg];
            size_t start = 0;
            while (start <= list.size()) {
                const size_t end = std::min(list.find(',', start), list.size());
                int seconds = -1;
                const auto result = std::from_chars(list.data() + start, list.data() + end, seconds);
                if (result.ec != std::errc{} || result.ptr != list.data() + end || seconds < 0 || seconds > 359) {
                    std::cerr << "--film-capture-seconds takes whole seconds in [0, 359], comma-separated.\n"; return 2;
                }
                gFilmCaptureSeconds.push_back(seconds);
                start = end + 1;
            }
        }
        else if (option == "--stress-iterations" && arg + 1 < argc) {
            const std::string value = argv[++arg];
            const auto result = std::from_chars(value.data(), value.data() + value.size(), gFilmStressIterations);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || gFilmStressIterations < 200 || gFilmStressIterations > 100000) {
                std::cerr << "--stress-iterations must be an integer in [200, 100000].\n"; return 2;
            }
        }
        else { std::cerr << "Unknown or incomplete option: " << option << "\n"; return 2; }
    }
    if (gBaseline4K && !cityBaseline) { std::cerr << "--baseline-4k requires a baseline capture suite.\n"; return 2; }
    if (gHeroCaptureStartSeconds && !heroVideoCapture) { std::cerr << "--capture-start-seconds requires --hero-video-capture.\n"; return 2; }
    if (!gFilmCaptureSeconds.empty() && !filmCaptures) { std::cerr << "--film-capture-seconds requires --film-captures.\n"; return 2; }
    if (int(trilogyCaptures) + int(filmCaptures) + int(heroVideoCapture) + int(interactiveCaptures) + int(validationCaptures) + int(effectsCaptures) + int(directorCaptures) + int(filmStress) + int(lifecycleTests) + int(cityBaseline) > 1) { std::cerr << "Choose one dedicated integration suite.\n"; return 2; }
    if ((trilogyCaptures || filmCaptures || heroVideoCapture || interactiveCaptures || validationCaptures || effectsCaptures || directorCaptures || filmStress || lifecycleTests || cityBaseline) && !noActivate) {
        std::cerr << "Automated integration gates require --no-activate.\n";
        return 2;
    }
    if (trilogyCaptures) { runTest("M2CapturesAndDeterminism", testM2CapturesAndDeterminism); return gTestsFailed ? 1 : 0; }
    if (cityBaseline) { runTest("CityBaseline", testCityBaseline); return gTestsFailed ? 1 : 0; }
    if (filmCaptures) { runTest("FullFilmCapturesAndMontage", testFullFilmCaptureGate); return gTestsFailed ? 1 : 0; }
    if (heroVideoCapture) { runTest("HeroTenSecondVideoCapture", testHeroVideoCaptureGate); return gTestsFailed ? 1 : 0; }
    if (interactiveCaptures) { runTest("InteractiveShowcaseCaptures", testInteractiveShowcaseCaptureGate); return gTestsFailed ? 1 : 0; }
    if (validationCaptures) { runTest("ValidationShowcaseCaptures", testValidationShowcaseCaptures); return gTestsFailed ? 1 : 0; }
    if (directorCaptures) { runTest("DirectorComparisonCaptures", testDirectorComparisonCaptures); return gTestsFailed ? 1 : 0; }
    if (effectsCaptures) { runTest("EffectsDetailCaptures", testEffectsDetailCaptures); return gTestsFailed ? 1 : 0; }
    if (filmStress) { runTest("FullFilmStressAndRecovery", testFilmStressGate); return gTestsFailed ? 1 : 0; }
    if (lifecycleTests) {
        runTest("QuarantinedGpuRetirement", testQuarantinedGpuRetirement);
        runTest("Spout2CleanSenderAndLiveD3d11Receiver", testSpout2CleanSenderAndLiveD3d11Receiver);
        return gTestsFailed ? 1 : 0;
    }
    runTest("CapabilityProbeHonestyAndPermutations", testCapabilityProbeHonestyAndPermutations);
    runTest("D3D12HeadlessAndShaderFallback", testD3D12HeadlessAndShaderFallback);
    runTest("QuarantinedGpuRetirement", testQuarantinedGpuRetirement);
    runTest("AudioAndSilentFallbackClockIntegration", testAudioAndSilentFallbackClockIntegration);
    runTest("WasapiPresentationClockAndMigrationLifecycle", testWasapiPresentationClockAndMigrationLifecycle);
    runTest("AdHocNumericalBenchmarksVerification", testAdHocNumericalBenchmarksVerification);
    runTest("M1HeadlessDeterminismAndDxrVerification", testM1HeadlessDeterminismAndDxrVerification);
    runTest("LiveFourPillarHardwareAttributionAndAbSplits", testLiveFourPillarHardwareAttributionAndAbSplits);
    runTest("StandaloneHardwareSolosRtCudaTensor", testStandaloneHardwareSolosRtCudaTensor);
    runTest("MadameNNpuOpenVinoAndSoloNpuFailover", testMadameNNpuOpenVinoAndSoloNpuFailover);
    runTest("FullFilm360sRevealsEditionsAndM7CombinedFailover", testFullFilm360sRevealsEditionsAndM7CombinedFailover);
    runTest("AudioReactiveDriveReachesCanvasDeterministically", testAudioReactiveDriveReachesCanvasDeterministically);
    runTest("Spout2CleanSenderAndLiveD3d11Receiver", testSpout2CleanSenderAndLiveD3d11Receiver);
    runTest("SignalHijackMediaIngestCameraPrivacyAndFallback", testSignalHijackMediaIngestCameraPrivacyAndFallback);
    runTest("InteractiveExploreLightingAndNrdConvergence", testInteractiveExploreLightingAndNrdConvergence);
    runTest("NativeCanvasModesAndCapture", testNativeCanvasModesAndCapture);
    if (noActivate) {
        runTest("NoActivateWindowTransitions", testNoActivateWindowTransitions);
        runTest("LiveNoActivateSwapchainDeterminism", testLiveNoActivateSwapchainDeterminism);
        runTest("M2LiveNoActivateSwapchainDeterminism", testM2LiveNoActivateSwapchainDeterminism);
    }

    std::cout << "\nIntegration Test Summary: " << (gTestsRun - gTestsFailed) << "/" << gTestsRun << " passed.\n";
    return gTestsFailed == 0 ? 0 : 1;
}
