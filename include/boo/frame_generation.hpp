#pragma once

#include "boo/api.hpp"
#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace boo {
struct M1CameraState;
struct FrameGenerationStats {
    RendererFeatureState state=RendererFeatureState::Unavailable;
    RendererFeatureState reflexState=RendererFeatureState::Unavailable;
    uint32_t generatedPerFrame=0, maxGeneratedPerFrame=0, framesActuallyPresented=0;
    double renderedFps=0, displayedFps=0;
};

class FrameGeneration {
public:
    FrameGeneration();
    ~FrameGeneration();
    void load();
    void shutdown();
    bool attach(ID3D12Device* device);
    bool upgrade(void** native);
    void disablePresentation();
    void beginSimulation(FrameGenerationMode mode,ReflexMode reflex);
    void endSimulation();
    void beginRender();
    void beforePresent();
    void afterPresent(bool presented);
    void invalidate();
    bool needsInputs() const noexcept;
    FrameGenerationStats stats() const noexcept;
    void scene(const M1CameraState& camera,ID3D12Resource* depth,ID3D12Resource* motion,
               uint32_t width,uint32_t height,const float* jitter,bool reset);
    bool hasScene() const noexcept;
    void tag(ID3D12GraphicsCommandList* cmd,ID3D12Resource* hudless,ID3D12Resource* uiAlpha);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
