#include "boo/render.hpp"
#include "boo/startup_progress.hpp"
#include "boo/dlss_bridge.hpp"
#include "boo/logging.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <vector>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <limits>

namespace boo {

using Microsoft::WRL::ComPtr;

namespace {
constexpr uint32_t kTimestampsPerFrame = 5;
constexpr ULONGLONG kGpuWaitTimeoutMs = 5000;

std::string w2u8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.pop_back();
    return s;
}

std::string hrHex(HRESULT hr) {
    char buf[24]{};
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
    return std::string(buf);
}
} // namespace

struct D3D12Context::Impl {
    FrameGeneration frameGeneration;
    bool directNgxInitialized = false;
    ComPtr<IDXGIFactory2> fgFactory;
    ComPtr<ID3D12Device> fgDevice;
    ComPtr<ID3D12Resource> fgHudless,fgUiAlpha;
    ComPtr<ID3D12DescriptorHeap> fgRtv;
    bool fgTargetsBound=false;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter3> adapter3;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Device5> removalDevice;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<IDXGISwapChain4> swapChain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    std::array<ComPtr<ID3D12Resource>, kFrameCount> renderTargets;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> commandAllocators;
    ComPtr<ID3D12GraphicsCommandList> commandList;

    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    HANDLE waitableHandle = nullptr;
    uint64_t nextFenceValue = 1;
    std::array<uint64_t, kFrameCount> frameFenceValues{0, 0, 0};

    // 5 GPU timestamp queries per buffered frame: [0]=start, [1]=scene, [2]=bloom, [3]=composite, [4]=presentPass
    ComPtr<ID3D12QueryHeap> timestampHeap;
    ComPtr<ID3D12Resource> timestampReadback;
    uint64_t gpuTimestampFreq = 0;
    uint8_t currentRecordedMask = 0;
    std::array<uint8_t, kFrameCount> frameRecordedMask{0, 0, 0};
    GpuPassTimingsMs lastPassTimings{};

    DXGI_FORMAT backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT swapChainFlags = 0;
    uint32_t rtvDescriptorSize = 0;
    uint32_t frameIndex = 0;
    bool allowTearing = false;
    bool headless = false;
    bool frameOpen = false;
    bool occluded = false;
    bool synchronizationFailed = false;
    bool teardownRetired = false;
    bool teardownDrained = false;
    bool injectedDeviceLost = false;
    bool debugLayerEnabled = false;
};

D3D12Context::D3D12Context() : impl_(std::make_unique<Impl>()) {}

D3D12Context::~D3D12Context() {
    shutdown();
}

bool D3D12Context::bypassShaderCaches(std::string* reason) {
    if (!impl_ || !impl_->device) {
        if (reason) *reason = "no device";
        return false;
    }
    ComPtr<ID3D12Device9> device9;
    if (FAILED(impl_->device.As(&device9))) {
        if (reason) *reason = "ID3D12Device9 is not available";
        return false;
    }
    const HRESULT hr = device9->ShaderCacheControl(
        D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED | D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_D3D_CACHE_FOR_DRIVER,
        D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE);
    if (FAILED(hr)) {
        if (reason) *reason = "ShaderCacheControl refused (" + hrHex(hr) + "); it needs Windows Developer Mode";
        return false;
    }
    return true;
}

