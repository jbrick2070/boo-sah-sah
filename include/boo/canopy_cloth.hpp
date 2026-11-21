#pragma once

// The woven canopy ribbons as PhysX cloth (docs/physx_suite_plan.md, step 6).
//
// The four neural lamellae of the enamel cathedral (parts 216..219 of tools/gen_trilogy_meshes.py,
// kit 4) arch 9.2 m overhead between pairs of bronze tendons. The bake tool (tools/physx_bake,
// `boo_physx_bake cloth`) hangs a strip of fabric a little wider than the gap between each pair of
// tendons, pinned along both tendons and at both feet, and lets PhysX's GPU cloth solver (a
// deformable surface) settle it under gravity and billow it in the shared terrace wind. It records
// how far each node of a coarse grid has moved from the woven rest shape, for the seconds of the film
// in which the ribbons are on screen. The renderer moves the lamella vertices by those offsets
// (the deforming-mesh path, step 5); the tendons stay where they are.
//
// Everything here is shared by the bake tool and the renderer, so both agree on the grid.

#include <algorithm>
#include <array>
#include <cmath>

namespace boo {

inline constexpr int kCanopyRibbons = 4;
inline constexpr int kCanopyFirstPart = 216;      // part of ribbon 0; the others follow
inline constexpr unsigned kCanopyMaterial = 21;   // the lamella fabric (the tendons are material 38)
// The simulated grid matches the generator's lamella sampling (closed_blade with nu=40, nv=12)...
inline constexpr int kCanopySimU = 41, kCanopySimV = 13;
// ...and the stored grid keeps every other node, which is plenty for a billow a metre or more long.
inline constexpr int kCanopyGridU = 21, kCanopyGridV = 7;
inline constexpr int kCanopyGridNodes = kCanopyGridU * kCanopyGridV;
inline constexpr int kCanopyTrackNodes = kCanopyRibbons * kCanopyGridNodes;
inline constexpr float kCanopyKeyRate = 10.0f;    // keys a second; played back with cubic interpolation
inline constexpr float kCanopyOffsetScale = 1.0f / 2000.0f; // stored offsets are half millimetres (int16)

// The film seconds that are baked: the ribbons are drawn in the Neural Velvet act and the sanctuary
// act before the finale (isPartActiveInShow in src/render/trilogy_renderer.cpp).
struct CanopySpan { float first, last; const char* track; };
inline constexpr std::array<CanopySpan, 2> kCanopySpans{{{100.0f, 150.0f, "canopy_velvet"}, {275.0f, 338.5f, "canopy_sanctuary"}}};

// The lamella mid-surface, exactly as the generator's `ribbon(u, v)` (u along, v across in -1..1),
// converted to engine space (Blender Z-up x,y,z -> engine Y-up x,z,y).
inline std::array<float, 3> canopyRibbonPoint(int ribbon, float u, float v) noexcept {
    constexpr float kPi = 3.14159265358979f;
    const float k = float(ribbon);
    const float x = (u - 0.5f) * 9.2f;
    const float y = 4.0f + k * 1.55f + v * 0.32f * std::cos(u * kPi);
    const float z = 3.0f + 5.4f * std::sin(kPi * u) + 0.22f * std::sin(u * 2.0f * kPi + k) + v * 0.34f * std::sin(u * kPi);
    return {x, z, y};
}

// Which film second of the bake a renderer time reads. The Tensor solo loop replays the Neural Velvet
// act's first fifty seconds.
inline float canopyBakeTime(float time, bool tensorSolo) noexcept {
    if (tensorSolo) return kCanopySpans[0].first + std::fmod(std::max(time, 0.0f), kCanopySpans[0].last - kCanopySpans[0].first);
    return time;
}

} // namespace boo
