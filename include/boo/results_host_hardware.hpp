#pragma once

#include <cstdint>

namespace boo {
// Public model identifiers only. Arbitrary CPUID/PnP strings, serials,
// instance IDs and user-chosen device names cannot enter the result card.
enum class ResultsCpuModel : uint8_t { Unknown, Intel, AMD, IntelCoreI9_13900H, IntelCoreUltra9_275HX };
enum class ResultsNpuModel : uint8_t { Unknown, NoneDetected, IntelAiBoost, AmdRyzenAI };
struct ResultsHostHardware {
    ResultsCpuModel cpu = ResultsCpuModel::Unknown;
    ResultsNpuModel npu = ResultsNpuModel::Unknown;
    uint16_t cpuFamily = 0, cpuModel = 0; // CPUID display family and model, never processor serial.
    uint16_t acceleratorVendor = 0, acceleratorDevice = 0; // Public PCI class device's model IDs.
};
} // namespace boo
