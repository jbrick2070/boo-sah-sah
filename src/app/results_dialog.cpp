#include "boo/results_dialog.hpp"
#include "boo/logging.hpp"
#include "boo/results_card.hpp"
#include "boo/results_export.hpp"
#include "boo/results_reference.hpp"
#include "boo/results_worker.hpp"
#include "boo/render_test_image.hpp"
#include "boo/ui_text.hpp"
#include "results_dialog_ids.h"
#include "results_dialog_theme.hpp"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <thread>

namespace boo {
namespace {
using T = ResultText;
enum class Page { Select, Progress, Card };
constexpr std::array<int, 5> kChoices{IDC_RESULTS_RASTER, IDC_RESULTS_PT, IDC_RESULTS_DLSS, IDC_RESULTS_RENDER_TEST, IDC_RESULTS_FILM_TOUR};
constexpr std::array<int, 5> kDescriptions{IDC_RESULTS_RASTER_DESC, IDC_RESULTS_PT_DESC, IDC_RESULTS_DLSS_DESC, IDC_RESULTS_RENDER_DESC, IDC_RESULTS_TOUR_DESC};
constexpr std::array<T, 5> kDescriptionText{T::RasterDescription, T::PathDescription, T::DlssDescription, T::RenderDescription, T::FilmTourDescription};
constexpr wchar_t kReportPage[] = L"https://github.com/jbrick2070/boo-sah-sah/issues/new";
constexpr COLORREF kBackground = RGB(32, 32, 32);
constexpr COLORREF kPrimaryText = RGB(230, 230, 230);
constexpr COLORREF kSecondaryText = RGB(160, 160, 160);

bool fontAvailable(HWND window, const wchar_t* face) {
    HDC dc = GetDC(window);
    if (!dc) return false;
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(query.lfFaceName, face);
    bool found = false;
    EnumFontFamiliesExW(dc, &query, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM data) -> int {
        *reinterpret_cast<bool*>(data) = true;
        return 0;
    }, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(window, dc);
    return found;
}

struct ProgressVisual {
    COLORREF accent = RGB(76, 194, 255);
    bool indeterminate = false;
    int position = 0;
};

// The header sends NM_CUSTOMDRAW to its parent (the table), not to the dialog: paint it dark here.
LRESULT CALLBACK tableProc(HWND table, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_NOTIFY) {
        auto* note = reinterpret_cast<NMHDR*>(lParam);
        if (note->hwndFrom == ListView_GetHeader(table) && note->code == NM_CUSTOMDRAW) {
            auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
            if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
            if (draw->dwDrawStage == CDDS_POSTPAINT) {
                // Fill the header area to the right of the last column.
                RECT client{}, last{};
                GetClientRect(note->hwndFrom, &client);
                const int count = Header_GetItemCount(note->hwndFrom);
                if (count > 0 && Header_GetItemRect(note->hwndFrom, count - 1, &last) && last.right < client.right) {
                    client.left = last.right;
                    HBRUSH brush = CreateSolidBrush(RGB(45, 45, 45));
                    FillRect(draw->hdc, &client, brush);
                    DeleteObject(brush);
                }
                return CDRF_DODEFAULT;
            }
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                HBRUSH brush = CreateSolidBrush(RGB(45, 45, 45));
                FillRect(draw->hdc, &draw->rc, brush);
                DeleteObject(brush);
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(64, 64, 64));
                HGDIOBJ old = SelectObject(draw->hdc, pen);
                MoveToEx(draw->hdc, draw->rc.right - 1, draw->rc.top + 4, nullptr);
                LineTo(draw->hdc, draw->rc.right - 1, draw->rc.bottom - 4);
                SelectObject(draw->hdc, old);
                DeleteObject(pen);
                wchar_t text[256]{};
                HDITEMW item{}; item.mask = HDI_TEXT | HDI_FORMAT; item.pszText = text; item.cchTextMax = 256;
                Header_GetItem(note->hwndFrom, static_cast<int>(draw->dwItemSpec), &item);
                RECT rect = draw->rc; rect.left += 6; rect.right -= 6;
                SetBkMode(draw->hdc, TRANSPARENT); SetTextColor(draw->hdc, RGB(230, 230, 230));
                const UINT alignment = (item.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT;
                DrawTextW(draw->hdc, text, -1, &rect, alignment | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                return CDRF_SKIPDEFAULT;
            }
            return CDRF_DODEFAULT;
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(table, tableProc, 1);
    return DefSubclassProc(table, message, wParam, lParam);
}

LRESULT CALLBACK progressProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam,
                              UINT_PTR, DWORD_PTR data) {
    auto* visual = reinterpret_cast<ProgressVisual*>(data);
    if (message == WM_ERASEBKGND) return 1;
    // The native progress control can reserve and paint a themed non-client
    // border. Make the entire window client area so only our track is visible.
    if (message == WM_NCCALCSIZE || message == WM_NCPAINT) return 0;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(control, &paint);
        RECT bounds{}; GetClientRect(control, &bounds);
        SetDCBrushColor(dc, kBackground);
        FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        const int height = (std::min)(4, static_cast<int>(bounds.bottom));
        const int top = (bounds.bottom - height) / 2;
        const int width = bounds.right;
        auto rounded = [&](int left, int right, COLORREF color) {
            if (right <= left || height <= 0) return;
            SetDCBrushColor(dc, color);
            const auto oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
            const auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, left, top, right, top + height, height, height);
            SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
        };
        rounded(0, width, RGB(58, 58, 58));
        if (visual->indeterminate) {
            const int segment = (std::max)(height, width / 4);
            const int travel = width + segment;
            const int left = static_cast<int>((GetTickCount64() / 5) % (std::max)(1, travel)) - segment;
            rounded((std::max)(0, left), (std::min)(width, left + segment), visual->accent);
        } else {
            rounded(0, MulDiv(width, std::clamp(visual->position, 0, 1000), 1000), visual->accent);
        }
        EndPaint(control, &paint);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(control, progressProc, 1);
    return DefSubclassProc(control, message, wParam, lParam);
}

struct SaveOperation {
    std::atomic<bool> done{false};
    ResultsSaveStatus status = ResultsSaveStatus::Failed;
    ResultsSaveKind kind = ResultsSaveKind::Picture;
};
struct Dialog {
    explicit Dialog(const ResultsDialogContext& c) : context(c) {}
    const ResultsDialogContext& context;
    ResultsWorker worker;
    HWND window{};
    Page page = Page::Select;
    BenchmarkMode mode = BenchmarkMode::Raster;
    bool closeViewer = false;
    bool detailsExpanded = false;
    ResultCard card;
    HFONT numericFont{};
    HFONT titleFont{};
    HFONT messageFont{};
    HBRUSH backgroundBrush{};
    ProgressVisual progressVisual;
    ULONGLONG startedAt{};
    HWND thumbnail{};
    std::shared_ptr<const RenderTestImage> image;
    std::vector<uint8_t> thumbnailBgra;
    bool hasComparison = false;
    std::wstring progress;
    std::shared_ptr<SaveOperation> saving;
    ~Dialog() { if (numericFont) DeleteObject(numericFont); if (titleFont) DeleteObject(titleFont); if (messageFont) DeleteObject(messageFont); if (backgroundBrush) DeleteObject(backgroundBrush); }

