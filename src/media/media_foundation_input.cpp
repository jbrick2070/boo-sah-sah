#include "boo/media_input.hpp"
#include "boo/logging.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace boo {

namespace {

using Microsoft::WRL::ComPtr;

using PFN_MFCreateSourceReaderFromURL = HRESULT(STDAPICALLTYPE*)(
    LPCWSTR pwszURL,
    IMFAttributes* pAttributes,
    IMFSourceReader** ppSourceReader);

using PFN_MFCreateSourceReaderFromMediaSource = HRESULT(STDAPICALLTYPE*)(
    IMFMediaSource* pMediaSource,
    IMFAttributes* pAttributes,
    IMFSourceReader** ppSourceReader);

constexpr uint64_t kMaxLocalFileBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL; // 4 GB safety cap
constexpr uint32_t kMinDimension = 16u;
constexpr uint32_t kMaxDimension = 4096u;

std::wstring pathToWide(const std::filesystem::path& p) {
    return p.wstring();
}

struct CropMap16x9 {
    uint32_t cropX = 0;
    uint32_t cropY = 0;
    uint32_t cropW = 1;
    uint32_t cropH = 1;
};

CropMap16x9 computeAspectCoverCrop(uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH) noexcept {
    CropMap16x9 c{};
    c.cropW = std::max(1u, srcW);
    c.cropH = std::max(1u, srcH);
    const uint64_t lhs = static_cast<uint64_t>(srcW) * dstH;
    const uint64_t rhs = static_cast<uint64_t>(dstW) * srcH;
    if (lhs > rhs) {
        // Source is wider than 16:9 -> center-crop horizontally
        c.cropW = std::clamp<uint32_t>(static_cast<uint32_t>((static_cast<uint64_t>(srcH) * dstW) / dstH), 1u, srcW);
        c.cropX = (srcW - c.cropW) / 2u;
    } else if (lhs < rhs) {
        // Source is taller than 16:9 -> center-crop vertically
        c.cropH = std::clamp<uint32_t>(static_cast<uint32_t>((static_cast<uint64_t>(srcW) * dstH) / dstW), 1u, srcH);
        c.cropY = (srcH - c.cropH) / 2u;
    }
    return c;
}

// Safely resamples top-down or bottom-up BGRA8/BGRX8 (MFVideoFormat_RGB32) into bounded 1280x720 BT.709 limited-range NV12.
void resampleRgb32ToNv12(
    const uint8_t* row0Ptr,
    ptrdiff_t rowPitchBytes,
    uint32_t srcW,
    uint32_t srcH,
    uint8_t* dstNv12) noexcept {
    constexpr uint32_t dstW = MediaIngestSource::kTargetWidth;
    constexpr uint32_t dstH = MediaIngestSource::kTargetHeight;
    const CropMap16x9 crop = computeAspectCoverCrop(srcW, srcH, dstW, dstH);

    uint8_t* yPlane  = dstNv12;
    uint8_t* uvPlane = dstNv12 + static_cast<size_t>(dstW) * dstH;

    // Precompute source X indices
    uint32_t xMap[dstW];
    for (uint32_t dx = 0; dx < dstW; ++dx) {
        const uint32_t sx = crop.cropX + static_cast<uint32_t>((static_cast<uint64_t>(dx) * crop.cropW) / dstW);
        xMap[dx] = std::min(sx, srcW - 1u);
    }

    for (uint32_t dy = 0; dy < dstH; dy += 2u) {
        const uint32_t sy0 = std::min(crop.cropY + static_cast<uint32_t>((static_cast<uint64_t>(dy) * crop.cropH) / dstH), srcH - 1u);
        const uint32_t sy1 = std::min(crop.cropY + static_cast<uint32_t>((static_cast<uint64_t>(dy + 1u) * crop.cropH) / dstH), srcH - 1u);

        const uint8_t* srcRow0 = row0Ptr + static_cast<ptrdiff_t>(sy0) * rowPitchBytes;
        const uint8_t* srcRow1 = row0Ptr + static_cast<ptrdiff_t>(sy1) * rowPitchBytes;
        uint8_t* dstY0 = yPlane + static_cast<size_t>(dy) * dstW;
        uint8_t* dstY1 = yPlane + static_cast<size_t>(dy + 1u) * dstW;
        uint8_t* dstUv = uvPlane + static_cast<size_t>(dy / 2u) * dstW;

        for (uint32_t dx = 0; dx < dstW; dx += 2u) {
            const uint32_t sx0 = xMap[dx] * 4u;
            const uint32_t sx1 = xMap[dx + 1u] * 4u;

            const int b00 = srcRow0[sx0 + 0u], g00 = srcRow0[sx0 + 1u], r00 = srcRow0[sx0 + 2u];
            const int b01 = srcRow0[sx1 + 0u], g01 = srcRow0[sx1 + 1u], r01 = srcRow0[sx1 + 2u];
            const int b10 = srcRow1[sx0 + 0u], g10 = srcRow1[sx0 + 1u], r10 = srcRow1[sx0 + 2u];
            const int b11 = srcRow1[sx1 + 0u], g11 = srcRow1[sx1 + 1u], r11 = srcRow1[sx1 + 2u];

            // BT.709 studio-swing Y in [16..235], UV in [16..240]
            dstY0[dx + 0u] = static_cast<uint8_t>(std::clamp(((47 * r00 + 157 * g00 + 16 * b00 + 128) >> 8) + 16, 16, 235));
            dstY0[dx + 1u] = static_cast<uint8_t>(std::clamp(((47 * r01 + 157 * g01 + 16 * b01 + 128) >> 8) + 16, 16, 235));
            dstY1[dx + 0u] = static_cast<uint8_t>(std::clamp(((47 * r10 + 157 * g10 + 16 * b10 + 128) >> 8) + 16, 16, 235));
            dstY1[dx + 1u] = static_cast<uint8_t>(std::clamp(((47 * r11 + 157 * g11 + 16 * b11 + 128) >> 8) + 16, 16, 235));

            const int rAvg = (r00 + r01 + r10 + r11 + 2) >> 2;
            const int gAvg = (g00 + g01 + g10 + g11 + 2) >> 2;
            const int bAvg = (b00 + b01 + b10 + b11 + 2) >> 2;

            const int u = ((-26 * rAvg - 87 * gAvg + 112 * bAvg + 128) >> 8) + 128;
            const int v = ((112 * rAvg - 102 * gAvg - 10 * bAvg + 128) >> 8) + 128;
            dstUv[dx + 0u] = static_cast<uint8_t>(std::clamp(u, 16, 240));
            dstUv[dx + 1u] = static_cast<uint8_t>(std::clamp(v, 16, 240));
        }
    }
}

// Safely resamples contiguous or pitched NV12 into bounded 1280x720 NV12.
void resampleNv12ToNv12(
    const uint8_t* srcYBase,
    const uint8_t* srcUvBase,
    size_t srcPitch,
    uint32_t srcW,
    uint32_t srcH,
    uint8_t* dstNv12) noexcept {
    constexpr uint32_t dstW = MediaIngestSource::kTargetWidth;
    constexpr uint32_t dstH = MediaIngestSource::kTargetHeight;
    if (srcW == dstW && srcH == dstH && srcPitch == dstW) {
        std::memcpy(dstNv12, srcYBase, static_cast<size_t>(dstW) * dstH);
        std::memcpy(dstNv12 + static_cast<size_t>(dstW) * dstH, srcUvBase, static_cast<size_t>(dstW) * (dstH / 2u));
        return;
    }

    const CropMap16x9 crop = computeAspectCoverCrop(srcW, srcH, dstW, dstH);
    uint8_t* dstY  = dstNv12;
    uint8_t* dstUv = dstNv12 + static_cast<size_t>(dstW) * dstH;

    uint32_t xMap[dstW];
    for (uint32_t dx = 0; dx < dstW; ++dx) {
        const uint32_t sx = crop.cropX + static_cast<uint32_t>((static_cast<uint64_t>(dx) * crop.cropW) / dstW);
        xMap[dx] = std::min(sx, srcW - 1u);
    }

    for (uint32_t dy = 0; dy < dstH; ++dy) {
        const uint32_t sy = std::min(crop.cropY + static_cast<uint32_t>((static_cast<uint64_t>(dy) * crop.cropH) / dstH), srcH - 1u);
        const uint8_t* sRow = srcYBase + static_cast<size_t>(sy) * srcPitch;
        uint8_t* dRow = dstY + static_cast<size_t>(dy) * dstW;
        for (uint32_t dx = 0; dx < dstW; ++dx) {
            dRow[dx] = sRow[xMap[dx]];
        }
    }

    const uint32_t uvH = std::max(1u, srcH / 2u);
    const uint32_t uvW = std::max(1u, srcW / 2u);
    for (uint32_t dy = 0; dy < dstH / 2u; ++dy) {
        const uint32_t sy = std::min((crop.cropY / 2u) + static_cast<uint32_t>((static_cast<uint64_t>(dy) * (crop.cropH / 2u)) / (dstH / 2u)), uvH - 1u);
        const uint8_t* sUvRow = srcUvBase + static_cast<size_t>(sy) * srcPitch;
        uint8_t* dUvRow = dstUv + static_cast<size_t>(dy) * dstW;
        for (uint32_t dx = 0; dx < dstW / 2u; ++dx) {
            const uint32_t sxUv = std::min(xMap[dx * 2u] / 2u, uvW - 1u);
            dUvRow[dx * 2u + 0u] = sUvRow[sxUv * 2u + 0u];
            dUvRow[dx * 2u + 1u] = sUvRow[sxUv * 2u + 1u];
        }
    }
}

bool decodeSampleIntoNv12(
    IMFSample* sample,
    bool sourceIsRgb32,
    uint32_t srcW,
    uint32_t srcH,
    INT32 defaultStride,
    std::vector<uint8_t>& outNv12) noexcept {
    if (!sample || srcW < kMinDimension || srcH < kMinDimension || srcW > kMaxDimension || srcH > kMaxDimension) {
        return false;
    }

    ComPtr<IMFMediaBuffer> mediaBuf;
    if (FAILED(sample->ConvertToContiguousBuffer(&mediaBuf)) || !mediaBuf) {
        return false;
    }

    outNv12.resize(MediaIngestSource::kNv12ByteSize);

    // Prefer IMF2DBuffer::Lock2D when supported so hardware surfaces & bottom-up pitches are exact
    ComPtr<IMF2DBuffer> buf2d;
    if (sourceIsRgb32 && SUCCEEDED(mediaBuf.As(&buf2d)) && buf2d) {
        BYTE* scanline0 = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buf2d->Lock2D(&scanline0, &pitch)) && scanline0 && std::abs(pitch) >= static_cast<LONG>(srcW * 4u)) {
            resampleRgb32ToNv12(scanline0, static_cast<ptrdiff_t>(pitch), srcW, srcH, outNv12.data());
            buf2d->Unlock2D();
            return true;
        }
    }

    BYTE* rawPtr = nullptr;
    DWORD maxLen = 0;
    DWORD curLen = 0;
    if (FAILED(mediaBuf->Lock(&rawPtr, &maxLen, &curLen)) || !rawPtr) {
        return false;
    }

    bool ok = false;
    if (sourceIsRgb32) {
        const ptrdiff_t stride = (defaultStride != 0)
            ? static_cast<ptrdiff_t>(defaultStride)
            : static_cast<ptrdiff_t>(srcW) * 4;
        const size_t absStride = static_cast<size_t>(std::abs(stride));
        const size_t neededBytes = absStride * static_cast<size_t>(srcH);
        if (absStride >= static_cast<size_t>(srcW) * 4u && static_cast<size_t>(curLen) >= neededBytes) {
            const uint8_t* row0 = (stride >= 0)
                ? rawPtr
                : (rawPtr + static_cast<size_t>(srcH - 1u) * absStride);
            resampleRgb32ToNv12(row0, stride, srcW, srcH, outNv12.data());
            ok = true;
        }
    } else {
        const size_t pitch = (defaultStride > 0) ? static_cast<size_t>(defaultStride) : static_cast<size_t>(srcW);
        const uint32_t alignedH16 = (srcH + 15u) & ~15u;
        const size_t minNeeded = pitch * static_cast<size_t>(srcH) + pitch * static_cast<size_t>(srcH / 2u);
        if (pitch >= srcW && static_cast<size_t>(curLen) >= minNeeded) {
            const size_t alignedTotal = pitch * static_cast<size_t>(alignedH16) + pitch * static_cast<size_t>(srcH / 2u);
            const size_t uvOffset = (static_cast<size_t>(curLen) >= alignedTotal)
                ? (pitch * static_cast<size_t>(alignedH16))
                : (pitch * static_cast<size_t>(srcH));
            resampleNv12ToNv12(rawPtr, rawPtr + uvOffset, pitch, srcW, srcH, outNv12.data());
            ok = true;
        }
    }

    mediaBuf->Unlock();
    return ok;
}

} // namespace

