#include "boo/cues.hpp"
#include "boo/clock.hpp"
#include "boo/json.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <windows.h>
#include <bcrypt.h>

namespace boo {

namespace {
bool isAssetPath(const std::string& value) {
    if (value.empty() || value.find_first_of(":\\") != std::string::npos) return false;
    const std::filesystem::path path(value);
    return !path.has_root_path() && path.has_filename() && *path.begin() != ".." &&
           path != "." && path.lexically_normal().generic_string() == value;
}

bool isSha256(std::string_view value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

std::string fileSha256(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!stream || !BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0))) {
        return {};
    }
    std::array<UCHAR, 64 * 1024> buffer;
    bool ok = true;
    while (stream.read(reinterpret_cast<char*>(buffer.data()), buffer.size()) || stream.gcount() > 0) {
        if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), static_cast<ULONG>(stream.gcount()), 0))) {
            ok = false;
            break;
        }
    }
    std::array<UCHAR, 32> digest{};
    ok = ok && stream.eof() && !stream.bad() &&
         BCRYPT_SUCCESS(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
    BCryptDestroyHash(hash);
    if (!ok) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (UCHAR byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

bool isValidPreferredEngine(std::string_view eng) {
    return eng == "RT" || eng == "Tensor" || eng == "NPU" ||
           eng == "CUDA" || eng == "Codec" || eng == "Raster" || eng == "Combined";
}

uint64_t mixDeterministicSeed(uint64_t baseSeed, std::string_view sceneId, size_t index) {
    uint64_t h = baseSeed ^ (0x9E3779B97F4A7C15ULL * (index + 1));
    for (unsigned char c : sceneId) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001B3ULL;
    }
    return h;
}
} // namespace

bool loadCueSheetFromString(std::string_view jsonText, CueSheet& outSheet, ValidationReport& outReport) {
    outReport = ValidationReport{};
    const JsonParseResult parsed = parseJson(jsonText);
    if (!parsed.ok) {
        outReport.addError(
            "JSON_PARSE_ERROR",
            "Line " + std::to_string(parsed.line) + ", Col " + std::to_string(parsed.column) + ": " + parsed.error);
        return false;
    }
    if (!parsed.value.isObject()) {
        outReport.addError("ROOT_NOT_OBJECT", "Cue sheet root must be a JSON object");
        return false;
    }

    const auto& root = parsed.value;
    outSheet = CueSheet{};
    outSheet.schema = static_cast<int32_t>(root["schema"].asInt64(-1));
    outSheet.filmId = root["film_id"].asString();
    outSheet.durationSamples = root["duration_samples"].asInt64(-1);
    outSheet.sampleRate = static_cast<int32_t>(root["sample_rate"].asInt64(-1));
    outSheet.seed = static_cast<uint64_t>(std::max<int64_t>(0, root["seed"].asInt64(0)));

    const std::string editionStr = root["edition"].asStringOr("family");
    if (!parseEdition(editionStr, outSheet.edition)) {
        outReport.addError("INVALID_EDITION", "Unsupported edition '" + editionStr + "' (expected 'family' or 'midnight')");
    }

    const JsonValue* scenesVal = root.find("scenes");
    if (!scenesVal || !scenesVal->isArray()) {
        outReport.addError("MISSING_SCENES", "Cue sheet requires a 'scenes' array");
        return false;
    }

    const auto& scenesArr = scenesVal->asArray();
    outSheet.scenes.reserve(scenesArr.size());
    for (size_t i = 0; i < scenesArr.size(); ++i) {
        const auto& s = scenesArr[i];
        if (!s.isObject()) {
            outReport.addError("SCENE_NOT_OBJECT", "Scene at index " + std::to_string(i) + " is not an object");
            continue;
        }
        SceneCue cue{};
        cue.sceneId = s["id"].asString();
        cue.startSample = s["start_sample"].asInt64(-1);
        cue.endSample   = s["end_sample"].asInt64(-1);
        cue.visualPreset = s["visual"].asString();
        cue.fallbackVisual = s["fallback_visual"].asString();
        cue.preferredEngine = s["preferred_engine"].asStringOr("Raster");
        cue.audioSection = s["audio_section"].asStringOr(cue.sceneId);
        cue.revealId = s["reveal"].asStringOr("");
        cue.revealSample = s["reveal_sample"].asInt64(-1);
        cue.allowDirectorModulation = s["allow_director_modulation"].asBool(true);

        const int32_t sr = outSheet.sampleRate > 0 ? outSheet.sampleRate : TimelineClock::kDefaultSampleRate;
        cue.startSeconds = TimelineClock::samplesToSeconds(cue.startSample, sr);
        cue.endSeconds   = TimelineClock::samplesToSeconds(cue.endSample, sr);
        cue.deterministicSeed = mixDeterministicSeed(outSheet.seed, cue.sceneId, i);
        outSheet.scenes.push_back(std::move(cue));
    }

    if (const JsonValue* loopsVal = root.find("solo_loops"); loopsVal && loopsVal->isObject()) {
        for (const auto& [loopName, rangeVal] : loopsVal->asObject()) {
            if (!rangeVal.isArray() || rangeVal.asArray().size() != 2 ||
                !rangeVal.asArray()[0].isNumber() || !rangeVal.asArray()[1].isNumber()) {
                outReport.addError(
                    "INVALID_SOLO_LOOP_FORMAT",
                    "solo_loops['" + loopName + "'] must be a 2-element numeric sample array [start, end]");
                continue;
            }
            SoloLoopRange range{};
            range.startSample = rangeVal.asArray()[0].asInt64(-1);
            range.endSample   = rangeVal.asArray()[1].asInt64(-1);
            outSheet.soloLoops[loopName] = range;
        }
    }

    const ValidationReport structural = validateCueSheet(
        outSheet,
        outSheet.filmId == "boo-sah-sah-glass-orchid");
    for (const auto& issue : structural.issues) {
        outReport.issues.push_back(issue);
        if (issue.isError) outReport.valid = false;
    }
    return outReport.valid;
}

