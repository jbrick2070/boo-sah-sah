#include "boo/results_benchmark.hpp"
#include "boo/cues.hpp"
#include "boo/director.hpp"
#include "boo/render.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxcore.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <cstdio>
#include <thread>

namespace boo {
using Microsoft::WRL::ComPtr;
namespace {
constexpr uint64_t MiB = 1024ull * 1024ull;
// The full-film stress gate peaks at 1,637,195,776 bytes on this content revision.
// Reserve 2.5 GiB for the extra worker, including PT/DLSS, plus the paused viewer.
constexpr uint64_t preparationBudget = 2560 * MiB;
constexpr uint64_t betweenFrameMargin = 128 * MiB;
constexpr uint64_t renderFullBudget = 3584 * MiB;
constexpr uint32_t kRenderTestSamplesPerPixel = 24;
constexpr uint32_t kRenderTestRasterSamples = 32;
constexpr uint32_t kRenderTestTileSize = 64;

// A small, independent presentation window lets the parent keep its paused viewer.
// The copied canvas is displayed as a top-down DIB; no worker surface is shared.
class RenderProgressWindow {
public:
    bool initialize(bool trace, std::uintptr_t ownerWindow, bool tour = false) {
        // The worker does not create the main AppWindow, which normally sets this.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const HWND owner = reinterpret_cast<HWND>(ownerWindow);
        const HMONITOR monitor = MonitorFromWindow(IsWindow(owner) ? owner : nullptr, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(monitor, &mi)) return false;
        const UINT dpi = IsWindow(owner) ? GetDpiForWindow(owner) : GetDpiForSystem();
        const UINT effectiveDpi = dpi ? dpi : 96u;
        constexpr DWORD style = WS_OVERLAPPEDWINDOW, exStyle = WS_EX_TOPMOST;
        RECT desired{0, 0, MulDiv(1280, effectiveDpi, 96), MulDiv(720, effectiveDpi, 96)};
        if (!AdjustWindowRectExForDpi(&desired, style, FALSE, exStyle, effectiveDpi)) return false;
        const int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
        int windowW = desired.right - desired.left, windowH = desired.bottom - desired.top;
        if (windowW > workW || windowH > workH) {
            const double fit = (std::min)(static_cast<double>(workW) / windowW, static_cast<double>(workH) / windowH);
            RECT fitted{0, 0, (std::max)(1, static_cast<int>(MulDiv(1280, effectiveDpi, 96) * fit)),
                (std::max)(1, static_cast<int>(MulDiv(720, effectiveDpi, 96) * fit))};
            if (!AdjustWindowRectExForDpi(&fitted, style, FALSE, exStyle, effectiveDpi)) return false;
            windowW = (std::min)(workW, static_cast<int>(fitted.right - fitted.left));
            windowH = (std::min)(workH, static_cast<int>(fitted.bottom - fitted.top));
        }
        WNDCLASSW wc{};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"BooRenderTestProgress";
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        RegisterClassW(&wc);
        window_ = CreateWindowExW(exStyle, wc.lpszClassName, tour ? L"Film tour" : (trace ? L"Render test" : L"Render test (raster)"),
            style, mi.rcWork.left + (workW - windowW) / 2, mi.rcWork.top + (workH - windowH) / 2, windowW, windowH,
            nullptr, nullptr, wc.hInstance, this);
        if (!window_) return false;
        tour_ = tour;
        ShowWindow(window_, SW_SHOW);
        return true;
    }
    ~RenderProgressWindow() { if (window_) DestroyWindow(window_); }
    bool pump() {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return !cancelled_;
    }
    void update(const RenderTestImage& image, const RenderTile& tile, const BenchmarkSnapshot& state) {
        width_ = image.width; height_ = image.height; state_ = state;
        (void)tile;
        // This is the same post-tonemap, sRGB-encoded CanvasClean readback used
        // for the final image. DIBs require BGRA byte order.
        pixels_ = image.rgba;
        for (size_t i = 0; i + 3 < pixels_.size(); i += 4) std::swap(pixels_[i], pixels_[i + 2]);
        InvalidateRect(window_, nullptr, FALSE);
        UpdateWindow(window_);
    }
    // Blend two already retired, sRGB canvas readbacks. No measured GPU work is
    // submitted during this presentation-only transition.
    void blend(const RenderTestImage& from, const RenderTestImage& to, float amount) {
        width_ = to.width; height_ = to.height;
        pixels_.resize(to.rgba.size());
        for (size_t i = 0; i + 3 < pixels_.size(); i += 4) {
            for (size_t c = 0; c < 3; ++c) {
                const size_t source = i + 2 - c;
                pixels_[i + c] = static_cast<uint8_t>(from.rgba[source] * (1.0f - amount) + to.rgba[source] * amount + 0.5f);
            }
            pixels_[i + 3] = 255;
        }
        InvalidateRect(window_, nullptr, FALSE);
        UpdateWindow(window_);
    }
private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<RenderProgressWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<RenderProgressWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, w, l);
        if (message == WM_CLOSE || (message == WM_KEYDOWN && w == VK_ESCAPE)) {
            self->cancelled_ = true; return 0;
        }
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_DPICHANGED) {
            const auto* suggested = reinterpret_cast<const RECT*>(l);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                suggested->bottom - suggested->top, SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        if (message == WM_PAINT) { self->paint(hwnd); return 0; }
        return DefWindowProcW(hwnd, message, w, l);
    }
    void paint(HWND hwnd) {
        PAINTSTRUCT ps{}; HDC target = BeginPaint(hwnd, &ps);
        RECT client{}; GetClientRect(hwnd, &client);
        HDC dc = CreateCompatibleDC(target);
        BITMAPINFO backing{}; backing.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        backing.bmiHeader.biWidth = (std::max)(1L, client.right);
        backing.bmiHeader.biHeight = -(std::max)(1L, client.bottom);
        backing.bmiHeader.biPlanes = 1; backing.bmiHeader.biBitCount = 32; backing.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(target, &backing, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dc || !bitmap) {
            if (bitmap) DeleteObject(bitmap);
            if (dc) DeleteDC(dc);
            EndPaint(hwnd, &ps);
            return;
        }
        HGDIOBJ previous = SelectObject(dc, bitmap);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if (!pixels_.empty() && width_ && height_) {
            const int cw = client.right, ch = client.bottom;
            const double scale = (std::min)(static_cast<double>(cw) / width_, static_cast<double>(ch) / height_);
            const int dw = static_cast<int>(width_ * scale), dh = static_cast<int>(height_ * scale);
            const int x = (cw - dw) / 2, y = (ch - dh) / 2;
            BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = static_cast<LONG>(width_); bmi.bmiHeader.biHeight = -static_cast<LONG>(height_);
            bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, x, y, dw, dh, 0, 0, width_, height_, pixels_.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        wchar_t label[160]{};
        if (!tour_) swprintf_s(label, L"Tiles %u / %u    %u spp    %.1f s    Esc: cancel",
            state_.tilesDone, state_.tilesTotal, state_.samplesPerPixel, state_.renderSeconds);
        if (!tour_) {
            SetBkColor(dc, RGB(0, 0, 0)); SetTextColor(dc, RGB(255, 255, 255));
            TextOutW(dc, 12, 12, label, static_cast<int>(wcslen(label)));
        }
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
    }
    HWND window_{}; bool cancelled_{}, tour_{};
    uint32_t width_{}, height_{}; BenchmarkSnapshot state_{};
    std::vector<uint8_t> pixels_;
};

BenchmarkPower readPower() {
    SYSTEM_POWER_STATUS power{};
    if (!GetSystemPowerStatus(&power)) return BenchmarkPower::Unknown;
    if (power.ACLineStatus == 0) return BenchmarkPower::Battery;
    if (power.ACLineStatus == 1) return BenchmarkPower::Mains;
    return BenchmarkPower::Unknown;
}

BenchmarkPower updatePower(BenchmarkPower previous) {
    const auto now = readPower();
    if (previous == BenchmarkPower::Changed || previous == BenchmarkPower::Unknown) return previous;
    if (now == BenchmarkPower::Unknown) return now;
    return now == previous ? previous : BenchmarkPower::Changed;
}

std::shared_ptr<std::atomic<uint64_t>> lookupDriver(LUID luid) {
    auto output = std::make_shared<std::atomic<uint64_t>>(0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    // A slow optional driver query must never delay the pass or teardown.
    try {
        std::thread([luid, output, deadline] {
            HMODULE module = LoadLibraryExW(L"dxcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) return;
            {
                using CreateFactory = HRESULT (WINAPI*)(REFIID, void**);
                const auto create = reinterpret_cast<CreateFactory>(GetProcAddress(module, "DXCoreCreateAdapterFactory"));
                ComPtr<IDXCoreAdapterFactory> factory;
                ComPtr<IDXCoreAdapter> adapter;
                uint64_t version = 0;
                if (create && SUCCEEDED(create(IID_PPV_ARGS(&factory))) &&
                    SUCCEEDED(factory->GetAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) &&
                    SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::DriverVersion, &version)) &&
                    std::chrono::steady_clock::now() <= deadline) output->store(version);
            }
            FreeLibrary(module);
        }).detach();
    } catch (const std::system_error&) {}
    return output;
}

bool hasHeadroom(IDXGIAdapter3* adapter, uint64_t viewerUsage, uint64_t required) {
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)) ||
        memory.CurrentUsage > memory.Budget) return false;
    const uint64_t remaining = memory.Budget - memory.CurrentUsage;
    // DXGI accounts usage per process. The separate viewer stays allocated throughout this check.
    if (viewerUsage > remaining || required > remaining - viewerUsage) return false;
    MEMORYSTATUSEX system{sizeof(system)};
    return GlobalMemoryStatusEx(&system) && system.ullAvailPhys >= required;
}

