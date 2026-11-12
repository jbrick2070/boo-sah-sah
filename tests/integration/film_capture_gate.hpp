// Included by integration_main.cpp inside its anonymous namespace. No end-user
// executable or image-library dependency: PNG uses inbox Windows Imaging Component.
std::filesystem::path gFilmCaptureDirectory = "build/film_captures";
bool gRequireDebugLayer = false;

struct CaptureComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~CaptureComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};

bool writeCapturePng(const std::filesystem::path& path, const std::vector<uint8_t>& rgba,
                     uint32_t width, uint32_t height) {
    using Microsoft::WRL::ComPtr;
    BOO_CHECK(width && height && rgba.size() == size_t(width) * height * 4u);
    ComPtr<IWICImagingFactory> factory;
    BOO_CHECK(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory))));
    const auto pending = std::filesystem::path(path.wstring() + L".tmp");
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    BOO_CHECK(SUCCEEDED(factory->CreateStream(&stream)));
    BOO_CHECK(SUCCEEDED(stream->InitializeFromFilename(pending.c_str(), GENERIC_WRITE)));
    BOO_CHECK(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)));
    BOO_CHECK(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)));
    BOO_CHECK(SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)));
    BOO_CHECK(SUCCEEDED(frame->Initialize(nullptr)));
    BOO_CHECK(SUCCEEDED(frame->SetSize(width, height)));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    BOO_CHECK(SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat24bppBGR));
    std::vector<uint8_t> rgb(size_t(width) * height * 3u);
    for (size_t p = 0; p < rgba.size() / 4u; ++p) {
        rgb[p * 3u] = rgba[p * 4u + 2u]; rgb[p * 3u + 1u] = rgba[p * 4u + 1u]; rgb[p * 3u + 2u] = rgba[p * 4u];
    }
    BOO_CHECK(SUCCEEDED(frame->WritePixels(height, width * 3u, static_cast<UINT>(rgb.size()), rgb.data())));
    BOO_CHECK(SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()));
    frame.Reset(); encoder.Reset(); stream.Reset();
    BOO_CHECK(MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    // Read the encoded PNG back through an independent decoder: correct size,
    // channel order and lossless pixel export are part of this gate.
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> decoded;
    ComPtr<IWICFormatConverter> converter;
    BOO_CHECK(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                        WICDecodeMetadataCacheOnLoad, &decoder)));
    BOO_CHECK(SUCCEEDED(decoder->GetFrame(0, &decoded)));
    UINT dw = 0, dh = 0;
    BOO_CHECK(SUCCEEDED(decoded->GetSize(&dw, &dh)) && dw == width && dh == height);
    BOO_CHECK(SUCCEEDED(factory->CreateFormatConverter(&converter)));
    BOO_CHECK(SUCCEEDED(converter->Initialize(decoded.Get(), GUID_WICPixelFormat24bppBGR,
                                             WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)));
    std::vector<uint8_t> decodedRgb(rgb.size());
    BOO_CHECK(SUCCEEDED(converter->CopyPixels(nullptr, width * 3u,
                                            static_cast<UINT>(decodedRgb.size()), decodedRgb.data())));
    BOO_CHECK(rgb == decodedRgb);
    return true;
}

std::string capturePixelHash(const std::vector<uint32_t>& words) {
    uint64_t hash = 14695981039346656037ULL;
    for (const auto word : words) for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (word >> shift) & 255u;
        hash *= 1099511628211ULL;
    }
    std::ostringstream text; text << std::hex << std::setfill('0') << std::setw(16) << hash;
    return text.str();
}

struct CaptureContactSheet {
    static constexpr uint32_t thumbWidth = 640, thumbHeight = 360, labelHeight = 36;
    std::vector<std::vector<uint8_t>> images;
    std::vector<std::string> labels;

    void add(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, std::string label) {
        std::vector<uint8_t> thumb(size_t(thumbWidth) * thumbHeight * 4u, 255u);
        for (uint32_t y = 0; y < thumbHeight; ++y) for (uint32_t x = 0; x < thumbWidth; ++x) {
            const uint32_t x0 = x * width / thumbWidth, x1 = (x + 1u) * width / thumbWidth;
            const uint32_t y0 = y * height / thumbHeight, y1 = (y + 1u) * height / thumbHeight;
            uint32_t sum[3]{};
            for (uint32_t sy = y0; sy < y1; ++sy) for (uint32_t sx = x0; sx < x1; ++sx)
                for (uint32_t c = 0; c < 3; ++c) sum[c] += rgba[(size_t(sy) * width + sx) * 4u + c];
            const auto count = std::max(1u, (x1 - x0) * (y1 - y0));
            for (uint32_t c = 0; c < 3; ++c) thumb[(size_t(y) * thumbWidth + x) * 4u + c] = uint8_t(sum[c] / count);
        }
        images.push_back(std::move(thumb)); labels.push_back(std::move(label));
    }

