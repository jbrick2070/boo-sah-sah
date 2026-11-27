#include "boo/dlss_bridge.hpp"
#include "boo/startup_progress.hpp"
#include "boo/pipeline_compiler.hpp"
#include "boo/render.hpp"
#include "boo/trilogy_renderer.hpp"
#include "boo/logging.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <shlobj.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef BOO_HAS_DLSS
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>
#endif
#ifdef BOO_HAS_DLSS_RR
#include <nvsdk_ngx_helpers_dlssd_d3d.h>
#endif

namespace boo {
using Microsoft::WRL::ComPtr;

struct DlssBridge::Impl {
    RendererFeatureState status = RendererFeatureState::Unavailable;
    RendererFeatureState rrStatus = RendererFeatureState::Unavailable;
    bool configuredRR = false;
    ComPtr<ID3D12Resource> inputColor, inputDistance;
    uint32_t inputW = 0, inputH = 0, sample = 0;
    float pixelJitter[2]{};
    bool historyReset = true;
    bool primePresentation = false;
#ifdef BOO_HAS_DLSS
    D3D12Context* context = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    bool supported = false;
    bool supportsRR = false;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> resolvePso;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12Resource> exposure;
    ComPtr<ID3D12DescriptorHeap> exposureRtv;
    ComPtr<ID3D12Resource> fullColor, fullDistance;
    uint32_t requestedWidth = 0, requestedHeight = 0;
    bool noUpscale = false;
    DlssMode configured = DlssMode::Off;
    DlssMode failed = DlssMode::Off;
    DlssMode failedRR = DlssMode::Off;
    ~Impl() {
        if (feature) NVSDK_NGX_D3D12_ReleaseFeature(feature);
        if (params) NVSDK_NGX_D3D12_DestroyParameters(params);
    }
    bool probe();
    bool makeResolvePipeline();
    bool configure(ID3D12GraphicsCommandList* cmd, DlssMode mode, bool useRR, ID3D12Resource* color, ID3D12Resource* distance);
#endif
};

DlssBridge::DlssBridge() : impl_(std::make_unique<Impl>()) {}
DlssBridge::~DlssBridge() = default;
RendererFeatureState DlssBridge::state() const noexcept { return impl_->status; }
RendererFeatureState DlssBridge::rayReconstructionState() const noexcept { return impl_->rrStatus; }
bool DlssBridge::renderNoisyRays() const noexcept { return impl_->configuredRR; }
uint32_t DlssBridge::width() const noexcept { return impl_->inputW; }
uint32_t DlssBridge::height() const noexcept { return impl_->inputH; }
ID3D12Resource* DlssBridge::color() const noexcept { return impl_->inputColor.Get(); }
ID3D12Resource* DlssBridge::distance() const noexcept { return impl_->inputDistance.Get(); }
const float* DlssBridge::jitter() const noexcept { return impl_->pixelJitter; }
void DlssBridge::reset() noexcept {
    impl_->historyReset = true;
    impl_->primePresentation = true;
    impl_->sample = 0;
}
void DlssBridge::rejectInputSize() noexcept {
#ifdef BOO_HAS_DLSS
    impl_->failed = impl_->configured;
    impl_->status = RendererFeatureState::Unavailable;
    if (impl_->configuredRR) {
        impl_->failedRR = impl_->configured;
        impl_->rrStatus = RendererFeatureState::Unavailable;
    }
#endif
    reset();
}

#ifdef BOO_HAS_DLSS
namespace {
void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    cmd->ResourceBarrier(1, &barrier);
}
float halton(uint32_t index, uint32_t base) {
    float value = 0.f, weight = 1.f;
    while (index) {
        weight /= float(base);
        value += float(index % base) * weight;
        index /= base;
    }
    return value;
}
}

