#include "boo/health.hpp"
#include "boo/logging.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <numeric>

namespace boo {

FallbackOrchestrator::FallbackOrchestrator() {
    for (size_t i = 0; i < kEngineKindCount; ++i) {
        engineStates_[i].engine = static_cast<EngineKind>(i);
        engineStates_[i].status = BackendStatus::Unavailable;
        engineStates_[i].health = HealthState::Unavailable;
        announcedAtBoundary_[i] = false;
    }
}

void FallbackOrchestrator::registerEngineState(const HardwareResult& initialResult) {
    const size_t idx = static_cast<size_t>(initialResult.engine);
    if (idx < kEngineKindCount) {
        engineStates_[idx] = initialResult;
    }
}

void FallbackOrchestrator::setPrimaryAndFallbackEffects(
    std::unique_ptr<IEffect> primary,
    std::unique_ptr<IEffect> fallback) {
    primaryEffect_  = std::move(primary);
    fallbackEffect_ = std::move(fallback);
    usingFallback_  = false;
    primaryFaulted_ = false;
}

void FallbackOrchestrator::syncLiveEngineStatuses() noexcept {
    if (!primaryEffect_) {
        return;
    }
    for (size_t i = 0; i < kEngineKindCount; ++i) {
        const auto kind = static_cast<EngineKind>(i);
        if (!primaryEffect_->ownsLiveEngineStatus(kind)) {
            continue;
        }
        if (!usingFallback_) {
            HardwareResult liveSt{};
            if (primaryEffect_->queryLiveEngineStatus(kind, liveSt)) {
                if (liveSt.detectedHardwareDetail.empty()) {
                    liveSt.detectedHardwareDetail = engineStates_[i].detectedHardwareDetail;
                }
                engineStates_[i] = std::move(liveSt);
            }
        } else {
            HardwareResult degraded = engineStates_[i];
            degraded.engine = kind;
            degraded.status = BackendStatus::Degraded;
            degraded.health = HealthState::ActiveDegraded;
            degraded.activeBackend = "D3D12_EmbeddedSafeShader";
            degraded.diagnosticCode = "SAFE_FALLBACK_SHADER_ACTIVE";
            degraded.humanReadableReason =
                "Primary M1 visual faulted or was injected to fail; guaranteed precompiled safe fallback active.";
            degraded.successfullyExecutedTestWork = false;
            engineStates_[i] = std::move(degraded);
        }
    }
}

HardwareResult FallbackOrchestrator::initializeEffects(bool injectPrimaryFailure) {
    HardwareResult fallbackRes{};
    if (fallbackEffect_) {
        try {
            fallbackRes = fallbackEffect_->probeAndInitialize();
        } catch (const std::exception& e) {
            BOO_LOG_ERROR("Health", std::string("Fallback effect threw during probe: ") + e.what());
        }
    }

    if (primaryEffect_ && !injectPrimaryFailure) {
        try {
            HardwareResult primRes = primaryEffect_->probeAndInitialize();
            if (primRes.status != BackendStatus::Faulted && primRes.status != BackendStatus::Unavailable) {
                usingFallback_ = false;
                syncLiveEngineStatuses();
                return primRes;
            }
            primaryFaulted_ = true;
            ++faultCount_;
            BOO_LOG_WARN(
                "Health",
                "Primary effect '" + std::string(primaryEffect_->name()) +
                    "' failed init (" + primRes.humanReadableReason + "); activating safe fallback.");
        } catch (const std::exception& e) {
            primaryFaulted_ = true;
            ++faultCount_;
            BOO_LOG_WARN("Health", std::string("Primary effect exception on init: ") + e.what());
        }
    } else if (injectPrimaryFailure) {
        primaryFaulted_ = true;
        ++faultCount_;
        BOO_LOG_WARN("Health", "Injected primary effect initialization failure; activating safe fallback.");
    }

    usingFallback_ = (fallbackEffect_ != nullptr);
    syncLiveEngineStatuses();
    return fallbackRes;
}

void FallbackOrchestrator::onSceneBoundary(uint32_t newActIndex, double filmTimeSeconds, bool retryQuarantined) {
    if (retryQuarantined && primaryFaulted_ && primaryEffect_) {
        try {
            const HardwareResult retryRes = primaryEffect_->probeAndInitialize();
            if (retryRes.status != BackendStatus::Faulted && retryRes.status != BackendStatus::Unavailable) {
                primaryFaulted_ = false;
                usingFallback_ = false;
                syncLiveEngineStatuses();
                BOO_LOG_INFO("Health", "Recovered primary effect at safe scene boundary.");
            }
        } catch (...) {
            // Keep quarantined until next explicit retry
        }
    }

    if (!usingFallback_ && primaryEffect_) {
        primaryEffect_->onSceneBoundary(newActIndex, filmTimeSeconds);
    } else if (fallbackEffect_) {
        fallbackEffect_->onSceneBoundary(newActIndex, filmTimeSeconds);
    }

    // Section 8.1: Emit at most one toast per non-accelerated engine at scene boundary (no error flood)
    for (size_t i = 0; i < kEngineKindCount; ++i) {
        const auto& st = engineStates_[i];
        if (st.status != BackendStatus::Accelerated && !announcedAtBoundary_[i]) {
            announcedAtBoundary_[i] = true;
            if (!st.humanReadableReason.empty()) {
                toasts_.push_back(ToastNotification{
                    filmTimeSeconds,
                    st.engine,
                    st.status,
                    std::string(toString(st.engine)) + " [" + toString(st.status) + "]: " + st.humanReadableReason});
            }
        }
    }
}

bool FallbackOrchestrator::syncPresetBeforeFrame(
    const QualityPresetSpec& spec,
    bool allowImmediateResize,
    double filmTimeSeconds) {
    if (!usingFallback_ && primaryEffect_) {
        bool ok = false;
        try {
            ok = primaryEffect_->syncPresetBeforeFrame(spec, allowImmediateResize);
        } catch (const std::exception& e) {
            BOO_LOG_WARN("Health", std::string("Primary effect threw during preset sync: ") + e.what());
            ok = false;
        }
        if (ok) {
            return true;
        }
        ++faultCount_;
        primaryFaulted_ = true;
        usingFallback_ = (fallbackEffect_ != nullptr);
        syncLiveEngineStatuses();
        toasts_.push_back(ToastNotification{
            filmTimeSeconds,
            EngineKind::RT,
            BackendStatus::Faulted,
            "Primary effect preset resize failed; switched seamlessly to safe fallback."});
    }

    if (fallbackEffect_) {
        try {
            return fallbackEffect_->syncPresetBeforeFrame(spec, allowImmediateResize);
        } catch (...) {
            return false;
        }
    }
    return false;
}

void FallbackOrchestrator::setHudTelemetry(const HudTelemetrySnapshot& snapshot) noexcept {
    if (primaryEffect_)  primaryEffect_->setHudTelemetry(snapshot);
    if (fallbackEffect_) fallbackEffect_->setHudTelemetry(snapshot);
}

void FallbackOrchestrator::setDisableRt(bool disable) noexcept {
    if (primaryEffect_) primaryEffect_->setDisableRt(disable);
    syncLiveEngineStatuses();
}

void FallbackOrchestrator::setDisableCuda(bool disable) noexcept {
    if (primaryEffect_) primaryEffect_->setDisableCuda(disable);
    syncLiveEngineStatuses();
}

void FallbackOrchestrator::setDisableTensor(bool disable) noexcept {
    if (primaryEffect_) primaryEffect_->setDisableTensor(disable);
    syncLiveEngineStatuses();
}

void FallbackOrchestrator::setDisableCodec(bool disable) noexcept {
    if (primaryEffect_) primaryEffect_->setDisableCodec(disable);
    syncLiveEngineStatuses();
}

void FallbackOrchestrator::setCodecSignalHijackActive(bool active) noexcept {
    if (primaryEffect_) primaryEffect_->setCodecSignalHijackActive(active);
    syncLiveEngineStatuses();
}

void FallbackOrchestrator::setAbCompareEngine(AbCompareEngine engine) noexcept {
    if (primaryEffect_) primaryEffect_->setAbCompareEngine(engine);
    syncLiveEngineStatuses();
}

void FallbackOrchestrator::resetTo(double filmTimeSeconds) {
    if (!usingFallback_ && primaryEffect_) {
        primaryEffect_->resetTo(filmTimeSeconds);
        syncLiveEngineStatuses();
    } else if (fallbackEffect_) {
        fallbackEffect_->resetTo(filmTimeSeconds);
    }
}

void FallbackOrchestrator::renderFrame(const FrameContext& ctx, bool injectRenderFaultThisFrame) {
    if (!usingFallback_ && primaryEffect_) {
        try {
            if (injectRenderFaultThisFrame) {
                throw std::runtime_error("Injected runtime render fault");
            }
            primaryEffect_->render(ctx);
            syncLiveEngineStatuses();
            return;
        } catch (const std::exception& e) {
            ++faultCount_;
            primaryFaulted_ = true;
            usingFallback_ = true;
            syncLiveEngineStatuses();
            BOO_LOG_WARN(
                "Health",
                "Effect '" + std::string(primaryEffect_->name()) + "' faulted at t=" +
                    std::to_string(ctx.filmTimeSeconds) + "s (" + e.what() + "); switching seamlessly to fallback.");
            toasts_.push_back(ToastNotification{
                ctx.filmTimeSeconds,
                EngineKind::RT,
                BackendStatus::Faulted,
                "Primary effect faulted; switched seamlessly to safe fallback."});
        }
    }

    if (fallbackEffect_) {
        try {
            fallbackEffect_->render(ctx);
        } catch (const std::exception& e) {
            ++faultCount_;
            BOO_LOG_ERROR("Health", std::string("Safe fallback effect threw during render: ") + e.what());
        }
    }
}

bool FallbackOrchestrator::captureClean(const FrameContext& ctx, const std::filesystem::path& directory, std::string* error) {
    auto* effect = usingFallback_ ? fallbackEffect_.get() : primaryEffect_.get();
    return effect && effect->captureClean(ctx, directory, error);
}

void FallbackOrchestrator::shutdown() noexcept {
    if (primaryEffect_) {
        primaryEffect_->shutdown();
        primaryEffect_.reset();
    }
    if (fallbackEffect_) {
        fallbackEffect_->shutdown();
        fallbackEffect_.reset();
    }
}

const HardwareResult& FallbackOrchestrator::engineStatus(EngineKind kind) const noexcept {
    return engineStates_[static_cast<size_t>(kind)];
}

void FallbackOrchestrator::updateEngineStatus(const HardwareResult& updated) {
    const size_t idx = static_cast<size_t>(updated.engine);
    if (idx < kEngineKindCount) {
        engineStates_[idx] = updated;
    }
}

bool FallbackOrchestrator::activeEffectOwnsEngineStatus(EngineKind kind) const noexcept {
    return primaryEffect_ && primaryEffect_->ownsLiveEngineStatus(kind);
}

bool FallbackOrchestrator::activeEffectUsesEmbeddedFallbackShader() const noexcept {
    if (usingFallback_) {
        return fallbackEffect_ ? fallbackEffect_->isUsingEmbeddedFallbackShader() : true;
    }
    return primaryEffect_ && primaryEffect_->isUsingEmbeddedFallbackShader();
}

const char* FallbackOrchestrator::activeEffectName() const noexcept {
    if (!usingFallback_ && primaryEffect_) return primaryEffect_->name();
    if (fallbackEffect_) return fallbackEffect_->name();
    return "None";
}

FrametimeStats computeFrametimeStats(const std::vector<double>& samplesMs) {
    FrametimeStats stats{};
    if (samplesMs.empty()) return stats;

    std::vector<double> sorted = samplesMs;
    std::sort(sorted.begin(), sorted.end());

    const double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
    stats.sampleCount = sorted.size();
    stats.meanMs = sum / static_cast<double>(sorted.size());
    stats.minMs = sorted.front();
    stats.maxMs = sorted.back();

    const auto percentileAt = [&](double p01) -> double {
        const double idx = p01 * static_cast<double>(sorted.size() - 1);
        const size_t lo  = static_cast<size_t>(std::floor(idx));
        const size_t hi  = static_cast<size_t>(std::ceil(idx));
        const double t   = idx - static_cast<double>(lo);
        return sorted[lo] * (1.0 - t) + sorted[hi] * t;
    };

    stats.p50Ms = percentileAt(0.50);
    stats.p95Ms = percentileAt(0.95);
    stats.p99Ms = percentileAt(0.99);
    return stats;
}

QualityLadder::QualityLadder(QualityPresetId initialPreset, size_t windowFrames)
    : currentPreset_(initialPreset), windowFrames_(std::max<size_t>(8, windowFrames)) {}

void QualityLadder::setPreset(QualityPresetId preset) noexcept {
    currentPreset_ = preset;
    consecutiveOverBudgetWindows_ = 0;
    consecutiveHeadroomWindows_ = 0;
}

void QualityLadder::stepDown() noexcept {
    if (currentPreset_ == QualityPresetId::Experimental4K) {
        currentPreset_ = QualityPresetId::Showcase;
        ++downgradeCount_;
    } else if (currentPreset_ == QualityPresetId::Showcase) {
        currentPreset_ = QualityPresetId::Balanced;
        ++downgradeCount_;
    } else if (currentPreset_ == QualityPresetId::Balanced) {
        currentPreset_ = QualityPresetId::Safe;
        ++downgradeCount_;
    }
}

void QualityLadder::stepUp() noexcept {
    // Do not auto-upgrade into manual 4K Experimental mode (Section 6.5)
    if (currentPreset_ == QualityPresetId::Safe) {
        currentPreset_ = QualityPresetId::Balanced;
        ++upgradeCount_;
    } else if (currentPreset_ == QualityPresetId::Balanced) {
        currentPreset_ = QualityPresetId::Showcase;
        ++upgradeCount_;
    }
}

void QualityLadder::pushFrameSample(
    double gpuFrameMs,
    uint64_t estimatedVramBytes,
    uint64_t softVramCapBytes,
    bool atSafeSceneCut) {
    // Section 6.5: Immediate shed on VRAM pressure regardless of FPS
    if (softVramCapBytes > 0 && estimatedVramBytes > softVramCapBytes) {
        memoryPressure_ = true;
        stepDown();
        consecutiveHeadroomWindows_ = 0;
        return;
    }
    memoryPressure_ = false;

    if (samplesMs_.size() >= windowFrames_) {
        samplesMs_.pop_front();
    }
    samplesMs_.push_back(gpuFrameMs);
    ++framesSinceEval_;

    if (samplesMs_.size() < windowFrames_ || framesSinceEval_ < windowFrames_) {
        return;
    }
    framesSinceEval_ = 0;

    const FrametimeStats stats = recentStats();
    // Section 6.5: p95 > ~18.0 ms for 2 consecutive windows -> degrade gradually
    if (stats.p95Ms > 18.0) {
        ++consecutiveOverBudgetWindows_;
        consecutiveHeadroomWindows_ = 0;
        if (consecutiveOverBudgetWindows_ >= 2) {
            stepDown();
            consecutiveOverBudgetWindows_ = 0;
        }
    } else if (stats.p95Ms < 11.5) {
        consecutiveOverBudgetWindows_ = 0;
        ++consecutiveHeadroomWindows_;
        if (consecutiveHeadroomWindows_ >= 3 && atSafeSceneCut) {
            stepUp();
            consecutiveHeadroomWindows_ = 0;
        }
    } else {
        consecutiveOverBudgetWindows_ = 0;
        consecutiveHeadroomWindows_ = 0;
    }
}

FrametimeStats QualityLadder::recentStats() const {
    return computeFrametimeStats(std::vector<double>(samplesMs_.begin(), samplesMs_.end()));
}

} // namespace boo