bool D3D12Context::initialize(
    HWND hwnd,
    uint32_t width,
    uint32_t height,
    bool forceWarp,
    bool enableDebugLayer,
    std::string* outError) {
    shutdown();
    impl_ = std::make_unique<Impl>();
    impl_->headless = (hwnd == nullptr);
    if (!impl_->headless && !forceWarp) impl_->frameGeneration.load();

    if (enableDebugLayer) {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
    }
    UINT dxgiFactoryFlags = 0;
    if (enableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            impl_->debugLayerEnabled = true;
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            BOO_LOG_INFO("D3D12", "D3D12 debug layer enabled.");
        } else {
            BOO_LOG_WARN("D3D12", "D3D12 debug layer requested, but Graphics Tools SDKLayers not installed; continuing without it.");
        }
    }

    HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&impl_->factory));
    if (FAILED(hr)) {
        hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&impl_->factory));
    }
    if (FAILED(hr) || !impl_->factory) {
        if (outError) *outError = "CreateDXGIFactory2 failed (" + hrHex(hr) + ")";
        return false;
    }

    BOOL tearing = FALSE;
    if (SUCCEEDED(impl_->factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
        impl_->allowTearing = (tearing == TRUE);
    }

    ComPtr<IDXGIAdapter1> chosenAdapter;
    if (forceWarp) {
        hr = impl_->factory->EnumWarpAdapter(IID_PPV_ARGS(&chosenAdapter));
        if (SUCCEEDED(hr) && chosenAdapter) {
            hr = D3D12CreateDevice(chosenAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&impl_->device));
            isWarp_ = true;
        }
    } else {
        ComPtr<IDXGIAdapter1> candidate;
        for (UINT idx = 0;
             SUCCEEDED(impl_->factory->EnumAdapterByGpuPreference(idx, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)));
             ++idx) {
            DXGI_ADAPTER_DESC1 desc{};
            candidate->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&impl_->device)))) {
                chosenAdapter = candidate;
                isWarp_ = false;
                break;
            }
        }
        // Graceful fallback to WARP if no hardware D3D12 GPU succeeded
        if (!impl_->device) {
            if (SUCCEEDED(impl_->factory->EnumWarpAdapter(IID_PPV_ARGS(&chosenAdapter))) && chosenAdapter) {
                if (SUCCEEDED(D3D12CreateDevice(chosenAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&impl_->device)))) {
                    isWarp_ = true;
                    BOO_LOG_WARN("D3D12", "No hardware D3D12 adapter succeeded; fell back to WARP software adapter.");
                }
            }
        }
    }

    if (!impl_->device || !chosenAdapter) {
        if (outError) *outError = "No compatible Direct3D 12 device found on this system.";
        return false;
    }
    // Windows 11 supplies Device5. Require its removal path before creating any
    // queue so a failed bounded drain can never leave live work using freed data.
    if (FAILED(impl_->device.As(&impl_->removalDevice))) {
        if (outError) *outError = "Direct3D 12 runtime lacks the safe device-removal interface required by Windows 11.";
        return false;
    }

    chosenAdapter.As(&impl_->adapter3);
    DXGI_ADAPTER_DESC1 adapterDesc{};
    chosenAdapter->GetDesc1(&adapterDesc);
    adapterName_ = w2u8(adapterDesc.Description);
    luidLow_     = static_cast<int32_t>(adapterDesc.AdapterLuid.LowPart);
    luidHigh_    = adapterDesc.AdapterLuid.HighPart;

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (!impl_->headless && !forceWarp && impl_->frameGeneration.attach(impl_->device.Get())) {
        void* factory=impl_->factory.Get();void* device=impl_->device.Get();
        if (impl_->frameGeneration.upgrade(&factory) && factory!=impl_->factory.Get())
            impl_->fgFactory.Attach(static_cast<IDXGIFactory2*>(factory));
        if (impl_->frameGeneration.upgrade(&device) && device!=impl_->device.Get())
            impl_->fgDevice.Attach(static_cast<ID3D12Device*>(device));
        if (!impl_->fgFactory || !impl_->fgDevice) {
            impl_->frameGeneration.disablePresentation();impl_->fgDevice.Reset();impl_->fgFactory.Reset();
        }
    }
    auto* queueDevice=impl_->fgDevice ? impl_->fgDevice.Get() : impl_->device.Get();
    hr = queueDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&impl_->commandQueue));
    if(FAILED(hr) && impl_->fgDevice) {
        impl_->frameGeneration.disablePresentation();impl_->fgFactory.Reset();impl_->fgDevice.Reset();
        hr=impl_->device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&impl_->commandQueue));
    }
    if (FAILED(hr)) {
        if (outError) *outError = "CreateCommandQueue failed (" + hrHex(hr) + ")";
        return false;
    }
    impl_->commandQueue->GetTimestampFrequency(&impl_->gpuTimestampFreq);

    hr = impl_->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl_->fence));
    if (FAILED(hr)) {
        if (outError) *outError = "CreateFence failed (" + hrHex(hr) + ")";
        return false;
    }
    impl_->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->fenceEvent) {
        if (outError) *outError = "CreateEventW for D3D12 fence failed";
        return false;
    }

    for (uint32_t i = 0; i < kFrameCount; ++i) {
        hr = impl_->device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&impl_->commandAllocators[i]));
        if (FAILED(hr)) {
            if (outError) *outError = "CreateCommandAllocator failed (" + hrHex(hr) + ")";
            return false;
        }
    }

    hr = impl_->device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        impl_->commandAllocators[0].Get(),
        nullptr,
        IID_PPV_ARGS(&impl_->commandList));
    if (FAILED(hr)) {
        if (outError) *outError = "CreateCommandList failed (" + hrHex(hr) + ")";
        return false;
    }
    impl_->commandList->Close();

    // Timestamp query heap + readback buffer (5 * kFrameCount uint64_t timestamps)
    D3D12_QUERY_HEAP_DESC qhDesc{};
    qhDesc.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhDesc.Count = kFrameCount * kTimestampsPerFrame;
    if (SUCCEEDED(impl_->device->CreateQueryHeap(&qhDesc, IID_PPV_ARGS(&impl_->timestampHeap)))) {
        D3D12_HEAP_PROPERTIES readbackHeap{};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Width = sizeof(uint64_t) * kFrameCount * kTimestampsPerFrame;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        impl_->device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&impl_->timestampReadback));
    }

    width_  = std::max<uint32_t>(1, width);
    height_ = std::max<uint32_t>(1, height);

    if (impl_->headless) {
        impl_->backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        return true;
    }

    // Section 3.1 + M1 [G1.1]: 10-bit SDR swapchain with checked Rec.709 G22 color space & scoped 8-bit fallback
    const DXGI_FORMAT candidateFormats[2] = {
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM
    };
    const UINT baseTearingFlag = impl_->allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
    const UINT candidateFlags[2] = {
        baseTearingFlag | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT,
        baseTearingFlag
    };

    HRESULT lastSwapHr = E_FAIL;
    for (int attempt=0;attempt<(impl_->fgFactory ? 2 : 1) && !impl_->swapChain;++attempt) {
    auto* factory=attempt==0 && impl_->fgFactory ? impl_->fgFactory.Get() : impl_->factory.Get();
    if (attempt==1) impl_->frameGeneration.disablePresentation();
    for (DXGI_FORMAT fmt : candidateFormats) {
        for (UINT flags : candidateFlags) {
            DXGI_SWAP_CHAIN_DESC1 scDesc{};
            scDesc.BufferCount = kFrameCount;
            scDesc.Width       = width_;
            scDesc.Height      = height_;
            scDesc.Format      = fmt;
            scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            scDesc.SampleDesc.Count = 1;
            scDesc.Flags       = flags;

            // Keep both candidate COM pointers strictly scoped so failed 10-bit checks
            // release all flip-model swapchain references on hwnd before retrying 8-bit!
            ComPtr<IDXGISwapChain1> candSc1;
            lastSwapHr = factory->CreateSwapChainForHwnd(
                impl_->commandQueue.Get(),
                hwnd,
                &scDesc,
                nullptr,
                nullptr,
                &candSc1);
            if (FAILED(lastSwapHr) || !candSc1) {
                continue;
            }

            ComPtr<IDXGISwapChain4> candSc4;
            if (FAILED(candSc1.As(&candSc4)) || !candSc4) {
                continue;
            }

            if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
                UINT csSupport = 0;
                const HRESULT csHr = candSc4->CheckColorSpaceSupport(
                    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                    &csSupport);
                if (FAILED(csHr) || (csSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) {
                    continue;
                }
                if (FAILED(candSc4->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709))) {
                    continue;
                }
            }

            impl_->swapChain = std::move(candSc4);
            impl_->backBufferFormat = fmt;
            impl_->swapChainFlags = flags;
            break;
        }
        if (impl_->swapChain) {
            break;
        }
    }

    }
    if (!impl_->swapChain) {
        if (outError) *outError = "CreateSwapChainForHwnd failed (" + hrHex(lastSwapHr) + ")";
        return false;
    }

    impl_->factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    if ((impl_->swapChainFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
        impl_->swapChain->SetMaximumFrameLatency(2);
        impl_->waitableHandle = impl_->swapChain->GetFrameLatencyWaitableObject();
    }

    BOO_LOG_INFO(
        "D3D12",
        std::string("Swapchain initialized: ") +
            (impl_->backBufferFormat == DXGI_FORMAT_R10G10B10A2_UNORM ? "10-bit R10G10B10A2_UNORM (Rec.709 G2.2 SDR)" : "8-bit R8G8B8A8_UNORM") +
            " | WaitableLatency=" + (impl_->waitableHandle ? "ON(max=2)" : "OFF"));
    startupCheckpoint();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = impl_->device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&impl_->rtvHeap));
    if (FAILED(hr)) {
        if (outError) *outError = "CreateDescriptorHeap(RTV) failed (" + hrHex(hr) + ")";
        return false;
    }
    impl_->rtvDescriptorSize = impl_->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = impl_->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        hr = impl_->swapChain->GetBuffer(i, IID_PPV_ARGS(&impl_->renderTargets[i]));
        if (FAILED(hr)) {
            if (outError) *outError = "SwapChain GetBuffer failed (" + hrHex(hr) + ")";
            return false;
        }
        impl_->device->CreateRenderTargetView(impl_->renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += impl_->rtvDescriptorSize;
    }
    impl_->frameIndex = impl_->swapChain->GetCurrentBackBufferIndex();
    return true;
}

