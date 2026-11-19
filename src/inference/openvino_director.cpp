#include "boo/director.hpp"
#include "boo/logging.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace boo {

namespace {

// Minimal opaque C structs matching openvino/c/*.h ABI
struct ov_core;
struct ov_model;
struct ov_compiled_model;
struct ov_infer_request;
struct ov_tensor;

struct ov_available_devices_t {
    char** devices = nullptr;
    size_t size = 0;
};

using OvGetErrorInfoFn                  = const char*(__cdecl*)(int);
using OvFreeFn                          = void(__cdecl*)(const char*);
using OvCoreCreateFn                    = int(__cdecl*)(ov_core**);
using OvCoreFreeFn                      = void(__cdecl*)(ov_core*);
using OvCoreGetAvailableDevicesFn       = int(__cdecl*)(const ov_core*, ov_available_devices_t*);
using OvAvailableDevicesFreeFn          = void(__cdecl*)(ov_available_devices_t*);
using OvCoreGetPropertyFn               = int(__cdecl*)(const ov_core*, const char*, const char*, char**);
using OvCoreReadModelUnicodeFn          = int(__cdecl*)(const ov_core*, const wchar_t*, const wchar_t*, ov_model**);
using OvCoreReadModelFn                 = int(__cdecl*)(const ov_core*, const char*, const char*, ov_model**);
using OvModelFreeFn                     = void(__cdecl*)(ov_model*);
using OvCoreCompileModelPropsFn         = int(__cdecl*)(const ov_core*, const ov_model*, const char*, size_t, const void*, ov_compiled_model**);
using OvCoreCompileModelVariadicFn      = int(__cdecl*)(const ov_core*, const ov_model*, const char*, size_t, ov_compiled_model**, ...);
using OvCompiledModelFreeFn             = void(__cdecl*)(ov_compiled_model*);
using OvCompiledModelCreateInferReqFn   = int(__cdecl*)(const ov_compiled_model*, ov_infer_request**);
using OvInferRequestFreeFn              = void(__cdecl*)(ov_infer_request*);
using OvInferRequestGetInputTensorFn    = int(__cdecl*)(const ov_infer_request*, ov_tensor**);
using OvInferRequestGetOutputTensorFn   = int(__cdecl*)(const ov_infer_request*, ov_tensor**);
using OvTensorDataFn                    = int(__cdecl*)(const ov_tensor*, void**);
using OvTensorGetSizeFn                 = int(__cdecl*)(const ov_tensor*, size_t*);
using OvTensorFreeFn                    = void(__cdecl*)(ov_tensor*);
using OvInferRequestStartAsyncFn        = int(__cdecl*)(ov_infer_request*);
using OvInferRequestWaitForFn           = int(__cdecl*)(ov_infer_request*, int64_t);
using OvInferRequestCancelFn            = int(__cdecl*)(ov_infer_request*);

struct OpenVinoCDispatch {
    HMODULE module = nullptr;
    OvGetErrorInfoFn getErrorInfo = nullptr;
    OvFreeFn freeStr = nullptr;
    OvCoreCreateFn coreCreate = nullptr;
    OvCoreFreeFn coreFree = nullptr;
    OvCoreGetAvailableDevicesFn getAvailableDevices = nullptr;
    OvAvailableDevicesFreeFn availableDevicesFree = nullptr;
    OvCoreGetPropertyFn getProperty = nullptr;
    OvCoreReadModelUnicodeFn readModelUnicode = nullptr;
    OvCoreReadModelFn readModelUtf8 = nullptr;
    OvModelFreeFn modelFree = nullptr;
    OvCoreCompileModelPropsFn compileModelProps = nullptr;
    OvCoreCompileModelVariadicFn compileModelVariadic = nullptr;
    OvCompiledModelFreeFn compiledModelFree = nullptr;
    OvCompiledModelCreateInferReqFn createInferRequest = nullptr;
    OvInferRequestFreeFn inferRequestFree = nullptr;
    OvInferRequestGetInputTensorFn getInputTensor = nullptr;
    OvInferRequestGetOutputTensorFn getOutputTensor = nullptr;
    OvTensorDataFn tensorData = nullptr;
    OvTensorGetSizeFn tensorGetSize = nullptr;
    OvTensorFreeFn tensorFree = nullptr;
    OvInferRequestStartAsyncFn startAsync = nullptr;
    OvInferRequestWaitForFn waitFor = nullptr;
    OvInferRequestCancelFn cancel = nullptr;

