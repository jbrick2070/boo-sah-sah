#include "boo/btex.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace boo {
namespace {

uint32_t readU32(const uint8_t* p) noexcept {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t readU64(const uint8_t* p) noexcept {
    return uint64_t(readU32(p)) | (uint64_t(readU32(p + 4)) << 32);
}

uint32_t computeCrc32(const uint8_t* p, size_t size) noexcept {
    uint32_t c = ~0u;
    for (size_t i = 0; i < size; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) {
            c = (c >> 1) ^ ((0u - (c & 1u)) & 0xedb88320u);
        }
    }
    return ~c;
}

bool computeMipLayout(uint32_t dxgiFormat, uint32_t mw, uint32_t mh, uint32_t& outRowBytes, uint32_t& outNumRows) noexcept {
    // DXGI_FORMAT_BC5_UNORM = 83, DXGI_FORMAT_BC7_UNORM = 98, DXGI_FORMAT_BC7_UNORM_SRGB = 99
    if (dxgiFormat == 83u || dxgiFormat == 98u || dxgiFormat == 99u) {
        outRowBytes = std::max(1u, (mw + 3u) / 4u) * 16u;
        outNumRows = std::max(1u, (mh + 3u) / 4u);
        return true;
    }
    // DXGI_FORMAT_R8G8B8A8_UNORM = 28, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29
    if (dxgiFormat == 28u || dxgiFormat == 29u) {
        outRowBytes = mw * 4u;
        outNumRows = mh;
        return true;
    }
    // DXGI_FORMAT_R16G16B16A16_FLOAT = 10
    if (dxgiFormat == 10u) {
        outRowBytes = mw * 8u;
        outNumRows = mh;
        return true;
    }
    return false;
}

Btex makeUniformRgba8Fallback(uint32_t dxgiFormat, uint32_t arraySize, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Btex tex{};
    tex.dxgiFormat = dxgiFormat;
    tex.width = 4u;
    tex.height = 4u;
    tex.arraySize = std::max(1u, arraySize);
    tex.mipCount = 1u;
    tex.proceduralFallback = true;
    const uint64_t sliceBytes = 4u * 4u * 4u;
    tex.payload.resize(static_cast<size_t>(tex.arraySize * sliceBytes));
    for (uint32_t s = 0; s < tex.arraySize; ++s) {
        BtexSubresource sub{};
        sub.byteOffset = uint64_t(s) * sliceBytes;
        sub.byteSize = sliceBytes;
        sub.rowBytes = 16u;
        sub.numRows = 4u;
        sub.width = 4u;
        sub.height = 4u;
        tex.subresources.push_back(sub);
        uint8_t* dst = tex.payload.data() + sub.byteOffset;
        for (uint32_t px = 0; px < 16u; ++px) {
            dst[px * 4u + 0u] = r;
            dst[px * 4u + 1u] = g;
            dst[px * 4u + 2u] = b;
            dst[px * 4u + 3u] = a;
        }
    }
    return tex;
}

} // namespace

