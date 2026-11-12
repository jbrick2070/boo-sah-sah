#pragma once
#include "boo/render.hpp"
#include "boo/m1_scene.hpp"
#include <memory>
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
                uint32_t slot, bool raytracing, bool canonical);
    bool hardwareVerified() const;
    uint32_t verifiedHits() const;
    uint32_t queriedRays() const;
    uint32_t fallbackKits() const;
    void retire(uint32_t slot);
    bool resize(uint32_t width, uint32_t height);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
