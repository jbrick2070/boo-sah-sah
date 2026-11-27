#include "boo/window.hpp"
#include "boo/localization.hpp"
#include "boo/results_text.hpp"
#include "world_portal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace boo {

namespace {
constexpr const wchar_t* kWindowClassName = L"BooSahSahGlassOrchidWindowClass";
constexpr UINT kMapAttributionCommand = 0x1D00;
constexpr UINT kWorldPortalCommand = 0x1D10;
constexpr UINT kFilmBenchmarkCommand = 0x1D20;
constexpr const wchar_t* kMapCredit = L"© OpenStreetMap contributors";

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

LRESULT CALLBACK mainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    auto* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_SIZE: {
            const UINT w = LOWORD(lParam);
            const UINT h = HIWORD(lParam);
            const bool min = (wParam == SIZE_MINIMIZED) || (w == 0) || (h == 0);
            self->onResizeFromWndProc(w, h, min);
            return 0;
        }
        case WM_DPICHANGED: {
            if (self->isFullscreen()) {
                self->fitFullscreenToMonitor();
            } else if (lParam != 0) {
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(
                    hwnd,
                    nullptr,
                    suggested->left,
                    suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0u) == kFilmBenchmarkCommand) {
                self->pushCommandFromWndProc(TransportCommand::RunFilmBenchmark);
                return 0;
            }
            if ((wParam & 0xFFF0u) == kWorldPortalCommand && worldPortalDialogAvailable()) {
                openWorldPortalDialog(hwnd);
                return 0;
            }
            if ((wParam & 0xFFF0u) == kMapAttributionCommand) {
                std::wstring credit =
                    L"Taipei · 臺北\n\n© OpenStreetMap contributors\n"
                    L"https://www.openstreetmap.org/copyright\nOpen Data Commons ODbL 1.0\n\n"
                    L"SRTM · NASA / NGA / USGS · Mapzen Terrain Tiles\n"
                    L"https://registry.opendata.aws/terrain-tiles/\n\n"
                    L"map-data/TAIPEI-MAP-NOTICE.txt\n\n"
                    L"Taipei 101 · 台北 101\nC.Y. Lee & Partners · 李祖原聯合建築師事務所\nhttps://www.cylee.com/project/Taipei-101\n\n";
                credit += uiText(UiText::IndependentProduction);
                MessageBoxW(hwnd, credit.c_str(), L"BOO SAH SAH · Taipei", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            break;
        case WM_DISPLAYCHANGE:
            if (self->isFullscreen()) self->fitFullscreenToMonitor();
            break;
        case WM_PAINT:
            if (self->paintGraphicsFailureFromWndProc()) return 0;
            if (self->paintLoadingFromWndProc()) return 0;
            break;
        case WM_SYSKEYDOWN: {
            if (wParam == VK_RETURN && (lParam & (1 << 29)) && !(lParam & 0x40000000)) {
                self->pushCommandFromWndProc(TransportCommand::ToggleFullscreen);
                return 0;
            }
            if (wParam == VK_MENU) return 0; // Keep Alt available as sun-orbit / slow-camera modifier without menu freeze
            break;
        }
        case WM_SYSKEYUP: {
            if (wParam == VK_MENU) return 0;
            break;
        }
        case WM_LBUTTONDOWN:
            self->onMouseButtonFromWndProc(0, true, static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            return 0;
        case WM_LBUTTONUP:
            self->onMouseButtonFromWndProc(0, false, static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            return 0;
        case WM_RBUTTONDOWN:
            self->onMouseButtonFromWndProc(1, true, static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            return 0;
        case WM_RBUTTONUP:
            self->onMouseButtonFromWndProc(1, false, static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            return 0;
        case WM_MBUTTONDOWN:
            self->onMouseButtonFromWndProc(2, true, static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            return 0;
        case WM_MOUSEMOVE:
            self->onMouseMoveFromWndProc(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
            return 0;
        case WM_MOUSEWHEEL:
            self->onMouseWheelFromWndProc(GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        case WM_KILLFOCUS:
        case WM_CAPTURECHANGED:
            self->onFocusLostFromWndProc();
            break;
        case WM_KEYDOWN: {
            const bool repeat = (lParam & 0x40000000) != 0;
            const bool inExplore = self->exploreKeyCapture();
            if (!repeat) {
                switch (wParam) {
                    case VK_LEFT: self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::SeekBackLong : TransportCommand::SeekBack); return 0;
                    case VK_RIGHT: self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::SeekForwardLong : TransportCommand::SeekForward); return 0;
                    case VK_OEM_COMMA: self->pushCommandFromWndProc(TransportCommand::StepBack); return 0;
                    case VK_OEM_4: if (!inExplore) self->pushCommandFromWndProc(TransportCommand::StepBack); return 0;
                    case VK_OEM_PERIOD: self->pushCommandFromWndProc(TransportCommand::StepForward); return 0;
                    case VK_OEM_6: if (!inExplore) self->pushCommandFromWndProc(TransportCommand::StepForward); return 0;
                    case 'G': self->pushCommandFromWndProc(TransportCommand::RevealPreroll); return 0;
                    case VK_OEM_3: self->pushCommandFromWndProc(TransportCommand::CycleDashboardPanel); return 0; // ` key
                    case '7': self->pushCommandFromWndProc(TransportCommand::CycleEffectsDetail); return 0;
                    case '8': self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::CycleReflex : TransportCommand::CyclePathTracing); return 0;
                    case '9': self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::CycleFrameGeneration : TransportCommand::CycleDlss); return 0;
                    case '0': self->pushCommandFromWndProc(TransportCommand::ToggleRayReconstruction); return 0;
                    case 'F':
                        self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) && !inExplore ? TransportCommand::CycleFlockFormation
                                                                                                   : TransportCommand::ToggleExploreMode);
                        return 0;
                    case VK_BACK: if (inExplore) self->pushCommandFromWndProc(TransportCommand::ToggleExploreMode); return 0;
                    case 'J': self->pushCommandFromWndProc(TransportCommand::TogglePracticalLamps); return 0;
                    case 'H': self->pushCommandFromWndProc(TransportCommand::ToggleRtxSplitWipe); return 0;
                    case VK_F2: self->pushCommandFromWndProc(TransportCommand::HeroPreset1); return 0;
                    case VK_F3: self->pushCommandFromWndProc(TransportCommand::HeroPreset2); return 0;
                    case VK_F4: self->pushCommandFromWndProc(TransportCommand::HeroPreset3); return 0;
                    case VK_F5: self->pushCommandFromWndProc(TransportCommand::HeroPreset4); return 0;
                    case VK_F6: self->pushCommandFromWndProc(TransportCommand::HeroPreset5); return 0;
                    case VK_F7: self->pushCommandFromWndProc(TransportCommand::HeroPreset6); return 0;
                    case VK_F8: self->pushCommandFromWndProc(TransportCommand::ResetInteractiveLighting); return 0;
                    case 'I':
                    case VK_F9:
                        self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::ResetAovMode : TransportCommand::CycleAovMode);
                        return 0;
                    case 'Y':
                    case VK_F10:
                        self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::ClearMarbles : TransportCommand::SpawnMarbles);
                        return 0;
                    case 'Z':
                        self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::PrevMusicTrack : TransportCommand::NextMusicTrack);
                        return 0;
                    case VK_DOWN:
                        if (GetKeyState(VK_SHIFT) & 0x8000) { self->pushCommandFromWndProc(TransportCommand::GpuWorkoutDown); return 0; }
                        [[fallthrough]];
                    case VK_OEM_MINUS: self->pushCommandFromWndProc(TransportCommand::VolumeDown); return 0;
                    case VK_UP:
                        if (GetKeyState(VK_SHIFT) & 0x8000) { self->pushCommandFromWndProc(TransportCommand::GpuWorkoutUp); return 0; }
                        [[fallthrough]];
                    case VK_OEM_PLUS: self->pushCommandFromWndProc(TransportCommand::VolumeUp); return 0;
                    case VK_F12: self->pushCommandFromWndProc(TransportCommand::CaptureClean); return 0;
                    // Esc gets you out of full screen first. Only from a window does it close the app.
                    case VK_ESCAPE:
                        self->pushCommandFromWndProc(self->isFullscreen() ? TransportCommand::LeaveFullscreen
                                                                          : TransportCommand::Quit);
                        return 0;
                    case VK_SPACE:  self->pushCommandFromWndProc(TransportCommand::TogglePlayPause); return 0;
                    case 'R':
                    case VK_HOME:   self->pushCommandFromWndProc(TransportCommand::Restart); return 0;
                    case 'N':
                        self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::ToggleNeuralRendering
                                                                                      : TransportCommand::NextAct);
                        return 0;
                    case 'P':
                        self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::CyclePhysxGenre
                                                                                      : TransportCommand::PrevAct);
                        return 0;
                    case 'L':
                        if ((GetKeyState(VK_CONTROL) & 0x8000) && worldPortalDialogAvailable()) {
                            openWorldPortalDialog(hwnd);
                        } else {
                            self->pushCommandFromWndProc(TransportCommand::ToggleLoop);
                        }
                        return 0;
                    case 'M':
                        if (inExplore) {
                            self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::ClearMarbles : TransportCommand::SpawnMarbles);
                        } else {
                            self->pushCommandFromWndProc(TransportCommand::ToggleMute);
                        }
                        return 0;
                    case 'Q':       if (!inExplore) self->pushCommandFromWndProc(TransportCommand::CycleQualityPreset); return 0;
                    case 'E':       if (!inExplore) self->pushCommandFromWndProc(TransportCommand::ToggleEdition); return 0;
                    case 'K':       self->pushCommandFromWndProc(TransportCommand::ToggleCaptions); return 0;
                    case VK_TAB:
                    case VK_F1:     self->pushCommandFromWndProc(TransportCommand::ToggleOverlay); return 0;
                    case 'B':
                        if (GetKeyState(VK_CONTROL) & 0x8000) {
                            self->pushCommandFromWndProc(TransportCommand::RunFilmBenchmark);
                            return 0;
                        }
                        self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::ToggleBirdCam
                                                                                      : TransportCommand::TriggerBenchSweep);
                        return 0;
                    case 'X':       self->pushCommandFromWndProc(TransportCommand::ToggleRtRuntime); return 0;
                    case 'C':       self->pushCommandFromWndProc(TransportCommand::ToggleCudaRuntime); return 0;
                    case 'T':       self->pushCommandFromWndProc(TransportCommand::ToggleTensorRuntime); return 0;
                    case 'U':       self->pushCommandFromWndProc(TransportCommand::ToggleNpuRuntime); return 0;
                    case 'V':       self->pushCommandFromWndProc(TransportCommand::ToggleCodecRuntime); return 0;
                    case 'O':       self->pushCommandFromWndProc(TransportCommand::ToggleSpoutRuntime); return 0;
                    case 'W':       if (!inExplore) self->pushCommandFromWndProc(TransportCommand::ToggleCameraRuntime); return 0;
                    case 'A':       if (!inExplore) self->pushCommandFromWndProc(TransportCommand::CycleAbCompareEngine); return 0;
                    case 'S':
                    case 'D':       if (inExplore) return 0; break;
                    case VK_F11:    self->pushCommandFromWndProc(TransportCommand::ToggleFullscreen); return 0;
                    case '1':       self->pushCommandFromWndProc(TransportCommand::SelectModeFull); return 0;
                    case '2':       self->pushCommandFromWndProc(TransportCommand::SelectModeRT); return 0;
                    case '3':       self->pushCommandFromWndProc(TransportCommand::SelectModeTensor); return 0;
                    case '4':       self->pushCommandFromWndProc(TransportCommand::SelectModeNPU); return 0;
                    case '5':       self->pushCommandFromWndProc(TransportCommand::SelectModeCUDA); return 0;
                    case '6':       self->pushCommandFromWndProc(TransportCommand::SelectModeFinale); return 0;
                    default: break;
                }
            }
            break;
        }
        case WM_DROPFILES: {
            const HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            if (hDrop) {
                const UINT needed = DragQueryFileW(hDrop, 0, nullptr, 0);
                if (needed > 0 && needed < 4096u) {
                    std::wstring path(static_cast<size_t>(needed), L'\0');
                    if (DragQueryFileW(hDrop, 0, path.data(), needed + 1u) > 0) {
                        self->onDroppedFileFromWndProc(std::move(path));
                    }
                }
                DragFinish(hDrop);
            }
            return 0;
        }
        case WM_CLOSE:
            self->pushCommandFromWndProc(TransportCommand::Quit);
            if (self->closeWhileLoadingFromWndProc()) return 0;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            self->onDestroyedFromWndProc();
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

