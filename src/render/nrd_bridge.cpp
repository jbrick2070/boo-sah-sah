#include "boo/nrd_bridge.hpp"
#include "boo/api.hpp"
#include "boo/logging.hpp"
#include "boo/render.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "../../third_party/nvidia_nrd/Include/NRD.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <vector>

namespace boo {
namespace {

using Microsoft::WRL::ComPtr;

DXGI_FORMAT toDxgiFormat(nrd::Format f) noexcept {
    switch (f) {
    case nrd::Format::R8_UNORM:             return DXGI_FORMAT_R8_UNORM;
    case nrd::Format::R8_SNORM:             return DXGI_FORMAT_R8_SNORM;
    case nrd::Format::R8_UINT:              return DXGI_FORMAT_R8_UINT;
    case nrd::Format::R8_SINT:              return DXGI_FORMAT_R8_SINT;
    case nrd::Format::RG8_UNORM:            return DXGI_FORMAT_R8G8_UNORM;
    case nrd::Format::RG8_SNORM:            return DXGI_FORMAT_R8G8_SNORM;
    case nrd::Format::RG8_UINT:             return DXGI_FORMAT_R8G8_UINT;
    case nrd::Format::RG8_SINT:             return DXGI_FORMAT_R8G8_SINT;
    case nrd::Format::RGBA8_UNORM:          return DXGI_FORMAT_R8G8B8A8_UNORM;
    case nrd::Format::RGBA8_SNORM:          return DXGI_FORMAT_R8G8B8A8_SNORM;
    case nrd::Format::RGBA8_UINT:           return DXGI_FORMAT_R8G8B8A8_UINT;
    case nrd::Format::RGBA8_SINT:           return DXGI_FORMAT_R8G8B8A8_SINT;
    case nrd::Format::RGBA8_SRGB:           return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case nrd::Format::R16_UNORM:            return DXGI_FORMAT_R16_UNORM;
    case nrd::Format::R16_SNORM:            return DXGI_FORMAT_R16_SNORM;
    case nrd::Format::R16_UINT:             return DXGI_FORMAT_R16_UINT;
    case nrd::Format::R16_SINT:             return DXGI_FORMAT_R16_SINT;
    case nrd::Format::R16_SFLOAT:           return DXGI_FORMAT_R16_FLOAT;
    case nrd::Format::RG16_UNORM:           return DXGI_FORMAT_R16G16_UNORM;
    case nrd::Format::RG16_SNORM:           return DXGI_FORMAT_R16G16_SNORM;
    case nrd::Format::RG16_UINT:            return DXGI_FORMAT_R16G16_UINT;
    case nrd::Format::RG16_SINT:            return DXGI_FORMAT_R16G16_SINT;
    case nrd::Format::RG16_SFLOAT:          return DXGI_FORMAT_R16G16_FLOAT;
    case nrd::Format::RGBA16_UNORM:         return DXGI_FORMAT_R16G16B16A16_UNORM;
    case nrd::Format::RGBA16_SNORM:         return DXGI_FORMAT_R16G16B16A16_SNORM;
    case nrd::Format::RGBA16_UINT:          return DXGI_FORMAT_R16G16B16A16_UINT;
    case nrd::Format::RGBA16_SINT:          return DXGI_FORMAT_R16G16B16A16_SINT;
    case nrd::Format::RGBA16_SFLOAT:        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case nrd::Format::R32_UINT:             return DXGI_FORMAT_R32_UINT;
    case nrd::Format::R32_SINT:             return DXGI_FORMAT_R32_SINT;
    case nrd::Format::R32_SFLOAT:           return DXGI_FORMAT_R32_FLOAT;
    case nrd::Format::RG32_UINT:            return DXGI_FORMAT_R32G32_UINT;
    case nrd::Format::RG32_SINT:            return DXGI_FORMAT_R32G32_SINT;
    case nrd::Format::RG32_SFLOAT:          return DXGI_FORMAT_R32G32_FLOAT;
    case nrd::Format::RGBA32_UINT:          return DXGI_FORMAT_R32G32B32A32_UINT;
    case nrd::Format::RGBA32_SINT:          return DXGI_FORMAT_R32G32B32A32_SINT;
    case nrd::Format::RGBA32_SFLOAT:        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case nrd::Format::R10_G10_B10_A2_UINT:  return DXGI_FORMAT_R10G10B10A2_UINT;
    case nrd::Format::R11_G11_B10_UFLOAT:   return DXGI_FORMAT_R11G11B10_FLOAT;
    case nrd::Format::R9_G9_B9_E5_UFLOAT:   return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    default:                                return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
}

void transitionTex(
    ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    if (!res || before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
}

struct PoolTex {
    ComPtr<ID3D12Resource> tex;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct PipelineEntry {
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    bool hasCbv = false;
    uint32_t cbvParamIdx = 0;
    int32_t srvTableParamIdx = -1;
    int32_t uavTableParamIdx = -1;
    uint32_t srvCount = 0;
    uint32_t uavCount = 0;
};

} // namespace

struct NrdBridge::Impl {
    D3D12Context& ctx;
    HMODULE dll = nullptr;

    using PFN_CreateInstance       = nrd::Result(NRD_CALL*)(const nrd::InstanceCreationDesc&, nrd::Instance*&);
    using PFN_DestroyInstance      = void(NRD_CALL*)(nrd::Instance&);
    using PFN_GetLibraryDesc       = const nrd::LibraryDesc*(NRD_CALL*)();
    using PFN_GetInstanceDesc      = const nrd::InstanceDesc*(NRD_CALL*)(const nrd::Instance&);
    using PFN_SetCommonSettings    = nrd::Result(NRD_CALL*)(nrd::Instance&, const nrd::CommonSettings&);
    using PFN_SetDenoiserSettings  = nrd::Result(NRD_CALL*)(nrd::Instance&, nrd::Identifier, const void*);
    using PFN_GetComputeDispatches = nrd::Result(NRD_CALL*)(nrd::Instance&, const nrd::Identifier*, uint32_t, const nrd::DispatchDesc*&, uint32_t&);

    PFN_CreateInstance       fnCreateInstance = nullptr;
    PFN_DestroyInstance      fnDestroyInstance = nullptr;
    PFN_GetLibraryDesc       fnGetLibraryDesc = nullptr;
    PFN_GetInstanceDesc      fnGetInstanceDesc = nullptr;
    PFN_SetCommonSettings    fnSetCommonSettings = nullptr;
    PFN_SetDenoiserSettings  fnSetDenoiserSettings = nullptr;
    PFN_GetComputeDispatches fnGetComputeDispatches = nullptr;

    nrd::Instance* instance = nullptr;
    const nrd::InstanceDesc* instDesc = nullptr;

    std::vector<PipelineEntry> pipelines;
    std::vector<PoolTex> permanentPool;
    std::vector<PoolTex> transientPool;
    PoolTex outSignal;
    PoolTex outDiff;
    PoolTex outSpec;

    ComPtr<ID3D12DescriptorHeap> descHeap;
    ComPtr<ID3D12Resource> cbUpload;
    uint8_t* cbMapped = nullptr;

    static constexpr uint32_t kFrameSlots = 4;
    static constexpr uint32_t kMaxDispatchesPerFrame = 40;
    static constexpr uint32_t kDescPerDispatch = 32;
    uint32_t cbStride = 1024;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t nrdFrameIndex = 0;
    float prevJitter[2]{0.0f, 0.0f};
    bool refCleared = false;
    bool reblurCleared = false;
    bool ready = false;
    std::string status = "NRD sidecar not initialized";

    explicit Impl(D3D12Context& c) : ctx(c) {}
};

NrdBridge::NrdBridge(D3D12Context& ctx) : impl_(std::make_unique<Impl>(ctx)) {}

NrdBridge::~NrdBridge() {
    shutdown();
}

void NrdBridge::shutdown() noexcept {
    if (!impl_) return;
    auto& s = *impl_;
    if (s.cbUpload && s.cbMapped) {
        s.cbUpload->Unmap(0, nullptr);
        s.cbMapped = nullptr;
    }
    s.cbUpload.Reset();
    s.descHeap.Reset();
    s.outSignal = {};
    s.outDiff = {};
    s.outSpec = {};
    s.permanentPool.clear();
    s.transientPool.clear();
    s.pipelines.clear();
    if (s.instance && s.fnDestroyInstance) {
        s.fnDestroyInstance(*s.instance);
        s.instance = nullptr;
    }
    s.instDesc = nullptr;
    if (s.dll) {
        FreeLibrary(s.dll);
        s.dll = nullptr;
    }
    s.ready = false;
    s.refCleared = false;
    s.reblurCleared = false;
    s.width = 0;
    s.height = 0;
}

bool NrdBridge::isAvailable() const noexcept {
    return impl_ && impl_->ready;
}

const std::string& NrdBridge::statusString() const noexcept {
    return impl_->status;
}

void* NrdBridge::outDiffResourceRaw() const noexcept {
    return (impl_ && impl_->outDiff.tex) ? impl_->outDiff.tex.Get() : nullptr;
}

void* NrdBridge::outSpecResourceRaw() const noexcept {
    return (impl_ && impl_->outSpec.tex) ? impl_->outSpec.tex.Get() : nullptr;
}

bool NrdBridge::initialize(std::string* outReason) {
    shutdown();
    auto& s = *impl_;
    if (s.ctx.isWarp() || !s.ctx.deviceRaw()) {
        s.status = "NRD skipped (WARP / no D3D12 device)";
        if (outReason) *outReason = s.status;
        return false;
    }

    // Try executable-sidecar nrd\NRD.dll first, then third_party\nvidia_nrd\bin\NRD.dll for dev builds
    std::vector<std::filesystem::path> candidates;
    wchar_t exeBuf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) > 0) {
        const auto exeDir = std::filesystem::path(exeBuf).parent_path();
        candidates.push_back(exeDir / "nrd" / "NRD.dll");
        candidates.push_back(exeDir / "NRD.dll");
    }
    candidates.push_back(resolveResourcePath("nrd/NRD.dll"));
    candidates.push_back(resolveResourcePath("third_party/nvidia_nrd/bin/NRD.dll"));

    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            s.dll = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (s.dll) break;
        }
    }
    if (!s.dll) {
        s.status = "NRD.dll sidecar absent; using built-in HLSL accumulator";
        if (outReason) *outReason = s.status;
        return false;
    }

