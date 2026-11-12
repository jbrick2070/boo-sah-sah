#include "boo/benchmark.hpp"
#include "boo/logging.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <vector>

namespace boo {

using Microsoft::WRL::ComPtr;

const char* toString(BenchCategory cat) noexcept {
    switch (cat) {
        case BenchCategory::RT:     return "RT";
        case BenchCategory::CUDA:   return "CUDA";
        case BenchCategory::Tensor: return "Tensor";
        case BenchCategory::NVENC:  return "NVENC";
    }
    return "Unknown";
}

namespace {

std::string nowIso8601() {
    using namespace std::chrono;
    const std::time_t tt = system_clock::to_time_t(system_clock::now());
    std::tm tmLocal{};
    localtime_s(&tmLocal, &tt);
    char buf[32]{};
    std::snprintf(
        buf,
        sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d",
        tmLocal.tm_year + 1900,
        tmLocal.tm_mon + 1,
        tmLocal.tm_mday,
        tmLocal.tm_hour,
        tmLocal.tm_min,
        tmLocal.tm_sec);
    return std::string(buf);
}

std::string wideToUtf8Local(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

bool loadCsoBytes(const char* relPath, std::vector<uint8_t>& outBytes) {
    const auto p = resolveResourcePath(relPath);
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const auto sz = ifs.tellg();
    if (sz <= 0) return false;
    outBytes.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    return static_cast<bool>(ifs.read(reinterpret_cast<char*>(outBytes.data()), sz));
}

struct BenchConstantsGpu {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t iteration = 0;
    uint32_t particleCount = 1000000;
    float time = 1.0f;
    float param0 = 0.0f;
    float param1 = 0.0f;
    float param2 = 0.0f;
};

struct D3D12BenchHelper {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList4> cmd4;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    uint64_t fenceVal = 1;
    ComPtr<ID3D12QueryHeap> tsHeap;
    ComPtr<ID3D12Resource> tsReadback;
    uint64_t tsFreq = 0;

    bool init(ID3D12Device* d, ID3D12CommandQueue* q) {
        dev = d;
        queue = q;
        if (!dev || !queue) return false;
        queue->GetTimestampFrequency(&tsFreq);
        if (tsFreq == 0) return false;

        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
        ComPtr<ID3D12GraphicsCommandList> baseCmd;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&baseCmd)))) return false;
        baseCmd->Close();
        if (FAILED(baseCmd.As(&cmd4))) return false;

        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) return false;

        D3D12_QUERY_HEAP_DESC qhd{};
        qhd.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qhd.Count = 2;
        if (FAILED(dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&tsHeap)))) return false;

        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(uint64_t) * 2;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tsReadback)));
    }

    ~D3D12BenchHelper() {
        if (fenceEvent) CloseHandle(fenceEvent);
    }

    bool submitAndWait() {
        if (FAILED(cmd4->Close())) return false;
        ID3D12CommandList* lists[] = {cmd4.Get()};
        queue->ExecuteCommandLists(1, lists);
        const uint64_t v = fenceVal++;
        if (FAILED(queue->Signal(fence.Get(), v))) return false;
        if (fence->GetCompletedValue() < v) {
            if (FAILED(fence->SetEventOnCompletion(v, fenceEvent))) return false;
            while (WaitForSingleObjectEx(fenceEvent, 100, FALSE) == WAIT_TIMEOUT) {
                if (FAILED(dev->GetDeviceRemovedReason())) return false;
            }
        }
        return SUCCEEDED(dev->GetDeviceRemovedReason()) && fence->GetCompletedValue() >= v;
    }

    double readGpuElapsedMs() {
        uint64_t* mapped = nullptr;
        D3D12_RANGE rr{0, sizeof(uint64_t) * 2};
        if (FAILED(tsReadback->Map(0, &rr, reinterpret_cast<void**>(&mapped))) || !mapped) return 0.0;
        const uint64_t t0 = mapped[0];
        const uint64_t t1 = mapped[1];
        D3D12_RANGE wr{0, 0};
        tsReadback->Unmap(0, &wr);
        return (t1 >= t0) ? (static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(tsFreq)) : 0.0;
    }

    ComPtr<ID3D12Resource> createBuffer(
        uint64_t bytes,
        D3D12_HEAP_TYPE heapType,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initState) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heapType;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = std::max<uint64_t>(256, bytes);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ComPtr<ID3D12Resource> res;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initState, nullptr, IID_PPV_ARGS(&res));
        return res;
    }
};