bool loadBtex(const std::filesystem::path& path, Btex& output, std::string* error) {
    const auto fail = [&](const char* reason) {
        if (error) *error = reason;
        return false;
    };
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return fail("Texture missing or unreadable");
    const auto fileSize = file.tellg();
    if (fileSize < 64 || fileSize > 256LL * 1024LL * 1024LL) return fail("Invalid texture file size");
    std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), fileSize)) return fail("Truncated texture file");

    const uint8_t* p = bytes.data();
    if (std::memcmp(p, "BTEX001\0", 8) != 0 || readU32(p + 8) != 1u || readU32(p + 12) != 64u) {
        return fail("Unsupported BTEX format header");
    }

    const uint32_t dxgiFormat = readU32(p + 16);
    const uint32_t width = readU32(p + 20);
    const uint32_t height = readU32(p + 24);
    const uint32_t arraySize = readU32(p + 28);
    const uint32_t mipCount = readU32(p + 32);
    const uint32_t expectedCrc = readU32(p + 36);
    const uint64_t payloadBytes = readU64(p + 40);

    if (!width || !height || width > 4096u || height > 4096u || !arraySize || arraySize > 128u || !mipCount || mipCount > 13u) {
        return fail("Invalid BTEX dimensions");
    }
    if (payloadBytes + 64ULL != bytes.size()) {
        return fail("BTEX payload size mismatch");
    }

    uint64_t cursor = 0;
    std::vector<BtexSubresource> subs;
    subs.reserve(static_cast<size_t>(arraySize) * mipCount);
    for (uint32_t s = 0; s < arraySize; ++s) {
        for (uint32_t m = 0; m < mipCount; ++m) {
            const uint32_t mw = std::max(1u, width >> m);
            const uint32_t mh = std::max(1u, height >> m);
            uint32_t rowBytes = 0, numRows = 0;
            if (!computeMipLayout(dxgiFormat, mw, mh, rowBytes, numRows)) {
                return fail("Unsupported BTEX DXGI format");
            }
            const uint64_t subBytes = uint64_t(rowBytes) * numRows;
            if (cursor + subBytes > payloadBytes) {
                return fail("BTEX subresource overflow");
            }
            subs.push_back(BtexSubresource{cursor, subBytes, rowBytes, numRows, mw, mh});
            cursor += subBytes;
        }
    }
    if (cursor != payloadBytes) {
        return fail("BTEX subresource coverage mismatch");
    }
    if (computeCrc32(p + 64, static_cast<size_t>(payloadBytes)) != expectedCrc) {
        return fail("BTEX CRC32 mismatch");
    }

    Btex result{};
    result.dxgiFormat = dxgiFormat;
    result.width = width;
    result.height = height;
    result.arraySize = arraySize;
    result.mipCount = mipCount;
    result.subresources = std::move(subs);
    result.payload.assign(bytes.begin() + 64, bytes.end());
    result.proceduralFallback = false;
    output = std::move(result);
    return true;
}

Btex buildFallbackAlbedoArray(uint32_t arraySize) {
    static constexpr float kLin[42][3] = {
        {0.080f, 0.092f, 0.110f}, {0.024f, 0.028f, 0.036f}, {0.180f, 0.168f, 0.150f}, {0.840f, 0.920f, 0.940f},
        {0.640f, 0.850f, 0.800f}, {0.045f, 0.150f, 0.180f}, {0.080f, 0.760f, 0.580f}, {0.012f, 0.038f, 0.054f},
        {0.860f, 0.600f, 0.200f}, {0.520f, 0.065f, 0.210f}, {0.042f, 0.180f, 0.068f}, {0.840f, 0.860f, 0.890f},
        {0.220f, 0.260f, 0.300f}, {0.140f, 0.120f, 0.105f}, {0.060f, 0.100f, 0.150f}, {0.880f, 0.680f, 0.400f},
        {0.960f, 0.640f, 0.220f}, {0.480f, 0.540f, 0.600f}, {0.080f, 0.120f, 0.170f}, {0.022f, 0.038f, 0.046f},
        {0.880f, 0.860f, 0.800f}, {0.120f, 0.420f, 0.400f}, {0.140f, 0.580f, 0.420f}, {0.660f, 0.645f, 0.615f},
        {0.760f, 0.720f, 0.650f}, {0.320f, 0.480f, 0.430f}, {0.420f, 0.280f, 0.160f}, {0.705f, 0.696f, 0.680f},
        {0.840f, 0.860f, 0.820f}, {0.090f, 0.280f, 0.110f}, {0.180f, 0.300f, 0.160f}, {0.745f, 0.722f, 0.684f},
        {0.720f, 0.690f, 0.640f}, {0.655f, 0.612f, 0.545f}, {0.940f, 0.950f, 0.945f}, {0.680f, 0.210f, 0.032f},
        {0.080f, 0.260f, 0.090f}, {0.055f, 0.060f, 0.068f}, {0.140f, 0.115f, 0.090f}, {0.260f, 0.300f, 0.240f},
        {0.930f, 0.965f, 0.955f}, {0.620f, 0.580f, 0.520f},
    };
    auto toSrgb8 = [](float x) -> uint8_t {
        x = std::clamp(x, 0.0f, 1.0f);
        const float s = (x <= 0.0031308f) ? (x * 12.92f) : (1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f);
        return static_cast<uint8_t>(std::clamp(std::lround(s * 255.0f), 0L, 255L));
    };
    Btex tex = makeUniformRgba8Fallback(29u, arraySize, 200u, 200u, 200u, 255u);
    for (uint32_t s = 0; s < tex.arraySize; ++s) {
        const auto& c = kLin[std::min<uint32_t>(s, 41u)];
        const uint8_t rgba[4] = {toSrgb8(c[0]), toSrgb8(c[1]), toSrgb8(c[2]), 255u};
        uint8_t* dst = tex.payload.data() + tex.subresources[s].byteOffset;
        for (uint32_t px = 0; px < 16u; ++px) std::memcpy(dst + px * 4u, rgba, 4u);
    }
    return tex;
}

