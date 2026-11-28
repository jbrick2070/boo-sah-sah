#pragma once

#include "boo/results_benchmark.hpp"
#include "boo/results_host_hardware.hpp"

namespace boo {
struct ResultsReference {
    const char* build;
    BenchmarkSnapshot snapshot;
    ResultsHostHardware hardware;
    double coldStartupSeconds, warmStartupSeconds;
};

// Points into immutable, bundled measurements from the designated reference rig.
// A partial result, a different build or incompatible run conditions have no reference.
const ResultsReference* resultsReferenceFor(const BenchmarkSnapshot& run) noexcept;
} // namespace boo
