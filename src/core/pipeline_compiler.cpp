#include "boo/pipeline_compiler.hpp"

#include "boo/logging.hpp"
#include "boo/startup_progress.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace boo {

struct PipelineCompiler::State {
    std::mutex mutex;
    std::condition_variable done;
    std::vector<std::thread> threads;
    uint32_t pending = 0;
    bool allSucceeded = true;

    void joinAll() {
        std::vector<std::thread> running;
        {
            std::lock_guard<std::mutex> lock(mutex);
            running.swap(threads);
        }
        for (auto& t : running) {
            if (t.joinable()) t.join();
        }
    }
};

PipelineCompiler::PipelineCompiler() : state_(std::make_unique<State>()) {}

PipelineCompiler::~PipelineCompiler() { state_->joinAll(); }

void PipelineCompiler::start(std::string name, uint64_t shaderBytes, std::function<bool()> create) {
    StartupProgress::instance().shaderQueued(shaderBytes);
    State* s = state_.get();
    std::lock_guard<std::mutex> lock(s->mutex);
    ++s->pending;
    s->threads.emplace_back([s, name = std::move(name), shaderBytes, create = std::move(create)] {
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = false;
        try {
            ok = create();
        } catch (const std::exception& e) {
            BOO_LOG_WARN("Startup", "Pipeline '" + name + "' failed: " + e.what());
        } catch (...) {
            BOO_LOG_WARN("Startup", "Pipeline '" + name + "' failed.");
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        char line[200]{};
        std::snprintf(line, sizeof(line), "Pipeline compiled in %.0f ms (%.0f KB of shader code): %s",
                      ms, static_cast<double>(shaderBytes) / 1024.0, name.c_str());
        BOO_LOG_DEBUG("Startup", line);
        StartupProgress::instance().shaderCompiled(shaderBytes);
        std::lock_guard<std::mutex> lock(s->mutex);
        --s->pending;
        if (!ok) s->allSucceeded = false;
        s->done.notify_all();
    });
}

bool PipelineCompiler::waitAll() {
    State& s = *state_;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            if (s.done.wait_for(lock, std::chrono::milliseconds(50), [&] { return s.pending == 0u; })) break;
        }
        // Still compiling: repaint the bar with what the gauges read now and answer the window.
        StartupProgress::instance().idle();
    }
    s.joinAll();
    StartupProgress::instance().idle();
    std::lock_guard<std::mutex> lock(s.mutex);
    const bool ok = s.allSucceeded;
    s.allSucceeded = true;
    return ok;
}

} // namespace boo
