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

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace boo {

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
};

class StartupProgress {
public:
    struct Setup {
        std::filesystem::path memoryFile;                         // totals remembered between runs (empty = none)
        std::vector<std::filesystem::path> assetFolders;          // first run: everything start-up may load
        std::map<std::string, std::string, std::less<>> digests;  // "assets/..." -> SHA-256 from the manifest
        std::function<void(const StartupProgressView&)> onChange; // called only on the thread that called begin()
    };

    static StartupProgress& instance();

    void begin(Setup setup);
    // How much GPU memory the app has in use. Set once the device exists; cleared by finish().
    void setGpuProbe(std::function<uint64_t()> gpuBytesInUse);
    void assetLoaded(const std::filesystem::path& file); // a loader has read this file into memory
    void checkpoint();                                   // a stage is done: sample GPU memory and repaint
    void step(const char* stage);                        // debug log: the stage's name and how long it took
    // Shader code a pipeline compile will turn into GPU code, and that compile being done. Any thread.
    void shaderQueued(uint64_t bytes);
    void shaderCompiled(uint64_t bytes);
    // Called again and again while start-up waits on other threads: read the gauges, repaint, and run
    // the pump so the window answers. Only on the thread that called begin(); elsewhere it does nothing.
    void idle();
    // What idle() runs to keep the window answering (its message loop). Cleared by finish().
    void setPump(std::function<void()> pump);
    void finish();                                       // the first frame is on screen
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

// "assets/textures/x.btex" for any path that ends that way; the file name otherwise.
std::string startupAssetKey(const std::filesystem::path& file);

} // namespace boo
