#include "boo/window.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <vector>

namespace boo {

namespace {
constexpr const wchar_t* kWindowClassName = L"BooSahSahGlassOrchidWindowClass";

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
        case WM_DISPLAYCHANGE:
            if (self->isFullscreen()) self->fitFullscreenToMonitor();
            break;
        case WM_PAINT:
            if (self->paintGraphicsFailureFromWndProc()) return 0;
            break;
        case WM_SYSKEYDOWN: {
            if (wParam == VK_RETURN && (lParam & (1 << 29)) && !(lParam & 0x40000000)) {
                self->pushCommandFromWndProc(TransportCommand::ToggleFullscreen);
                return 0;
            }
            break;
        }
        case WM_KEYDOWN: {
            const bool repeat = (lParam & 0x40000000) != 0;
            if (!repeat) {
                switch (wParam) {
                    case VK_LEFT: self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::SeekBackLong : TransportCommand::SeekBack); return 0;
                    case VK_RIGHT: self->pushCommandFromWndProc((GetKeyState(VK_SHIFT) & 0x8000) ? TransportCommand::SeekForwardLong : TransportCommand::SeekForward); return 0;
                    case VK_OEM_COMMA:
                    case VK_OEM_4: self->pushCommandFromWndProc(TransportCommand::StepBack); return 0;
                    case VK_OEM_PERIOD:
                    case VK_OEM_6: self->pushCommandFromWndProc(TransportCommand::StepForward); return 0;
                    case 'G': self->pushCommandFromWndProc(TransportCommand::RevealPreroll); return 0;
                    case VK_DOWN:
                    case VK_OEM_MINUS: self->pushCommandFromWndProc(TransportCommand::VolumeDown); return 0;
                    case VK_UP:
                    case VK_OEM_PLUS: self->pushCommandFromWndProc(TransportCommand::VolumeUp); return 0;
                    case VK_F12: self->pushCommandFromWndProc(TransportCommand::CaptureClean); return 0;
                    case VK_ESCAPE: self->pushCommandFromWndProc(TransportCommand::Quit); return 0;
                    case VK_SPACE:  self->pushCommandFromWndProc(TransportCommand::TogglePlayPause); return 0;
                    case 'R':
                    case VK_HOME:   self->pushCommandFromWndProc(TransportCommand::Restart); return 0;
                    case 'N':  self->pushCommandFromWndProc(TransportCommand::NextAct); return 0;
                    case 'P':   self->pushCommandFromWndProc(TransportCommand::PrevAct); return 0;
                    case 'L':       self->pushCommandFromWndProc(TransportCommand::ToggleLoop); return 0;
                    case 'M':       self->pushCommandFromWndProc(TransportCommand::ToggleMute); return 0;
                    case 'Q':       self->pushCommandFromWndProc(TransportCommand::CycleQualityPreset); return 0;
                    case 'E':       self->pushCommandFromWndProc(TransportCommand::ToggleEdition); return 0;
                    case 'K':       self->pushCommandFromWndProc(TransportCommand::ToggleCaptions); return 0;
                    case VK_TAB:
                    case VK_F1:     self->pushCommandFromWndProc(TransportCommand::ToggleOverlay); return 0;
                    case 'B':       self->pushCommandFromWndProc(TransportCommand::TriggerBenchSweep); return 0;
                    case 'X':       self->pushCommandFromWndProc(TransportCommand::ToggleRtRuntime); return 0;
                    case 'C':       self->pushCommandFromWndProc(TransportCommand::ToggleCudaRuntime); return 0;
                    case 'T':       self->pushCommandFromWndProc(TransportCommand::ToggleTensorRuntime); return 0;
                    case 'U':       self->pushCommandFromWndProc(TransportCommand::ToggleNpuRuntime); return 0;
                    case 'V':       self->pushCommandFromWndProc(TransportCommand::ToggleCodecRuntime); return 0;
                    case 'O':       self->pushCommandFromWndProc(TransportCommand::ToggleSpoutRuntime); return 0;
                    case 'W':       self->pushCommandFromWndProc(TransportCommand::ToggleCameraRuntime); return 0;
                    case 'A':       self->pushCommandFromWndProc(TransportCommand::CycleAbCompareEngine); return 0;
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
        case WM_CLOSE:
            self->pushCommandFromWndProc(TransportCommand::Quit);
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

    RECT actualClient{};
    if (GetClientRect(hwnd_, &actualClient)) {
        clientWidth_  = static_cast<uint32_t>(std::max<LONG>(1, actualClient.right - actualClient.left));
        clientHeight_ = static_cast<uint32_t>(std::max<LONG>(1, actualClient.bottom - actualClient.top));
    }

    ShowWindow(hwnd_, noActivate ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
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
    graphicsFailureText_ = L"BOO SAH SAH\n\nGraphics playback could not be restored.\n"
                           L"The audio and timeline remain available. Close the show and restart to retry.\n\n" +
                           utf8ToWide(reason);
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
    DrawTextW(dc, graphicsFailureText_.c_str(), -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    EndPaint(hwnd_, &ps);
    return true;
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

} // namespace boo