struct MediaIngestSource::Impl {
    mutable std::mutex mtx;
    std::condition_variable initCv;
    bool initDone = false;
    bool initSucceeded = false;

    std::atomic<bool> stopRequested{false};
    std::atomic<bool> active{false};
    std::atomic<bool> liveCamera{false};
    std::atomic<uint8_t> sourceMode{static_cast<uint8_t>(MediaSourceMode::None)};
    std::atomic<uint64_t> frameSeq{0};

    std::vector<uint8_t> latestNv12;
    std::string diagCode = "MEDIA_IDLE_OPT_IN";
    std::string reason = "Media ingest idle (strictly opt-in via --media-input or --camera-input).";
    std::thread worker;

    void setStatusLocked(const char* code, std::string msg) {
        diagCode = code ? code : "MEDIA_STATUS";
        reason = std::move(msg);
    }

    void stopWorker() noexcept {
        stopRequested.store(true, std::memory_order_release);
        if (worker.joinable()) {
            worker.join();
        }
        active.store(false, std::memory_order_release);
        liveCamera.store(false, std::memory_order_release);
        sourceMode.store(static_cast<uint8_t>(MediaSourceMode::None), std::memory_order_release);
    }

    bool startSource(MediaSourceMode requestedMode, const std::filesystem::path& filePath, bool injectFailure) noexcept {
        stopWorker();

        const std::string redactedTarget = (requestedMode == MediaSourceMode::LocalVideoFile)
            ? redactUserPath(filePath.string())
            : std::string("UVC_DEFAULT_CAMERA");

        if (injectFailure) {
            std::lock_guard<std::mutex> lk(mtx);
            latestNv12.clear();
            setStatusLocked(
                "MEDIA_FAULT_INJECTED",
                "Simulated media/camera ingest fault (--inject-media-fail) for '" + redactedTarget +
                    "'; falling back cleanly to procedural Signal Hijack.");
            return false;
        }

        if (requestedMode == MediaSourceMode::LocalVideoFile) {
            std::error_code ec;
            if (filePath.empty() || !std::filesystem::exists(filePath, ec) || !std::filesystem::is_regular_file(filePath, ec)) {
                std::lock_guard<std::mutex> lk(mtx);
                latestNv12.clear();
                setStatusLocked(
                    "MEDIA_FILE_NOT_FOUND",
                    "Opt-in media file not found: '" + redactedTarget + "'; falling back cleanly to procedural Signal Hijack.");
                return false;
            }
            const uint64_t fsz = std::filesystem::file_size(filePath, ec);
            if (ec || fsz == 0 || fsz > kMaxLocalFileBytes) {
                std::lock_guard<std::mutex> lk(mtx);
                latestNv12.clear();
                setStatusLocked(
                    "MEDIA_FILE_SIZE_REJECTED",
                    "Opt-in media file rejected by size bounds (0 < size <= 4 GB): '" + redactedTarget + "'.");
                return false;
            }
        }

        {
            std::lock_guard<std::mutex> lk(mtx);
            initDone = false;
            initSucceeded = false;
            latestNv12.clear();
        }
        stopRequested.store(false, std::memory_order_release);

        try {
            worker = std::thread([this, requestedMode, filePath, redactedTarget]() {
                runWorkerLoop(requestedMode, filePath, redactedTarget);
            });
        } catch (...) {
            std::lock_guard<std::mutex> lk(mtx);
            setStatusLocked("MEDIA_THREAD_FAILED", "Failed to launch background Media Foundation ingest worker.");
            return false;
        }

        std::unique_lock<std::mutex> lk(mtx);
        initCv.wait_for(lk, std::chrono::milliseconds(3500), [this]() { return initDone; });
        if (!initDone) {
            lk.unlock();
            stopWorker();
            std::lock_guard<std::mutex> lk2(mtx);
            setStatusLocked(
                "MEDIA_OPEN_TIMEOUT",
                "Timed out opening '" + redactedTarget + "'; falling back cleanly without stalling playback.");
            return false;
        }
        if (!initSucceeded) {
            lk.unlock();
            stopWorker();
            return false;
        }
        return true;
    }

