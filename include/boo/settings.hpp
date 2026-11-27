#pragma once

#include "boo/api.hpp"
#include "boo/logging.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace boo {

struct QualityPresetSpec {
    QualityPresetId id = QualityPresetId::Balanced;
    std::string label = "Balanced";
    uint32_t renderWidth = 1920;
    uint32_t renderHeight = 1080;
    uint32_t targetFps = 60;
    bool allowDxr = true;
    bool allowCuda = true;
    bool allowTensor = true;
    bool allowNpu = true;
    uint32_t rtBounces = 1;
    uint32_t particleBudget = 100000;
    uint32_t tensorResolutionDivisor = 2;
    uint64_t softVramLimitBytes = 12ULL * 1024ULL * 1024ULL * 1024ULL;
};

struct AppSettings {
    std::string title = "BOO SAH SAH — The Glass Orchid Protocol";
    Edition edition = Edition::Family;
    PlaybackMode playbackMode = PlaybackMode::FullFilm;
    std::filesystem::path configPath = "config/default.toml";
    std::filesystem::path cuePath = "assets/cues/film_360s.json";
    std::filesystem::path manifestPath = "assets/manifest.json";
    std::filesystem::path presetsPath = "config/quality_presets.json";
    bool loop = false;
    double startSeconds = 0.0;

    uint32_t width = 1920;
    uint32_t height = 1080;
    bool fullscreen = false;
    bool vsync = true;
    bool noActivateWindow = false;
    QualityPresetId qualityPreset = QualityPresetId::Balanced;
    bool showOverlay = true;
    uint32_t dashboardPanel = 0; // Upper-left dashboard: 0 full, 1 mini strip, 2 hidden tab (backtick cycles)
    bool showCaptions = true;
    // "auto" follows the Windows display language; otherwise a catalog tag such as "ja-JP" or "en-US".
    std::string language = "auto";
    std::filesystem::path stringsDir = "assets/strings";
    bool reducedFlash = false;
    uint32_t effectsDetail = 1;
    PathTracingLevel pathTracing = PathTracingLevel::Low;
    DlssMode dlssMode = DlssMode::Quality;
    bool rayReconstruction = true;
    FrameGenerationMode frameGeneration = FrameGenerationMode::Auto;
    ReflexMode reflex = ReflexMode::On;
    NeuralRenderingControls neural{};
    uint32_t physxGenre = 0;          // PhysX suite, live genre (boo/physx_genres.hpp)
    bool physxRouteTest = false;      // sweep every bird route through the PhysX city, report, exit
    uint32_t gpuWorkout = 0;          // 0..10: extra birds to load the GPU (0 = authored film)
    float workoutFloorFps = 24.0f;    // the workout never takes the frame rate under this (0 = no floor)
    uint32_t flockFormation = 0;      // 0 extended cluster, 1 vee, 2 echelon, 3 column, 4 front, 5 kettle
    bool birdCam = false;             // camera follows the flock instead of the film's shots
    float motionScale = 1.0f;
    bool legacyM0Visual = false;
    bool startInExploreMode = false;
    uint32_t initialExplorePreset = 0;

    int32_t sampleRate = 48000;
    bool mute = false;
    float masterVolume = 0.75f;
    bool forceSilentClock = false;
    std::filesystem::path audioInputPath;

    // Hardware & fault-injection flags (Section 8.3)
    bool enableSignalHijack = false;
    bool disableRT = false;
    bool disableCUDA = false;
    bool disableTensor = false;
    bool disableNPU = false;
    bool disableNVENC = false;
    bool forceWarp = false;
    bool lowPowerGpu = false; // diagnostics: run on the integrated (power-saving) adapter
    bool warpFullPath = false; // test only: with --force-warp, load the real film instead of the WARP diagnostic scene
    int slowDriverMs = 0;      // test only: pretend every pipeline compile takes this long (GPU waits a quarter of it)
    bool presetFromCommandLine = false; // --preset was given: integrated graphics keep it too
    bool coldShaderCache = false; // diagnostics: the driver ignores its shader caches, as on a first start after an update
    bool injectMeshFail = false;
    bool injectShaderFail = false;
    bool injectAudioFail = false;
    bool injectVramPressure = false;
    bool injectNpuTimeout = false;
    bool injectDeviceLost = false;
    bool enableD3DDebug = false;

    // Optional live-show integrations (Milestone M8 Spout2 clean sender & M9 Opt-In Media Ingest)
    bool enableSpout = false;
    std::string spoutSenderName = "BooSahSah_Clean";
    std::filesystem::path mediaInputPath;
    bool enableCameraInput = false;
    bool injectMediaFail = false;

    // Automation, diagnostics & ad-hoc numerical benchmark flags
    double autoExitSeconds = 0.0; // 0 = run until user closes window
    bool headlessProbeOnly = false;
    bool validateCuesOnly = false;
    bool runBenchCliOnly = false;
    bool runBenchOnStart = false;
    uint32_t benchIterations = 40;
    std::filesystem::path exportDiagnosticsPath;
    std::filesystem::path exportBenchJsonPath;
    std::filesystem::path exportBenchCsvPath;
    std::filesystem::path logFilePath;
    LogLevel logLevel = LogLevel::Info;
    bool showHelp = false;
};

bool loadSettingsFromToml(const std::filesystem::path& path, AppSettings& ioSettings, std::string* outError = nullptr);
bool parseCommandLineArgs(const std::vector<std::string>& args, AppSettings& ioSettings, std::string* outError = nullptr);
std::string buildCliHelpText();
// Turns on every GPU feature the app has, for hardware testing (--test-rig, [hardware] test_rig = true).
void applyTestRigProfile(AppSettings& settings) noexcept;

std::vector<QualityPresetSpec> loadQualityPresets(const std::filesystem::path& path);
QualityPresetSpec findQualityPresetSpec(const std::vector<QualityPresetSpec>& presets, QualityPresetId id);

} // namespace boo