AppWindow::AppWindow() = default;

AppWindow::~AppWindow() {
    destroy();
}

bool AppWindow::create(
    const std::string& utf8Title,
    uint32_t clientWidth,
    uint32_t clientHeight,
    bool fullscreen,
    bool noActivate) {
    if (hwnd_) return false;
    fullscreen_ = minimized_ = resized_ = false;
    commandQueue_.clear();
    graphicsFailureText_.clear();
    loading_ = false;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = mainWindowProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);

    // Clamp initial windowed size safely within work area so 1080p fits cleanly on scaled displays
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const uint32_t workW = static_cast<uint32_t>(std::max<LONG>(640, workArea.right - workArea.left));
    const uint32_t workH = static_cast<uint32_t>(std::max<LONG>(480, workArea.bottom - workArea.top));

    clientWidth_  = std::clamp(clientWidth, 1u, workW > 64 ? workW - 64 : workW);
    clientHeight_ = std::clamp(clientHeight, 1u, workH > 64 ? workH - 64 : workH);

    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = noActivate ? WS_EX_NOACTIVATE : 0;
    RECT winRect{0, 0, static_cast<LONG>(clientWidth_), static_cast<LONG>(clientHeight_)};
    AdjustWindowRectExForDpi(&winRect, style, FALSE, exStyle, GetDpiForSystem());

    const int outerW = winRect.right - winRect.left;
    const int outerH = winRect.bottom - winRect.top;
    const int posX = workArea.left + std::max<int>(0, (static_cast<int>(workW) - outerW) / 2);
    const int posY = workArea.top  + std::max<int>(0, (static_cast<int>(workH) - outerH) / 2);

    windowedRect_[0] = posX;
    windowedRect_[1] = posY;
    windowedRect_[2] = posX + outerW;
    windowedRect_[3] = posY + outerH;

    const std::wstring wTitle = utf8ToWide(utf8Title);

    hwnd_ = CreateWindowExW(
        exStyle,
        kWindowClassName,
        wTitle.c_str(),
        style,
        posX,
        posY,
        outerW,
        outerH,
        nullptr,
        nullptr,
        hInst,
        this);
    if (!hwnd_) {
        return false;
    }
    if (HMENU systemMenu = GetSystemMenu(hwnd_, FALSE)) {
        AppendMenuW(systemMenu, MF_SEPARATOR, 0, nullptr);
        const std::wstring benchmarkLabel = std::wstring(resultText(ResultText::Run)) + L"…\tCtrl+B";
        AppendMenuW(systemMenu, MF_STRING, kFilmBenchmarkCommand, benchmarkLabel.c_str());
        if (worldPortalDialogAvailable())
            AppendMenuW(systemMenu, MF_STRING, kWorldPortalCommand, L"Visit a Demo-Verse &world...\tCtrl+L");
        AppendMenuW(systemMenu, MF_STRING, kMapAttributionCommand, L"Taipei 101 · 臺北 · © OpenStreetMap…");
    }

    RECT actualClient{};
    if (GetClientRect(hwnd_, &actualClient)) {
        clientWidth_  = static_cast<uint32_t>(std::max<LONG>(1, actualClient.right - actualClient.left));
        clientHeight_ = static_cast<uint32_t>(std::max<LONG>(1, actualClient.bottom - actualClient.top));
    }

    const int cmdShow = noActivate ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL;
    ShowWindow(hwnd_, cmdShow);
    // Win32's very first ShowWindow call in a GUI process honors STARTUPINFO.wShowWindow
    // (which is SW_HIDE when spawned from a hidden console/task runner) and ignores cmdShow.
    // Call ShowWindow a second time if the window is still hidden so explicit launches and
    // SW_SHOWNOACTIVATE test runs always become visible on the desktop and taskbar.
    if (!IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, cmdShow);
    }
    if (!noActivate) {
        DragAcceptFiles(hwnd_, TRUE);
        SetForegroundWindow(hwnd_);
    }
    UpdateWindow(hwnd_);

    if (fullscreen) {
        toggleBorderlessFullscreen();
    }
    resized_ = false;
    return true;
}