    bool valid() const noexcept {
        return module && coreCreate && coreFree && getAvailableDevices && availableDevicesFree &&
               (readModelUnicode || readModelUtf8) && modelFree &&
               (compileModelProps || compileModelVariadic) && compiledModelFree &&
               createInferRequest && inferRequestFree &&
               getInputTensor && getOutputTensor && tensorData && tensorGetSize && tensorFree &&
               startAsync && waitFor;
    }
};

std::filesystem::path executableDirectory() {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(buf).parent_path();
}

std::vector<std::filesystem::path> candidateOpenVinoDllPaths(bool allowDevVenvDiscovery) {
    std::vector<std::filesystem::path> out;
    const auto exeDir = executableDirectory();
    out.push_back(exeDir / "openvino_c.dll");
    out.push_back(exeDir / "openvino" / "openvino_c.dll");

    if (allowDevVenvDiscovery) {
        const auto venvRel = std::filesystem::path(".venv") / "Lib" / "site-packages" / "openvino" / "libs" / "openvino_c.dll";
        const auto resolved = resolveResourcePath(venvRel);
        out.push_back(resolved);
    }
    return out;
}

// Process-lifetime singleton loader for openvino_c.dll so TBB/LevelZero TLS pools are never unloaded/reloaded mid-process.
const OpenVinoCDispatch* acquireOpenVinoDispatch(bool allowDevVenvDiscovery) noexcept {
    static std::mutex sMutex;
    static OpenVinoCDispatch sDispatch{};
    static bool sLoadedWithoutVenv = false;

    std::lock_guard<std::mutex> lk(sMutex);
    if (sDispatch.valid()) {
        if (allowDevVenvDiscovery || sLoadedWithoutVenv) {
            return &sDispatch;
        }
    }

    HMODULE hMod = nullptr;
    bool loadedNoVenv = false;

    // 1. Explicit app-local / dev-venv paths (with DLL_LOAD_DIR so sibling openvino.dll & tbb12.dll resolve cleanly)
    const auto candidates = candidateOpenVinoDllPaths(allowDevVenvDiscovery);
    for (size_t i = 0; i < candidates.size(); ++i) {
        std::error_code ec;
        if (!std::filesystem::exists(candidates[i], ec)) continue;
        const auto absPath = std::filesystem::weakly_canonical(candidates[i], ec);
        const std::wstring wpath = ec ? candidates[i].wstring() : absPath.wstring();
        hMod = LoadLibraryExW(wpath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (hMod) {
            loadedNoVenv = (i < 2u);
            break;
        }
    }

    // 2. Standard system/PATH lookup
    if (!hMod) {
        hMod = LoadLibraryExW(L"openvino_c.dll", nullptr, 0);
        if (hMod) loadedNoVenv = true;
    }

    if (!hMod) {
        return nullptr;
    }

    OpenVinoCDispatch d{};
    d.module               = hMod;
    d.getErrorInfo         = reinterpret_cast<OvGetErrorInfoFn>(GetProcAddress(hMod, "ov_get_error_info"));
    d.freeStr              = reinterpret_cast<OvFreeFn>(GetProcAddress(hMod, "ov_free"));
    d.coreCreate           = reinterpret_cast<OvCoreCreateFn>(GetProcAddress(hMod, "ov_core_create"));
    d.coreFree             = reinterpret_cast<OvCoreFreeFn>(GetProcAddress(hMod, "ov_core_free"));
    d.getAvailableDevices  = reinterpret_cast<OvCoreGetAvailableDevicesFn>(GetProcAddress(hMod, "ov_core_get_available_devices"));
    d.availableDevicesFree = reinterpret_cast<OvAvailableDevicesFreeFn>(GetProcAddress(hMod, "ov_available_devices_free"));
    d.getProperty          = reinterpret_cast<OvCoreGetPropertyFn>(GetProcAddress(hMod, "ov_core_get_property"));
    d.readModelUnicode     = reinterpret_cast<OvCoreReadModelUnicodeFn>(GetProcAddress(hMod, "ov_core_read_model_unicode"));
    d.readModelUtf8        = reinterpret_cast<OvCoreReadModelFn>(GetProcAddress(hMod, "ov_core_read_model"));
    d.modelFree            = reinterpret_cast<OvModelFreeFn>(GetProcAddress(hMod, "ov_model_free"));
    d.compileModelProps    = reinterpret_cast<OvCoreCompileModelPropsFn>(GetProcAddress(hMod, "ov_core_compile_model_props"));
    d.compileModelVariadic = reinterpret_cast<OvCoreCompileModelVariadicFn>(GetProcAddress(hMod, "ov_core_compile_model"));
    d.compiledModelFree    = reinterpret_cast<OvCompiledModelFreeFn>(GetProcAddress(hMod, "ov_compiled_model_free"));
    d.createInferRequest   = reinterpret_cast<OvCompiledModelCreateInferReqFn>(GetProcAddress(hMod, "ov_compiled_model_create_infer_request"));
    d.inferRequestFree     = reinterpret_cast<OvInferRequestFreeFn>(GetProcAddress(hMod, "ov_infer_request_free"));
    d.getInputTensor       = reinterpret_cast<OvInferRequestGetInputTensorFn>(GetProcAddress(hMod, "ov_infer_request_get_input_tensor"));
    d.getOutputTensor      = reinterpret_cast<OvInferRequestGetOutputTensorFn>(GetProcAddress(hMod, "ov_infer_request_get_output_tensor"));
    d.tensorData           = reinterpret_cast<OvTensorDataFn>(GetProcAddress(hMod, "ov_tensor_data"));
    d.tensorGetSize        = reinterpret_cast<OvTensorGetSizeFn>(GetProcAddress(hMod, "ov_tensor_get_size"));
    d.tensorFree           = reinterpret_cast<OvTensorFreeFn>(GetProcAddress(hMod, "ov_tensor_free"));
    d.startAsync           = reinterpret_cast<OvInferRequestStartAsyncFn>(GetProcAddress(hMod, "ov_infer_request_start_async"));
    d.waitFor              = reinterpret_cast<OvInferRequestWaitForFn>(GetProcAddress(hMod, "ov_infer_request_wait_for"));
    d.cancel               = reinterpret_cast<OvInferRequestCancelFn>(GetProcAddress(hMod, "ov_infer_request_cancel"));

    if (!d.valid()) {
        FreeLibrary(hMod);
        return nullptr;
    }

    sDispatch = d;
    if (loadedNoVenv) sLoadedWithoutVenv = true;
    return &sDispatch;
}

float clamp01(float v) noexcept {
    return std::clamp(v, 0.0f, 1.0f);
}

std::string queryDeviceFullName(const OpenVinoCDispatch& ov, const ov_core* core, const char* dev) {
    if (!ov.getProperty || !ov.freeStr) return dev;
    char* val = nullptr;
    if (ov.getProperty(core, dev, "FULL_DEVICE_NAME", &val) == 0 && val) {
        std::string out(val);
        ov.freeStr(val);
        if (!out.empty()) return out;
    }
    return dev;
}

std::string statusToString(const OpenVinoCDispatch& ov, int st) {
    if (ov.getErrorInfo) {
        const char* info = ov.getErrorInfo(st);
        if (info && *info) return std::string(info) + " (" + std::to_string(st) + ")";
    }
    return "ov_status=" + std::to_string(st);
}

} // namespace

bool isOpenVinoRuntimeAvailable(bool allowDevVenvDiscovery) noexcept {
    for (const auto& cand : candidateOpenVinoDllPaths(allowDevVenvDiscovery)) {
        std::error_code ec;
        if (std::filesystem::exists(cand, ec)) {
            return true;
        }
    }
    HMODULE hMod = LoadLibraryExW(L"openvino_c.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (hMod) {
        FreeLibrary(hMod);
        return true;
    }
    return false;
}

struct OpenVinoDirector::Impl {
    const OpenVinoCDispatch* ov = nullptr;
    ov_core* core = nullptr;
    ov_model* model = nullptr;
    ov_compiled_model* compiled = nullptr;
    ov_infer_request* inferReq = nullptr;

