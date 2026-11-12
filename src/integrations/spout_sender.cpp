#include "boo/spout_sender.hpp"
#include "boo/api.hpp"
#include "boo/logging.hpp"
#include "boo/render.hpp"
#include "m0_fallback_vs.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace boo {

using Microsoft::WRL::ComPtr;

namespace {

constexpr size_t kSpoutNameSlotBytes = 256;
constexpr size_t kSpoutDefaultMaxSenders = 64;
constexpr uint32_t kDxgiFormatB8G8R8A8Unorm = 87u; // DXGI_FORMAT_B8G8R8A8_UNORM

bool loadBinaryShaderFile(const std::filesystem::path& p, std::vector<uint8_t>& outBytes) {
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const auto sz = ifs.tellg();
    if (sz <= 0) return false;
    outBytes.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    return static_cast<bool>(ifs.read(reinterpret_cast<char*>(outBytes.data()), sz));
}

std::string sanitizeSenderName(std::string_view raw) {
    std::string out;
    out.reserve(std::min<size_t>(raw.size(), 240));
    for (char c : raw) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 32 && uc < 127 && c != '\\' && c != '/') {
            out.push_back(c);
            if (out.size() >= 240) break;
        }
    }
    if (out.empty()) {
        out = "BooSahSah_Clean";
    }
    return out;
}

} // namespace

bool querySpoutSenderInfo(
    std::string_view senderName,
    SpoutSharedTextureInfo& outInfo,
    bool* outRegisteredInNameList) noexcept {
    outInfo = SpoutSharedTextureInfo{};
    if (outRegisteredInNameList) *outRegisteredInNameList = false;
    if (senderName.empty() || senderName.size() >= kSpoutNameSlotBytes) {
        return false;
    }

    const std::string name(senderName);
    if (outRegisteredInNameList) {
        HANDLE hNames = OpenFileMappingA(FILE_MAP_READ, FALSE, "SpoutSenderNames");
        if (hNames) {
            const void* view = MapViewOfFile(hNames, FILE_MAP_READ, 0, 0, 0);
            if (view) {
                MEMORY_BASIC_INFORMATION mbi{};
                const size_t totalBytes = (VirtualQuery(view, &mbi, sizeof(mbi)) != 0) ? mbi.RegionSize : (10 * kSpoutNameSlotBytes);
                const size_t slotCount = totalBytes / kSpoutNameSlotBytes;
                const auto* base = static_cast<const char*>(view);
                for (size_t i = 0; i < slotCount; ++i) {
                    const char* slot = base + i * kSpoutNameSlotBytes;
                    if (slot[0] == '\0') break;
                    if (std::strncmp(slot, name.c_str(), kSpoutNameSlotBytes - 1) == 0) {
                        *outRegisteredInNameList = true;
                        break;
                    }
                }
                UnmapViewOfFile(view);
            }
            CloseHandle(hNames);
        }
    }

    HANDLE hInfo = OpenFileMappingA(FILE_MAP_READ, FALSE, name.c_str());
    if (!hInfo) {
        return false;
    }
    const void* infoView = MapViewOfFile(hInfo, FILE_MAP_READ, 0, 0, sizeof(SpoutSharedTextureInfo));
    bool ok = false;
    if (infoView) {
        std::memcpy(&outInfo, infoView, sizeof(SpoutSharedTextureInfo));
        UnmapViewOfFile(infoView);
        ok = (outInfo.shareHandle != 0u && outInfo.width > 0u && outInfo.height > 0u);
    }
    CloseHandle(hInfo);
    return ok;
}

struct SpoutSender::Impl {
    HMODULE d3d11Module = nullptr;
    PFN_D3D11_CREATE_DEVICE pfnCreateDevice = nullptr;

    ComPtr<ID3D11Device> d3d11Device;
    ComPtr<ID3D11DeviceContext> d3d11Context;
    ComPtr<ID3D11Texture2D> d3d11SharedTex;
    ComPtr<ID3D12Resource> d3d12SharedTex;
    HANDLE shareHandle = nullptr;

