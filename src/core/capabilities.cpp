#include "boo/capabilities.hpp"
#include "boo/director.hpp"
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
#include <wrl/client.h>

#include <initguid.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <intrin.h>
#include <charconv>
#include <cstring>
#include <string_view>

namespace boo {

using Microsoft::WRL::ComPtr;

namespace {

std::string wideToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), needed, nullptr, nullptr)) return {};
    out.pop_back();
    return out;
}

std::string queryCpuBrandString() {
    int cpuInfo[4] = {0, 0, 0, 0};
    __cpuid(cpuInfo, static_cast<int>(0x80000000u));
    const unsigned int maxExt = static_cast<unsigned int>(cpuInfo[0]);
    if (maxExt < 0x80000004u) {
        return "Unknown x64 Processor";
    }
    char brand[64]{};
    __cpuid(reinterpret_cast<int*>(brand + 0),  static_cast<int>(0x80000002u));
    __cpuid(reinterpret_cast<int*>(brand + 16), static_cast<int>(0x80000003u));
    __cpuid(reinterpret_cast<int*>(brand + 32), static_cast<int>(0x80000004u));
    std::string s(brand);
    const size_t first = s.find_first_not_of(' ');
    if (first == std::string::npos) return "Unknown x64 Processor";
    const size_t last = s.find_last_not_of(' ');
    return s.substr(first, last - first + 1);
}

void probeProcessorIdentity(SystemCapabilities& caps) {
    caps.cpuBrand = queryCpuBrandString();
    int leaf[4]{};
    __cpuid(leaf, 0);
    const bool hasSignature = leaf[0] >= 1;
    char vendor[13]{};
    std::memcpy(vendor, leaf + 1, 4);
    std::memcpy(vendor + 4, leaf + 3, 4);
    std::memcpy(vendor + 8, leaf + 2, 4);
    const std::string_view manufacturer(vendor);
    if (manufacturer == "GenuineIntel") caps.cpuVendor = ResultsCpuModel::Intel;
    else if (manufacturer == "AuthenticAMD") caps.cpuVendor = ResultsCpuModel::AMD;
    if (!hasSignature) return;
    __cpuid(leaf, 1);
    const auto signature = static_cast<unsigned>(leaf[0]);
    const unsigned baseFamily = (signature >> 8) & 15;
    const unsigned baseModel = (signature >> 4) & 15;
    const bool extendedModel = baseFamily == 6 || baseFamily == 15;
    caps.cpuFamily = static_cast<uint16_t>(baseFamily + (baseFamily == 15 ? (signature >> 20) & 255 : 0));
    caps.cpuModel = static_cast<uint16_t>(baseModel + (extendedModel ? (signature >> 12) & 0xf0 : 0));
}

std::string queryWindowsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) {
            RTL_OSVERSIONINFOW osvi{};
            osvi.dwOSVersionInfoSize = sizeof(osvi);
            if (fn(&osvi) == 0) {
                const bool isWin11 = (osvi.dwMajorVersion == 10 && osvi.dwBuildNumber >= 22000);
                return std::string(isWin11 ? "Windows 11 (NT " : "Windows (NT ") +
                       std::to_string(osvi.dwMajorVersion) + "." +
                       std::to_string(osvi.dwMinorVersion) + " Build " +
                       std::to_string(osvi.dwBuildNumber) + ")";
            }
        }
    }
    return "Windows x64";
}

const char* featureLevelToString(D3D_FEATURE_LEVEL fl) noexcept {
    switch (fl) {
        case D3D_FEATURE_LEVEL_12_2: return "12_2";
        case D3D_FEATURE_LEVEL_12_1: return "12_1";
        case D3D_FEATURE_LEVEL_12_0: return "12_0";
        case D3D_FEATURE_LEVEL_11_1: return "11_1";
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        default: return "Unknown";
    }
}

const char* shaderModelToString(D3D_SHADER_MODEL sm) noexcept {
    switch (sm) {
        case D3D_SHADER_MODEL_6_7: return "6_7";
        case D3D_SHADER_MODEL_6_6: return "6_6";
        case D3D_SHADER_MODEL_6_5: return "6_5";
        case D3D_SHADER_MODEL_6_4: return "6_4";
        case D3D_SHADER_MODEL_6_3: return "6_3";
        case D3D_SHADER_MODEL_6_2: return "6_2";
        case D3D_SHADER_MODEL_6_1: return "6_1";
        case D3D_SHADER_MODEL_6_0: return "6_0";
        case D3D_SHADER_MODEL_5_1: return "5_1";
        default: return "6_0+";
    }
}