FrameContext makeFrame(const ResultsSample& sample, BenchmarkMode mode, const SceneCue& cue,
                       ScriptedDirector& director, uint32_t actIndex = 0) {
    FrameContext frame{};
    frame.presentationFrame = sample.tag;
    frame.samplePosition = sample.sample48k;
    frame.actIndex = actIndex;
    frame.filmTimeSeconds = static_cast<double>(sample.sample48k) / frame.sampleRate;
    frame.dtSeconds = 1.0 / 30.0;
    frame.actProgress = static_cast<float>(sample.sample48k - cue.startSample) /
                        static_cast<float>(cue.endSample - cue.startSample);
    frame.randomSeed = cue.deterministicSeed;
    frame.qualityPreset = QualityPresetId::Safe;
    frame.effectsDetail = 0;
    frame.hudMode = 3;
    frame.showOverlay = false;
    frame.pathTracing = mode == BenchmarkMode::PathTracing ? PathTracingLevel::High : PathTracingLevel::Off;
    frame.dlssMode = mode == BenchmarkMode::DlssQuality ? DlssMode::Quality : DlssMode::Off;
    frame.rayReconstruction = false;
    frame.frameGeneration = FrameGenerationMode::Off;
    frame.reflex = ReflexMode::Off;
    const auto mood = director.evaluate(frame, &cue);
    frame.moodIrisAngle = mood.irisAngle;
    frame.moodBloomStrength = mood.bloomStrength;
    frame.moodIntensity = mood.lightTension;
    frame.moodPaletteAccent = mood.spectralShift;
    frame.moodCutAccent = mood.cutAccent;
    return frame;
}

