#pragma once

#include "boo/results_benchmark.hpp"
#include "boo/results_host_hardware.hpp"
#include <array>
#include <optional>

struct HWND__;

namespace boo {
struct ResultsDialogContext {
    BenchmarkRequest adapter{}; // only the LUID is used; the user selects the mode
    uint32_t vendorId = 0, deviceId = 0;
    ResultsHostHardware hardware{};
    std::array<BenchmarkReason, 3> availability{
        BenchmarkReason::UnsupportedDevice, BenchmarkReason::UnsupportedDevice,
        BenchmarkReason::UnsupportedDevice};
    // A busy or unavailable viewer can open a card without starting a child.
    BenchmarkReason initialFailure = BenchmarkReason::None;
    bool closeOnly = false; // No film window exists after a fatal startup failure.
    struct Startup {
        std::optional<double> coldSeconds, warmSeconds;
        bool thisLaunchCold = false;
        bool measuredThisLaunch = false;
    } startup;
};

// Modal native message loop: caller suspends film rendering and audio. Returns
// true if the viewer must close without submitting more work on its device.
bool openResultsDialog(HWND__* owner, const ResultsDialogContext& context);
} // namespace boo
