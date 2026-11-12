#include "boo/settings.hpp"
#include "boo/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace boo {

namespace {
std::string trim(std::string_view s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const size_t last = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(first, last - first + 1));
}

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool parseBoolVal(std::string_view raw, bool& out) {
    const std::string k = toLower(trim(raw));
    if (k == "true" || k == "1" || k == "yes" || k == "on")  { out = true; return true; }
    if (k == "false" || k == "0" || k == "no" || k == "off") { out = false; return true; }
    return false;
}

std::string unquoteTomlString(std::string_view v) {
    std::string t = trim(v);
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\''))) {
        return t.substr(1, t.size() - 2);
    }
    return t;
}
} // namespace

bool loadSettingsFromToml(const std::filesystem::path& path, AppSettings& ioSettings, std::string* outError) {
    std::ifstream ifs(path);
    if (!ifs) {
        if (outError) *outError = "Config file not found: " + path.string();
        return false;
    }

    std::string section;
    std::string line;
    size_t lineNum = 0;
    while (std::getline(ifs, line)) {
        ++lineNum;
        // Strip inline comments outside quotes
        bool inQuotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"') inQuotes = !inQuotes;
            else if (line[i] == '#' && !inQuotes) {
                line.resize(i);
                break;
            }
        }
        const std::string t = trim(line);
        if (t.empty()) continue;

        if (t.front() == '[' && t.back() == ']') {
            section = toLower(trim(std::string_view(t).substr(1, t.size() - 2)));
            continue;
        }

        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            if (outError) *outError = "Invalid TOML line " + std::to_string(lineNum);
            return false;
        }
        const std::string key = toLower(trim(std::string_view(t).substr(0, eq)));
        const std::string val = trim(std::string_view(t).substr(eq + 1));
        const std::string fullKey = section.empty() ? key : (section + "." + key);

        if (fullKey == "app.title") ioSettings.title = unquoteTomlString(val);
        else if (fullKey == "app.edition") parseEdition(unquoteTomlString(val), ioSettings.edition);
        else if (fullKey == "app.playback_mode") parsePlaybackMode(unquoteTomlString(val), ioSettings.playbackMode);
        else if (fullKey == "app.cue_file") ioSettings.cuePath = unquoteTomlString(val);
        else if (fullKey == "app.manifest_file") ioSettings.manifestPath = unquoteTomlString(val);
        else if (fullKey == "app.presets_file") ioSettings.presetsPath = unquoteTomlString(val);
        else if (fullKey == "app.loop") parseBoolVal(val, ioSettings.loop);
        else if (fullKey == "display.width") ioSettings.width = static_cast<uint32_t>(std::max(320, std::atoi(val.c_str())));
        else if (fullKey == "display.height") ioSettings.height = static_cast<uint32_t>(std::max(240, std::atoi(val.c_str())));
        else if (fullKey == "display.fullscreen") parseBoolVal(val, ioSettings.fullscreen);
        else if (fullKey == "display.vsync") parseBoolVal(val, ioSettings.vsync);
        else if (fullKey == "display.quality_preset") parseQualityPreset(unquoteTomlString(val), ioSettings.qualityPreset);
        else if (fullKey == "display.show_overlay") parseBoolVal(val, ioSettings.showOverlay);
        else if (fullKey == "display.captions") parseBoolVal(val, ioSettings.showCaptions);
        else if (fullKey == "display.reduced_flash") parseBoolVal(val, ioSettings.reducedFlash);
        else if (fullKey == "display.reduced_motion") {
            bool rm = false;
            if (parseBoolVal(val, rm)) ioSettings.motionScale = rm ? 0.35f : 1.0f;
        }
        else if (fullKey == "audio.sample_rate") ioSettings.sampleRate = std::atoi(val.c_str());
        else if (fullKey == "audio.mute") parseBoolVal(val, ioSettings.mute);
        else if (fullKey == "audio.master_volume") ioSettings.masterVolume = static_cast<float>(std::atof(val.c_str()));
        else if (fullKey == "audio.force_silent_clock") parseBoolVal(val, ioSettings.forceSilentClock);
        else if (fullKey == "hardware.enable_signal_hijack") parseBoolVal(val, ioSettings.enableSignalHijack);
        else if (fullKey == "hardware.disable_rt") parseBoolVal(val, ioSettings.disableRT);
        else if (fullKey == "hardware.disable_cuda") parseBoolVal(val, ioSettings.disableCUDA);
        else if (fullKey == "hardware.disable_tensor") parseBoolVal(val, ioSettings.disableTensor);
        else if (fullKey == "hardware.disable_npu") parseBoolVal(val, ioSettings.disableNPU);
        else if (fullKey == "hardware.disable_nvenc") parseBoolVal(val, ioSettings.disableNVENC);
        else if (fullKey == "integrations.enable_spout" || fullKey == "hardware.enable_spout") parseBoolVal(val, ioSettings.enableSpout);
        else if (fullKey == "integrations.spout_sender_name" || fullKey == "hardware.spout_sender_name") {
            const std::string sn = unquoteTomlString(val);
            if (!sn.empty()) ioSettings.spoutSenderName = sn;
        }
        else if (fullKey == "media.input_file" || fullKey == "integrations.media_input") {
            const std::string mp = unquoteTomlString(val);
            if (!mp.empty()) ioSettings.mediaInputPath = mp;
        }
        else if (fullKey == "media.camera_input" || fullKey == "integrations.camera_input") {
            parseBoolVal(val, ioSettings.enableCameraInput);
        }
        else if (fullKey == "diagnostics.log_level") parseLogLevel(unquoteTomlString(val), ioSettings.logLevel);
        else if (fullKey == "diagnostics.log_file") {
            const std::string lf = unquoteTomlString(val);
            if (!lf.empty()) ioSettings.logFilePath = lf;
        }
    }
    return true;
}

