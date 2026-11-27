#pragma once

#include "boo/results_benchmark.hpp"
#include <memory>

namespace boo {
enum class ResultsWorkerState { idle, starting, running, completed, cancelled, timedOut, failed };

class ResultsWorker {
public:
    ResultsWorker();
    ~ResultsWorker();
    ResultsWorker(const ResultsWorker&) = delete;
    ResultsWorker& operator=(const ResultsWorker&) = delete;

    // One attempt per instance. A previous child must have exited before another can run.
    bool start(const BenchmarkRequest& request);
    void cancel();
    ResultsWorkerState poll();
    BenchmarkSnapshot snapshot() const;
private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
};

// Dispatch before viewer startup. Private children inherit only the request mapping and progress pipe.
int runResultsWorkerIfRequested(int argc, char** argv);
} // namespace boo