void AppWindow::destroy() noexcept {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void AppWindow::hide() noexcept {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

bool AppWindow::pumpMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return hwnd_ != nullptr;
}

bool AppWindow::popCommand(TransportCommand& outCmd) {
    if (commandQueue_.empty()) return false;
    outCmd = commandQueue_.front();
    commandQueue_.pop_front();
    return true;
}

bool AppWindow::popDroppedFile(std::wstring& outPath) {
    if (droppedFiles_.empty()) return false;
    outPath = std::move(droppedFiles_.front());
    droppedFiles_.pop_front();
    return true;
}

void AppWindow::onDroppedFileFromWndProc(std::wstring path) {
    if (!path.empty() && droppedFiles_.size() < 8u) {
        droppedFiles_.push_back(std::move(path));
    }
}

void AppWindow::setTitle(const std::string& utf8Title) {
    if (hwnd_) {
        const std::wstring w = utf8ToWide(utf8Title);
        SetWindowTextW(hwnd_, w.c_str());
    }
}

void AppWindow::toggleBorderlessFullscreen() {
    if (!hwnd_) return;
    fullscreen_ = !fullscreen_;
    if (fullscreen_) {
        RECT r{};
        GetWindowRect(hwnd_, &r);
        windowedRect_[0] = r.left;
        windowedRect_[1] = r.top;
        windowedRect_[2] = r.right;
        windowedRect_[3] = r.bottom;

        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        fitFullscreenToMonitor();
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(
            hwnd_,
            nullptr,
            windowedRect_[0],
            windowedRect_[1],
            windowedRect_[2] - windowedRect_[0],
            windowedRect_[3] - windowedRect_[1],
            SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

void AppWindow::fitFullscreenToMonitor() {
    if (!hwnd_ || !fullscreen_) return;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) return;
    SetWindowPos(hwnd_, nullptr, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
}

void AppWindow::showGraphicsFailure(const std::string& reason) {
    graphicsFailureText_ = std::wstring(L"BOO SAH SAH\n\n") + uiText(UiText::GraphicsFailureBody) + L"\n\n" + utf8ToWide(reason);
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, TRUE);
        UpdateWindow(hwnd_);
    }
}

bool AppWindow::paintGraphicsFailureFromWndProc() {
    if (!hwnd_ || graphicsFailureText_.empty()) return false;
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT r{};
    GetClientRect(hwnd_, &r);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    const int inset = MulDiv(24, static_cast<int>(GetDpiForWindow(hwnd_)), 96);
    InflateRect(&r, -inset, -inset);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 240, 240));
    auto oldFont = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    const UINT align = uiIsRightToLeft() ? (DT_RIGHT | DT_RTLREADING) : DT_LEFT;
    DrawTextW(dc, graphicsFailureText_.c_str(), -1, &r, align | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    EndPaint(hwnd_, &ps);
    return true;
}

bool AppWindow::closeWhileLoadingFromWndProc() noexcept {
    if (!loading_ && !busy_) return false;
    // The loader thread still draws into this window: take it off the screen now, destroy it later.
    closeRequested_ = true;
    hide();
    return true;
}

void AppWindow::showLoading() {
    loading_ = true;
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, TRUE);
        UpdateWindow(hwnd_);
    }
}

