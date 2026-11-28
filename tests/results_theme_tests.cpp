#include "../src/app/results_theme_palette.hpp"

using namespace boo::results_theme;
static_assert(accentColor(false, RGB(1, 2, 3)) == RGB(76, 194, 255));
static_assert(accentColor(true, RGB(1, 2, 3)) == RGB(153, 154, 154));
static_assert(accentColor(true, RGB(255, 0, 0)) == RGB(255, 153, 153));
static_assert(accentColor(true, RGB(255, 255, 255)) == RGB(255, 255, 255));
static_assert(accentTextColor(RGB(0, 0, 0)) == RGB(255, 255, 255));
static_assert(accentTextColor(RGB(0, 0, 255)) == RGB(255, 255, 255));
static_assert(accentTextColor(RGB(116, 116, 116)) == RGB(255, 255, 255));
static_assert(accentTextColor(RGB(117, 117, 117)) == RGB(0, 0, 0));
static_assert(accentTextColor(accentColor(true, RGB(255, 0, 0))) == RGB(0, 0, 0));
static_assert(buttonColors(false, false, false, true, fallbackAccent).fill == RGB(45, 45, 45));
static_assert(buttonColors(false, true, false, true, fallbackAccent).fill == RGB(53, 53, 53));
static_assert(buttonColors(false, true, true, true, fallbackAccent).fill == RGB(41, 41, 41));
static_assert(buttonColors(false, false, false, true, fallbackAccent).border == RGB(58, 58, 58));
static_assert(buttonColors(true, false, false, true, fallbackAccent).fill == fallbackAccent);
static_assert(buttonColors(true, false, false, true, fallbackAccent).text == RGB(0, 0, 0));
static_assert(buttonColors(true, false, false, true, RGB(0, 0, 255)).text == RGB(255, 255, 255));
static_assert(buttonColors(false, false, false, false, fallbackAccent).fill == RGB(37, 37, 37));
static_assert(buttonColors(true, false, false, false, fallbackAccent).fill == RGB(49, 96, 121));