// =============================================================================
// 1A. Hardware DXR 1.1 Inline RayQuery Benchmark
// =============================================================================
BenchResultEntry runDxrHardwareBench(
    D3D12BenchHelper& h,
    const BenchmarkConfig& cfg,
    size_t stepIdx,
    size_t totalSteps,
    const BenchmarkProgressCallback& cb) {
    BenchResultEntry out{};
    out.category = BenchCategory::RT;
    out.id = "rt_dxr_1_1_rayquery";
    out.title = "DXR 1.1 Hardware Raytracing (64-Crystal Glass Orchid Lattice)";
    out.backendName = "D3D12 DXR Tier 1.1 (RayQuery BVH)";
    out.primaryUnit = "MRays/s";
    out.secondaryUnit = "1080p FPS";
    out.referenceTarget = 25000.0; // 25 GRays/s full-bar reference

    if (cfg.disableRT) {
        out.status = BackendStatus::Unavailable;
        out.notes = "Skipped (--disable-rt)";
        return out;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    if (FAILED(h.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5))) ||
        opt5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
        out.status = BackendStatus::Unavailable;
        out.notes = "Adapter does not support DXR Tier 1.1 inline RayQuery.";
        return out;
    }
    if (static_cast<uint32_t>(opt5.RaytracingTier) >= 12u) {
        out.backendName = "D3D12 DXR Tier 1.2 (RayQuery BVH)";
    }

    ComPtr<ID3D12Device5> dev5;
    if (FAILED(h.dev->QueryInterface(IID_PPV_ARGS(&dev5)))) {
        out.status = BackendStatus::Unavailable;
        out.notes = "ID3D12Device5 unavailable.";
        return out;
    }

    std::vector<uint8_t> csBytes;
    if (!loadCsoBytes("assets/shaders/m0_dxr_bench.cso", csBytes)) {
        out.status = BackendStatus::Faulted;
        out.notes = "Missing m0_dxr_bench.cso";
        return out;
    }

    const auto tColdStart = std::chrono::steady_clock::now();

    // Build octahedral crystal geometry (6 vertices, 8 triangles = 24 uint16 indices)
    const float verts[6][3] = {
        { 0.0f,  0.65f,  0.0f},
        { 0.0f, -0.65f,  0.0f},
        { 0.55f, 0.0f,   0.0f},
        {-0.55f, 0.0f,   0.0f},
        { 0.0f,  0.0f,   0.55f},
        { 0.0f,  0.0f,  -0.55f}
    };
    const uint16_t indices[24] = {
        0, 4, 2,  0, 2, 5,  0, 5, 3,  0, 3, 4,
        1, 2, 4,  1, 5, 2,  1, 3, 5,  1, 4, 3
    };

    auto vbUpload = h.createBuffer(sizeof(verts), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto ibUpload = h.createBuffer(sizeof(indices), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!vbUpload || !ibUpload) {
        out.status = BackendStatus::Faulted;
        out.notes = "Failed to allocate BLAS geometry buffers.";
        return out;
    }
    void* ptr = nullptr;
    vbUpload->Map(0, nullptr, &ptr); std::memcpy(ptr, verts, sizeof(verts)); vbUpload->Unmap(0, nullptr);
    ibUpload->Map(0, nullptr, &ptr); std::memcpy(ptr, indices, sizeof(indices)); ibUpload->Unmap(0, nullptr);

    D3D12_RAYTRACING_GEOMETRY_DESC geomDesc{};
    geomDesc.Type  = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geomDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geomDesc.Triangles.VertexBuffer.StartAddress  = vbUpload->GetGPUVirtualAddress();
    geomDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
    geomDesc.Triangles.VertexCount                = 6;
    geomDesc.Triangles.VertexFormat               = DXGI_FORMAT_R32G32B32_FLOAT;
    geomDesc.Triangles.IndexBuffer                = ibUpload->GetGPUVirtualAddress();
    geomDesc.Triangles.IndexCount                 = 24;
    geomDesc.Triangles.IndexFormat                = DXGI_FORMAT_R16_UINT;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs{};
    blasInputs.Type           = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blasInputs.Flags          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    blasInputs.NumDescs       = 1;
    blasInputs.DescsLayout    = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blasInputs.pGeometryDescs = &geomDesc;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasPrebuild{};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &blasPrebuild);

    auto blasScratch = h.createBuffer(blasPrebuild.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    auto blasResult  = h.createBuffer(blasPrebuild.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

    constexpr uint32_t kNumInstances = 64; // 8x8 Glass Orchid crystal lattice
    auto instUpload = h.createBuffer(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * kNumInstances, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_RAYTRACING_INSTANCE_DESC* instMapped = nullptr;
    instUpload->Map(0, nullptr, reinterpret_cast<void**>(&instMapped));
    for (uint32_t i = 0; i < kNumInstances; ++i) {
        auto& inst = instMapped[i];
        std::memset(&inst, 0, sizeof(inst));
        const float gx = (static_cast<float>(i % 8) - 3.5f) * 1.15f;
        const float gy = (static_cast<float>(i / 8) - 3.5f) * 0.85f;
        const float gz = std::sin(static_cast<float>(i) * 0.7f) * 1.2f;
        inst.Transform[0][0] = 1.0f; inst.Transform[0][3] = gx;
        inst.Transform[1][1] = 1.0f; inst.Transform[1][3] = gy;
        inst.Transform[2][2] = 1.0f; inst.Transform[2][3] = gz;
        inst.InstanceID = i;
        inst.InstanceMask = 0xFF;
        inst.AccelerationStructure = blasResult->GetGPUVirtualAddress();
    }
    instUpload->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
    tlasInputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasInputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlasInputs.NumDescs      = kNumInstances;
    tlasInputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasInputs.InstanceDescs = instUpload->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuild{};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasPrebuild);

    auto tlasScratch = h.createBuffer(tlasPrebuild.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    auto tlasResult  = h.createBuffer(tlasPrebuild.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

    // Execute BLAS + TLAS build on GPU
    h.alloc->Reset();
    h.cmd4->Reset(h.alloc.Get(), nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasBuild{};
    blasBuild.Inputs = blasInputs;
    blasBuild.ScratchAccelerationStructureData = blasScratch->GetGPUVirtualAddress();
    blasBuild.DestAccelerationStructureData    = blasResult->GetGPUVirtualAddress();
    h.cmd4->BuildRaytracingAccelerationStructure(&blasBuild, 0, nullptr);

    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = blasResult.Get();
    h.cmd4->ResourceBarrier(1, &uavBarrier);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasBuild{};
    tlasBuild.Inputs = tlasInputs;
    tlasBuild.ScratchAccelerationStructureData = tlasScratch->GetGPUVirtualAddress();
    tlasBuild.DestAccelerationStructureData    = tlasResult->GetGPUVirtualAddress();
    h.cmd4->BuildRaytracingAccelerationStructure(&tlasBuild, 0, nullptr);

    uavBarrier.UAV.pResource = tlasResult.Get();
    h.cmd4->ResourceBarrier(1, &uavBarrier);
    if (!h.submitAndWait()) {
        out.status = BackendStatus::Faulted;
        out.notes = "BLAS/TLAS build submission failed.";
        return out;
    }

    // Root signature: b0 (8 constants), t0 (Root SRV TLAS), u0 (Root UAV HitSummary)
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 8};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor = {0, 0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {0, 0};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sigBlob, errBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob))) {
        out.status = BackendStatus::Faulted;
        return out;
    }
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    if (FAILED(h.dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig)))) {
        out.status = BackendStatus::Faulted;
        return out;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpso{};
    cpso.pRootSignature = rootSig.Get();
    cpso.CS = {csBytes.data(), csBytes.size()};
    if (FAILED(h.dev->CreateComputePipelineState(&cpso, IID_PPV_ARGS(&pso)))) {
        out.status = BackendStatus::Faulted;
        out.notes = "CreateComputePipelineState(DXR RayQuery) failed.";
        return out;
    }

    auto hitGpu = h.createBuffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    auto hitReadback = h.createBuffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

    const auto tColdEnd = std::chrono::steady_clock::now();
    out.coldInitMs = std::chrono::duration<double, std::milli>(tColdEnd - tColdStart).count();

    constexpr uint32_t kW = 1920, kH = 1080;
    constexpr double kRaysPerPassMillion = (static_cast<double>(kW) * static_cast<double>(kH) * 4.0) / 1e6; // 8.2944 MRays

    std::vector<double> samplesMs;
    samplesMs.reserve(cfg.timedIterations);
    const uint32_t totalPasses = cfg.warmupIterations + cfg.timedIterations;

    for (uint32_t p = 0; p < totalPasses && !cfg.cancelled(); ++p) {
        BenchConstantsGpu bc{kW, kH, p, 0, 1.25f + static_cast<float>(p) * 0.02f, 0, 0, 0};

        h.alloc->Reset();
        h.cmd4->Reset(h.alloc.Get(), pso.Get());
        h.cmd4->SetComputeRootSignature(rootSig.Get());
        h.cmd4->SetComputeRoot32BitConstants(0, 8, &bc, 0);
        h.cmd4->SetComputeRootShaderResourceView(1, tlasResult->GetGPUVirtualAddress());
        h.cmd4->SetComputeRootUnorderedAccessView(2, hitGpu->GetGPUVirtualAddress());

        h.cmd4->EndQuery(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        h.cmd4->Dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = hitGpu.Get();
        h.cmd4->ResourceBarrier(1, &ub);
        h.cmd4->EndQuery(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        h.cmd4->ResolveQueryData(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, h.tsReadback.Get(), 0);

        if (p + 1 == totalPasses) {
            D3D12_RESOURCE_BARRIER toCopy{};
            toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy.Transition = {hitGpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
            h.cmd4->ResourceBarrier(1, &toCopy);
            h.cmd4->CopyBufferRegion(hitReadback.Get(), 0, hitGpu.Get(), 0, 64);
        }

        if (!h.submitAndWait()) break;
        const double ms = h.readGpuElapsedMs();
        if (p >= cfg.warmupIterations && ms > 0.0) {
            samplesMs.push_back(ms);
        }
        if (cb && ((p & 3u) == 0u || p + 1 == totalPasses)) {
            const float stg = static_cast<float>(p + 1) / static_cast<float>(totalPasses);
            cb(BenchmarkProgressEvent{
                stepIdx, totalSteps,
                (static_cast<float>(stepIdx) + stg) / static_cast<float>(totalSteps),
                stg, out.title,
                (p < cfg.warmupIterations) ? "WARMUP" : "SAMPLING",
                (ms > 0.0) ? (kRaysPerPassMillion / (ms * 0.001)) : 0.0,
                "MRays/s"});
        }
    }

    uint32_t hitSummary[2]{0, 0};
    D3D12_RANGE rr{0, sizeof(hitSummary)};
    uint32_t* mappedHits = nullptr;
    if (SUCCEEDED(hitReadback->Map(0, &rr, reinterpret_cast<void**>(&mappedHits))) && mappedHits) {
        hitSummary[0] = mappedHits[0];
        hitSummary[1] = mappedHits[1];
        D3D12_RANGE wr{0, 0};
        hitReadback->Unmap(0, &wr);
    }

    out.timingMs = computeFrametimeStats(samplesMs);
    out.executed = !samplesMs.empty();
    out.outputVerified = out.executed && (hitSummary[0] > 0);
    out.status = out.outputVerified ? BackendStatus::Accelerated : BackendStatus::Faulted;
    if (out.timingMs.p50Ms > 0.0) {
        out.primaryScore   = kRaysPerPassMillion / (out.timingMs.p50Ms * 0.001);
        out.secondaryScore = 1000.0 / out.timingMs.p50Ms;
    }
    out.verificationDetail = "BVH triangle intersections verified (sampled hit counter = " + std::to_string(hitSummary[0]) + ")";
    out.notes = "8.29M rays/pass (1920x1080 x 4 rays/px) against 64-instance TLAS (512 tris). Cold AS build: " +
                std::to_string(out.coldInitMs).substr(0, 5) + " ms.";
    return out;
}

// =============================================================================
// 1B. Analytic HLSL RT Substitute Benchmark (Fallback Comparison)
// =============================================================================
BenchResultEntry runRtAnalyticFallbackBench(
    D3D12BenchHelper& h,
    const BenchmarkConfig& cfg,
    size_t stepIdx,
    size_t totalSteps,
    const BenchmarkProgressCallback& cb) {
    BenchResultEntry out{};
    out.category = BenchCategory::RT;
    out.id = "rt_hlsl_analytic_fallback";
    out.title = "Analytic Reflection Shader (D3D12 Fallback -- NOT DXR)";
    out.backendName = "D3D12 SM 6.0 Compute (Substitute)";
    out.isFallbackComparison = true;
    out.primaryUnit = "MRays/s";
    out.secondaryUnit = "1080p FPS";
    out.referenceTarget = 25000.0;

    std::vector<uint8_t> csBytes;
    if (!loadCsoBytes("assets/shaders/m0_rt_fallback_bench.cso", csBytes)) {
        out.status = BackendStatus::Faulted;
        return out;
    }

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 8};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {0, 0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sigBlob, errBlob;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    h.dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpso{};
    cpso.pRootSignature = rootSig.Get();
    cpso.CS = {csBytes.data(), csBytes.size()};
    if (FAILED(h.dev->CreateComputePipelineState(&cpso, IID_PPV_ARGS(&pso)))) return out;

    auto hitGpu = h.createBuffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    auto hitRead = h.createBuffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

    constexpr uint32_t kW = 1920, kH = 1080;
    constexpr double kEquivRaysMillion = (static_cast<double>(kW) * static_cast<double>(kH) * 4.0) / 1e6; // 8.2944 MRays/pass (4 rays/px x 64 crystals)

    std::vector<double> samplesMs;
    const uint32_t totalPasses = cfg.warmupIterations + cfg.timedIterations;
    for (uint32_t p = 0; p < totalPasses && !cfg.cancelled(); ++p) {
        BenchConstantsGpu bc{kW, kH, p, 0, 1.0f + static_cast<float>(p) * 0.03f, 0, 0, 0};
        h.alloc->Reset();
        h.cmd4->Reset(h.alloc.Get(), pso.Get());
        h.cmd4->SetComputeRootSignature(rootSig.Get());
        h.cmd4->SetComputeRoot32BitConstants(0, 8, &bc, 0);
        h.cmd4->SetComputeRootUnorderedAccessView(1, hitGpu->GetGPUVirtualAddress());
        h.cmd4->EndQuery(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        h.cmd4->Dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = hitGpu.Get();
        h.cmd4->ResourceBarrier(1, &ub);
        h.cmd4->EndQuery(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        h.cmd4->ResolveQueryData(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, h.tsReadback.Get(), 0);
        if (p + 1 == totalPasses) {
            D3D12_RESOURCE_BARRIER toCopy{};
            toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy.Transition = {hitGpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
            h.cmd4->ResourceBarrier(1, &toCopy);
            h.cmd4->CopyBufferRegion(hitRead.Get(), 0, hitGpu.Get(), 0, 32);
        }
        if (!h.submitAndWait()) break;
        const double ms = h.readGpuElapsedMs();
        if (p >= cfg.warmupIterations && ms > 0.0) samplesMs.push_back(ms);
        if (cb && ((p & 7u) == 0u || p + 1 == totalPasses)) {
            const float stg = static_cast<float>(p + 1) / static_cast<float>(totalPasses);
            cb(BenchmarkProgressEvent{stepIdx, totalSteps, (static_cast<float>(stepIdx) + stg) / static_cast<float>(totalSteps), stg, out.title, "SAMPLING", (ms > 0.0) ? (kEquivRaysMillion / (ms * 0.001)) : 0.0, "MRays/s"});
        }
    }

    uint32_t hits = 0;
    uint32_t* mapped = nullptr;
    D3D12_RANGE rr{0, sizeof(uint32_t)};
    if (SUCCEEDED(hitRead->Map(0, &rr, reinterpret_cast<void**>(&mapped))) && mapped) {
        hits = mapped[0];
        D3D12_RANGE wr{0, 0};
        hitRead->Unmap(0, &wr);
    }

    out.timingMs = computeFrametimeStats(samplesMs);
    out.executed = !samplesMs.empty();
    out.outputVerified = out.executed && (hits > 0);
    out.status = out.outputVerified ? BackendStatus::Substituted : BackendStatus::Faulted;
    if (out.timingMs.p50Ms > 0.0) {
        out.primaryScore = kEquivRaysMillion / (out.timingMs.p50Ms * 0.001);
        out.secondaryScore = 1000.0 / out.timingMs.p50Ms;
    }
    out.verificationDetail = "Analytic 64-crystal ray-sphere intersections verified (sampled hits = " + std::to_string(hits) + ")";
    out.notes = "Brute-force 64-crystal x 4 rays/px HLSL stand-in without BVH hardware (labeled Substitute, never DXR).";
    return out;
}

// =============================================================================
// Dynamic CUDA Driver API Loader + PTX Kernels (Zero CUDA SDK Build Dependency!)
// =============================================================================
struct DynamicCudaApi {
    HMODULE lib = nullptr;
    int (*cuInit)(unsigned int) = nullptr;
    int (*cuDeviceGetCount)(int*) = nullptr;
    int (*cuDeviceGet)(int*, int) = nullptr;
    int (*cuDeviceGetName)(char*, int, int) = nullptr;
    int (*cuDeviceGetAttribute)(int*, int, int) = nullptr;
    int (*cuDeviceGetLuid)(char*, unsigned int*, int) = nullptr;
    int (*cuDevicePrimaryCtxRetain)(void**, int) = nullptr;
    int (*cuDevicePrimaryCtxRelease)(int) = nullptr;
    int (*cuCtxSetCurrent)(void*) = nullptr;
    int (*cuCtxSynchronize)() = nullptr;
    int (*cuModuleLoadDataEx)(void**, const void*, unsigned int, void*, void**) = nullptr;
    int (*cuModuleUnload)(void*) = nullptr;
    int (*cuModuleGetFunction)(void**, void*, const char*) = nullptr;
    int (*cuMemAlloc)(uint64_t*, size_t) = nullptr;
    int (*cuMemFree)(uint64_t) = nullptr;
    int (*cuMemcpyHtoD)(uint64_t, const void*, size_t) = nullptr;
    int (*cuMemcpyDtoH)(void*, uint64_t, size_t) = nullptr;
    int (*cuLaunchKernel)(void*, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, void*, void**, void**) = nullptr;
    int (*cuEventCreate)(void**, unsigned int) = nullptr;
    int (*cuEventDestroy)(void*) = nullptr;
    int (*cuEventRecord)(void*, void*) = nullptr;
    int (*cuEventSynchronize)(void*) = nullptr;
    int (*cuEventElapsedTime)(float*, void*, void*) = nullptr;

    bool load() {
        lib = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib) return false;
        auto sym = [&](auto& fnPtr, const char* name, const char* alt = nullptr) -> bool {
            void* p = reinterpret_cast<void*>(GetProcAddress(lib, name));
            if (!p && alt) p = reinterpret_cast<void*>(GetProcAddress(lib, alt));
            fnPtr = reinterpret_cast<std::decay_t<decltype(fnPtr)>>(p);
            return fnPtr != nullptr;
        };
        return sym(cuInit, "cuInit") &&
               sym(cuDeviceGetCount, "cuDeviceGetCount") &&
               sym(cuDeviceGet, "cuDeviceGet") &&
               sym(cuDeviceGetName, "cuDeviceGetName") &&
               sym(cuDeviceGetAttribute, "cuDeviceGetAttribute") &&
               sym(cuDeviceGetLuid, "cuDeviceGetLuid") &&
               sym(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain") &&
               sym(cuDevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2", "cuDevicePrimaryCtxRelease") &&
               sym(cuCtxSetCurrent, "cuCtxSetCurrent") &&
               sym(cuCtxSynchronize, "cuCtxSynchronize") &&
               sym(cuModuleLoadDataEx, "cuModuleLoadDataEx") &&
               sym(cuModuleUnload, "cuModuleUnload") &&
               sym(cuModuleGetFunction, "cuModuleGetFunction") &&
               sym(cuMemAlloc, "cuMemAlloc_v2", "cuMemAlloc") &&
               sym(cuMemFree, "cuMemFree_v2", "cuMemFree") &&
               sym(cuMemcpyHtoD, "cuMemcpyHtoD_v2", "cuMemcpyHtoD") &&
               sym(cuMemcpyDtoH, "cuMemcpyDtoH_v2", "cuMemcpyDtoH") &&
               sym(cuLaunchKernel, "cuLaunchKernel") &&
               sym(cuEventCreate, "cuEventCreate") &&
               sym(cuEventDestroy, "cuEventDestroy_v2", "cuEventDestroy") &&
               sym(cuEventRecord, "cuEventRecord") &&
               sym(cuEventSynchronize, "cuEventSynchronize") &&
               sym(cuEventElapsedTime, "cuEventElapsedTime");
    }

    ~DynamicCudaApi() {
        if (lib) FreeLibrary(lib);
    }

    int selectMatchingDevice(int32_t luidLow, int32_t luidHigh, std::string& outName, int& outMajor, int& outMinor, int& outSmCount) {
        if (cuInit(0) != 0) return -1;
        int count = 0;
        if (cuDeviceGetCount(&count) != 0 || count <= 0) return -1;
        int chosen = 0;
        for (int i = 0; i < count; ++i) {
            int d = 0;
            if (cuDeviceGet(&d, i) == 0) {
                LUID luid{};
                unsigned int mask = 0;
                if (cuDeviceGetLuid(reinterpret_cast<char*>(&luid), &mask, d) == 0) {
                    if (luid.LowPart == static_cast<DWORD>(luidLow) && luid.HighPart == luidHigh) {
                        chosen = i;
                        break;
                    }
                }
            }
        }
        int dev = -1;
        if (cuDeviceGet(&dev, chosen) != 0) return -1;
        char nbuf[128]{};
        cuDeviceGetName(nbuf, sizeof(nbuf), dev);
        outName = nbuf;
        cuDeviceGetAttribute(&outSmCount, 16, dev);
        cuDeviceGetAttribute(&outMajor, 75, dev);
        cuDeviceGetAttribute(&outMinor, 76, dev);
        return dev;
    }
};

// =============================================================================
// 2A. CUDA Driver PTX 1M-Particle Mutiny Benchmark
// =============================================================================
BenchResultEntry runCudaParticlesBench(
    int32_t luidLow,
    int32_t luidHigh,
    const BenchmarkConfig& cfg,
    size_t stepIdx,
    size_t totalSteps,
    const BenchmarkProgressCallback& cb) {
    BenchResultEntry out{};
    out.category = BenchCategory::CUDA;
    out.id = "cuda_driver_ptx_1m_particles";
    out.title = "CUDA Particle Mutiny (1,000,000 Particles Curl-Vortex SoA)";
    out.backendName = "NVIDIA CUDA Driver API (PTX JIT)";
    out.primaryUnit = "MPart/s";
    out.secondaryUnit = "GB/s";
    out.referenceTarget = 50000.0; // 50 Billion particle updates/sec full-bar reference

    if (cfg.disableCUDA) {
        out.status = BackendStatus::Unavailable;
        out.notes = "Skipped (--disable-cuda)";
        return out;
    }

    DynamicCudaApi cuda;
    if (!cuda.load()) {
        out.status = BackendStatus::Unavailable;
        out.notes = "nvcuda.dll not present on this machine.";
        return out;
    }

    const auto tCold0 = std::chrono::steady_clock::now();
    std::string gpuName;
    int ccMaj = 0, ccMin = 0, smCount = 0;
    const int dev = cuda.selectMatchingDevice(luidLow, luidHigh, gpuName, ccMaj, ccMin, smCount);
    if (dev < 0) {
        out.status = BackendStatus::Unavailable;
        out.notes = "No CUDA device available.";
        return out;
    }
    out.backendName = "CUDA Driver PTX (" + gpuName + " SM " + std::to_string(ccMaj) + "." + std::to_string(ccMin) + ")";

    void* ctx = nullptr;
    if (cuda.cuDevicePrimaryCtxRetain(&ctx, dev) != 0 || cuda.cuCtxSetCurrent(ctx) != 0) {
        out.status = BackendStatus::Faulted;
        out.notes = "cuDevicePrimaryCtxRetain failed.";
        return out;
    }

    static const char* kParticlePtx = R"PTX(
.version 7.0
.target sm_75
.address_size 64

.visible .entry boo_cuda_particles(
    .param .u64 p_pos,
    .param .u64 p_vel,
    .param .u32 p_count,
    .param .f32 p_time
)
{
    .reg .pred %p<3>;
    .reg .b32  %r<7>;
    .reg .b64  %rd<6>;
    .reg .f32  %f<18>;

    mov.u32 %r0, %ctaid.x;
    mov.u32 %r1, %ntid.x;
    mov.u32 %r2, %tid.x;
    mad.lo.u32 %r3, %r0, %r1, %r2;
    ld.param.u32 %r4, [p_count];
    setp.ge.u32 %p1, %r3, %r4;
    @%p1 bra DONE;

    ld.param.u64 %rd0, [p_pos];
    ld.param.u64 %rd1, [p_vel];
    ld.param.f32 %f0,  [p_time];

    mul.wide.u32 %rd2, %r3, 16;
    add.u64 %rd3, %rd0, %rd2;
    add.u64 %rd4, %rd1, %rd2;

    ld.global.v4.f32 {%f1, %f2, %f3, %f4}, [%rd3];
    ld.global.v4.f32 {%f5, %f6, %f7, %f8}, [%rd4];

    mov.f32 %f9,  0f3F7DF3B6;
    mov.f32 %f10, 0f3D3851EC;
    mov.f32 %f11, 0f3B088889;
    mov.f32 %f12, 0f3C888889;

    mov.u32 %r5, 0;
STEP_LOOP:
    setp.ge.u32 %p2, %r5, 8;
    @%p2 bra WRITE_BACK;

    sin.approx.f32 %f13, %f2;
    cos.approx.f32 %f14, %f3;
    sub.f32 %f15, %f13, %f14;
    mul.f32 %f5, %f5, %f9;
    fma.rn.f32 %f5, %f15, %f10, %f5;

    sin.approx.f32 %f13, %f3;
    cos.approx.f32 %f14, %f1;
    sub.f32 %f15, %f13, %f14;
    mul.f32 %f6, %f6, %f9;
    fma.rn.f32 %f6, %f15, %f10, %f6;

    sin.approx.f32 %f13, %f1;
    cos.approx.f32 %f14, %f2;
    sub.f32 %f15, %f13, %f14;
    mul.f32 %f7, %f7, %f9;
    fma.rn.f32 %f7, %f15, %f10, %f7;

    fma.rn.f32 %f1, %f5, %f11, %f1;
    fma.rn.f32 %f2, %f6, %f11, %f2;
    fma.rn.f32 %f3, %f7, %f11, %f3;

    add.u32 %r5, %r5, 1;
    bra STEP_LOOP;

WRITE_BACK:
    add.f32 %f4, %f4, %f12;
    st.global.v4.f32 [%rd3], {%f1, %f2, %f3, %f4};
    st.global.v4.f32 [%rd4], {%f5, %f6, %f7, %f8};
DONE:
    ret;
}
)PTX";

    void* mod = nullptr;
    void* fn  = nullptr;
    if (cuda.cuModuleLoadDataEx(&mod, kParticlePtx, 0, nullptr, nullptr) != 0 ||
        cuda.cuModuleGetFunction(&fn, mod, "boo_cuda_particles") != 0) {
        cuda.cuDevicePrimaryCtxRelease(dev);
        out.status = BackendStatus::Faulted;
        out.notes = "PTX JIT compile failed for boo_cuda_particles.";
        return out;
    }

    constexpr uint32_t kParticles = 1000000;
    const size_t bufBytes = sizeof(float) * 4 * kParticles; // 16 MB each (64 MB RW traffic/pass)
    std::vector<float> hostInit(kParticles * 4);
    for (uint32_t i = 0; i < kParticles; ++i) {
        const float fi = static_cast<float>(i) * 0.001f;
        hostInit[i * 4 + 0] = std::sin(fi) * 2.0f;
        hostInit[i * 4 + 1] = std::cos(fi) * 2.0f;
        hostInit[i * 4 + 2] = (static_cast<float>(i % 1000) - 500.0f) * 0.005f;
        hostInit[i * 4 + 3] = 0.0f;
    }

    uint64_t dPos = 0, dVel = 0;
    cuda.cuMemAlloc(&dPos, bufBytes);
    cuda.cuMemAlloc(&dVel, bufBytes);
    cuda.cuMemcpyHtoD(dPos, hostInit.data(), bufBytes);
    std::fill(hostInit.begin(), hostInit.end(), 0.1f);
    cuda.cuMemcpyHtoD(dVel, hostInit.data(), bufBytes);

    void* evStart = nullptr;
    void* evEnd   = nullptr;
    cuda.cuEventCreate(&evStart, 0);
    cuda.cuEventCreate(&evEnd, 0);

    out.coldInitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tCold0).count();

    std::vector<double> samplesMs;
    const uint32_t totalPasses = cfg.warmupIterations + cfg.timedIterations;
    const unsigned int blockDim = 256;
    const unsigned int gridDim  = (kParticles + blockDim - 1) / blockDim;

    for (uint32_t p = 0; p < totalPasses && !cfg.cancelled(); ++p) {
        uint32_t cnt = kParticles;
        float tm = 0.5f + static_cast<float>(p) * 0.016f;
        void* args[] = {&dPos, &dVel, &cnt, &tm};

        cuda.cuEventRecord(evStart, nullptr);
        cuda.cuLaunchKernel(fn, gridDim, 1, 1, blockDim, 1, 1, 0, nullptr, args, nullptr);
        cuda.cuEventRecord(evEnd, nullptr);
        cuda.cuEventSynchronize(evEnd);

        float ms = 0.0f;
        cuda.cuEventElapsedTime(&ms, evStart, evEnd);
        if (p >= cfg.warmupIterations && ms > 0.0f) {
            samplesMs.push_back(static_cast<double>(ms));
        }
        if (cb && ((p & 3u) == 0u || p + 1 == totalPasses)) {
            const float stg = static_cast<float>(p + 1) / static_cast<float>(totalPasses);
            cb(BenchmarkProgressEvent{
                stepIdx, totalSteps,
                (static_cast<float>(stepIdx) + stg) / static_cast<float>(totalSteps),
                stg, out.title,
                (p < cfg.warmupIterations) ? "WARMUP" : "SAMPLING",
                (ms > 0.0f) ? (1.0 / (static_cast<double>(ms) * 0.001)) : 0.0,
                "MPart/s"});
        }
    }

    float verifySlice[16]{};
    cuda.cuMemcpyDtoH(verifySlice, dPos, sizeof(verifySlice));

    cuda.cuEventDestroy(evStart);
    cuda.cuEventDestroy(evEnd);
    cuda.cuMemFree(dPos);
    cuda.cuMemFree(dVel);
    cuda.cuModuleUnload(mod);
    cuda.cuDevicePrimaryCtxRelease(dev);

    const float expectedAge = static_cast<float>(totalPasses) * 0.0166667f;
    const bool ageValid = std::isfinite(verifySlice[0]) && (std::abs(verifySlice[3] - expectedAge) < 0.01f);

    out.timingMs = computeFrametimeStats(samplesMs);
    out.executed = !samplesMs.empty();
    out.outputVerified = out.executed && ageValid;
    out.status = out.outputVerified ? BackendStatus::Accelerated : BackendStatus::Faulted;
    if (out.timingMs.p50Ms > 0.0) {
        const double sec = out.timingMs.p50Ms * 0.001;
        out.primaryScore   = 1.0 / sec;   // 1.0 Million particles / sec -> MPart/s
        out.secondaryScore = 0.064 / sec; // 64 MB RW traffic / pass -> GB/s
    }
    out.verificationDetail = "Particle lifetime & curl displacement verified (p0.w = " + std::to_string(verifySlice[3]).substr(0, 6) + "s)";
    out.notes = "1,000,000 SoA particles x 8 curl substeps/pass (" + std::to_string(smCount) + " SMs, 64 MB RW/pass).";
    return out;
}

// =============================================================================
// 2B. D3D12 HLSL Compute 1M-Particle Substitute Benchmark
// =============================================================================
BenchResultEntry runHlslParticlesFallbackBench(
    D3D12BenchHelper& h,
    const BenchmarkConfig& cfg,
    size_t stepIdx,
    size_t totalSteps,
    const BenchmarkProgressCallback& cb) {
    BenchResultEntry out{};
    out.category = BenchCategory::CUDA;
    out.id = "cuda_hlsl_compute_1m_particles_fallback";
    out.title = "HLSL Compute Particles (1,000,000 Particles -- NOT CUDA)";
    out.backendName = "D3D12 SM 6.0 Compute (Substitute)";
    out.isFallbackComparison = true;
    out.primaryUnit = "MPart/s";
    out.secondaryUnit = "GB/s";
    out.referenceTarget = 50000.0;

    std::vector<uint8_t> csBytes;
    if (!loadCsoBytes("assets/shaders/m0_particle_bench.cso", csBytes)) {
        out.status = BackendStatus::Faulted;
        return out;
    }

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 8};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {0, 0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {1, 0};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sigBlob, errBlob;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    h.dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpso{};
    cpso.pRootSignature = rootSig.Get();
    cpso.CS = {csBytes.data(), csBytes.size()};
    if (FAILED(h.dev->CreateComputePipelineState(&cpso, IID_PPV_ARGS(&pso)))) return out;

    constexpr uint32_t kParticles = 1000000;
    const uint64_t bufBytes = sizeof(float) * 4 * kParticles;
    auto posGpu = h.createBuffer(bufBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    auto velGpu = h.createBuffer(bufBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    auto readBuf = h.createBuffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

    std::vector<double> samplesMs;
    const uint32_t totalPasses = cfg.warmupIterations + cfg.timedIterations;
    for (uint32_t p = 0; p < totalPasses && !cfg.cancelled(); ++p) {
        BenchConstantsGpu bc{0, 0, p, kParticles, 0.5f + static_cast<float>(p) * 0.016f, 0, 0, 0};
        h.alloc->Reset();
        h.cmd4->Reset(h.alloc.Get(), pso.Get());
        h.cmd4->SetComputeRootSignature(rootSig.Get());
        h.cmd4->SetComputeRoot32BitConstants(0, 8, &bc, 0);
        h.cmd4->SetComputeRootUnorderedAccessView(1, posGpu->GetGPUVirtualAddress());
        h.cmd4->SetComputeRootUnorderedAccessView(2, velGpu->GetGPUVirtualAddress());
        h.cmd4->EndQuery(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        h.cmd4->Dispatch((kParticles + 255) / 256, 1, 1);
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = posGpu.Get();
        h.cmd4->ResourceBarrier(1, &ub);
        h.cmd4->EndQuery(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        h.cmd4->ResolveQueryData(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, h.tsReadback.Get(), 0);
        if (p + 1 == totalPasses) {
            D3D12_RESOURCE_BARRIER toCopy{};
            toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy.Transition = {posGpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
            h.cmd4->ResourceBarrier(1, &toCopy);
            h.cmd4->CopyBufferRegion(readBuf.Get(), 0, posGpu.Get(), 0, 64);
        }
        if (!h.submitAndWait()) break;
        const double ms = h.readGpuElapsedMs();
        if (p >= cfg.warmupIterations && ms > 0.0) samplesMs.push_back(ms);
        if (cb && ((p & 7u) == 0u || p + 1 == totalPasses)) {
            const float stg = static_cast<float>(p + 1) / static_cast<float>(totalPasses);
            cb(BenchmarkProgressEvent{stepIdx, totalSteps, (static_cast<float>(stepIdx) + stg) / static_cast<float>(totalSteps), stg, out.title, "SAMPLING", (ms > 0.0) ? (1.0 / (ms * 0.001)) : 0.0, "MPart/s"});
        }
    }

    float v4[4]{};
    float* mapped = nullptr;
    D3D12_RANGE rr{0, sizeof(v4)};
    if (SUCCEEDED(readBuf->Map(0, &rr, reinterpret_cast<void**>(&mapped))) && mapped) {
        std::memcpy(v4, mapped, sizeof(v4));
        D3D12_RANGE wr{0, 0};
        readBuf->Unmap(0, &wr);
    }

    out.timingMs = computeFrametimeStats(samplesMs);
    out.executed = !samplesMs.empty();
    out.outputVerified = out.executed && std::isfinite(v4[3]) && (v4[3] > 0.05f);
    out.status = out.outputVerified ? BackendStatus::Substituted : BackendStatus::Faulted;
    if (out.timingMs.p50Ms > 0.0) {
        const double sec = out.timingMs.p50Ms * 0.001;
        out.primaryScore   = 1.0 / sec;
        out.secondaryScore = 0.064 / sec;
    }
    out.verificationDetail = "HLSL UAV readback verified (p0.w = " + std::to_string(v4[3]).substr(0, 6) + "s)";
    out.notes = "Cross-vendor D3D12 HLSL Compute fallback (never labeled CUDA).";
    return out;
}

// =============================================================================
// 3A. Hardware Tensor Core Benchmark (PTX sm_80 mma.sync.m16n8k16.f32.f16.f16.f32)
// =============================================================================
BenchResultEntry runTensorMmaBench(
    int32_t luidLow,
    int32_t luidHigh,
    const BenchmarkConfig& cfg,
    size_t stepIdx,
    size_t totalSteps,
    const BenchmarkProgressCallback& cb) {
    BenchResultEntry out{};
    out.category = BenchCategory::Tensor;
    out.id = "tensor_ptx_mma_m16n8k16_fp16";
    out.title = "Tensor Core Matrix Math (FP16->FP32 mma.sync.m16n8k16)";
    out.backendName = "CUDA PTX mma.sync.m16n8k16";
    out.primaryUnit = "TFLOPS";
    out.secondaryUnit = "MLP FPS";
    out.referenceTarget = 120.0; // 120 FP16->FP32 Tensor TFLOPS full-bar reference

    if (cfg.disableTensor) {
        out.status = BackendStatus::Unavailable;
        out.notes = "Skipped (--disable-tensor)";
        return out;
    }

    DynamicCudaApi cuda;
    if (!cuda.load()) {
        out.status = BackendStatus::Unavailable;
        out.notes = "nvcuda.dll not present.";
        return out;
    }

    const auto tCold0 = std::chrono::steady_clock::now();
    std::string gpuName;
    int ccMaj = 0, ccMin = 0, smCount = 0;
    const int dev = cuda.selectMatchingDevice(luidLow, luidHigh, gpuName, ccMaj, ccMin, smCount);
    if (dev < 0 || ccMaj < 8) {
        out.status = BackendStatus::Unavailable;
        out.notes = "Requires SM 8.0+ NVIDIA Tensor Core GPU.";
        return out;
    }
    out.backendName = "PTX mma.sync.m16n8k16 (" + gpuName + " SM " + std::to_string(ccMaj) + "." + std::to_string(ccMin) + ")";

    void* ctx = nullptr;
    if (cuda.cuDevicePrimaryCtxRetain(&ctx, dev) != 0 || cuda.cuCtxSetCurrent(ctx) != 0) {
        out.status = BackendStatus::Faulted;
        return out;
    }

    static const char* kTensorMmaPtx = R"PTX(
.version 7.0
.target sm_80
.address_size 64

.visible .entry boo_tensor_mma_f16(
    .param .u64 p_out,
    .param .u32 p_loops,
    .param .u32 p_a_packed,
    .param .u32 p_b_packed
)
{
    .reg .pred %p<2>;
    .reg .b32  %r<12>;
    .reg .b64  %rd<4>;
    .reg .f32  %f<4>;

    ld.param.u64 %rd0, [p_out];
    ld.param.u32 %r8,  [p_loops];
    ld.param.u32 %r0,  [p_a_packed];
    ld.param.u32 %r4,  [p_b_packed];

    mov.b32 %r1, %r0;
    mov.b32 %r2, %r0;
    mov.b32 %r3, %r0;
    mov.b32 %r5, %r4;

    mov.f32 %f0, 0f00000000;
    mov.f32 %f1, 0f00000000;
    mov.f32 %f2, 0f00000000;
    mov.f32 %f3, 0f00000000;

    mov.u32 %r9, 0;
MMA_LOOP:
    setp.ge.u32 %p1, %r9, %r8;
    @%p1 bra DONE_MMA;

    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};

    add.u32 %r9, %r9, 1;
    bra MMA_LOOP;

DONE_MMA:
    mov.u32 %r6, %ctaid.x;
    mov.u32 %r7, %ntid.x;
    mov.u32 %r10, %tid.x;
    mad.lo.u32 %r11, %r6, %r7, %r10;
    mul.wide.u32 %rd1, %r11, 16;
    add.u64 %rd2, %rd0, %rd1;
    st.global.v4.f32 [%rd2], {%f0, %f1, %f2, %f3};
    ret;
}
)PTX";

    void* mod = nullptr;
    void* fn  = nullptr;
    if (cuda.cuModuleLoadDataEx(&mod, kTensorMmaPtx, 0, nullptr, nullptr) != 0 ||
        cuda.cuModuleGetFunction(&fn, mod, "boo_tensor_mma_f16") != 0) {
        cuda.cuDevicePrimaryCtxRelease(dev);
        out.status = BackendStatus::Faulted;
        out.notes = "PTX JIT compile failed for sm_80 mma.sync.m16n8k16.";
        return out;
    }

    const unsigned int threadsPerBlock = 256; // 8 full warps/block
    const unsigned int gridBlocks = static_cast<unsigned int>(std::max(64, smCount * 32));
    const size_t totalThreads = static_cast<size_t>(gridBlocks) * threadsPerBlock;
    const size_t outBytes = totalThreads * sizeof(float) * 4;

    uint64_t dOut = 0;
    cuda.cuMemAlloc(&dOut, outBytes);

    // Step 1: Exact mathematical checksum pass (loops=1, A=1.0h (0x3C003C00), B=1.0h -> 16 * 16.0f = 256.0f exact!)
    {
        uint32_t loops1 = 1;
        uint32_t oneHalf2 = 0x3C003C00u; // (1.0h, 1.0h)
        void* vargs[] = {&dOut, &loops1, &oneHalf2, &oneHalf2};
        cuda.cuLaunchKernel(fn, 1, 1, 1, 32, 1, 1, 0, nullptr, vargs, nullptr);
        cuda.cuCtxSynchronize();
    }
    std::vector<float> checkWarp(32 * 4, 0.0f);
    cuda.cuMemcpyDtoH(checkWarp.data(), dOut, checkWarp.size() * sizeof(float));
    bool exactCheckOk = true;
    for (float val : checkWarp) {
        if (std::abs(val - 256.0f) > 1e-3f) {
            exactCheckOk = false;
            break;
        }
    }

    void* evStart = nullptr;
    void* evEnd   = nullptr;
    cuda.cuEventCreate(&evStart, 0);
    cuda.cuEventCreate(&evEnd, 0);
    out.coldInitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tCold0).count();

    // Step 2: Sustained Tensor throughput pass
    // Per warp per loop: 16 * (2 * 16 * 8 * 16) = 65,536 FLOPs
    constexpr uint32_t kInnerLoops = 512;
    const uint64_t warpsTotal = static_cast<uint64_t>(gridBlocks) * (threadsPerBlock / 32);
    const double flopsPerPass = static_cast<double>(warpsTotal) * static_cast<double>(kInnerLoops) * 65536.0;
    // Neural Velvet 960x540 4-layer width-64 MLP FLOPs/frame = 518,400 * (2 * 3 * 64 * 64) ~= 12.74 GFLOPs
    constexpr double kNeuralVelvetFlopsPerFrame = 12.7401984e9;

    std::vector<double> samplesMs;
    const uint32_t totalPasses = cfg.warmupIterations + cfg.timedIterations;
    uint32_t eighthHalf2 = 0x30003000u; // (0.125h, 0.125h) -> exact 4.0f/iter -> 2048.0f after 512 iters!
    uint32_t loopsArg = kInnerLoops;

    for (uint32_t p = 0; p < totalPasses && !cfg.cancelled(); ++p) {
        void* args[] = {&dOut, &loopsArg, &eighthHalf2, &eighthHalf2};
        cuda.cuEventRecord(evStart, nullptr);
        cuda.cuLaunchKernel(fn, gridBlocks, 1, 1, threadsPerBlock, 1, 1, 0, nullptr, args, nullptr);
        cuda.cuEventRecord(evEnd, nullptr);
        cuda.cuEventSynchronize(evEnd);

        float ms = 0.0f;
        cuda.cuEventElapsedTime(&ms, evStart, evEnd);
        if (p >= cfg.warmupIterations && ms > 0.0f) {
            samplesMs.push_back(static_cast<double>(ms));
        }
        if (cb && ((p & 3u) == 0u || p + 1 == totalPasses)) {
            const float stg = static_cast<float>(p + 1) / static_cast<float>(totalPasses);
            const double liveTflops = (ms > 0.0f) ? ((flopsPerPass / (static_cast<double>(ms) * 0.001)) / 1e12) : 0.0;
            cb(BenchmarkProgressEvent{
                stepIdx, totalSteps,
                (static_cast<float>(stepIdx) + stg) / static_cast<float>(totalSteps),
                stg, out.title,
                (p < cfg.warmupIterations) ? "WARMUP" : "SAMPLING",
                liveTflops, "TFLOPS"});
        }
    }

    float finalCheck[4]{};
    cuda.cuMemcpyDtoH(finalCheck, dOut, sizeof(finalCheck));
    const bool loopCheckOk = std::abs(finalCheck[0] - 2048.0f) < 1e-2f;

    cuda.cuEventDestroy(evStart);
    cuda.cuEventDestroy(evEnd);
    cuda.cuMemFree(dOut);
    cuda.cuModuleUnload(mod);
    cuda.cuDevicePrimaryCtxRelease(dev);

    out.timingMs = computeFrametimeStats(samplesMs);
    out.executed = !samplesMs.empty();
    out.outputVerified = out.executed && exactCheckOk && loopCheckOk;
    out.status = out.outputVerified ? BackendStatus::Accelerated : BackendStatus::Faulted;
    if (out.timingMs.p50Ms > 0.0) {
        const double flopsPerSec = flopsPerPass / (out.timingMs.p50Ms * 0.001);
        out.primaryScore   = flopsPerSec / 1e12; // TFLOPS
        out.secondaryScore = flopsPerSec / kNeuralVelvetFlopsPerFrame; // Equiv Neural Velvet MLP FPS
    }
    out.verificationDetail =
        "Exact FP16xFP16->FP32 matrix checksum PASS (16x8x16x16 = " + std::to_string(checkWarp[0]).substr(0, 5) +
        ", 512-loop = " + std::to_string(finalCheck[0]).substr(0, 6) + ")";
    out.notes = "Executed warp-level PTX mma.sync.m16n8k16 instructions across " + std::to_string(warpsTotal) +
                " warps (use Nsight Compute to inspect SASS HMMA occupancy).";
    return out;
}

// =============================================================================
// 3B. D3D12 HLSL Procedural Neural Velvet Substitute Benchmark
// =============================================================================
BenchResultEntry runHlslNeuralVelvetFallbackBench(
    D3D12BenchHelper& h,
    const BenchmarkConfig& cfg,
    size_t stepIdx,
    size_t totalSteps,
    const BenchmarkProgressCallback& cb) {
    BenchResultEntry out{};
    out.category = BenchCategory::Tensor;
    out.id = "tensor_hlsl_neural_velvet_fallback";
    out.title = "HLSL Procedural Neural Velvet (960x540 MLP -- NOT Tensor Cores)";
    out.backendName = "D3D12 SM 6.0 Compute (Substitute)";
    out.isFallbackComparison = true;
    out.primaryUnit = "TFLOPS";
    out.secondaryUnit = "MLP FPS";
    out.referenceTarget = 120.0;

    std::vector<uint8_t> csBytes;
    if (!loadCsoBytes("assets/shaders/m0_tensor_fallback_bench.cso", csBytes)) {
        out.status = BackendStatus::Faulted;
        return out;
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 8};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &uavRange};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sigBlob, errBlob;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    h.dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpso{};
    cpso.pRootSignature = rootSig.Get();
    cpso.CS = {csBytes.data(), csBytes.size()};
    if (FAILED(h.dev->CreateComputePipelineState(&cpso, IID_PPV_ARGS(&pso)))) return out;

    constexpr uint32_t kW = 960, kH = 540;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kW;
    td.Height = kH;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> tex;
    if (FAILED(h.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&tex)))) return out;

    D3D12_DESCRIPTOR_HEAP_DESC dhd{};
    dhd.NumDescriptors = 1;
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dhd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> uavHeap;
    h.dev->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&uavHeap));
    h.dev->CreateUnorderedAccessView(tex.Get(), nullptr, nullptr, uavHeap->GetCPUDescriptorHandleForHeapStart());

    // Approx arithmetic ops per texel in 16-layer float4x2 MLP: ~512 FLOPs/texel -> 0.2654 GFLOPs/pass
    constexpr double kFlopsPerPass = static_cast<double>(kW) * static_cast<double>(kH) * 512.0;

    std::vector<double> samplesMs;
    const uint32_t totalPasses = cfg.warmupIterations + cfg.timedIterations;
    for (uint32_t p = 0; p < totalPasses && !cfg.cancelled(); ++p) {
        BenchConstantsGpu bc{kW, kH, p, 0, 0.25f + static_cast<float>(p) * 0.02f, 0, 0, 0};
        h.alloc->Reset();
        h.cmd4->Reset(h.alloc.Get(), pso.Get());
        ID3D12DescriptorHeap* heaps[] = {uavHeap.Get()};
        h.cmd4->SetDescriptorHeaps(1, heaps);
        h.cmd4->SetComputeRootSignature(rootSig.Get());
        h.cmd4->SetComputeRoot32BitConstants(0, 8, &bc, 0);
        h.cmd4->SetComputeRootDescriptorTable(1, uavHeap->GetGPUDescriptorHandleForHeapStart());
        h.cmd4->EndQuery(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        h.cmd4->Dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = tex.Get();
        h.cmd4->ResourceBarrier(1, &ub);
        h.cmd4->EndQuery(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        h.cmd4->ResolveQueryData(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, h.tsReadback.Get(), 0);
        if (!h.submitAndWait()) break;
        const double ms = h.readGpuElapsedMs();
        if (p >= cfg.warmupIterations && ms > 0.0) samplesMs.push_back(ms);
        if (cb && ((p & 7u) == 0u || p + 1 == totalPasses)) {
            const float stg = static_cast<float>(p + 1) / static_cast<float>(totalPasses);
            cb(BenchmarkProgressEvent{stepIdx, totalSteps, (static_cast<float>(stepIdx) + stg) / static_cast<float>(totalSteps), stg, out.title, "SAMPLING", (ms > 0.0) ? ((kFlopsPerPass / (ms * 0.001)) / 1e12) : 0.0, "TFLOPS"});
        }
    }

    out.timingMs = computeFrametimeStats(samplesMs);
    out.executed = !samplesMs.empty();
    out.outputVerified = out.executed;
    out.status = out.outputVerified ? BackendStatus::Substituted : BackendStatus::Faulted;
    if (out.timingMs.p50Ms > 0.0) {
        out.primaryScore   = (kFlopsPerPass / (out.timingMs.p50Ms * 0.001)) / 1e12;
        out.secondaryScore = 1000.0 / out.timingMs.p50Ms;
    }
    out.verificationDetail = "960x540 FP16 UAV neural texture pass completed cleanly";
    out.notes = "D3D12 HLSL procedural MLP texture stand-in (labeled Substitute, never Tensor Cores).";
    return out;
}

// =============================================================================
// 4A. Optional Hardware Video Encoder (NVENC / Hardware H.264 MFT, In-Memory Only)
// =============================================================================
BenchResultEntry runHardwareNvencMftBench(
    int32_t luidLow,
    int32_t luidHigh,
    const BenchmarkConfig& cfg,
    size_t stepIdx,
    size_t totalSteps,
    const BenchmarkProgressCallback& cb) {
    BenchResultEntry out{};
    out.category = BenchCategory::NVENC;
    out.id = "nvenc_hardware_h264_in_memory";
    out.title = "Hardware Video Encoder (In-Memory 1080p NV12 -> H.264)";
    out.backendName = "Hardware H.264 Encoder MFT";
    out.primaryUnit = "FPS";
    out.secondaryUnit = "Mbps";
    out.referenceTarget = 600.0; // 600 fps 1080p hardware encode reference bar

    if (cfg.disableNVENC) {
        out.status = BackendStatus::Unavailable;
        out.notes = "Skipped (--disable-nvenc)";
        return out;
    }

    // Query optional nvEncodeAPI64.dll driver API version for diagnostics transparency
    std::string nvencApiTag;
    if (HMODULE hNv = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using GetMaxVerFn = int(WINAPI*)(uint32_t*);
        if (auto pVer = reinterpret_cast<GetMaxVerFn>(GetProcAddress(hNv, "NvEncodeAPIGetMaxSupportedVersion"))) {
            uint32_t v = 0;
            if (pVer(&v) == 0) {
                nvencApiTag = " [NVENC API " + std::to_string(v >> 4) + "." + std::to_string(v & 0xFu) + "]";
            }
        }
        FreeLibrary(hNv);
    }

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didCoInit = (hrCo == S_OK || hrCo == S_FALSE);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        if (didCoInit) CoUninitialize();
        out.status = BackendStatus::Unavailable;
        out.notes = "Media Foundation startup failed.";
        return out;
    }

    // Scope every Media Foundation & MFT COM pointer strictly inside MFStartup..MFShutdown
    // so no ComPtr destructor ever fires after CoUninitialize() unloads the MFT DLL.
    [&]() {
        const auto tCold0 = std::chrono::steady_clock::now();

        MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};
        ComPtr<IMFAttributes> enumAttrs;
        MFCreateAttributes(&enumAttrs, 1);
        LUID adapterLuid{static_cast<DWORD>(luidLow), luidHigh};
        if (enumAttrs) {
            enumAttrs->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&adapterLuid), sizeof(LUID));
        }

        IMFActivate** ppAct = nullptr;
        UINT32 actCount = 0;
        const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
        HRESULT hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, enumAttrs.Get(), &ppAct, &actCount);
        if (FAILED(hr) || actCount == 0) {
            hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, &ppAct, &actCount);
        }
        enumAttrs.Reset();

        if (FAILED(hr) || actCount == 0 || !ppAct) {
            out.status = BackendStatus::Unavailable;
            out.notes = "No hardware H.264 encoder MFT present on this machine.";
            return;
        }

        std::string encoderFriendlyName = "Hardware H.264 Encoder";
        wchar_t* wName = nullptr;
        UINT32 wLen = 0;
        if (SUCCEEDED(ppAct[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wName, &wLen)) && wName) {
            encoderFriendlyName = wideToUtf8Local(wName);
            CoTaskMemFree(wName);
        }
        out.backendName = encoderFriendlyName + nvencApiTag;

        ComPtr<IMFTransform> encoder;
        hr = ppAct[0]->ActivateObject(IID_PPV_ARGS(&encoder));
        for (UINT32 i = 0; i < actCount; ++i) ppAct[i]->Release();
        CoTaskMemFree(ppAct);

        if (FAILED(hr) || !encoder) {
            out.status = BackendStatus::Unavailable;
            out.notes = "Hardware encoder ActivateObject failed.";
            return;
        }

        auto shutdownEncoder = [&]() {
            if (!encoder) return;
            ComPtr<IMFShutdown> mfShutdownObj;
            if (SUCCEEDED(encoder.As(&mfShutdownObj)) && mfShutdownObj) {
                mfShutdownObj->Shutdown();
            }
            mfShutdownObj.Reset();
            encoder.Reset();
        };

        UINT32 isAsync = 0;
        {
            ComPtr<IMFAttributes> mftAttrs;
            if (SUCCEEDED(encoder->GetAttributes(&mftAttrs)) && mftAttrs) {
                mftAttrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
                if (isAsync) {
                    mftAttrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                }
            }
        }

        // Configure low-latency mode if ICodecAPI is supported
        {
            ComPtr<ICodecAPI> codecApi;
            if (SUCCEEDED(encoder.As(&codecApi)) && codecApi) {
                VARIANT varLowLat{};
                varLowLat.vt = VT_BOOL;
                varLowLat.boolVal = VARIANT_TRUE;
                codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &varLowLat);
            }
        }

        constexpr uint32_t kW = 1920, kH = 1080, kFps = 60, kBitrate = 8000000;

        ComPtr<IMFMediaType> outType;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE, kBitrate);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, kW, kH);
        MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, kFps, 1);
        MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        if (FAILED(encoder->SetOutputType(0, outType.Get(), 0))) {
            outType.Reset();
            shutdownEncoder();
            out.status = BackendStatus::Unavailable;
            out.notes = "Hardware encoder rejected 1080p60 H.264 output media type.";
            return;
        }
        outType.Reset();

        // Pick NV12 from encoder's available input types (ensuring exact stride/attribute compatibility)
        ComPtr<IMFMediaType> chosenInType;
        for (DWORD idx = 0; idx < 32; ++idx) {
            ComPtr<IMFMediaType> cand;
            if (FAILED(encoder->GetInputAvailableType(0, idx, &cand))) break;
            GUID sub{};
            if (SUCCEEDED(cand->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
                MFSetAttributeSize(cand.Get(), MF_MT_FRAME_SIZE, kW, kH);
                MFSetAttributeRatio(cand.Get(), MF_MT_FRAME_RATE, kFps, 1);
                cand->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                if (SUCCEEDED(encoder->SetInputType(0, cand.Get(), 0))) {
                    chosenInType = cand;
                    break;
                }
            }
        }
        if (!chosenInType) {
            MFCreateMediaType(&chosenInType);
            chosenInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            chosenInType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            chosenInType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(chosenInType.Get(), MF_MT_FRAME_SIZE, kW, kH);
            MFSetAttributeRatio(chosenInType.Get(), MF_MT_FRAME_RATE, kFps, 1);
            if (FAILED(encoder->SetInputType(0, chosenInType.Get(), 0))) {
                chosenInType.Reset();
                shutdownEncoder();
                out.status = BackendStatus::Unavailable;
                out.notes = "Hardware encoder rejected 1080p NV12 input media type.";
                return;
            }
        }
        chosenInType.Reset();

        // Pre-build 8 distinct dynamic NV12 1080p frames in RAM (zero disk IO!)
        constexpr size_t kRingFrames = 8;
        const DWORD nv12Size = (kW * kH * 3) / 2;
        std::vector<ComPtr<IMFMediaBuffer>> nv12Buffers(kRingFrames);
        for (size_t f = 0; f < kRingFrames; ++f) {
            MFCreateMemoryBuffer(nv12Size, &nv12Buffers[f]);
            BYTE* dst = nullptr;
            if (SUCCEEDED(nv12Buffers[f]->Lock(&dst, nullptr, nullptr)) && dst) {
                // Animated Glass Orchid diagonal wave pattern so motion estimation works realistically
                for (uint32_t y = 0; y < kH; ++y) {
                    BYTE* row = dst + y * kW;
                    for (uint32_t x = 0; x < kW; ++x) {
                        row[x] = static_cast<BYTE>(16 + (((x * 3 + y * 5 + static_cast<uint32_t>(f) * 29) & 0xFF) * 200) / 256);
                    }
                }
                BYTE* uvBase = dst + (kW * kH);
                for (uint32_t y = 0; y < kH / 2; ++y) {
                    BYTE* uvRow = uvBase + y * kW;
                    for (uint32_t x = 0; x < kW; x += 2) {
                        uvRow[x + 0] = static_cast<BYTE>(96 + ((x + static_cast<uint32_t>(f) * 17) & 0x3F));
                        uvRow[x + 1] = static_cast<BYTE>(140 + ((y + static_cast<uint32_t>(f) * 13) & 0x3F));
                    }
                }
                nv12Buffers[f]->Unlock();
                nv12Buffers[f]->SetCurrentLength(nv12Size);
            }
        }

        MFT_OUTPUT_STREAM_INFO outStreamInfo{};
        encoder->GetOutputStreamInfo(0, &outStreamInfo);
        const bool mftAllocatesOut =
            (outStreamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        out.coldInitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tCold0).count();

        const uint32_t totalFrames = std::max<uint32_t>(30, cfg.warmupIterations + cfg.timedIterations * 2);
        uint32_t fedFrames = 0;
        uint32_t encodedFrames = 0;
        uint64_t totalCompressedBytes = 0;
        bool annexBStartCodeVerified = false;
        bool drainSent = false;

        const auto inspectOutputSample = [&](IMFSample* outSamp) {
            if (!outSamp) return;
            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(outSamp->ConvertToContiguousBuffer(&buf)) && buf) {
                BYTE* pBytes = nullptr;
                DWORD curLen = 0;
                if (SUCCEEDED(buf->Lock(&pBytes, nullptr, &curLen)) && pBytes && curLen > 4) {
                    totalCompressedBytes += curLen;
                    ++encodedFrames;
                    if ((pBytes[0] == 0 && pBytes[1] == 0 && pBytes[2] == 0 && pBytes[3] == 1) ||
                        (pBytes[0] == 0 && pBytes[1] == 0 && pBytes[2] == 1)) {
                        annexBStartCodeVerified = true;
                    }
                    buf->Unlock();
                }
            }
        };

        const auto makeInputSample = [&](uint32_t frameIdx) -> ComPtr<IMFSample> {
            ComPtr<IMFSample> samp;
            MFCreateSample(&samp);
            samp->AddBuffer(nv12Buffers[frameIdx % kRingFrames].Get());
            const LONGLONG dur100ns = 10000000LL / kFps;
            samp->SetSampleTime(static_cast<LONGLONG>(frameIdx) * dur100ns);
            samp->SetSampleDuration(dur100ns);
            return samp;
        };

        std::vector<double> perOutputIntervalsMs;
        const auto tEncodeStart = std::chrono::steady_clock::now();
        auto tPrevFrame = tEncodeStart;

        if (isAsync) {
            ComPtr<IMFMediaEventGenerator> evGen;
            encoder.As(&evGen);
            while (evGen) {
                ComPtr<IMFMediaEvent> ev;
                if (FAILED(evGen->GetEvent(0, &ev)) || !ev) break;
                MediaEventType met = MEUnknown;
                ev->GetType(&met);

                if (met == METransformNeedInput) {
                    if (fedFrames < totalFrames) {
                        auto samp = makeInputSample(fedFrames++);
                        encoder->ProcessInput(0, samp.Get(), 0);
                    } else if (!drainSent) {
                        drainSent = true;
                        encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
                    }
                } else if (met == METransformHaveOutput) {
                    MFT_OUTPUT_DATA_BUFFER odb{};
                    ComPtr<IMFSample> callerOutSamp;
                    if (!mftAllocatesOut) {
                        MFCreateSample(&callerOutSamp);
                        ComPtr<IMFMediaBuffer> ob;
                        MFCreateMemoryBuffer(std::max<DWORD>(262144, outStreamInfo.cbSize), &ob);
                        callerOutSamp->AddBuffer(ob.Get());
                        odb.pSample = callerOutSamp.Get();
                    }
                    DWORD status = 0;
                    if (SUCCEEDED(encoder->ProcessOutput(0, 1, &odb, &status))) {
                        const auto tNow = std::chrono::steady_clock::now();
                        if (encodedFrames >= cfg.warmupIterations) {
                            perOutputIntervalsMs.push_back(std::chrono::duration<double, std::milli>(tNow - tPrevFrame).count());
                        }
                        tPrevFrame = tNow;
                        inspectOutputSample(odb.pSample);
                        if (mftAllocatesOut && odb.pSample) odb.pSample->Release();
                        if (odb.pEvents) odb.pEvents->Release();

                        if (cb && ((encodedFrames & 7u) == 0u)) {
                            const float stg = std::min(1.0f, static_cast<float>(encodedFrames) / static_cast<float>(totalFrames));
                            const double elapsedS = std::chrono::duration<double>(tNow - tEncodeStart).count();
                            cb(BenchmarkProgressEvent{
                                stepIdx, totalSteps,
                                (static_cast<float>(stepIdx) + stg) / static_cast<float>(totalSteps),
                                stg, out.title, "SAMPLING",
                                (elapsedS > 0.0) ? (static_cast<double>(encodedFrames) / elapsedS) : 0.0,
                                "FPS"});
                        }
                    }
                } else if (met == METransformDrainComplete) {
                    break;
                }
            }
            evGen.Reset();
        } else {
            // Synchronous MFT loop
            while (!cfg.cancelled() && (fedFrames < totalFrames || !drainSent)) {
                if (fedFrames < totalFrames) {
                    auto samp = makeInputSample(fedFrames++);
                    encoder->ProcessInput(0, samp.Get(), 0);
                } else if (!drainSent) {
                    drainSent = true;
                    encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
                }
                while (true) {
                    MFT_OUTPUT_DATA_BUFFER odb{};
                    ComPtr<IMFSample> callerOutSamp;
                    if (!mftAllocatesOut) {
                        MFCreateSample(&callerOutSamp);
                        ComPtr<IMFMediaBuffer> ob;
                        MFCreateMemoryBuffer(std::max<DWORD>(262144, outStreamInfo.cbSize), &ob);
                        callerOutSamp->AddBuffer(ob.Get());
                        odb.pSample = callerOutSamp.Get();
                    }
                    DWORD status = 0;
                    const HRESULT hrOut = encoder->ProcessOutput(0, 1, &odb, &status);
                    if (hrOut == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
                    if (FAILED(hrOut)) { drainSent = true; break; }
                    const auto tNow = std::chrono::steady_clock::now();
                    if (encodedFrames >= cfg.warmupIterations) {
                        perOutputIntervalsMs.push_back(std::chrono::duration<double, std::milli>(tNow - tPrevFrame).count());
                    }
                    tPrevFrame = tNow;
                    inspectOutputSample(odb.pSample);
                    if (mftAllocatesOut && odb.pSample) odb.pSample->Release();
                    if (odb.pEvents) odb.pEvents->Release();
                }
            }
        }

        const auto tEncodeEnd = std::chrono::steady_clock::now();
        const double totalEncodeSec = std::chrono::duration<double>(tEncodeEnd - tEncodeStart).count();

        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        nv12Buffers.clear();
        shutdownEncoder();

        out.timingMs = computeFrametimeStats(perOutputIntervalsMs);
        out.executed = (encodedFrames > 0);
        out.outputVerified = out.executed && (totalCompressedBytes > 0) && annexBStartCodeVerified;
        out.status = out.outputVerified ? BackendStatus::Accelerated : BackendStatus::Faulted;
        if (totalEncodeSec > 0.0 && encodedFrames > 0) {
            out.primaryScore = static_cast<double>(encodedFrames) / totalEncodeSec; // Sustained 1080p Encode FPS
            const double filmDurationSec = static_cast<double>(encodedFrames) / static_cast<double>(kFps);
            out.secondaryScore = (filmDurationSec > 0.0)
                                     ? ((static_cast<double>(totalCompressedBytes) * 8.0) / (filmDurationSec * 1e6))
                                     : 0.0;
        }
        out.verificationDetail =
            "In-memory H.264 Annex-B NAL bitstream verified (" + std::to_string(encodedFrames) + " frames -> " +
            std::to_string(totalCompressedBytes / 1024) + " KiB, zero disk writes)";
        out.notes = "Optional Signal Hijack hardware encode path via " + out.backendName + ".";
    }();

    MFShutdown();
    if (didCoInit) CoUninitialize();
    return out;
}