void D3D12Context::shutdown() noexcept {
    if (!impl_) return;
    auto t = std::chrono::steady_clock::now();
    auto lap = [&t](const char* what) {
        const auto now = std::chrono::steady_clock::now();
        char line[160]{};
        std::snprintf(line, sizeof(line), "D3D12 shutdown: %s took %.0f ms", what,
                      std::chrono::duration<double, std::milli>(now - t).count());
        BOO_LOG_DEBUG("Shutdown", line);
        t = now;
    };
    retireGpuForShutdown();
    lap("GPU retire");
    impl_->frameGeneration.shutdown();
    lap("frame generation (Streamline)");
    if (impl_->directNgxInitialized) shutdownNgxDevice(impl_->device.Get());
    lap("NGX");
    if (impl_->waitableHandle) {
        CloseHandle(impl_->waitableHandle);
        impl_->waitableHandle = nullptr;
    }
    if (impl_->fenceEvent) {
        CloseHandle(impl_->fenceEvent);
        impl_->fenceEvent = nullptr;
    }
    // After a start that compiled shaders, the driver finishes its own background shader work
    // when the device is released: measured at 43 s after a cold start, under 0.1 s when warm.
    impl_.reset();
    lap("device and resources released");
    presentCount_ = 0;
    presentStallCount_ = 0;
}

