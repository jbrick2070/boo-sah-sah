#pragma once

// The dawn bathroom's countertop, as the Explore marbles feel it: the honed Calacatta counter with the
// superellipse porcelain vessel basin standing on it (world metres, Y up). Shared by the built-in
// marble solver (src/app/main.cpp) and the PhysX marbles' height field (src/physics/physx_marbles.cpp).

#include <algorithm>
#include <cmath>

namespace boo {

// 0 at the basin's centre, 1 at its rim.
inline float dawnBasinU(float px, float pz) noexcept {
    const float ax = std::abs(px + 0.180f) / 0.252f;
    const float az = std::abs(pz - 6.245f) / 0.182f;
    return std::pow(std::pow(ax, 2.38f) + std::pow(az, 2.38f), 1.0f / 2.38f);
}

inline float dawnCountertopHeight(float px, float pz) noexcept {
    const float u = dawnBasinU(px, pz);
    if (u <= 0.135f) {
        return 3.575f;
    }
    if (u <= 0.160f) {
        const float s = (u - 0.135f) / 0.025f;
        return 3.575f * (1.0f - s) + 3.569f * s;
    }
    if (u <= 0.985f) {
        const float t = std::clamp((u - 0.160f) / 0.825f, 0.0f, 1.0f);
        return 3.569f + 0.114f * std::pow(t, 2.32f);
    }
    if (u <= 1.065f) {
        const float s = std::clamp((u - 0.985f) / 0.080f, 0.0f, 1.0f);
        const float sm = s * s * (3.0f - 2.0f * s);
        return 3.683f * (1.0f - sm) + 3.550f * sm;
    }
    return 3.550f;
}

} // namespace boo
