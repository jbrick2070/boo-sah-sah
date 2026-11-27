#include "boo/results_benchmark.hpp"
#include "boo/cues.hpp"
#include "boo/director.hpp"
#include "boo/render.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxcore.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace boo {
using Microsoft::WRL::ComPtr;
namespace {
constexpr uint64_t MiB = 1024ull * 1024ull;
// The full-film stress gate peaks at 1,637,195,776 bytes on this content revision.
// Reserve 2.5 GiB for the extra worker, including PT/DLSS, plus the paused viewer.
constexpr uint64_t preparationBudget = 2560 * MiB;
constexpr uint64_t betweenFrameMargin = 128 * MiB;

BenchmarkPower readPower() {
    SYSTEM_POWER_STATUS power{};
    if (!GetSystemPowerStatus(&power)) return BenchmarkPower::Unknown;
    if (power.ACLineStatus == 0) return BenchmarkPower::Battery;
    if (power.ACLineStatus == 1) return BenchmarkPower::Mains;
    return BenchmarkPower::Unknown;
}

BenchmarkPower updatePower(BenchmarkPower previous) {
    const auto now = readPower();
    if (previous == BenchmarkPower::Changed || previous == BenchmarkPower::Unknown) return previous;
    if (now == BenchmarkPower::Unknown) return now;
    return now == previous ? previous : BenchmarkPower::Changed;
}

std::shared_ptr<std::atomic<uint64_t>> lookupDriver(LUID luid) {
    auto output = std::make_shared<std::atomic<uint64_t>>(0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    // A slow optional driver query must never delay the pass or teardown.
    try {
        std::thread([luid, output, deadline] {
            HMODULE module = LoadLibraryExW(L"dxcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) return;
            {
                using CreateFactory = HRESULT (WINAPI*)(REFIID, void**);
                const auto create = reinterpret_cast<CreateFactory>(GetProcAddress(module, "DXCoreCreateAdapterFactory"));
                ComPtr<IDXCoreAdapterFactory> factory;
                ComPtr<IDXCoreAdapter> adapter;
                uint64_t version = 0;
                if (create && SUCCEEDED(create(IID_PPV_ARGS(&factory))) &&
                    SUCCEEDED(factory->GetAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) &&
                    SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::DriverVersion, &version)) &&
                    std::chrono::steady_clock::now() <= deadline) output->store(version);
            }
            FreeLibrary(module);
        }).detach();
    } catch (const std::system_error&) {}
    return output;
}

bool hasHeadroom(IDXGIAdapter3* adapter, uint64_t viewerUsage, uint64_t required) {
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)) ||
        memory.CurrentUsage > memory.Budget) return false;
    const uint64_t remaining = memory.Budget - memory.CurrentUsage;
    // DXGI accounts usage per process. The separate viewer stays allocated throughout this check.
    if (viewerUsage > remaining || required > remaining - viewerUsage) return false;
    MEMORYSTATUSEX system{sizeof(system)};
    return GlobalMemoryStatusEx(&system) && system.ullAvailPhys >= required;
}

FrameContext makeFrame(const ResultsSample& sample, BenchmarkMode mode, const SceneCue& cue,
                       ScriptedDirector& director) {
    FrameContext frame{};
    frame.presentationFrame = sample.tag;
    frame.samplePosition = sample.sample48k;
    frame.filmTimeSeconds = static_cast<double>(sample.sample48k) / frame.sampleRate;
    frame.dtSeconds = 1.0 / 30.0;
    frame.actProgress = static_cast<float>(sample.sample48k - cue.startSample) /
                        static_cast<float>(cue.endSample - cue.startSample);
    frame.randomSeed = cue.deterministicSeed;
    frame.qualityPreset = QualityPresetId::Safe;
    frame.effectsDetail = 0;
    frame.hudMode = 3;
    frame.showOverlay = false;
    frame.pathTracing = mode == BenchmarkMode::PathTracing ? PathTracingLevel::High : PathTracingLevel::Off;
    frame.dlssMode = mode == BenchmarkMode::DlssQuality ? DlssMode::Quality : DlssMode::Off;
    frame.rayReconstruction = false;
    frame.frameGeneration = FrameGenerationMode::Off;
    frame.reflex = ReflexMode::Off;
    const auto mood = director.evaluate(frame, &cue);
    frame.moodIrisAngle = mood.irisAngle;
    frame.moodBloomStrength = mood.bloomStrength;
    frame.moodIntensity = mood.lightTension;
    frame.moodPaletteAccent = mood.spectralShift;
    frame.moodCutAccent = mood.cutAccent;
    return frame;
}

