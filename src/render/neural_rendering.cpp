#include "boo/neural_rendering.hpp"
#include "boo/pipeline_compiler.hpp"
#include "boo/startup_progress.hpp"

#include "boo/logging.hpp"
#include "boo/mapped_write.hpp"
#include "boo/render.hpp"

#ifdef BOO_HAS_LINALG
#include <initguid.h> // D3D12ExperimentalShaderModels
#endif
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace boo {
using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kHidden = 32u;
constexpr uint32_t kShadeIn = 16u;
constexpr uint32_t kMlpParams = kHidden * kShadeIn + kHidden + kHidden * kHidden + kHidden + 3u * kHidden + 3u; // 1699
constexpr uint32_t kLatent = 64u;
constexpr uint32_t kLatentChannels = 8u;
constexpr uint32_t kTex = NeuralRenderingSuite::kTextureSize;
constexpr uint32_t kCacheGridParams = 43520u;
constexpr uint32_t kCacheIn = 9u;
constexpr uint32_t kCacheMlpParams = kHidden * kCacheIn + kHidden + kHidden * kHidden + kHidden + 3u * kHidden + 3u; // 1475
constexpr uint32_t kCacheParams = kCacheGridParams + kCacheMlpParams;
constexpr uint32_t kSlots = D3D12Context::kFrameCount;
constexpr uint32_t kTimestamps = 4u;
constexpr uint32_t kStatWords = 16u;
constexpr double kStatScale = 65536.0;

// Network arithmetic (2 FLOP per multiply-add), used for the TFLOPS readout.
constexpr double kMlpFlopPerEval = 2.0 * (kShadeIn * kHidden + kHidden * kHidden + kHidden * 3.0);    // 3,264
constexpr double kCacheFlopPerEval = 2.0 * (kCacheIn * kHidden + kHidden * kHidden + kHidden * 3.0);  // 2,816
constexpr double kCacheTrainFlopPerSample = 3.0 * kCacheFlopPerEval;  // forward + two backward products

enum : uint32_t { kPsoClear, kPsoNtc, kPsoInit, kPsoTrain, kPsoUpdate, kPsoInfer, kPsoComposite, kPsoPack, kPsoCount };
constexpr const char* kEntryFiles[kPsoCount] = {
    "assets/shaders/neural_clear_cs.cso", "assets/shaders/neural_ntc_decode_cs.cso",
    "assets/shaders/neural_nrc_init_cs.cso", "assets/shaders/neural_nrc_train_cs.cso",
    "assets/shaders/neural_nrc_update_cs.cso", "assets/shaders/neural_nrc_infer_cs.cso",
    "assets/shaders/neural_composite_cs.cso", "assets/shaders/neural_nrc_pack_cs.cso"};

// Tensor-core variants (SM 6.10 LinAlg, built only with ENABLE_LINALG_PREVIEW).
enum : uint32_t { kTcNtc, kTcInfer, kTcComposite, kTcCount };
constexpr const char* kTensorFiles[kTcCount] = {
    "assets/shaders/neural_ntc_decode_tc_cs.cso", "assets/shaders/neural_nrc_infer_tc_cs.cso",
    "assets/shaders/neural_composite_tc_cs.cso"};
constexpr uint32_t kF16NetBytes = 3328u; // W1 32x16 @0, W2 32x32 @1024, W3 4x32 @3072

// Descriptor slots: SRV t0..t5 then UAV u0..u7, one table.
enum : uint32_t { kDescDepth, kDescWeights, kDescLatents, kDescReference, kDescMlpF16, kDescNrcF16,
                  kDescHdr, kDescParams, kDescAdam, kDescGrad, kDescNtcOut, kDescNrcOut, kDescStats, kDescNrcF16Out,
                  kDescCount };
constexpr uint32_t kSrvCount = kDescHdr;
constexpr uint32_t kUavCount = kDescCount - kDescHdr;

uint16_t halfBits(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exponent = int((x >> 23) & 0xFFu) - 127 + 15;
    const uint32_t mantissa = x & 0x7FFFFFu;
    if (exponent <= 0) return static_cast<uint16_t>(sign);
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7BFFu); // clamp to the largest finite half
    const uint32_t rounded = (static_cast<uint32_t>(exponent) << 10 | (mantissa >> 13)) + ((mantissa >> 12) & 1u);
    return static_cast<uint16_t>(sign | std::min(rounded, 0x7BFFu));
}

