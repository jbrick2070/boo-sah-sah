#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace boo {

struct HudTelemetrySnapshot;
struct QualityPresetSpec;

// Section 3.4: Core backend status contract
enum class BackendStatus : uint8_t {
    Accelerated = 0, // Featured hardware actually ran and verified this frame/segment
    Substituted = 1, // Another implementation executed successfully
    Degraded    = 2, // Reduced quality, partial effect, or scripted stand-in
    Unavailable = 3, // No viable equivalent; story-safe effect omitted
    Faulted     = 4  // Implementation errored; quarantined until safe boundary retry/restart
};

// Section 8.1: Per-engine health-state contract
enum class HealthState : uint8_t {
    Probing           = 0,
    Ready             = 1,
    ActiveAccelerated = 2,
    ActiveSubstitute  = 3,
    ActiveDegraded    = 4,
    Unavailable       = 5,
    Faulted           = 6,
    Cooldown          = 7
};

enum class EngineKind : uint8_t {
    RT     = 0,
    Tensor = 1,
    NPU    = 2,
    CUDA   = 3,
    Codec  = 4
};

constexpr size_t kEngineKindCount = 5;

enum class PlaybackMode : uint8_t {
    FullFilm   = 0,
    SoloRT     = 1,
    SoloTensor = 2,
    SoloNPU    = 3,
    SoloCUDA   = 4,
    FinaleLoop = 5
};

enum class Edition : uint8_t {
    Family   = 0,
    Midnight = 1
};

enum class QualityPresetId : uint8_t {
    Safe           = 0,
    Balanced       = 1,
    Showcase       = 2,
    Experimental4K = 3
};

enum class AbCompareEngine : uint8_t {
    RT     = 0,
    CUDA   = 1,
    Tensor = 2,
    Codec  = 3
};

enum class ShowVisualMode : uint8_t {
    LegacyM0      = 0,
    M1Slice60s    = 1,
    M2Trilogy180s = 2,
    FullFilm360s  = 3,
    SoloRT        = 4,
    SoloCUDA      = 5,
    SoloTensor    = 6,
    SoloNPU       = 7
};

// Section 3.4: Honest hardware probe/execution result
struct HardwareResult {
    EngineKind engine = EngineKind::RT;
    BackendStatus status = BackendStatus::Unavailable;
    HealthState health = HealthState::Unavailable;
    std::string requestedBackend;
    std::string activeBackend;
    std::string detectedHardwareDetail;
    std::string diagnosticCode;
    std::string humanReadableReason;
    bool capabilityDetected = false;
    bool successfullyExecutedTestWork = false;
};

struct SceneCue {
    std::string sceneId;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    int64_t startSample = 0;
    int64_t endSample = 0;
    uint64_t deterministicSeed = 0;
    std::string visualPreset;
    std::string fallbackVisual;
    std::string preferredEngine;
    std::string audioSection;
    std::string revealId;
    int64_t revealSample = -1;
    bool allowDirectorModulation = true;
};

// Live "Explore" mode (NVIDIA-demo style): free-fly camera, interactive sun/time-of-day and
// practical-lamp control, AOV buffer inspection, interactive ray-traced physics marbles, plus
// NRD REBLUR motion denoising and NRD REFERENCE progressive accumulation while still.
// Default-constructed (inactive) values leave the deterministic film output untouched.
struct InteractiveView {
    static constexpr uint32_t kMaxMarbles = 12;

    bool exploreActive = false;          // Active interactive Explore session
    float blendWeight = 0.0f;            // [0..1] camera blend (1=Explore, (0,1)=smooth cubic return to film)
    float eye[3]{0.0f, 0.0f, 0.0f};
    float target[3]{0.0f, 0.0f, 1.0f};
    float up[3]{0.0f, 1.0f, 0.0f};
    float fovYRadians = 0.92f;
    bool sunOverride = false;            // Use sunDir/sunScale instead of the scripted sun
    float sunDir[3]{0.80f, 0.34f, -0.49f};
    float sunScale = 1.0f;
    float sunElevation01 = 0.52f;        // [0..1] UI slider normalized elevation (night -> dawn -> day)
    float sunAzimuth01 = 0.42f;          // [0..1] UI slider normalized azimuth
    bool lampsOff = false;               // Switch off practical lamps (sconce / mirror halo / RTXDI emitters)
    bool accumulate = false;             // Progressive temporal accumulation + sub-pixel jitter
    uint32_t accumFrame = 0;             // 0 = reset history this frame
    float abSplitRatio = 0.5f;           // [0.05..0.95] draggable RTX ON / OFF A/B split curtain
    uint32_t activePreset = 0;           // 0 = free-fly, 1..6 = hero macro bookmark
    uint32_t aovMode = 0;                // 0 = Final, 1..7 = AOV buffer inspection modes
    uint32_t soloEngine = 0;             // 0 = All (Full Orchestra), 1 = RT, 2 = Tensor, 3 = NPU, 4 = CUDA, 5 = Codec
    bool studioDrawerOpen = false;       // Keeps persistent bottom Studio Dock visible across Film/Explore/Wipe
    uint32_t marbleCount = 0;            // 0 = none (canonical), 1..kMaxMarbles = active physics spheres
    float marblePosRad[kMaxMarbles][4]{};  // xyz = world position, w = sphere radius (m)
    float marblePrevPos[kMaxMarbles][4]{}; // xyz = previous frame world position (for 3D motion vectors)
    float marbleRotQuat[kMaxMarbles][4]{}; // xyzw = unit rotation quaternion for rolling grain/anisotropy
    uint32_t marbleMat[kMaxMarbles]{};     // Material ID (3 crystal, 4 celadon, 35 amber, 8 gold, 23 nickel)
};

