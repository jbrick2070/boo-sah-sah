#include "boo/api.hpp"
#include "boo/audio.hpp"
#include "boo/audio_reactive.hpp"
#include "boo/benchmark.hpp"
#include "boo/capabilities.hpp"
#include "boo/clock.hpp"
#include "boo/cues.hpp"
#include "boo/diagnostics.hpp"
#include "boo/director.hpp"
#include "boo/gpu_sensors.hpp"
#include "boo/health.hpp"
#include "boo/localization.hpp"
#include "boo/logging.hpp"
#include "boo/m1_scene.hpp"
#include "boo/media_input.hpp"
#include "boo/render.hpp"
#include "boo/results_worker.hpp"
#include "boo/results_dialog.hpp"
#include "boo/settings.hpp"
#include "boo/spout_sender.hpp"
#include "boo/startup_progress.hpp"
#include "boo/physx_genres.hpp"
#include "boo/physx_suite.hpp"
#include "boo/physx_rigid.hpp"
#include "boo/physx_marbles.hpp"
#include "boo/dawn_bathroom.hpp"
#include "boo/physx_fluid.hpp"
#include "boo/physx_soft.hpp"
#include "boo/physx_route_test.hpp"
#include "../render/bird_flight.hpp"
#include "boo/ui_text.hpp"
#include "boo/window.hpp"
#include "boo/workout_governor.hpp"
#include "../render/bird_flight.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

// Section 3.3: Export high-performance discrete GPU hints on hybrid laptops
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

// <Pictures>\BOO SAH SAH: where captures go when the working folder cannot be written. Empty on failure.
std::filesystem::path userCaptureFolder() {
    PWSTR pictures = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &pictures)) && pictures) {
        out = std::filesystem::path(pictures) / L"BOO SAH SAH";
    }
    CoTaskMemFree(pictures);
    return out;
}

// The dashboard row's figures for the genre on the dial.
boo::PhysxStats physxStatsFor(uint32_t genre, const boo::FlowSmokeStats& smoke, const boo::PhysxRuntimeInfo& runtime,
                              const boo::PhysxRigidGenre* rigid = nullptr, const boo::PhysxFluidGenre* fluid = nullptr,
                              const boo::PhysxSoftGenre* soft = nullptr) {
    using namespace boo;
    PhysxStats out;
    out.genre = genre < kPhysxGenreCount ? genre : 0u;
    if (out.genre == static_cast<uint32_t>(PhysxGenre::Smoke)) {
        // RendererFeatureState: Unavailable 0, Off 1, Active 2, Idle 3 (Idle is "starting" here).
        out.state = static_cast<uint32_t>(smoke.state);
        out.backend = smoke.backend;
        out.reason = smoke.reason;
        out.stepCpuMs = smoke.simulateCpuMs;
        out.stepGpuMs = smoke.simulateGpuMs;
        out.renderGpuMs = smoke.renderGpuMs;
        out.counters[0] = static_cast<float>(static_cast<double>(smoke.activeVoxels) / 1000.0);
        out.counters[1] = static_cast<float>(smoke.activeBlocks);
        out.counters[2] = smoke.gridMegabytes;
        out.counters[3] = static_cast<float>(smoke.sources);
        out.counterNames[0] = "thousand_voxels";
        out.counterNames[1] = "sparse_blocks";
        out.counterNames[2] = "grid_mb";
        out.counterNames[3] = "sources";
        out.gpuMemoryMegabytes = smoke.flowMemoryMegabytes;
        out.steps = smoke.steps;
    } else if (out.genre == static_cast<uint32_t>(PhysxGenre::Rigid) && rigid) {
        out = rigid->stats();
    } else if (out.genre == static_cast<uint32_t>(PhysxGenre::Fluid) && fluid) {
        out = fluid->stats();
    } else if (out.genre == static_cast<uint32_t>(PhysxGenre::Soft) && soft) {
        out = soft->stats();
    } else if (out.genre != 0u && physxGenreInfo(out.genre).usesPhysxSdk) {
        out.state = runtime.state == PhysxRuntimeState::Ready ? 2u : runtime.state == PhysxRuntimeState::Starting ? 3u : 0u;
        out.reason = runtime.reason;
    } else {
        out.state = 1u; // off
    }
    return out;
}

// <LocalAppData>\BOO SAH SAH\startup-memory.txt: what the last start loaded and how much GPU memory
// it reached, so the loading bar's totals are this machine's own. Empty on failure.
std::filesystem::path startupMemoryFile() {
    PWSTR local = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)) && local) {
        out = std::filesystem::path(local) / L"BOO SAH SAH" / L"startup-memory.txt";
    }
    CoTaskMemFree(local);
    return out;
}

// The figures under the loading bar. Numbers and units only, so they need no translation:
// megabytes read of the total, megabytes whose SHA-256 matched the manifest, system and GPU
// memory in use against what the last start reached, and shader code compiled into pipelines.
std::wstring loadingDetail(const boo::StartupProgressView& v) {
    constexpr double kMb = 1024.0 * 1024.0, kGb = kMb * 1024.0;
    wchar_t text[260]{};
    int at = 0;
    if (v.assetBytesTotal > 0) {
        at += std::swprintf(text + at, std::size(text) - at, L"%.0f / %.0f MB", v.assetBytesLoaded / kMb, v.assetBytesTotal / kMb);
        at += std::swprintf(text + at, std::size(text) - at, L"     SHA-256 verified  %.0f MB", v.assetBytesVerified / kMb);
        if (v.assetsMismatched > 0) at += std::swprintf(text + at, std::size(text) - at, L"     %u mismatched", v.assetsMismatched);
    }
    if (v.ramBytes > 0) {
        if (v.ramBytesExpected > 0) {
            at += std::swprintf(text + at, std::size(text) - at, L"     RAM  %.2f / %.2f GB", v.ramBytes / kGb, v.ramBytesExpected / kGb);
        } else {
            at += std::swprintf(text + at, std::size(text) - at, L"     RAM  %.2f GB", v.ramBytes / kGb);
        }
    }
    if (v.gpuBytes > 0) {
        if (v.gpuBytesExpected > 0) {
            at += std::swprintf(text + at, std::size(text) - at, L"     GPU  %.2f / %.2f GB", v.gpuBytes / kGb, v.gpuBytesExpected / kGb);
        } else {
            at += std::swprintf(text + at, std::size(text) - at, L"     GPU  %.2f GB", v.gpuBytes / kGb);
        }
    }
    if (v.shaderBytesTotal > 0) {
        // Shader code the graphics driver has compiled into pipelines, of the total.
        at += std::swprintf(text + at, std::size(text) - at, L"     Shaders  %.1f / %.1f MB",
                            v.shaderBytesCompiled / kMb, v.shaderBytesTotal / kMb);
    }
    return std::wstring(text, static_cast<size_t>(std::max(at, 0)));
}

// Minutes and seconds since start-up began.
std::wstring loadingClock(double seconds) {
    const int whole = std::max(0, static_cast<int>(seconds));
    wchar_t text[16]{};
    std::swprintf(text, std::size(text), L"%d:%02d", whole / 60, whole % 60);
    return text;
}

// The list under the bar: when each step began, what it is (in the Windows language, with the exact file
// or pipeline name), and how long it has taken.
std::vector<boo::LoadingLine> loadingLines(const boo::StartupProgressView& v) {
    std::vector<boo::LoadingLine> lines;
    lines.reserve(v.tasks.size());
    for (const boo::StartupTask& task : v.tasks) {
        constexpr boo::UiText kWords[] = {boo::UiText::LoadingDevice, boo::UiText::LoadingFiles,
                                          boo::UiText::LoadingShaders, boo::UiText::LoadingScene};
        boo::LoadingLine line;
        line.when = loadingClock(task.startedAt);
        line.what = boo::uiText(kWords[static_cast<size_t>(task.work)]);
        if (!task.item.empty()) line.what += L"   " + std::wstring(task.item.begin(), task.item.end());
        wchar_t took[24]{};
        std::swprintf(took, std::size(took), task.done && task.seconds < 10.0 ? L"%.1f s" : L"%.0f s",
                      task.done ? task.seconds : std::floor(task.seconds));
        line.took = took;
        line.running = !task.done;
        lines.push_back(std::move(line));
    }
    return lines;
}

// Changes with every build, so the first start after an install or update can say why it takes longer.
std::string startupBuildId() {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return {};
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return {};
    const auto written = std::filesystem::last_write_time(path, ec);
    if (ec) return {};
    return std::to_string(size) + "-" + std::to_string(written.time_since_epoch().count());
}