// One offline MLP (FP32, W1|b1|W2|b2|W3|b3) as FP16 row-major tensor-core matrices; W3 padded to 4 rows.
void packF16Net(const float* w, uint16_t* out) {
    for (uint32_t i = 0; i < kHidden * kShadeIn; ++i) out[i] = halfBits(w[i]);
    const float* w2 = w + kHidden * kShadeIn + kHidden;
    for (uint32_t i = 0; i < kHidden * kHidden; ++i) out[512 + i] = halfBits(w2[i]);
    const float* w3 = w2 + kHidden * kHidden + kHidden;
    for (uint32_t i = 0; i < 4u * kHidden; ++i) out[1536 + i] = i < 3u * kHidden ? halfBits(w3[i]) : 0;
}

struct NeuralConstants {
    uint32_t canvasW, canvasH, frame, flags, adamStep;
    float learnRate, distanceScale;
    uint32_t pad;
};
static_assert(sizeof(NeuralConstants) == 32);

void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    cmd->ResourceBarrier(1, &barrier);
}

void uavBarrier(ID3D12GraphicsCommandList* cmd) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cmd->ResourceBarrier(1, &barrier);
}

D3D12_RESOURCE_DESC bufferDesc(uint64_t bytes, bool uav) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 4u);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    return d;
}

D3D12_RESOURCE_DESC textureDesc(uint32_t w, uint32_t h, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return d;
}

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES h{};
    h.Type = type;
    h.CreationNodeMask = h.VisibleNodeMask = 1;
    return h;
}

float psnrFromMse(double mse) { return static_cast<float>(std::clamp(10.0 * std::log10(1.0 / std::max(mse, 1e-10)), 0.0, 99.0)); }

} // namespace

struct NeuralRenderingSuite::Impl {
    D3D12Context& d3d;
    explicit Impl(D3D12Context& context) : d3d(context) {}

    bool ready = false;
    std::string reason = "Not initialized";
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, kPsoCount> pso;
    ComPtr<ID3D12DescriptorHeap> heap;
    uint32_t descSize = 0;

    ComPtr<ID3D12Resource> weights, latents, reference;           // static, NON_PIXEL_SHADER_RESOURCE
    ComPtr<ID3D12Resource> params, adam, grads, stats, ntcOut;    // UNORDERED_ACCESS
    ComPtr<ID3D12Resource> nrcOut;                                // UNORDERED_ACCESS, half canvas size
    ComPtr<ID3D12Resource> mlpF16;                                // static FP16 matrices, NON_PIXEL_SHADER_RESOURCE
    ComPtr<ID3D12Resource> nrcF16;                                // cache FP16 matrices, packed every frame
    D3D12_RESOURCE_STATES nrcF16State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    std::array<ComPtr<ID3D12PipelineState>, kTcCount> tensorPso;
    bool tensorReady = false;
    std::string tensorReason = "Built without the D3D12 Agility SDK preview";
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> readback;                              // per slot: timestamps then stats words
    uint64_t timestampHz = 0;

    ID3D12Resource* boundHdr = nullptr;
    ID3D12Resource* boundDepth = nullptr;
    uint32_t nrcW = 0, nrcH = 0;
    bool cacheNeedsInit = true;
    uint32_t adamStep = 0;

    struct SlotRecord { bool valid = false; bool ran[kNeuralFeatureCount]{}; uint32_t pixels = 0; uint32_t cachePixels = 0; };
    std::array<SlotRecord, kSlots> slotRecord{};

