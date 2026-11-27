#include "boo/results_dialog.hpp"
#include "boo/results_card.hpp"
#include "boo/results_export.hpp"
#include "boo/results_worker.hpp"
#include "boo/ui_text.hpp"
#include "results_dialog_ids.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <thread>

namespace boo {
namespace {
using T = ResultText;
enum class Page { Select, Progress, Card };
constexpr std::array<int, 3> kChoices{IDC_RESULTS_RASTER, IDC_RESULTS_PT, IDC_RESULTS_DLSS};
constexpr wchar_t kReportPage[] = L"https://github.com/jbrick2070/boo-sah-sah/issues/new";

struct SaveOperation {
    std::atomic<bool> done{false};
    ResultsSaveStatus status = ResultsSaveStatus::Failed;
};
struct Dialog {
    explicit Dialog(const ResultsDialogContext& c) : context(c) {}
    const ResultsDialogContext& context;
    ResultsWorker worker;
    HWND window{};
    Page page = Page::Select;
    BenchmarkMode mode = BenchmarkMode::Raster;
    bool closeViewer = false;
    ResultCard card;
    HFONT numericFont{};
    std::wstring progress;
    std::shared_ptr<SaveOperation> saving;
    ~Dialog() { if (numericFont) DeleteObject(numericFont); }

    void label(int id, T key) const { SetDlgItemTextW(window, id, resultText(key)); }
    void show(int id, bool visible) const { ShowWindow(GetDlgItem(window, id), visible ? SW_SHOW : SW_HIDE); }
    void layout() const {
        RECT area{}, unit{0,0,8,8};
        GetClientRect(window,&area);
        MapDialogRect(window,&unit);
        const int dx = (std::max)(1L,unit.right), dy = (std::max)(1L,unit.bottom);
        const int w = area.right, h = area.bottom, inner = (std::max)(1,w-2*dx);
        const int buttons = (std::max)(dy*4,h-dy*4);
        auto place = [&](int id,int x,int y,int width,int height) {
            MoveWindow(GetDlgItem(window,id),x,y,(std::max)(1,width),(std::max)(1,height),TRUE);
        };
        place(IDC_RESULTS_TITLE,dx,dy/2,inner,dy*2);
        const int bw = (inner-2*dx)/3;
        place(IDOK,dx,buttons,bw,3*dy);
        place(IDC_RESULTS_SAVE,dx,buttons,bw,3*dy);
        place(IDC_RESULTS_REPORT,2*dx+bw,buttons,bw,3*dy);
        place(IDCANCEL,3*dx+2*bw,buttons,inner-2*(bw+dx),3*dy);
        if (page != Page::Card) {
            for (size_t i=0;i<kChoices.size();++i) place(kChoices[i],dx,3*dy+int(i)*2*dy,inner,2*dy);
            const int top = page==Page::Select ? 10*dy : 3*dy;
            place(IDC_RESULTS_BODY,dx,top,inner,buttons-dy/2-top);
            return;
        }
        place(IDC_RESULTS_SUBTITLE,dx,dy*3,inner,2*dy);
        const int tableY = 5*dy, noteY = buttons-3*dy, detailsY = noteY-5*dy;
        place(IDC_RESULTS_TABLE,dx,tableY,inner,detailsY-tableY-dy/2);
        place(IDC_RESULTS_BODY,dx,detailsY,inner,5*dy-dy/2);
        place(IDC_RESULTS_NOTE,dx,noteY,inner,3*dy);
        HWND table = GetDlgItem(window,IDC_RESULTS_TABLE);
        RECT bounds{};
        GetClientRect(table,&bounds);
        const int available = (std::max)(3L,bounds.right);
        const int metric=available*36/100, run=available*32/100;
        ListView_SetColumnWidth(table,0,metric);
        ListView_SetColumnWidth(table,1,run);
        ListView_SetColumnWidth(table,2,available-metric-run);
    }
    void expandForTable() const {
        RECT desired{0,0,450,304}, current{};
        MapDialogRect(window,&desired);
        AdjustWindowRectEx(&desired,GetWindowLongW(window,GWL_STYLE),FALSE,GetWindowLongW(window,GWL_EXSTYLE));
        GetWindowRect(window,&current);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)) return;
        const RECT screen=monitor.rcWork;
        const int w=(std::min)(screen.right-screen.left,(std::max)(current.right-current.left,desired.right-desired.left));
        const int h=(std::min)(screen.bottom-screen.top,(std::max)(current.bottom-current.top,desired.bottom-desired.top));
        const auto x=std::clamp(current.left+(current.right-current.left-w)/2,screen.left,screen.right-w);
        const auto y=std::clamp(current.top+(current.bottom-current.top-h)/2,screen.top,screen.bottom-h);
        SetWindowPos(window,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
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
        SetDlgItemTextW(window,IDC_RESULTS_SUBTITLE,card.subtitle.c_str());
        SetDlgItemTextW(window,IDC_RESULTS_NOTE,card.note.c_str());
        detail();
        layout();
        SetFocus(table);
    }
    void setPage(Page next) {
        page = next;
        for (int id : kChoices) show(id, next == Page::Select);
        show(IDOK, next == Page::Select);
        show(IDC_RESULTS_REPORT, next == Page::Card);
        show(IDC_RESULTS_SAVE, next == Page::Card);
        for (int id : {IDC_RESULTS_TABLE,IDC_RESULTS_SUBTITLE,IDC_RESULTS_NOTE}) show(id,next==Page::Card);
        if (next==Page::Card) expandForTable();
        layout();
        label(IDCANCEL, next == Page::Progress ? T::Cancel : (closeViewer ? T::Close : T::Back));
        if (next != Page::Select) SendMessageW(window, DM_SETDEFID, IDCANCEL, 0);
    }