// =============================================================================
// 4B. D3D12 HLSL "SHADER SIGNAL" Codec Art Substitute Benchmark
// =============================================================================
BenchResultEntry runShaderSignalFallbackBench(
    D3D12BenchHelper& h,
    const BenchmarkConfig& cfg,
    size_t stepIdx,
    size_t totalSteps,
    const BenchmarkProgressCallback& cb) {
    BenchResultEntry out{};
    out.category = BenchCategory::NVENC;
    out.id = "nvenc_shader_signal_fallback";
    out.title = "SHADER SIGNAL Art Substitute (1080p Macroblock -- NOT NVENC)";
    out.backendName = "D3D12 SM 6.0 SHADER SIGNAL (Substitute)";
    out.isFallbackComparison = true;
    out.primaryUnit = "FPS";
    out.secondaryUnit = "GPix/s";
    out.referenceTarget = 12000.0;

    std::vector<uint8_t> csBytes;
    if (!loadCsoBytes("assets/shaders/m0_signal_bench.cso", csBytes)) {
        out.status = BackendStatus::Faulted;
        return out;
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 8};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &uavRange};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> sigBlob, errBlob;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    h.dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpso{};
    cpso.pRootSignature = rootSig.Get();
    cpso.CS = {csBytes.data(), csBytes.size()};
    if (FAILED(h.dev->CreateComputePipelineState(&cpso, IID_PPV_ARGS(&pso)))) return out;

    constexpr uint32_t kW = 1920, kH = 1080;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kW;
    td.Height = kH;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> tex;
    if (FAILED(h.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&tex)))) return out;

    D3D12_DESCRIPTOR_HEAP_DESC dhd{};
    dhd.NumDescriptors = 1;
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    dhd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> uavHeap;
    h.dev->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&uavHeap));
    h.dev->CreateUnorderedAccessView(tex.Get(), nullptr, nullptr, uavHeap->GetCPUDescriptorHandleForHeapStart());

    std::vector<double> samplesMs;
    const uint32_t totalPasses = cfg.warmupIterations + cfg.timedIterations;
    for (uint32_t p = 0; p < totalPasses && !cfg.cancelled(); ++p) {
        BenchConstantsGpu bc{kW, kH, p, 0, 1.0f + static_cast<float>(p) * 0.05f, 0, 0, 0};
        h.alloc->Reset();
        h.cmd4->Reset(h.alloc.Get(), pso.Get());
        ID3D12DescriptorHeap* heaps[] = {uavHeap.Get()};
        h.cmd4->SetDescriptorHeaps(1, heaps);
        h.cmd4->SetComputeRootSignature(rootSig.Get());
        h.cmd4->SetComputeRoot32BitConstants(0, 8, &bc, 0);
        h.cmd4->SetComputeRootDescriptorTable(1, uavHeap->GetGPUDescriptorHandleForHeapStart());
        h.cmd4->EndQuery(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        h.cmd4->Dispatch((kW + 7) / 8, (kH + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER ub{};
        ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub.UAV.pResource = tex.Get();
        h.cmd4->ResourceBarrier(1, &ub);
        h.cmd4->EndQuery(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        h.cmd4->ResolveQueryData(h.tsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, h.tsReadback.Get(), 0);
        if (!h.submitAndWait()) break;
        const double ms = h.readGpuElapsedMs();
        if (p >= cfg.warmupIterations && ms > 0.0) samplesMs.push_back(ms);
        if (cb && ((p & 7u) == 0u || p + 1 == totalPasses)) {
            const float stg = static_cast<float>(p + 1) / static_cast<float>(totalPasses);
            cb(BenchmarkProgressEvent{stepIdx, totalSteps, (static_cast<float>(stepIdx) + stg) / static_cast<float>(totalSteps), stg, out.title, "SAMPLING", (ms > 0.0) ? (1000.0 / ms) : 0.0, "FPS"});
        }
    }

    out.timingMs = computeFrametimeStats(samplesMs);
    out.executed = !samplesMs.empty();
    out.outputVerified = out.executed;
    out.status = out.outputVerified ? BackendStatus::Substituted : BackendStatus::Faulted;
    if (out.timingMs.p50Ms > 0.0) {
        out.primaryScore   = 1000.0 / out.timingMs.p50Ms;
        out.secondaryScore = (1920.0 * 1080.0 / 1e9) / (out.timingMs.p50Ms * 0.001);
    }
    out.verificationDetail = "1920x1080 YCoCg macroblock shader output completed";
    out.notes = "Art-direction SHADER SIGNAL substitute when NVENC is disabled or absent.";
    return out;
}

std::string renderProgressBarAscii(double ratio01, size_t width, bool unicodeBlocks, const char* ansiColor) {
    const double clamped = std::clamp(ratio01, 0.0, 1.0);
    const size_t filled = static_cast<size_t>(std::llround(clamped * static_cast<double>(width)));
    std::string s;
    if (ansiColor && *ansiColor) s += ansiColor;
    for (size_t i = 0; i < width; ++i) {
        if (i < filled) {
            s += unicodeBlocks ? "\xE2\x96\x88" : "#"; // '█'
        } else if (i == filled && clamped > 0.0 && filled < width) {
            s += unicodeBlocks ? "\xE2\x96\x93" : "="; // '▓'
        } else {
            if (ansiColor && *ansiColor && i == filled + 1) s += "\x1b[90m";
            s += unicodeBlocks ? "\xE2\x96\x91" : "."; // '░'
        }
    }
    if (ansiColor && *ansiColor) s += "\x1b[0m";
    return s;
}

} // namespace

BenchmarkSuiteReport runAdHocBenchmarks(
    void* d3d12DevicePtr,
    void* d3d12CommandQueuePtr,
    int32_t adapterLuidLow,
    int32_t adapterLuidHigh,
    const std::string& adapterName,
    const std::string& cpuName,
    const BenchmarkConfig& requestedConfig,
    BenchmarkProgressCallback onProgress) {
    BenchmarkConfig config = requestedConfig;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    LUID luid{static_cast<DWORD>(adapterLuidLow), adapterLuidHigh};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
        FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        config.disableRT = config.disableCUDA = config.disableTensor = config.disableNVENC = true;
    }
    BenchmarkSuiteReport report{};
    report.timestampIso = nowIso8601();
    report.adapterName  = adapterName;
    report.cpuName      = cpuName;
    report.timedIterations = config.timedIterations;

    const auto tSuite0 = std::chrono::steady_clock::now();

    D3D12BenchHelper helper;
    const bool d3dOk = helper.init(
        static_cast<ID3D12Device*>(d3d12DevicePtr),
        static_cast<ID3D12CommandQueue*>(d3d12CommandQueuePtr));

    const size_t totalSteps = config.includeFallbacks ? 8 : 4;
    size_t step = 0;

    if (d3dOk) {
        if (!config.cancelled()) report.entries.push_back(runDxrHardwareBench(helper, config, step++, totalSteps, onProgress));
        if (config.includeFallbacks) {
            if (!config.cancelled()) report.entries.push_back(runRtAnalyticFallbackBench(helper, config, step++, totalSteps, onProgress));
        }
    }

    if (!config.cancelled()) report.entries.push_back(runCudaParticlesBench(adapterLuidLow, adapterLuidHigh, config, step++, totalSteps, onProgress));
    if (config.includeFallbacks && d3dOk) {
        if (!config.cancelled()) report.entries.push_back(runHlslParticlesFallbackBench(helper, config, step++, totalSteps, onProgress));
    }

    if (!config.cancelled()) report.entries.push_back(runTensorMmaBench(adapterLuidLow, adapterLuidHigh, config, step++, totalSteps, onProgress));
    if (config.includeFallbacks && d3dOk) {
        if (!config.cancelled()) report.entries.push_back(runHlslNeuralVelvetFallbackBench(helper, config, step++, totalSteps, onProgress));
    }

    if (!config.cancelled()) report.entries.push_back(runHardwareNvencMftBench(adapterLuidLow, adapterLuidHigh, config, step++, totalSteps, onProgress));
    if (config.includeFallbacks && d3dOk) {
        if (!config.cancelled()) report.entries.push_back(runShaderSignalFallbackBench(helper, config, step++, totalSteps, onProgress));
    }

    report.totalElapsedSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tSuite0).count();
    return report;
}