    ComPtr<ID3D12RootSignature> blitRootSig;
    ComPtr<ID3D12PipelineState> blitPso;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    void* cachedSrcResource = nullptr;

    HANDLE namesMutex = nullptr;
    HANDLE namesMap = nullptr;
    HANDLE activeNameMap = nullptr;
    HANDLE senderInfoMap = nullptr;
    void* senderInfoView = nullptr;
    HANDLE accessMutex = nullptr;

    std::wstring adapterDescW;
    std::string senderName = "BooSahSah_Clean";
    std::string statusReason = "Disabled";
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t publishedFrames = 0;
    bool active = false;

    bool ensureD3D12BlitPipeline(ID3D12Device* dev) {
        if (blitRootSig && blitPso && srvHeap && rtvHeap) {
            return true;
        }
        if (!dev) return false;

        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER rootParam{};
        rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParam.DescriptorTable.NumDescriptorRanges = 1;
        rootParam.DescriptorTable.pDescriptorRanges = &srvRange;
        rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC linClamp{};
        linClamp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        linClamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linClamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linClamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linClamp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        linClamp.MaxLOD = D3D12_FLOAT32_MAX;
        linClamp.ShaderRegister = 0;
        linClamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 1;
        rsDesc.pParameters = &rootParam;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &linClamp;

        ComPtr<ID3DBlob> sigBlob, errBlob;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob)) || !sigBlob ||
            FAILED(dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&blitRootSig)))) {
            statusReason = "Failed to create Spout2 clean-blit D3D12 root signature.";
            return false;
        }

        std::vector<uint8_t> vsBytes, psBytes;
        loadBinaryShaderFile(resolveResourcePath("assets/shaders/m0_test_vs.cso"), vsBytes);
        if (!loadBinaryShaderFile(resolveResourcePath("assets/shaders/m8_spout_blit.cso"), psBytes) || psBytes.empty()) {
            statusReason = "Missing compiled Spout2 clean-blit shader (m8_spout_blit.cso).";
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = blitRootSig.Get();
        if (!vsBytes.empty()) {
            pso.VS = {vsBytes.data(), vsBytes.size()};
        } else {
            pso.VS = {kFallbackVSBytecode, sizeof(kFallbackVSBytecode)};
        }
        pso.PS = {psBytes.data(), psBytes.size()};
        pso.BlendState.AlphaToCoverageEnable = FALSE;
        pso.BlendState.IndependentBlendEnable = FALSE;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX;
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        pso.SampleDesc.Count = 1;

        if (FAILED(dev->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&blitPso)))) {
            statusReason = "Failed to create Spout2 clean-blit D3D12 PSO.";
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
        srvHeapDesc.NumDescriptors = 1;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)))) {
            statusReason = "Failed to create Spout2 SRV descriptor heap.";
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.NumDescriptors = 1;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(dev->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)))) {
            statusReason = "Failed to create Spout2 RTV descriptor heap.";
            return false;
        }

        return true;
    }

    bool writeSenderInfoSharedMemory() noexcept {
        if (!senderInfoMap) {
            senderInfoMap = CreateFileMappingA(
                INVALID_HANDLE_VALUE,
                nullptr,
                PAGE_READWRITE,
                0,
                static_cast<DWORD>(sizeof(SpoutSharedTextureInfo)),
                senderName.c_str());
            if (!senderInfoMap) {
                statusReason = "CreateFileMappingA failed for Spout2 sender info.";
                return false;
            }
        }
        if (!senderInfoView) {
            senderInfoView = MapViewOfFile(senderInfoMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SpoutSharedTextureInfo));
            if (!senderInfoView) {
                statusReason = "MapViewOfFile failed for Spout2 sender info.";
                return false;
            }
        }

        SpoutSharedTextureInfo info{};
        info.shareHandle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(shareHandle));
        info.width = width;
        info.height = height;
        info.format = kDxgiFormatB8G8R8A8Unorm;
        info.usage = 0u;
        info.partnerId = 0u;
        if (!adapterDescW.empty()) {
            const size_t maxWChars = std::min<size_t>(adapterDescW.size(), 127u);
            std::memcpy(info.description, adapterDescW.data(), maxWChars * sizeof(wchar_t));
        }

        std::memcpy(senderInfoView, &info, sizeof(info));
        return true;
    }

    bool registerSenderNameInGlobalList() noexcept {
        if (!namesMutex) {
            namesMutex = CreateMutexA(nullptr, FALSE, "SpoutSenderNames_mutex");
        }
        if (!accessMutex) {
            const std::string accName = senderName + "_SpoutAccessMutex";
            accessMutex = CreateMutexA(nullptr, FALSE, accName.c_str());
        }

        const bool locked = namesMutex && (WaitForSingleObject(namesMutex, 50) == WAIT_OBJECT_0 ||
                                           WaitForSingleObject(namesMutex, 0) == WAIT_ABANDONED);

        if (!namesMap) {
            namesMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, "SpoutSenderNames");
            if (!namesMap) {
                const DWORD mapBytes = static_cast<DWORD>(kSpoutDefaultMaxSenders * kSpoutNameSlotBytes);
                namesMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, mapBytes, "SpoutSenderNames");
                if (namesMap && GetLastError() != ERROR_ALREADY_EXISTS) {
                    if (void* zeroPtr = MapViewOfFile(namesMap, FILE_MAP_ALL_ACCESS, 0, 0, mapBytes)) {
                        std::memset(zeroPtr, 0, mapBytes);
                        UnmapViewOfFile(zeroPtr);
                    }
                }
            }
        }

        bool registered = false;
        if (namesMap) {
            void* view = MapViewOfFile(namesMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
            if (view) {
                MEMORY_BASIC_INFORMATION mbi{};
                const size_t totalBytes = (VirtualQuery(view, &mbi, sizeof(mbi)) != 0)
                                              ? mbi.RegionSize
                                              : (10 * kSpoutNameSlotBytes);
                const size_t slotCount = totalBytes / kSpoutNameSlotBytes;
                auto* base = static_cast<char*>(view);
                ptrdiff_t emptyIdx = -1;
                for (size_t i = 0; i < slotCount; ++i) {
                    char* slot = base + i * kSpoutNameSlotBytes;
                    if (slot[0] == '\0') {
                        emptyIdx = static_cast<ptrdiff_t>(i);
                        break;
                    }
                    if (std::strncmp(slot, senderName.c_str(), kSpoutNameSlotBytes - 1) == 0) {
                        registered = true;
                        break;
                    }
                }
                if (!registered && emptyIdx >= 0) {
                    char* dst = base + static_cast<size_t>(emptyIdx) * kSpoutNameSlotBytes;
                    std::memset(dst, 0, kSpoutNameSlotBytes);
                    std::memcpy(dst, senderName.data(), std::min<size_t>(senderName.size(), kSpoutNameSlotBytes - 1));
                    registered = true;
                }
                UnmapViewOfFile(view);
            }
        }

        if (!activeNameMap) {
            activeNameMap = CreateFileMappingA(
                INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(kSpoutNameSlotBytes), "ActiveSenderName");
        }
        if (activeNameMap) {
            if (void* actView = MapViewOfFile(activeNameMap, FILE_MAP_ALL_ACCESS, 0, 0, kSpoutNameSlotBytes)) {
                std::memset(actView, 0, kSpoutNameSlotBytes);
                std::memcpy(actView, senderName.data(), std::min<size_t>(senderName.size(), kSpoutNameSlotBytes - 1));
                UnmapViewOfFile(actView);
            }
        }

        if (locked && namesMutex) {
            ReleaseMutex(namesMutex);
        }
        return registered;
    }

    void unregisterSenderFromGlobalList() noexcept {
        const bool locked = namesMutex && (WaitForSingleObject(namesMutex, 50) == WAIT_OBJECT_0);
        if (namesMap) {
            if (void* view = MapViewOfFile(namesMap, FILE_MAP_ALL_ACCESS, 0, 0, 0)) {
                MEMORY_BASIC_INFORMATION mbi{};
                const size_t totalBytes = (VirtualQuery(view, &mbi, sizeof(mbi)) != 0)
                                              ? mbi.RegionSize
                                              : (10 * kSpoutNameSlotBytes);
                const size_t slotCount = totalBytes / kSpoutNameSlotBytes;
                auto* base = static_cast<char*>(view);
                std::vector<std::string> remaining;
                for (size_t i = 0; i < slotCount; ++i) {
                    const char* slot = base + i * kSpoutNameSlotBytes;
                    if (slot[0] == '\0') break;
                    if (std::strncmp(slot, senderName.c_str(), kSpoutNameSlotBytes - 1) != 0) {
                        remaining.emplace_back(slot);
                    }
                }
                std::memset(base, 0, slotCount * kSpoutNameSlotBytes);
                for (size_t i = 0; i < remaining.size() && i < slotCount; ++i) {
                    char* dst = base + i * kSpoutNameSlotBytes;
                    std::memcpy(dst, remaining[i].data(), std::min<size_t>(remaining[i].size(), kSpoutNameSlotBytes - 1));
                }
                UnmapViewOfFile(view);
            }
        }
        if (activeNameMap) {
            if (void* actView = MapViewOfFile(activeNameMap, FILE_MAP_ALL_ACCESS, 0, 0, kSpoutNameSlotBytes)) {
                auto* actStr = static_cast<char*>(actView);
                if (std::strncmp(actStr, senderName.c_str(), kSpoutNameSlotBytes - 1) == 0) {
                    std::memset(actStr, 0, kSpoutNameSlotBytes);
                }
                UnmapViewOfFile(actView);
            }
        }
        if (locked && namesMutex) {
            ReleaseMutex(namesMutex);
        }
    }

    bool allocateSharedTexture(ID3D12Device* d3d12Dev, uint32_t newW, uint32_t newH) {
        if (!d3d12Dev || !d3d11Device || newW == 0 || newH == 0) {
            return false;
        }

        d3d12SharedTex.Reset();
        d3d11SharedTex.Reset();
        shareHandle = nullptr;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = newW;
        desc.Height = newH;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        if (FAILED(d3d11Device->CreateTexture2D(&desc, nullptr, &d3d11SharedTex)) || !d3d11SharedTex) {
            statusReason = "D3D11 CreateTexture2D(D3D11_RESOURCE_MISC_SHARED) failed.";
            return false;
        }

        ComPtr<IDXGIResource> dxgiRes;
        if (FAILED(d3d11SharedTex.As(&dxgiRes)) || !dxgiRes || FAILED(dxgiRes->GetSharedHandle(&shareHandle)) || !shareHandle) {
            statusReason = "IDXGIResource::GetSharedHandle failed for Spout2 texture.";
            d3d11SharedTex.Reset();
            return false;
        }

        if (FAILED(d3d12Dev->OpenSharedHandle(shareHandle, IID_PPV_ARGS(&d3d12SharedTex))) || !d3d12SharedTex) {
            statusReason = "ID3D12Device::OpenSharedHandle failed for Spout2 texture.";
            d3d11SharedTex.Reset();
            shareHandle = nullptr;
            return false;
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        d3d12Dev->CreateRenderTargetView(d3d12SharedTex.Get(), &rtvDesc, rtvHeap->GetCPUDescriptorHandleForHeapStart());

        width = newW;
        height = newH;
        if (!writeSenderInfoSharedMemory() || !registerSenderNameInGlobalList()) {
            return false;
        }
        return true;
    }
};