bool D3D12Context::waitForGpu() noexcept {
    if (!impl_ || !impl_->commandQueue || !impl_->fence || !impl_->fenceEvent) return false;
    if (impl_->synchronizationFailed || impl_->injectedDeviceLost ||
        FAILED(impl_->device->GetDeviceRemovedReason())) return false;
    const uint64_t value = impl_->nextFenceValue++;
    if (FAILED(impl_->commandQueue->Signal(impl_->fence.Get(), value))) {
        impl_->synchronizationFailed = true;
        return false;
    }
    // UINT64_MAX is the documented removed-device sentinel, not fence completion.
    if (impl_->fence->GetCompletedValue() == std::numeric_limits<uint64_t>::max()) {
        impl_->synchronizationFailed = true;
        return false;
    }
    if (impl_->fence->GetCompletedValue() < value) {
        if (FAILED(impl_->fence->SetEventOnCompletion(value, impl_->fenceEvent))) {
            impl_->synchronizationFailed = true;
            return false;
        }
        const ULONGLONG deadline = GetTickCount64() + kGpuWaitTimeoutMs;
        for (;;) {
            const DWORD result = WaitForSingleObjectEx(impl_->fenceEvent, 100, FALSE);
            if (result == WAIT_OBJECT_0) break;
            if (result != WAIT_TIMEOUT || FAILED(impl_->device->GetDeviceRemovedReason()) ||
                GetTickCount64() >= deadline) {
                impl_->synchronizationFailed = true;
                return false;
            }
        }
    }
    return checkDeviceHealthy();
}

bool D3D12Context::retireGpuForShutdown() noexcept {
    if (!impl_ || !impl_->device || !impl_->commandQueue) return true;
    if (impl_->teardownRetired) return impl_->teardownDrained;
    impl_->synchronizationFailed = true; // Never make allocators reusable again.
    const auto finish = [&](bool drained) {
        impl_->teardownRetired = true;
        impl_->teardownDrained = drained;
        return drained;
    };
    if (FAILED(impl_->device->GetDeviceRemovedReason())) return finish(false);
    // The synthetic removal hook already drained the queue before setting its flag.
    if (impl_->injectedDeviceLost) return finish(true);

    // A previous timed-out SetEventOnCompletion can still signal its event later.
    // Poll this fresh fence target instead of trusting an old event notification.
    const uint64_t value = impl_->nextFenceValue++;
    if (impl_->fence && SUCCEEDED(impl_->commandQueue->Signal(impl_->fence.Get(), value))) {
        const ULONGLONG deadline = GetTickCount64() + kGpuWaitTimeoutMs;
        for (;;) {
            const uint64_t completed = impl_->fence->GetCompletedValue();
            if (completed == std::numeric_limits<uint64_t>::max() ||
                FAILED(impl_->device->GetDeviceRemovedReason())) return finish(false);
            if (completed >= value) return finish(true);
            if (GetTickCount64() >= deadline) break;
            Sleep(1);
        }
    }
    // RemoveDevice cancels use of this device and signals its fences to UINT64_MAX;
    // that is removal, not successful GPU completion. Device5 was required before
    // queue creation, so all paths with submitted work have this interface.
    BOO_LOG_ERROR("D3D12", "GPU teardown drain failed; removing D3D12 device before releasing resources.");
    impl_->removalDevice->RemoveDevice();
    return finish(false);
}

bool D3D12Context::injectDeviceLostForTest() noexcept {
    if (!impl_ || impl_->frameOpen || !waitForGpu()) return false;
    impl_->injectedDeviceLost = true;
    BOO_LOG_WARN("D3D12", "Injected device loss after retiring GPU work (physical device remains intact).");
    return true;
}

bool D3D12Context::injectSynchronizationFailureForTest() noexcept {
    if (!impl_ || impl_->frameOpen || !checkDeviceHealthy()) return false;
    impl_->synchronizationFailed = true;
    return true;
}

bool D3D12Context::executeSingleCommandListAndWait(
    const std::function<void(void* cmdList)>& recordFn,
    std::string* outError) {
    if (!impl_ || !impl_->device || !impl_->commandQueue || !impl_->commandList) {
        if (outError) *outError = "D3D12Context not initialized";
        return false;
    }
    if (impl_->frameOpen) {
        if (outError) *outError = "Cannot call executeSingleCommandListAndWait while a swapchain frame is open";
        return false;
    }

    waitForGpu();
    if (!checkDeviceHealthy(outError)) return false;
    HRESULT hr = impl_->commandAllocators[0]->Reset();
    if (FAILED(hr)) {
        if (outError) *outError = "CommandAllocator[0]->Reset failed (" + hrHex(hr) + ")";
        return false;
    }
    hr = impl_->commandList->Reset(impl_->commandAllocators[0].Get(), nullptr);
    if (FAILED(hr)) {
        if (outError) *outError = "CommandList->Reset failed (" + hrHex(hr) + ")";
        return false;
    }

    recordFn(impl_->commandList.Get());

    hr = impl_->commandList->Close();
    if (FAILED(hr)) {
        if (outError) *outError = "CommandList->Close failed (" + hrHex(hr) + ")";
        return false;
    }

    ID3D12CommandList* lists[] = {impl_->commandList.Get()};
    impl_->commandQueue->ExecuteCommandLists(1, lists);
    waitForGpu();
    return checkDeviceHealthy(outError);
}