    bool save(const std::filesystem::path& path, uint32_t columns = 2) const {
        BOO_CHECK(!images.empty() && columns > 0);
        const uint32_t rows = (static_cast<uint32_t>(images.size()) + columns - 1) / columns;
        const uint32_t width = columns * thumbWidth, height = rows * (thumbHeight + labelHeight);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width); info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void* dibBits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        BOO_CHECK(dc != nullptr);
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        if (!bitmap || !dibBits) { DeleteDC(dc); return false; }
        const auto oldBitmap = SelectObject(dc, bitmap);
        auto* bgra = static_cast<uint8_t*>(dibBits);
        std::fill_n(bgra, size_t(width) * height * 4u, uint8_t(18));
        HFONT font = CreateFontW(-20, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const auto oldFont = font ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(230, 238, 240));
        for (size_t i = 0; i < images.size(); ++i) {
            const uint32_t ox = static_cast<uint32_t>(i % columns) * thumbWidth;
            const uint32_t oy = static_cast<uint32_t>(i / columns) * (thumbHeight + labelHeight);
            for (uint32_t y = 0; y < thumbHeight; ++y) for (uint32_t x = 0; x < thumbWidth; ++x) {
                auto* dst = bgra + (size_t(oy + y) * width + ox + x) * 4u;
                const auto* src = images[i].data() + (size_t(y) * thumbWidth + x) * 4u;
                dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255u;
            }
            RECT labelRect{LONG(ox + 10), LONG(oy + thumbHeight + 4), LONG(ox + thumbWidth - 8), LONG(oy + thumbHeight + labelHeight)};
            DrawTextA(dc, labels[i].c_str(), static_cast<int>(labels[i].size()), &labelRect, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        GdiFlush();
        std::vector<uint8_t> rgba(size_t(width) * height * 4u);
        for (size_t p = 0; p < rgba.size(); p += 4u) { rgba[p] = bgra[p + 2]; rgba[p + 1] = bgra[p + 1]; rgba[p + 2] = bgra[p]; rgba[p + 3] = 255u; }
        if (font) { SelectObject(dc, oldFont); DeleteObject(font); }
        SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
        return writeCapturePng(path, rgba, width, height);
    }
};

boo::FrameContext makeFilmGateContext(const boo::CueSheet& cue, int64_t sample, boo::Edition edition) {
    auto ctx = makeM1TestContext(sample, 0);
    const size_t index = boo::findSceneIndexAtSample(cue, sample);
    const auto& scene = cue.scenes.at(index);
    ctx.sampleRate = cue.sampleRate; ctx.actIndex = static_cast<uint32_t>(index);
    ctx.actProgress = float(double(sample - scene.startSample) / double(std::max<int64_t>(1, scene.endSample - scene.startSample)));
    ctx.randomSeed = scene.deterministicSeed; ctx.edition = edition;
    ctx.qualityPreset = boo::QualityPresetId::Showcase;
    ctx.audioEnvelope = boo::computeAnalyticalAudioBands(sample, cue.sampleRate, cue.durationSamples, edition).rms;
    ctx.showOverlay = false; ctx.hudMode = 3; ctx.showCaptions = false;
    // Fresh analytical director makes visual reference capture independent of
    // worker scheduling. Live NPU execution is verified separately by the soak.
    boo::ScriptedDirector director; director.reset(cue.seed);
    const auto mood = director.evaluate(ctx, &scene);
    ctx.moodIrisAngle = mood.irisAngle; ctx.moodIntensity = mood.lightTension; ctx.moodPaletteAccent = mood.spectralShift;
    ctx.moodBloomStrength = mood.bloomStrength; ctx.moodCutAccent = mood.cutAccent;
    return ctx;
}

bool writeFilmGateJson(const std::filesystem::path& path, const boo::JsonValue& value) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << boo::serializeJson(value, 2) << '\n';
    out.close();
    BOO_CHECK(out.good());
    return true;
}