BenchmarkReason measureRenderTest(const BenchmarkRequest& request, BenchmarkReporter& reporter,
                                  BenchmarkSnapshot& state, D3D12Context& device,
                                  std::unique_ptr<M1TaipeiSliceEffect>& scene) {
    const LUID luid{static_cast<DWORD>(request.luidLow), request.luidHigh};
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter3> adapter; DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return BenchmarkReason::UnsupportedDevice;
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) || FAILED(adapter->GetDesc1(&desc)))
        return BenchmarkReason::AdapterMismatch;
    state.vendorId = desc.VendorId; state.deviceId = desc.DeviceId;
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return BenchmarkReason::SoftwareRenderer;
    if (!hasHeadroom(adapter.Get(), request.viewerLocalUsage, preparationBudget)) return BenchmarkReason::MemoryBudget;
    const bool full = hasHeadroom(adapter.Get(), request.viewerLocalUsage, renderFullBudget);
    const uint32_t width = full ? 1920u : 1280u, height = full ? 1080u : 720u;
    const auto driver = lookupDriver(luid);
    device.selectBenchmarkAdapter(request);
    device.setRayTracingAllowed(true);
    if (!device.initialize(nullptr, width, height, false, false)) return BenchmarkReason::UnsupportedDevice;
    if (device.isWarp()) return BenchmarkReason::SoftwareRenderer;
    if (device.adapterLuidLow() != request.luidLow || device.adapterLuidHigh() != request.luidHigh)
        return BenchmarkReason::AdapterMismatch;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 rt{};
    auto* d3d = static_cast<ID3D12Device*>(device.deviceRaw());
    if (FAILED(d3d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &rt, sizeof(rt))))
        return BenchmarkReason::RendererFailed;
    const bool trace = rt.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    state.renderPathTraced = trace;
    if (!useInstalledAssetDirectory()) return BenchmarkReason::RendererFailed;
    CueSheet sheet; ValidationReport validation;
    if (!loadCueSheetFromFile(resolveResourcePath("assets/cues/film_360s.json"), sheet, validation) ||
        sheet.seed != 266203 || sheet.sampleRate != 48000 || sheet.edition != Edition::Family)
        return BenchmarkReason::WorkloadChanged;
    constexpr int64_t shot = 47 * 48000;
    const auto cueIndex = findSceneIndexAtSample(sheet, shot);
    if (cueIndex >= sheet.scenes.size() || sheet.scenes[cueIndex].sceneId != "mirror_intelligence")
        return BenchmarkReason::WorkloadChanged;
    const auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    auto safe = findQualityPresetSpec(presets, QualityPresetId::Safe);
    if (safe.id != QualityPresetId::Safe || safe.renderWidth != 1280 || safe.renderHeight != 720 ||
        safe.allowDxr || safe.allowNpu || safe.allowCuda || safe.allowTensor) return BenchmarkReason::WorkloadChanged;
    safe.renderWidth = width; safe.renderHeight = height; safe.allowDxr = trace;
    scene = std::make_unique<M1TaipeiSliceEffect>(device, safe, !trace, false, true, true, true, false,
                                                  true, false, ShowVisualMode::FullFilm360s);
    const auto init = scene->probeAndInitialize();
    state.driverVersion = driver->load();
    if (!device.checkDeviceHealthy()) return BenchmarkReason::GraphicsReset;
    if (init.status == BackendStatus::Faulted) return BenchmarkReason::RendererFailed;
    if (!scene->hasAllAuthoredSceneAssets() || scene->canvasWidth() != width || scene->canvasHeight() != height)
        return BenchmarkReason::WorkloadChanged;
    state.width = state.inputWidth = width; state.height = state.inputHeight = height;
    const auto tiles = renderTileOrder(width, height, kRenderTestTileSize);
    state.tilesTotal = static_cast<uint32_t>(tiles.size());
    const uint32_t spp = trace ? kRenderTestSamplesPerPixel : kRenderTestRasterSamples;
    state.samplesPerPixel = spp;
    scene->resetTo(47.0);
    ScriptedDirector director; director.reset(sheet.seed);
    const ResultsSample shotSample{0, shot, ResultsSampleKind::Measured};
    FrameContext frame = makeFrame(shotSample, trace ? BenchmarkMode::PathTracing : BenchmarkMode::Raster,
                                   sheet.scenes[cueIndex], director);
    frame.interactive.accumulate = true;
    frame.interactive.renderTileEnabled = true;
    RenderProgressWindow window;
    if (!window.initialize(trace, request.ownerWindow)) return BenchmarkReason::RendererFailed;
    state.stage = BenchmarkStage::Measuring;
    if (!reporter.publish(state)) return BenchmarkReason::Cancelled;
    const auto start = std::chrono::steady_clock::now();
    auto nextPresent = start;
    RenderTestImage image;
    for (const auto& tile : tiles) {
        frame.interactive.renderTileX = tile.x; frame.interactive.renderTileY = tile.y;
        frame.interactive.renderTileWidth = tile.width; frame.interactive.renderTileHeight = tile.height;
        for (uint32_t sample = 0; sample < spp; ++sample) {
            if (reporter.cancelled() || !window.pump()) return BenchmarkReason::Cancelled;
            if (std::chrono::steady_clock::now() - start >= std::chrono::minutes(10))
                return BenchmarkReason::TimeLimit;
            if (!device.checkDeviceHealthy()) return BenchmarkReason::GraphicsReset;
            frame.interactive.accumFrame = sample;
            frame.interactive.renderTileReset = state.tilesDone == 0 && sample == 0;
            // Vary the deterministic sub-pixel sequence while keeping film time and audio fixed.
            frame.presentationFrame = sample;
            double milliseconds{};
            const auto result = scene->drawRenderTestFrame(frame, milliseconds);
            if (result != BenchmarkReason::None) return result;
            state.gpuSeconds += milliseconds / 1000.0;
            const auto now = std::chrono::steady_clock::now();
            state.renderSeconds = std::chrono::duration<double>(now - start).count();
            if (now >= nextPresent) {
                if (!scene->readLastRenderedCanvasForTest(image.rgba, image.width, image.height) || !image.valid())
                    return BenchmarkReason::RendererFailed;
                window.update(image, tile, state);
                if (!reporter.publish(state)) return BenchmarkReason::Cancelled;
                nextPresent = now + std::chrono::milliseconds(100);
            }
        }
        ++state.tilesDone;
        state.power = updatePower(state.power);
        if (!hasHeadroom(adapter.Get(), request.viewerLocalUsage, betweenFrameMargin)) return BenchmarkReason::MemoryBudget;
    }
    // The last sample's timestamp ends the same wall-time interval shown to the user.
    const auto rate = renderMegaSamplesPerSecond(width, height, spp, state.renderSeconds);
    if (!rate) return BenchmarkReason::TimingUnavailable;
    state.megaSamplesPerSecond = *rate;
    if (!scene->readLastRenderedCanvasForTest(image.rgba, image.width, image.height) || !image.valid() ||
        !reporter.publishImage(image)) return BenchmarkReason::RendererFailed;
    window.update(image, tiles.back(), state);
    return BenchmarkReason::None;
}