const char* dxrTierToString(D3D12_RAYTRACING_TIER tier) noexcept {
    if (tier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
        return "NotSupported";
    }
    if (static_cast<uint32_t>(tier) >= 12u) {
        return "Tier_1_2";
    }
    if (tier >= D3D12_RAYTRACING_TIER_1_1) {
        return "Tier_1_1";
    }
    if (tier >= D3D12_RAYTRACING_TIER_1_0) {
        return "Tier_1_0";
    }
    return "NotSupported";
}

void readAcceleratorChip(ComputeAcceleratorDevice& dev) {
    const std::string_view id = dev.instanceId;
    if (!id.starts_with("PCI\\VEN_") || id.size() < 21 || id.substr(12, 5) != "&DEV_") return;
    const auto hex4 = [](std::string_view digits) -> uint16_t {
        uint16_t value = 0;
        const auto end = digits.data() + digits.size();
        const auto parsed = std::from_chars(digits.data(), end, value, 16);
        return parsed.ec == std::errc{} && parsed.ptr == end ? value : 0;
    };
    dev.vendorId = hex4(id.substr(8, 4));
    dev.deviceId = hex4(id.substr(17, 4));
}

ResultsNpuModel publicNpuModel(const ComputeAcceleratorDevice& dev, DEVINST node) {
    // Only the public PCI vendor/chip prefix and the driver-supplied description affect this enum.
    // A friendly name is configurable; neither it nor the per-device instance suffix goes on the card.
    if (dev.vendorId == 0x8086 && dev.deviceId == 0xad1d) return ResultsNpuModel::IntelAiBoost;
    DEVPROPTYPE type = 0;
    wchar_t model[256]{};
    ULONG bytes = sizeof(model);
    if (CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_DeviceDesc, &type,
                                 reinterpret_cast<PBYTE>(model), &bytes, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING) return ResultsNpuModel::Unknown;
    const std::wstring_view label(model);
    if (dev.vendorId == 0x8086 && (label == L"Intel(R) AI Boost" || label == L"Intel AI Boost"))
        return ResultsNpuModel::IntelAiBoost;
    if (dev.vendorId == 0x1022 && (label == L"AMD IPU Device" || label == L"AMD Ryzen AI" ||
        label == L"AMD(R) Ryzen(TM) AI" || label == L"AMD NPU Device"))
        return ResultsNpuModel::AmdRyzenAI;
    return ResultsNpuModel::Unknown;
}

bool enumerateComputeAccelerators(std::vector<ComputeAcceleratorDevice>& outDevices) {
    // Windows ComputeAccelerator setup class GUID: {f01a9d53-3ff6-48d2-9f97-c8a7004be10c}
    const wchar_t* kClassGuidStr = L"{f01a9d53-3ff6-48d2-9f97-c8a7004be10c}";
    ULONG listChars = 0;
    if (CM_Get_Device_ID_List_SizeW(&listChars, kClassGuidStr, CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS)
        return false;
    if (listChars <= 1) return true;

    std::vector<wchar_t> buffer(listChars, L'\0');
    if (CM_Get_Device_ID_ListW(kClassGuidStr, buffer.data(), listChars, CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS) {
        return false;
    }

    const wchar_t* cur = buffer.data();
    while (*cur) {
        ComputeAcceleratorDevice dev{};
        dev.instanceId = wideToUtf8(cur);
        readAcceleratorChip(dev);

        DEVINST devInst = 0;
        if (CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(cur), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) {
            dev.cardModel = publicNpuModel(dev, devInst);
            ULONG status = 0;
            ULONG problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, devInst, 0) == CR_SUCCESS) {
                dev.statusOk = (problem == 0);
            }

            DEVPROPTYPE propType = 0;
            wchar_t nameBuf[256]{};
            ULONG nameBytes = sizeof(nameBuf);
            if (CM_Get_DevNode_PropertyW(devInst, &DEVPKEY_NAME, &propType, reinterpret_cast<PBYTE>(nameBuf), &nameBytes, 0) == CR_SUCCESS) {
                dev.friendlyName = wideToUtf8(nameBuf);
            }
        }
        if (dev.friendlyName.empty()) {
            dev.friendlyName = "Compute Accelerator";
        }
        outDevices.push_back(std::move(dev));
        cur += std::wcslen(cur) + 1;
    }
    return true;
}

