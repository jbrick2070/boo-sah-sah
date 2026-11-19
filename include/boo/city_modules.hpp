#pragma once

#include "boo/bmesh.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace boo {
struct CityModulePlacement {
    uint32_t kit = 0, part = 0, seed = 0;
    std::array<uint32_t, 3> modules{}; // Near, middle and distant meshes.
    std::array<float, 12> rows{}; // Module to authored kit, row-major affine transform.
    std::array<float, 3> center{}; // Bounding sphere in authored kit coordinates.
    float radius = 0.0f;
    std::array<int32_t, 2> chunk{};
};

struct CityModuleLayout {
    std::vector<Bmesh> modules;
    std::vector<CityModulePlacement> placements;
};

// Convert the two procedural district partitions into placements at their original
// block coordinates. Retain authored structures, hero kits and unrecognized geometry.
void appendCityModules(CityModuleLayout& layout, uint32_t kit, Bmesh& authored);
uint32_t selectCityModuleLod(float distanceWorld, float worldUnitsPerMeter, uint32_t seed) noexcept;
float cityDetailScale(float distanceWorld, float worldUnitsPerMeter, uint32_t seed, uint32_t detailTier) noexcept;
} // namespace boo
