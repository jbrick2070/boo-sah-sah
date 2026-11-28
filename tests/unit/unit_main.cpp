#include "boo/api.hpp"
#include "boo/audio.hpp"
#include "boo/audio_reactive.hpp"
#include "boo/bmesh.hpp"
#include "boo/city_modules.hpp"
#include <fstream>
#include "boo/clock.hpp"
#include "boo/cues.hpp"
#include "boo/director.hpp"
#include "boo/health.hpp"
#include "boo/json.hpp"
#include "boo/localization.hpp"
#include "boo/logging.hpp"
#include "boo/m1_scene.hpp"
#include "boo/media_input.hpp"
#include "boo/settings.hpp"
#include "boo/spout_sender.hpp"
#include "../../src/app/world_portal.h"
#include "../../src/render/bird_flight.hpp"
#include "boo/ui_text.hpp"
#include "boo/results_text.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int gTestsRun = 0;
int gTestsFailed = 0;

#define BOO_CHECK(cond)                                                                                  \
    do {                                                                                                 \
        if (!(cond)) {                                                                                   \
            std::cerr << "  [FAIL] " << __FILE__ << ":" << __LINE__ << " -> " << #cond << "\n";          \
            return false;                                                                                \
        }                                                                                                \
    } while (0)

void runTest(const char* name, bool (*fn)()) {
    ++gTestsRun;
    if (fn()) {
        std::cout << "[PASS] " << name << "\n";
    } else {
        ++gTestsFailed;
        std::cerr << "[FAIL] " << name << "\n";
    }
}

bool hasErrorCode(const boo::ValidationReport& rep, std::string_view code) {
    for (const auto& iss : rep.issues) {
        if (iss.isError && iss.code == code) return true;
    }
    return false;
}

bool testPublicWorldLocators() {
    using boo::isPublicWorldLocator;
    const std::wstring path = L"/worlds/9a693db7-51ee-4622-a118-1966f080c8a8";
    const std::wstring address = L"https://room-1.host.example" + path;
    BOO_CHECK(isPublicWorldLocator(address));
    BOO_CHECK(boo::worldVisitorUri(address) == L"demoverse://visit/room-1.host.example" + path);
    BOO_CHECK(boo::worldVisitorUri(address + L"#invite").empty());
    for (const auto* origin : {L"http://host.example", L"file://host.example", L"https://local",
             L"https://user@host.example", L"https://host.example:443", L"https://host.example.",
             L"https://host..example", L"https://-host.example", L"https://host-.example",
             L"https://host.123", L"https://HOST.example", L"https://host.example\\other",
             L"https://host.example%2fother", L"https://example.test?x=", L"https://example.test#x"}) {
        BOO_CHECK(!isPublicWorldLocator(origin + path));
    }
    for (const auto* suffix : {L"#invite", L"?arg=1", L"/", L"\r\n", L" "})
        BOO_CHECK(!isPublicWorldLocator(address + suffix));
    BOO_CHECK(!isPublicWorldLocator(address.substr(0, address.size() - 1)));
    BOO_CHECK(!isPublicWorldLocator(L"https://host.example/worlds/9a693db7-51ee-4622-A118-1966f080c8a8"));
    const std::wstring label(63, L'a');
    const auto maxAddress = L"https://" + label + L"." + label + L"." + label + L"." + std::wstring(61, L'b') + path;
    BOO_CHECK(maxAddress.size() == 305 && isPublicWorldLocator(maxAddress));
    BOO_CHECK(!isPublicWorldLocator(L"https://" + label + L"a.example" + path));
    return true;
}

bool testJsonParserAndSerializer() {
    using namespace boo;
    const char* raw = R"({
        "title": "BOO SAH SAH \u2014 Glass Orchid",
        "samples": 17280000,
        "active": true,
        "empty": null,
        "tags": ["rt", "tensor", "npu", "cuda"]
    })";
    const auto parsed = parseJson(raw);
    BOO_CHECK(parsed.ok);
    BOO_CHECK(parsed.value.isObject());
    BOO_CHECK(parsed.value["samples"].asInt64() == 17280000);
    BOO_CHECK(parsed.value["active"].asBool() == true);
    BOO_CHECK(parsed.value["empty"].isNull());
    BOO_CHECK(parsed.value["tags"].asArray().size() == 4);
    BOO_CHECK(parsed.value["tags"].asArray()[2].asString() == "npu");

    const std::string roundTrip = serializeJson(parsed.value, 2);
    const auto reparsed = parseJson(roundTrip);
    BOO_CHECK(reparsed.ok);
    BOO_CHECK(reparsed.value["samples"].asInt64() == 17280000);

    const auto bad = parseJson("{\n  \"broken\": [1, 2,\n}");
    BOO_CHECK(!bad.ok);
    BOO_CHECK(bad.line == 3);
    return true;
}

bool testProductionCueSheetsAndManifest() {
    using namespace boo;
    AssetManifest manifest{};
    ValidationReport manRep{};
    const auto manPath = resolveResourcePath("assets/manifest.json");
    BOO_CHECK(loadAssetManifestFromFile(manPath, manifest, manRep));
    BOO_CHECK(manRep.valid);

    const auto repoRoot = manPath.parent_path().parent_path();

    CueSheet fullSheet{};
    ValidationReport cueRep{};
    BOO_CHECK(loadCueSheetFromFile(resolveResourcePath("assets/cues/film_360s.json"), fullSheet, cueRep));
    validateCueSheetAgainstManifest(fullSheet, manifest, repoRoot, cueRep);
    BOO_CHECK(cueRep.valid);
    BOO_CHECK(fullSheet.scenes.size() == 7);
    BOO_CHECK(fullSheet.durationSamples == 17280000);
    BOO_CHECK(fullSheet.sampleRate == 48000);
    BOO_CHECK(!cueUsesM1Visuals(fullSheet));

    // Verify Milestone M1 60-second vertical slice cue sheet & manifest presets
    CueSheet m1Sheet{};
    ValidationReport m1Rep{};
    BOO_CHECK(loadCueSheetFromFile(resolveResourcePath("assets/cues/m1_taipei_60s.json"), m1Sheet, m1Rep));
    validateCueSheetAgainstManifest(m1Sheet, manifest, repoRoot, m1Rep);
    BOO_CHECK(m1Rep.valid);
    BOO_CHECK(m1Sheet.scenes.size() == 3);
    BOO_CHECK(m1Sheet.durationSamples == 2880000);
    BOO_CHECK(m1Sheet.sampleRate == 48000);
    BOO_CHECK(cueUsesM1Visuals(m1Sheet));
    BOO_CHECK(findSceneIndexAtSample(m1Sheet, 0) == 0);
    BOO_CHECK(findSceneIndexAtSample(m1Sheet, 959999) == 0);
    BOO_CHECK(findSceneIndexAtSample(m1Sheet, 960000) == 1);
    BOO_CHECK(findSceneIndexAtSample(m1Sheet, 1919999) == 1);
    BOO_CHECK(findSceneIndexAtSample(m1Sheet, 1920000) == 2);
    BOO_CHECK(findSceneIndexAtSample(m1Sheet, 2879999) == 2);
    BOO_CHECK(m1Sheet.scenes[2].revealId == "opening_scale_reveal");
    BOO_CHECK(m1Sheet.scenes[2].revealSample == 2160000);

    // Mixed M0/M1 scene sheet must NOT report cueUsesM1Visuals == true
    CueSheet mixedSheet = m1Sheet;
    mixedSheet.scenes[1].visualPreset = "enamel_monolith";
    BOO_CHECK(!cueUsesM1Visuals(mixedSheet));

    // Verify exact boundary lookup (left-inclusive, right-exclusive)
    BOO_CHECK(findSceneIndexAtSample(fullSheet, 0) == 0);
    BOO_CHECK(findSceneIndexAtSample(fullSheet, 2159999) == 0);
    BOO_CHECK(findSceneIndexAtSample(fullSheet, 2160000) == 1);
    BOO_CHECK(findSceneIndexAtSample(fullSheet, 17279999) == 6);

    SoloLoopRange rtRange{};
    BOO_CHECK(resolvePlaybackLoopRange(fullSheet, PlaybackMode::SoloRT, rtRange));
    BOO_CHECK(rtRange.startSample == 2160000 && rtRange.endSample == 4800000);

    const char* solos[] = {
        "assets/cues/solo_rt.json",
        "assets/cues/solo_tensor.json",
        "assets/cues/solo_npu.json",
        "assets/cues/solo_cuda.json"
    };
    for (const char* sp : solos) {
        CueSheet s{};
        ValidationReport sr{};
        BOO_CHECK(loadCueSheetFromFile(resolveResourcePath(sp), s, sr));
        validateCueSheetAgainstManifest(s, manifest, repoRoot, sr);
        BOO_CHECK(sr.valid);
    }
    return true;
}

bool testInvalidCueFixturesRejected() {
    using namespace boo;
    {
        CueSheet s{};
        ValidationReport r{};
        BOO_CHECK(!loadCueSheetFromFile(resolveResourcePath("tests/fixtures/invalid_gap.json"), s, r));
        BOO_CHECK(hasErrorCode(r, "TIMELINE_GAP"));
    }
    {
        CueSheet s{};
        ValidationReport r{};
        BOO_CHECK(!loadCueSheetFromFile(resolveResourcePath("tests/fixtures/invalid_overlap.json"), s, r));
        BOO_CHECK(hasErrorCode(r, "TIMELINE_OVERLAP"));
    }
    {
        CueSheet s{};
        ValidationReport r{};
        BOO_CHECK(!loadCueSheetFromFile(resolveResourcePath("tests/fixtures/invalid_duration.json"), s, r));
        BOO_CHECK(hasErrorCode(r, "FULL_FILM_DURATION_MISMATCH"));
    }
    return true;
}

