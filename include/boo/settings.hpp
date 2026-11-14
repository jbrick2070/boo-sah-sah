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
    bool showCaptions = true;
    bool reducedFlash = false;
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

std::vector<QualityPresetSpec> loadQualityPresets(const std::filesystem::path& path);
QualityPresetSpec findQualityPresetSpec(const std::vector<QualityPresetSpec>& presets, QualityPresetId id);

} // namespace boo
