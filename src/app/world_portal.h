#pragma once

#define IDD_WORLD_PORTAL 210
#define IDC_WORLD_ADDRESS 211
#define IDC_WORLD_STATUS 212
#define IDD_WORLD_DESTINATION 213
#define IDC_WORLD_VISITOR 214
#define IDC_WORLD_BROWSER 215

#ifndef RC_INVOKED
#include <string>
#include <string_view>

struct HWND__;

namespace boo {

bool isPublicWorldLocator(std::wstring_view address) noexcept;
std::wstring worldVisitorUri(std::wstring_view address);
bool worldPortalDialogAvailable() noexcept;
void openWorldPortalDialog(HWND__* owner);

} // namespace boo
#endif