    NeuralRenderingStats published{};
    float shadeOffline = 0.0f, textureOffline = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint32_t i) const {
        auto h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * descSize;
        return h;
    }

    bool fail(std::string why, std::string* error) {
        reason = std::move(why);
        if (error) *error = reason;
        BOO_LOG_WARN("Neural", "Neural rendering unavailable: " + reason);
        return false;
    }

    bool createBuffer(ComPtr<ID3D12Resource>& out, uint64_t bytes, bool uav, D3D12_RESOURCE_STATES state, D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT) {
        const auto hp = heapProps(type);
        const auto desc = bufferDesc(bytes, uav);
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out)));
    }

    void bufferSrv(ID3D12Resource* r, uint32_t slot, uint32_t elements, uint32_t stride, bool raw) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
        d.Buffer.NumElements = elements;
        d.Buffer.StructureByteStride = raw ? 0 : stride;
        d.Buffer.Flags = raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(r, &d, cpu(slot));
    }

    void bufferUav(ID3D12Resource* r, uint32_t slot, uint32_t elements, uint32_t stride, bool raw) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
        d.Buffer.NumElements = elements;
        d.Buffer.StructureByteStride = raw ? 0 : stride;
        d.Buffer.Flags = raw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
        device->CreateUnorderedAccessView(r, nullptr, &d, cpu(slot));
    }

    void textureUav(ID3D12Resource* r, uint32_t slot, DXGI_FORMAT format) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Format = format;
        device->CreateUnorderedAccessView(r, nullptr, &d, cpu(slot));
    }

    bool initialize(std::string* error);
    void probeTensorCores();
    bool bindTargets(ID3D12Resource* hdr, ID3D12Resource* depth, uint32_t w, uint32_t h);
    void collect(uint32_t slot);
};

