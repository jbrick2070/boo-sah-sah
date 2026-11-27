#pragma once

// Compiles pipeline state objects on worker threads. A graphics driver's first compile of a large
// shader can take tens of seconds, and its shader cache starts empty after every app or driver
// update. So the big pipelines compile side by side while start-up carries on, and whoever waits
// for them keeps the loading screen painting. Each pipeline's shader bytes count on the loading
// bar when its compile is done: the bar counts work finished, never time.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace boo {

class PipelineCompiler {
public:
    PipelineCompiler();
    ~PipelineCompiler(); // waits for any job still compiling
    PipelineCompiler(const PipelineCompiler&) = delete;
    PipelineCompiler& operator=(const PipelineCompiler&) = delete;

    // Runs `create` on a thread of its own. It builds one pipeline (or a few that share one shader)
    // and returns whether it succeeded. Everything it reads must outlive waitAll(); a description
    // that the caller goes on to change is captured by value. `shaderBytes` is the size of the
    // shader code it compiles, for the loading bar.
    void start(std::string name, uint64_t shaderBytes, std::function<bool()> create);
    // Waits for every job started so far, keeping the loading screen painting meanwhile.
    // True if every one succeeded.
    bool waitAll();

private:
    struct State;
    std::unique_ptr<State> state_;
};

// One slow driver call on the loading thread, usually a single pipeline compile. While start-up runs,
// `create` runs on a thread of its own and the caller keeps the loading screen painting and the window
// answering until it returns, however long the driver takes. At any other time it simply runs.
// Returns what `create` returned (an HRESULT).
long compileWhilePainting(std::string name, uint64_t shaderBytes, const std::function<long()>& create);

// Test only (--slow-driver-ms): every pipeline compile first sleeps this long on whatever thread does
// it, and every GPU wait a quarter of it, to imitate a slow integrated-GPU driver. Zero (the default)
// changes nothing.
void setSimulatedSlowDriverMs(int ms);
int simulatedSlowDriverMs();

} // namespace boo
