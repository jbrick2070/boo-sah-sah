#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace boo {

class D3D12Context;

// Dynamically-loaded bridge to NVIDIA Real-Time Denoisers (NRD.dll v4.x, DXIL compute).
// Preserves the strict zero-optional-SDK static PE import gate via runtime LoadLibraryW.
// When NRD.dll is absent or disabled, callers fall back cleanly to the built-in HLSL pass.
class NrdBridge {
public:
    explicit NrdBridge(D3D12Context& ctx);
    ~NrdBridge();

    NrdBridge(const NrdBridge&) = delete;
    NrdBridge& operator=(const NrdBridge&) = delete;

    bool initialize(std::string* outReason = nullptr);
    bool resize(uint32_t width, uint32_t height);
    void shutdown() noexcept;

    bool isAvailable() const noexcept;
    const std::string& statusString() const noexcept;

    // Executes NRD REFERENCE temporal accumulation + anti-firefly compute passes on `hdrTex`
    // (RGBA16F, enters & exits in D3D12_RESOURCE_STATE_RENDER_TARGET). Returns true if NRD ran.
    bool recordAccumulation(
        void* commandListRaw,
        void* hdrResourceRaw,
        uint32_t width,
        uint32_t height,
        uint32_t frameSlot,
        uint32_t accumFrame,
        const float jitterPixels[2]);

    struct ReblurInputs {
        void* inMv = nullptr;              // RGBA16F (enters & exits NON_PIXEL_SHADER_RESOURCE; must allow UAV)
        void* inNormalRoughness = nullptr; // R10G10B10A2_UNORM (NON_PIXEL_SHADER_RESOURCE)
        void* inViewZ = nullptr;           // R32F (NON_PIXEL_SHADER_RESOURCE)
        void* inDiffRadHit = nullptr;      // RGBA16F (NON_PIXEL_SHADER_RESOURCE)
        void* inSpecRadHit = nullptr;      // RGBA16F (NON_PIXEL_SHADER_RESOURCE)
        float worldToView[16]{};
        float worldToViewPrev[16]{};
        float viewToClip[16]{};
        float viewToClipPrev[16]{};
        float jitterPixels[2]{0.0f, 0.0f};
        bool resetHistory = false;
    };

    // Executes NRD REBLUR_DIFFUSE_SPECULAR real-time motion denoiser.
    // On success, `outDiffResourceRaw()` and `outSpecResourceRaw()` are in PIXEL_SHADER_RESOURCE state.
    bool recordReblurMotion(
        void* commandListRaw,
        uint32_t width,
        uint32_t height,
        uint32_t frameSlot,
        const ReblurInputs& inputs);

    void* outDiffResourceRaw() const noexcept;
    void* outSpecResourceRaw() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