bool testManifestProvenanceAndByteIntegrity() {
    using namespace boo;
    const auto source = std::filesystem::absolute(resolveResourcePath("assets/manifest.json"));
    const auto root = source.parent_path().parent_path();
    ValidationReport report;
    AssetManifest manifest;
    BOO_CHECK(loadAssetManifestFromFile(source, manifest, report));
    BOO_CHECK(manifest.schema == 2);
    const auto cue = std::find_if(manifest.assets.begin(), manifest.assets.end(),
        [](const AssetManifestEntry& entry) { return entry.id == "cue_film_360s"; });
    BOO_CHECK(cue != manifest.assets.end());
    BOO_CHECK(cue->license == "CC-BY-NC-4.0" && cue->attributionRequired);
    AssetManifest single;
    single.assets.push_back(*cue);
    verifyManifestAssetFiles(single, root, report);
    BOO_CHECK(report.valid);
    single.assets.front().sha256.assign(64, '0');
    verifyManifestAssetFiles(single, root, report);
    BOO_CHECK(hasErrorCode(report, "MANIFEST_SHA256_MISMATCH"));

    auto parsed = parseJsonFile(source);
    BOO_CHECK(parsed.ok);
    parsed.value["assets"].asArrayMut().front()["author"] = "";
    auto invalid = std::filesystem::temp_directory_path() /
        ("boo-invalid-provenance-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    std::ofstream(invalid, std::ios::binary) << serializeJson(parsed.value);
    const bool accepted = loadAssetManifestFromFile(invalid, manifest, report);
    std::filesystem::remove(invalid);
    BOO_CHECK(!accepted && hasErrorCode(report, "MANIFEST_PROVENANCE_ERROR"));
    return true;
}

bool testTimelineClockArithmeticAndFailover() {
    using namespace boo;
    BOO_CHECK(std::abs(TimelineClock::samplesToSeconds(17280000, 48000) - 360.0) < 1e-9);
    BOO_CHECK(TimelineClock::secondsToSamples(360.0, 48000) == 17280000);
    BOO_CHECK(TimelineClock::formatTimecode(0, 48000) == "00:00.000");
    BOO_CHECK(TimelineClock::formatTimecode(1728000, 48000) == "00:36.000");
    BOO_CHECK(TimelineClock::formatTimecode(17280000, 48000) == "06:00.000");

    int64_t fakeAudioSamples = 0;
    bool audioAlive = true;
    int64_t fakeMonoTicks = 1000000; // 1.0s at 1 MHz

    TimelineClock clk;
    clk.configure(48000, 17280000);
    clk.setCustomMonotonicSource([&]() { return fakeMonoTicks; }, 1000000);
    clk.setAudioSampleReader([&](int64_t& out) {
        if (!audioAlive) return false;
        out = fakeAudioSamples;
        return true;
    });

    clk.start(true, false);
    BOO_CHECK(clk.source() == ClockSource::AudioSampleClock);

    // Advance 1.5 seconds via audio sample counter
    fakeAudioSamples += 72000;
    fakeMonoTicks += 1500000;
    auto snap1 = clk.tickFrame();
    BOO_CHECK(snap1.samplePosition == 72000);
    BOO_CHECK(std::abs(snap1.filmTimeSeconds - 1.5) < 1e-6);
    // Stall clamp check: 1.5s wall jump must clamp dtSeconds to 0.100s
    BOO_CHECK(std::abs(snap1.dtSeconds - TimelineClock::kMaxClampedDtSec) < 1e-6);

    // Simulate sudden audio endpoint drop mid-show -> must preserve exact 72,000 sample position!
    audioAlive = false;
    auto snapDrop = clk.tickFrame();
    BOO_CHECK(snapDrop.source == ClockSource::SilentMonotonicClock);
    BOO_CHECK(snapDrop.samplePosition == 72000);

    // Advance 0.5 seconds on monotonic QPC fallback clock -> 72,000 + 24,000 = 96,000 samples
    fakeMonoTicks += 500000;
    auto snapMono = clk.tickFrame();
    BOO_CHECK(snapMono.samplePosition == 96000);

    // Test pause & resume without drift
    clk.pause();
    fakeMonoTicks += 2000000;
    auto snapPaused = clk.tickFrame();
    BOO_CHECK(snapPaused.samplePosition == 96000);
    clk.resume();
    fakeMonoTicks += 250000;
    auto snapResumed = clk.tickFrame();
    BOO_CHECK(snapResumed.samplePosition == 108000);

    // Test exact solo loop boundary wraparound [2160000, 2208000)
    clk.setLoopRange(2160000, 2208000);
    clk.setLoopEnabled(true);
    BOO_CHECK(clk.currentSample() == 2160000);
    fakeMonoTicks += 1250000; // +60,000 samples across a 48,000-sample loop -> wraps to 2160000 + 12000!
    auto snapLoop = clk.tickFrame();
    BOO_CHECK(snapLoop.loopedThisTick);
    BOO_CHECK(snapLoop.samplePosition == 2172000);
    BOO_CHECK(snapLoop.loopCount == 1);
    return true;
}

bool testPrecisionTransport() {
    using namespace boo;
    int64_t ticks = 0;
    TimelineClock clock;
    clock.configure(48000, 2880000);
    clock.setCustomMonotonicSource([&] { return ticks; }, 48000);
    clock.start(false, false);
    clock.stepFrame(1);
    BOO_CHECK(clock.currentSample() == 0);
    clock.pause();
    clock.stepFrame(-1);
    BOO_CHECK(clock.currentSample() == 0);
    clock.stepFrame(1);
    BOO_CHECK(clock.currentSample() == 800);
    clock.stepFrame(-1);
    BOO_CHECK(clock.currentSample() == 0);
    clock.seekRelativeSeconds(2);
    BOO_CHECK(clock.currentSample() == 96000);
    clock.seekRelativeSeconds(10);
    BOO_CHECK(clock.currentSample() == 576000);
    clock.seekRelativeSeconds(-10);
    clock.seekRelativeSeconds(-2);
    BOO_CHECK(clock.currentSample() == 0);
    clock.seekToSeconds(43);
    BOO_CHECK(clock.currentSample() == 2064000);
    clock.seekToSample(2880000);
    clock.stepFrame(1);
    BOO_CHECK(clock.currentSample() == 2880000);
    ticks += 48000;
    BOO_CHECK(clock.tickFrame().samplePosition == 2880000);
    BOO_CHECK(clock.state() == TransportState::Paused);
    return true;
}

bool testQualityLadderHysteresisAndVramPressure() {
    using namespace boo;
    QualityLadder ladder(QualityPresetId::Showcase, 10);
    const uint64_t cap = 12ULL << 30;
    const uint64_t normalVram = 2ULL << 30;

    // 1 single window > 18 ms should NOT step down yet (requires 2 consecutive windows)
    for (int i = 0; i < 10; ++i) ladder.pushFrameSample(22.0, normalVram, cap, false);
    BOO_CHECK(ladder.currentPreset() == QualityPresetId::Showcase);

    // 2nd consecutive window > 18 ms -> steps down to Balanced
    for (int i = 0; i < 10; ++i) ladder.pushFrameSample(22.0, normalVram, cap, false);
    BOO_CHECK(ladder.currentPreset() == QualityPresetId::Balanced);

    // Immediate VRAM pressure (> cap) sheds immediately to Safe even with fast GPU frame time!
    ladder.pushFrameSample(4.0, 13ULL << 30, cap, false);
    BOO_CHECK(ladder.currentPreset() == QualityPresetId::Safe);
    BOO_CHECK(ladder.memoryPressureActive());

    // Sustained headroom (3 windows < 11.5 ms) upgrades ONLY when atSafeSceneCut == true
    for (int w = 0; w < 3; ++w) {
        for (int i = 0; i < 9; ++i) ladder.pushFrameSample(5.0, normalVram, cap, false);
        ladder.pushFrameSample(5.0, normalVram, cap, w == 2); // Safe cut at end of 3rd window
    }
    BOO_CHECK(ladder.currentPreset() == QualityPresetId::Balanced);
    BOO_CHECK(ladder.upgradeCount() == 1);
    return true;
}

bool testLiteStartupAndLadder() {
    using namespace boo;
    BOO_CHECK(isIntelGen9Gpu(0x8086u, 0x5917u));
    BOO_CHECK(isIntelGen9Gpu(0x8086u, 0x1902u));
    constexpr uint32_t addedGen9Ids[] = {
        0x190A, 0x190E, 0x1913, 0x1915, 0x1917, 0x191A, 0x192A,
        0x5908, 0x590A, 0x590E, 0x5913, 0x5915, 0x591A, 0x593B,
        0x3E9C, 0x3EA9, 0x9BA2, 0x9BC2
    };
    for (const auto id : addedGen9Ids) BOO_CHECK(isIntelGen9Gpu(0x8086u, id));
    BOO_CHECK(isIntelGen9Gpu(0x8086u, 0x9B41u));
    BOO_CHECK(!isIntelGen9Gpu(0x8086u, 0x9A49u)); // Iris Xe
    BOO_CHECK(!isIntelGen9Gpu(0x10DEu, 0x5917u));
    BOO_CHECK(!isIntelGen9Gpu(0x8086u, 0x59FFu)); // not an enumerated device
    BOO_CHECK(selectStartupPreset(QualityPresetId::Balanced, false, true, false, 0x8086u, 0x5917u) == QualityPresetId::Lite);
    BOO_CHECK(selectStartupPreset(QualityPresetId::Balanced, false, true, false, 0x8086u, 0x9A49u) == QualityPresetId::Safe);
    BOO_CHECK(selectStartupPreset(QualityPresetId::Lite, false, true, false, 0x8086u, 0x9A49u) == QualityPresetId::Lite);
    BOO_CHECK(selectStartupPreset(QualityPresetId::Showcase, true, true, false, 0x8086u, 0x5917u) == QualityPresetId::Showcase);
    BOO_CHECK(selectStartupPreset(QualityPresetId::Balanced, false, false, true, 0u, 0u) == QualityPresetId::Lite);
    QualityPresetId parsed{};
    BOO_CHECK(parseQualityPreset("lite", parsed) && parsed == QualityPresetId::Lite);
    QualityLadder ladder(QualityPresetId::Safe, 60);
    ladder.setIntegratedGraphics(true);
    for (int i = 0; i < 120; ++i) ladder.pushFrameSample(45.0f, 0, 0, true);
    BOO_CHECK(ladder.currentPreset() == QualityPresetId::Lite);
    for (int i = 0; i < 299; ++i) ladder.pushFrameSample(10.0f, 0, 0, true, 0.02f);
    BOO_CHECK(ladder.currentPreset() == QualityPresetId::Lite);
    ladder.pushFrameSample(10.0f, 0, 0, true, 0.02f);
    BOO_CHECK(ladder.currentPreset() == QualityPresetId::Safe);
    QualityLadder offWindowCut(QualityPresetId::Lite, 60);
    offWindowCut.setIntegratedGraphics(true);
    for (int i = 0; i < 300; ++i) offWindowCut.pushFrameSample(10.0f, 0, 0, false, 0.02f);
    for (int i = 0; i < 3; ++i) offWindowCut.pushFrameSample(30.0f, 0, 0, false);
    offWindowCut.pushFrameSample(30.0f, 0, 0, true);
    BOO_CHECK(offWindowCut.currentPreset() == QualityPresetId::Lite); // cut is no longer fast
    for (int i = 0; i < 360; ++i) offWindowCut.pushFrameSample(10.0f, 0, 0, false, 0.02f);
    offWindowCut.pushFrameSample(10.0f, 0, 0, true, 0.02f); // not aligned to an evaluation
    BOO_CHECK(offWindowCut.currentPreset() == QualityPresetId::Safe);
    QualityLadder veryFast(QualityPresetId::Lite, 60);
    veryFast.setIntegratedGraphics(true);
    for (int i = 0; i < 300; ++i) veryFast.pushFrameSample(1.0f, 0, 0, true, 0.001f);
    BOO_CHECK(veryFast.currentPreset() == QualityPresetId::Lite); // 300 frames are not five seconds
    for (int i = 0; i < 240; ++i) veryFast.pushFrameSample(1.0f, 0, 0, true, 0.02f);
    BOO_CHECK(veryFast.currentPreset() == QualityPresetId::Safe);
    // The final safety rung needs two sustained windows above 55 ms, and
    // changes the actual requested canvas without adding a selectable preset.
    QualityLadder emergency(QualityPresetId::Lite, 10);
    for (int i = 0; i < 20; ++i) emergency.pushFrameSample(54.0, 0, 0, false);
    BOO_CHECK(!emergency.safetyResolutionActive());
    for (int i = 0; i < 20; ++i) emergency.pushFrameSample(56.0, 0, 0, false);
    BOO_CHECK(emergency.safetyResolutionActive());
    QualityPresetSpec liteSpec{};
    liteSpec.id = QualityPresetId::Lite;
    liteSpec.renderWidth = 960;
    liteSpec.renderHeight = 540;
    const auto emergencySpec = emergency.effectiveSpec(liteSpec);
    BOO_CHECK(emergencySpec.renderWidth == 640 && emergencySpec.renderHeight == 360);
    for (int i = 0; i < 100; ++i) emergency.pushFrameSample(1.0, 0, 0, true);
    BOO_CHECK(emergency.safetyResolutionActive());
    QualityLadder pressuredLite(QualityPresetId::Lite, 10);
    for (int i = 0; i < 20; ++i) pressuredLite.pushFrameSample(101.0, 2, 1, true);
    BOO_CHECK(pressuredLite.memoryPressureActive());
    BOO_CHECK(pressuredLite.safetyResolutionActive());
    QualityLadder pressuredFast(QualityPresetId::Lite, 10);
    for (int i = 0; i < 100; ++i) pressuredFast.pushFrameSample(1.0, 2, 1, true);
    BOO_CHECK(pressuredFast.memoryPressureActive());
    BOO_CHECK(pressuredFast.currentPreset() == QualityPresetId::Lite);
    BOO_CHECK(!pressuredFast.safetyResolutionActive());
    emergency.setPreset(QualityPresetId::Lite);
    BOO_CHECK(!emergency.safetyResolutionActive());
    BOO_CHECK(emergency.effectiveSpec(liteSpec).renderWidth == 960);

    QualityLadder capped(QualityPresetId::Safe, 10);
    capped.setIntegratedGraphics(true);
    for (int i = 0; i < 400; ++i) capped.pushFrameSample(1.0, 0, 0, true, 0.02f);
    BOO_CHECK(capped.currentPreset() == QualityPresetId::Safe);
    QualityLadder failedUpgrade(QualityPresetId::Lite, 10);
    for (int i = 0; i < 30; ++i) failedUpgrade.pushFrameSample(1.0, 0, 0, true);
    BOO_CHECK(failedUpgrade.currentPreset() == QualityPresetId::Safe);
    for (int i = 0; i < 20; ++i) failedUpgrade.pushFrameSample(50.0, 0, 0, true);
    BOO_CHECK(failedUpgrade.currentPreset() == QualityPresetId::Lite);
    for (int i = 0; i < 100; ++i) failedUpgrade.pushFrameSample(1.0, 0, 0, true);
    BOO_CHECK(failedUpgrade.currentPreset() == QualityPresetId::Lite);
    failedUpgrade.setPreset(QualityPresetId::Lite);
    for (int i = 0; i < 30; ++i) failedUpgrade.pushFrameSample(1.0, 0, 0, true);
    BOO_CHECK(failedUpgrade.currentPreset() == QualityPresetId::Safe);

    QualityPresetSpec unsafe{};
    unsafe.id = QualityPresetId::Lite;
    const auto safeSpec = findQualityPresetSpec({unsafe}, QualityPresetId::Lite);
    BOO_CHECK(!safeSpec.allowDxr && !safeSpec.allowCuda && !safeSpec.allowTensor && !safeSpec.allowNpu);
    const auto missingSafe = findQualityPresetSpec({unsafe}, QualityPresetId::Safe);
    BOO_CHECK(missingSafe.id == QualityPresetId::Safe && missingSafe.renderWidth == 1280u && missingSafe.renderHeight == 720u);
    BOO_CHECK(!missingSafe.allowDxr && !missingSafe.allowCuda && !missingSafe.allowTensor && !missingSafe.allowNpu);
    return true;
}

class MockEffect final : public boo::IEffect {
public:
    MockEffect(const char* nm, bool failInit, int* renderCounter)
        : name_(nm), failInit_(failInit), counter_(renderCounter) {}
    const char* name() const noexcept override { return name_; }
    boo::HardwareResult probeAndInitialize() override {
        boo::HardwareResult r{};
        r.engine = boo::EngineKind::RT;
        r.status = failInit_ ? boo::BackendStatus::Faulted : boo::BackendStatus::Substituted;
        r.humanReadableReason = failInit_ ? "Mock failure" : "Mock ready";
        return r;
    }
    void preload(double) override {}
    void resetTo(double) override {}
    void render(const boo::FrameContext&) override { ++(*counter_); }
    void shutdown() noexcept override {}

private:
    const char* name_;
    bool failInit_;
    int* counter_;
};

bool testFallbackOrchestratorFaultIsolation() {
    using namespace boo;
    int primRenders = 0;
    int fbRenders = 0;

    FallbackOrchestrator orch;
    HardwareResult npuState{};
    npuState.engine = EngineKind::NPU;
    npuState.status = BackendStatus::Substituted;
    npuState.humanReadableReason = "Scripted fallback";
    orch.registerEngineState(npuState);

    orch.setPrimaryAndFallbackEffects(
        std::make_unique<MockEffect>("PrimaryMock", false, &primRenders),
        std::make_unique<MockEffect>("FallbackMock", false, &fbRenders));
    orch.initializeEffects(false);
    BOO_CHECK(!orch.isUsingFallbackEffect());

    FrameContext ctx{};
    orch.renderFrame(ctx, false);
    BOO_CHECK(primRenders == 1 && fbRenders == 0);

    // Inject runtime fault on primary -> must catch, switch to fallback, and render fallback this frame!
    orch.renderFrame(ctx, true);
    BOO_CHECK(orch.isUsingFallbackEffect());
    BOO_CHECK(orch.faultCount() == 1);
    BOO_CHECK(fbRenders == 1);

    // Verify scene boundary toast does not duplicate across repeated scene boundaries
    orch.onSceneBoundary(1, 45.0, false);
    const size_t toastCountAfterFirst = orch.toasts().size();
    orch.onSceneBoundary(2, 100.0, false);
    BOO_CHECK(orch.toasts().size() == toastCountAfterFirst);

    orch.shutdown();
    return true;
}

bool testScriptedDirectorDeterminismAndBounds() {
    using namespace boo;
    ScriptedDirector d1, d2;
    d1.reset(266203);
    d2.reset(266203);

    SceneCue cue{};
    cue.sceneId = "madame_n";
    cue.allowDirectorModulation = true;

    for (int step = 0; step < 30; ++step) {
        FrameContext ctx{};
        ctx.samplePosition = step * 8000;
        ctx.filmTimeSeconds = static_cast<double>(ctx.samplePosition) / 48000.0;
        ctx.dtSeconds = 8000.0 / 48000.0;
        ctx.actIndex = 3;
        ctx.actProgress = static_cast<float>(step) / 30.0f;
        ctx.audioEnvelope = 0.5f;

        const auto m1 = d1.evaluate(ctx, &cue);
        const auto m2 = d2.evaluate(ctx, &cue);
        BOO_CHECK(m1.category == m2.category);
        BOO_CHECK(std::abs(m1.bloomStrength - m2.bloomStrength) < 1e-6f);
        BOO_CHECK(m1.bloomStrength >= 0.0f && m1.bloomStrength <= 1.0f);
        BOO_CHECK(m1.irisAngle >= 0.0f && m1.irisAngle <= 1.0f);
        BOO_CHECK(m1.lightTension >= 0.0f && m1.lightTension <= 1.0f);
        BOO_CHECK(m1.spectralShift >= 0.0f && m1.spectralShift <= 1.0f);
        BOO_CHECK(m1.cutAccent >= 0.0f && m1.cutAccent <= 1.0f);
        BOO_CHECK(m1.isScriptedFallback);
    }
    return true;
}

bool testSettingsAndPathRedaction() {
    using namespace boo;
    BOO_CHECK(redactUserPath("C:\\Users\\alice\\Videos\\clip.mp4") == "<USER_HOME>/Videos/clip.mp4");
    BOO_CHECK(redactUserPath("D:/boo-sah-sah/assets/cues/film_360s.json") == "D:/boo-sah-sah/assets/cues/film_360s.json");

    AppSettings s{};
    BOO_CHECK(loadSettingsFromToml(resolveResourcePath("config/default.toml"), s));
    BOO_CHECK(s.width == 1920 && s.height == 1080);

    std::vector<std::string> cli = {
        "--preset", "safe",
        "--edition", "midnight",
        "--disable-rt",
        "--disable-npu",
        "--reduced-flash",
        "--legacy-m0-visual",
        "--duration-seconds", "5.5"
    };
    BOO_CHECK(parseCommandLineArgs(cli, s));
    BOO_CHECK(s.qualityPreset == QualityPresetId::Safe);
    BOO_CHECK(s.edition == Edition::Midnight);
    BOO_CHECK(s.disableRT && s.disableNPU && !s.disableCUDA);
    BOO_CHECK(s.reducedFlash && s.legacyM0Visual);
    BOO_CHECK(std::abs(s.autoExitSeconds - 5.5) < 1e-6);

    // Neural rendering: off by default so the film is untouched; each network and the solo view parse.
    AppSettings n{};
    BOO_CHECK(loadSettingsFromToml(resolveResourcePath("config/default.toml"), n));
    BOO_CHECK(!n.neural.enabled && n.neural.solo == NeuralSolo::None);
    BOO_CHECK(!n.neural.runs(NeuralFeature::Shading) && !n.neural.runs(NeuralFeature::RadianceCache));
    std::vector<std::string> neuralCli = {"--neural", "on", "--neural-textures", "off", "--neural-solo", "cache"};
    BOO_CHECK(parseCommandLineArgs(neuralCli, n));
    BOO_CHECK(n.neural.enabled && n.neural.shading && !n.neural.textures && n.neural.radianceCache);
    BOO_CHECK(n.neural.solo == NeuralSolo::RadianceCache);
    BOO_CHECK(!n.neural.runs(NeuralFeature::Textures) && n.neural.runs(NeuralFeature::RadianceCache));
    // Solo runs its network even with the suite switched off.
    NeuralRenderingControls soloOnly{};
    soloOnly.solo = NeuralSolo::Textures;
    BOO_CHECK(soloOnly.runs(NeuralFeature::Textures) && !soloOnly.runs(NeuralFeature::Shading));
    std::vector<std::string> badNeural = {"--neural-solo", "everything"};
    BOO_CHECK(!parseCommandLineArgs(badNeural, n));
    BOO_CHECK(n.neural.tensorCores);
    std::vector<std::string> coresCli = {"--neural-tensor-cores", "off", "--dashboard", "mini"};
    BOO_CHECK(parseCommandLineArgs(coresCli, n));
    BOO_CHECK(!n.neural.tensorCores && n.dashboardPanel == 1u);
    std::vector<std::string> badPanel = {"--dashboard", "tiny"};
    BOO_CHECK(!parseCommandLineArgs(badPanel, n));
    // GPU workout: off by default; level, formation and bird camera parse, and levels clamp to 0..10.
    BOO_CHECK(n.gpuWorkout == 0u && n.flockFormation == 0u && !n.birdCam);
    std::vector<std::string> workoutCli = {"--gpu-workout", "7", "--flock", "kettle", "--bird-cam"};
    BOO_CHECK(parseCommandLineArgs(workoutCli, n));
    BOO_CHECK(n.gpuWorkout == 7u && n.flockFormation == 5u && n.birdCam);
    std::vector<std::string> overCli = {"--gpu-workout", "99"};
    BOO_CHECK(parseCommandLineArgs(overCli, n) && n.gpuWorkout == 10u);
    // The workout's frame-rate floor: film speed unless told otherwise, 0 switches it off.
    BOO_CHECK(n.workoutFloorFps == 24.0f);
    std::vector<std::string> floorCli = {"--workout-floor-fps", "60"};
    BOO_CHECK(parseCommandLineArgs(floorCli, n) && n.workoutFloorFps == 60.0f);
    std::vector<std::string> noFloorCli = {"--workout-floor-fps", "0"};
    BOO_CHECK(parseCommandLineArgs(noFloorCli, n) && n.workoutFloorFps == 0.0f);
    std::vector<std::string> lowFloorCli = {"--workout-floor-fps", "3"};
    BOO_CHECK(parseCommandLineArgs(lowFloorCli, n) && n.workoutFloorFps == 10.0f);
    std::vector<std::string> badFloorCli = {"--workout-floor-fps", "fast"};
    BOO_CHECK(!parseCommandLineArgs(badFloorCli, n));
    std::vector<std::string> badFlock = {"--flock", "murmuration"};
    BOO_CHECK(!parseCommandLineArgs(badFlock, n));

    // Test rig: every GPU feature on, including all three neural networks.
    AppSettings rig{};
    rig.disableCUDA = true;
    std::vector<std::string> rigCli = {"--test-rig"};
    BOO_CHECK(parseCommandLineArgs(rigCli, rig));
    BOO_CHECK(!rig.disableRT && !rig.disableCUDA && !rig.disableTensor);
    BOO_CHECK(rig.pathTracing == PathTracingLevel::High && rig.dlssMode == DlssMode::Quality && rig.rayReconstruction);
    BOO_CHECK(rig.frameGeneration == FrameGenerationMode::X6 && rig.reflex == ReflexMode::Boost);
    BOO_CHECK(rig.neural.enabled && rig.neural.runs(NeuralFeature::Shading) &&
              rig.neural.runs(NeuralFeature::Textures) && rig.neural.runs(NeuralFeature::RadianceCache));
    return true;
}

bool testM1CameraLetterboxAndFlashSafety() {
    using namespace boo;

    // 1. Sample-deterministic camera evaluation across all 3 shots + 45.0s reveal
    const auto c0a = evaluateM1Camera(0, 48000, 0.5f, false);
    const auto c0b = evaluateM1Camera(0, 48000, 0.5f, false);
    BOO_CHECK(c0a.shotIndex == 0);
    BOO_CHECK(std::abs(c0a.eye[0] - c0b.eye[0]) < 1e-7f);
    BOO_CHECK(std::abs(c0a.eye[1] - c0b.eye[1]) < 1e-7f);
    BOO_CHECK(std::abs(c0a.eye[2] - c0b.eye[2]) < 1e-7f);

    const auto c1 = evaluateM1Camera(960000, 48000, 0.5f, false);
    BOO_CHECK(c1.shotIndex == 1);

    const auto cReveal = evaluateM1Camera(2160000, 48000, 0.5f, false);
    BOO_CHECK(cReveal.shotIndex == 2);
    BOO_CHECK(cReveal.revealOpen01 > 0.80f && cReveal.revealOpen01 <= 1.0f);

    const auto cEnd = evaluateM1Camera(2879999, 48000, 0.5f, false);
    BOO_CHECK(cEnd.shotIndex == 2);

    // 2. Bounded irisOffset modulation in [-0.15, +0.15] even under extreme mood inputs
    const auto cLow = evaluateM1Camera(2160000, 48000, -50.0f, false);
    const auto cHigh = evaluateM1Camera(2160000, 48000, 50.0f, false);
    BOO_CHECK(std::abs(cLow.eye[0] - cReveal.eye[0]) <= 0.1501f);
    BOO_CHECK(std::abs(cHigh.eye[0] - cReveal.eye[0]) <= 0.1501f);

    // 3. Flash-safe musical envelope bounds & --reduced-flash exact zeroing
    for (int64_t s = 0; s <= 2880000; s += 6000) {
        const float envNormal = evaluateM1MusicalEnvelope(s, 48000, false);
        const float envReduced = evaluateM1MusicalEnvelope(s, 48000, true);
        BOO_CHECK(envNormal >= 0.0f && envNormal <= 0.06001f);
        BOO_CHECK(envReduced == 0.0f);
    }

    // 4. Analytical 3-band audio envelope determinism for silent-clock & headless parity
    const auto b1 = computeAnalyticalAudioBands(2160000, 48000);
    const auto b2 = computeAnalyticalAudioBands(2160000, 48000);
    BOO_CHECK(std::abs(b1.low - b2.low) < 1e-7f);
    BOO_CHECK(std::abs(b1.mid - b2.mid) < 1e-7f);
    BOO_CHECK(std::abs(b1.high - b2.high) < 1e-7f);
    BOO_CHECK(b1.low >= 0.0f && b1.low <= 1.0f);
    BOO_CHECK(b1.mid >= 0.0f && b1.mid <= 1.0f);
    BOO_CHECK(b1.high >= 0.0f && b1.high <= 1.0f);

    // 5. 16:9 aspect-preserving letterbox / pillarbox geometry
    const auto vp1080 = computeLetterboxViewport16x9(1920, 1080);
    BOO_CHECK(std::abs(vp1080.x - 0.0f) < 1e-4f && std::abs(vp1080.y - 0.0f) < 1e-4f);
    BOO_CHECK(std::abs(vp1080.width - 1920.0f) < 1e-4f && std::abs(vp1080.height - 1080.0f) < 1e-4f);

    const auto vp16x10 = computeLetterboxViewport16x9(1280, 800);
    BOO_CHECK(std::abs(vp16x10.x - 0.0f) < 1e-4f && std::abs(vp16x10.y - 40.0f) < 1e-4f);
    BOO_CHECK(std::abs(vp16x10.width - 1280.0f) < 1e-4f && std::abs(vp16x10.height - 720.0f) < 1e-4f);

    const auto vp4x3 = computeLetterboxViewport16x9(1024, 768);
    BOO_CHECK(std::abs(vp4x3.x - 0.0f) < 1e-4f && std::abs(vp4x3.y - 96.0f) < 1e-4f);
    BOO_CHECK(std::abs(vp4x3.width - 1024.0f) < 1e-4f && std::abs(vp4x3.height - 576.0f) < 1e-4f);

    const auto vp21x9 = computeLetterboxViewport16x9(2560, 1080);
    BOO_CHECK(std::abs(vp21x9.x - 320.0f) < 1e-4f && std::abs(vp21x9.y - 0.0f) < 1e-4f);
    BOO_CHECK(std::abs(vp21x9.width - 1920.0f) < 1e-4f && std::abs(vp21x9.height - 1080.0f) < 1e-4f);

    return true;
}

bool testAudioClicklessCrossfadeAndGainRamps() {
    using namespace boo;

    AudioSynthesizer48k synth;
    synth.reset(2304000u, 0.0f); // Start at 48.0s Glass Orchid climax with 0.0 initial gain

    std::vector<float> pcm(480u * 2u, 0.0f);
    float prevL = 0.0f;
    float prevR = 0.0f;
    float maxStep = 0.0f;

    auto renderAndMeasure = [&](uint32_t frames, bool paused, bool muted, float vol) {
        pcm.assign(static_cast<size_t>(frames) * 2u, 0.0f);
        AudioBandSnapshot bands{};
        const uint32_t adv = synth.renderFrames(pcm.data(), frames, paused, muted, vol, &bands);
        for (uint32_t i = 0; i < frames; ++i) {
            const float curL = pcm[i * 2u + 0u];
            const float curR = pcm[i * 2u + 1u];
            maxStep = std::max(maxStep, std::abs(curL - prevL));
            maxStep = std::max(maxStep, std::abs(curR - prevR));
            prevL = curL;
            prevR = curR;
        }
        return adv;
    };

    // 1. The master ramps from silence to unity while the procedural mix retains its headroom.
    BOO_CHECK(renderAndMeasure(1024u, false, false, 1.0f) == 1024u);
    BOO_CHECK(std::abs(synth.currentGain() - 1.0f) < 1e-4f);
    BOO_CHECK(maxStep < 0.010f);

    // 2. Abrupt long-distance seek during full-scale playback (48.0s -> 5.0s -> 30.0s -> 56.0s)
    synth.seekTo(240000u);
    BOO_CHECK(renderAndMeasure(256u, false, false, 1.0f) == 256u);
    // Rapid overlapping seeks while previous crossfades are still mid-flight!
    synth.seekTo(1440000u);
    BOO_CHECK(renderAndMeasure(128u, false, false, 1.0f) == 128u);
    synth.seekTo(2688000u);
    BOO_CHECK(renderAndMeasure(96u, false, false, 1.0f) == 96u);
    synth.seekTo(0u); // Loop wrap simulation (end-of-film -> sample 0)
    BOO_CHECK(renderAndMeasure(1024u, false, false, 1.0f) == 1024u);
    BOO_CHECK(maxStep < 0.010f);

    // 3. Pause C1 ramp-down: must report 0 timeline frames advanced while smoothly ramping waveform to exact 0.0f
    BOO_CHECK(renderAndMeasure(AudioSynthesizer48k::kGainRampSamples + 64u, true, false, 1.0f) == 0u);
    const uint64_t posAfterRamp = synth.primarySamplePosition();
    BOO_CHECK(renderAndMeasure(256u, true, false, 1.0f) == 0u);
    BOO_CHECK(synth.primarySamplePosition() == posAfterRamp);
    BOO_CHECK(synth.currentGain() == 0.0f);
    BOO_CHECK(std::abs(prevL) < 1e-6f && std::abs(prevR) < 1e-6f);
    BOO_CHECK(maxStep < 0.010f);

    // 4. Seek while paused + resume C1 ramp-up: zero discontinuous jump
    synth.seekTo(2160000u);
    BOO_CHECK(renderAndMeasure(128u, true, false, 1.0f) == 0u);
    BOO_CHECK(std::abs(prevL) < 1e-6f && std::abs(prevR) < 1e-6f);
    BOO_CHECK(renderAndMeasure(1024u, false, false, 1.0f) == 1024u);
    BOO_CHECK(std::abs(synth.currentGain() - 1.0f) < 1e-4f);
    BOO_CHECK(maxStep < 0.010f);

    return true;
}

bool testTimelineClockSwitchAndTaipei101FinaleFraming() {
    using namespace boo;

    // 1. Seamless SilentMonotonicClock <-> AudioSampleClock migration & Ended state recovery
    int64_t monoTicks = 100000;
    int64_t hwPresentedSamples = 0;
    bool hwHealthy = false;

    TimelineClock clk;
    clk.configure(48000, 2880000);
    clk.setCustomMonotonicSource([&]() { return monoTicks; }, 48000);
    clk.setAudioSampleReader([&](int64_t& out) {
        if (!hwHealthy) return false;
        out = hwPresentedSamples;
        return true;
    });

    clk.start(false, false);
    BOO_CHECK(clk.source() == ClockSource::SilentMonotonicClock);
    monoTicks += 96000; // +2.0 s
    BOO_CHECK(clk.tickFrame().samplePosition == 96000);

    // Hot-migrate onto newly recovered WASAPI endpoint without jumping timeline position!
    hwHealthy = true;
    hwPresentedSamples = 5000;
    BOO_CHECK(clk.switchToAudioClock());
    BOO_CHECK(clk.source() == ClockSource::AudioSampleClock);
    BOO_CHECK(clk.currentSample() == 96000);

    hwPresentedSamples += 24000; // +0.5 s on hardware presentation clock -> 120,000
    monoTicks += 24000;
    const auto migratedSnap = clk.tickFrame();
    BOO_CHECK(migratedSnap.source == ClockSource::AudioSampleClock);
    BOO_CHECK(migratedSnap.samplePosition == 120000);

    // Advance to end of non-looping 60s film -> Ended state, then verify backward step recovers cleanly
    hwPresentedSamples += 3000000;
    monoTicks += 3000000;
    const auto endSnap = clk.tickFrame();
    BOO_CHECK(endSnap.samplePosition == 2880000);
    BOO_CHECK(endSnap.state == TransportState::Ended);

    clk.pause();
    clk.stepFrame(-1);
    BOO_CHECK(clk.currentSample() == 2879200);
    BOO_CHECK(clk.state() == TransportState::Paused);

    // 2. Verify Taipei 101 Grand Finale camera pullback & tilt-up framing (49.0s -> 58.0s)
    const auto cam48 = evaluateM1Camera(2304000, 48000, 0.5f, false); // 48.0s intimate Glass Orchid climax
    const auto cam58 = evaluateM1Camera(2784000, 48000, 0.5f, false); // 58.0s Taipei 101 Finale crown reveal
    BOO_CHECK(cam58.shotIndex == 2);
    BOO_CHECK(cam58.revealOpen01 > 0.99f);
    BOO_CHECK(cam58.eye[2] < cam48.eye[2] - 5.0f);        // Dramatic pullback down boulevard
    BOO_CHECK(cam58.target[1] > cam48.target[1] + 3.5f);  // Tilts upward toward Taipei 101 bamboo modules & TMD crown
    BOO_CHECK(cam58.target[2] > 16.0f);
    BOO_CHECK(cam58.fovYRadians > cam48.fovYRadians + 0.08f);

    return true;
}

bool testFilmOrbits() {
    using namespace boo;
    constexpr double tau = 6.283185307179586;
    struct Orbit { double start, end, centerX, centerZ, minRadius, maxRadius; };
    for (const Orbit orbit : {Orbit{46.0, 80.0, 0.0, 5.5, 2.8, 3.25},
                              Orbit{301.5, 330.5, 18.5, 60.0, 15.0, 30.0}}) {
        for (const float motion : {1.0f, 0.35f, 0.f}) {
            std::vector<M1CameraState> poses;
            double previousAngle = 0.0, signedSweep = 0.0, totalTravel = 0.0;
            double minSweep = 0.0, maxSweep = 0.0;
            for (double time = orbit.start; time <= orbit.end; time += 0.25) {
                const auto cam = evaluateShowCamera(ShowVisualMode::FullFilm360s, int64_t(time * 48000), 48000, 0.85f, false, motion);
                const double dx = cam.eye[0] - orbit.centerX, dz = cam.eye[2] - orbit.centerZ;
                const double radius = std::hypot(dx, dz), angle = std::atan2(dx, -dz);
                BOO_CHECK(radius >= orbit.minRadius && radius <= orbit.maxRadius);
                if (time <= 49.0 || time >= 71.0 || motion == 0.f) {
                    BOO_CHECK(std::hypot(cam.target[0] - orbit.centerX, cam.target[2] - orbit.centerZ) < 0.1 * radius);
                } else if (time >= 59.5 && time <= 64.0) {
                    // Keep the identifiable northeast mirror face centered throughout the close pass.
                    BOO_CHECK(std::abs(cam.target[0] - 3.86f) < 0.03f && std::abs(cam.target[1] - 7.20f) < 0.03f);
                    BOO_CHECK(std::abs(cam.target[2] - 7.57f) < 0.03f && cam.fovYRadians < 1.05f);
                }
                BOO_CHECK(cam.up[0] == 0.0f && cam.up[1] == 1.0f && cam.up[2] == 0.0f);
                if (!poses.empty()) {
                    const double delta = std::remainder(angle - previousAngle, tau);
                    BOO_CHECK(std::abs(delta) < 0.09); // A single quarter-second step cannot hide a cut.
                    if (time > 49.0 && time <= 71.0) {
                        const auto& previous = poses.back();
                        double dot = 0.0, currentLength2 = 0.0, previousLength2 = 0.0;
                        for (int axis = 0; axis < 3; ++axis) {
                            const double currentDirection = cam.target[axis] - cam.eye[axis];
                            const double previousDirection = previous.target[axis] - previous.eye[axis];
                            dot += currentDirection * previousDirection;
                            currentLength2 += currentDirection * currentDirection;
                            previousLength2 += previousDirection * previousDirection;
                        }
                        const double maxAngle = (motion < 1.f ? 12.0 : 26.0) * 0.25 * tau / 360.0;
                        BOO_CHECK(dot > std::cos(maxAngle) * std::sqrt(currentLength2 * previousLength2));
                    }
                    signedSweep += delta;
                    totalTravel += std::abs(delta);
                    minSweep = std::min(minSweep, signedSweep);
                    maxSweep = std::max(maxSweep, signedSweep);
                }
                previousAngle = angle;
                poses.push_back(cam);
            }
            if (motion == 1.0f) {
                BOO_CHECK(std::abs(std::abs(signedSweep) - tau) < 0.001);
                BOO_CHECK(std::abs(totalTravel - tau) < 0.001);
            } else if (motion > 0.f) {
                BOO_CHECK(maxSweep - minSweep > 0.05 && maxSweep - minSweep < 0.45);
                BOO_CHECK(totalTravel < 0.9);
            } else {
                BOO_CHECK(totalTravel == 0.0);
                for (const auto& cam : poses) {
                    for (int axis = 0; axis < 3; ++axis) {
                        BOO_CHECK(cam.eye[axis] == poses.front().eye[axis] && cam.target[axis] == poses.front().target[axis]);
                    }
                }
            }

            // Seek back through the same quarter-second frames at a different audio sample rate.
            for (size_t i = poses.size(); i-- > 0;) {
                const double time = orbit.start + 0.25 * static_cast<double>(i);
                const auto seek = evaluateShowCamera(ShowVisualMode::FullFilm360s, int64_t(time * 44100), 44100, 0.85f, false, motion);
                for (int axis = 0; axis < 3; ++axis) {
                    BOO_CHECK(seek.eye[axis] == poses[i].eye[axis] && seek.target[axis] == poses[i].target[axis]);
                }
                BOO_CHECK(seek.fovYRadians == poses[i].fovYRadians);
            }
        }
    }
    return true;
}

bool testMirrorWitnessFrustum() {
    using namespace boo;
    const Bmesh bird = buildCityBird(CityBirdSpecies::NightHeron);
    BOO_CHECK(!bird.vertices.empty());
    const float scale = mirrorWitnessPose(59.5f, 1.f).scale;
    const float halfSpan = std::max(std::abs(bird.bounds[0]), std::abs(bird.bounds[3])) * scale;
    const float yExtent = std::max(std::abs(bird.bounds[1]), std::abs(bird.bounds[4]));
    const float zExtent = std::max(std::abs(bird.bounds[2]), std::abs(bird.bounds[5]));
    // Wing joints rotate in XY about points on X: this bounds every flap pose.
    const float radius = std::hypot(halfSpan / scale + yExtent, zExtent) * scale;
    for (float motion : {1.f}) {
        for (int irisStep = 0; irisStep <= 20; ++irisStep) {
            const float iris = irisStep * 0.05f;
            const int steps = int(std::lround((kMirrorWitnessEnd - kMirrorWitnessStart) / .05f));
            for (int step = 0; step <= steps; ++step) {
            const float time = std::min(kMirrorWitnessStart + 0.05f * step, kMirrorWitnessEnd);
                const auto cam = evaluateShowCamera(ShowVisualMode::FullFilm360s,
                    int64_t(std::lround(time * 48000.f)), 48000, iris, false, motion);
                const auto pose = mirrorWitnessPose(time, motion);
                const float dx = cam.target[0] - cam.eye[0];
                const float dy = cam.target[1] - cam.eye[1];
                const float dz = cam.target[2] - cam.eye[2];
                const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float horizontal = std::hypot(dx, dz);
                const std::array<float, 3> forward{dx / length, dy / length, dz / length};
                const std::array<float, 3> right{dz / horizontal, 0.f, -dx / horizontal};
                const std::array<float, 3> up{-dx * dy / (horizontal * length), horizontal / length,
                                               -dz * dy / (horizontal * length)};
                const std::array<float, 3> delta{pose.center[0] - cam.eye[0], pose.center[1] - cam.eye[1],
                                                  pose.center[2] - cam.eye[2]};
                const auto dot = [&](const std::array<float, 3>& v) {
                    return delta[0] * v[0] + delta[1] * v[1] + delta[2] * v[2];
                };
                const float depth = dot(forward), x = std::abs(dot(right)), y = std::abs(dot(up));
                const float tanY = std::tan(cam.fovYRadians * 0.5f);
                for (float aspect : {16.f / 9.f, 21.f / 9.f, 32.f / 9.f}) {
                    const float tanX = tanY * aspect;
                    // Sphere is fully outside at least one normalized frustum plane.
                    const bool outside = depth < -radius ||
                        x - depth * tanX > radius * std::sqrt(1.f + tanX * tanX) ||
                        y - depth * tanY > radius * std::sqrt(1.f + tanY * tanY);
                    BOO_CHECK(outside);
                }
            }
        }
    }
    Bmesh cathedral;
    std::string error;
    BOO_CHECK(loadBmesh(resolveResourcePath("assets/meshes/enamel_cathedral.bmesh"), cathedral, &error));
    // Sample triangles and interpolated smooth normals of the actual stela 205
    // pane at the hold, then trace each reflected ray against the actual body.
    const auto cam = evaluateShowCamera(ShowVisualMode::FullFilm360s,
        int64_t(59.5f * 48000.f), 48000, .5f, false, 1.f);
    const auto birdAtHold = mirrorWitnessPose(59.5f, 1.f);
    float model[12];
    writeModelTransform(model, birdAtHold.center.data(), birdAtHold.yaw, birdAtHold.pitch,
                        birdAtHold.roll, birdAtHold.scale);
    using Vec = std::array<float, 3>;
    const auto dot3 = [](const Vec& a, const Vec& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
    const auto sub3 = [](const Vec& a, const Vec& b) -> Vec { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; };
    const auto cross = [](const Vec& a, const Vec& b) -> Vec {
        return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
    };
    bool reflected = false;
    for (const auto& sub : cathedral.submeshes) {
        if (sub.part != 205u || sub.material != 12u) continue;
        for (uint32_t j = 0; j + 2 < sub.indexCount && !reflected; j += 3) {
            const auto& a = cathedral.vertices[cathedral.indices[sub.firstIndex+j]];
            const auto& b = cathedral.vertices[cathedral.indices[sub.firstIndex+j+1]];
            const auto& c = cathedral.vertices[cathedral.indices[sub.firstIndex+j+2]];
            for (int iu = 0; iu <= 8 && !reflected; ++iu) for (int iv = 0; iv <= 8-iu && !reflected; ++iv) {
                const float u = iu/8.f, v = iv/8.f, w = 1.f-u-v;
                Vec p{}, normal{};
                for (int axis = 0; axis < 3; ++axis) {
                    p[axis] = w*a.positionMaterial[axis]+u*b.positionMaterial[axis]+v*c.positionMaterial[axis];
                    normal[axis] = w*a.normalPart[axis]+u*b.normalPart[axis]+v*c.normalPart[axis];
                }
                Vec incident{p[0]-cam.eye[0], p[1]-cam.eye[1], p[2]-cam.eye[2]};
                const float il = std::sqrt(dot3(incident, incident));
                const float nl = std::sqrt(dot3(normal, normal));
                if (il < 1.e-5f || nl < 1.e-5f) continue;
                for (int axis = 0; axis < 3; ++axis) { incident[axis] /= il; normal[axis] /= nl; }
                const float incidence = dot3(incident, normal);
                if (incidence >= 0.f) continue;
                Vec ray{};
                for (int axis = 0; axis < 3; ++axis) ray[axis] = incident[axis] - 2.f*incidence*normal[axis];
                const Vec toBird = sub3(birdAtHold.center, p);
                const float along = dot3(toBird, ray);
                Vec separation{};
                for (int axis = 0; axis < 3; ++axis) separation[axis] = toBird[axis]-along*ray[axis];
                if (along <= 0.f || dot3(separation, separation) >= .25f*.25f) continue;

                // Inverse of the renderer's uniform-scale model transform.
                Vec originLocal{}, rayLocal{};
                for (int axis = 0; axis < 3; ++axis) for (int row = 0; row < 3; ++row) {
                    originLocal[axis] += model[4*row+axis]*(p[row]-model[4*row+3])/(scale*scale);
                    rayLocal[axis] += model[4*row+axis]*ray[row]/(scale*scale);
                }
                for (const auto& body : bird.submeshes) {
                    if (body.part != kNightHeronBody) continue;
                    for (uint32_t k = 0; k + 2 < body.indexCount; k += 3) {
                        const auto& va = bird.vertices[bird.indices[body.firstIndex+k]].positionMaterial;
                        const auto& vb = bird.vertices[bird.indices[body.firstIndex+k+1]].positionMaterial;
                        const auto& vc = bird.vertices[bird.indices[body.firstIndex+k+2]].positionMaterial;
                        const Vec a0{va[0],va[1],va[2]}, b0{vb[0],vb[1],vb[2]}, c0{vc[0],vc[1],vc[2]};
                        const Vec e1 = sub3(b0,a0), e2 = sub3(c0,a0), h = cross(rayLocal,e2);
                        const float det = dot3(e1,h);
                        if (std::abs(det) < 1.e-7f) continue;
                        const Vec s = sub3(originLocal,a0);
                        const float hitU = dot3(s,h)/det;
                        const Vec q = cross(s,e1);
                        const float hitV = dot3(rayLocal,q)/det;
                        if (hitU >= 0.f && hitV >= 0.f && hitU+hitV <= 1.f && dot3(e2,q)/det > 0.f) {
                            reflected = true;
                            break;
                        }
                    }
                    if (reflected) break;
                }
            }
        }
    }
    BOO_CHECK(reflected);

    // Triangle AABBs enclose the loaded stelae, ribs, columns and canopy.
    // Their distance is a conservative lower bound on distance to the mesh.
    // Add half a sampling interval of travel to cover the continuous route.
    const int steps = int(std::lround((kMirrorWitnessEnd - kMirrorWitnessStart) / .05f));
    const auto first = mirrorWitnessPose(kMirrorWitnessStart, 1.f).center;
    const auto last = mirrorWitnessPose(kMirrorWitnessEnd, 1.f).center;
    const float routeLength = std::sqrt((last[0]-first[0])*(last[0]-first[0]) +
        (last[1]-first[1])*(last[1]-first[1]) + (last[2]-first[2])*(last[2]-first[2]));
    const float clearance = 5.f + halfSpan + routeLength / (2.f * steps);
    for (const auto& sub : cathedral.submeshes) {
        if (sub.part < 200u || sub.part > 219u) continue;
        for (uint32_t j = 0; j + 2 < sub.indexCount; j += 3) {
            const auto& a = cathedral.vertices[cathedral.indices[sub.firstIndex+j]].positionMaterial;
            const auto& b = cathedral.vertices[cathedral.indices[sub.firstIndex+j+1]].positionMaterial;
            const auto& c = cathedral.vertices[cathedral.indices[sub.firstIndex+j+2]].positionMaterial;
            std::array<float, 3> lo{}, hi{};
            for (int axis = 0; axis < 3; ++axis) {
                lo[axis] = std::min({a[axis], b[axis], c[axis]});
                hi[axis] = std::max({a[axis], b[axis], c[axis]});
            }
            for (int step = 0; step <= steps; ++step) {
                const float time = std::min(kMirrorWitnessStart + .05f * step, kMirrorWitnessEnd);
                const auto p = mirrorWitnessPose(time, 1.f).center;
                float distance2 = 0.f;
                for (int axis = 0; axis < 3; ++axis) {
                    const float d = std::max({lo[axis]-p[axis], 0.f, p[axis]-hi[axis]});
                    distance2 += d*d;
                }
                BOO_CHECK(distance2 >= clearance*clearance);
            }
        }
    }
    return true;
}

bool testDawnEgretFramingAndClearance() {
    using namespace boo;
    using Vec = std::array<float, 3>;
    const auto sub = [](const Vec& a, const Vec& b) -> Vec { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; };
    const auto dot = [](const Vec& a, const Vec& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
    const auto cross = [](const Vec& a, const Vec& b) -> Vec {
        return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
    };
    const Bmesh bird = buildCityBird(CityBirdSpecies::Egret);
    BOO_CHECK(!bird.vertices.empty());
    const float halfSpan = std::max(std::abs(bird.bounds[0]), std::abs(bird.bounds[3]));
    const float yExtent = std::max(std::abs(bird.bounds[1]), std::abs(bird.bounds[4]));
    const float zExtent = std::max(std::abs(bird.bounds[2]), std::abs(bird.bounds[5]));
    // The rotating wing joints stay inside this sphere, at every stroke and orientation.
    const float radius = std::hypot(halfSpan + yExtent, zExtent) * 1.08f;
    Bmesh alcove, city;
    std::string error;
    BOO_CHECK(loadBmesh(resolveResourcePath("assets/meshes/dawn_alcove.bmesh"), alcove, &error));
    BOO_CHECK(loadBmesh(resolveResourcePath("assets/meshes/taipei_101_hero.bmesh"), city, &error));

    // Trace to the actual glass, then reject sightlines blocked by the alcove's opaque triangles.
    const auto hitsWindow = [&](const Vec& eye, const Vec& point) {
        const Vec ray = sub(point, eye);
        float glassT=2.f, opaqueT=2.f;
        for (const auto& piece : alcove.submeshes) {
            for (uint32_t i=0; i+2<piece.indexCount; i+=3) {
                Vec a{}, b{}, c{};
                for (int axis=0; axis<3; ++axis) {
                    a[axis]=alcove.vertices[alcove.indices[piece.firstIndex+i]].positionMaterial[axis];
                    b[axis]=alcove.vertices[alcove.indices[piece.firstIndex+i+1]].positionMaterial[axis];
                    c[axis]=alcove.vertices[alcove.indices[piece.firstIndex+i+2]].positionMaterial[axis];
                }
                const Vec e1=sub(b,a), e2=sub(c,a), h=cross(ray,e2);
                const float det=dot(e1,h);
                if (std::abs(det)<1.e-7f) continue;
                const Vec v=sub(eye,a), q=cross(v,e1);
                const float u=dot(v,h)/det, w=dot(ray,q)/det, t=dot(e2,q)/det;
                if (u<0.f || w<0.f || u+w>1.f || t<=0.f || t>=1.f) continue;
                if (piece.material==40u && piece.part==237u) glassT=std::min(glassT,t);
                else if (piece.material!=40u) opaqueT=std::min(opaqueT,t);
            }
        }
        return glassT<1.f && glassT<opaqueT;
    };
    std::array<int, kDawnEgretCount> windowSamples{};
    int titleWindowSamples=0;
    constexpr float aspect=16.f/9.f;
    const int steps=int(std::lround((kDawnEgretEnd-kDawnEgretStart)/.05f));
    static_assert(kDawnEgretEnd < 355.f);
    for (int step=0; step<=steps; ++step) {
        const float time=std::min(kDawnEgretStart+.05f*step,kDawnEgretEnd);
        const auto cam=evaluateShowCamera(ShowVisualMode::FullFilm360s,
            int64_t(std::lround(time*48000.f)),48000,.5f,false,1.f);
        const Vec eye{cam.eye[0],cam.eye[1],cam.eye[2]};
        const Vec target{cam.target[0],cam.target[1],cam.target[2]};
        const Vec f0=sub(target,eye);
        const float length=std::sqrt(dot(f0,f0)), horizontal=std::hypot(f0[0],f0[2]);
        const Vec f{f0[0]/length,f0[1]/length,f0[2]/length};
        const Vec right{f0[2]/horizontal,0.f,-f0[0]/horizontal};
        const Vec up{-f0[0]*f0[1]/(horizontal*length),horizontal/length,-f0[2]*f0[1]/(horizontal*length)};
        const float tanY=std::tan(cam.fovYRadians*.5f), tanX=tanY*aspect;
        for (uint32_t b=0; b<kDawnEgretCount; ++b) {
            const auto pose=dawnEgretPose(time,b,1.f);
            const Vec delta=sub(pose.center,eye);
            const float depth=dot(delta,f), x=dot(delta,right), y=dot(delta,up);
            const bool outside=depth < -radius || std::abs(x)-depth*tanX > radius*std::hypot(1.f,tanX) ||
                std::abs(y)-depth*tanY > radius*std::hypot(1.f,tanY);
            if (step==0 || step==steps) {
                for (float wideAspect : {16.f/9.f,21.f/9.f,32.f/9.f}) {
                    const float wideTanX=tanY*wideAspect;
                    BOO_CHECK(depth < -radius || std::abs(x)-depth*wideTanX > radius*std::hypot(1.f,wideTanX) ||
                              std::abs(y)-depth*tanY > radius*std::hypot(1.f,tanY));
                }
            }
            if (outside || depth<=radius) continue;
            // Conservative projected sphere box in 1920x1080 normalized canvas coordinates.
            const float pxRadius=radius*std::hypot(depth,x)/(depth*(depth-radius)*tanX);
            const float pyRadius=radius*std::hypot(depth,y)/(depth*(depth-radius)*tanY);
            const float px=.5f+.5f*x/(depth*tanX), py=.5f-.5f*y/(depth*tanY);
            const float x0=px-.5f*pxRadius, x1=px+.5f*pxRadius;
            const float y0=py-.5f*pyRadius, y1=py+.5f*pyRadius;
            const auto separate=[&](float l,float t,float r,float bottom) {
                return x1<l || x0>r || y1<t || y0>bottom;
            };
            // The end-title glyphs and plate are active from 351 s. The overlay variant
            // is moved into the upper right; this conservative box includes its scrim.
            if (time>=351.f) {
                BOO_CHECK(separate(.075f,.105f,.37f,.41f));
                BOO_CHECK(separate(.64f,.33f,.95f,.62f));
            }
            // Captions and the end-card plate occupy the lower canvas.
            BOO_CHECK(y1 < .70f);
            if (std::abs(x)<depth*tanX && std::abs(y)<depth*tanY && hitsWindow(eye,pose.center)) {
                ++windowSamples[b];
                if (time>=351.f) ++titleWindowSamples;
            }
        }
    }
    for (int count : windowSamples) BOO_CHECK(count >= 5);
    BOO_CHECK(titleWindowSamples >= 2);

    // Triangle AABBs conservatively enclose actual building geometry. Each coordinate of
    // the route is monotonic, so each segment AABB encloses its whole continuous piece.
    struct Box { Vec lo, hi; };
    std::vector<Box> route;
    for (uint32_t b=0; b<kDawnEgretCount; ++b) for (int i=0; i<128; ++i) {
        const float t0=kDawnEgretStart+(kDawnEgretEnd-kDawnEgretStart)*float(i)/128.f;
        const float t1=kDawnEgretStart+(kDawnEgretEnd-kDawnEgretStart)*float(i+1)/128.f;
        const auto a=dawnEgretPose(t0,b,1.f).center, c=dawnEgretPose(t1,b,1.f).center;
        Box box{};
        for (int axis=0; axis<3; ++axis) { box.lo[axis]=std::min(a[axis],c[axis]); box.hi[axis]=std::max(a[axis],c[axis]); }
        route.push_back(box);
    }
    const float clearance=kBirdBuildingClearance+halfSpan*1.08f;
    const auto boxDistance2=[](const Box& a,const Box& b) {
        float d2=0.f;
        for (int axis=0; axis<3; ++axis) {
            const float d=std::max({a.lo[axis]-b.hi[axis],b.lo[axis]-a.hi[axis],0.f}); d2+=d*d;
        }
        return d2;
    };
    Box whole=route.front();
    for (const auto& segment:route) for (int axis=0; axis<3; ++axis) {
        whole.lo[axis]=std::min(whole.lo[axis],segment.lo[axis]);
        whole.hi[axis]=std::max(whole.hi[axis],segment.hi[axis]);
    }
    const auto checkMesh=[&](const Bmesh& mesh, const std::array<float,12>& transform) {
        for (const auto& piece:mesh.submeshes) for (uint32_t i=0; i+2<piece.indexCount; i+=3) {
            Box tri{};
            for (int j=0; j<3; ++j) {
                const auto& v=mesh.vertices[mesh.indices[piece.firstIndex+i+j]].positionMaterial;
                for (int axis=0; axis<3; ++axis) {
                    const float* row=transform.data()+4*axis;
                    const float p=row[0]*v[0]+row[1]*v[1]+row[2]*v[2]+row[3];
                    if (!j) tri.lo[axis]=tri.hi[axis]=p;
                    else { tri.lo[axis]=std::min(tri.lo[axis],p); tri.hi[axis]=std::max(tri.hi[axis],p); }
                }
            }
            if (boxDistance2(whole,tri)>=clearance*clearance) continue;
            for (const auto& segment:route) if (boxDistance2(segment,tri)<clearance*clearance) return false;
        }
        return true;
    };
    BOO_CHECK(checkMesh(alcove,{1,0,0,0, 0,1,0,0, 0,0,1,0}));
    CityModuleLayout modules;
    appendCityModules(modules,2,city);
    // At this cue the renderer moves/scales the entire Taipei kit to (33,-9.6,66), .064.
    BOO_CHECK(checkMesh(city,{.064f,0,0,33.f, 0,.064f,0,-9.6f, 0,0,.064f,66.f}));
    for (const auto& placement:modules.placements) {
        const Vec center{33.f+.064f*placement.center[0],-9.6f+.064f*placement.center[1],66.f+.064f*placement.center[2]};
        Box bounds{};
        for (int axis=0; axis<3; ++axis) {
            bounds.lo[axis]=center[axis]-.064f*placement.radius;
            bounds.hi[axis]=center[axis]+.064f*placement.radius;
        }
        if (boxDistance2(whole,bounds)>=clearance*clearance) continue;
        auto transform=placement.rows;
        for (int axis=0; axis<3; ++axis) {
            for (int col=0; col<4; ++col) transform[4*axis+col]*=.064f;
            transform[4*axis+3]+=(axis==0?33.f:axis==1?-9.6f:66.f);
        }
        for (uint32_t module:placement.modules) BOO_CHECK(checkMesh(modules.modules[module],transform));
    }
    return true;
}

bool testAircraftHeroFraming() {
    using namespace boo;
    for (float motion : {1.0f, 0.35f}) {
        // The film uses the trilogy's 60-second flight in a 30-second cue. Stop before the outgoing camera blend.
        for (int step = 0; step <= 54; ++step) {
            const float filmTime = 245.0f + 0.5f * step;
            const auto cam = evaluateShowCamera(ShowVisualMode::FullFilm360s, int64_t(filmTime * 48000), 48000, 0.5f, false, motion);
            const auto trilogy = evaluateTrilogyCamera(int64_t(60 + step) * 48000, 48000, false, motion);
            const auto pose = evaluateEvtolPose(ShowVisualMode::FullFilm360s, filmTime);
            for (int i = 0; i < 3; ++i) {
                BOO_CHECK(std::abs(trilogy.eye[i] - cam.eye[i]) < 1e-4f);
                BOO_CHECK(std::abs(trilogy.target[i] - cam.target[i]) < 1e-4f);
            }
            BOO_CHECK(cam.focusWeight == 1.0f && cam.focusDistance > 9.0f);

            float dx = cam.target[0] - cam.eye[0], dy = cam.target[1] - cam.eye[1], dz = cam.target[2] - cam.eye[2];
            const float length = std::sqrt(dx*dx + dy*dy + dz*dz);
            dx /= length; dy /= length; dz /= length;
            const float horizontal = std::hypot(dx, dz);
            const auto relay=relayFlyby(float(step),motion);
            if (relay.cameraWeight>0.f) {
                BOO_CHECK(cam.focusDistance < 95.f && cam.fovYRadians >= .45f && cam.fovYRadians < .76f);
                if (step>=38 && step<=48) {
                    // At the cutaway hold the tanker and receiver should both fit comfortably
                    // inside the shot with the focus at their distance.
                    BOO_CHECK(cam.focusDistance>50.f);
                    float group[12];
                    writeModelTransform(group,relay.center.data(),relay.yaw,relay.pitch,relay.bank,1.8f);
                    for (const auto local : {std::array<float,3>{0,0,0}, std::array<float,3>{.58f,-.8f,-6.45f}}) {
                        float v[3];
                        for (int axis=0; axis<3; ++axis) {
                            const float* row=group+4*axis;
                            v[axis]=row[0]*local[0]+row[1]*local[1]+row[2]*local[2]+row[3]-cam.eye[axis];
                        }
                        const float depth=dx*v[0]+dy*v[1]+dz*v[2];
                        const float halfHeight=depth*std::tan(.5f*cam.fovYRadians);
                        BOO_CHECK(std::abs(depth-cam.focusDistance)<10.f);
                        BOO_CHECK(std::abs((dz*v[0]-dx*v[2])/horizontal)+5.3f < halfHeight*(16.f/9.f));
                        BOO_CHECK(std::abs((v[1]-dy*depth)/horizontal)+2.1f < halfHeight);
                    }
                }
                continue;
            }
            BOO_CHECK(cam.focusDistance<19.f);
            const float horizonY = 0.5f + 0.5f * dy / (horizontal * std::tan(0.5f * cam.fovYRadians));
            BOO_CHECK(horizonY > 0.24f && horizonY < 0.35f);
            if (step == 30) BOO_CHECK(horizonY > 0.32f); // 260s banking shot looks across the bay.
            const float slopeY = 0.90f * std::tan(0.5f * cam.fovYRadians);
            const float slopeX = slopeY * (16.0f / 9.0f);
            float rows[12];
            writeModelTransform(rows, pose.position, pose.yaw, pose.pitch, pose.roll);
            for (const auto& hub : kEvtolRotorPivots) {
                float v[3]{};
                for (int i = 0; i < 3; ++i) {
                    const float* row = rows + 4*i;
                    v[i] = row[0]*hub[0] + row[1]*hub[1] + row[2]*hub[2] + row[3] - cam.eye[i];
                }
                const float depth = dx*v[0] + dy*v[1] + dz*v[2];
                const float right = (dz*v[0] - dx*v[2]) / horizontal;
                const float up = (v[1] - dy*depth) / horizontal;
                // A 1.25-unit sphere encloses the mast and swept blades at any rotor phase.
                BOO_CHECK(std::abs(right) + 1.25f*std::hypot(1.0f, slopeX) < depth*slopeX);
                BOO_CHECK(std::abs(up) + 1.25f*std::hypot(1.0f, slopeY) < depth*slopeY);
            }
        }
    }
    return true;
}

bool testTrilogyMeshAudioAndCamera() {
    using namespace boo;
    for(const char* name:{"shanghai_trestle","shenzhen_evtol","taipei_101_hero","glass_orchid_hero"}) {
        Bmesh mesh;std::string err;BOO_CHECK(loadBmesh(resolveResourcePath(std::string("assets/meshes/")+name+".bmesh"),mesh,&err));
        BOO_CHECK(mesh.indices.size()>24000 && !mesh.submeshes.empty());
        if(std::string(name)=="glass_orchid_hero")BOO_CHECK(mesh.indices.size()/3>=24000 && mesh.indices.size()/3<=36000);
    }
    Bmesh missing;std::string err;BOO_CHECK(!loadBmesh("this-mesh-does-not-exist.bmesh",missing,&err));
    auto source=resolveResourcePath("assets/meshes/glass_orchid_hero.bmesh");
    std::ifstream input(source,std::ios::binary);std::vector<char> bytes((std::istreambuf_iterator<char>(input)),{});bytes.back()^=1;
    auto corrupt=std::filesystem::temp_directory_path()/"boo-corrupt-test.bmesh";
    {std::ofstream out(corrupt,std::ios::binary);out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));}
    BOO_CHECK(!loadBmesh(corrupt,missing,&err));std::filesystem::remove(corrupt);
    for(uint32_t k=0;k<4;++k) {auto fallback=buildTrilogyFallbackKit(k);BOO_CHECK(fallback.proceduralFallback&&!fallback.indices.empty());}
    AudioSynthesizer48k synth;synth.setDurationSamples(8640000);synth.reset(0,0);
    std::vector<float> audio(8192);float last[2]={};float maxStep=0;
    for(uint64_t sample:{0ULL,2879600ULL,2880000ULL,5759600ULL,5760000ULL,8064000ULL,8639600ULL,0ULL}) {
        synth.seekTo(sample);synth.renderFrames(audio.data(),4096,false,false,1);
        for(size_t i=0;i<audio.size();++i){maxStep=std::max(maxStep,std::abs(audio[i]-last[i%2]));last[i%2]=audio[i];}
    }
    std::cout<<"M2 maximum seek/wrap waveform step: "<<maxStep<<"\n";BOO_CHECK(maxStep<.010f);
    for(int sec:{15,45,75,108,168,176}) {auto a=evaluateTrilogyCamera(int64_t(sec)*48000),b=evaluateTrilogyCamera(int64_t(sec)*48000);for(int i=0;i<3;++i){BOO_CHECK(std::isfinite(a.eye[i]));BOO_CHECK(a.eye[i]==b.eye[i]);}}
    for(int64_t boundary:{2880000LL,5760000LL}) {auto a=evaluateTrilogyCamera(boundary-1),b=evaluateTrilogyCamera(boundary);for(int i=0;i<3;++i){BOO_CHECK(std::abs(a.eye[i]-b.eye[i])<.001f);BOO_CHECK(std::abs(a.target[i]-b.target[i])<.001f);}}
    BOO_CHECK(evaluateTrilogyCamera(8064000).revealOpen01>.99f);
    return true;
}

bool testShowVisualModeRoutingAndEnamelMicrocosmKit() {
    using namespace boo;

    struct ExpectedCueMode {
        const char* relPath;
        PlaybackMode playbackMode;
        ShowVisualMode expectedVisualMode;
    };
    const ExpectedCueMode kCases[] = {
        {"assets/cues/m1_taipei_60s.json", PlaybackMode::FullFilm, ShowVisualMode::M1Slice60s},
        {"assets/cues/m2_trilogy_180s.json", PlaybackMode::FullFilm, ShowVisualMode::M2Trilogy180s},
        {"assets/cues/film_360s.json", PlaybackMode::FullFilm, ShowVisualMode::FullFilm360s},
        {"assets/cues/solo_rt.json", PlaybackMode::SoloRT, ShowVisualMode::SoloRT},
        {"assets/cues/solo_cuda.json", PlaybackMode::SoloCUDA, ShowVisualMode::SoloCUDA},
        {"assets/cues/solo_tensor.json", PlaybackMode::SoloTensor, ShowVisualMode::SoloTensor},
        {"assets/cues/solo_npu.json", PlaybackMode::SoloNPU, ShowVisualMode::SoloNPU},
    };

    for (const auto& c : kCases) {
        CueSheet sheet{};
        ValidationReport rep{};
        BOO_CHECK(loadCueSheetFromFile(resolveResourcePath(c.relPath), sheet, rep));
        BOO_CHECK(rep.valid);
        BOO_CHECK(cueUses3dVisuals(sheet));
        BOO_CHECK(resolveShowVisualMode(sheet, c.playbackMode) == c.expectedVisualMode);
        BOO_CHECK(!std::string(toString(c.expectedVisualMode)).empty());
    }

    const Bmesh kit4 = buildEnamelMicrocosmKit();
    BOO_CHECK(!kit4.vertices.empty() && !kit4.indices.empty() && kit4.submeshes.size() >= 28);
    for (const auto& v : kit4.vertices) {
        for (int i = 0; i < 3; ++i) {
            BOO_CHECK(std::isfinite(v.positionMaterial[i]) && std::isfinite(v.normalPart[i]));
        }
        const float n2 = v.normalPart[0] * v.normalPart[0] + v.normalPart[1] * v.normalPart[1] + v.normalPart[2] * v.normalPart[2];
        BOO_CHECK(n2 >= 0.98f && n2 <= 1.02f);
    }

    for (const ShowVisualMode mode : {ShowVisualMode::SoloRT, ShowVisualMode::SoloCUDA, ShowVisualMode::SoloTensor, ShowVisualMode::SoloNPU, ShowVisualMode::FullFilm360s}) {
        const auto c0 = evaluateShowCamera(mode, 960000);
        const auto c1 = evaluateShowCamera(mode, 960000);
        for (int i = 0; i < 3; ++i) {
            BOO_CHECK(std::isfinite(c0.eye[i]) && std::isfinite(c0.target[i]));
            BOO_CHECK(c0.eye[i] == c1.eye[i] && c0.target[i] == c1.target[i]);
        }
        BOO_CHECK(c0.fovYRadians > 0.2f && c0.fovYRadians < 1.8f);
    }

    // Check continuous movements within each shot. Camera edits between cities and
    // the cut out of the ribbon insert at 140 seconds have their own coverage.
    for (const int64_t boundary : {1920000LL, 4320000LL, 6480000LL, 9360000LL}) {
        const auto a = evaluateShowCamera(ShowVisualMode::FullFilm360s, boundary - 1);
        const auto b = evaluateShowCamera(ShowVisualMode::FullFilm360s, boundary);
        for (int i = 0; i < 3; ++i) {
            BOO_CHECK(std::abs(a.eye[i] - b.eye[i]) < 0.002f);
            BOO_CHECK(std::abs(a.target[i] - b.target[i]) < 0.002f);
        }
    }

    return true;
}

bool testMadameNModelManifestAndAsyncFallback() {
    using namespace boo;

    const auto xmlPath = resolveResourcePath("assets/models/madame_n_director.xml");
    const auto binPath = resolveResourcePath("assets/models/madame_n_director.bin");
    const auto metaPath = resolveResourcePath("assets/models/madame_n_metadata.json");
    BOO_CHECK(std::filesystem::exists(xmlPath));
    BOO_CHECK(std::filesystem::exists(binPath));
    BOO_CHECK(std::filesystem::exists(metaPath));

    const auto parsedMeta = parseJsonFile(metaPath);
    BOO_CHECK(parsedMeta.ok && parsedMeta.value.isObject());
    BOO_CHECK(parsedMeta.value["model_id"].asString() == "madame_n_director_v1");
    BOO_CHECK(parsedMeta.value["seed"].asInt64() == 266203);

    // Verify graceful fallback when disabled or when OpenVINO runtime DLL is simulated absent
    {
        OpenVinoDirector ovDir;
        OpenVinoDirectorConfig disCfg{};
        disCfg.modelXmlPath = xmlPath;
        disCfg.allowNpu = false;
        disCfg.allowCpuFallback = false;
        const auto disRes = ovDir.initialize(disCfg);
        BOO_CHECK(!ovDir.isReady());
        BOO_CHECK(disRes.status == BackendStatus::Substituted);
        BOO_CHECK(disRes.diagnosticCode == "DISABLED_BY_FLAG");
        BOO_CHECK(!disRes.successfullyExecutedTestWork);

        OpenVinoDirectorConfig missCfg{};
        missCfg.modelXmlPath = xmlPath;
        missCfg.simulateMissingRuntimeForTest = true;
        const auto missRes = ovDir.initialize(missCfg);
        BOO_CHECK(!ovDir.isReady());
        BOO_CHECK(missRes.status == BackendStatus::Substituted);
        BOO_CHECK(missRes.diagnosticCode == "OPENVINO_RUNTIME_ABSENT");
    }

    // Verify AsyncDirectorController simulated-missing-runtime fallback & bounded mood controls
    {
        AsyncDirectorController ctrl;
        ctrl.start(266203, xmlPath, false, false, true, true);
        (void)ctrl.waitForInitialCompile(500);
        FrameContext ctx{};
        ctx.samplePosition = 480000;
        ctx.sampleRate = 48000;
        ctx.filmTimeSeconds = 10.0;
        ctx.dtSeconds = 1.0 / 60.0;
        ctx.actIndex = 3;
        ctx.actProgress = 0.70f;
        const auto mood = ctrl.evaluateFrame(ctx, nullptr);
        BOO_CHECK(mood.isScriptedFallback);
        BOO_CHECK(mood.bloomStrength >= 0.0f && mood.bloomStrength <= 1.0f);
        BOO_CHECK(mood.irisAngle >= 0.0f && mood.irisAngle <= 1.0f);
        BOO_CHECK(mood.lightTension >= 0.0f && mood.lightTension <= 1.0f);
        BOO_CHECK(mood.spectralShift >= 0.0f && mood.spectralShift <= 1.0f);
        ctrl.shutdown();
    }

    return true;
}

bool testLateFilmEditionSeekAudioContinuity() {
    // Regression for absolute-time frequency interpolation: the prior score
    // chirped at late seeks while the Family/Midnight blend changed every sample.
    boo::AudioSynthesizer48k synth;
    synth.setDurationSamples(17280000);
    synth.reset(0, 0);
    std::array<float, 2048> pcm{};
    float previous[2]{}, maxStep=0;
    constexpr int seconds[]={18,95,145,175,225,255,318};
    for(uint32_t iteration=0;iteration<224;++iteration) {
        synth.setEdition((iteration&1u)?boo::Edition::Midnight:boo::Edition::Family);
        synth.seekTo(uint64_t(seconds[(iteration*3u)%7u])*48000+(iteration%11u)*800);
        const auto count=96u+(iteration%8u)*128u;
        synth.renderFrames(pcm.data(),count,iteration%19u==0u,iteration%23u==0u,1.f);
        for(uint32_t f=0;f<count;++f)for(size_t c=0;c<2;++c) {
            const float sample=pcm[f*2u+c];
            BOO_CHECK(std::isfinite(sample));
            maxStep=std::max(maxStep,std::abs(sample-previous[c]));previous[c]=sample;
        }
    }
    std::cout << "Late-film seek/edition maximum audio step: " << maxStep << '\n';
    BOO_CHECK(maxStep<.010f);
    return true;
}

bool testFullFilm360sScoreCaptionsAndEditions() {
    using namespace boo;

    FrameContext cut{};
    cut.sampleRate = 44100;
    auto orbitAt = [&](int64_t sample) {
        cut.samplePosition = sample;
        return isOrbitalCutaway(ShowVisualMode::FullFilm360s, cut);
    };
    BOO_CHECK(!orbitAt(295LL * cut.sampleRate - 1) && orbitAt(295LL * cut.sampleRate));
    BOO_CHECK(orbitAt(301LL * cut.sampleRate - 1) && !orbitAt(301LL * cut.sampleRate));
    BOO_CHECK(orbitAt(298LL * cut.sampleRate));
    BOO_CHECK(!isOrbitalCutaway(ShowVisualMode::M2Trilogy180s, cut));
    cut.interactive.exploreActive = true;
    BOO_CHECK(!isOrbitalCutaway(ShowVisualMode::FullFilm360s, cut));
    cut.interactive.exploreActive = false;
    cut.interactive.blendWeight = .5f;
    BOO_CHECK(!isOrbitalCutaway(ShowVisualMode::FullFilm360s, cut));

    // Neighboring terrace compositions blend across these four act boundaries.
    for (const int64_t boundary : {2160000LL, 4800000LL, 7200000LL, 9360000LL}) {
        const auto a = evaluateShowCamera(ShowVisualMode::FullFilm360s, boundary - 1, 48000, 0.5f, false, 1.0f);
        const auto b = evaluateShowCamera(ShowVisualMode::FullFilm360s, boundary, 48000, 0.5f, false, 1.0f);
        for (int i = 0; i < 3; ++i) {
            BOO_CHECK(std::abs(a.eye[i] - b.eye[i]) < 0.005f);
            BOO_CHECK(std::abs(a.target[i] - b.target[i]) < 0.005f);
        }
    }
    // Each city holds its own camera until the actual cut.
    for (const int64_t boundary : {11760000LL, 13200000LL}) {
        const auto earlier = evaluateShowCamera(ShowVisualMode::FullFilm360s, boundary - 480, 48000);
        const auto before = evaluateShowCamera(ShowVisualMode::FullFilm360s, boundary - 1, 48000);
        const auto after = evaluateShowCamera(ShowVisualMode::FullFilm360s, boundary, 48000);
        double cutDistanceSquared = 0;
        for (int i = 0; i < 3; ++i) {
            BOO_CHECK(std::abs(before.eye[i] - earlier.eye[i]) < 0.1f);
            const double delta = after.eye[i] - before.eye[i];
            cutDistanceSquared += delta * delta;
        }
        BOO_CHECK(cutDistanceSquared > 1.0);
    }
    // Verify exact reveal open states at 36.0s (1,728,000), 175.0s (8,400,000), and 342.0s (16,416,000)
    BOO_CHECK(evaluateShowCamera(ShowVisualMode::FullFilm360s, 1728000).revealOpen01 > 0.85f);
    BOO_CHECK(evaluateShowCamera(ShowVisualMode::FullFilm360s, 8400000).revealOpen01 > 0.99f);
    BOO_CHECK(evaluateShowCamera(ShowVisualMode::FullFilm360s, 16416000).revealOpen01 > 0.99f);

    // 2. Verify 7-movement 360.000s (17,280,000-sample) procedural score clickless seeks & Family vs Midnight differentiation
    {
        AudioSynthesizer48k synth;
        synth.setDurationSamples(17280000);
        synth.setEdition(Edition::Family);
        synth.reset(0, 0.85f);

        std::vector<float> buf(480 * 2, 0.0f);
        const int64_t seekTargets[] = {
            480000LL, 2160000LL, 4800000LL, 7200000LL, 8400000LL,
            9360000LL, 11760000LL, 13200000LL, 16416000LL, 17279000LL, 0LL
        };
        for (int64_t tgt : seekTargets) {
            synth.renderFrames(buf.data(), 240, false, false, 0.85f);
            const float prevL = buf[(239) * 2 + 0];
            const float prevR = buf[(239) * 2 + 1];
            synth.seekTo(static_cast<uint64_t>(tgt));
            synth.renderFrames(buf.data(), 480, false, false, 0.85f);

            const float seamStep = std::max(std::abs(buf[0] - prevL), std::abs(buf[1] - prevR));
            float peakAbs = 0.0f;
            for (uint32_t f = 0; f < 480; ++f) {
                peakAbs = std::max(peakAbs, std::max(std::abs(buf[f * 2 + 0]), std::abs(buf[f * 2 + 1])));
            }
            // < 0.010 across the seek crossfade seam (SPEC_v4 Section 13 M6) & finite non-clipping output
            BOO_CHECK(std::isfinite(seamStep) && seamStep < 0.010f);
            BOO_CHECK(peakAbs <= 0.99f);
        }

        // Family vs Midnight score differentiation at Act 3 (175.0s = 8,400,000 samples)
        AudioSynthesizer48k sFam, sMid;
        sFam.setDurationSamples(17280000);
        sMid.setDurationSamples(17280000);
        sFam.setEdition(Edition::Family);
        sMid.setEdition(Edition::Midnight);
        std::vector<float> warm(1024 * 2, 0.0f), outFam(512 * 2, 0.0f), outMid(512 * 2, 0.0f);
        sFam.reset(8400000, 0.85f);
        sMid.reset(8400000, 0.85f);
        sFam.renderFrames(warm.data(), 1024, false, false, 0.85f);
        sMid.renderFrames(warm.data(), 1024, false, false, 0.85f);
        sFam.renderFrames(outFam.data(), 512, false, false, 0.85f);
        sMid.renderFrames(outMid.data(), 512, false, false, 0.85f);
        double diffAccum = 0.0;
        for (size_t i = 0; i < outFam.size(); ++i) {
            diffAccum += std::abs(outFam[i] - outMid[i]);
        }
        BOO_CHECK(diffAccum > 0.5);

        const auto bFam = computeAnalyticalAudioBands(8400000, 48000, 17280000, Edition::Family);
        const auto bMid = computeAnalyticalAudioBands(8400000, 48000, 17280000, Edition::Midnight);
        BOO_CHECK(bFam.rms > 0.05f && bMid.rms > 0.05f);
        BOO_CHECK(std::abs(bFam.high - bMid.high) > 1e-4f || std::abs(bFam.low - bMid.low) > 1e-4f);
    }

    // 3. Verify Family vs Midnight captions, M1/M2 exclusion, and non-medical closing end card
    {
        BOO_CHECK(!evaluateFilmCaption(ShowVisualMode::M1Slice60s, 1728000, 48000, Edition::Family, true).active);
        BOO_CHECK(!evaluateFilmCaption(ShowVisualMode::M2Trilogy180s, 1728000, 48000, Edition::Family, true).active);
        BOO_CHECK(!evaluateFilmCaption(ShowVisualMode::FullFilm360s, 1728000, 48000, Edition::Family, false).active);

        const int64_t cueSamples[] = {384000LL, 1728000LL, 2688000LL, 5376000LL, 8400000LL, 9696000LL, 12096000LL, 13632000LL, 16416000LL};
        for (int64_t smp : cueSamples) {
            const auto cFam = evaluateFilmCaption(ShowVisualMode::FullFilm360s, smp, 48000, Edition::Family, true);
            const auto cMid = evaluateFilmCaption(ShowVisualMode::FullFilm360s, smp, 48000, Edition::Midnight, true);
            BOO_CHECK(cFam.active && cMid.active);
            BOO_CHECK(cFam.alpha > 0.5f && cMid.alpha > 0.5f);
            BOO_CHECK(!cFam.isEndCard && !cMid.isEndCard);
            BOO_CHECK(cFam.text != cMid.text);
            BOO_CHECK(cFam.text.size() <= 64u && cMid.text.size() <= 64u);
        }

        // End card at 355.0s (17,040,000 samples) is active regardless of captions toggle
        const auto endCard = evaluateFilmCaption(ShowVisualMode::FullFilm360s, 17040000, 48000, Edition::Family, false);
        BOO_CHECK(endCard.active && endCard.isEndCard && endCard.alpha > 0.95f);
        BOO_CHECK(endCard.text == "A CELEBRATION OF ORAL CARE - NOT MEDICAL ADVICE.");
    }

    // 4. Verify default settings now target film_360s.json with captions enabled & CLI overrides
    {
        AppSettings def{};
        BOO_CHECK(loadSettingsFromToml(resolveResourcePath("config/default.toml"), def));
        BOO_CHECK(def.cuePath.filename().string() == "film_360s.json");
        BOO_CHECK(def.showCaptions);
        BOO_CHECK(std::abs(def.motionScale - 1.0f) < 1e-6f);

        BOO_CHECK(def.effectsDetail == 1u);
        BOO_CHECK(def.pathTracing == PathTracingLevel::Low && def.dlssMode == DlssMode::Quality);
        BOO_CHECK(parseCommandLineArgs({"--path-tracing", "High", "--dlss", "Balanced"}, def));
        BOO_CHECK(def.pathTracing == PathTracingLevel::High && def.dlssMode == DlssMode::Balanced);
        BOO_CHECK(!parseCommandLineArgs({"--dlss", "fastest"}, def));
        BOO_CHECK(!parseCommandLineArgs({"--path-tracing", "quality"}, def));
        BOO_CHECK(def.rayReconstruction && def.frameGeneration == FrameGenerationMode::Auto && def.reflex == ReflexMode::On);
        BOO_CHECK(parseCommandLineArgs({"--ray-reconstruction", "off", "--frame-generation", "4x", "--reflex", "boost"}, def));
        BOO_CHECK(!def.rayReconstruction && def.frameGeneration == FrameGenerationMode::X4 && def.reflex == ReflexMode::Boost);
        BOO_CHECK(parseCommandLineArgs({"--frame-generation", "5x"}, def) && def.frameGeneration == FrameGenerationMode::X5);
        BOO_CHECK(parseCommandLineArgs({"--frame-generation", "6x"}, def) && def.frameGeneration == FrameGenerationMode::X6);
        BOO_CHECK(!parseCommandLineArgs({"--frame-generation", "7x"}, def));
        BOO_CHECK(availableFrameGenerationMode(FrameGenerationMode::X6,1) == FrameGenerationMode::X2);
        BOO_CHECK(availableFrameGenerationMode(FrameGenerationMode::X5,5) == FrameGenerationMode::X5);
        BOO_CHECK(availableFrameGenerationMode(FrameGenerationMode::X6,0) == FrameGenerationMode::Off);
        BOO_CHECK(nextFrameGenerationMode(FrameGenerationMode::Auto,0) == FrameGenerationMode::Off);
        BOO_CHECK(nextFrameGenerationMode(FrameGenerationMode::X2,1) == FrameGenerationMode::Off);
        BOO_CHECK(nextFrameGenerationMode(FrameGenerationMode::X4,5) == FrameGenerationMode::X5);
        BOO_CHECK(nextFrameGenerationMode(FrameGenerationMode::X5,5) == FrameGenerationMode::X6);
        BOO_CHECK(nextFrameGenerationMode(FrameGenerationMode::X6,8) == FrameGenerationMode::Off);
        BOO_CHECK(!parseCommandLineArgs({"--reflex", "4x"}, def));
        BOO_CHECK(!parseCommandLineArgs({"--ray-reconstruction", "sometimes"}, def));
        BOO_CHECK(!parseCommandLineArgs({"--effects-detail", "extreme"}, def));
        BOO_CHECK(parseCommandLineArgs({"--no-captions", "--reduced-motion", "--enable-spout", "--spout-name", "BooStage_Clean", "--effects-detail", "high"}, def));
        BOO_CHECK(def.effectsDetail == 2u);
        BOO_CHECK(!def.showCaptions);
        BOO_CHECK(std::abs(def.motionScale - 0.35f) < 1e-5f);
        BOO_CHECK(def.enableSpout);
        BOO_CHECK(def.spoutSenderName == "BooStage_Clean");

        SpoutSharedTextureInfo dummyInfo{};
        bool inList = true;
        BOO_CHECK(!querySpoutSenderInfo("NonExistentSender_BooTest_999", dummyInfo, &inList));
        BOO_CHECK(!inList);
    }

    return true;
}

bool testMediaPrivacyRedactionAndFaultFallback() {
    using namespace boo;

    // 1. Default settings & default MediaIngestSource are 100% opt-in idle (zero file or camera access)
    AppSettings def{};
    BOO_CHECK(loadSettingsFromToml(resolveResourcePath("config/default.toml"), def));
    BOO_CHECK(def.mediaInputPath.empty());
    BOO_CHECK(!def.enableCameraInput);
    BOO_CHECK(!def.injectMediaFail);

    BOO_CHECK(parseCommandLineArgs(
        {"--media-input", "C:\\Users\\secret_agent\\Videos\\sample.mp4", "--camera-input", "--inject-media-fail"}, def));
    BOO_CHECK(!def.mediaInputPath.empty());
    BOO_CHECK(def.enableCameraInput);
    BOO_CHECK(def.injectMediaFail);

    MediaIngestSource media;
    BOO_CHECK(!media.isActive());
    BOO_CHECK(!media.isLiveCameraActive());
    BOO_CHECK(media.mode() == MediaSourceMode::None);
    BOO_CHECK(media.decodedFrameCount() == 0ULL);
    BOO_CHECK(media.diagnosticCode() == "MEDIA_IDLE_OPT_IN");

    std::vector<uint8_t> nv12;
    uint64_t seq = 0;
    BOO_CHECK(!media.copyLatestNv12Frame(nv12, seq));

    // 2. Missing file under C:\Users\<username>\... must fail gracefully AND redact the username!
    const std::string homeSecretPath = "C:\\Users\\secret_agent_007\\Videos\\missing_orchid_clip.mp4";
    BOO_CHECK(!media.openLocalFile(homeSecretPath, false));
    BOO_CHECK(!media.isActive());
    BOO_CHECK(!media.isLiveCameraActive());
    BOO_CHECK(media.diagnosticCode() == "MEDIA_FILE_NOT_FOUND");
    BOO_CHECK(media.statusReason().find("secret_agent_007") == std::string::npos);
    BOO_CHECK(media.statusReason().find("<USER_HOME>") != std::string::npos);

    // 3. Fault injection (--inject-media-fail) on both file and camera paths
    BOO_CHECK(!media.openLocalFile(resolveResourcePath("assets/manifest.json"), true));
    BOO_CHECK(!media.isActive());
    BOO_CHECK(media.diagnosticCode() == "MEDIA_FAULT_INJECTED");

    BOO_CHECK(!media.openLiveCamera(true));
    BOO_CHECK(!media.isActive());
    BOO_CHECK(!media.isLiveCameraActive());
    BOO_CHECK(media.diagnosticCode() == "MEDIA_FAULT_INJECTED");

    // 4. Malformed / truncated MP4 container fixture must be rejected safely without throwing
    const auto corruptMp4 = resolveResourcePath("tests/fixtures/corrupt_sample.mp4");
    BOO_CHECK(!media.openLocalFile(corruptMp4, false));
    BOO_CHECK(!media.isActive());
    BOO_CHECK(!media.isLiveCameraActive());
    BOO_CHECK(!media.copyLatestNv12Frame(nv12, seq));

    media.shutdown();
    return true;
}

bool testRecordedSoundtrackFidelity() {
    using namespace boo;
    constexpr uint32_t frames = 12000u;
    constexpr uint32_t block = 1024u;
    constexpr double radiansPerHz = 6.283185307179586 / 48000.0;
    std::vector<float> source(frames * 2u);
    for (uint32_t i = 0; i < frames; ++i) {
        // A strong bass note and quiet cymbal harmonics sit under distinct left and right attacks.
        source[i * 2u] = static_cast<float>(0.48 * std::sin(i * 96.0 * radiansPerHz) +
                                           0.014 * std::sin(i * 15000.0 * radiansPerHz));
        source[i * 2u + 1u] = static_cast<float>(0.022 * std::sin(i * 11500.0 * radiansPerHz));
        if (i % 73u == 0u) source[i * 2u] = 0.93f;
        if (i % 101u == 0u) source[i * 2u + 1u] = -0.81f;
    }

    for (const bool compact : {false, true}) {
        CustomAudioTrack48k track{};
        track.frameCount = frames;
        if (compact) {
            track.interleavedPcm16.resize(source.size());
            std::transform(source.begin(), source.end(), track.interleavedPcm16.begin(),
                [](float s) { return static_cast<int16_t>(std::lround(s * 32768.0f)); });
        } else {
            track.interleavedStereo = source;
        }
        AudioSynthesizer48k mixer;
        mixer.setCustomTrack(&track);
        mixer.reset(3200u); // Within the recording, clear of the deliberate loop seam.
        std::vector<float> output(block * 2u);
        for (const float volume : {1.0f, 0.37f}) {
            mixer.renderFrames(output.data(), block, false, false, volume); // Settle the volume ramp.
            const size_t sampleOffset = static_cast<size_t>(mixer.primarySamplePosition()) * 2u;
            BOO_CHECK(mixer.renderFrames(output.data(), block, false, false, volume) == block);
            const float tolerance = compact ? 0.00005f : 0.000001f;
            for (size_t i = 0; i < output.size(); ++i) {
                BOO_CHECK(std::abs(output[i] - source[sampleOffset + i] * volume) < tolerance);
            }
        }
    }
    return true;
}

bool testCustomAudioTrackDecoderAndCrossfade() {
    using namespace boo;

    // 1. CLI --audio-file parsing
    AppSettings cfg{};
    BOO_CHECK(parseCommandLineArgs({"--audio-file", "suno_orchid_demo.mp3"}, cfg));
    BOO_CHECK(cfg.audioInputPath == "suno_orchid_demo.mp3");

    // 2. Missing file & fault-injection privacy redaction
    CustomAudioTrack48k badTrack{};
    std::string errMsg;
    BOO_CHECK(!loadCustomAudioFile48k(LR"(C:\Users\secret_composer\éŸ³æ¨‚\å¤œæ›².mp3)", badTrack, &errMsg, false));
    BOO_CHECK(errMsg.find("secret_composer") == std::string::npos);
    BOO_CHECK(errMsg.find("<USER_HOME>") != std::string::npos);
    BOO_CHECK(errMsg.find("éŸ³æ¨‚/å¤œæ›².mp3") != std::string::npos);

    errMsg.clear();
    BOO_CHECK(!loadCustomAudioFile48k(resolveResourcePath("assets/manifest.json"), badTrack, &errMsg, true));
    BOO_CHECK(!errMsg.empty());

    // 3. Generate a temporary 44,100 Hz stereo 16-bit PCM WAV file (0.50s warm chord) and decode to 48,000 Hz stereo float
    const std::filesystem::path tmpWav = std::filesystem::temp_directory_path() / L"boo_test_è‡ºåŒ—å¤œæ›²_44k1.wav";
    {
        constexpr uint32_t kSr = 44100u;
        constexpr uint32_t kFrames = 22050u; // 0.50s
        constexpr uint16_t kCh = 2u;
        constexpr uint32_t kDataBytes = kFrames * kCh * sizeof(int16_t);
        std::vector<int16_t> samples(kFrames * kCh);
        for (uint32_t i = 0; i < kFrames; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(kSr);
            const double sL = 0.45 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * t);
            const double sR = 0.45 * std::sin(2.0 * 3.14159265358979323846 * 329.628 * t);
            samples[i * 2u + 0u] = static_cast<int16_t>(sL * 32767.0);
            samples[i * 2u + 1u] = static_cast<int16_t>(sR * 32767.0);
        }
        std::ofstream out(tmpWav, std::ios::binary);
        const uint32_t riffSize = 36u + kDataBytes;
        const uint32_t fmtSize = 16u;
        const uint16_t audioFmt = 1u;
        const uint32_t byteRate = kSr * kCh * 2u;
        const uint16_t blockAlign = kCh * 2u;
        const uint16_t bitsPerSample = 16u;
        out.write("RIFF", 4);
        out.write(reinterpret_cast<const char*>(&riffSize), 4);
        out.write("WAVEfmt ", 8);
        out.write(reinterpret_cast<const char*>(&fmtSize), 4);
        out.write(reinterpret_cast<const char*>(&audioFmt), 2);
        out.write(reinterpret_cast<const char*>(&kCh), 2);
        out.write(reinterpret_cast<const char*>(&kSr), 4);
        out.write(reinterpret_cast<const char*>(&byteRate), 4);
        out.write(reinterpret_cast<const char*>(&blockAlign), 2);
        out.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
        out.write("data", 4);
        out.write(reinterpret_cast<const char*>(&kDataBytes), 4);
        out.write(reinterpret_cast<const char*>(samples.data()), kDataBytes);
    }

    CustomAudioTrack48k custom{};
    errMsg.clear();
    const bool decodedOk = loadCustomAudioFile48k(tmpWav, custom, &errMsg, false);
    std::error_code rmEc;
    std::filesystem::remove(tmpWav, rmEc);

    if (decodedOk) {
        // 0.50s @ 48 kHz ~= 24,000 frames
        BOO_CHECK(custom.frameCount >= 23500u && custom.frameCount <= 24500u);
        BOO_CHECK(custom.interleavedStereo.size() == custom.frameCount * 2u);
        BOO_CHECK(custom.label == "boo_test_è‡ºåŒ—å¤œæ›²_44k1.wav");
        // A quiet source keeps its authored level through 44.1 -> 48 kHz conversion.
        for (size_t channel = 0; channel < 2u; ++channel) {
            float peak = 0.0f;
            for (size_t frame = 2400u; frame < 21600u; ++frame) {
                peak = std::max(peak, std::abs(custom.interleavedStereo[frame * 2u + channel]));
            }
            BOO_CHECK(std::abs(peak - 0.45f) < 0.005f);
        }
    } else {
        // Fallback synthetic custom buffer if running in a headless server container without Media Foundation codecs
        custom.frameCount = 24000u;
        custom.label = "synthetic_fallback.wav";
        custom.interleavedStereo.resize(custom.frameCount * 2u);
        for (uint64_t i = 0; i < custom.frameCount; ++i) {
            const double t = static_cast<double>(i) / 48000.0;
            custom.interleavedStereo[static_cast<size_t>(i) * 2u + 0u] = static_cast<float>(0.6 * std::sin(6.2831853 * 220.0 * t));
            custom.interleavedStereo[static_cast<size_t>(i) * 2u + 1u] = static_cast<float>(0.6 * std::sin(6.2831853 * 329.628 * t));
        }
    }

    // 4. Verify live clickless crossfade: Built-in RTX Score -> Custom Track -> Built-in RTX Score
    AudioSynthesizer48k synth;
    synth.setDurationSamples(17280000);
    synth.reset(480000, 0.0f);

    std::vector<float> pcm(1024u * 2u, 0.0f);
    auto measure = [&](uint32_t frames, AudioBandSnapshot* outB = nullptr) -> bool {
        pcm.assign(static_cast<size_t>(frames) * 2u, 0.0f);
        synth.renderFrames(pcm.data(), frames, false, false, 0.85f, outB);
        for (const float sample : pcm) BOO_CHECK(std::isfinite(sample));
        return true;
    };

    AudioSynthesizer48k uninterrupted;
    uninterrupted.setDurationSamples(17280000);
    uninterrupted.reset(480000, 0.0f);
    std::vector<float> reference(1025u * 2u);
    uninterrupted.renderFrames(reference.data(), 1025u, false, false, 0.85f);
    BOO_CHECK(measure(1024u));
    synth.setCustomTrack(&custom);
    synth.seekTo(6000u);
    AudioBandSnapshot customBands{};
    BOO_CHECK(measure(1024u, &customBands));
    BOO_CHECK(customBands.rms > 0.02f);
    for (size_t channel = 0; channel < 2u; ++channel) {
        BOO_CHECK(std::abs(pcm[channel] - reference[1024u * 2u + channel]) < 0.00001f);
    }

    const size_t outgoingOffset = static_cast<size_t>(synth.primarySamplePosition()) * 2u;
    synth.setCustomTrack(nullptr);
    synth.seekTo(960000u);
    BOO_CHECK(measure(1024u));
    for (size_t channel = 0; channel < 2u; ++channel) {
        BOO_CHECK(std::abs(pcm[channel] - custom.interleavedStereo[outgoingOffset + channel] * 0.85f) < 0.00001f);
    }

    // 5. Verify bundled 4-track first-party OST assets, cooperative cancellation, compact PCM16, and playlist cycling
    const std::array<std::filesystem::path, AudioEngine::kBundledTrackCount> ostPaths = {
        resolveResourcePath("assets/audio/01_neon_orchid.flac"),
        resolveResourcePath("assets/audio/02_acid_lotus.flac"),
        resolveResourcePath("assets/audio/03_velvet_cipher.flac"),
        resolveResourcePath("assets/audio/04_glass_dragon.flac")
    };
    for (const auto& p : ostPaths) {
        BOO_CHECK(std::filesystem::exists(p));
        BOO_CHECK(std::filesystem::file_size(p) > (3ULL * 1024ULL * 1024ULL));
        std::ifstream file(p, std::ios::binary);
        char signature[4]{};
        BOO_CHECK(file.read(signature, sizeof(signature)));
        BOO_CHECK(std::string(signature, sizeof(signature)) == "fLaC");
    }

    std::atomic<bool> preCancelled{true};
    CustomAudioTrack48k cancelledTrack{};
    BOO_CHECK(!loadCustomAudioFile48k(ostPaths[0], cancelledTrack, &errMsg, false, &preCancelled, true));
    BOO_CHECK(cancelledTrack.frameCount == 0u);

    if (decodedOk) {
        for (const auto& path : ostPaths) {
            CustomAudioTrack48k ost{};
            BOO_CHECK(loadCustomAudioFile48k(path, ost, &errMsg, false, nullptr, true));
            BOO_CHECK(ost.frameCount > (180ULL * 48000ULL));
            BOO_CHECK(ost.interleavedStereo.empty());
            BOO_CHECK(ost.interleavedPcm16.size() == ost.frameCount * 2u);
            AudioSynthesizer48k ostMixer;
            ostMixer.setCustomTrack(&ost);
            ostMixer.reset(48000ULL * 30ULL);
            AudioBandSnapshot ostBands{};
            BOO_CHECK(ostMixer.renderFrames(pcm.data(), 1024u, false, false, 0.85f, &ostBands) == 1024u);
            BOO_CHECK(ostBands.rms > 0.01f);
            const size_t sourceOffset = static_cast<size_t>(ostMixer.primarySamplePosition()) * 2u;
            BOO_CHECK(ostMixer.renderFrames(pcm.data(), 1024u, false, false, 0.85f) == 1024u);
            for (size_t i = 0; i < pcm.size(); ++i) {
                const float mastered = static_cast<float>(ost.interleavedPcm16[sourceOffset + i]) / 32768.0f;
                BOO_CHECK(pcm[i] == mastered * 0.85f);
            }
        }
    }

    AudioEngine ostEngine;
    ostEngine.startBundledSoundtrack(ostPaths, 1u);
    BOO_CHECK(ostEngine.hasBundledSoundtrack());
    BOO_CHECK(ostEngine.activeSoundtrackSlot() == 1u);
    BOO_CHECK(ostEngine.cycleSoundtrackForward() == 2u);
    BOO_CHECK(ostEngine.cycleSoundtrackForward() == 3u);
    BOO_CHECK(ostEngine.cycleSoundtrackForward() == 4u);
    BOO_CHECK(ostEngine.cycleSoundtrackForward() == 0u);
    BOO_CHECK(ostEngine.cycleSoundtrackForward() == 1u);
    BOO_CHECK(ostEngine.cycleSoundtrackBackward() == 0u);
    BOO_CHECK(ostEngine.cycleSoundtrackBackward() == 4u);
    ostEngine.shutdown();

    return true;
}

