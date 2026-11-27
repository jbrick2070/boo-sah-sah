#include "boo/startup_progress.hpp"

#include "boo/cues.hpp"
#include "boo/logging.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define PSAPI_VERSION 2 // K32GetProcessMemoryInfo, in kernel32
#include <windows.h>
#include <objbase.h>
#include <psapi.h>

namespace boo {

namespace {
uint64_t sizeOnDisk(const std::filesystem::path& file) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    return ec ? 0u : static_cast<uint64_t>(size);
}
// The most private memory this process has had committed at once. It only ever goes up.
uint64_t peakCommittedBytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
               ? static_cast<uint64_t>(counters.PeakPagefileUsage) : 0u;
}
constexpr double kListedSeconds = 0.15; // a finished step quicker than this leaves the list
constexpr size_t kListedTasks = 16;
std::string megabytes(uint64_t bytes) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}
} // namespace

std::string startupAssetKey(const std::filesystem::path& file) {
    const std::string generic = file.generic_string();
    const size_t at = generic.rfind("assets/");
    if (at != std::string::npos && (at == 0 || generic[at - 1] == '/')) return generic.substr(at);
    return file.filename().generic_string();
}

struct StartupProgress::State {
    mutable std::mutex mutex;
    std::atomic<bool> active{false};
    std::thread::id owner, loader; // loader: none unless runWhilePainting has one at work
    bool loadingHereLocked() const {
        const std::thread::id here = std::this_thread::get_id();
        return here == owner || here == loader;
    }
    // GPU memory, read by whichever thread may talk to the driver now (null for the others).
    std::function<uint64_t()> probeHereLocked() const {
        const std::thread::id here = std::this_thread::get_id();
        const bool mayAsk = loader == std::thread::id() ? here == owner : here == loader;
        return mayAsk ? gpuProbe : nullptr;
    }
    void sampleGpuLocked(uint64_t bytes) {
        gpuNow = bytes;
        gpuPeak = std::max(gpuPeak, bytes);
    }
    Setup setup;
    std::function<uint64_t()> gpuProbe;

    std::set<std::string, std::less<>> expected; // files the last start loaded (or the folders, on a first run)
    std::set<std::string, std::less<>> counted;  // files counted this start
    std::vector<std::pair<std::string, uint64_t>> loadedFiles; // key and bytes, in the order they loaded
    uint64_t loaded = 0, total = 0, verified = 0;
    uint32_t verifiedFiles = 0, mismatched = 0;
    uint64_t gpuNow = 0, gpuPeak = 0, gpuExpected = 0;
    uint64_t ramPeak = 0, ramExpected = 0;
    uint64_t shaderQueuedBytes = 0, shaderDoneBytes = 0, shaderExpected = 0;
    float fraction = 0.0f;
    std::chrono::steady_clock::time_point lastStep{}, lastIdleLog{};
    std::function<void()> pump;

    struct Task {
        uint32_t id;
        StartupWork work;
        std::string item;
        std::chrono::steady_clock::time_point begun, ended;
        bool done;
    };
    std::vector<Task> tasks; // in the order they began
    uint32_t nextTask = 1, stageTask = 0;
    std::chrono::steady_clock::time_point started{};
    bool firstStart = false;

    uint32_t beginTaskLocked(StartupWork work, std::string item) {
        const uint32_t id = nextTask++;
        tasks.push_back({id, work, std::move(item), std::chrono::steady_clock::now(), {}, false});
        return id;
    }
    void endTaskLocked(uint32_t id) {
        const auto it = std::find_if(tasks.begin(), tasks.end(), [id](const Task& t) { return t.id == id; });
        if (it == tasks.end() || it->done) return;
        it->done = true;
        it->ended = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(it->ended - it->begun).count();
        static constexpr const char* kWork[] = {"device", "files", "shaders", "scene"};
        char line[240]{};
        std::snprintf(line, sizeof(line), "Step took %.2f s, from +%.2f s: %s %s", seconds,
                      std::chrono::duration<double>(it->begun - started).count(),
                      kWork[static_cast<size_t>(it->work) & 3u], it->item.c_str());
        BOO_LOG_DEBUG("Startup", line);
        if (seconds < kListedSeconds) tasks.erase(it);
    }
    // The list as the screen shows it: everything still running, then the latest finished.
    std::vector<StartupTask> listLocked() const {
        const auto now = std::chrono::steady_clock::now();
        std::vector<const Task*> shown;
        for (const Task& t : tasks) {
            if (!t.done && shown.size() < kListedTasks) shown.push_back(&t);
        }
        for (auto it = tasks.rbegin(); it != tasks.rend() && shown.size() < kListedTasks; ++it) {
            if (it->done) shown.push_back(&*it);
        }
        std::sort(shown.begin(), shown.end(), [](const Task* a, const Task* b) { return a->id < b->id; });
        std::vector<StartupTask> list;
        list.reserve(shown.size());
        for (const Task* t : shown) {
            list.push_back({t->id, t->work, t->item, std::chrono::duration<double>(t->begun - started).count(),
                            std::chrono::duration<double>((t->done ? t->ended : now) - t->begun).count(), t->done});
        }
        return list;
    }

