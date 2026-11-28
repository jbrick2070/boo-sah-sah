#include "boo/flow_smoke.hpp"

#include "boo/logging.hpp"
#include "boo/mapped_write.hpp"
#include "boo/m1_scene.hpp"
#include "boo/render.hpp"
#include "boo/startup_progress.hpp"
#include "boo/pipeline_compiler.hpp"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <thread>
#include <fstream>
#include <vector>

#if defined(BOO_HAS_FLOW)
// NVIDIA Flow's headers, from the PhysX SDK checkout. The loader is header-only and finds
// nvflow.dll and nvflowext.dll at run time.
#pragma warning(push, 0)
#include "NvFlowLoader.h"
#define PNANOVDB_C
#include "nanovdb/PNanoVDB.h"
#pragma warning(pop)
#endif

namespace boo {
using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kSlots = 4;        // frames that can be in flight, as elsewhere in the renderer
constexpr uint32_t kMaxSources = 8;
constexpr uint32_t kFlowBlocks = 1536; // most sparse blocks Flow may allocate (each 32 voxels cubed)
constexpr float kVoxelMetres = 0.5f;

// Mirrors SmokeConstants in assets/shaders/flow_smoke.hlsl.
struct SmokeConstants {
    float eye[3];        float tanHalfFov;
    float right[3];      float aspect;
    float up[3];         float extinction;
    float forward[3];    float stepMetres;
    float lightDir[3];   float voxelMetres;
    float lightColor[3]; uint32_t frame;
    float ambient[3];    uint32_t width;
    float boxMin[3];     uint32_t height;
    float boxMax[3];     float shadowReach;
};
static_assert(sizeof(SmokeConstants) == 144);

void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    cmd->ResourceBarrier(1, &barrier);
}

D3D12_RESOURCE_DESC bufferDesc(uint64_t bytes) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 256u);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES h{};
    h.Type = type;
    h.CreationNodeMask = h.VisibleNodeMask = 1;
    return h;
}

void normalize3(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 1e-8f) { v[0] /= length; v[1] /= length; v[2] /= length; }
}
void cross3(const float a[3], const float b[3], float r[3]) {
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}

#if defined(BOO_HAS_FLOW)
void flowLoaderError(const char* text, void*) { BOO_LOG_WARN("PhysX", std::string("NVIDIA Flow library: ") + (text ? text : "")); }
void flowLog(NvFlowLogLevel level, const char* format, ...) {
    if (level != eNvFlowLogLevel_error && level != eNvFlowLogLevel_warning) return;
    char text[512]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (level == eNvFlowLogLevel_error) BOO_LOG_ERROR("PhysX", std::string("NVIDIA Flow: ") + text);
    else BOO_LOG_WARN("PhysX", std::string("NVIDIA Flow: ") + text);
}
#endif

} // namespace

struct FlowSmoke::Impl {
    explicit Impl(D3D12Context& context) : d3d(context) {}
    D3D12Context& d3d;
    ID3D12Device* device = nullptr;
    bool d3dReady = false;

    // The film's side: one compute pass that marches the grid over SceneHDR.
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> heap; // [0] scene distance SRV, [1] SceneHDR UAV
    ID3D12Resource* boundHdr = nullptr;
    ID3D12Resource* boundDistance = nullptr;
    struct GridSlot {
        ComPtr<ID3D12Resource> buffer; // the NanoVDB grid as a structured buffer of words
        uint8_t* mapped = nullptr;
        uint64_t capacityBytes = 0;
        uint64_t serial = 0;           // which grid it holds
    };
    std::array<GridSlot, kSlots> gridSlots;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> queryReadback;
    uint64_t timestampHz = 0;
    std::array<bool, kSlots> timedSlot{};

    // The latest smoke Flow has finished.
    std::vector<uint32_t> grid;
    uint64_t gridSerial = 0;
    float boxMin[3]{}, boxMax[3]{};
    uint64_t activeVoxels = 0;