    void select() {
        setPage(Page::Select);
        label(IDC_RESULTS_TITLE, T::Select);
        label(IDOK, T::Run);
        std::wstring reasons;
        int first = 0;
        for (size_t i = 0; i < kChoices.size(); ++i) {
            const auto name = std::wstring(resultText(resultsModeText(static_cast<BenchmarkMode>(i)))) + L" · GPU";
            const auto reason = context.availability[i];
            const bool eligible = reason == BenchmarkReason::None;
            SetDlgItemTextW(window, kChoices[i], name.c_str());
            EnableWindow(GetDlgItem(window, kChoices[i]), eligible);
            if (eligible && !first) first = kChoices[i];
            if (!eligible) {
                if (!reasons.empty()) reasons += L"\r\n";
                reasons += name + L" — " + resultText(resultsReasonText(reason));
            }
        }
        SetDlgItemTextW(window, IDC_RESULTS_BODY, reasons.c_str());
        show(IDC_RESULTS_BODY, !reasons.empty());
        if (first) CheckRadioButton(window, kChoices.front(), kChoices.back(), first);
        else label(IDOK, T::Results); // An unsupported adapter still has a useful compatibility card.
        SetFocus(GetDlgItem(window, first ? first : IDC_RESULTS_BODY));
    }

    void finish(BenchmarkSnapshot last, BenchmarkReason reason = BenchmarkReason::None) {
        KillTimer(window, 1);
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
        setPage(Page::Card);
        SetDlgItemTextW(window,IDC_RESULTS_TITLE,card.title.c_str());
        label(IDC_RESULTS_REPORT, T::Report);
        label(IDC_RESULTS_SAVE, T::Save);
        show(IDC_RESULTS_BODY, true);
        populate();
    }