#endif
bool initializeNgxDevice(ID3D12Device* device, const std::filesystem::path& folder) {
#ifdef BOO_HAS_DLSS
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return false;
    const auto cache = std::filesystem::path(local) / L"BooSahSah" / L"NGX";
    CoTaskMemFree(local);
    std::error_code error;
    std::filesystem::create_directories(cache, error);
    if (error) return false;
    const wchar_t* search = folder.c_str();
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo = {&search, 1};
    // Stable UUIDv5 of this project's canonical repository URL, for NVIDIA's custom engine API.
    const auto result = NVSDK_NGX_D3D12_Init_with_ProjectID("5b6c8338-1aa7-5abb-8586-78d064a60981",
        NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", cache.c_str(), device, &info);
    if (NVSDK_NGX_FAILED(result)) {
        BOO_LOG_INFO("DLSS", "NGX unavailable, result " + std::to_string(static_cast<uint32_t>(result)));
        return false;
    }
    return true;
#else
    (void)device; (void)folder;
    return false;
#endif
}
void shutdownNgxDevice(ID3D12Device* device) noexcept {
#ifdef BOO_HAS_DLSS
    // Streamline's frame-generation features on this device have already retired.
    NVSDK_NGX_D3D12_Shutdown1(device);
#else
    (void)device;
#endif
}
#ifdef BOO_HAS_DLSS
bool DlssBridge::Impl::probe() {
    // NVIDIA's runtime serves NVIDIA graphics only: elsewhere it is not started and creates no cache folder.
    if (context->adapterVendorId() != 0x10DE) return false;
    std::array<wchar_t, 32768> exe{};
    if (!GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()))) return false;
    const auto folder = std::filesystem::path(exe.data()).parent_path() / L"dlss";
    std::error_code error;
    if (!std::filesystem::is_regular_file(folder / L"nvngx_dlss.dll", error) || !context->ensureNgxRuntime(folder)) return false;
    int available = 0;
    const auto query = NVSDK_NGX_D3D12_GetCapabilityParameters(&params);
    if (NVSDK_NGX_FAILED(query) || !params ||
        NVSDK_NGX_FAILED(params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available)) || !available) {
        BOO_LOG_INFO("DLSS","Driver reported no Super Resolution capability; NGX parameter result " +
            std::to_string(static_cast<uint32_t>(query)));
        return false;
    }
#ifdef BOO_HAS_DLSS_RR
    int rrAvailable = 0;
    supportsRR = std::filesystem::is_regular_file(folder / L"nvngx_dlssd.dll", error) &&
        NVSDK_NGX_SUCCEED(params->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rrAvailable)) && rrAvailable != 0;
#endif
    if (makeResolvePipeline()) return true;
    BOO_LOG_WARN("DLSS","Failed to create the output reconstruction pipeline");
    return false;
}

bool DlssBridge::Impl::makeResolvePipeline() {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,2}};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {2, ranges};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0,0,1};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_ROOT_SIGNATURE_DESC desc{2,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) ||
        FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)))) return false;
    std::ifstream shader(resolveResourcePath("assets/shaders/dlss_resolve.cso"), std::ios::binary);
    const std::vector<char> code((std::istreambuf_iterator<char>(shader)), {});
    startupAssetLoaded(resolveResourcePath("assets/shaders/dlss_resolve.cso"));
    if (code.empty()) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root.Get();
    pso.CS = {code.data(),code.size()};
    if (FAILED(compileWhilePainting("dlss_resolve.cso", code.size(), [&] {
            return device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&resolvePso));
        }))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 4;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    return SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors)));
}