bool parseCommandLineArgs(const std::vector<std::string>& args, AppSettings& ioSettings, std::string* outError) {
    auto requireNext = [&](size_t& idx, std::string_view flag) -> const std::string* {
        if (idx + 1 >= args.size()) {
            if (outError) *outError = "Missing argument after " + std::string(flag);
            return nullptr;
        }
        return &args[++idx];
    };

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h") {
            ioSettings.showHelp = true;
        } else if (a == "--config") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.configPath = *v;
        } else if (a == "--cue") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.cuePath = *v;
        } else if (a == "--manifest") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.manifestPath = *v;
        } else if (a == "--preset") {
            const auto* v = requireNext(i, a); if (!v) return false;
            if (!parseQualityPreset(*v, ioSettings.qualityPreset)) {
                if (outError) *outError = "Unknown quality preset: " + *v;
                return false;
            }
        } else if (a == "--edition") {
            const auto* v = requireNext(i, a); if (!v) return false;
            if (!parseEdition(*v, ioSettings.edition)) {
                if (outError) *outError = "Unknown edition: " + *v;
                return false;
            }
        } else if (a == "--mode") {
            const auto* v = requireNext(i, a); if (!v) return false;
            if (!parsePlaybackMode(*v, ioSettings.playbackMode)) {
                if (outError) *outError = "Unknown playback mode: " + *v;
                return false;
            }
        } else if (a == "--width") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.width = static_cast<uint32_t>(std::max(320, std::atoi(v->c_str())));
        } else if (a == "--height") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.height = static_cast<uint32_t>(std::max(240, std::atoi(v->c_str())));
        } else if (a == "--fullscreen") {
            ioSettings.fullscreen = true;
        } else if (a == "--windowed") {
            ioSettings.fullscreen = false;
        } else if (a == "--no-activate") {
            ioSettings.noActivateWindow = true;
        } else if (a == "--vsync") {
            const auto* v = requireNext(i, a); if (!v) return false;
            parseBoolVal(*v, ioSettings.vsync);
        } else if (a == "--no-vsync") {
            ioSettings.vsync = false;
        } else if (a == "--loop") {
            ioSettings.loop = true;
        } else if (a == "--mute") {
            ioSettings.mute = true;
        } else if (a == "--force-silent-clock" || a == "--inject-audio-fail") {
            ioSettings.forceSilentClock = true;
            ioSettings.injectAudioFail = (a == "--inject-audio-fail");
        } else if (a == "--enable-signal-hijack") {
            ioSettings.enableSignalHijack = true;
        } else if (a == "--disable-rt") {
            ioSettings.disableRT = true;
        } else if (a == "--disable-cuda") {
            ioSettings.disableCUDA = true;
        } else if (a == "--disable-tensor") {
            ioSettings.disableTensor = true;
        } else if (a == "--disable-npu") {
            ioSettings.disableNPU = true;
        } else if (a == "--disable-nvenc") {
            ioSettings.disableNVENC = true;
        } else if (a == "--force-warp") {
            ioSettings.forceWarp = true;
        } else if (a == "--inject-mesh-fail") {
            ioSettings.injectMeshFail = true;
        } else if (a == "--inject-shader-fail") {
            ioSettings.injectShaderFail = true;
        } else if (a == "--inject-vram-pressure") {
            ioSettings.injectVramPressure = true;
        } else if (a == "--inject-npu-timeout") {
            ioSettings.injectNpuTimeout = true;
        } else if (a == "--inject-device-lost") {
            ioSettings.injectDeviceLost = true;
        } else if (a == "--enable-spout") {
            ioSettings.enableSpout = true;
        } else if (a == "--disable-spout") {
            ioSettings.enableSpout = false;
        } else if (a == "--spout-name") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.spoutSenderName = *v;
        } else if (a == "--media-input") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.mediaInputPath = *v;
        } else if (a == "--camera-input") {
            ioSettings.enableCameraInput = true;
        } else if (a == "--inject-media-fail") {
            ioSettings.injectMediaFail = true;
        } else if (a == "--d3d-debug") {
            ioSettings.enableD3DDebug = true;
        } else if (a == "--start-seconds") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.startSeconds = std::max(0.0, std::atof(v->c_str()));
        } else if (a == "--duration-seconds") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.autoExitSeconds = std::max(0.0, std::atof(v->c_str()));
        } else if (a == "--headless-probe") {
            ioSettings.headlessProbeOnly = true;
        } else if (a == "--validate-cues") {
            ioSettings.validateCuesOnly = true;
        } else if (a == "--bench") {
            ioSettings.runBenchCliOnly = true;
        } else if (a == "--bench-on-start") {
            ioSettings.runBenchOnStart = true;
        } else if (a == "--bench-iterations") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.benchIterations = static_cast<uint32_t>(std::clamp(std::atoi(v->c_str()), 3, 500));
        } else if (a == "--export-diagnostics") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.exportDiagnosticsPath = *v;
        } else if (a == "--export-bench") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.exportBenchJsonPath = *v;
        } else if (a == "--export-bench-csv") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.exportBenchCsvPath = *v;
        } else if (a == "--log-file") {
            const auto* v = requireNext(i, a); if (!v) return false;
            ioSettings.logFilePath = *v;
        } else if (a == "--log-level") {
            const auto* v = requireNext(i, a); if (!v) return false;
            parseLogLevel(*v, ioSettings.logLevel);
        } else if (a == "--hide-overlay") {
            ioSettings.showOverlay = false;
        } else if (a == "--captions") {
            ioSettings.showCaptions = true;
        } else if (a == "--no-captions") {
            ioSettings.showCaptions = false;
        } else if (a == "--reduced-flash") {
            ioSettings.reducedFlash = true;
        } else if (a == "--reduced-motion") {
            ioSettings.motionScale = 0.35f;
        } else if (a == "--legacy-m0-visual") {
            ioSettings.legacyM0Visual = true;
        } else {
            if (outError) *outError = "Unrecognized command-line argument: " + a;
            return false;
        }
    }
    return true;
}

