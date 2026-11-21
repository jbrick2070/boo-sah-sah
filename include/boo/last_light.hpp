#pragma once

// "Last Light", the full film's opening (docs/shot_spec_last_light.md, revision 2): film 0:00 to
// 0:45 at dusk on the Taipei terrace. The sky is first seen upside down in the reflecting pool; a gust
// streams a silk banner; the camera turns upwind to the kettle of black kites beside Taipei 101 while
// the sun sets and the city switches on; then it descends to the orchid as it blooms and hands over to
// the film's existing camera through its 1.8 s blend before the 45 s cut.
//
// Everything here is a pure function of film time, so the opening is identical every run and seeks
// exactly. Header-only, so the unit tests can call it.

#include <algorithm>
#include <array>
#include <cmath>

namespace boo {

inline constexpr double kLastLightEnd = 45.0;       // the opening's extent: the whole first act
inline constexpr double kLastLightRejoin = 33.0;    // from here the film's own opening camera runs unchanged

// The film's own kettle of black kites in the opening. Taiwan's 2025 synchronised survey counted 1,033
// black kites nationwide, roosting mostly outside central Taipei, so a modest pre-roost kettle is the
// honest number; the framing gives it its scale.
inline constexpr unsigned kLastLightKites = 32;
// Its column leans downwind along the terrace breeze (kWindHeading in include/boo/wind_field.hpp),
// by as much per metre of climb as the workout's kettles lean.
inline constexpr std::array<float, 2> kLastLightKettleLean{-0.62f * 0.193f, -0.78f * 0.193f};

// How much daylight the terrace has: a low dusk sun until 16 s, then sunset; dark (0) from 31 s.
// At 0.70 the city's windows are still dark (the shader's night factor is smoothstep(0.68, 0.08,
// daylight)), so the lights are seen coming on.
inline float lastLightDaylight(double t) noexcept {
    if (t >= 31.0) return 0.0f;
    const double u = std::clamp((t - 16.0) / 15.0, 0.0, 1.0);
    const double s = u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
    return static_cast<float>(0.70 * (1.0 - s));
}

// The camera's keyframes, in film world metres (Y up, +X east, +Z north). The final keyframe is the
// film's own opening camera at kLastLightRejoin, blended in from the last key.
struct LastLightKey { double t; std::array<float, 3> eye, target; float fov, focus; };
inline constexpr std::array<LastLightKey, 6> kLastLightKeys{{
    // The sky in the water: a low camera skimming the pool towards the west-south-west sunset. Water
    // reflects only a few per cent of the sky seen from above, but nearly half of it at a grazing angle,
    // so the pool is a sheet of gold mirroring the sky and the skyline, the orchid and the banner
    // silhouetted against it.
    {0.0,  {5.40f, 3.05f, 5.60f},  {-5.60f, 2.80f, 1.04f}, 0.95f, 0.0f},
    {3.5,  {3.60f, 3.15f, 5.00f},  {-7.40f, 3.60f, 1.20f}, 0.95f, 0.0f},
    // The banner: side-on, held nearly still from 6 s through its largest baked swing (the silk sinks about
    // 2.5 m at the tip as the wind eases to its 7 s lull, then lifts in the next gust).
    {6.0,  {0.40f, 3.90f, -1.50f}, {-7.60f, 7.90f, 3.20f}, 1.00f, 8.6f},
    {12.5, {0.55f, 3.95f, -1.55f}, {-7.60f, 7.90f, 3.20f}, 1.00f, 8.6f},
    // Upwind: the kettle and Taipei 101 together, focus racked out to the skyline; held as the light goes.
    // From here the orchid sits left of frame, the kettle centre and Taipei 101 right, about 33 degrees
    // apart, so the glass flower never covers the tower.
    {19.0, {2.50f, 3.30f, -1.80f}, {6.00f, 26.00f, 30.00f}, 1.02f, 48.0f},
    {27.0, {2.70f, 3.35f, -1.75f}, {6.20f, 25.60f, 30.00f}, 1.02f, 48.0f},
}};

struct LastLightCamera { std::array<float, 3> eye, target; float fov, focusDistance, focusWeight; };

namespace last_light_detail {
inline double smoother(double u) noexcept { u = std::clamp(u, 0.0, 1.0); return u * u * u * (u * (u * 6.0 - 15.0) + 10.0); }
// Interpolates between two views by eye position, look direction (spherically) and look distance, so
// a pan between a near and a far target turns evenly.
inline LastLightCamera blend(const LastLightCamera& a, const LastLightCamera& b, double w) noexcept {
    LastLightCamera out{};
    std::array<float, 3> da{}, db{};
    float la = 0.0f, lb = 0.0f;
    for (int i = 0; i < 3; ++i) { da[i] = a.target[i] - a.eye[i]; db[i] = b.target[i] - b.eye[i]; la += da[i] * da[i]; lb += db[i] * db[i]; }
    la = std::sqrt(std::max(la, 1e-12f)); lb = std::sqrt(std::max(lb, 1e-12f));
    for (int i = 0; i < 3; ++i) { da[i] /= la; db[i] /= lb; }
    const float cosine = std::clamp(da[0] * db[0] + da[1] * db[1] + da[2] * db[2], -1.0f, 1.0f);
    const float angle = std::acos(cosine);
    std::array<float, 3> dir{};
    if (angle < 1e-4f) {
        for (int i = 0; i < 3; ++i) dir[i] = da[i] + (db[i] - da[i]) * float(w);
    } else {
        const float sa = std::sin((1.0f - float(w)) * angle) / std::sin(angle), sb = std::sin(float(w) * angle) / std::sin(angle);
        for (int i = 0; i < 3; ++i) dir[i] = da[i] * sa + db[i] * sb;
    }
    const float length = la + (lb - la) * float(w);
    for (int i = 0; i < 3; ++i) {
        out.eye[i] = a.eye[i] + (b.eye[i] - a.eye[i]) * float(w);
        out.target[i] = out.eye[i] + dir[i] * length;
    }
    out.fov = a.fov + (b.fov - a.fov) * float(w);
    out.focusDistance = a.focusDistance + (b.focusDistance - a.focusDistance) * float(w);
    out.focusWeight = a.focusWeight + (b.focusWeight - a.focusWeight) * float(w);
    return out;
}
inline LastLightCamera keyCamera(const LastLightKey& k) noexcept {
    return {k.eye, k.target, k.fov, k.focus, k.focus > 0.0f ? 1.0f : 0.0f};
}
} // namespace last_light_detail

// The opening camera at film time t (0 <= t < kLastLightRejoin). `film` is the film's own opening
// camera at the same time, which the shot hands over to between the last key and kLastLightRejoin.
inline LastLightCamera lastLightCamera(double t, const LastLightCamera& film) noexcept {
    using namespace last_light_detail;
    const auto& keys = kLastLightKeys;
    if (t >= kLastLightRejoin) return film;
    if (t <= keys.front().t) return keyCamera(keys.front());
    for (size_t i = 0; i + 1 < keys.size(); ++i)
        if (t < keys[i + 1].t)
            return blend(keyCamera(keys[i]), keyCamera(keys[i + 1]), smoother((t - keys[i].t) / (keys[i + 1].t - keys[i].t)));
    return blend(keyCamera(keys.back()), film, smoother((t - keys.back().t) / (kLastLightRejoin - keys.back().t)));
}

} // namespace boo