void probeDynamicCudaDriver(SystemCapabilities& caps, const AdapterInfo* selectedAdapter) {
    HMODULE hCuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hCuda) {
        return;
    }
    caps.cudaDriverDllPresent = true;

    using CuInitFn = int(*)(unsigned int);
    using CuDriverGetVersionFn = int(*)(int*);
    using CuDeviceGetCountFn = int(*)(int*);
    using CuDeviceGetFn = int(*)(int*, int);
    using CuDeviceGetNameFn = int(*)(char*, int, int);
    using CuDeviceGetAttributeFn = int(*)(int*, int, int);
    using CuDeviceGetLuidFn = int(*)(char*, unsigned int*, int);

    const auto pCuInit = reinterpret_cast<CuInitFn>(GetProcAddress(hCuda, "cuInit"));
    const auto pCuVer  = reinterpret_cast<CuDriverGetVersionFn>(GetProcAddress(hCuda, "cuDriverGetVersion"));
    const auto pCuCnt  = reinterpret_cast<CuDeviceGetCountFn>(GetProcAddress(hCuda, "cuDeviceGetCount"));
    const auto pCuDev  = reinterpret_cast<CuDeviceGetFn>(GetProcAddress(hCuda, "cuDeviceGet"));
    const auto pCuName = reinterpret_cast<CuDeviceGetNameFn>(GetProcAddress(hCuda, "cuDeviceGetName"));
    const auto pCuAttr = reinterpret_cast<CuDeviceGetAttributeFn>(GetProcAddress(hCuda, "cuDeviceGetAttribute"));
    const auto pCuLuid = reinterpret_cast<CuDeviceGetLuidFn>(GetProcAddress(hCuda, "cuDeviceGetLuid"));

    if (pCuInit && pCuInit(0) == 0) {
        if (pCuVer) {
            int ver = 0;
            if (pCuVer(&ver) == 0) caps.cudaDriverVersion = ver;
        }
        int count = 0;
        if (pCuCnt && pCuDev && pCuCnt(&count) == 0 && count > 0) {
            int chosenOrd = 0;
            if (selectedAdapter && pCuLuid) {
                for (int i = 0; i < count; ++i) {
                    int dev = 0;
                    if (pCuDev(&dev, i) == 0) {
                        LUID cudaLuid{};
                        unsigned int nodeMask = 0;
                        if (pCuLuid(reinterpret_cast<char*>(&cudaLuid), &nodeMask, dev) == 0) {
                            if (cudaLuid.LowPart == static_cast<DWORD>(selectedAdapter->luidLowPart) &&
                                cudaLuid.HighPart == selectedAdapter->luidHighPart) {
                                chosenOrd = i;
                                caps.cudaMatchesD3D12Luid = true;
                                break;
                            }
                        }
                    }
                }
            }
            int dev = 0;
            if (pCuDev(&dev, chosenOrd) == 0) {
                if (pCuName) {
                    char nameBuf[128]{};
                    if (pCuName(nameBuf, sizeof(nameBuf), dev) == 0) {
                        caps.cudaMatchedDeviceName = nameBuf;
                    }
                }
                if (pCuAttr) {
                    // CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT = 16
                    // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR = 75, MINOR = 76
                    int smCount = 0, ccMaj = 0, ccMin = 0;
                    if (pCuAttr(&smCount, 16, dev) == 0) caps.cudaMultiProcessorCount = smCount;
                    if (pCuAttr(&ccMaj, 75, dev) == 0)   caps.cudaComputeMajor = ccMaj;
                    if (pCuAttr(&ccMin, 76, dev) == 0)   caps.cudaComputeMinor = ccMin;
                }
            }
        }
    }
    FreeLibrary(hCuda);

    // Check optional cuBLASLt runtime DLL availability without requiring it
    const wchar_t* cublasCandidates[] = {L"cublasLt64_13.dll", L"cublasLt64_12.dll", L"cublasLt64_11.dll"};
    for (const wchar_t* dll : cublasCandidates) {
        HMODULE hMod = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_AS_DATAFILE);
        if (hMod) {
            caps.cublasLtDllPresent = true;
            FreeLibrary(hMod);
            break;
        }
    }
}