    // Checksums are checked off the loading thread, so they cost start-up no time.
    struct Job { std::filesystem::path file; std::string key, digest; uint64_t bytes; };
    std::deque<Job> jobs;
    uint32_t jobsRunning = 0;
    bool stop = false;
    bool summarise = false;
    std::condition_variable wake, idle;
    std::thread worker;

    StartupProgressView viewLocked() const {
        StartupProgressView v;
        v.fraction = fraction;
        v.assetBytesLoaded = loaded;
        v.assetBytesTotal = std::max(total, loaded);
        v.assetBytesVerified = verified;
        v.assetsVerified = verifiedFiles;
        v.assetsMismatched = mismatched;
        v.ramBytes = ramPeak;
        v.ramBytesExpected = ramExpected;
        v.gpuBytes = gpuNow;
        v.gpuBytesExpected = gpuExpected;
        v.shaderBytesCompiled = shaderDoneBytes;
        v.shaderBytesTotal = std::max(shaderExpected, shaderQueuedBytes);
        v.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        v.firstStart = firstStart;
        v.tasks = listLocked();
        return v;
    }

    void recomputeLocked() {
        ramPeak = std::max(ramPeak, peakCommittedBytes());
        const double all = static_cast<double>(std::max(total, loaded));
        const double files = all > 0.0 ? static_cast<double>(loaded) / all : 0.0;
        // Shader code compiled, of what the last start compiled (or of what is queued, if that is more).
        // A start that compiles no pipelines through the compiler has no shader gauge.
        const double shaderAll = static_cast<double>(std::max(shaderExpected, shaderQueuedBytes));
        const bool hasShaders = shaderAll > 0.0;
        const double shaders = hasShaders ? std::min(1.0, static_cast<double>(shaderDoneBytes) / shaderAll) : 0.0;
        double f;
        if (ramExpected > 0 && gpuExpected > 0) {
            // The average of the gauges.
            const double ram = std::min(1.0, static_cast<double>(ramPeak) / static_cast<double>(ramExpected));
            const double gpu = std::min(1.0, static_cast<double>(gpuPeak) / static_cast<double>(gpuExpected));
            f = hasShaders ? (files + ram + gpu + shaders) / 4.0 : (files + ram + gpu) / 3.0;
        } else {
            // First run: how much memory start-up needs is not known yet, so the files and the
            // shaders alone never quite fill the bar.
            f = (hasShaders ? (files + shaders) / 2.0 : files) * 0.9;
        }
        // Never backwards, and never full before the first frame is on screen.
        fraction = std::clamp(static_cast<float>(f), fraction, 0.99f);
    }

    void workerLoop() {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            wake.wait(lock, [&] { return stop || !jobs.empty(); });
            if (jobs.empty()) {
                if (stop) return;
                continue;
            }
            Job job = std::move(jobs.front());
            jobs.pop_front();
            ++jobsRunning;
            lock.unlock();
            const std::string digest = sha256OfFile(job.file);
            lock.lock();
            --jobsRunning;
            if (!digest.empty() && digest == job.digest) {
                verified += job.bytes;
                ++verifiedFiles;
            } else {
                ++mismatched;
                BOO_LOG_WARN("Assets", "Checksum mismatch: '" + job.key + "' is not the file the manifest describes.");
            }
            if (jobs.empty() && jobsRunning == 0u) {
                if (summarise) {
                    summarise = false;
                    BOO_LOG_INFO("Assets", "Start-up checksums: " + std::to_string(verifiedFiles) + " files, " +
                                           megabytes(verified) + " match the manifest's SHA-256; " +
                                           std::to_string(mismatched) + " mismatched.");
                }
                idle.notify_all();
            }
        }
    }
};