bool loadCueSheetFromFile(const std::filesystem::path& path, CueSheet& outSheet, ValidationReport& outReport) {
    const JsonParseResult fileCheck = parseJsonFile(path);
    if (!fileCheck.ok) {
        outReport = ValidationReport{};
        outReport.addError(
            "FILE_OR_PARSE_ERROR",
            path.string() + " (" + std::to_string(fileCheck.line) + ":" + std::to_string(fileCheck.column) + "): " + fileCheck.error);
        return false;
    }
    return loadCueSheetFromString(serializeJson(fileCheck.value, 0), outSheet, outReport);
}

ValidationReport validateCueSheet(const CueSheet& sheet, bool requireFull360Film) {
    ValidationReport report{};

    if (sheet.schema != 1) {
        report.addError("UNSUPPORTED_SCHEMA", "Expected cue schema == 1, got " + std::to_string(sheet.schema));
    }
    if (sheet.filmId.empty()) {
        report.addError("EMPTY_FILM_ID", "film_id must not be empty");
    }
    if (sheet.sampleRate != TimelineClock::kDefaultSampleRate) {
        report.addError(
            "INVALID_SAMPLE_RATE",
            "Master cue sample_rate must be 48000 Hz, got " + std::to_string(sheet.sampleRate));
    }
    if (sheet.durationSamples <= 0) {
        report.addError("INVALID_DURATION", "duration_samples must be positive");
    }
    if (requireFull360Film && sheet.durationSamples != TimelineClock::kFullFilmSamples) {
        report.addError(
            "FULL_FILM_DURATION_MISMATCH",
            "Primary 360.000 s film must span exactly 17,280,000 samples @ 48 kHz, got " +
                std::to_string(sheet.durationSamples));
    }
    if (sheet.scenes.empty()) {
        report.addError("EMPTY_SCENE_LIST", "Cue sheet must contain at least one scene");
        return report;
    }

    std::set<std::string> seenIds;
    int64_t expectedStart = 0;

    for (size_t i = 0; i < sheet.scenes.size(); ++i) {
        const SceneCue& s = sheet.scenes[i];
        const std::string prefix = "Scene[" + std::to_string(i) + "] ('" + s.sceneId + "'): ";

        if (s.sceneId.empty()) {
            report.addError("EMPTY_SCENE_ID", prefix + "id must not be empty");
        } else if (!seenIds.insert(s.sceneId).second) {
            report.addError("DUPLICATE_SCENE_ID", prefix + "duplicate scene id");
        }

        if (s.startSample < 0) {
            report.addError("NEGATIVE_START_SAMPLE", prefix + "start_sample < 0");
        }
        if (s.endSample <= s.startSample) {
            report.addError("NON_POSITIVE_RANGE", prefix + "end_sample must be > start_sample");
        }

        if (s.startSample > expectedStart) {
            report.addError(
                "TIMELINE_GAP",
                prefix + "gap detected between sample " + std::to_string(expectedStart) +
                    " and " + std::to_string(s.startSample));
        } else if (s.startSample < expectedStart) {
            report.addError(
                "TIMELINE_OVERLAP",
                prefix + "overlap detected at sample " + std::to_string(s.startSample) +
                    " (previous ended at " + std::to_string(expectedStart) + ")");
        }
        expectedStart = s.endSample;

        if (s.visualPreset.empty()) {
            report.addError("MISSING_VISUAL", prefix + "visual preset name is empty");
        }
        if (s.fallbackVisual.empty()) {
            report.addError("MISSING_FALLBACK_VISUAL", prefix + "fallback_visual name is empty");
        }
        if (!isValidPreferredEngine(s.preferredEngine)) {
            report.addError(
                "INVALID_PREFERRED_ENGINE",
                prefix + "unknown preferred_engine '" + s.preferredEngine + "'");
        }
        if (s.revealSample >= 0 && (s.revealSample < s.startSample || s.revealSample >= s.endSample)) {
            report.addError("REVEAL_OUT_OF_SCENE_RANGE", prefix + "reveal_sample lies outside [start_sample, end_sample)");
        }
    }

    if (expectedStart != sheet.durationSamples) {
        report.addError(
            "INCOMPLETE_TIMELINE_COVERAGE",
            "Final scene ends at sample " + std::to_string(expectedStart) +
                ", expected duration_samples == " + std::to_string(sheet.durationSamples));
    }

    for (const auto& [name, loop] : sheet.soloLoops) {
        if (loop.startSample < 0 || loop.endSample <= loop.startSample || loop.endSample > sheet.durationSamples) {
            report.addError(
                "INVALID_SOLO_LOOP_RANGE",
                "solo_loops['" + name + "'] range [" + std::to_string(loop.startSample) + ", " +
                    std::to_string(loop.endSample) + ") is outside [0, " + std::to_string(sheet.durationSamples) + "]");
        }
    }

    return report;
}

