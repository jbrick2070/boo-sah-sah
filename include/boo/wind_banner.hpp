#pragma once

// The silk banner of "Last Light" (docs/shot_spec_last_light.md): a 1 m by 6 m vermilion silk banner on
// a bronze mast at the west edge of the Taipei terrace, pinned along its top to a yard square to the
// terrace wind. The bake tool (`boo_physx_bake banner`) lets PhysX's GPU cloth solver stream it in the
// shared terrace wind (include/boo/wind_field.hpp) over the opening's 45 seconds; the renderer plays it
// back through the deforming-mesh path. Light silk pinned along one edge swings and streams by metres,
// where the canopy, pinned on all sides, can only belly. Shared by the bake tool and the renderer.

#include <array>
#include <cstdint>

namespace boo {

inline constexpr uint32_t kBannerMast = 190, kBannerCloth = 191; // kit 2 parts; 190..199 were unused
inline constexpr unsigned kBannerClothMaterial = 33;            // the fabric slot; shaded as silk by part
inline constexpr float kBannerLength = 6.0f, kBannerWidth = 1.0f;
// Simulated 81 x 13; stored every other node, 41 x 7, as the canopy is.
inline constexpr int kBannerSimU = 81, kBannerSimV = 13;
inline constexpr int kBannerGridU = 41, kBannerGridV = 7, kBannerGridNodes = kBannerGridU * kBannerGridV;
inline constexpr float kBannerOffsetScale = 1.0f / 2000.0f;     // int16 half millimetres: +-16.4 m
inline constexpr const char* kBannerWindTrack = "banner_wind";  // the wind as it is, 30 keys a second
inline constexpr const char* kBannerCalmTrack = "banner_calm";  // Reduced Motion: the wind at 45 %, 15 keys a second
inline constexpr float kBannerWindRate = 30.0f, kBannerCalmRate = 15.0f, kBannerCalmWind = 0.45f;
inline constexpr float kBannerFirst = 0.0f, kBannerLast = 45.5f;

// The yard's centre, the direction across the banner (square to the wind heading, horizontal) and the
// mast. World metres, Y up.
inline constexpr std::array<float, 3> kBannerTop{-6.82f, 10.30f, 4.13f};
inline constexpr std::array<float, 3> kBannerAcross{0.78f, 0.0f, -0.62f};
inline constexpr std::array<float, 3> kBannerMastBase{-6.60f, 2.63f, 4.40f};
inline constexpr float kBannerMastTop = 10.45f;

// The banner hanging still: u along its length (0 at the yard), v across (0..1).
inline std::array<float, 3> bannerRestPoint(float u, float v) noexcept {
    return {kBannerTop[0] + (v - 0.5f) * kBannerWidth * kBannerAcross[0],
            kBannerTop[1] - u * kBannerLength,
            kBannerTop[2] + (v - 0.5f) * kBannerWidth * kBannerAcross[2]};
}

// Which track plays: the calm one under Reduced Motion, since scaling the offsets of a sheet that has
// swung through tens of degrees would shrink the cloth.
inline const char* bannerTrackFor(float motion) noexcept { return motion >= 0.75f ? kBannerWindTrack : kBannerCalmTrack; }

} // namespace boo
