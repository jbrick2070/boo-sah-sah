// Long-running opt-in hardware gate; kept out of the ordinary short smoke suite.
uint32_t gFilmStressIterations = 224;

struct FilmStressMemory {
    uint64_t workingSet = 0, privateBytes = 0, localVram = 0;
    uint32_t handles = 0;
};

bool readFilmStressMemory(const boo::D3D12Context& d3d, FilmStressMemory& out) {
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    DWORD handles = 0;
    BOO_CHECK(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)));
    BOO_CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles));
    out.workingSet = memory.WorkingSetSize; out.privateBytes = memory.PrivateUsage;
    out.localVram = d3d.queryEstimatedAppVramBytes(); out.handles = handles;
    return true;
}

boo::JsonValue stressMemoryJson(const FilmStressMemory& memory) {
    boo::JsonValue out(boo::JsonValue::Object{});
    out["working_set_bytes"] = memory.workingSet; out["private_bytes"] = memory.privateBytes;
    out["dxgi_process_local_vram_bytes"] = memory.localVram; out["process_handles"] = int(memory.handles);
    return out;
}

bool testFilmStressGate() {
    using namespace boo;
    BOO_CHECK(gFilmStressIterations >= 200 && gFilmStressIterations <= 100000);
    std::error_code ec; std::filesystem::create_directories(gFilmCaptureDirectory, ec); BOO_CHECK(!ec);
    const auto reportPath = gFilmCaptureDirectory / "stress.json";
    JsonValue report(JsonValue::Object{}); report["schema"] = 1; report["complete"] = false;
    report["requested_iterations"] = int(gFilmStressIterations);
    report["memory_samples"] = JsonValue::Array{};
    report["audio_measurement"] = "48 kHz stereo synthesizer output, full master volume; not a physical loopback recording";
    report["device_loss_injection"] = "safe simulated removal after real queue retirement; not a physical TDR";
    BOO_CHECK(writeFilmGateJson(reportPath, report));

    AppWindow window;
    BOO_CHECK(window.create("BOO film stress gate (non-activating)", 960, 540, false, true));
    D3D12Context d3d; std::string error;
    BOO_CHECK(d3d.initialize(window.hwnd(), window.clientWidth(), window.clientHeight(), false, true, &error));
    BOO_CHECK(!gRequireDebugLayer || d3d.debugLayerActive());
    const bool debugActive = d3d.debugLayerActive();
    report["adapter"] = d3d.adapterName(); report["debug_layer_active"] = debugActive;
    const auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    const auto showcase = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    const auto safe = findQualityPresetSpec(presets, QualityPresetId::Safe);
    CueSheet cue; ValidationReport validation;
    BOO_CHECK(loadCueSheetFromFile(resolveResourcePath("assets/cues/film_360s.json"), cue, validation) && validation.valid);
    BOO_CHECK(cue.durationSamples == 17280000 && cue.sampleRate == 48000 && cue.scenes.size() == 7);
    auto fx = std::make_unique<M1TaipeiSliceEffect>(d3d, showcase, false, false, false, false, false,
                                                 false, false, false, ShowVisualMode::FullFilm360s);
    BOO_CHECK(fx->probeAndInitialize().status != BackendStatus::Faulted);
    AsyncDirectorController director;
    director.start(cue.seed, resolveResourcePath("assets/models/madame_n_director.xml"), false, false, !d3d.isWarp(), false);
    (void)director.waitForInitialCompile(5000);
    AudioSynthesizer48k synth; synth.setDurationSamples(cue.durationSamples); synth.reset(0, 0);
    std::array<float, 2048> pcm{}; float previousAudio[2]{}; float maxAudioStep = 0;

    const auto mediaPath = gFilmCaptureDirectory / "synthetic_stress_clip.mp4";
    BOO_CHECK(generateSyntheticTestMp4(mediaPath));
    MediaIngestSource media; SpoutSender spout;
    const std::string senderName = "BooFilmStress_" + std::to_string(GetCurrentProcessId());
    uint32_t mediaStarts = 0, mediaStops = 0, spoutStarts = 0, spoutStops = 0, resizeCount = 0;
    uint64_t spoutFrames = 0, nonFiniteComponents = 0, audioFrames = 0;
    std::array<uint32_t, kEngineKindCount> verifiedFrames{};
    std::array<uint32_t, 7> actVisits{};
    std::vector<uint8_t> rgba, mediaFrame; uint64_t mediaSequence = 0;
    FilmStressMemory warmMaximum{}, observedMaximum{};
    constexpr uint32_t warmupIterations = 32;
    constexpr uint64_t vramGrowthLimit = 256ULL * 1024ULL * 1024ULL;
    constexpr uint64_t workingSetGrowthLimit = 192ULL * 1024ULL * 1024ULL;
    constexpr uint64_t privateGrowthLimit = 192ULL * 1024ULL * 1024ULL;
    constexpr uint32_t handleGrowthLimit = 64;
    static constexpr int secondsPerAct[] = {18, 95, 145, 175, 225, 255, 318};
    FrameContext lastContext{};
    for (uint32_t iteration = 0; iteration < gFilmStressIterations; ++iteration) {
        BOO_CHECK(window.pumpMessages());
        const uint32_t act = (iteration * 3u) % 7u;
        auto ctx = makeFilmGateContext(cue, int64_t(secondsPerAct[act]) * 48000 + int64_t(iteration % 11u) * 800,
                                      (iteration & 1u) ? Edition::Midnight : Edition::Family);
        ++actVisits[act]; ctx.presentationFrame = iteration; ctx.seeking = true;
        const auto& spec = (iteration & 1u) ? safe : showcase;
        ctx.qualityPreset = spec.id; ctx.showOverlay = true; ctx.showCaptions = true; ctx.hudMode = iteration % 4u;
        const bool disableRt = (iteration & 2u) != 0u, disableCuda = (iteration & 4u) != 0u;
        const bool disableTensor = (iteration & 8u) != 0u, disableCodec = (iteration & 16u) != 0u;
        const bool disableNpu = (iteration % 4u) < 2u;
        fx->setDisableRt(disableRt); fx->setDisableCuda(disableCuda); fx->setDisableTensor(disableTensor);
        fx->setDisableCodec(disableCodec); fx->setCodecSignalHijackActive(iteration % 4u != 0u);
        fx->setAbCompareEngine(static_cast<AbCompareEngine>(iteration % 4u));
        director.setDisableNpu(disableNpu); director.reset(cue.seed);
        const auto mood = director.evaluateFrame(ctx, &cue.scenes[act]);
        for (const auto value : {mood.bloomStrength, mood.irisAngle, mood.lightTension, mood.spectralShift, mood.cutAccent})
            BOO_CHECK(std::isfinite(value) && value >= 0.0f && value <= 1.0f);
        ctx.moodIrisAngle = mood.irisAngle; ctx.moodIntensity = mood.lightTension; ctx.moodPaletteAccent = mood.spectralShift;
        ctx.moodBloomStrength = mood.bloomStrength; ctx.moodCutAccent = mood.cutAccent;
        fx->resetTo(ctx.filmTimeSeconds); fx->onSceneBoundary(act, ctx.filmTimeSeconds);
        BOO_CHECK(fx->syncPresetBeforeFrame(spec, true));
        BOO_CHECK(fx->canvasWidth() == spec.renderWidth && fx->canvasHeight() == spec.renderHeight);

        // Synthetic file only; this gate never requests a real webcam or reads user media.
        if (iteration % 8u == 0u) {
            BOO_CHECK(media.openLocalFile(mediaPath)); ++mediaStarts;
            BOO_CHECK(media.copyLatestNv12Frame(mediaFrame, mediaSequence));
            BOO_CHECK(mediaFrame.size() == MediaIngestSource::kNv12ByteSize && !media.isLiveCameraActive());
            fx->setExternalNv12Frame(mediaFrame.data(), mediaFrame.size(), mediaSequence);
        } else if (iteration % 8u == 4u) {
            media.shutdown(); fx->clearExternalNv12Frame(); ++mediaStops;
            BOO_CHECK(!media.isActive() && !fx->hasExternalMediaFrame());
        }

        if (iteration % 4u == 0u && !d3d.isWarp()) {
            BOO_CHECK(spout.initialize(d3d, senderName, fx->canvasWidth(), fx->canvasHeight(), &error)); ++spoutStarts;
        } else if (iteration % 4u == 2u && spout.isActive()) {
            BOO_CHECK(d3d.waitForGpu()); spoutFrames += spout.publishedFrameCount(); spout.shutdown(); ++spoutStops;
            SpoutSharedTextureInfo info{}; bool listed = true;
            BOO_CHECK(!querySpoutSenderInfo(senderName, info, &listed) && !listed);
        }

        // Real swapchain resizes include 1x1 and odd dimensions. A minimized 0x0
        // request preserves the last valid backbuffers instead of creating zero-sized resources.
        if (iteration % 16u == 0u) { BOO_CHECK(d3d.resize(1, 1)); ++resizeCount; }
        else if (iteration % 16u == 1u) { BOO_CHECK(d3d.resize(961, 541)); ++resizeCount; }
        else if (iteration % 16u == 2u) { BOO_CHECK(d3d.resize(1279, 719)); ++resizeCount; }
        else if (iteration % 16u == 3u) { BOO_CHECK(d3d.resize(960, 540)); ++resizeCount; }
        if (iteration % 32u == 8u) {
            ShowWindow(window.hwnd(), SW_SHOWMINNOACTIVE); BOO_CHECK(window.pumpMessages());
            const auto beforeWidth = d3d.width(), beforeHeight = d3d.height();
            BOO_CHECK(d3d.resize(0, 0)); BOO_CHECK(d3d.width() == beforeWidth && d3d.height() == beforeHeight);
            ShowWindow(window.hwnd(), SW_SHOWNOACTIVATE); BOO_CHECK(window.pumpMessages());
        }
        if (iteration % 32u == 12u) {
            window.toggleBorderlessFullscreen(); BOO_CHECK(window.pumpMessages());
            BOO_CHECK(GetForegroundWindow() != window.hwnd());
            window.toggleBorderlessFullscreen(); BOO_CHECK(window.pumpMessages());
            RECT bounds{}; BOO_CHECK(GetWindowRect(window.hwnd(), &bounds));
            SendMessageW(window.hwnd(), WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&bounds));
            BOO_CHECK(window.pumpMessages() && GetForegroundWindow() != window.hwnd());
        }
        if (window.consumeResizeFlag() && !window.isMinimized()) {
            BOO_CHECK(d3d.resize(window.clientWidth(), window.clientHeight())); ++resizeCount;
        }

        HudTelemetrySnapshot hud{}; hud.filmTimeSeconds = ctx.filmTimeSeconds; hud.durationSeconds = 360;
        hud.edition = ctx.edition; hud.actIndex = act; hud.totalActs = 7; hud.preset = spec.id;
        hud.canvasWidth = fx->canvasWidth(); hud.canvasHeight = fx->canvasHeight(); fx->setHudTelemetry(hud);
        BOO_CHECK(d3d.beginFrame()); fx->render(ctx);
        if (spout.isActive()) BOO_CHECK(spout.recordCleanPublish(d3d, d3d.commandListRaw(), fx->canvasCleanResourceRaw(), fx->canvasWidth(), fx->canvasHeight()));
        double gpuMs = 0; BOO_CHECK(d3d.endFrameAndPresent(false, gpuMs));
        if (spout.isActive()) BOO_CHECK(spout.completePublish());
        BOO_CHECK(std::isfinite(gpuMs) && gpuMs >= 0);
        uint32_t width = 0, height = 0;
        BOO_CHECK(fx->readLastRenderedCanvasForTest(rgba, width, height, &error));
        BOO_CHECK(width == spec.renderWidth && height == spec.renderHeight);
        uint64_t lumaSum = 0; uint32_t maxPixel = 0;
        for (size_t p = 0; p < rgba.size(); p += 4) {
            const auto sum = uint32_t(rgba[p]) + rgba[p + 1] + rgba[p + 2];
            lumaSum += sum; maxPixel = std::max(maxPixel, sum);
        }
        BOO_CHECK(double(lumaSum) / double(width * height * 3ULL * 255ULL) > 0.005 && maxPixel > 30);
        uint64_t invalid = 0; BOO_CHECK(fx->readSceneHdrFiniteForTest(invalid, &error));
        nonFiniteComponents += invalid; BOO_CHECK(invalid == 0);
        for (auto engine : {EngineKind::RT, EngineKind::CUDA, EngineKind::Tensor, EngineKind::Codec}) {
            HardwareResult status; BOO_CHECK(fx->queryLiveEngineStatus(engine, status));
            if (status.status == BackendStatus::Accelerated) {
                BOO_CHECK(status.successfullyExecutedTestWork && !d3d.isWarp()); ++verifiedFrames[static_cast<size_t>(engine)];
            }
        }
        const auto npuStatus = director.telemetry();
        if (npuStatus.hardwareResult.status == BackendStatus::Accelerated) {
            BOO_CHECK(npuStatus.hardwareResult.successfullyExecutedTestWork && !disableNpu);
            ++verifiedFrames[static_cast<size_t>(EngineKind::NPU)];
        }

        synth.setEdition(ctx.edition); synth.seekTo(static_cast<uint64_t>(ctx.samplePosition));
        const uint32_t frameCount = 96u + (iteration % 8u) * 128u;
        synth.renderFrames(pcm.data(), frameCount, iteration % 19u == 0u, iteration % 23u == 0u, 1.0f);
        audioFrames += frameCount;
        for (uint32_t frame = 0; frame < frameCount; ++frame) for (size_t channel = 0; channel < 2; ++channel) {
            const float value = pcm[frame * 2u + channel]; BOO_CHECK(std::isfinite(value));
            maxAudioStep = std::max(maxAudioStep, std::abs(value - previousAudio[channel])); previousAudio[channel] = value;
        }
        BOO_CHECK(maxAudioStep < 0.010f);
        BOO_CHECK(d3d.debugErrorCount() == 0 && d3d.debugWarningCount() == 0);
        FilmStressMemory memory; BOO_CHECK(readFilmStressMemory(d3d, memory));
        auto& peak = iteration < warmupIterations ? warmMaximum : observedMaximum;
        peak.workingSet = std::max(peak.workingSet, memory.workingSet); peak.privateBytes = std::max(peak.privateBytes, memory.privateBytes);
        peak.localVram = std::max(peak.localVram, memory.localVram); peak.handles = std::max(peak.handles, memory.handles);
        if (iteration >= warmupIterations) {
            BOO_CHECK(memory.localVram <= warmMaximum.localVram + vramGrowthLimit);
            BOO_CHECK(memory.workingSet <= warmMaximum.workingSet + workingSetGrowthLimit);
            BOO_CHECK(memory.privateBytes <= warmMaximum.privateBytes + privateGrowthLimit);
            BOO_CHECK(memory.handles <= warmMaximum.handles + handleGrowthLimit);
        }
        auto record = stressMemoryJson(memory); record["iteration"] = int(iteration); record["act"] = int(act);
        record["sample"] = ctx.samplePosition; record["preset"] = spec.label;
        report["memory_samples"].asArrayMut().push_back(std::move(record));
        report["completed_iterations"] = int(iteration + 1u); report["max_audio_step"] = double(maxAudioStep);
        if ((iteration + 1u) % 16u == 0u) {
            BOO_CHECK(writeFilmGateJson(reportPath, report));
            std::cout << "Film stress " << iteration + 1u << '/' << gFilmStressIterations
                      << " | private MiB " << memory.privateBytes / (1024 * 1024)
                      << " | handles " << memory.handles << " | audio step " << maxAudioStep << '\n' << std::flush;
        }
        lastContext = ctx;
    }
    if (media.isActive()) { media.shutdown(); ++mediaStops; }
    fx->clearExternalNv12Frame();
    if (spout.isActive()) { BOO_CHECK(d3d.waitForGpu()); spoutFrames += spout.publishedFrameCount(); spout.shutdown(); ++spoutStops; }
    director.shutdown();
    BOO_CHECK(mediaStarts >= 25 && mediaStops == mediaStarts);
    BOO_CHECK(d3d.isWarp() || (spoutStarts >= 50 && spoutStarts == spoutStops && spoutFrames >= 100));
    for (const auto visits : actVisits) BOO_CHECK(visits > 20);

    // Exactly one injected loss/recreate attempt, preserving the requested sample.
    const auto recoveryStart = std::chrono::steady_clock::now();
    BOO_CHECK(d3d.injectDeviceLostForTest()); BOO_CHECK(!d3d.beginFrame());
    fx->shutdown(); fx.reset(); d3d.shutdown();
    BOO_CHECK(d3d.initialize(window.hwnd(), 960, 540, false, true, &error));
    BOO_CHECK(!gRequireDebugLayer || d3d.debugLayerActive());
    fx = std::make_unique<M1TaipeiSliceEffect>(d3d, safe, true, false, true, true, true,
                                             false, false, false, ShowVisualMode::FullFilm360s);
    BOO_CHECK(fx->probeAndInitialize().status != BackendStatus::Faulted);
    lastContext.qualityPreset = safe.id; lastContext.showOverlay = false; lastContext.hudMode = 3;
    fx->resetTo(lastContext.filmTimeSeconds);
    uint32_t width = 0, height = 0;
    BOO_CHECK(fx->renderHeadlessCanvasForTest(lastContext, rgba, width, height, &error));
    BOO_CHECK(computeRgba8LumaMetrics(rgba).meanLuma > 0.005);
    const double recoverySeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - recoveryStart).count();
    BOO_CHECK(recoverySeconds < 30.0 && d3d.checkDeviceHealthy());
    BOO_CHECK(d3d.debugErrorCount() == 0 && d3d.debugWarningCount() == 0);
    fx->shutdown(); fx.reset(); BOO_CHECK(d3d.waitForGpu()); d3d.shutdown(); window.destroy();
    MSG message{}; while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    std::filesystem::remove(mediaPath, ec);
    report["complete"] = true; report["max_audio_step"] = double(maxAudioStep); report["audio_frames_checked"] = audioFrames;
    report["hdr_nonfinite_components"] = nonFiniteComponents; report["warmup_iterations"] = int(warmupIterations);
    report["warmup_peak"] = stressMemoryJson(warmMaximum); report["subsequent_peak"] = stressMemoryJson(observedMaximum);
    report["vram_growth_limit_bytes"] = vramGrowthLimit; report["working_set_growth_limit_bytes"] = workingSetGrowthLimit;
    report["private_growth_limit_bytes"] = privateGrowthLimit; report["handle_growth_limit"] = int(handleGrowthLimit);
    report["media_start_stop_cycles"] = int(mediaStarts); report["spout_start_stop_cycles"] = int(spoutStarts);
    report["spout_frames"] = spoutFrames; report["swapchain_resizes"] = int(resizeCount);
    report["simulated_device_loss_recovery_seconds"] = recoverySeconds;
    report["recovered_sample"] = lastContext.samplePosition;
    report["debug_errors"] = debugActive ? JsonValue(0) : JsonValue(nullptr);
    report["debug_warnings"] = debugActive ? JsonValue(0) : JsonValue(nullptr);
    report["warning_validation"] = debugActive ? "passed" : "unavailable: Windows Graphics Tools debug layer is absent";
    report["verified_accelerated_frame_counts"] = JsonValue::Object{};
    for (size_t engine = 0; engine < kEngineKindCount; ++engine)
        report["verified_accelerated_frame_counts"][toString(static_cast<EngineKind>(engine))] = int(verifiedFrames[engine]);
    BOO_CHECK(writeFilmGateJson(reportPath, report));
    std::cout << "Film stress receipt: " << reportPath.string() << '\n';
    return true;
}