void probeDynamicOpenVinoRuntime(SystemCapabilities& caps) {
    if (isOpenVinoRuntimeAvailable(true)) {
        caps.openvinoRuntimeDllPresent = true;
        return;
    }
    const wchar_t* candidates[] = {L"openvino.dll"};
    for (const wchar_t* dll : candidates) {
        HMODULE hMod = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_AS_DATAFILE);
        if (hMod) {
            caps.openvinoRuntimeDllPresent = true;
            FreeLibrary(hMod);
            break;
        }
    }
}

void probeOptionalNvencAndHardwareMft(SystemCapabilities& caps, const AdapterInfo* selectedAdapter) {
    HMODULE hNvenc = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hNvenc) {
        caps.nvencDriverDllPresent = true;
        using GetMaxVerFn = int(WINAPI*)(uint32_t*);
        auto pGetMax = reinterpret_cast<GetMaxVerFn>(GetProcAddress(hNvenc, "NvEncodeAPIGetMaxSupportedVersion"));
        if (pGetMax) {
            uint32_t ver = 0;
            if (pGetMax(&ver) == 0) {
                caps.nvencMaxApiVersionMajor = (ver >> 4);
                caps.nvencMaxApiVersionMinor = (ver & 0xFu);
            }
        }
        FreeLibrary(hNvenc);
    }

    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        MFT_REGISTER_TYPE_INFO outType{MFMediaType_Video, MFVideoFormat_H264};
        ComPtr<IMFAttributes> enumAttrs;
        if (selectedAdapter && SUCCEEDED(MFCreateAttributes(&enumAttrs, 1))) {
            LUID luid{};
            luid.LowPart  = static_cast<DWORD>(selectedAdapter->luidLowPart);
            luid.HighPart = selectedAdapter->luidHighPart;
            enumAttrs->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&luid), sizeof(LUID));
        }

        IMFActivate** ppActivate = nullptr;
        UINT32 count = 0;
        const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
        HRESULT hr = MFTEnum2(
            MFT_CATEGORY_VIDEO_ENCODER,
            flags,
            nullptr,
            &outType,
            enumAttrs.Get(),
            &ppActivate,
            &count);
        if (FAILED(hr) || count == 0) {
            hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outType, &ppActivate, &count);
        }
        if (SUCCEEDED(hr) && count > 0 && ppActivate) {
            caps.hardwareVideoEncoderMftPresent = true;
            wchar_t* nameW = nullptr;
            UINT32 cch = 0;
            if (SUCCEEDED(ppActivate[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nameW, &cch)) && nameW) {
                caps.hardwareVideoEncoderName = wideToUtf8(nameW);
                CoTaskMemFree(nameW);
            }
            for (UINT32 i = 0; i < count; ++i) {
                ppActivate[i]->Release();
            }
            CoTaskMemFree(ppActivate);
        }
        MFShutdown();
    }
}

} // namespace

ResultsHostHardware resultsHostHardware(const SystemCapabilities& caps) noexcept {
    ResultsHostHardware host;
    host.cpu = caps.cpuVendor;
    host.cpuFamily = caps.cpuFamily;
    host.cpuModel = caps.cpuModel;
    const std::string_view cpu = caps.cpuBrand;
    if (caps.cpuVendor == ResultsCpuModel::Intel && cpu == "Intel(R) Core(TM) Ultra 9 275HX")
        host.cpu = ResultsCpuModel::IntelCoreUltra9_275HX;
    else if (caps.cpuVendor == ResultsCpuModel::Intel && cpu == "13th Gen Intel(R) Core(TM) i9-13900H")
        host.cpu = ResultsCpuModel::IntelCoreI9_13900H;
    for (const auto& accelerator : caps.npuDevices) {
        if (!host.acceleratorVendor && accelerator.vendorId && accelerator.deviceId) {
            host.acceleratorVendor = accelerator.vendorId;
            host.acceleratorDevice = accelerator.deviceId;
        }
        if (accelerator.cardModel == ResultsNpuModel::Unknown) continue;
        host.npu = accelerator.cardModel;
        host.acceleratorVendor = accelerator.vendorId;
        host.acceleratorDevice = accelerator.deviceId;
        return host;
    }
    if (caps.acceleratorEnumerationSucceeded && caps.npuDevices.empty()) host.npu = ResultsNpuModel::NoneDetected;
    return host;
}

