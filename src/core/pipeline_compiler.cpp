#include "boo/pipeline_compiler.hpp"

#include "boo/logging.hpp"
#include "boo/startup_progress.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace boo {

namespace {
std::atomic<int> gSlowDriverMs{0};
void slowDriverCompilePause() {
    const int ms = gSlowDriverMs.load(std::memory_order_relaxed);
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
} // namespace

void setSimulatedSlowDriverMs(int ms) { gSlowDriverMs.store(ms > 0 ? ms : 0, std::memory_order_relaxed); }
int simulatedSlowDriverMs() { return gSlowDriverMs.load(std::memory_order_relaxed); }

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
    const uint32_t task = StartupProgress::instance().taskBegin(StartupWork::Shaders, name);
    State* s = state_.get();
    std::lock_guard<std::mutex> lock(s->mutex);
    ++s->pending;
    s->threads.emplace_back([s, task, name = std::move(name), shaderBytes, create = std::move(create)] {
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = false;
        try {
            slowDriverCompilePause();
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
        StartupProgress::instance().taskEnd(task);
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

long compileWhilePainting(std::string name, uint64_t shaderBytes, const std::function<long()>& create) {
    auto& progress = StartupProgress::instance();
    if (!progress.onLoadingThread()) {
        slowDriverCompilePause();
        return create();
    }
    progress.stageDone(); // the list now shows this compile, not the stage before it
    long result = static_cast<long>(0x80004005L); // E_FAIL, if create throws
    PipelineCompiler one;
    one.start(std::move(name), shaderBytes, [&] {
        result = create();
        return result >= 0;
    });
    one.waitAll();
    return result;
}

} // namespace boo