    void updateMessageFont() {
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        using MetricsForDpi = BOOL (WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
        const auto forDpi = reinterpret_cast<MetricsForDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"));
        const bool ok = forDpi ? forDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, GetDpiForWindow(window)) != FALSE
                               : SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != FALSE;
        if (!ok) return;
        wcscpy_s(metrics.lfMessageFont.lfFaceName,
                 fontAvailable(window, L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI");
        HFONT replacement = CreateFontIndirectW(&metrics.lfMessageFont);
        if (!replacement) return;
        SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), FALSE);
        EnumChildWindows(window, [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(replacement));
        if (messageFont) DeleteObject(messageFont);
        messageFont = replacement;
    }

    void initializeTheme() {
        // RtlGetVersion reports the actual build without a compatibility-manifest dependency.
        using VersionFn = LONG (WINAPI*)(OSVERSIONINFOW*);
        const auto versionFn = reinterpret_cast<VersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW version{sizeof(version)};
        if (versionFn && versionFn(&version) == 0 && version.dwBuildNumber >= 22000) {
            const DWORD corner = 2; // DWMWCP_ROUND
            DwmSetWindowAttribute(window, 33, &corner, sizeof(corner));
            if (version.dwBuildNumber >= 22621) {
                const DWORD backdrop = 2; // DWMSBT_MAINWINDOW
                DwmSetWindowAttribute(window, 38, &backdrop, sizeof(backdrop));
            }
        }
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        DWORD color = 0; BOOL opaque = FALSE;
        const bool accentAvailable = SUCCEEDED(DwmGetColorizationColor(&color, &opaque));
        progressVisual.accent = results_theme::accentColor(accentAvailable,
            RGB((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff));
        backgroundBrush = CreateSolidBrush(kBackground);
        updateMessageFont();
        HWND bar = GetDlgItem(window, IDC_RESULTS_PROGRESS);
        SetWindowSubclass(bar, progressProc, 1, reinterpret_cast<DWORD_PTR>(&progressVisual));
        SetWindowPos(bar, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        results_theme::install(window, progressVisual.accent, UiStrings::instance().rightToLeft());
        HWND table = GetDlgItem(window, IDC_RESULTS_TABLE);
        SetWindowTheme(table, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(ListView_GetHeader(table), L"DarkMode_ItemsView", nullptr);
        SetWindowSubclass(table, tableProc, 1, 0);
        ListView_SetBkColor(table, kBackground);
        ListView_SetTextBkColor(table, kBackground);
        ListView_SetTextColor(table, kPrimaryText);
    }

    void updateTitleFont() {
        LOGFONTW font{};
        const auto normal=reinterpret_cast<HFONT>(SendMessageW(window,WM_GETFONT,0,0));
        if (!GetObjectW(normal,sizeof(font),&font)) return;
        font.lfHeight = MulDiv(font.lfHeight, 11, 9);
        font.lfWeight = FW_SEMIBOLD;
        wcscpy_s(font.lfFaceName,
                 fontAvailable(window, L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI");
        HFONT replacement=CreateFontIndirectW(&font);
        if (!replacement) return;
        SendDlgItemMessageW(window,IDC_RESULTS_TITLE,WM_SETFONT,reinterpret_cast<WPARAM>(replacement),TRUE);
        if (titleFont) DeleteObject(titleFont);
        titleFont=replacement;
    }
    void label(int id, T key) const { SetDlgItemTextW(window, id, resultText(key)); }
    void show(int id, bool visible) const { ShowWindow(GetDlgItem(window, id), visible ? SW_SHOW : SW_HIDE); }
    int dluX(int value) const { RECT r{0,0,value,0}; MapDialogRect(window,&r); return r.right; }
    int dluY(int value) const { RECT r{0,0,0,value}; MapDialogRect(window,&r); return r.bottom; }
    // The same measurement pass determines the window height and places controls,
    // so translated text wraps without changing the requested page width.
    int arrange(int clientWidth, bool apply) const {
        const int margin=dluX(7), gap=dluX(4), inner=(std::max)(1,clientWidth-2*margin);
        const int lineGap=dluY(3), buttonMinH=dluY(14);
        const int buttonPadding=MulDiv(12,GetDpiForWindow(window),96);
        HDC dc=GetDC(window);
        if (!dc) return dluY(145);
        auto measure = [&](int id, int width) {
            HWND control=GetDlgItem(window,id);
            const int length=GetWindowTextLengthW(control);
            std::wstring text(static_cast<size_t>(length)+1,L'\0');
            GetWindowTextW(control,text.data(),length+1);
            HFONT font=reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0));
            HGDIOBJ old=SelectObject(dc,font);
            RECT rect{0,0,(std::max)(1,width),0};
            if (length) DrawTextW(dc,text.c_str(),length,&rect,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
            SelectObject(dc,old);
            return static_cast<int>(rect.bottom);
        };
        auto naturalWidth = [&](int id) {
            HWND control=GetDlgItem(window,id);
            const int length=GetWindowTextLengthW(control);
            std::wstring text(static_cast<size_t>(length)+1,L'\0');
            GetWindowTextW(control,text.data(),length+1);
            HGDIOBJ old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0)));
            SIZE size{}; GetTextExtentPoint32W(dc,text.c_str(),length,&size);
            SelectObject(dc,old);
            return (std::max)(dluX(50),static_cast<int>(size.cx)+buttonPadding);
        };
        auto place = [&](int id,int x,int y,int width,int height) {
            if (apply) MoveWindow(GetDlgItem(window,id),x,y,(std::max)(1,width),(std::max)(1,height),TRUE);
        };
        int y=dluY(7);
        const int titleH=(std::max)(dluY(14),measure(IDC_RESULTS_TITLE,inner));
        place(IDC_RESULTS_TITLE,margin,y,inner,titleH);
        y+=titleH+dluY(3);
        if (page == Page::Select) {
            const int radioIndent=MulDiv(28,GetDpiForWindow(window),96);
            for (size_t i=0;i<kChoices.size();++i) {
                const int radioH=(std::max)(dluY(14),measure(kChoices[i],inner-radioIndent));
                place(kChoices[i],margin,y,inner,radioH);
                y+=radioH;
                const int descriptionH=(std::max)(dluY(10),measure(kDescriptions[i],inner-radioIndent));
                place(kDescriptions[i],margin+radioIndent,y,inner-radioIndent,descriptionH);
                y+=descriptionH+lineGap;
            }
        } else if (page == Page::Progress) {
            const int stageH=(std::max)(dluY(12),measure(IDC_RESULTS_STAGE,inner));
            place(IDC_RESULTS_STAGE,margin,y,inner,stageH); y+=stageH+lineGap;
            place(IDC_RESULTS_PROGRESS,margin,y,inner,dluY(12)); y+=dluY(12)+lineGap;
            const int elapsedH=(std::max)(dluY(12),measure(IDC_RESULTS_ELAPSED,inner));
            place(IDC_RESULTS_ELAPSED,margin,y,inner,elapsedH); y+=elapsedH+lineGap;
        } else {
            const bool imageBeside=thumbnail && !detailsExpanded && image && image->valid() && inner>=dluX(260);
            const int imageSlot=imageBeside ? inner*27/100 : 0;
            const int summaryWidth=imageBeside ? inner-imageSlot-gap : inner;
            const int metric=summaryWidth*36/100, run=summaryWidth*32/100, reference=summaryWidth-metric-run;
            const int summaryTop=y;
            const auto summaryRow = [&](int first, int second, int third) {
                const int h=(std::max)({dluY(12),measure(first,metric),measure(second,run),measure(third,reference)});
                place(first,margin,y,metric,h);
                place(second,margin+metric,y,run,h);
                place(third,margin+metric+run,y,reference,h);
                y+=h;
            };
            summaryRow(IDC_RESULTS_SUMMARY_METRIC,IDC_RESULTS_SUMMARY_RUN,IDC_RESULTS_SUMMARY_REFERENCE);
            summaryRow(IDC_RESULTS_TYPICAL_LABEL,IDC_RESULTS_TYPICAL_RUN,IDC_RESULTS_TYPICAL_REFERENCE);
            if (mode != BenchmarkMode::FilmTour)
                summaryRow(IDC_RESULTS_WORST_LABEL,IDC_RESULTS_WORST_RUN,IDC_RESULTS_WORST_REFERENCE);
            auto summaryText = [&](int id) {
                const int h=measure(id,summaryWidth);
                if (h) { place(id,margin,y,summaryWidth,h); y+=h+lineGap; }
            };
            if (hasComparison) summaryText(IDC_RESULTS_COMPARISON);
            summaryText(IDC_RESULTS_RAN);
            summaryText(IDC_RESULTS_DLSS_NOTE);
            if (thumbnail && !detailsExpanded && image && image->valid()) {
                const int slotX=imageBeside ? margin+summaryWidth+gap : margin;
                const int slotY=imageBeside ? summaryTop : y;
                const int slotW=imageBeside ? imageSlot : inner;
                const int slotH=imageBeside ? (std::max)(dluY(72),y-summaryTop) : dluY(72);
                const double scale=(std::min)(double(slotW)/image->width,double(slotH)/image->height);
                const int imageW=(std::max)(1,int(image->width*scale));
                const int imageH=(std::max)(1,int(image->height*scale));
                if (apply) MoveWindow(thumbnail,slotX+(slotW-imageW)/2,slotY+(slotH-imageH)/2,imageW,imageH,TRUE);
                y=(std::max)(y,slotY+slotH)+lineGap;
            }
            if (detailsExpanded) {
                place(IDC_RESULTS_TABLE,margin,y,inner,dluY(95)); y+=dluY(95)+lineGap;
                place(IDC_RESULTS_BODY,margin,y,inner,dluY(36)); y+=dluY(36)+lineGap;
                const int noteH=(std::max)(dluY(12),measure(IDC_RESULTS_NOTE,inner));
                place(IDC_RESULTS_NOTE,margin,y,inner,noteH); y+=noteH+lineGap;
                if (apply) {
                    HWND table=GetDlgItem(window,IDC_RESULTS_TABLE);
                    RECT bounds{}; GetClientRect(table,&bounds);
                    const int available=(std::max)(3L,bounds.right);
                    const int tableMetric=available*36/100, tableRun=available*32/100;
                    ListView_SetColumnWidth(table,0,tableMetric);
                    ListView_SetColumnWidth(table,1,tableRun);
                    ListView_SetColumnWidth(table,2,available-tableMetric-tableRun);
                }
            }
        }
        y+=lineGap;
        const std::array<int,4> ids=page == Page::Select ? std::array<int,4>{IDOK,IDCANCEL,0,0} :
            page == Page::Progress ? std::array<int,4>{IDCANCEL,0,0,0} :
            std::array<int,4>{IDC_RESULTS_SAVE,IDC_RESULTS_SAVE_TEXT,IDC_RESULTS_REPORT,IDCANCEL};
        const int count=page == Page::Select ? 2 : page == Page::Progress ? 1 : 4;
        std::array<int,4> widths{};
        int total=gap*(count-1);
        for (int i=0;i<count;++i) { widths[i]=naturalWidth(ids[i]); total+=widths[i]; }
        // Buttons may wrap their captions, but never force the window wider.
        if (total>inner) {
            const int each=(std::max)(1,(inner-gap*(count-1))/count);
            total=gap*(count-1);
            for (int i=0;i<count;++i) { widths[i]=each; total+=each; }
        }
        int buttonH=buttonMinH;
        for (int i=0;i<count;++i) buttonH=(std::max)(buttonH,measure(ids[i],widths[i]-buttonPadding)+dluY(4));
        const int detailWidth=page == Page::Card ? (std::min)(inner,naturalWidth(IDC_RESULTS_DETAILS)) : 0;
        const bool separateDetails=page == Page::Card && detailWidth+gap+total>inner;
        if (separateDetails) {
            const int detailH=(std::max)(buttonMinH,measure(IDC_RESULTS_DETAILS,detailWidth-buttonPadding)+dluY(4));
            place(IDC_RESULTS_DETAILS,margin,y,detailWidth,detailH); y+=detailH+lineGap;
        } else if (page == Page::Card) {
            buttonH=(std::max)(buttonH,measure(IDC_RESULTS_DETAILS,detailWidth-buttonPadding)+dluY(4));
            place(IDC_RESULTS_DETAILS,margin,y,detailWidth,buttonH);
        }
        int x=clientWidth-margin-total;
        for (int i=0;i<count;++i) { place(ids[i],x,y,widths[i],buttonH); x+=widths[i]+gap; }
        y+=buttonH+dluY(7);
        ReleaseDC(window,dc);
        return y;
    }
    void layout() const {
        RECT area{}; GetClientRect(window,&area);
        arrange(area.right,true);
    }
    void sizeForPage() const {
        const UINT dpi=GetDpiForWindow(window);
        const int widthDip=page == Page::Select ? 440 : page == Page::Progress ? 400 :
            detailsExpanded ? 600 : image ? 560 : 480;
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)) return;
        const RECT screen=monitor.rcWork;
        const DWORD style=GetWindowLongW(window,GWL_STYLE), exStyle=GetWindowLongW(window,GWL_EXSTYLE);
        using AdjustForDpi = BOOL (WINAPI*)(LPRECT,DWORD,BOOL,DWORD,UINT);
        const auto adjustForDpi=reinterpret_cast<AdjustForDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"AdjustWindowRectExForDpi"));
        auto adjust=[&](RECT* r) {
            if (adjustForDpi) adjustForDpi(r,style,FALSE,exStyle,dpi);
            else AdjustWindowRectEx(r,style,FALSE,exStyle);
        };
        RECT frame{}; adjust(&frame);
        const int frameWidth=frame.right-frame.left;
        const int maxWindowWidth=(screen.right-screen.left)*9/10;
        const int width=(std::max)(1,(std::min)(MulDiv(widthDip,dpi,96),maxWindowWidth-frameWidth));
        RECT desired{0,0,width,arrange(width,false)}, current{};
        adjust(&desired);
        GetWindowRect(window,&current);
        const int w=(std::min)(maxWindowWidth,static_cast<int>(desired.right-desired.left));
        const int h=(std::min)(screen.bottom-screen.top,desired.bottom-desired.top);
        const auto x=std::clamp(current.left+(current.right-current.left-w)/2,screen.left,screen.right-w);
        const auto y=std::clamp(current.top+(current.bottom-current.top-h)/2,screen.top,screen.bottom-h);
        SetWindowPos(window,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    void toggleDetails() {
        if (page != Page::Card) return;
        detailsExpanded = !detailsExpanded;
        if (thumbnail) ShowWindow(thumbnail, detailsExpanded ? SW_HIDE : SW_SHOW);
        label(IDC_RESULTS_DETAILS, detailsExpanded ? T::HideDetails : T::Details);
        for (int id : {IDC_RESULTS_TABLE, IDC_RESULTS_BODY, IDC_RESULTS_NOTE}) show(id, detailsExpanded);
        sizeForPage();
        layout();
    }
    void detail() const {
        const int index=ListView_GetNextItem(GetDlgItem(window,IDC_RESULTS_TABLE),-1,LVNI_SELECTED);
        if (index<0 || static_cast<size_t>(index)>=card.rows.size()) return;
        const auto& row=card.rows[index];
        std::wstring value=row.label;
        for (size_t i=0;i<row.values.size();++i)
            value += L"\r\n" + card.headings[i+1] + L"  —  " + row.values[i].text;
        SetDlgItemTextW(window,IDC_RESULTS_BODY,value.c_str());
    }
    void populate() {
        HWND table=GetDlgItem(window,IDC_RESULTS_TABLE);
        ListView_SetExtendedListViewStyle(table,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP|LVS_EX_GRIDLINES);
        SetWindowTextW(table,card.title.c_str());
        LOGFONTW font{};
        if (GetObjectW(reinterpret_cast<HFONT>(SendMessageW(table,WM_GETFONT,0,0)),sizeof(font),&font)) {
            wcscpy_s(font.lfFaceName,L"Consolas");
            numericFont=CreateFontIndirectW(&font);
        }
        for (int i=0;i<3;++i) {
            LVCOLUMNW column{};
            column.mask=LVCF_TEXT|LVCF_FMT|LVCF_WIDTH;
            column.pszText=card.headings[i].data();
            column.fmt=i==0 ? LVCFMT_LEFT : LVCFMT_RIGHT;
            column.cx=120;
            SendMessageW(table,LVM_INSERTCOLUMNW,i,reinterpret_cast<LPARAM>(&column));
        }
        for (int i=0;i<static_cast<int>(card.rows.size());++i) {
            auto& row=card.rows[i];
            LVITEMW item{};
            item.mask=LVIF_TEXT;
            item.iItem=i;
            item.pszText=row.label.data();
            SendMessageW(table,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&item));
            for (int j=0;j<2;++j) {
                item.iSubItem=j+1;
                item.pszText=row.values[j].text.data();
                SendMessageW(table,LVM_SETITEMTEXTW,i,reinterpret_cast<LPARAM>(&item));
            }
        }
        ListView_SetItemState(table,0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        SetDlgItemTextW(window,IDC_RESULTS_NOTE,card.note.c_str());
        detail();
        layout();
        SetFocus(GetDlgItem(window, IDC_RESULTS_DETAILS));
    }
    void setPage(Page next) {
        page = next;
        for (int id : kChoices) show(id, next == Page::Select);
        for (int id : kDescriptions) show(id, next == Page::Select);
        for (int id : {IDC_RESULTS_STAGE, IDC_RESULTS_PROGRESS, IDC_RESULTS_ELAPSED}) show(id, next == Page::Progress);
        show(IDOK, next == Page::Select);
        show(IDC_RESULTS_REPORT, next == Page::Card);
        show(IDC_RESULTS_SAVE, next == Page::Card);
        show(IDC_RESULTS_SAVE_TEXT, next == Page::Card);
        for (int id : {IDC_RESULTS_SUMMARY_METRIC, IDC_RESULTS_SUMMARY_RUN, IDC_RESULTS_SUMMARY_REFERENCE,
                       IDC_RESULTS_TYPICAL_LABEL, IDC_RESULTS_TYPICAL_RUN, IDC_RESULTS_TYPICAL_REFERENCE,
                       IDC_RESULTS_WORST_LABEL, IDC_RESULTS_WORST_RUN, IDC_RESULTS_WORST_REFERENCE,
                       IDC_RESULTS_COMPARISON, IDC_RESULTS_RAN, IDC_RESULTS_DLSS_NOTE, IDC_RESULTS_DETAILS})
            show(id, next == Page::Card);
        show(IDC_RESULTS_COMPARISON, next == Page::Card && hasComparison);
        for (int id : {IDC_RESULTS_WORST_LABEL, IDC_RESULTS_WORST_RUN, IDC_RESULTS_WORST_REFERENCE})
            show(id, next == Page::Card && mode != BenchmarkMode::FilmTour);
        for (int id : {IDC_RESULTS_TABLE, IDC_RESULTS_NOTE}) show(id, next == Page::Card && detailsExpanded);
        if (thumbnail) ShowWindow(thumbnail, next == Page::Card && !detailsExpanded ? SW_SHOW : SW_HIDE);
        show(IDC_RESULTS_BODY, next == Page::Card && detailsExpanded);
        label(IDCANCEL, next == Page::Progress ? T::Cancel : (closeViewer ? T::Close : T::Back));
        sizeForPage();
        layout();
        SendMessageW(window, DM_SETDEFID, next == Page::Select ? IDOK : IDCANCEL, 0);
    }

    void select() {
        setPage(Page::Select);
        label(IDC_RESULTS_TITLE, T::Select);
        updateTitleFont();
        label(IDOK, T::Run);
        int first = 0;
        for (size_t i = 0; i < kChoices.size(); ++i) {
            const auto reason = context.availability[i];
            const bool eligible = reason == BenchmarkReason::None;
            const auto description = resultText(eligible ? kDescriptionText[i] : resultsReasonText(reason));
            auto name = std::wstring(resultText(resultsModeText(static_cast<BenchmarkMode>(i)))) + L" \u00b7 GPU";
            SetDlgItemTextW(window, kChoices[i], name.c_str());
            SetDlgItemTextW(window, kDescriptions[i], description);
            EnableWindow(GetDlgItem(window, kChoices[i]), eligible);
            if (eligible && !first) first = kChoices[i];
        }
        if (first) CheckRadioButton(window, kChoices.front(), kChoices.back(), first);
        else label(IDOK, T::Results);
        sizeForPage();
        layout();
        SetFocus(GetDlgItem(window, first ? first : IDOK));
    }

    void finish(BenchmarkSnapshot last, BenchmarkReason reason = BenchmarkReason::None) {
        KillTimer(window, 1);
        KillTimer(window, 3);
        last.mode = mode;
        if (reason != BenchmarkReason::None) {
            last.reason = reason;
            switch (reason) {
            case BenchmarkReason::Cancelled: last.outcome = ResultsOutcome::Cancelled; break;
            case BenchmarkReason::TimeLimit:
            case BenchmarkReason::WorkerExited:
            case BenchmarkReason::GraphicsReset: last.outcome = ResultsOutcome::Interrupted; break;
            default: last.outcome = ResultsOutcome::Unavailable; break;
            }
        }
        closeViewer = context.closeOnly || last.reason == BenchmarkReason::GraphicsReset || last.reason == BenchmarkReason::TimeLimit;
        card = makeResultsCard(context,last);
        if (last.mode == BenchmarkMode::RenderTest && last.outcome == ResultsOutcome::Completed) image = worker.image();
        if (image && image->valid()) {
            // Keep the full-resolution DIB and draw it directly into the fitted slot.
            thumbnailBgra = image->rgba;
            for (size_t i = 0; i < thumbnailBgra.size(); i += 4) std::swap(thumbnailBgra[i], thumbnailBgra[i+2]);
            thumbnail = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_OWNERDRAW,
                0, 0, 1, 1, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        // Full precision for the record (the card rounds to one decimal); used to measure the
        // RTX 5080 reference for each build (docs/BENCHMARK_REFERENCE.md).
        BOO_LOG_INFO("Results", std::format("Benchmark mode {} outcome {} input {}x{} output {}x{} warm-up {} samples {} median {:.4f} ms worst {:.4f} ms",
            static_cast<int>(last.mode), static_cast<int>(last.outcome), last.inputWidth, last.inputHeight,
            last.width, last.height, last.warmupCount, last.measuredCount, last.medianMs, last.worstMs));
        if (last.mode == BenchmarkMode::FilmTour)
            for (size_t i = 0; i < kFilmTourSceneCount; ++i)
                BOO_LOG_INFO("Results", std::format("Film tour scene {} median {:.4f} ms worst {:.4f} ms",
                    i + 1, last.tourMediansMs[i], last.tourWorstMs[i]));
        setPage(Page::Card);
        SendDlgItemMessageW(window,IDC_RESULTS_TITLE,WM_SETFONT,SendMessageW(window,WM_GETFONT,0,0),TRUE);
        SetDlgItemTextW(window,IDC_RESULTS_TITLE,card.title.c_str());
        label(IDC_RESULTS_REPORT, T::ReportButton);
        label(IDC_RESULTS_SAVE, T::SavePicture);
        EnableWindow(GetDlgItem(window, IDC_RESULTS_SAVE), last.mode != BenchmarkMode::RenderTest || (image && image->valid()));
        label(IDC_RESULTS_SAVE_TEXT, T::SaveText);
        label(IDC_RESULTS_DETAILS, T::Details);
        label(IDC_RESULTS_SUMMARY_METRIC, T::Metric);
        label(IDC_RESULTS_SUMMARY_RUN, T::ThisRun);
        label(IDC_RESULTS_SUMMARY_REFERENCE, T::Reference);
        if (card.rows.size() >= 2) {
            const auto summary = [&](size_t row, int name, int run, int reference) {
                SetDlgItemTextW(window, name, card.rows[row].label.c_str());
                SetDlgItemTextW(window, run, card.rows[row].values[0].text.c_str());
                SetDlgItemTextW(window, reference, card.rows[row].values[1].text.c_str());
            };
            summary(0, IDC_RESULTS_TYPICAL_LABEL, IDC_RESULTS_TYPICAL_RUN, IDC_RESULTS_TYPICAL_REFERENCE);
            summary(1, IDC_RESULTS_WORST_LABEL, IDC_RESULTS_WORST_RUN, IDC_RESULTS_WORST_REFERENCE);
        }
        const std::wstring comparison = resultsComparisonForRun(last);
        SetDlgItemTextW(window, IDC_RESULTS_COMPARISON, comparison.c_str());
        hasComparison = !comparison.empty();
        show(IDC_RESULTS_COMPARISON, hasComparison);
        std::wstring ran = last.mode == BenchmarkMode::RenderTest && last.tilesTotal
            ? resultText(last.renderPathTraced ? T::RenderPathRan : T::RenderRaster)
            : resultText(resultsModeText(last.mode));
        if (last.mode == BenchmarkMode::RenderTest && last.width == 1280 && last.height == 720)
            ran += std::wstring(L" \u00b7 ") + resultText(T::RenderResolutionFallback);
        if (last.inputWidth && last.inputHeight && last.width && last.height) {
            const auto resolution = std::format(L"{} \u00d7 {} \u2192 {} \u00d7 {}", last.inputWidth, last.inputHeight, last.width, last.height);
            ran += L" \u00b7 " + (UiStrings::instance().rightToLeft() ? L"\u202d" + resolution + L"\u202c" : resolution);
        }
        if (last.mode == BenchmarkMode::DlssQuality)
            ran += std::wstring(L" \u00b7 ") + resultText(last.evidence[static_cast<size_t>(BenchmarkFeature::Upscaling)] == BenchmarkEvidence::DlssGpu ? T::DlssRanGpu : T::DlssDidNotRunGpu);
        if (last.mode == BenchmarkMode::FilmTour)
            ran += std::wstring(L" \u00b7 ") + resultText(T::Raster) + L" \u00b7 " + resultText(T::TourConditions);
        SetDlgItemTextW(window, IDC_RESULTS_RAN, ran.c_str());
        SetDlgItemTextW(window, IDC_RESULTS_DLSS_NOTE, last.mode == BenchmarkMode::DlssQuality ? resultText(T::DlssNote) : L"");
        populate();
        sizeForPage();
        layout();
    }

    void start() {
        if (page != Page::Select) return;
        for (size_t i = 0; i < kChoices.size(); ++i) {
            if (IsDlgButtonChecked(window, kChoices[i]) != BST_CHECKED || context.availability[i] != BenchmarkReason::None) continue;
            mode = static_cast<BenchmarkMode>(i);
            auto request = context.adapter;
            request.mode = mode;
            request.ownerWindow = reinterpret_cast<uintptr_t>(window);
            setPage(Page::Progress);
            SendDlgItemMessageW(window,IDC_RESULTS_TITLE,WM_SETFONT,SendMessageW(window,WM_GETFONT,0,0),TRUE);
            label(IDC_RESULTS_TITLE, resultsModeText(mode));
            label(IDC_RESULTS_PROGRESS, T::Progress);
            label(IDC_RESULTS_STAGE, T::Preparing);
            startedAt=GetTickCount64();
            HWND bar=GetDlgItem(window, IDC_RESULTS_PROGRESS);
            SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
            progressVisual.indeterminate = true;
            progressVisual.position = 0;
            if (IsWindowVisible(bar) && !IsIconic(window)) SetTimer(window, 3, 16, nullptr);
            SetDlgItemTextW(window, IDC_RESULTS_ELAPSED, (std::wstring(resultText(T::Elapsed)) + L"  0:00").c_str());
            sizeForPage();
            layout();
            SetFocus(GetDlgItem(window, IDCANCEL));
            if (!SetTimer(window, 1, 50, nullptr)) { finish({}, BenchmarkReason::WorkerExited); return; }
            if (!worker.start(request)) finish({}, BenchmarkReason::Busy);
            return;
        }
        if (std::ranges::none_of(context.availability, [](auto r) { return r == BenchmarkReason::None; }))
            finish({}, context.availability[0]);
    }

    void tick() {
        if (page != Page::Progress) return;
        const auto state = worker.poll();
        const auto snapshot = worker.snapshot();
        if (snapshot.reason == BenchmarkReason::GraphicsReset) {
            // A reset observed before driver teardown remains terminal even if
            // the viewer cancels, the pipe closes, or final retirement stalls.
            worker.cancel();
            finish(snapshot, BenchmarkReason::GraphicsReset);
            return;
        }
        switch (state) {
        case ResultsWorkerState::completed: finish(snapshot); return;
        case ResultsWorkerState::cancelled: finish(snapshot, BenchmarkReason::Cancelled); return;
        case ResultsWorkerState::timedOut: finish(snapshot, BenchmarkReason::TimeLimit); return;
        case ResultsWorkerState::failed:
            finish(snapshot, snapshot.reason == BenchmarkReason::None ? BenchmarkReason::WorkerExited : snapshot.reason);
            return;
        default: break;
        }
        T stage = T::Preparing;
        std::wstring frames;
        switch (snapshot.stage) {
        case BenchmarkStage::Warming:
            stage = T::Warming;
            frames = snapshot.mode == BenchmarkMode::FilmTour
                ? std::format(L"{} / {}", snapshot.tourWarmupCount, kFilmTourWarmupCount)
                : std::format(L"{} / {}", snapshot.warmupCount, kResultsWarmupCount);
            break;
        case BenchmarkStage::Measuring:
            stage = T::Measuring;
            frames = snapshot.mode == BenchmarkMode::FilmTour
                ? std::format(L"{} / {}", snapshot.tourMeasuredCount, kFilmTourMeasuredCount)
                : snapshot.mode == BenchmarkMode::RenderTest
                ? std::format(L"{} / {} \u00b7 {} spp", snapshot.tilesDone, snapshot.tilesTotal, snapshot.samplesPerPixel)
                : std::format(L"{} / {}", snapshot.measuredCount, kResultsMeasuredCount);
            break;
        case BenchmarkStage::Finishing:
        case BenchmarkStage::Finished: stage = T::Finishing; break;
        default: break;
        }
        if (!frames.empty() && UiStrings::instance().rightToLeft()) frames = L"\u202d" + frames + L"\u202c";
        const auto scene = snapshot.mode == BenchmarkMode::FilmTour &&
            (snapshot.stage == BenchmarkStage::Warming || snapshot.stage == BenchmarkStage::Measuring)
            ? std::format(L"{} {} / {} \u00b7 ", resultText(T::TourScene),
                          (std::min)(snapshot.tourScene + 1, static_cast<uint32_t>(kFilmTourSceneCount)), kFilmTourSceneCount)
            : std::wstring{};
        const auto status = scene + resultText(stage) + (frames.empty() ? L"" : L"  " + frames);
        if (progress != status) {
            progress = status;
            SetDlgItemTextW(window, IDC_RESULTS_STAGE, progress.c_str());
            sizeForPage();
            layout();
        }
        const auto fraction = resultsProgress(snapshot);
        HWND bar=GetDlgItem(window, IDC_RESULTS_PROGRESS);
        if (fraction.marquee != progressVisual.indeterminate) {
            progressVisual.indeterminate = fraction.marquee;
            if (fraction.marquee && IsWindowVisible(bar) && !IsIconic(window)) SetTimer(window, 3, 16, nullptr);
            else KillTimer(window, 3);
        }
        if (!fraction.marquee) {
            progressVisual.position = fraction.position;
            SendMessageW(bar,PBM_SETPOS,fraction.position,0);
        }
        InvalidateRect(bar, nullptr, FALSE);
        const auto seconds=(GetTickCount64()-startedAt)/1000;
        const auto elapsed=std::format(L"{}  {}:{:02}", resultText(T::Elapsed), seconds/60, seconds%60);
        SetDlgItemTextW(window,IDC_RESULTS_ELAPSED,elapsed.c_str());
    }

    void leave() {
        if (saving) return; // The STA picker owns this window until its operation completes.
        if (page == Page::Progress) {
            worker.cancel();
            // A deadline or reset that already won the race must keep the viewer closed.
            tick();
            if (page == Page::Progress) finish(worker.snapshot(), BenchmarkReason::Cancelled);
        } else {
            EndDialog(window, closeViewer ? IDCLOSE : IDCANCEL);
        }
    }

    void save(ResultsSaveKind kind) {
        if (page != Page::Card || saving || (mode == BenchmarkMode::RenderTest && kind == ResultsSaveKind::Picture && !image)) return;
        const auto& strings = UiStrings::instance();
        const auto& tag = strings.languageTag();
        auto job = std::make_shared<SaveOperation>();
        job->kind = kind;
        if (!SetTimer(window, 2, 80, nullptr)) { label(IDC_RESULTS_TITLE, T::FileSaveFailed); return; }
        try {
            // Playback may have initialized the viewer thread as MTA. The shell
            // picker needs its own STA. No worker touches the mutable dialog state.
            std::thread([job, owner = window, frozen = card, rtl = strings.rightToLeft(),
                         locale = std::wstring(tag.begin(), tag.end()),
                         title = std::wstring(resultText(kind == ResultsSaveKind::Picture ? T::SavePicture : T::SaveText)),
                         kind, mode = mode, image = image] {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (SUCCEEDED(com)) {
                    try { if (IsWindow(owner)) job->status = pickAndSaveResults(owner, frozen, locale, rtl, title, kind, mode,
                        mode == BenchmarkMode::RenderTest && kind == ResultsSaveKind::Picture ? image.get() : nullptr); }
                    catch (...) { job->status = ResultsSaveStatus::Failed; }
                    CoUninitialize();
                }
                job->done.store(true);
            }).detach();
        } catch (...) {
            KillTimer(window, 2);
            label(IDC_RESULTS_TITLE, T::FileSaveFailed);
            return;
        }
        saving = std::move(job);
        EnableWindow(GetDlgItem(window, IDC_RESULTS_SAVE), FALSE);
        EnableWindow(GetDlgItem(window, IDC_RESULTS_SAVE_TEXT), FALSE);
        EnableWindow(GetDlgItem(window, IDC_RESULTS_DETAILS), FALSE);
        EnableWindow(GetDlgItem(window, IDC_RESULTS_REPORT), FALSE);
        EnableWindow(GetDlgItem(window, IDCANCEL), FALSE);
    }
    void checkSave() {
        if (!saving || !saving->done.load()) return;
        KillTimer(window, 2);
        auto status = saving->status;
        const auto kind = saving->kind;
        saving.reset();
        EnableWindow(GetDlgItem(window, IDC_RESULTS_SAVE), mode != BenchmarkMode::RenderTest || (image && image->valid()));
        EnableWindow(GetDlgItem(window, IDC_RESULTS_SAVE_TEXT), TRUE);
        EnableWindow(GetDlgItem(window, IDC_RESULTS_DETAILS), TRUE);
        EnableWindow(GetDlgItem(window, IDC_RESULTS_REPORT), TRUE);
        EnableWindow(GetDlgItem(window, IDCANCEL), TRUE);
        if (status != ResultsSaveStatus::Cancelled) {
            const auto rtl = UiStrings::instance().rightToLeft() ? MB_RTLREADING | MB_RIGHT : 0;
            MessageBoxW(window, resultText(status == ResultsSaveStatus::Saved ?
                    (kind == ResultsSaveKind::Picture ? T::PictureSaved : T::TextSaved) : T::FileSaveFailed),
                resultText(kind == ResultsSaveKind::Picture ? T::SavePicture : T::SaveText), MB_OK | (status == ResultsSaveStatus::Saved ? MB_ICONINFORMATION : MB_ICONWARNING) | rtl);
        }
        SetFocus(GetDlgItem(window, kind == ResultsSaveKind::Picture ? IDC_RESULTS_SAVE : IDC_RESULTS_SAVE_TEXT));
    }

    void report() const {
        if (page != Page::Card || saving) return;
        const auto rtl = UiStrings::instance().rightToLeft() ? MB_RTLREADING | MB_RIGHT : 0;
        if (MessageBoxW(window, resultText(T::ConfirmReport), resultText(T::ReportButton),
            MB_YESNO | MB_DEFBUTTON2 | MB_ICONINFORMATION | rtl) == IDYES) {
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", kReportPage, nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                MessageBoxW(window, resultText(T::Unavailable), resultText(T::ReportButton), MB_OK | MB_ICONWARNING | rtl);
        }
    }
};

INT_PTR CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<Dialog*>(lParam);
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        dialog->window = window;
        dialog->initializeTheme();
        SetWindowTextW(window, resultText(T::WindowTitle));
        if (dialog->context.initialFailure != BenchmarkReason::None) dialog->finish({}, dialog->context.initialFailure);
        else dialog->select();
        return FALSE; // The selected page placed the keyboard focus.
    }
    if (!dialog) return FALSE;
    if (message == WM_DRAWITEM) {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (results_theme::drawButton(*draw, dialog->progressVisual.accent)) return TRUE;
        if (dialog->thumbnail && draw->hwndItem == dialog->thumbnail && dialog->image && dialog->image->valid()) {
            BITMAPINFO source{};
            source.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            source.bmiHeader.biWidth = static_cast<LONG>(dialog->image->width);
            source.bmiHeader.biHeight = -static_cast<LONG>(dialog->image->height);
            source.bmiHeader.biPlanes = 1;
            source.bmiHeader.biBitCount = 32;
            source.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(draw->hDC, HALFTONE);
            SetBrushOrgEx(draw->hDC, 0, 0, nullptr);
            StretchDIBits(draw->hDC, draw->rcItem.left, draw->rcItem.top,
                draw->rcItem.right-draw->rcItem.left, draw->rcItem.bottom-draw->rcItem.top,
                0, 0, dialog->image->width, dialog->image->height,
                dialog->thumbnailBgra.data(), &source, DIB_RGB_COLORS, SRCCOPY);
            return TRUE;
        }
    }
    if (message == WM_CTLCOLORDLG) return reinterpret_cast<INT_PTR>(dialog->backgroundBrush);
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN || message == WM_CTLCOLOREDIT) {
        const HWND control=reinterpret_cast<HWND>(lParam);
        bool secondary = control == GetDlgItem(window, IDC_RESULTS_NOTE) || control == GetDlgItem(window, IDC_RESULTS_DLSS_NOTE);
        for (int id : kDescriptions) secondary |= control == GetDlgItem(window, id);
        HDC dc=reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, secondary ? kSecondaryText : kPrimaryText);
        SetBkColor(dc, kBackground);
        SetBkMode(dc, control == GetDlgItem(window, IDC_RESULTS_BODY) ? OPAQUE : TRANSPARENT);
        return reinterpret_cast<INT_PTR>(dialog->backgroundBrush);
    }
    if (message == WM_DPICHANGED) {
        // Let the dialog manager apply its per-monitor font and child scaling first.
        PostMessageW(window,WM_APP+1,0,0);
        return FALSE;
    }
    if (message == WM_APP+1) {
        dialog->updateMessageFont();
        if (dialog->page == Page::Select) dialog->updateTitleFont();
        dialog->sizeForPage();
        dialog->layout();
        return TRUE;
    }
    if (message==WM_SIZE || message==WM_SHOWWINDOW) {
        if (message == WM_SIZE) dialog->layout();
        if (dialog->page == Page::Progress && dialog->progressVisual.indeterminate &&
            IsWindowVisible(GetDlgItem(window, IDC_RESULTS_PROGRESS)) && !IsIconic(window) &&
            (message != WM_SHOWWINDOW || wParam)) SetTimer(window, 3, 16, nullptr);
        else KillTimer(window, 3);
        if (message == WM_SIZE) return TRUE;
    }
    if (message==WM_NOTIFY) {
        auto* note=reinterpret_cast<NMHDR*>(lParam);
        if (note->hwndFrom == ListView_GetHeader(GetDlgItem(window, IDC_RESULTS_TABLE)) && note->code == NM_CUSTOMDRAW) {
            auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
            LRESULT result = CDRF_DODEFAULT;
            if (draw->dwDrawStage == CDDS_PREPAINT) result = CDRF_NOTIFYITEMDRAW;
            else if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                HBRUSH brush = CreateSolidBrush(RGB(45, 45, 45));
                FillRect(draw->hdc, &draw->rc, brush);
                DeleteObject(brush);
                wchar_t text[256]{};
                HDITEMW item{}; item.mask = HDI_TEXT | HDI_FORMAT; item.pszText = text; item.cchTextMax = 256;
                Header_GetItem(note->hwndFrom, static_cast<int>(draw->dwItemSpec), &item);
                RECT rect = draw->rc; rect.left += 6; rect.right -= 6;
                SetBkMode(draw->hdc, TRANSPARENT); SetTextColor(draw->hdc, kPrimaryText);
                const UINT alignment = (item.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT;
                DrawTextW(draw->hdc, text, -1, &rect, alignment | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                result = CDRF_SKIPDEFAULT;
            }
            SetWindowLongPtrW(window, DWLP_MSGRESULT, result);
            return TRUE;
        }
        if (note->idFrom!=IDC_RESULTS_TABLE) return FALSE;
        if (note->code==LVN_ITEMCHANGED) { dialog->detail(); return TRUE; }
        if (note->code==NM_CUSTOMDRAW && dialog->numericFont) {
            auto* draw=reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            LRESULT result=CDRF_DODEFAULT;
            switch (draw->nmcd.dwDrawStage) {
                case CDDS_PREPAINT: result=CDRF_NOTIFYITEMDRAW; break;
                case CDDS_ITEMPREPAINT: {
                    // Dark selection instead of the light system highlight.
                    if (ListView_GetItemState(note->hwndFrom, static_cast<int>(draw->nmcd.dwItemSpec), LVIS_SELECTED)) {
                        draw->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
                        draw->clrTextBk = GetFocus() == note->hwndFrom ? RGB(62, 62, 62) : RGB(52, 52, 52);
                        draw->clrText = RGB(255, 255, 255);
                    }
                    result=CDRF_NOTIFYSUBITEMDRAW;
                    break;
                }
                case CDDS_ITEMPREPAINT|CDDS_SUBITEM: {
                    const size_t row=draw->nmcd.dwItemSpec;
                    const bool selected = ListView_GetItemState(note->hwndFrom, static_cast<int>(row), LVIS_SELECTED) != 0;
                    draw->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
                    draw->clrTextBk = selected ? (GetFocus() == note->hwndFrom ? RGB(62, 62, 62) : RGB(52, 52, 52)) : kBackground;
                    draw->clrText = selected ? RGB(255, 255, 255) : kPrimaryText;
                    result = CDRF_NEWFONT;
                    const int column=draw->iSubItem;
                    if (row<dialog->card.rows.size() && column>0 && column<=2 &&
                        dialog->card.rows[row].values[column-1].numeric)
                        SelectObject(draw->nmcd.hdc,dialog->numericFont);
                    else
                        SelectObject(draw->nmcd.hdc,reinterpret_cast<HFONT>(SendMessageW(note->hwndFrom,WM_GETFONT,0,0)));
                    break;
                }
            }
            SetWindowLongPtrW(window,DWLP_MSGRESULT,result);
            return TRUE;
        }
    }
    if (message == WM_TIMER && wParam == 1) { dialog->tick(); return TRUE; }
    if (message == WM_TIMER && wParam == 2) { dialog->checkSave(); return TRUE; }
    if (message == WM_TIMER && wParam == 3) {
        HWND bar = GetDlgItem(window, IDC_RESULTS_PROGRESS);
        if (dialog->page == Page::Progress && dialog->progressVisual.indeterminate && IsWindowVisible(bar) && !IsIconic(window))
            InvalidateRect(bar, nullptr, FALSE);
        else KillTimer(window, 3);
        return TRUE;
    }
    if (message == WM_CLOSE) { dialog->leave(); return TRUE; }
    if (message != WM_COMMAND) return FALSE;
    switch (LOWORD(wParam)) {
    case IDOK: dialog->start(); return TRUE;
    case IDCANCEL: dialog->leave(); return TRUE;
    case IDC_RESULTS_REPORT: dialog->report(); return TRUE;
    case IDC_RESULTS_DETAILS: dialog->toggleDetails(); return TRUE;
    case IDC_RESULTS_SAVE: dialog->save(ResultsSaveKind::Picture); return TRUE;
    case IDC_RESULTS_SAVE_TEXT: dialog->save(ResultsSaveKind::Text); return TRUE;
    default: return FALSE;
    }
}
} // namespace

bool openResultsDialog(HWND owner, const ResultsDialogContext& context) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
    if (!InitCommonControlsEx(&controls)) return context.closeOnly || context.initialFailure==BenchmarkReason::GraphicsReset;
    Dialog dialog(context);
    const int resource = UiStrings::instance().rightToLeft() ? IDD_RESULTS_RTL : IDD_RESULTS;
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resource), owner, proc, reinterpret_cast<LPARAM>(&dialog));
    return dialog.closeViewer || context.initialFailure == BenchmarkReason::GraphicsReset;
}
} // namespace boo