    s.fnCreateInstance       = reinterpret_cast<Impl::PFN_CreateInstance>(GetProcAddress(s.dll, "CreateInstance"));
    s.fnDestroyInstance      = reinterpret_cast<Impl::PFN_DestroyInstance>(GetProcAddress(s.dll, "DestroyInstance"));
    s.fnGetLibraryDesc       = reinterpret_cast<Impl::PFN_GetLibraryDesc>(GetProcAddress(s.dll, "GetLibraryDesc"));
    s.fnGetInstanceDesc      = reinterpret_cast<Impl::PFN_GetInstanceDesc>(GetProcAddress(s.dll, "GetInstanceDesc"));
    s.fnSetCommonSettings    = reinterpret_cast<Impl::PFN_SetCommonSettings>(GetProcAddress(s.dll, "SetCommonSettings"));
    s.fnSetDenoiserSettings  = reinterpret_cast<Impl::PFN_SetDenoiserSettings>(GetProcAddress(s.dll, "SetDenoiserSettings"));
    s.fnGetComputeDispatches = reinterpret_cast<Impl::PFN_GetComputeDispatches>(GetProcAddress(s.dll, "GetComputeDispatches"));

    if (!s.fnCreateInstance || !s.fnDestroyInstance || !s.fnGetLibraryDesc ||
        !s.fnGetInstanceDesc || !s.fnSetCommonSettings || !s.fnSetDenoiserSettings || !s.fnGetComputeDispatches) {
        s.status = "NRD.dll missing required entry points";
        if (outReason) *outReason = s.status;
        shutdown();
        return false;
    }