bool NeuralRenderingSuite::Impl::initialize(std::string* error) {
    device = static_cast<ID3D12Device*>(d3d.deviceRaw());
    if (!device) return fail("No D3D12 device", error);
    // The composite and cache-training shaders read 16-bit float and 8-bit images in compute, an optional ability.
    D3D12_FEATURE_DATA_D3D12_OPTIONS abilities{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &abilities, sizeof(abilities))) ||
        !abilities.TypedUAVLoadAdditionalFormats) {
        return fail("This graphics chip cannot read these image formats in compute shaders", error);
    }

    // Trained networks
    startupStage(StartupWork::Files, "neural_networks.bin");
    std::ifstream file(resolveResourcePath("assets/neural/neural_networks.bin"), std::ios::binary);
    const std::vector<char> blob((std::istreambuf_iterator<char>(file)), {});
    startupAssetLoaded(resolveResourcePath("assets/neural/neural_networks.bin"));
    constexpr size_t kHeader = 4 + 7 * 4 + 2 * 4;
    constexpr size_t kWeightsBytes = (2u * kMlpParams + 2u * kLatentChannels) * 4u;
    constexpr size_t kLatentBytes = kLatent * kLatent * kLatentChannels;
    constexpr size_t kReferenceBytes = kTex * kTex * 4u;
    if (blob.size() != kHeader + kWeightsBytes + kLatentBytes + kReferenceBytes || std::memcmp(blob.data(), "BNRL", 4) != 0)
        return fail("assets/neural/neural_networks.bin is missing or has the wrong size", error);
    uint32_t header[7]{};
    std::memcpy(header, blob.data() + 4, sizeof(header));
    if (header[0] != 1u || header[1] != kShadeIn || header[2] != kHidden || header[3] != 3u || header[4] != kTex ||
        header[5] != kLatent || header[6] != kLatentChannels)
        return fail("assets/neural/neural_networks.bin has an unexpected layout", error);
    std::memcpy(&shadeOffline, blob.data() + 32, 4);
    std::memcpy(&textureOffline, blob.data() + 36, 4);
    const char* weightsSrc = blob.data() + kHeader;

    // Pipelines
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvCount, 0, 0, 0},
                                        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavCount, 0, 0, kSrvCount}};
    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants = {0, 0, sizeof(NeuralConstants) / 4};
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC rsDesc{2, rootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> rsBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr)) ||
        FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&root))))
        return fail("Root signature creation failed", error);
    // On a cold shader cache (the first start after an update) these take about 8 s one after another,
    // the last wait before the film; side by side they take as long as the slowest, and the loading
    // bar keeps moving while they compile.
    std::vector<std::vector<char>> code(kPsoCount);
    for (uint32_t i = 0; i < kPsoCount; ++i) {
        startupReading(resolveResourcePath(kEntryFiles[i]));
        std::ifstream cso(resolveResourcePath(kEntryFiles[i]), std::ios::binary);
        code[i].assign(std::istreambuf_iterator<char>(cso), {});
        startupAssetLoaded(resolveResourcePath(kEntryFiles[i]));
        if (code[i].empty()) return fail(std::string("Missing shader ") + kEntryFiles[i], error);
    }
    StartupProgress::instance().stageDone();
    {
        PipelineCompiler compiler;
        for (uint32_t i = 0; i < kPsoCount; ++i) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
            d.pRootSignature = root.Get();
            d.CS = {code[i].data(), code[i].size()};
            ID3D12Device* dev = device.Get();
            ComPtr<ID3D12PipelineState>* out = &pso[i];
            compiler.start(std::string("neural ") + kEntryFiles[i], code[i].size(), [dev, d, out] {
                return SUCCEEDED(dev->CreateComputePipelineState(&d, IID_PPV_ARGS(out->ReleaseAndGetAddressOf()))) && *out;
            });
        }
        if (!compiler.waitAll()) {
            for (uint32_t i = 0; i < kPsoCount; ++i)
                if (!pso[i]) return fail(std::string("Pipeline creation failed for ") + kEntryFiles[i], error);
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kDescCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) return fail("Descriptor heap creation failed", error);
    descSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Resources
    const auto defaultHeap = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    const auto ntcDesc = textureDesc(kTex, kTex, DXGI_FORMAT_R8G8B8A8_UNORM);
    const D3D12_RESOURCE_STATES uavState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (!createBuffer(weights, kWeightsBytes, false, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !createBuffer(latents, kLatentBytes, false, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !createBuffer(reference, kReferenceBytes, false, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !createBuffer(params, kCacheParams * 4ull, true, uavState) ||
        !createBuffer(adam, kCacheParams * 8ull, true, uavState) ||
        !createBuffer(grads, kCacheParams * 4ull, true, uavState) ||
        !createBuffer(stats, kStatWords * 4ull, true, uavState) ||
        !createBuffer(mlpF16, 2ull * kF16NetBytes, false, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !createBuffer(nrcF16, kF16NetBytes, true, uavState) ||
        !createBuffer(readback, kSlots * (kTimestamps * 8ull + kStatWords * 4ull), false, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK) ||
        FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &ntcDesc, uavState, nullptr, IID_PPV_ARGS(&ntcOut))))
        return fail("GPU buffer allocation failed", error);
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kSlots * kTimestamps;
    if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)))) return fail("Timestamp query heap creation failed", error);
    if (auto* queue = static_cast<ID3D12CommandQueue*>(d3d.commandQueueRaw())) queue->GetTimestampFrequency(&timestampHz);

    std::vector<uint16_t> f16(kF16NetBytes); // two networks x kF16NetBytes / 2 halves
    std::vector<float> floats(2u * kMlpParams);
    std::memcpy(floats.data(), weightsSrc, floats.size() * 4u);
    packF16Net(floats.data(), f16.data());
    packF16Net(floats.data() + kMlpParams, f16.data() + kF16NetBytes / 2u);

    ComPtr<ID3D12Resource> upload;
    const uint64_t blobBytes = kWeightsBytes + kLatentBytes + kReferenceBytes;
    const uint64_t uploadBytes = blobBytes + 2ull * kF16NetBytes;
    if (!createBuffer(upload, uploadBytes, false, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD))
        return fail("Upload buffer allocation failed", error);
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    if (FAILED(upload->Map(0, &none, reinterpret_cast<void**>(&mapped))) || !mapped) return fail("Upload map failed", error);
    const bool blobFits = BOO_MAPPED_WRITE_OK(upload.Get(), "Neural weights upload", 0, blobBytes);
    const bool f16Fits = BOO_MAPPED_WRITE_OK(upload.Get(), "Neural weights upload", blobBytes, 2ull * kF16NetBytes);
    if (blobFits) std::memcpy(mapped, weightsSrc, blobBytes);
    if (f16Fits) std::memcpy(mapped + blobBytes, f16.data(), 2ull * kF16NetBytes);
    upload->Unmap(0, nullptr);
    if (!blobFits || !f16Fits) return fail("Weight upload exceeds buffer", error);
    std::string copyError;
    const bool copied = d3d.executeSingleCommandListAndWait([&](void* raw) {
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(raw);
        cmd->CopyBufferRegion(weights.Get(), 0, upload.Get(), 0, kWeightsBytes);
        cmd->CopyBufferRegion(latents.Get(), 0, upload.Get(), kWeightsBytes, kLatentBytes);
        cmd->CopyBufferRegion(reference.Get(), 0, upload.Get(), kWeightsBytes + kLatentBytes, kReferenceBytes);
        cmd->CopyBufferRegion(mlpF16.Get(), 0, upload.Get(), blobBytes, 2ull * kF16NetBytes);
        for (ID3D12Resource* r : {weights.Get(), latents.Get(), reference.Get(), mlpF16.Get()})
            transition(cmd, r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }, &copyError);
    if (!copied) return fail("Weight upload failed: " + copyError, error);

    bufferSrv(weights.Get(), kDescWeights, 2u * kMlpParams + 2u * kLatentChannels, 4u, false);
    bufferSrv(latents.Get(), kDescLatents, static_cast<uint32_t>(kLatentBytes / 4u), 0u, true);
    bufferSrv(reference.Get(), kDescReference, static_cast<uint32_t>(kReferenceBytes / 4u), 0u, true);
    bufferUav(params.Get(), kDescParams, kCacheParams, 4u, false);
    bufferUav(adam.Get(), kDescAdam, kCacheParams, 8u, false);
    bufferUav(grads.Get(), kDescGrad, kCacheParams, 4u, false);
    textureUav(ntcOut.Get(), kDescNtcOut, DXGI_FORMAT_R8G8B8A8_UNORM);
    bufferUav(stats.Get(), kDescStats, kStatWords, 0u, true);
    bufferSrv(mlpF16.Get(), kDescMlpF16, 2u * kF16NetBytes / 4u, 0u, true);
    bufferSrv(nrcF16.Get(), kDescNrcF16, kF16NetBytes / 4u, 0u, true);
    bufferUav(nrcF16.Get(), kDescNrcF16Out, kF16NetBytes / 4u, 0u, true);
    probeTensorCores();

    published.available = true;
    published.backend = d3d.isWarp() ? "HLSL SM 6.0 FP32 (WARP software)" : "HLSL SM 6.0 FP32 shader cores";
    published.tensorCoresAvailable = tensorReady;
    published.tensorCoresReason = tensorReady ? std::string() : tensorReason;
    published.textureCompressionRatio = static_cast<float>(
        double(kReferenceBytes) / double(kLatentBytes + kMlpParams * 4u + 2u * kLatentChannels * 4u));
    published.offlineShadingPsnr = shadeOffline;
    published.offlineTexturePsnr = textureOffline;
    published.cacheSamplesPerFrame = kCacheSamplesPerFrame;
    published.cacheParameters = kCacheParams;
    for (auto& f : published.features) f.state = RendererFeatureState::Off;
    reason.clear();
    ready = true;
    BOO_LOG_INFO("Neural", tensorReady ? std::string("Tensor-core path ready: SM 6.10 LinAlg FP16 thread-scope matrices")
                                       : "Tensor-core path unavailable: " + tensorReason);
    BOO_LOG_INFO("Neural", "Neural rendering ready (" + published.backend + "); offline shading PSNR " +
        std::to_string(shadeOffline) + " dB, texture PSNR " + std::to_string(textureOffline) + " dB");
    return true;
}

