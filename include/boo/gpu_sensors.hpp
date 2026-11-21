#pragma once

// Live GPU sensors through NVIDIA's NVML (nvml.dll, installed with every GeForce driver).
// Loaded at runtime like nvcuda.dll: no SDK, no import, nothing to ship. On other vendors, or when
// the library is missing, available() stays false and the dashboard says N/A.
// A background thread samples four times a second so the render thread never waits on the driver.
// CPU load (system and this app) is sampled on the same thread on every machine, NVIDIA or not.

#include <cstdint>
#include <memory>
#include <string>

namespace boo {

struct GpuSensorReading {
    bool available = false;
    float temperatureC = 0.0f;
    float powerW = 0.0f;
    float powerLimitW = 0.0f;
    float graphicsClockMHz = 0.0f;
    float memoryClockMHz = 0.0f;
    float utilizationPct = 0.0f;
    float memoryUtilizationPct = 0.0f;
    float fanPct = -1.0f;          // < 0 when the GPU reports no fan (most laptops)
    bool powerLimited = false;     // driver is holding clocks down for power (SW power cap / power brake)
    bool thermalLimited = false;   // ... or for temperature (SW/HW thermal slowdown)

    // CPU (Windows only reports load to normal apps; temperature and power need a kernel driver)
    bool cpuAvailable = false;
    float cpuSystemPct = 0.0f;     // whole machine, all logical processors
    float cpuAppPct = 0.0f;        // this app, as a share of the whole machine
    uint32_t cpuLogicalProcessors = 0;
};

// Run-wide extremes for the diagnostics report.
struct GpuSensorSummary {
    uint32_t samples = 0;
    float peakTemperatureC = 0.0f;
    float peakPowerW = 0.0f;
    float averagePowerW = 0.0f;
    float averageUtilizationPct = 0.0f;
    float minGraphicsClockMHz = 0.0f;  // under load (utilization >= 50 %)
    float maxGraphicsClockMHz = 0.0f;
    uint32_t powerLimitedSamples = 0;
    uint32_t thermalLimitedSamples = 0;
    float averageCpuSystemPct = 0.0f;
    float peakCpuSystemPct = 0.0f;
    float averageCpuAppPct = 0.0f;
};

class GpuSensors {
public:
    GpuSensors();
    ~GpuSensors();
    GpuSensors(const GpuSensors&) = delete;
    GpuSensors& operator=(const GpuSensors&) = delete;

    // Follows exactly the GPU the app renders on. The D3D adapter LUID is matched to a CUDA device,
    // whose PCI bus id selects the NVML device, so two identical cards cannot be confused. Without
    // CUDA it falls back to a unique name match. If the render adapter is not an NVIDIA GPU (for
    // example the integrated GPU of a hybrid laptop) the sensors stay unavailable rather than
    // reporting a different GPU.
    // allowCudaLookup false (CUDA disabled by the user) skips the CUDA step and matches by name.
    bool start(const std::string& adapterName, int32_t luidLow, int32_t luidHigh, bool allowCudaLookup = true);
    void stop() noexcept;

    bool available() const noexcept;
    const std::string& deviceName() const noexcept;
    const std::string& matchMethod() const noexcept;  // "LUID" or "name"
    uint32_t nvidiaGpuCount() const noexcept;
    const std::string& unavailableReason() const noexcept;
    GpuSensorReading latest() const;
    GpuSensorSummary summary() const;
    // Starts the run statistics again. Called when playback begins so loading time is not averaged in.
    void resetSummary();

private:
    bool startNvml(const std::string& adapterName, int32_t luidLow, int32_t luidHigh, bool allowCudaLookup);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
