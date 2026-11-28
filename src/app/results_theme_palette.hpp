#pragma once

#include <windows.h>

namespace boo::results_theme {
constexpr COLORREF background = RGB(32, 32, 32);
constexpr COLORREF foreground = RGB(230, 230, 230);
constexpr COLORREF secondary = RGB(160, 160, 160);
constexpr COLORREF fallbackAccent = RGB(76, 194, 255);
constexpr unsigned lightAccentChannel(unsigned channel) {
    return (channel * 2 + 255 * 3 + 2) / 5;
}
constexpr COLORREF accentColor(bool available, COLORREF systemAccent) {
    return available ? RGB(lightAccentChannel(GetRValue(systemAccent)),
                           lightAccentChannel(GetGValue(systemAccent)),
                           lightAccentChannel(GetBValue(systemAccent))) : fallbackAccent;
}
// The WCAG sRGB transfer function needs a 2.4 power. Calculate it as
// x^2 * fifth_root(x^2) so this palette remains usable in static assertions.
constexpr double linearChannel(unsigned channel) {
    const double srgb = channel / 255.0;
    if (srgb <= 0.04045) return srgb / 12.92;
    const double base = (srgb + 0.055) / 1.055;
    const double square = base * base;
    double root = 1.0;
    for (int i = 0; i < 48; ++i) {
        const double fourth = root * root * root * root;
        root = (4.0 * root + square / fourth) / 5.0;
    }
    return square * root;
}
constexpr COLORREF accentTextColor(COLORREF accent) {
    const double luminance = 0.2126 * linearChannel(GetRValue(accent)) +
                             0.7152 * linearChannel(GetGValue(accent)) +
                             0.0722 * linearChannel(GetBValue(accent));
    return luminance >= 0.175 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}
constexpr COLORREF blend40(COLORREF color) {
    return RGB(((color & 0xffu) * 2 + (background & 0xffu) * 3) / 5,
               (((color >> 8) & 0xffu) * 2 + ((background >> 8) & 0xffu) * 3) / 5,
               (((color >> 16) & 0xffu) * 2 + ((background >> 16) & 0xffu) * 3) / 5);
}
struct ButtonColors { COLORREF fill, border, text; };
constexpr ButtonColors buttonColors(bool primary, bool hovered, bool pressed, bool enabled, COLORREF accent) {
    ButtonColors colors = primary
        ? ButtonColors{accent, accent, accentTextColor(accent)}
        : ButtonColors{pressed ? RGB(41, 41, 41) : hovered ? RGB(53, 53, 53) : RGB(45, 45, 45),
                       RGB(58, 58, 58), foreground};
    if (!enabled) colors = {blend40(colors.fill), blend40(colors.border), blend40(colors.text)};
    return colors;
}
} // namespace boo::results_theme