void NeuralRenderingSuite::Impl::probeTensorCores() {
#ifdef BOO_HAS_LINALG
    if (d3d.isWarp()) { tensorReason = "WARP has no tensor cores"; return; }
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_10};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_10) {
        tensorReason = "Shader Model 6.10 unavailable (needs Windows Developer Mode and a preview driver)";
        return;
    }
    D3D12_FEATURE_DATA_LINEAR_ALGEBRA_SUPPORT tier{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_SUPPORT, &tier, sizeof(tier))) ||
        tier.LinearAlgebraTier < D3D12_LINEAR_ALGEBRA_TIER_1_0) {
        tensorReason = "Driver reports no D3D12 linear algebra support";
        return;
    }
    D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT op{};
    op.OperationType = D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREAD_VECTOR_MATRIX_MULTIPLY;
    op.ThreadVectorMatrixMultiply.VectorInputType = D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
    op.ThreadVectorMatrixMultiply.MatrixInputType = D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
    op.ThreadVectorMatrixMultiply.BiasInputType = D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
    op.ThreadVectorMatrixMultiply.VectorResultType = D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT, &op, sizeof(op))) ||
        !(op.ThreadVectorMatrixMultiply.SupportFlags & D3D12_LINEAR_ALGEBRA_MULTIPLICATION_SUPPORT_FLAG_SUPPORTED)) {
        tensorReason = "Driver does not support FP16 thread vector x matrix multiply";
        return;
    }
    for (uint32_t i = 0; i < kTcCount; ++i) {
        std::ifstream cso(resolveResourcePath(kTensorFiles[i]), std::ios::binary);
        const std::vector<char> code((std::istreambuf_iterator<char>(cso)), {});
        startupAssetLoaded(resolveResourcePath(kTensorFiles[i]));
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root.Get();
        d.CS = {code.data(), code.size()};
        if (code.empty() || FAILED(compileWhilePainting(kTensorFiles[i], code.size(), [&] {
                return device->CreateComputePipelineState(&d, IID_PPV_ARGS(&tensorPso[i]));
            }))) {
            tensorReason = std::string("Tensor pipeline failed for ") + kTensorFiles[i];
            return;
        }
    }
    tensorReady = true;
