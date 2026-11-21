#pragma once

#include "boo/api.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace boo {

// Section 4.2.3: Frozen categorical mood states
enum class DirectorCategory : uint8_t {
    Calm       = 0,
    Suspense   = 1,
    Bloom      = 2,
    Comic      = 3,
    Transition = 4
};

const char* toString(DirectorCategory cat) noexcept;

// Section 4.2.3: Frozen 16-value feature vector contract across NPU, CPU, and Scripted backends
struct DirectorFeatureVector {
    std::array<float, 16> values{};
};

// Section 7.3: Bounded mood variables controlled by director when cue permits
struct DirectorMoodState {
    DirectorCategory category = DirectorCategory::Calm;
    float confidence = 1.0f;
    float bloomStrength = 0.35f; // [0.0, 1.0]
    float irisAngle = 0.50f;     // [0.0, 1.0]
    float lightTension = 0.25f;  // [0.0, 1.0]
    float spectralShift = 0.15f; // [0.0, 1.0]
    float cutAccent = 0.0f;      // [0.0, 1.0]
    bool isScriptedFallback = true;
};

class ScriptedDirector {
public:
    ScriptedDirector() = default;

    void reset(uint64_t seed) noexcept;

    DirectorFeatureVector buildFeatures(
        const FrameContext& ctx,
        const SceneCue* activeCue) const noexcept;

    DirectorMoodState evaluate(
        const FrameContext& ctx,
        const SceneCue* activeCue) noexcept;

    const DirectorMoodState& currentState() const noexcept { return smoothedState_; }
    uint64_t evaluationCount() const noexcept { return evalCount_; }

private:
    uint64_t seed_ = 266203;
    uint64_t evalCount_ = 0;
    double lastCategoryChangeTime_ = -10.0;
    DirectorMoodState smoothedState_{};
    bool initialized_ = false;
    static constexpr double kMinStateHoldSeconds = 1.5;
};

struct OpenVinoDirectorConfig {
    std::filesystem::path modelXmlPath;
    bool allowNpu = true;
    bool allowCpuFallback = true;
    bool allowDevVenvDiscovery = true;
    bool simulateMissingRuntimeForTest = false;
    int64_t inferTimeoutMs = 250;
    int64_t npuInferTimeoutMs = 0; // Optional async deadline on a verified NPU; zero uses inferTimeoutMs.
};

struct NeuralInferenceSample {
    DirectorCategory category = DirectorCategory::Calm;
    float confidence = 1.0f;
    std::array<float, 5> controls{0.35f, 0.50f, 0.25f, 0.15f, 0.0f}; // bloom, iris, tension, spectral, cut
    float latencyMs = 0.0f;
};

struct NeuralInferenceFailure {
    std::string reason;
    bool timedOut = false;
};

// Checks whether openvino_c.dll is dynamically discoverable (app dir, system PATH, or local .venv).
bool isOpenVinoRuntimeAvailable(bool allowDevVenvDiscovery = true) noexcept;

// Synchronous dynamically-loaded OpenVINO C API wrapper (zero static .lib link dependencies).
// Explicitly targets "NPU" first (never unqualified "AUTO"), verifies real inference output,
// and falls back to explicit "CPU" when NPU is absent or fails compilation.
class OpenVinoDirector {
public:
    OpenVinoDirector();
    ~OpenVinoDirector();

    OpenVinoDirector(const OpenVinoDirector&) = delete;
    OpenVinoDirector& operator=(const OpenVinoDirector&) = delete;

    HardwareResult initialize(const OpenVinoDirectorConfig& config);
    bool infer(const DirectorFeatureVector& features, NeuralInferenceSample& outSample,
               bool injectTimeout = false, NeuralInferenceFailure* failure = nullptr);
    void shutdown() noexcept;

    bool isReady() const noexcept;
    bool isUsingHardwareNpu() const noexcept;
    const HardwareResult& hardwareResult() const noexcept;
    const std::string& activeDeviceName() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct AsyncDirectorTelemetry {
    HardwareResult hardwareResult{};
    float lastInferMs = 0.0f;
    float resultAgeMs = 0.0f;
    float completedRateHz = 0.0f; // Successful completions in the trailing second of the active route.
    uint64_t completedInferences = 0;
    uint32_t timeoutCount = 0;
};

// Section 3.3, 3.5 & 4.2.3: Non-blocking 5 Hz background neural stage-director controller.
// Never stalls UI/render/audio threads during model compilation or inference timeouts.
class AsyncDirectorController {
public:
    AsyncDirectorController();
    ~AsyncDirectorController();

    AsyncDirectorController(const AsyncDirectorController&) = delete;
    AsyncDirectorController& operator=(const AsyncDirectorController&) = delete;

    void start(
        uint64_t seed,
        const std::filesystem::path& modelXmlPath,
        bool disableNpu,
        bool injectNpuTimeout = false,
        bool allowNpuHardware = true,
        bool simulateMissingRuntimeForTest = false);

    // Optional bounded wait used by tests or short CLI runs; returns immediately once initial compile finishes or maxWaitMs elapses.
    bool waitForInitialCompile(uint32_t maxWaitMs) noexcept;

    void reset(uint64_t seed) noexcept;
    void setDisableNpu(bool disable) noexcept;
    bool retryIfDegraded() noexcept;
    void setInjectTimeout(bool inject) noexcept;

    DirectorMoodState evaluateFrame(
        const FrameContext& ctx,
        const SceneCue* activeCue,
        DirectorMoodState* scriptedBaseline = nullptr) noexcept;

    AsyncDirectorTelemetry telemetry() const noexcept;
    void shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
