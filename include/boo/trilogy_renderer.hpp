#pragma once
#include "boo/city_draw_stats.hpp"
#include "boo/render.hpp"
#include "boo/m1_scene.hpp"
#include <memory>
#include <vector>
namespace boo {
// Shares the existing M1 postprocess, particles, Tensor texture and isolated A/B buffers.
class TrilogyMeshRenderer {
public:
    explicit TrilogyMeshRenderer(D3D12Context& context);
    ~TrilogyMeshRenderer();
    bool initialize(bool injectMeshFailure, std::string* error,
                    ShowVisualMode visualMode = ShowVisualMode::M2Trilogy180s,
                    void* tensorTexture = nullptr);
    // Pass target resources must be in PIXEL_SHADER_RESOURCE state on entry and exit.
    bool record(void* commandList, const FrameContext& frame, const M1CameraState& camera,
                void* hdr, void* distance, uint64_t tensorSrv, uint32_t width, uint32_t height,
                uint32_t slot, bool raytracing, bool canonical, bool compareDirectorLighting = false,
                const float* dlssJitterPixels = nullptr, bool rayReconstruction = false,
                bool temporalSuperResolution = true);
    void recordOrbital(void* commandList, const FrameContext& frame, void* hdr, void* distance,
                       uint32_t width, uint32_t height, uint32_t slot, bool canonical);
    // Guides describe the current canonical RR frame and remain compute-readable
    // until the next scene draw. The two matrices are unjittered.
    bool rrGuidesReady() const noexcept;
    void* rrDiffuseAlbedo() const noexcept;
    void* rrSpecularAlbedo() const noexcept;
    void* rrNormalRoughness() const noexcept;
    void* rrSpecularHitDistance() const noexcept;
    void rrViewMatrices(float worldToView[16], float viewToClip[16]) const noexcept;
    void* dlssMotionResource() const noexcept;
    void* dlssDepthResource() const noexcept;
    bool dlssHistoryReset() const noexcept;
    bool hardwareVerified() const;
    RendererFeatureState pathTracingState() const noexcept;
    bool nrdActive() const;
    // 0 = Deterministic Spatial Cross-Bilateral, 1 = NRD REBLUR_DIFFUSE_SPECULAR Motion, 2 = NRD REFERENCE PT Accum
    uint32_t activeDenoiseMode() const noexcept;
    uint32_t verifiedHits() const;
    uint32_t birdCount() const noexcept;  // birds drawn on the last canonical frame (authored + workout)
    // The workout flock is drawn up to the room its buffers have. Making more waits for the GPU
    // and replaces them, so call it between frames, never while one is being recorded.
    uint32_t workoutBirdCapacity() const noexcept;
    // The buildings of the city last drawn (act 1 Shenzhen or 2 Taipei) as world-space boxes. Filled
    // on the first canonical frame of each act; `act` reports which (UINT32_MAX before any).
    const std::vector<CityCollisionBox>& cityCollision(uint32_t* act) const noexcept;
    std::vector<CityCollisionBox> buildCityCollision(uint32_t act); // the same, for a city not being drawn
    bool reserveWorkoutBirds(uint32_t birds);
    FlockFocus flockFocus() const noexcept; // where they were, for the bird camera
    uint32_t queriedRays() const;
    uint32_t fallbackKits() const;
    CityDrawStats lastCityDrawStats() const noexcept;
    void resetTemporalHistory() noexcept;
    bool hasAllAuthoredSceneAssets() const noexcept;
    void retire(uint32_t slot);
    bool resize(uint32_t width, uint32_t height);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