StartupProgress::StartupProgress() : state_(new State) {}

StartupProgress::~StartupProgress() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stop = true;
        state_->jobs.clear();
    }
    state_->wake.notify_all();
    if (state_->worker.joinable()) state_->worker.join();
    delete state_;
}

StartupProgress& StartupProgress::instance() {
    static StartupProgress progress;
    return progress;
}

bool StartupProgress::active() const noexcept { return state_->active.load(std::memory_order_relaxed); }

StartupProgressView StartupProgress::view() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->viewLocked();
}

void StartupProgress::begin(Setup setup) {
    auto& s = *state_;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.setup = std::move(setup);
    s.owner = std::this_thread::get_id();
    s.loader = {};
    s.gpuProbe = nullptr;
    s.expected.clear(); s.counted.clear(); s.loadedFiles.clear();
    s.loaded = s.total = s.verified = 0;
    s.verifiedFiles = s.mismatched = 0;
    s.gpuNow = s.gpuPeak = s.gpuExpected = 0;
    s.ramPeak = s.ramExpected = 0;
    s.shaderQueuedBytes = s.shaderDoneBytes = s.shaderExpected = 0;
    s.fraction = 0.0f;
    s.summarise = false;
    s.lastStep = s.lastIdleLog = s.started = std::chrono::steady_clock::now();
    s.tasks.clear();
    s.nextTask = 1;
    s.stageTask = 0;
    std::string rememberedBuild;

    // What the last start on this machine loaded, and the GPU memory it reached.
    if (!s.setup.memoryFile.empty()) {
        std::ifstream file(s.setup.memoryFile);
        std::string line;
        if (file && std::getline(file, line) && line == "boo-startup-memory 1") {
            while (std::getline(file, line) && s.expected.size() < 4096u) {
                if (line.rfind("gpu ", 0) == 0) {
                    try { s.gpuExpected = std::stoull(line.substr(4)); } catch (...) { s.gpuExpected = 0; }
                } else if (line.rfind("ram ", 0) == 0) {
                    try { s.ramExpected = std::stoull(line.substr(4)); } catch (...) { s.ramExpected = 0; }
                } else if (line.rfind("build ", 0) == 0) {
                    rememberedBuild = line.substr(6);
                } else if (line.rfind("shaders ", 0) == 0) {
                    try { s.shaderExpected = std::stoull(line.substr(8)); } catch (...) { s.shaderExpected = 0; }
                } else if (line.rfind("asset ", 0) == 0) {
                    // asset <bytes> <key>
                    const size_t space = line.find(' ', 6);
                    if (space == std::string::npos || space + 1 >= line.size()) continue;
                    uint64_t bytes = 0;
                    try { bytes = std::stoull(line.substr(6, space - 6)); } catch (...) { continue; }
                    if (bytes > 0 && s.expected.insert(line.substr(space + 1)).second) s.total += bytes;
                }
            }
        }
    }
    s.firstStart = s.expected.empty() || (!s.setup.buildId.empty() && rememberedBuild != s.setup.buildId);
    if (s.expected.empty()) {
        // First run (or nothing remembered): every file in the asset folders.
        s.gpuExpected = 0;
        s.ramExpected = 0;
        s.shaderExpected = 0;
        for (const auto& folder : s.setup.assetFolders) {
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                if (!s.expected.insert(startupAssetKey(it->path())).second) continue;
                const uint64_t bytes = sizeOnDisk(it->path());
                s.total += bytes;
                // The shader code there is to compile, so one quick compile does not read as all of it.
                if (it->path().extension() == ".cso") s.shaderExpected += bytes;
            }
        }
    }
    if (!s.worker.joinable()) s.worker = std::thread([&s] { s.workerLoop(); });
    s.active.store(true, std::memory_order_relaxed);
}

void StartupProgress::setGpuProbe(std::function<uint64_t()> gpuBytesInUse) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->gpuProbe = std::move(gpuBytesInUse);
}