void printCreativeBenchmarkDashboard(
    const BenchmarkSuiteReport& report,
    bool enableAnsiColors) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD consoleMode = 0;
    bool useAnsi = false;
    bool useUtf8 = false;
    if (hOut && hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &consoleMode)) {
        SetConsoleOutputCP(CP_UTF8);
        useUtf8 = true;
        if (enableAnsiColors && SetConsoleMode(hOut, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            useAnsi = true;
        }
    }

    const char* cReset   = useAnsi ? "\x1b[0m"    : "";
    const char* cJade    = useAnsi ? "\x1b[96m"   : "";
    const char* cOrchid  = useAnsi ? "\x1b[95m"   : "";
    const char* cAmber   = useAnsi ? "\x1b[93m"   : "";
    const char* cEmerald = useAnsi ? "\x1b[92m"   : "";
    const char* cDim     = useAnsi ? "\x1b[90m"   : "";
    const char* cBold    = useAnsi ? "\x1b[1m"    : "";

    std::cout << "\n";
    std::cout << cJade << "+==============================================================================================+" << cReset << "\n";
    std::cout << cJade << "|" << cBold << "   BOO SAH SAH - THE GLASS ORCHID PROTOCOL :: SILICON TELEMETRY & NUMERICAL BENCHMARK DECK    " << cReset << cJade << "|" << cReset << "\n";
    std::cout << cJade << "+==============================================================================================+" << cReset << "\n";
    std::cout << "  GPU Adapter : " << cBold << report.adapterName << cReset << "\n";
    std::cout << "  CPU Host    : " << report.cpuName << "\n";
    std::cout << "  Methodology : Warmup + " << report.timedIterations << " timed passes/kernel | Elapsed: "
              << std::fixed;
    {
        char tbuf[32]{};
        std::snprintf(tbuf, sizeof(tbuf), "%.2f s", report.totalElapsedSeconds);
        std::cout << tbuf << " | Timestamp: " << report.timestampIso << "\n";
    }
    std::cout << cDim << "------------------------------------------------------------------------------------------------" << cReset << "\n";

    for (const auto& e : report.entries) {
        const char* accent = cJade;
        if (e.category == BenchCategory::CUDA)   accent = "\x1b[94m";
        if (e.category == BenchCategory::Tensor) accent = cOrchid;
        if (e.category == BenchCategory::NVENC)  accent = cAmber;
        if (!useAnsi) accent = "";
        if (e.isFallbackComparison) accent = cDim;

        const double fill = (e.referenceTarget > 0.0) ? (e.primaryScore / e.referenceTarget) : 0.0;
        const std::string bar = renderProgressBarAscii(fill, 26, useUtf8, useAnsi ? (e.isFallbackComparison ? "\x1b[36m" : accent) : "");

        char numBuf[128]{};
        if (e.executed) {
            std::snprintf(
                numBuf,
                sizeof(numBuf),
                "%9.2f %-8s | %8.2f %-8s | p50:%6.3f ms  p95:%6.3f ms",
                e.primaryScore,
                e.primaryUnit.c_str(),
                e.secondaryScore,
                e.secondaryUnit.c_str(),
                e.timingMs.p50Ms,
                e.timingMs.p95Ms);
        } else {
            std::snprintf(numBuf, sizeof(numBuf), "   --     %-8s |    --    %-8s | [%s]",
                          e.primaryUnit.c_str(), e.secondaryUnit.c_str(), toString(e.status));
        }

        const char* badge = e.outputVerified
                                ? (e.isFallbackComparison ? "[SUBSTITUTE OK]" : "[VERIFIED HW]  ")
                                : "[UNAVAILABLE]  ";
        const char* badgeCol = e.outputVerified
                                   ? (e.isFallbackComparison ? cJade : cEmerald)
                                   : cAmber;

        std::cout << "  " << cBold << "[" << toString(e.category) << "]" << cReset << " "
                  << e.title << "\n";
        std::cout << "    " << badgeCol << badge << cReset << " [" << bar << "] " << cBold << numBuf << cReset << "\n";
        std::cout << "    " << cDim << "Backend: " << e.backendName << " | "
                  << (e.outputVerified ? e.verificationDetail : e.notes) << cReset << "\n\n";
    }

    // Side-by-side Accelerator vs HLSL Fallback Summary Card
    std::cout << cJade << "+---------------------------------- SILICON ORCHESTRA SCORECARD -------------------------------+" << cReset << "\n";
    const BenchCategory cats[] = {BenchCategory::RT, BenchCategory::CUDA, BenchCategory::Tensor, BenchCategory::NVENC};
    for (BenchCategory cat : cats) {
        const BenchResultEntry* hw = nullptr;
        const BenchResultEntry* fb = nullptr;
        for (const auto& e : report.entries) {
            if (e.category == cat && !e.isFallbackComparison) hw = &e;
            if (e.category == cat &&  e.isFallbackComparison) fb = &e;
        }
        if (hw && hw->outputVerified) {
            char line[196]{};
            if (cat == BenchCategory::NVENC && fb && fb->outputVerified) {
                std::snprintf(
                    line, sizeof(line),
                    "  %-6s : %9.2f %-8s (HW H.264 Bitstream @ %.2f Mbps) | Art Shader Sub: %8.2f FPS",
                    toString(cat),
                    hw->primaryScore, hw->primaryUnit.c_str(),
                    hw->secondaryScore,
                    fb->primaryScore);
            } else if (fb && fb->outputVerified && fb->primaryScore > 0.0 && hw->primaryUnit == fb->primaryUnit) {
                const double ratio = hw->primaryScore / fb->primaryScore;
                std::snprintf(
                    line, sizeof(line),
                    "  %-6s : %9.2f %-8s (HW Verified)  vs  %9.2f %-8s (HLSL Sub)  =>  %5.2fx",
                    toString(cat),
                    hw->primaryScore, hw->primaryUnit.c_str(),
                    fb->primaryScore, fb->primaryUnit.c_str(),
                    ratio);
            } else {
                std::snprintf(
                    line, sizeof(line),
                    "  %-6s : %9.2f %-8s (HW Verified)  |  %8.2f %-8s",
                    toString(cat),
                    hw->primaryScore, hw->primaryUnit.c_str(),
                    hw->secondaryScore, hw->secondaryUnit.c_str());
            }
            std::cout << cEmerald << line << cReset << "\n";
        } else if (fb && fb->outputVerified) {
            char line[160]{};
            std::snprintf(
                line, sizeof(line),
                "  %-6s : HW Unavailable -> Graceful Fallback Active (%8.2f %s)",
                toString(cat), fb->primaryScore, fb->primaryUnit.c_str());
            std::cout << cAmber << line << cReset << "\n";
        }
    }
    std::cout << cJade << "+==============================================================================================+" << cReset << "\n\n";
}

} // namespace boo