    std::string activeDevice;
    std::string activeDeviceFullName;
    int64_t inferTimeoutMs = 250;
    bool ready = false;
    bool usingHardwareNpu = false;
    HardwareResult hwResult{};

    void releaseHandles() noexcept {
        ready = false;
        usingHardwareNpu = false;
        if (ov) {
            if (inferReq) {
                if (ov->cancel) (void)ov->cancel(inferReq);
                ov->inferRequestFree(inferReq);
                inferReq = nullptr;
            }
            if (compiled) {
                ov->compiledModelFree(compiled);
                compiled = nullptr;
            }
            if (model) {
                ov->modelFree(model);
                model = nullptr;
            }
            if (core) {
                ov->coreFree(core);
                core = nullptr;
            }
        }
    }

    bool executeSingleInference(
        const DirectorFeatureVector& fv,
        NeuralInferenceSample& outSample,
        int64_t timeoutMs,
        bool injectTimeout,
        NeuralInferenceFailure& failure) {
        failure = {};
        if (!ov || !inferReq) {
            failure.reason = "Inference request is not initialized.";
            return false;
        }
        if (!std::all_of(fv.values.begin(), fv.values.end(), [](float value) { return std::isfinite(value); })) {
            failure.reason = "Director input contains a non-finite feature.";
            return false;
        }

        ov_tensor* inTensor = nullptr;
        const int inputStatus = ov->getInputTensor(inferReq, &inTensor);
        if (inputStatus != 0 || !inTensor) {
            failure.reason = "Input tensor unavailable: " + statusToString(*ov, inputStatus);
            return false;
        }
        size_t inCount = 0;
        void* inPtr = nullptr;
        const bool inOk = (ov->tensorGetSize(inTensor, &inCount) == 0) &&
                          (inCount == 16u) &&
                          (ov->tensorData(inTensor, &inPtr) == 0) &&
                          (inPtr != nullptr);
        if (!inOk) {
            ov->tensorFree(inTensor);
            failure.reason = "Input tensor is unreadable or does not contain 16 values.";
            return false;
        }
        std::memcpy(inPtr, fv.values.data(), sizeof(float) * 16u);
        ov->tensorFree(inTensor);

        const auto t0 = std::chrono::steady_clock::now();
        if (injectTimeout) {
            failure.timedOut = true;
            failure.reason = "Injected inference timeout (--inject-npu-timeout).";
            return false;
        }

        const int startSt = ov->startAsync(inferReq);
        if (startSt != 0) {
            failure.reason = "Could not start inference: " + statusToString(*ov, startSt);
            return false;
        }
        const int waitSt = ov->waitFor(inferReq, timeoutMs);
        if (waitSt != 0) {
            constexpr int kOvResultNotReady = -9; // ov_status_e::RESULT_NOT_READY from the C API.
            failure.timedOut = waitSt == kOvResultNotReady;
            failure.reason = (failure.timedOut
                ? "Inference did not complete within " + std::to_string(timeoutMs) + " ms: "
                : "Inference wait failed: ") + statusToString(*ov, waitSt);
            if (ov->cancel) (void)ov->cancel(inferReq);
            return false;
        }
        const auto t1 = std::chrono::steady_clock::now();
        const float elapsedMs = static_cast<float>(
            std::chrono::duration<double, std::milli>(t1 - t0).count());

        ov_tensor* outTensor = nullptr;
        const int outputStatus = ov->getOutputTensor(inferReq, &outTensor);
        if (outputStatus != 0 || !outTensor) {
            failure.reason = "Output tensor unavailable: " + statusToString(*ov, outputStatus);
            return false;
        }
        size_t outCount = 0;
        void* outPtr = nullptr;
        const bool outOk = (ov->tensorGetSize(outTensor, &outCount) == 0) &&
                           (outCount == 10u) &&
                           (ov->tensorData(outTensor, &outPtr) == 0) &&
                           (outPtr != nullptr);
        if (!outOk) {
            ov->tensorFree(outTensor);
            failure.reason = "Output tensor is unreadable or does not contain 10 values.";
            return false;
        }

        std::array<float, 10> raw{};
        std::memcpy(raw.data(), outPtr, sizeof(float) * 10u);
        ov->tensorFree(outTensor);

        for (float v : raw) {
            if (!std::isfinite(v)) {
                failure.reason = "Model returned a non-finite output.";
                return false;
            }
        }

        float probSum = 0.0f;
        uint8_t bestIdx = 0;
        float bestProb = raw[0];
        for (uint8_t c = 0; c < 5u; ++c) {
            if (raw[c] < -0.01f || raw[c] > 1.01f) {
                failure.reason = "Model returned an invalid category probability of " + std::to_string(raw[c]) + ".";
                return false;
            }
            probSum += raw[c];
            if (raw[c] > bestProb) {
                bestProb = raw[c];
                bestIdx = c;
            }
        }
        if (probSum < 0.95f || probSum > 1.05f) {
            failure.reason = "Model category probabilities sum to " + std::to_string(probSum) + " instead of 1.";
            return false;
        }

        outSample.category = static_cast<DirectorCategory>(bestIdx);
        outSample.confidence = clamp01(bestProb);
        for (size_t m = 0; m < 5u; ++m) {
            outSample.controls[m] = clamp01(raw[5u + m]);
        }
        outSample.latencyMs = elapsedMs;
        return true;
    }

