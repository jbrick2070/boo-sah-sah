#pragma once

// The loading bar (include/boo/startup_progress.hpp) counts real bytes: files read, files whose
// SHA-256 matches the manifest, memory filled, shader code compiled. It must never run backwards
// or fill early.

#include "boo/cues.hpp"
#include "boo/pipeline_compiler.hpp"
#include "boo/startup_progress.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

bool testStartupProgress() {
    using namespace boo;
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "boo_startup_progress_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "assets" / "meshes", ec);
    auto write = [&](const char* name, size_t bytes, char fill) {
        const fs::path file = root / "assets" / "meshes" / name;
        std::ofstream(file, std::ios::binary) << std::string(bytes, fill);
        return file;
    };
    const fs::path a = write("a.bin", 300000u, 'a'), b = write("b.bin", 100000u, 'b'), c = write("c.bin", 50000u, 'c');

    BOO_CHECK(startupAssetKey(a) == "assets/meshes/a.bin");
    BOO_CHECK(startupAssetKey("C:/somewhere/else/readme.txt") == "readme.txt");
    const std::string digestA = sha256OfFile(a);
    BOO_CHECK(digestA.size() == 64u && digestA != sha256OfFile(b) && sha256OfFile(root / "missing.bin").empty());

    auto& progress = StartupProgress::instance();
    std::vector<float> seen;
    StartupProgress::Setup first;
    first.memoryFile = root / "memory.txt";
    first.assetFolders = {root / "assets"};
    first.digests["assets/meshes/a.bin"] = digestA;
    first.digests["assets/meshes/b.bin"] = std::string(64, '0'); // not b's digest, on purpose
    first.onChange = [&](const StartupProgressView& v) { seen.push_back(v.fraction); };
    progress.begin(first);
    BOO_CHECK(progress.active());
    auto v = progress.view();
    // A first run expects everything in the folders and does not yet know how much memory start-up needs.
    BOO_CHECK(v.assetBytesTotal == 450000u && v.assetBytesLoaded == 0u && v.fraction == 0.f);
    BOO_CHECK(v.ramBytesExpected == 0u && v.gpuBytesExpected == 0u);

    progress.assetLoaded(a);
    progress.assetLoaded(a); // the same file is counted once
    v = progress.view();
    BOO_CHECK(v.assetBytesLoaded == 300000u);
    BOO_CHECK(v.fraction > 0.55f && v.fraction < 0.65f); // 300 of 450, held to nine tenths on a first run
    progress.assetLoaded(b);
    progress.setGpuProbe([] { return uint64_t(64) << 20; });
    progress.checkpoint();
    BOO_CHECK(progress.waitForChecksums());
    v = progress.view();
    BOO_CHECK(v.assetBytesLoaded == 400000u);
    BOO_CHECK(v.assetBytesVerified == 300000u && v.assetsVerified == 1u && v.assetsMismatched == 1u);
    BOO_CHECK(v.ramBytes > 0u && v.gpuBytes == (uint64_t(64) << 20));
    BOO_CHECK(v.fraction < 1.f); // never full before the first frame
    BOO_CHECK(!seen.empty() && std::is_sorted(seen.begin(), seen.end())); // never backwards

    progress.finish();
    BOO_CHECK(!progress.active() && progress.view().fraction == 1.f && seen.back() == 1.f);
    progress.assetLoaded(c); // start-up is over: ignored
    progress.checkpoint();
    BOO_CHECK(progress.view().assetBytesLoaded == 400000u);

    // The next start knows what that one loaded and the memory it reached.
    seen.clear();
    StartupProgress::Setup second;
    second.memoryFile = root / "memory.txt";
    second.assetFolders = {root / "assets"};
    second.onChange = [&](const StartupProgressView& view) { seen.push_back(view.fraction); };
    progress.begin(second);
    v = progress.view();
    BOO_CHECK(v.assetBytesTotal == 400000u); // a.bin and b.bin, not c.bin
    BOO_CHECK(v.ramBytesExpected > 0u && v.gpuBytesExpected == (uint64_t(64) << 20));
    progress.setGpuProbe([] { return uint64_t(32) << 20; }); // half of last time's GPU memory so far
    progress.assetLoaded(a);
    progress.checkpoint();
    v = progress.view();
    // Three gauges: files 3/4, system memory at or near its mark, GPU memory at half.
    BOO_CHECK(v.fraction > 0.55f && v.fraction < 0.80f);
    progress.assetLoaded(c); // something new this time widens the total instead of overfilling the bar
    BOO_CHECK(progress.view().assetBytesTotal == 450000u && progress.view().fraction < 1.f);
    BOO_CHECK(std::is_sorted(seen.begin(), seen.end()));
    progress.finish();
    BOO_CHECK(progress.waitForChecksums());
    BOO_CHECK(progress.view().fraction == 1.f);

    // The shader gauge: pipelines compile side by side on the compiler's threads, and the thread that
    // waits for them keeps repainting the bar and answering the window.
    seen.clear();
    int pumped = 0;
    StartupProgress::Setup third;
    third.memoryFile = root / "memory.txt";
    third.assetFolders = {root / "assets"};
    third.onChange = [&](const StartupProgressView& view) { seen.push_back(view.fraction); };
    progress.begin(third);
    progress.setPump([&] { ++pumped; });
    progress.setGpuProbe([] { return uint64_t(64) << 20; });
    BOO_CHECK(progress.view().shaderBytesTotal == 0u); // the last start compiled nothing
    progress.assetLoaded(a);
    {
        PipelineCompiler compiler;
        std::atomic<int> started{0};
        std::atomic<bool> overlapped{false};
        auto job = [&](bool result) {
            return [&started, &overlapped, result] {
                ++started;
                // Both run at once: each sees the other start (or gives up after two seconds).
                for (int i = 0; i < 200 && started.load() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (started.load() == 2) overlapped = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(150)); // the waiter repaints meanwhile
                return result;
            };
        };
        compiler.start("test pipeline A", 3000u, job(true));
        compiler.start("test pipeline B", 1000u, job(false));
        v = progress.view();
        BOO_CHECK(v.shaderBytesTotal == 4000u && v.shaderBytesCompiled == 0u);
        BOO_CHECK(!compiler.waitAll()); // B failed
        BOO_CHECK(overlapped.load());
        BOO_CHECK(progress.view().shaderBytesCompiled == 4000u); // a failed compile is done all the same
        BOO_CHECK(pumped > 0);
        compiler.start("test pipeline C", 500u, [] { return true; });
        BOO_CHECK(compiler.waitAll()); // the next batch starts clean
        BOO_CHECK(progress.view().shaderBytesCompiled == 4500u);
    }
    BOO_CHECK(std::is_sorted(seen.begin(), seen.end()));
    BOO_CHECK(progress.view().fraction < 1.f);
    progress.finish();
    BOO_CHECK(progress.waitForChecksums());

    // The next start expects the shader code that one compiled.
    StartupProgress::Setup fourth;
    fourth.memoryFile = root / "memory.txt";
    fourth.assetFolders = {root / "assets"};
    progress.begin(fourth);
    BOO_CHECK(progress.view().shaderBytesTotal == 4500u);
    progress.shaderQueued(4500u);
    const float before = progress.view().fraction;
    progress.shaderCompiled(4500u);
    v = progress.view();
    BOO_CHECK(v.shaderBytesCompiled == 4500u && v.fraction > before + 0.2f); // one of four gauges, now full
    progress.finish();
    BOO_CHECK(progress.waitForChecksums());

    // The list of steps: each stage and compile is named while it runs, with its time, and the first start of
    // a build says so. A slow compile on the loading thread runs beside it, and the screen keeps painting.
    {
        StartupProgress::Setup listed;
        listed.memoryFile = root / "list.txt";
        listed.assetFolders = {root / "assets"};
        listed.buildId = "build-1";
        std::vector<StartupProgressView> views;
        listed.onChange = [&](const StartupProgressView& view) { views.push_back(view); };
        int pumps = 0;
        progress.begin(listed);
        progress.setPump([&] { ++pumps; });
        BOO_CHECK(progress.onLoadingThread() && progress.view().firstStart);
        progress.stage(StartupWork::Files);
        progress.reading(a);
        progress.assetLoaded(a); // only a start that loaded something is remembered
        BOO_CHECK(!views.empty() && views.back().tasks.size() == 1u && views.back().tasks[0].item == "a.bin" &&
                  !views.back().tasks[0].done);
        progress.stage(StartupWork::Scene); // the quick file stage leaves the list
        BOO_CHECK(progress.view().tasks.size() == 1u && progress.view().tasks[0].work == StartupWork::Scene);
        views.clear();
        pumps = 0;
        const std::thread::id loading = std::this_thread::get_id();
        std::atomic<bool> beside{false};
        const long result = compileWhilePainting("slow.cso", 100u, [&] {
            beside = std::this_thread::get_id() != loading;
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            return 0L;
        });
        BOO_CHECK(result == 0L && beside.load() && pumps >= 3); // painted and answered while it compiled
        bool sawRunning = false;
        for (const auto& view : views) {
            for (const auto& task : view.tasks) sawRunning |= task.item == "slow.cso" && !task.done && task.seconds > 0.2;
        }
        BOO_CHECK(sawRunning);
        const auto after = progress.view().tasks;
        BOO_CHECK(!after.empty() && after.back().item == "slow.cso" && after.back().done && after.back().seconds >= 0.35);
        BOO_CHECK(compileWhilePainting("fails.cso", 1u, [] { return -1L; }) == -1L);
        // Any other slow call likewise: a stage on a thread of its own, its failure passed back to the caller.
        pumps = 0;
        bool ran = false, threw = false;
        runWhilePainting(StartupWork::Device, "slow check", [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            ran = std::this_thread::get_id() != loading;
        });
        BOO_CHECK(ran && pumps >= 3 && progress.view().tasks.back().item == "slow check" && progress.view().tasks.back().done);
        try {
            runWhilePainting(StartupWork::Device, "broken", [] { throw std::runtime_error("broken"); });
        } catch (const std::runtime_error&) {
            threw = true;
        }
        BOO_CHECK(threw);
        // The loader names its own stages, files and compiles; only the window's thread paints and pumps.
        std::atomic<int> paintedElsewhere{0};
        progress.setPump([&] {
            ++pumps;
            if (std::this_thread::get_id() != loading) ++paintedElsewhere;
        });
        pumps = 0;
        bool loaderOk = false;
        runWhilePainting(StartupWork::Scene, {}, [&] {
            const bool here = progress.onLoadingThread() && !progress.onWindowThread();
            progress.stage(StartupWork::Files);
            progress.reading(b);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const auto reading = progress.view().tasks;
            const long compiled = compileWhilePainting("inside.cso", 10u, [] {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                return 0L;
            });
            const auto listed = progress.view().tasks;
            loaderOk = here && compiled == 0L && !reading.empty() && reading.back().item == "b.bin" &&
                       std::any_of(listed.begin(), listed.end(), [](const StartupTask& t) { return t.item == "inside.cso"; });
        });
        BOO_CHECK(loaderOk && pumps >= 5 && paintedElsewhere.load() == 0);
        BOO_CHECK(progress.onWindowThread());
        progress.finish();
        BOO_CHECK(!progress.onLoadingThread());
        BOO_CHECK(compileWhilePainting("later.cso", 1u, [&] { return std::this_thread::get_id() == loading ? 7L : -1L; }) == 7L);
        BOO_CHECK(progress.waitForChecksums());
        progress.begin(listed);
        BOO_CHECK(!progress.view().firstStart); // the same build again
        progress.finish();
        listed.buildId = "build-2";
        progress.begin(listed);
        BOO_CHECK(progress.view().firstStart); // an update
        progress.finish();
        BOO_CHECK(progress.waitForChecksums());
    }

    fs::remove_all(root, ec);
    return true;
}

} // namespace