std::string buildCliHelpText() {
    return
        "BOO SAH SAH — The Glass Orchid Protocol\n"
        "Usage: boo_sah_sah.exe [options]\n\n"
        "Playback & Display:\n"
        "  --cue <path>                Cue JSON file (default: assets/cues/film_360s.json)\n"
        "  --mode <full|solo_rt|solo_tensor|solo_npu|solo_cuda|finale_loop>\n"
        "  --edition <family|midnight> Story edition\n"
        "  --preset <safe|balanced|showcase|4k_experimental>\n"
        "  --width <px> --height <px>  Window client size (default: 1920x1080)\n"
        "  --fullscreen / --windowed   Display mode\n"
        "  --vsync <0|1> / --no-vsync  Vertical sync toggle (default: on)\n"
        "  --captions / --no-captions  Toggle HUD lower-third captions (default: on)\n"
        "  --reduced-flash             Photosensitivity-safe zero-pulse lighting envelope\n"
        "  --reduced-motion            Dampen orbital camera excursion (motion_scale=0.35)\n"
        "  --legacy-m0-visual          Force Milestone M0 test visual renderer\n"
        "  --loop                      Loop timeline playback\n"
        "  --mute                      Start audio muted\n"
        "  --force-silent-clock        Use monotonic QPC silence-fallback clock\n\n"
        "Hardware Fault Injection & Overrides (SPEC_v4 Section 8.3):\n"
        "  --disable-rt                Disable DXR raytracing\n"
        "  --disable-cuda              Disable CUDA compute\n"
        "  --disable-tensor            Disable Tensor math\n"
        "  --disable-npu               Disable NPU inference\n"
        "  --disable-nvenc             Disable NVENC codec\n"
        "  --enable-signal-hijack      Opt-in probe for Signal Hijack codec path\n"
        "  --force-warp                Force Microsoft Basic Render Driver (WARP)\n"
        "  --inject-shader-fail        Force fallback to embedded precompiled safe shader\n"
        "  --inject-audio-fail         Simulate missing audio endpoint\n"
        "  --inject-npu-timeout        Simulate NPU inference stall/timeout -> graceful failover\n"
        "  --inject-device-lost        Exercise one bounded graphics teardown/recreate (safe simulation)\n"
        "  --enable-spout              Publish clean 16:9 CanvasClean via Spout2 (zero HUD)\n"
        "  --spout-name <name>         Spout2 sender name (default: BooSahSah_Clean)\n"
        "  --media-input <path>        Opt-in local video file for Signal Hijack (1280x720 NV12 cap)\n"
        "  --camera-input              Opt-in UVC webcam for Signal Hijack (shows LIVE CAMERA badge)\n"
        "  --inject-media-fail         Simulate missing/corrupt media stream -> procedural fallback\n\n"
        "Diagnostics, Benchmarks & Smoke Automation:\n"
        "  --bench                     Run creative RT/CUDA/Tensor/NVENC numerical benchmark dashboard & exit\n"
        "  --bench-on-start            Run live numerical benchmark sweep inside the D3D12 HUD on launch\n"
        "  --bench-iterations <N>      Timed passes per benchmark kernel (default: 40)\n"
        "  --export-bench <json>       Export numerical benchmark report to JSON\n"
        "  --export-bench-csv <csv>    Export numerical benchmark report to CSV\n"
        "  --start-seconds <sec>       Deterministic initial cue seek for capture/measurement\n"
        "  --inject-mesh-fail          Exercise M2 procedural mesh fallback\n"
        "  --duration-seconds <sec>    Auto-exit cleanly after <sec> seconds (for smoke tests)\n"
        "  --no-activate               Show window without stealing foreground focus\n"
        "  --validate-cues             Verify SHA-256 asset manifest and all cue schedules & exit\n"
        "  --headless-probe            Probe capabilities, validate cues, and exit\n"
        "  --export-diagnostics <json> Write local Hardware Orchestra diagnostics JSON\n"
        "  --log-file <path>           Write structured log file\n\n"
        "Keyboard Transport (Noninteractive Demoscene Controls):\n"
        "  ,/.: Paused frame step | Left/Right: Seek 2s (Shift: 10s) | G: Reveal pre-roll\n"
        "  -/=: Volume | F12: Clean BMP + JSON | Tab: Full/Compact/A-B/Off | K: Captions\n"
        "  Space: Play/Pause | R: Restart | N/P: Next/Prev Act | L: Loop | M: Mute\n"
        "  Q: Quality Preset | X/C/T/U/V: Toggle Pillars | O: Spout2 | W: Webcam Stop | E: Edition\n"
        "  B: Run Live RT/CUDA/Tensor/NVENC Numerical Benchmark | F11: Fullscreen | Esc: Exit\n";
}