void StartupProgress::assetLoaded(const std::filesystem::path& file) {
    auto& s = *state_;
    if (!s.active.load(std::memory_order_relaxed)) return;
    const uint64_t bytes = sizeOnDisk(file);
    std::string key = startupAssetKey(file);
    std::function<void(const StartupProgressView&)> notify;
    StartupProgressView v;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.active.load(std::memory_order_relaxed) || !s.counted.insert(key).second) return;
        s.loaded += bytes;
        if (!s.expected.contains(key)) s.total += bytes; // something the last start did not load
        s.loadedFiles.emplace_back(key, bytes);
        if (const auto it = s.setup.digests.find(key); it != s.setup.digests.end()) {
            s.jobs.push_back({file, key, it->second, bytes});
            s.wake.notify_one();
        }
        s.recomputeLocked();
        if (std::this_thread::get_id() == s.owner) { notify = s.setup.onChange; v = s.viewLocked(); }
    }
    if (notify) notify(v);
}

void StartupProgress::checkpoint() {
    auto& s = *state_;
    if (!s.active.load(std::memory_order_relaxed)) return;
    std::function<uint64_t()> probe;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.loadingHereLocked()) return;
        probe = s.probeHereLocked();
    }
    const uint64_t gpu = probe ? probe() : 0u;
    std::function<void(const StartupProgressView&)> notify;
    StartupProgressView v;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.active.load(std::memory_order_relaxed)) return;
        if (probe) s.sampleGpuLocked(gpu);
        s.recomputeLocked();
        if (std::this_thread::get_id() == s.owner) notify = s.setup.onChange;
        v = s.viewLocked();
    }
    {
        // For anyone following the bar in a debug log: the three gauges at each stage.
        char line[240]{};
        std::snprintf(line, sizeof(line), "Loading bar %.0f%%: files %.0f/%.0f MB, system memory %.0f/%.0f MB, GPU %.0f/%.0f MB, shaders %.2f/%.2f MB",
                      static_cast<double>(v.fraction) * 100.0, v.assetBytesLoaded / 1048576.0, v.assetBytesTotal / 1048576.0,
                      v.ramBytes / 1048576.0, v.ramBytesExpected / 1048576.0, v.gpuBytes / 1048576.0, v.gpuBytesExpected / 1048576.0,
                      v.shaderBytesCompiled / 1048576.0, v.shaderBytesTotal / 1048576.0);
        BOO_LOG_DEBUG("Startup", line);
    }
    if (notify) notify(v);
}

void StartupProgress::step(const char* stage) {
    auto& s = *state_;
    if (!s.active.load(std::memory_order_relaxed)) return;
    double ms = 0.0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto now = std::chrono::steady_clock::now();
        ms = std::chrono::duration<double, std::milli>(now - s.lastStep).count();
        s.lastStep = now;
    }
    char line[200]{};
    std::snprintf(line, sizeof(line), "Stage done in %.0f ms: %s", ms, stage ? stage : "");
    BOO_LOG_DEBUG("Startup", line);
}

void StartupProgress::stage(StartupWork work, std::string item) {
    auto& s = *state_;
    if (!s.active.load(std::memory_order_relaxed)) return;
    std::function<void(const StartupProgressView&)> notify;
    std::function<void()> pump;
    StartupProgressView v;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.active.load(std::memory_order_relaxed) || !s.loadingHereLocked()) return;
        if (s.stageTask != 0) s.endTaskLocked(s.stageTask);
        s.stageTask = s.beginTaskLocked(work, std::move(item));
        if (std::this_thread::get_id() == s.owner) {
            notify = s.setup.onChange;
            pump = s.pump;
            v = s.viewLocked();
        }
    }
    if (notify) notify(v);
    if (pump) pump();
}

void StartupProgress::stageDone() {
    auto& s = *state_;
    if (!s.active.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.loadingHereLocked() || s.stageTask == 0) return;
    s.endTaskLocked(s.stageTask);
    s.stageTask = 0;
}

void StartupProgress::reading(const std::filesystem::path& file) {
    auto& s = *state_;
    if (!s.active.load(std::memory_order_relaxed)) return;
    std::function<void(const StartupProgressView&)> notify;
    std::function<void()> pump;
    StartupProgressView v;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.active.load(std::memory_order_relaxed) || !s.loadingHereLocked()) return;
        const auto it = std::find_if(s.tasks.begin(), s.tasks.end(),
                                     [&](const State::Task& t) { return t.id == s.stageTask; });
        if (it == s.tasks.end() || it->done || it->work != StartupWork::Files) return;
        it->item = file.filename().generic_string();
        if (std::this_thread::get_id() == s.owner) {
            notify = s.setup.onChange;
            pump = s.pump;
            v = s.viewLocked();
        }
    }
    if (notify) notify(v);
    if (pump) pump();
}

