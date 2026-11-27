#pragma once

#include "boo/api.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct HWND__;
struct HDC__;
struct tagRECT;
using HWND = HWND__*;

namespace boo {

// Noninteractive demoscene transport & diagnostic commands only (Section 0.2 & 4.1)
enum class TransportCommand : uint8_t {
    None = 0,
    TogglePlayPause,
    Restart,
    NextAct,
    PrevAct,
    ToggleLoop,
    ToggleMute,
    CycleQualityPreset,
    CycleEffectsDetail,
    CyclePathTracing, CycleDlss,
    ToggleRayReconstruction, CycleFrameGeneration, CycleReflex,
    ToggleEdition,
    ToggleCaptions,
    ToggleOverlay,
    TriggerBenchSweep,
    ToggleRtRuntime,
    ToggleCudaRuntime,
    ToggleTensorRuntime,
    ToggleNpuRuntime,
    ToggleCodecRuntime,
    ToggleSpoutRuntime,
    ToggleCameraRuntime,
    CycleAbCompareEngine,
    CompareRT, CompareCUDA, CompareTensor, CompareCodec, CompareNPU, CloseComparison,
    ToggleFullscreen,
    LeaveFullscreen, // Esc while full screen: back to the window, never out of the app
    SelectModeFull,
    SelectModeRT,
    SelectModeTensor,
    SelectModeNPU,
    SelectModeCUDA,
    SelectModeFinale,
    StepBack, StepForward, SeekBack, SeekForward, SeekBackLong, SeekForwardLong,
    RevealPreroll, VolumeDown, VolumeUp, CaptureClean,
    // Interactive NVIDIA-RTX-style Explore & Studio commands
    ToggleExploreMode,
    TogglePracticalLamps,
    ToggleRtxSplitWipe,
    ResetInteractiveLighting,
    HeroPreset1,
    HeroPreset2,
    HeroPreset3,
    HeroPreset4,
    HeroPreset5,
    HeroPreset6,
    CycleAovMode,
    ResetAovMode,
    SpawnMarbles,
    ClearMarbles,
    SoloEngineRT,
    SoloEngineTensor,
    SoloEngineNPU,
    SoloEngineCUDA,
    SoloEngineCodec,
    RestoreAllEngines,
    NextMusicTrack,
    PrevMusicTrack,
    OpenAudioFileDialog,
    ClearCustomAudio,
    // Neural rendering suite (dashboard NEURAL rows, Shift+N)
    ToggleNeuralRendering, ToggleNeuralTensorCores,
    CycleDashboardPanel, DashboardFull, DashboardMini, DashboardHidden,
    GpuWorkoutDown, GpuWorkoutUp, CycleFlockFormation, ToggleBirdCam, CyclePhysxGenre,
    ToggleNeuralShading, ToggleNeuralTextures, ToggleNeuralCache,
    SoloNeuralShading, SoloNeuralTextures, SoloNeuralCache,
    Quit
};

struct WindowInputSnapshot {
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    float dragRmbDx = 0.0f;
    float dragRmbDy = 0.0f;
    float dragLmbDx = 0.0f;
    float dragLmbDy = 0.0f;
    float wheelNotches = 0.0f;
    bool lmbDown = false;
    bool lmbJustPressed = false;
    bool rmbDown = false;
    bool rmbJustPressed = false;
    bool mmbJustPressed = false;
    bool altDown = false;
    bool shiftDown = false;
    bool hasFocus = false;
    // Continuous 6-DOF flight axes when window has focus & Explore mode is active
    float moveForward = 0.0f; // W (+1) / S (-1)
    float moveRight   = 0.0f; // D (+1) / A (-1)
    float moveUp      = 0.0f; // E,PgUp (+1) / Q,PgDn,Ctrl (-1)
    float sunAzimKey  = 0.0f; // ] (+1) / [ (-1) when in Explore mode
    float sunElevKey  = 0.0f; // Shift+] (+1) / Shift+[ (-1)
};

// One line of the loading screen's list of start-up steps.
struct LoadingLine {
    std::wstring when, what, took;
    bool running = false;
    bool operator==(const LoadingLine&) const = default;
};

class AppWindow {
public:
    AppWindow();
    ~AppWindow();

    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    bool create(
        const std::string& utf8Title,
        uint32_t clientWidth,
        uint32_t clientHeight,
        bool fullscreen,
        bool noActivate);

