// Included by integration_main.cpp after film_capture_gate.hpp for the verified WIC
// PNG writer and deterministic film context. This suite never shows its swapchain HWND.
struct CityBaselineShot {
    int seconds;
    const char* sceneId;
    const char* name;
};

bool captureCityBaselineShot(boo::D3D12Context& d3d, boo::M1TaipeiSliceEffect& fx,
                             const boo::CueSheet& cue, const std::string& cuePath, const CityBaselineShot& shot,
                             const std::filesystem::path& directory, std::ostream& summary,
                             std::ostream& samples, boo::JsonValue& entry) {
    using namespace boo;
    constexpr int warmup = 24;
    constexpr int count = 60;
    const int64_t anchor = int64_t(shot.seconds) * cue.sampleRate;
    const int64_t step = cue.sampleRate / 60;
    BOO_CHECK(cue.sampleRate == 48000 && anchor + count * step < cue.durationSamples);
    const auto& scene = cue.scenes.at(findSceneIndexAtSample(cue, anchor));
    BOO_CHECK(scene.sceneId == shot.sceneId);
    BOO_CHECK(anchor - warmup * step >= scene.startSample && anchor + (count - 1) * step < scene.endSample);

    auto context = [&](int offset) {
        auto ctx = makeFilmGateContext(cue, anchor + offset * step, Edition::Family);
        ctx.presentationFrame = uint64_t(ctx.samplePosition / step);
        return ctx;
    };
    auto present = [&](const FrameContext& ctx) {
        fx.render(ctx);
        double ignored = 0.0;
        return d3d.endFrameAndPresent(false, ignored);
    };
    std::string error;
    fx.resetTo(context(-warmup).filmTimeSeconds);
    for (int i = -warmup; i < 0; ++i) {
        BOO_CHECK(d3d.beginFrame() && present(context(i)));
    }

    // beginFrame retires the previous submission for its current backbuffer slot.
    // Associate each timing with that submission even when DXGI reuses a slot.
    std::array<int, D3D12Context::kFrameCount> pending;
    pending.fill(-1);
    std::array<double, count> gpuMs{};
    uint64_t peakVram = 0;
    int retired = 0;
    for (int i = 0; retired < count; ++i) {
        BOO_CHECK(i < count + 60 && d3d.beginFrame());
        const auto slot = d3d.currentBackBufferIndex();
        const int frame = pending.at(slot);
        const uint64_t vram = d3d.queryEstimatedAppVramBytes();
        BOO_CHECK(vram > 0);
        if (frame >= 0) {
            const auto timing = d3d.lastPassTimings();
            BOO_CHECK(std::isfinite(timing.totalMs) && timing.totalMs > 0 && timing.sceneMs > 0);
            gpuMs.at(frame) = timing.totalMs;
            peakVram = std::max(peakVram, vram);
            samples << shot.name << ',' << frame << ',' << anchor + frame * step << ','
                    << timing.totalMs << ',' << timing.sceneMs << ',' << timing.bloomMs << ','
                    << timing.compositeMs << ',' << timing.presentPassMs << ',' << vram << '\n';
            ++retired;
        }
        pending[slot] = i < count ? i : -1;
        BOO_CHECK(present(context(i)));
    }

    // Record backend status at the end of the timing window, before the reference
    // seeks clear transient state such as the in-flight signal-hijack codec ring.
    for (const auto kind : {EngineKind::RT, EngineKind::CUDA, EngineKind::Tensor, EngineKind::Codec}) {
        HardwareResult result;
        BOO_CHECK(fx.queryLiveEngineStatus(kind, result));
        auto& backend = entry["backends"][toString(kind)];
        backend["status"] = toString(result.status);
        backend["backend"] = result.activeBackend;
        backend["verified"] = result.successfullyExecutedTestWork;
        backend["diagnostic"] = result.diagnosticCode;
    }

    // Compare the post-seek live CanvasClean with an independently re-entered
    // reference at the same sample. Capture/readback never overlaps the measured window.
    auto captureAnchor = [&](std::vector<uint8_t>& rgba, std::vector<uint32_t>& rgb10) {
        const auto ctx = context(0);
        fx.resetTo(ctx.filmTimeSeconds);
        BOO_CHECK(d3d.beginFrame() && present(ctx));
        uint32_t w = 0, h = 0;
        BOO_CHECK(fx.readLastRenderedCanvasForTest(rgba, w, h, &error, &rgb10));
        BOO_CHECK(w == 2560 && h == 1440);
        return true;
    };
    std::vector<uint32_t> reference, replay;
    std::vector<uint8_t> rgba;
    BOO_CHECK(captureAnchor(rgba, reference));
    fx.resetTo(context(-warmup).filmTimeSeconds);
    BOO_CHECK(d3d.beginFrame() && present(context(-warmup)));
    BOO_CHECK(captureAnchor(rgba, replay) && reference == replay);
    uint64_t nonFinite = 0;
    BOO_CHECK(fx.readSceneHdrFiniteForTest(nonFinite, &error) && nonFinite == 0);
    const auto luma = computeRgba8LumaMetrics(rgba);
    BOO_CHECK(luma.meanLuma > 0.008 && luma.p99Luma - luma.p10Luma > 0.04);
    const auto filename = std::string(shot.name) + ".png";
    BOO_CHECK(writeCapturePng(directory / filename, rgba, 2560, 1440));

    std::sort(gpuMs.begin(), gpuMs.end());
    const double p50 = gpuMs.at(29), p95 = gpuMs.at(56); // Nearest rank over 60 samples.
    summary << shot.name << ',' << cuePath << ',' << cue.filmId << ',' << shot.sceneId << ',' << shot.seconds << ','
            << anchor << ',' << count << ',' << p50 << ',' << p95 << ',' << peakVram << '\n';
    entry["png"] = filename;
    entry["cue"] = cuePath;
    entry["film_id"] = cue.filmId;
    entry["cue_seed"] = std::to_string(cue.seed);
    entry["scene_seed"] = std::to_string(scene.deterministicSeed);
    entry["scene"] = shot.sceneId;
    entry["seconds"] = shot.seconds;
    entry["sample"] = anchor;
    entry["rgb10_fnv1a64"] = capturePixelHash(reference);
    entry["peak_process_local_bytes"] = peakVram;
    entry["gpu_p50_ms"] = p50;
    entry["gpu_p95_ms"] = p95;
    std::cout << shot.name << " GPU p50=" << p50 << " ms, p95=" << p95
              << " ms, peak local=" << peakVram << " bytes\n" << std::flush;
    return true;
}