    const nrd::LibraryDesc* lib = s.fnGetLibraryDesc();
    if (!lib || lib->versionMajor != NRD_VERSION_MAJOR) {
        s.status = "NRD.dll ABI version mismatch";
        if (outReason) *outReason = s.status;
        shutdown();
        return false;
    }

    const nrd::DenoiserDesc denDescs[2] = {
        {0u, nrd::Denoiser::REFERENCE},
        {1u, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR},
    };
    nrd::InstanceCreationDesc createDesc{};
    createDesc.denoisers = denDescs;
    createDesc.denoisersNum = 2u;

    if (s.fnCreateInstance(createDesc, s.instance) != nrd::Result::SUCCESS || !s.instance) {
        s.status = "NRD CreateInstance(REFERENCE + REBLUR_DIFFUSE_SPECULAR) failed";
        if (outReason) *outReason = s.status;
        shutdown();
        return false;
    }

    s.instDesc = s.fnGetInstanceDesc(*s.instance);
    if (!s.instDesc || s.instDesc->pipelinesNum == 0) {
        s.status = "NRD InstanceDesc returned 0 DXIL pipelines";
        if (outReason) *outReason = s.status;
        shutdown();
        return false;
    }
    s.cbStride = (std::max<uint32_t>(256u, s.instDesc->constantBufferMaxDataSize) + 255u) & ~255u;

    auto* dev = static_cast<ID3D12Device*>(s.ctx.deviceRaw());