ResultsHostHardware probeResultsHostHardware() {
    SystemCapabilities caps;
    probeProcessorIdentity(caps);
    caps.acceleratorEnumerationSucceeded = enumerateComputeAccelerators(caps.npuDevices);
    return resultsHostHardware(caps);
}

SystemCapabilities probeSystemCapabilities(const ProbeFlags& flags) {
    SystemCapabilities caps{};
    caps.osVersion = queryWindowsVersion();
    probeProcessorIdentity(caps);

    SYSTEM_INFO sysInfo{};
    GetSystemInfo(&sysInfo);
    caps.logicalCores = sysInfo.dwNumberOfProcessors;

    MEMORYSTATUSEX memStatus{};
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        caps.totalSystemRamBytes = memStatus.ullTotalPhys;
    }

    // 1. Enumerate DXGI adapters with high-performance preference (Section 3.3)
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory6)))) {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT idx = 0;
             SUCCEEDED(factory6->EnumAdapterByGpuPreference(idx, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
             ++idx) {
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc))) continue;

            AdapterInfo info{};
            info.index = idx;
            info.description = wideToUtf8(desc.Description);
            info.vendorId = desc.VendorId;
            info.deviceId = desc.DeviceId;
            info.subSysId = desc.SubSysId;
            info.revision = desc.Revision;
            info.dedicatedVideoMemoryBytes  = desc.DedicatedVideoMemory;
            info.dedicatedSystemMemoryBytes = desc.DedicatedSystemMemory;
            info.sharedSystemMemoryBytes    = desc.SharedSystemMemory;
            info.luidLowPart  = static_cast<int32_t>(desc.AdapterLuid.LowPart);
            info.luidHighPart = desc.AdapterLuid.HighPart;
            info.isSoftwareWarp = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

            ComPtr<ID3D12Device> testDev;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDev)))) {
                info.supportsD3D12 = true;

                const D3D_FEATURE_LEVEL levels[] = {
                    D3D_FEATURE_LEVEL_12_2,
                    D3D_FEATURE_LEVEL_12_1,
                    D3D_FEATURE_LEVEL_12_0,
                    D3D_FEATURE_LEVEL_11_1,
                    D3D_FEATURE_LEVEL_11_0};
                D3D12_FEATURE_DATA_FEATURE_LEVELS flData{};
                flData.NumFeatureLevels = static_cast<UINT>(std::size(levels));
                flData.pFeatureLevelsRequested = levels;
                if (SUCCEEDED(testDev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &flData, sizeof(flData)))) {
                    info.maxFeatureLevel = featureLevelToString(flData.MaxSupportedFeatureLevel);
                }

                D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
                if (SUCCEEDED(testDev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5)))) {
                    info.dxrTier = dxrTierToString(opt5.RaytracingTier);
                }

                D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_7};
                const D3D_SHADER_MODEL smTry[] = {
                    D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
                    D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_0};
                for (D3D_SHADER_MODEL candidate : smTry) {
                    sm.HighestShaderModel = candidate;
                    if (SUCCEEDED(testDev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
                        info.shaderModel = shaderModelToString(sm.HighestShaderModel);
                        break;
                    }
                }
            }

            if (caps.selectedAdapterIndex < 0 && info.supportsD3D12) {
                if (flags.forceWarp && info.isSoftwareWarp) {
                    caps.selectedAdapterIndex = static_cast<int32_t>(caps.adapters.size());
                } else if (!flags.forceWarp && !info.isSoftwareWarp) {
                    caps.selectedAdapterIndex = static_cast<int32_t>(caps.adapters.size());
                }
            }
            caps.adapters.push_back(std::move(info));
        }

        // Fallback to any D3D12 adapter (e.g. WARP) if no hardware adapter matched
        if (caps.selectedAdapterIndex < 0) {
            for (size_t i = 0; i < caps.adapters.size(); ++i) {
                if (caps.adapters[i].supportsD3D12) {
                    caps.selectedAdapterIndex = static_cast<int32_t>(i);
                    break;
                }
            }
        }
    }

    const AdapterInfo* sel =
        (caps.selectedAdapterIndex >= 0 && static_cast<size_t>(caps.selectedAdapterIndex) < caps.adapters.size())
            ? &caps.adapters[static_cast<size_t>(caps.selectedAdapterIndex)]
            : nullptr;

    if (sel) {
        caps.d3d12Available   = true;
        caps.d3d12UsingWarp   = sel->isSoftwareWarp;
        caps.d3d12FeatureLevel = sel->maxFeatureLevel;
        caps.d3d12ShaderModel = sel->shaderModel;
        caps.dxrTierString    = sel->dxrTier;
        caps.dxrTier11Supported = (sel->dxrTier == "Tier_1_1" || sel->dxrTier == "Tier_1_2");
        caps.dxrTier10Supported = caps.dxrTier11Supported || (sel->dxrTier == "Tier_1_0");
    }

    // 2. Non-blocking dynamic probes for optional accelerators
    if (!flags.disableCUDA || !flags.disableTensor) {
        probeDynamicCudaDriver(caps, sel);
    }
    if (!flags.disableNPU) {
        caps.acceleratorEnumerationSucceeded = enumerateComputeAccelerators(caps.npuDevices);
        probeDynamicOpenVinoRuntime(caps);
    }
    // Section 3.5: Set NVENC state = not_loaded unless Signal Hijack is explicitly enabled
    if (flags.enableSignalHijack && !flags.disableNVENC) {
        probeOptionalNvencAndHardwareMft(caps, sel);
    }

    // 3. Populate strictly honest HardwareResult records (Section 3.4 & 8.1)
    // Note: M0 baseline show uses D3D12 raster test visual + deterministic scripted director.
    // Optional hardware is marked Accelerated ONLY when an actual hardware workload executes and verifies!
    {
        HardwareResult rt{};
        rt.engine = EngineKind::RT;
        rt.requestedBackend = "D3D12_DXR";
        if (flags.disableRT) {
            rt.status = BackendStatus::Substituted;
            rt.health = HealthState::ActiveSubstitute;
            rt.activeBackend = "D3D12_Raster_Fallback";
            rt.diagnosticCode = "DISABLED_BY_FLAG";
            rt.humanReadableReason = "DXR disabled via --disable-rt; using D3D12 raster path.";
        } else if (caps.dxrTier10Supported) {
            rt.capabilityDetected = true;
            rt.detectedHardwareDetail = sel->description + " (DXR " + caps.dxrTierString + ")";
            rt.status = BackendStatus::Substituted;
            rt.health = HealthState::Ready; // Hardware capable; M0 baseline runs raster test visual unless benchmarked
            rt.activeBackend = "D3D12_Raster_M0_Baseline";
            rt.diagnosticCode = "DXR_CAPABLE_M0_RASTER_ACTIVE";
            rt.humanReadableReason = "DXR " + caps.dxrTierString + " detected; M0 baseline runs raster test visual (use --bench or press B to execute & verify DXR rays).";
        } else {
            rt.status = BackendStatus::Substituted;
            rt.health = HealthState::ActiveSubstitute;
            rt.activeBackend = "D3D12_Raster_Fallback";
            rt.diagnosticCode = "DXR_NOT_SUPPORTED";
            rt.humanReadableReason = "Adapter does not support DXR; using D3D12 raster substitute.";
        }
        caps.engines[static_cast<size_t>(EngineKind::RT)] = std::move(rt);
    }

    {
        HardwareResult tensor{};
        tensor.engine = EngineKind::Tensor;
        tensor.requestedBackend = "CUDA_Tensor_FP16";
        if (flags.disableTensor) {
            tensor.status = BackendStatus::Substituted;
            tensor.health = HealthState::ActiveSubstitute;
            tensor.activeBackend = "D3D12_HLSL_Procedural_Substitute";
            tensor.diagnosticCode = "DISABLED_BY_FLAG";
            tensor.humanReadableReason = "Tensor module disabled via --disable-tensor; using D3D12 procedural substitute.";
        } else if (caps.cudaDriverDllPresent && caps.cudaComputeMajor >= 8) {
            tensor.capabilityDetected = true;
            tensor.detectedHardwareDetail =
                caps.cudaMatchedDeviceName + " (SM " + std::to_string(caps.cudaComputeMajor) + "." +
                std::to_string(caps.cudaComputeMinor) +
                (caps.cublasLtDllPresent ? ", cuBLASLt DLL present)" : ", Driver API PTX available; cuBLASLt SDK absent)");
            tensor.status = BackendStatus::Substituted;
            tensor.health = HealthState::Ready;
            tensor.activeBackend = "D3D12_HLSL_Procedural_Substitute";
            tensor.diagnosticCode = caps.cublasLtDllPresent ? "TENSOR_CAPABLE_M0_BASELINE" : "CUBLASLT_ABSENT_PTX_CAPABLE";
            tensor.humanReadableReason =
                "SM " + std::to_string(caps.cudaComputeMajor) + "." + std::to_string(caps.cudaComputeMinor) +
                " Tensor-capable GPU detected; M0 baseline uses HLSL substitute (use --bench or press B to execute & verify FP16 MMA).";
        } else {
            tensor.status = BackendStatus::Substituted;
            tensor.health = HealthState::ActiveSubstitute;
            tensor.activeBackend = "D3D12_HLSL_Procedural_Substitute";
            tensor.diagnosticCode = "TENSOR_UNAVAILABLE";
            tensor.humanReadableReason = "No SM 8.0+ CUDA Tensor runtime detected; using D3D12 procedural substitute.";
        }
        caps.engines[static_cast<size_t>(EngineKind::Tensor)] = std::move(tensor);
    }

    {
        HardwareResult npu{};
        npu.engine = EngineKind::NPU;
        npu.requestedBackend = "OpenVINO_NPU";
        if (flags.disableNPU) {
            npu.status = BackendStatus::Substituted;
            npu.health = HealthState::ActiveSubstitute;
            npu.activeBackend = "Scripted_Deterministic_Director";
            npu.diagnosticCode = "DISABLED_BY_FLAG";
            npu.humanReadableReason = "NPU disabled via --disable-npu; using deterministic Scripted Director.";
        } else if (!caps.npuDevices.empty()) {
            npu.capabilityDetected = true;
            npu.detectedHardwareDetail = caps.npuDevices.front().friendlyName + " (" + caps.npuDevices.front().instanceId + ")";
            npu.status = BackendStatus::Substituted;
            npu.health = caps.openvinoRuntimeDllPresent ? HealthState::Ready : HealthState::ActiveSubstitute;
            npu.activeBackend = "Scripted_Deterministic_Director";
            npu.diagnosticCode = caps.openvinoRuntimeDllPresent ? "NPU_CAPABLE_OPENVINO_READY" : "OPENVINO_RUNTIME_ABSENT";
            npu.humanReadableReason =
                "NPU hardware detected (" + caps.npuDevices.front().friendlyName + "), " +
                (caps.openvinoRuntimeDllPresent
                     ? "OpenVINO C runtime + Madame N IR model ready for explicit NPU compilation; "
                     : "OpenVINO runtime not installed; ") +
                "Scripted Director standby until verified NPU inference.";
        } else {
            npu.status = BackendStatus::Substituted;
            npu.health = HealthState::ActiveSubstitute;
            npu.activeBackend = "Scripted_Deterministic_Director";
            npu.diagnosticCode = "NPU_NOT_PRESENT";
            npu.humanReadableReason = "No ComputeAccelerator NPU detected; running deterministic Scripted Director fallback.";
        }
        caps.engines[static_cast<size_t>(EngineKind::NPU)] = std::move(npu);
    }

    {
        HardwareResult cuda{};
        cuda.engine = EngineKind::CUDA;
        cuda.requestedBackend = "CUDA_Driver_Runtime";
        if (flags.disableCUDA) {
            cuda.status = BackendStatus::Substituted;
            cuda.health = HealthState::ActiveSubstitute;
            cuda.activeBackend = "D3D12_HLSL_Compute_Substitute";
            cuda.diagnosticCode = "DISABLED_BY_FLAG";
            cuda.humanReadableReason = "CUDA disabled via --disable-cuda; using D3D12 HLSL substitute.";
        } else if (caps.cudaDriverDllPresent && !caps.cudaMatchedDeviceName.empty()) {
            cuda.capabilityDetected = true;
            cuda.detectedHardwareDetail =
                caps.cudaMatchedDeviceName + " (CUDA Driver " +
                std::to_string(caps.cudaDriverVersion / 1000) + "." +
                std::to_string((caps.cudaDriverVersion % 1000) / 10) +
                (caps.cudaMatchesD3D12Luid ? ", LUID matched to D3D12)" : ", LUID differs from D3D12)");
            cuda.status = BackendStatus::Substituted;
            cuda.health = HealthState::Ready;
            cuda.activeBackend = "D3D12_HLSL_Compute_Substitute";
            cuda.diagnosticCode = "CUDA_DRIVER_DETECTED_M0_BASELINE";
            cuda.humanReadableReason =
                "CUDA driver detected on " + caps.cudaMatchedDeviceName +
                "; M0 baseline launches without CUDA SDK dependency (use --bench or press B to execute & verify CUDA kernels).";
        } else {
            cuda.status = BackendStatus::Substituted;
            cuda.health = HealthState::ActiveSubstitute;
            cuda.activeBackend = "D3D12_HLSL_Compute_Substitute";
            cuda.diagnosticCode = "CUDA_DRIVER_ABSENT";
            cuda.humanReadableReason = "nvcuda.dll absent; using D3D12 HLSL substitute.";
        }
        caps.engines[static_cast<size_t>(EngineKind::CUDA)] = std::move(cuda);
    }

    {
        HardwareResult codec{};
        codec.engine = EngineKind::Codec;
        codec.requestedBackend = "NVENC_SignalHijack_Optional";
        if (flags.disableNVENC) {
            codec.status = BackendStatus::Substituted;
            codec.health = HealthState::ActiveSubstitute;
            codec.activeBackend = "SHADER_SIGNAL";
            codec.diagnosticCode = "DISABLED_BY_FLAG";
            codec.humanReadableReason = "NVENC disabled via --disable-nvenc; SHADER SIGNAL substitute ready.";
        } else if (!flags.enableSignalHijack) {
            codec.status = BackendStatus::Unavailable;
            codec.health = HealthState::Unavailable;
            codec.activeBackend = "not_loaded (SHADER_SIGNAL standby)";
            codec.diagnosticCode = "NOT_LOADED_BY_DEFAULT";
            codec.humanReadableReason =
                "Optional NVENC module is not_loaded by default per SPEC_v4 Section 3.5; timeline uses SHADER SIGNAL if needed.";
        } else if (caps.nvencDriverDllPresent || caps.hardwareVideoEncoderMftPresent) {
            codec.capabilityDetected = true;
            codec.detectedHardwareDetail =
                (caps.hardwareVideoEncoderMftPresent ? caps.hardwareVideoEncoderName : "nvEncodeAPI64.dll") +
                (caps.nvencMaxApiVersionMajor > 0
                     ? (" (NVENC API " + std::to_string(caps.nvencMaxApiVersionMajor) + "." +
                        std::to_string(caps.nvencMaxApiVersionMinor) + ")")
                     : "");
            codec.status = BackendStatus::Substituted;
            codec.health = HealthState::Ready;
            codec.activeBackend = "SHADER_SIGNAL";
            codec.diagnosticCode = "NVENC_CAPABLE_M9_OPTIONAL";
            codec.humanReadableReason =
                "Hardware encoder detected (" + codec.detectedHardwareDetail +
                "); M0 uses SHADER SIGNAL standby (use --bench or press B to benchmark hardware H.264 encode).";
        } else {
            codec.status = BackendStatus::Substituted;
            codec.health = HealthState::ActiveSubstitute;
            codec.activeBackend = "SHADER_SIGNAL";
            codec.diagnosticCode = "NVENC_NOT_PRESENT";
            codec.humanReadableReason = "Hardware encoder unavailable; using SHADER SIGNAL art-direction substitute.";
        }
        caps.engines[static_cast<size_t>(EngineKind::Codec)] = std::move(codec);
    }

    return caps;
}

const HardwareResult& getEngineResult(const SystemCapabilities& caps, EngineKind kind) noexcept {
    return caps.engines[static_cast<size_t>(kind)];
}

} // namespace boo