    FlowSmokeStats published;
    bool loggedRunning = false;

#if defined(BOO_HAS_FLOW)
    bool flowRunning = false;
    bool flowFailed = false; // do not retry every frame
    // Flow starts on a worker thread so the frame never waits for its Vulkan device. The worker only
    // writes the fields below; the render thread reads them once it sees startDone.
    std::thread starter;
    std::atomic<bool> startDone{false};
    bool starting = false;
    bool startOk = false;
    std::string startBackend, startReason;
    NvFlowLoader loader{};
    NvFlowContextInterface contextInterface{};
    NvFlowDeviceManager* manager = nullptr;
    NvFlowDevice* flowDevice = nullptr;
    NvFlowDeviceQueue* queue = nullptr;
    NvFlowContext* context = nullptr;
    NvFlowGrid* flowGrid = nullptr;
    NvFlowGridParamsNamed* consumerNamed = nullptr;
    NvFlowGridParamsNamed* producerNamed = nullptr;
    NvFlowGridParams* producer = nullptr;
    HMODULE moduleFlow = nullptr, moduleFlowExt = nullptr;
    NvFlowGridSimulateLayerParams simParams{};
    NvFlowGridOffscreenLayerParams offscreenParams{};
    NvFlowGridRenderLayerParams renderParams{};
    std::array<NvFlowGridEmitterSphereParams, kMaxSources> spheres{};
    std::array<NvFlowGridEmitterSphereParams*, kMaxSources> spherePointers{};
    NvFlowGridSimulateLayerParams* simPointer = nullptr;
    NvFlowGridOffscreenLayerParams* offscreenPointer = nullptr;
    NvFlowGridRenderLayerParams* renderPointer = nullptr;
    uint64_t version = 0;
    double simTime = 0.0;
    float profiledGpuMs = 0.0f;

    static void NV_FLOW_ABI profilerReport(void* userdata, NvFlowUint64, NvFlowUint count, NvFlowProfilerEntry* entries) {
        auto* self = static_cast<Impl*>(userdata);
        float seconds = 0.0f;
        for (NvFlowUint i = 0; i < count; ++i) seconds += entries[i].gpuDeltaTime;
        // Smoothed: the readout should be steady enough to read.
        const float ms = seconds * 1000.0f;
        self->profiledGpuMs = self->profiledGpuMs > 0.0f ? self->profiledGpuMs * 0.9f + ms * 0.1f : ms;
    }

    bool startFlow();
    void stopFlow();
#endif

    bool initializeD3D(std::string* error);
    bool bindTargets(ID3D12Resource* hdr, ID3D12Resource* distance);
    void collect(uint32_t slot);
};

bool FlowSmoke::Impl::initializeD3D(std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        published.reason = what;
        return false;
    };
    device = static_cast<ID3D12Device*>(d3d.deviceRaw());
    if (!device) return fail("No D3D12 device");
#if !defined(BOO_HAS_FLOW)
    published.reason = "This build does not include the NVIDIA PhysX SDK";
    return true;
#else
    std::ifstream file(resolveResourcePath("assets/shaders/flow_smoke_cs.cso"), std::ios::binary);
    const std::vector<char> code((std::istreambuf_iterator<char>(file)), {});
    if (code.empty()) return fail("Missing shader assets/shaders/flow_smoke_cs.cso");

    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, sizeof(SmokeConstants) / 4};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; // t1: the NanoVDB words
    params[2].Descriptor = {1, 0};
    D3D12_ROOT_SIGNATURE_DESC rootDesc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root))))
        return fail("Smoke root signature creation failed");
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = root.Get();
    psoDesc.CS = {code.data(), code.size()};
    if (FAILED(compileWhilePainting("smoke renderer", code.size(), [&] {
            return device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso));
        }))) return fail("Smoke pipeline creation failed");

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)))) return fail("Smoke descriptor heap creation failed");

    D3D12_QUERY_HEAP_DESC queryDesc{};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDesc.Count = kSlots * 2u;
    const auto readbackHeap = heapProps(D3D12_HEAP_TYPE_READBACK);
    const auto readbackDesc = bufferDesc(kSlots * 16ull);
    if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries))) ||
        FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&queryReadback))))
        return fail("Smoke timing resources failed");
    if (auto* commandQueue = static_cast<ID3D12CommandQueue*>(d3d.commandQueueRaw())) commandQueue->GetTimestampFrequency(&timestampHz);
    d3dReady = true;
    published.state = RendererFeatureState::Off;
    published.reason.clear();
    return true;
