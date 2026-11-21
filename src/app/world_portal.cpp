#include "world_portal.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <array>
#include <string>

namespace boo {
namespace {

constexpr size_t kMaxWorldAddress = 305;

bool lowerLetter(wchar_t c) noexcept { return c >= L'a' && c <= L'z'; }
bool decimal(wchar_t c) noexcept { return c >= L'0' && c <= L'9'; }

bool validPublicHost(std::wstring_view host) noexcept {
    if (host.empty() || host.size() > 253 || host.find(L'.') == host.npos) return false;
    for (;;) {
        const auto dot = host.find(L'.');
        const auto label = host.substr(0, dot);
        if (label.empty() || label.size() > 63 || label.front() == L'-' || label.back() == L'-')
            return false;
        if (!std::all_of(label.begin(), label.end(), [](wchar_t c) {
            return lowerLetter(c) || decimal(c) || c == L'-';
        })) return false;
        if (dot == host.npos) return lowerLetter(label.front());
        host.remove_prefix(dot + 1);
    }
}

INT_PTR CALLBACK dialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        // Keep excess text visible so overlength addresses are rejected.
        SendDlgItemMessageW(dialog, IDC_WORLD_ADDRESS, EM_SETLIMITTEXT, 2048, 0);
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL)) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message != WM_COMMAND || LOWORD(wParam) != IDOK) return FALSE;

    std::array<wchar_t, kMaxWorldAddress + 1> buffer{};
    const HWND edit = GetDlgItem(dialog, IDC_WORLD_ADDRESS);
    const int length = GetWindowTextLengthW(edit);
    if (length > 0 && static_cast<size_t>(length) <= kMaxWorldAddress &&
        GetWindowTextW(edit, buffer.data(), static_cast<int>(buffer.size())) == length &&
        isPublicWorldLocator({buffer.data(), static_cast<size_t>(length)})) {
        auto* destination = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dialog, DWLP_USER));
        destination->assign(buffer.data(), static_cast<size_t>(length));
        EndDialog(dialog, IDOK);
    } else {
        SetDlgItemTextW(dialog, IDC_WORLD_STATUS,
            L"Use https://host.name/worlds/ followed by a lowercase UUID. "
            L"Leave out invite secrets, ports and extra parameters.");
        SetFocus(edit);
    }
    return TRUE;
}

bool hasWorldVisitor() noexcept {
    DWORD chars = 0;
    const HRESULT result = AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_PROGID,
                                              L"demoverse", nullptr, nullptr, &chars);
    return SUCCEEDED(result) && chars > 1;
}

INT_PTR CALLBACK destinationProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        const auto* address = reinterpret_cast<const std::wstring*>(lParam);
        SetDlgItemTextW(dialog, IDC_WORLD_ADDRESS, address->c_str());
        const bool available = hasWorldVisitor();
        EnableWindow(GetDlgItem(dialog, IDC_WORLD_VISITOR), available);
        SetDlgItemTextW(dialog, IDC_WORLD_STATUS, available
            ? L"Open this world in the registered Demo-Verse visitor, or view its public page in your browser."
            : L"Windows has no registered Demo-Verse visitor. You can open the public page in your browser.");
        SetFocus(GetDlgItem(dialog, IDCANCEL));
        return FALSE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL)) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    const WORD choice = LOWORD(wParam);
    if (choice != IDC_WORLD_VISITOR && choice != IDC_WORLD_BROWSER) return FALSE;
    const auto* address = reinterpret_cast<const std::wstring*>(GetWindowLongPtrW(dialog, DWLP_USER));
    std::wstring uri = *address;
    if (choice == IDC_WORLD_VISITOR) {
        if (!hasWorldVisitor()) {
            EnableWindow(GetDlgItem(dialog, IDC_WORLD_VISITOR), FALSE);
            SetDlgItemTextW(dialog, IDC_WORLD_STATUS, L"Windows has no registered Demo-Verse visitor.");
            return TRUE;
        }
        uri = worldVisitorUri(*address);
        if (uri.empty()) return TRUE;
    }
    SHELLEXECUTEINFOW request{};
    request.cbSize = sizeof(request);
    request.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    request.hwnd = dialog;
    request.lpVerb = L"open";
    request.lpFile = uri.c_str();
    request.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&request)) {
        EndDialog(dialog, IDOK);
    } else {
        SetDlgItemTextW(dialog, IDC_WORLD_STATUS, L"Windows could not open the selected application.");
    }
    return TRUE;
}

} // namespace

bool isPublicWorldLocator(std::wstring_view address) noexcept {
    constexpr std::wstring_view scheme = L"https://";
    constexpr std::wstring_view path = L"/worlds/";
    if (address.size() > kMaxWorldAddress || !address.starts_with(scheme)) return false;
    address.remove_prefix(scheme.size());
    const auto slash = address.find(L'/');
    if (slash == address.npos || !validPublicHost(address.substr(0, slash))) return false;
    address.remove_prefix(slash);
    if (!address.starts_with(path)) return false;
    address.remove_prefix(path.size());
    if (address.size() != 36) return false;
    for (size_t i = 0; i < address.size(); ++i) {
        const wchar_t c = address[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != L'-') return false;
        } else if (!(decimal(c) || (c >= L'a' && c <= L'f'))) return false;
    }
    return true;
}

bool worldPortalDialogAvailable() noexcept {
    return FindResourceW(nullptr, MAKEINTRESOURCEW(IDD_WORLD_PORTAL), MAKEINTRESOURCEW(5)) != nullptr;
}

std::wstring worldVisitorUri(std::wstring_view address) {
    if (!isPublicWorldLocator(address)) return {};
    address.remove_prefix(std::wstring_view(L"https://").size());
    std::wstring uri = L"demoverse://visit/";
    return uri.append(address);
}

void openWorldPortalDialog(HWND owner) {
    std::wstring destination;
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_WORLD_PORTAL),
                        owner, dialogProc, reinterpret_cast<LPARAM>(&destination)) != IDOK) return;

    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_WORLD_DESTINATION),
                    owner, destinationProc, reinterpret_cast<LPARAM>(&destination));
}

} // namespace boo