std::vector<QualityPresetSpec> loadQualityPresets(const std::filesystem::path& path) {
    std::vector<QualityPresetSpec> out;
    const JsonParseResult parsed = parseJsonFile(path);
    if (parsed.ok && parsed.value.isObject()) {
        if (const JsonValue* arrVal = parsed.value.find("presets"); arrVal && arrVal->isArray()) {
            for (const auto& item : arrVal->asArray()) {
                QualityPresetSpec spec{};
                parseQualityPreset(item["id"].asStringOr("balanced"), spec.id);
                spec.label = item["label"].asStringOr("Balanced");
                spec.renderWidth = static_cast<uint32_t>(item["render_width"].asInt64(1920));
                spec.renderHeight = static_cast<uint32_t>(item["render_height"].asInt64(1080));
                spec.targetFps = static_cast<uint32_t>(item["target_fps"].asInt64(60));
                spec.allowDxr = item["allow_dxr"].asBool(true);
                spec.allowCuda = item["allow_cuda"].asBool(true);
                spec.allowTensor = item["allow_tensor"].asBool(true);
                spec.allowNpu = item["allow_npu"].asBool(true);
                spec.rtBounces = static_cast<uint32_t>(item["rt_bounces"].asInt64(1));
                spec.particleBudget = static_cast<uint32_t>(item["particle_budget"].asInt64(100000));
                spec.tensorResolutionDivisor = static_cast<uint32_t>(item["tensor_resolution_divisor"].asInt64(2));
                spec.softVramLimitBytes = static_cast<uint64_t>(item["soft_vram_limit_mb"].asInt64(12288)) * 1024ULL * 1024ULL;
                out.push_back(std::move(spec));
            }
        }
    }
    if (out.empty()) {
        out.push_back(QualityPresetSpec{QualityPresetId::Safe, "Safe", 1280, 720, 30, false, false, false, false, 0, 25000, 4, 4ULL << 30});
        out.push_back(QualityPresetSpec{QualityPresetId::Balanced, "Balanced", 1920, 1080, 60, true, true, true, true, 1, 100000, 2, 12ULL << 30});
        out.push_back(QualityPresetSpec{QualityPresetId::Showcase, "Showcase", 2560, 1440, 60, true, true, true, true, 2, 500000, 2, 14ULL << 30});
        out.push_back(QualityPresetSpec{QualityPresetId::Experimental4K, "4K Experimental", 3840, 2160, 60, true, true, true, true, 2, 1000000, 2, 14ULL << 30});
    }
    return out;
}

QualityPresetSpec findQualityPresetSpec(const std::vector<QualityPresetSpec>& presets, QualityPresetId id) {
    for (const auto& p : presets) {
        if (p.id == id) return p;
    }
    return QualityPresetSpec{};
}

} // namespace boo
