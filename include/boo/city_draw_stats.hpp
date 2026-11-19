#pragma once

#include <array>
#include <cstdint>

namespace boo {
struct CityDrawStats {
    uint32_t activeInstances = 0;
    uint32_t cityPlacements = 0;
    uint32_t visibleCityPlacements = 0;
    uint32_t visibleCityInstances = 0;
    uint32_t cityDrawBatches = 0;
    uint32_t rasterBatches = 0;
    uint32_t rayInstances = 0;
    uint32_t templateBlases = 0;
    uint64_t blasBytes = 0;
    uint64_t tlasBytes = 0;
    std::array<uint32_t, 3> lodPlacements{};
};
}