#endif
}

bool FlowSmoke::Impl::bindTargets(ID3D12Resource* hdr, ID3D12Resource* distance) {
    if (hdr == boundHdr && distance == boundDistance) return true;
    // Descriptors in use by frames still in flight must not change under them.
    if (!d3d.waitForGpu()) return false;
    const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto at = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(distance, &srv, at);
    at.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(hdr, nullptr, &uav, at);
    boundHdr = hdr;
    boundDistance = distance;
    return true;
}

void FlowSmoke::Impl::collect(uint32_t slot) {
    if (!timedSlot[slot] || !queryReadback || !timestampHz) return;
    timedSlot[slot] = false;
    const SIZE_T base = static_cast<SIZE_T>(slot) * 16u;
    D3D12_RANGE range{base, base + 16u};
    uint8_t* mapped = nullptr;
    if (FAILED(queryReadback->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped) return;
    uint64_t stamps[2]{};
    std::memcpy(stamps, mapped + base, sizeof(stamps));
    D3D12_RANGE nothing{0, 0};
    queryReadback->Unmap(0, &nothing);
    if (stamps[1] <= stamps[0]) return;
    const float ms = static_cast<float>(double(stamps[1] - stamps[0]) * 1000.0 / double(timestampHz));
    published.renderGpuMs = published.renderGpuMs > 0.0f ? published.renderGpuMs * 0.9f + ms * 0.1f : ms;
}

#if defined(BOO_HAS_FLOW)
bool FlowSmoke::Impl::startFlow() {
    if (flowRunning) return true;
    if (flowFailed) return false;
    auto fail = [&](const std::string& what) {
        startReason = what;
        BOO_LOG_WARN("PhysX", "Smoke unavailable: " + what);
        stopFlow();
        return false;
    };
    // Load the two libraries from beside the executable, whatever characters its path has in it;
    // Flow's own loader then finds them by name.
    wchar_t exePath[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    const std::filesystem::path folder = std::filesystem::path(exePath).parent_path();
    const auto flowClock = std::chrono::steady_clock::now();
    auto stage = [&](const char* what) {
        BOO_LOG_DEBUG("PhysX", std::string("Flow start: ") + what + " at " + std::to_string(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - flowClock).count()) + " ms");
    };
    moduleFlow = LoadLibraryW((folder / L"nvflow.dll").c_str());
    moduleFlowExt = LoadLibraryW((folder / L"nvflowext.dll").c_str());
    if (!moduleFlow || !moduleFlowExt) return fail("nvflow.dll and nvflowext.dll are not beside the app");
    startupAssetLoaded(folder / L"nvflow.dll"); // on the loading bar when smoke starts with the app
    startupAssetLoaded(folder / L"nvflowext.dll");
    stage("libraries loaded");
    NvFlowLoaderInit(&loader, flowLoaderError, nullptr);
    if (!loader.module_nvflow || !loader.module_nvflowext) return fail("NVIDIA Flow's libraries did not load");

    stage("loader ready");
    manager = loader.deviceInterface.createDeviceManager(NV_FLOW_FALSE, nullptr, 0u);
    if (!manager) return fail("NVIDIA Flow could not start Vulkan");
    // Simulate on the GPU the film renders on.
    LUID wanted{};
    wanted.LowPart = static_cast<DWORD>(d3d.adapterLuidLow());
    wanted.HighPart = static_cast<LONG>(d3d.adapterLuidHigh());
    NvFlowUint chosen = 0u;
    bool sameGpu = false;
    for (NvFlowUint i = 0u; i < 16u; ++i) {
        NvFlowPhysicalDeviceDesc physical{};
        if (!loader.deviceInterface.enumerateDevices(manager, i, &physical)) break;
        if (physical.deviceLUIDValid && std::memcmp(physical.deviceLUID, &wanted, sizeof(LUID)) == 0) {
            chosen = i;
            sameGpu = true;
            break;
        }
    }
    stage("device manager (Vulkan instance) ready");
    NvFlowDeviceDesc deviceDesc{};
    deviceDesc.deviceIndex = chosen;
    deviceDesc.enableExternalUsage = NV_FLOW_FALSE;
    deviceDesc.logPrint = flowLog;
    flowDevice = loader.deviceInterface.createDevice(manager, &deviceDesc);
    if (!flowDevice) return fail("NVIDIA Flow could not create its Vulkan device");
    stage("Vulkan device created");
    queue = loader.deviceInterface.getDeviceQueue(flowDevice);
    NvFlowContextInterface_duplicate(&contextInterface, loader.deviceInterface.getContextInterface(queue));
    context = loader.deviceInterface.getContext(queue);

    NvFlowGridDesc gridDesc = NvFlowGridDesc_default;
    gridDesc.maxLocations = kFlowBlocks;
    flowGrid = loader.gridInterface.createGrid(&contextInterface, context, loader.opList_orig, loader.extOpList_orig, &gridDesc);
    stage("grid created");
    consumerNamed = loader.gridParamsInterface.createGridParamsNamed("booSmoke");
    producerNamed = loader.gridParamsInterface.createGridParamsNamed("booSmoke");
    if (!flowGrid || !consumerNamed || !producerNamed) return fail("NVIDIA Flow could not create its grid");
    producer = loader.gridParamsInterface.mapGridParamsNamed(producerNamed);
    stage("parameters ready");
    loader.deviceInterface.enableProfiler(context, this, profilerReport);

    // Metres, Y up. Flow's defaults are centimetres with Z up.
    simParams = NvFlowGridSimulateLayerParams_default;
    simParams.densityCellSize = kVoxelMetres;
    simParams.enableVariableTimeStep = NV_FLOW_TRUE; // one step a frame, as long as the frame was
    simParams.nanoVdbExport.enabled = NV_FLOW_TRUE;
    simParams.nanoVdbExport.readbackEnabled = NV_FLOW_TRUE;
    simParams.nanoVdbExport.smokeEnabled = NV_FLOW_TRUE;
    simParams.advection.gravity = {0.0f, -1.0f, 0.0f};
    simParams.advection.combustionEnabled = NV_FLOW_FALSE; // warm air and vapour, nothing burning
    simParams.advection.smoke.damping = 0.03f;
    simParams.advection.smoke.fade = 0.06f;                // vapour thins out over about a quarter of a minute
    simParams.advection.temperature.damping = 0.02f;
    simParams.advection.coolingRate = 0.35f;
    offscreenParams = NvFlowGridOffscreenLayerParams_default;
    renderParams = NvFlowGridRenderLayerParams_default;
    simPointer = &simParams;
    offscreenPointer = &offscreenParams;
    renderPointer = &renderParams;
    for (uint32_t i = 0; i < kMaxSources; ++i) spherePointers[i] = &spheres[i];
    version = 0;
    simTime = 0.0;
    profiledGpuMs = 0.0f;
    flowRunning = true;
    startBackend = std::string("NVIDIA Flow (PhysX SDK), Vulkan compute on ") +
                   (sameGpu ? d3d.adapterName() : std::string("another GPU than the film's"));
    BOO_LOG_INFO("PhysX", "Smoke running: " + startBackend + ", " + std::to_string(kVoxelMetres).substr(0, 4) +
                          " m voxels, up to " + std::to_string(kFlowBlocks) + " sparse blocks.");
    return true;
}

void FlowSmoke::Impl::stopFlow() {
    if (queue) loader.deviceInterface.waitIdle(queue);
    if (flowGrid && context) loader.gridInterface.destroyGrid(context, flowGrid);
    flowGrid = nullptr;
    if (consumerNamed) loader.gridParamsInterface.destroyGridParamsNamed(consumerNamed);
    if (producerNamed) loader.gridParamsInterface.destroyGridParamsNamed(producerNamed);
    consumerNamed = producerNamed = nullptr;
    producer = nullptr;
    if (queue) {
        NvFlowUint64 flushed = 0u;
        loader.deviceInterface.flush(queue, &flushed, nullptr, nullptr);
        loader.deviceInterface.waitIdle(queue);
    }
    if (flowDevice) loader.deviceInterface.destroyDevice(manager, flowDevice);
    if (manager) loader.deviceInterface.destroyDeviceManager(manager);
    flowDevice = nullptr;
    manager = nullptr;
    queue = nullptr;
    context = nullptr;
    if (loader.module_nvflow || loader.module_nvflowext) NvFlowLoaderDestroy(&loader);
    loader = NvFlowLoader{};
    if (moduleFlowExt) FreeLibrary(moduleFlowExt);
    if (moduleFlow) FreeLibrary(moduleFlow);
    moduleFlow = moduleFlowExt = nullptr;
    if (flowRunning) BOO_LOG_INFO("PhysX", "Smoke stopped; NVIDIA Flow's device released.");
    flowRunning = false;
}
#endif

FlowSmoke::FlowSmoke(D3D12Context& d3d) : impl_(std::make_unique<Impl>(d3d)) {}

FlowSmoke::~FlowSmoke() {
#if defined(BOO_HAS_FLOW)
    if (impl_->starter.joinable()) impl_->starter.join();
    impl_->stopFlow();
#endif
}

bool FlowSmoke::initialize(std::string* error) { return impl_->initializeD3D(error); }

FlowSmokeStats FlowSmoke::stats() const { return impl_->published; }

void FlowSmoke::simulate(bool enabled, float dtSeconds, const FlowSmokeSource* sources, uint32_t count, bool reset) {
    Impl& m = *impl_;
#if !defined(BOO_HAS_FLOW)
    (void)enabled; (void)dtSeconds; (void)sources; (void)count; (void)reset;
    m.published.state = RendererFeatureState::Unavailable;
#else
    if (!m.d3dReady) {
        m.published.state = RendererFeatureState::Unavailable;
        return;
    }
    if (m.starting) {
        // Starting on the worker thread: nothing to simulate yet, and the frame does not wait.
        if (!m.startDone.load(std::memory_order_acquire)) {
            m.published.state = RendererFeatureState::Idle;
            return;
        }
        m.starter.join();
        m.starting = false;
        if (m.startOk) {
            m.published.backend = m.startBackend;
            m.published.reason.clear();
        } else {
            m.flowFailed = true;
            m.published.state = RendererFeatureState::Unavailable;
            m.published.reason = m.startReason;
            return;
        }
    }
    if (!enabled) {
        if (m.flowRunning) m.stopFlow();
        m.flowFailed = false; // switching it on again may try again
        m.grid.clear();
        m.gridSerial = 0;
        m.activeVoxels = 0;
        const std::string reason = m.published.reason;
        m.published = FlowSmokeStats{};
        m.published.state = RendererFeatureState::Off;
        return;
    }
    if (!m.flowRunning) {
        if (m.flowFailed) return;
        m.starting = true;
        m.startDone.store(false, std::memory_order_relaxed);
        m.published.state = RendererFeatureState::Idle; // shown as "starting"
        m.starter = std::thread([&m] {
            m.startOk = m.startFlow();
            m.startDone.store(true, std::memory_order_release);
        });
        return;
    }
    count = sources ? std::min(count, kMaxSources) : 0u;
    m.published.state = RendererFeatureState::Active;
    m.published.sources = count;
    m.published.voxelMetres = kVoxelMetres;
    const float dt = std::clamp(dtSeconds, 0.0f, 1.0f / 20.0f);
    if (dt <= 0.0f && !reset) return; // paused: the air holds still

    const auto started = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < count; ++i) {
        NvFlowGridEmitterSphereParams sphere = NvFlowEmitterSphereParams_default;
        sphere.luid = 1000u + i;
        sphere.position = {sources[i].position[0], sources[i].position[1], sources[i].position[2]};
        sphere.radius = sources[i].radius;
        sphere.velocity = {sources[i].drift[0], sources[i].rise, sources[i].drift[1]};
        sphere.temperature = sources[i].warmth;
        sphere.smoke = sources[i].smoke;
        sphere.fuel = 0.0f;
        sphere.coupleRateFuel = 0.0f;
        sphere.coupleRateSmoke = 2.0f;
        sphere.coupleRateTemperature = 2.0f;
        sphere.coupleRateVelocity = 2.0f;
        m.spheres[i] = sphere;
    }
    m.simParams.luid = 1u;
    m.simParams.forceClear = reset ? NV_FLOW_TRUE : NV_FLOW_FALSE;
    ++m.version;
    m.simTime += dt;
    NvFlowDatabaseTypeSnapshot types[4] = {
        {m.version, &NvFlowGridSimulateLayerParams_NvFlowReflectDataType, reinterpret_cast<NvFlowUint8**>(&m.simPointer), 1u},
        {m.version, &NvFlowGridEmitterSphereParams_NvFlowReflectDataType, reinterpret_cast<NvFlowUint8**>(m.spherePointers.data()), count},
        {m.version, &NvFlowGridOffscreenLayerParams_NvFlowReflectDataType, reinterpret_cast<NvFlowUint8**>(&m.offscreenPointer), 1u},
        {m.version, &NvFlowGridRenderLayerParams_NvFlowReflectDataType, reinterpret_cast<NvFlowUint8**>(&m.renderPointer), 1u}};
    NvFlowDatabaseSnapshot snapshot = {m.version, types, 4u};
    NvFlowGridParamsDescSnapshot commit = {snapshot, m.simTime, dt, static_cast<NvFlowBool32>(reset ? NV_FLOW_TRUE : NV_FLOW_FALSE), nullptr, 0u};
    m.loader.gridParamsInterface.commitParams(m.producer, &commit);

    NvFlowGridRenderData renderData{};
    NvFlowGridParams* consumer = m.loader.gridParamsInterface.mapGridParamsNamed(m.consumerNamed);
    NvFlowGridParamsDesc desc{};
    NvFlowGridParamsSnapshot* paramsSnapshot = m.loader.gridParamsInterface.getParamsSnapshot(consumer, m.simTime, 0llu);
    if (m.loader.gridParamsInterface.mapParamsDesc(consumer, paramsSnapshot, &desc)) {
        m.loader.gridInterface.simulate(m.context, m.flowGrid, &desc, NV_FLOW_FALSE);
        m.loader.gridInterface.getRenderData(m.context, m.flowGrid, &renderData);
        m.loader.gridParamsInterface.unmapParamsDesc(consumer, paramsSnapshot);
    }
    NvFlowUint64 flushed = 0u;
    if (m.loader.deviceInterface.flush(m.queue, &flushed, nullptr, nullptr)) {
        // Flow lost its device. Let it go; switching smoke off and on starts a new one.
        m.stopFlow();
        m.flowFailed = true;
        m.published.state = RendererFeatureState::Unavailable;
        m.published.reason = "NVIDIA Flow's device was reset";
        BOO_LOG_WARN("PhysX", "Smoke unavailable: NVIDIA Flow's device was reset.");
        return;
    }
    ++m.published.steps;

    // The newest smoke the GPU has finished, a frame or two behind what was just submitted.
    const NvFlowUint64 completed = m.contextInterface.getLastFrameCompleted(m.context);
    for (NvFlowUint64 i = renderData.nanoVdb.readbackCount; i-- > 0u;) {
        const auto& back = renderData.nanoVdb.readbacks[i];
        if (completed < back.globalFrameCompleted || !back.smokeNanoVdbReadback || back.smokeNanoVdbReadbackSize < 1024u) continue;
        if (back.globalFrameCompleted != m.gridSerial) {
            const uint64_t words = back.smokeNanoVdbReadbackSize / 4u;
            m.grid.resize(static_cast<size_t>(words));
            std::memcpy(m.grid.data(), back.smokeNanoVdbReadback, static_cast<size_t>(words) * 4u);
            m.gridSerial = back.globalFrameCompleted;
            pnanovdb_buf_t buf = pnanovdb_make_buf(m.grid.data(), words);
            pnanovdb_grid_handle_t handle = {pnanovdb_address_null()};
            const pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, handle);
            m.activeVoxels = pnanovdb_tree_get_voxel_count(buf, tree);
            for (uint32_t axis = 0; axis < 3; ++axis) {
                m.boxMin[axis] = static_cast<float>(pnanovdb_grid_get_world_bbox(buf, handle, axis));
                m.boxMax[axis] = static_cast<float>(pnanovdb_grid_get_world_bbox(buf, handle, axis + 3u));
            }
            m.published.activeVoxels = m.activeVoxels;
            m.published.gridMegabytes = static_cast<float>(double(words) * 4.0 / (1024.0 * 1024.0));
        }
        break;
    }
    m.published.activeBlocks = m.loader.gridInterface.getActiveBlockCount(m.flowGrid);
    NvFlowDeviceMemoryStats memory{};
    m.loader.deviceInterface.getMemoryStats(m.flowDevice, &memory);
    m.published.flowMemoryMegabytes = static_cast<float>(double(memory.deviceMemoryBytes) / (1024.0 * 1024.0));
    m.published.simulateGpuMs = m.profiledGpuMs;
    const float cpuMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (m.published.steps % 120u == 1u) {
        char line[260]{};
        std::snprintf(line, sizeof(line), "Smoke step %llu: %u sources, %u blocks, %llu voxels, grid %.2f MB, box (%.0f %.0f %.0f)-(%.0f %.0f %.0f), first source (%.0f %.0f %.0f)",
            static_cast<unsigned long long>(m.published.steps), count, m.published.activeBlocks, static_cast<unsigned long long>(m.activeVoxels),
            m.published.gridMegabytes, m.boxMin[0], m.boxMin[1], m.boxMin[2], m.boxMax[0], m.boxMax[1], m.boxMax[2],
            count ? sources[0].position[0] : 0.f, count ? sources[0].position[1] : 0.f, count ? sources[0].position[2] : 0.f);
        BOO_LOG_DEBUG("PhysX", line);
    }
    m.published.simulateCpuMs = m.published.simulateCpuMs > 0.0f ? m.published.simulateCpuMs * 0.9f + cpuMs * 0.1f : cpuMs;