    bool tryCompileAndVerifyDevice(const char* devName, std::string& outDetailError) {
        ov_compiled_model* candCompiled = nullptr;
        int compSt = -1;
        if (ov->compileModelProps) {
            compSt = ov->compileModelProps(core, model, devName, 0u, nullptr, &candCompiled);
        } else {
            compSt = ov->compileModelVariadic(core, model, devName, 0u, &candCompiled);
        }
        if (compSt != 0 || !candCompiled) {
            outDetailError = std::string(devName) + " compile failed: " + statusToString(*ov, compSt);
            return false;
        }

        ov_infer_request* candReq = nullptr;
        const int reqSt = ov->createInferRequest(candCompiled, &candReq);
        if (reqSt != 0 || !candReq) {
            ov->compiledModelFree(candCompiled);
            outDetailError = std::string(devName) + " create_infer_request failed: " + statusToString(*ov, reqSt);
            return false;
        }

        inferReq = candReq;
        compiled = candCompiled;

        // Mandatory verification test inference (SPEC_v4 Section 3.5 & 8.1)
        DirectorFeatureVector testVec{};
        testVec.values = {0.45f, 0.50f, 0.42f, 0.50f, 0.35f, 0.65f, 0.50f, 0.0f, 1.0f, 0.0f, 0.48f, 0.68f, 0.20f, 0.32f, 0.15f, 0.5f};
        NeuralInferenceSample verifiedSample{};
        NeuralInferenceFailure verifyErr;
        // The first request can include device setup. Give it a separate cold-start deadline.
        constexpr int64_t kVerifyTimeoutMs = 2000;
        if (!executeSingleInference(testVec, verifiedSample, kVerifyTimeoutMs, false, verifyErr)) {
            ov->inferRequestFree(inferReq);
            inferReq = nullptr;
            ov->compiledModelFree(compiled);
            compiled = nullptr;
            outDetailError = std::string(devName) + " test inference failed: " + verifyErr.reason;
            return false;
        }

        activeDevice = devName;
        activeDeviceFullName = queryDeviceFullName(*ov, core, devName);
        ready = true;
        usingHardwareNpu = (std::strcmp(devName, "NPU") == 0);
        return true;
    }
};

OpenVinoDirector::OpenVinoDirector() : impl_(std::make_unique<Impl>()) {}
OpenVinoDirector::~OpenVinoDirector() { shutdown(); }

HardwareResult OpenVinoDirector::initialize(const OpenVinoDirectorConfig& config) {
    shutdown();
    impl_->inferTimeoutMs = std::max<int64_t>(20, config.inferTimeoutMs);

    HardwareResult res{};
    res.engine = EngineKind::NPU;
    res.requestedBackend = "OpenVINO_NPU";

    if (!config.allowNpu && !config.allowCpuFallback) {
        res.status = BackendStatus::Substituted;
        res.health = HealthState::ActiveSubstitute;
        res.activeBackend = "Scripted_Deterministic_Director";
        res.diagnosticCode = "DISABLED_BY_FLAG";
        res.humanReadableReason = "NPU disabled via --disable-npu; running deterministic Scripted Director.";
        impl_->hwResult = res;
        return res;
    }

    if (config.simulateMissingRuntimeForTest) {
        res.status = BackendStatus::Substituted;
        res.health = HealthState::ActiveSubstitute;
        res.activeBackend = "Scripted_Deterministic_Director";
        res.diagnosticCode = "OPENVINO_RUNTIME_ABSENT";
        res.humanReadableReason = "OpenVINO C runtime absent; running deterministic Scripted Director fallback.";
        impl_->hwResult = res;
        return res;
    }

    impl_->ov = acquireOpenVinoDispatch(config.allowDevVenvDiscovery);
    if (!impl_->ov) {
        res.status = BackendStatus::Substituted;
        res.health = HealthState::ActiveSubstitute;
        res.activeBackend = "Scripted_Deterministic_Director";
        res.diagnosticCode = "OPENVINO_RUNTIME_ABSENT";
        res.humanReadableReason = "openvino_c.dll not found; running deterministic Scripted Director fallback.";
        impl_->hwResult = res;
        return res;
    }

    std::filesystem::path xmlPath = config.modelXmlPath;
    std::error_code ec;
    if (!std::filesystem::exists(xmlPath, ec)) {
        xmlPath = resolveResourcePath(config.modelXmlPath);
    }
    std::filesystem::path binPath = xmlPath;
    binPath.replace_extension(".bin");
    if (!std::filesystem::exists(xmlPath, ec) || !std::filesystem::exists(binPath, ec)) {
        res.status = BackendStatus::Substituted;
        res.health = HealthState::ActiveSubstitute;
        res.activeBackend = "Scripted_Deterministic_Director";
        res.diagnosticCode = "NPU_MODEL_MISSING";
        res.humanReadableReason = "Madame N OpenVINO IR model files missing; running deterministic Scripted Director fallback.";
        impl_->hwResult = res;
        return res;
    }

    if (impl_->ov->coreCreate(&impl_->core) != 0 || !impl_->core) {
        res.status = BackendStatus::Substituted;
        res.health = HealthState::ActiveSubstitute;
        res.activeBackend = "Scripted_Deterministic_Director";
        res.diagnosticCode = "OPENVINO_CORE_INIT_FAILED";
        res.humanReadableReason = "ov_core_create failed; running deterministic Scripted Director fallback.";
        impl_->hwResult = res;
        return res;
    }

    bool hasNpuDevice = false;
    bool hasCpuDevice = false;
    std::string npuDeviceLabel;
    ov_available_devices_t avail{};
    if (impl_->ov->getAvailableDevices(impl_->core, &avail) == 0) {
        for (size_t i = 0; i < avail.size; ++i) {
            if (!avail.devices[i]) continue;
            const std::string dname(avail.devices[i]);
            if (dname == "NPU" || dname.rfind("NPU.", 0) == 0) {
                hasNpuDevice = true;
                if (npuDeviceLabel.empty()) {
                    npuDeviceLabel = queryDeviceFullName(*impl_->ov, impl_->core, "NPU");
                }
            } else if (dname == "CPU") {
                hasCpuDevice = true;
            }
        }
        impl_->ov->availableDevicesFree(&avail);
    }

    int readSt = -1;
    if (impl_->ov->readModelUnicode) {
        const std::wstring wXml = xmlPath.wstring();
        const std::wstring wBin = binPath.wstring();
        readSt = impl_->ov->readModelUnicode(impl_->core, wXml.c_str(), wBin.c_str(), &impl_->model);
    } else {
        const std::string uXml = xmlPath.string();
        const std::string uBin = binPath.string();
        readSt = impl_->ov->readModelUtf8(impl_->core, uXml.c_str(), uBin.c_str(), &impl_->model);
    }

    if (readSt != 0 || !impl_->model) {
        impl_->releaseHandles();
        res.status = BackendStatus::Substituted;
        res.health = HealthState::ActiveSubstitute;
        res.activeBackend = "Scripted_Deterministic_Director";
        res.diagnosticCode = "NPU_MODEL_READ_FAILED";
        res.humanReadableReason = "Madame N IR model read failed (" + statusToString(*impl_->ov, readSt) +
                                  "); running deterministic Scripted Director fallback.";
        impl_->hwResult = res;
        return res;
    }

    std::string npuFailureReason;
    if (config.allowNpu && hasNpuDevice) {
        res.capabilityDetected = true;
        res.detectedHardwareDetail = npuDeviceLabel + " (OpenVINO NPU plugin)";
        // Explicit "NPU" target — NEVER unqualified "AUTO" (SPEC_v4 Section 4.2.3)
        if (impl_->tryCompileAndVerifyDevice("NPU", npuFailureReason)) {
            if (config.npuInferTimeoutMs > 0) {
                impl_->inferTimeoutMs = std::max<int64_t>(20, config.npuInferTimeoutMs);
            }
            res.status = BackendStatus::Accelerated;
            res.health = HealthState::ActiveAccelerated;
            res.successfullyExecutedTestWork = true;
            res.activeBackend = "OpenVINO_NPU (" + impl_->activeDeviceFullName + ")";
            res.diagnosticCode = "NPU_OPENVINO_LIVE_VERIFIED";
            res.humanReadableReason = "Explicit OpenVINO NPU model compile + test inference verified on " +
                                      impl_->activeDeviceFullName + ".";
            impl_->hwResult = res;
            BOO_LOG_INFO("NPU", res.humanReadableReason);
            return res;
        }
        BOO_LOG_WARN("NPU", "Explicit NPU compilation/verification failed (" + npuFailureReason + "); attempting OpenVINO CPU fallback.");
    }

    std::string cpuFailureReason;
    if (config.allowCpuFallback && hasCpuDevice) {
        if (impl_->tryCompileAndVerifyDevice("CPU", cpuFailureReason)) {
            res.capabilityDetected = hasNpuDevice;
            if (hasNpuDevice && !npuDeviceLabel.empty()) {
                res.detectedHardwareDetail = npuDeviceLabel;
            }
            res.status = BackendStatus::Substituted;
            res.health = HealthState::ActiveSubstitute;
            res.successfullyExecutedTestWork = true;
            res.activeBackend = "OpenVINO_CPU (" + impl_->activeDeviceFullName + ")";
            res.diagnosticCode = !config.allowNpu
                                     ? "NPU_DISABLED_OPENVINO_CPU"
                                     : (hasNpuDevice ? "NPU_COMPILE_FAIL_CPU_ACTIVE" : "NPU_ABSENT_OPENVINO_CPU");
            res.humanReadableReason =
                (hasNpuDevice && config.allowNpu
                     ? ("NPU compile failed (" + npuFailureReason + "); ")
                     : (config.allowNpu ? "No physical NPU detected; " : "NPU disabled; ")) +
                std::string("running verified Madame N neural model on OpenVINO CPU.");
            impl_->hwResult = res;
            BOO_LOG_INFO("NPU", res.humanReadableReason);
            return res;
        }
    }

    impl_->releaseHandles();
    res.capabilityDetected = hasNpuDevice;
    res.detectedHardwareDetail = npuDeviceLabel;
    res.status = BackendStatus::Substituted;
    res.health = HealthState::ActiveSubstitute;
    res.successfullyExecutedTestWork = false;
    res.activeBackend = "Scripted_Deterministic_Director";
    res.diagnosticCode = hasNpuDevice ? "NPU_COMPILE_FAILED_SCRIPTED" : "NPU_ABSENT_SCRIPTED";
    res.humanReadableReason = "OpenVINO NPU/CPU unavailable (" +
                              (!npuFailureReason.empty() ? npuFailureReason : cpuFailureReason) +
                              "); running deterministic Scripted Director fallback.";
    impl_->hwResult = res;
    return res;
}

bool OpenVinoDirector::infer(
    const DirectorFeatureVector& features,
    NeuralInferenceSample& outSample,
    bool injectTimeout,
    NeuralInferenceFailure* failure) {
    if (!impl_ || !impl_->ready) {
        if (failure) *failure = {"Director is not initialized.", false};
        return false;
    }
    NeuralInferenceFailure ignored;
    return impl_->executeSingleInference(features, outSample, impl_->inferTimeoutMs, injectTimeout, failure ? *failure : ignored);
}

void OpenVinoDirector::shutdown() noexcept {
    if (!impl_) return;
    impl_->releaseHandles();
}

bool OpenVinoDirector::isReady() const noexcept {
    return impl_ && impl_->ready;
}

bool OpenVinoDirector::isUsingHardwareNpu() const noexcept {
    return impl_ && impl_->usingHardwareNpu;
}

const HardwareResult& OpenVinoDirector::hardwareResult() const noexcept {
    return impl_->hwResult;
}

const std::string& OpenVinoDirector::activeDeviceName() const noexcept {
    static const std::string kEmpty;
    return impl_ ? impl_->activeDevice : kEmpty;
}

// ============================================================================
// AsyncDirectorController — Non-blocking 5 Hz Neural Stage-Director Worker
// ============================================================================

struct AsyncDirectorController::Impl {
    static constexpr double kMinStateHoldSeconds = 1.5;