bool DlssBridge::Impl::configure(ID3D12GraphicsCommandList* cmd, DlssMode mode, bool useRR, ID3D12Resource* color, ID3D12Resource* distance) {
    noUpscale = false;
    if (feature && configured == mode && configuredRR == useRR && fullColor.Get() == color && fullDistance.Get() == distance) return true;
    const auto output = color->GetDesc();
    if (output.Width < 32 || output.Height < 32) { noUpscale = true; return false; }
    const auto quality = mode == DlssMode::Performance ? NVSDK_NGX_PerfQuality_Value_MaxPerf :
                         mode == DlssMode::Balanced ? NVSDK_NGX_PerfQuality_Value_Balanced : NVSDK_NGX_PerfQuality_Value_MaxQuality;
    unsigned w = 0, h = 0, maxW, maxH, minW, minH;
    float sharpness;
    auto querySettings = NGX_DLSS_GET_OPTIMAL_SETTINGS;
#ifdef BOO_HAS_DLSS_RR
    if (useRR) querySettings = NGX_DLSSD_GET_OPTIMAL_SETTINGS;
#endif
    const auto optimal = querySettings(params, static_cast<unsigned>(output.Width), output.Height,
        quality, &w, &h, &maxW, &maxH, &minW, &minH, &sharpness);
    if (NVSDK_NGX_FAILED(optimal)) return false;
    if (!w || !h || w >= output.Width || h >= output.Height) { noUpscale = true; return false; }
    if (!context->waitForGpu()) return false;
    if (feature) { NVSDK_NGX_D3D12_ReleaseFeature(feature); feature = nullptr; }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = hp.VisibleNodeMask = 1;
    if (!exposure) {
        // M2 outputs linear scene radiance; the exposure and tone map are applied after NGX.
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(exposureRtv.ReleaseAndGetAddressOf())))) return false;
        D3D12_RESOURCE_DESC tex{};
        tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex.Width = tex.Height = tex.DepthOrArraySize = tex.MipLevels = 1;
        tex.Format = DXGI_FORMAT_R32_FLOAT;
        tex.SampleDesc.Count = 1;
        tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&tex,D3D12_RESOURCE_STATE_RENDER_TARGET,
                nullptr,IID_PPV_ARGS(&exposure)))) return false;
        const auto rtv = exposureRtv->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(exposure.Get(),nullptr,rtv);
        const float unitExposure[4] = {1.f,0.f,0.f,0.f};
        cmd->ClearRenderTargetView(rtv,unitExposure,0,nullptr);
        transition(cmd,exposure.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    auto makeInput = [&](ID3D12Resource* full, ComPtr<ID3D12Resource>& input) {
        auto desc = full->GetDesc();
        desc.Width = w; desc.Height = h;
        return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(input.ReleaseAndGetAddressOf())));
    };
    if (!makeInput(color, inputColor) || !makeInput(distance, inputDistance)) return false;
    constexpr int flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    NVSDK_NGX_Result result;
#ifdef BOO_HAS_DLSS_RR
    if (useRR) {
        NVSDK_NGX_DLSSD_Create_Params create{};
        create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
        create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
        create.InWidth = w; create.InHeight = h;
        create.InTargetWidth = static_cast<unsigned>(output.Width); create.InTargetHeight = output.Height;
        create.InPerfQualityValue = quality; create.InFeatureCreateFlags = flags;
        result = NGX_D3D12_CREATE_DLSSD_EXT(cmd,1,1,&feature,params,&create);
    } else
#endif
    {
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature = {w,h,static_cast<unsigned>(output.Width),output.Height,quality};
        create.InFeatureCreateFlags = flags;
        result = NGX_D3D12_CREATE_DLSS_EXT(cmd,1,1,&feature,params,&create);
    }
    if (NVSDK_NGX_FAILED(result) || !feature) {
        BOO_LOG_WARN("DLSS","Feature creation failed: " + std::to_string(static_cast<uint32_t>(result)));
        return false;
    }
    auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
    const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (ID3D12Resource* input : {inputColor.Get(),inputDistance.Get()}) {
        device->CreateShaderResourceView(input,nullptr,handle);
        handle.ptr += stride;
    }
    for (ID3D12Resource* out : {color,distance}) {
        device->CreateUnorderedAccessView(out,nullptr,nullptr,handle);
        handle.ptr += stride;
    }
    fullColor = color;
    fullDistance = distance;
    configured = mode;
    configuredRR = useRR;
    inputW = w; inputH = h;
    historyReset = true; sample = 0;
    BOO_LOG_INFO("DLSS",std::string(useRR ? "NGX Ray Reconstruction" : "NGX Super Resolution") + " initialized, " + std::to_string(w) + "x" + std::to_string(h) +
        " -> " + std::to_string(output.Width) + "x" + std::to_string(output.Height));
    return true;
}
#endif

