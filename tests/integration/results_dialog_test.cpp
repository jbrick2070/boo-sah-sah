// Exercise the real Win32 dialog and the viewer's system-menu entry without activating the user's desktop.
#include <windows.h>
#include <commctrl.h>
#include <oleacc.h>
#include <wrl/client.h>
#include "boo/results_card.hpp"
#include "../../src/app/results_dialog_ids.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}
template<class F> bool until(std::chrono::milliseconds duration, F condition) {
    const auto end = Clock::now() + duration;
    do {
        if (condition()) return true;
        Sleep(10);
    } while (Clock::now() < end);
    return false;
}
struct Viewer {
    PROCESS_INFORMATION pi{};
    ~Viewer() {
        if (pi.hProcess) {
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 2000);
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
};
struct Search { DWORD pid; const wchar_t* name; HWND owner{}, result{}; };
BOOL CALLBACK findWindow(HWND wnd, LPARAM data) {
    auto& s = *reinterpret_cast<Search*>(data);
    wchar_t cls[96]{};
    DWORD pid{};
    GetWindowThreadProcessId(wnd, &pid);
    if (pid == s.pid && GetClassNameW(wnd, cls, 96) && std::wstring_view(cls) == s.name && IsWindowVisible(wnd) &&
        (!s.owner || s.owner == GetWindow(wnd, GW_OWNER))) {
        s.result = wnd;
        return FALSE;
    }
    return TRUE;
}
HWND find(HDESK desktop, DWORD pid, const wchar_t* cls, HWND owner = nullptr) {
    Search s{pid, cls, owner};
    EnumDesktopWindows(desktop, findWindow, reinterpret_cast<LPARAM>(&s));
    return s.result;
}
std::wstring textOf(HWND control) {
    if (!control) return {};
    wchar_t buffer[8192]{};
    DWORD_PTR length{};
    if (!SendMessageTimeoutW(control, WM_GETTEXT, std::size(buffer), reinterpret_cast<LPARAM>(buffer),
                             SMTO_ABORTIFHUNG, 200, &length)) return {};
    return std::wstring(buffer, length);
}
HWND focused(HWND dialog) {
    GUITHREADINFO state{sizeof(state)};
    if (!GetGUIThreadInfo(GetWindowThreadProcessId(dialog, nullptr), &state)) return nullptr;
    return state.hwndFocus;
}
void key(HWND target, WPARAM keyCode) {
    require(target && PostMessageW(target, WM_KEYDOWN, keyCode, 1) && PostMessageW(target, WM_KEYUP, keyCode, 0xC0000001),
            "Could not post keyboard command to private desktop window");
}
std::vector<std::wstring> readRows(HWND dialog) {
    const HWND table=GetDlgItem(dialog,IDC_RESULTS_TABLE);
    const HWND header=reinterpret_cast<HWND>(SendMessageW(table,LVM_GETHEADER,0,0));
    require(IsWindowVisible(table) && focused(dialog)==table && header &&
        SendMessageW(header,HDM_GETITEMCOUNT,0,0)==3, "Keyboard-accessible three-column table missing");
    Microsoft::WRL::ComPtr<IAccessible> headerAccessibility;
    require(SUCCEEDED(AccessibleObjectFromWindow(header,OBJID_CLIENT,IID_PPV_ARGS(&headerAccessibility))),
            "Column headers have no accessibility provider");
    for (const auto& [childId,heading] : {std::pair{1,L"Measurement"},std::pair{2,L"This run"},std::pair{3,L"RTX 5080 reference"}}) {
        VARIANT child{};
        child.vt=VT_I4;
        child.lVal=childId;
        BSTR name{};
        const bool matches=SUCCEEDED(headerAccessibility->get_accName(child,&name)) && name &&
            std::wstring_view(name,SysStringLen(name)).find(heading)!=std::wstring_view::npos;
        SysFreeString(name);
        require(matches,"Screen reader did not receive the expected column heading");
    }
    const int count=static_cast<int>(SendMessageW(table,LVM_GETITEMCOUNT,0,0));
    require(count>10 && count<100, "Incomplete benchmark table");
    std::vector<std::wstring> rows;
    for (int i=0;i<count;++i) {
        if (i) key(table,VK_DOWN);
        std::wstring row;
        require(until(1s,[&] {
            row=textOf(GetDlgItem(dialog,IDC_RESULTS_BODY));
            return !row.empty() && (rows.empty() || rows.back()!=row);
        }), "Arrow key did not expose the next row");
        require(row.find(L"\r\nThis run  —  ")!=row.npos && row.find(L"\r\nRTX 5080 reference  —  ")!=row.npos,
            "Expanded row lacks one of its accessible column headings");
        rows.push_back(std::move(row));
    }
    return rows;
}
UINT benchmarkMenu(HWND film) {
    const auto menu = GetSystemMenu(film, FALSE);
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        wchar_t label[256]{};
        if (GetMenuStringW(menu, i, label, 256, MF_BYPOSITION) && std::wstring_view(label).find(L"Ctrl+B") != std::wstring_view::npos)
            return GetMenuItemID(menu, i);
    }
    return 0;
}
void run(HDESK desktop, const std::wstring& desktopName, const std::filesystem::path& exe, bool warp) {
    Viewer viewer;
    STARTUPINFOW si{sizeof(si)};
    std::wstring assigned = desktopName;
    si.lpDesktop = assigned.data();
    std::wstring args = L"\"" + exe.wstring() + L"\" --no-activate --mute --windowed --preset safe --lang en-US --disable-npu --disable-rt --force-silent-clock";
    const auto log = exe.parent_path() / (warp ? L"results-dialog-warp.log" : L"results-dialog-hardware.log");
    args += L" --log-file \"" + log.wstring() + L"\"";
    if (warp) args += L" --force-warp";
    else args += L" --legacy-m0-visual"; // Keep viewer startup independent of full-film shader-cache warmth.
    require(CreateProcessW(exe.c_str(), args.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           exe.parent_path().c_str(), &si, &viewer.pi) != FALSE, "Could not start viewer on private desktop");
    HWND film{}, dialog{};
    require(until(25s, [&] {
        film = find(desktop, viewer.pi.dwProcessId, L"BooSahSahGlassOrchidWindowClass");
        return film && benchmarkMenu(film);
    }), "Film window with benchmark menu did not initialize");
    // The first GPU memory observation precedes admission in the live viewer.
    require(until(25s,[&]{
        std::ifstream input(log);
        for (std::string line; std::getline(input,line); ) if (line.find("Start-up checksums")!=line.npos) return true;
        return false;
    }), "Viewer did not finish its first presentation");
    const HWND initialFocus = focused(film);
    auto open = [&] {
        require(PostMessageW(film, WM_SYSCOMMAND, benchmarkMenu(film), 0) != FALSE, "Could not select benchmark menu command");
        require(until(25s, [&] {
            dialog = find(desktop, viewer.pi.dwProcessId, L"#32770");
            return dialog && GetDlgItem(dialog, IDC_RESULTS_RASTER) && !IsWindowEnabled(film);
        }), "Benchmark selector did not open modally");
    };
    open();
    auto control = [&](int id) { return GetDlgItem(dialog, id); };
    require(textOf(control(IDC_RESULTS_TITLE)) == L"Select a test", "Missing selector heading");
    const auto reasons = textOf(control(IDC_RESULTS_BODY));
    const bool raster = IsWindowEnabled(control(IDC_RESULTS_RASTER)) != FALSE;
    if (warp || !raster) {
        require(!IsWindowEnabled(control(IDC_RESULTS_RASTER)) && !IsWindowEnabled(control(IDC_RESULTS_PT)) &&
                !IsWindowEnabled(control(IDC_RESULTS_DLSS)), "Software test controls accepted input");
        require(reasons.find(L"Raster") != reasons.npos && reasons.find(L"Path tracing") != reasons.npos &&
                reasons.find(L"DLSS") != reasons.npos, "Missing explanations for disabled options");
        if (warp) require(reasons.find(L"requires a hardware GPU") != reasons.npos, "Wrong WARP reason");
        require(focused(dialog) == control(IDC_RESULTS_BODY), "Keyboard cannot reach disabled-mode explanations");
        key(focused(dialog), VK_TAB);
        require(until(1s, [&] { return focused(dialog) == control(IDOK); }), "Tab from explanation did not reach View Results");
        key(focused(dialog), VK_RETURN);
    } else {
        require(focused(dialog) == control(IDC_RESULTS_RASTER), "Enabled raster choice did not receive focus");
        key(focused(dialog), VK_TAB);
        require(until(1s, [&] { return focused(dialog) != control(IDC_RESULTS_RASTER); }), "Tab did not leave test choices");
        if (!reasons.empty()) require(focused(dialog) == control(IDC_RESULTS_BODY), "Ineligible option reason is missing from tab order");
        for (int i=0; i<4 && focused(dialog)!=control(IDOK); ++i) {
            auto previous = focused(dialog);
            key(previous, VK_TAB);
            require(until(1s, [&] { return focused(dialog)!=previous; }), "Tab did not advance");
        }
        require(focused(dialog)==control(IDOK), "Run is missing from tab order");
        key(focused(dialog), VK_RETURN);
        require(until(2s, [&] { return textOf(control(IDCANCEL)) == L"Cancel" && focused(dialog) == control(IDCANCEL); }),
                "Cancellation inaccessible on progress page");
        key(focused(dialog), VK_ESCAPE);
    }
    require(until(2s, [&] { return IsWindowVisible(control(IDC_RESULTS_TABLE)) && focused(dialog)==control(IDC_RESULTS_TABLE); }),
        "Native result card did not appear");
    std::wstring card;
    for (const auto& row : readRows(dialog)) card+=row+L"\n";
    require(card.find(L"0 / 180") != card.npos && card.find(L"Typical GPU frame (ms)\r\nThis run  —  Unavailable") != card.npos &&
            card.find(L"Worst GPU frame (ms)\r\nThis run  —  Unavailable") != card.npos && card.find(L"RC1") != card.npos,
            "Missing score-free card fields");
    require(card.find(raster ? L"Cancelled" : L"Unavailable") != card.npos, "Wrong outcome on card");
    require(textOf(control(IDCANCEL)) == L"Back to film" && focused(dialog) == control(IDC_RESULTS_TABLE), "Card navigation inaccessible");
    require(PostMessageW(dialog,WM_COMMAND,IDC_RESULTS_REPORT,reinterpret_cast<LPARAM>(control(IDC_RESULTS_REPORT))) != FALSE,
            "Could not activate report confirmation");
    HWND confirmation{};
    require(until(2s, [&] {
        confirmation = find(desktop, viewer.pi.dwProcessId, L"#32770", dialog);
        return confirmation && GetDlgItem(confirmation,IDNO) && !IsWindowEnabled(dialog);
    }), "Report action omitted confirmation");
    require(focused(confirmation) == GetDlgItem(confirmation,IDNO), "Public reporting must default to No");
    require(PostMessageW(confirmation,WM_COMMAND,IDNO,0) != FALSE &&
            until(2s, [&] { return !IsWindow(confirmation) && IsWindowEnabled(dialog); }),
            "Declining public reporting did not return to the result");
    key(focused(dialog), VK_ESCAPE);
    require(until(2s, [&] { return !IsWindow(dialog) && IsWindowEnabled(film); }), "Escape failed to restore film window");
    if (initialFocus) require(until(1s, [&] { return focused(film) == initialFocus; }), "Film focus was not restored");
    // A second entry demonstrates that the film owner is processing commands again.
    open();
    key(focused(dialog), VK_ESCAPE);
    require(until(2s, [&] { return !IsWindow(dialog) && IsWindowEnabled(film); }), "Back did not restore owner");
    require(PostMessageW(film, WM_CLOSE, 0, 0) != FALSE, "Could not close viewer");
    require(WaitForSingleObject(viewer.pi.hProcess, 8000) == WAIT_OBJECT_0, "Viewer did not close after test");
    DWORD code{};
    require(GetExitCodeProcess(viewer.pi.hProcess,&code) && code==0, "Viewer exited unsuccessfully");
    std::cout << (warp ? "WARP" : "Default adapter") << (raster ? " cancellation" : " unavailable-mode")
        << " dialog passed on unshown desktop\n";
}