bool testAudioReactiveFollowerBeatAndFlashSafety() {
    using boo::AudioReactiveFollower;
    using boo::AudioReactiveState;
    const double dt = 1.0 / 60.0;

    // 1. A quiet 2 Hz kick is auto-levelled and yields roughly one beat per kick.
    AudioReactiveFollower f;
    int beats = 0;
    float prevBeat = 0.0f, maxLow = 0.0f;
    for (int i = 0; i < 600; ++i) {
        const double ph = std::fmod(i * dt, 0.5);
        const AudioReactiveState s = f.update(0.02f + 0.10f * static_cast<float>(std::exp(-ph / 0.08)), 0.2f, 0.05f, 0.3f, dt, false);
        BOO_CHECK(s.low >= 0.0f && s.low <= 1.0f && s.beat >= 0.0f && s.beat <= 1.0f);
        if (i > 120) {
            if (s.beat > prevBeat + 0.2f) ++beats;
            maxLow = std::max(maxLow, s.low);
        }
        prevBeat = s.beat;
    }
    BOO_CHECK(beats >= 13 && beats <= 17); // 8 s of 2 Hz kicks
    BOO_CHECK(maxLow > 0.5f);              // 0.12 raw peak normalised by automatic gain

    // 2. Flash safety: an 8 Hz kick can never retrigger the beat 3 or more times per second.
    f.reset();
    beats = 0;
    prevBeat = 0.0f;
    for (int i = 0; i < 600; ++i) {
        const double ph = std::fmod(i * dt, 0.125);
        const AudioReactiveState s = f.update(0.05f + 0.5f * static_cast<float>(std::exp(-ph / 0.02)), 0.2f, 0.1f, 0.3f, dt, false);
        if (s.beat > prevBeat + 0.05f) ++beats;
        prevBeat = s.beat;
    }
    BOO_CHECK(beats < 30);

    // 3. Reduced flash: no beat envelope, band signals halved.
    f.reset();
    float maxBeat = 0.0f;
    maxLow = 0.0f;
    for (int i = 0; i < 600; ++i) {
        const double ph = std::fmod(i * dt, 0.5);
        const AudioReactiveState s = f.update(0.02f + 0.5f * static_cast<float>(std::exp(-ph / 0.08)), 0.2f, 0.1f, 0.3f, dt, true);
        maxBeat = std::max(maxBeat, s.beat);
        maxLow = std::max(maxLow, s.low);
    }
    BOO_CHECK(maxBeat == 0.0f);
    BOO_CHECK(maxLow <= 0.5f);

    // 4. Silence stays silent, hold glides to rest, non-finite input is ignored, packing is RGBA8.
    f.reset();
    AudioReactiveState s{};
    for (int i = 0; i < 300; ++i) s = f.update(0.0f, 0.0f, 0.0f, 0.0f, dt, false);
    BOO_CHECK(s.low == 0.0f && s.mid == 0.0f && s.high == 0.0f && s.beat == 0.0f);
    for (int i = 0; i < 60; ++i) s = f.update(0.9f, 0.9f, 0.9f, 0.9f, dt, false);
    BOO_CHECK(s.low > 0.5f);
    for (int i = 0; i < 180; ++i) s = f.update(0.9f, 0.9f, 0.9f, 0.9f, dt, false, true);
    BOO_CHECK(s.low < 0.01f && s.beat < 0.01f);
    s = f.update(std::nanf(""), std::nanf(""), std::nanf(""), std::nanf(""), dt, false);
    BOO_CHECK(std::isfinite(s.low) && std::isfinite(s.mid) && std::isfinite(s.high) && std::isfinite(s.beat));
    AudioReactiveState full{};
    full.low = 1.0f; full.mid = 0.5f; full.high = 0.0f; full.beat = 1.0f;
    BOO_CHECK(AudioReactiveFollower::pack(full) == 0xFF0080FFu);
    BOO_CHECK(AudioReactiveFollower::pack(AudioReactiveState{}) == 0u);
    return true;
}

