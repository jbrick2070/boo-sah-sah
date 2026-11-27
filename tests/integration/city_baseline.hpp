// Included by integration_main.cpp after film_capture_gate.hpp for the verified WIC
// PNG writer and deterministic film context. This suite never shows its swapchain HWND.
bool gCityForceRaster = false;
bool gWholeShowBaseline = false;
bool gBaseline4K = false;

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
        if (gBaseline4K) ctx.qualityPreset = QualityPresetId::Experimental4K;
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
    const auto measureStart=std::chrono::steady_clock::now();
    for (int i = 0; retired < count; ++i) {
        if(i==count) std::printf("seek %s submit FPS=%.3f\n",shot.name,double(count)/std::chrono::duration<double>(std::chrono::steady_clock::now()-measureStart).count());
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
        if (gCityForceRaster && kind == EngineKind::RT) BOO_CHECK(result.status == BackendStatus::Substituted);
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
        if(GetEnvironmentVariableW(L"BOO_SEEK_DRAIN",nullptr,0) || GetEnvironmentVariableW(L"BOO_SEEK_DRAIN_FIXED",nullptr,0)) BOO_CHECK(d3d.waitForGpu());
        fx.resetTo(ctx.filmTimeSeconds);
        BOO_CHECK(d3d.beginFrame() && present(ctx));
        uint32_t w = 0, h = 0;
        BOO_CHECK(fx.readLastRenderedCanvasForTest(rgba, w, h, &error, &rgb10));
        BOO_CHECK(w == fx.canvasWidth() && h == fx.canvasHeight());
        return true;
    };
    std::vector<uint32_t> reference, replay;
    std::vector<uint8_t> rgba;
    auto captureHdr = [&](const char* label) {
        if (!GetEnvironmentVariableW(L"BOO_REPLAY_GARDEN", nullptr, 0)) return true;
        auto path=directory / (std::string(shot.name)+"-"+label+".fp16");
        SetEnvironmentVariableW(L"BOO_REPLAY_HDR_FILE",path.c_str());
        SetEnvironmentVariableA("BOO_SEEK_LABEL",label);
        uint64_t invalid=0; bool ok=fx.readSceneHdrFiniteForTest(invalid,&error) && invalid==0;
        SetEnvironmentVariableA("BOO_SEEK_LABEL",nullptr);
        SetEnvironmentVariableW(L"BOO_REPLAY_HDR_FILE",nullptr);
        return ok;
    };
    SetEnvironmentVariableA("BOO_SEEK_LABEL","first");
    BOO_CHECK(captureAnchor(rgba, reference));
    SetEnvironmentVariableA("BOO_SEEK_LABEL",nullptr);
    BOO_CHECK(captureHdr("first"));
    fx.resetTo(context(-warmup).filmTimeSeconds);
    BOO_CHECK(d3d.beginFrame() && present(context(-warmup)));
    SetEnvironmentVariableA("BOO_SEEK_LABEL","second");
    BOO_CHECK(captureAnchor(rgba, replay) && reference.size() == replay.size());
    SetEnvironmentVariableA("BOO_SEEK_LABEL",nullptr);
    BOO_CHECK(captureHdr("second"));
    if(GetEnvironmentVariableW(L"BOO_SEEK_EARLY_MATRIX",nullptr,0)) {
        SetEnvironmentVariableA("BOO_SEEK_DRAIN_FIXED","1");
        for (int attempt=0;attempt<3;++attempt) {
            std::vector<uint8_t> trialRgba;std::vector<uint32_t> trial;
            SetEnvironmentVariableA("BOO_SEEK_LABEL","locked");
            BOO_CHECK(captureAnchor(trialRgba,trial));
            SetEnvironmentVariableA("BOO_SEEK_LABEL",nullptr);
            std::printf("seek fixed bindings attempt=%d hash=%s exactFirst=%d exactReplay=%d\n",attempt,capturePixelHash(trial).c_str(),trial==reference,trial==replay);
        }
        SetEnvironmentVariableA("BOO_SEEK_DRAIN_FIXED",nullptr);
    }
    uint32_t changed = 0, maxCode = 0, x0 = fx.canvasWidth(), x1 = 0, y0 = fx.canvasHeight(), y1 = 0;
    for (size_t j = 0; j < replay.size(); ++j) {
        if (replay[j] == reference[j]) continue;
        uint32_t x = uint32_t(j % fx.canvasWidth()), y = uint32_t(j / fx.canvasWidth());
        ++changed;
        uint32_t err = 0;
        for (int off : {0, 10, 20}) err = std::max(err, uint32_t(std::abs(int(reference[j] >> off & 1023) - int(replay[j] >> off & 1023))));
        maxCode = std::max(maxCode, err);
        x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
        if (changed < 5) std::cout << shot.name << " (" << x << "," << y << ") " << std::hex << reference[j] << " -> " << replay[j] << std::dec << "\n";
    }
    entry["diagnostic_changed_pixels"] = uint64_t(changed);
    entry["diagnostic_max_channel_code_delta"] = uint64_t(maxCode);
    entry["diagnostic_replay_hash"] = capturePixelHash(replay);
    if (changed) std::cout << shot.name << " changed=" << changed << " maxCode=" << maxCode << " bbox=" << x0 << "," << y0 << ".." << x1 << "," << y1 << "\n" << std::flush;
    BOO_CHECK(!changed || (gBaseline4K && GetEnvironmentVariableW(L"BOO_INSPECT_OLD_4K", nullptr, 0)));
    const auto stats = fx.lastCityDrawStats();
    BOO_CHECK(stats.visibleCityPlacements <= stats.cityPlacements);
    BOO_CHECK(stats.cityDrawBatches <= stats.visibleCityInstances);
    if (gCityForceRaster) BOO_CHECK(stats.rayInstances == 0);
    if (std::string_view(shot.sceneId) == "signal_hijack" || std::string_view(shot.sceneId) == "taipei_sanctuary") {
        BOO_CHECK(stats.cityDrawBatches > 0 && stats.visibleCityInstances >= 5u * stats.cityDrawBatches);
    }
    auto& city = entry["geometry"];
    city["active_instances"] = uint64_t(stats.activeInstances);
    city["city_placements"] = uint64_t(stats.cityPlacements);
    city["visible_city_placements"] = uint64_t(stats.visibleCityPlacements);
    city["visible_city_instances"] = uint64_t(stats.visibleCityInstances);
    city["city_color_draws"] = uint64_t(stats.cityDrawBatches);
    city["color_draws"] = uint64_t(stats.rasterBatches);
    city["ray_instances"] = uint64_t(stats.rayInstances);
    city["module_blases"] = uint64_t(stats.templateBlases);
    city["blas_bytes"] = stats.blasBytes;
    city["tlas_bytes"] = stats.tlasBytes;
    city["lod_placements"] = JsonValue::Array{};
    for (const auto countAtLod : stats.lodPlacements) {
        city["lod_placements"].asArrayMut().emplace_back(uint64_t(countAtLod));
    }
    uint64_t nonFinite = 0;
    BOO_CHECK(fx.readSceneHdrFiniteForTest(nonFinite, &error) && nonFinite == 0);
    const auto luma = computeRgba8LumaMetrics(rgba);
    BOO_CHECK(luma.meanLuma > 0.008 && luma.p99Luma - luma.p10Luma > 0.04);
    const auto filename = std::string(shot.name) + ".png";
    BOO_CHECK(writeCapturePng(directory / filename, rgba, fx.canvasWidth(), fx.canvasHeight()));

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
    if (gRequireDebugLayer) {
        BOO_CHECK(Logger::instance().openLogFile(directory / "d3d12-validation.log"));
    }
    JsonValue report(JsonValue::Object{});
    report["schema"] = 1;
    report["complete"] = false;
    const auto manifest = directory / "manifest.json";
    BOO_CHECK(writeFilmGateJson(manifest, report));

    const auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    const auto spec = findQualityPresetSpec(presets, gBaseline4K ? QualityPresetId::Experimental4K : QualityPresetId::Showcase);
    BOO_CHECK(spec.renderWidth == (gBaseline4K ? 3840u : 2560u) && spec.renderHeight == (gBaseline4K ? 2160u : 1440u));
    struct HiddenWindow {
        HWND hwnd;
        HiddenWindow(int width, int height) : hwnd(CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"",
                                    WS_POPUP, 0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)) {}
        ~HiddenWindow() { if (hwnd) DestroyWindow(hwnd); }
    } window(spec.renderWidth, spec.renderHeight);
    BOO_CHECK(window.hwnd && !IsWindowVisible(window.hwnd));
    D3D12Context d3d;
    std::string error;
    BOO_CHECK(d3d.initialize(window.hwnd, spec.renderWidth, spec.renderHeight, false, gRequireDebugLayer, &error));
    if(GetEnvironmentVariableW(L"BOO_SEEK_DRIVER",nullptr,0)) {
        Microsoft::WRL::ComPtr<ID3D12Device6> dev6;
        BOO_CHECK(SUCCEEDED(static_cast<ID3D12Device*>(d3d.deviceRaw())->QueryInterface(IID_PPV_ARGS(&dev6))));
        const auto res=dev6->SetBackgroundProcessingMode(D3D12_BACKGROUND_PROCESSING_MODE_DISABLE_PROFILING_BY_SYSTEM,D3D12_MEASUREMENTS_ACTION_DISCARD_PREVIOUS,nullptr,nullptr);
        std::printf("seek lock driver optimization 0x%08lx\n",res);
    }
    BOO_CHECK(!d3d.isWarp() && (!gRequireDebugLayer || d3d.debugLayerActive()));

    report["adapter"] = d3d.adapterName();
    report["debug_layer"] = d3d.debugLayerActive();
    report["preset"] = gBaseline4K ? "4K Experimental" : "Showcase";
    report["edition"] = "Family";
    report["sample_rate"] = 48000;
    report["width"] = int(spec.renderWidth);
    report["height"] = int(spec.renderHeight);
    report["render_path"] = gCityForceRaster ? "forced raster" : "preferred";
    report["controls"] = "Family, clean canvas, sample-derived ScriptedDirector and analytical audio";
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
    Sequence sequences[] = {
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
    if (gWholeShowBaseline) {
        auto& filmShots = sequences[0].shots;
        filmShots.insert(filmShots.end(), {
            {95, "mirror_intelligence", "film_mirror_95s"},
            {145, "neural_velvet", "film_neural_145s"},
            {225, "particle_mutiny", "film_particle_225s"}
        });
        std::sort(filmShots.begin(), filmShots.end(), [](const auto& a, const auto& b) { return a.seconds < b.seconds; });
    }
    for (const auto& sequence : sequences) {
        CueSheet cue;
        ValidationReport validation;
        const auto cuePath = std::string("assets/cues/") + sequence.file + ".json";
        if(GetEnvironmentVariableW(L"BOO_SEEK_PICK",nullptr,0) && std::string_view(sequence.file)!="film_360s") continue;
        BOO_CHECK(loadCueSheetFromFile(resolveResourcePath(cuePath), cue, validation) && validation.valid);
        M1TaipeiSliceEffect fx(d3d, spec, false, false, false, false, false, false, false, false, resolveShowVisualMode(cue));
        if (gCityForceRaster) fx.setDisableRt(true);
        BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);
        BOO_CHECK(fx.canvasWidth() == spec.renderWidth && fx.canvasHeight() == spec.renderHeight);
        BOO_CHECK(fx.hasAllAuthoredSceneAssets());
        for (const auto& shot : sequence.shots) {
            char which[16]{};
            if(GetEnvironmentVariableA("BOO_SEEK_PICK",which,sizeof(which)) && (shot.seconds!=std::atoi(which) || std::string_view(sequence.file)!="film_360s")) continue;
            if (GetEnvironmentVariableW(L"BOO_REPLAY_GARDEN", nullptr, 0) &&
                (std::string_view(sequence.file) != "film_360s" || (shot.seconds != 168 && shot.seconds != 176 && shot.seconds != 318))) continue;
            JsonValue entry(JsonValue::Object{});
            if(GetEnvironmentVariableW(L"BOO_SEEK_MATRIX",nullptr,0)) {
                const char* normal[]={"control","BOO_SEEK_FROZEN_TLAS","BOO_SEEK_NO_SHAFT","BOO_SEEK_NO_SPATIAL","BOO_SEEK_DRAIN","control-repeat"};
                const char* early[]={"control-1","BOO_SEEK_EARLY","control-2","BOO_SEEK_EARLY","control-3","BOO_SEEK_EARLY"};
                const auto& conditions=GetEnvironmentVariableA("BOO_SEEK_EARLY_MATRIX",nullptr,0)?early:normal;
                int run=0;
                for(const char* name:conditions) {
                    const bool hasFlag=name[0]=='B';
                    if(hasFlag) SetEnvironmentVariableA(name,"1");
                    const std::string suffix=std::string(shot.name)+"-"+name+"-"+std::to_string(++run);
                    CityBaselineShot caseShot=shot;caseShot.name=suffix.c_str();
                    std::printf("\nCASE %s\n",name);
                    BOO_CHECK(captureCityBaselineShot(d3d,fx,cue,cuePath,caseShot,directory,summary,samples,entry));
                    if(hasFlag) SetEnvironmentVariableA(name,nullptr);
                    report["frames"].asArrayMut().push_back(std::move(entry));
                    entry=JsonValue(JsonValue::Object{});
                }
                BOO_CHECK(writeFilmGateJson(manifest,report));continue;
            }
            BOO_CHECK(captureCityBaselineShot(d3d, fx, cue, cuePath, shot, directory, summary, samples, entry));
            report["frames"].asArrayMut().push_back(std::move(entry));
            BOO_CHECK(writeFilmGateJson(manifest, report));
        }
        BOO_CHECK(d3d.waitForGpu() && d3d.checkDeviceHealthy());
    }
    summary.close();
    samples.close();
    BOO_CHECK(summary.good() && samples.good());
    if (d3d.debugLayerActive()) {
        const auto errors = d3d.debugErrorCount();
        report["validation_errors"] = uint64_t(errors);
        report["validation_warnings"] = uint64_t(d3d.debugWarningCount());
        BOO_CHECK(writeFilmGateJson(manifest, report));
        BOO_CHECK(errors == 0);
    }
    report["complete"] = true;
    return writeFilmGateJson(manifest, report);
}