#endif
}

bool enableNeuralPreviewFeatures() noexcept {
#ifdef BOO_HAS_LINALG
    // Must run before any D3D12 device exists. Without Developer Mode this fails and the
    // shader-core path is used.
    UUID features[] = {D3D12ExperimentalShaderModels};
    const HRESULT hr = D3D12EnableExperimentalFeatures(1, features, nullptr, nullptr);
    BOO_LOG_INFO("Neural", SUCCEEDED(hr) ? std::string("D3D12 experimental shader models enabled (SM 6.10 preview)")
                                         : "D3D12 experimental shader models refused; enable Windows Developer Mode for tensor cores");
    return SUCCEEDED(hr);
#else
    return false;
#endif
}

bool NeuralRenderingSuite::Impl::bindTargets(ID3D12Resource* hdr, ID3D12Resource* depth, uint32_t w, uint32_t h) {
    const uint32_t halfW = (w + 1u) / 2u, halfH = (h + 1u) / 2u;
    if (hdr == boundHdr && depth == boundDepth && halfW == nrcW && halfH == nrcH) return true;
    // Descriptors in use by frames still in flight must not change under them.
    if (!d3d.waitForGpu()) return false;
    if (halfW != nrcW || halfH != nrcH) {
        nrcOut.Reset();
        const auto hp = heapProps(D3D12_HEAP_TYPE_DEFAULT);
        const auto desc = textureDesc(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   nullptr, IID_PPV_ARGS(&nrcOut)))) {
            nrcW = nrcH = 0;
            return false;
        }
        textureUav(nrcOut.Get(), kDescNrcOut, DXGI_FORMAT_R16G16B16A16_FLOAT);
        nrcW = halfW;
        nrcH = halfH;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(depth, &srv, cpu(kDescDepth));
    textureUav(hdr, kDescHdr, DXGI_FORMAT_R16G16B16A16_FLOAT);
    boundHdr = hdr;
    boundDepth = depth;
    return true;
}

