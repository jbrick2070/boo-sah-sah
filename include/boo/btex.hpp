#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace boo {

struct BtexSubresource {
    uint64_t byteOffset = 0;
    uint64_t byteSize = 0;
    uint32_t rowBytes = 0;
    uint32_t numRows = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Btex {
    uint32_t dxgiFormat = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t arraySize = 0;
    uint32_t mipCount = 0;
    std::vector<BtexSubresource> subresources; // size == arraySize * mipCount (slice-major, matching D3D12)
    std::vector<uint8_t> payload;
    bool proceduralFallback = false;
};

bool loadBtex(const std::filesystem::path& path, Btex& output, std::string* error = nullptr);

// Deterministic neutral fallback textures for fault-injection and missing-file resilience.
Btex buildFallbackAlbedoArray(uint32_t arraySize = 48);
Btex buildFallbackNormalArray(uint32_t arraySize = 48);
Btex buildFallbackOrmtArray(uint32_t arraySize = 48);
Btex buildFallbackEnvIblArray(uint32_t arraySize = 3);

} // namespace boo