uint32_t StartupProgress::taskBegin(StartupWork work, std::string item) {
    auto& s = *state_;
    if (!s.active.load(std::memory_order_relaxed)) return 0;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.active.load(std::memory_order_relaxed)) return 0;
    return s.beginTaskLocked(work, std::move(item));
}

void StartupProgress::taskEnd(uint32_t id) {
    auto& s = *state_;
    if (id == 0 || !s.active.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.endTaskLocked(id);
}

bool StartupProgress::onWindowThread() const {
    if (!state_->active.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return std::this_thread::get_id() == state_->owner;
}

bool StartupProgress::onLoadingThread() const {
    if (!state_->active.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->loadingHereLocked();
}

void StartupProgress::beginLoader() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->loader = std::this_thread::get_id();
}

void StartupProgress::endLoader() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->loader = {};
}

void StartupProgress::shaderQueued(uint64_t bytes) {
    auto& s = *state_;
    if (!s.active.load(std::memory_order_relaxed) || bytes == 0) return;
    std::function<void(const StartupProgressView&)> notify;
    StartupProgressView v;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.active.load(std::memory_order_relaxed)) return;
        s.shaderQueuedBytes += bytes;
        s.recomputeLocked();
        if (std::this_thread::get_id() == s.owner) { notify = s.setup.onChange; v = s.viewLocked(); }
    }
    if (notify) notify(v);
}

void StartupProgress::shaderCompiled(uint64_t bytes) {
    auto& s = *state_;
    if (!s.active.load(std::memory_order_relaxed) || bytes == 0) return;
    std::function<void(const StartupProgressView&)> notify;
    StartupProgressView v;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.active.load(std::memory_order_relaxed)) return;
        s.shaderDoneBytes += bytes;
        s.recomputeLocked();
        if (std::this_thread::get_id() == s.owner) { notify = s.setup.onChange; v = s.viewLocked(); }
    }
    if (notify) notify(v); // from a compile thread, the waiting thread's idle() repaints instead
}

void StartupProgress::idle() {
    auto& s = *state_;
    if (!s.active.load(std::memory_order_relaxed)) return;
    std::function<uint64_t()> probe;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.loadingHereLocked()) return;
        probe = s.probeHereLocked();
    }
    const uint64_t gpu = probe ? probe() : 0u;
    std::function<void(const StartupProgressView&)> notify;
    std::function<void()> pump;
    StartupProgressView v;
    bool log = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.active.load(std::memory_order_relaxed)) return;
        if (probe) s.sampleGpuLocked(gpu);
        if (std::this_thread::get_id() != s.owner) return; // the window's thread does the painting
        s.recomputeLocked();
        notify = s.setup.onChange;
        pump = s.pump;
        v = s.viewLocked();
        const auto now = std::chrono::steady_clock::now();
        if (now - s.lastIdleLog >= std::chrono::seconds(2)) { s.lastIdleLog = now; log = true; }
    }
    if (log) {
        // For anyone following the bar in a debug log while start-up waits.
        char line[240]{};
        std::snprintf(line, sizeof(line), "Loading bar %.0f%% while waiting: files %.0f/%.0f MB, system memory %.0f/%.0f MB, GPU %.0f/%.0f MB, shaders %.2f/%.2f MB",
                      static_cast<double>(v.fraction) * 100.0, v.assetBytesLoaded / 1048576.0, v.assetBytesTotal / 1048576.0,
                      v.ramBytes / 1048576.0, v.ramBytesExpected / 1048576.0, v.gpuBytes / 1048576.0, v.gpuBytesExpected / 1048576.0,
                      v.shaderBytesCompiled / 1048576.0, v.shaderBytesTotal / 1048576.0);
        BOO_LOG_DEBUG("Startup", line);
    }
    if (notify) notify(v);
    if (pump) pump();
}

void StartupProgress::setPump(std::function<void()> pump) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->pump = std::move(pump);
}

void StartupProgress::cancel() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->active.store(false);
    state_->gpuProbe = nullptr;
}