SpoutSender::SpoutSender() : impl_(std::make_unique<Impl>()) {}

SpoutSender::~SpoutSender() {
    shutdown();
}

bool SpoutSender::initialize(
    D3D12Context& d3d,
    std::string_view senderName,
    uint32_t initialWidth,
    uint32_t initialHeight,
    std::string* outReason) {
    shutdown();
    impl_ = std::make_unique<Impl>();
    impl_->senderName = sanitizeSenderName(senderName);

    if (d3d.isWarp()) {
        impl_->statusReason = "Spout2 DXGI cross-API shared texture skipped on WARP software adapter.";
        if (outReason) *outReason = impl_->statusReason;
        return false;
    }

    auto* d3d12Dev = static_cast<ID3D12Device*>(d3d.deviceRaw());
    if (!d3d12Dev) {
        impl_->statusReason = "D3D12 device is null.";
        if (outReason) *outReason = impl_->statusReason;
        return false;
    }

    impl_->d3d11Module = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!impl_->d3d11Module) {
        impl_->statusReason = "Inbox d3d11.dll unavailable for Spout2 interop.";
        if (outReason) *outReason = impl_->statusReason;
        return false;
    }
    impl_->pfnCreateDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
        GetProcAddress(impl_->d3d11Module, "D3D11CreateDevice"));
    if (!impl_->pfnCreateDevice) {
        impl_->statusReason = "D3D11CreateDevice entry point missing.";
        if (outReason) *outReason = impl_->statusReason;
        shutdown();
        return false;
    }

    ComPtr<IDXGIFactory4> factory4;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))) || !factory4) {
        impl_->statusReason = "CreateDXGIFactory1 failed for Spout2 adapter LUID lookup.";
        if (outReason) *outReason = impl_->statusReason;
        shutdown();
        return false;
    }

    LUID luid{};
    luid.LowPart = d3d.adapterLuidLow();
    luid.HighPart = d3d.adapterLuidHigh();
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) || !adapter) {
        impl_->statusReason = "EnumAdapterByLuid failed for Spout2 D3D11 interop.";
        if (outReason) *outReason = impl_->statusReason;
        shutdown();
        return false;
    }

    DXGI_ADAPTER_DESC1 adDesc{};
    if (SUCCEEDED(adapter->GetDesc1(&adDesc))) {
        impl_->adapterDescW = adDesc.Description;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL outFl = D3D_FEATURE_LEVEL_11_0;
    const HRESULT hr11 = impl_->pfnCreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,
        2,
        D3D11_SDK_VERSION,
        &impl_->d3d11Device,
        &outFl,
        &impl_->d3d11Context);
    if (FAILED(hr11) || !impl_->d3d11Device) {
        impl_->statusReason = "D3D11CreateDevice failed on target DXGI adapter.";
        if (outReason) *outReason = impl_->statusReason;
        shutdown();
        return false;
    }

    if (!impl_->ensureD3D12BlitPipeline(d3d12Dev) ||
        !impl_->allocateSharedTexture(d3d12Dev, std::max(16u, initialWidth), std::max(16u, initialHeight))) {
        if (outReason) *outReason = impl_->statusReason;
        const std::string keepReason = impl_->statusReason;
        shutdown();
        impl_->statusReason = keepReason;
        return false;
    }

    impl_->active = true;
    impl_->statusReason = "Active Spout2 clean-feed sender '" + impl_->senderName + "' (" +
                          std::to_string(impl_->width) + "x" + std::to_string(impl_->height) + " BGRA8)";
    if (outReason) *outReason = impl_->statusReason;
    return true;
}