BenchmarkReason measure(const BenchmarkRequest& request, BenchmarkReporter& reporter, BenchmarkSnapshot& state,
                        D3D12Context& device, std::unique_ptr<M1TaipeiSliceEffect>& scene,
                        ResultsMeasurements& timings) {
    if (request.mode != BenchmarkMode::Raster && request.mode != BenchmarkMode::PathTracing &&
        request.mode != BenchmarkMode::DlssQuality) return BenchmarkReason::UnsupportedDevice;
    const LUID luid{static_cast<DWORD>(request.luidLow), request.luidHigh};
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return BenchmarkReason::UnsupportedDevice;
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) || FAILED(adapter->GetDesc1(&desc)))
        return BenchmarkReason::AdapterMismatch;
    state.vendorId = desc.VendorId;
    state.deviceId = desc.DeviceId;
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return BenchmarkReason::SoftwareRenderer;
    if (!hasHeadroom(adapter.Get(), request.viewerLocalUsage, preparationBudget)) return BenchmarkReason::MemoryBudget;
    const auto driver = lookupDriver(luid);
    if (!reporter.publish(state) || reporter.cancelled()) return BenchmarkReason::Cancelled;

    const bool trace = request.mode == BenchmarkMode::PathTracing;
    device.selectBenchmarkAdapter(request);
    device.setRayTracingAllowed(trace);
    if (!device.initialize(nullptr, 1280, 720, false, false)) return BenchmarkReason::UnsupportedDevice;
    if (device.isWarp()) return BenchmarkReason::SoftwareRenderer;
    if (device.adapterLuidLow() != request.luidLow || device.adapterLuidHigh() != request.luidHigh)
        return BenchmarkReason::AdapterMismatch;
    if (trace) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 rt{};
        auto* d3d = static_cast<ID3D12Device*>(device.deviceRaw());
        if (FAILED(d3d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &rt, sizeof(rt))))
            return BenchmarkReason::RendererFailed;
        if (rt.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) return BenchmarkReason::RequiresDxr;
    }
    if (request.mode == BenchmarkMode::DlssQuality) {
        if (!resultsDlssInBuild()) return BenchmarkReason::RequiresDlss;
        if (desc.VendorId != 0x10de) return BenchmarkReason::UnsupportedDevice;
    }
    if (reporter.cancelled()) return BenchmarkReason::Cancelled;
    if (!useInstalledAssetDirectory()) return BenchmarkReason::RendererFailed;
    CueSheet sheet;
    ValidationReport validation;
    if (!loadCueSheetFromFile(resolveResourcePath("assets/cues/film_360s.json"), sheet, validation) ||
        sheet.scenes.empty() || sheet.scenes[0].sceneId != "taipei_intro" || sheet.seed != 266203 ||
        sheet.sampleRate != 48000 || sheet.edition != Edition::Family || sheet.scenes[0].startSample != 0 ||
        sheet.scenes[0].endSample != 2160000 || !sheet.scenes[0].allowDirectorModulation)
        return BenchmarkReason::WorkloadChanged;
    const auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    auto safe = findQualityPresetSpec(presets, QualityPresetId::Safe);
    if (safe.id != QualityPresetId::Safe || safe.renderWidth != 1280 || safe.renderHeight != 720 ||
        safe.allowDxr || safe.allowNpu || safe.allowCuda || safe.allowTensor) return BenchmarkReason::WorkloadChanged;
    safe.allowDxr = trace;
    scene = std::make_unique<M1TaipeiSliceEffect>(device, safe, !trace, false, true, true, true, false,
                                                  true, false, ShowVisualMode::FullFilm360s);
    const auto init = scene->probeAndInitialize();
    state.driverVersion = driver->load();
    if (!device.checkDeviceHealthy()) return BenchmarkReason::GraphicsReset;
    if (init.status == BackendStatus::Faulted) return BenchmarkReason::RendererFailed;
    if (!scene->hasAllAuthoredSceneAssets()) return BenchmarkReason::WorkloadChanged;
    state.width = scene->canvasWidth();
    state.height = scene->canvasHeight();
    if (state.width != 1280 || state.height != 720) return BenchmarkReason::WorkloadChanged;
    if (request.mode == BenchmarkMode::DlssQuality) {
        const auto support = scene->dlssBenchmarkAvailability();
        if (support != BenchmarkReason::None) return support;
    }

    scene->resetTo(7.0);
    ScriptedDirector director;
    director.reset(sheet.seed);
    for (size_t index = 0; index < kResultsSampleCount; ++index) {
        const auto sample = *resultsSampleAt(index);
        const auto stage = sample.kind == ResultsSampleKind::Warmup ? BenchmarkStage::Warming : BenchmarkStage::Measuring;
        if (state.stage != stage) {
            state.stage = stage;
            if (!reporter.publish(state)) return BenchmarkReason::Cancelled;
        }
        if (reporter.cancelled()) return BenchmarkReason::Cancelled;
        if (!device.checkDeviceHealthy()) return BenchmarkReason::GraphicsReset;
        if (!hasHeadroom(adapter.Get(), request.viewerLocalUsage, betweenFrameMargin)) return BenchmarkReason::MemoryBudget;
        const FrameContext frame = makeFrame(sample, request.mode, sheet.scenes[0], director);
        double milliseconds = 0;
        const auto reason = scene->drawBenchmarkFrame(frame, sample, state, milliseconds);
        if (reason != BenchmarkReason::None) return reason;
        if (sample.kind == ResultsSampleKind::Warmup) ++state.warmupCount;
        else {
            if (!timings.record(sample.tag, milliseconds)) return BenchmarkReason::TimingUnavailable;
            state.measuredCount = static_cast<uint32_t>(timings.validSampleCount());
        }
        state.power = updatePower(state.power);
        if (!reporter.publish(state)) return BenchmarkReason::Cancelled;
    }
    return BenchmarkReason::None;
}
} // namespace

