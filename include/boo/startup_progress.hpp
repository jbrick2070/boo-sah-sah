#pragma once

// A real loading bar. It counts bytes, not seconds. Four gauges, each read from the machine:
//  - asset files a loader has read into memory, each one checked on a worker thread against its
//    SHA-256 in the asset manifest;
//  - system memory the app has committed, as it fills;
//  - GPU memory the app has in use, as it fills;
//  - shader code the graphics driver has compiled into pipelines (see pipeline_compiler.hpp).
// The bar is the average of the four. Their totals are real too: the sizes on disk of the files
// this machine loaded on its last start, the system and GPU memory that start reached and the
// shader code it compiled, kept in a small file between runs. On a first run the file total is
// everything in the asset folders and the memory totals are not known yet, so the bar follows the
// files and the shaders alone.
//
// Under the bar the loading screen lists what start-up is doing: each stage and each pipeline being
// compiled, when it began and how long it has taken, so a screen that is waiting says what for.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace boo {

// What a line of the start-up list is about. The loading screen words these in the Windows language.
enum class StartupWork : uint8_t { Device, Files, Shaders, Scene };

// One line of the start-up list: a stage of the loading thread, or one pipeline compile.
struct StartupTask {
    uint32_t id = 0;
    StartupWork work = StartupWork::Scene;
    std::string item;       // the exact thing, a file or pipeline name (may be empty)
    double startedAt = 0.0; // seconds since start-up began
    double seconds = 0.0;   // how long it has taken so far, or took
    bool done = false;
};

struct StartupProgressView {
    float fraction = 0.0f;           // how full the bar is, 0..1
    uint64_t assetBytesLoaded = 0;   // read from disk into memory so far
    uint64_t assetBytesTotal = 0;    // what start-up is expected to read
    uint64_t assetBytesVerified = 0; // of the loaded bytes, those whose SHA-256 matched the manifest
    uint32_t assetsVerified = 0;     // files whose bytes matched
    uint32_t assetsMismatched = 0;   // files whose bytes differ from the manifest
    uint64_t ramBytes = 0;           // system memory the app has committed (its peak so far)
    uint64_t ramBytesExpected = 0;   // what the last start reached (0 on a first run)
    uint64_t gpuBytes = 0;           // GPU memory the app has in use now
    uint64_t gpuBytesExpected = 0;   // what the last start reached (0 on a first run)
    uint64_t shaderBytesCompiled = 0; // shader code compiled into pipelines so far
    uint64_t shaderBytesTotal = 0;    // what the last start compiled, or what is queued if that is more
    double seconds = 0.0;             // since start-up began
    bool firstStart = false;          // the first start of this build here: the driver has its shaders to compile
    std::vector<StartupTask> tasks;   // oldest first: everything running, and the latest that took a while
};

class StartupProgress {
public:
    struct Setup {
        std::filesystem::path memoryFile;                         // totals remembered between runs (empty = none)
        std::vector<std::filesystem::path> assetFolders;          // first run: everything start-up may load
        std::map<std::string, std::string, std::less<>> digests;  // "assets/..." -> SHA-256 from the manifest
        std::function<void(const StartupProgressView&)> onChange; // called only on the thread that called begin()
        std::string buildId;                                      // changes with every build (empty = not tracked)
    };

    static StartupProgress& instance();

    void begin(Setup setup);
    // How much GPU memory the app has in use. Set once the device exists; cleared by finish().
    void setGpuProbe(std::function<uint64_t()> gpuBytesInUse);
    void assetLoaded(const std::filesystem::path& file); // a loader has read this file into memory
    void checkpoint();                                   // a stage is done: sample GPU memory and repaint
    void step(const char* stage);                        // debug log: the stage's name and how long it took
    // The loading thread starts its next stage; the one before it is done. Repaints at once, so the
    // screen names a step before the step begins. Only on the thread that called begin().
    void stage(StartupWork work, std::string item = {});
    void stageDone();
    // A loader is about to read this file: shown beside the current stage if that is reading files.
    void reading(const std::filesystem::path& file);
    // Work that runs beside the stages, such as one pipeline compile on its own thread. Any thread.
    uint32_t taskBegin(StartupWork work, std::string item);
    void taskEnd(uint32_t id);
    // True while start-up runs and the caller is the thread that called begin(), the window's thread.
    bool onWindowThread() const;
    // True while start-up runs and the caller is loading: the window's thread, or the loader below.
    bool onLoadingThread() const;
    // A second thread that does the loading while the window's thread only paints (runWhilePainting). It
    // names stages and files like the window's thread, and it alone reads the GPU probe meanwhile, so the
    // window's thread never calls into the graphics driver while the driver is busy.
    void beginLoader();
    void endLoader();
    // Shader code a pipeline compile will turn into GPU code, and that compile being done. Any thread.
    void shaderQueued(uint64_t bytes);
    void shaderCompiled(uint64_t bytes);
    // Called again and again while start-up waits on other threads: read the gauges, repaint, and run
    // the pump so the window answers. Only on the thread that called begin(); elsewhere it does nothing.
    void idle();
    // What idle() runs to keep the window answering (its message loop). Cleared by finish().
    void setPump(std::function<void()> pump);
    void finish();                                       // the first frame is on screen
    void cancel();                                       // start-up is abandoned: nothing is kept for the next one
    // Blocks until every queued checksum has been checked (at most the given time). For shutdown and tests.
    bool waitForChecksums(uint32_t timeoutMs = 10000);

    StartupProgressView view() const;
    bool active() const noexcept;

    ~StartupProgress();

private:
    StartupProgress();
    struct State;
    State* state_;
};

// Shorthand for loaders and initialisation code. Both do nothing once start-up is over.
void startupAssetLoaded(const std::filesystem::path& file);
void startupCheckpoint();
// For the debug log: a stage has ended, and how long it took since the last one ended.
void startupStep(const char* stage);
// Shorthand for stage() and reading() above.
void startupStage(StartupWork work, std::string item = {});
void startupReading(const std::filesystem::path& file);
// Slow work started from the window's thread, shown as a stage. While start-up runs, `call` runs on a
// thread of its own (the loader, COM multithreaded) and the window's thread only paints the loading screen
// and answers the window until it returns; whatever it throws is thrown again here. At any other time, or
// on another thread, it simply runs. `call` must not leave anything behind that is tied to its thread.
void runWhilePainting(StartupWork work, std::string item, const std::function<void()>& call);

// "assets/textures/x.btex" for any path that ends that way; the file name otherwise.
std::string startupAssetKey(const std::filesystem::path& file);

} // namespace boo