bool SpoutSender::recordCleanPublish(
    D3D12Context& d3d,
    void* cmdListRaw,
    void* canvasCleanResourceRaw,
    uint32_t canvasWidth,
    uint32_t canvasHeight) noexcept {
    if (!impl_ || !impl_->active || !cmdListRaw || !canvasCleanResourceRaw || canvasWidth == 0 || canvasHeight == 0) {
        return false;
    }

    auto* dev = static_cast<ID3D12Device*>(d3d.deviceRaw());
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(cmdListRaw);
    auto* srcRes = static_cast<ID3D12Resource*>(canvasCleanResourceRaw);
    if (!dev || !cmd || !srcRes) {
        return false;
    }

    if (canvasWidth != impl_->width || canvasHeight != impl_->height) {
        d3d.waitForGpu();
        if (!impl_->allocateSharedTexture(dev, canvasWidth, canvasHeight)) {
            BOO_LOG_WARN("Spout", "Disabling Spout2 sender after resize failure: " + impl_->statusReason);
            impl_->active = false;
            return false;
        }
        impl_->cachedSrcResource = nullptr;
    }

    if (impl_->cachedSrcResource != canvasCleanResourceRaw) {
        const D3D12_RESOURCE_DESC srcDesc = srcRes->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = srcDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(srcRes, &srvDesc, impl_->srvHeap->GetCPUDescriptorHandleForHeapStart());
        impl_->cachedSrcResource = canvasCleanResourceRaw;
    }

    D3D12_RESOURCE_BARRIER toRtv{};
    toRtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRtv.Transition.pResource = impl_->d3d12SharedTex.Get();
    toRtv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRtv.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    toRtv.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd->ResourceBarrier(1, &toRtv);

    ID3D12DescriptorHeap* heaps[] = {impl_->srvHeap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = impl_->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(impl_->width), static_cast<float>(impl_->height), 0.0f, 1.0f};
    const D3D12_RECT scis{0, 0, static_cast<LONG>(impl_->width), static_cast<LONG>(impl_->height)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &scis);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd->SetGraphicsRootSignature(impl_->blitRootSig.Get());
    cmd->SetPipelineState(impl_->blitPso.Get());
    cmd->SetGraphicsRootDescriptorTable(0, impl_->srvHeap->GetGPUDescriptorHandleForHeapStart());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER toCommon{};
    toCommon.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCommon.Transition.pResource = impl_->d3d12SharedTex.Get();
    toCommon.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCommon.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toCommon.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cmd->ResourceBarrier(1, &toCommon);

    ++impl_->publishedFrames;
    return true;
}