void NeuralRenderingSuite::Impl::collect(uint32_t slot) {
    SlotRecord& rec = slotRecord[slot];
    if (!rec.valid) return;
    rec.valid = false;
    const uint64_t base = slot * (kTimestamps * 8ull + kStatWords * 4ull);
    D3D12_RANGE range{static_cast<SIZE_T>(base), static_cast<SIZE_T>(base + kTimestamps * 8ull + kStatWords * 4ull)};
    uint8_t* mapped = nullptr;
    if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped) return;
    uint64_t ts[kTimestamps];
    uint32_t words[kStatWords];
    std::memcpy(ts, mapped + base, sizeof(ts));
    std::memcpy(words, mapped + base + sizeof(ts), sizeof(words));
    D3D12_RANGE nothing{0, 0};
    readback->Unmap(0, &nothing);

    auto ms = [&](uint32_t a, uint32_t b) {
        return (timestampHz && ts[b] > ts[a]) ? static_cast<float>(double(ts[b] - ts[a]) * 1000.0 / double(timestampHz)) : 0.0f;
    };
    const float featureMs[kNeuralFeatureCount] = {ms(2, 3), ms(1, 2), ms(0, 1)};
    const double gflop[kNeuralFeatureCount] = {
        rec.pixels * kMlpFlopPerEval * 1e-9,
        double(kTex) * kTex * kMlpFlopPerEval * 1e-9,
        (kCacheSamplesPerFrame * kCacheTrainFlopPerSample + rec.cachePixels * kCacheFlopPerEval) * 1e-9};
    float quality[kNeuralFeatureCount] = {
        words[1] ? psnrFromMse(words[0] / kStatScale / words[1]) : 0.0f,
        words[3] ? psnrFromMse(words[2] / kStatScale / words[3]) : 0.0f,
        words[5] ? psnrFromMse(words[4] / kStatScale / words[5]) : 0.0f};
    for (uint32_t f = 0; f < kNeuralFeatureCount; ++f) {
        NeuralFeatureStats& s = published.features[f];
        if (!rec.ran[f]) continue;
        const float k = s.gpuMs > 0.0f ? 0.1f : 1.0f; // Smoothed so the dashboard reads steadily
        s.gpuMs += (featureMs[f] - s.gpuMs) * k;
        s.quality += (quality[f] - s.quality) * (s.quality > 0.0f ? 0.1f : 1.0f);
        s.gflopPerFrame = static_cast<float>(gflop[f]);
        s.tflops = s.gpuMs > 0.0f ? static_cast<float>(gflop[f] / s.gpuMs) : 0.0f; // GFLOP per ms == TFLOPS
    }
}

NeuralRenderingSuite::NeuralRenderingSuite(D3D12Context& d3d) : impl_(std::make_unique<Impl>(d3d)) {}
NeuralRenderingSuite::~NeuralRenderingSuite() = default;

bool NeuralRenderingSuite::initialize(std::string* error) {
    if (impl_->ready) return true;
    impl_->ready = false;
    const bool ok = impl_->initialize(error);
    if (!ok) {
        impl_->published = NeuralRenderingStats{};
        impl_->published.unavailableReason = impl_->reason;
    }
    return ok;
}

bool NeuralRenderingSuite::ready() const noexcept { return impl_->ready; }

void NeuralRenderingSuite::skip(std::string reason) {
    if (impl_->ready) return;
    impl_->published = NeuralRenderingStats{};
    impl_->published.unavailableReason = std::move(reason);
}

void NeuralRenderingSuite::resetCache() noexcept {
    impl_->cacheNeedsInit = true;
    impl_->adamStep = 0;
    impl_->published.cacheTrainingSteps = 0;
}

void NeuralRenderingSuite::shutdown() noexcept {
    auto& d3d = impl_->d3d;
    impl_ = std::make_unique<Impl>(d3d);
}

NeuralRenderingStats NeuralRenderingSuite::stats() const { return impl_->published; }

