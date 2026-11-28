#pragma once
#include "results_theme_palette.hpp"
#include "results_dialog_ids.h"
#include <commctrl.h>
#include <algorithm>
#include <memory>

namespace boo::results_theme {
namespace detail {
struct ControlState { COLORREF accent; bool rtl; bool hover = false; bool radio = false; int radioId = 0; bool defaultButton = false; };
inline int scale(HWND window, int px) {
    const UINT dpi = GetDpiForWindow(window);
    return MulDiv(px, dpi ? dpi : 96, 96);
}
inline void rounded(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF border, int borderWidth = 1) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, borderWidth, border);
    const auto oldBrush = SelectObject(dc, brush);
    const auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
    DeleteObject(pen); DeleteObject(brush);
}
inline void paintRadio(HWND window, ControlState& state) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window, &ps);
    RECT r{}; GetClientRect(window, &r);
    HBRUSH bg = CreateSolidBrush(background); FillRect(dc, &r, bg); DeleteObject(bg);
    const int diameter = (std::min)(scale(window, 20), static_cast<int>(r.bottom-r.top));
    const int left = scale(window, 1);
    const int top = (r.bottom-r.top-diameter)/2;
    RECT circle{left, top, left+diameter, top+diameter};
    const bool enabled = IsWindowEnabled(window) != FALSE;
    const bool checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const COLORREF ring = checked ? state.accent : (state.hover ? RGB(197,197,197) : RGB(154,154,154));
    const COLORREF visibleRing = enabled ? ring : blend40(ring);
    rounded(dc, circle, diameter/2, checked ? visibleRing : background, visibleRing);
    if (checked) {
        const int dotDiameter = (std::min)(scale(window, 8), diameter);
        const int dotLeft = circle.left + (diameter-dotDiameter)/2;
        const int dotTop = circle.top + (diameter-dotDiameter)/2;
        RECT dot{dotLeft,dotTop,dotLeft+dotDiameter,dotTop+dotDiameter};
        const COLORREF dotColor = accentTextColor(visibleRing);
        rounded(dc, dot, dotDiameter/2, dotColor, dotColor);
    }
    wchar_t label[512]{}; GetWindowTextW(window, label, static_cast<int>(std::size(label)));
    RECT text = r; text.left = circle.right + scale(window, 7);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, enabled ? foreground : blend40(foreground));
    const auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
    const UINT flags = DT_WORDBREAK | DT_LEFT | (state.rtl ? DT_RTLREADING : 0);
    RECT measured = text;
    DrawTextW(dc, label, -1, &measured, flags | DT_CALCRECT);
    text.top += (std::max)(0L, (text.bottom-text.top-(measured.bottom-measured.top))/2);
    DrawTextW(dc, label, -1, &text, flags);
    SelectObject(dc, oldFont);
    if (GetFocus() == window && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT focus = r; InflateRect(&focus, -1, -1);
        HBRUSH clear = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
        HPEN pen = CreatePen(PS_SOLID, scale(window, 2), foreground);
        const auto oldBrush = SelectObject(dc, clear); const auto oldPen = SelectObject(dc, pen);
        RoundRect(dc, focus.left, focus.top, focus.right, focus.bottom, scale(window, 8), scale(window, 8));
        SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen);
    }
    EndPaint(window, &ps);
}
inline LRESULT CALLBACK controlProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto* state = reinterpret_cast<ControlState*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, controlProc, 1);
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        delete state; return result;
    }
    if (state->radioId) {
        if (message == WM_LBUTTONUP) {
            HWND radio = GetDlgItem(GetParent(window), state->radioId);
            if (IsWindowEnabled(radio)) { SetFocus(radio); SendMessageW(radio, BM_CLICK, 0, 0); }
            return 0;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }
    switch (message) {
    case WM_MOUSEMOVE:
        if (!state->hover) {
            state->hover = true; TRACKMOUSEEVENT event{sizeof(event), TME_LEAVE, window, 0};
            TrackMouseEvent(&event); InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE: state->hover = false; InvalidateRect(window, nullptr, FALSE); break;
    case WM_PAINT: if (state->radio) { paintRadio(window, *state); return 0; } break;
    case WM_GETDLGCODE:
        if (!state->radio) {
            const auto result = DefSubclassProc(window, message, wParam, lParam);
            return (result & ~(DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) | DLGC_BUTTON |
                (state->defaultButton ? DLGC_DEFPUSHBUTTON : DLGC_UNDEFPUSHBUTTON);
        }
        break;
    case BM_SETSTYLE:
        if (!state->radio) {
            const auto type = wParam & BS_TYPEMASK;
            if (type == BS_DEFPUSHBUTTON) state->defaultButton = true;
            else if (type == BS_PUSHBUTTON) state->defaultButton = false;
            wParam = (wParam & ~BS_TYPEMASK) | BS_OWNERDRAW;
        }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: case WM_UPDATEUISTATE:
    case BM_SETCHECK: case BM_SETSTATE:
        InvalidateRect(window, nullptr, FALSE); break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}
// The description is indented to align with its label. Forward clicks in the
// remaining strip under the radio as well, so the entire choice row is active.
inline LRESULT CALLBACK rowProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(dialog, rowProc, 2);
    } else if (message == WM_LBUTTONUP) {
        constexpr int radios[]{IDC_RESULTS_RASTER, IDC_RESULTS_PT, IDC_RESULTS_DLSS, IDC_RESULTS_RENDER_TEST};
        constexpr int descriptions[]{IDC_RESULTS_RASTER_DESC, IDC_RESULTS_PT_DESC, IDC_RESULTS_DLSS_DESC, IDC_RESULTS_RENDER_DESC};
        POINT point{}; GetCursorPos(&point);
        for (int i = 0; i < 4; ++i) {
            HWND radio = GetDlgItem(dialog, radios[i]), description = GetDlgItem(dialog, descriptions[i]);
            if (!IsWindowVisible(radio) || !IsWindowVisible(description) || !IsWindowEnabled(radio)) continue;
            RECT row{}, line{}; GetWindowRect(radio, &row); GetWindowRect(description, &line);
            row.top = line.top; row.bottom = line.bottom;
            if (PtInRect(&row, point)) {
                SetFocus(radio); SendMessageW(radio, BM_CLICK, 0, 0); return 0;
            }
        }
    }
    return DefSubclassProc(dialog, message, wParam, lParam);
}
inline void attach(HWND window, COLORREF accent, bool rtl, bool radio, int radioId = 0) {
    if (!window) return;
    const bool isDefault = (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
    auto state = std::make_unique<ControlState>(ControlState{accent, rtl, false, radio, radioId, isDefault});
    if (SetWindowSubclass(window, controlProc, 1, reinterpret_cast<DWORD_PTR>(state.get()))) state.release();
}
} // namespace detail

inline void install(HWND dialog, COLORREF accent, bool rtl) {
    SetWindowSubclass(dialog, detail::rowProc, 2, 0);
    for (int id : {IDOK, IDCANCEL, IDC_RESULTS_DETAILS, IDC_RESULTS_SAVE, IDC_RESULTS_SAVE_TEXT, IDC_RESULTS_REPORT}) {
        HWND button = GetDlgItem(dialog, id);
        detail::attach(button, accent, rtl, false);
        SendMessageW(button, BM_SETSTYLE, BS_OWNERDRAW, TRUE);
    }
    constexpr int radios[]{IDC_RESULTS_RASTER, IDC_RESULTS_PT, IDC_RESULTS_DLSS, IDC_RESULTS_RENDER_TEST};
    constexpr int descriptions[]{IDC_RESULTS_RASTER_DESC, IDC_RESULTS_PT_DESC, IDC_RESULTS_DLSS_DESC, IDC_RESULTS_RENDER_DESC};
    for (int i = 0; i < 4; ++i) {
        detail::attach(GetDlgItem(dialog, radios[i]), accent, rtl, true);
        HWND description = GetDlgItem(dialog, descriptions[i]);
        SetWindowLongPtrW(description, GWL_STYLE, GetWindowLongPtrW(description, GWL_STYLE) | SS_NOTIFY);
        detail::attach(description, accent, rtl, false, radios[i]);
    }
}
inline bool drawButton(const DRAWITEMSTRUCT& draw, COLORREF accent) {
    if (draw.CtlType != ODT_BUTTON) return false;
    DWORD_PTR data{};
    if (!GetWindowSubclass(draw.hwndItem, detail::controlProc, 1, &data)) return false;
    auto* state = reinterpret_cast<detail::ControlState*>(data);
    if (state->radio || state->radioId) return false;
    const bool enabled = !(draw.itemState & ODS_DISABLED);
    const auto colors = buttonColors(draw.CtlID == IDOK, state->hover, (draw.itemState & ODS_SELECTED) != 0, enabled, accent);
    RECT bounds = draw.rcItem;
    HBRUSH bg = CreateSolidBrush(background); FillRect(draw.hDC, &bounds, bg); DeleteObject(bg);
    RECT shape = bounds; InflateRect(&shape, -1, -1);
    detail::rounded(draw.hDC, shape, detail::scale(draw.hwndItem, 4), colors.fill, colors.border);
    wchar_t text[512]{}; GetWindowTextW(draw.hwndItem, text, static_cast<int>(std::size(text)));
    SetBkMode(draw.hDC, TRANSPARENT); SetTextColor(draw.hDC, colors.text);
    const auto oldFont = SelectObject(draw.hDC, reinterpret_cast<HFONT>(SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0)));
    RECT textBounds = bounds;
    const int textPadding = detail::scale(draw.hwndItem, 6);
    textBounds.left += textPadding;
    textBounds.right = (std::max)(textBounds.left + 1, textBounds.right - textPadding);
    const UINT textFlags = DT_CENTER | DT_WORDBREAK | (state->rtl ? DT_RTLREADING : 0);
    RECT measured = textBounds;
    DrawTextW(draw.hDC, text, -1, &measured, textFlags | DT_CALCRECT);
    textBounds.top += (std::max)(0L, (textBounds.bottom-textBounds.top-(measured.bottom-measured.top))/2);
    DrawTextW(draw.hDC, text, -1, &textBounds, textFlags);
    SelectObject(draw.hDC, oldFont);
    if ((draw.itemState & ODS_FOCUS) && !(draw.itemState & ODS_NOFOCUSRECT) &&
        !(SendMessageW(draw.hwndItem, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT focus = bounds; InflateRect(&focus, -2, -2);
        HPEN pen = CreatePen(PS_SOLID, detail::scale(draw.hwndItem, 2), RGB(255,255,255));
        const auto oldPen = SelectObject(draw.hDC, pen);
        const auto oldBrush = SelectObject(draw.hDC, GetStockObject(NULL_BRUSH));
        RoundRect(draw.hDC, focus.left, focus.top, focus.right, focus.bottom, detail::scale(draw.hwndItem, 8), detail::scale(draw.hwndItem, 8));
        SelectObject(draw.hDC, oldBrush); SelectObject(draw.hDC, oldPen); DeleteObject(pen);
    }
    return true;
}
} // namespace boo::results_theme