void StartupProgress::finish() {
    auto& s = *state_;
    if (!s.active.load(std::memory_order_relaxed)) return;
    std::function<uint64_t()> probe;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        probe = s.gpuProbe;
    }
    const uint64_t gpu = probe ? probe() : 0u;
    std::function<void(const StartupProgressView&)> notify;
    StartupProgressView v;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.active.exchange(false)) return;
        s.gpuNow = gpu;
        s.gpuPeak = std::max(s.gpuPeak, gpu);
        s.gpuProbe = nullptr;
        if (s.stageTask != 0) {
            s.endTaskLocked(s.stageTask);
            s.stageTask = 0;
        }
        s.ramPeak = std::max(s.ramPeak, peakCommittedBytes());
        s.fraction = 1.0f;
        // Remember what this start loaded and how much GPU memory it reached, for the next one.
        if (!s.setup.memoryFile.empty() && !s.loadedFiles.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(s.setup.memoryFile.parent_path(), ec);
            std::ofstream file(s.setup.memoryFile, std::ios::out | std::ios::trunc);
            if (file) {
                file << "boo-startup-memory 1\n" << "gpu " << s.gpuPeak << '\n' << "ram " << s.ramPeak << '\n'
                     << "shaders " << s.shaderDoneBytes << '\n';
                if (!s.setup.buildId.empty()) file << "build " << s.setup.buildId << '\n';
                for (const auto& [key, bytes] : s.loadedFiles) file << "asset " << bytes << ' ' << key << '\n';
            }
        }
        BOO_LOG_INFO("Startup", "Loaded " + megabytes(s.loaded) + " of assets in " + std::to_string(s.loadedFiles.size()) +
                                " files; system memory committed " + megabytes(s.ramPeak) +
                                "; GPU memory in use " + megabytes(s.gpuNow) + ".");
        if (s.jobs.empty() && s.jobsRunning == 0u) {
            BOO_LOG_INFO("Assets", "Start-up checksums: " + std::to_string(s.verifiedFiles) + " files, " +
                                   megabytes(s.verified) + " match the manifest's SHA-256; " +
                                   std::to_string(s.mismatched) + " mismatched.");
        } else {
            s.summarise = true; // the worker reports when the last one is checked
        }
        if (std::this_thread::get_id() == s.owner) notify = s.setup.onChange;
        v = s.viewLocked();
        s.setup.onChange = nullptr; // whatever it captured may not outlive start-up
        s.pump = nullptr;
    }
    if (notify) notify(v);
}

bool StartupProgress::waitForChecksums(uint32_t timeoutMs) {
    auto& s = *state_;
    std::unique_lock<std::mutex> lock(s.mutex);
    return s.idle.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return s.jobs.empty() && s.jobsRunning == 0u; });
}

void startupAssetLoaded(const std::filesystem::path& file) { StartupProgress::instance().assetLoaded(file); }
void startupCheckpoint() { StartupProgress::instance().checkpoint(); }

void startupStep(const char* stage) {
    auto& progress = StartupProgress::instance();
    progress.step(stage);
}
void startupStage(StartupWork work, std::string item) { StartupProgress::instance().stage(work, std::move(item)); }
void startupReading(const std::filesystem::path& file) { StartupProgress::instance().reading(file); }

void runWhilePainting(StartupWork work, std::string item, const std::function<void()>& call) {
    auto& progress = StartupProgress::instance();
    if (!progress.onWindowThread()) {
        call();
        return;
    }
    // Objects the loader creates stay usable after it ends: keep the process in the multithreaded apartment.
    static CO_MTA_USAGE_COOKIE apartment = nullptr;
    if (!apartment) (void)CoIncrementMTAUsage(&apartment);
    progress.stage(work, std::move(item));
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    std::exception_ptr error;
    std::thread worker([&] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        progress.beginLoader();
        try {
            call();
        } catch (...) {
            error = std::current_exception();
        }
        progress.endLoader();
        if (SUCCEEDED(com)) CoUninitialize();
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
        finished.notify_one();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (!finished.wait_for(lock, std::chrono::milliseconds(50), [&] { return done; })) {
            lock.unlock();
            progress.idle();
            lock.lock();
        }
    }
    worker.join();
    progress.checkpoint();
    progress.stageDone();
    if (error) std::rethrow_exception(error);
}

} // namespace boo