    void start() {
        if (page != Page::Select) return;
        for (size_t i = 0; i < kChoices.size(); ++i) {
            if (IsDlgButtonChecked(window, kChoices[i]) != BST_CHECKED || context.availability[i] != BenchmarkReason::None) continue;
            mode = static_cast<BenchmarkMode>(i);
            auto request = context.adapter;
            request.mode = mode;
            setPage(Page::Progress);
            label(IDC_RESULTS_TITLE, T::Preparing);
            show(IDC_RESULTS_BODY, true);
            SetDlgItemTextW(window, IDC_RESULTS_BODY, resultText(resultsModeText(mode)));
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
            frames = std::format(L"{} / {}", snapshot.warmupCount, kResultsWarmupCount);
            break;
        case BenchmarkStage::Measuring:
            stage = T::Checking;
            frames = std::format(L"{} / {}", snapshot.measuredCount, kResultsMeasuredCount);
            break;
        case BenchmarkStage::Finishing:
        case BenchmarkStage::Finished: stage = T::Finishing; break;
        default: break;
        }
        if (!frames.empty() && UiStrings::instance().rightToLeft()) frames = L"\u202d" + frames + L"\u202c";
        const auto status = std::wstring(resultText(stage)) + (frames.empty() ? L"" : L"  " + frames);
        if (progress != status) {
            progress = status;
            SetDlgItemTextW(window, IDC_RESULTS_TITLE, progress.c_str());
        }
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

    void save() {
        if (page != Page::Card || saving) return;
        const auto& strings = UiStrings::instance();
        const auto& tag = strings.languageTag();
        auto job = std::make_shared<SaveOperation>();
        if (!SetTimer(window, 2, 80, nullptr)) { label(IDC_RESULTS_TITLE, T::SaveFailed); return; }
        try {
            // Playback may have initialized the viewer thread as MTA. The shell
            // picker needs its own STA. No worker touches the mutable dialog state.
            std::thread([job, owner = window, frozen = card, rtl = strings.rightToLeft(),
                         locale = std::wstring(tag.begin(), tag.end()), title = std::wstring(resultText(T::Save))] {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (SUCCEEDED(com)) {
                    try { if (IsWindow(owner)) job->status = pickAndSaveResults(owner, frozen, locale, rtl, title); }
                    catch (...) { job->status = ResultsSaveStatus::Failed; }
                    CoUninitialize();
                }
                job->done.store(true);
            }).detach();
        } catch (...) {
            KillTimer(window, 2);
            label(IDC_RESULTS_TITLE, T::SaveFailed);
            return;
        }
        saving = std::move(job);
        EnableWindow(GetDlgItem(window, IDC_RESULTS_SAVE), FALSE);
        EnableWindow(GetDlgItem(window, IDC_RESULTS_REPORT), FALSE);
        EnableWindow(GetDlgItem(window, IDCANCEL), FALSE);
    }
    void checkSave() {
        if (!saving || !saving->done.load()) return;
        KillTimer(window, 2);
        auto status = saving->status;
        saving.reset();
        EnableWindow(GetDlgItem(window, IDC_RESULTS_SAVE), TRUE);
        EnableWindow(GetDlgItem(window, IDC_RESULTS_REPORT), TRUE);
        EnableWindow(GetDlgItem(window, IDCANCEL), TRUE);
        if (status == ResultsSaveStatus::Cancelled) return;
        const auto rtl = UiStrings::instance().rightToLeft() ? MB_RTLREADING | MB_RIGHT : 0;
        MessageBoxW(window, resultText(status == ResultsSaveStatus::Saved ? T::Saved : T::SaveFailed),
            resultText(T::Save), MB_OK | (status == ResultsSaveStatus::Saved ? MB_ICONINFORMATION : MB_ICONWARNING) | rtl);
    }

    void report() const {
        if (page != Page::Card || saving) return;
        const auto rtl = UiStrings::instance().rightToLeft() ? MB_RTLREADING | MB_RIGHT : 0;
        if (MessageBoxW(window, resultText(T::ConfirmReport), resultText(T::Report),
            MB_YESNO | MB_DEFBUTTON2 | MB_ICONINFORMATION | rtl) == IDYES) {
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", kReportPage, nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                MessageBoxW(window, resultText(T::Unavailable), resultText(T::Report), MB_OK | MB_ICONWARNING | rtl);
        }
    }
};

INT_PTR CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        dialog = reinterpret_cast<Dialog*>(lParam);
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        dialog->window = window;
        SetWindowTextW(window, resultText(T::Run));
        if (dialog->context.initialFailure != BenchmarkReason::None) dialog->finish({}, dialog->context.initialFailure);
        else dialog->select();
        return FALSE; // The selected page placed the keyboard focus.
    }
    if (!dialog) return FALSE;
    if (message==WM_SIZE) { dialog->layout(); return TRUE; }
    if (message==WM_GETMINMAXINFO) {
        RECT minimum{0,0,250,205};
        MapDialogRect(window,&minimum);
        auto* info=reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x=minimum.right;
        info->ptMinTrackSize.y=minimum.bottom;
        return TRUE;
    }
    if (message==WM_NOTIFY) {
        auto* note=reinterpret_cast<NMHDR*>(lParam);
        if (note->idFrom!=IDC_RESULTS_TABLE) return FALSE;
        if (note->code==LVN_ITEMCHANGED) { dialog->detail(); return TRUE; }
        if (note->code==NM_CUSTOMDRAW && dialog->numericFont) {
            auto* draw=reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            LRESULT result=CDRF_DODEFAULT;
            switch (draw->nmcd.dwDrawStage) {
                case CDDS_PREPAINT: result=CDRF_NOTIFYITEMDRAW; break;
                case CDDS_ITEMPREPAINT: result=CDRF_NOTIFYSUBITEMDRAW; break;
                case CDDS_ITEMPREPAINT|CDDS_SUBITEM: {
                    const size_t row=draw->nmcd.dwItemSpec;
                    const int column=draw->iSubItem;
                    if (row<dialog->card.rows.size() && column>0 && column<=2 &&
                        dialog->card.rows[row].values[column-1].numeric) {
                        SelectObject(draw->nmcd.hdc,dialog->numericFont);
                        result=CDRF_NEWFONT;
                    }
                    break;
                }
            }
            SetWindowLongPtrW(window,DWLP_MSGRESULT,result);
            return TRUE;
        }
    }
    if (message == WM_TIMER && wParam == 1) { dialog->tick(); return TRUE; }
    if (message == WM_TIMER && wParam == 2) { dialog->checkSave(); return TRUE; }
    if (message == WM_CLOSE) { dialog->leave(); return TRUE; }
    if (message != WM_COMMAND) return FALSE;
    switch (LOWORD(wParam)) {
    case IDOK: dialog->start(); return TRUE;
    case IDCANCEL: dialog->leave(); return TRUE;
    case IDC_RESULTS_REPORT: dialog->report(); return TRUE;
    case IDC_RESULTS_SAVE: dialog->save(); return TRUE;
    default: return FALSE;
    }
}
} // namespace

bool openResultsDialog(HWND owner, const ResultsDialogContext& context) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&controls)) return context.closeOnly || context.initialFailure==BenchmarkReason::GraphicsReset;
    Dialog dialog(context);
    const int resource = UiStrings::instance().rightToLeft() ? IDD_RESULTS_RTL : IDD_RESULTS;
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resource), owner, proc, reinterpret_cast<LPARAM>(&dialog));
    return dialog.closeViewer || context.initialFailure == BenchmarkReason::GraphicsReset;
}
} // namespace boo
