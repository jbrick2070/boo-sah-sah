#pragma once

#include "boo/api.hpp"

#include <cstdint>
#include <deque>
#include <string>

struct HWND__;
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
    ToggleFullscreen,
    SelectModeFull,
    SelectModeRT,
    SelectModeTensor,
    SelectModeNPU,
    SelectModeCUDA,
    SelectModeFinale,
    StepBack, StepForward, SeekBack, SeekForward, SeekBackLong, SeekForwardLong,
    RevealPreroll, VolumeDown, VolumeUp, CaptureClean,
    Quit
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

    // Pumps Win32 messages without blocking; returns false when WM_QUIT received.
    bool pumpMessages();

    bool popCommand(TransportCommand& outCmd);
    void setTitle(const std::string& utf8Title);
    void toggleBorderlessFullscreen();

    HWND hwnd() const noexcept { return hwnd_; }
    uint32_t clientWidth() const noexcept { return clientWidth_; }
    uint32_t clientHeight() const noexcept { return clientHeight_; }
    bool isMinimized() const noexcept { return minimized_; }
    bool isFullscreen() const noexcept { return fullscreen_; }
    bool consumeResizeFlag() noexcept;

    void pushCommandFromWndProc(TransportCommand cmd);
    void onResizeFromWndProc(uint32_t w, uint32_t h, bool minimized);

private:
    HWND hwnd_ = nullptr;
    uint32_t clientWidth_ = 1920;
    uint32_t clientHeight_ = 1080;
    bool fullscreen_ = false;
    bool minimized_ = false;
    bool resized_ = false;
    int windowedRect_[4] = {100, 100, 1380, 820};
    std::deque<TransportCommand> commandQueue_;
};

} // namespace boo
