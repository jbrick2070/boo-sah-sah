#pragma once

#include "boo/api.hpp"
#include <memory>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct ID3D12Device;

namespace boo {
class D3D12Context;
class TrilogyMeshRenderer;
bool initializeNgxDevice(ID3D12Device* device, const std::filesystem::path& folder);
void shutdownNgxDevice(ID3D12Device* device) noexcept;

class DlssBridge {
public:
    DlssBridge();
    ~DlssBridge();
    // Asks NVIDIA's runtime what this GPU supports. Done on the loading thread, so the first frame does not wait.
    void warmUp(D3D12Context& context);
    bool prepare(D3D12Context& context, ID3D12GraphicsCommandList* cmd, DlssMode mode, bool eligible, bool requestRR, bool raytracing,
                 ID3D12Resource* fullColor, ID3D12Resource* fullDistance);
    void resolve(ID3D12GraphicsCommandList* cmd, const TrilogyMeshRenderer& scene, float frameSeconds);
    void reset() noexcept;
    void rejectInputSize() noexcept;
    bool renderNoisyRays() const noexcept;
    RendererFeatureState state() const noexcept;
    RendererFeatureState rayReconstructionState() const noexcept;
    uint32_t width() const noexcept;
    uint32_t height() const noexcept;
    ID3D12Resource* color() const noexcept;
    ID3D12Resource* distance() const noexcept;
    const float* jitter() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
