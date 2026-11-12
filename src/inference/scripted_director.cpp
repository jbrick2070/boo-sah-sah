#include "boo/director.hpp"

#include <algorithm>
#include <cmath>

namespace boo {

namespace {
float clamp01(float x) noexcept {
    return std::clamp(x, 0.0f, 1.0f);
}

float hashFloat01(uint64_t seed, uint64_t step) noexcept {
    uint64_t z = seed + 0x9E3779B97F4A7C15ULL * (step + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return static_cast<float>((z >> 40) & 0xFFFFFFULL) / static_cast<float>(0x1000000ULL);
}
} // namespace

const char* toString(DirectorCategory cat) noexcept {
    switch (cat) {
        case DirectorCategory::Calm:       return "Calm";
        case DirectorCategory::Suspense:   return "Suspense";
        case DirectorCategory::Bloom:      return "Bloom";
        case DirectorCategory::Comic:      return "Comic";
        case DirectorCategory::Transition: return "Transition";
    }
    return "Calm";
}

void ScriptedDirector::reset(uint64_t seed) noexcept {
    seed_ = seed;
    evalCount_ = 0;
    lastCategoryChangeTime_ = -10.0;
    smoothedState_ = DirectorMoodState{};
    initialized_ = false;
}

DirectorFeatureVector ScriptedDirector::buildFeatures(
    const FrameContext& ctx,
    const SceneCue* activeCue) const noexcept {
    DirectorFeatureVector fv{};
    const float t = static_cast<float>(ctx.filmTimeSeconds);
    const float env = clamp01(ctx.audioEnvelope);
    const float prog = clamp01(ctx.actProgress);

    fv.values[0]  = env;
    fv.values[1]  = clamp01(0.5f + 0.5f * std::sin(t * 2.094395f)); // ~120 BPM beat proxy
    fv.values[2]  = clamp01(env * 0.8f + 0.2f * std::cos(t * 0.7f));
    fv.values[3]  = clamp01(0.4f + 0.4f * std::sin(t * 1.3f));
    fv.values[4]  = clamp01(0.3f + 0.5f * std::sin(t * 3.1f));
    fv.values[5]  = prog;
    fv.values[6]  = clamp01(static_cast<float>(ctx.actIndex) / 6.0f);
    fv.values[7]  = (ctx.edition == Edition::Midnight) ? 1.0f : 0.0f;
    fv.values[8]  = (activeCue && !activeCue->revealId.empty()) ? 1.0f : 0.0f;
    fv.values[9]  = (prog > 0.92f || prog < 0.05f) ? 1.0f : 0.0f;
    fv.values[10] = smoothedState_.bloomStrength;
    fv.values[11] = smoothedState_.irisAngle;
    fv.values[12] = smoothedState_.lightTension;
    fv.values[13] = smoothedState_.spectralShift;
    fv.values[14] = smoothedState_.cutAccent;
    fv.values[15] = hashFloat01(seed_, static_cast<uint64_t>(ctx.samplePosition / 24000));
    return fv;
}

DirectorMoodState ScriptedDirector::evaluate(
    const FrameContext& ctx,
    const SceneCue* activeCue) noexcept {
    ++evalCount_;
    const DirectorFeatureVector fv = buildFeatures(ctx, activeCue);

    DirectorMoodState target{};
    target.isScriptedFallback = true;
    target.confidence = 1.0f;

    // Deterministic act-aware mood choreography honoring SPEC_v4 Section 2.2 & 4.2.3
    const float prog = fv.values[5];
    const float beat = fv.values[1];
    const float editionBias = (ctx.edition == Edition::Midnight) ? 0.08f : 0.0f;

    if (prog > 0.93f) {
        target.category = DirectorCategory::Transition;
    } else {
        switch (ctx.actIndex % 7) {
            case 0: // Taipei Intro -> Opening Reveal
                target.category = (prog > 0.75f) ? DirectorCategory::Bloom : DirectorCategory::Suspense;
                break;
            case 1: // Mirror Intelligence
                target.category = DirectorCategory::Suspense;
                break;
            case 2: // Neural Velvet
                target.category = DirectorCategory::Calm;
                break;
            case 3: // Madame N: Suspense dissolves into Comic/Calm lounge at midpoint (~02:55)
                target.category = (prog < 0.55f) ? DirectorCategory::Suspense : DirectorCategory::Comic;
                break;
            case 4: // Particle Mutiny
                target.category = DirectorCategory::Bloom;
                break;
            case 5: // Signal Hijack
                target.category = DirectorCategory::Suspense;
                break;
            default: // Silicon Rebellion Finale
                target.category = DirectorCategory::Bloom;
                break;
        }
    }

    switch (target.category) {
        case DirectorCategory::Calm:
            target.bloomStrength = 0.35f + 0.10f * beat;
            target.irisAngle     = 0.55f;
            target.lightTension  = 0.20f + editionBias;
            target.spectralShift = 0.22f;
            target.cutAccent     = 0.05f;
            break;
        case DirectorCategory::Suspense:
            target.bloomStrength = 0.28f + 0.12f * fv.values[0];
            target.irisAngle     = 0.32f;
            target.lightTension  = 0.74f + editionBias;
            target.spectralShift = 0.40f;
            target.cutAccent     = 0.22f * beat;
            break;
        case DirectorCategory::Bloom:
            target.bloomStrength = 0.78f + 0.15f * beat;
            target.irisAngle     = 0.82f;
            target.lightTension  = 0.42f;
            target.spectralShift = 0.68f;
            target.cutAccent     = 0.35f * beat;
            break;
        case DirectorCategory::Comic:
            target.bloomStrength = 0.48f;
            target.irisAngle     = 0.68f;
            target.lightTension  = 0.18f;
            target.spectralShift = 0.30f;
            target.cutAccent     = 0.15f * beat;
            break;
        case DirectorCategory::Transition:
            target.bloomStrength = 0.60f;
            target.irisAngle     = 0.50f;
            target.lightTension  = 0.50f;
            target.spectralShift = 0.55f;
            target.cutAccent     = 0.70f;
            break;
    }

    if (activeCue && !activeCue->allowDirectorModulation) {
        // Section 7.3: Respect cue metadata disabling director modulation
        target.bloomStrength = 0.40f;
        target.irisAngle = 0.50f;
        target.lightTension = 0.35f;
        target.spectralShift = 0.20f;
        target.cutAccent = 0.0f;
    }

    if (!initialized_ || ctx.seeking) {
        smoothedState_ = target;
        smoothedState_.bloomStrength = clamp01(smoothedState_.bloomStrength);
        smoothedState_.irisAngle     = clamp01(smoothedState_.irisAngle);
        smoothedState_.lightTension  = clamp01(smoothedState_.lightTension);
        smoothedState_.spectralShift = clamp01(smoothedState_.spectralShift);
        smoothedState_.cutAccent     = clamp01(smoothedState_.cutAccent);
        lastCategoryChangeTime_ = ctx.filmTimeSeconds;
        initialized_ = true;
        return smoothedState_;
    }

    // Hysteresis: enforce minimum state duration to prevent category flicker (Section 4.2.3)
    if (target.category != smoothedState_.category &&
        std::abs(ctx.filmTimeSeconds - lastCategoryChangeTime_) >= kMinStateHoldSeconds) {
        smoothedState_.category = target.category;
        lastCategoryChangeTime_ = ctx.filmTimeSeconds;
    }

    const float dt = static_cast<float>(std::clamp(ctx.dtSeconds, 0.0, 0.1));
    const float alpha = clamp01(1.0f - std::exp(-6.0f * (dt > 0.0f ? dt : (1.0f / 60.0f))));
    const auto lerpClamped = [&](float cur, float tgt) {
        return clamp01(cur + (tgt - cur) * alpha);
    };

    smoothedState_.bloomStrength = lerpClamped(smoothedState_.bloomStrength, target.bloomStrength);
    smoothedState_.irisAngle     = lerpClamped(smoothedState_.irisAngle, target.irisAngle);
    smoothedState_.lightTension  = lerpClamped(smoothedState_.lightTension, target.lightTension);
    smoothedState_.spectralShift = lerpClamped(smoothedState_.spectralShift, target.spectralShift);
    smoothedState_.cutAccent     = lerpClamped(smoothedState_.cutAccent, target.cutAccent);
    smoothedState_.confidence    = 1.0f;
    smoothedState_.isScriptedFallback = true;
    return smoothedState_;
}

} // namespace boo
