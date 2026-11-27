#pragma once

#include "boo/api.hpp"
#include "boo/city_modules.hpp"

#include <bit>
#include <cmath>

namespace {

// Index compaction may renumber a vertex. Hash the actual triangle stream so a
// moved, reordered or missing hero face still fails on either generation of kit.
std::pair<uint64_t, size_t> cityHeroGeometry(const boo::Bmesh& mesh, uint32_t kit) {
    uint64_t hash = 14695981039346656037ull;
    size_t count = 0;
    for (const auto& sub : mesh.submeshes) {
        if (kit == 1 ? sub.part < 100 : sub.part != 0) continue;
        count += sub.indexCount;
        for (uint32_t i = 0; i < sub.indexCount; ++i) {
            const auto& vertex = mesh.vertices[mesh.indices[sub.firstIndex + i]];
            for (uint32_t word : std::bit_cast<std::array<uint32_t, 16>>(vertex))
                hash = (hash ^ word) * 1099511628211ull;
        }
    }
    return {hash, count};
}

bool testCityModuleLodTransitions() {
    for (float units : {1.f, .08f}) {
        for (uint32_t seed : {0u, 93u, 255u}) {
            for (uint32_t tier : {1u, 2u}) {
                const float phase = .94f + .12f * float(seed) / 255.f;
                const float end = (tier == 1 ? 120.f : 350.f) * units * phase;
                BOO_CHECK(boo::cityDetailScale(end * .80f, units, seed, tier) == 1.f);
                BOO_CHECK(std::abs(boo::cityDetailScale(end * .91f, units, seed, tier) - .5f) < .001f);
                BOO_CHECK(boo::cityDetailScale(end, units, seed, tier) == 0.f);
                BOO_CHECK(boo::cityDetailScale(end, units, seed, 0) == 1.f);
                BOO_CHECK(boo::selectCityModuleLod(end * .999f, units, seed) == tier - 1);
                BOO_CHECK(boo::selectCityModuleLod(end * 1.001f, units, seed) == tier);
            }
        }
    }
    return true;
}

bool testCityModulesPreserveAuthoredKits() {
    for (uint32_t kit : {1u, 2u}) {
        const auto path = boo::resolveResourcePath(kit == 1 ? "assets/meshes/shenzhen_evtol.bmesh"
                                                            : "assets/meshes/taipei_101_hero.bmesh");
        boo::Bmesh mesh;
        BOO_CHECK(boo::loadBmesh(path, mesh));
        const uint32_t crc = mesh.sourcePayloadCrc;
        const size_t originalIndices = mesh.indices.size();
        const size_t originalVertices = mesh.vertices.size();
        const auto hero = cityHeroGeometry(mesh, kit);
        BOO_CHECK(hero.second > 0);

        boo::CityModuleLayout first;
        mesh.proceduralFallback = true;
        boo::appendCityModules(first, kit, mesh);
        mesh.proceduralFallback = false;
        mesh.sourcePayloadCrc = 0;
        boo::appendCityModules(first, kit, mesh);
        mesh.sourcePayloadCrc = crc;
        BOO_CHECK(first.placements.empty() && first.modules.empty());
        BOO_CHECK(mesh.indices.size() == originalIndices && mesh.vertices.size() == originalVertices);

        const bool legacy = crc == (kit == 1 ? 0x751738d4u : 0x0019c664u);
        const bool extended = crc == (kit == 1 ? 0x300082cdu : 0x252bd557u);
        boo::appendCityModules(first, kit, mesh);
        BOO_CHECK(cityHeroGeometry(mesh, kit) == hero);
        if (!legacy && !extended) {
            BOO_CHECK(first.placements.empty() && first.modules.empty());
            continue;
        }
        const size_t expected = legacy ? (kit == 1 ? 676u : 625u) : (kit == 1 ? 467u : 449u);
        BOO_CHECK(first.placements.size() == expected);
        if (legacy) {
            BOO_CHECK(mesh.indices.size() == originalIndices && mesh.vertices.size() == originalVertices);
        } else {
            BOO_CHECK(mesh.indices.size() < originalIndices && mesh.vertices.size() < originalVertices);
        }

        // Reopen sequentially to keep just one decoded source kit in memory.
        mesh = {};
        BOO_CHECK(boo::loadBmesh(path, mesh));
        boo::CityModuleLayout replay;
        boo::appendCityModules(replay, kit, mesh);
        BOO_CHECK(first.modules.size() == replay.modules.size());
        BOO_CHECK(first.placements.size() == replay.placements.size());
        for (size_t i = 0; i < first.placements.size(); ++i) {
            const auto& p = first.placements[i];
            const auto& q = replay.placements[i];
            BOO_CHECK(p.kit == q.kit && p.part == q.part && p.seed == q.seed && p.modules == q.modules);
            BOO_CHECK(p.rows == q.rows && p.center == q.center && p.chunk == q.chunk && p.radius == q.radius);
            for (unsigned int id : p.modules) {
                BOO_CHECK(id < first.modules.size());
                const auto& module = first.modules[id];
                BOO_CHECK(!module.vertices.empty() && !module.indices.empty());
                // The convex local box encloses every vertex, including geometry
                // shrinking toward its surface center throughout either LOD fade.
                for (int corner = 0; corner < 8; ++corner) {
                    float distance2 = 0;
                    for (int axis = 0; axis < 3; ++axis) {
                        float offset = p.rows[4 * axis + 3] - p.center[axis];
                        for (int j = 0; j < 3; ++j)
                            offset += p.rows[4 * axis + j] * module.bounds[j + (corner & (1 << j) ? 3 : 0)];
                        distance2 += offset * offset;
                    }
                    BOO_CHECK(distance2 <= p.radius * p.radius * 1.00001f);
                }
            }
        }
    }
    return true;
}
} // namespace