    ScriptedDirector scripted{};
    OpenVinoDirector neural{};

    std::filesystem::path modelXmlPath;
    uint64_t seed = 266203;
    bool allowNpuHardware = true;
    bool simulateMissingRuntime = false;

    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> disableNpu{false};
    std::atomic<bool> injectTimeout{false};
    std::atomic<bool> initialCompileDone{false};
    std::atomic<uint64_t> routeRevision{0};

    mutable std::mutex mtx;
    std::condition_variable cvWorker;
    std::condition_variable cvInit;

    DirectorFeatureVector latestFeatures{};
    bool hasFeatureInput = false;
    uint64_t timelineRevision = 0;

    bool hasValidNeuralSample = false;
    std::atomic<bool> runtimeQuarantined{false};
    bool retryableHardwareFailure = false;
    NeuralInferenceSample latestNeuralSample{};
    std::chrono::steady_clock::time_point lastNeuralSampleTime{};
    // At most five successful worker ticks can finish in one second (200 ms cadence).
    std::array<std::chrono::steady_clock::time_point, 5> completionTimes{};
    HardwareResult currentHwResult{};
    float lastInferMs = 0.0f;
    uint64_t completedInferences = 0;
    uint32_t timeoutCount = 0;

    // Render-thread smoothing & hysteresis state
    DirectorMoodState smoothedState{};
    double lastCategoryChangeTime = -10.0;
    bool smoothingInitialized = false;

