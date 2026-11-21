#include "boo/gpu_sensors.hpp"

#include "boo/logging.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace boo {

namespace {

// The handful of NVML entry points used here (stable C ABI, cdecl, return 0 on success).
using NvmlDevice = void*;
struct NvmlUtilization { unsigned int gpu; unsigned int memory; };
constexpr int kNvmlSuccess = 0;
constexpr int kNvmlTemperatureGpu = 0;
constexpr int kNvmlClockGraphics = 0;
constexpr int kNvmlClockMemory = 2;
// clocksEventReasons bits
constexpr unsigned long long kReasonSwPowerCap = 0x4ull;
constexpr unsigned long long kReasonHwSlowdown = 0x8ull;
constexpr unsigned long long kReasonSwThermal = 0x20ull;
constexpr unsigned long long kReasonHwThermal = 0x40ull;
constexpr unsigned long long kReasonHwPowerBrake = 0x80ull;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

struct GpuSensors::Impl {
    HMODULE lib = nullptr;
    int (*init)() = nullptr;
    int (*shutdown)() = nullptr;
    int (*getCount)(unsigned int*) = nullptr;
    int (*getHandle)(unsigned int, NvmlDevice*) = nullptr;
    int (*getName)(NvmlDevice, char*, unsigned int) = nullptr;
    int (*getTemperature)(NvmlDevice, int, unsigned int*) = nullptr;
    int (*getPower)(NvmlDevice, unsigned int*) = nullptr;
    int (*getPowerLimit)(NvmlDevice, unsigned int*) = nullptr;
    int (*getClock)(NvmlDevice, int, unsigned int*) = nullptr;
    int (*getUtilization)(NvmlDevice, NvmlUtilization*) = nullptr;
    int (*getFan)(NvmlDevice, unsigned int*) = nullptr;
    int (*getReasons)(NvmlDevice, unsigned long long*) = nullptr;
    int (*getByBusId)(const char*, NvmlDevice*) = nullptr;
    std::string method;
    uint32_t gpuCount = 0;

    NvmlDevice device = nullptr;
    bool initialized = false;
    std::string name;
    std::string reason = "Not started";

    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread worker;
    GpuSensorReading reading;
    GpuSensorSummary sum;
    double powerTotal = 0.0, utilTotal = 0.0, cpuSysTotal = 0.0, cpuAppTotal = 0.0;
    uint32_t cpuSamples = 0;
    uint64_t lastIdle = 0, lastKernel = 0, lastUser = 0, lastProc = 0, lastWall = 0;
    uint32_t logical = 0;

