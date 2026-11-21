#pragma once

// The PhysX suite's shared foundation (docs/physx_suite_plan.md, step 1).
//
// One NVIDIA PhysX runtime for the whole app: the foundation, the physics object, a CPU dispatcher
// and a CUDA context manager on the GPU the film renders on. The CUDA context is that GPU's primary
// context, the same one the app's CUDA workloads already use, so there is never a second context.
//
// Starting is slow (the CUDA context and PhysX's GPU library take seconds), so it happens on a
// worker thread: the frame never waits. Genres create their own scene on the runtime when dialled
// in and release it when dialled out, so only one genre holds GPU memory at a time.
//
// Builds without ENABLE_PHYSX compile the same interface and report the runtime unavailable.

#include <cstdint>
#include <memory>
#include <string>

namespace boo {

enum class PhysxRuntimeState : uint32_t { Unavailable = 0, Stopped, Starting, Ready };

struct PhysxRuntimeInfo {
    PhysxRuntimeState state = PhysxRuntimeState::Unavailable;
    std::string reason;          // why it is unavailable, when it is
    std::string sdkVersion;      // e.g. "5.13.0"
    std::string cudaDevice;      // GPU running the GPU solvers; empty when CPU only
    bool gpu = false;            // the GPU solvers are available
    bool sharedContext = false;  // PhysX uses the GPU's primary CUDA context, as the rest of the app does
    float startMs = 0.0f;        // how long the last start took
};

// What the dashboard row and diagnostics report for the active genre. Each genre fills the counters
// it has and names them; the row prints the first three.
struct PhysxStats {
    uint32_t genre = 0;                    // PhysxGenre
    uint32_t state = 0;                    // 0 unavailable, 1 off, 2 active, 3 starting
    std::string backend;
    std::string reason;
    float stepCpuMs = 0.0f;                // CPU time to submit a step
    float stepGpuMs = 0.0f;                // GPU time of a step where the library measures it
    float stepWallMs = 0.0f;               // simulate to results, where only that can be measured
    float renderGpuMs = 0.0f;              // GPU time to draw the genre
    float counters[4]{};
    const char* counterNames[4]{"", "", "", ""};
    float gpuMemoryMegabytes = 0.0f;
    uint64_t steps = 0;
};

class PhysxSuite {
public:
    // adapterLuidLow/High: the D3D12 adapter the film renders on.
    PhysxSuite(int32_t adapterLuidLow, int32_t adapterLuidHigh);
    ~PhysxSuite();
    PhysxSuite(const PhysxSuite&) = delete;
    PhysxSuite& operator=(const PhysxSuite&) = delete;

    void requestStart();      // begins starting on a worker thread; does nothing if started or starting
    void stop();              // waits for a start in progress, then releases everything
    bool waitUntilSettled(uint32_t timeoutMs); // for tests and shutdown: true once not Starting
    PhysxRuntimeInfo info() const;

    // For genre code: the live objects, valid only while state is Ready (opaque here so the PhysX
    // headers stay out of the rest of the app). Cast to physx::PxPhysics*, PxCpuDispatcher*,
    // PxCudaContextManager* (nullptr when there is no GPU).
    void* physics() const noexcept;
    void* dispatcher() const noexcept;
    void* cudaContextManager() const noexcept;

    // A quick end-to-end check for tests: an empty scene with GPU dynamics where available, stepped
    // a few times. Returns false with a reason if anything fails.
    bool selfTest(std::string* reason);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