    void failInit(const char* code, std::string msg) noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx);
            setStatusLocked(code, std::move(msg));
            initSucceeded = false;
            initDone = true;
        }
        initCv.notify_all();
    }

    void runWorkerLoop(MediaSourceMode reqMode, const std::filesystem::path& filePath, const std::string& redactedTarget) noexcept {
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool needCoUninit = SUCCEEDED(hrCo);

        bool mfStarted = false;
        HMODULE hMfRw = nullptr;
        ComPtr<IMFMediaSource> cameraMediaSource;
        ComPtr<IMFSourceReader> reader;

        auto cleanupAll = [&]() noexcept {
            reader.Reset();
            if (cameraMediaSource) {
                cameraMediaSource->Shutdown();
                cameraMediaSource.Reset();
            }
            if (mfStarted) {
                MFShutdown();
                mfStarted = false;
            }
            if (hMfRw) {
                FreeLibrary(hMfRw);
                hMfRw = nullptr;
            }
            if (needCoUninit) {
                CoUninitialize();
            }
            active.store(false, std::memory_order_release);
            liveCamera.store(false, std::memory_order_release);
        };

        try {
            hMfRw = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!hMfRw) {
                failInit("MFREADWRITE_UNAVAILABLE",
                         "Windows Media Foundation mfreadwrite.dll unavailable (e.g. Windows N without Media Feature Pack).");
                cleanupAll();
                return;
            }

            const auto pfnReaderFromUrl = reinterpret_cast<PFN_MFCreateSourceReaderFromURL>(
                GetProcAddress(hMfRw, "MFCreateSourceReaderFromURL"));
            const auto pfnReaderFromSource = reinterpret_cast<PFN_MFCreateSourceReaderFromMediaSource>(
                GetProcAddress(hMfRw, "MFCreateSourceReaderFromMediaSource"));
            if (!pfnReaderFromUrl || !pfnReaderFromSource) {
                failInit("MFREADWRITE_ENTRY_MISSING", "mfreadwrite.dll missing MFCreateSourceReader entry points.");
                cleanupAll();
                return;
            }

            if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
                failInit("MF_STARTUP_FAILED", "MFStartup failed while opening opt-in media source.");
                cleanupAll();
                return;
            }
            mfStarted = true;

            ComPtr<IMFAttributes> readerAttrs;
            if (FAILED(MFCreateAttributes(&readerAttrs, 2)) || !readerAttrs) {
                failInit("MF_ATTR_FAILED", "Failed to create Media Foundation source reader attributes.");
                cleanupAll();
                return;
            }
            readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            readerAttrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);

            if (reqMode == MediaSourceMode::LocalVideoFile) {
                const std::wstring wideUrl = pathToWide(filePath);
                const HRESULT hrOpen = pfnReaderFromUrl(wideUrl.c_str(), readerAttrs.Get(), &reader);
                if (FAILED(hrOpen) || !reader) {
                    failInit("MEDIA_UNSUPPORTED_OR_CORRUPT",
                             "Media Foundation rejected '" + redactedTarget +
                                 "' (unsupported container/codec or corrupt header); falling back to procedural Signal Hijack.");
                    cleanupAll();
                    return;
                }
            } else {
                ComPtr<IMFAttributes> devAttrs;
                if (FAILED(MFCreateAttributes(&devAttrs, 1)) ||
                    FAILED(devAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
                    failInit("CAMERA_ENUM_ATTR_FAILED", "Failed to configure video-only UVC capture enumeration.");
                    cleanupAll();
                    return;
                }
                IMFActivate** devices = nullptr;
                UINT32 deviceCount = 0;
                const HRESULT hrEnum = MFEnumDeviceSources(devAttrs.Get(), &devices, &deviceCount);
                if (FAILED(hrEnum) || deviceCount == 0 || !devices) {
                    if (devices) CoTaskMemFree(devices);
                    failInit("CAMERA_UNAVAILABLE",
                             "No UVC video capture camera available or OS camera privacy access disabled; continuing procedural Signal Hijack.");
                    cleanupAll();
                    return;
                }
                const HRESULT hrAct = devices[0]->ActivateObject(IID_PPV_ARGS(&cameraMediaSource));
                for (UINT32 i = 0; i < deviceCount; ++i) {
                    if (devices[i]) devices[i]->Release();
                }
                CoTaskMemFree(devices);

                if (FAILED(hrAct) || !cameraMediaSource ||
                    FAILED(pfnReaderFromSource(cameraMediaSource.Get(), readerAttrs.Get(), &reader)) || !reader) {
                    failInit("CAMERA_ACTIVATE_FAILED",
                             "Failed to open UVC video capture device (in use or denied); continuing procedural Signal Hijack.");
                    cleanupAll();
                    return;
                }
            }

            // Strictly deselect ALL audio/metadata streams; select ONLY first video stream (SPEC_v4 Section 11)
            reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            if (FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE))) {
                failInit("MEDIA_NO_VIDEO_STREAM",
                         "No decodable video stream found in '" + redactedTarget + "'.");
                cleanupAll();
                return;
            }

            bool sourceIsRgb32 = true;
            {
                ComPtr<IMFMediaType> outType;
                MFCreateMediaType(&outType);
                outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, outType.Get()))) {
                    sourceIsRgb32 = false;
                    ComPtr<IMFMediaType> nv12Type;
                    MFCreateMediaType(&nv12Type);
                    nv12Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                    nv12Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                    if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, nv12Type.Get()))) {
                        failInit("MEDIA_FORMAT_UNSUPPORTED",
                                 "Unsupported video pixel format in '" + redactedTarget + "'.");
                        cleanupAll();
                        return;
                    }
                }
            }

            uint32_t srcW = 0;
            uint32_t srcH = 0;
            INT32 defaultStride = 0;
            auto refreshMediaDimensions = [&]() -> bool {
                ComPtr<IMFMediaType> curType;
                if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &curType)) || !curType) {
                    return false;
                }
                UINT32 w = 0, h = 0;
                if (FAILED(MFGetAttributeSize(curType.Get(), MF_MT_FRAME_SIZE, &w, &h))) {
                    return false;
                }
                if (w < kMinDimension || h < kMinDimension || w > kMaxDimension || h > kMaxDimension) {
                    return false;
                }
                srcW = w;
                srcH = h;
                UINT32 rawStride = 0;
                if (SUCCEEDED(curType->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride))) {
                    defaultStride = static_cast<INT32>(rawStride);
                } else {
                    defaultStride = sourceIsRgb32 ? static_cast<INT32>(srcW * 4u) : static_cast<INT32>(srcW);
                }
                return true;
            };

            if (!refreshMediaDimensions()) {
                failInit("MEDIA_DIMENSIONS_REJECTED",
                         "Rejected invalid or out-of-bounds frame dimensions (valid: 16..4096) in '" + redactedTarget + "'.");
                cleanupAll();
                return;
            }

            // Decode and validate 1st video frame before reporting active
            std::vector<uint8_t> stagingNv12;
            bool gotInitialFrame = false;
            for (int attempt = 0; attempt < 24 && !stopRequested.load(std::memory_order_acquire); ++attempt) {
                DWORD streamIdx = 0;
                DWORD flags = 0;
                LONGLONG ts = 0;
                ComPtr<IMFSample> sample;
                const HRESULT hrRead = reader->ReadSample(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                    0, &streamIdx, &flags, &ts, &sample);
                if (FAILED(hrRead) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
                    break;
                }
                if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                    if (!refreshMediaDimensions()) break;
                }
                if (sample && decodeSampleIntoNv12(sample.Get(), sourceIsRgb32, srcW, srcH, defaultStride, stagingNv12)) {
                    gotInitialFrame = true;
                    break;
                }
                if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                    break;
                }
            }

            if (!gotInitialFrame) {
                failInit("MEDIA_DECODE_FAILED",
                         "Failed to decode a valid video frame from '" + redactedTarget +
                             "'; falling back cleanly to procedural Signal Hijack.");
                cleanupAll();
                return;
            }

            sourceMode.store(static_cast<uint8_t>(reqMode), std::memory_order_release);
            liveCamera.store(reqMode == MediaSourceMode::LiveCamera, std::memory_order_release);
            active.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(mtx);
                latestNv12 = stagingNv12;
                frameSeq.store(1ULL, std::memory_order_release);
                setStatusLocked(
                    (reqMode == MediaSourceMode::LiveCamera) ? "MEDIA_LIVE_CAMERA_ACTIVE" : "MEDIA_LOCAL_FILE_ACTIVE",
                    (reqMode == MediaSourceMode::LiveCamera)
                        ? ("Opt-in UVC camera active (" + std::to_string(srcW) + "x" + std::to_string(srcH) +
                           " -> 1280x720 NV12, LIVE CAMERA indicator illuminated).")
                        : ("Opt-in local video active: '" + redactedTarget + "' (" +
                           std::to_string(srcW) + "x" + std::to_string(srcH) + " -> 1280x720 NV12)."));
                initSucceeded = true;
                initDone = true;
            }
            initCv.notify_all();

            // Non-blocking <=30 fps decode loop (SPEC_v4 Section 5.3)
            constexpr auto kFramePeriod = std::chrono::milliseconds(33);
            uint32_t consecutiveErrors = 0;
            while (!stopRequested.load(std::memory_order_acquire)) {
                const auto tickStart = std::chrono::steady_clock::now();

                DWORD streamIdx = 0;
                DWORD flags = 0;
                LONGLONG ts = 0;
                ComPtr<IMFSample> sample;
                const HRESULT hrRead = reader->ReadSample(
                    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                    0, &streamIdx, &flags, &ts, &sample);

                if (FAILED(hrRead) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
                    if (++consecutiveErrors >= 4u) {
                        std::lock_guard<std::mutex> lk(mtx);
                        setStatusLocked(
                            "MEDIA_RUNTIME_STREAM_LOST",
                            "Media stream interrupted on '" + redactedTarget + "'; reverting to procedural Signal Hijack.");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                    continue;
                }
                consecutiveErrors = 0;

                if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                    if (!refreshMediaDimensions()) {
                        break;
                    }
                }

                if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 && reqMode == MediaSourceMode::LocalVideoFile) {
                    PROPVARIANT varZero;
                    PropVariantInit(&varZero);
                    varZero.vt = VT_I8;
                    varZero.hVal.QuadPart = 0;
                    reader->SetCurrentPosition(GUID_NULL, varZero);
                    PropVariantClear(&varZero);
                    continue;
                }

                if (sample && decodeSampleIntoNv12(sample.Get(), sourceIsRgb32, srcW, srcH, defaultStride, stagingNv12)) {
                    std::lock_guard<std::mutex> lk(mtx);
                    latestNv12 = stagingNv12;
                    frameSeq.fetch_add(1ULL, std::memory_order_release);
                }

                const auto elapsed = std::chrono::steady_clock::now() - tickStart;
                if (elapsed < kFramePeriod && !stopRequested.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(kFramePeriod - elapsed);
                }
            }
        } catch (...) {
            if (!initDone) {
                failInit("MEDIA_EXCEPTION_CAUGHT", "Caught unexpected exception inside media boundary; continuing safely.");
            }
        }

        cleanupAll();
    }
};

