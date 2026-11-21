#pragma once

// The terrace wind (docs/physx_suite_plan.md, step 3): one closed-form breeze that everything moved by
// air answers to, so a gust reaches the orchids, the PhysX smoke, and later the cloth and the water,
// together. It is the breeze the film already had in its orchid sway, lifted out unchanged: the film's
// pixels do not move.
//
// Pure functions of position and film time: seek-safe, identical every run, usable by the bake tool.

#include <array>
#include <cmath>

namespace boo {

// The orchid sway's breeze term, exactly as the film computed it (do not reorder the arithmetic:
// the film is checked bit for bit). `phase` offsets one plant from another.
inline float terraceBreeze(float time, float phase) noexcept {
    return std::sin(time * .78f + phase) + .34f * std::sin(time * 1.37f + .8f * phase);
}

// The breeze sets out from the north-east over the Xinyi basin, as the sway's lean implies.
inline constexpr std::array<float, 3> kWindHeading{-0.62f, 0.0f, -0.78f};
inline constexpr float kWindMetresPerSecond = 2.2f; // a light breeze at roof height

// Wind velocity at a point, metres a second. The same breeze as the orchids, travelling across the city
// (the phase grows with distance along the heading), stronger with height, and never reversing: the
// breeze term swings from -1.34 to 1.34 and is mapped onto 0.2 .. 1.0 of the full speed.
inline std::array<float, 3> windAt(const float position[3], float time) noexcept {
    const float along = position[0] * kWindHeading[0] + position[2] * kWindHeading[2];
    const float gust = terraceBreeze(time, -along / (kWindMetresPerSecond * 6.0f));
    const float strength = 0.6f + 0.4f * (gust / 1.34f);
    const float height = std::fmax(position[1], 0.0f);
    const float profile = std::fmin(1.6f, 0.55f + height / 40.0f); // slow near the ground, faster aloft
    const float speed = kWindMetresPerSecond * strength * profile;
    return {kWindHeading[0] * speed, 0.0f, kWindHeading[2] * speed};
}

} // namespace boo