    std::thread worker;

    void clearRouteSamples() noexcept {
        hasValidNeuralSample = false;
        lastInferMs = 0.0f;
        completedInferences = 0;
    }

    HardwareResult makeDisabledStatus() const {
        HardwareResult r{};
        r.engine = EngineKind::NPU;
        r.requestedBackend = "OpenVINO_NPU";
        r.status = BackendStatus::Substituted;
        r.health = HealthState::ActiveSubstitute;
        r.successfullyExecutedTestWork = false;
        r.activeBackend = "Scripted_Deterministic_Director";
        r.diagnosticCode = "DISABLED_BY_FLAG";
        r.humanReadableReason = "NPU disabled via CLI/hotkey; running deterministic Scripted Director.";
        return r;
    }

    HardwareResult makeProbingStatus() const {
        HardwareResult r{};
        r.engine = EngineKind::NPU;
        r.requestedBackend = "OpenVINO_NPU";
        r.status = BackendStatus::Substituted;
        r.health = HealthState::Probing;
        r.successfullyExecutedTestWork = false;
        r.activeBackend = "Scripted_Director_Warmup";
        r.diagnosticCode = "NPU_ASYNC_COMPILING";
        r.humanReadableReason = "Compiling Madame N OpenVINO model asynchronously in background; Scripted Director active until verified.";
        return r;
    }

    void workerLoop() {
        bool compiledAttempted = false;
        uint64_t activeRevision = routeRevision.load(std::memory_order_acquire);

        while (!stopRequested.load(std::memory_order_acquire)) {
            const uint64_t revision = routeRevision.load(std::memory_order_acquire);
            if (revision != activeRevision) {
                neural.shutdown();
                compiledAttempted = false;
                activeRevision = revision;
            }
            const bool isDisabled = disableNpu.load(std::memory_order_acquire);
            if (isDisabled) {
                if (!initialCompileDone.exchange(true, std::memory_order_acq_rel)) {
                    cvInit.notify_all();
                }
                std::unique_lock<std::mutex> lk(mtx);
                cvWorker.wait_for(lk, std::chrono::milliseconds(100), [&] {
                    return stopRequested.load(std::memory_order_acquire) || !disableNpu.load(std::memory_order_acquire);
                });
                continue;
            }

            if (!compiledAttempted && !runtimeQuarantined) {
                compiledAttempted = true;
                OpenVinoDirectorConfig cfg{};
                cfg.modelXmlPath = modelXmlPath;
                cfg.allowNpu = allowNpuHardware;
                cfg.allowCpuFallback = true;
                cfg.allowDevVenvDiscovery = true;
                cfg.simulateMissingRuntimeForTest = simulateMissingRuntime;
                cfg.inferTimeoutMs = 250;
                // At 5 Hz on Intel AI Boost, verified requests occasionally finished in 347–557 ms.
                // Keep that same request pending on the worker; the render thread can hold its last mood.
                cfg.npuInferTimeoutMs = 1000;

                const HardwareResult initRes = neural.initialize(cfg);
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    if (routeRevision.load(std::memory_order_acquire) != revision) continue;
                    currentHwResult = initRes;
                }
                if (!initialCompileDone.exchange(true, std::memory_order_acq_rel)) {
                    cvInit.notify_all();
                }
            }

            if (neural.isReady() && !runtimeQuarantined) {
                DirectorFeatureVector fv{};
                uint64_t inputRevision;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    inputRevision = timelineRevision;
                    fv = latestFeatures;
                    if (!hasFeatureInput) {
                        fv.values = {0.42f, 0.50f, 0.40f, 0.48f, 0.35f, 0.50f, 0.50f, 0.0f, 0.0f, 0.0f,
                                     0.45f, 0.60f, 0.30f, 0.35f, 0.10f, 0.50f};
                    }
                }

                NeuralInferenceSample sample{};
                NeuralInferenceFailure failure{};
                const bool forceTimeout = injectTimeout.load(std::memory_order_acquire);
                if (neural.infer(fv, sample, forceTimeout, &failure)) {
                    const auto now = std::chrono::steady_clock::now();
                    std::lock_guard<std::mutex> lk(mtx);
                    if (routeRevision.load(std::memory_order_acquire) != revision) continue;
                    lastInferMs = sample.latencyMs;
                    completionTimes[completedInferences % completionTimes.size()] = now;
                    ++completedInferences;
                    currentHwResult = neural.hardwareResult();
                    if (inputRevision == timelineRevision) {
                        latestNeuralSample = sample;
                        lastNeuralSampleTime = now;
                        hasValidNeuralSample = true;
                    }
                } else {
                    // Graceful mid-film stall/timeout failover (SPEC_v4 Section 8.2):
                    // Hold current smoothed mood on render thread and transition cleanly to Scripted Director!
                    const bool onNpu = neural.isUsingHardwareNpu();
                    const std::string device = neural.activeDeviceName();
                    neural.shutdown();
                    std::unique_lock<std::mutex> lk(mtx);
                    if (routeRevision.load(std::memory_order_acquire) != revision) continue;
                    const uint64_t previousCompletions = completedInferences;
                    runtimeQuarantined = true;
                    retryableHardwareFailure = onNpu && !forceTimeout;
                    clearRouteSamples();
                    if (failure.timedOut) ++timeoutCount;

                    HardwareResult failRes{};
                    failRes.engine = EngineKind::NPU;
                    failRes.requestedBackend = "OpenVINO_NPU";
                    failRes.capabilityDetected = currentHwResult.capabilityDetected;
                    failRes.detectedHardwareDetail = currentHwResult.detectedHardwareDetail;
                    failRes.status = BackendStatus::Degraded;
                    failRes.health = HealthState::ActiveDegraded;
                    failRes.successfullyExecutedTestWork = false;
                    failRes.activeBackend = failure.timedOut ? "Scripted_Director_Timeout_Failover" : "Scripted_Director_Inference_Error";
                    failRes.diagnosticCode = failure.timedOut ? "NPU_INFERENCE_TIMEOUT_FAILOVER" : "NPU_INFERENCE_ERROR_FAILOVER";
                    failRes.humanReadableReason = "OpenVINO " + device + " after " + std::to_string(previousCompletions) +
                        " successful completions: " + failure.reason + " Switched to the scripted director.";
                    currentHwResult = failRes;
                    lk.unlock();
                    BOO_LOG_WARN("NPU", failRes.humanReadableReason);
                }
            }