void DlssBridge::warmUp(D3D12Context& context) {
#ifdef BOO_HAS_DLSS
    auto& s = *impl_;
    if (s.context) return;
    s.context = &context;
    s.device = static_cast<ID3D12Device*>(context.deviceRaw());
    s.supported = !context.isWarp() && s.device && s.probe();
#else
    (void)context;
#endif
}

bool DlssBridge::prepare(D3D12Context& context, ID3D12GraphicsCommandList* cmd, DlssMode mode, bool eligible,
                         bool requestRR, bool raytracing, ID3D12Resource* fullColor, ID3D12Resource* fullDistance) {
#ifdef BOO_HAS_DLSS
    auto& s = *impl_;
    warmUp(context);
    if (!s.supported) return false;
    const auto output = fullColor->GetDesc();
    if (output.Width != s.requestedWidth || output.Height != s.requestedHeight) {
        s.failed = s.failedRR = DlssMode::Off;
        s.requestedWidth = static_cast<uint32_t>(output.Width);
        s.requestedHeight = output.Height;
    }
    if (s.failed != mode) s.failed = DlssMode::Off;
    if (s.failedRR != mode || !requestRR) s.failedRR = DlssMode::Off;
    s.rrStatus = !s.supportsRR ? RendererFeatureState::Unavailable :
                 requestRR ? RendererFeatureState::Idle : RendererFeatureState::Off;
    s.status = mode == DlssMode::Off ? RendererFeatureState::Off : RendererFeatureState::Idle;
    if (!eligible || mode == DlssMode::Off) { reset(); return false; }
    bool useRR = requestRR && raytracing && s.supportsRR;
    if (useRR) {
        if (s.failedRR == mode) {
            s.rrStatus = RendererFeatureState::Unavailable;
            useRR = false;
        } else if (!s.configure(cmd, mode, true, fullColor, fullDistance)) {
            if (!s.noUpscale) {
                s.rrStatus = RendererFeatureState::Unavailable;
                s.failedRR = mode;
            }
            useRR = false;
        }
    }
    if (!useRR && (s.failed == mode || !s.configure(cmd, mode, false, fullColor, fullDistance))) {
        if (s.failed == mode || !s.noUpscale) {
            s.status = RendererFeatureState::Unavailable;
            s.failed = mode;
        }
        return false;
    }
    const auto phase = ++s.sample;
    s.pixelJitter[0] = halton(phase,2) - .5f;
    s.pixelJitter[1] = halton(phase,3) - .5f;
    return true;
#else
    (void)context; (void)cmd; (void)mode; (void)eligible; (void)requestRR; (void)raytracing; (void)fullColor; (void)fullDistance;
    return false;
#endif
}