    // Build tight per-pipeline D3D12 Root Signatures + Compute PSOs from NRD's embedded DXIL shaders
    s.pipelines.resize(s.instDesc->pipelinesNum);
    for (uint32_t i = 0; i < s.instDesc->pipelinesNum; ++i) {
        const nrd::PipelineDesc& pd = s.instDesc->pipelines[i];
        if (!pd.computeShaderDXIL.bytecode || pd.computeShaderDXIL.size == 0) {
            s.status = "NRD pipeline missing DXIL bytecode";
            if (outReason) *outReason = s.status;
            shutdown();
            return false;
        }

        PipelineEntry pe{};
        std::vector<D3D12_ROOT_PARAMETER> rootParams;
        D3D12_DESCRIPTOR_RANGE srvRange{};
        D3D12_DESCRIPTOR_RANGE uavRange{};

        if (pd.hasConstantData) {
            D3D12_ROOT_PARAMETER cbParam{};
            cbParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            cbParam.Descriptor.ShaderRegister = s.instDesc->constantBufferRegisterIndex;
            cbParam.Descriptor.RegisterSpace = s.instDesc->constantBufferAndSamplersSpaceIndex;
            cbParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            pe.hasCbv = true;
            pe.cbvParamIdx = static_cast<uint32_t>(rootParams.size());
            rootParams.push_back(cbParam);
        }

        for (uint32_t r = 0; r < pd.resourceRangesNum; ++r) {
            const nrd::ResourceRangeDesc& rr = pd.resourceRanges[r];
            if (rr.descriptorsNum == 0) continue;
            if (rr.descriptorType == nrd::DescriptorType::TEXTURE) {
                srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                srvRange.NumDescriptors = rr.descriptorsNum;
                srvRange.BaseShaderRegister = s.instDesc->resourcesBaseRegisterIndex;
                srvRange.RegisterSpace = s.instDesc->resourcesSpaceIndex;
                srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

                D3D12_ROOT_PARAMETER tp{};
                tp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                tp.DescriptorTable = {1u, &srvRange};
                tp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                pe.srvTableParamIdx = static_cast<int32_t>(rootParams.size());
                pe.srvCount = rr.descriptorsNum;
                rootParams.push_back(tp);
            } else {
                uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                uavRange.NumDescriptors = rr.descriptorsNum;
                uavRange.BaseShaderRegister = s.instDesc->resourcesBaseRegisterIndex;
                uavRange.RegisterSpace = s.instDesc->resourcesSpaceIndex;
                uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

                D3D12_ROOT_PARAMETER tp{};
                tp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                tp.DescriptorTable = {1u, &uavRange};
                tp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
                pe.uavTableParamIdx = static_cast<int32_t>(rootParams.size());
                pe.uavCount = rr.descriptorsNum;
                rootParams.push_back(tp);
            }
        }

        std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers(s.instDesc->samplersNum);
        for (uint32_t sm = 0; sm < s.instDesc->samplersNum; ++sm) {
            auto& sd = staticSamplers[sm];
            const bool linear = (s.instDesc->samplers[sm] == nrd::Sampler::LINEAR_CLAMP);
            sd.Filter = linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.ShaderRegister = s.instDesc->samplersBaseRegisterIndex + sm;
            sd.RegisterSpace = s.instDesc->constantBufferAndSamplersSpaceIndex;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            sd.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = static_cast<UINT>(rootParams.size());
        rsd.pParameters = rootParams.empty() ? nullptr : rootParams.data();
        rsd.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
        rsd.pStaticSamplers = staticSamplers.empty() ? nullptr : staticSamplers.data();

        ComPtr<ID3DBlob> sigBlob, sigErr;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigErr)) ||
            FAILED(dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&pe.rootSig)))) {
            s.status = "NRD D3D12 RootSignature creation failed";
            if (outReason) *outReason = s.status;
            shutdown();
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpsd{};
        cpsd.pRootSignature = pe.rootSig.Get();
        cpsd.CS = {pd.computeShaderDXIL.bytecode, static_cast<SIZE_T>(pd.computeShaderDXIL.size)};
        if (FAILED(dev->CreateComputePipelineState(&cpsd, IID_PPV_ARGS(&pe.pso)))) {
            s.status = "NRD D3D12 ComputePSO creation failed";
            if (outReason) *outReason = s.status;
            shutdown();
            return false;
        }
        s.pipelines[i] = std::move(pe);
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = Impl::kFrameSlots * Impl::kMaxDispatchesPerFrame * Impl::kDescPerDispatch;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.descHeap)))) {
        s.status = "NRD descriptor heap allocation failed";
        if (outReason) *outReason = s.status;
        shutdown();
        return false;
    }

    D3D12_HEAP_PROPERTIES upHeap{};
    upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC cbDesc{};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = uint64_t(Impl::kFrameSlots) * Impl::kMaxDispatchesPerFrame * s.cbStride;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RANGE emptyRead{0, 0};
    if (FAILED(dev->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s.cbUpload))) ||
        FAILED(s.cbUpload->Map(0, &emptyRead, reinterpret_cast<void**>(&s.cbMapped)))) {
        s.status = "NRD constant upload buffer allocation failed";
        if (outReason) *outReason = s.status;
        shutdown();
        return false;
    }

    nrd::ReferenceSettings refSettings{};
    refSettings.maxAccumulatedFrameNum = nrd::REFERENCE_MAX_HISTORY_FRAME_NUM;
    s.fnSetDenoiserSettings(*s.instance, 0u, &refSettings);

    nrd::ReblurSettings reblurSettings{};
    reblurSettings.hitDistanceParameters = {3.0f, 0.1f, 20.0f};
    reblurSettings.maxAccumulatedFrameNum = 28u;
    reblurSettings.maxFastAccumulatedFrameNum = 6u;
    reblurSettings.diffusePrepassBlurRadius = 24.0f;
    reblurSettings.specularPrepassBlurRadius = 36.0f;
    reblurSettings.minBlurRadius = 1.0f;
    reblurSettings.maxBlurRadius = 26.0f;
    reblurSettings.fastHistoryClampingSigmaScale = 1.75f;
    reblurSettings.enableAntiFirefly = true;
    s.fnSetDenoiserSettings(*s.instance, 1u, &reblurSettings);

    s.ready = true;
    s.status = "NVIDIA NRD v" + std::to_string(lib->versionMajor) + "." +
               std::to_string(lib->versionMinor) + "." + std::to_string(lib->versionBuild) +
               " (REBLUR_DIFFUSE_SPECULAR + REFERENCE DXIL compute)";
    BOO_LOG_INFO("NRD", "Loaded " + s.status);
    if (outReason) *outReason = s.status;
    return true;
}