    static uint64_t ft(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

    // System load from idle vs busy time; app load from this process's CPU time over wall time.
    void sampleCpu(GpuSensorReading& r) {
        FILETIME idle, kernel, user, create, exit, pk, pu, now;
        if (!GetSystemTimes(&idle, &kernel, &user) ||
            !GetProcessTimes(GetCurrentProcess(), &create, &exit, &pk, &pu)) return;
        GetSystemTimeAsFileTime(&now);
        const uint64_t i = ft(idle), k = ft(kernel), u = ft(user), p = ft(pk) + ft(pu), w = ft(now);
        if (lastWall != 0 && w > lastWall) {
            const uint64_t total = (k - lastKernel) + (u - lastUser); // kernel time includes idle
            const uint64_t busy = total - (i - lastIdle);
            if (total > 0) r.cpuSystemPct = 100.0f * float(double(busy) / double(total));
            const double wall = double(w - lastWall) * std::max(1u, logical);
            r.cpuAppPct = float(std::clamp(100.0 * double(p - lastProc) / wall, 0.0, 100.0));
            r.cpuAvailable = true;
        }
        lastIdle = i; lastKernel = k; lastUser = u; lastProc = p; lastWall = w;
        r.cpuLogicalProcessors = logical;
    }

    template <typename F> bool bind(F& fn, const char* symbol) {
        fn = reinterpret_cast<F>(GetProcAddress(lib, symbol));
        return fn != nullptr;
    }

    GpuSensorReading sample() {
        GpuSensorReading r;
        sampleCpu(r);
        if (!device) return r;
        r.available = true;
        unsigned int v = 0;
        if (getTemperature(device, kNvmlTemperatureGpu, &v) == kNvmlSuccess) r.temperatureC = static_cast<float>(v);
        if (getPower(device, &v) == kNvmlSuccess) r.powerW = v * 0.001f;
        if (getPowerLimit && getPowerLimit(device, &v) == kNvmlSuccess) r.powerLimitW = v * 0.001f;
        if (getClock(device, kNvmlClockGraphics, &v) == kNvmlSuccess) r.graphicsClockMHz = static_cast<float>(v);
        if (getClock(device, kNvmlClockMemory, &v) == kNvmlSuccess) r.memoryClockMHz = static_cast<float>(v);
        NvmlUtilization u{};
        if (getUtilization(device, &u) == kNvmlSuccess) {
            r.utilizationPct = static_cast<float>(u.gpu);
            r.memoryUtilizationPct = static_cast<float>(u.memory);
        }
        if (getFan && getFan(device, &v) == kNvmlSuccess) r.fanPct = static_cast<float>(v);
        unsigned long long reasons = 0;
        if (getReasons && getReasons(device, &reasons) == kNvmlSuccess) {
            r.powerLimited = (reasons & (kReasonSwPowerCap | kReasonHwPowerBrake)) != 0;
            r.thermalLimited = (reasons & (kReasonSwThermal | kReasonHwThermal | kReasonHwSlowdown)) != 0;
        }
        return r;
    }

    void record(const GpuSensorReading& r) {
        std::lock_guard<std::mutex> lock(mutex);
        reading = r;
        if (r.available) {
        ++sum.samples;
        sum.peakTemperatureC = std::max(sum.peakTemperatureC, r.temperatureC);
        sum.peakPowerW = std::max(sum.peakPowerW, r.powerW);
        powerTotal += r.powerW;
        utilTotal += r.utilizationPct;
        sum.averagePowerW = static_cast<float>(powerTotal / sum.samples);
        sum.averageUtilizationPct = static_cast<float>(utilTotal / sum.samples);
        if (r.utilizationPct >= 50.0f && r.graphicsClockMHz > 0.0f) {
            sum.minGraphicsClockMHz = sum.minGraphicsClockMHz > 0.0f ? std::min(sum.minGraphicsClockMHz, r.graphicsClockMHz)
                                                                    : r.graphicsClockMHz;
        }
        sum.maxGraphicsClockMHz = std::max(sum.maxGraphicsClockMHz, r.graphicsClockMHz);
        if (r.powerLimited) ++sum.powerLimitedSamples;
        if (r.thermalLimited) ++sum.thermalLimitedSamples;
        }
        if (r.cpuAvailable) {
            ++cpuSamples;
            cpuSysTotal += r.cpuSystemPct;
            cpuAppTotal += r.cpuAppPct;
            sum.averageCpuSystemPct = float(cpuSysTotal / cpuSamples);
            sum.averageCpuAppPct = float(cpuAppTotal / cpuSamples);
            sum.peakCpuSystemPct = std::max(sum.peakCpuSystemPct, r.cpuSystemPct);
        }
    }

    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        while (!stopping) {
            lock.unlock();
            const GpuSensorReading r = sample();
            record(r);
            lock.lock();
            wake.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopping; });
        }
    }
};

GpuSensors::GpuSensors() : impl_(std::make_unique<Impl>()) {}
GpuSensors::~GpuSensors() { stop(); }

