// Included by integration_main.cpp after film_capture_gate.hpp for the verified WIC
// PNG writer and deterministic film context. This suite never shows its swapchain HWND.
bool gCityForceRaster = false;
bool gRoomBaseline = false;
bool gWholeShowBaseline = false;
bool gBaseline4K = false;
bool gGraphicsFeatures = false;
bool gRequireNgx = false;

struct CityBaselineShot {
    int seconds;
    const char* sceneId;
    const char* name;
    bool oblique = false;
    boo::PathTracingLevel pathTracing = boo::PathTracingLevel::Low;
    boo::DlssMode dlss = boo::DlssMode::Quality;
    bool rayReconstruction = true;
    bool disableRt = false;
};

const char* rendererFeatureLabel(boo::RendererFeatureState state) {
    using enum boo::RendererFeatureState;
    switch (state) {
        case Unavailable: return "unavailable";
        case Off: return "off";
        case Active: return "active";
        case Idle: return "idle";
    }
    return "unknown";
}

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
        ctx.pathTracing = shot.pathTracing;
        ctx.dlssMode = shot.dlss;
        ctx.rayReconstruction = shot.rayReconstruction;
        if (gGraphicsFeatures) {
            ctx.frameGeneration = FrameGenerationMode::Off;
            ctx.reflex = ReflexMode::Off;
        }
        if (gBaseline4K) ctx.qualityPreset = QualityPresetId::Experimental4K;
        if (gRoomBaseline) {
            auto& view = ctx.interactive;
            // Pin the camera without submitting Explore motion to the temporal denoiser.
            // Hardware interactive history is exercised by the dedicated InteractiveCaptures suite.
            view.blendWeight = 1.0f;
            view.eye[0] = shot.oblique ? -1.50f : -0.45f;
            view.eye[1] = 0.16f;
            view.eye[2] = shot.oblique ? 18.30f : 18.64f;
            view.target[0] = shot.oblique ? -0.22f : -0.30f;
            view.target[1] = shot.oblique ? 0.35f : 0.15f;
            view.target[2] = shot.oblique ? 20.55f : 20.245f;
            view.fovYRadians = 0.67f;
        }
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
        if (kind == EngineKind::RT && isOrbitalCutaway(resolveShowVisualMode(cue), context(0)))
            BOO_CHECK(result.diagnosticCode == "ORBITAL_SKY_NO_DXR_WORK");
        if (gCityForceRaster && kind == EngineKind::RT) BOO_CHECK(result.status == BackendStatus::Substituted);
        auto& backend = entry["backends"][toString(kind)];
        backend["status"] = toString(result.status);
        backend["backend"] = result.activeBackend;
        backend["verified"] = result.successfullyExecutedTestWork;
        backend["diagnostic"] = result.diagnosticCode;
    }

    // A reset reference is intentionally spatial. Replay a continuous preroll
    // ending on the identical audio sample to capture the settled temporal output.
    fx.resetTo(context(-warmup).filmTimeSeconds);
    for (int i = -warmup; i <= 0; ++i) {
        BOO_CHECK(d3d.beginFrame() && present(context(i)));
    }
    std::vector<uint32_t> settled;
    std::vector<uint8_t> settledRgba;
    uint32_t settledW=0, settledH=0;
    BOO_CHECK(fx.readLastRenderedCanvasForTest(settledRgba,settledW,settledH,&error,&settled));
    BOO_CHECK(settledW==fx.canvasWidth() && settledH==fx.canvasHeight());
    const std::string settledName=std::string(shot.name)+"-settled";
    const auto writeWords = [&](const std::string& name,const std::vector<uint32_t>& words) {
        std::ofstream file(directory / (name+".rgb10"),std::ios::binary);
        file.write(reinterpret_cast<const char*>(words.data()),std::streamsize(words.size()*sizeof(uint32_t)));
        file.close();
        return file.good();
    };
    BOO_CHECK(writeCapturePng(directory/(settledName+".png"),settledRgba,settledW,settledH));
    BOO_CHECK(writeWords(settledName,settled));
    const auto settledLuma=computeRgba8LumaMetrics(settledRgba);
    BOO_CHECK(settledLuma.meanLuma>.008 && settledLuma.p99Luma-settledLuma.p10Luma>.04);
    entry["shot"] = shot.name;
    entry["settled_png"] = settledName+".png";
    entry["settled_rgb10"] = settledName+".rgb10";
    entry["settled_rgb10_fnv1a64"] = capturePixelHash(settled);
    entry["settled_mean_luma"] = settledLuma.meanLuma;
    entry["path_tracing_selection"] = int(shot.pathTracing);
    entry["dlss_selection"] = int(shot.dlss);
    entry["ray_reconstruction_requested"] = shot.rayReconstruction;
    entry["rt_disabled"] = gCityForceRaster || shot.disableRt;
    entry["path_tracing_state"] = rendererFeatureLabel(fx.pathTracingState());
    entry["dlss_state"] = rendererFeatureLabel(fx.dlssState());
    entry["ray_reconstruction_state"] = rendererFeatureLabel(fx.rayReconstructionState());
    entry["dlss_input_width"] = static_cast<int>(fx.dlssInputWidth());
    entry["dlss_input_height"] = static_cast<int>(fx.dlssInputHeight());
    if (fx.dlssState() == RendererFeatureState::Active) {
        BOO_CHECK(shot.dlss != DlssMode::Off);
        BOO_CHECK(fx.dlssInputWidth() > 0 && fx.dlssInputWidth() < fx.canvasWidth());
        BOO_CHECK(fx.dlssInputHeight() > 0 && fx.dlssInputHeight() < fx.canvasHeight());
    }
    BOO_CHECK(shot.pathTracing != PathTracingLevel::Off || fx.pathTracingState() != RendererFeatureState::Active);
    if (gCityForceRaster) BOO_CHECK(fx.pathTracingState() != RendererFeatureState::Active);

    // Compare the post-seek live CanvasClean with an independently re-entered
    // reference at the same sample. Capture/readback never overlaps the measured window.
    auto captureAnchor = [&](std::vector<uint8_t>& rgba, std::vector<uint32_t>& rgb10) {
        const auto ctx = context(0);
        fx.resetTo(ctx.filmTimeSeconds);
        BOO_CHECK(d3d.beginFrame() && present(ctx));
        uint32_t w = 0, h = 0;
        BOO_CHECK(fx.readLastRenderedCanvasForTest(rgba, w, h, &error, &rgb10));
        BOO_CHECK(w == fx.canvasWidth() && h == fx.canvasHeight());
        return true;
    };
    std::vector<uint32_t> reference, replay;
    std::vector<uint8_t> rgba;
    BOO_CHECK(captureAnchor(rgba, reference));
    entry["reset_dlss_state"] = rendererFeatureLabel(fx.dlssState());
    entry["reset_ray_reconstruction_state"] = rendererFeatureLabel(fx.rayReconstructionState());
    entry["reset_rgb10"] = std::string(shot.name)+".rgb10";
    BOO_CHECK(writeWords(shot.name,reference));
    std::vector<uint8_t> replayRgba;
    fx.resetTo(context(-warmup).filmTimeSeconds);
    BOO_CHECK(d3d.beginFrame() && present(context(-warmup)));
    BOO_CHECK(captureAnchor(replayRgba, replay) && replay.size() == reference.size());
    uint32_t changedPixels = 0, maxCodeDelta = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
        if (reference[i] == replay[i]) continue;
        ++changedPixels;
        for (unsigned shift : {0u, 10u, 20u, 30u}) {
            const int a = int((reference[i] >> shift) & 1023u), b = int((replay[i] >> shift) & 1023u);
            maxCodeDelta = std::max(maxCodeDelta, uint32_t(std::abs(a - b)));
        }
    }
    entry["seek_repeat_equal"] = changedPixels == 0;
    entry["seek_changed_pixels"] = uint64_t(changedPixels);
    entry["seek_max_code_delta"] = uint64_t(maxCodeDelta);
    entry["replay_rgb10_fnv1a64"] = capturePixelHash(replay);
    const auto stats = fx.lastCityDrawStats();
    if (isOrbitalCutaway(resolveShowVisualMode(cue), context(0)))
        BOO_CHECK(stats.activeInstances == 0 && stats.rayInstances == 0 && stats.rasterBatches == 0);
    BOO_CHECK(stats.visibleCityPlacements <= stats.cityPlacements);
    BOO_CHECK(stats.cityDrawBatches <= stats.visibleCityInstances);
    if (gCityForceRaster) BOO_CHECK(stats.rayInstances == 0);
    if (gRoomBaseline) {
        BOO_CHECK(stats.lodPlacements[0] > 0 && stats.visibleCityInstances > 0);
    } else if (std::string_view(shot.sceneId) == "signal_hijack" || std::string_view(shot.sceneId) == "taipei_sanctuary") {
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
    BOO_CHECK(!d3d.isWarp() && (!gRequireDebugLayer || d3d.debugLayerActive()));

    report["driver_profiling_frozen"] = freezeDriverProfilingForTest(d3d);
    report["adapter"] = d3d.adapterName();
    report["debug_layer"] = d3d.debugLayerActive();
    report["preset"] = gBaseline4K ? "4K Experimental" : "Showcase";
    report["edition"] = "Family";
    report["sample_rate"] = 48000;
    report["width"] = int(spec.renderWidth);
    report["height"] = int(spec.renderHeight);
    report["render_path"] = gCityForceRaster ? "forced raster" : "preferred";
    report["controls"] = "Family, clean canvas, sample-derived ScriptedDirector and analytical audio";
    report["capture"] = "Clean CanvasClean at the anchor: packed little-endian R10G10B10A2 and WIC RGB8 PNG after an explicit reset and after 24 preceding 60 Hz frames. The reset capture is compared word for word after a backward seek.";
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
                       {86, "mirror_intelligence", "film_shanghai_river_86s"},
                       {176, "madame_n", "film_sanctuary_176s"},
                       {258, "signal_hijack", "film_shenzhen_258s"},
                       {298, "silicon_rebellion", "film_orbit_298s"},
                       {63, "mirror_intelligence", "film_orchid_orbit_63s"},
                       {103, "neural_velvet", "film_pool_103s"},
                       {134, "neural_velvet", "film_ribbon_134s"},
                       {235, "particle_mutiny", "film_kites_235s"},
                       {318, "silicon_rebellion", "film_taipei_crown_318s"},
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
    if (gRoomBaseline) {
        sequences[0].shots.clear();
        sequences[1].shots = {{176, "taipei_sanctuary", "arcade_front"}, {176, "taipei_sanctuary", "arcade_oblique", true}};
    }
    bool ngxRuntimePresent=false;
    if (gGraphicsFeatures) {
        using P = PathTracingLevel;
        using D = DlssMode;
        std::array<wchar_t,32768> exe{};
        BOO_CHECK(GetModuleFileNameW(nullptr,exe.data(),DWORD(exe.size())) > 0);
        const auto ngxDir=std::filesystem::path(exe.data()).parent_path()/"dlss";
        ngxRuntimePresent=std::filesystem::is_regular_file(ngxDir/"nvngx_dlss.dll");
        report["ngx_runtime_present"]=ngxRuntimePresent;
        sequences[1].shots.clear();
        sequences[0].shots = {
            {95, "mirror_intelligence", "native", false, P::Off, D::Off, false},
            {95, "mirror_intelligence", "sr_quality", false, P::Off, D::Quality, false},
            {95, "mirror_intelligence", "rr_quality", false, P::Off, D::Quality, true},
        };
        if (ngxRuntimePresent) {
            auto& matrix=sequences[0].shots;
            matrix.insert(matrix.end(),{
                {95, "mirror_intelligence", "pt_low", false, P::Low, D::Off, false},
                {95, "mirror_intelligence", "pt_medium", false, P::Medium, D::Off, false},
                {95, "mirror_intelligence", "pt_high", false, P::High, D::Off, false},
                {95, "mirror_intelligence", "sr_balanced", false, P::Off, D::Balanced, false},
                {95, "mirror_intelligence", "sr_performance", false, P::Off, D::Performance, false},
                {95, "mirror_intelligence", "combined", false, P::Low, D::Quality, false},
                {95, "mirror_intelligence", "combined_rr", false, P::Low, D::Quality, true},
                {95, "mirror_intelligence", "rr_without_sr", false, P::Off, D::Off, true},
                {95, "mirror_intelligence", "sr_raster", false, P::Off, D::Quality, false, true},
                {95, "mirror_intelligence", "rr_raster", false, P::Off, D::Quality, true, true},
            });
        }
    }
    bool allSeeksMatched = true;
    for (const auto& sequence : sequences) {
        if (sequence.shots.empty()) continue;
        CueSheet cue;
        ValidationReport validation;
        const auto cuePath = std::string("assets/cues/") + sequence.file + ".json";
        BOO_CHECK(loadCueSheetFromFile(resolveResourcePath(cuePath), cue, validation) && validation.valid);
        M1TaipeiSliceEffect fx(d3d, spec, false, false, false, false, false, false, false, false, resolveShowVisualMode(cue));
        if (gCityForceRaster) fx.setDisableRt(true);
        BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);
        BOO_CHECK(fx.canvasWidth() == spec.renderWidth && fx.canvasHeight() == spec.renderHeight);
        BOO_CHECK(fx.hasAllAuthoredSceneAssets());
        for (const auto& shot : sequence.shots) {
            if (gGraphicsFeatures) fx.setDisableRt(gCityForceRaster || shot.disableRt);
            JsonValue entry(JsonValue::Object{});
            BOO_CHECK(captureCityBaselineShot(d3d, fx, cue, cuePath, shot, directory, summary, samples, entry));
            allSeeksMatched = entry["seek_repeat_equal"].asBool(false) && allSeeksMatched;
            report["frames"].asArrayMut().push_back(std::move(entry));
            BOO_CHECK(writeFilmGateJson(manifest, report));
        }
        BOO_CHECK(d3d.waitForGpu() && d3d.checkDeviceHealthy());
    }
    summary.close();
    samples.close();
    BOO_CHECK(summary.good() && samples.good());
    if (gGraphicsFeatures) {
        const auto& frames=report["frames"].asArray();
        const auto& native=frames.front();
        const bool srSupported=native["dlss_state"].asString()=="off";
        const bool rrSupported=native["ray_reconstruction_state"].asString()=="off";
        const bool ptSupported=native["path_tracing_state"].asString()=="off";
        if (gRequireNgx) BOO_CHECK(ngxRuntimePresent && srSupported && rrSupported);
        if (!ngxRuntimePresent) BOO_CHECK(!srSupported && !rrSupported);
        for (const auto& frame:frames) {
            const bool requested=frame["dlss_selection"].asInt64()!=0;
            const bool requestRR=frame["ray_reconstruction_requested"].asBool();
            const bool rt=!frame["rt_disabled"].asBool();
            const auto expectedSR=srSupported ? (requested ? "active" : "off") : "unavailable";
            const auto expectedRR=!rrSupported ? "unavailable" : (!requestRR ? "off" : (requested && rt ? "active" : "idle"));
            BOO_CHECK(frame["dlss_state"].asString()==expectedSR);
            BOO_CHECK(frame["ray_reconstruction_state"].asString()==expectedRR);
            if (requested && srSupported) {
                BOO_CHECK(frame["settled_rgb10_fnv1a64"].asString()!=frame["rgb10_fnv1a64"].asString());
            }
            const bool pt=frame["path_tracing_selection"].asInt64()!=0;
            if (pt && ptSupported && rt) BOO_CHECK(frame["path_tracing_state"].asString()=="active");
        }
        // Identical requests on unsupported hardware must actually reach the same native image.
        // Conversely, two active reconstruction algorithms must produce distinct settled pixels.
        auto compare = [&](const std::string& a,const std::string& b,bool expectDifference) {
            std::ifstream first(directory/(a+"-settled.rgb10"),std::ios::binary);
            std::ifstream second(directory/(b+"-settled.rgb10"),std::ios::binary);
            BOO_CHECK(first && second);
            uint64_t changed=0,totalDelta=0;
            std::array<uint32_t,4096> left{},right{};
            const size_t pixels=size_t(spec.renderWidth)*spec.renderHeight;
            for (size_t i=0;i<pixels;i+=left.size()) {
                const auto n=std::min(left.size(),pixels-i);
                BOO_CHECK(first.read(reinterpret_cast<char*>(left.data()),n*4) && second.read(reinterpret_cast<char*>(right.data()),n*4));
                for (size_t j=0;j<n;++j) {
                    if (left[j]==right[j]) continue;
                    ++changed;
                    for (unsigned shift:{0u,10u,20u})
                        totalDelta+=std::abs(int((left[j]>>shift)&1023u)-int((right[j]>>shift)&1023u));
                }
            }
            auto& comparison=report["settled_comparisons"][a+"_vs_"+b];
            comparison["changed_pixels"]=changed;
            comparison["mean_rgb_code_delta"]=double(totalDelta)/(3.0*double(pixels));
            BOO_CHECK(writeFilmGateJson(manifest,report));
            if (expectDifference) BOO_CHECK(changed>pixels/1000 && totalDelta>pixels/20);
            else BOO_CHECK(changed==0);
            return true;
        };
        BOO_CHECK(compare("native","sr_quality",srSupported));
        BOO_CHECK(compare("sr_quality","rr_quality",srSupported && rrSupported && !gCityForceRaster));
        if (ngxRuntimePresent) {
            BOO_CHECK(compare("native","rr_without_sr",false));
            BOO_CHECK(compare("sr_raster","rr_raster",false));
            BOO_CHECK(compare("combined","combined_rr",srSupported && rrSupported && !gCityForceRaster));
        }
    }
    if (d3d.debugLayerActive()) {
        const auto errors = d3d.debugErrorCount();
        report["validation_errors"] = uint64_t(errors);
        report["validation_warnings"] = uint64_t(d3d.debugWarningCount());
        BOO_CHECK(writeFilmGateJson(manifest, report));
        BOO_CHECK(errors == 0);
    }
    report["complete"] = true;
    report["all_seeks_matched"] = allSeeksMatched;
    BOO_CHECK(writeFilmGateJson(manifest, report));
    BOO_CHECK(allSeeksMatched);
    return true;
}
