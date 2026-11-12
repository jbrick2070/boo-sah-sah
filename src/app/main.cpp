#include "boo/api.hpp"
#include "boo/audio.hpp"
#include "boo/benchmark.hpp"
#include "boo/capabilities.hpp"
#include "boo/clock.hpp"
#include "boo/cues.hpp"
#include "boo/diagnostics.hpp"
#include "boo/director.hpp"
#include "boo/health.hpp"
#include "boo/logging.hpp"
#include "boo/media_input.hpp"
#include "boo/render.hpp"
#include "boo/settings.hpp"
#include "boo/spout_sender.hpp"
#include "boo/window.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

// Section 3.3: Export high-performance discrete GPU hints on hybrid laptops
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

boo::CueSheet makeEmergencyFallbackCueSheet() {
    boo::CueSheet cs{};
    cs.schema = 1;
    cs.filmId = "boo-sah-sah-emergency-fallback";
    cs.sampleRate = boo::TimelineClock::kDefaultSampleRate;
    cs.durationSamples = boo::TimelineClock::kFullFilmSamples;
    cs.seed = 266203;
    cs.edition = boo::Edition::Family;

    boo::SceneCue cue{};
    cue.sceneId = "emergency_safe_scene";
    cue.startSample = 0;
    cue.endSample = cs.durationSamples;
    cue.startSeconds = 0.0;
    cue.endSeconds = 360.0;
    cue.deterministicSeed = cs.seed;
    cue.visualPreset = "intro_raster";
    cue.fallbackVisual = "intro_raster";
    cue.preferredEngine = "Raster";
    cue.audioSection = "silent_safe";
    cs.scenes.push_back(cue);
    return cs;
}

void applyBenchmarkResultsToOrchestrator(
    const boo::BenchmarkSuiteReport& report,
    boo::FallbackOrchestrator& orchestrator) {
    for (const auto& e : report.entries) {
        if (e.isFallbackComparison || !e.executed || !e.outputVerified || e.status != boo::BackendStatus::Accelerated) continue;
        boo::EngineKind kind = boo::EngineKind::RT;
        if (e.category == boo::BenchCategory::RT)     kind = boo::EngineKind::RT;
        if (e.category == boo::BenchCategory::CUDA)   kind = boo::EngineKind::CUDA;
        if (e.category == boo::BenchCategory::Tensor) kind = boo::EngineKind::Tensor;
        if (e.category == boo::BenchCategory::NVENC)  kind = boo::EngineKind::Codec;

        // Never overwrite live renderer-owned engine state (e.g. M1 per-frame verified DXR or safe fallback)
        if (orchestrator.activeEffectOwnsEngineStatus(kind)) {
            continue;
        }

        boo::HardwareResult updated = orchestrator.engineStatus(kind);
        updated.status = boo::BackendStatus::Accelerated;
        updated.health = boo::HealthState::ActiveAccelerated;
        updated.activeBackend = e.backendName;
        updated.successfullyExecutedTestWork = true;
        updated.diagnosticCode = "BENCHMARK_VERIFIED";
        char summary[160]{};
        std::snprintf(summary, sizeof(summary), "Verified hardware execution: %.2f %s (p50 %.3f ms)",
                      e.primaryScore, e.primaryUnit.c_str(), e.timingMs.p50Ms);
        updated.humanReadableReason = summary;
        orchestrator.updateEngineStatus(updated);
    }
}

boo::M1TaipeiSliceEffect* createAndRegisterShowEffects(
    boo::D3D12Context& d3d,
    const boo::AppSettings& settings,
    const boo::CueSheet& cueSheet,
    const boo::QualityPresetSpec& initialSpec,
    boo::FallbackOrchestrator& orchestrator) {
    bool anyM1 = false;
    for (const auto& s : cueSheet.scenes) {
        if (s.visualPreset.rfind("m1_", 0) == 0 || s.visualPreset.rfind("m2_", 0) == 0) {
            anyM1 = true;
            break;
        }
    }
    const bool allM1 = boo::cueUsesM1Visuals(cueSheet);
    if (anyM1 && !allM1) {
        BOO_LOG_WARN("Cues", "Mixed M0/M1 visual presets detected in cue sheet; routing safely to M0 visual renderer.");
    }

    const boo::ShowVisualMode showMode = boo::resolveShowVisualMode(cueSheet, settings.playbackMode);
    auto fallbackVisual = std::make_unique<boo::M0TestVisualEffect>(d3d, true, false);
    if (!settings.legacyM0Visual && showMode != boo::ShowVisualMode::LegacyM0) {
        auto primaryM1 = std::make_unique<boo::M1TaipeiSliceEffect>(
            d3d,
            initialSpec,
            settings.disableRT,
            settings.injectShaderFail,
            settings.disableCUDA,
            settings.disableTensor,
            settings.disableNVENC,
            settings.enableSignalHijack,
            showMode == boo::ShowVisualMode::M2Trilogy180s,
            settings.injectMeshFail,
            showMode);
        auto* rawM1 = primaryM1.get();
        orchestrator.setPrimaryAndFallbackEffects(std::move(primaryM1), std::move(fallbackVisual));
        return rawM1;
    } else {
        auto primaryM0 = std::make_unique<boo::M0TestVisualEffect>(d3d, false, settings.injectShaderFail);
        orchestrator.setPrimaryAndFallbackEffects(std::move(primaryM0), std::move(fallbackVisual));
        return nullptr;
    }
}

} // namespace

