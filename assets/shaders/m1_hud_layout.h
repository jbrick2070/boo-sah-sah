#ifndef BOO_M1_HUD_LAYOUT_H
#define BOO_M1_HUD_LAYOUT_H

// Shared scalar layout formulas. Coordinates are in swapchain pixels, except
// panel row offsets, which are in UI units. Used by both HLSL and C++ scissors.
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#define BOO_HUD_FLOOR(x) std::floor(x)
#define BOO_HUD_MIN(a, b) (std::min)((a), (b))
#define BOO_HUD_MAX(a, b) (std::max)((a), (b))
#else
#define BOO_HUD_FLOOR(x) floor(x)
#define BOO_HUD_MIN(a, b) min((a), (b))
#define BOO_HUD_MAX(a, b) max((a), (b))
#endif

#define BOO_HUD_SCALE(h) BOO_HUD_MIN(3.0f, BOO_HUD_MAX(1.0f, BOO_HUD_FLOOR((h) / 540.0f)))
#define BOO_HUD_FULLSCREEN_PILL_WIDTH(isFull, u) (((isFull) ? 150.0f : 86.0f) * (u))
#define BOO_HUD_FULLSCREEN_PILL_X(w, width, u) ((w) - 14.0f * (u) - (width))
#define BOO_HUD_FULLSCREEN_PILL_Y(u) (14.0f * (u))
#define BOO_HUD_STUDIO_X(w, u) (0.5f * ((w) - 536.0f * (u)))
#define BOO_HUD_STUDIO_Y(h, mode, u) ((h) - ((mode) == 1u ? 60.0f : 104.0f) * (u))
#define BOO_HUD_TIMELINE_X(u) (82.0f * (u))
#define BOO_HUD_TIMELINE_WIDTH(w) BOO_HUD_MAX(90.0f, (w) * 0.20f)
#define BOO_HUD_PANEL_X(u) (14.0f * (u))
#define BOO_HUD_PANEL_Y(u, volumeVisible) (((volumeVisible) ? 44.0f : 14.0f) * (u))
#define BOO_HUD_PANEL_COMPACT(mode, w, u, bench) ((mode) == 1u || (w) < 330.0f * (u) || ((bench) && (w) < 610.0f * (u)))
#define BOO_HUD_FEATURE_ROW_HEIGHT(compact) ((compact) ? 18.0f : 22.0f)
#define BOO_HUD_NEURAL_TOP(rowHeight) (139.0f + 5.0f * (rowHeight))
#define BOO_HUD_NEURAL_ROWS_TOP(neuralTop) ((neuralTop) + 20.0f)
#define BOO_HUD_FOOTER_TOP(neuralRowsTop, rowHeight) ((neuralRowsTop) + 3.0f * (rowHeight))
#define BOO_HUD_SENSOR_TOP(footerTop) ((footerTop) + 20.0f)
#define BOO_HUD_CPU_TOP(sensorTop) ((sensorTop) + 18.0f)
#define BOO_HUD_WORKOUT_TOP(cpuTop) ((cpuTop) + 18.0f)
#define BOO_HUD_FLOCK_TOP(workoutTop) ((workoutTop) + 20.0f)
#define BOO_HUD_PHYSX_TOP(flockTop) ((flockTop) + 20.0f)
#define BOO_HUD_BENCHMARK_TOP(physxTop) ((physxTop) + 20.0f)

#define BOO_HUD_BENCH_STACK(w, u) ((w) < 426.0f * (u))
#define BOO_HUD_BENCH_X(w, u) BOO_HUD_MAX(0.0f, (w) - 278.0f * (u))
#define BOO_HUD_BENCH_Y(stack, top, height, u) ((stack) ? ((top) + (height) + 6.0f * (u)) : (14.0f * (u)))
#define BOO_HUD_AB_SCALE(h, lbWidth) BOO_HUD_MIN(3.0f, BOO_HUD_MAX(1.0f, BOO_HUD_MIN(BOO_HUD_FLOOR((h) / 540.0f), BOO_HUD_FLOOR((lbWidth) / 480.0f))))
#define BOO_HUD_AB_BANNER_WIDTH(lbWidth, u) BOO_HUD_MIN(262.0f * (u), ((lbWidth) - 40.0f * (u)) * 0.5f)
#define BOO_HUD_AB_LEFT_COORD(lbOrigin, u) ((lbOrigin) + 14.0f * (u))

#endif