bool D3D12Context::resize(uint32_t width, uint32_t height) {
    if (!impl_ || impl_->headless || !impl_->swapChain) return false;
    if (impl_->frameOpen || !checkDeviceHealthy()) return false;
    if (width == 0 || height == 0) return true;
    if (width == width_ && height == height_) return true;

    impl_->frameGeneration.invalidate();
    waitForGpu();
    impl_->fgHudless.Reset();impl_->fgUiAlpha.Reset();impl_->fgTargetsBound=false;
    if (!checkDeviceHealthy()) return false;
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        impl_->renderTargets[i].Reset();
        impl_->frameFenceValues[i] = 0;
        impl_->frameRecordedMask[i] = 0;
    }

    const HRESULT hr = impl_->swapChain->ResizeBuffers(
        kFrameCount,
        width,
        height,
        impl_->backBufferFormat,
        impl_->swapChainFlags);
    if (FAILED(hr)) {
        BOO_LOG_ERROR("D3D12", "SwapChain::ResizeBuffers failed (" + hrHex(hr) + ")");
        return false;
    }

    width_  = width;
    height_ = height;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = impl_->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        if (FAILED(impl_->swapChain->GetBuffer(i, IID_PPV_ARGS(&impl_->renderTargets[i])))) {
            return false;
        }
        impl_->device->CreateRenderTargetView(impl_->renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += impl_->rtvDescriptorSize;
    }
    impl_->frameIndex = impl_->swapChain->GetCurrentBackBufferIndex();
    return true;
}