namespace {
// Where the loading bar and the figures under it go. The paint routine and the repaint request share this.
struct LoadingBarLayout { RECT track, detail, dirty; };
// The title sits above the middle, which leaves the lower half to the list of start-up steps.
int loadingMidY(const RECT& client) { return client.top + (client.bottom - client.top) * 3 / 10; }
LoadingBarLayout loadingBarLayout(const RECT& client, int dpi) {
    const int bodyPx = MulDiv(17, dpi, 96);
    const int midY = loadingMidY(client);
    const int clientW = client.right - client.left;
    const int width = std::max(40, std::min(std::clamp(clientW * 2 / 5, MulDiv(220, dpi, 96), MulDiv(560, dpi, 96)),
                                            clientW - MulDiv(32, dpi, 96)));
    const int height = std::max(4, MulDiv(6, dpi, 96));
    const int left = (client.left + client.right - width) / 2;
    const int top = midY + bodyPx * 5 / 2;
    LoadingBarLayout l{};
    l.track = {left, top, left + width, top + height};
    l.detail = {client.left, top + height + bodyPx / 2, client.right, top + height + bodyPx / 2 + bodyPx * 3 / 2};
    // The clock beside "Loading", the bar, the figures, the notice and the list change; the credits do not.
    l.dirty = {client.left, midY, client.right, std::max<LONG>(l.detail.bottom + 2, client.bottom - bodyPx * 5)};
    return l;
}
} // namespace

