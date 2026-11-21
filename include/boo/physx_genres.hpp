#pragma once

// The PhysX suite's genres, in dial order. Settings, the dial, the dashboard row and diagnostics
// all read this one table, so a new genre is added here and nowhere else in that plumbing.
// See docs/physx_suite_plan.md for the whole suite and its build order.

#include <array>
#include <cstdint>
#include <string_view>

namespace boo {

enum class PhysxGenre : uint32_t {
    Off = 0,
    Smoke,  // NVIDIA Flow: vapour rising through the kites' thermals
    Rigid,  // PhysX GPU rigid bodies: enamel marbles poured over the city's buildings
    Fluid,  // PhysX GPU position-based fluid: a cloudburst pouring off the buildings
    Soft,   // PhysX GPU deformable volumes: soft microbes squashing onto the buildings
    Count
};

struct PhysxGenreInfo {
    std::string_view key;     // settings and command-line value
    std::string_view label;   // log and diagnostics
    bool usesPhysxSdk;        // needs the PhysX runtime (Flow is a sibling library and does not)
    bool usesGpu;
};

inline constexpr std::array<PhysxGenreInfo, static_cast<size_t>(PhysxGenre::Count)> kPhysxGenres{{
    {"off", "off", false, false},
    {"smoke", "smoke (NVIDIA Flow)", false, true},
    {"rigid", "rigid bodies (PhysX)", true, true},
    {"fluid", "fluids (PhysX PBF)", true, true},
    {"soft", "soft bodies (PhysX FEM)", true, true},
}};

inline constexpr uint32_t kPhysxGenreCount = static_cast<uint32_t>(PhysxGenre::Count);

inline constexpr const PhysxGenreInfo& physxGenreInfo(uint32_t genre) noexcept {
    return kPhysxGenres[genre < kPhysxGenreCount ? genre : 0u];
}

inline constexpr uint32_t nextPhysxGenre(uint32_t genre) noexcept {
    return (genre + 1u) % kPhysxGenreCount;
}

} // namespace boo