MediaIngestSource::MediaIngestSource()
    : impl_(std::make_unique<Impl>()) {}

MediaIngestSource::~MediaIngestSource() {
    shutdown();
}

bool MediaIngestSource::openLocalFile(const std::filesystem::path& filePath, bool injectFailure) noexcept {
    return impl_ && impl_->startSource(MediaSourceMode::LocalVideoFile, filePath, injectFailure);
}

bool MediaIngestSource::openLiveCamera(bool injectFailure) noexcept {
    return impl_ && impl_->startSource(MediaSourceMode::LiveCamera, {}, injectFailure);
}

void MediaIngestSource::shutdown() noexcept {
    if (!impl_) return;
    impl_->stopWorker();
}

bool MediaIngestSource::isActive() const noexcept {
    return impl_ && impl_->active.load(std::memory_order_acquire);
}

bool MediaIngestSource::isLiveCameraActive() const noexcept {
    return impl_ && impl_->liveCamera.load(std::memory_order_acquire);
}

MediaSourceMode MediaIngestSource::mode() const noexcept {
    return impl_ ? static_cast<MediaSourceMode>(impl_->sourceMode.load(std::memory_order_acquire)) : MediaSourceMode::None;
}

bool MediaIngestSource::copyLatestNv12Frame(std::vector<uint8_t>& outNv12, uint64_t& outFrameSeq) const noexcept {
    if (!impl_ || !impl_->active.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (impl_->latestNv12.size() != kNv12ByteSize) {
        return false;
    }
    outNv12 = impl_->latestNv12;
    outFrameSeq = impl_->frameSeq.load(std::memory_order_acquire);
    return true;
}

uint64_t MediaIngestSource::decodedFrameCount() const noexcept {
    return impl_ ? impl_->frameSeq.load(std::memory_order_acquire) : 0ULL;
}

std::string MediaIngestSource::diagnosticCode() const noexcept {
    if (!impl_) return "MEDIA_UNINITIALIZED";
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->diagCode;
}

std::string MediaIngestSource::statusReason() const noexcept {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->reason;
}

} // namespace boo