    void destroy() noexcept;
    // Takes the window off the screen and the taskbar at once, while the app finishes closing.
    void hide() noexcept;

    // Pumps Win32 messages without blocking; returns false when WM_QUIT received.
    bool pumpMessages();

    bool popCommand(TransportCommand& outCmd);
    bool popDroppedFile(std::wstring& outPath);
    void onDroppedFileFromWndProc(std::wstring path);
    void setTitle(const std::string& utf8Title);
    void toggleBorderlessFullscreen();
    void showGraphicsFailure(const std::string& reason);
    bool paintGraphicsFailureFromWndProc();
    // GDI "Loading..." screen shown from window creation until the first frame is presented.
    void showLoading();
    void hideLoading() noexcept { loading_ = false; }
    // The loading bar: how full it is (0..1), the line of figures under it, the time since start-up
    // began, a notice (empty for none) and the list of steps. Repaints at once, without pumping
    // messages, so it can be called from the middle of start-up.
    void setLoadingProgress(float fraction, std::wstring detail, std::wstring clock = {}, std::wstring notice = {},
                            std::vector<LoadingLine> lines = {});
    bool paintLoadingFromWndProc();
    // Closed while loading: the window is hidden at once and start-up ends as soon as it can.
    bool closeWhileLoadingFromWndProc() noexcept;
    bool closeRequested() const noexcept { return closeRequested_; }
    void paintLoadingList(::HDC__* dc, const ::tagRECT& client, int top, int dpi);
    void onDestroyedFromWndProc() noexcept { hwnd_ = nullptr; }
    void fitFullscreenToMonitor();

    HWND hwnd() const noexcept { return hwnd_; }
    uint32_t clientWidth() const noexcept { return clientWidth_; }
    uint32_t clientHeight() const noexcept { return clientHeight_; }
    bool isMinimized() const noexcept { return minimized_; }
    bool isFullscreen() const noexcept { return fullscreen_; }
    bool consumeResizeFlag() noexcept;

    void pushCommandFromWndProc(TransportCommand cmd);
    void onResizeFromWndProc(uint32_t w, uint32_t h, bool minimized);

    void setExploreKeyCapture(bool enabled) noexcept { exploreKeyCapture_ = enabled; }
    bool exploreKeyCapture() const noexcept { return exploreKeyCapture_; }
    WindowInputSnapshot consumeInputSnapshot() noexcept;
    void onMouseButtonFromWndProc(int button, bool down, int x, int y);
    void onMouseMoveFromWndProc(int x, int y);
    void onMouseWheelFromWndProc(int delta);
    void onFocusLostFromWndProc() noexcept;

private:
    HWND hwnd_ = nullptr;
    uint32_t clientWidth_ = 1920;
    uint32_t clientHeight_ = 1080;
    bool fullscreen_ = false;
    bool minimized_ = false;
    bool resized_ = false;
    bool exploreKeyCapture_ = false;
    int lastMouseX_ = 0;
    int lastMouseY_ = 0;
    bool hasMousePos_ = false;
    bool lmbDown_ = false;
    bool rmbDown_ = false;
    bool lmbJustPressed_ = false;
    bool rmbJustPressed_ = false;
    bool mmbJustPressed_ = false;
    float accumRmbDx_ = 0.0f;
    float accumRmbDy_ = 0.0f;
    float accumLmbDx_ = 0.0f;
    float accumLmbDy_ = 0.0f;
    float accumWheel_ = 0.0f;
    int windowedRect_[4] = {100, 100, 1380, 820};
    std::deque<TransportCommand> commandQueue_;
    std::deque<std::wstring> droppedFiles_;
    std::wstring graphicsFailureText_;
    bool loading_ = false;
    bool closeRequested_ = false;
    float loadingFraction_ = -1.0f; // below zero: no bar yet
    std::wstring loadingDetail_;
    std::wstring loadingClock_;
    std::wstring loadingNotice_;
    std::vector<LoadingLine> loadingLines_;
};

} // namespace boo
