#pragma once

#include "boo/api.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace boo {

struct AdapterInfo {
    uint32_t index = 0;
    std::string description;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint32_t subSysId = 0;
    uint32_t revision = 0;
    uint64_t dedicatedVideoMemoryBytes = 0;
    uint64_t dedicatedSystemMemoryBytes = 0;
    uint64_t sharedSystemMemoryBytes = 0;
    int32_t luidLowPart = 0;
    int32_t luidHighPart = 0;
    bool isSoftwareWarp = false;
    bool supportsD3D12 = false;
    std::string maxFeatureLevel = "None";
    std::string dxrTier = "NotSupported";
    std::string shaderModel = "Unknown";
};

struct ComputeAcceleratorDevice {
    std::string friendlyName;
    std::string instanceId;
    bool statusOk = false;
};

struct ProbeFlags {
    bool disableRT = false;
    bool disableCUDA = false;
    bool disableTensor = false;
    bool disableNPU = false;
    bool disableNVENC = false;
    bool enableSignalHijack = false;
    bool forceWarp = false;
};

struct SystemCapabilities {
    std::string osVersion;
    std::string cpuBrand;
    uint32_t logicalCores = 0;
    uint64_t totalSystemRamBytes = 0;

    std::vector<AdapterInfo> adapters;
    int32_t selectedAdapterIndex = -1;

    // Baseline D3D12 device status
    bool d3d12Available = false;
    bool d3d12UsingWarp = false;
    std::string d3d12FeatureLevel;
    std::string d3d12ShaderModel;
    std::string dxrTierString = "NotSupported";
    bool dxrTier10Supported = false;
    bool dxrTier11Supported = false;

    // Dynamic CUDA driver probe (zero link dependency)
    bool cudaDriverDllPresent = false;
    int32_t cudaDriverVersion = 0;
    std::string cudaMatchedDeviceName;
    int32_t cudaComputeMajor = 0;
    int32_t cudaComputeMinor = 0;
    int32_t cudaMultiProcessorCount = 0;
    bool cudaMatchesD3D12Luid = false;
    bool cublasLtDllPresent = false;

    // Dynamic NPU / OpenVINO probe
    std::vector<ComputeAcceleratorDevice> npuDevices;
    bool openvinoRuntimeDllPresent = false;

    // Dynamic NVENC / Media Foundation hardware encoder probe
    bool nvencDriverDllPresent = false;
    uint32_t nvencMaxApiVersionMajor = 0;
    uint32_t nvencMaxApiVersionMinor = 0;
    bool hardwareVideoEncoderMftPresent = false;
    std::string hardwareVideoEncoderName;

    // Per-engine honest HardwareResult table (RT, Tensor, NPU, CUDA, Codec)
    std::array<HardwareResult, kEngineKindCount> engines{};
};

SystemCapabilities probeSystemCapabilities(const ProbeFlags& flags);
const HardwareResult& getEngineResult(const SystemCapabilities& caps, EngineKind kind) noexcept;

} // namespace boo