std::wstring wideFromUtf8(const std::string& text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(length, 0)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// Shown over the emergency test picture: what happened, this PC's graphics chip and the reason, and an offer to
// open the project's report page. Nothing is sent: the page opens in the browser only if the viewer says yes.
void offerFallbackReport(HWND owner, const std::string& adapter, const std::string& reason) {
    const std::wstring page = L"https://github.com/jbrick2070/boo-sah-sah/issues";
    std::wstring text = boo::uiText(boo::UiText::FallbackBody);
    text += L"\n\n" + wideFromUtf8(adapter) + L"\n" + wideFromUtf8(reason.substr(0, 300)) + L"\n" + page;
    const UINT reading = boo::uiIsRightToLeft() ? (MB_RTLREADING | MB_RIGHT) : 0u;
    if (MessageBoxW(owner, text.c_str(), boo::uiText(boo::UiText::FallbackTitle), MB_ICONWARNING | MB_YESNO | reading) == IDYES) {
        ShellExecuteW(owner, L"open", (page + L"/new").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

bool openNativeAudioFilePicker(void* hwndRaw, std::filesystem::path& outPath) {
    HMODULE hComDlg = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hComDlg) {
        return false;
    }
    using PFN_GetOpenFileNameW = BOOL(WINAPI*)(LPOPENFILENAMEW);
    const auto pfnGetOpen = reinterpret_cast<PFN_GetOpenFileNameW>(GetProcAddress(hComDlg, "GetOpenFileNameW"));
    if (!pfnGetOpen) {
        FreeLibrary(hComDlg);
        return false;
    }

    wchar_t fileBuf[1024] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(hwndRaw);
    ofn.lpstrFilter =
        L"Audio Soundtrack (*.mp3;*.wav;*.flac;*.m4a;*.aac;*.ogg)\0*.mp3;*.wav;*.flac;*.m4a;*.aac;*.ogg\0"
        L"All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(fileBuf));
    ofn.lpstrTitle = L"BOO SAH SAH — Select Custom Soundtrack (MP3 / Suno / WAV / FLAC)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

    const BOOL chosen = pfnGetOpen(&ofn);
    FreeLibrary(hComDlg);
    if (chosen && fileBuf[0] != L'\0') {
        outPath = std::filesystem::path(fileBuf);
        return true;
    }
    return false;
}

bool isAudioFileExtension(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".mp3" || ext == ".wav" || ext == ".flac" ||
           ext == ".m4a" || ext == ".aac" || ext == ".ogg" || ext == ".wma";
}

bool isVideoFileExtension(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".mp4" || ext == ".mov" || ext == ".mkv" || ext == ".avi" || ext == ".webm";
}

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
    // The film's full HUD and mesh shaders require a hardware adapter. WARP uses
    // the timeline-driven procedural visual for both explicit and automatic fallback.
    if (!d3d.isWarp() && !settings.legacyM0Visual && showMode != boo::ShowVisualMode::LegacyM0) {
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

// Dynamically loaded XInput gamepad reader (preserves strict zero-static-SDK PE import gate).
struct BooXInputGamepad {
    WORD wButtons = 0;
    BYTE bLeftTrigger = 0;
    BYTE bRightTrigger = 0;
    SHORT sThumbLX = 0;
    SHORT sThumbLY = 0;
    SHORT sThumbRX = 0;
    SHORT sThumbRY = 0;
};
struct BooXInputState {
    DWORD dwPacketNumber = 0;
    BooXInputGamepad Gamepad{};
};
static_assert(sizeof(BooXInputState) == 16, "BooXInputState must match Win32 XINPUT_STATE ABI");

class DynamicXInput {
public:
    using PfnGetState = DWORD(WINAPI*)(DWORD, BooXInputState*);

    DynamicXInput() noexcept {
        mod_ = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!mod_) {
            mod_ = LoadLibraryExW(L"xinput9_1_0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        if (mod_) {
            getState_ = reinterpret_cast<PfnGetState>(GetProcAddress(mod_, "XInputGetState"));
        }
    }

    ~DynamicXInput() noexcept {
        if (mod_) {
            FreeLibrary(mod_);
            mod_ = nullptr;
        }
    }

    DynamicXInput(const DynamicXInput&) = delete;
    DynamicXInput& operator=(const DynamicXInput&) = delete;

    bool poll(BooXInputGamepad& outPad) noexcept {
        if (!getState_) return false;
        BooXInputState st{};
        if (getState_(0, &st) != ERROR_SUCCESS) return false;
        outPad = st.Gamepad;
        return true;
    }

private:
    HMODULE mod_ = nullptr;
    PfnGetState getState_ = nullptr;
};

struct InteractiveExploreController {
    enum class DragTarget : uint8_t {
        None = 0,
        SplitCurtain,
        SunSlider,
        AzmSlider,
        TimelineScrub,
        AltSunOrbit,
        ViewportLook
    };

    bool exploreActive = false;
    float blendT = 0.0f;
    float eye[3]{-0.56f, 3.90f, 4.32f};
    float yaw = 0.22f;
    float pitch = -0.02f;
    float fovY = 0.62f;

    bool sunOverride = false;
    float sunElevation01 = 0.43f;   // Golden Taipei dawn default (~+19.9 deg)
    float sunAzimuth01 = 0.4125f;   // Southeast window portal default (~-31.5 deg)
    float sunDir[3]{0.80f, 0.34f, -0.49f};
    float sunScale = 1.0f;
    bool lampsOff = false;
    float abSplitRatio = 0.50f;
    uint32_t activePreset = 0;
    uint32_t aovMode = 0;
    uint32_t soloEngine = 0;
    bool studioDrawerOpen = false;
    float pendingScrubRatio = -1.0f;

    struct MarbleBody {
        float pos[3]{0.0f, 0.0f, 0.0f};
        float prevPos[3]{0.0f, 0.0f, 0.0f};
        float vel[3]{0.0f, 0.0f, 0.0f};
        float rotQuat[4]{0.0f, 0.0f, 0.0f, 1.0f};
        float radius = 0.020f;
        uint32_t matId = 3u;
        float restTimer = 0.0f;
        bool active = false;
    };
    std::array<MarbleBody, boo::InteractiveView::kMaxMarbles> marbles{};
    uint32_t marbleCount = 0;
    uint32_t marbleSpawnGen = 0;
    bool marblesMoving = false;
    // Set once PhysX's runtime is ready (test-rig builds): the marbles then roll on PhysX, and the
    // built-in solver below is the fallback.
    boo::PhysxMarbles* physxMarbles = nullptr;
    int physxMarbleWorld = -1; // 1 bathroom, 0 altar: the world the PhysX scene was last given

    // Hands the marbles as they are now to PhysX, in the bathroom's world or the altar's.
    void handMarblesToPhysx(bool inDawnBathroom) noexcept {
        if (!physxMarbles) return;
        std::array<boo::MarbleState, boo::InteractiveView::kMaxMarbles> states{};
        for (uint32_t i = 0; i < marbleCount; ++i) {
            const MarbleBody& m = marbles[i];
            std::copy_n(m.pos, 3, states[i].position);
            std::copy_n(m.vel, 3, states[i].velocity);
            std::copy_n(m.rotQuat, 4, states[i].rotation);
            states[i].radius = m.radius;
        }
        physxMarbles->reset(inDawnBathroom, states.data(), marbleCount);
        physxMarbleWorld = inDawnBathroom ? 1 : 0;
    }

    uint32_t accumFrame = 0;
    bool hasPrevState = false;
    int64_t prevSamplePos = -1;
    uint32_t prevCanvasW = 0;
    uint32_t prevCanvasH = 0;
    bool prevDisableRt = false;
    boo::Edition prevEdition = boo::Edition::Family;
    float audioToastTimer = 0.0f;
    uint32_t lastAudioTrackSlot = 0xFFFFFFFFu;

    DragTarget dragTarget = DragTarget::None;
    WORD prevPadButtons = 0;

    // The full-screen pill in the top-right corner: FULL SCREEN in a window, EXIT FULL SCREEN when
    // full screen. It is always there while the HUD is up. With the HUD off it comes back whenever
    // the mouse moves, and for a few seconds on entering full screen, so there is always an obvious
    // way out. The present shader draws it; these are the same rectangle.
    static constexpr float kFullscreenPillWindowed = 86.0f, kFullscreenPillExit = 150.0f;
    float fullscreenPillOpacity = 0.0f;
    float pillMouseX = -1.0f, pillMouseY = -1.0f;
    bool pillWasFullscreen = false;
    std::chrono::steady_clock::time_point pillShownUntil{};

    void updateFullscreenPill(const boo::WindowInputSnapshot& snap, const boo::AppWindow& window, bool hudShown) {
        const auto now = std::chrono::steady_clock::now();
        const bool moved = pillMouseX >= 0.0f &&
                           (std::abs(snap.mouseX - pillMouseX) + std::abs(snap.mouseY - pillMouseY)) > 0.5f;
        pillMouseX = snap.mouseX;
        pillMouseY = snap.mouseY;
        const bool entered = window.isFullscreen() && !pillWasFullscreen;
        pillWasFullscreen = window.isFullscreen();
        if (moved) pillShownUntil = std::max(pillShownUntil, now + std::chrono::milliseconds(3000));
        if (entered) pillShownUntil = std::max(pillShownUntil, now + std::chrono::milliseconds(5000));
        const float left = std::chrono::duration<float>(pillShownUntil - now).count();
        // Fade over the last half second.
        fullscreenPillOpacity = hudShown ? 1.0f : std::clamp(left * 2.0f, 0.0f, 1.0f);
    }
    DynamicXInput xinput;

    void recomputeSunDir() noexcept {
        const float elev = -0.35f + std::clamp(sunElevation01, 0.0f, 1.0f) * 1.62f;
        const float azim = (std::clamp(sunAzimuth01, 0.0f, 1.0f) - 0.5f) * 6.2831853f;
        const float ce = std::cos(elev);
        sunDir[0] = std::cos(azim) * ce;
        sunDir[1] = std::sin(elev);
        sunDir[2] = std::sin(azim) * ce;
    }

    void setPoseFromEyeTarget(const float e[3], const float t[3], float fovRad) noexcept {
        eye[0] = e[0]; eye[1] = e[1]; eye[2] = e[2];
        float dx = t[0] - e[0];
        float dy = t[1] - e[1];
        float dz = t[2] - e[2];
        const float len = std::max(1e-5f, std::sqrt(dx * dx + dy * dy + dz * dz));
        dx /= len; dy /= len; dz /= len;
        pitch = std::asin(std::clamp(dy, -0.985f, 0.985f));
        yaw = std::atan2(dx, dz);
        fovY = std::clamp(fovRad, 0.30f, 1.65f);
    }

    void enterExplore(
        boo::ShowVisualMode showMode,
        boo::TimelineClock& clock,
        boo::AudioEngine& audio,
        bool audioClockReady,
        boo::AppWindow& window,
        const boo::AppSettings& settings,
        float moodIris) {
        if (!exploreActive) {
            const boo::M1CameraState cur = boo::evaluateShowCamera(
                showMode, clock.currentSample(), clock.sampleRate(), moodIris, settings.reducedFlash, settings.motionScale);
            setPoseFromEyeTarget(cur.eye, cur.target, cur.fovYRadians);
        }
        exploreActive = true;
        blendT = 1.0f;
        hasPrevState = false;
        window.setExploreKeyCapture(true);
        // The film and music keep running while you fly; Space pauses inside Explore, and a paused,
        // still camera refines into the progressive path-traced frame.
        if (audioClockReady) {
            audio.setExploreAmbient(true);
        }
        BOO_LOG_INFO("Explore", "Interactive RTX Explore camera ENABLED [F] (WASDQE + Mouse + Wheel + Alt-Drag Sun + NRD PT)");
    }

    void clearMarbles() noexcept {
        for (auto& m : marbles) {
            m = MarbleBody{};
        }
        if (physxMarbles) physxMarbles->reset(physxMarbleWorld == 1, nullptr, 0u);
        marbleCount = 0u;
        marblesMoving = false;
        hasPrevState = false;
    }

    static float evalBasinU(float px, float pz) noexcept { return boo::dawnBasinU(px, pz); }
    static float evalCountertopHeight(float px, float pz) noexcept { return boo::dawnCountertopHeight(px, pz); }

    void spawnMarbles(bool inDawnBathroom) noexcept {
        static constexpr uint32_t kPalette[6] = {3u, 4u, 35u, 8u, 23u, 31u};
        auto pcg01 = [](uint32_t& st) noexcept -> float {
            st = st * 747796405u + 2891336453u;
            uint32_t w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u;
            w = (w >> 22u) ^ w;
            return static_cast<float>(w & 0x00FFFFFFu) * (1.0f / 16777216.0f);
        };
        uint32_t rng = 0x9E3779B9u ^ (++marbleSpawnGen * 0x85EBCA6Bu);
        marbleCount = 10u;
        marblesMoving = true;
        hasPrevState = false;

        for (uint32_t i = 0; i < boo::InteractiveView::kMaxMarbles; ++i) {
            MarbleBody& m = marbles[i];
            m = MarbleBody{};
            if (i >= marbleCount) continue;
            m.active = true;
            m.matId = kPalette[i % 6u];
            m.radius = 0.0185f + 0.0050f * pcg01(rng);

            if (inDawnBathroom) {
                if (i < 6u) {
                    // Orbital cascade into the superellipse porcelain vessel basin
                    const float ang = static_cast<float>(i) * 1.0471976f + 0.28f * pcg01(rng);
                    const float rFrac = 0.72f + 0.16f * pcg01(rng);
                    m.pos[0] = -0.180f + std::cos(ang) * (0.252f * rFrac);
                    m.pos[2] =  6.245f + std::sin(ang) * (0.182f * rFrac);
                    m.pos[1] =  3.715f + 0.032f * static_cast<float>(i);
                    const float spd = 0.62f + 0.28f * pcg01(rng);
                    m.vel[0] = -std::sin(ang) * spd;
                    m.vel[1] = -0.18f;
                    m.vel[2] =  std::cos(ang) * (spd * 0.74f);
                } else {
                    // Countertop cascade banking across honed Calacatta marble around glass props
                    const uint32_t k = i - 6u;
                    const bool rightSide = (k % 2u) == 0u;
                    m.pos[0] = rightSide ? (0.06f + 0.08f * static_cast<float>(k)) : (-0.48f - 0.06f * static_cast<float>(k));
                    m.pos[2] = 6.38f - 0.05f * static_cast<float>(k);
                    m.pos[1] = 3.635f + 0.035f * static_cast<float>(k);
                    m.vel[0] = rightSide ? (0.34f + 0.14f * pcg01(rng)) : (0.42f + 0.15f * pcg01(rng));
                    m.vel[1] = -0.22f;
                    m.vel[2] = -0.52f - 0.22f * pcg01(rng);
                }
            } else {
                const float cp = std::cos(pitch);
                const float fwd[3] = {std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp};
                const float rgt[3] = {std::cos(yaw), 0.0f, -std::sin(yaw)};
                const float spread = (static_cast<float>(i) - 4.5f) * 0.085f;
                m.radius = 0.055f + 0.018f * pcg01(rng);
                m.pos[0] = eye[0] + fwd[0] * 2.15f + rgt[0] * spread;
                m.pos[1] = std::max(1.25f, eye[1] + fwd[1] * 1.4f + 0.35f + 0.09f * static_cast<float>(i));
                m.pos[2] = eye[2] + fwd[2] * 2.15f + rgt[2] * spread;
                m.vel[0] = fwd[0] * (1.25f + 0.35f * pcg01(rng)) + rgt[0] * (0.25f * (pcg01(rng) - 0.5f));
                m.vel[1] = 0.45f;
                m.vel[2] = fwd[2] * (1.25f + 0.35f * pcg01(rng)) + rgt[2] * (0.25f * (pcg01(rng) - 0.5f));
            }
            for (int c = 0; c < 3; ++c) m.prevPos[c] = m.pos[c];
        }
        handMarblesToPhysx(inDawnBathroom);
    }

    void stepMarbles(float wallDt, bool inDawnBathroom) noexcept {
        if (marbleCount == 0u) {
            marblesMoving = false;
            return;
        }
        for (uint32_t i = 0; i < marbleCount; ++i) {
            for (int c = 0; c < 3; ++c) marbles[i].prevPos[c] = marbles[i].pos[c];
        }
        if (physxMarbles) {
            // The film crossing into or out of the dawn bathroom changes the world they roll in.
            if (physxMarbleWorld != (inDawnBathroom ? 1 : 0)) handMarblesToPhysx(inDawnBathroom);
            std::array<boo::MarbleState, boo::InteractiveView::kMaxMarbles> states{};
            const bool moving = physxMarbles->step(wallDt, states.data(), marbleCount);
            for (uint32_t i = 0; i < marbleCount; ++i) {
                MarbleBody& m = marbles[i];
                std::copy_n(states[i].position, 3, m.pos);
                std::copy_n(states[i].velocity, 3, m.vel);
                std::copy_n(states[i].rotation, 4, m.rotQuat);
            }
            if (!moving)
                for (uint32_t i = 0; i < marbleCount; ++i) std::copy_n(marbles[i].pos, 3, marbles[i].prevPos);
            marblesMoving = moving;
            return;
        }

        const float dt = std::clamp(wallDt, 0.001f, 0.033f) / 8.0f;
        struct CylObs { float x, z, r; };
        static constexpr CylObs kBathCyls[4] = {
            {-0.180f, 6.472f, 0.030f},
            { 0.240f, 6.320f, 0.045f},
            { 0.395f, 6.250f, 0.046f},
            {-0.590f, 6.265f, 0.116f}
        };

        for (uint32_t sub = 0; sub < 8u; ++sub) {
            for (uint32_t i = 0; i < marbleCount; ++i) {
                MarbleBody& m = marbles[i];
                if (!m.active) continue;

                m.vel[1] -= 9.81f * dt;
                // Subtle air drag
                const float airDamp = std::max(0.0f, 1.0f - 0.18f * dt);
                m.vel[0] *= airDamp; m.vel[1] *= airDamp; m.vel[2] *= airDamp;

                m.pos[0] += m.vel[0] * dt;
                m.pos[1] += m.vel[1] * dt;
                m.pos[2] += m.vel[2] * dt;

                float supY = 0.0f;
                float supN[3]{0.0f, 1.0f, 0.0f};
                if (inDawnBathroom) {
                    const bool onCounter = (m.pos[0] >= -0.895f && m.pos[0] <= 0.515f &&
                                            m.pos[2] >=  5.990f && m.pos[2] <= 6.580f &&
                                            m.pos[1] >=  3.505f);
                    if (onCounter) {
                        constexpr float eps = 0.004f;
                        const float hC = evalCountertopHeight(m.pos[0], m.pos[2]);
                        const float hx = (evalCountertopHeight(m.pos[0] + eps, m.pos[2]) - evalCountertopHeight(m.pos[0] - eps, m.pos[2])) / (2.0f * eps);
                        const float hz = (evalCountertopHeight(m.pos[0], m.pos[2] + eps) - evalCountertopHeight(m.pos[0], m.pos[2] - eps)) / (2.0f * eps);
                        const float invLen = 1.0f / std::sqrt(hx * hx + 1.0f + hz * hz);
                        supN[0] = -hx * invLen;
                        supN[1] =  1.0f * invLen;
                        supN[2] = -hz * invLen;
                        supY = hC + m.radius / std::max(0.35f, supN[1]);
                    } else {
                        supY = 2.650f + m.radius;
                        // Floating walnut vanity front face deflection
                        if (m.pos[0] >= -0.885f && m.pos[0] <= 0.505f &&
                            m.pos[1] >=  2.960f && m.pos[1] <= 3.535f &&
                            m.pos[2] >   6.010f - m.radius) {
                            m.pos[2] = 6.010f - m.radius;
                            if (m.vel[2] > 0.0f) m.vel[2] = -m.vel[2] * 0.52f;
                        }
                    }

                    // Countertop cylindrical fixture & glass prop collisions
                    if (m.pos[1] >= 3.52f && m.pos[1] <= 3.88f) {
                        for (const auto& ob : kBathCyls) {
                            const float dx = m.pos[0] - ob.x;
                            const float dz = m.pos[2] - ob.z;
                            const float minD = ob.r + m.radius;
                            const float d2 = dx * dx + dz * dz;
                            if (d2 < minD * minD && d2 > 1e-10f) {
                                const float d = std::sqrt(d2);
                                const float nx = dx / d, nz = dz / d;
                                m.pos[0] = ob.x + nx * minD;
                                m.pos[2] = ob.z + nz * minD;
                                const float vn = m.vel[0] * nx + m.vel[2] * nz;
                                if (vn < 0.0f) {
                                    m.vel[0] -= 1.72f * vn * nx;
                                    m.vel[2] -= 1.72f * vn * nz;
                                }
                            }
                        }
                    }

                    // Bathroom architectural enclosure walls
                    if (m.pos[2] > 6.555f - m.radius) { m.pos[2] = 6.555f - m.radius; if (m.vel[2] > 0.0f) m.vel[2] *= -0.62f; }
                    if (m.pos[2] < 2.300f + m.radius) { m.pos[2] = 2.300f + m.radius; if (m.vel[2] < 0.0f) m.vel[2] *= -0.62f; }
                    if (m.pos[0] < -2.440f + m.radius) { m.pos[0] = -2.440f + m.radius; if (m.vel[0] < 0.0f) m.vel[0] *= -0.62f; }
                    if (m.pos[0] >  1.480f - m.radius) { m.pos[0] =  1.480f - m.radius; if (m.vel[0] > 0.0f) m.vel[0] *= -0.62f; }
                } else {
                    const bool onAltar = (std::abs(m.pos[0]) <= 0.58f && m.pos[2] >= 5.70f && m.pos[2] <= 6.86f && m.pos[1] >= 2.10f);
                    supY = (onAltar ? 2.160f : 0.0f) + m.radius;
                }

                // Support contact resolution, glass-on-stone bounce, rolling friction & quaternion spin
                bool inContact = false;
                if (m.pos[1] <= supY) {
                    m.pos[1] = supY;
                    inContact = true;
                    const float vn = m.vel[0] * supN[0] + m.vel[1] * supN[1] + m.vel[2] * supN[2];
                    if (vn < 0.0f) {
                        const float bounce = ( vn < -0.22f) ? 0.56f : 0.0f;
                        m.vel[0] -= (1.0f + bounce) * vn * supN[0];
                        m.vel[1] -= (1.0f + bounce) * vn * supN[1];
                        m.vel[2] -= (1.0f + bounce) * vn * supN[2];
                    }
                    const float rollDrag = std::max(0.0f, 1.0f - 0.82f * dt);
                    m.vel[0] *= rollDrag;
                    m.vel[1] *= rollDrag;
                    m.vel[2] *= rollDrag;
                }

                const float vDotN = m.vel[0] * supN[0] + m.vel[1] * supN[1] + m.vel[2] * supN[2];
                const float vt[3] = {
                    m.vel[0] - supN[0] * vDotN,
                    m.vel[1] - supN[1] * vDotN,
                    m.vel[2] - supN[2] * vDotN
                };
                const float spinScale = inContact ? (1.0f / std::max(0.008f, m.radius)) : (0.25f / std::max(0.008f, m.radius));
                const float wx = (supN[1] * vt[2] - supN[2] * vt[1]) * spinScale;
                const float wy = (supN[2] * vt[0] - supN[0] * vt[2]) * spinScale;
                const float wz = (supN[0] * vt[1] - supN[1] * vt[0]) * spinScale;
                const float qx = m.rotQuat[0], qy = m.rotQuat[1], qz = m.rotQuat[2], qw = m.rotQuat[3];
                m.rotQuat[0] += 0.5f * ( wx * qw + wy * qz - wz * qy) * dt;
                m.rotQuat[1] += 0.5f * (-wx * qz + wy * qw + wz * qx) * dt;
                m.rotQuat[2] += 0.5f * ( wx * qy - wy * qx + wz * qw) * dt;
                m.rotQuat[3] += 0.5f * (-wx * qx - wy * qy - wz * qz) * dt;
                const float qNorm = 1.0f / std::max(1e-6f, std::sqrt(
                    m.rotQuat[0] * m.rotQuat[0] + m.rotQuat[1] * m.rotQuat[1] +
                    m.rotQuat[2] * m.rotQuat[2] + m.rotQuat[3] * m.rotQuat[3]));
                for (int c = 0; c < 4; ++c) m.rotQuat[c] *= qNorm;
            }

            // Elastic sphere-sphere marble collisions
            for (uint32_t i = 0; i < marbleCount; ++i) {
                for (uint32_t j = i + 1u; j < marbleCount; ++j) {
                    MarbleBody& a = marbles[i];
                    MarbleBody& b = marbles[j];
                    if (!a.active || !b.active) continue;
                    const float dx = b.pos[0] - a.pos[0];
                    const float dy = b.pos[1] - a.pos[1];
                    const float dz = b.pos[2] - a.pos[2];
                    const float minD = a.radius + b.radius;
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < minD * minD && d2 > 1e-12f) {
                        const float d = std::sqrt(d2);
                        const float nx = dx / d, ny = dy / d, nz = dz / d;
                        const float pen = minD - d;
                        const float ma = a.radius * a.radius * a.radius;
                        const float mb = b.radius * b.radius * b.radius;
                        const float invSum = 1.0f / std::max(1e-8f, ma + mb);
                        a.pos[0] -= nx * pen * (mb * invSum);
                        a.pos[1] -= ny * pen * (mb * invSum);
                        a.pos[2] -= nz * pen * (mb * invSum);
                        b.pos[0] += nx * pen * (ma * invSum);
                        b.pos[1] += ny * pen * (ma * invSum);
                        b.pos[2] += nz * pen * (ma * invSum);

                        const float rvx = b.vel[0] - a.vel[0];
                        const float rvy = b.vel[1] - a.vel[1];
                        const float rvz = b.vel[2] - a.vel[2];
                        const float vn = rvx * nx + rvy * ny + rvz * nz;
                        if (vn < 0.0f) {
                            constexpr float restitution = 0.82f;
                            const float jImp = -(1.0f + restitution) * vn * (ma * mb * invSum);
                            a.vel[0] -= (jImp / ma) * nx;
                            a.vel[1] -= (jImp / ma) * ny;
                            a.vel[2] -= (jImp / ma) * nz;
                            b.vel[0] += (jImp / mb) * nx;
                            b.vel[1] += (jImp / mb) * ny;
                            b.vel[2] += (jImp / mb) * nz;
                        }
                    }
                }
            }
        }

        bool anyAwake = false;
        for (uint32_t i = 0; i < marbleCount; ++i) {
            MarbleBody& m = marbles[i];
            if (!m.active) continue;
            const float spd2 = m.vel[0] * m.vel[0] + m.vel[1] * m.vel[1] + m.vel[2] * m.vel[2];
            if (spd2 < 0.00025f) {
                m.restTimer += wallDt;
                if (m.restTimer > 0.32f) {
                    m.vel[0] = m.vel[1] = m.vel[2] = 0.0f;
                    for (int c = 0; c < 3; ++c) m.prevPos[c] = m.pos[c];
                } else {
                    anyAwake = true;
                }
            } else {
                m.restTimer = 0.0f;
                anyAwake = true;
            }
        }
        marblesMoving = anyAwake;
    }

    void exitExplore(
        boo::TimelineClock& clock,
        boo::AudioEngine& audio,
        bool audioClockReady,
        boo::AppWindow& window) {
        exploreActive = false;
        activePreset = 0;
        aovMode = 0u;
        clearMarbles();
        hasPrevState = false;
        window.setExploreKeyCapture(false);
        clock.resume();
        if (audioClockReady) {
            audio.setExploreAmbient(false);
            audio.setPaused(false);
        }
        BOO_LOG_INFO("Explore", "Returning smoothly to scripted film choreography [F]");
    }

    void applyHeroPreset(
        uint32_t preset1To6,
        const boo::CueSheet& cueSheet,
        boo::AppSettings& settings,
        boo::TimelineClock& clock,
        boo::AudioEngine& audio,
        bool audioClockReady,
        boo::FallbackOrchestrator& orchestrator,
        boo::AppWindow& window) {
        preset1To6 = std::clamp(preset1To6, 1u, 6u);
        const boo::ShowVisualMode curMode = boo::resolveShowVisualMode(cueSheet, settings.playbackMode);

        double targetSec = 343.5;
        float pEye[3]{-0.56f, 3.90f, 4.32f};
        float pTgt[3]{-0.08f, 3.85f, 6.38f};
        float pFov = 0.58f;
        const char* label = "Dawn Penthouse Bathroom Wide";

        if (curMode == boo::ShowVisualMode::M2Trilogy180s) {
            static constexpr double kTrilogySec[6] = {15.0, 45.0, 75.0, 108.0, 132.0, 168.0};
            static constexpr const char* kTrilogyNames[6] = {
                "Shanghai Steel Truss", "Huangpu Bund Panorama", "Shenzhen eVTOL Cruise",
                "Shenzhen Civic Hover", "Taipei 101 Golden TMD", "Rooftop Glass Orchid"};
            targetSec = kTrilogySec[preset1To6 - 1u];
            label = kTrilogyNames[preset1To6 - 1u];
            const auto cam = boo::evaluateTrilogyCamera(
                boo::TimelineClock::secondsToSamples(targetSec, cueSheet.sampleRate), cueSheet.sampleRate, settings.reducedFlash);
            for (int c = 0; c < 3; ++c) { pEye[c] = cam.eye[c]; pTgt[c] = cam.target[c]; }
            pFov = cam.fovYRadians;
        } else if (curMode == boo::ShowVisualMode::M1Slice60s || curMode == boo::ShowVisualMode::LegacyM0) {
            static constexpr double kM1Sec[6] = {8.0, 18.0, 28.0, 38.0, 46.0, 55.0};
            targetSec = kM1Sec[preset1To6 - 1u];
            label = "Taipei Slice Bookmark";
            const auto cam = boo::evaluateM1Camera(
                boo::TimelineClock::secondsToSamples(targetSec, cueSheet.sampleRate), cueSheet.sampleRate, 0.5f, settings.reducedFlash);
            for (int c = 0; c < 3; ++c) { pEye[c] = cam.eye[c]; pTgt[c] = cam.target[c]; }
            pFov = cam.fovYRadians;
        } else {
            if (cueSheet.durationSamples >= boo::TimelineClock::kFullFilmSamples &&
                settings.playbackMode != boo::PlaybackMode::FullFilm) {
                settings.playbackMode = boo::PlaybackMode::FullFilm;
                clock.clearLoopRange();
                clock.setLoopEnabled(settings.loop);
            }
            switch (preset1To6) {
            case 1u:
                targetSec = 343.5; label = "Dawn Penthouse Bathroom Wide";
                pEye[0] = -0.56f; pEye[1] = 3.90f; pEye[2] = 4.32f;
                pTgt[0] = -0.08f; pTgt[1] = 3.85f; pTgt[2] = 6.38f;
                pFov = 0.58f;
                break;
            case 2u:
                targetSec = 346.0; label = "CNC Faucet, Aerator Droplet & Porcelain Basin Macro";
                pEye[0] = 0.05f;  pEye[1] = 3.83f; pEye[2] = 5.76f;
                pTgt[0] = -0.18f; pTgt[1] = 3.67f; pTgt[2] = 6.34f;
                pFov = 0.44f;
                break;
            case 3u:
                targetSec = 348.0; label = "Backlit Calathea Veins & Keepsake Glass Orchid";
                pEye[0] = -0.86f; pEye[1] = 3.87f; pEye[2] = 5.58f;
                pTgt[0] = -0.46f; pTgt[1] = 3.82f; pTgt[2] = 6.27f;
                pFov = 0.50f;
                break;
            case 4u:
                targetSec = 36.0; label = "Hero Glass Orchid Crystal Sanctuary";
                pEye[0] = 0.56f;  pEye[1] = 5.08f; pEye[2] = -0.82f;
                pTgt[0] = 0.00f;  pTgt[1] = 5.28f; pTgt[2] = 5.50f;
                pFov = 0.72f;
                break;
            case 5u:
                targetSec = 72.0; label = "Enamel Cathedral & Hall of Mirrors";
                pEye[0] = 1.68f;  pEye[1] = 5.12f; pEye[2] = 0.25f;
                pTgt[0] = -0.15f; pTgt[1] = 5.62f; pTgt[2] = 5.65f;
                pFov = 0.76f;
                break;
            case 6u:
                targetSec = 175.0; label = "Botanical Sanctuary & Machined Brass Iris";
                pEye[0] = 0.72f;  pEye[1] = 4.66f; pEye[2] = -0.78f;
                pTgt[0] = 0.00f;  pTgt[1] = 4.85f; pTgt[2] = 5.50f;
                pFov = 0.78f;
                break;
            }
            const double maxCueSec = std::max(0.0, boo::TimelineClock::samplesToSeconds(cueSheet.durationSamples, cueSheet.sampleRate) - 0.5);
            targetSec = std::min(targetSec, maxCueSec);
        }

        const bool wasExploring = exploreActive;
        const bool wasPaused = clock.state() == boo::TransportState::Paused || clock.state() == boo::TransportState::Ended;
        clock.seekToSeconds(targetSec);
        if (wasPaused) clock.pause(); else clock.resume();
        if (audioClockReady) {
            audio.setExploreAmbient(true);
            if (!wasExploring || !audio.hasExternalAudioFile()) {
                if (!audio.hasExternalAudioFile()) {
                    audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                }
            }
            audio.setPaused(clock.state() == boo::TransportState::Paused || clock.state() == boo::TransportState::Ended);
        }
        orchestrator.resetTo(clock.currentSeconds());

        setPoseFromEyeTarget(pEye, pTgt, pFov);
        exploreActive = true;
        blendT = 1.0f;
        activePreset = preset1To6;
        hasPrevState = false;
        window.setExploreKeyCapture(true);
        BOO_LOG_INFO("Explore", std::string("Hero Preset [") + std::to_string(preset1To6) + "]: " + label);
    }

    void handlePreTickInput(
        const boo::WindowInputSnapshot& snap,
        boo::AppWindow& window,
        const boo::AppSettings& settings,
        uint32_t hudMode,
        bool hasHardwareTable,
        std::array<bool, 5> graphicsAvailable,
        bool volumeVisible,
        bool benchmarkVisible,
        bool filmEnded,          // the HUD shows PLAY AGAIN in place of PLAY/NEXT and the dock's NEXT SCN
        BooXInputGamepad& outPad,
        bool& outHasPad) {
        outHasPad = xinput.poll(outPad);
        if (outHasPad && snap.hasFocus) {
            const WORD pressed = static_cast<WORD>(outPad.wButtons & ~prevPadButtons);
            if (pressed & 0x8000u) window.pushCommandFromWndProc(boo::TransportCommand::ToggleExploreMode);    // Y
            if (pressed & 0x4000u) window.pushCommandFromWndProc(boo::TransportCommand::TogglePracticalLamps); // X
            if (pressed & 0x2000u) window.pushCommandFromWndProc(boo::TransportCommand::ToggleRtxSplitWipe);   // B
            if (pressed & 0x1000u) window.pushCommandFromWndProc(boo::TransportCommand::TogglePlayPause);      // A
            if (pressed & 0x0200u) window.pushCommandFromWndProc(boo::TransportCommand::SpawnMarbles);         // RB -> Marbles
            if (pressed & 0x0100u) window.pushCommandFromWndProc(boo::TransportCommand::CycleAovMode);         // LB -> AOV
            if (pressed & 0x0008u) { // DPad Right -> next hero preset
                const uint32_t nxt = (activePreset % 6u) + 1u;
                window.pushCommandFromWndProc(static_cast<boo::TransportCommand>(
                    static_cast<int>(boo::TransportCommand::HeroPreset1) + static_cast<int>(nxt - 1u)));
            }
            if (pressed & 0x0004u) { // DPad Left -> prev hero preset
                const uint32_t prv = (activePreset <= 1u) ? 6u : (activePreset - 1u);
                window.pushCommandFromWndProc(static_cast<boo::TransportCommand>(
                    static_cast<int>(boo::TransportCommand::HeroPreset1) + static_cast<int>(prv - 1u)));
            }
            prevPadButtons = outPad.wButtons;
        } else if (!outHasPad) {
            prevPadButtons = 0;
        }

        if (!snap.lmbDown) {
            dragTarget = DragTarget::None;
        }

        const float w = static_cast<float>(std::max<uint32_t>(1u, window.clientWidth()));
        const float h = static_cast<float>(std::max<uint32_t>(1u, window.clientHeight()));
        const float u = std::clamp(std::floor(h / 540.0f), 1.0f, 3.0f);
        const bool barVisible = settings.showOverlay && (hudMode != 3u) &&
                                (studioDrawerOpen || exploreActive || sunOverride || lampsOff || aovMode != 0u || marbleCount != 0u || soloEngine != 0u) &&
                                w >= 640.0f && h >= 300.0f;
        const float barW = 536.0f * u;
        const float barH = 54.0f * u;
        const float barMinX = 0.5f * (w - barW);
        const float barMinY = (hudMode == 1u) ? (h - 60.0f * u) : (h - 104.0f * u);
        const float tlMinX  = 82.0f * u;
        const float tlSizeX = std::max(90.0f, w * 0.20f);
        const float statsRight = tlMinX + tlSizeX + 44.0f * u + 90.0f * u;
        const bool enginePillsVisible = w - 245.0f * u > statsRight;

        // Right-click on the 5 bottom-right Engine Pills toggles that single engine in place,
        // and right-click on Row 2 [TRK1..4] steps backward through the built-in soundtrack!
        if (snap.rmbJustPressed) {
            if (barVisible &&
                snap.mouseX >= barMinX + 310.0f * u && snap.mouseX <= barMinX + 350.0f * u &&
                snap.mouseY >= barMinY + 27.0f * u  && snap.mouseY <= barMinY + 49.0f * u) {
                window.pushCommandFromWndProc(boo::TransportCommand::PrevMusicTrack);
            } else if (settings.showOverlay && enginePillsVisible && (hudMode == 0u || hudMode == 2u) && w >= 480.0f && h >= 300.0f) {
                const float rightBase = w - 245.0f * u;
                const float pTop = h - 34.0f * u;
                if (snap.mouseY >= pTop && snap.mouseY <= pTop + 22.0f * u) {
                    static constexpr boo::TransportCommand kToggleCmds[5] = {
                        boo::TransportCommand::ToggleRtRuntime,
                        boo::TransportCommand::ToggleTensorRuntime,
                        boo::TransportCommand::ToggleNpuRuntime,
                        boo::TransportCommand::ToggleCudaRuntime,
                        boo::TransportCommand::ToggleCodecRuntime
                    };
                    for (uint32_t e = 0; e < 5u; ++e) {
                        const float pMinX = rightBase + static_cast<float>(e) * 47.0f * u;
                        if (snap.mouseX >= pMinX && snap.mouseX <= pMinX + 42.0f * u) {
                            window.pushCommandFromWndProc(kToggleCmds[e]);
                            break;
                        }
                    }
                }
            }
        }

        updateFullscreenPill(snap, window, settings.showOverlay && hudMode != 3u);

        if (snap.lmbJustPressed) {
            bool consumed = false;
            // These coordinates mirror the hardware table and the A/B selector in the present shader.
            static constexpr boo::TransportCommand compareCommands[] = {
                boo::TransportCommand::CompareRT, boo::TransportCommand::CompareCUDA, boo::TransportCommand::CompareTensor,
                boo::TransportCommand::CompareCodec, boo::TransportCommand::CompareNPU, boo::TransportCommand::CloseComparison
            };
            const float pillW = (window.isFullscreen() ? kFullscreenPillExit : kFullscreenPillWindowed) * u;
            const bool onFullscreenPill = hasHardwareTable && fullscreenPillOpacity > 0.2f && w >= 480.0f && h >= 300.0f &&
                snap.mouseX >= w - 14.0f * u - pillW && snap.mouseX < w - 14.0f * u &&
                snap.mouseY >= 14.0f * u && snap.mouseY < 32.0f * u;
            if (onFullscreenPill) {
                window.pushCommandFromWndProc(boo::TransportCommand::ToggleFullscreen);
                consumed = true;
            } else if (hasHardwareTable && settings.showOverlay && hudMode < 2u && w >= 480.0f && h >= 300.0f) {
                const bool wide = hudMode == 0u && w >= 330.0f * u && (!benchmarkVisible || w >= 610.0f * u);
                const float x = snap.mouseX / u - 14.0f;
                const float y = snap.mouseY / u - (volumeVisible ? 44.0f : 14.0f);
                const float tableW = wide ? 306.0f : 220.0f;
                // Panel buttons mirror the present shader: [-] in the full header, [+]/[x] on the mini strip,
                // and the HUD+ tab when hidden.
                if (settings.dashboardPanel == 2u) {
                    if (x >= 0 && x < 44.0f && y >= 0 && y < 16.0f) {
                        window.pushCommandFromWndProc(boo::TransportCommand::DashboardFull);
                        consumed = true;
                    }
                } else if (settings.dashboardPanel == 1u) {
                    if (x >= 0 && x < 228.0f && y >= 0 && y < 62.0f) {
                        const bool hideButton = x >= 211.0f && x < 225.0f && y >= 3.0f && y < 17.0f;
                        window.pushCommandFromWndProc(hideButton ? boo::TransportCommand::DashboardHidden
                                                                 : boo::TransportCommand::DashboardFull);
                        consumed = true;
                    }
                } else if (x >= tableW - 20.0f && x < tableW - 6.0f && y >= 3.0f && y < 17.0f) {
                    window.pushCommandFromWndProc(boo::TransportCommand::DashboardMini);
                    consumed = true;
                } else if (x >= 0 && x < tableW && y >= 19 && y < 119) {
                    const auto row = static_cast<unsigned>((y - 19) / 20);
                    const bool sanctuaryShortcut = row == 4 && x >= 32 && x < 55;
                    window.pushCommandFromWndProc(sanctuaryShortcut ? boo::TransportCommand::SelectModeNPU : compareCommands[row]);
                    consumed = true;
                } else if (x >= 92 && x < 120 && y >= 1 && y < 19) {
                    window.pushCommandFromWndProc(boo::TransportCommand::CycleAbCompareEngine);
                    consumed = true;
                } else if (x >= 4 && x < (wide ? 302 : 216) && y >= 139.f) {
                    constexpr boo::TransportCommand featureCommands[] = {
                        boo::TransportCommand::CyclePathTracing, boo::TransportCommand::CycleDlss,
                        boo::TransportCommand::ToggleRayReconstruction, boo::TransportCommand::CycleFrameGeneration,
                        boo::TransportCommand::CycleReflex};
                    // Rows mirror the present shader: five graphics rows, the NEURAL heading, three neural rows.
                    const float rowH = wide ? 22.f : 18.f;
                    const float neuralTop = 139.f + 5.f * rowH;
                    const float neuralRowsTop = neuralTop + 20.f;
                    if (y < neuralTop) {
                        const auto row = static_cast<unsigned>((y - 139.f) / rowH);
                        if (row < graphicsAvailable.size()) {
                            if (graphicsAvailable[row]) window.pushCommandFromWndProc(featureCommands[row]);
                            consumed = true;
                        }
                    } else if (y < neuralRowsTop) {
                        window.pushCommandFromWndProc(snap.shiftDown ? boo::TransportCommand::ToggleNeuralTensorCores
                                                                     : boo::TransportCommand::ToggleNeuralRendering);
                        consumed = true;
                    } else if (const float workoutTop = neuralRowsTop + 3.f * rowH + 20.f + 36.f; y >= workoutTop + 20.f) {
                        // FLOCK row: the name cycles the formation, the CAM pill toggles the bird camera.
                        if (y >= workoutTop + 60.f && y < workoutTop + 82.f) {
                            window.pushCommandFromWndProc(boo::TransportCommand::RunFilmBenchmark);
                            consumed = true;
                        } else if (y >= workoutTop + 40.f && y < workoutTop + 60.f) {
                            // PHYSX row: dial through the genres.
                            window.pushCommandFromWndProc(boo::TransportCommand::CyclePhysxGenre);
                            consumed = true;
                        } else if (y < workoutTop + 40.f) {
                            window.pushCommandFromWndProc(x >= tableW - 62.f ? boo::TransportCommand::ToggleBirdCam
                                                                             : boo::TransportCommand::CycleFlockFormation);
                            consumed = true;
                        }
                    } else if (y >= workoutTop) {
                        // WORKOUT row: [-] and [+] (the rest of the row does nothing).
                        if (y >= workoutTop + 3.f && y < workoutTop + 18.f && x >= tableW - 56.f && x < tableW - 20.f) {
                            window.pushCommandFromWndProc(x < tableW - 38.f ? boo::TransportCommand::GpuWorkoutDown
                                                                           : boo::TransportCommand::GpuWorkoutUp);
                        }
                        consumed = true;
                    } else if (y < neuralRowsTop + 3.f * rowH) {
                        constexpr boo::TransportCommand toggles[] = {boo::TransportCommand::ToggleNeuralShading,
                            boo::TransportCommand::ToggleNeuralTextures, boo::TransportCommand::ToggleNeuralCache};
                        constexpr boo::TransportCommand solos[] = {boo::TransportCommand::SoloNeuralShading,
                            boo::TransportCommand::SoloNeuralTextures, boo::TransportCommand::SoloNeuralCache};
                        const auto row = std::min(2u, static_cast<unsigned>((y - neuralRowsTop) / rowH));
                        const bool onBadge = x >= (wide ? 238.f : 146.f);
                        window.pushCommandFromWndProc((onBadge || snap.shiftDown) ? solos[row] : toggles[row]);
                        consumed = true;
                    }
                }
            } else if (hasHardwareTable && settings.showOverlay && hudMode == 2u && w >= 480.0f && h >= 300.0f) {
                const auto lb = boo::computeLetterboxViewport16x9(window.clientWidth(), window.clientHeight());
                const float scale = std::clamp(std::min(u, std::floor(lb.width / 480.0f)), 1.0f, 3.0f);
                const float x = (snap.mouseX - lb.x) / scale - 14.0f;
                const float y = (snap.mouseY - lb.y) / scale - 46.0f;
                if (y >= 0 && y < 21 && x >= 0 && x < 5 * 43 + 23) {
                    const auto tab = static_cast<unsigned>(x / 43);
                    if (x - static_cast<float>(tab) * 43 < 40) {
                        window.pushCommandFromWndProc(compareCommands[tab]);
                        consumed = true;
                    }
                }
            }

            // 1. Persistent Bottom Transport Bar (Timeline Scrubber + [< PRV] [PLAY/PAUS] [NEXT >] [F:FREE/FILM] + 5 SOLO Engine Pills)
            if (!consumed && settings.showOverlay && (hudMode == 0u || hudMode == 2u) && w >= 480.0f && h >= 300.0f) {
                const float rightBase = w - 245.0f * u;
                const float pTop = h - 34.0f * u;
                const float pBot = pTop + 22.0f * u;
                const float statX = tlMinX + tlSizeX + 44.0f * u;

                // 1a. Timeline Scrub Bar
                if (snap.mouseX >= tlMinX && snap.mouseX <= tlMinX + tlSizeX &&
                    snap.mouseY >= h - 36.0f * u && snap.mouseY <= h - 10.0f * u) {
                    dragTarget = DragTarget::TimelineScrub;
                    pendingScrubRatio = std::clamp((snap.mouseX - tlMinX) / std::max(1.0f, tlSizeX), 0.0f, 0.9995f);
                    consumed = true;
                }
                // 1b. 5 Clickable Hardware SOLO / ISOLATE Pills ([RT] [TNSR] [NPU] [CUDA] [CODC])
                else if (rightBase > statX + 90.0f * u && snap.mouseY >= pTop && snap.mouseY <= pBot) {
                    static constexpr boo::TransportCommand kSoloCmds[5] = {
                        boo::TransportCommand::SoloEngineRT,
                        boo::TransportCommand::SoloEngineTensor,
                        boo::TransportCommand::SoloEngineNPU,
                        boo::TransportCommand::SoloEngineCUDA,
                        boo::TransportCommand::SoloEngineCodec
                    };
                    static constexpr boo::TransportCommand kToggleCmds[5] = {
                        boo::TransportCommand::ToggleRtRuntime,
                        boo::TransportCommand::ToggleTensorRuntime,
                        boo::TransportCommand::ToggleNpuRuntime,
                        boo::TransportCommand::ToggleCudaRuntime,
                        boo::TransportCommand::ToggleCodecRuntime
                    };
                    for (uint32_t e = 0; e < 5u; ++e) {
                        const float pMinX = rightBase + static_cast<float>(e) * 47.0f * u;
                        if (snap.mouseX >= pMinX && snap.mouseX <= pMinX + 42.0f * u) {
                            window.pushCommandFromWndProc(snap.shiftDown ? kToggleCmds[e] : kSoloCmds[e]);
                            consumed = true;
                            break;
                        }
                    }

                    // 1c. [F:FREE / F:FILM] pill
                    if (!consumed && rightBase - 86.0f * u > statX + 90.0f * u &&
                        snap.mouseX >= rightBase - 86.0f * u && snap.mouseX <= rightBase - 8.0f * u) {
                        window.pushCommandFromWndProc(boo::TransportCommand::ToggleExploreMode);
                        consumed = true;
                    }
                    // 1d. [< PRV] [PLAY/PAUS] [NEXT >] pills
                    if (!consumed && rightBase - 272.0f * u > statX + 82.0f * u) {
                        if (snap.mouseX >= rightBase - 272.0f * u && snap.mouseX <= rightBase - 220.0f * u) {
                            window.pushCommandFromWndProc(boo::TransportCommand::PrevAct);
                            consumed = true;
                        } else if (filmEnded && snap.mouseX >= rightBase - 214.0f * u && snap.mouseX <= rightBase - 92.0f * u) {
                            window.pushCommandFromWndProc(boo::TransportCommand::Restart); // [PLAY AGAIN]
                            consumed = true;
                        } else if (snap.mouseX >= rightBase - 214.0f * u && snap.mouseX <= rightBase - 162.0f * u) {
                            window.pushCommandFromWndProc(boo::TransportCommand::TogglePlayPause);
                            consumed = true;
                        } else if (snap.mouseX >= rightBase - 156.0f * u && snap.mouseX <= rightBase - 92.0f * u) {
                            window.pushCommandFromWndProc(boo::TransportCommand::NextAct);
                            consumed = true;
                        }
                    }
                }
            }

            // 1e. Clickable "SOLO: ... [ALL]" Banner (returns to Full 5-Engine Orchestra)
            if (!consumed && settings.showOverlay && (hudMode != 3u) && (soloEngine != 0u) && w >= 480.0f && h >= 300.0f) {
                const float soloX = std::max(14.0f * u, w - 245.0f * u);
                const float soloY = std::max(12.0f * u, barVisible ? (barMinY - 25.0f * u) : (h - 70.0f * u));
                if (snap.mouseX >= soloX && snap.mouseX <= soloX + 230.0f * u &&
                    snap.mouseY >= soloY && snap.mouseY <= soloY + 20.0f * u) {
                    window.pushCommandFromWndProc(boo::TransportCommand::RestoreAllEngines);
                    consumed = true;
                }
            }

            // 2. Persistent Bottom Interactive RTX Studio Dock buttons & sliders
            if (!consumed && barVisible) {
                if (snap.mouseX >= barMinX && snap.mouseX <= barMinX + barW &&
                    snap.mouseY >= barMinY && snap.mouseY <= barMinY + barH) {
                    consumed = true;
                    const float rx = (snap.mouseX - barMinX) / u;
                    const float ry = (snap.mouseY - barMinY) / u;
                    if (ry >= 4.0f && ry <= 24.0f) {
                        if (rx >= 6.0f && rx <= 56.0f) {
                            window.pushCommandFromWndProc(boo::TransportCommand::ToggleExploreMode);
                        } else if (rx >= 156.0f && rx <= 196.0f) {
                            window.pushCommandFromWndProc(boo::TransportCommand::TogglePracticalLamps);
                        } else if (rx >= 200.0f && rx <= 240.0f) {
                            window.pushCommandFromWndProc(boo::TransportCommand::ToggleRtxSplitWipe);
                        } else if (rx >= 244.0f && rx <= 284.0f) {
                            window.pushCommandFromWndProc(snap.shiftDown ? boo::TransportCommand::ResetAovMode : boo::TransportCommand::CycleAovMode);
                        } else if (rx >= 288.0f && rx <= 330.0f) {
                            window.pushCommandFromWndProc(snap.shiftDown ? boo::TransportCommand::ClearMarbles : boo::TransportCommand::SpawnMarbles);
                        } else if (rx >= 336.0f && rx <= 354.0f) {
                            window.pushCommandFromWndProc(boo::TransportCommand::PrevAct);
                        } else if (rx >= 357.0f && rx <= 444.0f) {
                            const uint32_t k = std::min<uint32_t>(5u, static_cast<uint32_t>((rx - 357.0f) / 14.5f));
                            window.pushCommandFromWndProc(static_cast<boo::TransportCommand>(
                                static_cast<int>(boo::TransportCommand::HeroPreset1) + static_cast<int>(k)));
                        } else if (rx >= 447.0f && rx <= 530.0f) {
                            window.pushCommandFromWndProc(boo::TransportCommand::NextAct);
                        }
                    } else if (ry >= 27.0f && ry <= 49.0f) {
                        if (rx >= 30.0f && rx <= 155.0f) {
                            dragTarget = DragTarget::SunSlider;
                            sunOverride = true;
                            sunElevation01 = std::clamp((snap.mouseX - (barMinX + 34.0f * u)) / (118.0f * u), 0.0f, 1.0f);
                            recomputeSunDir();
                            hasPrevState = false;
                        } else if (rx >= 182.0f && rx <= 307.0f) {
                            dragTarget = DragTarget::AzmSlider;
                            sunOverride = true;
                            sunAzimuth01 = std::clamp((snap.mouseX - (barMinX + 186.0f * u)) / (118.0f * u), 0.0f, 1.0f);
                            recomputeSunDir();
                            hasPrevState = false;
                        } else if (rx >= 310.0f && rx <= 350.0f) {
                            window.pushCommandFromWndProc(
                                snap.shiftDown ? boo::TransportCommand::OpenAudioFileDialog
                                               : boo::TransportCommand::NextMusicTrack);
                        } else if (rx >= 354.0f && rx <= 396.0f) {
                            window.pushCommandFromWndProc(boo::TransportCommand::ToggleMute);
                        } else if (rx >= 400.0f && rx <= 438.0f) {
                            window.pushCommandFromWndProc(boo::TransportCommand::ResetInteractiveLighting);
                        } else if (rx >= 442.0f && rx <= 530.0f) {
                            window.pushCommandFromWndProc(filmEnded ? boo::TransportCommand::Restart  // [PLAY AGAIN]
                                                                    : boo::TransportCommand::NextAct);
                        }
                    }
                }
            }

            // 3. Draggable RTX ON / OFF A/B Split-Screen Curtain
            if (!consumed && settings.showOverlay && hudMode == 2u) {
                const auto lb = boo::computeLetterboxViewport16x9(window.clientWidth(), window.clientHeight());
                const float splitX = lb.x + lb.width * abSplitRatio;
                if (std::abs(snap.mouseX - splitX) <= 18.0f * u &&
                    snap.mouseY >= lb.y && snap.mouseY <= lb.y + lb.height) {
                    dragTarget = DragTarget::SplitCurtain;
                    consumed = true;
                }
            }

            // 4. Viewport Alt+LMB Sun Orbit or LMB Camera Look
            if (!consumed) {
                if (snap.altDown) {
                    dragTarget = DragTarget::AltSunOrbit;
                } else if (exploreActive) {
                    dragTarget = DragTarget::ViewportLook;
                }
            }
        }

        // Ongoing slider / timeline / split-curtain drag updates
        if (snap.lmbDown) {
            if (dragTarget == DragTarget::TimelineScrub) {
                pendingScrubRatio = std::clamp((snap.mouseX - tlMinX) / std::max(1.0f, tlSizeX), 0.0f, 0.9995f);
            } else if (dragTarget == DragTarget::SplitCurtain) {
                const auto lb = boo::computeLetterboxViewport16x9(window.clientWidth(), window.clientHeight());
                abSplitRatio = std::clamp((snap.mouseX - lb.x) / std::max(1.0f, lb.width), 0.05f, 0.95f);
            } else if (dragTarget == DragTarget::SunSlider) {
                const float nextEl = std::clamp((snap.mouseX - (barMinX + 34.0f * u)) / (118.0f * u), 0.0f, 1.0f);
                if (std::abs(nextEl - sunElevation01) > 1e-4f || !sunOverride) {
                    sunOverride = true;
                    sunElevation01 = nextEl;
                    recomputeSunDir();
                    hasPrevState = false;
                }
            } else if (dragTarget == DragTarget::AzmSlider) {
                const float nextAz = std::clamp((snap.mouseX - (barMinX + 186.0f * u)) / (118.0f * u), 0.0f, 1.0f);
                if (std::abs(nextAz - sunAzimuth01) > 1e-4f || !sunOverride) {
                    sunOverride = true;
                    sunAzimuth01 = nextAz;
                    recomputeSunDir();
                    hasPrevState = false;
                }
            } else if (dragTarget == DragTarget::AltSunOrbit &&
                       (std::abs(snap.dragLmbDx) > 0.1f || std::abs(snap.dragLmbDy) > 0.1f)) {
                sunOverride = true;
                sunAzimuth01 = std::fmod(sunAzimuth01 + snap.dragLmbDx * 0.0018f + 1.0f, 1.0f);
                sunElevation01 = std::clamp(sunElevation01 - snap.dragLmbDy * 0.0022f, 0.0f, 1.0f);
                recomputeSunDir();
                hasPrevState = false;
            }
        }
    }

    bool wantsMouseExplore(const boo::WindowInputSnapshot& snap) const noexcept {
        return !exploreActive && snap.rmbDown && (std::abs(snap.dragRmbDx) > 1.5f || std::abs(snap.dragRmbDy) > 1.5f);
    }

    void updateFrame(
        const boo::WindowInputSnapshot& snap,
        const BooXInputGamepad& pad,
        bool hasPad,
        const boo::ClockSnapshot& tick,
        const boo::QualityPresetSpec& activeSpec,
        const boo::CueSheet& cueSheet,
        const boo::AppSettings& settings,
        boo::TimelineClock& clock,
        boo::AudioEngine& audio,
        bool audioClockReady,
        boo::AppWindow& window,
        boo::FrameContext& ioCtx) {
        const float wallDt = std::clamp(static_cast<float>(tick.unclampedDtSeconds), 0.0005f, 0.100f);
        const boo::ShowVisualMode showMode = boo::resolveShowVisualMode(cueSheet, settings.playbackMode);

        // Right-mouse drag anywhere in the viewport immediately enters Explore mode & orbits look!
        if (wantsMouseExplore(snap)) {
            enterExplore(showMode, clock, audio, audioClockReady, window, settings, ioCtx.moodIrisAngle);
            ioCtx.paused = (clock.state() == boo::TransportState::Paused);
        }

        bool cameraOrLightChanged = false;

        // Keyboard [ / ] sun azimuth & Shift+[ / Shift+] sun elevation nudges in Explore mode
        if (exploreActive && (std::abs(snap.sunAzimKey) > 0.01f || std::abs(snap.sunElevKey) > 0.01f)) {
            sunOverride = true;
            sunAzimuth01 = std::fmod(sunAzimuth01 + snap.sunAzimKey * wallDt * 0.25f + 1.0f, 1.0f);
            sunElevation01 = std::clamp(sunElevation01 + snap.sunElevKey * wallDt * 0.25f, 0.0f, 1.0f);
            recomputeSunDir();
            cameraOrLightChanged = true;
        }

        if (exploreActive) {
            blendT = 1.0f;

            // Mouse look (RMB drag or Viewport LMB drag)
            float lookDx = snap.dragRmbDx;
            float lookDy = snap.dragRmbDy;
            if (dragTarget == DragTarget::ViewportLook) {
                lookDx += snap.dragLmbDx;
                lookDy += snap.dragLmbDy;
            }
            if (std::abs(lookDx) > 0.01f || std::abs(lookDy) > 0.01f) {
                constexpr float kMouseSens = 0.0024f;
                yaw += lookDx * kMouseSens;
                pitch = std::clamp(pitch - lookDy * kMouseSens, -1.48f, 1.48f);
                activePreset = 0;
                cameraOrLightChanged = true;
            }

            // Wheel FOV zoom + MMB FOV reset
            if (std::abs(snap.wheelNotches) > 0.01f) {
                fovY = std::clamp(fovY - snap.wheelNotches * 0.045f, 0.30f, 1.60f);
                cameraOrLightChanged = true;
            }
            if (snap.mmbJustPressed) {
                fovY = (showMode == boo::ShowVisualMode::FullFilm360s && ioCtx.filmTimeSeconds >= 336.0) ? 0.58f : 0.76f;
                cameraOrLightChanged = true;
            }

            // Analog XInput stick look & movement
            auto deadzone = [](SHORT v, SHORT dz) noexcept -> float {
                const float fv = static_cast<float>(v);
                const float fdz = static_cast<float>(dz);
                if (std::abs(fv) <= fdz) return 0.0f;
                const float sign = (fv >= 0.0f) ? 1.0f : -1.0f;
                return sign * std::min(1.0f, (std::abs(fv) - fdz) / (32767.0f - fdz));
            };
            float moveF = snap.moveForward;
            float moveR = snap.moveRight;
            float moveU = snap.moveUp;
            if (hasPad && snap.hasFocus) {
                const float lx = deadzone(pad.sThumbLX, 7849);
                const float ly = deadzone(pad.sThumbLY, 7849);
                const float rx = deadzone(pad.sThumbRX, 8689);
                const float ry = deadzone(pad.sThumbRY, 8689);
                const float trig = (static_cast<float>(pad.bRightTrigger) - static_cast<float>(pad.bLeftTrigger)) / 255.0f;
                moveR = std::clamp(moveR + lx, -1.0f, 1.0f);
                moveF = std::clamp(moveF + ly, -1.0f, 1.0f);
                if (std::abs(trig) > 0.08f) moveU = std::clamp(moveU + trig, -1.0f, 1.0f);
                if (std::abs(rx) > 0.01f || std::abs(ry) > 0.01f) {
                    yaw += rx * 2.1f * wallDt;
                    pitch = std::clamp(pitch + ry * 1.6f * wallDt, -1.48f, 1.48f);
                    activePreset = 0;
                    cameraOrLightChanged = true;
                }
            }

            if (std::abs(moveF) > 0.001f || std::abs(moveR) > 0.001f || std::abs(moveU) > 0.001f) {
                const bool inMetricRoom = (showMode == boo::ShowVisualMode::FullFilm360s && ioCtx.filmTimeSeconds >= 335.5);
                float speed = inMetricRoom ? 1.65f : 4.60f;
                if (snap.shiftDown) speed *= 2.85f;
                if (snap.altDown)   speed *= 0.22f;

                const float cp = std::cos(pitch);
                const float fwd[3] = {std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp};
                const float rgt[3] = {std::cos(yaw), 0.0f, -std::sin(yaw)};

                eye[0] += (fwd[0] * moveF + rgt[0] * moveR) * speed * wallDt;
                eye[1] += (fwd[1] * moveF + moveU) * speed * wallDt;
                eye[2] += (fwd[2] * moveF + rgt[2] * moveR) * speed * wallDt;
                activePreset = 0;
                cameraOrLightChanged = true;
            }
        } else if (blendT > 0.0f) {
            blendT = std::max(0.0f, blendT - wallDt / 0.65f);
            cameraOrLightChanged = true;
        }

        const bool inDawnBathroom = (showMode == boo::ShowVisualMode::FullFilm360s && ioCtx.filmTimeSeconds >= 335.5);
        if (marbleCount > 0u) {
            stepMarbles(wallDt, inDawnBathroom);
        }

        const float smoothBlend = blendT * blendT * (3.0f - 2.0f * blendT);
        const float cp = std::cos(pitch);
        const float fwd[3] = {std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp};

        ioCtx.interactive.exploreActive = exploreActive;
        ioCtx.interactive.blendWeight = smoothBlend;
        for (int c = 0; c < 3; ++c) {
            ioCtx.interactive.eye[c] = eye[c];
            ioCtx.interactive.target[c] = eye[c] + fwd[c] * 2.5f;
            ioCtx.interactive.up[c] = (c == 1) ? 1.0f : 0.0f;
            ioCtx.interactive.sunDir[c] = sunDir[c];
        }
        ioCtx.interactive.fovYRadians = fovY;
        ioCtx.interactive.sunOverride = sunOverride;
        ioCtx.interactive.sunScale = sunScale;
        ioCtx.interactive.sunElevation01 = sunElevation01;
        ioCtx.interactive.sunAzimuth01 = sunAzimuth01;
        ioCtx.interactive.lampsOff = lampsOff;
        ioCtx.interactive.abSplitRatio = abSplitRatio;
        ioCtx.interactive.activePreset = activePreset;
        ioCtx.interactive.aovMode = aovMode;
        audio.setExploreAmbient(exploreActive);
        const uint32_t curAudioSlot = audio.activeSoundtrackSlot();
        if (!settings.noActivateWindow) {
            if (lastAudioTrackSlot == 0xFFFFFFFFu) {
                lastAudioTrackSlot = curAudioSlot;
                audioToastTimer = 4.2f;
            } else if (curAudioSlot != lastAudioTrackSlot) {
                lastAudioTrackSlot = curAudioSlot;
                audioToastTimer = 3.8f;
            } else {
                audioToastTimer = std::max(0.0f, audioToastTimer - wallDt);
            }
        }
        ioCtx.interactive.soloEngine = soloEngine;
        ioCtx.interactive.studioDrawerOpen = studioDrawerOpen;
        ioCtx.interactive.customAudioActive = audio.hasExternalAudioFile();
        ioCtx.interactive.audioMuted = audio.isMuted();
        ioCtx.interactive.audioTrackSlot = curAudioSlot;
        ioCtx.interactive.audioToastAlpha = (!settings.noActivateWindow)
                                                ? std::clamp(audioToastTimer / 0.65f, 0.0f, 1.0f)
                                                : 0.0f;
        ioCtx.interactive.marbleCount = marbleCount;
        for (uint32_t m = 0; m < marbleCount && m < boo::InteractiveView::kMaxMarbles; ++m) {
            ioCtx.interactive.marblePosRad[m][0] = marbles[m].pos[0];
            ioCtx.interactive.marblePosRad[m][1] = marbles[m].pos[1];
            ioCtx.interactive.marblePosRad[m][2] = marbles[m].pos[2];
            ioCtx.interactive.marblePosRad[m][3] = marbles[m].radius;
            for (int c = 0; c < 3; ++c) ioCtx.interactive.marblePrevPos[m][c] = marbles[m].prevPos[c];
            ioCtx.interactive.marblePrevPos[m][3] = 0.0f;
            for (int c = 0; c < 4; ++c) ioCtx.interactive.marbleRotQuat[m][c] = marbles[m].rotQuat[c];
            ioCtx.interactive.marbleMat[m] = marbles[m].matId;
        }

        // Live NRD REBLUR motion denoising during flight/sun-scrub/rolling-marbles,
        // handing off seamlessly to NRD REFERENCE progressive path-tracing (1->4096 SPP) when still!
        const bool interactiveSession = exploreActive || sunOverride || lampsOff || (marbleCount > 0u);
        const bool liveMotionActive = cameraOrLightChanged || marblesMoving;
        const bool sceneInvalidated =
            !hasPrevState ||
            ioCtx.seeking ||
            !ioCtx.paused ||
            ioCtx.samplePosition != prevSamplePos ||
            activeSpec.renderWidth != prevCanvasW ||
            activeSpec.renderHeight != prevCanvasH ||
            settings.disableRT != prevDisableRt ||
            settings.edition != prevEdition;

        if (interactiveSession && ioCtx.paused && !ioCtx.seeking && !settings.disableRT && (aovMode == 0u) && !liveMotionActive) {
            ioCtx.interactive.accumulate = true;
            if (sceneInvalidated) {
                accumFrame = 0u;
                hasPrevState = true;
            } else {
                accumFrame = std::min<uint32_t>(accumFrame + 1u, 4095u);
            }
            ioCtx.interactive.accumFrame = accumFrame;
        } else {
            ioCtx.interactive.accumulate = false;
            ioCtx.interactive.accumFrame = 0u;
            accumFrame = 0u;
            hasPrevState = false;
        }

        prevSamplePos = ioCtx.samplePosition;
        prevCanvasW = activeSpec.renderWidth;
        prevCanvasH = activeSpec.renderHeight;
        prevDisableRt = settings.disableRT;
        prevEdition = settings.edition;
    }
};

// Runs slow graphics work on a thread of its own while this thread keeps answering the window. A close
// asked for meanwhile hides the window; it is destroyed after the work is done.
template <class Work> void runKeepingWindowAlive(boo::AppWindow& window, Work&& work) {
    std::exception_ptr failure;
    std::atomic<bool> done{false};
    window.setBusy(true);
    std::thread worker([&] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        try { work(); } catch (...) { failure = std::current_exception(); }
        if (SUCCEEDED(com)) CoUninitialize();
        done.store(true);
    });
    while (!done.load()) {
        (void)window.pumpMessages();
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    worker.join();
    window.setBusy(false);
    if (failure) std::rethrow_exception(failure);
}

} // namespace

// Bird camera: follows the flock from above and behind its centre, framing all of it, and turns
// slowly round it. It drives the same camera override Explore uses, so the film, music and
// lighting carry on underneath. Explore always wins while it is active.
struct BirdCamera {
    float blend = 0.0f;   // 0 = film camera, 1 = bird camera
    float eye[3]{}, target[3]{};
    bool placed = false;

    void apply(bool wanted, const boo::FlockFocus& focus, float wallDt, boo::FrameContext& ctx) noexcept {
        const bool exploreHasCamera = ctx.interactive.exploreActive || ctx.interactive.blendWeight > 0.0f;
        if (exploreHasCamera) { blend = 0.0f; placed = false; return; }
        const bool follow = wanted && focus.valid;
        blend = std::clamp(blend + (follow ? wallDt / 0.9f : -wallDt / 0.6f), 0.0f, 1.0f);
        if (focus.valid) {
            constexpr float kFov = 0.82f;
            // White egrets read best from well above, against the city. Dark kites vanish against
            // it, so they are watched from nearer their own height, against the sky, as a kettle is.
            const float kElevation = focus.species == 1u ? 0.62f : 0.14f;
            const float distance = std::clamp(focus.radius / std::tan(kFov * 0.5f) * 1.05f + 3.0f, 10.0f, 80.0f);
            const float azimuth = 0.9f + 0.07f * static_cast<float>(ctx.filmTimeSeconds);
            const float want[3] = {focus.center[0] + distance * std::sin(azimuth) * std::cos(kElevation),
                                   focus.center[1] + distance * std::sin(kElevation),
                                   focus.center[2] + distance * std::cos(azimuth) * std::cos(kElevation)};
            float jump = 0.0f;
            for (int c = 0; c < 3; ++c) jump += (want[c] - eye[c]) * (want[c] - eye[c]);
            // A new act or a seek moves the flock across the city: cut, do not fly through buildings.
            const float k = (!placed || jump > 45.0f * 45.0f) ? 1.0f : 1.0f - std::exp(-wallDt * 3.5f);
            for (int c = 0; c < 3; ++c) {
                eye[c] += (want[c] - eye[c]) * k;
                target[c] += (focus.center[c] - target[c]) * k;
            }
            placed = true;
        }
        if (blend <= 0.0f || !placed) return;
        ctx.interactive.blendWeight = blend * blend * (3.0f - 2.0f * blend);
        for (int c = 0; c < 3; ++c) {
            ctx.interactive.eye[c] = eye[c];
            ctx.interactive.target[c] = target[c];
            ctx.interactive.up[c] = (c == 1) ? 1.0f : 0.0f;
        }
        ctx.interactive.fovYRadians = 0.82f;
    }
};

#ifdef BOO_HAS_LINALG
// Load the bundled D3D12 Agility SDK runtime (preview 1.721, Shader Model 6.10) from .\D3D12\.
// Only test-rig builds configured with ENABLE_LINALG_PREVIEW carry it; Store builds use the in-box runtime.
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 721; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }
#endif

#ifdef BOO_VERIFY_FILM_BENCHMARK
namespace boo { int verifyFilmBenchmarkIfRequested(int argc, char** argv); }
#endif

int main(int argc, char** argv) {
    const auto viewerEntry = std::chrono::steady_clock::now();
    using namespace boo;

    if (const int workerResult = runResultsWorkerIfRequested(argc, argv); workerResult >= 0) return workerResult;
#ifdef BOO_VERIFY_FILM_BENCHMARK
    if (const int verification = verifyFilmBenchmarkIfRequested(argc, argv); verification >= 0) return verification;
#endif

    std::vector<std::string> rawArgs;
    bool isCliQuery = false;
    for (int i = 1; i < argc; ++i) {
        rawArgs.emplace_back(argv[i]);
        const std::string& a = rawArgs.back();
        if (a == "--help" || a == "-h" || a == "--validate-cues" || a == "--bench" || a == "--headless-probe") {
            isCliQuery = true;
        }
    }
    if (isCliQuery) {
        const HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        const bool hasPipeOrFile = (hOut != nullptr && hOut != INVALID_HANDLE_VALUE && GetFileType(hOut) != FILE_TYPE_UNKNOWN);
        if (!hasPipeOrFile && AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fOut = nullptr;
            FILE* fErr = nullptr;
            (void)freopen_s(&fOut, "CONOUT$", "w", stdout);
            (void)freopen_s(&fErr, "CONOUT$", "w", stderr);
        }
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
        const auto repoRoot = std::filesystem::absolute(resolvedManifest).parent_path().parent_path();
        if (loadAssetManifestFromFile(resolvedManifest, manifest, manifestRep)) {
            verifyManifestAssetFiles(manifest, repoRoot, manifestRep);
        }
        if (!manifestRep.valid) {
            std::cerr << "[FAIL] Asset manifest validation failed (" << resolvedManifest.string() << "):\n";
            for (const auto& iss : manifestRep.issues) {
                std::cerr << "  - [" << iss.code << "] " << iss.message << "\n";
            }
            return 1;
        }
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
            const auto resolvedCue = repoRoot / cf;
            CueSheet sheet{};
            ValidationReport rep{};
            loadCueSheetFromFile(resolvedCue, sheet, rep);
            if (rep.valid) {
                validateCueSheetAgainstManifest(sheet, manifest, {}, rep);
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

    // The window and its loading screen come before anything slow, so a first launch never shows nothing.
    // Start-up works for seconds at a time without reading its message queue; keep Windows from swapping
    // in a frozen "Not Responding" copy of the window, which would hide the loading screen.
    // On-screen language. "auto" follows the Windows display language; timed automation runs stay
    // English so their frames do not depend on the PC they run on.
    {
        // Found beside the exe when the working directory is elsewhere (Start menu, Store package).
        const std::filesystem::path stringsDir = resolveResourcePath(settings.stringsDir);
        std::string lang = settings.language;
        if (lang.empty() || lang == "auto") {
            lang = (settings.autoExitSeconds > 0.0)
                ? std::string("en-US")
                : uiMatchLanguage(uiWindowsPreferredLanguages(), uiAvailableLanguages(stringsDir));
        }
        std::string langError;
        if (!UiStrings::instance().load(stringsDir, lang, &langError)) {
            BOO_LOG_WARN("UI", "On-screen text stays English: " + langError);
        } else {
            BOO_LOG_INFO("UI", "On-screen language: " + UiStrings::instance().languageTag());
        }
    }
    AppWindow window;
    if (!settings.runBenchCliOnly && !settings.headlessProbeOnly) {
        DisableProcessWindowsGhosting();
        if (!window.create(settings.title, settings.width, settings.height, settings.fullscreen, settings.noActivateWindow)) {
            BOO_LOG_ERROR("Window", "Failed to create Win32 application window.");
            return 1;
        }
        window.showLoading();
        (void)window.pumpMessages();

        // A real loading bar. It counts bytes: every asset read into memory, each checked against its
        // SHA-256 in the manifest, and GPU memory as it fills. Loaders and start-up stages report in.
        StartupProgress::Setup progress;
        progress.memoryFile = startupMemoryFile();
        for (const char* folder : {"assets/meshes", "assets/textures", "assets/shaders", "assets/neural"}) {
            progress.assetFolders.push_back(resolveResourcePath(folder));
        }
        for (const auto& asset : manifest.assets) progress.digests.emplace(asset.path, asset.sha256);
        progress.buildId = startupBuildId();
        progress.onChange = [&window](const StartupProgressView& v) {
            window.setLoadingProgress(v.fraction, loadingDetail(v), loadingClock(v.seconds),
                                      v.firstStart ? uiText(UiText::LoadingFirstStart) : L"", loadingLines(v));
        };
        StartupProgress::instance().begin(std::move(progress));
        // While start-up waits on other threads, the window keeps answering (it can be moved or closed).
        StartupProgress::instance().setPump([&window] { (void)window.pumpMessages(); });
        startupCheckpoint();
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

    // Tensor-core neural rendering (ENABLE_LINALG_PREVIEW builds) must opt in before any device exists.
    enableNeuralPreviewFeatures();
    SystemCapabilities probed{};
    runWhilePainting(StartupWork::Device, "hardware check", [&] { probed = probeSystemCapabilities(pflags); });
    const SystemCapabilities& caps = probed;
    const auto cardHardware = resultsHostHardware(caps);
    FallbackOrchestrator orchestrator;
    for (size_t i = 0; i < kEngineKindCount; ++i) {
        orchestrator.registerEngineState(caps.engines[i]);
    }

    // CLI-only ad-hoc numerical benchmark mode (--bench)
    if (settings.runBenchCliOnly) {
        D3D12Context headlessD3D;
        headlessD3D.preferLowPowerAdapter(settings.lowPowerGpu);
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
        window.destroy();
        if (!settings.noActivateWindow && settings.autoExitSeconds <= 0.0) {
            ResultsDialogContext unavailable{};
            unavailable.hardware = cardHardware;
            unavailable.closeOnly = true;
            unavailable.initialFailure = BenchmarkReason::UnsupportedDevice;
            openResultsDialog(nullptr, unavailable);
        }
        return 1;
    }

    const auto titleWord = [](const char* key, const wchar_t* english) {
        const std::wstring w = UiStrings::instance().textOr(key, english);
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(std::max(n, 0)), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    };
    const std::string titleAct = titleWord("win.act", L"Act");
    const std::string titleExplore = titleWord("win.explore", L"Explore");
    const std::string titleWipe = titleWord("win.wipe", L"Wipe");
    const std::string titleLamp = titleWord("win.lamp", L"Lamp");
    const std::string titleDetail = titleWord("win.effects_detail", L"Effects detail");

    // 3. Initialize 48 kHz WASAPI Audio Engine or silent monotonic clock fallback
    AudioEngine audio;
    bool audioClockReady = false;
    if (!settings.forceSilentClock) {
        // Decode during renderer warm-up while keeping the soundtrack at its opening sample.
        audio.setPaused(true);
        audio.setMasterVolume(settings.masterVolume);
        audio.setMuted(settings.mute);
        audio.setEdition(settings.edition);
        audio.setDurationSamples(cueSheet.durationSamples);
        startupStage(StartupWork::Scene, "audio");
        audioClockReady = audio.initialize(settings.injectAudioFail);
        if (!audioClockReady) {
            BOO_LOG_WARN("Audio", audio.fallbackReason() + " -> Switching to monotonic silence clock.");
        } else {
            BOO_LOG_INFO("Audio", "48 kHz stereo float audio active on '" + audio.endpointDescription() + "'.");
        }
    } else {
        BOO_LOG_INFO("Audio", "Using monotonic silence-fallback clock (--force-silent-clock).");
    }

    // Activate built-in 4-track original soundtrack (or optional --audio-file override) with zero main-thread stall
    if (!settings.forceSilentClock && !settings.injectAudioFail) {
        std::filesystem::path startupAudioPath;
        if (!settings.audioInputPath.empty()) {
            startupAudioPath = std::filesystem::exists(settings.audioInputPath)
                                   ? std::filesystem::path(settings.audioInputPath)
                                   : resolveResourcePath(settings.audioInputPath);
        } else if (!settings.noActivateWindow && settings.autoExitSeconds <= 0.0) {
            const std::filesystem::path candRoot = resolveResourcePath("soundtrack.mp3");
            if (std::filesystem::exists(candRoot)) {
                startupAudioPath = candRoot;
            }
        }
        if (!startupAudioPath.empty()) {
            std::string audErr;
            if (audio.loadExternalAudioFile(startupAudioPath, &audErr)) {
                BOO_LOG_INFO("Audio", "Loaded custom 48 kHz soundtrack: '" + audio.externalAudioLabel() + "'");
            } else {
                BOO_LOG_WARN("Audio", "Custom soundtrack fallback to built-in OST: " + audErr);
            }
        }
        if (!audio.hasExternalAudioFile()) {
            const std::array<std::filesystem::path, AudioEngine::kBundledTrackCount> ostPaths = {
                resolveResourcePath("assets/audio/01_neon_orchid.flac"),
                resolveResourcePath("assets/audio/02_acid_lotus.flac"),
                resolveResourcePath("assets/audio/03_velvet_cipher.flac"),
                resolveResourcePath("assets/audio/04_glass_dragon.flac")
            };
            audio.startBundledSoundtrack(ostPaths, 1u);
            if (audio.hasBundledSoundtrack()) {
                BOO_LOG_INFO("Audio", "Built-in 4-track OST active: '" + audio.activeSoundtrackTitle() +
                                      "' (click [TRK1] or press Z to switch tracks)");
            }
        }
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
    QualityPresetSpec startupSpec = findQualityPresetSpec(presets, qualityLadder.currentPreset());
    AsyncDirectorController asyncDirector;
    asyncDirector.start(
        cueSheet.seed,
        resolveResourcePath("assets/models/madame_n_director.xml"),
        settings.disableNPU || !startupSpec.allowNpu,
        settings.injectNpuTimeout,
        !settings.forceWarp);

    // 4. Triple-buffered D3D12 swap chain on the window created above
    (void)window.pumpMessages();
    if (window.closeRequested()) { // closed while loading
        asyncDirector.shutdown();
        audio.shutdown();
        return 0;
    }

    // Graphics setup runs on a loader thread while this thread only paints and answers the window: no
    // driver call, however long it takes, can make the window look frozen.
    D3D12Context d3d;
    std::string d3dInitErr;
    d3d.preferLowPowerAdapter(settings.lowPowerGpu);
    bool d3dReady = false;
    // Read here: the window's thread may change these while the loader works.
    const HWND hwnd = window.hwnd();
    const uint32_t clientW = window.clientWidth(), clientH = window.clientHeight();
    runWhilePainting(StartupWork::Device, "Direct3D 12", [&] {
        d3dReady = d3d.initialize(hwnd, clientW, clientH, settings.forceWarp, settings.enableD3DDebug, &d3dInitErr);
    });
    if (window.closeRequested()) {
        StartupProgress::instance().cancel();
        asyncDirector.shutdown();
        d3d.shutdown();
        audio.shutdown();
        return 0;
    }
    if (!d3dReady) {
        BOO_LOG_ERROR("D3D12", "D3D12 initialization failed: " + d3dInitErr);
        StartupProgress::instance().cancel();
        asyncDirector.shutdown();
        window.destroy();
        audio.shutdown();
        if (!settings.noActivateWindow && settings.autoExitSeconds <= 0.0) {
            ResultsDialogContext unavailable{};
            unavailable.hardware = cardHardware;
            unavailable.closeOnly = true;
            unavailable.initialFailure = BenchmarkReason::RendererFailed;
            openResultsDialog(nullptr, unavailable);
        }
        return 1;
    }
    StartupProgress::instance().setGpuProbe([&d3d] { return d3d.queryEstimatedAppVramBytes(); });
    // Integrated graphics share the PC's memory and are many times slower. The standard preset runs them at a
    // few frames a second and building ray tracing takes minutes on a first start, so they begin light.
    if (d3d.integratedAdapter() && !settings.presetFromCommandLine) {
        d3d.setRayTracingAllowed(false);
        qualityLadder.setPreset(QualityPresetId::Safe);
        startupSpec = findQualityPresetSpec(presets, QualityPresetId::Safe);
        BOO_LOG_INFO("Render", "Integrated graphics: starting light (Safe preset, raster lighting).");
    }
    // An AMD card hung on the ray-traced city (Radeon Pro V710: driver timeout, tested on 1.1.13): until ray
    // tracing is proven there, AMD graphics start with raster lighting. --preset still asks for everything.
    if (!d3d.integratedAdapter() && d3d.adapterVendorId() == 0x1002 && (!settings.presetFromCommandLine || settings.disableRT)) {
        d3d.setRayTracingAllowed(false);
        BOO_LOG_INFO("Render", "AMD graphics: starting with raster lighting.");
    }
    if (settings.coldShaderCache) {
        std::string reason;
        if (d3d.bypassShaderCaches(&reason)) BOO_LOG_INFO("D3D12", "Shader caches bypassed: every pipeline compiles as on a first start.");
        else BOO_LOG_WARN("D3D12", "--cold-shader-cache unavailable: " + reason);
    }
    // The PhysX suite's shared runtime. It starts in the background the first time a genre needs it.
    PhysxSuite physxRuntime(d3d.adapterLuidLow(), d3d.adapterLuidHigh());
    std::unique_ptr<PhysxRigidGenre> physxRigid; // exists only while the rigid genre is dialled in
    std::unique_ptr<PhysxMarbles> physxMarbles;  // the Explore marbles' scene, once the runtime is ready
    std::unique_ptr<PhysxSoftGenre> physxSoft;   // exists only while the soft-body genre is dialled in
    std::unique_ptr<PhysxFluidGenre> physxFluid; // likewise for fluids
    if (physxGenreInfo(settings.physxGenre).usesPhysxSdk) physxRuntime.requestStart(); // loads beside the scene; see below
    startupStep("D3D12 device, Streamline and swap chain");
    startupCheckpoint();

    (void)window.pumpMessages();

    // Live GPU sensors (NVML via nvml.dll at runtime); N/A on non-NVIDIA GPUs.
    GpuSensors gpuSensors;
    if (!gpuSensors.start(d3d.adapterName(), d3d.adapterLuidLow(), d3d.adapterLuidHigh(), !settings.disableCUDA && !d3d.isWarp())) {
        BOO_LOG_INFO("Sensors", "GPU sensors unavailable: " + gpuSensors.unavailableReason());
    }
    startupStep("GPU sensors");

    // 5. Initialize cue-routed primary + embedded-safe-fallback visual effects
    M1TaipeiSliceEffect* liveM1Ptr = nullptr;
    HardwareResult visualInit{};
    runWhilePainting(StartupWork::Scene, {}, [&] {
        liveM1Ptr = createAndRegisterShowEffects(d3d, settings, cueSheet, startupSpec, orchestrator);
        startupStep("show effects created");
        startupCheckpoint();
        visualInit = orchestrator.initializeEffects(false);
    });
    startupStep("show effects initialised");
    startupCheckpoint();
    if (settings.physxRouteTest) physxRuntime.requestStart();
    if (!window.closeRequested() && (physxGenreInfo(settings.physxGenre).usesPhysxSdk || settings.physxRouteTest)) {
        // Opening on a PhysX genre: the runtime loaded alongside the scene; let it finish inside the
        // loading screen, so its libraries are on the bar and the first frame does not wait for them.
        runWhilePainting(StartupWork::Scene, "PhysX", [&] { physxRuntime.waitUntilSettled(30000); });
        startupCheckpoint();
    }
    startupStage(StartupWork::Scene); // runs until the first frame is on screen
    (void)window.pumpMessages();
    BOO_LOG_INFO("Render", std::string("Active visual backend: ") + orchestrator.activeEffectName() +
                           " [" + visualInit.diagnosticCode + "]");
    gpuSensors.resetSummary(); // Run statistics cover playback, not loading

    SpoutSender spoutSender;
    if (settings.enableSpout && !window.closeRequested()) {
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
    if (window.closeRequested()) {
        // Closed while loading: open no camera or file for a film nobody will see.
    } else if (settings.enableCameraInput && (!liveM1Ptr || orchestrator.isUsingFallbackEffect() || settings.physxRouteTest)) {
        settings.enableCameraInput = false;
        BOO_LOG_WARN("Media", "Camera left off: its LIVE CAMERA label cannot be shown here.");
    } else if (settings.enableCameraInput) {
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
        const bool rayTracingOff = !d3d.rayTracingAllowed(); // a light start measures without ray tracing too
        benchThread = std::thread([&, benchSettings, benchDevice, benchLuidLow, benchLuidHigh, benchAdapter, rayTracingOff]() {
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
            bcfg.disableRT = benchSettings.disableRT || rayTracingOff;
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
    int64_t prerollAudioSamples = 0;
    if (audioClockReady && audio.readPlayedSamples48k(prerollAudioSamples)) {
        BOO_LOG_INFO("Audio", "Timeline samples presented during graphics warm-up: " + std::to_string(prerollAudioSamples));
    }
    clock.start(audioClockReady, settings.loop || isSoloOrLoopMode);
    if(settings.startSeconds>0) { clock.seekToSeconds(settings.startSeconds); if(audioClockReady)audio.resetPhase(static_cast<uint64_t>(clock.currentSample())); }
    if (audioClockReady && !window.closeRequested()) audio.setPaused(false);
    const int64_t initialSample = clock.currentSample();
    size_t lastActIdx = SIZE_MAX;
    orchestrator.onSceneBoundary(0, clock.currentSeconds(), false);

    if (settings.runBenchOnStart && !window.closeRequested()) {
        startBackgroundBenchSweep();
    }

    const auto wallStart = std::chrono::steady_clock::now();
    auto lastTitleUpdate = wallStart;
    std::vector<double> cpuFrameSamplesMs;
    std::vector<double> gpuFrameSamplesMs;
    cpuFrameSamplesMs.reserve(4096);
    gpuFrameSamplesMs.reserve(4096);

    uint64_t peakVramBytes = 0;
    uint32_t maxFramesPresentedPerRender = 0;
    uint64_t frameCounter = 0;
    AudioReactiveFollower audioReactive;
    double smoothFps = 60.0;
    double lastGpuMs = 0.25;
    bool appliedBenchBadges = false;
    bool running = true;
    bool recoveryAttempted = false;
    bool graphicsUnavailable = false;
    std::optional<double> viewerStartupSeconds;
    uint64_t viewerLocalUsage = 0;
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
    uint32_t hudBeforeComparison = hudMode;
    AbCompareEngine abCompareEngine = AbCompareEngine::RT;
    struct NpuViewReturn {
        PlaybackMode mode;
        AbCompareEngine comparison;
        uint32_t hud, solo, aov;
        float split, cameraBlend;
        int64_t sample;
        SoloLoopRange loopRange{};
        bool loop, customRange, paused, ended, exploring;
    };
    std::optional<NpuViewReturn> npuViewReturn;
    InteractiveExploreController interactiveCtrl{};
    BirdCamera birdCamera{};
    // The workout dial releases birds in batches and never takes the frame rate under the floor.
    WorkoutGovernor workoutGovernor{};
    uint32_t workoutLimit = kMaxWorkoutBirds; // lowered if the machine cannot hold that many
    uint32_t workoutLoggedBirds = 0;
    uint32_t workoutTraceBirds = 0;
    bool workoutLoggedFloor = false;
    std::optional<std::chrono::steady_clock::time_point> previousFrameStart;
    interactiveCtrl.studioDrawerOpen = !settings.noActivateWindow;
    interactiveCtrl.recomputeSunDir();
    if (settings.initialExplorePreset >= 1u && settings.initialExplorePreset <= 6u) {
        interactiveCtrl.applyHeroPreset(
            settings.initialExplorePreset, cueSheet, settings, clock, audio, audioClockReady, orchestrator, window);
    } else if (settings.startInExploreMode) {
        interactiveCtrl.enterExplore(
            resolveShowVisualMode(cueSheet, settings.playbackMode), clock, audio, audioClockReady, window, settings, 0.5f);
    }

    auto leaveNpuView = [&]() {
        if (!npuViewReturn) return;
        const auto previous = *npuViewReturn;
        npuViewReturn.reset();
        hudMode = previous.hud;
        settings.showOverlay = hudMode != 3u;
        settings.playbackMode = previous.mode;
        abCompareEngine = previous.comparison;
        orchestrator.setAbCompareEngine(abCompareEngine);
        interactiveCtrl.soloEngine = previous.solo;
        interactiveCtrl.aovMode = previous.aov;
        interactiveCtrl.abSplitRatio = previous.split;
        interactiveCtrl.exploreActive = previous.exploring;
        interactiveCtrl.blendT = previous.cameraBlend;
        window.setExploreKeyCapture(previous.exploring);
        modeRange = previous.loopRange;
        if (previous.customRange) clock.setLoopRange(modeRange.startSample, modeRange.endSample);
        else clock.clearLoopRange();
        clock.setLoopEnabled(previous.loop);
        clock.seekToSample(previous.sample);
        if (previous.paused) clock.pause();
        else clock.resume();
        if (audioClockReady) {
            audio.resetPhase(static_cast<uint64_t>(previous.sample));
            audio.setExploreAmbient(previous.exploring);
            audio.setPaused(previous.paused || previous.ended);
        }
        interactiveCtrl.hasPrevState = false;
        orchestrator.resetTo(clock.currentSeconds());
    };
    auto openComparison = [&](AbCompareEngine engine) {
        if (npuViewReturn && engine != AbCompareEngine::NPU) leaveNpuView();
        if (hudMode != 2u) hudBeforeComparison = hudMode;
        hudMode = 2u;
        settings.showOverlay = true;
        abCompareEngine = engine;
        orchestrator.setAbCompareEngine(engine);
        BOO_LOG_INFO("Render", std::string("A/B comparison: ") + toString(engine));
    };
    auto closeComparison = [&]() {
        if (npuViewReturn) {
            leaveNpuView();
        } else {
            hudMode = hudBeforeComparison;
            settings.showOverlay = hudMode != 3u;
        }
        BOO_LOG_INFO("Render", "Closed A/B comparison; HUD=" + std::to_string(hudMode));
    };
    auto applyAllEnginesRestored = [&]() {
        leaveNpuView();
        interactiveCtrl.soloEngine = 0u;
        settings.disableRT = false;
        settings.disableCUDA = false;
        settings.disableTensor = false;
        settings.disableNVENC = false;
        settings.enableSignalHijack = false;
        orchestrator.setDisableRt(false);
        orchestrator.setDisableCuda(false);
        orchestrator.setDisableTensor(false);
        orchestrator.setDisableCodec(false);
        orchestrator.setCodecSignalHijackActive(false);
        settings.playbackMode = PlaybackMode::FullFilm;
        clock.clearLoopRange();
        clock.setLoopEnabled(settings.loop);
        interactiveCtrl.hasPrevState = false;
    };

    // Neural settings may ask for networks that were never built here (the command line, or a recovery onto a
    // GPU without them): turn them off, so no heading or solo caption shows a suite that is not running.
    const auto dropUnavailableNeural = [&] {
        if ((settings.neural.enabled || settings.neural.solo != NeuralSolo::None) &&
            !(liveM1Ptr && liveM1Ptr->neuralRenderingStats().available)) {
            settings.neural.enabled = false;
            settings.neural.solo = NeuralSolo::None;
        }
    };

    // A camera runs only while its red LIVE CAMERA label can be drawn: by the film renderer.
    // It is drawn only on a visible window of at least 320 x 200 pixels.
    const auto cameraLabelAvailable = [&] {
        return liveM1Ptr && !orchestrator.isUsingFallbackEffect() && !graphicsUnavailable && !window.isMinimized() &&
               window.clientWidth() >= 320u && window.clientHeight() >= 200u && !settings.physxRouteTest;
    };
    const auto stopCamera = [&](const char* why) {
        if (!mediaIngest.isLiveCameraActive()) return;
        mediaIngest.shutdown();
        settings.enableCameraInput = false;
        if (liveM1Ptr) liveM1Ptr->clearExternalNv12Frame();
        BOO_LOG_INFO("Media", std::string("LIVE CAMERA STOPPED: ") + why);
    };

    // One recovery policy covers loss during resize, fence waits, resource sync,
    // and Present. The 48 kHz transport is deliberately never stopped or rebased.
    auto recoverGraphics = [&](const std::string& cause) {
        const bool mayRecover = !recoveryAttempted;
        recoveryAttempted = true;
        BOO_LOG_WARN("D3D12", "Graphics lifecycle failure: " + cause);
        // A loss before the first frame leaves the loading screen: recovery must neither pump window messages
        // from inside pipeline builds nor leave the window in its loading state.
        StartupProgress::instance().cancel();
        window.hideLoading();
        stopCamera("graphics are being restarted.");
        benchCancel.store(true, std::memory_order_release);
        if (benchThread.joinable()) benchThread.join();
        benchCompleted.store(false, std::memory_order_release);
        retiredPresentCount += d3d.presentCount();
        retiredPresentStalls += d3d.presentStallCount();
        retiredDebugErrors += d3d.debugErrorCount();
        retiredDebugWarnings += d3d.debugWarningCount();
        const HWND recoveryWindow = window.hwnd();
        const uint32_t recoveryW = window.clientWidth(), recoveryH = window.clientHeight();
        std::string recoveryError = mayRecover ? std::string{} : "The single graphics recovery attempt was already used.";
        bool rebuilt = false;
        // Releasing a lost device and building a new one can take many seconds: another thread does it while
        // this one keeps answering the window.
        const auto rebuild = [&] {
        // Retire resources before release on non-removal failures as well.
        d3d.retireGpuForShutdown();
        // Whatever the card, come back without ray tracing: it is the heaviest work and the likeliest cause.
        d3d.setRayTracingAllowed(false);
        spoutSender.shutdown();
        liveM1Ptr = nullptr;
        orchestrator.shutdown();
        d3d.shutdown();
        if (mayRecover && recoveryWindow && !window.closeRequested()) {
            BOO_LOG_WARN("D3D12", "Attempting single D3D12 device recovery with bounded fence waits...");
            const auto spec = findQualityPresetSpec(presets, qualityLadder.currentPreset());
            if (d3d.initialize(recoveryWindow, recoveryW, recoveryH,
                               settings.forceWarp, settings.enableD3DDebug, &recoveryError)) {
                liveM1Ptr = createAndRegisterShowEffects(d3d, settings, cueSheet, spec, orchestrator);
                const auto result = orchestrator.initializeEffects(false);
                if (result.status != BackendStatus::Faulted && result.status != BackendStatus::Unavailable &&
                    d3d.checkDeviceHealthy(&recoveryError)) {
                    dropUnavailableNeural();
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
                    rebuilt = true;
                    return;
                }
                if (recoveryError.empty()) recoveryError = "Neither primary nor safe graphics could initialize.";
            }
        }
        d3d.retireGpuForShutdown();
        liveM1Ptr = nullptr;
        orchestrator.shutdown();
        d3d.shutdown();
        };
        try {
            runKeepingWindowAlive(window, rebuild);
        } catch (const std::exception& e) {
            recoveryError = e.what();
            rebuilt = false;
            liveM1Ptr = nullptr;
            orchestrator.shutdown();
            d3d.shutdown();
        }
        if (window.closeRequested()) { // closed while recovering: nothing more to draw or explain
            graphicsUnavailable = graphicsUnavailable || !rebuilt;
            running = false;
            return;
        }
        if (rebuilt) return;
        graphicsUnavailable = true;
        runtimeExitCode = 1;
        BOO_LOG_ERROR("D3D12", "Graphics recovery unsuccessful: " + recoveryError);
        window.showGraphicsFailure(recoveryError);
    };

    if (settings.physxRouteTest) {
        // The bird-route sweep: every bird the film can show, through both cities' buildings.
        int routeExit = 0;
        if (!liveM1Ptr || physxRuntime.info().state != PhysxRuntimeState::Ready) {
            BOO_LOG_ERROR("RouteTest", "Bird-route sweep needs the film renderer and the PhysX runtime: " + physxRuntime.info().reason);
            routeExit = 2;
        }
        for (uint32_t city : {1u, 2u}) {
            if (routeExit == 2) break;
            const auto buildings = liveM1Ptr->buildCityCollision(city);
            const BirdRouteReport r = sweepBirdRoutes(physxRuntime, buildings, city);
            const char* name = city == 1u ? "Shenzhen" : "Taipei";
            if (!r.ran) {
                BOO_LOG_ERROR("RouteTest", std::string(name) + ": did not run: " + r.reason);
                routeExit = 2;
                continue;
            }
            BOO_LOG_INFO("RouteTest", std::string(name) + ": " + std::to_string(r.samples) + " bird samples against " +
                                      std::to_string(r.buildings) + " buildings; authored birds within the clearance " +
                                      std::to_string(r.authoredHits) + " times, workout birds " + std::to_string(r.workoutHits) + " times; tallest building top " + std::to_string(r.tallest) + " m at (" + std::to_string(r.tallestAt[0]) + ", " + std::to_string(r.tallestAt[1]) + ").");
            for (const auto& [flock, hits] : r.hitsByFlock) {
                const auto lift = r.liftByFlock.find(flock);
                BOO_LOG_WARN("RouteTest", std::string(name) + ": " + flock + ": " + std::to_string(hits) + " samples within " +
                                          std::to_string(static_cast<int>(kBirdBuildingClearance)) + " m of a building" +
                                          (lift != r.liftByFlock.end() ? ", needs to fly " + std::to_string(lift->second) + " m higher" : std::string()));
            }
            if (r.authoredClosest < 1e8f)
                BOO_LOG_INFO("RouteTest", std::string(name) + ": the film's own birds come within " + std::to_string(r.authoredClosest) +
                                          " m of a building at the closest");
            for (const auto& n : r.nudges)
                BOO_LOG_WARN("RouteTest", std::string(name) + ": film bird " + std::to_string(n.bird) +
                                          (n.found ? " clears every building by moving " + std::to_string(n.across) + " m across and " +
                                                         std::to_string(n.above) + " m up (kTaipeiFlockNudge in src/render/bird_flight.hpp)"
                                                   : std::string(" has no move within 6 m across and 12 m up that clears it")));
            for (const auto& h : r.examples) {
                char line[220]{};
                std::snprintf(line, sizeof(line), "%s: %s at %.1f s, (%.1f %.1f %.1f)", name, h.who.c_str(), h.filmTime,
                              h.position[0], h.position[1], h.position[2]);
                BOO_LOG_WARN("RouteTest", line);
            }
            if (r.workoutHits) routeExit = 1; // the film's own birds are reported, never failed: they are not moved
        }
        runtimeExitCode = routeExit;
        running = false;
    }

    dropUnavailableNeural();
    if (running && (visualInit.status == BackendStatus::Faulted || visualInit.status == BackendStatus::Unavailable)) {
        // Neither the film nor the safe picture could be built: say so instead of presenting black frames.
        graphicsUnavailable = true;
        runtimeExitCode = 1;
        BOO_LOG_ERROR("Render", "No visual effect could be built: " + visualInit.humanReadableReason);
        StartupProgress::instance().cancel();
        window.hideLoading();
        window.showGraphicsFailure(visualInit.humanReadableReason);
    }
    bool fallbackReported = false;
    while (running && window.pumpMessages()) {
        auto cpuFrameT0 = std::chrono::steady_clock::now();
        bool manualPresetChanged = false;

        d3d.beginSimulation(settings.frameGeneration, settings.reflex);
        const auto presentation = d3d.frameGenerationStats();
        maxFramesPresentedPerRender = std::max(maxFramesPresentedPerRender, presentation.framesActuallyPresented);
        const WindowInputSnapshot winInput = window.consumeInputSnapshot();
        BooXInputGamepad padState{};
        bool hasGamepad = false;
        interactiveCtrl.handlePreTickInput(winInput, window, settings, hudMode,
            liveM1Ptr && !orchestrator.isUsingFallbackEffect(),
            {liveM1Ptr && liveM1Ptr->pathTracingState() != RendererFeatureState::Unavailable,
             liveM1Ptr && liveM1Ptr->dlssState() != RendererFeatureState::Unavailable,
             liveM1Ptr && liveM1Ptr->rayReconstructionState() != RendererFeatureState::Unavailable,
             presentation.state != RendererFeatureState::Unavailable, presentation.reflexState != RendererFeatureState::Unavailable},
            std::chrono::steady_clock::now() < volumeVisibleUntil,
            benchRunning.load(std::memory_order_relaxed) || benchCompleted.load(std::memory_order_relaxed),
            clock.state() == TransportState::Ended,
            padState, hasGamepad);

        // Drag-and-drop custom MP3/WAV/FLAC/M4A soundtrack (or MP4 Signal Hijack video) directly onto window!
        std::wstring droppedFile;
        while (window.popDroppedFile(droppedFile)) {
            const std::filesystem::path dropPath(droppedFile);
            if (isAudioFileExtension(dropPath)) {
                std::string audErr;
                if (audio.loadExternalAudioFile(dropPath, &audErr)) {
                    if (audio.isMuted()) {
                        settings.mute = false;
                        audio.setMuted(false);
                    }
                    BOO_LOG_INFO("Audio", "Imported custom soundtrack via drag & drop: '" + audio.externalAudioLabel() + "'");
                } else {
                    BOO_LOG_WARN("Audio", "Could not decode dropped audio file: " + audErr);
                }
            } else if (isVideoFileExtension(dropPath)) {
                if (mediaIngest.openLocalFile(dropPath.string(), false)) {
                    settings.enableSignalHijack = true;
                    orchestrator.setCodecSignalHijackActive(true);
                    BOO_LOG_INFO("Media", "Imported Signal Hijack video via drag & drop: " + mediaIngest.statusReason());
                } else {
                    BOO_LOG_WARN("Media", mediaIngest.statusReason());
                }
            }
        }
        if (interactiveCtrl.pendingScrubRatio >= 0.0f) {
            leaveNpuView();
            if (interactiveCtrl.exploreActive) {
                interactiveCtrl.exploreActive = false;
                interactiveCtrl.activePreset = 0;
                interactiveCtrl.blendT = 0.0f;
                interactiveCtrl.hasPrevState = false;
                window.setExploreKeyCapture(false);
            }
            if (settings.playbackMode != PlaybackMode::FullFilm) {
                settings.playbackMode = PlaybackMode::FullFilm;
            }
            clock.clearLoopRange();
            clock.setLoopEnabled(settings.loop);
            const int64_t targetSample = std::clamp<int64_t>(
                static_cast<int64_t>(static_cast<double>(interactiveCtrl.pendingScrubRatio) * static_cast<double>(cueSheet.durationSamples)),
                0, std::max<int64_t>(0, cueSheet.durationSamples - 1));
            clock.seekToSample(targetSample);
            if (audioClockReady) {
                audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                audio.setPaused(clock.state() == TransportState::Paused || clock.state() == TransportState::Ended);
            }
            orchestrator.resetTo(clock.currentSeconds());
            interactiveCtrl.pendingScrubRatio = -1.0f;
        }

        TransportCommand cmd = TransportCommand::None;
        while (window.popCommand(cmd)) {
            // The neural networks may not be built (a light start on integrated graphics, or a GPU without a
            // feature they need): their controls then say why and change nothing, never showing the suite as on.
            const bool neuralControl = cmd == TransportCommand::ToggleNeuralRendering ||
                cmd == TransportCommand::ToggleNeuralTensorCores || cmd == TransportCommand::ToggleNeuralShading ||
                cmd == TransportCommand::ToggleNeuralTextures || cmd == TransportCommand::ToggleNeuralCache ||
                cmd == TransportCommand::SoloNeuralShading || cmd == TransportCommand::SoloNeuralTextures ||
                cmd == TransportCommand::SoloNeuralCache;
            if (neuralControl && !(liveM1Ptr && liveM1Ptr->neuralRenderingStats().available)) {
                BOO_LOG_INFO("Neural", "Neural rendering is unavailable here. " +
                    (liveM1Ptr ? liveM1Ptr->neuralRenderingStats().unavailableReason : std::string()));
                continue;
            }
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
                    leaveNpuView();
                    if (interactiveCtrl.exploreActive) {
                        interactiveCtrl.exploreActive = false;
                        interactiveCtrl.activePreset = 0;
                        interactiveCtrl.blendT = 0.0f;
                        interactiveCtrl.hasPrevState = false;
                        window.setExploreKeyCapture(false);
                    }
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
                    leaveNpuView();
                    if (interactiveCtrl.exploreActive) {
                        const uint32_t nxtP = (interactiveCtrl.activePreset % 6u) + 1u;
                        interactiveCtrl.applyHeroPreset(
                            nxtP, cueSheet, settings, clock, audio, audioClockReady, orchestrator, window);
                        break;
                    }
                    if (!cueSheet.scenes.empty()) {
                        if (settings.playbackMode != PlaybackMode::FullFilm) {
                            settings.playbackMode = PlaybackMode::FullFilm;
                        }
                        clock.clearLoopRange();
                        clock.setLoopEnabled(settings.loop);
                        const size_t cur = findSceneIndexAtSample(cueSheet, clock.currentSample());
                        const size_t nxt = (cur + 1) % cueSheet.scenes.size();
                        clock.seekToSample(cueSheet.scenes[nxt].startSample);
                        if (audioClockReady) {
                            audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                            audio.setPaused(clock.state() == TransportState::Paused || clock.state() == TransportState::Ended);
                        }
                        orchestrator.resetTo(clock.currentSeconds());
                    }
                    break;
                }
                case TransportCommand::PrevAct: {
                    leaveNpuView();
                    if (interactiveCtrl.exploreActive) {
                        const uint32_t prvP = (interactiveCtrl.activePreset <= 1u) ? 6u : (interactiveCtrl.activePreset - 1u);
                        interactiveCtrl.applyHeroPreset(
                            prvP, cueSheet, settings, clock, audio, audioClockReady, orchestrator, window);
                        break;
                    }
                    if (!cueSheet.scenes.empty()) {
                        if (settings.playbackMode != PlaybackMode::FullFilm) {
                            settings.playbackMode = PlaybackMode::FullFilm;
                        }
                        clock.clearLoopRange();
                        clock.setLoopEnabled(settings.loop);
                        const size_t cur = findSceneIndexAtSample(cueSheet, clock.currentSample());
                        const size_t prv = (cur > 0) ? (cur - 1) : (cueSheet.scenes.size() - 1);
                        clock.seekToSample(cueSheet.scenes[prv].startSample);
                        if (audioClockReady) {
                            audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                            audio.setPaused(clock.state() == TransportState::Paused || clock.state() == TransportState::Ended);
                        }
                        orchestrator.resetTo(clock.currentSeconds());
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
                case TransportCommand::CycleEffectsDetail:
                    settings.effectsDetail = (settings.effectsDetail + 1u) % 3u;
                    interactiveCtrl.hasPrevState = false;
                    break;
                case TransportCommand::CyclePathTracing:
                    settings.pathTracing = static_cast<PathTracingLevel>((static_cast<unsigned>(settings.pathTracing) + 1u) % 4u);
                    interactiveCtrl.hasPrevState = false;
                    break;
                case TransportCommand::CycleDlss:
                    settings.dlssMode = static_cast<DlssMode>((static_cast<unsigned>(settings.dlssMode) + 1u) % 4u);
                    interactiveCtrl.hasPrevState = false;
                    break;
                case TransportCommand::ToggleRayReconstruction:
                    settings.rayReconstruction = !settings.rayReconstruction;
                    interactiveCtrl.hasPrevState = false;
                    break;
                case TransportCommand::CycleFrameGeneration:
                    settings.frameGeneration = nextFrameGenerationMode(settings.frameGeneration, presentation.maxGeneratedPerFrame);
                    break;
                case TransportCommand::CycleReflex:
                    settings.reflex = static_cast<ReflexMode>((static_cast<unsigned>(settings.reflex) + 1u) % 3u);
                    break;
                case TransportCommand::ToggleNeuralRendering:
                    settings.neural.enabled = !settings.neural.enabled;
                    if (!settings.neural.enabled) settings.neural.solo = NeuralSolo::None; // Back to the untouched frame
                    interactiveCtrl.hasPrevState = false;
                    BOO_LOG_INFO("Neural", std::string("Neural rendering ") + (settings.neural.enabled ? "ON" : "OFF"));
                    break;
                case TransportCommand::CycleFlockFormation: {
                    static constexpr const char* kNames[] = {"extended cluster", "vee", "echelon", "column", "front", "kettle"};
                    settings.flockFormation = (settings.flockFormation + 1u) % 6u;
                    // A formation with no flock shows nothing: start a small one so the change is visible.
                    if (settings.gpuWorkout == 0u) settings.gpuWorkout = 3u;
                    interactiveCtrl.hasPrevState = false;
                    BOO_LOG_INFO("Workout", std::string("Flock formation: ") + kNames[settings.flockFormation]);
                    break;
                }
                case TransportCommand::CyclePhysxGenre:
                    settings.physxGenre = nextPhysxGenre(settings.physxGenre);
                    BOO_LOG_INFO("PhysX", "PhysX genre: " + std::string(physxGenreInfo(settings.physxGenre).label));
                    break;
                case TransportCommand::ToggleBirdCam:
                    settings.birdCam = !settings.birdCam;
                    interactiveCtrl.hasPrevState = false;
                    BOO_LOG_INFO("Workout", std::string("Bird camera ") + (settings.birdCam ? "ON" : "OFF"));
                    break;
                case TransportCommand::GpuWorkoutDown:
                case TransportCommand::GpuWorkoutUp:
                    settings.gpuWorkout = cmd == TransportCommand::GpuWorkoutUp ? std::min(settings.gpuWorkout + 1u, 10u)
                                                                                : (settings.gpuWorkout > 0u ? settings.gpuWorkout - 1u : 0u);
                    interactiveCtrl.hasPrevState = false;
                    BOO_LOG_INFO("Workout", "GPU workout level " + std::to_string(settings.gpuWorkout) + ": asks for " +
                                            std::to_string(workoutBirdCount(settings.gpuWorkout)) + " birds");
                    break;
                case TransportCommand::CycleDashboardPanel:
                    settings.dashboardPanel = (settings.dashboardPanel + 1u) % 3u;
                    break;
                case TransportCommand::DashboardFull: settings.dashboardPanel = 0u; break;
                case TransportCommand::DashboardMini: settings.dashboardPanel = 1u; break;
                case TransportCommand::DashboardHidden: settings.dashboardPanel = 2u; break;
                case TransportCommand::ToggleNeuralTensorCores:
                    settings.neural.tensorCores = !settings.neural.tensorCores;
                    BOO_LOG_INFO("Neural", std::string("Neural networks on ") + (settings.neural.tensorCores ? "tensor cores" : "shader cores"));
                    break;
                case TransportCommand::ToggleNeuralShading:
                case TransportCommand::ToggleNeuralTextures:
                case TransportCommand::ToggleNeuralCache: {
                    bool& feature = cmd == TransportCommand::ToggleNeuralShading ? settings.neural.shading
                                  : (cmd == TransportCommand::ToggleNeuralTextures ? settings.neural.textures
                                                                                  : settings.neural.radianceCache);
                    // With the suite off, clicking a row turns the suite on with that network running.
                    if (!settings.neural.enabled) { settings.neural.enabled = true; feature = true; }
                    else feature = !feature;
                    interactiveCtrl.hasPrevState = false;
                    break;
                }
                case TransportCommand::SoloNeuralShading:
                case TransportCommand::SoloNeuralTextures:
                case TransportCommand::SoloNeuralCache: {
                    const NeuralSolo pick = cmd == TransportCommand::SoloNeuralShading ? NeuralSolo::Shading
                                          : (cmd == TransportCommand::SoloNeuralTextures ? NeuralSolo::Textures
                                                                                        : NeuralSolo::RadianceCache);
                    settings.neural.solo = settings.neural.solo == pick ? NeuralSolo::None : pick;
                    interactiveCtrl.hasPrevState = false;
                    break;
                }
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
                case TransportCommand::ToggleOverlay: {
                    const uint32_t nextHud = (hudMode + 1u) % 4u;
                    leaveNpuView();
                    if (hudMode == 1u) hudBeforeComparison = hudMode;
                    hudMode = nextHud;
                    settings.showOverlay = hudMode != 3u;
                    break;
                }
                case TransportCommand::RunFilmBenchmark: {
                    ResultsDialogContext context{};
                    context.hardware = cardHardware;
                    context.viewerStartupSeconds = viewerStartupSeconds;
                    context.adapter.luidLow = d3d.adapterLuidLow();
                    context.adapter.luidHigh = d3d.adapterLuidHigh();
                    context.adapter.viewerLocalUsage = viewerLocalUsage;
                    if (graphicsUnavailable) context.initialFailure = BenchmarkReason::GraphicsReset;
                    else if (benchRunning.load()) context.initialFailure = BenchmarkReason::Busy;
                    else if (d3d.isWarp()) context.availability.fill(BenchmarkReason::SoftwareRenderer);
                    else if (!viewerLocalUsage) context.availability.fill(BenchmarkReason::MemoryBudget);
                    else {
                        context.availability.fill(BenchmarkReason::AdapterMismatch);
                        for (const auto& adapter : caps.adapters) {
                            if (adapter.luidLowPart != context.adapter.luidLow || adapter.luidHighPart != context.adapter.luidHigh)
                                continue;
                            context.vendorId = adapter.vendorId;
                            context.deviceId = adapter.deviceId;
                            if (!adapter.supportsD3D12 || adapter.shaderModel == "5_1") {
                                context.availability.fill(BenchmarkReason::UnsupportedDevice);
                                break;
                            }
                            if (adapter.shaderModel == "Unknown") break;
                            context.availability[0] = BenchmarkReason::None;
                            const bool dxr = adapter.dxrTier == "Tier_1_1" || adapter.dxrTier == "Tier_1_2";
                            context.availability[1] = adapter.dxrTier == "Unknown" ? BenchmarkReason::AdapterMismatch :
                                dxr ? BenchmarkReason::None : BenchmarkReason::RequiresDxr;
                            context.availability[2] = !resultsDlssInBuild() ? BenchmarkReason::RequiresDlss :
                                adapter.vendorId != 0x10de ? BenchmarkReason::UnsupportedDevice :
                                liveM1Ptr ? liveM1Ptr->dlssBenchmarkAvailability() : BenchmarkReason::DlssInitFailed;
                            break;
                        }
                    }
                    const bool resume = clock.state() != TransportState::Paused && clock.state() != TransportState::Ended;
                    if (resume) clock.pause();
                    if (audioClockReady) audio.setPaused(true);
                    // A paused film cannot display the live camera indicator.
                    stopCamera("the film is paused for a graphics benchmark.");
                    if (openResultsDialog(window.hwnd(), context)) {
                        // Driver calls can remain stuck after removal. The native card has closed,
                        // no further frames are submitted, and teardown never owns the UI thread.
                        window.hide();
                        std::thread cleanup([&] {
                            gpuSensors.stop();
                            d3d.retireGpuForShutdown();
                            spoutSender.shutdown();
                            orchestrator.shutdown();
                            d3d.shutdown();
                        });
                        WaitForSingleObject(cleanup.native_handle(), 1000);
                        TerminateProcess(GetCurrentProcess(), 1);
                        std::_Exit(1);
                    }
                    if (resume) {
                        if (audioClockReady) audio.resetPhase(static_cast<uint64_t>(clock.currentSample()));
                        clock.resume();
                        if (audioClockReady) audio.setPaused(false);
                    }
                    cpuFrameT0 = std::chrono::steady_clock::now();
                    previousFrameStart.reset();
                    break;
                }
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
                    BOO_LOG_INFO("NPU", std::string("Neural director preference [U]: ") + (settings.disableNPU ? "OFF" : "ON"));
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
                    openComparison(hudMode == 2u ? static_cast<AbCompareEngine>((static_cast<uint8_t>(abCompareEngine) + 1u) % 5u) : abCompareEngine);
                    break;
                case TransportCommand::CompareRT:
                case TransportCommand::CompareCUDA:
                case TransportCommand::CompareTensor:
                case TransportCommand::CompareCodec:
                case TransportCommand::CompareNPU:
                    openComparison(static_cast<AbCompareEngine>(int(cmd) - int(TransportCommand::CompareRT)));
                    break;
                case TransportCommand::CloseComparison:
                    if (hudMode == 2u) closeComparison();
                    break;
                case TransportCommand::ToggleFullscreen:
                    window.toggleBorderlessFullscreen();
                    BOO_LOG_INFO("Window", window.isFullscreen() ? "Full screen" : "Windowed");
                    break;
                case TransportCommand::LeaveFullscreen:
                    // Idempotent, so a second Esc before this one lands cannot bounce back in.
                    if (window.isFullscreen()) {
                        window.toggleBorderlessFullscreen();
                        BOO_LOG_INFO("Window", "Windowed (Esc)");
                    }
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
                    if (!mediaIngest.isLiveCameraActive() && !cameraLabelAvailable()) {
                        BOO_LOG_WARN("Media", "Camera left off: its LIVE CAMERA label cannot be shown here.");
                    } else if (mediaIngest.isLiveCameraActive()) {
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
                case TransportCommand::SoloEngineNPU:
                case TransportCommand::SelectModeNPU: {
                    if (npuViewReturn) {
                        leaveNpuView();
                        break;
                    }
                    SoloLoopRange comparisonRange{};
                    if (!resolvePlaybackLoopRange(cueSheet, PlaybackMode::SoloNPU, comparisonRange)) {
                        if (hudMode == 2u && abCompareEngine == AbCompareEngine::NPU) closeComparison();
                        else openComparison(AbCompareEngine::NPU);
                        break;
                    }
                    NpuViewReturn previous{};
                    previous.mode = settings.playbackMode;
                    previous.comparison = abCompareEngine;
                    previous.hud = hudMode;
                    previous.solo = interactiveCtrl.soloEngine;
                    previous.aov = interactiveCtrl.aovMode;
                    previous.split = interactiveCtrl.abSplitRatio;
                    previous.cameraBlend = interactiveCtrl.blendT;
                    previous.exploring = interactiveCtrl.exploreActive;
                    previous.sample = clock.currentSample();
                    previous.loop = clock.loopEnabled();
                    previous.customRange = clock.getLoopRange(previous.loopRange.startSample, previous.loopRange.endSample);
                    previous.paused = clock.state() == TransportState::Paused;
                    previous.ended = clock.state() == TransportState::Ended;
                    npuViewReturn = previous;

                    // Both sides share the current GPU settings, camera and soundtrack.
                    interactiveCtrl.soloEngine = 3u;
                    interactiveCtrl.abSplitRatio = 0.5f;
                    interactiveCtrl.blendT = 0.0f;
                    interactiveCtrl.exploreActive = false;
                    interactiveCtrl.aovMode = 0u;
                    interactiveCtrl.hasPrevState = false;
                    window.setExploreKeyCapture(false);
                    hudMode = 2u;
                    settings.showOverlay = true;
                    abCompareEngine = AbCompareEngine::NPU;
                    orchestrator.setAbCompareEngine(abCompareEngine);
                    settings.playbackMode = PlaybackMode::SoloNPU;
                    modeRange = comparisonRange;
                    clock.setLoopRange(modeRange.startSample, modeRange.endSample);
                    clock.setLoopEnabled(true);
                    const int64_t reveal = modeRange.startSample + TimelineClock::secondsToSamples(18.0, cueSheet.sampleRate);
                    clock.seekToSample(reveal < modeRange.endSample ? reveal : modeRange.startSample);
                    clock.resume();
                    // The dedicated comparison is an explicit request to test the director.
                    // The user's NPU preference still applies, even after another hardware solo.
                    asyncDirector.setDisableNpu(settings.disableNPU);
                    asyncDirector.retryIfDegraded();
                    if (audioClockReady) {
                        audio.resetPhase(static_cast<uint64_t>(clock.currentSample()));
                        audio.setExploreAmbient(false);
                        audio.setPaused(false);
                    }
                    orchestrator.resetTo(clock.currentSeconds());
                    BOO_LOG_INFO("Show", "Director comparison [4]: same scene and camera, active director versus scripted controls.");
                    break;
                }
                case TransportCommand::SelectModeFull:
                case TransportCommand::SelectModeRT:
                case TransportCommand::SelectModeTensor:
                case TransportCommand::SelectModeCUDA:
                case TransportCommand::SelectModeFinale: {
                    leaveNpuView();
                    if (cmd == TransportCommand::SelectModeFull) {
                        applyAllEnginesRestored();
                    }
                    if (cmd == TransportCommand::SelectModeRT) settings.playbackMode = PlaybackMode::SoloRT;
                    if (cmd == TransportCommand::SelectModeTensor) settings.playbackMode = PlaybackMode::SoloTensor;
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
                case TransportCommand::ToggleExploreMode:
                    leaveNpuView();
                    if (interactiveCtrl.exploreActive) {
                        interactiveCtrl.exitExplore(clock, audio, audioClockReady, window);
                    } else {
                        interactiveCtrl.enterExplore(
                            resolveShowVisualMode(cueSheet, settings.playbackMode),
                            clock, audio, audioClockReady, window, settings, 0.5f);
                    }
                    break;
                case TransportCommand::TogglePracticalLamps:
                    interactiveCtrl.lampsOff = !interactiveCtrl.lampsOff;
                    interactiveCtrl.hasPrevState = false;
                    BOO_LOG_INFO("Explore", std::string("Practical lamps / RTXDI emitters [J]: ") +
                                            (interactiveCtrl.lampsOff ? "OFF" : "ON"));
                    break;
                case TransportCommand::ToggleRtxSplitWipe:
                    if (hudMode == 2u) {
                        closeComparison();
                    } else {
                        if (interactiveCtrl.soloEngine == 2u) {
                            abCompareEngine = AbCompareEngine::Tensor;
                        } else if (interactiveCtrl.soloEngine == 3u) {
                            abCompareEngine = AbCompareEngine::NPU;
                        } else if (interactiveCtrl.soloEngine == 4u) {
                            abCompareEngine = AbCompareEngine::CUDA;
                        } else if (interactiveCtrl.soloEngine == 5u) {
                            abCompareEngine = AbCompareEngine::Codec;
                        } else {
                            abCompareEngine = AbCompareEngine::RT;
                        }
                        openComparison(abCompareEngine);
                        BOO_LOG_INFO("Render", std::string("Split-Screen Wipe [H]: LIVE ") + toString(abCompareEngine) +
                                               " vs FALLBACK (drag divider curtain)");
                    }
                    break;
                case TransportCommand::ResetInteractiveLighting:
                    if (interactiveCtrl.soloEngine != 0u) {
                        applyAllEnginesRestored();
                    }
                    interactiveCtrl.sunOverride = false;
                    interactiveCtrl.lampsOff = false;
                    interactiveCtrl.aovMode = 0u;
                    interactiveCtrl.clearMarbles();
                    interactiveCtrl.sunElevation01 = 0.43f;
                    interactiveCtrl.sunAzimuth01 = 0.4125f;
                    interactiveCtrl.recomputeSunDir();
                    interactiveCtrl.abSplitRatio = 0.50f;
                    interactiveCtrl.hasPrevState = false;
                    BOO_LOG_INFO("Explore", "Reset interactive sun, practical lamps, AOVs, marbles, solo & split curtain [F8]");
                    break;
                case TransportCommand::RestoreAllEngines:
                    applyAllEnginesRestored();
                    if (audioClockReady) {
                        audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                    }
                    orchestrator.resetTo(clock.currentSeconds());
                    BOO_LOG_INFO("Show", "Hardware Orchestra [ALL]: Restored all 5 live hardware pillars");
                    break;
                case TransportCommand::SoloEngineRT:
                case TransportCommand::SoloEngineTensor:
                case TransportCommand::SoloEngineCUDA:
                case TransportCommand::SoloEngineCodec: {
                    leaveNpuView();
                    const uint32_t targetSolo =
                        (cmd == TransportCommand::SoloEngineRT)     ? 1u :
                        (cmd == TransportCommand::SoloEngineTensor) ? 2u :
                        (cmd == TransportCommand::SoloEngineCUDA)   ? 4u : 5u;

                    if (interactiveCtrl.soloEngine == targetSolo) {
                        applyAllEnginesRestored();
                        if (audioClockReady) {
                            audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                        }
                        orchestrator.resetTo(clock.currentSeconds());
                        BOO_LOG_INFO("Show", "Hardware SOLO toggled OFF -> Full film resumed");
                        break;
                    }

                    // Exit static camera lock so the isolated hardware showcase animates live immediately
                    if (interactiveCtrl.exploreActive) {
                        interactiveCtrl.exploreActive = false;
                        interactiveCtrl.activePreset = 0;
                        interactiveCtrl.blendT = 0.0f;
                        interactiveCtrl.aovMode = 0u;
                        interactiveCtrl.clearMarbles();
                        interactiveCtrl.hasPrevState = false;
                        window.setExploreKeyCapture(false);
                    }
                    if (clock.state() == TransportState::Paused || clock.state() == TransportState::Ended) {
                        clock.resume();
                        if (audioClockReady) audio.setPaused(false);
                    }

                    interactiveCtrl.soloEngine = targetSolo;
                    settings.disableRT = false;
                    settings.disableCUDA = false;
                    settings.disableTensor = false;
                    settings.disableNVENC = false;
                    settings.enableSignalHijack = false;

                    const bool isFullFilm = (cueSheet.durationSamples >= TimelineClock::kFullFilmSamples);
                    const char* soloDesc = "";
                    if (targetSolo == 1u) { // RT DXR
                        settings.disableCUDA = true;
                        settings.disableTensor = true;
                        abCompareEngine = AbCompareEngine::RT;
                        soloDesc = "SOLO 1/5: RT DXR 1.1 Ray Tracing (Mirror Intelligence & Glass Orchid)";
                        if (isFullFilm) {
                            settings.playbackMode = PlaybackMode::SoloRT;
                            if (resolvePlaybackLoopRange(cueSheet, settings.playbackMode, modeRange)) {
                                clock.setLoopRange(modeRange.startSample, modeRange.endSample);
                                clock.setLoopEnabled(true);
                                clock.seekToSample(modeRange.startSample);
                            }
                        }
                    } else if (targetSolo == 2u) { // Tensor MMA
                        settings.disableCUDA = true;
                        abCompareEngine = AbCompareEngine::Tensor;
                        soloDesc = "SOLO 2/5: Tensor Core MMA SIREN Caustics (Neural Velvet)";
                        if (isFullFilm) {
                            settings.playbackMode = PlaybackMode::SoloTensor;
                            if (resolvePlaybackLoopRange(cueSheet, settings.playbackMode, modeRange)) {
                                clock.setLoopRange(modeRange.startSample, modeRange.endSample);
                                clock.setLoopEnabled(true);
                                clock.seekToSample(modeRange.startSample);
                            }
                        }
                    } else if (targetSolo == 4u) { // CUDA 65.5K Vortex
                        settings.disableTensor = true;
                        abCompareEngine = AbCompareEngine::CUDA;
                        soloDesc = "SOLO 4/5: CUDA 65,536-Particle Curl-Noise Vortex (Particle Mutiny)";
                        if (isFullFilm) {
                            settings.playbackMode = PlaybackMode::SoloCUDA;
                            if (resolvePlaybackLoopRange(cueSheet, settings.playbackMode, modeRange)) {
                                clock.setLoopRange(modeRange.startSample, modeRange.endSample);
                                clock.setLoopEnabled(true);
                                clock.seekToSample(modeRange.startSample);
                            }
                        }
                    } else { // Codec NVENC + D3D12 Video Decode
                        settings.disableCUDA = true;
                        settings.disableTensor = true;
                        settings.enableSignalHijack = true;
                        abCompareEngine = AbCompareEngine::Codec;
                        soloDesc = "SOLO 5/5: NVENC + D3D12 Video H.264 Hardware Round-Trip (Signal Hijack)";
                        if (isFullFilm) {
                            settings.playbackMode = PlaybackMode::FullFilm;
                            modeRange.startSample = TimelineClock::secondsToSamples(245.0, cueSheet.sampleRate);
                            modeRange.endSample = TimelineClock::secondsToSamples(275.0, cueSheet.sampleRate);
                            clock.setLoopRange(modeRange.startSample, modeRange.endSample);
                            clock.setLoopEnabled(true);
                            clock.seekToSeconds(246.0);
                        }
                    }

                    orchestrator.setDisableRt(settings.disableRT);
                    orchestrator.setDisableCuda(settings.disableCUDA);
                    orchestrator.setDisableTensor(settings.disableTensor);
                    orchestrator.setDisableCodec(settings.disableNVENC);
                    orchestrator.setCodecSignalHijackActive(settings.enableSignalHijack);
                    orchestrator.setAbCompareEngine(abCompareEngine);
                    interactiveCtrl.hasPrevState = false;
                    if (audioClockReady) {
                        audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                        audio.setPaused(false);
                    }
                    orchestrator.resetTo(clock.currentSeconds());
                    BOO_LOG_INFO("Show", soloDesc);
                    break;
                }
                case TransportCommand::CycleAovMode: {
                    static constexpr const char* kAovNames[8] = {
                        "0: Final Rendered",
                        "1: Base Color (Albedo)",
                        "2: World Normal",
                        "3: Roughness / Metallic / AO",
                        "4: Direct + Caustics + RTXDI",
                        "5: Indirect Diffuse GI",
                        "6: Reflections & Dielectric Refraction",
                        "7: DXR Ray Heatmap"
                    };
                    interactiveCtrl.aovMode = (interactiveCtrl.aovMode + 1u) % 8u;
                    interactiveCtrl.hasPrevState = false;
                    BOO_LOG_INFO("Explore", std::string("Buffer / AOV Inspection [I / F9]: ") + kAovNames[interactiveCtrl.aovMode]);
                    break;
                }
                case TransportCommand::ResetAovMode:
                    interactiveCtrl.aovMode = 0u;
                    interactiveCtrl.hasPrevState = false;
                    BOO_LOG_INFO("Explore", "Buffer / AOV Inspection [Shift+I]: 0: Final Rendered");
                    break;
                case TransportCommand::SpawnMarbles: {
                    leaveNpuView();
                    const ShowVisualMode curVis = resolveShowVisualMode(cueSheet, settings.playbackMode);
                    if (!interactiveCtrl.exploreActive) {
                        interactiveCtrl.enterExplore(curVis, clock, audio, audioClockReady, window, settings, 0.5f);
                    }
                    const bool inBath = (curVis == ShowVisualMode::FullFilm360s && clock.currentSeconds() >= 335.5);
                    interactiveCtrl.spawnMarbles(inBath);
                    BOO_LOG_INFO("Explore", "Spawned 10 ray-traced Marbles RTX rigid-body spheres [Y / F10 / M]");
                    break;
                }
                case TransportCommand::ClearMarbles:
                    interactiveCtrl.clearMarbles();
                    BOO_LOG_INFO("Explore", "Cleared interactive physics marbles [Shift+Y]");
                    break;
                case TransportCommand::NextMusicTrack: {
                    const uint32_t slot = audio.cycleSoundtrackForward();
                    if (slot == 0u && audioClockReady) {
                        audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                    }
                    if (audio.isMuted()) {
                        settings.mute = false;
                        audio.setMuted(false);
                    }
                    BOO_LOG_INFO("Audio", "Soundtrack [Z / TRK]: " + audio.activeSoundtrackTitle());
                    break;
                }
                case TransportCommand::PrevMusicTrack: {
                    const uint32_t slot = audio.cycleSoundtrackBackward();
                    if (slot == 0u && audioClockReady) {
                        audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                    }
                    if (audio.isMuted()) {
                        settings.mute = false;
                        audio.setMuted(false);
                    }
                    BOO_LOG_INFO("Audio", "Soundtrack [Shift+Z / TRK]: " + audio.activeSoundtrackTitle());
                    break;
                }
                case TransportCommand::OpenAudioFileDialog: {
                    std::filesystem::path pickedPath;
                    if (openNativeAudioFilePicker(window.hwnd(), pickedPath)) {
                        std::string audErr;
                        if (audio.loadExternalAudioFile(pickedPath, &audErr)) {
                            if (audio.isMuted()) {
                                settings.mute = false;
                                audio.setMuted(false);
                            }
                            BOO_LOG_INFO("Audio", "Loaded custom soundtrack [MP3]: '" + audio.externalAudioLabel() + "'");
                        } else {
                            BOO_LOG_WARN("Audio", "Failed to load selected audio file: " + audErr);
                        }
                    }
                    break;
                }
                case TransportCommand::ClearCustomAudio:
                    if (audio.hasExternalAudioFile()) {
                        audio.clearExternalAudioFile();
                        if (audioClockReady) {
                            audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, clock.currentSample())));
                        }
                        BOO_LOG_INFO("Audio", "Restored built-in polyphonic NVIDIA RTX showcase score");
                    }
                    break;
                case TransportCommand::HeroPreset1:
                case TransportCommand::HeroPreset2:
                case TransportCommand::HeroPreset3:
                case TransportCommand::HeroPreset4:
                case TransportCommand::HeroPreset5:
                case TransportCommand::HeroPreset6: {
                    leaveNpuView();
                    const uint32_t idx = static_cast<uint32_t>(
                        static_cast<int>(cmd) - static_cast<int>(TransportCommand::HeroPreset1) + 1);
                    interactiveCtrl.applyHeroPreset(
                        idx, cueSheet, settings, clock, audio, audioClockReady, orchestrator, window);
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

        if (npuViewReturn && interactiveCtrl.wantsMouseExplore(winInput)) leaveNpuView();
        const ClockSnapshot tick = clock.tickFrame();
        if (audioClockReady) {
            if (tick.loopedThisTick &&
                (!audio.hasExternalAudioFile() || settings.playbackMode == PlaybackMode::FullFilm)) {
                audio.resetPhase(static_cast<uint64_t>(std::max<int64_t>(0, tick.samplePosition)));
            }
            audio.setPaused(tick.state == TransportState::Paused || tick.state == TransportState::Ended);
            // The music fades out over the film's last seconds and is silent as the picture ends, instead
            // of stopping dead on the last frame. Not when the film loops, and not in the free camera,
            // where the music plays on as ambience.
            constexpr double kEndFadeSeconds = 8.0;
            float endFade = 1.0f;
            int64_t loopStart = 0, loopEnd = 0;
            if (settings.playbackMode == PlaybackMode::FullFilm && !clock.loopEnabled() &&
                !clock.getLoopRange(loopStart, loopEnd) && !interactiveCtrl.exploreActive) {
                const double remaining = static_cast<double>(clock.durationSamples() - tick.samplePosition) /
                                         static_cast<double>(std::max<int32_t>(1, cueSheet.sampleRate));
                endFade = static_cast<float>(std::clamp(remaining / kEndFadeSeconds, 0.0, 1.0));
            }
            audio.setEndFade(endFade);
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
        viewerLocalUsage = appVram;
        peakVramBytes = std::max(peakVramBytes, appVram);
        const QualityPresetSpec prevSpec = findQualityPresetSpec(presets, qualityLadder.currentPreset());
        const bool comparingDirector = settings.showOverlay && hudMode == 2u && abCompareEngine == AbCompareEngine::NPU;
        // A comparison draws the scene twice. Keep these timings out of the film's
        // preset decisions, but keep the memory-pressure response active throughout.
        if (!comparingDirector || appVram > prevSpec.softVramLimitBytes || qualityLadder.memoryPressureActive()) {
            qualityLadder.pushFrameSample(lastGpuMs, appVram, prevSpec.softVramLimitBytes, sceneChanged);
        }

        const QualityPresetSpec activeSpec = findQualityPresetSpec(presets, qualityLadder.currentPreset());
        const bool allowImmediateCanvasResize =
            manualPresetChanged || sceneChanged || qualityLadder.memoryPressureActive();
        if (!graphicsUnavailable) {
            orchestrator.syncPresetBeforeFrame(activeSpec, allowImmediateCanvasResize, tick.filmTimeSeconds);
            if (!d3d.checkDeviceHealthy(&d3dInitErr)) recoverGraphics(d3dInitErr);
        }
        const bool otherHardwareSolo = interactiveCtrl.soloEngine != 0u && interactiveCtrl.soloEngine != 3u;
        const bool inspectDirector = npuViewReturn.has_value() || (comparingDirector && activeCue.allowDirectorModulation &&
                                     liveM1Ptr && !orchestrator.isUsingFallbackEffect());
        asyncDirector.setDisableNpu(settings.disableNPU || (!inspectDirector && (otherHardwareSolo || !activeSpec.allowNpu)));

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
        // Live audio reactivity: follows whatever is actually playing (score, OST or imported track).
        fctx.audioReactive = audioReactive.update(
            bands.low, bands.mid, bands.high, bands.rms, tick.dtSeconds, settings.reducedFlash,
            tick.state == TransportState::Seeking || tick.state == TransportState::Paused || audio.isMuted());
        fctx.edition = settings.edition;
        fctx.qualityPreset = qualityLadder.currentPreset();
        fctx.showOverlay = settings.showOverlay;
        fctx.showCaptions = settings.showCaptions;
        fctx.reducedFlash = settings.reducedFlash;
        fctx.motionScale = settings.motionScale;
        fctx.effectsDetail = settings.effectsDetail;
        fctx.pathTracing = settings.pathTracing;
        fctx.dlssMode = settings.dlssMode;
        fctx.rayReconstruction = settings.rayReconstruction;
        fctx.frameGeneration = settings.frameGeneration;
        fctx.reflex = settings.reflex;
        fctx.neural = settings.neural;
        fctx.gpuWorkout = settings.gpuWorkout;
        fctx.physxGenre = settings.physxGenre;
        if (physxGenreInfo(settings.physxGenre).usesPhysxSdk) physxRuntime.requestStart();
        // The Explore marbles move onto PhysX as soon as its runtime is ready, mid-roll if need be.
        if (interactiveCtrl.marbleCount > 0u && !physxMarbles) {
            const PhysxRuntimeState state = physxRuntime.info().state;
            if (state == PhysxRuntimeState::Stopped) physxRuntime.requestStart();
            if (state == PhysxRuntimeState::Ready) {
                physxMarbles = std::make_unique<PhysxMarbles>(physxRuntime, [](float x, float z) {
                    return InteractiveExploreController::evalCountertopHeight(x, z);
                });
                if (physxMarbles->ok()) {
                    interactiveCtrl.physxMarbles = physxMarbles.get();
                    interactiveCtrl.handMarblesToPhysx(resolveShowVisualMode(cueSheet, settings.playbackMode) == ShowVisualMode::FullFilm360s &&
                                                       tick.filmTimeSeconds >= 335.5);
                } else {
                    physxMarbles.reset();
                }
            }
        }
        if (settings.physxGenre == static_cast<uint32_t>(PhysxGenre::Rigid) && liveM1Ptr && !graphicsUnavailable) {
            if (!physxRigid) physxRigid = std::make_unique<PhysxRigidGenre>(physxRuntime);
            uint32_t cityAct = UINT32_MAX;
            const auto& buildings = liveM1Ptr->cityCollision(&cityAct);
            const FlockFocus focus = liveM1Ptr->flockFocus();
            physxRigid->frame(std::clamp(tick.unclampedDtSeconds, 0.0, 0.1), fctx.paused, buildings, cityAct, focus.air, focus.valid);
            fctx.physxBodies = physxRigid->bodies();
        } else if (physxRigid) {
            physxRigid.reset(); // releases the scene and its GPU memory
        }
        if (settings.physxGenre == static_cast<uint32_t>(PhysxGenre::Fluid) && liveM1Ptr && !graphicsUnavailable) {
            if (!physxFluid) physxFluid = std::make_unique<PhysxFluidGenre>(physxRuntime);
            uint32_t cityAct = UINT32_MAX;
            const auto& buildings = liveM1Ptr->cityCollision(&cityAct);
            const FlockFocus focus = liveM1Ptr->flockFocus();
            physxFluid->frame(std::clamp(tick.unclampedDtSeconds, 0.0, 0.1), tick.filmTimeSeconds, fctx.paused, buildings, cityAct,
                              focus.air, focus.valid);
            fctx.physxBodies = physxFluid->bodies();
        } else if (physxFluid) {
            physxFluid.reset();
        }
        if (settings.physxGenre == static_cast<uint32_t>(PhysxGenre::Soft) && liveM1Ptr && !graphicsUnavailable) {
            if (!physxSoft) physxSoft = std::make_unique<PhysxSoftGenre>(physxRuntime);
            uint32_t cityAct = UINT32_MAX;
            const auto& buildings = liveM1Ptr->cityCollision(&cityAct);
            const FlockFocus focus = liveM1Ptr->flockFocus();
            physxSoft->frame(std::clamp(tick.unclampedDtSeconds, 0.0, 0.1), fctx.paused, buildings, cityAct, focus.air, focus.valid);
            fctx.physxBodies = physxSoft->bodies();
        } else if (physxSoft) {
            physxSoft.reset();
        }
        fctx.flockFormation = settings.flockFormation;
        fctx.hudMode = hudMode;

        // The workout flock. The dial asks; the governor releases birds a batch at a time and checks
        // the frame rate after each, so the machine is never taken under the floor.
        {
            const bool m1Live = liveM1Ptr && !orchestrator.isUsingFallbackEffect() && !graphicsUnavailable;
            WorkoutGovernorInput gin{};
            gin.frameSeconds = previousFrameStart
                ? std::chrono::duration<double>(cpuFrameT0 - *previousFrameStart).count() : 0.0;
            gin.gpuSeconds = lastGpuMs * 0.001;
            gin.requested = m1Live ? workoutBirdCount(settings.gpuWorkout) : 0u;
            gin.limit = workoutLimit;
            gin.floorFps = settings.workoutFloorFps;
            // Shots with no birds at all (the orbital views, the last seconds) teach it nothing.
            gin.flockVisible = m1Live && !window.isMinimized() && liveM1Ptr->birdCount() > 0u;
            workoutGovernor.update(gin);
            previousFrameStart = cpuFrameT0;
            if (m1Live && workoutGovernor.released() > liveM1Ptr->workoutBirdCapacity()) {
                // More birds than the buffers hold: make room now, between frames.
                if (!liveM1Ptr->reserveWorkoutBirds(workoutGovernor.released())) {
                    workoutLimit = liveM1Ptr->workoutBirdCapacity();
                    BOO_LOG_WARN("Workout", "This machine has room for " + std::to_string(workoutLimit) + " workout birds.");
                }
                workoutGovernor.restartMeasurement();
                previousFrameStart.reset(); // the pause for the new buffers is not a frame time
            }
            if (workoutGovernor.released() != workoutTraceBirds) {
                // Every batch, for anyone following the governor in a debug log.
                char line[160]{};
                std::snprintf(line, sizeof(line), "Flock %u -> %u birds (measured %.1f fps, %.3f us a bird%s)",
                              workoutTraceBirds, workoutGovernor.released(), static_cast<double>(workoutGovernor.measuredFps()),
                              workoutGovernor.secondsPerBird() * 1e6, workoutGovernor.atFloor() ? ", at the floor" : "");
                BOO_LOG_DEBUG("Workout", line);
                workoutTraceBirds = workoutGovernor.released();
            }
            fctx.workout.birds = workoutGovernor.birds();
            fctx.workout.grown = workoutGovernor.grown();
            fctx.workout.front = workoutGovernor.front();
            fctx.workout.band = workoutGovernor.band();
            fctx.workout.size = workoutGovernor.flockSize();
            // One line when the frame rate, not the dial, settles the size of the flock, and another
            // if that size later moves by more than a twentieth: a log shows what the machine could fly.
            if (workoutGovernor.atFloor() && !workoutGovernor.releasing() && workoutGovernor.released() > 0u) {
                const uint32_t now = workoutGovernor.released();
                const uint32_t change = now > workoutLoggedBirds ? now - workoutLoggedBirds : workoutLoggedBirds - now;
                if (!workoutLoggedFloor || change * 20u > std::max(now, 20u)) {
                    char line[160]{};
                    std::snprintf(line, sizeof(line), "Holding %u workout birds at %.1f fps (floor %.0f fps, asked for %u)",
                                  now, static_cast<double>(workoutGovernor.measuredFps()),
                                  static_cast<double>(settings.workoutFloorFps), gin.requested);
                    BOO_LOG_INFO("Workout", line);
                    workoutLoggedFloor = true;
                    workoutLoggedBirds = now;
                }
            } else if (workoutGovernor.released() >= gin.requested) {
                workoutLoggedFloor = false; // the dial is satisfied again
                workoutLoggedBirds = 0;
            }
        }

        // M1/M2 clean-feed modulation must not inherit audio buffering or presentation history,
        // while M5 (SoloNPU) and M6 (FullFilm360s) consume live 5 Hz OpenVINO NPU/CPU stage direction.
        ScriptedDirector sampleDirector;
        sampleDirector.reset(cueSheet.seed);
        FrameContext deterministicCtx = fctx;
        deterministicCtx.audioEnvelope = computeAnalyticalAudioBands(tick.samplePosition, cueSheet.sampleRate, cueSheet.durationSamples, settings.edition).rms;
        DirectorMoodState scriptedMood{};
        const DirectorMoodState asyncMood = asyncDirector.evaluateFrame(fctx, &activeCue, &scriptedMood);
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
        const bool usesNeuralDirection = !mood.isScriptedFallback && activeCue.allowDirectorModulation;
        const auto& baseline = usesNeuralDirection ? scriptedMood : mood;
        fctx.scriptedDirectorComparison.bloomStrength = baseline.bloomStrength;
        fctx.scriptedDirectorComparison.irisAngle = baseline.irisAngle;
        fctx.scriptedDirectorComparison.lightTension = baseline.lightTension;
        fctx.scriptedDirectorComparison.spectralShift = baseline.spectralShift;
        fctx.scriptedDirectorComparison.cutAccent = baseline.cutAccent;

        interactiveCtrl.updateFrame(
            winInput, padState, hasGamepad, tick, activeSpec, cueSheet, settings,
            clock, audio, audioClockReady, window, fctx);
        birdCamera.apply(settings.birdCam, liveM1Ptr ? liveM1Ptr->flockFocus() : FlockFocus{},
                         std::clamp(static_cast<float>(tick.unclampedDtSeconds), 0.0005f, 0.100f), fctx);

        if (captureRequested && !graphicsUnavailable) {
            captureRequested = false;
            std::string captureError;
            if (!orchestrator.captureClean(fctx, "diagnostics/captures", &captureError)) {
                // Started from the Start menu or the Store package, the working folder is not writable
                // (System32, or the read-only install folder). Save to the user's Pictures instead.
                std::string fallbackError = "Pictures folder unavailable";
                const std::filesystem::path pictures = userCaptureFolder();
                if (pictures.empty() || !orchestrator.captureClean(fctx, pictures, &fallbackError))
                    BOO_LOG_WARN("Capture", captureError + " | " + fallbackError);
            }
        }
        if (!cameraLabelAvailable()) stopCamera("its label can no longer be shown.");
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
            hud.filmEnded = (tick.state == TransportState::Ended);
            hud.preset = qualityLadder.currentPreset();
            hud.edition = settings.edition;
            hud.playbackMode = settings.playbackMode;
            hud.mood = mood;
            // The model may be running while the current shot holds its authored lighting.
            hud.mood.isScriptedFallback = !usesNeuralDirection;
            hud.audioBands = bands;
            hud.frameGenerationState = presentation.state;
            hud.maxGeneratedFrames = presentation.maxGeneratedPerFrame;
            hud.framesActuallyPresented = presentation.framesActuallyPresented;
            hud.reflexState = presentation.reflexState;
            hud.renderedFps = static_cast<float>(presentation.renderedFps);
            hud.displayedFps = static_cast<float>(presentation.displayedFps);
            hud.canvasWidth = activeSpec.renderWidth;
            hud.canvasHeight = activeSpec.renderHeight;
            hud.swapchainBits = d3d.swapchainChannelBits();
            hud.abCompareEngine = abCompareEngine;
            hud.codecSignalHijackActive = settings.enableSignalHijack;
            if (liveM1Ptr) hud.neural = liveM1Ptr->neuralRenderingStats();
            hud.dashboardPanel = settings.dashboardPanel;
            hud.gpuSensors = gpuSensors.latest();
            hud.gpuWorkout = settings.gpuWorkout;
            hud.flockFormation = settings.flockFormation;
            hud.birdCam = settings.birdCam;
            if (liveM1Ptr) hud.flockSpecies = liveM1Ptr->flockFocus().species;
            if (liveM1Ptr) hud.birdCount = liveM1Ptr->birdCount();
            hud.fullscreen = window.isFullscreen();
            hud.physxGenre = settings.physxGenre;
            hud.bakedCloth = liveM1Ptr ? liveM1Ptr->bakedCloth() : BakedClothInfo{};
            hud.physx = physxStatsFor(settings.physxGenre, liveM1Ptr ? liveM1Ptr->physxSmokeStats() : FlowSmokeStats{}, physxRuntime.info(),
                                      physxRigid.get(), physxFluid.get(), physxSoft.get());
            hud.fullscreenPill = interactiveCtrl.fullscreenPillOpacity;
            hud.workoutFloorFps = static_cast<uint32_t>(std::lround(settings.workoutFloorFps));
            hud.workoutState = workoutGovernor.atFloor() ? 2u
                : (workoutGovernor.releasing() || workoutGovernor.released() < workoutBirdCount(settings.gpuWorkout)) ? 1u : 0u;
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
            hud.directorBackend = classifyDirectorHudBackend(npuTel.hardwareResult);
            hud.directorRateHz = npuTel.completedRateHz;
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

            d3d.endSimulation();
            if (d3d.beginFrame()) {
                orchestrator.renderFrame(fctx, false);
                if (!cameraLabelAvailable()) stopCamera("its label can no longer be shown.");
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
                if (!viewerStartupSeconds && presented && d3d.lastPresentationVisible() &&
                    !orchestrator.isUsingFallbackEffect() && IsWindowVisible(window.hwnd())) {
                    viewerStartupSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - viewerEntry).count();
                }
                const bool published = spoutSender.completePublish();
                if (presented) {
                    window.hideLoading();
                    StartupProgress::instance().finish(); // does nothing after the first frame
                }
                if (presented && !fallbackReported && !window.closeRequested() && orchestrator.isUsingFallbackEffect()) {
                    // The emergency test picture explains itself instead of just pulsing.
                    fallbackReported = true;
                    if (!settings.noActivateWindow && settings.autoExitSeconds <= 0.0) {
                        offerFallbackReport(window.hwnd(), d3d.adapterName(), orchestrator.primaryFailureReason());
                    }
                }
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
            char expTag[64]{};
            if (interactiveCtrl.exploreActive) {
                const uint32_t spp = fctx.interactive.accumulate ? (fctx.interactive.accumFrame + 1u) : 1u;
                std::snprintf(expTag, sizeof(expTag), " [EXPLORE %u SPP] |", spp);
            }
            char titleBuf[512]{};
            std::snprintf(
                titleBuf,
                sizeof(titleBuf),
                "BOO SAH SAH |%s %s | %s %zu/%zu (%s) | %.1f FPS (GPU %.2f ms) | %u-bit %s | [7] %s %u/3 | [F]=%s [H]=%s [J]=%s [Tab]=HUD",
                expTag,
                TimelineClock::formatTimecode(tick.samplePosition, cueSheet.sampleRate).c_str(),
                titleAct.c_str(),
                actIdx + 1,
                cueSheet.scenes.size(),
                activeCue.sceneId.c_str(),
                smoothFps,
                lastGpuMs,
                d3d.swapchainChannelBits(),
                toString(qualityLadder.currentPreset()),
                titleDetail.c_str(), effectiveEffectsDetail(fctx) + 1u,
                titleExplore.c_str(),
                titleWipe.c_str(),
                titleLamp.c_str());
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
    summary.presentation = d3d.frameGenerationStats();
    summary.maxFramesPresentedPerRender = std::max(maxFramesPresentedPerRender, summary.presentation.framesActuallyPresented);
    if (liveM1Ptr && !orchestrator.isUsingFallbackEffect()) {
        summary.pathTracing = liveM1Ptr->pathTracingState();
        summary.superResolution = liveM1Ptr->dlssState();
        summary.rayReconstruction = liveM1Ptr->rayReconstructionState();
        summary.dlssInputWidth = liveM1Ptr->dlssInputWidth();
        summary.dlssInputHeight = liveM1Ptr->dlssInputHeight();
        summary.neural = liveM1Ptr->neuralRenderingStats();
        summary.gpuSensorsDevice = gpuSensors.available() ? gpuSensors.deviceName() : std::string();
        summary.gpuSensorsMatch = gpuSensors.matchMethod();
        summary.gpuSensorsNvidiaCount = gpuSensors.nvidiaGpuCount();
        summary.gpuSensorsReason = gpuSensors.unavailableReason();
        summary.gpuSensorsLast = gpuSensors.latest();
        summary.gpuSensorsRun = gpuSensors.summary();
        summary.gpuWorkout = settings.gpuWorkout;
        summary.flockFormation = settings.flockFormation;
        summary.birdCam = settings.birdCam;
        summary.flockSpecies = liveM1Ptr->flockFocus().species;
        summary.birdCount = liveM1Ptr->birdCount();
        summary.workoutRequested = workoutBirdCount(settings.gpuWorkout);
        summary.workoutReleased = workoutGovernor.released();
        summary.workoutFloorFps = settings.workoutFloorFps;
        summary.workoutAtFloor = workoutGovernor.atFloor();
        summary.workoutSteadyBirds = workoutGovernor.steadyBirds();
        summary.workoutSteadyFps = workoutGovernor.steadyFps();
        summary.workoutPeakBirds = workoutGovernor.peakBirds();
        summary.workoutMicrosecondsPerBird = workoutGovernor.secondsPerBird() * 1e6;
        summary.workoutCapacity = liveM1Ptr->workoutBirdCapacity();
        summary.physxGenre = settings.physxGenre;
        summary.physx = physxStatsFor(settings.physxGenre, liveM1Ptr->physxSmokeStats(), physxRuntime.info(), physxRigid.get(),
                                      physxFluid.get(), physxSoft.get());
        summary.physxRuntime = physxRuntime.info();
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

    // For anyone following shutdown in a debug log: how long each step took.
    auto shutdownStep = [last = std::chrono::steady_clock::now()](const char* what) mutable {
        const auto now = std::chrono::steady_clock::now();
        char line[160]{};
        std::snprintf(line, sizeof(line), "Step done in %.0f ms: %s",
                      std::chrono::duration<double, std::milli>(now - last).count(), what);
        BOO_LOG_DEBUG("Shutdown", line);
        last = now;
    };
    d3d.retireGpuForShutdown();
    shutdownStep("GPU retired");
    if (settings.enableD3DDebug && (summary.d3dDebugErrors != 0 || summary.d3dDebugWarnings != 0)) runtimeExitCode = 1;
    mediaIngest.shutdown();
    shutdownStep("media ingest");
    spoutSender.shutdown();
    shutdownStep("Spout");
    clock.shutdown();
    shutdownStep("clock");
    orchestrator.shutdown();
    shutdownStep("effects");
    // The sound stops and the window leaves the screen before the device goes: after a start
    // that compiled shaders (the first after an update, with an empty driver cache), releasing
    // the D3D12 device waits while the driver finishes its own shader work, which can take
    // most of a minute. The app should look and sound closed meanwhile.
    audio.shutdown();
    shutdownStep("audio");
    window.hide();
    shutdownStep("window hidden");
    d3d.shutdown();
    shutdownStep("D3D12");
    window.destroy();
    shutdownStep("window");
    Logger::instance().closeLogFile();
    return runtimeExitCode;
}
