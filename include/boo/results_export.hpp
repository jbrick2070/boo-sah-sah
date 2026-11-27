#pragma once

#include <filesystem>
#include <string>
#include <string_view>

struct HWND__;
namespace boo {
struct ResultCard;
enum class ResultsSaveStatus { Cancelled, Saved, Failed };

// Creates a new subdirectory and never replaces an existing result. Both files
// come from the immutable card that was displayed. Requires an initialized COM apartment.
bool writeResultsFiles(const std::filesystem::path& directory, const ResultCard& card,
                       std::wstring_view language, bool rtl);
// Called on a dedicated STA for the native picker and offline software rendering.
ResultsSaveStatus pickAndSaveResults(HWND__* owner, const ResultCard& card,
                                    const std::wstring& language, bool rtl, const std::wstring& title);
} // namespace boo