            // 5 Hz cadence (200 ms) per SPEC_v4 Section 4.2.3 (2-10 Hz recommended)
            std::unique_lock<std::mutex> lk(mtx);
            cvWorker.wait_for(lk, std::chrono::milliseconds(200), [&] {
                return stopRequested.load(std::memory_order_acquire) || routeRevision.load(std::memory_order_acquire) != revision;
            });
        }
    }
};

AsyncDirectorController::AsyncDirectorController() : impl_(std::make_unique<Impl>()) {}
AsyncDirectorController::~AsyncDirectorController() { shutdown(); }

void AsyncDirectorController::start(
    uint64_t seed,
    const std::filesystem::path& modelXmlPath,
    bool disableNpu,
    bool injectNpuTimeout,
    bool allowNpuHardware,
    bool simulateMissingRuntimeForTest) {
    shutdown();

    impl_->seed = seed;
    impl_->modelXmlPath = modelXmlPath;
    impl_->allowNpuHardware = allowNpuHardware;
    impl_->simulateMissingRuntime = simulateMissingRuntimeForTest;
    impl_->scripted.reset(seed);
    impl_->disableNpu.store(disableNpu, std::memory_order_release);
    impl_->injectTimeout.store(injectNpuTimeout, std::memory_order_release);
    impl_->stopRequested.store(false, std::memory_order_release);
    impl_->initialCompileDone.store(false, std::memory_order_release);
    impl_->hasFeatureInput = false;
    impl_->timelineRevision = 0;
    impl_->clearRouteSamples();
    impl_->runtimeQuarantined = false;
    impl_->retryableHardwareFailure = false;
    impl_->timeoutCount = 0;
    impl_->smoothedState = DirectorMoodState{};
    impl_->lastCategoryChangeTime = -10.0;
    impl_->smoothingInitialized = false;
    impl_->currentHwResult = disableNpu ? impl_->makeDisabledStatus() : impl_->makeProbingStatus();

    impl_->running.store(true, std::memory_order_release);
    impl_->worker = std::thread([this] { impl_->workerLoop(); });
}

bool AsyncDirectorController::waitForInitialCompile(uint32_t maxWaitMs) noexcept {
    if (!impl_ || !impl_->running.load(std::memory_order_acquire)) return false;
    std::unique_lock<std::mutex> lk(impl_->mtx);
    return impl_->cvInit.wait_for(lk, std::chrono::milliseconds(maxWaitMs), [&] {
        return impl_->initialCompileDone.load(std::memory_order_acquire);
    });
}

void AsyncDirectorController::reset(uint64_t seed) noexcept {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        ++impl_->timelineRevision;
        impl_->hasValidNeuralSample = false;
        impl_->hasFeatureInput = false;
    }
    impl_->seed = seed;
    impl_->scripted.reset(seed);
    impl_->smoothingInitialized = false;
    impl_->lastCategoryChangeTime = -10.0;
}

void AsyncDirectorController::setDisableNpu(bool disable) noexcept {
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        if (impl_->disableNpu.exchange(disable, std::memory_order_acq_rel) == disable) return;
        // Publish the selected route synchronously; an in-flight worker result
        // must not leave an Accelerated badge visible after the user disables it.
        impl_->clearRouteSamples();
        impl_->retryableHardwareFailure = false;
        impl_->routeRevision.fetch_add(1, std::memory_order_acq_rel);
        impl_->currentHwResult = disable ? impl_->makeDisabledStatus() : impl_->makeProbingStatus();
        if (!disable) impl_->runtimeQuarantined = false;
    }
    impl_->cvWorker.notify_all();
}

bool AsyncDirectorController::retryIfDegraded() noexcept {
    if (!impl_) return false;
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        if (!impl_->running || impl_->disableNpu || impl_->injectTimeout ||
            !impl_->runtimeQuarantined || !impl_->retryableHardwareFailure) return false;
        impl_->runtimeQuarantined = false;
        impl_->retryableHardwareFailure = false;
        impl_->clearRouteSamples();
        impl_->currentHwResult = impl_->makeProbingStatus();
        impl_->routeRevision.fetch_add(1, std::memory_order_acq_rel);
    }
    impl_->cvWorker.notify_all();
    return true;
}

