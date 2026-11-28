#pragma once

#include "boo/api.hpp"
#include <cstdint>

namespace boo {

struct Bmesh;
struct M1CameraState;
M1CameraState evaluateTrilogyCamera(int64_t samplePosition, int32_t sampleRate = 48000, bool reducedFlash = false, float motionScale = 1.0f) noexcept;

struct EvtolPose {
    float position[3]{};
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    float hover = 0.0f, touchdown = 0.0f;
    float rotorTilt() const noexcept { return 0.86f - 0.82f * hover; }
};

inline constexpr float kEvtolRotorPivots[6][3] = {
    {-3.05f, 0.36f, -1.65f}, {3.05f, 0.36f, -1.65f},
    {-4.10f, 0.40f,  0.15f}, {4.10f, 0.40f,  0.15f},
    {-2.60f, 0.46f,  1.95f}, {2.60f, 0.46f,  1.95f}
};

EvtolPose evaluateEvtolPose(ShowVisualMode mode, float filmSeconds) noexcept;
bool isOrbitalCutaway(ShowVisualMode mode, const FrameContext& frame) noexcept;
inline constexpr double kCrownOrbitStartSeconds = 301.0;
inline constexpr double kCrownOrbitEndSeconds = 331.0;
// The full film borrows the river camera from the trilogy. A negative sample keeps its usual view.
int64_t shanghaiRiverSample(ShowVisualMode mode, const FrameContext& frame) noexcept;
void writeModelTransform(float rows[12], const float position[3], float yaw, float pitch, float roll, float scale = 1.0f) noexcept;

// Procedural smooth-curved stage & enamel microcosm geometry kit (kit 4) for M3-M6 Solos & 360s Full Film.
Bmesh buildEnamelMicrocosmKit();

enum class FilmCameraInsert : uint8_t { None, CanopyRibbon, ParcelDock, BlackKites };

struct M1CameraState {
    float eye[3]{0.0f, 1.35f, -14.0f};
    float target[3]{0.0f, 1.60f, 4.0f};
    float up[3]{0.0f, 1.0f, 0.0f};
    float fovYRadians = 0.92f;
    float revealOpen01 = 0.0f;  // [0..1] enamel monolith / microcosm opening factor
    float emissivePulse = 0.0f; // Bounded flash-safe musical accent in [0.0, 0.06] (0.0 under --reduced-flash)
    uint32_t shotIndex = 0;     // Act / shot index
    float focusDistance = 0.0f; // Authored focal distance in scene units
    float focusWeight = 0.0f;   // 0 selects the normal scene autofocus
    float rasterFocusDistance = 0.0f; // Optional surface focus for a raster mirror substitute
    float anamorphicWeight = 0.0f; // Authored cue pupil shape; zero keeps circular bokeh
    FilmCameraInsert insert = FilmCameraInsert::None;
};

struct LetterboxViewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 1920.0f;
    float height = 1080.0f;
};

// Closed-form flash-safe (< 2 Hz) musical emissive envelope; returns 0.0f identically when reducedFlash is true.
float evaluateM1MusicalEnvelope(int64_t samplePosition, int32_t sampleRate = 48000, bool reducedFlash = false) noexcept;

// Stateless, seek-deterministic M1 camera rig over the 60-second (2,880,000-sample @ 48 kHz) Taipei slice.
M1CameraState evaluateM1Camera(
    int64_t samplePosition,
    int32_t sampleRate = 48000,
    float moodIrisAngle = 0.5f,
    bool reducedFlash = false) noexcept;

// Unified stateless, seek-deterministic camera choreography across M1, M2, M3-M5 Solos, and M6 360s Full Film.
M1CameraState evaluateShowCamera(
    ShowVisualMode mode,
    int64_t samplePosition,
    int32_t sampleRate = 48000,
    float moodIrisAngle = 0.5f,
    bool reducedFlash = false,
    float motionScale = 1.0f) noexcept;

// Computes centered 16:9 aspect-preserving destination rectangle inside any swapchain client size.
LetterboxViewport computeLetterboxViewport16x9(uint32_t swapchainWidth, uint32_t swapchainHeight) noexcept;

} // namespace boo