BenchmarkReason measure(const BenchmarkRequest& request, BenchmarkReporter& reporter, BenchmarkSnapshot& state,
                        D3D12Context& device, std::unique_ptr<M1TaipeiSliceEffect>& scene,
                        ResultsMeasurements& timings) {
    if (request.mode == BenchmarkMode::RenderTest) return measureRenderTest(request, reporter, state, device, scene);
    if (request.mode != BenchmarkMode::Raster && request.mode != BenchmarkMode::PathTracing &&
        request.mode != BenchmarkMode::DlssQuality && request.mode != BenchmarkMode::FilmTour) return BenchmarkReason::UnsupportedDevice;
    const LUID luid{static_cast<DWORD>(request.luidLow), request.luidHigh};
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return BenchmarkReason::UnsupportedDevice;
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) || FAILED(adapter->GetDesc1(&desc)))
        return BenchmarkReason::AdapterMismatch;
    state.vendorId = desc.VendorId;
    state.deviceId = desc.DeviceId;
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) return BenchmarkReason::SoftwareRenderer;
    if (!hasHeadroom(adapter.Get(), request.viewerLocalUsage, preparationBudget)) return BenchmarkReason::MemoryBudget;
    const auto driver = lookupDriver(luid);
    if (!reporter.publish(state) || reporter.cancelled()) return BenchmarkReason::Cancelled;

    const bool trace = request.mode == BenchmarkMode::PathTracing;
    device.selectBenchmarkAdapter(request);
    device.setRayTracingAllowed(trace);
    if (!device.initialize(nullptr, 1280, 720, false, false)) return BenchmarkReason::UnsupportedDevice;
    if (device.isWarp()) return BenchmarkReason::SoftwareRenderer;
    if (device.adapterLuidLow() != request.luidLow || device.adapterLuidHigh() != request.luidHigh)
        return BenchmarkReason::AdapterMismatch;
    if (trace) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 rt{};
        auto* d3d = static_cast<ID3D12Device*>(device.deviceRaw());
        if (FAILED(d3d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &rt, sizeof(rt))))
            return BenchmarkReason::RendererFailed;
        if (rt.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) return BenchmarkReason::RequiresDxr;
    }
    if (request.mode == BenchmarkMode::DlssQuality) {
        if (!resultsDlssInBuild()) return BenchmarkReason::RequiresDlss;
        if (desc.VendorId != 0x10de) return BenchmarkReason::UnsupportedDevice;
    }
    if (reporter.cancelled()) return BenchmarkReason::Cancelled;
    if (!useInstalledAssetDirectory()) return BenchmarkReason::RendererFailed;
    CueSheet sheet;
    ValidationReport validation;
    if (!loadCueSheetFromFile(resolveResourcePath("assets/cues/film_360s.json"), sheet, validation) ||
        sheet.scenes.empty() || sheet.scenes[0].sceneId != "taipei_intro" || sheet.seed != 266203 ||
        sheet.sampleRate != 48000 || sheet.edition != Edition::Family || sheet.scenes[0].startSample != 0 ||
        sheet.scenes[0].endSample != 2160000 || !sheet.scenes[0].allowDirectorModulation)
        return BenchmarkReason::WorkloadChanged;
    const auto presets = loadQualityPresets(resolveResourcePath("config/quality_presets.json"));
    auto safe = findQualityPresetSpec(presets, QualityPresetId::Safe);
    if (safe.id != QualityPresetId::Safe || safe.renderWidth != 1280 || safe.renderHeight != 720 ||
        safe.allowDxr || safe.allowNpu || safe.allowCuda || safe.allowTensor) return BenchmarkReason::WorkloadChanged;
    safe.allowDxr = trace;
    scene = std::make_unique<M1TaipeiSliceEffect>(device, safe, !trace, false, true, true, true, false,
                                                  true, false, ShowVisualMode::FullFilm360s);
    const auto init = scene->probeAndInitialize();
    state.driverVersion = driver->load();
    if (!device.checkDeviceHealthy()) return BenchmarkReason::GraphicsReset;
    if (init.status == BackendStatus::Faulted) return BenchmarkReason::RendererFailed;
    if (!scene->hasAllAuthoredSceneAssets()) return BenchmarkReason::WorkloadChanged;
    state.width = scene->canvasWidth();
    state.height = scene->canvasHeight();
    if (state.width != 1280 || state.height != 720) return BenchmarkReason::WorkloadChanged;
    if (request.mode == BenchmarkMode::DlssQuality) {
        const auto support = scene->dlssBenchmarkAvailability();
        if (support != BenchmarkReason::None) return support;
    }

    if (request.mode == BenchmarkMode::FilmTour) {
        if (sheet.durationSamples != 360LL * 48000) return BenchmarkReason::WorkloadChanged;
        RenderProgressWindow window;
        if (!window.initialize(false, request.ownerWindow, true)) return BenchmarkReason::RendererFailed;
        RenderTestImage previous;
        state.stage = BenchmarkStage::Measuring;
        if (!reporter.publish(state)) return BenchmarkReason::Cancelled;
        for (uint32_t shot = 0; shot < kFilmTourSceneCount; ++shot) {
            state.tourScene = shot;
            state.tourWarmupCount = state.tourMeasuredCount = 0;
            scene->resetTo(kFilmTourTimesSeconds[shot]);
            ScriptedDirector director;
            director.reset(sheet.seed);
            const auto first = *filmTourSampleAt(shot, 0);
            const size_t cueIndex = findSceneIndexAtSample(sheet, first.sample48k);
            if (cueIndex >= sheet.scenes.size() || first.sample48k < sheet.scenes[cueIndex].startSample ||
                first.sample48k >= sheet.scenes[cueIndex].endSample) return BenchmarkReason::WorkloadChanged;
            const FrameContext frozenFrame = makeFrame(first, request.mode, sheet.scenes[cueIndex], director,
                                                        static_cast<uint32_t>(cueIndex));
            std::array<double, kFilmTourMeasuredCount> values{};
            auto sceneStart = std::chrono::steady_clock::now();
            for (uint32_t index = 0; index < kFilmTourFramesPerScene; ++index) {
                if (reporter.cancelled() || !window.pump()) return BenchmarkReason::Cancelled;
                if (!device.checkDeviceHealthy()) return BenchmarkReason::GraphicsReset;
                if (!hasHeadroom(adapter.Get(), request.viewerLocalUsage, betweenFrameMargin)) return BenchmarkReason::MemoryBudget;
                const auto sample = *filmTourSampleAt(shot, index);
                FrameContext frame = frozenFrame;
                frame.presentationFrame = sample.tag;
                double milliseconds = 0;
                const auto reason = scene->drawBenchmarkFrame(frame, sample, state, milliseconds);
                if (reason != BenchmarkReason::None) return reason;
                if (index + 1 == kFilmTourWarmupCount) {
                    RenderTestImage current;
                    if (!scene->readLastRenderedCanvasForTest(current.rgba, current.width, current.height) ||
                        !current.valid()) return BenchmarkReason::RendererFailed;
                    if (shot) {
                        if (previous.width != current.width || previous.height != current.height) return BenchmarkReason::WorkloadChanged;
                        const auto fadeStart = std::chrono::steady_clock::now();
                        // Do not skip blend steps if the CPU or window is slow: a
                        // longer fade is preferable to an abrupt brightness jump.
                        for (uint32_t step = 1; step <= 15; ++step) {
                            const auto due = fadeStart + std::chrono::milliseconds(step * 500 / 15);
                            while (std::chrono::steady_clock::now() < due) {
                                if (reporter.cancelled() || !window.pump()) return BenchmarkReason::Cancelled;
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                            }
                            if (reporter.cancelled() || !window.pump()) return BenchmarkReason::Cancelled;
                            window.blend(previous, current, static_cast<float>(step) / 15.0f);
                        }
                    } else window.blend(current, current, 1.0f);
                    previous = std::move(current);
                    sceneStart = std::chrono::steady_clock::now();
                }
                if (sample.kind == ResultsSampleKind::Warmup) ++state.tourWarmupCount;
                else {
                    if (!std::isfinite(milliseconds) || milliseconds < 0) return BenchmarkReason::TimingUnavailable;
                    values[index - kFilmTourWarmupCount] = milliseconds;
                    ++state.tourMeasuredCount;
                }
                ++state.tourFramesDone;
                state.power = updatePower(state.power);
                if (!reporter.publish(state)) return BenchmarkReason::Cancelled;
            }
            std::sort(values.begin(), values.end());
            state.tourMediansMs[shot] = values[44] * 0.5 + values[45] * 0.5;
            state.tourWorstMs[shot] = values.back();
            if (state.tourMediansMs[shot] <= 0) return BenchmarkReason::TimingUnavailable;
            // Leave the frozen composition on screen for four seconds even on a
            // very fast GPU. The wait and the crossfade are outside timing queries.
            while (std::chrono::steady_clock::now() - sceneStart < std::chrono::seconds(4)) {
                if (reporter.cancelled() || !window.pump()) return BenchmarkReason::Cancelled;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        const auto score = filmTourGeometricMean(state.tourMediansMs);
        if (!score) return BenchmarkReason::TimingUnavailable;
        state.medianMs = *score;
        state.worstMs = *std::max_element(state.tourWorstMs.begin(), state.tourWorstMs.end());
        return BenchmarkReason::None;
    }

    scene->resetTo(7.0);
    ScriptedDirector director;
    director.reset(sheet.seed);
    for (size_t index = 0; index < kResultsSampleCount; ++index) {
        const auto sample = *resultsSampleAt(index);
        const auto stage = sample.kind == ResultsSampleKind::Warmup ? BenchmarkStage::Warming : BenchmarkStage::Measuring;
        if (state.stage != stage) {
            state.stage = stage;
            if (!reporter.publish(state)) return BenchmarkReason::Cancelled;
        }
        if (reporter.cancelled()) return BenchmarkReason::Cancelled;
        if (!device.checkDeviceHealthy()) return BenchmarkReason::GraphicsReset;
        if (!hasHeadroom(adapter.Get(), request.viewerLocalUsage, betweenFrameMargin)) return BenchmarkReason::MemoryBudget;
        const FrameContext frame = makeFrame(sample, request.mode, sheet.scenes[0], director);
        double milliseconds = 0;
        const auto reason = scene->drawBenchmarkFrame(frame, sample, state, milliseconds);
        if (reason != BenchmarkReason::None) return reason;
        if (sample.kind == ResultsSampleKind::Warmup) ++state.warmupCount;
        else {
            if (!timings.record(sample.tag, milliseconds)) return BenchmarkReason::TimingUnavailable;
            state.measuredCount = static_cast<uint32_t>(timings.validSampleCount());
        }
        state.power = updatePower(state.power);
        if (!reporter.publish(state)) return BenchmarkReason::Cancelled;
    }
    return BenchmarkReason::None;
}
} // namespace