bool resultsDlssInBuild() noexcept {
#ifdef BOO_HAS_DLSS
    return true;
#else
    return false;
#endif
}

void runResultsBenchmark(const BenchmarkRequest& request, BenchmarkReporter& reporter) {
    BenchmarkSnapshot state{};
    state.mode = request.mode;
    state.power = readPower();
    if (!reporter.publish(state)) return;
    ResultsMeasurements timings;
    D3D12Context device;
    std::unique_ptr<M1TaipeiSliceEffect> scene;
    {
        try {
            state.reason = measure(request, reporter, state, device, scene, timings);
        } catch (const BenchmarkWorkloadChanged&) {
            state.reason = BenchmarkReason::WorkloadChanged;
        } catch (const std::bad_alloc&) {
            state.reason = BenchmarkReason::MemoryBudget;
        } catch (...) {
            state.reason = device.deviceRaw() && !device.checkDeviceHealthy() ?
                BenchmarkReason::GraphicsReset : BenchmarkReason::RendererFailed;
        }
        state.stage = BenchmarkStage::Finishing;
        reporter.publish(state);
        if (device.deviceRaw() && !device.retireGpuForShutdown()) state.reason = BenchmarkReason::GraphicsReset;
    }
    state.power = updatePower(state.power);
    auto outcome = ResultsOutcome::Unavailable;
    if (state.reason == BenchmarkReason::None) outcome = ResultsOutcome::Completed;
    else if (state.reason == BenchmarkReason::Cancelled) outcome = ResultsOutcome::Cancelled;
    else if (state.reason == BenchmarkReason::GraphicsReset || state.reason == BenchmarkReason::WorkloadChanged ||
             (state.reason == BenchmarkReason::MemoryBudget && state.warmupCount != 0)) outcome = ResultsOutcome::Interrupted;
    const auto result = timings.finish(outcome);
    state.outcome = result.outcome;
    if (result.summary) {
        state.medianMs = result.summary->medianMs;
        state.worstMs = result.summary->worstMs;
    } else if (state.reason == BenchmarkReason::None) state.reason = BenchmarkReason::TimingUnavailable;
    state.stage = BenchmarkStage::Finished;
    reporter.finish(state);
}
} // namespace boo