void AppWindow::setLoadingProgress(float fraction, std::wstring detail, std::wstring clock, std::wstring notice,
                                   std::vector<LoadingLine> lines) {
    if (!loading_) return;
    const bool first = loadingFraction_ < 0.0f;
    const float filled = std::clamp(fraction, 0.0f, 1.0f);
    if (!first && std::fabs(filled - loadingFraction_) < 0.0005f && detail == loadingDetail_ && clock == loadingClock_ &&
        notice == loadingNotice_ && lines == loadingLines_) {
        return; // nothing to repaint
    }
    loadingFraction_ = filled;
    loadingDetail_ = std::move(detail);
    loadingClock_ = std::move(clock);
    loadingNotice_ = std::move(notice);
    loadingLines_ = std::move(lines);
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const LoadingBarLayout l = loadingBarLayout(client, static_cast<int>(GetDpiForWindow(hwnd_)));
    // Only the bar's strip is repainted, so the rest of the screen does not flicker.
    InvalidateRect(hwnd_, first ? nullptr : &l.dirty, FALSE);
    UpdateWindow(hwnd_);
}

bool AppWindow::paintLoadingFromWndProc() {
    if (!hwnd_ || !loading_) return false;
    PAINTSTRUCT ps{};
    HDC screen = BeginPaint(hwnd_, &ps);
    RECT r{};
    GetClientRect(hwnd_, &r);
    // Drawn off screen and copied in one go, so the ticking list does not flicker.
    const int paintW = std::max<LONG>(ps.rcPaint.right - ps.rcPaint.left, 1);
    const int paintH = std::max<LONG>(ps.rcPaint.bottom - ps.rcPaint.top, 1);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = dc ? CreateCompatibleBitmap(screen, paintW, paintH) : nullptr;
    HGDIOBJ oldBitmap = nullptr;
    if (bitmap) {
        oldBitmap = SelectObject(dc, bitmap);
        SetWindowOrgEx(dc, ps.rcPaint.left, ps.rcPaint.top, nullptr);
    } else {
        if (dc) DeleteDC(dc);
        dc = screen;
    }
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    const int dpi = static_cast<int>(GetDpiForWindow(hwnd_));
    const int titlePx = MulDiv(30, dpi, 96);
    const int bodyPx = MulDiv(17, dpi, 96);
    HFONT titleFont = CreateFontW(-titlePx, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT bodyFont = CreateFontW(-bodyPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SetBkMode(dc, TRANSPARENT);
    const int midY = loadingMidY(r);
    const UINT rtl = uiIsRightToLeft() ? DT_RTLREADING : 0;
    auto oldFont = SelectObject(dc, titleFont ? titleFont : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc, RGB(235, 240, 240));
    RECT titleRect{r.left, midY - titlePx - bodyPx / 2, r.right, midY};
    DrawTextW(dc, L"BOO SAH SAH", -1, &titleRect, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, bodyFont ? bodyFont : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc, RGB(150, 200, 195));
    RECT bodyRect{r.left, midY + bodyPx / 2, r.right, midY + bodyPx * 3};
    std::wstring loadingWord = uiText(UiText::Loading);
    if (!loadingClock_.empty()) loadingWord += L"   " + loadingClock_;
    DrawTextW(dc, loadingWord.c_str(), -1, &bodyRect, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | rtl);
    if (loadingFraction_ >= 0.0f) {
        // The loading bar and its figures: megabytes read and checksummed, GPU memory in use.
        const LoadingBarLayout l = loadingBarLayout(r, dpi);
        HBRUSH trackBrush = CreateSolidBrush(RGB(30, 44, 46));
        HBRUSH fillBrush = CreateSolidBrush(RGB(120, 214, 200));
        FillRect(dc, &l.track, trackBrush);
        RECT fill = l.track;
        fill.right = fill.left + static_cast<LONG>(static_cast<float>(l.track.right - l.track.left) * loadingFraction_ + 0.5f);
        if (fill.right > fill.left) FillRect(dc, &fill, fillBrush);
        DeleteObject(trackBrush);
        DeleteObject(fillBrush);
        if (!loadingDetail_.empty()) {
            const int detailPx = MulDiv(13, dpi, 96);
            HFONT detailFont = CreateFontW(-detailPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            auto previousFont = SelectObject(dc, detailFont ? detailFont : GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(dc, RGB(128, 150, 152));
            RECT detailRect = l.detail;
            DrawTextW(dc, loadingDetail_.c_str(), -1, &detailRect, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, previousFont);
            if (detailFont) DeleteObject(detailFont);
        }
        paintLoadingList(dc, r, l.detail.bottom, dpi);
    }
    RECT creditsRect{r.left, midY + bodyPx * 3, r.right, r.bottom - bodyPx};
    SetTextColor(dc, RGB(154, 170, 173));
    DrawTextW(dc, kMapCredit, -1, &creditsRect, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
#if defined(BOO_HAS_DLSS)
    creditsRect.bottom -= bodyPx * 2;
    DrawTextW(dc, L"NVIDIA RTX™   ·   NVIDIA DLSS SDK", -1, &creditsRect,
              DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
#endif
    SelectObject(dc, oldFont);
    if (titleFont) DeleteObject(titleFont);
    if (bodyFont) DeleteObject(bodyFont);
    if (dc != screen) {
        BitBlt(screen, ps.rcPaint.left, ps.rcPaint.top, paintW, paintH, dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    EndPaint(hwnd_, &ps);
    return true;
}

// The first-start notice and the list of start-up steps, between the figures and the credits: when each
// step began, what it is, and how long it has taken. Running steps are bright, finished ones dim.
void AppWindow::paintLoadingList(HDC dc, const RECT& client, int top, int dpi) {
    const int bodyPx = MulDiv(17, dpi, 96);
    const int fontPx = MulDiv(13, dpi, 96);
    const int lineH = fontPx * 3 / 2;
    const int bottom = client.bottom - bodyPx * 5; // the credits start here
    const int width = std::max(40, std::min<int>(client.right - client.left - MulDiv(32, dpi, 96), MulDiv(640, dpi, 96)));
    const int left = (client.left + client.right - width) / 2;
    const bool rightToLeft = uiIsRightToLeft();
    const UINT reading = rightToLeft ? DT_RTLREADING : 0;
    HFONT font = CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto previousFont = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    int y = top + fontPx / 2;
    if (!loadingNotice_.empty()) {
        RECT box{left, y, left + width, bottom};
        RECT measured = box;
        DrawTextW(dc, loadingNotice_.c_str(), -1, &measured, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT | reading);
        box.bottom = std::min<LONG>(box.top + (measured.bottom - measured.top), bottom);
        SetTextColor(dc, RGB(222, 196, 128));
        DrawTextW(dc, loadingNotice_.c_str(), -1, &box, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | reading);
        y = box.bottom + fontPx / 2;
    }
    // As many steps as fit; finished ones make room first, oldest first.
    const size_t fits = static_cast<size_t>(std::max(0, (bottom - y) / std::max(lineH, 1)));
    std::vector<const LoadingLine*> shown;
    for (const LoadingLine& line : loadingLines_) shown.push_back(&line);
    while (shown.size() > fits) {
        auto drop = std::find_if(shown.begin(), shown.end(), [](const LoadingLine* l) { return !l->running; });
        shown.erase(drop != shown.end() ? drop : shown.begin());
    }
    const int whenW = MulDiv(48, dpi, 96), tookW = MulDiv(64, dpi, 96);
    for (const LoadingLine* line : shown) {
        RECT when{left, y, left + whenW, y + lineH};
        RECT what{left + whenW, y, left + width - tookW, y + lineH};
        RECT took{left + width - tookW, y, left + width, y + lineH};
        if (rightToLeft) {
            when = {left + width - whenW, y, left + width, y + lineH};
            what = {left + tookW, y, left + width - whenW, y + lineH};
            took = {left, y, left + tookW, y + lineH};
        }
        constexpr UINT kOneLine = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
        SetTextColor(dc, line->running ? RGB(120, 214, 200) : RGB(92, 110, 112));
        DrawTextW(dc, line->when.c_str(), -1, &when, kOneLine | (rightToLeft ? DT_RIGHT : DT_LEFT));
        SetTextColor(dc, line->running ? RGB(235, 240, 240) : RGB(128, 150, 152));
        DrawTextW(dc, line->what.c_str(), -1, &what,
                  kOneLine | DT_END_ELLIPSIS | reading | (rightToLeft ? DT_RIGHT : DT_LEFT));
        DrawTextW(dc, line->took.c_str(), -1, &took, kOneLine | (rightToLeft ? DT_LEFT : DT_RIGHT));
        y += lineH;
    }
    SelectObject(dc, previousFont);
    if (font) DeleteObject(font);
}

bool AppWindow::consumeResizeFlag() noexcept {
    const bool r = resized_;
    resized_ = false;
    return r;
}

void AppWindow::pushCommandFromWndProc(TransportCommand cmd) {
    commandQueue_.push_back(cmd);
}

void AppWindow::onResizeFromWndProc(uint32_t w, uint32_t h, bool minimized) {
    minimized_ = minimized;
    if (!minimized && w > 0 && h > 0 && (w != clientWidth_ || h != clientHeight_)) {
        clientWidth_ = w;
        clientHeight_ = h;
        resized_ = true;
    }
}

void AppWindow::onMouseButtonFromWndProc(int button, bool down, int x, int y) {
    lastMouseX_ = x;
    lastMouseY_ = y;
    hasMousePos_ = true;
    if (button == 0) {
        if (down && !lmbDown_) lmbJustPressed_ = true;
        lmbDown_ = down;
    } else if (button == 1) {
        if (down && !rmbDown_) rmbJustPressed_ = true;
        rmbDown_ = down;
    } else if (button == 2 && down) {
        mmbJustPressed_ = true;
    }
    if (hwnd_) {
        if (lmbDown_ || rmbDown_) {
            SetCapture(hwnd_);
        } else if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
    }
}

void AppWindow::onMouseMoveFromWndProc(int x, int y) {
    if (hasMousePos_) {
        const float dx = static_cast<float>(x - lastMouseX_);
        const float dy = static_cast<float>(y - lastMouseY_);
        if (rmbDown_) {
            accumRmbDx_ += dx;
            accumRmbDy_ += dy;
        }
        if (lmbDown_) {
            accumLmbDx_ += dx;
            accumLmbDy_ += dy;
        }
    }
    lastMouseX_ = x;
    lastMouseY_ = y;
    hasMousePos_ = true;
}

void AppWindow::onMouseWheelFromWndProc(int delta) {
    accumWheel_ += static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA);
}

void AppWindow::onFocusLostFromWndProc() noexcept {
    lmbDown_ = false;
    rmbDown_ = false;
}

WindowInputSnapshot AppWindow::consumeInputSnapshot() noexcept {
    WindowInputSnapshot snap{};
    snap.mouseX = static_cast<float>(lastMouseX_);
    snap.mouseY = static_cast<float>(lastMouseY_);
    snap.dragRmbDx = accumRmbDx_;
    snap.dragRmbDy = accumRmbDy_;
    snap.dragLmbDx = accumLmbDx_;
    snap.dragLmbDy = accumLmbDy_;
    snap.wheelNotches = accumWheel_;
    snap.lmbDown = lmbDown_;
    snap.lmbJustPressed = lmbJustPressed_;
    snap.rmbDown = rmbDown_;
    snap.rmbJustPressed = rmbJustPressed_;
    snap.mmbJustPressed = mmbJustPressed_;
    snap.hasFocus = (hwnd_ != nullptr) && (GetForegroundWindow() == hwnd_);

    accumRmbDx_ = accumRmbDy_ = 0.0f;
    accumLmbDx_ = accumLmbDy_ = 0.0f;
    accumWheel_ = 0.0f;
    lmbJustPressed_ = false;
    rmbJustPressed_ = false;
    mmbJustPressed_ = false;

    if (snap.hasFocus) {
        auto keyDown = [](int vk) noexcept -> bool {
            return (GetAsyncKeyState(vk) & 0x8000) != 0;
        };
        snap.altDown = keyDown(VK_MENU);
        snap.shiftDown = keyDown(VK_SHIFT);
        if (exploreKeyCapture_) {
            snap.moveForward = (keyDown('W') ? 1.0f : 0.0f) - (keyDown('S') ? 1.0f : 0.0f);
            snap.moveRight   = (keyDown('D') ? 1.0f : 0.0f) - (keyDown('A') ? 1.0f : 0.0f);
            snap.moveUp      = ((keyDown('E') || keyDown(VK_PRIOR)) ? 1.0f : 0.0f) -
                               ((keyDown('Q') || keyDown(VK_NEXT) || keyDown(VK_CONTROL)) ? 1.0f : 0.0f);
            const float bracketAxis = (keyDown(VK_OEM_6) ? 1.0f : 0.0f) - (keyDown(VK_OEM_4) ? 1.0f : 0.0f);
            if (snap.shiftDown) {
                snap.sunElevKey = bracketAxis;
            } else {
                snap.sunAzimKey = bracketAxis;
            }
        }
    }
    return snap;
}

} // namespace boo