bool loadAssetManifestFromFile(
    const std::filesystem::path& path,
    AssetManifest& outManifest,
    ValidationReport& outReport) {
    outReport = ValidationReport{};
    const JsonParseResult parsed = parseJsonFile(path);
    if (!parsed.ok || !parsed.value.isObject()) {
        outReport.addError("MANIFEST_PARSE_ERROR", "Failed to parse asset manifest: " + parsed.error);
        return false;
    }

    const auto& root = parsed.value;
    outManifest = AssetManifest{};
    outManifest.schema = static_cast<int32_t>(root["schema"].asInt64(-1));
    outManifest.project = root["project"].asString();
    outManifest.milestone = root["milestone"].asString();

    if (outManifest.schema != 1 && outManifest.schema != 2) {
        outReport.addError("MANIFEST_SCHEMA_ERROR", "Expected manifest schema 1 or 2");
    }

    if (const JsonValue* assetsVal = root.find("assets"); assetsVal && assetsVal->isArray() && !assetsVal->asArray().empty()) {
        std::set<std::string> ids, paths;
        for (size_t i = 0; i < assetsVal->asArray().size(); ++i) {
            const auto& item = assetsVal->asArray()[i];
            AssetManifestEntry e{};
            e.id = item["id"].asString();
            e.path = item["path"].asString();
            e.kind = item["kind"].asString();
            e.provenance = item["provenance"].asString();
            e.license = item["license"].asString();
            e.attributionRequired = item["attribution_required"].asBool(false);
            e.sourceUrl = item["source_url"].asString();
            e.author = item["author"].asString();
            e.sha256 = item["sha256"].asString();

            if (e.id.empty() || !isAssetPath(e.path) || !ids.insert(e.id).second || !paths.insert(e.path).second) {
                outReport.addError("MANIFEST_ENTRY_ID_OR_PATH", "Asset index " + std::to_string(i) + " has an invalid or duplicate id/path");
            }
            if (e.license.empty() || e.provenance.empty()) {
                outReport.addError(
                    "MANIFEST_LICENSE_OMISSION",
                    "Asset '" + e.id + "' is missing required license or provenance metadata");
            }
            if (outManifest.schema == 2 &&
                (e.kind.empty() || e.author.find_first_not_of(" \t\r\n") == std::string::npos ||
                 !e.sourceUrl.starts_with("https://") || e.sourceUrl.size() <= 8 ||
                 e.sourceUrl.find_first_of(" \t\r\n") != std::string::npos ||
                 !item["attribution_required"].isBool() || !isSha256(e.sha256))) {
                outReport.addError("MANIFEST_PROVENANCE_ERROR", "Asset '" + e.id + "' has invalid provenance fields");
            }
            outManifest.assets.push_back(std::move(e));
        }
    } else {
        outReport.addError("MANIFEST_MISSING_ASSETS", "Asset manifest requires an 'assets' array");
    }

    if (const JsonValue* vpVal = root.find("visual_presets"); vpVal && vpVal->isArray()) {
        for (const auto& v : vpVal->asArray()) {
            if (v.isString() && !v.asString().empty()) {
                outManifest.visualPresets.push_back(v.asString());
            }
        }
    }
    return outReport.valid;
}