namespace {
// Map a D3D adapter LUID to a PCI bus id through the CUDA driver API (nvcuda.dll, shipped with the driver).
std::string pciBusIdForLuid(int32_t luidLow, int32_t luidHigh) {
    HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda) return {};
    using CuInit = int (*)(unsigned int);
    using CuCount = int (*)(int*);
    using CuGet = int (*)(int*, int);
    using CuLuid = int (*)(char*, unsigned int*, int);
    using CuBus = int (*)(char*, int, int);
    auto cuInit = reinterpret_cast<CuInit>(GetProcAddress(cuda, "cuInit"));
    auto cuCount = reinterpret_cast<CuCount>(GetProcAddress(cuda, "cuDeviceGetCount"));
    auto cuGet = reinterpret_cast<CuGet>(GetProcAddress(cuda, "cuDeviceGet"));
    auto cuLuid = reinterpret_cast<CuLuid>(GetProcAddress(cuda, "cuDeviceGetLuid"));
    auto cuBus = reinterpret_cast<CuBus>(GetProcAddress(cuda, "cuDeviceGetPCIBusId"));
    std::string bus;
    int count = 0;
    if (cuInit && cuCount && cuGet && cuLuid && cuBus && cuInit(0) == 0 && cuCount(&count) == 0) {
        for (int i = 0; i < count && bus.empty(); ++i) {
            int dev = 0;
            char luid[8]{};
            unsigned int mask = 0;
            if (cuGet(&dev, i) != 0 || cuLuid(luid, &mask, dev) != 0) continue;
            int32_t low = 0, high = 0;
            std::memcpy(&low, luid, 4);
            std::memcpy(&high, luid + 4, 4);
            if (low == luidLow && high == luidHigh) {
                char id[32]{};
                if (cuBus(id, static_cast<int>(sizeof(id)), dev) == 0) bus = id;
            }
        }
    }
    // nvcuda.dll stays loaded: the live accelerators may hold it too, and unloading CUDA mid-process is unsafe.
    return bus;
}
} // namespace

bool GpuSensors::start(const std::string& adapterName, int32_t luidLow, int32_t luidHigh, bool allowCudaLookup) {
    Impl& m = *impl_;
    if (m.worker.joinable()) return available();
    m.logical = static_cast<uint32_t>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    const bool gpu = startNvml(adapterName, luidLow, luidHigh, allowCudaLookup);
    if (gpu) {
        m.reason.clear();
        BOO_LOG_INFO("Sensors", "NVML GPU sensors active on " + m.name + " (matched by " + m.method + ", " +
                     std::to_string(m.gpuCount) + " NVIDIA GPU" + (m.gpuCount == 1 ? "" : "s") + " present)");
    }
    // CPU load is sampled on every machine; the first sample only primes the counters.
    m.stopping = false;
    m.record(m.sample());
    m.worker = std::thread([&m] { m.run(); });
    return gpu;
}

