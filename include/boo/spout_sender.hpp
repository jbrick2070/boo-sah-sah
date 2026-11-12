#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace boo {

class D3D12Context;

// Binary-exact 280-byte Spout2 SharedTextureInfo layout published in named Win32 shared memory.
struct SpoutSharedTextureInfo {
    uint32_t shareHandle = 0;       // 32-bit legacy DXGI share handle (HandleToULong)
    uint32_t width = 0;             // Shared texture width in pixels
    uint32_t height = 0;            // Shared texture height in pixels
    uint32_t format = 0;            // DXGI_FORMAT (87 = DXGI_FORMAT_B8G8R8A8_UNORM)
    uint32_t usage = 0;             // Adapter index / flags (0)
    uint8_t  description[256]{};    // UTF-16LE DXGI adapter description (wchar_t[128])
    uint32_t partnerId = 0;         // Spout2 partner ID (0)
};
static_assert(sizeof(SpoutSharedTextureInfo) == 280, "Spout2 SharedTextureInfo must be exactly 280 bytes");

// Reads an active Spout2 sender's shared memory record & verifies presence in SpoutSenderNames.
bool querySpoutSenderInfo(
    std::string_view senderName,
    SpoutSharedTextureInfo& outInfo,
    bool* outRegisteredInNameList = nullptr) noexcept;

// Zero-SDK Spout2 D3D11/D3D12 interop clean-feed sender (SPEC_v4 Section 7.6 & Section 13 M8).
// Dynamically loads inbox d3d11.dll at runtime, allocates a LUID-matched DXGI shared B8G8R8A8_UNORM
// texture, opens it in D3D12, registers in SpoutSenderNames shared memory, and blits strictly from
// Pass-3 CanvasClean (never including Pass-4 HUD, captions, A/B splits, or letterbox bars).
class SpoutSender {
public:
    SpoutSender();
    ~SpoutSender();

    SpoutSender(const SpoutSender&) = delete;
    SpoutSender& operator=(const SpoutSender&) = delete;

    bool initialize(
        D3D12Context& d3d,
        std::string_view senderName,
        uint32_t initialWidth,
        uint32_t initialHeight,
        std::string* outReason = nullptr);

    // Records a clean 16:9 CanvasClean -> shared B8G8R8A8_UNORM blit onto `cmdListRaw`.
    // Safe no-op (returns false without stalling playback) if inactive or if interop unavailable.
    bool recordCleanPublish(
        D3D12Context& d3d,
        void* cmdListRaw,
        void* canvasCleanResourceRaw,
        uint32_t canvasWidth,
        uint32_t canvasHeight) noexcept;

    void shutdown() noexcept;

    bool isActive() const noexcept;
    const std::string& senderName() const noexcept;
    uint32_t width() const noexcept;
    uint32_t height() const noexcept;
    uint32_t sharedHandleValue() const noexcept;
    uint64_t publishedFrameCount() const noexcept;
    const std::string& statusReason() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
