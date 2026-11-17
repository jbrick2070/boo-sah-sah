#include "boo/api.hpp"
#include "boo/audio.hpp"
#include "boo/audio_reactive.hpp"
#include "boo/bmesh.hpp"
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
#include "boo/ui_text.hpp"

#include <algorithm>
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

    // 1. Cold start ramp-up from 0.0 -> full 0.14f headroom gain (must start at 0 with zero click)
    BOO_CHECK(renderAndMeasure(1024u, false, false, 1.0f) == 1024u);
    BOO_CHECK(std::abs(synth.currentGain() - 0.14f) < 1e-4f);
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
    BOO_CHECK(std::abs(synth.currentGain() - 0.14f) < 1e-4f);
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

    for (const int64_t boundary : {1920000LL, 4320000LL, 6720000LL, 9360000LL, 11760000LL, 13200000LL}) {
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

    // 1. Verify C1 camera continuity across all 6 act boundaries of film_360s.json + reduced-motion dampening
    for (const int64_t boundary : {2160000LL, 4800000LL, 7200000LL, 9360000LL, 11760000LL, 13200000LL}) {
        const auto a = evaluateShowCamera(ShowVisualMode::FullFilm360s, boundary - 1, 48000, 0.5f, false, 1.0f);
        const auto b = evaluateShowCamera(ShowVisualMode::FullFilm360s, boundary, 48000, 0.5f, false, 1.0f);
        for (int i = 0; i < 3; ++i) {
            BOO_CHECK(std::abs(a.eye[i] - b.eye[i]) < 0.005f);
            BOO_CHECK(std::abs(a.target[i] - b.target[i]) < 0.005f);
        }
    }
    const auto camFullMotion = evaluateShowCamera(ShowVisualMode::FullFilm360s, 2820000, 48000, 0.85f, false, 1.0f);
    const auto camRedMotion  = evaluateShowCamera(ShowVisualMode::FullFilm360s, 2820000, 48000, 0.85f, false, 0.35f);
    BOO_CHECK(std::abs(camRedMotion.eye[0]) < std::abs(camFullMotion.eye[0]));

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

        BOO_CHECK(parseCommandLineArgs({"--no-captions", "--reduced-motion", "--enable-spout", "--spout-name", "BooStage_Clean"}, def));
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

bool testCustomAudioTrackDecoderAndCrossfade() {
    using namespace boo;

    // 1. CLI --audio-file parsing
    AppSettings cfg{};
    BOO_CHECK(parseCommandLineArgs({"--audio-file", "suno_orchid_demo.mp3"}, cfg));
    BOO_CHECK(cfg.audioInputPath == "suno_orchid_demo.mp3");

    // 2. Missing file & fault-injection privacy redaction
    CustomAudioTrack48k badTrack{};
    std::string errMsg;
    BOO_CHECK(!loadCustomAudioFile48k("C:\\Users\\secret_composer\\Music\\missing_suno.mp3", badTrack, &errMsg, false));
    BOO_CHECK(errMsg.find("secret_composer") == std::string::npos);
    BOO_CHECK(errMsg.find("<USER_HOME>") != std::string::npos);

    errMsg.clear();
    BOO_CHECK(!loadCustomAudioFile48k(resolveResourcePath("assets/manifest.json"), badTrack, &errMsg, true));
    BOO_CHECK(!errMsg.empty());

    // 3. Generate a temporary 44,100 Hz stereo 16-bit PCM WAV file (0.50s warm chord) and decode to 48,000 Hz stereo float
    const std::filesystem::path tmpWav = std::filesystem::temp_directory_path() / "boo_test_suno_44k1.wav";
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
        BOO_CHECK(custom.label == "boo_test_suno_44k1.wav");
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
    float prevL = 0.0f, prevR = 0.0f, maxStep = 0.0f;
    auto measure = [&](uint32_t frames, AudioBandSnapshot* outB = nullptr) -> bool {
        pcm.assign(static_cast<size_t>(frames) * 2u, 0.0f);
        synth.renderFrames(pcm.data(), frames, false, false, 0.85f, outB);
        for (uint32_t i = 0; i < frames; ++i) {
            const float l = pcm[i * 2u + 0u];
            const float r = pcm[i * 2u + 1u];
            BOO_CHECK(std::isfinite(l) && std::isfinite(r));
            maxStep = std::max(maxStep, std::max(std::abs(l - prevL), std::abs(r - prevR)));
            prevL = l;
            prevR = r;
        }
        return true;
    };

    BOO_CHECK(measure(1024u));
    synth.setCustomTrack(&custom);
    synth.seekTo(6000u);
    AudioBandSnapshot customBands{};
    BOO_CHECK(measure(1024u, &customBands));
    BOO_CHECK(customBands.rms > 0.02f);

    synth.setCustomTrack(nullptr);
    synth.seekTo(960000u);
    BOO_CHECK(measure(1024u));
    BOO_CHECK(maxStep < 0.020f);

    // 5. Verify bundled 4-track first-party OST assets, cooperative cancellation, compact PCM16, and playlist cycling
    const std::array<std::filesystem::path, AudioEngine::kBundledTrackCount> ostPaths = {
        resolveResourcePath("assets/audio/01_neon_orchid.mp3"),
        resolveResourcePath("assets/audio/02_acid_lotus.mp3"),
        resolveResourcePath("assets/audio/03_velvet_cipher.mp3"),
        resolveResourcePath("assets/audio/04_glass_dragon.mp3")
    };
    for (const auto& p : ostPaths) {
        BOO_CHECK(std::filesystem::exists(p));
        BOO_CHECK(std::filesystem::file_size(p) > (3ULL * 1024ULL * 1024ULL));
    }

    std::atomic<bool> preCancelled{true};
    CustomAudioTrack48k cancelledTrack{};
    BOO_CHECK(!loadCustomAudioFile48k(ostPaths[0], cancelledTrack, &errMsg, false, &preCancelled, true));
    BOO_CHECK(cancelledTrack.frameCount == 0u);

    CustomAudioTrack48k ost01{};
    if (loadCustomAudioFile48k(ostPaths[0], ost01, &errMsg, false, nullptr, true)) {
        BOO_CHECK(ost01.frameCount > (180ULL * 48000ULL)); // > 3 minutes @ 48 kHz
        BOO_CHECK(ost01.interleavedStereo.empty());
        BOO_CHECK(ost01.interleavedPcm16.size() == ost01.frameCount * 2u);
        synth.setCustomTrack(&ost01);
        synth.seekTo(48000ULL * 30ULL); // 0:30 into Neon Orchid
        AudioBandSnapshot ostBands{};
        BOO_CHECK(measure(1024u, &ostBands));
        BOO_CHECK(ostBands.rms > 0.01f);
        synth.setCustomTrack(nullptr);
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

bool testUiTextLanguages() {
    using boo::UiText;
    BOO_CHECK(boo::uiLanguageCount() == 25);
    BOO_CHECK(std::string(boo::uiLanguageTag(0)) == "en-US");
    for (size_t lang = 0; lang < boo::uiLanguageCount(); ++lang) {
        for (uint32_t id = 0; id < static_cast<uint32_t>(UiText::Count); ++id) {
            const std::wstring text = boo::uiTextAt(lang, static_cast<UiText>(id));
            BOO_CHECK(text.size() > (id == static_cast<uint32_t>(UiText::Loading) ? 2u : 8u));
            // Every translation of the first two strings keeps the product name; the box body keeps the API name.
            if (id != static_cast<uint32_t>(UiText::GraphicsFailureBody) && id != static_cast<uint32_t>(UiText::Loading)) BOO_CHECK(text.find(L"BOO SAH SAH") != std::wstring::npos);
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
    BOO_CHECK(keys.size() == 72);

    UiTextLayer layer;
    size_t problems = 0;
    for (const std::string& tag : languages) {
        if (tag == "en-US") continue;
        BOO_CHECK(catalog.load(dir, tag));
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
            if (key == "hud.live_gpu") ok = keeps(L"GPU");
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

int main() {
    boo::Logger::instance().setConsoleEnabled(false);

    runTest("TrilogyMeshAudioAndCamera",testTrilogyMeshAudioAndCamera);
    runTest("ShowVisualModeRoutingAndEnamelMicrocosmKit", testShowVisualModeRoutingAndEnamelMicrocosmKit);
    runTest("MadameNModelManifestAndAsyncFallback", testMadameNModelManifestAndAsyncFallback);
    runTest("FullFilm360sScoreCaptionsAndEditions", testFullFilm360sScoreCaptionsAndEditions);
    runTest("LateFilmEditionSeekAudioContinuity", testLateFilmEditionSeekAudioContinuity);
    runTest("MediaPrivacyRedactionAndFaultFallback", testMediaPrivacyRedactionAndFaultFallback);
    runTest("CustomAudioTrackDecoderAndCrossfade", testCustomAudioTrackDecoderAndCrossfade);
    runTest("JsonParserAndSerializer", testJsonParserAndSerializer);
    runTest("ProductionCueSheetsAndManifest", testProductionCueSheetsAndManifest);
    runTest("InvalidCueFixturesRejected", testInvalidCueFixturesRejected);
    runTest("TimelineClockArithmeticAndFailover", testTimelineClockArithmeticAndFailover);
    runTest("PrecisionTransport", testPrecisionTransport);
    runTest("QualityLadderHysteresisAndVramPressure", testQualityLadderHysteresisAndVramPressure);
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