bool D3D12Context::beginFrame() {
    if (!impl_ || impl_->headless || !impl_->swapChain || impl_->frameOpen) return false;

    std::string healthReason;
    if (!checkDeviceHealthy(&healthReason)) {
        BOO_LOG_ERROR("D3D12", "beginFrame checkDeviceHealthy failed: " + healthReason);
        return false;
    }
    if (impl_->occluded) {
        const HRESULT testHr = impl_->swapChain->Present(0, DXGI_PRESENT_TEST);
        if (FAILED(testHr)) {
            impl_->synchronizationFailed = true;
            return false;
        }
        if (testHr == DXGI_STATUS_OCCLUDED) {
            // Display is powered off / locked during non-minimized (--no-activate) execution.
            // Pace gently at ~60 Hz while still executing and verifying real GPU frames via fence sync.
            Sleep(16);
        } else {
            impl_->occluded = false;
        }
    }
    if (impl_->waitableHandle) {
        if (impl_->occluded) {
            // Drain any signaled latency token without blocking if DWM VBlank is asleep while occluded.
            (void)WaitForSingleObjectEx(impl_->waitableHandle, 0, FALSE);
        } else {
            const DWORD waitRes = WaitForSingleObjectEx(impl_->waitableHandle, 1000, FALSE);
            if (waitRes != WAIT_OBJECT_0) {
                BOO_LOG_WARN("D3D12", "beginFrame waitableHandle wait returned " + std::to_string(waitRes));
                return false;
            }
        }
    }

    impl_->frameIndex = impl_->swapChain->GetCurrentBackBufferIndex();
    const uint64_t requiredFence = impl_->frameFenceValues[impl_->frameIndex];
    if (requiredFence > 0 && impl_->fence->GetCompletedValue() < requiredFence) {
        if (SUCCEEDED(impl_->fence->SetEventOnCompletion(requiredFence, impl_->fenceEvent))) {
            if (WaitForSingleObjectEx(impl_->fenceEvent, 2000, FALSE) != WAIT_OBJECT_0) {
                impl_->synchronizationFailed = true;
                return false;
            }
        } else {
            impl_->synchronizationFailed = true;
            return false;
        }
    }
    if (!checkDeviceHealthy()) return false;

    // A missing timestamp must not repeat the preceding slot's measurement.
    impl_->lastPassTimings = {};
    // Read resolved 5-point GPU timestamps from this retired frame slot (zero GPU stall)
    if (requiredFence > 0 && impl_->timestampReadback && impl_->gpuTimestampFreq > 0) {
        const uint32_t slot = impl_->frameIndex;
        const size_t base = static_cast<size_t>(slot) * kTimestampsPerFrame;
        D3D12_RANGE readRange{
            sizeof(uint64_t) * base,
            sizeof(uint64_t) * (base + kTimestampsPerFrame)};
        void* mapped = nullptr;
        if (SUCCEEDED(impl_->timestampReadback->Map(0, &readRange, &mapped)) && mapped) {
            const auto* stamps = reinterpret_cast<const uint64_t*>(mapped);
            const uint64_t t0 = stamps[base + 0];
            const uint64_t t1 = stamps[base + 1];
            const uint64_t t2 = stamps[base + 2];
            const uint64_t t3 = stamps[base + 3];
            const uint64_t t4 = stamps[base + 4];
            D3D12_RANGE emptyWrite{0, 0};
            impl_->timestampReadback->Unmap(0, &emptyWrite);

            const double invFreqMs = 1000.0 / static_cast<double>(impl_->gpuTimestampFreq);
            const uint8_t mask = impl_->frameRecordedMask[slot];

            GpuPassTimingsMs pt{};
            if (t4 >= t0) {
                pt.totalMs = static_cast<double>(t4 - t0) * invFreqMs;
            }
            if ((mask & (1u << 1)) != 0 && t1 >= t0) {
                pt.sceneMs = static_cast<double>(t1 - t0) * invFreqMs;
            }
            if ((mask & (1u << 2)) != 0 && (mask & (1u << 1)) != 0 && t2 >= t1) {
                pt.bloomMs = static_cast<double>(t2 - t1) * invFreqMs;
            }
            if ((mask & (1u << 3)) != 0 && (mask & (1u << 2)) != 0 && t3 >= t2) {
                pt.compositeMs = static_cast<double>(t3 - t2) * invFreqMs;
            }
            if ((mask & (1u << 3)) != 0 && t4 >= t3) {
                pt.presentPassMs = static_cast<double>(t4 - t3) * invFreqMs;
            }
            impl_->lastPassTimings = pt;
        }
    }

    auto& alloc = impl_->commandAllocators[impl_->frameIndex];
    if (FAILED(alloc->Reset()) || FAILED(impl_->commandList->Reset(alloc.Get(), nullptr))) {
        impl_->synchronizationFailed = true;
        return false;
    }

    impl_->currentRecordedMask = 0;
    if (impl_->timestampHeap) {
        const UINT baseIdx = impl_->frameIndex * kTimestampsPerFrame;
        impl_->commandList->EndQuery(impl_->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, baseIdx + 0);
    }

    D3D12_RESOURCE_BARRIER toRt{};
    toRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRt.Transition.pResource   = impl_->renderTargets[impl_->frameIndex].Get();
    toRt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRt.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    impl_->commandList->ResourceBarrier(1, &toRt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = impl_->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(impl_->frameIndex) * impl_->rtvDescriptorSize;

    // Pure matte black clear for clean 16:9 letterbox/pillarbox bars
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    impl_->commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    impl_->commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    impl_->commandList->RSSetViewports(1, &vp);
    impl_->commandList->RSSetScissorRects(1, &scissor);

    impl_->frameOpen = true;
    impl_->frameGeneration.beginRender();
    return true;
}

void D3D12Context::bindSwapchainRenderTarget() noexcept {
    if (!impl_ || !impl_->frameOpen || !impl_->rtvHeap) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = impl_->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(impl_->frameIndex) * impl_->rtvDescriptorSize;
    impl_->commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
}

void D3D12Context::beginSimulation(FrameGenerationMode mode,ReflexMode reflex) {if(impl_ && impl_->swapChain) impl_->frameGeneration.beginSimulation(mode,reflex);}
void D3D12Context::endSimulation() {if(impl_) impl_->frameGeneration.endSimulation();}
FrameGenerationStats D3D12Context::frameGenerationStats() const noexcept {return impl_ ? impl_->frameGeneration.stats() : FrameGenerationStats{};}
bool D3D12Context::frameGenerationInputsNeeded() const noexcept {return impl_ && impl_->frameOpen && impl_->swapChain && impl_->frameGeneration.needsInputs();}
void D3D12Context::setFrameGenerationScene(const M1CameraState& camera,void* depth,void* motion,uint32_t w,uint32_t h,const float* jitter,bool reset) {
    if(frameGenerationInputsNeeded()) impl_->frameGeneration.scene(camera,static_cast<ID3D12Resource*>(depth),static_cast<ID3D12Resource*>(motion),w,h,jitter,reset);
}
bool D3D12Context::bindFrameGenerationTargets() {
    if(!frameGenerationInputsNeeded() || !impl_->frameGeneration.hasScene()) return false;
    auto& s=*impl_;
    if(!s.fgHudless || !s.fgUiAlpha) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=2;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if(!s.fgRtv && FAILED(s.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s.fgRtv)))) return false;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        auto tex=s.renderTargets[s.frameIndex]->GetDesc();tex.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> hudless,alpha;
        if(FAILED(s.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&tex,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&hudless)))) return false;
        tex.Format=DXGI_FORMAT_R8_UNORM;
        if(FAILED(s.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&tex,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&alpha)))) return false;
        auto rtv=s.fgRtv->GetCPUDescriptorHandleForHeapStart();s.device->CreateRenderTargetView(hudless.Get(),nullptr,rtv);
        rtv.ptr+=s.rtvDescriptorSize;s.device->CreateRenderTargetView(alpha.Get(),nullptr,rtv);
        s.fgHudless=std::move(hudless);s.fgUiAlpha=std::move(alpha);
    }
    std::array<D3D12_RESOURCE_BARRIER,2> barriers{};
    for(size_t i=0;i<2;++i) {
        barriers[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition={i ? s.fgUiAlpha.Get() : s.fgHudless.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET};
    }
    s.commandList->ResourceBarrier(2,barriers.data());
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE,3> handles{};handles[0]=s.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handles[0].ptr+=SIZE_T(s.frameIndex)*s.rtvDescriptorSize;handles[1]=s.fgRtv->GetCPUDescriptorHandleForHeapStart();handles[2]=handles[1];handles[2].ptr+=s.rtvDescriptorSize;
    s.commandList->OMSetRenderTargets(3,handles.data(),FALSE,nullptr);s.fgTargetsBound=true;return true;
}
void D3D12Context::finishFrameGenerationInputs() {
    if(!impl_ || !impl_->fgTargetsBound) return;
    auto& s=*impl_;s.fgTargetsBound=false;
    std::array<D3D12_RESOURCE_BARRIER,2> barriers{};
    for(size_t i=0;i<2;++i) {
        barriers[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition={i ? s.fgUiAlpha.Get() : s.fgHudless.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    }
    s.commandList->ResourceBarrier(2,barriers.data());
    s.frameGeneration.tag(s.commandList.Get(),s.fgHudless.Get(),s.fgUiAlpha.Get());
}

void D3D12Context::writePassTimestamp(uint32_t passSlot1To3) noexcept {
    if (!impl_ || !impl_->frameOpen || !impl_->timestampHeap) return;
    if (passSlot1To3 < 1 || passSlot1To3 > 3) return;
    const UINT idx = impl_->frameIndex * kTimestampsPerFrame + passSlot1To3;
    impl_->commandList->EndQuery(impl_->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, idx);
    impl_->currentRecordedMask = static_cast<uint8_t>(impl_->currentRecordedMask | (1u << passSlot1To3));
}

bool D3D12Context::endFrameAndPresent(bool vsync, double& outLastCompletedGpuMs) {
    outLastCompletedGpuMs = 0.0;
    if (!impl_ || !impl_->frameOpen) return false;
    impl_->frameOpen = false;

    D3D12_RESOURCE_BARRIER toPresent{};
    toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPresent.Transition.pResource   = impl_->renderTargets[impl_->frameIndex].Get();
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    impl_->commandList->ResourceBarrier(1, &toPresent);

    impl_->frameRecordedMask[impl_->frameIndex] = impl_->currentRecordedMask;
    if (impl_->timestampHeap && impl_->timestampReadback) {
        const UINT baseIdx = impl_->frameIndex * kTimestampsPerFrame;
        // Record EndQuery for any unrecorded intermediate pass slots so ResolveQueryData(base, 5)
        // never triggers RESOLVE_QUERY_INVALID_QUERY_STATE under D3D12 debug validation.
        for (UINT s = 1; s <= 3; ++s) {
            if ((impl_->currentRecordedMask & (1u << s)) == 0) {
                impl_->commandList->EndQuery(impl_->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, baseIdx + s);
            }
        }
        impl_->commandList->EndQuery(impl_->timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, baseIdx + 4);
        impl_->commandList->ResolveQueryData(
            impl_->timestampHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            baseIdx,
            kTimestampsPerFrame,
            impl_->timestampReadback.Get(),
            sizeof(uint64_t) * baseIdx);
    }

    if (FAILED(impl_->commandList->Close())) {
        impl_->synchronizationFailed = true;
        return false;
    }

    ID3D12CommandList* lists[] = {impl_->commandList.Get()};
    impl_->commandQueue->ExecuteCommandLists(1, lists);

    LARGE_INTEGER t0{}, t1{}, qpf{};
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);

    const UINT syncInterval = vsync ? 1u : 0u;
    const UINT presentFlags = (!vsync && impl_->allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    impl_->frameGeneration.beforePresent();
    const HRESULT hr = impl_->swapChain->Present(syncInterval, presentFlags);
    impl_->frameGeneration.afterPresent(hr==S_OK);

    QueryPerformanceCounter(&t1);
    const double presentWallMs =
        (qpf.QuadPart > 0) ? (static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(qpf.QuadPart)) : 0.0;
    if (presentWallMs > 34.0) {
        ++presentStallCount_;
    }

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        const HRESULT reason = impl_->device->GetDeviceRemovedReason();
        BOO_LOG_ERROR("D3D12", "DXGI Present reported device lost! GetDeviceRemovedReason=" + hrHex(reason));
        ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
        if (SUCCEEDED(impl_->device.As(&dred))) {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
            D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
            if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
                uint32_t count = 0;
                for (auto* node = crumbs.pHeadAutoBreadcrumbNode; node && count < 64; node = node->pNext, ++count)
                    BOO_LOG_ERROR("DRED", "Command list progress " + std::to_string(node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0) + "/" + std::to_string(node->BreadcrumbCount));
            }
            if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)))
                BOO_LOG_ERROR("DRED", "Page-fault GPU VA: " + std::to_string(fault.PageFaultVA));
        }
        return false;
    }
    if (FAILED(hr)) {
        BOO_LOG_ERROR("D3D12", "SwapChain::Present failed (" + hrHex(hr) + ")");
        return false;
    }

    const uint64_t fenceVal = impl_->nextFenceValue++;
    if (FAILED(impl_->commandQueue->Signal(impl_->fence.Get(), fenceVal))) {
        impl_->synchronizationFailed = true;
        return false;
    }
    impl_->frameFenceValues[impl_->frameIndex] = fenceVal;
    impl_->occluded = (hr == DXGI_STATUS_OCCLUDED);
    ++presentCount_;

    outLastCompletedGpuMs = impl_->lastPassTimings.totalMs;
    return true;
}

