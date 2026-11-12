#include "boo/benchmark.hpp"
#include "boo/capabilities.hpp"
#include "boo/diagnostics.hpp"
#include "boo/render.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    using namespace boo;

    BenchmarkConfig cfg{};
    cfg.timedIterations = 40;
    cfg.warmupIterations = 6;
    cfg.includeFallbacks = true;

    std::filesystem::path outJson;
    std::filesystem::path outCsv;
    bool forceWarp = false;
    bool noAnsi = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--iterations" || a == "-n") && i + 1 < argc) {
            cfg.timedIterations = static_cast<uint32_t>(std::clamp(std::atoi(argv[++i]), 3, 500));
        } else if (a == "--json" && i + 1 < argc) {
            outJson = argv[++i];
        } else if (a == "--csv" && i + 1 < argc) {
            outCsv = argv[++i];
        } else if (a == "--disable-rt") {
            cfg.disableRT = true;
        } else if (a == "--disable-cuda") {
            cfg.disableCUDA = true;
        } else if (a == "--disable-tensor") {
            cfg.disableTensor = true;
        } else if (a == "--disable-nvenc") {
            cfg.disableNVENC = true;
        } else if (a == "--no-fallbacks") {
            cfg.includeFallbacks = false;
        } else if (a == "--force-warp") {
            forceWarp = true;
        } else if (a == "--no-ansi") {
            noAnsi = true;
        } else if (a == "--help" || a == "-h") {
            std::cout << "boo_bench — Ad-Hoc Numerical Accelerator Benchmark Suite (RT, CUDA, Tensor, NVENC)\n"
                      << "Options:\n"
                      << "  -n, --iterations <N>   Timed passes per kernel (default: 40)\n"
                      << "  --json <path>          Export JSON benchmark report\n"
                      << "  --csv <path>           Export CSV benchmark report\n"
                      << "  --disable-rt           Skip DXR hardware raytracing\n"
                      << "  --disable-cuda         Skip CUDA PTX particle kernel\n"
                      << "  --disable-tensor       Skip Tensor Core mma.sync kernel\n"
                      << "  --disable-nvenc        Skip Hardware H.264 encoder MFT\n"
                      << "  --no-fallbacks         Skip D3D12 HLSL substitute comparison kernels\n"
                      << "  --force-warp           Run D3D12 kernels on Microsoft WARP software adapter\n";
            return 0;
        }
    }

    ProbeFlags pf{};
    pf.forceWarp = forceWarp;
    const SystemCapabilities caps = probeSystemCapabilities(pf);

    D3D12Context d3d;
    std::string err;
    if (!d3d.initialize(nullptr, 1920, 1080, forceWarp, false, &err)) {
        std::cerr << "Failed to initialize D3D12 context for benchmark: " << err << "\n";
        return 1;
    }

    std::cout << "Running BOO SAH SAH Glass Orchid numerical benchmark sweep on " << d3d.adapterName() << "...\n";

    const auto report = runAdHocBenchmarks(
        d3d.deviceRaw(),
        d3d.commandQueueRaw(),
        d3d.adapterLuidLow(),
        d3d.adapterLuidHigh(),
        d3d.adapterName(),
        caps.cpuBrand,
        cfg,
        [&](const BenchmarkProgressEvent& ev) {
            const int pct = static_cast<int>(ev.overallProgress01 * 100.0f);
            const int barFill = static_cast<int>(ev.overallProgress01 * 24.0f);
            std::string bar(24, '.');
            for (int k = 0; k < barFill && k < 24; ++k) bar[static_cast<size_t>(k)] = '#';
            char line[180]{};
            std::snprintf(
                line,
                sizeof(line),
                "\r  [%s] %3d%% | %-8s | %-44.44s | %8.2f %-8s",
                bar.c_str(),
                pct,
                ev.phaseLabel.c_str(),
                ev.activeStageTitle.c_str(),
                ev.liveMetricValue,
                ev.liveMetricUnit.c_str());
            std::cout << line << std::flush;
        });
    std::cout << "\r" << std::string(115, ' ') << "\r";

    printCreativeBenchmarkDashboard(report, !noAnsi);

    if (!outJson.empty()) {
        std::string jerr;
        if (exportBenchmarkJson(outJson, report, &jerr)) {
            std::cout << "Exported JSON report -> " << outJson.string() << "\n";
        } else {
            std::cerr << jerr << "\n";
        }
    }
    if (!outCsv.empty()) {
        std::string cerr;
        if (exportBenchmarkCsv(outCsv, report, &cerr)) {
            std::cout << "Exported CSV report  -> " << outCsv.string() << "\n";
        } else {
            std::cerr << cerr << "\n";
        }
    }

    d3d.shutdown();
    return 0;
}
