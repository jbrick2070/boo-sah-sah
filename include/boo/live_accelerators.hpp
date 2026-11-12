#pragma once

#include "boo/api.hpp"
#include "boo/capabilities.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace boo {

// 32-byte per-particle kinematic + shading state rendered directly in M1 3D space.
struct LiveParticleState {
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float age01 = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;
    float kindAndGlow = 0.0f; // < 0.5: Taipei rain-mist streak; >= 0.5: golden/orchid sanctuary pollen crystal
};

// Live per-frame attribution telemetry for a single hardware pillar (RT, CUDA, Tensor, Codec).
// Strictly separated from standalone offscreen `B` benchmark numbers.
struct LivePillarTelemetry {
    BackendStatus status = BackendStatus::Unavailable;
    bool activeInScene = false;      // True when currently driving visible pixels on screen
    bool hardwareVerified = false;   // True ONLY when real hardware executed & passed output verification
    uint32_t workloadItems = 0;      // e.g. rays, particles (65536 vs 8192), texels (320x180), or 720p pixels
    uint32_t workloadSteps = 0;      // e.g. substeps (16 vs 2), MMA ops/warp (512), or target kbps
    float executionMs = 0.0f;        // Live measured ms for the visible workload
    float throughput = 0.0f;         // MRays/s, MSteps/s, TFLOPS, or Encode+Decode FPS
    float qualityOrBitrate = 0.0f;   // Codec: PSNR (dB)
    float auxMetric = 0.0f;          // Codec: measured bitrate (Mbps)
};

// Zero-SDK dynamic live hardware accelerator manager for M1:
//   - CUDA Driver API (`nvcuda.dll` via LoadLibraryExW): 65,536 deterministic 3D curl-vortex particles
//   - Tensor Cores (`nvcuda.dll` sm_80+ PTX `mma.sync.aligned.m16n8k16`): 320x180 64-layer SIREN caustic/spectral field
//   - Hardware Codec (inbox Media Foundation H.264 Hardware Encoder MFT -> H.264 Decoder MFT): live 1280x720 round-trip
class LiveAcceleratorOrchestra {
public:
    static constexpr uint32_t kCudaParticleCount      = 65536u;
    static constexpr uint32_t kSoloCudaParticleCount  = 102400u;
    static constexpr uint32_t kMaxCudaParticleCount   = 262144u;
    static constexpr uint32_t kSubParticleCount       = 8192u;
    static constexpr uint32_t kCudaSubsteps           = 16u;
    static constexpr uint32_t kSubSubsteps            = 2u;

    static constexpr uint32_t kTensorWidth        = 320u;
    static constexpr uint32_t kTensorHeight       = 180u;
    static constexpr uint32_t kTensorLayers       = 64u;
    static constexpr uint32_t kTensorMmaPerWarp   = 512u; // 64 layers * 8 mma.sync.m16n8k16 ops/layer = 7.55 GFLOP/frame

    static constexpr uint32_t kCodecWidth         = 1280u;
    static constexpr uint32_t kCodecHeight        = 720u;
    static constexpr uint32_t kCodecTargetBitrate = 950000u; // 0.95 Mbps low-latency H.264 showcase stream

    LiveAcceleratorOrchestra();
    ~LiveAcceleratorOrchestra();

    LiveAcceleratorOrchestra(const LiveAcceleratorOrchestra&) = delete;
    LiveAcceleratorOrchestra& operator=(const LiveAcceleratorOrchestra&) = delete;

    void initialize(
        int32_t adapterLuidLow,
        int32_t adapterLuidHigh,
        bool isWarpAdapter,
        bool allowCuda,
        bool allowTensor,
        bool allowCodec);

    void shutdown() noexcept;

    bool cudaHardwareAvailable() const noexcept;
    bool tensorHardwareAvailable() const noexcept;
    bool codecHardwareAvailable() const noexcept;

    const std::string& cudaBackendName() const noexcept;
    const std::string& tensorBackendName() const noexcept;
    const std::string& codecBackendName() const noexcept;

    // Deterministic sample-locked 65,536..262,144-particle 16-substep 3D curl-vortex & sanctuary helix simulation.
    // Returns pointer to pinned host buffer of `requestedParticleCount` LiveParticleState entries on verified success,
    // or nullptr if unavailable/faulted (caller falls back to HLSL substitute).
    const LiveParticleState* simulateCudaParticles(
        int64_t samplePosition,
        int32_t sampleRate,
        uint32_t deterministicSeed,
        Edition edition,
        float orchidReveal01,
        LivePillarTelemetry& outTelemetry,
        uint32_t requestedParticleCount = kCudaParticleCount,
        uint32_t simMode = 0u) noexcept;

    // Deterministic sample-locked 320x180 64-layer Tensor Core (`mma.sync.m16n8k16.f32.f16.f16.f32`)
    // neural caustic & thin-film spectral field. Returns pointer to pinned RGBA16F (`uint64_t` per texel,
    // row pitch = 320 * 8 = 2560 bytes) buffer on verified success, or nullptr on fallback.
    const uint16_t* evaluateTensorCausticField(
        int64_t samplePosition,
        int32_t sampleRate,
        uint32_t deterministicSeed,
        Edition edition,
        float orchidReveal01,
        LivePillarTelemetry& outTelemetry) noexcept;

    // Submits a 1280x720 NV12 frame (`1,382,400` bytes) to the live Hardware H.264 Encoder -> Decoder round-trip.
    // When `waitForExactFrame` is true (headless deterministic tests / paused A-B capture), processes synchronously.
    // When false (live 60+ FPS playback), feeds the background low-latency worker without blocking the render thread
    // and copies the latest verified decoded 1280x720 RGBA8 frame into `outRgba8` (`1280 * 720 * 4` bytes).
    bool processCodecRoundTrip(
        const uint8_t* nv12FrameBytes,
        size_t nv12ByteCount,
        bool waitForExactFrame,
        std::vector<uint8_t>& outRgba8,
        LivePillarTelemetry& outTelemetry);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