void verifyManifestAssetFiles(const AssetManifest& manifest, const std::filesystem::path& repoRoot, ValidationReport& report) {
    for (const auto& asset : manifest.assets) {
        if (!isAssetPath(asset.path) || !isSha256(asset.sha256)) {
            report.addError("MANIFEST_INVALID_DIGEST_OR_PATH", "Invalid SHA-256 or asset path for '" + asset.id + "'");
            continue;
        }
        const auto hash = fileSha256(repoRoot / asset.path);
        if (hash.empty()) {
            report.addError("MANIFEST_ASSET_UNREADABLE", "Cannot read or hash '" + asset.path + "'");
        } else if (hash != asset.sha256) {
            report.addError("MANIFEST_SHA256_MISMATCH", "File bytes differ from the manifest for '" + asset.path + "'");
        }
    }
}

std::string sha256OfFile(const std::filesystem::path& path) { return fileSha256(path); }

void validateCueSheetAgainstManifest(
    const CueSheet& sheet,
    const AssetManifest& manifest,
    const std::filesystem::path& repoRoot,
    ValidationReport& ioReport) {
    std::set<std::string> knownPresets(manifest.visualPresets.begin(), manifest.visualPresets.end());

    for (const auto& s : sheet.scenes) {
        if (!knownPresets.contains(s.visualPreset)) {
            ioReport.addError(
                "UNREGISTERED_VISUAL_PRESET",
                "Scene '" + s.sceneId + "' references unregistered visual '" + s.visualPreset + "'");
        }
        if (!knownPresets.contains(s.fallbackVisual)) {
            ioReport.addError(
                "UNREGISTERED_FALLBACK_VISUAL",
                "Scene '" + s.sceneId + "' references unregistered fallback_visual '" + s.fallbackVisual + "'");
        }
    }

    if (!repoRoot.empty()) {
        std::error_code ec;
        for (const auto& a : manifest.assets) {
            const std::filesystem::path fullPath = repoRoot / a.path;
            if (!std::filesystem::exists(fullPath, ec)) {
                if (a.kind == "mesh") {
                    ioReport.addWarning("MESH_PROCEDURAL_FALLBACK", "Mesh '" + a.id + "' is missing; the renderer will use procedural geometry.");
                    continue;
                }
                ioReport.addError(
                    "MISSING_MANIFEST_ASSET_FILE",
                    "Manifest asset '" + a.id + "' does not exist on disk at '" + a.path + "'");
            }
        }
    }
}

