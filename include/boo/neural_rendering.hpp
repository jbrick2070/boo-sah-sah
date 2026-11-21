#pragma once

// Neural rendering suite: three neural networks that run on the GPU inside the frame.
//
//   Neural shading           an MLP applied to every HDR pixel, trained offline to reproduce an analytic
//                            film grade. Changes the picture when on; quality = PSNR against that grade.
//   Neural textures          a 64x64x8 grid of 8-bit latents plus a decoder MLP rebuilds a 512x512 texture
//                            every frame (about 26x smaller than RGBA8). Quality = PSNR against the original.
//   Neural radiance cache    an MLP over a multi-resolution feature grid, trained online on 16,384 live scene
//                            samples per frame and queried for every pixel. Quality = fit PSNR on the live frame.
//
// Weights for the first two come from tools/train_neural_weights.py (assets/neural/neural_networks.bin).
// All passes are SM 6.0 compute shaders (assets/shaders/neural_rendering.hlsl) and run on any D3D12 GPU.
// Builds with ENABLE_LINALG_PREVIEW add SM 6.10 LinAlg variants that run inference on tensor cores in FP16.
// Only neural shading alters the normal frame; the other two show their output in their solo views.

#include "boo/api.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace boo {

class D3D12Context;

struct NeuralFeatureStats {
    RendererFeatureState state = RendererFeatureState::Unavailable;
    float gpuMs = 0.0f;          // Measured with GPU timestamps, smoothed
    float quality = 0.0f;        // PSNR (dB). Shading: vs the analytic grade. Textures: vs the original.
                                 // Cache: how closely the online network fits the live frame (log radiance).
    float gflopPerFrame = 0.0f;  // Network arithmetic per frame
    float tflops = 0.0f;         // gflopPerFrame / gpuMs
};

struct NeuralRenderingStats {
    bool available = false;
    std::string backend;                      // Which hardware path ran the networks
    std::string unavailableReason;
    std::array<NeuralFeatureStats, kNeuralFeatureCount> features{};
    bool tensorCoresAvailable = false;        // SM 6.10 LinAlg path built, supported and compiled
    bool tensorCoresActive = false;           // ... and selected this frame
    std::string tensorCoresReason;            // Why the tensor-core path is unavailable
    float textureCompressionRatio = 0.0f;     // RGBA8 bytes / (latents + decoder) bytes
    float offlineShadingPsnr = 0.0f;          // Measured by the trainer on held-out samples
    float offlineTexturePsnr = 0.0f;
    uint32_t cacheTrainingSteps = 0;          // Optimizer steps since the cache was last reset
    uint32_t cacheSamplesPerFrame = 0;
    uint32_t cacheParameters = 0;
};

// Enables D3D12 experimental shader models for the SM 6.10 tensor-core path. Call once before any D3D12
// device is created. No-op (false) in builds without ENABLE_LINALG_PREVIEW.
bool enableNeuralPreviewFeatures() noexcept;

class NeuralRenderingSuite {
public:
    explicit NeuralRenderingSuite(D3D12Context& d3d);
    ~NeuralRenderingSuite();
    NeuralRenderingSuite(const NeuralRenderingSuite&) = delete;
    NeuralRenderingSuite& operator=(const NeuralRenderingSuite&) = delete;

    // Loads the trained networks and builds the pipelines. False (with a reason) leaves the suite unavailable.
    bool initialize(std::string* error = nullptr);
    bool ready() const noexcept;

    // hdr (R16G16B16A16_FLOAT, UAV-capable) and depth (R32_FLOAT) must be in PIXEL_SHADER_RESOURCE on entry
    // and are returned in it. Leaves the descriptor heap binding changed; the caller rebinds its own.
    // Canonical passes train the cache and record timings; other passes only apply shading and solo views.
    void record(void* commandList, void* hdr, void* depth, uint32_t width, uint32_t height,
                const NeuralRenderingControls& controls, uint64_t frameIndex, uint32_t slot, bool canonical);

    NeuralRenderingStats stats() const;
    void resetCache() noexcept;
    void shutdown() noexcept;

    static constexpr uint32_t kCacheSamplesPerFrame = 16384u;
    static constexpr uint32_t kTextureSize = 512u;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