bool NrdBridge::resize(uint32_t width, uint32_t height) {
    auto& s = *impl_;
    if (!s.ready || !s.instDesc || width == 0 || height == 0) return false;
    if (s.width == width && s.height == height && s.outSignal.tex && s.outDiff.tex && s.outSpec.tex) return true;

    auto* dev = static_cast<ID3D12Device*>(s.ctx.deviceRaw());
    if (!dev) return false;

    auto allocPoolTex = [&](DXGI_FORMAT fmt, uint32_t w, uint32_t h, PoolTex& out) -> bool {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = std::max(1u, w);
        rd.Height = std::max(1u, h);
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> res;
        if (FAILED(dev->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res)))) {
            return false;
        }
        out.tex = std::move(res);
        out.format = fmt;
        out.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return true;
    };

    s.permanentPool.clear();
    s.transientPool.clear();
    s.permanentPool.resize(s.instDesc->permanentPoolSize);
    for (uint32_t i = 0; i < s.instDesc->permanentPoolSize; ++i) {
        const auto& td = s.instDesc->permanentPool[i];
        const uint32_t div = std::max<uint32_t>(1u, td.downsampleFactor);
        if (!allocPoolTex(toDxgiFormat(td.format), (width + div - 1u) / div, (height + div - 1u) / div, s.permanentPool[i])) {
            return false;
        }
    }
    s.transientPool.resize(s.instDesc->transientPoolSize);
    for (uint32_t i = 0; i < s.instDesc->transientPoolSize; ++i) {
        const auto& td = s.instDesc->transientPool[i];
        const uint32_t div = std::max<uint32_t>(1u, td.downsampleFactor);
        if (!allocPoolTex(toDxgiFormat(td.format), (width + div - 1u) / div, (height + div - 1u) / div, s.transientPool[i])) {
            return false;
        }
    }
    if (!allocPoolTex(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, s.outSignal) ||
        !allocPoolTex(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, s.outDiff) ||
        !allocPoolTex(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, s.outSpec)) {
        return false;
    }

    s.refCleared = false;
    s.reblurCleared = false;
    s.width = width;
    s.height = height;
    return true;
}