#endif
}

void FlowSmoke::record(void* commandList, void* sceneHdr, void* sceneDistance, uint32_t width, uint32_t height,
                       const M1CameraState& camera, const float lightDir[3], const float lightColor[3], const float ambient[3],
                       uint64_t frameIndex, uint32_t slot, bool canonical) {
    Impl& m = *impl_;
    if (!m.d3dReady || !commandList || !sceneHdr || !sceneDistance || width == 0 || height == 0) return;
    slot %= kSlots;
    if (canonical) m.collect(slot);
    if (m.grid.empty() || m.activeVoxels == 0u || m.published.state != RendererFeatureState::Active) return;
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    auto* hdr = static_cast<ID3D12Resource*>(sceneHdr);
    auto* distance = static_cast<ID3D12Resource*>(sceneDistance);

    // This frame's copy of the grid. A slot is only reused once the frame that read it has finished.
    Impl::GridSlot& gridSlot = m.gridSlots[slot];
    const uint64_t bytes = m.grid.size() * 4ull;
    if (gridSlot.capacityBytes < bytes) {
        const uint64_t capacity = std::max<uint64_t>(bytes * 3u / 2u, 4ull << 20);
        const auto uploadHeap = heapProps(D3D12_HEAP_TYPE_UPLOAD);
        const auto desc = bufferDesc(capacity);
        ComPtr<ID3D12Resource> buffer;
        void* mapped = nullptr;
        D3D12_RANGE noRead{0, 0};
        if (FAILED(m.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                     nullptr, IID_PPV_ARGS(&buffer))) ||
            FAILED(buffer->Map(0, &noRead, &mapped)))
            return;
        gridSlot.buffer = std::move(buffer);
        gridSlot.mapped = static_cast<uint8_t*>(mapped);
        gridSlot.capacityBytes = capacity;
        gridSlot.serial = 0;
    }
    if (gridSlot.serial != m.gridSerial) {
        if (!BOO_MAPPED_WRITE_OK(gridSlot.buffer.Get(), "Flow grid upload", 0, bytes)) return;
        std::memcpy(gridSlot.mapped, m.grid.data(), static_cast<size_t>(bytes));
        gridSlot.serial = m.gridSerial;
    }
    if (!m.bindTargets(hdr, distance)) return;

    SmokeConstants c{};
    float forward[3], right[3], up[3];
    for (int i = 0; i < 3; ++i) forward[i] = camera.target[i] - camera.eye[i];
    normalize3(forward);
    cross3(camera.up, forward, right);
    normalize3(right);
    cross3(forward, right, up);
    for (int i = 0; i < 3; ++i) {
        c.eye[i] = camera.eye[i];
        c.right[i] = right[i];
        c.up[i] = up[i];
        c.forward[i] = forward[i];
        c.lightDir[i] = lightDir[i];
        c.lightColor[i] = lightColor[i];
        c.ambient[i] = ambient[i];
        c.boxMin[i] = m.boxMin[i];
        c.boxMax[i] = m.boxMax[i];
    }
    normalize3(c.lightDir);
    c.tanHalfFov = std::tan(camera.fovYRadians * 0.5f);
    c.aspect = static_cast<float>(width) / static_cast<float>(height);
    c.extinction = 1.4f;
    c.stepMetres = kVoxelMetres * 0.7f;
    c.voxelMetres = kVoxelMetres;
    c.frame = static_cast<uint32_t>(frameIndex);
    c.width = width;
    c.height = height;
    c.shadowReach = 10.0f;

    ID3D12DescriptorHeap* heaps[] = {m.heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m.root.Get());
    cmd->SetPipelineState(m.pso.Get());
    cmd->SetComputeRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
    cmd->SetComputeRootDescriptorTable(1, m.heap->GetGPUDescriptorHandleForHeapStart());
    cmd->SetComputeRootShaderResourceView(2, gridSlot.buffer->GetGPUVirtualAddress());
    transition(cmd, hdr, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd, distance, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    const bool timed = canonical && m.queries && m.queryReadback;
    if (timed) cmd->EndQuery(m.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u);
    cmd->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1);
    if (!m.loggedRunning) {
        m.loggedRunning = true;
        BOO_LOG_DEBUG("PhysX", "Smoke ray march dispatched " + std::to_string(width) + "x" + std::to_string(height) +
                               ", eye (" + std::to_string(camera.eye[0]) + " " + std::to_string(camera.eye[1]) + " " + std::to_string(camera.eye[2]) + ")");
    }
    if (timed) {
        cmd->EndQuery(m.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u + 1u);
        cmd->ResolveQueryData(m.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u, 2u, m.queryReadback.Get(), slot * 16ull);
        m.timedSlot[slot] = true;
    }
    transition(cmd, distance, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(cmd, hdr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

} // namespace boo