int main(int argc, char** argv) {
    using namespace boo;

    std::vector<std::string> rawArgs;
    for (int i = 1; i < argc; ++i) {
        rawArgs.emplace_back(argv[i]);
    }

    // Pre-scan for custom --config before loading defaults
    AppSettings settings{};
    for (size_t i = 0; i + 1 < rawArgs.size(); ++i) {
        if (rawArgs[i] == "--config") {
            settings.configPath = rawArgs[i + 1];
        }
    }

    const auto resolvedConfig = resolveResourcePath(settings.configPath);
    std::string cfgErr;
    if (!loadSettingsFromToml(resolvedConfig, settings, &cfgErr)) {
        // Non-fatal: built-in defaults remain active
    }

    std::string cliErr;
    if (!parseCommandLineArgs(rawArgs, settings, &cliErr)) {
        std::cerr << "CLI Error: " << cliErr << "\n\n" << buildCliHelpText();
        return 2;
    }
    if (settings.showHelp) {
        std::cout << buildCliHelpText();
        return 0;
    }

    if (settings.validateCuesOnly) {
        AssetManifest manifest{};
        ValidationReport manifestRep{};
        const auto resolvedManifest = resolveResourcePath(settings.manifestPath);
        if (!loadAssetManifestFromFile(resolvedManifest, manifest, manifestRep)) {
            std::cerr << "[FAIL] Asset manifest validation failed (" << resolvedManifest.string() << "):\n";
            for (const auto& iss : manifestRep.issues) {
                std::cerr << "  - [" << iss.code << "] " << iss.message << "\n";
            }
            return 1;
        }
        const auto repoRoot = resolvedManifest.parent_path().parent_path();
        const std::vector<std::filesystem::path> defaultCues = {
            "assets/cues/film_360s.json",
            "assets/cues/m2_trilogy_180s.json",
            "assets/cues/m1_taipei_60s.json",
            "assets/cues/solo_rt.json",
            "assets/cues/solo_cuda.json",
            "assets/cues/solo_tensor.json",
            "assets/cues/solo_npu.json"
        };
        bool allValid = true;
        for (const auto& cf : defaultCues) {
            const auto resolvedCue = resolveResourcePath(cf);
            CueSheet sheet{};
            ValidationReport rep{};
            loadCueSheetFromFile(resolvedCue, sheet, rep);
            if (rep.valid) {
                validateCueSheetAgainstManifest(sheet, manifest, repoRoot, rep);
            }
            if (rep.valid) {
                std::cout << "[PASS] " << resolvedCue.filename().string()
                          << " | film_id=" << sheet.filmId
                          << " | scenes=" << sheet.scenes.size()
                          << " | samples=" << sheet.durationSamples << "\n";
            } else {
                allValid = false;
                std::cerr << "[FAIL] " << resolvedCue.string() << ":\n";
                for (const auto& iss : rep.issues) {
                    std::cerr << "  - [" << iss.code << "] " << iss.message << "\n";
                }
            }
        }
        return allValid ? 0 : 1;
    }

    Logger::instance().setMinLevel(settings.logLevel);
    if (!settings.logFilePath.empty()) {
        std::string logErr;
        if (!Logger::instance().openLogFile(settings.logFilePath, &logErr)) {
            BOO_LOG_WARN("Startup", logErr);
        }
    }

    BOO_LOG_INFO("Startup", "BOO SAH SAH — The Glass Orchid Protocol");

    // 1. Load quality presets + validate cue sheet & asset manifest
    const auto presets = loadQualityPresets(resolveResourcePath(settings.presetsPath));
    CueSheet cueSheet{};
    ValidationReport cueReport{};
    const auto resolvedCue = resolveResourcePath(settings.cuePath);
    if (!loadCueSheetFromFile(resolvedCue, cueSheet, cueReport)) {
        for (const auto& iss : cueReport.issues) {
            BOO_LOG_WARN("Cues", "[" + iss.code + "] " + iss.message);
        }
        if (settings.headlessProbeOnly) {
            BOO_LOG_ERROR("Cues", "Headless probe failed cue validation.");
            return 1;
        }
        BOO_LOG_WARN("Cues", "Activating built-in emergency safe cue schedule (Section 8.2).");
        cueSheet = makeEmergencyFallbackCueSheet();
    } else {
        BOO_LOG_INFO("Cues", "Loaded cue sheet '" + cueSheet.filmId + "' (" +
                             std::to_string(cueSheet.scenes.size()) + " scenes, " +
                             std::to_string(cueSheet.durationSamples) + " samples @ " +
                             std::to_string(cueSheet.sampleRate) + " Hz).");
    }

    AssetManifest manifest{};
    ValidationReport manifestReport{};
    if (loadAssetManifestFromFile(resolveResourcePath(settings.manifestPath), manifest, manifestReport)) {
        BOO_LOG_INFO("Assets", "Loaded artifact provenance manifest (" +
                               std::to_string(manifest.assets.size()) + " tracked entries).");
    }

    // 2. Probe GPU and optional accelerator capabilities without requiring optional SDKs
    ProbeFlags pflags{};
    pflags.disableRT = settings.disableRT;
    pflags.disableCUDA = settings.disableCUDA;
    pflags.disableTensor = settings.disableTensor;
    pflags.disableNPU = settings.disableNPU;
    pflags.disableNVENC = settings.disableNVENC;
    pflags.enableSignalHijack = settings.enableSignalHijack;
    pflags.forceWarp = settings.forceWarp;

    const SystemCapabilities caps = probeSystemCapabilities(pflags);
    FallbackOrchestrator orchestrator;
    for (size_t i = 0; i < kEngineKindCount; ++i) {
        orchestrator.registerEngineState(caps.engines[i]);
    }

    // CLI-only ad-hoc numerical benchmark mode (--bench)
    if (settings.runBenchCliOnly) {
        D3D12Context headlessD3D;
        std::string d3dErr;
        if (!headlessD3D.initialize(nullptr, 1920, 1080, settings.forceWarp, settings.enableD3DDebug, &d3dErr)) {
            BOO_LOG_ERROR("Bench", "Unable to initialize D3D12 device for benchmark: " + d3dErr);
            return 1;
        }

        BenchmarkConfig bcfg{};
        bcfg.timedIterations = settings.benchIterations;
        bcfg.disableRT = settings.disableRT;
        bcfg.disableCUDA = settings.disableCUDA;
        bcfg.disableTensor = settings.disableTensor;
        bcfg.disableNVENC = settings.disableNVENC;
        bcfg.includeFallbacks = true;

        const auto report = runAdHocBenchmarks(
            headlessD3D.deviceRaw(),
            headlessD3D.commandQueueRaw(),
            headlessD3D.adapterLuidLow(),
            headlessD3D.adapterLuidHigh(),
            headlessD3D.adapterName(),
            caps.cpuBrand,
            bcfg,
            [](const BenchmarkProgressEvent& ev) {
                const int pct = static_cast<int>(ev.overallProgress01 * 100.0f);
                char line[160]{};
                std::snprintf(line, sizeof(line), "\r  [%3d%%] %-7s | %-52.52s | %8.2f %-8s",
                              pct, ev.phaseLabel.c_str(), ev.activeStageTitle.c_str(),
                              ev.liveMetricValue, ev.liveMetricUnit.c_str());
                std::cout << line << std::flush;
            });
        std::cout << "\r" << std::string(100, ' ') << "\r";

        printCreativeBenchmarkDashboard(report, true);
        applyBenchmarkResultsToOrchestrator(report, orchestrator);

        if (!settings.exportBenchJsonPath.empty()) {
            exportBenchmarkJson(settings.exportBenchJsonPath, report);
        }
        if (!settings.exportBenchCsvPath.empty()) {
            exportBenchmarkCsv(settings.exportBenchCsvPath, report);
        }
        headlessD3D.shutdown();
        return 0;
    }

    // Headless capability probe mode (--headless-probe)
    if (settings.headlessProbeOnly) {
        std::cout << formatHardwareOrchestraTable(
            caps, orchestrator, ClockSource::SilentMonotonicClock, "Headless Probe (Audio skipped)");
        if (!settings.exportDiagnosticsPath.empty()) {
            RuntimeDiagnosticsSummary diag{};
            diag.settings = settings;
            diag.capabilities = caps;
            diag.finalQualityPreset = settings.qualityPreset;
            exportDiagnosticsJson(settings.exportDiagnosticsPath, diag, orchestrator);
        }
        return caps.d3d12Available ? 0 : 1;
    }

    if (!caps.d3d12Available) {
        const std::string msg =
            "BOO SAH SAH requires a Direct3D 12 compatible graphics device on Windows 11.\n"
            "No usable D3D12 adapter was detected.";
        BOO_LOG_ERROR("D3D12", msg);
        if (!settings.noActivateWindow && settings.autoExitSeconds <= 0.0) {
            MessageBoxW(nullptr, L"BOO SAH SAH requires a compatible Direct3D 12 device.\nSee logs for Hardware Orchestra diagnostics.",
                        L"BOO SAH SAH — Unsupported Graphics Device", MB_ICONERROR | MB_OK);
        }
        return 1;
    }

    // 3. Initialize 48 kHz WASAPI Audio Engine or silent monotonic clock fallback
    AudioEngine audio;
    audio.setMasterVolume(settings.masterVolume);
    audio.setMuted(settings.mute);
    audio.setEdition(settings.edition);
    bool audioClockReady = false;
    if (!settings.forceSilentClock) {
        audio.setMasterVolume(settings.masterVolume);
        audio.setMuted(settings.mute);
        audio.setEdition(settings.edition);
        audio.setDurationSamples(cueSheet.durationSamples);
        audioClockReady = audio.initialize(settings.injectAudioFail);
        if (!audioClockReady) {
            BOO_LOG_WARN("Audio", audio.fallbackReason() + " -> Switching to monotonic silence clock.");
        } else {
            BOO_LOG_INFO("Audio", "48 kHz stereo float audio active on '" + audio.endpointDescription() + "'.");
        }
    } else {
        BOO_LOG_INFO("Audio", "Using monotonic silence-fallback clock (--force-silent-clock).");
    }

    TimelineClock clock;
    clock.configure(cueSheet.sampleRate, cueSheet.durationSamples);
    if (!settings.forceSilentClock) {
        clock.setAudioSampleReader([&audio](int64_t& outSamples) {
            return audio.readPlayedSamples48k(outSamples);
        });
    }

    SoloLoopRange modeRange{};
    const bool isSoloOrLoopMode = (settings.playbackMode != PlaybackMode::FullFilm);
    resolvePlaybackLoopRange(cueSheet, settings.playbackMode, modeRange);
    if (isSoloOrLoopMode) {
        clock.setLoopRange(modeRange.startSample, modeRange.endSample);
        if (audioClockReady) {
            audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, modeRange.startSample)));
        }
    }

    // 3b. Launch non-blocking background OpenVINO NPU/CPU neural stage-director while D3D12 initializes
    QualityLadder qualityLadder(settings.qualityPreset, 60);
    const QualityPresetSpec startupSpec = findQualityPresetSpec(presets, qualityLadder.currentPreset());
    AsyncDirectorController asyncDirector;
    asyncDirector.start(
        cueSheet.seed,
        resolveResourcePath("assets/models/madame_n_director.xml"),
        settings.disableNPU || !startupSpec.allowNpu,
        settings.injectNpuTimeout,
        !settings.forceWarp);

    // 4. Create Win32 Window + Triple-Buffered D3D12 SwapChain
    AppWindow window;
    if (!window.create(settings.title, settings.width, settings.height, settings.fullscreen, settings.noActivateWindow)) {
        BOO_LOG_ERROR("Window", "Failed to create Win32 application window.");
        asyncDirector.shutdown();
        audio.shutdown();
        return 1;
    }

    D3D12Context d3d;
    std::string d3dInitErr;
    if (!d3d.initialize(window.hwnd(), window.clientWidth(), window.clientHeight(),
                        settings.forceWarp, settings.enableD3DDebug, &d3dInitErr)) {
        BOO_LOG_ERROR("D3D12", "D3D12 initialization failed: " + d3dInitErr);
        asyncDirector.shutdown();
        window.destroy();
        audio.shutdown();
        return 1;
    }

    // 5. Initialize cue-routed primary + embedded-safe-fallback visual effects
    M1TaipeiSliceEffect* liveM1Ptr = createAndRegisterShowEffects(d3d, settings, cueSheet, startupSpec, orchestrator);

    const HardwareResult visualInit = orchestrator.initializeEffects(false);
    BOO_LOG_INFO("Render", std::string("Active visual backend: ") + orchestrator.activeEffectName() +
                           " [" + visualInit.diagnosticCode + "]");

    SpoutSender spoutSender;
    if (settings.enableSpout) {
        std::string spoutReason;
        const uint32_t spW = (liveM1Ptr && liveM1Ptr->canvasWidth() > 0) ? liveM1Ptr->canvasWidth() : startupSpec.renderWidth;
        const uint32_t spH = (liveM1Ptr && liveM1Ptr->canvasHeight() > 0) ? liveM1Ptr->canvasHeight() : startupSpec.renderHeight;
        if (spoutSender.initialize(d3d, settings.spoutSenderName, spW, spH, &spoutReason)) {
            BOO_LOG_INFO("Spout", spoutReason);
        } else {
            BOO_LOG_WARN("Spout", "Spout2 sender unavailable (continuing playback cleanly): " + spoutReason);
        }
    }

    // Opt-in Signal Hijack media / webcam ingest (SPEC_v4 Sections 5.1-5.3 & 11: strictly idle unless opted in)
    MediaIngestSource mediaIngest;
    std::vector<uint8_t> latestMediaNv12;
    if (settings.enableCameraInput) {
        if (mediaIngest.openLiveCamera(settings.injectMediaFail)) {
            settings.enableSignalHijack = true;
            orchestrator.setCodecSignalHijackActive(true);
            BOO_LOG_INFO("Media", mediaIngest.statusReason());
        } else {
            BOO_LOG_WARN("Media", mediaIngest.statusReason());
        }
    } else if (!settings.mediaInputPath.empty() || settings.injectMediaFail) {
        if (mediaIngest.openLocalFile(settings.mediaInputPath, settings.injectMediaFail)) {
            settings.enableSignalHijack = true;
            orchestrator.setCodecSignalHijackActive(true);
            BOO_LOG_INFO("Media", mediaIngest.statusReason());
        } else {
            BOO_LOG_WARN("Media", mediaIngest.statusReason());
        }
    }

    // Non-stalling bounded sync so parallel NPU model verification appears in startup Hardware Orchestra table when fast
    (void)asyncDirector.waitForInitialCompile(250);
    orchestrator.updateEngineStatus(asyncDirector.telemetry().hardwareResult);

    std::cout << formatHardwareOrchestraTable(
        caps,
        orchestrator,
        audioClockReady ? ClockSource::AudioSampleClock : ClockSource::SilentMonotonicClock,
        audioClockReady ? audio.endpointDescription() : (audio.fallbackReason().empty() ? "Silent Monotonic QPC" : audio.fallbackReason()));

    // Background live numerical benchmark worker state (non-blocking UI/render!)
    std::mutex benchMutex;
    std::atomic<bool> benchRunning{false};
    std::atomic<bool> benchCancel{false};
    std::atomic<bool> benchCompleted{false};
    float benchOverallProg = 0.0f;
    std::array<float, 4> liveBenchPrimary{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> liveBenchFallback{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> liveBenchFill{0.0f, 0.0f, 0.0f, 0.0f};
    BenchmarkSuiteReport latestBenchReport{};
    std::thread benchThread;

    auto startBackgroundBenchSweep = [&]() {
        if (benchRunning.load(std::memory_order_acquire)) return;
        if (benchThread.joinable()) benchThread.join();
        benchRunning.store(true, std::memory_order_release);
        benchCompleted.store(false, std::memory_order_release);
        benchCancel.store(false, std::memory_order_release);
        const AppSettings benchSettings = settings;
        Microsoft::WRL::ComPtr<ID3D12Device> benchDevice = static_cast<ID3D12Device*>(d3d.deviceRaw());
        const auto benchLuidLow = d3d.adapterLuidLow();
        const auto benchLuidHigh = d3d.adapterLuidHigh();
        const auto benchAdapter = d3d.adapterName();
        benchThread = std::thread([&, benchSettings, benchDevice, benchLuidLow, benchLuidHigh, benchAdapter]() {
          try {
            auto* dev = benchDevice.Get();
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> asyncQueue;
            D3D12_COMMAND_QUEUE_DESC qd{};
            qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            if (!dev || FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&asyncQueue)))) {
                benchRunning.store(false, std::memory_order_release);
                return;
            }

            BenchmarkConfig bcfg{};
            bcfg.timedIterations = benchSettings.benchIterations;
            bcfg.disableRT = benchSettings.disableRT;
            bcfg.disableCUDA = benchSettings.disableCUDA;
            bcfg.disableTensor = benchSettings.disableTensor;
            bcfg.disableNVENC = benchSettings.disableNVENC;
            bcfg.includeFallbacks = true;
            bcfg.cancelRequested = &benchCancel;

            BenchmarkSuiteReport rep = runAdHocBenchmarks(
                dev,
                asyncQueue.Get(),
                benchLuidLow,
                benchLuidHigh,
                benchAdapter,
                caps.cpuBrand,
                bcfg,
                [&](const BenchmarkProgressEvent& ev) {
                    std::lock_guard<std::mutex> lk(benchMutex);
                    benchOverallProg = ev.overallProgress01;
                    size_t slot = 0;
                    double ref = 25000.0;
                    if (ev.activeStageTitle.find("CUDA") != std::string::npos || ev.activeStageTitle.find("Particle") != std::string::npos) { slot = 1; ref = 50000.0; }
                    else if (ev.activeStageTitle.find("Tensor") != std::string::npos || ev.activeStageTitle.find("Neural") != std::string::npos) { slot = 2; ref = 120.0; }
                    else if (ev.activeStageTitle.find("Encoder") != std::string::npos || ev.activeStageTitle.find("SIGNAL") != std::string::npos) { slot = 3; ref = 600.0; }
                    if (ev.activeStageTitle.find("NOT ") == std::string::npos) {
                        liveBenchPrimary[slot] = static_cast<float>(ev.liveMetricValue);
                        liveBenchFill[slot] = static_cast<float>(std::clamp(ev.liveMetricValue / ref, 0.0, 1.0));
                    }
                });

            {
                std::lock_guard<std::mutex> lk(benchMutex);
                for (const auto& e : rep.entries) {
                    const size_t s = static_cast<size_t>(e.category);
                    if (s < 4) {
                        if (!e.isFallbackComparison && e.outputVerified) {
                            liveBenchPrimary[s] = static_cast<float>(e.primaryScore);
                            liveBenchFill[s] = static_cast<float>(std::clamp(e.primaryScore / e.referenceTarget, 0.0, 1.0));
                        } else if (e.isFallbackComparison && e.outputVerified) {
                            liveBenchFallback[s] = static_cast<float>(e.primaryScore);
                            if (liveBenchPrimary[s] <= 0.0f) {
                                liveBenchPrimary[s] = static_cast<float>(e.primaryScore);
                                liveBenchFill[s] = static_cast<float>(std::clamp(e.primaryScore / e.referenceTarget, 0.0, 1.0));
                            }
                        }
                    }
                }
                benchOverallProg = 1.0f;
                latestBenchReport = rep;
            }
            printCreativeBenchmarkDashboard(rep, true);
            benchCompleted.store(!benchCancel.load(std::memory_order_acquire), std::memory_order_release);
            } catch (const std::exception& e) {
            BOO_LOG_ERROR("Bench", std::string("Worker failed: ") + e.what());
          } catch (...) {
            BOO_LOG_ERROR("Bench", "Worker failed with unknown exception.");
          }
          benchRunning.store(false, std::memory_order_release);
        });
    };

    // 6. Start master timeline clock & main render loop
    clock.start(audioClockReady, settings.loop || isSoloOrLoopMode);
    if(settings.startSeconds>0) { clock.seekToSeconds(settings.startSeconds); if(audioClockReady)audio.resetPhase(static_cast<uint64_t>(clock.currentSample())); }
    const int64_t initialSample = clock.currentSample();
    size_t lastActIdx = SIZE_MAX;
    orchestrator.onSceneBoundary(0, clock.currentSeconds(), false);

    if (settings.runBenchOnStart) {
        startBackgroundBenchSweep();
    }

    const auto wallStart = std::chrono::steady_clock::now();
    auto lastTitleUpdate = wallStart;
    std::vector<double> cpuFrameSamplesMs;
    std::vector<double> gpuFrameSamplesMs;
    cpuFrameSamplesMs.reserve(4096);
    gpuFrameSamplesMs.reserve(4096);

    uint64_t peakVramBytes = 0;
    uint64_t frameCounter = 0;
    double smoothFps = 60.0;
    double lastGpuMs = 0.25;
    bool appliedBenchBadges = false;
    bool running = true;
    bool recoveryAttempted = false;
    bool graphicsUnavailable = false;
    bool deviceLostInjected = false;
    uint64_t retiredPresentCount = 0;
    uint64_t retiredPresentStalls = 0;
    uint32_t deviceRecoverySuccesses = 0;
    uint32_t retiredDebugErrors = 0;
    uint32_t retiredDebugWarnings = 0;
    bool debugLayerWasActive = d3d.debugLayerActive();
    bool audioEndpointFailed = false;
    uint32_t audioEndpointMigrations = 0;
    int runtimeExitCode = 0;
    bool captureRequested = false;
    auto volumeVisibleUntil = wallStart;
    std::array<float, 16> recentGpuMs{};
    uint32_t hudMode = settings.showOverlay ? 0u : 3u;
    AbCompareEngine abCompareEngine = AbCompareEngine::RT;

    // One recovery policy covers loss during resize, fence waits, resource sync,
    // and Present. The 48 kHz transport is deliberately never stopped or rebased.
    auto recoverGraphics = [&](const std::string& cause) {
        const bool mayRecover = !recoveryAttempted;
        recoveryAttempted = true;
        BOO_LOG_WARN("D3D12", "Graphics lifecycle failure: " + cause);
        benchCancel.store(true, std::memory_order_release);
        if (benchThread.joinable()) benchThread.join();
        benchCompleted.store(false, std::memory_order_release);
        retiredPresentCount += d3d.presentCount();
        retiredPresentStalls += d3d.presentStallCount();
        retiredDebugErrors += d3d.debugErrorCount();
        retiredDebugWarnings += d3d.debugWarningCount();
        // Retire resources before release on non-removal failures as well.
        d3d.retireGpuForShutdown();
        spoutSender.shutdown();
        liveM1Ptr = nullptr;
        orchestrator.shutdown();
        d3d.shutdown();
        std::string recoveryError = mayRecover ? std::string{} : "The single graphics recovery attempt was already used.";
        if (mayRecover) {
            BOO_LOG_WARN("D3D12", "Attempting single D3D12 device recovery with bounded fence waits...");
            const auto spec = findQualityPresetSpec(presets, qualityLadder.currentPreset());
            if (d3d.initialize(window.hwnd(), window.clientWidth(), window.clientHeight(),
                               settings.forceWarp, settings.enableD3DDebug, &recoveryError)) {
                liveM1Ptr = createAndRegisterShowEffects(d3d, settings, cueSheet, spec, orchestrator);
                const auto result = orchestrator.initializeEffects(false);
                if (result.status != BackendStatus::Faulted && result.status != BackendStatus::Unavailable &&
                    d3d.checkDeviceHealthy(&recoveryError)) {
                    orchestrator.setAbCompareEngine(abCompareEngine);
                    orchestrator.setCodecSignalHijackActive(settings.enableSignalHijack);
                    orchestrator.resetTo(clock.currentSeconds());
                    lastActIdx = SIZE_MAX;
                    if (settings.enableSpout && liveM1Ptr && !orchestrator.isUsingFallbackEffect()) {
                        spoutSender.initialize(d3d, settings.spoutSenderName, liveM1Ptr->canvasWidth(), liveM1Ptr->canvasHeight());
                    }
                    BOO_LOG_INFO("D3D12", "Device recovery completed; sample timeline preserved.");
                    ++deviceRecoverySuccesses;
                    debugLayerWasActive = debugLayerWasActive && d3d.debugLayerActive();
                    return;
                }
                if (recoveryError.empty()) recoveryError = "Neither primary nor safe graphics could initialize.";
            }
        }
        d3d.retireGpuForShutdown();
        liveM1Ptr = nullptr;
        orchestrator.shutdown();
        d3d.shutdown();
        graphicsUnavailable = true;
        runtimeExitCode = 1;
        BOO_LOG_ERROR("D3D12", "Graphics recovery unsuccessful: " + recoveryError);
        window.showGraphicsFailure(recoveryError);
    };

    while (running && window.pumpMessages()) {
        const auto cpuFrameT0 = std::chrono::steady_clock::now();
        bool manualPresetChanged = false;

        TransportCommand cmd = TransportCommand::None;
        while (window.popCommand(cmd)) {
            switch (cmd) {
                case TransportCommand::StepBack:
                case TransportCommand::StepForward:
                case TransportCommand::SeekBack:
                case TransportCommand::SeekForward:
                case TransportCommand::SeekBackLong:
                case TransportCommand::SeekForwardLong:
                case TransportCommand::RevealPreroll: {
                    if (cmd == TransportCommand::StepBack || cmd == TransportCommand::StepForward) {
                        if (clock.state() != TransportState::Paused) break;
                        clock.stepFrame(cmd == TransportCommand::StepBack ? -1 : 1);
                    } else if (cmd == TransportCommand::RevealPreroll) {
                        const double prerollSec =
                            (cueSheet.durationSamples >= 17280000) ? 338.0 :
                            (cueSheet.durationSamples == 8640000)  ? 163.0 :
                            (cueSheet.durationSamples == 2160000)  ? 21.0  : 43.0;
                        clock.seekToSeconds(prerollSec);
                    }
                    else clock.seekRelativeSeconds(cmd == TransportCommand::SeekBack ? -2 : cmd == TransportCommand::SeekForward ? 2 : cmd == TransportCommand::SeekBackLong ? -10 : 10);
                    if (audioClockReady) {
                        audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                        audio.setPaused(clock.state() == TransportState::Paused || clock.state() == TransportState::Ended);
                    }
                    orchestrator.resetTo(clock.currentSeconds());
                    break;
                }
                case TransportCommand::VolumeDown:
                case TransportCommand::VolumeUp:
                    settings.masterVolume = std::clamp(settings.masterVolume + (cmd == TransportCommand::VolumeUp ? 0.05f : -0.05f), 0.0f, 1.0f);
                    audio.setMasterVolume(settings.masterVolume);
                    volumeVisibleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    BOO_LOG_INFO("Audio", "Volume: " + std::to_string(static_cast<int>(settings.masterVolume * 100)) + "%");
                    break;
                case TransportCommand::CaptureClean:
                    captureRequested = true;
                    break;
                case TransportCommand::Quit:
                    running = false;
                    break;
                case TransportCommand::TogglePlayPause:
                    clock.togglePause();
                    audio.setPaused(clock.state() == TransportState::Paused || clock.state() == TransportState::Ended);
                    break;
                case TransportCommand::Restart: {
                    const int64_t rstSample = (settings.playbackMode != PlaybackMode::FullFilm) ? modeRange.startSample : 0;
                    clock.seekToSample(rstSample);
                    if (audioClockReady) {
                        audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                        audio.setPaused(clock.state() == TransportState::Paused || clock.state() == TransportState::Ended);
                    }
                    orchestrator.resetTo(clock.currentSeconds());
                    break;
                }
                case TransportCommand::NextAct: {
                    const size_t cur = findSceneIndexAtSample(cueSheet, clock.currentSample());
                    const size_t nxt = (cur + 1) % cueSheet.scenes.size();
                    clock.seekToSample(cueSheet.scenes[nxt].startSample);
                    if (audioClockReady) {
                        audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                        audio.setPaused(clock.state() == TransportState::Paused || clock.state() == TransportState::Ended);
                    }
                    break;
                }
                case TransportCommand::PrevAct: {
                    const size_t cur = findSceneIndexAtSample(cueSheet, clock.currentSample());
                    const size_t prv = (cur > 0) ? (cur - 1) : 0;
                    clock.seekToSample(cueSheet.scenes[prv].startSample);
                    if (audioClockReady) {
                        audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                        audio.setPaused(clock.state() == TransportState::Paused || clock.state() == TransportState::Ended);
                    }
                    break;
                }
                case TransportCommand::ToggleLoop:
                    settings.loop = !clock.loopEnabled();
                    clock.setLoopEnabled(settings.loop);
                    break;
                case TransportCommand::ToggleMute:
                    settings.mute = !audio.isMuted();
                    audio.setMuted(settings.mute);
                    break;
                case TransportCommand::CycleQualityPreset: {
                    const uint8_t nextP = (static_cast<uint8_t>(qualityLadder.currentPreset()) + 1u) % 4u;
                    qualityLadder.setPreset(static_cast<QualityPresetId>(nextP));
                    manualPresetChanged = true;
                    break;
                }
                case TransportCommand::ToggleEdition:
                    settings.edition = (settings.edition == Edition::Family) ? Edition::Midnight : Edition::Family;
                    audio.setEdition(settings.edition);
                    BOO_LOG_INFO("Show", std::string("Story edition [E]: ") + toString(settings.edition));
                    break;
                case TransportCommand::ToggleCaptions:
                    settings.showCaptions = !settings.showCaptions;
                    BOO_LOG_INFO("Show", std::string("Captions [K]: ") + (settings.showCaptions ? "ON" : "OFF"));
                    break;
                case TransportCommand::ToggleOverlay:
                    hudMode = (hudMode + 1u) % 4u;
                    settings.showOverlay = hudMode != 3u;
                    break;
                case TransportCommand::TriggerBenchSweep:
                    appliedBenchBadges = false;
                    startBackgroundBenchSweep();
                    break;
                case TransportCommand::ToggleRtRuntime:
                    settings.disableRT = !settings.disableRT;
                    orchestrator.setDisableRt(settings.disableRT);
                    BOO_LOG_INFO("Render", std::string("Runtime RT pillar [X]: ") + (settings.disableRT ? "RASTER SUBSTITUTE" : "LIVE DXR ENABLED"));
                    break;
                case TransportCommand::ToggleCudaRuntime:
                    settings.disableCUDA = !settings.disableCUDA;
                    orchestrator.setDisableCuda(settings.disableCUDA);
                    BOO_LOG_INFO("Render", std::string("Runtime CUDA pillar [C]: ") + (settings.disableCUDA ? "8.2K HLSL SUBSTITUTE" : "65.5K LIVE CUDA VORTEX"));
                    break;
                case TransportCommand::ToggleTensorRuntime:
                    settings.disableTensor = !settings.disableTensor;
                    orchestrator.setDisableTensor(settings.disableTensor);
                    BOO_LOG_INFO("Render", std::string("Runtime Tensor pillar [T]: ") + (settings.disableTensor ? "SCALAR HLSL SUBSTITUTE" : "LIVE MMA SIREN CAUSTICS"));
                    break;
                case TransportCommand::ToggleNpuRuntime:
                    settings.disableNPU = !settings.disableNPU;
                    asyncDirector.setDisableNpu(settings.disableNPU || !findQualityPresetSpec(presets, qualityLadder.currentPreset()).allowNpu);
                    BOO_LOG_INFO("NPU", std::string("Runtime NPU director [U]: ") + (settings.disableNPU ? "SCRIPTED DIRECTOR FALLBACK" : "OPENVINO NEURAL DIRECTOR ENABLED"));
                    break;
                case TransportCommand::ToggleCodecRuntime:
                    if (hudMode == 2u && abCompareEngine == AbCompareEngine::Codec) {
                        settings.disableNVENC = !settings.disableNVENC;
                        orchestrator.setDisableCodec(settings.disableNVENC);
                        BOO_LOG_INFO("Render", std::string("Runtime Codec A/B [V]: ") + (settings.disableNVENC ? "SHADER QUANT SUB" : "LIVE HW H.264 ROUND-TRIP"));
                    } else if (!settings.enableSignalHijack) {
                        settings.enableSignalHijack = true;
                        settings.disableNVENC = false;
                        orchestrator.setDisableCodec(false);
                        orchestrator.setCodecSignalHijackActive(true);
                        BOO_LOG_INFO("Render", "Runtime Codec pillar [V]: LIVE HW H.264 ENCODE+DECODE STREAM");
                    } else if (!settings.disableNVENC) {
                        settings.disableNVENC = true;
                        orchestrator.setDisableCodec(true);
                        BOO_LOG_INFO("Render", "Runtime Codec pillar [V]: SHADER QUANTIZATION SUBSTITUTE");
                    } else {
                        settings.enableSignalHijack = false;
                        settings.disableNVENC = false;
                        orchestrator.setDisableCodec(false);
                        orchestrator.setCodecSignalHijackActive(false);
                        BOO_LOG_INFO("Render", "Runtime Codec pillar [V]: OFF (10-BIT CANCASCLEAN BYPASS)");
                    }
                    break;
                case TransportCommand::CycleAbCompareEngine:
                    if (hudMode != 2u) {
                        hudMode = 2u;
                        settings.showOverlay = true;
                    } else {
                        abCompareEngine = static_cast<AbCompareEngine>((static_cast<uint8_t>(abCompareEngine) + 1u) % 4u);
                    }
                    orchestrator.setAbCompareEngine(abCompareEngine);
                    BOO_LOG_INFO("Render", std::string("A/B Split Compare [A]: ") + toString(abCompareEngine));
                    break;
                case TransportCommand::ToggleFullscreen:
                    window.toggleBorderlessFullscreen();
                    break;
                case TransportCommand::ToggleSpoutRuntime:
                    settings.enableSpout = !spoutSender.isActive();
                    if (settings.enableSpout) {
                        std::string spoutReason;
                        const QualityPresetSpec curSpec = findQualityPresetSpec(presets, qualityLadder.currentPreset());
                        const uint32_t spW = (liveM1Ptr && liveM1Ptr->canvasWidth() > 0) ? liveM1Ptr->canvasWidth() : curSpec.renderWidth;
                        const uint32_t spH = (liveM1Ptr && liveM1Ptr->canvasHeight() > 0) ? liveM1Ptr->canvasHeight() : curSpec.renderHeight;
                        if (spoutSender.initialize(d3d, settings.spoutSenderName, spW, spH, &spoutReason)) {
                            BOO_LOG_INFO("Spout", "Runtime Spout2 sender [O]: " + spoutReason);
                        } else {
                            settings.enableSpout = false;
                            BOO_LOG_WARN("Spout", "Runtime Spout2 sender [O] unavailable: " + spoutReason);
                        }
                    } else {
                        d3d.waitForGpu();
                        spoutSender.shutdown();
                        BOO_LOG_INFO("Spout", "Runtime Spout2 sender [O]: OFF");
                    }
                    break;
                case TransportCommand::ToggleCameraRuntime:
                    if (mediaIngest.isLiveCameraActive()) {
                        mediaIngest.shutdown();
                        settings.enableCameraInput = false;
                        if (liveM1Ptr) liveM1Ptr->clearExternalNv12Frame();
                        BOO_LOG_INFO("Media", "Runtime webcam privacy kill-switch [W]: LIVE CAMERA STOPPED");
                    } else {
                        if (mediaIngest.openLiveCamera(settings.injectMediaFail)) {
                            settings.enableCameraInput = true;
                            settings.enableSignalHijack = true;
                            orchestrator.setCodecSignalHijackActive(true);
                            BOO_LOG_INFO("Media", "Runtime webcam [W]: " + mediaIngest.statusReason());
                        } else {
                            BOO_LOG_WARN("Media", "Runtime webcam [W] unavailable: " + mediaIngest.statusReason());
                        }
                    }
                    break;
                case TransportCommand::SelectModeFull:
                case TransportCommand::SelectModeRT:
                case TransportCommand::SelectModeTensor:
                case TransportCommand::SelectModeNPU:
                case TransportCommand::SelectModeCUDA:
                case TransportCommand::SelectModeFinale: {
                    if (cmd == TransportCommand::SelectModeFull) settings.playbackMode = PlaybackMode::FullFilm;
                    if (cmd == TransportCommand::SelectModeRT) settings.playbackMode = PlaybackMode::SoloRT;
                    if (cmd == TransportCommand::SelectModeTensor) settings.playbackMode = PlaybackMode::SoloTensor;
                    if (cmd == TransportCommand::SelectModeNPU) settings.playbackMode = PlaybackMode::SoloNPU;
                    if (cmd == TransportCommand::SelectModeCUDA) settings.playbackMode = PlaybackMode::SoloCUDA;
                    if (cmd == TransportCommand::SelectModeFinale) settings.playbackMode = PlaybackMode::FinaleLoop;

                    if (settings.playbackMode == PlaybackMode::FullFilm) {
                        clock.clearLoopRange();
                        clock.setLoopEnabled(settings.loop);
                    } else if (resolvePlaybackLoopRange(cueSheet, settings.playbackMode, modeRange)) {
                        clock.setLoopRange(modeRange.startSample, modeRange.endSample);
                        clock.setLoopEnabled(true);
                        if (audioClockReady) {
                            audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                        }
                    }
                    break;
                }
                default: break;
            }
        }
        if (!running) break;

        if (!appliedBenchBadges && benchCompleted.load(std::memory_order_acquire) && !benchRunning.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(benchMutex);
            // applyBenchmarkResultsToOrchestrator respects activeEffectOwnsEngineStatus(kind), so M1's live RT/CUDA/Tensor/Codec
            // badges are NEVER overwritten by standalone offscreen `B` benchmark numbers!
            applyBenchmarkResultsToOrchestrator(latestBenchReport, orchestrator);
            appliedBenchBadges = true;
        }

        // Exact [G1.2] pre-frame sequencing: swapchain resize -> clock -> scene boundary -> quality ladder -> canvas sync
        if (!graphicsUnavailable && settings.injectDeviceLost && !deviceLostInjected && d3d.presentCount() >= 2) {
            deviceLostInjected = d3d.injectDeviceLostForTest();
        }
        if (!graphicsUnavailable && !d3d.checkDeviceHealthy(&d3dInitErr)) {
            recoverGraphics(d3dInitErr);
        }
        if (window.consumeResizeFlag() && !graphicsUnavailable && !window.isMinimized()) {
            if (!d3d.resize(window.clientWidth(), window.clientHeight())) {
                recoverGraphics("Swapchain resize failed.");
            }
        }

        if (audioClockReady && !audio.deviceHealthy()) {
            audioEndpointFailed = true;
            audioClockReady = false;
            clock.switchToSilentFallback();
            BOO_LOG_WARN("Audio", "Endpoint invalidated; preserving timeline on SilentMonotonicClock.");
        }
        if (!settings.forceSilentClock && !settings.injectAudioFail &&
            (audio.migrationPending() || !audioClockReady)) {
            if (audio.pollEndpointMigration(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())))) {
                ++audioEndpointMigrations;
                audioClockReady = true;
                audioEndpointFailed = false;
                audio.setPaused(clock.state() == TransportState::Paused || clock.state() == TransportState::Ended);
                clock.switchToAudioClock();
            }
        }

        const ClockSnapshot tick = clock.tickFrame();
        if (audioClockReady) {
            if (tick.loopedThisTick) {
                audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, tick.samplePosition)));
            }
            audio.setPaused(tick.state == TransportState::Paused || tick.state == TransportState::Ended);
        }
        if (tick.unclampedDtSeconds > 0.0001) {
            const double instFps = 1.0 / tick.unclampedDtSeconds;
            smoothFps = smoothFps * 0.92 + std::clamp(instFps, 1.0, 1000.0) * 0.08;
        }

        const size_t actIdx = findSceneIndexAtSample(cueSheet, tick.samplePosition);
        const SceneCue& activeCue = cueSheet.scenes[actIdx];
        const bool sceneChanged = (actIdx != lastActIdx);
        if (sceneChanged) {
            lastActIdx = actIdx;
            orchestrator.onSceneBoundary(static_cast<uint32_t>(actIdx), tick.filmTimeSeconds, false);
        }

        const uint64_t appVram = settings.injectVramPressure
                                     ? (15ULL * 1024ULL * 1024ULL * 1024ULL)
                                     : d3d.queryEstimatedAppVramBytes();
        peakVramBytes = std::max(peakVramBytes, appVram);
        const QualityPresetSpec prevSpec = findQualityPresetSpec(presets, qualityLadder.currentPreset());
        qualityLadder.pushFrameSample(lastGpuMs, appVram, prevSpec.softVramLimitBytes, sceneChanged);

        const QualityPresetSpec activeSpec = findQualityPresetSpec(presets, qualityLadder.currentPreset());
        const bool allowImmediateCanvasResize =
            manualPresetChanged || sceneChanged || qualityLadder.memoryPressureActive();
        if (!graphicsUnavailable) {
            orchestrator.syncPresetBeforeFrame(activeSpec, allowImmediateCanvasResize, tick.filmTimeSeconds);
            if (!d3d.checkDeviceHealthy(&d3dInitErr)) recoverGraphics(d3dInitErr);
        }
        asyncDirector.setDisableNpu(settings.disableNPU || !activeSpec.allowNpu);

        const double cueSpan = std::max(1.0, static_cast<double>(activeCue.endSample - activeCue.startSample));
        const float actProg = static_cast<float>(std::clamp(
            static_cast<double>(tick.samplePosition - activeCue.startSample) / cueSpan, 0.0, 1.0));

        const AudioBandSnapshot bands = (audioClockReady && audio.deviceHealthy())
                                            ? audio.currentBandSnapshot()
                                            : computeAnalyticalAudioBands(tick.samplePosition, cueSheet.sampleRate, cueSheet.durationSamples, settings.edition);

        FrameContext fctx{};
        fctx.presentationFrame = frameCounter;
        fctx.samplePosition = tick.samplePosition;
        fctx.sampleRate = cueSheet.sampleRate;
        fctx.filmTimeSeconds = tick.filmTimeSeconds;
        fctx.dtSeconds = tick.dtSeconds;
        fctx.seeking = (tick.state == TransportState::Seeking);
        fctx.paused = (tick.state == TransportState::Paused);
        fctx.randomSeed = activeCue.deterministicSeed;
        fctx.actIndex = static_cast<uint32_t>(actIdx);
        fctx.actProgress = actProg;
        fctx.audioEnvelope = bands.rms;
        fctx.edition = settings.edition;
        fctx.qualityPreset = qualityLadder.currentPreset();
        fctx.showOverlay = settings.showOverlay;
        fctx.showCaptions = settings.showCaptions;
        fctx.reducedFlash = settings.reducedFlash;
        fctx.motionScale = settings.motionScale;
        fctx.hudMode = hudMode;

        // M1/M2 clean-feed modulation must not inherit audio buffering or presentation history,
        // while M5 (SoloNPU) and M6 (FullFilm360s) consume live 5 Hz OpenVINO NPU/CPU stage direction.
        ScriptedDirector sampleDirector;
        sampleDirector.reset(cueSheet.seed);
        FrameContext deterministicCtx = fctx;
        deterministicCtx.audioEnvelope = computeAnalyticalAudioBands(tick.samplePosition, cueSheet.sampleRate, cueSheet.durationSamples, settings.edition).rms;
        const DirectorMoodState asyncMood = asyncDirector.evaluateFrame(fctx, &activeCue);
        const AsyncDirectorTelemetry npuTel = asyncDirector.telemetry();
        orchestrator.updateEngineStatus(npuTel.hardwareResult);
        const DirectorMoodState mood = cueUsesM1Visuals(cueSheet)
            ? sampleDirector.evaluate(deterministicCtx, &activeCue)
            : asyncMood;
        fctx.moodIrisAngle = mood.irisAngle;
        fctx.moodBloomStrength = mood.bloomStrength;
        fctx.moodCutAccent = mood.cutAccent;
        fctx.moodIntensity = mood.lightTension;
        fctx.moodPaletteAccent = mood.spectralShift;

        if (captureRequested && !graphicsUnavailable) {
            captureRequested = false;
            std::string captureError;
            if (!orchestrator.captureClean(fctx, "diagnostics/captures", &captureError))
                BOO_LOG_WARN("Capture", captureError);
        }
        if (!window.isMinimized() && !graphicsUnavailable) {
            HudTelemetrySnapshot hud{};
            hud.filmTimeSeconds = tick.filmTimeSeconds;
            hud.durationSeconds = TimelineClock::samplesToSeconds(cueSheet.durationSamples, cueSheet.sampleRate);
            hud.actIndex = static_cast<uint32_t>(actIdx);
            hud.totalActs = static_cast<uint32_t>(cueSheet.scenes.size());
            hud.actProgress01 = actProg;
            hud.fps = static_cast<float>(smoothFps);
            hud.gpuFrameMs = static_cast<float>(lastGpuMs);
            hud.recentGpuMs = recentGpuMs;
            hud.volumePercent = std::chrono::steady_clock::now() < volumeVisibleUntil ? settings.masterVolume * 100.0f : -1.0f;
            hud.p95FrameMs = static_cast<float>(qualityLadder.recentStats().p95Ms);
            hud.vramUsedMb = static_cast<float>(static_cast<double>(appVram) / (1024.0 * 1024.0));
            hud.vramBudgetMb = static_cast<float>(static_cast<double>(activeSpec.softVramLimitBytes) / (1024.0 * 1024.0));
            hud.audioClockActive = (tick.source == ClockSource::AudioSampleClock);
            hud.muted = audio.isMuted();
            hud.paused = fctx.paused;
            hud.loopEnabled = clock.loopEnabled();
            hud.preset = qualityLadder.currentPreset();
            hud.edition = settings.edition;
            hud.playbackMode = settings.playbackMode;
            hud.mood = mood;
            hud.audioBands = bands;
            hud.canvasWidth = activeSpec.renderWidth;
            hud.canvasHeight = activeSpec.renderHeight;
            hud.swapchainBits = d3d.swapchainChannelBits();
            hud.abCompareEngine = abCompareEngine;
            hud.codecSignalHijackActive = settings.enableSignalHijack;
            uint64_t mediaFrameSeq = 0;
            if (mediaIngest.isActive() && mediaIngest.copyLatestNv12Frame(latestMediaNv12, mediaFrameSeq)) {
                if (liveM1Ptr) {
                    liveM1Ptr->setExternalNv12Frame(latestMediaNv12.data(), latestMediaNv12.size(), mediaFrameSeq);
                }
            } else if (liveM1Ptr && liveM1Ptr->hasExternalMediaFrame()) {
                liveM1Ptr->clearExternalNv12Frame();
            }
            hud.liveCameraActive = mediaIngest.isLiveCameraActive();
            hud.npuInferMs = npuTel.lastInferMs;
            hud.npuResultAgeMs = npuTel.resultAgeMs;
            hud.npuCompletedInferences = npuTel.completedInferences;
            if (liveM1Ptr && !orchestrator.isUsingFallbackEffect()) {
                for (size_t p = 0; p < 4; ++p) {
                    hud.livePillars[p] = liveM1Ptr->livePillarTelemetry(p);
                }
            }
            for (size_t i = 0; i < kEngineKindCount; ++i) {
                hud.engineStatus[i] = orchestrator.engineStatus(static_cast<EngineKind>(i)).status;
            }
            {
                std::lock_guard<std::mutex> lk(benchMutex);
                hud.benchRunning = benchRunning.load(std::memory_order_relaxed);
                hud.benchHasResults = benchCompleted.load(std::memory_order_relaxed);
                hud.benchOverallProgress01 = benchOverallProg;
                hud.benchPrimaryValues = liveBenchPrimary;
                hud.benchFallbackValues = liveBenchFallback;
                hud.benchBarFill01 = liveBenchFill;
            }

            orchestrator.setHudTelemetry(hud);

            if (d3d.beginFrame()) {
                orchestrator.renderFrame(fctx, false);
                if (spoutSender.isActive() && liveM1Ptr && !orchestrator.isUsingFallbackEffect()) {
                    spoutSender.recordCleanPublish(
                        d3d,
                        d3d.commandListRaw(),
                        liveM1Ptr->canvasCleanResourceRaw(),
                        liveM1Ptr->canvasWidth(),
                        liveM1Ptr->canvasHeight());
                }
                double completedGpuMs = 0.0;
                const bool presented = d3d.endFrameAndPresent(settings.vsync, completedGpuMs);
                const bool published = spoutSender.completePublish();
                if (!presented || !published) {
                    recoverGraphics(presented ? "Spout GPU publication did not retire." : "Frame submission or Present failed.");
                } else if (completedGpuMs > 0.0) {
                    lastGpuMs = completedGpuMs;
                    std::move(recentGpuMs.begin() + 1, recentGpuMs.end(), recentGpuMs.begin());
                    recentGpuMs.back() = static_cast<float>(completedGpuMs);
                    if (gpuFrameSamplesMs.size() < 60000) {
                        gpuFrameSamplesMs.push_back(completedGpuMs);
                    }
                }
            } else if (!d3d.checkDeviceHealthy(&d3dInitErr)) {
                recoverGraphics(d3dInitErr);
            }
        } else {
            Sleep(16);
        }

        const auto cpuFrameT1 = std::chrono::steady_clock::now();
        const double cpuMs = std::chrono::duration<double, std::milli>(cpuFrameT1 - cpuFrameT0).count();
        if (cpuFrameSamplesMs.size() < 60000) {
            cpuFrameSamplesMs.push_back(cpuMs);
        }
        ++frameCounter;

        if (std::chrono::duration<double>(cpuFrameT1 - lastTitleUpdate).count() >= 0.5) {
            lastTitleUpdate = cpuFrameT1;
            char titleBuf[280]{};
            std::snprintf(
                titleBuf,
                sizeof(titleBuf),
                "BOO SAH SAH | %s | Act %zu/%zu (%s) | %.1f FPS (GPU %.2f ms) | %u-bit %s | [X/C/T/U/V]=Pillars [A]=A/B [K]=Cap [Tab]=HUD",
                TimelineClock::formatTimecode(tick.samplePosition, cueSheet.sampleRate).c_str(),
                actIdx + 1,
                cueSheet.scenes.size(),
                activeCue.sceneId.c_str(),
                smoothFps,
                lastGpuMs,
                d3d.swapchainChannelBits(),
                toString(qualityLadder.currentPreset()));
            window.setTitle(titleBuf);
        }

        const double wallElapsed = std::chrono::duration<double>(cpuFrameT1 - wallStart).count();
        if (settings.autoExitSeconds > 0.0 && wallElapsed >= settings.autoExitSeconds) {
            BOO_LOG_INFO("Smoke", "Reached requested --duration-seconds (" +
                                  std::to_string(settings.autoExitSeconds) + " s); exiting cleanly.");
            break;
        }
    }

    benchCancel.store(true, std::memory_order_release);
    if (benchThread.joinable()) {
        benchThread.join();
    }
    const AsyncDirectorTelemetry finalNpuTel = asyncDirector.telemetry();
    orchestrator.updateEngineStatus(finalNpuTel.hardwareResult);
    asyncDirector.shutdown();

    const auto wallEnd = std::chrono::steady_clock::now();
    const double totalWallSec = std::chrono::duration<double>(wallEnd - wallStart).count();

    RuntimeDiagnosticsSummary summary{};
    summary.settings = settings;
    summary.capabilities = caps;
    summary.activeClockSource = clock.source();
    summary.audioEndpoint = audioClockReady ? audio.endpointDescription() : "None";
    summary.audioFallbackReason = audioEndpointFailed ? "WASAPI endpoint invalidated during playback" : audio.fallbackReason();
    summary.audioUsesHardwarePresentationClock = audio.usesHardwarePresentationClock();
    summary.audioSubmittedSamples48k = audio.submittedFrames48k();
    summary.audioPresentedSamples48k = audio.presentedFrames48k();
    summary.audioBufferLatencyMs = audio.estimatedBufferLatencyMs();
    summary.audioEndpointMigrations = audioEndpointMigrations;
    summary.renderedFrames = retiredPresentCount + d3d.presentCount();
    summary.presentStalls = retiredPresentStalls + d3d.presentStallCount();
    d3d.waitForGpu();
    summary.deviceRecoveryAttempts = recoveryAttempted ? 1u : 0u;
    summary.deviceRecoverySuccesses = deviceRecoverySuccesses;
    summary.deviceLostInjected = deviceLostInjected;
    summary.graphicsUnavailable = graphicsUnavailable;
    summary.d3dDebugLayerActive = debugLayerWasActive;
    summary.d3dDebugErrors = retiredDebugErrors + d3d.debugErrorCount();
    summary.d3dDebugWarnings = retiredDebugWarnings + d3d.debugWarningCount();
    summary.startSample = initialSample;
    summary.finalSample = clock.currentSample();
    summary.wallElapsedSeconds = totalWallSec;
    summary.cpuFrameStats = computeFrametimeStats(cpuFrameSamplesMs);
    summary.gpuFrameStats = computeFrametimeStats(gpuFrameSamplesMs);
    summary.lastGpuPassTimings = d3d.lastPassTimings();
    if (liveM1Ptr && !orchestrator.isUsingFallbackEffect()) {
        for (size_t p = 0; p < 4; ++p) {
            summary.lastLivePillars[p] = liveM1Ptr->livePillarTelemetry(p);
        }
    }
    summary.lastNpuInferMs = finalNpuTel.lastInferMs;
    summary.lastNpuResultAgeMs = finalNpuTel.resultAgeMs;
    summary.npuCompletedInferences = finalNpuTel.completedInferences;
    summary.swapchainBits = d3d.swapchainChannelBits();
    summary.peakEstimatedVramBytes = peakVramBytes;
    summary.finalQualityPreset = qualityLadder.currentPreset();
    summary.qualityDowngrades = qualityLadder.downgradeCount();
    summary.effectFaultCount = orchestrator.faultCount();
    summary.usedEmbeddedFallbackShader = orchestrator.activeEffectUsesEmbeddedFallbackShader();
    summary.spoutActive = spoutSender.isActive();
    summary.spoutSenderName = spoutSender.senderName();
    summary.spoutPublishedFrames = spoutSender.publishedFrameCount();
    summary.mediaInputActive = mediaIngest.isActive();
    summary.liveCameraActive = mediaIngest.isLiveCameraActive();
    summary.mediaDecodedFrames = mediaIngest.decodedFrameCount();
    summary.mediaDiagnosticCode = mediaIngest.diagnosticCode();
    summary.mediaStatusReason = mediaIngest.statusReason();
    summary.hasBenchmarkReport = benchCompleted.load();
    summary.benchmarkReport = latestBenchReport;

    if (!settings.exportDiagnosticsPath.empty()) {
        std::string expErr;
        if (exportDiagnosticsJson(settings.exportDiagnosticsPath, summary, orchestrator, &expErr)) {
            BOO_LOG_INFO("Diagnostics", "Exported JSON report to " + redactUserPath(settings.exportDiagnosticsPath.string()));
        } else {
            BOO_LOG_ERROR("Diagnostics", expErr);
        }
    }
    if (summary.hasBenchmarkReport && !settings.exportBenchJsonPath.empty()) {
        exportBenchmarkJson(settings.exportBenchJsonPath, summary.benchmarkReport);
    }
    if (summary.hasBenchmarkReport && !settings.exportBenchCsvPath.empty()) {
        exportBenchmarkCsv(settings.exportBenchCsvPath, summary.benchmarkReport);
    }

    BOO_LOG_INFO(
        "Shutdown",
        "Clean shutdown: presented " + std::to_string(summary.renderedFrames) +
            " frames over " + std::to_string(totalWallSec).substr(0, 5) +
            " s (GPU p50=" + std::to_string(summary.gpuFrameStats.p50Ms).substr(0, 5) +
            " ms, p95=" + std::to_string(summary.gpuFrameStats.p95Ms).substr(0, 5) + " ms).");

    d3d.retireGpuForShutdown();
    if (settings.enableD3DDebug && (summary.d3dDebugErrors != 0 || summary.d3dDebugWarnings != 0)) runtimeExitCode = 1;
    mediaIngest.shutdown();
    spoutSender.shutdown();
    clock.shutdown();
    orchestrator.shutdown();
    d3d.shutdown();
    window.destroy();
    audio.shutdown();
    Logger::instance().closeLogFile();
    return runtimeExitCode;
}