bool NrdBridge::recordAccumulation(
    void* commandListRaw,
    void* hdrResourceRaw,
    uint32_t width,
    uint32_t height,
    uint32_t frameSlot,
    uint32_t accumFrame,
    const float jitterPixels[2]) {
    auto& s = *impl_;
    if (!s.ready || !commandListRaw || !hdrResourceRaw) return false;
    if (s.width != width || s.height != height || !s.outSignal.tex) {
        if (!resize(width, height)) return false;
    }

    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandListRaw);
    auto* hdr = static_cast<ID3D12Resource*>(hdrResourceRaw);
    auto* dev = static_cast<ID3D12Device*>(s.ctx.deviceRaw());

    nrd::CommonSettings cs{};
    cs.viewToClipMatrix[0] = cs.viewToClipMatrix[5] = cs.viewToClipMatrix[10] = cs.viewToClipMatrix[15] = 1.0f;
    cs.viewToClipMatrixPrev[0] = cs.viewToClipMatrixPrev[5] = cs.viewToClipMatrixPrev[10] = cs.viewToClipMatrixPrev[15] = 1.0f;
    cs.worldToViewMatrix[0] = cs.worldToViewMatrix[5] = cs.worldToViewMatrix[10] = cs.worldToViewMatrix[15] = 1.0f;
    cs.worldToViewMatrixPrev[0] = cs.worldToViewMatrixPrev[5] = cs.worldToViewMatrixPrev[10] = cs.worldToViewMatrixPrev[15] = 1.0f;
    cs.resourceSize[0] = cs.resourceSizePrev[0] = cs.rectSize[0] = cs.rectSizePrev[0] = static_cast<uint16_t>(width);
    cs.resourceSize[1] = cs.resourceSizePrev[1] = cs.rectSize[1] = cs.rectSizePrev[1] = static_cast<uint16_t>(height);
    cs.cameraJitter[0] = std::clamp(jitterPixels[0], -0.5f, 0.5f);
    cs.cameraJitter[1] = std::clamp(jitterPixels[1], -0.5f, 0.5f);
    cs.cameraJitterPrev[0] = (accumFrame == 0u) ? cs.cameraJitter[0] : s.prevJitter[0];
    cs.cameraJitterPrev[1] = (accumFrame == 0u) ? cs.cameraJitter[1] : s.prevJitter[1];
    s.prevJitter[0] = cs.cameraJitter[0];
    s.prevJitter[1] = cs.cameraJitter[1];
    cs.frameIndex = ++s.nrdFrameIndex;
    if (!s.refCleared) {
        cs.accumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
        s.refCleared = true;
    } else {
        cs.accumulationMode = (accumFrame == 0u) ? nrd::AccumulationMode::RESTART : nrd::AccumulationMode::CONTINUE;
    }

    if (s.fnSetCommonSettings(*s.instance, cs) != nrd::Result::SUCCESS) {
        return false;
    }

    const nrd::Identifier id = 0u;
    const nrd::DispatchDesc* dispatches = nullptr;
    uint32_t dispatchNum = 0;
    if (s.fnGetComputeDispatches(*s.instance, &id, 1u, dispatches, dispatchNum) != nrd::Result::SUCCESS ||
        !dispatches || dispatchNum == 0 || dispatchNum > Impl::kMaxDispatchesPerFrame) {
        return false;
    }

    transitionTex(cmd, hdr, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12DescriptorHeap* heaps[] = {s.descHeap.Get()};
    cmd->SetDescriptorHeaps(1u, heaps);

    const UINT descInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t slotIdx = frameSlot % Impl::kFrameSlots;

    for (uint32_t dIdx = 0; dIdx < dispatchNum; ++dIdx) {
        const nrd::DispatchDesc& dd = dispatches[dIdx];
        if (dd.pipelineIndex >= s.pipelines.size()) continue;
        const PipelineEntry& pe = s.pipelines[dd.pipelineIndex];
        if (pe.srvCount + pe.uavCount > Impl::kDescPerDispatch) return false;

        const uint32_t baseSlot = (slotIdx * Impl::kMaxDispatchesPerFrame + dIdx) * Impl::kDescPerDispatch;
        auto cpuAt = [&](uint32_t off) {
            auto h = s.descHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(baseSlot + off) * descInc;
            return h;
        };
        auto gpuAt = [&](uint32_t off) {
            auto h = s.descHeap->GetGPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<UINT64>(baseSlot + off) * descInc;
            return h;
        };

        uint32_t srvIdx = 0;
        uint32_t uavIdx = 0;
        for (uint32_t r = 0; r < dd.resourcesNum; ++r) {
            const nrd::ResourceDesc& rd = dd.resources[r];
            PoolTex* poolEntry = nullptr;
            ID3D12Resource* resPtr = nullptr;
            DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;

            if (rd.type == nrd::ResourceType::IN_SIGNAL) {
                resPtr = hdr;
                fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
            } else if (rd.type == nrd::ResourceType::OUT_SIGNAL) {
                poolEntry = &s.outSignal;
            } else if (rd.type == nrd::ResourceType::PERMANENT_POOL && rd.indexInPool < s.permanentPool.size()) {
                poolEntry = &s.permanentPool[rd.indexInPool];
            } else if (rd.type == nrd::ResourceType::TRANSIENT_POOL && rd.indexInPool < s.transientPool.size()) {
                poolEntry = &s.transientPool[rd.indexInPool];
            }

            if (poolEntry) {
                resPtr = poolEntry->tex.Get();
                fmt = poolEntry->format;
                const D3D12_RESOURCE_STATES targetState =
                    (rd.descriptorType == nrd::DescriptorType::TEXTURE)
                        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                        : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                transitionTex(cmd, resPtr, poolEntry->state, targetState);
                poolEntry->state = targetState;
            }

            if (!resPtr) return false;

            if (rd.descriptorType == nrd::DescriptorType::TEXTURE) {
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Format = fmt;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                dev->CreateShaderResourceView(resPtr, &sd, cpuAt(srvIdx++));
            } else {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = fmt;
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                dev->CreateUnorderedAccessView(resPtr, nullptr, &ud, cpuAt(pe.srvCount + uavIdx++));
            }
        }

        cmd->SetComputeRootSignature(pe.rootSig.Get());
        cmd->SetPipelineState(pe.pso.Get());

        if (pe.hasCbv) {
            const uint32_t cbOff = (slotIdx * Impl::kMaxDispatchesPerFrame + dIdx) * s.cbStride;
            if (dd.constantBufferData && dd.constantBufferDataSize > 0 && dd.constantBufferDataSize <= s.cbStride) {
                std::memcpy(s.cbMapped + cbOff, dd.constantBufferData, dd.constantBufferDataSize);
            }
            cmd->SetComputeRootConstantBufferView(pe.cbvParamIdx, s.cbUpload->GetGPUVirtualAddress() + cbOff);
        }
        if (pe.srvTableParamIdx >= 0) {
            cmd->SetComputeRootDescriptorTable(static_cast<UINT>(pe.srvTableParamIdx), gpuAt(0u));
        }
        if (pe.uavTableParamIdx >= 0) {
            cmd->SetComputeRootDescriptorTable(static_cast<UINT>(pe.uavTableParamIdx), gpuAt(pe.srvCount));
        }

        cmd->Dispatch(dd.gridWidth, dd.gridHeight, 1u);

        for (uint32_t r = 0; r < dd.resourcesNum; ++r) {
            if (dd.resources[r].descriptorType == nrd::DescriptorType::STORAGE_TEXTURE) {
                ID3D12Resource* uavRes = nullptr;
                if (dd.resources[r].type == nrd::ResourceType::OUT_SIGNAL) uavRes = s.outSignal.tex.Get();
                else if (dd.resources[r].type == nrd::ResourceType::PERMANENT_POOL) uavRes = s.permanentPool[dd.resources[r].indexInPool].tex.Get();
                else if (dd.resources[r].type == nrd::ResourceType::TRANSIENT_POOL) uavRes = s.transientPool[dd.resources[r].indexInPool].tex.Get();
                if (uavRes) {
                    D3D12_RESOURCE_BARRIER ub{};
                    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    ub.UAV.pResource = uavRes;
                    cmd->ResourceBarrier(1, &ub);
                }
            }
        }
    }

    transitionTex(cmd, s.outSignal.tex.Get(), s.outSignal.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    s.outSignal.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    transitionTex(cmd, hdr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(hdr, s.outSignal.tex.Get());
    transitionTex(cmd, hdr, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    transitionTex(cmd, s.outSignal.tex.Get(), s.outSignal.state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.outSignal.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    return true;
}

bool NrdBridge::recordReblurMotion(
    void* commandListRaw,
    uint32_t width,
    uint32_t height,
    uint32_t frameSlot,
    const ReblurInputs& inputs) {
    auto& s = *impl_;
    if (!s.ready || !commandListRaw || !inputs.inMv || !inputs.inNormalRoughness ||
        !inputs.inViewZ || !inputs.inDiffRadHit || !inputs.inSpecRadHit) {
        return false;
    }
    if (s.width != width || s.height != height || !s.outDiff.tex || !s.outSpec.tex) {
        if (!resize(width, height)) return false;
    }

    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandListRaw);
    auto* dev = static_cast<ID3D12Device*>(s.ctx.deviceRaw());
    auto* inMv = static_cast<ID3D12Resource*>(inputs.inMv);
    auto* inNormRough = static_cast<ID3D12Resource*>(inputs.inNormalRoughness);
    auto* inViewZ = static_cast<ID3D12Resource*>(inputs.inViewZ);
    auto* inDiff = static_cast<ID3D12Resource*>(inputs.inDiffRadHit);
    auto* inSpec = static_cast<ID3D12Resource*>(inputs.inSpecRadHit);

    nrd::CommonSettings cs{};
    std::memcpy(cs.worldToViewMatrix, inputs.worldToView, sizeof(cs.worldToViewMatrix));
    std::memcpy(cs.worldToViewMatrixPrev, inputs.worldToViewPrev, sizeof(cs.worldToViewMatrixPrev));
    std::memcpy(cs.viewToClipMatrix, inputs.viewToClip, sizeof(cs.viewToClipMatrix));
    std::memcpy(cs.viewToClipMatrixPrev, inputs.viewToClipPrev, sizeof(cs.viewToClipMatrixPrev));
    cs.motionVectorScale[0] = 1.0f;
    cs.motionVectorScale[1] = 1.0f;
    cs.motionVectorScale[2] = 1.0f;
    cs.isMotionVectorInWorldSpace = true;
    cs.denoisingRange = 500.0f;
    cs.disocclusionThreshold = 0.015f;
    cs.resourceSize[0] = cs.resourceSizePrev[0] = cs.rectSize[0] = cs.rectSizePrev[0] = static_cast<uint16_t>(width);
    cs.resourceSize[1] = cs.resourceSizePrev[1] = cs.rectSize[1] = cs.rectSizePrev[1] = static_cast<uint16_t>(height);
    cs.cameraJitter[0] = std::clamp(inputs.jitterPixels[0], -0.5f, 0.5f);
    cs.cameraJitter[1] = std::clamp(inputs.jitterPixels[1], -0.5f, 0.5f);
    cs.cameraJitterPrev[0] = inputs.resetHistory ? cs.cameraJitter[0] : s.prevJitter[0];
    cs.cameraJitterPrev[1] = inputs.resetHistory ? cs.cameraJitter[1] : s.prevJitter[1];
    s.prevJitter[0] = cs.cameraJitter[0];
    s.prevJitter[1] = cs.cameraJitter[1];
    cs.frameIndex = ++s.nrdFrameIndex;
    if (!s.reblurCleared) {
        cs.accumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
        s.reblurCleared = true;
    } else {
        cs.accumulationMode = inputs.resetHistory ? nrd::AccumulationMode::RESTART : nrd::AccumulationMode::CONTINUE;
    }

    if (s.fnSetCommonSettings(*s.instance, cs) != nrd::Result::SUCCESS) {
        return false;
    }

    const nrd::Identifier id = 1u;
    const nrd::DispatchDesc* dispatches = nullptr;
    uint32_t dispatchNum = 0;
    if (s.fnGetComputeDispatches(*s.instance, &id, 1u, dispatches, dispatchNum) != nrd::Result::SUCCESS ||
        !dispatches || dispatchNum == 0 || dispatchNum > Impl::kMaxDispatchesPerFrame) {
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = {s.descHeap.Get()};
    cmd->SetDescriptorHeaps(1u, heaps);

    const UINT descInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t slotIdx = frameSlot % Impl::kFrameSlots;
    D3D12_RESOURCE_STATES mvState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    for (uint32_t dIdx = 0; dIdx < dispatchNum; ++dIdx) {
        const nrd::DispatchDesc& dd = dispatches[dIdx];
        if (dd.pipelineIndex >= s.pipelines.size()) continue;
        const PipelineEntry& pe = s.pipelines[dd.pipelineIndex];
        if (pe.srvCount + pe.uavCount > Impl::kDescPerDispatch) {
            transitionTex(cmd, inMv, mvState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            return false;
        }

        const uint32_t baseSlot = (slotIdx * Impl::kMaxDispatchesPerFrame + dIdx) * Impl::kDescPerDispatch;
        auto cpuAt = [&](uint32_t off) {
            auto h = s.descHeap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<SIZE_T>(baseSlot + off) * descInc;
            return h;
        };
        auto gpuAt = [&](uint32_t off) {
            auto h = s.descHeap->GetGPUDescriptorHandleForHeapStart();
            h.ptr += static_cast<UINT64>(baseSlot + off) * descInc;
            return h;
        };

        uint32_t srvIdx = 0;
        uint32_t uavIdx = 0;
        for (uint32_t r = 0; r < dd.resourcesNum; ++r) {
            const nrd::ResourceDesc& rd = dd.resources[r];
            PoolTex* poolEntry = nullptr;
            ID3D12Resource* resPtr = nullptr;
            DXGI_FORMAT fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;

            switch (rd.type) {
            case nrd::ResourceType::IN_MV: {
                resPtr = inMv;
                fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
                const D3D12_RESOURCE_STATES targetState =
                    (rd.descriptorType == nrd::DescriptorType::TEXTURE)
                        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                        : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                transitionTex(cmd, inMv, mvState, targetState);
                mvState = targetState;
                break;
            }
            case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
                resPtr = inNormRough;
                fmt = DXGI_FORMAT_R10G10B10A2_UNORM;
                break;
            case nrd::ResourceType::IN_VIEWZ:
                resPtr = inViewZ;
                fmt = DXGI_FORMAT_R32_FLOAT;
                break;
            case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
                resPtr = inDiff;
                fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
                break;
            case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
                resPtr = inSpec;
                fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
                break;
            case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
                poolEntry = &s.outDiff;
                break;
            case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
                poolEntry = &s.outSpec;
                break;
            case nrd::ResourceType::PERMANENT_POOL:
                if (rd.indexInPool < s.permanentPool.size()) poolEntry = &s.permanentPool[rd.indexInPool];
                break;
            case nrd::ResourceType::TRANSIENT_POOL:
                if (rd.indexInPool < s.transientPool.size()) poolEntry = &s.transientPool[rd.indexInPool];
                break;
            default:
                break;
            }

            if (poolEntry) {
                resPtr = poolEntry->tex.Get();
                fmt = poolEntry->format;
                const D3D12_RESOURCE_STATES targetState =
                    (rd.descriptorType == nrd::DescriptorType::TEXTURE)
                        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                        : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                transitionTex(cmd, resPtr, poolEntry->state, targetState);
                poolEntry->state = targetState;
            }

            if (!resPtr) {
                transitionTex(cmd, inMv, mvState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                return false;
            }

            if (rd.descriptorType == nrd::DescriptorType::TEXTURE) {
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Format = fmt;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                dev->CreateShaderResourceView(resPtr, &sd, cpuAt(srvIdx++));
            } else {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = fmt;
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                dev->CreateUnorderedAccessView(resPtr, nullptr, &ud, cpuAt(pe.srvCount + uavIdx++));
            }
        }

        cmd->SetComputeRootSignature(pe.rootSig.Get());
        cmd->SetPipelineState(pe.pso.Get());

        if (pe.hasCbv) {
            const uint32_t cbOff = (slotIdx * Impl::kMaxDispatchesPerFrame + dIdx) * s.cbStride;
            if (dd.constantBufferData && dd.constantBufferDataSize > 0 && dd.constantBufferDataSize <= s.cbStride) {
                std::memcpy(s.cbMapped + cbOff, dd.constantBufferData, dd.constantBufferDataSize);
            }
            cmd->SetComputeRootConstantBufferView(pe.cbvParamIdx, s.cbUpload->GetGPUVirtualAddress() + cbOff);
        }
        if (pe.srvTableParamIdx >= 0) {
            cmd->SetComputeRootDescriptorTable(static_cast<UINT>(pe.srvTableParamIdx), gpuAt(0u));
        }
        if (pe.uavTableParamIdx >= 0) {
            cmd->SetComputeRootDescriptorTable(static_cast<UINT>(pe.uavTableParamIdx), gpuAt(pe.srvCount));
        }

        cmd->Dispatch(dd.gridWidth, dd.gridHeight, 1u);

        for (uint32_t r = 0; r < dd.resourcesNum; ++r) {
            if (dd.resources[r].descriptorType == nrd::DescriptorType::STORAGE_TEXTURE) {
                ID3D12Resource* uavRes = nullptr;
                switch (dd.resources[r].type) {
                case nrd::ResourceType::IN_MV: uavRes = inMv; break;
                case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST: uavRes = s.outDiff.tex.Get(); break;
                case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST: uavRes = s.outSpec.tex.Get(); break;
                case nrd::ResourceType::PERMANENT_POOL: uavRes = s.permanentPool[dd.resources[r].indexInPool].tex.Get(); break;
                case nrd::ResourceType::TRANSIENT_POOL: uavRes = s.transientPool[dd.resources[r].indexInPool].tex.Get(); break;
                default: break;
                }
                if (uavRes) {
                    D3D12_RESOURCE_BARRIER ub{};
                    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    ub.UAV.pResource = uavRes;
                    cmd->ResourceBarrier(1, &ub);
                }
            }
        }
    }

    transitionTex(cmd, inMv, mvState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transitionTex(cmd, s.outDiff.tex.Get(), s.outDiff.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    s.outDiff.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    transitionTex(cmd, s.outSpec.tex.Get(), s.outSpec.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    s.outSpec.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    return true;
}

} // namespace boo