size_t findSceneIndexAtSample(const CueSheet& sheet, int64_t samplePos) noexcept {
    if (sheet.scenes.empty()) return 0;
    if (samplePos <= sheet.scenes.front().startSample) return 0;
    if (samplePos >= sheet.scenes.back().endSample) return sheet.scenes.size() - 1;

    for (size_t i = 0; i < sheet.scenes.size(); ++i) {
        if (samplePos >= sheet.scenes[i].startSample && samplePos < sheet.scenes[i].endSample) {
            return i;
        }
    }
    return sheet.scenes.size() - 1;
}

bool resolvePlaybackLoopRange(const CueSheet& sheet, PlaybackMode mode, SoloLoopRange& outRange) noexcept {
    const char* key = nullptr;
    switch (mode) {
        case PlaybackMode::FullFilm:
            outRange = SoloLoopRange{0, sheet.durationSamples};
            return true;
        case PlaybackMode::SoloRT:     key = "rt"; break;
        case PlaybackMode::SoloTensor: key = "tensor"; break;
        case PlaybackMode::SoloNPU:    key = "npu"; break;
        case PlaybackMode::SoloCUDA:   key = "cuda"; break;
        case PlaybackMode::FinaleLoop: key = "finale"; break;
    }
    if (key) {
        const auto it = sheet.soloLoops.find(key);
        if (it != sheet.soloLoops.end()) {
            outRange = it->second;
            return true;
        }
    }
    outRange = SoloLoopRange{0, sheet.durationSamples};
    return false;
}

bool cueUsesM1Visuals(const CueSheet& sheet) noexcept {
    if (sheet.scenes.empty()) return false;
    for (const auto& s : sheet.scenes) {
        if (s.visualPreset.rfind("m1_", 0) != 0 && s.visualPreset.rfind("m2_", 0) != 0) {
            return false;
        }
    }
    return true;
}

ShowVisualMode resolveShowVisualMode(const CueSheet& sheet, PlaybackMode /*playbackMode*/) noexcept {
    if (sheet.scenes.empty() || sheet.filmId == "boo-sah-sah-emergency-fallback") {
        return ShowVisualMode::LegacyM0;
    }

    // Preserve strict rejection of mixed M0 + M1/M2 cue sheets
    bool anyM1OrM2 = false;
    bool allM1 = true;
    bool allM2 = true;
    for (const auto& s : sheet.scenes) {
        const bool isM1 = (s.visualPreset.rfind("m1_", 0) == 0);
        const bool isM2 = (s.visualPreset.rfind("m2_", 0) == 0);
        if (isM1 || isM2) anyM1OrM2 = true;
        if (!isM1) allM1 = false;
        if (!isM2) allM2 = false;
    }
    if (allM1) return ShowVisualMode::M1Slice60s;
    if (allM2) return ShowVisualMode::M2Trilogy180s;
    if (cueUsesM1Visuals(sheet)) {
        return (sheet.durationSamples == 8640000) ? ShowVisualMode::M2Trilogy180s : ShowVisualMode::M1Slice60s;
    }
    if (anyM1OrM2) {
        return ShowVisualMode::LegacyM0;
    }

    // Standalone hardware solo cue sheets
    if (sheet.filmId == "boo-sah-sah-solo-rt" ||
        (sheet.scenes.size() == 1 && (sheet.scenes[0].visualPreset == "mirror_orchid" ||
                                      sheet.scenes[0].visualPreset == "mirror_probes"))) {
        return ShowVisualMode::SoloRT;
    }
    if (sheet.filmId == "boo-sah-sah-solo-cuda" ||
        (sheet.scenes.size() == 1 && (sheet.scenes[0].visualPreset == "particle_mutiny_cuda" ||
                                      sheet.scenes[0].visualPreset == "particle_mutiny_hlsl"))) {
        return ShowVisualMode::SoloCUDA;
    }
    if (sheet.filmId == "boo-sah-sah-solo-tensor" ||
        (sheet.scenes.size() == 1 && (sheet.scenes[0].visualPreset == "neural_velvet_field" ||
                                      sheet.scenes[0].visualPreset == "neural_velvet_procedural"))) {
        return ShowVisualMode::SoloTensor;
    }
    if (sheet.filmId == "boo-sah-sah-solo-npu" ||
        (sheet.scenes.size() == 1 && (sheet.scenes[0].visualPreset == "madame_n_stage" ||
                                      sheet.scenes[0].visualPreset == "madame_n_scripted"))) {
        return ShowVisualMode::SoloNPU;
    }

    // Full 360.000s Glass Orchid 7-act film (or custom multi-act cue sheets using show presets)
    auto isShowPreset = [](std::string_view vp) noexcept {
        return vp == "intro_taipei_to_enamel" || vp == "intro_raster" ||
               vp == "mirror_orchid" || vp == "mirror_probes" ||
               vp == "neural_velvet_field" || vp == "neural_velvet_procedural" ||
               vp == "madame_n_stage" || vp == "madame_n_scripted" ||
               vp == "particle_mutiny_cuda" || vp == "particle_mutiny_hlsl" ||
               vp == "signal_hijack_nvenc" || vp == "signal_hijack_shader" ||
               vp == "silicon_rebellion_combined" || vp == "silicon_rebellion_raster";
    };
    bool allShow = true;
    for (const auto& s : sheet.scenes) {
        if (!isShowPreset(s.visualPreset)) {
            allShow = false;
            break;
        }
    }
    if (sheet.filmId == "boo-sah-sah-glass-orchid" || allShow) {
        return ShowVisualMode::FullFilm360s;
    }

    return ShowVisualMode::LegacyM0;
}