// Every shipped shader must run on every Direct3D 12 GPU the app supports. Intel Iris Xe graphics has no
// double-precision or 64-bit integer math: a shader that asks for either cannot be built there, and the app
// falls back to its test picture (Store versions 1.0.0 to 1.1.12 did). DXC types a choice between two plain
// numbers (c ? 0.5 : 1.0) as double, so such literals need an f.
bool testShadersRunOnEveryGpu() {
    // Beside this executable, where the build puts them: from the source tree (as CTest runs it) the relative
    // path finds only the .hlsl sources.
    char* program = nullptr;
    std::filesystem::path folder;
    if (_get_pgmptr(&program) == 0 && program) folder = std::filesystem::path(program).parent_path() / "assets" / "shaders";
    std::error_code ec;
    if (!std::filesystem::exists(folder, ec)) folder = boo::resolveResourcePath("assets/shaders");
    // The raster film, Safe and Lite load these even on GPUs without optional
    // shader features. Include the M1 fallback and Explore accumulation paths.
    constexpr const char* rasterShaders[] = {
        "m0_test_vs.cso", "m0_test_ps.cso", "m1_raster_scene.cso",
        "m1_bloom_down.cso", "m1_bloom_up.cso", "m1_composite.cso",
        "m1_present_hud.cso", "m1_present_clean.cso", "m1_hud_cache.cso", "m1_hud_cache_clean.cso",
        "m1_present_cached.cso", "m1_present_cached_frame_generation.cso", "m1_present_frame_generation.cso",
        "m1_present_clean_frame_generation.cso", "m1_particles_sub_cs.cso",
        "m1_particles_vs.cso", "m1_particles_ps.cso", "m1_tensor_sub_cs.cso",
        "m1_nv12_pack_cs.cso", "m2_mesh_vs.cso", "m2_depth_cutout_ps.cso",
        "m2_mesh_ps.cso", "m2_mesh_glass_ps.cso", "m2_mesh_pane_ps.cso", "m2_mesh_pane_distance_ps.cso",
        "m2_mesh_cutout_ps.cso", "m2_sky_vs.cso", "m2_sky_ps.cso",
        "m2_accum_vs.cso", "m2_accum_blend_ps.cso", "m2_accum_resolve_ps.cso",
        "m2_motion_vs.cso", "m2_motion_ps.cso", "m2_motion_cutout_ps.cso", "m2_sky_motion_ps.cso"
    };
    for (const char* name : rasterShaders) {
        std::ifstream file(folder / name, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
        if (bytes.size() <= 32 || std::memcmp(bytes.data(), "DXBC", 4) != 0) {
            std::cout << "  Missing or invalid raster shader: " << name << std::endl;
            BOO_CHECK(false);
        }
        const auto read32 = [&](size_t at) { uint32_t v = 0; if (at + 4 <= bytes.size()) std::memcpy(&v, bytes.data() + at, 4); return v; };
        bool foundFeatures = false, foundDxil = false;
        uint64_t features = 0;
        uint32_t version = 0;
        for (uint32_t part = 0, parts = read32(28); part < parts && part < 64; ++part) {
            const size_t at = read32(32 + 4 * static_cast<size_t>(part));
            if (at + 16 > bytes.size()) continue;
            if (std::memcmp(bytes.data() + at, "SFI0", 4) == 0 && read32(at + 4) >= 8) {
                std::memcpy(&features, bytes.data() + at + 8, 8);
                foundFeatures = true;
            }
            if (std::memcmp(bytes.data() + at, "DXIL", 4) == 0 && read32(at + 4) >= 8) {
                version = read32(at + 8);
                foundDxil = true;
            }
        }
        if (!foundFeatures || !foundDxil || features != 0 || (version & 0xffu) != 0x60u) {
            std::cout << "  Raster shader requires optional features or is not SM 6.0: " << name << std::endl;
            BOO_CHECK(false);
        }
    }
    size_t checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (entry.path().extension() != ".cso") continue;
        std::ifstream file(entry.path(), std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
        BOO_CHECK(bytes.size() > 32 && std::memcmp(bytes.data(), "DXBC", 4) == 0);
        const auto read32 = [&](size_t at) { uint32_t v = 0; if (at + 4 <= bytes.size()) std::memcpy(&v, bytes.data() + at, 4); return v; };
        uint64_t features = 0; // the container's SFI0 part: what the shader requires of the GPU
        for (uint32_t part = 0, parts = read32(28); part < parts && part < 64; ++part) {
            const size_t at = read32(32 + 4 * static_cast<size_t>(part));
            if (at + 16 <= bytes.size() && std::memcmp(bytes.data() + at, "SFI0", 4) == 0) std::memcpy(&features, bytes.data() + at + 8, 8);
        }
        constexpr uint64_t kDoubles = 0x1, kDoubleExtensions = 0x20, kInt64 = 0x8000;
        if ((features & (kDoubles | kDoubleExtensions | kInt64)) != 0) {
            std::cout << "  " << entry.path().filename().string() << " needs double or 64-bit integer math" << std::endl;
            BOO_CHECK(false);
        }
        ++checked;
    }
    BOO_CHECK(checked > 30);
    return true;
}

bool testUiTextLanguages() {
    using boo::UiText;
    BOO_CHECK(boo::uiLanguageCount() == 25);
    BOO_CHECK(std::string(boo::uiLanguageTag(0)) == "en-US");
    for (size_t lang = 0; lang < boo::uiLanguageCount(); ++lang) {
        for (uint32_t id = 0; id < static_cast<uint32_t>(UiText::Count); ++id) {
            const std::wstring text = boo::uiTextAt(lang, static_cast<UiText>(id));
            // The words of the start-up list are short in some scripts.
            const bool stepWords = id >= static_cast<uint32_t>(UiText::LoadingDevice);
            BOO_CHECK(text.size() > (id == static_cast<uint32_t>(UiText::Loading) || stepWords ? 2u : 8u));
            // Every translation of the other strings keeps the product name; the box body keeps the API name.
            if (!stepWords && id != static_cast<uint32_t>(UiText::GraphicsFailureBody) && id != static_cast<uint32_t>(UiText::Loading))
                BOO_CHECK(text.find(L"BOO SAH SAH") != std::wstring::npos);
            if (id == static_cast<uint32_t>(UiText::UnsupportedGpuBody)) BOO_CHECK(text.find(L"Direct3D 12") != std::wstring::npos);
            if (lang > 0) BOO_CHECK(text != boo::uiTextAt(0, static_cast<UiText>(id)));
        }
    }
    // Windows LANGIDs: fr-FR, zh-CN, zh-TW, zh-HK, pt-PT, ar-SA; an unlisted language (Greek) falls back to English.
    BOO_CHECK(boo::uiTextForLanguage(UiText::UnsupportedGpuTitle, 0x040C) == boo::uiTextAt(1, UiText::UnsupportedGpuTitle));
    const wchar_t* simplified = boo::uiTextForLanguage(UiText::UnsupportedGpuTitle, 0x0804);
    const wchar_t* traditional = boo::uiTextForLanguage(UiText::UnsupportedGpuTitle, 0x0404);
    BOO_CHECK(simplified != traditional);
    BOO_CHECK(boo::uiTextForLanguage(UiText::UnsupportedGpuTitle, 0x0C04) == traditional);
    BOO_CHECK(boo::uiTextForLanguage(UiText::UnsupportedGpuTitle, 0x0816) == boo::uiTextForLanguage(UiText::UnsupportedGpuTitle, 0x0416));
    BOO_CHECK(boo::uiTextForLanguage(UiText::UnsupportedGpuTitle, 0x0408) == boo::uiTextAt(0, UiText::UnsupportedGpuTitle));
    BOO_CHECK(boo::uiLanguageIsRightToLeft(0x0401));
    BOO_CHECK(!boo::uiLanguageIsRightToLeft(0x0409) && !boo::uiLanguageIsRightToLeft(0x0408));
    return true;
}

// The string catalogs: every language has every key, protected tokens survive translation, every
// caption the film can show has a catalog entry, and every HUD label fits its box in every language.
bool testUiStringCatalogs() {
    using namespace boo;
    const std::filesystem::path dir = "assets/strings";
    const std::vector<std::string> languages = uiAvailableLanguages(dir);
    BOO_CHECK(languages.size() == 25);

    UiStrings catalog;
    BOO_CHECK(catalog.load(dir, "en-US"));
    BOO_CHECK(!catalog.active()); // English never uses the text atlas

    // Every caption line the film or the NPU solo can show resolves to a catalog key.
    size_t captionLines = 0;
    for (const ShowVisualMode mode : {ShowVisualMode::FullFilm360s, ShowVisualMode::SoloNPU}) {
        for (const Edition edition : {Edition::Family, Edition::Midnight}) {
            std::string last;
            for (int half = 0; half <= 720; ++half) {
                const CaptionLine cap = evaluateFilmCaption(mode, static_cast<int64_t>(half) * 24000, 48000, edition, true);
                if (!cap.active || std::string(cap.text) == last) continue;
                last = std::string(cap.text);
                ++captionLines;
                if (catalog.captionKey(last).empty()) std::cout << "  caption not in catalog: " << last << "\n";
                BOO_CHECK(!catalog.captionKey(last).empty());
            }
        }
    }
    BOO_CHECK(captionLines >= 24);

    std::vector<std::string> keys;
    {
        const JsonParseResult source = parseJsonFile(dir / "en-US.json");
        BOO_CHECK(source.ok);
        for (const auto& [key, entry] : source.value.find("strings")->asObject()) keys.push_back(key);
    }
    // 97 interface strings, plus the 8 solo-wipe labels (B2), plus the results card's.
    BOO_CHECK(keys.size() == 105 + static_cast<size_t>(ResultText::Count));
    BOO_CHECK(std::find(keys.begin(), keys.end(), "win.preset_lite") != keys.end());
    BOO_CHECK(std::find(keys.begin(), keys.end(), "hud.wipe_dxr_on") != keys.end());
    BOO_CHECK(std::find(keys.begin(), keys.end(), "hud.state_idle") != keys.end());
    BOO_CHECK(std::find(keys.begin(), keys.end(), "hud.play_again") != keys.end());

    UiTextLayer layer;
    size_t problems = 0;
    for (const std::string& tag : languages) {
        BOO_CHECK(catalog.load(dir, tag));
        const auto& signs = cityBlimpSigns(catalog.languageTag());
        BOO_CHECK(signs.languageTag == tag);
        const auto checkFace = [&](const BlimpSignFace& face, const char* key) {
            const auto* text = catalog.active() ? catalog.find(key) : catalog.findEnglish(key);
            return text && face.text == *text && face.width > 0 && face.height > 0 &&
                   face.rows.size() == ((face.width+3)/4)*face.height &&
                   face.rows.find_first_not_of('0') != std::string_view::npos;
        };
        BOO_CHECK(checkFace(signs.front,"blimp.bloomLate"));
        BOO_CHECK(checkFace(signs.back,"blimp.stayWild"));
        if (tag == "en-US") continue;
        BOO_CHECK(catalog.active() && catalog.languageTag() == tag);
        BOO_CHECK(catalog.rightToLeft() == (tag == "ar-SA"));
        for (const std::string& key : keys) {
            const std::wstring* text = catalog.find(key);
            if (!text || text->empty()) { std::cout << "  " << tag << " is missing " << key << "\n"; ++problems; continue; }
            const auto keeps = [&](const wchar_t* token) { return text->find(token) != std::wstring::npos; };
            bool ok = true;
            if (key == "hud.act") ok = keeps(L"{n}");
            if (key == "hud.mode_film" || key == "hud.mode_free") ok = keeps(L"F:");
            if (key == "hud.bench") ok = keeps(L"[B]");
            if (key == "hud.all") ok = keeps(L"[") && keeps(L"]");
            if (key == "hud.ost.synth") ok = keeps(L"RTX");
            if (key == "hud.ost.custom") ok = keeps(L"MP3+");
            if (!ok) { std::cout << "  " << tag << " " << key << " lost a protected token\n"; ++problems; }
        }
        if (layer.ready()) {
            for (uint32_t uiScale = 1; uiScale <= 3; ++uiScale) {
                for (uint32_t i = 0; i < static_cast<uint32_t>(UiLabel::Count); ++i) {
                    const float fit = layer.labelFit(catalog, static_cast<UiLabel>(i), uiScale);
                    if (fit < UiTextLayer::kMinLabelFit - 0.01f) {
                        std::cout << "  " << tag << " " << uiLabelSpec(static_cast<UiLabel>(i)).key << " does not fit at HUD scale " << uiScale << "\n";
                        ++problems;
                    }
                }
            }
        }
    }
    BOO_CHECK(problems == 0);

    // The pseudo-language exercises the same path with accented, longer text.
    BOO_CHECK(catalog.load(dir, "qps-ploc") && catalog.active() && catalog.size() == keys.size());
    // A language without a catalog, and English, both leave the glyph tables in charge.
    BOO_CHECK(catalog.load(dir, "el-GR") && !catalog.active());

    BOO_CHECK(uiMatchLanguage({"pt-PT"}, languages) == "pt-BR");
    BOO_CHECK(uiMatchLanguage({"zh-Hant-HK"}, languages) == "zh-TW");
    BOO_CHECK(uiMatchLanguage({"zh-Hans-SG"}, languages) == "zh-CN");
    BOO_CHECK(uiMatchLanguage({"nn-NO"}, languages) == "nb-NO");
    BOO_CHECK(uiMatchLanguage({"el-GR", "fr-CA"}, languages) == "fr-FR");
    BOO_CHECK(uiMatchLanguage({"en-GB", "de-DE"}, languages) == "en-US");
    BOO_CHECK(uiMatchLanguage({"el-GR"}, languages) == "en-US");
    return true;
}

} // namespace

