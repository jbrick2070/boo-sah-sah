#include "boo/api.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>

namespace boo {

FrameGenerationMode availableFrameGenerationMode(FrameGenerationMode request, uint32_t maxGenerated) noexcept {
    if (request == FrameGenerationMode::Off || request == FrameGenerationMode::Auto) return request;
    if (!maxGenerated) return FrameGenerationMode::Off;
    const auto maxMultiplier = 1u + std::min(maxGenerated, 5u);
    return static_cast<FrameGenerationMode>(std::min(static_cast<uint32_t>(request), maxMultiplier));
}

FrameGenerationMode nextFrameGenerationMode(FrameGenerationMode current, uint32_t maxGenerated) noexcept {
    const uint32_t last = 1u + std::min(maxGenerated, 5u);
    return static_cast<uint32_t>(current) >= last ? FrameGenerationMode::Off
        : static_cast<FrameGenerationMode>(static_cast<uint32_t>(current) + 1u);
}

namespace {
std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}
} // namespace

const char* toString(BackendStatus status) noexcept {
    switch (status) {
        case BackendStatus::Accelerated: return "Accelerated";
        case BackendStatus::Substituted: return "Substituted";
        case BackendStatus::Degraded:    return "Degraded";
        case BackendStatus::Unavailable: return "Unavailable";
        case BackendStatus::Faulted:     return "Faulted";
    }
    return "Unknown";
}

const char* toString(HealthState state) noexcept {
    switch (state) {
        case HealthState::Probing:           return "Probing";
        case HealthState::Ready:             return "Ready";
        case HealthState::ActiveAccelerated: return "ActiveAccelerated";
        case HealthState::ActiveSubstitute:  return "ActiveSubstitute";
        case HealthState::ActiveDegraded:    return "ActiveDegraded";
        case HealthState::Unavailable:       return "Unavailable";
        case HealthState::Faulted:           return "Faulted";
        case HealthState::Cooldown:          return "Cooldown";
    }
    return "Unknown";
}

const char* toString(EngineKind kind) noexcept {
    switch (kind) {
        case EngineKind::RT:     return "RT";
        case EngineKind::Tensor: return "Tensor";
        case EngineKind::NPU:    return "NPU";
        case EngineKind::CUDA:   return "CUDA";
        case EngineKind::Codec:  return "Codec";
    }
    return "Unknown";
}

const char* toString(PlaybackMode mode) noexcept {
    switch (mode) {
        case PlaybackMode::FullFilm:   return "FullFilm";
        case PlaybackMode::SoloRT:     return "SoloRT";
        case PlaybackMode::SoloTensor: return "SoloTensor";
        case PlaybackMode::SoloNPU:    return "SoloNPU";
        case PlaybackMode::SoloCUDA:   return "SoloCUDA";
        case PlaybackMode::FinaleLoop: return "FinaleLoop";
    }
    return "FullFilm";
}

const char* toString(Edition edition) noexcept {
    switch (edition) {
        case Edition::Family:   return "family";
        case Edition::Midnight: return "midnight";
    }
    return "family";
}

const char* toString(QualityPresetId preset) noexcept {
    switch (preset) {
        case QualityPresetId::Safe:           return "safe";
        case QualityPresetId::Balanced:       return "balanced";
        case QualityPresetId::Showcase:       return "showcase";
        case QualityPresetId::Experimental4K: return "4k_experimental";
    }
    return "balanced";
}

const char* toString(AbCompareEngine engine) noexcept {
    switch (engine) {
        case AbCompareEngine::RT:     return "RT";
        case AbCompareEngine::CUDA:   return "CUDA";
        case AbCompareEngine::Tensor: return "Tensor";
        case AbCompareEngine::Codec:  return "Codec";
        case AbCompareEngine::NPU:    return "NPU";
    }
    return "RT";
}

const char* toString(ShowVisualMode mode) noexcept {
    switch (mode) {
        case ShowVisualMode::LegacyM0:      return "LegacyM0";
        case ShowVisualMode::M1Slice60s:    return "M1Slice60s";
        case ShowVisualMode::M2Trilogy180s: return "M2Trilogy180s";
        case ShowVisualMode::FullFilm360s:  return "FullFilm360s";
        case ShowVisualMode::SoloRT:        return "SoloRT";
        case ShowVisualMode::SoloCUDA:      return "SoloCUDA";
        case ShowVisualMode::SoloTensor:    return "SoloTensor";
        case ShowVisualMode::SoloNPU:       return "SoloNPU";
    }
    return "LegacyM0";
}

bool parsePlaybackMode(std::string_view text, PlaybackMode& out) noexcept {
    const std::string k = toLowerAscii(text);
    if (k == "full" || k == "fullfilm" || k == "full_film") { out = PlaybackMode::FullFilm; return true; }
    if (k == "solo_rt" || k == "solort" || k == "rt")       { out = PlaybackMode::SoloRT; return true; }
    if (k == "solo_tensor" || k == "solotensor" || k == "tensor") { out = PlaybackMode::SoloTensor; return true; }
    if (k == "solo_npu" || k == "solonpu" || k == "npu")    { out = PlaybackMode::SoloNPU; return true; }
    if (k == "solo_cuda" || k == "solocuda" || k == "cuda") { out = PlaybackMode::SoloCUDA; return true; }
    if (k == "finale" || k == "finale_loop" || k == "finaleloop") { out = PlaybackMode::FinaleLoop; return true; }
    return false;
}

bool parseEdition(std::string_view text, Edition& out) noexcept {
    const std::string k = toLowerAscii(text);
    if (k == "family")   { out = Edition::Family; return true; }
    if (k == "midnight") { out = Edition::Midnight; return true; }
    return false;
}

bool parseQualityPreset(std::string_view text, QualityPresetId& out) noexcept {
    const std::string k = toLowerAscii(text);
    if (k == "safe")            { out = QualityPresetId::Safe; return true; }
    if (k == "balanced")        { out = QualityPresetId::Balanced; return true; }
    if (k == "showcase")        { out = QualityPresetId::Showcase; return true; }
    if (k == "4k" || k == "4k_experimental" || k == "experimental4k") {
        out = QualityPresetId::Experimental4K;
        return true;
    }
    return false;
}

std::filesystem::path resolveResourcePath(const std::filesystem::path& relativeOrAbs) {
    if (relativeOrAbs.empty()) {
        return relativeOrAbs;
    }
    std::error_code ec;
    if (relativeOrAbs.is_absolute() || std::filesystem::exists(relativeOrAbs, ec)) {
        return relativeOrAbs;
    }

    wchar_t exeBuf[MAX_PATH * 4]{};
    const DWORD len = GetModuleFileNameW(nullptr, exeBuf, static_cast<DWORD>(std::size(exeBuf)));
    if (len > 0 && len < std::size(exeBuf)) {
        std::filesystem::path exeDir = std::filesystem::path(exeBuf).parent_path();
        std::filesystem::path p1 = exeDir / relativeOrAbs;
        if (std::filesystem::exists(p1, ec)) {
            return p1;
        }
        std::filesystem::path p2 = exeDir.parent_path() / relativeOrAbs;
        if (std::filesystem::exists(p2, ec)) {
            return p2;
        }
        std::filesystem::path p3 = exeDir.parent_path().parent_path() / relativeOrAbs;
        if (std::filesystem::exists(p3, ec)) {
            return p3;
        }
    }
    return relativeOrAbs;
}

} // namespace boo