bool testFullFilmCaptureGate() {
    using namespace boo;
    CaptureComScope com;
    BOO_CHECK(SUCCEEDED(com.result) || com.result == RPC_E_CHANGED_MODE);
    std::error_code ec; std::filesystem::create_directories(gFilmCaptureDirectory, ec); BOO_CHECK(!ec);
    D3D12Context d3d; std::string error;
    BOO_CHECK(d3d.initialize(nullptr, 2560, 1440, false, true, &error));
    BOO_CHECK(!gRequireDebugLayer || d3d.debugLayerActive());
    const auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    const auto spec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    BOO_CHECK(spec.renderWidth == 2560 && spec.renderHeight == 1440);
    JsonValue report(JsonValue::Object{});
    report["schema"] = 1; report["complete"] = false; report["width"] = 2560; report["height"] = 1440;
    report["sample_rate"] = 48000; report["adapter"] = d3d.adapterName();
    report["director"] = "sample-derived ScriptedDirector; no NPU execution claim";
    report["clean_bits"] = 10; report["png_bits"] = 8;
    report["pixel_hash"] = "FNV-1a64 over little-endian RGB10A2; regression identity, not cryptographic integrity";
    report["frames"] = JsonValue::Array{};
    const auto reportPath = gFilmCaptureDirectory / "manifest.json";
    BOO_CHECK(writeFilmGateJson(reportPath, report));
    struct CaptureCue { const char* file; const char* label; std::vector<int> seconds; };
    const CaptureCue cues[] = {
        {"film_360s", "film", {18, 36, 95, 145, 175, 225, 260, 275, 318, 342, 355}},
        {"solo_rt", "solo_rt", {24}}, {"solo_cuda", "solo_cuda", {24}},
        {"solo_tensor", "solo_tensor", {24}}, {"solo_npu", "solo_npu", {25}}
    };
    CaptureContactSheet narrativeSheet, soloSheet;
    for (const auto& selection : cues) {
        CueSheet cue; ValidationReport validation;
        const auto cuePath = std::string("assets/cues/") + selection.file + ".json";
        BOO_CHECK(loadCueSheetFromFile(resolveResourcePath(cuePath), cue, validation) && validation.valid);
        BOO_CHECK(cue.sampleRate == 48000 && !cue.scenes.empty());
        const auto mode = resolveShowVisualMode(cue);
        M1TaipeiSliceEffect fx(d3d, spec, false, false, false, false, false, false, false, false, mode);
        BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);
        for (const auto edition : {Edition::Family, Edition::Midnight}) {
            CaptureContactSheet filmSheet;
            const std::string editionName = edition == Edition::Family ? "family" : "midnight";
            for (const auto seconds : selection.seconds) {
                const auto sample = int64_t(seconds) * cue.sampleRate;
                BOO_CHECK(sample < cue.durationSamples);
                auto ctx = makeFilmGateContext(cue, sample, edition);
                const std::string stem = std::string(selection.label) + "_" + editionName + "_" + std::to_string(seconds) + "s";
                std::vector<uint8_t> rgba, repeat, canonical;
                std::vector<uint32_t> rgb10, repeated10, canonical10;
                uint32_t width = 0, height = 0;
                for (const bool raster : {false, true}) {
                    fx.setDisableRt(raster); fx.resetTo(ctx.filmTimeSeconds);
                    BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx, rgba, width, height, &error, &rgb10));
                    BOO_CHECK(width == 2560 && height == 1440);
                    uint64_t nonFinite = 0;
                    BOO_CHECK(fx.readSceneHdrFiniteForTest(nonFinite, &error) && nonFinite == 0);
                    const auto metrics = computeRgba8LumaMetrics(rgba);
                    BOO_CHECK(std::isfinite(metrics.meanLuma) && metrics.meanLuma > 0.008 && metrics.p99Luma - metrics.p10Luma > 0.04);
                    // Travel away before replay so equality checks actual reset/seek state.
                    fx.resetTo(0.0); auto repeated = ctx; repeated.presentationFrame = 991;
                    BOO_CHECK(fx.renderHeadlessCanvasForTest(repeated, repeat, width, height, &error, &repeated10));
                    BOO_CHECK(rgb10 == repeated10 && rgba == repeat);
                    HardwareResult rt;
                    BOO_CHECK(fx.queryLiveEngineStatus(EngineKind::RT, rt));
                    BOO_CHECK(rt.status != BackendStatus::Accelerated || (rt.successfullyExecutedTestWork && !d3d.isWarp()));
                    BOO_CHECK(!raster || rt.status == BackendStatus::Substituted);
                    const std::string backend = rt.status == BackendStatus::Accelerated ? "DXR verified" : "raster substitute";
                    const std::string file = stem + (raster ? "_raster.png" : "_preferred.png");
                    BOO_CHECK(writeCapturePng(gFilmCaptureDirectory / file, rgba, width, height));
                    const std::string label = std::string(selection.label) + " " + editionName + " " + std::to_string(seconds) + "s | " + backend;
                    (mode == ShowVisualMode::FullFilm360s ? filmSheet : soloSheet).add(rgba, width, height, label);
                    JsonValue entry(JsonValue::Object{});
                    entry["file"] = file; entry["cue"] = cuePath; entry["scene"] = cue.scenes[ctx.actIndex].sceneId;
                    entry["edition"] = editionName; entry["sample"] = sample; entry["seconds"] = seconds;
                    entry["seed"] = ctx.randomSeed; entry["rt_disabled"] = raster; entry["rt_status"] = toString(rt.status);
                    entry["rt_backend"] = rt.activeBackend; entry["rt_verified"] = rt.successfullyExecutedTestWork;
                    entry["rgb10_hash"] = capturePixelHash(rgb10); entry["mean_luma"] = metrics.meanLuma;
                    entry["seek_repeat_equal"] = true; entry["pass"] = "CanvasClean";
                    entry["hdr_nonfinite_components"] = nonFinite;
                    report["frames"].asArrayMut().push_back(std::move(entry));
                    if (!raster) { canonical = rgba; canonical10 = rgb10; }
                }
                fx.setDisableRt(false);
                // Pass-4 HUD, captions, camera badge and A/B must not alter clean bytes.
                HudTelemetrySnapshot hud{}; hud.durationSeconds = double(cue.durationSamples) / cue.sampleRate;
                hud.filmTimeSeconds = seconds; hud.edition = edition; hud.actIndex = ctx.actIndex;
                hud.totalActs = static_cast<uint32_t>(cue.scenes.size()); hud.canvasWidth = 2560; hud.canvasHeight = 1440;
                hud.liveCameraActive = true; // Test badge only: never opens a physical camera.
                auto guarded = ctx; guarded.showOverlay = true; guarded.showCaptions = true; guarded.hudMode = 2;
                fx.setAbCompareEngine(AbCompareEngine::RT);
                std::vector<uint8_t> presented;
                BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(guarded, hud, 2560, 1440, presented, &error));
                BOO_CHECK(fx.readLastRenderedCanvasForTest(repeat, width, height, &error, &repeated10));
                BOO_CHECK(canonical10 == repeated10);
                BOO_CHECK(presented != canonical);
                if (mode == ShowVisualMode::FullFilm360s && editionName == "family" && (seconds == 36 || seconds == 355)) {
                    auto statsContext=ctx; statsContext.showOverlay=true; statsContext.showCaptions=true; statsContext.hudMode=0;
                    auto statsHud=hud; statsHud.liveCameraActive=false; statsHud.benchHasResults=true;
                    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(statsContext,statsHud,2560,1440,presented,&error));
                    BOO_CHECK(fx.readLastRenderedCanvasForTest(repeat,width,height,&error,&repeated10));
                    BOO_CHECK(canonical10 == repeated10);
                    BOO_CHECK(writeCapturePng(gFilmCaptureDirectory / (stem+"_stats_layout.png"),presented,2560,1440));
                }
                if (mode == ShowVisualMode::FullFilm360s && (seconds == 36 || seconds == 175 || seconds == 342 || seconds == 355)) {
                    hud.liveCameraActive = false; guarded.showOverlay = false; guarded.hudMode = 3;
                    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(guarded, hud, 2560, 1440, presented, &error));
                    BOO_CHECK(fx.readLastRenderedCanvasForTest(repeat, width, height, &error, &repeated10));
                    BOO_CHECK(canonical10 == repeated10);
                    const std::string file = stem + "_presentation.png";
                    BOO_CHECK(writeCapturePng(gFilmCaptureDirectory / file, presented, 2560, 1440));
                    narrativeSheet.add(presented, 2560, 1440, editionName + " " + std::to_string(seconds) + "s | captions / title");
                    JsonValue entry(JsonValue::Object{}); entry["file"] = file; entry["sample"] = sample;
                    entry["edition"] = editionName; entry["pass"] = "Presentation with captions / end card";
                    entry["clean_feed_equal"] = true;
                    report["frames"].asArrayMut().push_back(std::move(entry));
                }
                BOO_CHECK(writeFilmGateJson(reportPath, report));
                std::cout << "Captured " << stem << " (native 1440p, repeat + clean-feed checks passed)\n" << std::flush;
            }
            if (mode == ShowVisualMode::FullFilm360s)
                BOO_CHECK(filmSheet.save(gFilmCaptureDirectory / ("film_" + editionName + "_montage.png")));
        }
        fx.shutdown();
    }
    BOO_CHECK(soloSheet.save(gFilmCaptureDirectory / "solos_montage.png"));
    BOO_CHECK(narrativeSheet.save(gFilmCaptureDirectory / "narrative_montage.png"));
    d3d.waitForGpu();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    BOO_CHECK(d3d.debugWarningCount() == 0);
    report["complete"] = true; report["debug_layer_active"] = d3d.debugLayerActive();
    report["debug_errors"] = d3d.debugLayerActive() ? JsonValue(int(d3d.debugErrorCount())) : JsonValue(nullptr);
    report["debug_warnings"] = d3d.debugLayerActive() ? JsonValue(int(d3d.debugWarningCount())) : JsonValue(nullptr);
    report["warning_validation"] = d3d.debugLayerActive() ? "passed" : "unavailable: Windows Graphics Tools debug layer is absent";
    report["clean_feed_guards_passed"] = true;
    BOO_CHECK(writeFilmGateJson(reportPath, report));
    std::cout << "Film capture receipt: " << reportPath.string() << '\n';
    return true;
}

