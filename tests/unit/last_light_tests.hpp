#pragma once

// "Last Light", the full film's opening (docs/shot_spec_last_light.md): its camera hands over exactly to
// the film's own, its light is a pure function of film time, its kettle is the honest size and leans with
// the wind, and its banner's mesh is laid out on the cloth grid the bake uses.

#include "boo/city_modules.hpp"
#include "boo/last_light.hpp"
#include "boo/m1_scene.hpp"
#include "boo/wind_banner.hpp"
#include "boo/wind_field.hpp"

#include <cmath>

namespace {

bool testLastLight() {
    using namespace boo;
    constexpr int32_t rate = 48000;
    auto at = [&](double t, float motion = 1.f) {
        return evaluateShowCamera(ShowVisualMode::FullFilm360s, int64_t(t * rate), rate, 0.5f, false, motion);
    };
    auto gap = [](const M1CameraState& a, const M1CameraState& b) {
        float d = 0.f;
        for (int i = 0; i < 3; ++i) d = std::max({d, std::fabs(a.eye[i] - b.eye[i]), std::fabs(a.target[i] - b.target[i])});
        return std::max(d, std::fabs(a.fovYRadians - b.fovYRadians));
    };
    auto travel = [](const M1CameraState& a, const M1CameraState& b) {
        return std::sqrt((a.eye[0] - b.eye[0]) * (a.eye[0] - b.eye[0]) + (a.eye[1] - b.eye[1]) * (a.eye[1] - b.eye[1]) + (a.eye[2] - b.eye[2]) * (a.eye[2] - b.eye[2]));
    };
    auto turn = [](const M1CameraState& a, const M1CameraState& b) {
        float da[3], db[3], la = 0.f, lb = 0.f, dot = 0.f;
        for (int i = 0; i < 3; ++i) { da[i] = a.target[i] - a.eye[i]; db[i] = b.target[i] - b.eye[i]; la += da[i] * da[i]; lb += db[i] * db[i]; }
        for (int i = 0; i < 3; ++i) dot += da[i] * db[i];
        return std::acos(std::clamp(dot / std::sqrt(la * lb), -1.f, 1.f));
    };
    // The camera: continuous through every keyframe and the hand-over (no faster than 6 m/s or 1.6 rad/s,
    // checked every 0.05 s), identical on repeated calls.
    for (double t = 0.0; t < 44.9; t += 0.05) {
        BOO_CHECK(travel(at(t), at(t + 0.05)) < 0.30f);
        BOO_CHECK(turn(at(t), at(t + 0.05)) < 0.08f);
        BOO_CHECK(std::fabs(at(t).fovYRadians - at(t + 0.05).fovYRadians) < 0.02f);
    }
    BOO_CHECK(gap(at(kLastLightRejoin - 0.001), at(kLastLightRejoin)) < 0.01f);
    BOO_CHECK(gap(at(9.0), at(9.0)) == 0.f);
    // It starts low over the pool, looking west-south-west into the sunset.
    const M1CameraState first = at(0.0);
    BOO_CHECK(first.eye[1] < 3.2f && first.target[0] < first.eye[0] && first.target[2] < first.eye[2]);
    // Under Reduced Motion it holds the wide view instead of skimming the pool.
    BOO_CHECK(at(2.0, 0.f).eye[1] > first.eye[1] - 0.5f && gap(at(2.0, 0.f), at(2.0)) > 1.f);

    // The light: windows still dark at the start, sunset from 16 s, night from 31 s, never rising.
    BOO_CHECK(std::fabs(lastLightDaylight(0.0) - 0.70f) < 1e-6f && lastLightDaylight(31.0) == 0.f && lastLightDaylight(44.0) == 0.f);
    for (double t = 0.0; t < 45.0; t += 0.25) BOO_CHECK(lastLightDaylight(t + 0.25) <= lastLightDaylight(t) + 1e-6f);

    // The kettle: the honest size, leaning downwind along the terrace breeze.
    static_assert(kLastLightKites == 32u);
    constexpr float along = kLastLightKettleLean[0] * kWindHeading[0] + kLastLightKettleLean[1] * kWindHeading[2];
    static_assert(along > 0.18f && along < 0.2f);

    // The banner: its silk is laid out on the bake's grid (texture coordinates are grid coordinates), and
    // its top row hangs from the yard.
    const Bmesh cloth = buildWindBanner(kBannerCloth);
    BOO_CHECK(cloth.submeshes.size() == 1u && cloth.submeshes[0].material == kBannerClothMaterial && cloth.submeshes[0].part == kBannerCloth);
    BOO_CHECK(cloth.vertices.size() == size_t(2 * kBannerSimU * kBannerSimV));
    float worst = 0.f;
    for (const auto& v : cloth.vertices) {
        const auto rest = bannerRestPoint(v.uvThicknessRoughness[0], v.uvThicknessRoughness[1]);
        for (int axis = 0; axis < 3; ++axis) worst = std::max(worst, std::fabs(v.positionMaterial[axis] - rest[size_t(axis)]));
    }
    BOO_CHECK(worst < 0.001f);
    const auto top = bannerRestPoint(0.f, 0.5f), bottom = bannerRestPoint(1.f, 0.5f);
    BOO_CHECK(std::fabs(top[1] - kBannerTop[1]) < 1e-5f && std::fabs(top[1] - bottom[1] - kBannerLength) < 1e-4f && bottom[1] > 2.63f + 1.0f);
    BOO_CHECK(std::string(bannerTrackFor(1.f)) == kBannerWindTrack && std::string(bannerTrackFor(0.2f)) == kBannerCalmTrack);
    const Bmesh mast = buildWindBanner(kBannerMast);
    BOO_CHECK(!mast.vertices.empty() && mast.bounds[4] >= kBannerMastTop);
    return true;
}

} // namespace
