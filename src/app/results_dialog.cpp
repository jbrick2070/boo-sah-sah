#include "boo/results_dialog.hpp"
#include "boo/logging.hpp"
#include "boo/results_card.hpp"
#include "boo/results_export.hpp"
#include "boo/results_reference.hpp"
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
        if (page != Page::Card) {
            const int bw = (inner-2*dx)/3;
            place(IDOK,dx,buttons,bw,3*dy);
            place(IDCANCEL,3*dx+2*bw,buttons,inner-2*(bw+dx),3*dy);
            for (size_t i=0;i<kChoices.size();++i) place(kChoices[i],dx,3*dy+int(i)*2*dy,inner,2*dy);
            const int top = page==Page::Select ? 10*dy : 3*dy;
            place(IDC_RESULTS_BODY,dx,top,inner,buttons-dy/2-top);
            return;
        }
        const int bw = (inner-3*dx)/4;
        place(IDC_RESULTS_SAVE,dx,buttons,bw,3*dy);
        place(IDC_RESULTS_SAVE_TEXT,2*dx+bw,buttons,bw,3*dy);
        place(IDC_RESULTS_REPORT,3*dx+2*bw,buttons,bw,3*dy);
        place(IDCANCEL,4*dx+3*bw,buttons,inner-3*(bw+dx),3*dy);
        const int metric=inner*36/100, run=inner*32/100, reference=inner-metric-run;
        const auto summaryRow = [&](int first, int second, int third, int y) {
            place(first,dx,y,metric,3*dy/2);
            place(second,dx+metric,y,run,3*dy/2);
            place(third,dx+metric+run,y,reference,3*dy/2);
        };
        summaryRow(IDC_RESULTS_SUMMARY_METRIC, IDC_RESULTS_SUMMARY_RUN, IDC_RESULTS_SUMMARY_REFERENCE, 3*dy);
        summaryRow(IDC_RESULTS_TYPICAL_LABEL, IDC_RESULTS_TYPICAL_RUN, IDC_RESULTS_TYPICAL_REFERENCE, 9*dy/2);
        summaryRow(IDC_RESULTS_WORST_LABEL, IDC_RESULTS_WORST_RUN, IDC_RESULTS_WORST_REFERENCE, 6*dy);
        place(IDC_RESULTS_COMPARISON,dx,15*dy/2,inner,3*dy/2);
        place(IDC_RESULTS_RAN,dx,9*dy,inner,3*dy/2);
        place(IDC_RESULTS_DLSS_NOTE,dx,21*dy/2,inner,3*dy);
        place(IDC_RESULTS_DETAILS,dx,27*dy/2,inner/4,5*dy/2);
        if (!detailsExpanded) return;
        const int tableY = 33*dy/2, noteY = buttons-3*dy, detailsY = noteY-5*dy;
        place(IDC_RESULTS_TABLE,dx,tableY,inner,(std::max)(dy,detailsY-tableY-dy/2));
        place(IDC_RESULTS_BODY,dx,detailsY,inner,5*dy-dy/2);
        place(IDC_RESULTS_NOTE,dx,noteY,inner,3*dy);
        HWND table = GetDlgItem(window,IDC_RESULTS_TABLE);
        RECT bounds{};
        GetClientRect(table,&bounds);
        const int available = (std::max)(3L,bounds.right);
        const int tableMetric=available*36/100, tableRun=available*32/100;
        ListView_SetColumnWidth(table,0,tableMetric);
        ListView_SetColumnWidth(table,1,tableRun);
        ListView_SetColumnWidth(table,2,available-tableMetric-tableRun);
    }
    void sizeForCard() const {
        RECT desired{0,0,450,detailsExpanded ? 350 : 168}, current{};
        MapDialogRect(window,&desired);
        AdjustWindowRectEx(&desired,GetWindowLongW(window,GWL_STYLE),FALSE,GetWindowLongW(window,GWL_EXSTYLE));
        GetWindowRect(window,&current);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)) return;
        const RECT screen=monitor.rcWork;
        const int w=(std::min)(screen.right-screen.left,(std::max)(current.right-current.left,desired.right-desired.left));
        const int h=(std::min)(screen.bottom-screen.top,desired.bottom-desired.top);
        const auto x=std::clamp(current.left+(current.right-current.left-w)/2,screen.left,screen.right-w);
        const auto y=std::clamp(current.top+(current.bottom-current.top-h)/2,screen.top,screen.bottom-h);
        SetWindowPos(window,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    void toggleDetails() {
        if (page != Page::Card) return;
        detailsExpanded = !detailsExpanded;
        label(IDC_RESULTS_DETAILS, detailsExpanded ? T::HideDetails : T::Details);
        for (int id : {IDC_RESULTS_TABLE, IDC_RESULTS_BODY, IDC_RESULTS_NOTE}) show(id, detailsExpanded);
        sizeForCard();
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
        show(IDOK, next == Page::Select);
        show(IDC_RESULTS_REPORT, next == Page::Card);
        show(IDC_RESULTS_SAVE, next == Page::Card);
        show(IDC_RESULTS_SAVE_TEXT, next == Page::Card);
        for (int id : {IDC_RESULTS_SUMMARY_METRIC, IDC_RESULTS_SUMMARY_RUN, IDC_RESULTS_SUMMARY_REFERENCE,
                       IDC_RESULTS_TYPICAL_LABEL, IDC_RESULTS_TYPICAL_RUN, IDC_RESULTS_TYPICAL_REFERENCE,
                       IDC_RESULTS_WORST_LABEL, IDC_RESULTS_WORST_RUN, IDC_RESULTS_WORST_REFERENCE,
                       IDC_RESULTS_COMPARISON, IDC_RESULTS_RAN, IDC_RESULTS_DLSS_NOTE, IDC_RESULTS_DETAILS})
            show(id, next == Page::Card);
        for (int id : {IDC_RESULTS_TABLE, IDC_RESULTS_NOTE}) show(id, next == Page::Card && detailsExpanded);
        if (next==Page::Card) { show(IDC_RESULTS_BODY, detailsExpanded); sizeForCard(); }
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
        // Full precision for the record (the card rounds to one decimal); used to measure the
        // RTX 5080 reference for each build (docs/BENCHMARK_REFERENCE.md).
        BOO_LOG_INFO("Results", std::format("Benchmark mode {} outcome {} input {}x{} output {}x{} warm-up {} samples {} median {:.4f} ms worst {:.4f} ms",
            static_cast<int>(last.mode), static_cast<int>(last.outcome), last.inputWidth, last.inputHeight,
            last.width, last.height, last.warmupCount, last.measuredCount, last.medianMs, last.worstMs));
        setPage(Page::Card);
        SetDlgItemTextW(window,IDC_RESULTS_TITLE,card.title.c_str());
        label(IDC_RESULTS_REPORT, T::ReportButton);
        label(IDC_RESULTS_SAVE, T::SavePicture);
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
        std::wstring comparison;
        if (card.rows.size() >= 2 && card.rows[0].values[0].numeric && card.rows[0].values[1].numeric) {
            if (const auto* reference = resultsReferenceFor(last))
                comparison = resultsComparisonLine(last.medianMs, reference->snapshot.medianMs);
        }
        SetDlgItemTextW(window, IDC_RESULTS_COMPARISON, comparison.c_str());
        std::wstring ran = resultText(resultsModeText(last.mode));
        if (last.inputWidth && last.inputHeight && last.width && last.height) {
            const auto resolution = std::format(L"{} \u00d7 {} \u2192 {} \u00d7 {}", last.inputWidth, last.inputHeight, last.width, last.height);
            ran += L" \u00b7 " + (UiStrings::instance().rightToLeft() ? L"\u202d" + resolution + L"\u202c" : resolution);
        }
        if (last.mode == BenchmarkMode::DlssQuality)
            ran += std::wstring(L" \u00b7 ") + resultText(last.evidence[static_cast<size_t>(BenchmarkFeature::Upscaling)] == BenchmarkEvidence::DlssGpu ? T::DlssRanGpu : T::DlssDidNotRunGpu);
        SetDlgItemTextW(window, IDC_RESULTS_RAN, ran.c_str());
        SetDlgItemTextW(window, IDC_RESULTS_DLSS_NOTE, last.mode == BenchmarkMode::DlssQuality ? resultText(T::DlssNote) : L"");
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

    void save(ResultsSaveKind kind) {
        if (page != Page::Card || saving) return;
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
                         kind, mode = mode] {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (SUCCEEDED(com)) {
                    try { if (IsWindow(owner)) job->status = pickAndSaveResults(owner, frozen, locale, rtl, title, kind, mode); }
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
        EnableWindow(GetDlgItem(window, IDC_RESULTS_SAVE), TRUE);
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
        SetWindowTextW(window, resultText(T::Run));
        if (dialog->context.initialFailure != BenchmarkReason::None) dialog->finish({}, dialog->context.initialFailure);
        else dialog->select();
        return FALSE; // The selected page placed the keyboard focus.
    }
    if (!dialog) return FALSE;
    if (message==WM_SIZE) { dialog->layout(); return TRUE; }
    if (message==WM_GETMINMAXINFO) {
        RECT minimum{0,0,dialog->page == Page::Card ? 400 : 250,dialog->page == Page::Card ? (dialog->detailsExpanded ? 260 : 168) : 205};
        MapDialogRect(window,&minimum);
        AdjustWindowRectEx(&minimum, GetWindowLongW(window,GWL_STYLE), FALSE, GetWindowLongW(window,GWL_EXSTYLE));
        auto* info=reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x=minimum.right-minimum.left;
        info->ptMinTrackSize.y=minimum.bottom-minimum.top;
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
    case IDC_RESULTS_DETAILS: dialog->toggleDetails(); return TRUE;
    case IDC_RESULTS_SAVE: dialog->save(ResultsSaveKind::Picture); return TRUE;
    case IDC_RESULTS_SAVE_TEXT: dialog->save(ResultsSaveKind::Text); return TRUE;
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
