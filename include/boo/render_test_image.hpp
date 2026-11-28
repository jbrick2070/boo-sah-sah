#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace boo {
// Worker image payload is tightly packed top-down RGBA8, with no row padding.
inline constexpr std::size_t kRenderTestMaxImageBytes = std::size_t{1920} * 1080 * 4;
inline bool validRenderTestImageSize(uint32_t width, uint32_t height, std::size_t bytes) noexcept {
    const bool supported = (width == 1920 && height == 1080) || (width == 1280 && height == 720);
    return supported && bytes == static_cast<std::size_t>(width) * height * 4 && bytes <= kRenderTestMaxImageBytes;
}
struct RenderTestImage {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
    bool valid() const noexcept { return validRenderTestImageSize(width, height, rgba.size()); }
};
} // namespace boo