bool writeFastCapturePng(const std::filesystem::path& path, const std::vector<uint8_t>& rgba,
                         uint32_t width, uint32_t height) {
    using Microsoft::WRL::ComPtr;
    BOO_CHECK(width && height && rgba.size() == size_t(width) * height * 4u);
    ComPtr<IWICImagingFactory> factory;
    BOO_CHECK(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&factory))));
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    BOO_CHECK(SUCCEEDED(factory->CreateStream(&stream)));
    BOO_CHECK(SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)));
    BOO_CHECK(SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)));
    BOO_CHECK(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)));
    BOO_CHECK(SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)));
    BOO_CHECK(SUCCEEDED(frame->Initialize(nullptr)));
    BOO_CHECK(SUCCEEDED(frame->SetSize(width, height)));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    BOO_CHECK(SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat24bppBGR));
    std::vector<uint8_t> bgr(size_t(width) * height * 3u);
    for (size_t p = 0; p < rgba.size() / 4u; ++p) {
        bgr[p * 3u] = rgba[p * 4u + 2u];
        bgr[p * 3u + 1u] = rgba[p * 4u + 1u];
        bgr[p * 3u + 2u] = rgba[p * 4u];
    }
    BOO_CHECK(SUCCEEDED(frame->WritePixels(height, width * 3u, static_cast<UINT>(bgr.size()), bgr.data())));
    BOO_CHECK(SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()));
    return true;
}

bool testHeroVideoCaptureGate() {
    using namespace boo;
    CaptureComScope com;
    BOO_CHECK(SUCCEEDED(com.result) || com.result == RPC_E_CHANGED_MODE);
    const auto framesDir = gFilmCaptureDirectory / "hero_10s_frames";
    std::error_code ec;
    std::filesystem::create_directories(framesDir, ec);
    BOO_CHECK(!ec);

    D3D12Context d3d;
    std::string error;
    BOO_CHECK(d3d.initialize(nullptr, 2560, 1440, false, true, &error));
    const auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    const auto spec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    BOO_CHECK(spec.renderWidth == 2560 && spec.renderHeight == 1440);

    CueSheet cue;
    ValidationReport validation;
    BOO_CHECK(loadCueSheetFromFile(resolveResourcePath("assets/cues/film_360s.json"), cue, validation) && validation.valid);
    const auto mode = resolveShowVisualMode(cue);
    M1TaipeiSliceEffect fx(d3d, spec, false, false, false, false, false, false, false, false, mode);
    BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);
    fx.setDisableRt(false);

    // 300 native 2560x1440 frames @ 30 fps (1600 samples/frame @ 48 kHz) across [342.000s .. 352.000s)
    constexpr uint32_t kTotalFrames = 300;
    constexpr int64_t kStartSample = 342LL * 48000LL;
    constexpr int64_t kStepSamples = 1600LL;
    fx.resetTo(342.0);

    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
    for (uint32_t f = 0; f < kTotalFrames; ++f) {
        const int64_t sample = kStartSample + int64_t(f) * kStepSamples;
        auto ctx = makeFilmGateContext(cue, sample, Edition::Family);
        ctx.presentationFrame = f;
        BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx, rgba, width, height, &error, nullptr));
        BOO_CHECK(width == 2560 && height == 1440);
        if ((f % 60u) == 0u || f + 1u == kTotalFrames) {
            uint64_t nonFinite = 0;
            BOO_CHECK(fx.readSceneHdrFiniteForTest(nonFinite, &error) && nonFinite == 0);
            HardwareResult rt;
            BOO_CHECK(fx.queryLiveEngineStatus(EngineKind::RT, rt));
            BOO_CHECK(rt.status == BackendStatus::Accelerated && rt.successfullyExecutedTestWork);
        }
        std::ostringstream fname;
        fname << "frame_" << std::setfill('0') << std::setw(4) << f << ".png";
        BOO_CHECK(writeFastCapturePng(framesDir / fname.str(), rgba, width, height));
        if ((f % 60u) == 0u || f + 1u == kTotalFrames) {
            std::cout << "Captured hero 10s frame " << (f + 1u) << "/" << kTotalFrames
                      << " (t=" << std::fixed << std::setprecision(2) << (double(sample) / 48000.0) << "s)\n" << std::flush;
        }
    }

    fx.shutdown();
    d3d.waitForGpu();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    return true;
}