void DlssBridge::resolve(ID3D12GraphicsCommandList* cmd, const TrilogyMeshRenderer& scene, float frameSeconds) {
#ifdef BOO_HAS_DLSS
    auto& s = *impl_;
    constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr auto pixel = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition(cmd,s.inputColor.Get(),pixel,read);
    transition(cmd,s.inputDistance.Get(),pixel,read);
    transition(cmd,s.fullColor.Get(),pixel,write);
    transition(cmd,s.fullDistance.Get(),pixel,write);
    auto* depth = static_cast<ID3D12Resource*>(scene.dlssDepthResource());
    auto* motion = static_cast<ID3D12Resource*>(scene.dlssMotionResource());
    const bool resetHistory = s.historyReset || scene.dlssHistoryReset();
    const float dtMs = frameSeconds > 0.f ? frameSeconds * 1000.f : (1000.f / 60.f);
    NVSDK_NGX_Result result = NVSDK_NGX_Result_FAIL_InvalidParameter;
#ifdef BOO_HAS_DLSS_RR
    if (s.configuredRR) {
        if (depth && motion && scene.rrDiffuseAlbedo() && scene.rrSpecularAlbedo() &&
            scene.rrNormalRoughness() && scene.rrSpecularHitDistance()) {
            NVSDK_NGX_D3D12_DLSSD_Eval_Params args{};
            args.pInColor = s.inputColor.Get(); args.pInOutput = s.fullColor.Get();
            args.pInDepth = depth; args.pInMotionVectors = motion;
            args.pInExposureTexture = s.exposure.Get();
            args.InFrameTimeDeltaInMsec = dtMs;
            args.pInDiffuseAlbedo = static_cast<ID3D12Resource*>(scene.rrDiffuseAlbedo());
            args.pInSpecularAlbedo = static_cast<ID3D12Resource*>(scene.rrSpecularAlbedo());
            args.pInNormals = static_cast<ID3D12Resource*>(scene.rrNormalRoughness());
            args.pInSpecularHitDistance = static_cast<ID3D12Resource*>(scene.rrSpecularHitDistance());
            float view[16], projection[16];
            scene.rrViewMatrices(view,projection);
            args.pInWorldToViewMatrix = view; args.pInViewToClipMatrix = projection;
            args.InJitterOffsetX = s.pixelJitter[0]; args.InJitterOffsetY = s.pixelJitter[1];
            args.InMVScaleX = 1.f; args.InMVScaleY = 1.f;
            args.InRenderSubrectDimensions = {s.inputW,s.inputH};
            args.InReset = resetHistory;
            result = NGX_D3D12_EVALUATE_DLSSD_EXT(cmd,s.feature,s.params,&args);
        }
    } else
#endif
    if (depth && motion) {
        NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
        eval.Feature.pInColor = s.inputColor.Get();
        eval.Feature.pInOutput = s.fullColor.Get();
        eval.pInDepth = depth; eval.pInMotionVectors = motion;
        eval.pInExposureTexture = s.exposure.Get();
        eval.InFrameTimeDeltaInMsec = dtMs;
        eval.InJitterOffsetX = s.pixelJitter[0]; eval.InJitterOffsetY = s.pixelJitter[1];
        eval.InMVScaleX = 1.f; eval.InMVScaleY = 1.f;
        eval.InRenderSubrectDimensions = {s.inputW,s.inputH};
        eval.InReset = resetHistory;
        result = NGX_D3D12_EVALUATE_DLSS_EXT(cmd,s.feature,s.params,&eval);
    }
    const bool success = NVSDK_NGX_SUCCEED(result);
    const bool spatialColor = !success || s.primePresentation;
    s.status = !success ? RendererFeatureState::Unavailable :
        (s.primePresentation ? RendererFeatureState::Idle : RendererFeatureState::Active);
    if (s.configuredRR) s.rrStatus = s.status;
    if (spatialColor) {
        // A direct seek primes NGX privately. A failed evaluation may also have enqueued writes.
        // Serialize either case before the spatial pass overwrites the output.
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = s.fullColor.Get();
        cmd->ResourceBarrier(1,&barrier);
    }
    s.primePresentation = false;
    if (!success) {
        BOO_LOG_WARN("DLSS","Reconstruction failed, result " + std::to_string(static_cast<uint32_t>(result)));
        if (s.configuredRR) s.failedRR = s.configured;
        else s.failed = s.configured;
    }
    s.historyReset = !success;
    ID3D12DescriptorHeap* heaps[] = {s.descriptors.Get()};
    cmd->SetDescriptorHeaps(1,heaps);
    cmd->SetComputeRootSignature(s.root.Get());
    cmd->SetComputeRootDescriptorTable(0,s.descriptors->GetGPUDescriptorHandleForHeapStart());
    cmd->SetComputeRoot32BitConstant(1,spatialColor ? 1u : 0u,0);
    cmd->SetPipelineState(s.resolvePso.Get());
    const auto full = s.fullColor->GetDesc();
    cmd->Dispatch((static_cast<UINT>(full.Width)+7)/8,(full.Height+7)/8,1);
    transition(cmd,s.fullDistance.Get(),write,pixel);
    transition(cmd,s.fullColor.Get(),write,pixel);
    transition(cmd,s.inputDistance.Get(),read,pixel);
    transition(cmd,s.inputColor.Get(),read,pixel);
#else
    (void)cmd; (void)scene; (void)frameSeconds;
#endif
}
}