void NeuralRenderingSuite::record(void* commandList, void* hdrRaw, void* depthRaw, uint32_t width, uint32_t height,
                                  const NeuralRenderingControls& controls, uint64_t frameIndex, uint32_t slot, bool canonical) {
    Impl& m = *impl_;
    const bool runShading = controls.runs(NeuralFeature::Shading);
    const bool runTextures = controls.runs(NeuralFeature::Textures);
    const bool runCache = controls.runs(NeuralFeature::RadianceCache);
    const uint32_t solo = static_cast<uint32_t>(controls.solo);
    const bool tensor = m.tensorReady && controls.tensorCores;
    if (m.ready && canonical) {
        m.published.tensorCoresActive = tensor;
        m.published.backend = tensor ? "SM 6.10 LinAlg FP16 tensor cores (D3D12 preview)"
                                     : (m.d3d.isWarp() ? "HLSL SM 6.0 FP32 (WARP software)" : "HLSL SM 6.0 FP32 shader cores");
        m.published.features[0].state = runShading ? RendererFeatureState::Active : RendererFeatureState::Off;
        m.published.features[1].state = runTextures ? RendererFeatureState::Active : RendererFeatureState::Off;
        m.published.features[2].state = runCache ? RendererFeatureState::Active : RendererFeatureState::Off;
    }
    if (!m.ready || !commandList || !hdrRaw || !depthRaw || width == 0 || height == 0) return;
    if (!runShading && !runTextures && !runCache) return;

    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(commandList);
    auto* hdr = static_cast<ID3D12Resource*>(hdrRaw);
    auto* depth = static_cast<ID3D12Resource*>(depthRaw);
    const bool timed = canonical && slot < kSlots;
    if (timed) m.collect(slot);
    if (!m.bindTargets(hdr, depth, width, height)) return;

    ID3D12DescriptorHeap* heaps[] = {m.heap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m.root.Get());
    cmd->SetComputeRootDescriptorTable(1, m.heap->GetGPUDescriptorHandleForHeapStart());
    transition(cmd, hdr, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    NeuralConstants c{};
    c.canvasW = width;
    c.canvasH = height;
    c.frame = static_cast<uint32_t>(frameIndex);
    c.flags = (runShading ? 1u : 0u) | ((solo & 3u) << 1u) | (timed ? 8u : 0u);
    c.learnRate = 1e-2f;
    c.distanceScale = 4.0f;
    auto dispatch = [&](uint32_t which, uint32_t x, uint32_t y) {
        ID3D12PipelineState* chosen = m.pso[which].Get();
        if (tensor && which == kPsoNtc) chosen = m.tensorPso[kTcNtc].Get();
        if (tensor && which == kPsoInfer) chosen = m.tensorPso[kTcInfer].Get();
        if (tensor && which == kPsoComposite) chosen = m.tensorPso[kTcComposite].Get();
        cmd->SetPipelineState(chosen);
        cmd->SetComputeRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
        cmd->Dispatch(x, y, 1);
        uavBarrier(cmd);
    };
    const uint32_t qBase = slot * kTimestamps;
    auto stamp = [&](uint32_t i) { if (timed) cmd->EndQuery(m.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, qBase + i); };

    if (timed) dispatch(kPsoClear, 1, 1);
    stamp(0);
    // The cache only learns on canonical frames; A/B and capture passes reuse what it has learned.
    if (runCache && canonical) {
        if (m.cacheNeedsInit) {
            dispatch(kPsoInit, (kCacheParams + 63u) / 64u, 1);
            m.cacheNeedsInit = false;
            m.adamStep = 0;
        }
        c.adamStep = ++m.adamStep;
        dispatch(kPsoTrain, kCacheSamplesPerFrame / 128u, 1);
        dispatch(kPsoUpdate, (kCacheParams + 63u) / 64u, 1);
        if (tensor) {
            // Repack the just-updated weights as FP16 matrices for the tensor-core query.
            if (m.nrcF16State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                transition(cmd, m.nrcF16.Get(), m.nrcF16State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch(kPsoPack, (kF16NetBytes / 4u + 63u) / 64u, 1);
            transition(cmd, m.nrcF16.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m.nrcF16State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        dispatch(kPsoInfer, (m.nrcW + 7u) / 8u, (m.nrcH + 7u) / 8u);
        m.published.cacheTrainingSteps = m.adamStep;
    }
    stamp(1);
    if (runTextures && canonical) dispatch(kPsoNtc, kTex / 8u, kTex / 8u);
    stamp(2);
    if (runShading || solo != 0u) dispatch(kPsoComposite, (width + 7u) / 8u, (height + 7u) / 8u);
    stamp(3);

    transition(cmd, depth, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(cmd, hdr, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (timed) {
        const uint64_t base = slot * (kTimestamps * 8ull + kStatWords * 4ull);
        cmd->ResolveQueryData(m.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, qBase, kTimestamps, m.readback.Get(), base);
        transition(cmd, m.stats.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(m.readback.Get(), base + kTimestamps * 8ull, m.stats.Get(), 0, kStatWords * 4ull);
        transition(cmd, m.stats.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Impl::SlotRecord& rec = m.slotRecord[slot];
        rec.valid = true;
        rec.ran[0] = runShading;
        rec.ran[1] = runTextures;
        rec.ran[2] = runCache;
        rec.pixels = width * height;
        rec.cachePixels = m.nrcW * m.nrcH;
    }
}

} // namespace boo