struct FrameContext {
    InteractiveView interactive{};
    uint64_t presentationFrame = 0;
    int64_t samplePosition = 0;
    int32_t sampleRate = 48000;
    double filmTimeSeconds = 0.0;
    double dtSeconds = 0.0; // Bounded/clamped after stall
    bool seeking = false;
    bool paused = false;
    uint64_t randomSeed = 0;
    uint32_t actIndex = 0;
    float actProgress = 0.0f;
    float audioEnvelope = 0.0f;
    float moodIrisAngle = 0.5f;
    float moodIntensity = 0.5f;
    float moodPaletteAccent = 0.5f;
    float moodBloomStrength = 0.35f;
    float moodCutAccent = 0.0f;
    Edition edition = Edition::Family;
    QualityPresetId qualityPreset = QualityPresetId::Balanced;
    uint32_t hudMode = 0; // Full, compact, A/B, off
    bool showOverlay = true;
    bool showCaptions = false;
    bool reducedFlash = false;
    float motionScale = 1.0f;
};

struct IEffect {
    virtual ~IEffect() = default;
    virtual const char* name() const noexcept = 0;
    virtual HardwareResult probeAndInitialize() = 0;
    virtual void preload(double atFilmTime) = 0;
    virtual void resetTo(double filmTime) = 0;
    virtual void render(const FrameContext& ctx) = 0;
    virtual void shutdown() noexcept = 0;
    virtual bool captureClean(const FrameContext&, const std::filesystem::path&, std::string* error) { if (error) *error = "Clean capture requires the M1 renderer."; return false; }

    // Optional Milestone M1+ live orchestration hooks
    virtual void onSceneBoundary(uint32_t /*actIndex*/, double /*filmTimeSeconds*/) noexcept {}
    virtual bool syncPresetBeforeFrame(const QualityPresetSpec& /*spec*/, bool /*allowImmediateResize*/) { return true; }
    virtual void setHudTelemetry(const HudTelemetrySnapshot& /*snapshot*/) noexcept {}
    virtual void setDisableRt(bool /*disable*/) noexcept {}
    virtual void setDisableCuda(bool /*disable*/) noexcept {}
    virtual void setDisableTensor(bool /*disable*/) noexcept {}
    virtual void setDisableCodec(bool /*disable*/) noexcept {}
    virtual void setCodecSignalHijackActive(bool /*active*/) noexcept {}
    virtual void setAbCompareEngine(AbCompareEngine /*engine*/) noexcept {}
    virtual bool ownsLiveEngineStatus(EngineKind /*kind*/) const noexcept { return false; }
    virtual bool queryLiveEngineStatus(EngineKind /*kind*/, HardwareResult& /*outStatus*/) const noexcept { return false; }
    virtual bool isUsingEmbeddedFallbackShader() const noexcept { return false; }
};

const char* toString(BackendStatus status) noexcept;
const char* toString(HealthState state) noexcept;
const char* toString(EngineKind kind) noexcept;
const char* toString(PlaybackMode mode) noexcept;
const char* toString(Edition edition) noexcept;
const char* toString(QualityPresetId preset) noexcept;
const char* toString(AbCompareEngine engine) noexcept;
const char* toString(ShowVisualMode mode) noexcept;

bool parsePlaybackMode(std::string_view text, PlaybackMode& out) noexcept;
bool parseEdition(std::string_view text, Edition& out) noexcept;
bool parseQualityPreset(std::string_view text, QualityPresetId& out) noexcept;

// Resolves relative asset/config paths across working directory, exe directory, and repo root.
std::filesystem::path resolveResourcePath(const std::filesystem::path& relativeOrAbs);

} // namespace boo