struct ChildById { int id; HWND window{}; };
BOOL CALLBACK matchId(HWND child, LPARAM data) {
    auto& target = *reinterpret_cast<ChildById*>(data);
    if (GetDlgCtrlID(child) != target.id) return TRUE;
    target.window = child;
    return FALSE;
}
HWND byId(HWND parent, int id) {
    ChildById target{id};
    EnumChildWindows(parent, matchId, reinterpret_cast<LPARAM>(&target));
    return target.window;
}

void testResetSave(HDESK desktop, DWORD pid, HWND dialog, const std::wstring& card) {
    const auto scratch = std::filesystem::temp_directory_path() /
        (L"BooResetCard-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    require(std::filesystem::create_directory(scratch), "Could not create private test destination");
    const auto folder = scratch / L"BOO-SAH-SAH-results";
    const auto txt = folder / L"BOO-SAH-SAH-results.txt";
    const auto png = folder / L"BOO-SAH-SAH-results.png";
    struct Cleanup {
        const std::filesystem::path& txt, &png, &folder, &scratch;
        ~Cleanup() { DeleteFileW(txt.c_str()); DeleteFileW(png.c_str()); RemoveDirectoryW(folder.c_str()); RemoveDirectoryW(scratch.c_str()); }
    } cleanup{txt,png,folder,scratch};
    auto openPicker = [&] {
        HWND picker{};
        const auto save = GetDlgItem(dialog,IDC_RESULTS_SAVE);
        require(IsWindowVisible(save) && IsWindowEnabled(save), "Save is unavailable after device reset");
        require(PostMessageW(dialog,WM_COMMAND,IDC_RESULTS_SAVE,reinterpret_cast<LPARAM>(save)) != FALSE, "Could not activate Save");
        require(until(6s,[&] {
            picker = find(desktop,pid,L"#32770",dialog);
            return picker && textOf(picker) == L"Save picture and text" && GetDlgItem(picker,IDOK);
        }), "Folder picker did not open after reset");
        return picker;
    };
    auto picker = openPicker();
    PostMessageW(picker,WM_COMMAND,IDCANCEL,0);
    require(until(3s,[&] { return !IsWindow(picker) && IsWindowEnabled(GetDlgItem(dialog,IDC_RESULTS_SAVE)); }) &&
        std::filesystem::is_empty(scratch), "Cancelled folder picker wrote files or prevented another save");
    picker = openPicker();
    // Depending on the OS version, File Open exposes edt1 directly or nested under cmb13.
    // Verify the exact, absolute test destination before accepting the picker.
    HWND edit{};
    const bool foundEdit = until(3s,[&] {
        edit = byId(picker,0x0480);
        const auto combo = byId(picker,0x047c);
        if (!edit && combo) edit = byId(combo,0x03e9);
        if (!edit && combo) {
            const auto inner = FindWindowExW(combo,nullptr,L"ComboBox",nullptr);
            if (inner) edit = FindWindowExW(inner,nullptr,L"Edit",nullptr);
        }
        wchar_t cls[32]{};
        return edit && GetClassNameW(edit,cls,32) && std::wstring_view(cls)==L"Edit";
    });
    require(foundEdit, "Could not locate the folder name edit; destination left untouched");
    DWORD_PTR result{};
    require(SendMessageTimeoutW(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(scratch.c_str()),SMTO_ABORTIFHUNG,500,&result) &&
        result && textOf(edit)==scratch.wstring(), "Could not verify the selected test destination");
    require(PostMessageW(picker,WM_COMMAND,IDOK,reinterpret_cast<LPARAM>(GetDlgItem(picker,IDOK))) != FALSE,
        "Could not accept verified folder");
    HWND acknowledgement{};
    const auto saved = [&] {
        acknowledgement = find(desktop,pid,L"#32770",dialog);
        return !IsWindow(picker) && acknowledgement && textOf(GetDlgItem(acknowledgement,65535))==L"Picture and text saved." &&
            std::filesystem::is_regular_file(png) && std::filesystem::is_regular_file(txt);
    };
    if (!until(1s,saved) && IsWindow(picker)) {
        // The first activation can navigate into the typed folder. Confirm the breadcrumb's
        // full absolute address before pressing Select Folder in this version of the picker.
        const auto address = byId(picker,1001);
        wchar_t cls[64]{};
        require(address && GetClassNameW(address,cls,64) && std::wstring_view(cls)==L"ToolbarWindow32" &&
            textOf(address).ends_with(scratch.wstring()), "Picker did not navigate into the private test folder");
        PostMessageW(picker,WM_COMMAND,IDOK,reinterpret_cast<LPARAM>(GetDlgItem(picker,IDOK)));
    }
    require(until(8s,saved), "Picker did not confirm saving both files in the selected folder");
    std::ifstream stream(txt,std::ios::binary);
    std::string content{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
    const int count = WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,card.data(),static_cast<int>(card.size()),nullptr,0,nullptr,nullptr);
    std::string expected(count,'\0');
    require(count>0 && WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,card.data(),static_cast<int>(card.size()),
        expected.data(),count,nullptr,nullptr)==count && expected==content, "Saved reset text differs from native card");
    require(std::filesystem::file_size(png)>100, "Reset result PNG is empty");
    require(PostMessageW(acknowledgement,WM_CLOSE,0,0) && until(2s,[&] { return !IsWindow(acknowledgement) && IsWindowEnabled(dialog); }),
        "Save confirmation did not close");
}
void testFixtureCard(HDESK desktop,std::wstring name,const std::filesystem::path& exe,bool stalled,bool comparison=false) {
    Viewer viewer;
    STARTUPINFOW si{sizeof(si)};
    si.lpDesktop = name.data();
    auto args = L"\"" + exe.wstring() + L"\" --no-activate --mute" + (stalled ? L" --stall-cleanup" : L"");
    if (comparison) args+=L" --comparison";
    require(CreateProcessW(exe.c_str(),args.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&si,&viewer.pi),
        "Could not launch reset fixture");
    HWND dialog{};
    require(until(4s,[&] {
        dialog = find(desktop,viewer.pi.dwProcessId,L"#32770");
        return dialog && IsWindowVisible(GetDlgItem(dialog,IDC_RESULTS_RASTER));
    }), "Reset fixture selector did not open");
    PostMessageW(dialog,WM_COMMAND,IDOK,0);
    if (comparison) {
        require(until(3s,[&]{return IsWindowVisible(GetDlgItem(dialog,IDC_RESULTS_TABLE)) &&
            focused(dialog)==GetDlgItem(dialog,IDC_RESULTS_TABLE);}),"Completed comparison did not open");
        const auto rows=readRows(dialog);
        require(rows[0].starts_with(L"Typical GPU frame (ms)\r\nThis run  —  9.4") &&
            rows[1].starts_with(L"Worst GPU frame (ms)\r\nThis run  —  11.6"),"Typical and worst values are misaligned");
        for (int i=0;i<2;++i) {
            const auto index=rows[i].rfind(L"  —  ");
            require(index!=rows[i].npos && index+5<rows[i].size() && iswdigit(rows[i][index+5]),
                "Measured reference timing did not appear beside this run");
        }
        const auto hasHardwareRow=[&](std::wstring_view prefix,std::wstring_view status) {
            return std::any_of(rows.begin(),rows.end(),[&](const auto& row) {
                const auto reference=row.find(L"\r\nRTX 5080 reference");
                return row.starts_with(prefix) && row.substr(0,reference).find(status)!=row.npos;
            });
        };
        require(hasHardwareRow(L"CPU\r\nThis run  —  AMD",L"Scripted"),
            "Completed native table omitted CPU model or verified scripted work");
        require(hasHardwareRow(L"NPU\r\nThis run  —  AMD",L"Off by preset"),
            "Completed native table omitted NPU model or preset state");
        require(textOf(GetDlgItem(dialog,IDCANCEL))==L"Back to film","Completed result did not offer Back");
        PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);
        DWORD code{};
        require(WaitForSingleObject(viewer.pi.hProcess,1800)==WAIT_OBJECT_0 && GetExitCodeProcess(viewer.pi.hProcess,&code) && code==0,
            "Completed result incorrectly requested session exit");
        std::cout << "Completed synthetic run with measured reference displayed side by side\n";
        return;
    }
    if (stalled) {
        require(until(1s,[&]{return textOf(GetDlgItem(dialog,IDC_RESULTS_TITLE))==L"Finishing" ||
            textOf(GetDlgItem(dialog,IDCANCEL))==L"Close";}), "Failed teardown never reached viewer");
        if (textOf(GetDlgItem(dialog,IDCANCEL))!=L"Close") key(focused(dialog),VK_ESCAPE);
    }
    require(until(3s,[&]{return textOf(GetDlgItem(dialog,IDCANCEL))==L"Close";}), "Reset incorrectly allowed returning to the film");
    boo::BenchmarkSnapshot snapshot{};
    snapshot.stage=boo::BenchmarkStage::Finished;
    snapshot.reason=boo::BenchmarkReason::GraphicsReset;
    snapshot.outcome=boo::ResultsOutcome::Interrupted;
    snapshot.warmupCount=30;
    snapshot.measuredCount=9;
    boo::ResultsDialogContext context;
    context.hardware={boo::ResultsCpuModel::IntelCoreUltra9_275HX,boo::ResultsNpuModel::IntelAiBoost};
    const auto expected=boo::makeResultsCard(context,snapshot);
    const auto rows=readRows(dialog);
    require(rows.size()==expected.rows.size() && textOf(GetDlgItem(dialog,IDC_RESULTS_NOTE))==expected.note &&
        textOf(GetDlgItem(dialog,IDC_RESULTS_SUBTITLE))==expected.subtitle,"Reset table metadata differs from export");
    for (size_t i=0;i<rows.size();++i) {
        const auto& row=expected.rows[i];
        require(rows[i]==row.label+L"\r\n"+expected.headings[1]+L"  —  "+row.values[0].text+
            L"\r\n"+expected.headings[2]+L"  —  "+row.values[1].text,"Reset table cell differs from export");
    }
    if (!stalled) testResetSave(desktop,viewer.pi.dwProcessId,dialog,boo::resultsCardText(expected));
    PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);
    DWORD code{};
    require(WaitForSingleObject(viewer.pi.hProcess,1800)==WAIT_OBJECT_0 && GetExitCodeProcess(viewer.pi.hProcess,&code) && code==0,
        "Reset did not request bounded session exit");
    std::cout << (stalled ? "Cancel during failed teardown" : "Save and close after reported reset") << " passed\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool softwareOnly=argc==4 && std::wstring_view(argv[3])==L"--software-only";
    if (argc != 3 && !softwareOnly) return 2;
    std::wstring name = L"BooResultsUi_" + std::to_wstring(GetCurrentProcessId());
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) { std::cerr << "CreateDesktop failed: " << GetLastError() << "\n"; return 3; }
    int result = 0;
    try {
        require(SetThreadDesktop(desktop) != FALSE, "Could not attach test thread to private desktop");
        require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)), "Could not initialize MSAA test apartment");
        struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
        const auto exe = std::filesystem::absolute(argv[1]);
        run(desktop, name, exe, true);
        if (!softwareOnly) run(desktop, name, exe, false);
        const auto fixture = std::filesystem::absolute(argv[2]);
        testFixtureCard(desktop,name,fixture,false);
        testFixtureCard(desktop,name,fixture,true);
        testFixtureCard(desktop,name,fixture,false,true);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        result = 1;
    }
    SetThreadDesktop(original);
    CloseDesktop(desktop);
    return result;
}