bool testInteractiveShowcaseCaptureGate() {
    using namespace boo;
    CaptureComScope com;
    BOO_CHECK(SUCCEEDED(com.result) || com.result == RPC_E_CHANGED_MODE);
    const std::filesystem::path outDir = (gFilmCaptureDirectory == std::filesystem::path("build/film_captures"))
        ? std::filesystem::path("build/interactive_captures")
        : gFilmCaptureDirectory;
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    BOO_CHECK(!ec);

    D3D12Context d3d;
    std::string error;
    BOO_CHECK(d3d.initialize(nullptr, 2560, 1440, false, true, &error));
    const auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    const auto spec = findQualityPresetSpec(presets, QualityPresetId::Showcase);
    BOO_CHECK(spec.renderWidth == 2560 && spec.renderHeight == 1440);

    CueSheet cue;
    ValidationReport validation;
    BOO_CHECK(loadCueSheetFromFile(resolveResourcePath("assets/cues/film_360s.json"), cue, validation) && validation.valid);
    const auto mode = resolveShowVisualMode(cue);
    M1TaipeiSliceEffect fx(d3d, spec, false, false, false, false, false, false, false, false, mode);
    BOO_CHECK(fx.probeAndInitialize().status != BackendStatus::Faulted);
    fx.setDisableRt(false);

    const int64_t bathroomSample = 346LL * 48000LL;
    auto baseCtx = makeFilmGateContext(cue, bathroomSample, Edition::Family);

    HudTelemetrySnapshot hud{};
    hud.filmTimeSeconds = 346.0;
    hud.durationSeconds = 360.0;
    hud.fps = 60.0f;
    hud.gpuFrameMs = 13.8f;
    hud.actIndex = baseCtx.actIndex;
    hud.totalActs = static_cast<uint32_t>(cue.scenes.size());
    hud.edition = Edition::Family;
    hud.canvasWidth = 2560;
    hud.canvasHeight = 1440;
    hud.swapchainBits = 10;

    CaptureContactSheet sheet;
    JsonValue report(JsonValue::Object{});
    report["schema"] = 1;
    report["width"] = 2560;
    report["height"] = 1440;
    report["adapter"] = d3d.adapterName();
    report["frames"] = JsonValue::Array{};

    auto renderConvergedView = [&](FrameContext ctx, uint32_t targetSpp, std::vector<uint8_t>& outClean, uint32_t& outW, uint32_t& outH) -> bool {
        fx.resetTo(ctx.filmTimeSeconds);
        for (uint32_t s = 0; s < targetSpp; ++s) {
            ctx.presentationFrame = s;
            ctx.interactive.accumulate = (targetSpp > 1u);
            ctx.interactive.accumFrame = s;
            if (s + 1u == targetSpp) {
                BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx, outClean, outW, outH, &error));
            } else {
                // Pump accumulation without slow CPU readback on intermediate samples
                std::vector<uint8_t> tmp;
                BOO_CHECK(fx.renderHeadlessCanvasForTest(ctx, tmp, outW, outH, &error));
            }
        }
        uint64_t nonFinite = 0;
        BOO_CHECK(fx.readSceneHdrFiniteForTest(nonFinite, &error) && nonFinite == 0);
        return true;
    };

    auto setSunSpherical = [](InteractiveView& iv, float elev01, float azim01) {
        iv.sunOverride = true;
        iv.sunElevation01 = elev01;
        iv.sunAzimuth01 = azim01;
        const float elevRad = (-0.28f + elev01 * 1.48f);
        const float azimRad = (azim01 * 6.2831853f) - 0.58f;
        const float ce = std::cos(elevRad);
        iv.sunDir[0] = ce * std::cos(azimRad);
        iv.sunDir[1] = std::sin(elevRad);
        iv.sunDir[2] = ce * std::sin(azimRad);
        iv.sunScale = 1.0f;
    };

    // 1. Preset 2: CNC Faucet, Aerator Droplet & Porcelain Basin Macro (1 SPP vs 64 SPP NRD Reference PT + Studio Bar)
    {
        FrameContext ctx = baseCtx;
        ctx.interactive.exploreActive = true;
        ctx.interactive.blendWeight = 1.0f;
        ctx.interactive.activePreset = 2u;
        ctx.interactive.eye[0] = 0.05f;    ctx.interactive.eye[1] = 3.83f;    ctx.interactive.eye[2] = 5.76f;
        ctx.interactive.target[0] = -0.18f; ctx.interactive.target[1] = 3.67f; ctx.interactive.target[2] = 6.34f;
        ctx.interactive.fovYRadians = 0.44f;

        std::vector<uint8_t> spp1, spp64, studioPres;
        uint32_t w = 0, h = 0;
        BOO_CHECK(renderConvergedView(ctx, 1u, spp1, w, h));
        BOO_CHECK(writeCapturePng(outDir / "interactive_preset2_faucet_1spp_clean.png", spp1, w, h));

        BOO_CHECK(renderConvergedView(ctx, 64u, spp64, w, h));
        BOO_CHECK(writeCapturePng(outDir / "interactive_preset2_faucet_64spp_clean.png", spp64, w, h));
        sheet.add(spp64, w, h, "Preset [2] Faucet & Porcelain Macro | 64 SPP NRD PT");

        ctx.showOverlay = true;
        ctx.hudMode = 1u;
        ctx.interactive.accumulate = true;
        ctx.interactive.accumFrame = 63u;
        ctx.presentationFrame = 63u;
        BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(ctx, hud, 2560, 1440, studioPres, &error));
        BOO_CHECK(writeCapturePng(outDir / "interactive_preset2_faucet_64spp_studio.png", studioPres, 2560, 1440));
        sheet.add(studioPres, 2560, 1440, "Preset [2] with Pass-4 RTX Showcase Studio Bar");
        std::cout << "Captured Preset 2 Faucet Macro (1 SPP + 64 SPP NRD + Studio Bar)\n" << std::flush;
    }

    // 2. Preset 3: Backlit Calathea Veins & Keepsake Glass Orchid (64 SPP NRD Reference PT)
    {
        FrameContext ctx = baseCtx;
        ctx.interactive.exploreActive = true;
        ctx.interactive.blendWeight = 1.0f;
        ctx.interactive.activePreset = 3u;
        ctx.interactive.eye[0] = -0.16f;   ctx.interactive.eye[1] = 3.88f;   ctx.interactive.eye[2] = 5.62f;
        ctx.interactive.target[0] = -0.58f; ctx.interactive.target[1] = 3.82f; ctx.interactive.target[2] = 6.26f;
        ctx.interactive.fovYRadians = 0.48f;

        std::vector<uint8_t> spp64;
        uint32_t w = 0, h = 0;
        BOO_CHECK(renderConvergedView(ctx, 64u, spp64, w, h));
        BOO_CHECK(writeCapturePng(outDir / "interactive_preset3_calathea_64spp_clean.png", spp64, w, h));
        sheet.add(spp64, w, h, "Preset [3] Calathea Subsurface Veins | 64 SPP NRD PT");
        std::cout << "Captured Preset 3 Calathea Subsurface Macro (64 SPP NRD)\n" << std::flush;
    }

    // 3. Preset 4: Keepsake Glass Orchid & Celadon Vase Refraction Macro (64 SPP NRD Reference PT)
    {
        FrameContext ctx = baseCtx;
        ctx.interactive.exploreActive = true;
        ctx.interactive.blendWeight = 1.0f;
        ctx.interactive.activePreset = 4u;
        ctx.interactive.eye[0] = 0.14f;    ctx.interactive.eye[1] = 3.84f;    ctx.interactive.eye[2] = 5.72f;
        ctx.interactive.target[0] = 0.402f; ctx.interactive.target[1] = 3.75f; ctx.interactive.target[2] = 6.235f;
        ctx.interactive.fovYRadians = 0.44f;

        std::vector<uint8_t> spp64;
        uint32_t w = 0, h = 0;
        BOO_CHECK(renderConvergedView(ctx, 64u, spp64, w, h));
        BOO_CHECK(writeCapturePng(outDir / "interactive_preset4_orchid_vase_64spp_clean.png", spp64, w, h));
        sheet.add(spp64, w, h, "Preset [4] Keepsake Glass Orchid & Celadon Vase | 64 SPP NRD");
        std::cout << "Captured Preset 4 Keepsake Glass Orchid Macro (64 SPP NRD)\n" << std::flush;
    }

    // 4. Interactive Sun Time-of-Day Sweep & RTXDI ReSTIR Practical Lamps in Dawn Bathroom (Preset 1 Wide)
    {
        FrameContext wide = baseCtx;
        wide.interactive.exploreActive = true;
        wide.interactive.blendWeight = 1.0f;
        wide.interactive.activePreset = 1u;
        wide.interactive.eye[0] = 0.34f;    wide.interactive.eye[1] = 4.02f;    wide.interactive.eye[2] = 5.12f;
        wide.interactive.target[0] = -0.18f; wide.interactive.target[1] = 3.88f; wide.interactive.target[2] = 6.32f;
        wide.interactive.fovYRadians = 0.56f;

        std::vector<uint8_t> golden32, highDay32, nightLampsOn32, nightLampsOff32;
        uint32_t w = 0, h = 0;

        FrameContext golden = wide;
        setSunSpherical(golden.interactive, 0.31f, 0.14f);
        BOO_CHECK(renderConvergedView(golden, 32u, golden32, w, h));
        BOO_CHECK(writeCapturePng(outDir / "interactive_bathroom_golden_dawn_32spp.png", golden32, w, h));
        sheet.add(golden32, w, h, "Interactive Sun: Low Golden Dawn | 32 SPP NRD");

        FrameContext highDay = wide;
        setSunSpherical(highDay.interactive, 0.72f, 0.22f);
        BOO_CHECK(renderConvergedView(highDay, 32u, highDay32, w, h));
        BOO_CHECK(writeCapturePng(outDir / "interactive_bathroom_high_day_32spp.png", highDay32, w, h));
        sheet.add(highDay32, w, h, "Interactive Sun: Crisp High Day | 32 SPP NRD");

        FrameContext nightOn = wide;
        setSunSpherical(nightOn.interactive, 0.04f, 0.14f);
        nightOn.interactive.lampsOff = false;
        BOO_CHECK(renderConvergedView(nightOn, 32u, nightLampsOn32, w, h));
        BOO_CHECK(writeCapturePng(outDir / "interactive_bathroom_midnight_rtxdi_lamps_on_32spp.png", nightLampsOn32, w, h));
        sheet.add(nightLampsOn32, w, h, "Midnight + RTXDI ReSTIR Practical Sconces ON | 32 SPP");

        FrameContext nightOff = nightOn;
        nightOff.interactive.lampsOff = true;
        BOO_CHECK(renderConvergedView(nightOff, 32u, nightLampsOff32, w, h));
        BOO_CHECK(writeCapturePng(outDir / "interactive_bathroom_midnight_lamps_off_32spp.png", nightLampsOff32, w, h));
        sheet.add(nightLampsOff32, w, h, "Midnight + Practical Lamps OFF [J] | 32 SPP");
        std::cout << "Captured Interactive Sun & RTXDI Practical Lamp sweep (4 lighting states)\n" << std::flush;
    }

    // 5. Draggable RTX ON / Raster OFF Split-Screen Wipe across Bathroom Mirror & Vanity (abSplitRatio = 0.42)
    {
        FrameContext wipeCtx = baseCtx;
        wipeCtx.interactive.exploreActive = true;
        wipeCtx.interactive.blendWeight = 1.0f;
        wipeCtx.interactive.activePreset = 1u;
        wipeCtx.interactive.eye[0] = 0.34f;    wipeCtx.interactive.eye[1] = 4.02f;    wipeCtx.interactive.eye[2] = 5.12f;
        wipeCtx.interactive.target[0] = -0.18f; wipeCtx.interactive.target[1] = 3.88f; wipeCtx.interactive.target[2] = 6.32f;
        wipeCtx.interactive.fovYRadians = 0.56f;
        wipeCtx.interactive.abSplitRatio = 0.42f;

        std::vector<uint8_t> cleanTmp, wipePres;
        uint32_t w = 0, h = 0;
        BOO_CHECK(renderConvergedView(wipeCtx, 24u, cleanTmp, w, h));
        wipeCtx.showOverlay = true;
        wipeCtx.hudMode = 2u;
        wipeCtx.interactive.accumulate = true;
        wipeCtx.interactive.accumFrame = 23u;
        wipeCtx.presentationFrame = 23u;
        fx.setAbCompareEngine(AbCompareEngine::RT);
        BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(wipeCtx, hud, 2560, 1440, wipePres, &error));
        BOO_CHECK(writeCapturePng(outDir / "interactive_bathroom_rtx_split_wipe_studio.png", wipePres, 2560, 1440));
        sheet.add(wipePres, 2560, 1440, "Draggable RTX ON | Raster OFF Split Curtain (42%)");
        std::cout << "Captured Draggable RTX ON / Raster OFF Split-Screen Wipe\n" << std::flush;
    }

    // 6. Interactive Marbles RTX Rigid-Body Physics Showcase in Superellipse Porcelain Basin (64 SPP NRD + Studio Bar)
    {
        auto evalBowlY = [](float x, float z) -> float {
            const float ax = std::abs(x - (-0.180f)) / 0.268f;
            const float az = std::abs(z - 6.245f) / 0.194f;
            const float u = std::pow(std::pow(ax, 2.45f) + std::pow(az, 2.45f), 1.0f / 2.45f);
            if (u <= 0.135f) return 3.575f;
            if (u <= 0.160f) {
                const float s = (u - 0.135f) / 0.025f;
                return 3.575f * (1.0f - s) + 3.569f * s;
            }
            if (u <= 0.985f) {
                const float t = std::clamp((u - 0.160f) / 0.825f, 0.0f, 1.0f);
                return 3.569f + 0.114f * std::pow(t, 2.32f);
            }
            return 3.550f;
        };

        FrameContext mCtx = baseCtx;
        mCtx.interactive.exploreActive = true;
        mCtx.interactive.blendWeight = 1.0f;
        mCtx.interactive.activePreset = 2u;
        mCtx.interactive.eye[0] = 0.09f;     mCtx.interactive.eye[1] = 3.89f;    mCtx.interactive.eye[2] = 5.74f;
        mCtx.interactive.target[0] = -0.15f; mCtx.interactive.target[1] = 3.63f; mCtx.interactive.target[2] = 6.27f;
        mCtx.interactive.fovYRadians = 0.46f;
        mCtx.interactive.marbleCount = 9u;

        struct MarbleSpec { float x, z, r; uint32_t mat; };
        static constexpr MarbleSpec kShowcaseMarbles[9] = {
            {-0.245f, 6.215f, 0.0225f, 35u}, // Cognac-amber glass in basin
            {-0.145f, 6.205f, 0.0210f,  4u}, // Celadon-jade glass in basin
            {-0.195f, 6.275f, 0.0235f,  3u}, // Liuli crystal glass in basin
            {-0.095f, 6.265f, 0.0195f,  8u}, // 24k brushed gold in basin
            {-0.275f, 6.275f, 0.0200f, 23u}, // Brushed champagne nickel in basin
            {-0.155f, 6.320f, 0.0190f, 31u}, // Honed Calacatta marble in basin
            { 0.165f, 6.175f, 0.0225f, 35u}, // Cognac-amber marble on sunlit Calacatta vanity
            { 0.235f, 6.145f, 0.0215f,  3u}, // Crystal caustic marble on sunlit Calacatta vanity
            { 0.145f, 6.105f, 0.0205f,  4u}, // Celadon jade marble on sunlit Calacatta vanity
        };
        for (uint32_t i = 0; i < 9u; ++i) {
            const float x = kShowcaseMarbles[i].x;
            const float z = kShowcaseMarbles[i].z;
            const float r = kShowcaseMarbles[i].r;
            const float y = evalBowlY(x, z) + r;
            mCtx.interactive.marblePosRad[i][0] = x;
            mCtx.interactive.marblePosRad[i][1] = y;
            mCtx.interactive.marblePosRad[i][2] = z;
            mCtx.interactive.marblePosRad[i][3] = r;
            mCtx.interactive.marblePrevPos[i][0] = x;
            mCtx.interactive.marblePrevPos[i][1] = y;
            mCtx.interactive.marblePrevPos[i][2] = z;
            mCtx.interactive.marbleRotQuat[i][3] = 1.0f;
            mCtx.interactive.marbleMat[i] = kShowcaseMarbles[i].mat;
        }

        std::vector<uint8_t> marblesClean, marblesPres, marblesReblurPres;
        uint32_t w = 0, h = 0;

        // Live Motion NRD::REBLUR_DIFFUSE_SPECULAR (4-frame temporal motion warmup -> RBLR Studio Bar badge!)
        {
            FrameContext reblurCtx = mCtx;
            reblurCtx.interactive.eye[0] = 0.24f;     reblurCtx.interactive.eye[1] = 3.94f;    reblurCtx.interactive.eye[2] = 5.58f;
            reblurCtx.interactive.target[0] = -0.06f; reblurCtx.interactive.target[1] = 3.63f; reblurCtx.interactive.target[2] = 6.24f;
            reblurCtx.interactive.fovYRadians = 0.50f;
            reblurCtx.interactive.accumulate = false;
            for (uint32_t f = 0; f < 4u; ++f) {
                reblurCtx.presentationFrame = f;
                reblurCtx.interactive.accumFrame = f;
                reblurCtx.interactive.eye[0] = 0.24f - 0.012f * static_cast<float>(f);
                for (uint32_t i = 0; i < 9u; ++i) {
                    const float baseX = kShowcaseMarbles[i].x;
                    reblurCtx.interactive.marblePrevPos[i][0] = baseX + 0.008f * static_cast<float>(f);
                    reblurCtx.interactive.marblePosRad[i][0]  = baseX + 0.008f * static_cast<float>(f + 1u);
                }
                if (f < 3u) {
                    std::vector<uint8_t> tmp;
                    BOO_CHECK(fx.renderHeadlessCanvasForTest(reblurCtx, tmp, w, h, &error));
                } else {
                    reblurCtx.showOverlay = true;
                    reblurCtx.hudMode = 1u;
                    BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(reblurCtx, hud, 2560, 1440, marblesReblurPres, &error));
                }
            }
            BOO_CHECK(writeCapturePng(outDir / "interactive_marbles_rtx_reblur_motion_studio.png", marblesReblurPres, 2560, 1440));
            sheet.add(marblesReblurPres, 2560, 1440, "Live Motion NRD REBLUR [RBLR] | Rolling Marbles Cascade");
        }

        BOO_CHECK(renderConvergedView(mCtx, 64u, marblesClean, w, h));
        BOO_CHECK(writeCapturePng(outDir / "interactive_marbles_rtx_basin_64spp_clean.png", marblesClean, w, h));

        mCtx.showOverlay = true;
        mCtx.hudMode = 1u;
        mCtx.interactive.accumulate = true;
        mCtx.interactive.accumFrame = 63u;
        mCtx.presentationFrame = 63u;
        BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(mCtx, hud, 2560, 1440, marblesPres, &error));
        BOO_CHECK(writeCapturePng(outDir / "interactive_marbles_rtx_basin_64spp_studio.png", marblesPres, 2560, 1440));
        sheet.add(marblesPres, 2560, 1440, "Interactive Marbles RTX [Y/M] | 64 SPP NRD + Caustics");
        std::cout << "Captured Interactive Marbles RTX Basin & Countertop Showcase (REBLUR + 64 SPP NRD)\n" << std::flush;

        // 7. Live Buffer / AOV Quad Inspection (World Normal | Indirect GI | Refl & Refr | DXR Ray Heatmap)
        FrameContext aovBase = mCtx;
        aovBase.showOverlay = true;
        aovBase.hudMode = 1u;
        aovBase.interactive.accumulate = false;
        aovBase.interactive.accumFrame = 0u;
        aovBase.presentationFrame = 0u;

        std::vector<uint8_t> aovNorm, aovGi, aovRefl, aovHeat;
        aovBase.interactive.aovMode = 2u;
        BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(aovBase, hud, 2560, 1440, aovNorm, &error));
        BOO_CHECK(writeCapturePng(outDir / "interactive_aov_world_normal.png", aovNorm, 2560, 1440));

        aovBase.interactive.aovMode = 5u;
        BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(aovBase, hud, 2560, 1440, aovGi, &error));
        BOO_CHECK(writeCapturePng(outDir / "interactive_aov_indirect_gi.png", aovGi, 2560, 1440));

        aovBase.interactive.aovMode = 6u;
        BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(aovBase, hud, 2560, 1440, aovRefl, &error));
        BOO_CHECK(writeCapturePng(outDir / "interactive_aov_reflections_refraction.png", aovRefl, 2560, 1440));

        aovBase.interactive.aovMode = 7u;
        BOO_CHECK(fx.renderHeadlessPresentedFrameForTest(aovBase, hud, 2560, 1440, aovHeat, &error));
        BOO_CHECK(writeCapturePng(outDir / "interactive_aov_ray_heatmap.png", aovHeat, 2560, 1440));

        // Compose 2x2 2560x1440 Live AOV Quad Inspection tile
        std::vector<uint8_t> aovQuad(2560u * 1440u * 4u, 255u);
        auto blitQuadrant = [&](const std::vector<uint8_t>& src, uint32_t qx, uint32_t qy) {
            const uint32_t qw = 1280u, qh = 720u;
            for (uint32_t y = 0; y < qh; ++y) {
                for (uint32_t x = 0; x < qw; ++x) {
                    const size_t s00 = ((y * 2u + 0u) * 2560u + (x * 2u + 0u)) * 4u;
                    const size_t s10 = ((y * 2u + 0u) * 2560u + (x * 2u + 1u)) * 4u;
                    const size_t s01 = ((y * 2u + 1u) * 2560u + (x * 2u + 0u)) * 4u;
                    const size_t s11 = ((y * 2u + 1u) * 2560u + (x * 2u + 1u)) * 4u;
                    const size_t dst = ((qy * qh + y) * 2560u + (qx * qw + x)) * 4u;
                    for (uint32_t c = 0; c < 3u; ++c) {
                        aovQuad[dst + c] = static_cast<uint8_t>(
                            (uint32_t(src[s00 + c]) + uint32_t(src[s10 + c]) + uint32_t(src[s01 + c]) + uint32_t(src[s11 + c]) + 2u) >> 2u);
                    }
                    aovQuad[dst + 3u] = 255u;
                }
            }
        };
        blitQuadrant(aovNorm, 0u, 0u);
        blitQuadrant(aovGi,   1u, 0u);
        blitQuadrant(aovRefl, 0u, 1u);
        blitQuadrant(aovHeat, 1u, 1u);
        BOO_CHECK(writeCapturePng(outDir / "interactive_aov_quad_inspection.png", aovQuad, 2560, 1440));
        sheet.add(aovQuad, 2560, 1440, "Live AOV Inspection [I/F9]: Normal | GI | Refl/Refr | Ray Heat");
        std::cout << "Captured Live Buffer / AOV Inspection Quad (Normal, GI, Refl/Refr, Ray Heatmap)\n" << std::flush;
    }

    BOO_CHECK(sheet.save(outDir / "interactive_showcase_montage.png", 2));
    report["complete"] = true;
    BOO_CHECK(writeFilmGateJson(outDir / "manifest.json", report));

    fx.shutdown();
    d3d.waitForGpu();
    BOO_CHECK(d3d.debugErrorCount() == 0);
    std::cout << "Interactive showcase captures saved to: " << outDir.string() << '\n';
    return true;
}
