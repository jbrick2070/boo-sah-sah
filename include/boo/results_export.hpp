#pragma once

#include "boo/results_benchmark.hpp"
#include <filesystem>
#include <string>
#include <string_view>

struct HWND__;
namespace boo {
struct ResultCard;
enum class ResultsSaveStatus { Cancelled, Saved, Failed };
enum class ResultsSaveKind { Picture, Text };

// Writes one immutable result via a verified, same-directory atomic replacement.
// Requires an initialized COM apartment for picture rendering.
bool writeResultsFile(const std::filesystem::path& file, const ResultCard& card,
                      std::wstring_view language, bool rtl, ResultsSaveKind kind);
// Called on a dedicated STA for the native picker and offline software rendering.
ResultsSaveStatus pickAndSaveResults(HWND__* owner, const ResultCard& card,
                                    const std::wstring& language, bool rtl, const std::wstring& title,
                                    ResultsSaveKind kind, BenchmarkMode mode);
} // namespace boo
