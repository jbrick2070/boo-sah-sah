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
