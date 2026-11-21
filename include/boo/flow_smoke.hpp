#pragma once

// PhysX suite, part one: smoke.
//
// The smoke is NVIDIA Flow, the sparse-grid smoke and fire solver of the PhysX family, built from
// the PhysX SDK checkout (tools/fetch_physx_sdk.py). Flow simulates on its own Vulkan device, on
// the same GPU the film renders on, and hands its smoke back as a sparse NanoVDB grid. The film's
// renderer then marches that grid itself (assets/shaders/flow_smoke.hlsl) against its own scene
// distance and lighting, so the smoke sits in the picture like anything else in it.
//
// Nothing here is an in-house stand-in: without the SDK the feature reports itself unavailable.
// Builds without ENABLE_PHYSX compile the same interface and always report that.

#include "boo/api.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace boo {

class D3D12Context;
struct M1CameraState;

// A patch of warm, smoky air. Metres, Y up.
struct FlowSmokeSource {
    float position[3]{0.0f, 0.0f, 0.0f};
    float radius = 3.0f;
    float rise = 2.0f;         // upward speed given to the air, metres a second
    float drift[2]{0.0f, 0.0f}; // sideways speed (x, z), metres a second
    float warmth = 1.5f;       // what makes it rise
    float smoke = 1.0f;        // what makes it show
};

struct FlowSmokeStats {
    RendererFeatureState state = RendererFeatureState::Unavailable;
    std::string reason;              // why it is unavailable, when it is
    std::string backend;             // what is running it
    float simulateCpuMs = 0.0f;      // CPU time to record and submit one Flow step
    float simulateGpuMs = 0.0f;      // GPU time of that step, from Flow's own profiler
    float renderGpuMs = 0.0f;        // GPU time of the film's ray march, from GPU timestamps
    uint32_t activeBlocks = 0;       // Flow's sparse blocks in use
    uint64_t activeVoxels = 0;       // voxels in the grid handed to the renderer
    float gridMegabytes = 0.0f;      // size of that grid
    float flowMemoryMegabytes = 0.0f; // GPU memory Flow's device holds
    float voxelMetres = 0.0f;
    uint32_t sources = 0;
    uint64_t steps = 0;              // simulation steps taken
};

class FlowSmoke {
public:
    explicit FlowSmoke(D3D12Context& d3d);
    ~FlowSmoke();
    FlowSmoke(const FlowSmoke&) = delete;
    FlowSmoke& operator=(const FlowSmoke&) = delete;

    // Builds the D3D12 side. Flow itself starts the first time smoke is switched on, and its
    // device is released again when smoke is switched off.
    bool initialize(std::string* error);

    // Advance the smoke by dtSeconds. Returns at once: the result arrives a frame or two later.
    // `reset` clears the air (a cut to another place).
    void simulate(bool enabled, float dtSeconds, const FlowSmokeSource* sources, uint32_t count, bool reset);

    // March the latest smoke over SceneHDR. Both targets must be in PIXEL_SHADER_RESOURCE state.
    void record(void* commandList, void* sceneHdr, void* sceneDistance, uint32_t width, uint32_t height,
                const M1CameraState& camera, const float lightDir[3], const float lightColor[3], const float ambient[3],
                uint64_t frameIndex, uint32_t slot, bool canonical);

    FlowSmokeStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