bool resultsDlssInBuild() noexcept {
#ifdef BOO_HAS_DLSS
    return true;
#else
    return false;
#endif
}

void runResultsBenchmark(const BenchmarkRequest& request, BenchmarkReporter& reporter) {
    BenchmarkSnapshot state{};
    state.mode = request.mode;
    state.power = readPower();
    if (!reporter.publish(state)) return;
    ResultsMeasurements timings;
    D3D12Context device;
    std::unique_ptr<M1TaipeiSliceEffect> scene;
    {
        try {
            state.reason = measure(request, reporter, state, device, scene, timings);
        } catch (const BenchmarkWorkloadChanged&) {
            state.reason = BenchmarkReason::WorkloadChanged;
        } catch (const std::bad_alloc&) {
            state.reason = BenchmarkReason::MemoryBudget;
        } catch (...) {
            state.reason = device.deviceRaw() && !device.checkDeviceHealthy() ?
                BenchmarkReason::GraphicsReset : BenchmarkReason::RendererFailed;
        }
        state.stage = BenchmarkStage::Finishing;
        reporter.publish(state);
        if (device.deviceRaw() && !device.retireGpuForShutdown()) state.reason = BenchmarkReason::GraphicsReset;
    }
    state.power = updatePower(state.power);
    auto outcome = ResultsOutcome::Unavailable;
    if (state.reason == BenchmarkReason::None) outcome = ResultsOutcome::Completed;
    else if (state.reason == BenchmarkReason::Cancelled) outcome = ResultsOutcome::Cancelled;
    else if (state.reason == BenchmarkReason::GraphicsReset || state.reason == BenchmarkReason::WorkloadChanged ||
             (state.reason == BenchmarkReason::MemoryBudget && (state.warmupCount != 0 || state.tilesDone != 0 ||
                                                                 state.tourFramesDone != 0))) outcome = ResultsOutcome::Interrupted;
    if (request.mode == BenchmarkMode::RenderTest || request.mode == BenchmarkMode::FilmTour) {
        state.outcome = outcome;
        state.stage = BenchmarkStage::Finished;
        reporter.finish(state);
        return;
    }
    const auto result = timings.finish(outcome);
    state.outcome = result.outcome;
    if (result.summary) {
        state.medianMs = result.summary->medianMs;
        state.worstMs = result.summary->worstMs;
    } else if (state.reason == BenchmarkReason::None) state.reason = BenchmarkReason::TimingUnavailable;
    state.stage = BenchmarkStage::Finished;
    reporter.finish(state);
}
} // namespace boo
