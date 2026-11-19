#include "boo/api.hpp"
#include "boo/cues.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    using namespace boo;

    std::vector<std::filesystem::path> cueFiles;
    std::filesystem::path manifestFile = "assets/manifest.json";
    std::filesystem::path repoRoot;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--manifest" && i + 1 < argc) {
            manifestFile = argv[++i];
        } else if (arg == "--repo-root" && i + 1 < argc) {
            repoRoot = argv[++i];
        } else {
            cueFiles.emplace_back(arg);
        }
    }

    const bool useBundledCues = cueFiles.empty();
    if (useBundledCues) {
        cueFiles = {
            "assets/cues/m2_trilogy_180s.json",
            "assets/cues/m1_taipei_60s.json",
            "assets/cues/film_360s.json",
            "assets/cues/solo_rt.json",
            "assets/cues/solo_tensor.json",
            "assets/cues/solo_npu.json",
            "assets/cues/solo_cuda.json"
        };
    }

    AssetManifest manifest{};
    ValidationReport manifestRep{};
    const auto resolvedManifest = resolveResourcePath(manifestFile);
    if (repoRoot.empty()) {
        repoRoot = std::filesystem::absolute(resolvedManifest).parent_path().parent_path();
    }
    if (loadAssetManifestFromFile(resolvedManifest, manifest, manifestRep)) {
        verifyManifestAssetFiles(manifest, repoRoot, manifestRep);
    }
    if (!manifestRep.valid) {
        std::cerr << "[FAIL] Asset manifest validation failed (" << resolvedManifest.string() << "):\n";
        for (const auto& iss : manifestRep.issues) {
            std::cerr << "  - [" << iss.code << "] " << iss.message << "\n";
        }
        return 1;
    }

    bool allValid = true;
    for (const auto& cf : cueFiles) {
        const auto resolvedCue = useBundledCues ? repoRoot / cf : resolveResourcePath(cf);
        CueSheet sheet{};
        ValidationReport rep{};
        loadCueSheetFromFile(resolvedCue, sheet, rep);
        if (rep.valid) {
            validateCueSheetAgainstManifest(sheet, manifest, {}, rep);
        }
        if (rep.valid) {
            std::cout << "[PASS] " << resolvedCue.filename().string()
                      << " | film_id=" << sheet.filmId
                      << " | scenes=" << sheet.scenes.size()
                      << " | samples=" << sheet.durationSamples << "\n";
        } else {
            allValid = false;
            std::cerr << "[FAIL] " << resolvedCue.string() << ":\n";
            for (const auto& iss : rep.issues) {
                std::cerr << "  - [" << iss.code << "] " << iss.message << "\n";
            }
        }
    }

    return allValid ? 0 : 1;
}
