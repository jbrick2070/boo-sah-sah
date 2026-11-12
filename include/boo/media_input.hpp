#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace boo {

enum class MediaSourceMode : uint8_t {
    None = 0,
    LocalVideoFile = 1,
    LiveCamera = 2
};

// Milestone M9 Opt-In Media Foundation Video & UVC Camera Ingest (SPEC_v4 Sections 3.1, 5.1-5.3, 11):
//   - Strictly opt-in: default state opens zero local files and zero camera devices.
//   - Audio/microphone streams are unconditionally deselected; zero audio or pixel data is ever logged or saved.
//   - Dynamically loads inbox `mfreadwrite.dll` via LoadLibraryExW(LOAD_LIBRARY_SEARCH_SYSTEM32)
//     so `boo_sah_sah.exe` gains zero extra static DLL imports.
//   - Validates source dimensions [16..4096] and buffer lengths before reading, resampling into
//     a bounded 1280x720 BT.709 NV12 staging ring (1,382,400 bytes) on a non-blocking <=30 fps worker thread.
class MediaIngestSource {
public:
    static constexpr uint32_t kTargetWidth  = 1280;
    static constexpr uint32_t kTargetHeight = 720;
    static constexpr size_t   kNv12ByteSize = static_cast<size_t>(kTargetWidth) * kTargetHeight * 3u / 2u;

    MediaIngestSource();
    ~MediaIngestSource();

    MediaIngestSource(const MediaIngestSource&) = delete;
    MediaIngestSource& operator=(const MediaIngestSource&) = delete;

    bool openLocalFile(const std::filesystem::path& filePath, bool injectFailure = false) noexcept;
    bool openLiveCamera(bool injectFailure = false) noexcept;
    void shutdown() noexcept;

    bool isActive() const noexcept;
    bool isLiveCameraActive() const noexcept;
    MediaSourceMode mode() const noexcept;

    // Non-blocking copy of the latest decoded 1280x720 NV12 frame if available.
    // Returns true when a valid frame is present in `outNv12` (resized to `kNv12ByteSize`).
    bool copyLatestNv12Frame(std::vector<uint8_t>& outNv12, uint64_t& outFrameSeq) const noexcept;

    uint64_t decodedFrameCount() const noexcept;
    std::string diagnosticCode() const noexcept;
    std::string statusReason() const noexcept; // Always path-redacted via redactUserPath()

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