#include "city_module_tests.hpp"
#include "flock_formation_tests.hpp"
#include "workout_governor_tests.hpp"
#include "startup_progress_tests.hpp"
#include "physx_suite_tests.hpp"
#include "last_light_tests.hpp"

int main() {
    boo::Logger::instance().setConsoleEnabled(false);

    runTest("SanctuaryParcelHandoff", testSanctuaryParcelHandoff);
    runTest("CityModuleLodTransitions", testCityModuleLodTransitions);
    runTest("CityModulesPreserveAuthoredKits", testCityModulesPreserveAuthoredKits);
    runTest("FlockFormations", testFlockFormations);
    runTest("WorkoutGovernor", testWorkoutGovernor);
    runTest("StartupProgress", testStartupProgress);
    runTest("ShadersRunOnEveryGpu", testShadersRunOnEveryGpu);
    runTest("PhysxSuite", testPhysxSuite);
    runTest("LastLight", testLastLight);
    runTest("TrilogyMeshAudioAndCamera",testTrilogyMeshAudioAndCamera);
    runTest("FilmOrbits", testFilmOrbits);
    runTest("MirrorWitnessFrustum", testMirrorWitnessFrustum);
    runTest("DawnEgretFramingAndClearance", testDawnEgretFramingAndClearance);
    runTest("AircraftHeroFraming",testAircraftHeroFraming);
    runTest("ShowVisualModeRoutingAndEnamelMicrocosmKit", testShowVisualModeRoutingAndEnamelMicrocosmKit);
    runTest("MadameNModelManifestAndAsyncFallback", testMadameNModelManifestAndAsyncFallback);
    runTest("FullFilm360sScoreCaptionsAndEditions", testFullFilm360sScoreCaptionsAndEditions);
    runTest("LateFilmEditionSeekAudioContinuity", testLateFilmEditionSeekAudioContinuity);
    runTest("MediaPrivacyRedactionAndFaultFallback", testMediaPrivacyRedactionAndFaultFallback);
    runTest("CustomAudioTrackDecoderAndCrossfade", testCustomAudioTrackDecoderAndCrossfade);
    runTest("RecordedSoundtrackFidelity", testRecordedSoundtrackFidelity);
    runTest("JsonParserAndSerializer", testJsonParserAndSerializer);
    runTest("PublicWorldLocators", testPublicWorldLocators);
    runTest("ProductionCueSheetsAndManifest", testProductionCueSheetsAndManifest);
    runTest("ManifestProvenanceAndByteIntegrity", testManifestProvenanceAndByteIntegrity);
    runTest("InvalidCueFixturesRejected", testInvalidCueFixturesRejected);
    runTest("TimelineClockArithmeticAndFailover", testTimelineClockArithmeticAndFailover);
    runTest("PrecisionTransport", testPrecisionTransport);
    runTest("QualityLadderHysteresisAndVramPressure", testQualityLadderHysteresisAndVramPressure);
    runTest("LiteStartupAndLadder", testLiteStartupAndLadder);
    runTest("FallbackOrchestratorFaultIsolation", testFallbackOrchestratorFaultIsolation);
    runTest("ScriptedDirectorDeterminismAndBounds", testScriptedDirectorDeterminismAndBounds);
    runTest("SettingsAndPathRedaction", testSettingsAndPathRedaction);
    runTest("M1CameraLetterboxAndFlashSafety", testM1CameraLetterboxAndFlashSafety);
    runTest("AudioClicklessCrossfadeAndGainRamps", testAudioClicklessCrossfadeAndGainRamps);
    runTest("AudioReactiveFollowerBeatAndFlashSafety", testAudioReactiveFollowerBeatAndFlashSafety);
    runTest("TimelineClockSwitchAndTaipei101FinaleFraming", testTimelineClockSwitchAndTaipei101FinaleFraming);
    runTest("UiTextLanguages", testUiTextLanguages);
    runTest("UiStringCatalogs", testUiStringCatalogs);

    std::cout << "\nUnit Test Summary: " << (gTestsRun - gTestsFailed) << "/" << gTestsRun << " passed.\n";
    return gTestsFailed == 0 ? 0 : 1;
}
