#pragma once

// The loading bar (include/boo/startup_progress.hpp) counts real bytes: files read, files whose
// SHA-256 matches the manifest, memory filled. It must never run backwards or fill early.

#include "boo/cues.hpp"
#include "boo/startup_progress.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
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

    fs::remove_all(root, ec);
    return true;
}

} // namespace