void SpoutSender::shutdown() noexcept {
    if (!impl_) return;
    if (impl_->namesMap || impl_->activeNameMap) {
        impl_->unregisterSenderFromGlobalList();
    }
    if (impl_->senderInfoView) {
        UnmapViewOfFile(impl_->senderInfoView);
        impl_->senderInfoView = nullptr;
    }
    if (impl_->senderInfoMap) {
        CloseHandle(impl_->senderInfoMap);
        impl_->senderInfoMap = nullptr;
    }
    if (impl_->activeNameMap) {
        CloseHandle(impl_->activeNameMap);
        impl_->activeNameMap = nullptr;
    }
    if (impl_->namesMap) {
        CloseHandle(impl_->namesMap);
        impl_->namesMap = nullptr;
    }
    if (impl_->accessMutex) {
        CloseHandle(impl_->accessMutex);
        impl_->accessMutex = nullptr;
    }
    if (impl_->namesMutex) {
        CloseHandle(impl_->namesMutex);
        impl_->namesMutex = nullptr;
    }

    impl_->rtvHeap.Reset();
    impl_->srvHeap.Reset();
    impl_->blitPso.Reset();
    impl_->blitRootSig.Reset();
    impl_->d3d12SharedTex.Reset();
    impl_->d3d11SharedTex.Reset();
    impl_->d3d11Context.Reset();
    impl_->d3d11Device.Reset();
    impl_->shareHandle = nullptr;
    impl_->cachedSrcResource = nullptr;

    if (impl_->d3d11Module) {
        FreeLibrary(impl_->d3d11Module);
        impl_->d3d11Module = nullptr;
    }
    impl_->pfnCreateDevice = nullptr;
    impl_->active = false;
}

bool SpoutSender::isActive() const noexcept {
    return impl_ && impl_->active;
}

const std::string& SpoutSender::senderName() const noexcept {
    static const std::string kEmpty;
    return impl_ ? impl_->senderName : kEmpty;
}

uint32_t SpoutSender::width() const noexcept {
    return impl_ ? impl_->width : 0u;
}

uint32_t SpoutSender::height() const noexcept {
    return impl_ ? impl_->height : 0u;
}

uint32_t SpoutSender::sharedHandleValue() const noexcept {
    return (impl_ && impl_->shareHandle) ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(impl_->shareHandle)) : 0u;
}

uint64_t SpoutSender::publishedFrameCount() const noexcept {
    return impl_ ? impl_->publishedFrames : 0ULL;
}

const std::string& SpoutSender::statusReason() const noexcept {
    static const std::string kEmpty;
    return impl_ ? impl_->statusReason : kEmpty;
}

} // namespace boo
