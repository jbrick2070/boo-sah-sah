#pragma once

#include <cstddef>
#include <cstdint>

namespace boo {

// The words the app shows through Windows itself (message boxes, the GDI loading screen and the GDI graphics-failure panel).
// The in-film HUD and titles are drawn by shader glyph tables and are not covered here.
enum class UiText : uint32_t {
    UnsupportedGpuBody,
    UnsupportedGpuTitle,
    GraphicsFailureBody,
    Loading,
    IndependentProduction,
    Count
};

// Text for a Windows LANGID (GetUserDefaultUILanguage). Unknown languages get English.
const wchar_t* uiTextForLanguage(UiText id, uint16_t langId);
bool uiLanguageIsRightToLeft(uint16_t langId);

// Text for the signed-in user's Windows display language.
const wchar_t* uiText(UiText id);
bool uiIsRightToLeft();

// For tests: every language table, English first.
size_t uiLanguageCount();
const char* uiLanguageTag(size_t index);
const wchar_t* uiTextAt(size_t index, UiText id);

} // namespace boo