bool D3D12Context::checkDeviceHealthy(std::string* outReason) const {
    if (!impl_ || !impl_->device) {
        if (outReason) *outReason = "D3D12 device not initialized";
        return false;
    }
    if (impl_->synchronizationFailed) {
        if (outReason) *outReason = "GPU synchronization failed";
        return false;
    }
    if (impl_->injectedDeviceLost) {
        if (outReason) *outReason = "Injected DXGI_ERROR_DEVICE_REMOVED lifecycle fault";
        return false;
    }
    const HRESULT reason = impl_->device->GetDeviceRemovedReason();
    if (FAILED(reason)) {
        if (outReason) *outReason = "Device removed (" + hrHex(reason) + ")";
        return false;
    }
    return true;
}

uint64_t D3D12Context::queryEstimatedAppVramBytes() const noexcept {
    if (!impl_ || !impl_->adapter3) return 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO memInfo{};
    if (SUCCEEDED(impl_->adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) {
        return memInfo.CurrentUsage;
    }
    return 0;
}

void* D3D12Context::deviceRaw() const noexcept {
    return impl_ ? impl_->device.Get() : nullptr;
}
bool D3D12Context::ensureNgxRuntime(const std::filesystem::path& folder) {
    if (!impl_ || !impl_->device || isWarp_) return false;
    if (!impl_->directNgxInitialized) impl_->directNgxInitialized = initializeNgxDevice(impl_->device.Get(),folder);
    return impl_->directNgxInitialized;
}

void* D3D12Context::commandQueueRaw() const noexcept {
    return impl_ ? impl_->commandQueue.Get() : nullptr;
}

void* D3D12Context::commandListRaw() const noexcept {
    return impl_ ? impl_->commandList.Get() : nullptr;
}

uint32_t D3D12Context::backBufferDxgiFormat() const noexcept {
    return impl_ ? static_cast<uint32_t>(impl_->backBufferFormat) : static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
}

uint32_t D3D12Context::swapchainChannelBits() const noexcept {
    if (impl_ && impl_->backBufferFormat == DXGI_FORMAT_R10G10B10A2_UNORM) {
        return 10u;
    }
    return 8u;
}

uint32_t D3D12Context::currentBackBufferIndex() const noexcept {
    return impl_ ? impl_->frameIndex : 0u;
}

uint32_t D3D12Context::debugErrorCount() const {
    if (!impl_ || !impl_->device) return 0;
    ComPtr<ID3D12InfoQueue> queue;
    if (FAILED(impl_->device.As(&queue))) return 0;
    uint32_t count = 0;
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<uint8_t> bytes(size);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if (SUCCEEDED(queue->GetMessage(i, msg, &size)) && msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            ++count;
            BOO_LOG_ERROR("D3D12Validation", msg->pDescription);
        }
    }
    return count;
}

uint32_t D3D12Context::debugWarningCount() const {
    if (!impl_ || !impl_->device) return 0;
    ComPtr<ID3D12InfoQueue> queue;
    if (FAILED(impl_->device.As(&queue))) return 0;
    uint32_t count = 0;
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        if (FAILED(queue->GetMessage(i, nullptr, &size)) || size < sizeof(D3D12_MESSAGE)) continue;
        std::vector<uint8_t> bytes(size);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if (SUCCEEDED(queue->GetMessage(i, msg, &size)) && msg->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
            ++count;
            BOO_LOG_WARN("D3D12Validation", msg->pDescription);
        }
    }
    return count;
}

bool D3D12Context::debugLayerActive() const noexcept {
    return impl_ && impl_->debugLayerEnabled;
}

GpuPassTimingsMs D3D12Context::lastPassTimings() const noexcept {
    return impl_ ? impl_->lastPassTimings : GpuPassTimingsMs{};
}

} // namespace boo
