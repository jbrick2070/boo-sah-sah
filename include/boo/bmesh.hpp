#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace boo {
struct BmeshVertex {
    float positionMaterial[4]{};
    float normalPart[4]{};
    float uvThicknessRoughness[4]{};
    float tangentSign[4]{};
};
static_assert(sizeof(BmeshVertex) == 64);
struct BmeshSubmesh {
    uint32_t firstIndex=0, indexCount=0, material=0, part=0, firstVertex=0, vertexCount=0;
};
struct Bmesh {
    std::vector<BmeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<BmeshSubmesh> submeshes;
    std::array<float, 6> bounds{};
    bool proceduralFallback=false;
    uint32_t sourcePayloadCrc = 0; // Populated after file integrity verification.
};
bool loadBmesh(const std::filesystem::path& path, Bmesh& output, std::string* error=nullptr);
// Original deterministic low-detail fallback authored in m1_scene.cpp.
Bmesh buildTrilogyFallbackKit(uint32_t kit);
}