bool testCityBaseline() {
    using namespace boo;
    CaptureComScope com;
    BOO_CHECK(SUCCEEDED(com.result) || com.result == RPC_E_CHANGED_MODE);
    const auto directory = (gFilmCaptureDirectory == "build/film_captures")
        ? std::filesystem::path("build/city_baseline") : gFilmCaptureDirectory;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    BOO_CHECK(!ec);
    JsonValue report(JsonValue::Object{});
    report["schema"] = 1;
    report["complete"] = false;
    const auto manifest = directory / "manifest.json";
    BOO_CHECK(writeFilmGateJson(manifest, report));

    struct HiddenWindow {
        HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"",
                                    WS_POPUP, 0, 0, 2560, 1440, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~HiddenWindow() { if (hwnd) DestroyWindow(hwnd); }
    } window;
    BOO_CHECK(window.hwnd && !IsWindowVisible(window.hwnd));
    D3D12Context d3d;
    std::string error;
    BOO_CHECK(d3d.initialize(window.hwnd, 2560, 1440, false, gRequireDebugLayer, &error));
    BOO_CHECK(!d3d.isWarp() && (!gRequireDebugLayer || d3d.debugLayerActive()));
    const auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    const auto spec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    BOO_CHECK(spec.renderWidth == 2560 && spec.renderHeight == 1440);

    report["adapter"] = d3d.adapterName();
    report["debug_layer"] = d3d.debugLayerActive();
    report["preset"] = "Showcase";
    report["edition"] = "Family";
    report["sample_rate"] = 48000;
    report["width"] = 2560;
    report["height"] = 1440;
    report["controls"] = "Family, clean canvas, sample-derived ScriptedDirector and analytical audio, default preferred graphics paths";
    report["capture"] = "HUD off, no captions or audio-reactive input; RGB10A2 CanvasClean compared word for word after a backward seek; lossless WIC RGB8 PNG of the 8-bit conversion";
    report["timing"] = "D3D12 direct queue, clear through presentation draw, excluding DXGI Present wait, external compute queues and readback";
    report["window"] = "24 preroll frames then 60 consecutive samples at 60 Hz from each anchor; nearest-rank percentiles";
    report["vram"] = "Maximum sampled DXGI local-segment CurrentUsage for this process at measured frame retirement, bytes";
    report["frames"] = JsonValue::Array{};
    std::ofstream summary(directory / "telemetry.csv", std::ios::binary);
    std::ofstream samples(directory / "samples.csv", std::ios::binary);
    BOO_CHECK(summary && samples);
    summary << std::fixed << std::setprecision(6)
            << "shot,cue,film_id,scene,seconds,sample,frame_count,gpu_p50_ms,gpu_p95_ms,peak_process_local_bytes\n";
    samples << std::fixed << std::setprecision(6)
            << "shot,frame,sample,gpu_ms,scene_ms,bloom_ms,composite_ms,presentation_pass_ms,process_local_bytes\n";

    struct Sequence { const char* file; std::vector<CityBaselineShot> shots; };
    const Sequence sequences[] = {
        {"film_360s", {{8, "taipei_intro", "film_taipei_8s"},
                       {168, "madame_n", "film_sanctuary_168s"},
                       {176, "madame_n", "film_sanctuary_176s"},
                       {258, "signal_hijack", "film_shenzhen_258s"},
                       {318, "silicon_rebellion", "film_orchid_finale_318s"},
                       {352, "silicon_rebellion", "film_dawn_352s"}}},
        {"m2_trilogy_180s", {{15, "shanghai_trestle", "trilogy_shanghai_15s"},
                             {168, "taipei_sanctuary", "trilogy_taipei_168s"},
                             {176, "taipei_sanctuary", "trilogy_taipei_176s"}}}
    };
    for (const auto& sequence : sequences) {
        CueSheet cue;
        ValidationReport validation;
        const auto cuePath = std::string("assets/cues/") + sequence.file + ".json";
        BOO_CHECK(loadCueSheetFromFile(resolveResourcePath(cuePath), cue, validation) && validation.valid);
        M1TaipeiSliceEffect fx(d3d, spec, false, false, false, false, false, false, false, false, resolveShowVisualMode(cue));
        BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);
        BOO_CHECK(fx.canvasWidth() == spec.renderWidth && fx.canvasHeight() == spec.renderHeight);
        BOO_CHECK(fx.hasAllAuthoredSceneAssets());
        for (const auto& shot : sequence.shots) {
            JsonValue entry(JsonValue::Object{});
            BOO_CHECK(captureCityBaselineShot(d3d, fx, cue, cuePath, shot, directory, summary, samples, entry));
            report["frames"].asArrayMut().push_back(std::move(entry));
            BOO_CHECK(writeFilmGateJson(manifest, report));
        }
        BOO_CHECK(d3d.waitForGpu() && d3d.checkDeviceHealthy());
    }
    summary.close();
    samples.close();
    BOO_CHECK(summary.good() && samples.good() && d3d.debugErrorCount() == 0);
    report["complete"] = true;
    return writeFilmGateJson(manifest, report);
}