bool cueUses3dVisuals(const CueSheet& sheet) noexcept {
    return resolveShowVisualMode(sheet) != ShowVisualMode::LegacyM0;
}

CaptionLine evaluateFilmCaption(
    ShowVisualMode mode,
    int64_t samplePosition,
    int32_t sampleRate,
    Edition edition,
    bool captionsEnabled) noexcept {
    CaptionLine out{};
    if (mode != ShowVisualMode::FullFilm360s && mode != ShowVisualMode::SoloNPU) {
        return out;
    }

    const double sr = static_cast<double>(std::max<int32_t>(1, sampleRate));
    const double tRaw = std::max(0.0, static_cast<double>(samplePosition) / sr);
    const bool midnight = (edition == Edition::Midnight);

    auto smoothFade = [](double t, double startSec, double endSec, double fadeSec = 0.65) noexcept -> float {
        if (t < startSec || t > endSec) return 0.0f;
        const double fi = std::clamp((t - startSec) / std::max(fadeSec, 1e-4), 0.0, 1.0);
        const double fo = std::clamp((endSec - t) / std::max(fadeSec, 1e-4), 0.0, 1.0);
        const double u = std::min(fi, fo);
        return static_cast<float>(u * u * (3.0 - 2.0 * u));
    };

    if (mode == ShowVisualMode::SoloNPU) {
        if (!captionsEnabled) return out;
        const double t45 = std::fmod(tRaw, 45.0);
        if (t45 >= 4.0 && t45 <= 13.0) {
            out.active = true;
            out.alpha = smoothFade(t45, 4.0, 13.0);
            out.text = midnight
                ? "MADAME N: DARLINGS, PUT AWAY ROUGH HABITS. LISTEN CLOSELY."
                : "MADAME N: WELCOME! PLEASE DO NOT ZAP MY TINY RESIDENTS.";
        } else if (t45 >= 22.0 && t45 <= 34.0) {
            out.active = true;
            out.alpha = smoothFade(t45, 22.0, 34.0);
            out.text = midnight
                ? "MADAME N: GOOD FLORA KEEP THE PEACE. ECOLOGY OVER EXCESS."
                : "MADAME N: BENEFICIAL MICROBES! PROTECT BALANCE, NOT ZERO.";
        }
        return out;
    }

    const double t = std::clamp(tRaw, 0.0, 360.0);

    // Non-medical closing end card (SPEC_v4 Section 2.3) — tasteful, brief, always shown in final hold [351..360s]
    if (t >= 351.0 && t <= 360.0) {
        out.active = true;
        out.isEndCard = true;
        const double fi = std::clamp((t - 351.0) / 1.1, 0.0, 1.0);
        out.alpha = static_cast<float>(fi * fi * (3.0 - 2.0 * fi));
        out.text = "A CELEBRATION OF ORAL CARE - NOT MEDICAL ADVICE.";
        return out;
    }

    if (!captionsEnabled) {
        return out;
    }

    struct CueSub {
        double t0;
        double t1;
        const char* familyText;
        const char* midnightText;
    };

    static constexpr CueSub kSubs[] = {
        // Act 0 [0..45s): Taipei intro + Opening Scale Reveal @ 00:36.000
        {  4.0,  14.0,
          "MEI-LIN: XINYI RAIN ON GLASS... KEEP YOUR EYES UP, IRIS.",
          "MEI-LIN: RAIN, NEON, AND LATE COFFEE WITH YOU, DR. LIN." },
        { 33.5,  43.5,
          "DR. IRIS: NOT SKYSCRAPERS - HYDROXYAPATITE ENAMEL PRISMS!",
          "DR. IRIS: A WHOLE CITY CARVED IN MINERAL ENAMEL, MEI-LIN." },
        // Act 1 [45..100s): Mirror Intelligence (DXR)
        { 52.0,  64.0,
          "MEI-LIN: EIGHT MIRROR STELAE BOUNCING EVERY ORCHID CODE.",
          "MEI-LIN: CAREFUL IN THE MIRRORS - SECRETS DOUBLE IN GLASS." },
        // Act 2 [100..150s): Neural Velvet (Tensor)
        {108.0, 120.0,
          "DR. IRIS: LIVING BIOFILM RHYTHMS... FRIEND OR TROUBLE?",
          "DR. IRIS: VELVET CAUSTICS ALONG THE MARGIN. WHO RUNS THIS?" },
        // Act 3 [150..195s): Madame N (NPU) + Midpoint Ecology Reveal @ 02:55.000 (175.0s)
        {153.0, 164.0,
          "MADAME N: WELCOME! PLEASE DO NOT ZAP MY TINY RESIDENTS.",
          "MADAME N: DARLINGS, PUT AWAY ROUGH HABITS. LISTEN CLOSELY." },
        {172.0, 186.0,
          "MADAME N: BENEFICIAL MICROBES! PROTECT BALANCE, NOT ZERO.",
          "MADAME N: GOOD FLORA KEEP THE PEACE. ECOLOGY OVER EXCESS." },
        // Act 4 [195..245s): Particle Mutiny (CUDA) 5-city dental world medley
        {198.0, 210.0,
          "DR. IRIS: TORONTO PLAQUE TOWERS TO NEW ORLEANS BRASS!",
          "DR. IRIS: FIVE CITIES IN ONE BREATH - PH NEAR 5.5 DRIFT." },
        {222.0, 236.0,
          "MEI-LIN: BUFFERING SPRINGS AND MINERAL COASTS IN MOTION!",
          "MEI-LIN: SALIVA BUFFERS, FLUORIDE HELPS REMINERALIZE." },
        // Act 5 [245..275s): Signal Hijack (Codec)
        {248.0, 260.0,
          "MEI-LIN: SIGNAL HIJACK! EVEN COMPRESSED, THE ORCHID HOLDS.",
          "MEI-LIN: STATIC ON THE BAY CHANNEL... OUR SIGNAL SURVIVES." },
        // Act 6 [275..360s): Silicon Rebellion + Final Everyday Care Reveal @ 05:42.000 (342.0s)
        {280.0, 292.0,
          "MADAME N: ALL ENGINES TOGETHER! GRAND GLASS ORCHID BLOOM!",
          "MADAME N: EVERY INSTRUMENT IN HARMONY. ONE LAST DANCE." },
        {339.0, 350.0,
          "MEI-LIN: JUST MORNING LIGHT, A MIRROR, AND GENTLE CARE.",
          "DR. IRIS: DAWN AT THE VANITY MIRROR. SEE YOU TONIGHT." }
    };

    for (const auto& s : kSubs) {
        if (t >= s.t0 && t <= s.t1) {
            out.active = true;
            out.alpha = smoothFade(t, s.t0, s.t1);
            out.text = midnight ? s.midnightText : s.familyText;
            break;
        }
    }
    return out;
}

} // namespace boo
