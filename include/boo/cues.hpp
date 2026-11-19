#pragma once

#include "boo/api.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace boo {

struct SoloLoopRange {
    int64_t startSample = 0;
    int64_t endSample = 0;
};

struct CueSheet {
    int32_t schema = 1;
    std::string filmId;
    int64_t durationSamples = 17280000;
    int32_t sampleRate = 48000;
    uint64_t seed = 0;
    Edition edition = Edition::Family;
    std::vector<SceneCue> scenes;
    std::map<std::string, SoloLoopRange, std::less<>> soloLoops;
};

struct AssetManifestEntry {
    std::string id;
    std::string path;
    std::string kind;
    std::string provenance;
    std::string license;
    bool attributionRequired = false;
    std::string sourceUrl;
    std::string author;
    std::string sha256;
};

struct AssetManifest {
    int32_t schema = 1;
    std::string project;
    std::string milestone;
    std::vector<AssetManifestEntry> assets;
    std::vector<std::string> visualPresets;
};

struct ValidationIssue {
    bool isError = true;
    std::string code;
    std::string message;
};

struct ValidationReport {
    bool valid = true;
    std::vector<ValidationIssue> issues;

    void addError(std::string code, std::string message) {
        valid = false;
        issues.push_back(ValidationIssue{true, std::move(code), std::move(message)});
    }
    void addWarning(std::string code, std::string message) {
        issues.push_back(ValidationIssue{false, std::move(code), std::move(message)});
    }
};

bool loadCueSheetFromString(std::string_view jsonText, CueSheet& outSheet, ValidationReport& outReport);
bool loadCueSheetFromFile(const std::filesystem::path& path, CueSheet& outSheet, ValidationReport& outReport);
ValidationReport validateCueSheet(const CueSheet& sheet, bool requireFull360Film = false);

bool loadAssetManifestFromFile(const std::filesystem::path& path, AssetManifest& outManifest, ValidationReport& outReport);
void verifyManifestAssetFiles(const AssetManifest& manifest, const std::filesystem::path& repoRoot, ValidationReport& report);
void validateCueSheetAgainstManifest(
    const CueSheet& sheet,
    const AssetManifest& manifest,
    const std::filesystem::path& repoRoot,
    ValidationReport& ioReport);

size_t findSceneIndexAtSample(const CueSheet& sheet, int64_t samplePos) noexcept;
bool resolvePlaybackLoopRange(const CueSheet& sheet, PlaybackMode mode, SoloLoopRange& outRange) noexcept;
bool cueUsesM1Visuals(const CueSheet& sheet) noexcept;
ShowVisualMode resolveShowVisualMode(const CueSheet& sheet, PlaybackMode playbackMode = PlaybackMode::FullFilm) noexcept;
bool cueUses3dVisuals(const CueSheet& sheet) noexcept;

struct CaptionLine {
    bool active = false;
    bool isEndCard = false;
    float alpha = 0.0f;
    std::string text;
};

CaptionLine evaluateFilmCaption(
    ShowVisualMode mode,
    int64_t samplePosition,
    int32_t sampleRate,
    Edition edition,
    bool captionsEnabled) noexcept;

} // namespace boo