bool GpuSensors::startNvml(const std::string& adapterName, int32_t luidLow, int32_t luidHigh, bool allowCudaLookup) {
    Impl& m = *impl_;
    m.lib = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m.lib) {
        m.reason = "nvml.dll not found (no NVIDIA driver)";
        return false;
    }
    const bool bound = m.bind(m.init, "nvmlInit_v2") && m.bind(m.shutdown, "nvmlShutdown") &&
        m.bind(m.getCount, "nvmlDeviceGetCount_v2") && m.bind(m.getHandle, "nvmlDeviceGetHandleByIndex_v2") &&
        m.bind(m.getName, "nvmlDeviceGetName") && m.bind(m.getTemperature, "nvmlDeviceGetTemperature") &&
        m.bind(m.getPower, "nvmlDeviceGetPowerUsage") && m.bind(m.getClock, "nvmlDeviceGetClockInfo") &&
        m.bind(m.getUtilization, "nvmlDeviceGetUtilizationRates");
    m.bind(m.getPowerLimit, "nvmlDeviceGetEnforcedPowerLimit");
    m.bind(m.getFan, "nvmlDeviceGetFanSpeed");
    if (!m.bind(m.getReasons, "nvmlDeviceGetCurrentClocksEventReasons"))
        m.bind(m.getReasons, "nvmlDeviceGetCurrentClocksThrottleReasons");
    m.bind(m.getByBusId, "nvmlDeviceGetHandleByPciBusId_v2");
    if (!bound || m.init() != kNvmlSuccess) {
        m.reason = bound ? "nvmlInit failed" : "nvml.dll is missing expected functions";
        FreeLibrary(m.lib);
        m.lib = nullptr;
        return false;
    }
    m.initialized = true;
    unsigned int count = 0;
    m.getCount(&count);
    m.gpuCount = count;
    // 1. Exact: render adapter LUID -> CUDA device -> PCI bus id -> NVML device.
    if (m.getByBusId && allowCudaLookup) {
        const std::string bus = pciBusIdForLuid(luidLow, luidHigh);
        NvmlDevice d = nullptr;
        if (!bus.empty() && m.getByBusId(bus.c_str(), &d) == kNvmlSuccess && d) {
            char buf[96]{};
            m.getName(d, buf, sizeof(buf));
            m.device = d;
            m.name = buf;
            m.method = "LUID";
        }
    }
    // 2. Fallback: a name match, accepted only when exactly one NVIDIA GPU carries that name.
    if (!m.device) {
        const std::string wanted = lower(adapterName);
        uint32_t matches = 0;
        for (unsigned int i = 0; i < count && !wanted.empty(); ++i) {
            NvmlDevice d = nullptr;
            char buf[96]{};
            if (m.getHandle(i, &d) != kNvmlSuccess) continue;
            m.getName(d, buf, sizeof(buf));
            const std::string n = lower(buf);
            if (!n.empty() && (wanted.find(n) != std::string::npos || n.find(wanted) != std::string::npos)) {
                if (++matches == 1) { m.device = d; m.name = buf; }
            }
        }
        if (matches > 1) { m.device = nullptr; m.name.clear(); }
        if (m.device) m.method = "name";
        else if (count == 0) m.reason = "NVML reports no NVIDIA GPU";
        else if (matches > 1) m.reason = "Several identical NVIDIA GPUs and no CUDA mapping; cannot tell which one renders";
        else m.reason = "Rendering on a non-NVIDIA GPU (" + adapterName + ")";
    }
    if (!m.device) {
        if (m.shutdown) m.shutdown();
        m.initialized = false;
        FreeLibrary(m.lib);
        m.lib = nullptr;
        return false;
    }
    return true;
}

void GpuSensors::stop() noexcept {
    Impl& m = *impl_;
    {
        std::lock_guard<std::mutex> lock(m.mutex);
        m.stopping = true;
    }
    m.wake.notify_all();
    if (m.worker.joinable()) m.worker.join();
    if (m.initialized && m.shutdown) m.shutdown();
    m.initialized = false;
    if (m.lib) FreeLibrary(m.lib);
    m.lib = nullptr;
    m.device = nullptr;
}

bool GpuSensors::available() const noexcept { return impl_->device != nullptr && impl_->initialized; }
const std::string& GpuSensors::deviceName() const noexcept { return impl_->name; }
const std::string& GpuSensors::unavailableReason() const noexcept { return impl_->reason; }
const std::string& GpuSensors::matchMethod() const noexcept { return impl_->method; }
uint32_t GpuSensors::nvidiaGpuCount() const noexcept { return impl_->gpuCount; }

GpuSensorReading GpuSensors::latest() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->reading;
}

void GpuSensors::resetSummary() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sum = GpuSensorSummary{};
    impl_->powerTotal = impl_->utilTotal = impl_->cpuSysTotal = impl_->cpuAppTotal = 0.0;
    impl_->cpuSamples = 0;
}

GpuSensorSummary GpuSensors::summary() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sum;
}

} // namespace boo