Btex buildFallbackNormalArray(uint32_t arraySize) {
    // DXGI_FORMAT_R8G8B8A8_UNORM = 28; flat tangent-space normal XY = (0.5, 0.5) -> (128, 128)
    return makeUniformRgba8Fallback(28u, arraySize, 128u, 128u, 255u, 255u);
}

Btex buildFallbackOrmtArray(uint32_t arraySize) {
    static constexpr uint8_t kRm[42][3] = {
        {90, 224, 0},  {80, 0, 80},   {150, 0, 0},   {10, 0, 245},
        {12, 0, 242},  {40, 64, 180}, {42, 0, 220},  {8, 0, 245},
        {48, 245, 0},  {28, 0, 235},  {96, 0, 190},  {44, 20, 180},
        {12, 220, 230},{64, 48, 160}, {40, 56, 180}, {60, 0, 230},
        {40, 0, 235},  {56, 140, 0},  {10, 0, 235},  {230, 0, 0},
        {26, 0, 210},  {60, 30, 230}, {22, 0, 235},  {42, 248, 0},
        {185, 0, 0},   {32, 0, 180},  {96, 0, 35},   {12, 0, 230},
        {48, 0, 148},  {80, 0, 180},  {110, 0, 120}, {48, 0, 76},
        {86, 0, 25},   {212, 0, 82},  {4, 252, 0},   {10, 0, 240},
        {86, 0, 214},  {56, 90, 218}, {66, 232, 0},  {150, 0, 56},
        {4, 0, 250},   {135, 0, 12},
    };
    Btex tex = makeUniformRgba8Fallback(28u, arraySize, 255u, 128u, 0u, 0u);
    for (uint32_t s = 0; s < tex.arraySize; ++s) {
        const auto& rm = kRm[std::min<uint32_t>(s, 41u)];
        const uint8_t rgba[4] = {245u, rm[0], rm[1], rm[2]};
        uint8_t* dst = tex.payload.data() + tex.subresources[s].byteOffset;
        for (uint32_t px = 0; px < 16u; ++px) std::memcpy(dst + px * 4u, rgba, 4u);
    }
    return tex;
}

Btex buildFallbackEnvIblArray(uint32_t arraySize) {
    // DXGI_FORMAT_R16G16B16A16_FLOAT = 10; 4x4xN subtle neutral sky fallback (FP16 0x3000 = 0.125)
    Btex tex{};
    tex.dxgiFormat = 10u;
    tex.width = 4u;
    tex.height = 4u;
    tex.arraySize = std::max(1u, arraySize);
    tex.mipCount = 1u;
    tex.proceduralFallback = true;
    const uint64_t sliceBytes = 4u * 4u * 8u;
    tex.payload.resize(static_cast<size_t>(tex.arraySize * sliceBytes), 0u);
    const uint16_t eighth[4] = {0x2E00u, 0x2F00u, 0x3000u, 0x3C00u};
    for (uint32_t s = 0; s < tex.arraySize; ++s) {
        tex.subresources.push_back(BtexSubresource{uint64_t(s) * sliceBytes, sliceBytes, 32u, 4u, 4u, 4u});
        uint8_t* dst = tex.payload.data() + uint64_t(s) * sliceBytes;
        for (uint32_t px = 0; px < 16u; ++px) {
            std::memcpy(dst + px * 8u, eighth, 8u);
        }
    }
    return tex;
}

} // namespace boo