void AsyncDirectorController::setInjectTimeout(bool inject) noexcept {
    if (!impl_) return;
    impl_->injectTimeout.store(inject, std::memory_order_release);
    if (inject) {
        impl_->cvWorker.notify_all();
    }
}

DirectorMoodState AsyncDirectorController::evaluateFrame(
    const FrameContext& ctx,
    const SceneCue* activeCue,
    DirectorMoodState* scriptedBaseline) noexcept {
    if (!impl_) {
        if (scriptedBaseline) *scriptedBaseline = {};
        return {};
    }

    const DirectorMoodState scriptedTarget = impl_->scripted.evaluate(ctx, activeCue);
    if (scriptedBaseline) *scriptedBaseline = scriptedTarget;
    const DirectorFeatureVector fv = impl_->scripted.buildFeatures(ctx, activeCue);

    bool useNeural = false;
    NeuralInferenceSample neuralSample{};
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        if (ctx.seeking) {
            ++impl_->timelineRevision;
            impl_->hasValidNeuralSample = false;
        }
        impl_->latestFeatures = fv;
        impl_->hasFeatureInput = true;
        if (impl_->hasValidNeuralSample && !impl_->disableNpu.load(std::memory_order_relaxed) && !impl_->runtimeQuarantined) {
            useNeural = true;
            neuralSample = impl_->latestNeuralSample;
        }
    }

    DirectorMoodState target = scriptedTarget;
    if (useNeural) {
        target.category           = neuralSample.category;
        target.confidence         = neuralSample.confidence;
        target.bloomStrength      = neuralSample.controls[0];
        target.irisAngle          = neuralSample.controls[1];
        target.lightTension       = neuralSample.controls[2];
        target.spectralShift      = neuralSample.controls[3];
        target.cutAccent          = neuralSample.controls[4];
        target.isScriptedFallback = false;

        if (activeCue && !activeCue->allowDirectorModulation) {
            target.bloomStrength = 0.40f;
            target.irisAngle     = 0.50f;
            target.lightTension  = 0.35f;
            target.spectralShift = 0.20f;
            target.cutAccent     = 0.0f;
        }
    }

    if (!impl_->smoothingInitialized || ctx.seeking) {
        impl_->smoothedState = target;
        impl_->lastCategoryChangeTime = ctx.filmTimeSeconds;
        impl_->smoothingInitialized = true;
        return impl_->smoothedState;
    }

    if (target.category != impl_->smoothedState.category &&
        std::abs(ctx.filmTimeSeconds - impl_->lastCategoryChangeTime) >= Impl::kMinStateHoldSeconds) {
        impl_->smoothedState.category = target.category;
        impl_->lastCategoryChangeTime = ctx.filmTimeSeconds;
    }

    const float dt = static_cast<float>(std::clamp(ctx.dtSeconds, 0.0, 0.1));
    const float alpha = clamp01(1.0f - std::exp(-6.0f * (dt > 0.0f ? dt : (1.0f / 60.0f))));
    const auto lerpClamped = [&](float cur, float tgt) {
        return clamp01(cur + (tgt - cur) * alpha);
    };

    impl_->smoothedState.bloomStrength      = lerpClamped(impl_->smoothedState.bloomStrength, target.bloomStrength);
    impl_->smoothedState.irisAngle          = lerpClamped(impl_->smoothedState.irisAngle, target.irisAngle);
    impl_->smoothedState.lightTension       = lerpClamped(impl_->smoothedState.lightTension, target.lightTension);
    impl_->smoothedState.spectralShift      = lerpClamped(impl_->smoothedState.spectralShift, target.spectralShift);
    impl_->smoothedState.cutAccent          = lerpClamped(impl_->smoothedState.cutAccent, target.cutAccent);
    impl_->smoothedState.confidence         = target.confidence;
    impl_->smoothedState.isScriptedFallback = target.isScriptedFallback;
    return impl_->smoothedState;
}

AsyncDirectorTelemetry AsyncDirectorController::telemetry() const noexcept {
    AsyncDirectorTelemetry out{};
    if (!impl_) return out;
    std::lock_guard<std::mutex> lk(impl_->mtx);
    out.hardwareResult = impl_->disableNpu.load(std::memory_order_acquire)
        ? impl_->makeDisabledStatus() : impl_->currentHwResult;
    if (!impl_->hasValidNeuralSample && out.hardwareResult.successfullyExecutedTestWork) {
        out.hardwareResult.health = HealthState::Probing;
        out.hardwareResult.status = BackendStatus::Substituted;
        out.hardwareResult.successfullyExecutedTestWork = false;
        out.hardwareResult.activeBackend = "Scripted_Director_Warmup";
        out.hardwareResult.diagnosticCode = "NPU_AWAITING_CURRENT_CUE";
        out.hardwareResult.humanReadableReason = "Running scripted controls while the model processes the current scene.";
    }
    out.timeoutCount = impl_->timeoutCount;
    if (impl_->running && impl_->hasValidNeuralSample) {
        const auto now = std::chrono::steady_clock::now();
        out.lastInferMs = impl_->lastInferMs;
        out.completedInferences = impl_->completedInferences;
        out.resultAgeMs = static_cast<float>(
            std::chrono::duration<double, std::milli>(now - impl_->lastNeuralSampleTime).count());
        const auto start = now - std::chrono::seconds(1);
        const size_t count = static_cast<size_t>(std::min<uint64_t>(out.completedInferences, impl_->completionTimes.size()));
        out.completedRateHz = static_cast<float>(std::count_if(impl_->completionTimes.begin(),
            impl_->completionTimes.begin() + count, [start](auto t) { return t > start; }));
    }
    return out;
}

void AsyncDirectorController::shutdown() noexcept {
    if (!impl_) return;
    if (impl_->running.exchange(false, std::memory_order_acq_rel)) {
        impl_->stopRequested.store(true, std::memory_order_release);
        impl_->cvWorker.notify_all();
        if (impl_->worker.joinable()) {
            impl_->worker.join();
        }
        impl_->neural.shutdown();
    }
}

} // namespace boo
