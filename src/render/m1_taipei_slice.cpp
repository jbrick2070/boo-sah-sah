#include "boo/render.hpp"
#include "boo/cues.hpp"
#include "boo/logging.hpp"
#include "boo/m1_scene.hpp"
#include "boo/trilogy_renderer.hpp"
#include "boo/ui_text.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace boo {

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kNumTlasInstances = 112;
constexpr uint32_t kNumBlasMeshes    = 5;
constexpr uint32_t kNumBloomMips     = 3;
constexpr uint32_t kFrameSlots       = D3D12Context::kFrameCount;
constexpr uint32_t kHeadlessSlot     = kFrameSlots;
constexpr uint32_t kTotalUploadSlots = kFrameSlots + 1u;
constexpr float    kPi               = 3.14159265358979323846f;
constexpr float    kTwoPi            = 6.28318530717958647692f;

struct alignas(16) GpuM1Vertex {
    float posAndU[4]{};  // xyz = object position, w = surface u in [0,1]
    float normAndV[4]{}; // xyz = object smooth normal, w = surface v in [0,1]
};
static_assert(sizeof(GpuM1Vertex) == 32, "GpuM1Vertex must be 32 bytes");

struct MeshSlice {
    uint32_t baseIndex = 0;
    uint32_t indexCount = 0;
};

struct alignas(16) GpuM1SceneConstants {
    float camEye[3]{};
    float fovY = 0.92f;
    float camRight[3]{};
    float aspect = 16.0f / 9.0f;
    float camUp[3]{};
    float filmTime = 0.0f;
    float camForward[3]{};
    float revealOpen01 = 0.0f;
    uint32_t canvasWidth = 1920;
    uint32_t canvasHeight = 1080;
    uint32_t expectSeq = 0;
    uint32_t sceneFlags = 0;
    float emissivePulse = 0.0f;
    float moodIntensity = 0.5f;
    uint32_t shotIndex = 0;
    uint32_t deterministicSeed = 0;
    uint32_t meshBaseIndices[4]{0, 0, 0, 0};
};
static_assert(sizeof(GpuM1SceneConstants) == 112, "GpuM1SceneConstants must be 28 DWORDs (112 bytes)");

struct alignas(16) GpuM1PostConstants {
    float invSize[2]{};
    float bloomThreshold = 1.35f;
    float scaleOrVignette = 1.0f;
    uint32_t isFirstDownsample = 0;
    uint32_t ditherSeed = 0;
    float preExposure = 0.92f;
    float bloomIntensity = 0.22f;
};
static_assert(sizeof(GpuM1PostConstants) == 32, "GpuM1PostConstants must be 8 DWORDs (32 bytes)");

struct alignas(16) GpuM1TensorSubConstants {
    float filmTime = 0.0f;
    float revealOpen01 = 0.0f;
    uint32_t midnight = 0;
    uint32_t seed = 0;
};
static_assert(sizeof(GpuM1TensorSubConstants) == 16, "GpuM1TensorSubConstants must be 4 DWORDs (16 bytes)");

struct alignas(16) GpuM1PresentConstants {
    float swapchainAndCanvas[4]{};
    float letterboxRect[4]{};
    float filmTime = 0.0f;
    float duration = 60.0f;
    float actProgress = 0.0f;
    float fps = 60.0f;

    float gpuMs = 0.5f;
    float p95Ms = 1.0f;
    float vramUsedMb = 128.0f;
    float vramBudgetMb = 12288.0f;

    uint32_t hudFlags = 1u;
    uint32_t enginePacked = 0u;
    uint32_t swapchainBits = 8u;
    float benchProgress = 0.0f;

    float audioBands[4]{};
    float benchPrimary[4]{};
    float benchFill[4]{};
    float recentGpuMs[16]{};
    float volumeAndPadding[4]{-1.0f, 0, 0, 0};

    float livePillarMetrics[4][4]{};
    uint32_t livePillarMeta[4]{0, 0, 0, 0};

    uint32_t captionPacked[16]{};
    float captionStyle[4]{};

    float interactiveState[4]{0.5f, 0.52f, 0.42f, 1.0f}; // x=abSplitRatio, y=sunElev01, z=sunAzim01, w=accumSpp
    uint32_t interactiveMeta[4]{0, 0, 0, 0};             // x: bit0=explore, bit1=sunOvr, bit2=lampsOff, bit3=nrd, bits4..7=preset
};
static_assert(sizeof(GpuM1PresentConstants) == 400, "GpuM1PresentConstants must be 100 DWORDs (400 bytes)");

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 operator+(const Vec3& a, const Vec3& b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3& a, const Vec3& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3& a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }

float dotV(const Vec3& a, const Vec3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 crossV(const Vec3& a, const Vec3& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 normV(const Vec3& v) noexcept {
    const float lenSq = dotV(v, v);
    if (lenSq <= 1e-12f) return {0.0f, 1.0f, 0.0f};
    const float inv = 1.0f / std::sqrt(lenSq);
    return {v.x * inv, v.y * inv, v.z * inv};
}

float smoothstepF(float e0, float e1, float x) noexcept {
    const float t = std::clamp((x - e0) / std::max(e1 - e0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool readShaderCso(const char* relPath, std::vector<uint8_t>& outBytes) {
    const auto resolved = resolveResourcePath(relPath);
    std::ifstream ifs(resolved, std::ios::binary | std::ios::ate);
    if (!ifs) return false;
    const auto sz = ifs.tellg();
    if (sz <= 0) return false;
    outBytes.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    return static_cast<bool>(ifs.read(reinterpret_cast<char*>(outBytes.data()), sz));
}

void normalize3(float v[3]) noexcept {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-6f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

void cross3(const float a[3], const float b[3], float out[3]) noexcept {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

void buildCameraBasis(const M1CameraState& cam, float outFwd[3], float outRight[3], float outUp[3]) noexcept {
    outFwd[0] = cam.target[0] - cam.eye[0];
    outFwd[1] = cam.target[1] - cam.eye[1];
    outFwd[2] = cam.target[2] - cam.eye[2];
    normalize3(outFwd);

    // Left-handed camera basis matching M0 benchmark & HLSL conventions: right = up x forward
    cross3(cam.up, outFwd, outRight);
    normalize3(outRight);
    cross3(outFwd, outRight, outUp);
    normalize3(outUp);
}

constexpr uint32_t packInstanceMetadata(uint32_t meshId, uint32_t matKind, uint32_t subIdx = 0u) noexcept {
    return (meshId & 7u) | ((matKind & 15u) << 3u) | ((subIdx & 255u) << 7u);
}

GpuM1Vertex makeVert(const Vec3& p, const Vec3& n, float u, float v) noexcept {
    GpuM1Vertex out{};
    out.posAndU[0] = p.x;
    out.posAndU[1] = p.y;
    out.posAndU[2] = p.z;
    out.posAndU[3] = u;
    out.normAndV[0] = n.x;
    out.normAndV[1] = n.y;
    out.normAndV[2] = n.z;
    out.normAndV[3] = v;
    return out;
}

// Mesh 0: Watertight chamfered/beveled architectural prism in [-1, 1]^3 (smooth bevel edge speculars + face UVs)
void appendBeveledBoxMesh(std::vector<GpuM1Vertex>& verts, std::vector<uint32_t>& indices, MeshSlice& outSlice) {
    outSlice.baseIndex = static_cast<uint32_t>(indices.size());
    constexpr float kGrid[4] = {-1.0f, -0.92f, 0.92f, 1.0f};
    constexpr float kBevelR  = 0.08f;
    constexpr float kInner   = 1.0f - kBevelR;

    struct FaceBasis {
        Vec3 normal;
        Vec3 uAxis;
        Vec3 vAxis;
    };
    const FaceBasis faces[6] = {
        {{ 0.0f,  0.0f, -1.0f}, { 1.0f, 0.0f,  0.0f}, {0.0f, 1.0f,  0.0f}}, // -Z front
        {{ 0.0f,  0.0f,  1.0f}, {-1.0f, 0.0f,  0.0f}, {0.0f, 1.0f,  0.0f}}, // +Z back
        {{-1.0f,  0.0f,  0.0f}, { 0.0f, 0.0f, -1.0f}, {0.0f, 1.0f,  0.0f}}, // -X left
        {{ 1.0f,  0.0f,  0.0f}, { 0.0f, 0.0f,  1.0f}, {0.0f, 1.0f,  0.0f}}, // +X right
        {{ 0.0f,  1.0f,  0.0f}, { 1.0f, 0.0f,  0.0f}, {0.0f, 0.0f, -1.0f}}, // +Y top
        {{ 0.0f, -1.0f,  0.0f}, { 1.0f, 0.0f,  0.0f}, {0.0f, 0.0f,  1.0f}}, // -Y bottom
    };

    for (const auto& f : faces) {
        const uint32_t baseV = static_cast<uint32_t>(verts.size());
        for (uint32_t iy = 0; iy < 4; ++iy) {
            for (uint32_t ix = 0; ix < 4; ++ix) {
                const float su = kGrid[ix];
                const float sv = kGrid[iy];
                const Vec3 q = f.normal + f.uAxis * su + f.vAxis * sv;
                const Vec3 clamped{
                    std::clamp(q.x, -kInner, kInner),
                    std::clamp(q.y, -kInner, kInner),
                    std::clamp(q.z, -kInner, kInner)};
                const Vec3 diff = q - clamped;
                const float dLen = std::sqrt(dotV(diff, diff));
                const Vec3 n = (dLen > 1e-5f) ? (diff * (1.0f / dLen)) : f.normal;
                const Vec3 p = (dLen > 1e-5f) ? (clamped + n * kBevelR) : q;
                verts.push_back(makeVert(p, n, 0.5f * (su + 1.0f), 0.5f * (sv + 1.0f)));
            }
        }
        for (uint32_t iy = 0; iy < 3; ++iy) {
            for (uint32_t ix = 0; ix < 3; ++ix) {
                const uint32_t i0 = baseV + iy * 4u + ix;
                const uint32_t i1 = i0 + 1u;
                const uint32_t i2 = i0 + 4u;
                const uint32_t i3 = i2 + 1u;
                indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
                indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
            }
        }
    }
    outSlice.indexCount = static_cast<uint32_t>(indices.size()) - outSlice.baseIndex;
}

Vec3 evalOrchidPetalMidSurface(float u, float v, bool isLabellum, float& outHalfThick) noexcept {
    const float sinPiV = std::sin(v * kPi);
    if (!isLabellum) {
        // Sculpted clawed elliptic-ovate Phalaenopsis petal/sepal blade:
        // Slender clawed base (v < 0.24) creates crisp negative-space notches between individual petals!
        const float clawPinch = smoothstepF(0.0f, 0.24f, v);
        const float env = std::pow(std::max(std::sin(kPi * std::pow(std::max(v, 1e-4f), 0.92f)), 0.0f), 0.88f);
        const float tipTaper = 1.0f - 0.95f * smoothstepF(0.86f, 1.0f, v);
        const float halfW = (0.022f + 0.295f * env * (0.28f + 0.72f * clawPinch)) * tipTaper;
        const float x = u * halfW;
        const float y = v;
        const float zArch = 0.135f * std::sin(v * kPi * 0.92f) - 0.105f * v * v * v;
        const float zCup  = 0.088f * (u * u) * sinPiV;
        const float zKeel = -0.022f * std::exp(-18.0f * u * u) * (1.0f - 0.40f * v);
        const float zWave = 0.018f * std::pow(std::abs(u), 2.2f) * std::sin(v * 4.5f * kPi) * sinPiV;
        outHalfThick = (0.028f * (1.0f - 0.58f * v) + 0.007f) *
                       (1.0f - 0.72f * u * u) *
                       (1.0f - 0.62f * v * v * v);
        return {x, y, zArch + zCup + zKeel + zWave};
    }

    // Sculpted 3-lobed cupped Orchid Labellum (upcurved lateral throat lobes + flared bilobed lip)
    const float throatFactor = 1.0f - smoothstepF(0.36f, 0.72f, v);
    const float lipFactor    = smoothstepF(0.36f, 0.80f, v);
    const float span = (0.25f + 0.35f * sinPiV) * throatFactor +
                       (0.52f * (1.0f - 0.52f * smoothstepF(0.88f, 1.0f, v))) * lipFactor;
    const float thetaMax = 1.18f * throatFactor + 0.36f * lipFactor;
    const float sinMax   = std::max(std::sin(thetaMax), 0.25f);
    const float theta    = u * thetaMax;

    const float x = span * std::sin(theta) / sinMax;
    const float cleftNotch = 0.068f * smoothstepF(0.76f, 1.0f, v) * std::exp(-14.0f * u * u);
    const float y = v - cleftNotch;

    const float zThroatCup = span * (1.0f - std::cos(theta)) / sinMax * (0.92f * throatFactor + 0.32f * lipFactor);
    const float zLipDrop   = -0.185f * smoothstepF(0.48f, 1.0f, v) + 0.075f * sinPiV;
    const float callusDist = std::abs(u) - 0.20f;
    const float zCallus    = 0.024f * std::exp(-28.0f * callusDist * callusDist) * throatFactor;
    const float zRuffle    = 0.022f * std::pow(std::abs(u), 2.4f) * std::sin(v * 5.5f * kPi) * lipFactor;

    outHalfThick = (0.032f * (1.0f - 0.52f * v) + 0.008f) *
                   (1.0f - 0.68f * u * u) *
                   (1.0f - 0.55f * v * v * v);
    return {x, y, zThroatCup + zLipDrop + zCallus + zRuffle};
}

Vec3 evalMidNormal(float u, float v, bool isLabellum) noexcept {
    constexpr float eps = 0.008f;
    float dummy = 0.0f;
    const Vec3 pu0 = evalOrchidPetalMidSurface(std::max(-1.0f, u - eps), v, isLabellum, dummy);
    const Vec3 pu1 = evalOrchidPetalMidSurface(std::min( 1.0f, u + eps), v, isLabellum, dummy);
    const Vec3 pv0 = evalOrchidPetalMidSurface(u, std::max(0.0f, v - eps), isLabellum, dummy);
    const Vec3 pv1 = evalOrchidPetalMidSurface(u, std::min(1.0f, v + eps), isLabellum, dummy);
    Vec3 n = normV(crossV(pu1 - pu0, pv1 - pv0));
    if (n.z < 0.0f && v < 0.85f && std::abs(u) < 0.3f) {
        n = n * -1.0f;
    }
    return n;
}

Vec3 evalDisplacedShellPos(float u, float v, bool isLabellum, float signSide) noexcept {
    float halfT = 0.0f;
    const Vec3 mid = evalOrchidPetalMidSurface(u, v, isLabellum, halfT);
    const Vec3 nMid = evalMidNormal(u, v, isLabellum);
    return mid + nMid * (halfT * signSide);
}

// Mesh 1 & Mesh 2: Closed thin-shell botanical crystal petal / 3-lobed labellum with smooth shell & rim bevel normals
void appendThinShellPetalMesh(
    std::vector<GpuM1Vertex>& verts,
    std::vector<uint32_t>& indices,
    uint32_t nu,
    uint32_t nv,
    bool isLabellum,
    MeshSlice& outSlice) {
    outSlice.baseIndex = static_cast<uint32_t>(indices.size());

    auto buildSide = [&](float signSide) {
        const uint32_t base = static_cast<uint32_t>(verts.size());
        for (uint32_t iv = 0; iv < nv; ++iv) {
            const float v = static_cast<float>(iv) / static_cast<float>(nv - 1u);
            for (uint32_t iu = 0; iu < nu; ++iu) {
                const float u01 = static_cast<float>(iu) / static_cast<float>(nu - 1u);
                const float u = u01 * 2.0f - 1.0f;
                const Vec3 pos = evalDisplacedShellPos(u, v, isLabellum, signSide);

                constexpr float eps = 0.012f;
                const Vec3 du = evalDisplacedShellPos(std::min(1.0f, u + eps), v, isLabellum, signSide) -
                                evalDisplacedShellPos(std::max(-1.0f, u - eps), v, isLabellum, signSide);
                const Vec3 dv = evalDisplacedShellPos(u, std::min(1.0f, v + eps), isLabellum, signSide) -
                                evalDisplacedShellPos(u, std::max(0.0f, v - eps), isLabellum, signSide);
                Vec3 n = normV(crossV(du, dv) * signSide);
                // Blend subtle lateral edge roundness along perimeter so grazing Fresnel thin-film catches the rim
                if (iu == 0u || iu == nu - 1u) {
                    const Vec3 lat = normV(Vec3{u, 0.0f, 0.0f});
                    n = normV(n * 0.72f + lat * 0.68f);
                }
                verts.push_back(makeVert(pos, n, u01, v));
            }
        }
        for (uint32_t iv = 0; iv + 1u < nv; ++iv) {
            for (uint32_t iu = 0; iu + 1u < nu; ++iu) {
                const uint32_t i0 = base + iv * nu + iu;
                const uint32_t i1 = i0 + 1u;
                const uint32_t i2 = i0 + nu;
                const uint32_t i3 = i2 + 1u;
                if (signSide > 0.0f) {
                    indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
                    indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
                } else {
                    indices.push_back(i0); indices.push_back(i1); indices.push_back(i2);
                    indices.push_back(i1); indices.push_back(i3); indices.push_back(i2);
                }
            }
        }
        return base;
    };

    const uint32_t frontBase = buildSide(+1.0f);
    const uint32_t backBase  = buildSide(-1.0f);

    // Stitch closed watertight perimeter loop around (iv=0 -> iu=nu-1 -> iv=nv-1 -> iu=0)
    std::vector<uint32_t> loopGridIdx;
    loopGridIdx.reserve((nu + nv) * 2u);
    for (uint32_t iu = 0; iu < nu; ++iu) loopGridIdx.push_back(0u * nu + iu);
    for (uint32_t iv = 1; iv < nv; ++iv) loopGridIdx.push_back(iv * nu + (nu - 1u));
    for (uint32_t iu = 1; iu < nu; ++iu) loopGridIdx.push_back((nv - 1u) * nu + (nu - 1u - iu));
    for (uint32_t iv = 1; iv + 1u < nv; ++iv) loopGridIdx.push_back((nv - 1u - iv) * nu + 0u);

    const size_t loopLen = loopGridIdx.size();
    for (size_t k = 0; k < loopLen; ++k) {
        const uint32_t g0 = loopGridIdx[k];
        const uint32_t g1 = loopGridIdx[(k + 1u) % loopLen];
        const uint32_t f0 = frontBase + g0;
        const uint32_t f1 = frontBase + g1;
        const uint32_t b0 = backBase + g0;
        const uint32_t b1 = backBase + g1;
        indices.push_back(f0); indices.push_back(f1); indices.push_back(b0);
        indices.push_back(f1); indices.push_back(b1); indices.push_back(b0);
    }

    outSlice.indexCount = static_cast<uint32_t>(indices.size()) - outSlice.baseIndex;
}

// Mesh 3: Tapered fluted botanical/crystal spire along local +Y in [0, 1] (for pedicel, column, core & gold stamens)
void appendBotanicalSpireMesh(std::vector<GpuM1Vertex>& verts, std::vector<uint32_t>& indices, MeshSlice& outSlice) {
    outSlice.baseIndex = static_cast<uint32_t>(indices.size());
    constexpr uint32_t kRadial = 12;
    constexpr uint32_t kRings  = 9;

    auto evalRadius = [](float v, float ang) noexcept {
        const float baseRound = smoothstepF(0.0f, 0.07f, v + 0.03f);
        const float tipTaper  = 1.0f - 0.84f * smoothstepF(0.82f, 1.0f, v);
        const float flute     = 1.0f + 0.065f * std::cos(6.0f * ang);
        return (0.94f * (1.0f - 0.36f * v * v) * baseRound * tipTaper) * flute;
    };

    const uint32_t ringBase = static_cast<uint32_t>(verts.size());
    for (uint32_t ir = 0; ir < kRings; ++ir) {
        const float v = static_cast<float>(ir) / static_cast<float>(kRings - 1u);
        for (uint32_t it = 0; it <= kRadial; ++it) {
            const float u01 = static_cast<float>(it) / static_cast<float>(kRadial);
            const float ang = u01 * kTwoPi;
            const float r   = evalRadius(v, ang);
            const float rNext = evalRadius(std::min(1.0f, v + 0.02f), ang);
            const float drdv  = (rNext - r) / 0.02f;
            const Vec3 pos{std::cos(ang) * r, v, std::sin(ang) * r};
            const Vec3 n = normV(Vec3{std::cos(ang), -drdv, std::sin(ang)});
            verts.push_back(makeVert(pos, n, u01, v));
        }
    }

    const uint32_t stride = kRadial + 1u;
    for (uint32_t ir = 0; ir + 1u < kRings; ++ir) {
        for (uint32_t it = 0; it < kRadial; ++it) {
            const uint32_t i0 = ringBase + ir * stride + it;
            const uint32_t i1 = i0 + 1u;
            const uint32_t i2 = i0 + stride;
            const uint32_t i3 = i2 + 1u;
            indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
            indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
        }
    }

    // Bottom & apex caps
    const uint32_t botCenter = static_cast<uint32_t>(verts.size());
    verts.push_back(makeVert({0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 0.5f, 0.0f));
    const uint32_t topCenter = static_cast<uint32_t>(verts.size());
    verts.push_back(makeVert({0.0f, 1.015f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.5f, 1.0f));

    for (uint32_t it = 0; it < kRadial; ++it) {
        indices.push_back(botCenter);
        indices.push_back(ringBase + it);
        indices.push_back(ringBase + it + 1u);

        const uint32_t topRing = ringBase + (kRings - 1u) * stride;
        indices.push_back(topRing + it);
        indices.push_back(topCenter);
        indices.push_back(topRing + it + 1u);
    }

    outSlice.indexCount = static_cast<uint32_t>(indices.size()) - outSlice.baseIndex;
}

// Mesh 4: Watertight Taipei 101 Double-Notched 7-deg Outward-Canted Ruyi / Bamboo Tier in y in [0, 1]
void appendTaipei101BambooModuleMesh(std::vector<GpuM1Vertex>& verts, std::vector<uint32_t>& indices, MeshSlice& outSlice) {
    outSlice.baseIndex = static_cast<uint32_t>(indices.size());

    // 16-vertex symmetric W-notched Taipei 101 octagram perimeter in [-1, 1]^2 (CCW around +Y)
    constexpr float kN = 0.76f; // Corner setback begins at 76% of half-width
    const float perim[16][2] = {
        {-kN, -1.0f}, { kN, -1.0f}, { kN,  -kN}, { 1.0f,  -kN}, // South (-Z) face + SE notch
        { 1.0f,  kN}, { kN,   kN}, { kN,  1.0f}, {-kN,  1.0f}, // East (+X) face + NE notch + North (+Z)
        {-kN,   kN}, {-1.0f,  kN}, {-1.0f, -kN}, {-kN,  -kN}, // NW notch + West (-X) face + SW notch
        {-kN, -1.0f}, { kN, -1.0f}, { kN,  -kN}, { 1.0f,  -kN}  // Unused pad; we iterate 12 clean segments!
    };
    (void)perim;
    const float pts[12][2] = {
        {-kN, -1.0f}, { kN, -1.0f}, // 0->1: South main curtain wall
        { kN,  -kN},  { 1.0f, -kN}, // 1->2->3: SE ruyi corner notch
        { 1.0f,  kN}, { kN,   kN},  // 3->4->5: East main wall + NE notch
        { kN,  1.0f}, {-kN,  1.0f}, // 5->6->7: North main wall
        {-kN,   kN},  {-1.0f,  kN}, // 7->8->9: NW notch
        {-1.0f, -kN}, {-kN,  -kN}   // 9->10->11->0: West main wall + SW notch
    };

    constexpr float kBotScale = 0.848f; // ~7-deg outward cant over 2.0m module height
    constexpr float kSlopeY   = -(1.0f - kBotScale);

    // 12 crisp faceted curtain-wall / notch panels so reflections & mullions read with architectural precision
    for (uint32_t seg = 0; seg < 12u; ++seg) {
        const uint32_t next = (seg + 1u) % 12u;
        const float x0 = pts[seg][0],  z0 = pts[seg][1];
        const float x1 = pts[next][0], z1 = pts[next][1];
        const Vec3 edge{x1 - x0, 0.0f, z1 - z0};
        const Vec3 out2d = normV(Vec3{edge.z, 0.0f, -edge.x}); // Outward 2D normal for (-Z -> +X -> +Z -> -X) loop
        const Vec3 faceN = normV(Vec3{out2d.x, kSlopeY, out2d.z});

        const float u0 = (seg % 3u == 0u) ? 0.08f : 0.0f;
        const float u1 = (seg % 3u == 0u) ? 0.92f : 0.48f;
        const uint32_t b = static_cast<uint32_t>(verts.size());
        verts.push_back(makeVert({x0 * kBotScale, 0.0f, z0 * kBotScale}, faceN, u0, 0.0f));
        verts.push_back(makeVert({x1 * kBotScale, 0.0f, z1 * kBotScale}, faceN, u1, 0.0f));
        verts.push_back(makeVert({x1,             1.0f, z1            }, faceN, u1, 1.0f));
        verts.push_back(makeVert({x0,             1.0f, z0            }, faceN, u0, 1.0f));
        indices.push_back(b + 0); indices.push_back(b + 1); indices.push_back(b + 2);
        indices.push_back(b + 0); indices.push_back(b + 2); indices.push_back(b + 3);
    }

    // Watertight top & bottom terrace caps (triangle fan around center)
    for (int cap = 0; cap < 2; ++cap) {
        const float y = (cap == 0) ? 0.0f : 1.0f;
        const float sc = (cap == 0) ? kBotScale : 1.0f;
        const Vec3 n{0.0f, (cap == 0) ? -1.0f : 1.0f, 0.0f};
        const uint32_t cIdx = static_cast<uint32_t>(verts.size());
        verts.push_back(makeVert({0.0f, y, 0.0f}, n, 0.5f, (cap == 0) ? 0.0f : 1.0f));
        for (uint32_t i = 0; i < 12u; ++i) {
            verts.push_back(makeVert({pts[i][0] * sc, y, pts[i][1] * sc}, n, 0.5f, (cap == 0) ? 0.0f : 1.0f));
        }
        for (uint32_t i = 0; i < 12u; ++i) {
            const uint32_t iNext = (i + 1u) % 12u;
            indices.push_back(cIdx);
            indices.push_back(cIdx + 1u + ((cap == 0) ? iNext : i));
            indices.push_back(cIdx + 1u + ((cap == 0) ? i : iNext));
        }
    }

    outSlice.indexCount = static_cast<uint32_t>(indices.size()) - outSlice.baseIndex;
}

void buildProceduralM1Meshes(
    std::vector<GpuM1Vertex>& outVerts,
    std::vector<uint32_t>& outIndices,
    std::array<MeshSlice, kNumBlasMeshes>& outSlices) {
    outVerts.clear();
    outIndices.clear();
    outVerts.reserve(1800);
    outIndices.reserve(8400);

    appendBeveledBoxMesh(outVerts, outIndices, outSlices[0]);
    appendThinShellPetalMesh(outVerts, outIndices, 13u, 17u, false, outSlices[1]);
    appendThinShellPetalMesh(outVerts, outIndices, 17u, 17u, true,  outSlices[2]);
    appendBotanicalSpireMesh(outVerts, outIndices, outSlices[3]);
    appendTaipei101BambooModuleMesh(outVerts, outIndices, outSlices[4]);
}

void fillInstanceTransformAxisAligned(
    D3D12_RAYTRACING_INSTANCE_DESC& d,
    D3D12_GPU_VIRTUAL_ADDRESS blasVa,
    float cx, float cy, float cz,
    float sx, float sy, float sz,
    uint32_t packedId) noexcept {
    std::memset(&d, 0, sizeof(d));
    d.Transform[0][0] = sx; d.Transform[0][3] = cx;
    d.Transform[1][1] = sy; d.Transform[1][3] = cy;
    d.Transform[2][2] = sz; d.Transform[2][3] = cz;
    d.InstanceID = packedId;
    d.InstanceMask = 0xFFu;
    d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    d.AccelerationStructure = blasVa;
}

void fillInstanceBasisAndOrigin(
    D3D12_RAYTRACING_INSTANCE_DESC& d,
    D3D12_GPU_VIRTUAL_ADDRESS blasVa,
    const Vec3& colX,
    const Vec3& colY,
    const Vec3& colZ,
    const Vec3& origin,
    uint32_t packedId) noexcept {
    std::memset(&d, 0, sizeof(d));
    d.Transform[0][0] = colX.x; d.Transform[0][1] = colY.x; d.Transform[0][2] = colZ.x; d.Transform[0][3] = origin.x;
    d.Transform[1][0] = colX.y; d.Transform[1][1] = colY.y; d.Transform[1][2] = colZ.y; d.Transform[1][3] = origin.y;
    d.Transform[2][0] = colX.z; d.Transform[2][1] = colY.z; d.Transform[2][2] = colZ.z; d.Transform[2][3] = origin.z;
    d.InstanceID = packedId;
    d.InstanceMask = 0xFFu;
    d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
    d.AccelerationStructure = blasVa;
}

void fillInstanceSegmentAB(
    D3D12_RAYTRACING_INSTANCE_DESC& d,
    D3D12_GPU_VIRTUAL_ADDRESS blasVa,
    const Vec3& a,
    const Vec3& b,
    float radX,
    float radZ,
    uint32_t packedId) noexcept {
    const Vec3 axisY = b - a;
    const Vec3 dirY  = normV(axisY);
    const Vec3 refUp = (std::abs(dirY.y) < 0.92f) ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 dirX  = normV(crossV(refUp, dirY));
    const Vec3 dirZ  = normV(crossV(dirX, dirY));
    fillInstanceBasisAndOrigin(d, blasVa, dirX * radX, axisY, dirZ * radZ, a, packedId);
}

// Places a botanical petal/sepal/labellum pivoting cleanly from claw `pivot`:
// rollRad rotates radial opening direction around flower -Z forward axis;
// openRad unfurls petal from closed forward bud (0.2 rad) to full open corolla (~1.3 rad).
void fillInstanceBotanicalPetal(
    D3D12_RAYTRACING_INSTANCE_DESC& d,
    D3D12_GPU_VIRTUAL_ADDRESS blasVa,
    const Vec3& pivot,
    float rollRad,
    float openRad,
    float twistRad,
    float length,
    float widthScale,
    float cupScale,
    uint32_t packedId) noexcept {
    const float cr = std::cos(rollRad);
    const float sr = std::sin(rollRad);
    const Vec3 latBase{-sr, cr, 0.0f};
    const Vec3 radXY{cr, sr, 0.0f};
    const Vec3 fwdAxis{0.0f, 0.0f, -1.0f};

    const float co = std::cos(openRad);
    const float so = std::sin(openRad);
    const Vec3 dirY = normV(fwdAxis * co + radXY * so);
    const Vec3 baseZ = normV(fwdAxis * so - radXY * co);

    const float ct = std::cos(twistRad);
    const float st = std::sin(twistRad);
    const Vec3 dirX = latBase * ct + baseZ * st;
    const Vec3 dirZ = baseZ * ct - latBase * st;

    fillInstanceBasisAndOrigin(
        d,
        blasVa,
        dirX * (length * widthScale),
        dirY * length,
        dirZ * (length * cupScale),
        pivot,
        packedId);
}

void populateTaipei96Instances(
    D3D12_RAYTRACING_INSTANCE_DESC* outDescs,
    const std::array<D3D12_GPU_VIRTUAL_ADDRESS, kNumBlasMeshes>& blasVas,
    float filmTime,
    float revealOpen01) noexcept {
    uint32_t idx = 0;

    // [0]: Wet Taipei asphalt boulevard slab (top face at y = 0.0)
    fillInstanceTransformAxisAligned(
        outDescs[idx++], blasVas[0], 0.0f, -0.25f, 4.0f, 28.0f, 0.25f, 36.0f,
        packInstanceMetadata(0u, 0u, 0u));

    // [1..18]: 18 chamfered black-enamel boulevard colonnade pillars (subIdx = 0)
    for (int i = 0; i < 9; ++i) {
        const float z = -12.0f + static_cast<float>(i) * 4.0f;
        fillInstanceTransformAxisAligned(
            outDescs[idx++], blasVas[0], -4.5f, 3.2f, z, 0.48f, 3.2f, 0.48f,
            packInstanceMetadata(0u, 1u, 0u));
        fillInstanceTransformAxisAligned(
            outDescs[idx++], blasVas[0],  4.5f, 3.2f, z, 0.48f, 3.2f, 0.48f,
            packInstanceMetadata(0u, 1u, 0u));
    }

    // [19..36]: 18 framed Taipei boulevard vertical light-boxes (matKind = 2)
    for (int i = 0; i < 9; ++i) {
        const float z = -12.0f + static_cast<float>(i) * 4.0f;
        fillInstanceTransformAxisAligned(
            outDescs[idx++], blasVas[0], -3.92f, 2.6f, z, 0.08f, 1.35f, 0.28f,
            packInstanceMetadata(0u, 2u, 0u));
        fillInstanceTransformAxisAligned(
            outDescs[idx++], blasVas[0],  3.92f, 2.6f, z, 0.08f, 1.35f, 0.28f,
            packInstanceMetadata(0u, 2u, 0u));
    }

    // [37..38]: Twin colossal chamfered black-enamel monoliths parting on revealOpen01 (subIdx = 1)
    const float monolithX = 2.35f + revealOpen01 * 1.45f;
    fillInstanceTransformAxisAligned(
        outDescs[idx++], blasVas[0], -monolithX, 7.5f, 5.5f, 1.15f, 7.5f, 2.2f,
        packInstanceMetadata(0u, 1u, 1u));
    fillInstanceTransformAxisAligned(
        outDescs[idx++], blasVas[0],  monolithX, 7.5f, 5.5f, 1.15f, 7.5f, 2.2f,
        packInstanceMetadata(0u, 1u, 1u));

    // [39..50]: 12 sleek dark-titanium vault louvers retracting cleanly inside monolith flanks before t=43s
    const float smoothOpen = revealOpen01 * revealOpen01 * (3.0f - 2.0f * revealOpen01);
    const float louverRetract = smoothstepF(0.0f, 0.72f, revealOpen01);
    const float louverX = 0.62f + louverRetract * 2.95f;
    for (int r = 0; r < 6; ++r) {
        const float y = 5.0f + static_cast<float>(r) * 1.1f;
        fillInstanceTransformAxisAligned(
            outDescs[idx++], blasVas[0], -louverX, y, 3.8f, 0.64f, 0.13f, 0.20f,
            packInstanceMetadata(0u, 6u, 1u));
        fillInstanceTransformAxisAligned(
            outDescs[idx++], blasVas[0],  louverX, y, 3.8f, 0.64f, 0.13f, 0.20f,
            packInstanceMetadata(0u, 6u, 1u));
    }

    // [51..54]: Sinuous S-curved spy-optical brushed-brass Inflorescence Pedicel Stem & Receptacle Collar
    fillInstanceSegmentAB(
        outDescs[idx++], blasVas[3],
        {0.0f, 2.18f, 6.28f}, {-0.12f, 4.25f, 6.18f}, 0.100f, 0.100f,
        packInstanceMetadata(3u, 6u, 0u));
    fillInstanceSegmentAB(
        outDescs[idx++], blasVas[3],
        {-0.11f, 4.18f, 6.18f}, {0.08f, 6.18f, 5.92f}, 0.086f, 0.086f,
        packInstanceMetadata(3u, 6u, 0u));
    fillInstanceSegmentAB(
        outDescs[idx++], blasVas[3],
        {0.07f, 6.12f, 5.93f}, {0.0f, 7.68f, 5.58f}, 0.080f, 0.080f,
        packInstanceMetadata(3u, 6u, 0u));
    fillInstanceSegmentAB(
        outDescs[idx++], blasVas[3],
        {0.0f, 7.56f, 5.64f}, {0.0f, 7.82f, 5.48f}, 0.165f, 0.155f,
        packInstanceMetadata(3u, 6u, 0u));

    const Vec3 hub{0.0f, 7.80f, 5.50f};
    const float breath = 0.025f * std::sin(filmTime * 0.85f);

    // [55]: Central Warm Porcelain-Ivory Enamel Gynostemium Column (matKind = 5)
    fillInstanceSegmentAB(
        outDescs[idx++], blasVas[3],
        hub + Vec3{0.0f, 0.02f, 0.06f},
        hub + Vec3{0.0f, 0.11f, -0.58f},
        0.130f, 0.112f,
        packInstanceMetadata(3u, 5u, 0u));

    // [56]: Jewel-sized Molten Amber-Ruby Pollinia Nectar Core nestled beneath column tip (matKind = 4)
    fillInstanceSegmentAB(
        outDescs[idx++], blasVas[3],
        hub + Vec3{0.0f, -0.05f, -0.28f},
        hub + Vec3{0.0f, -0.02f, -0.53f},
        0.125f, 0.118f,
        packInstanceMetadata(3u, 4u, 0u));

    // [57..62]: Separated Posterior Sepal Whorl (celadon-jade) + Anterior Wing Petals (orchid-amethyst) + 3-Lobed Labellum
    // [57] Dorsal Sepal (12h, posterior z + 0.14)
    const float lenDorsal = 1.05f + 0.97f * smoothOpen;
    fillInstanceBotanicalPetal(
        outDescs[idx++], blasVas[1],
        hub + Vec3{0.0f, 0.04f, 0.14f},
        1.5707963f, 0.24f + 1.10f * smoothOpen + breath, 0.0f,
        lenDorsal, 0.92f, 0.92f,
        packInstanceMetadata(1u, 3u, 0u));

    // [58..59] Anterior Lateral Butterfly Wing Petals (9:45 & 2:15, anterior z - 0.06, crisp notches above & below!)
    const float lenWing = 1.10f + 0.96f * smoothOpen;
    const float openWing = 0.26f + 0.90f * smoothOpen - breath;
    fillInstanceBotanicalPetal(
        outDescs[idx++], blasVas[1],
        hub + Vec3{0.05f, 0.02f, -0.06f},
        0.2792527f, openWing, -0.08f * smoothOpen,
        lenWing, 1.28f, 1.00f,
        packInstanceMetadata(1u, 3u, 1u));
    fillInstanceBotanicalPetal(
        outDescs[idx++], blasVas[1],
        hub + Vec3{-0.05f, 0.02f, -0.06f},
        kPi - 0.2792527f, openWing, 0.08f * smoothOpen,
        lenWing, 1.28f, 1.00f,
        packInstanceMetadata(1u, 3u, 2u));

    // [60..61] Posterior Swept Lower Lateral Sepals (7:30 & 4:30, posterior z + 0.14)
    const float lenLower = 1.00f + 0.92f * smoothOpen;
    const float openLower = 0.25f + 1.11f * smoothOpen + breath * 0.7f;
    fillInstanceBotanicalPetal(
        outDescs[idx++], blasVas[1],
        hub + Vec3{0.04f, -0.04f, 0.14f},
        -0.9075712f, openLower, 0.06f * smoothOpen,
        lenLower, 0.86f, 0.90f,
        packInstanceMetadata(1u, 3u, 3u));
    fillInstanceBotanicalPetal(
        outDescs[idx++], blasVas[1],
        hub + Vec3{-0.04f, -0.04f, 0.14f},
        -kPi + 0.9075712f, openLower, -0.06f * smoothOpen,
        lenLower, 0.86f, 0.90f,
        packInstanceMetadata(1u, 3u, 4u));

    // [62]: Central Sculpted 3-Lobed Cupped Labellum Lip (Mesh 2, subIdx = 5 -> crimson-ruby & nectar-gold throat)
    const float lenLip = 0.86f + 0.62f * smoothOpen;
    fillInstanceBotanicalPetal(
        outDescs[idx++], blasVas[2],
        hub + Vec3{0.0f, -0.07f, -0.14f},
        -1.5707963f, 0.20f + 0.72f * smoothOpen - breath * 0.5f, 0.0f,
        lenLip, 1.12f, 1.04f,
        packInstanceMetadata(2u, 3u, 5u));

    // [63..68]: 6 Compact Inner Throat Cristae Petals cupping the column without blocking outer petal clefts
    const float innerAngles[6] = {1.01f, 2.13f, 0.04f, 3.10f, -0.48f, -2.66f};
    for (uint32_t p = 0; p < 6u; ++p) {
        const float wob = 0.02f * std::sin(filmTime * 0.95f + static_cast<float>(p) * 1.1f);
        const float lenIn = 0.52f + 0.30f * smoothOpen;
        const float openIn = 0.18f + 0.54f * smoothOpen + wob;
        const float twistIn = ((p % 2u) == 0u) ? 0.14f : -0.14f;
        fillInstanceBotanicalPetal(
            outDescs[idx++], blasVas[1],
            hub + Vec3{0.0f, 0.0f, -0.06f},
            innerAngles[p], openIn, twistIn * smoothOpen,
            lenIn, 0.76f, 1.08f,
            packInstanceMetadata(1u, 3u, 6u + p));
    }

    // [69..78]: 10 Slender Recessed Emerald-Jade Calyx Lancets aligned behind sepal ribs (preserving crisp notches!)
    const float calyxAngles[10] = {
        1.5707963f, 1.33f, 1.81f,
        0.28f, kPi - 0.28f,
        -0.91f, -1.12f, -kPi + 0.91f, -kPi + 1.12f,
        -1.5707963f
    };
    for (uint32_t c = 0; c < 10u; ++c) {
        const float ang = calyxAngles[c];
        const float lenCal = 0.76f + 0.64f * smoothOpen;
        const float openCal = 0.28f + 1.12f * smoothOpen + 0.015f * std::cos(filmTime * 0.7f + ang * 2.0f);
        fillInstanceBotanicalPetal(
            outDescs[idx++], blasVas[1],
            hub + Vec3{0.0f, 0.0f, 0.24f},
            ang, openCal, 0.0f,
            lenCal, 0.34f, 0.78f,
            packInstanceMetadata(1u, 3u, 12u + c));
    }

    // [79..80]: Grounded Sanctuary Obsidian-Enamel Altar Plinth + Brushed-Brass Pedestal Collar (grounds stem to y = 0!)
    fillInstanceTransformAxisAligned(
        outDescs[idx++], blasVas[0], 0.0f, 1.05f, 6.28f, 0.56f, 1.05f, 0.56f,
        packInstanceMetadata(0u, 1u, 1u));
    fillInstanceTransformAxisAligned(
        outDescs[idx++], blasVas[0], 0.0f, 2.16f, 6.28f, 0.38f, 0.09f, 0.38f,
        packInstanceMetadata(0u, 6u, 0u));

    // [81..88]: 6 Delicate 24k Gold Stamen Filaments + 2 Labellum Throat Cirri Whiskers (Mesh 3, matKind = 6, subIdx = 2)
    Vec3 stamenTips[6]{};
    const float stamRolls[6] = {0.35f, 0.95f, 2.19f, 2.79f, -0.52f, -2.62f};
    for (uint32_t s = 0; s < 6u; ++s) {
        const float r = stamRolls[s];
        const float spread = 0.07f + 0.32f * smoothOpen;
        const Vec3 base = hub + Vec3{std::cos(r) * 0.05f, std::sin(r) * 0.05f - 0.03f, -0.14f};
        const Vec3 tip  = hub + Vec3{
            std::cos(r) * spread,
            std::sin(r) * spread * 0.78f - 0.04f,
            -0.26f - 0.44f * smoothOpen};
        stamenTips[s] = tip;
        fillInstanceSegmentAB(
            outDescs[idx++], blasVas[3], base, tip, 0.020f, 0.020f,
            packInstanceMetadata(3u, 6u, 2u));
    }

    // 2 Curved lip cirri whiskers sweeping forward from the labellum throat
    for (int w = 0; w < 2; ++w) {
        const float sgn = (w == 0) ? -1.0f : 1.0f;
        const Vec3 base = hub + Vec3{sgn * 0.08f, -0.12f, -0.22f};
        const Vec3 tip  = hub + Vec3{sgn * (0.14f + 0.24f * smoothOpen), -0.22f - 0.26f * smoothOpen, -0.34f - 0.52f * smoothOpen};
        fillInstanceSegmentAB(
            outDescs[idx++], blasVas[3], base, tip, 0.017f, 0.017f,
            packInstanceMetadata(3u, 6u, 2u));
    }

    // [89..95]: 6 Golden-Amber Anther Tip Jewels + 1 Dorsal Column Crest Prism (total = 96 instances!)
    for (uint32_t s = 0; s < 6u; ++s) {
        const Vec3 dir = normV(stamenTips[s] - hub);
        fillInstanceSegmentAB(
            outDescs[idx++], blasVas[3],
            stamenTips[s] - dir * 0.03f,
            stamenTips[s] + dir * 0.065f,
            0.042f, 0.042f,
            packInstanceMetadata(3u, (s % 2u == 0u) ? 4u : 3u, 0u));
    }
    fillInstanceSegmentAB(
        outDescs[idx++], blasVas[3],
        hub + Vec3{0.0f, 0.14f, -0.52f},
        hub + Vec3{0.0f, 0.24f, -0.64f},
        0.055f, 0.045f,
        packInstanceMetadata(3u, 3u, 0u));

    // [96..111]: Architecturally Accurate Taipei 101 at (0, 0..28.6, 37.5) — 16 instances!
    // [96..97]: 2-stage inward-sloped notched Podium Base (1F-25F, y = 0..5.2) via upside-down Mesh 4!
    fillInstanceBasisAndOrigin(
        outDescs[idx++], blasVas[4],
        Vec3{-2.48f, 0.0f, 0.0f}, Vec3{0.0f, -2.60f, 0.0f}, Vec3{0.0f, 0.0f, 2.48f},
        Vec3{0.0f, 2.60f, 37.5f},
        packInstanceMetadata(4u, 7u, 0u));
    fillInstanceBasisAndOrigin(
        outDescs[idx++], blasVas[4],
        Vec3{-2.14f, 0.0f, 0.0f}, Vec3{0.0f, -2.60f, 0.0f}, Vec3{0.0f, 0.0f, 2.14f},
        Vec3{0.0f, 5.20f, 37.5f},
        packInstanceMetadata(4u, 7u, 0u));

    // [98..105]: 8 Iconic 7-deg Outward-Canted Double-Notched Ruyi / Bamboo Modules (26F-89F, y = 5.2..21.2)
    for (uint32_t tier = 0; tier < 8u; ++tier) {
        const float yBase = 5.20f + static_cast<float>(tier) * 2.0f;
        const float taper = 1.0f - 0.018f * static_cast<float>(tier);
        const float wTop  = 1.98f * taper;
        // Slightly set back top module (tier 7) in +Z so the 660-ton Golden TMD Sphere gleams in front of its south sanctuary arch!
        const float zCenter = (tier == 7u) ? 38.05f : 37.5f;
        const float zScale  = (tier == 7u) ? (wTop * 0.78f) : wTop;
        fillInstanceBasisAndOrigin(
            outDescs[idx++], blasVas[4],
            Vec3{wTop, 0.0f, 0.0f}, Vec3{0.0f, 2.0f, 0.0f}, Vec3{0.0f, 0.0f, zScale},
            Vec3{0.0f, yBase, zCenter},
            packInstanceMetadata(4u, 7u, (tier == 7u) ? 1u : 0u));
    }

    // [106]: 660-Metric-Ton Polished 24k-Gold Tuned Mass Damper (TMD) Sphere at (0, 20.25, 36.15)
    fillInstanceSegmentAB(
        outDescs[idx++], blasVas[3],
        Vec3{0.0f, 19.67f, 36.15f},
        Vec3{0.0f, 20.83f, 36.15f},
        0.58f, 0.58f,
        packInstanceMetadata(3u, 6u, 3u));

    // [107..111]: Observatory Sanctuary Lantern (90F-91F) + Stepped Communications Spire + 508m Pinnacle Beacon
    fillInstanceBasisAndOrigin(
        outDescs[idx++], blasVas[4],
        Vec3{1.18f, 0.0f, 0.0f}, Vec3{0.0f, 1.60f, 0.0f}, Vec3{0.0f, 0.0f, 1.18f},
        Vec3{0.0f, 21.20f, 37.5f},
        packInstanceMetadata(4u, 7u, 1u));
    fillInstanceTransformAxisAligned(
        outDescs[idx++], blasVas[0], 0.0f, 23.60f, 37.5f, 0.58f, 0.82f, 0.58f,
        packInstanceMetadata(0u, 6u, 0u));
    fillInstanceTransformAxisAligned(
        outDescs[idx++], blasVas[0], 0.0f, 24.75f, 37.5f, 0.34f, 0.42f, 0.34f,
        packInstanceMetadata(0u, 6u, 2u));
    fillInstanceSegmentAB(
        outDescs[idx++], blasVas[3],
        Vec3{0.0f, 25.10f, 37.5f},
        Vec3{0.0f, 28.25f, 37.5f},
        0.14f, 0.14f,
        packInstanceMetadata(3u, 6u, 2u));
    fillInstanceSegmentAB(
        outDescs[idx++], blasVas[3],
        Vec3{0.0f, 28.15f, 37.5f},
        Vec3{0.0f, 28.75f, 37.5f},
        0.09f, 0.09f,
        packInstanceMetadata(3u, 4u, 0u));
}

void transitionResource(
    ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    if (!res || before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
}

uint64_t singleResourceAllocBytes(ID3D12Device* dev, const D3D12_RESOURCE_DESC& desc) noexcept {
    const D3D12_RESOURCE_ALLOCATION_INFO info = dev->GetResourceAllocationInfo(0, 1, &desc);
    return (info.SizeInBytes != UINT64_MAX) ? info.SizeInBytes : 0ULL;
}

D3D12_RESOURCE_DESC makeBufferDesc(uint64_t bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) noexcept {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<uint64_t>(256ULL, bytes);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    return rd;
}

D3D12_RESOURCE_DESC makeTex2dDesc(uint32_t w, uint32_t h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags) noexcept {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = std::max<uint32_t>(1u, w);
    td.Height = std::max<uint32_t>(1u, h);
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = flags;
    return td;
}

bool unpackRgb10ReadbackBuffer(
    ID3D12Resource* readbackBuf,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
    uint64_t totalBytes,
    uint32_t width,
    uint32_t height,
    std::vector<uint8_t>& outRgba,
    std::vector<uint32_t>* outRgb10,
    std::string* outError) {
    if (outRgb10) outRgb10->resize(static_cast<size_t>(width) * height);
    outRgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);

    D3D12_RANGE rr{0, static_cast<SIZE_T>(totalBytes)};
    const uint8_t* mapped = nullptr;
    if (FAILED(readbackBuf->Map(0, &rr, reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped)))) || !mapped) {
        if (outError) *outError = "Failed to map RGB10 readback buffer";
        return false;
    }
    for (uint32_t y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch);
        if (outRgb10) {
            std::copy_n(row, width, outRgb10->data() + static_cast<size_t>(y) * width);
        }
        for (uint32_t x = 0; x < width; ++x) {
            const size_t dst = (static_cast<size_t>(y) * width + x) * 4u;
            for (uint32_t c = 0; c < 3; ++c) {
                outRgba[dst + c] = static_cast<uint8_t>((((row[x] >> (c * 10u)) & 1023u) * 255u + 511u) / 1023u);
            }
            outRgba[dst + 3] = 255;
        }
    }
    D3D12_RANGE wr{0, 0};
    readbackBuf->Unmap(0, &wr);
    return true;
}

bool unpackRgba8ReadbackBuffer(
    ID3D12Resource* readbackBuf,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
    uint64_t totalBytes,
    uint32_t width,
    uint32_t height,
    std::vector<uint8_t>& outRgba,
    std::string* outError) {
    outRgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    D3D12_RANGE rr{0, static_cast<SIZE_T>(totalBytes)};
    const uint8_t* mapped = nullptr;
    if (FAILED(readbackBuf->Map(0, &rr, reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped)))) || !mapped) {
        if (outError) *outError = "Failed to map RGBA8 readback buffer";
        return false;
    }
    const size_t rowBytes = static_cast<size_t>(width) * 4u;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* srcRow = mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        std::memcpy(outRgba.data() + static_cast<size_t>(y) * rowBytes, srcRow, rowBytes);
    }
    D3D12_RANGE wr{0, 0};
    readbackBuf->Unmap(0, &wr);
    return true;
}

constexpr D3D12_RESOURCE_STATES kCanvasReadState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kTensorReadState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

} // namespace

struct M1TaipeiSliceEffect::Impl {
    bool trilogy = false, injectMeshFailure = false;
    ShowVisualMode visualMode = ShowVisualMode::M1Slice60s;
    QualityPresetSpec activeSpecForParticles{};
    std::unique_ptr<TrilogyMeshRenderer> trilogyMesh;

    void resolveCudaSimParams(
        int64_t samplePosition,
        int32_t sampleRate,
        const QualityPresetSpec& spec,
        uint32_t& outCount,
        uint32_t& outSimMode) const noexcept {
        const double tSec = static_cast<double>(samplePosition) / static_cast<double>(std::max<int32_t>(1, sampleRate));
        const bool isMutiny = (visualMode == ShowVisualMode::SoloCUDA) ||
                              (visualMode == ShowVisualMode::FullFilm360s && tSec >= 195.0 && tSec < 245.0);
        if (isMutiny) {
            outCount = std::clamp<uint32_t>(
                spec.particleBudget,
                LiveAcceleratorOrchestra::kSoloCudaParticleCount,
                LiveAcceleratorOrchestra::kMaxCudaParticleCount);
            outSimMode = 1u;
        } else {
            outCount = LiveAcceleratorOrchestra::kCudaParticleCount;
            outSimMode = 0u;
        }
    }

    bool isCueSignalHijackAt(double filmTimeSeconds) const noexcept {
        return visualMode == ShowVisualMode::FullFilm360s &&
               filmTimeSeconds >= 245.0 && filmTimeSeconds < 275.0;
    }
    // Compiled DXIL shader bytecodes
    std::vector<uint8_t> vsFullscreenBytes;
    std::vector<uint8_t> csDxrSceneBytes;
    std::vector<uint8_t> psRasterSceneBytes;
    std::vector<uint8_t> psBloomDownBytes;
    std::vector<uint8_t> psBloomUpBytes;
    std::vector<uint8_t> psCompositeBytes;
    std::vector<uint8_t> psPresentHudBytes;
    std::vector<uint8_t> csParticlesSubBytes;
    std::vector<uint8_t> vsParticlesBytes;
    std::vector<uint8_t> psParticlesBytes;
    std::vector<uint8_t> csTensorSubBytes;
    std::vector<uint8_t> csNv12PackBytes;

    // Root signatures & PSOs
    ComPtr<ID3D12RootSignature> gfxRootSig;
    ComPtr<ID3D12RootSignature> compositeRootSig;
    ComPtr<ID3D12RootSignature> presentRootSig;
    ComPtr<ID3D12RootSignature> particlesGfxRootSig;
    ComPtr<ID3D12RootSignature> auxComputeRootSig;
    ComPtr<ID3D12RootSignature> dxrRootSig;

    ComPtr<ID3D12PipelineState> psoDxrScene;
    ComPtr<ID3D12PipelineState> psoRasterScene;
    ComPtr<ID3D12PipelineState> psoParticles3D;
    ComPtr<ID3D12PipelineState> psoParticlesSubCs;
    ComPtr<ID3D12PipelineState> psoTensorSubCs;
    ComPtr<ID3D12PipelineState> psoNv12PackCs;
    ComPtr<ID3D12PipelineState> psoBloomDown;
    ComPtr<ID3D12PipelineState> psoBloomUpAdditive;
    ComPtr<ID3D12PipelineState> psoCompositeCanvas;
    ComPtr<ID3D12PipelineState> psoPresentHud;
    DXGI_FORMAT presentPsoFormat = DXGI_FORMAT_UNKNOWN;

    // Owned descriptor heaps:
    // rtvHeap (7 entries):
    //   [0]=sceneHdr, [1]=sceneDepth, [2..4]=bloomMips[0..2], [5]=canvasClean, [6]=canvasAbCompare
    // srvUavHeap (18 entries):
    //   [0]=UAV sceneHdr (u1), [1]=UAV sceneDepth (u2),
    //   [2]=SRV sceneDepth, [3]=SRV sceneHdr, [4..6]=SRV bloomMips[0..2], [7]=SRV tensorFieldTex,
    //   [8]=SRV canvasClean, [9]=SRV canvasAbCompare, [10]=SRV codecDecodedTex,
    //   [11]=UAV tensorFieldTex, [12]=RAW UAV nv12PackBuf, [13]=Structured UAV particleGpuBuf,
    //   [14..17]=present-pass table: SRV canvasClean, canvasAbCompare, codecDecodedTex, uiTextTex
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> srvUavHeap;
    uint32_t rtvDescSize = 0;
    uint32_t srvUavDescSize = 0;

    // Resolution-dependent offscreen 16:9 canvas bundle
    uint32_t canvasW = 0;
    uint32_t canvasH = 0;
    uint64_t currentCanvasBundleBytes = 0;
    ComPtr<ID3D12Resource> sceneHdr;
    ComPtr<ID3D12Resource> sceneDepth;
    std::array<ComPtr<ID3D12Resource>, kNumBloomMips> bloomMips;
    std::array<uint32_t, kNumBloomMips> bloomW{0, 0, 0};
    std::array<uint32_t, kNumBloomMips> bloomH{0, 0, 0};
    ComPtr<ID3D12Resource> canvasClean;
    ComPtr<ID3D12Resource> canvasAbCompare;

    // Static 4-pillar GPU buffers & textures
    ComPtr<ID3D12Resource> tensorFieldTex;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT tensorFootprint{};
    uint64_t tensorUploadBytes = 0;
    std::array<ComPtr<ID3D12Resource>, kTotalUploadSlots> tensorUploadBuf;

    ComPtr<ID3D12Resource> particleGpuBuf;
    std::array<ComPtr<ID3D12Resource>, kTotalUploadSlots> particleUploadBuf;

    ComPtr<ID3D12Resource> nv12PackBuf;
    std::array<ComPtr<ID3D12Resource>, kTotalUploadSlots> nv12ReadbackBuf;
    std::array<bool, kFrameSlots> nv12SlotReady{false, false, false};

    ComPtr<ID3D12Resource> codecDecodedTex;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT codecFootprint{};
    uint64_t codecUploadBytes = 0;
    std::array<ComPtr<ID3D12Resource>, kTotalUploadSlots> codecRgbaUploadBuf;
    std::vector<uint8_t> codecStagingRgba;
    bool codecHwTexValid = false;

    ComPtr<ID3D12Resource> presentCbUpload; // 512 bytes * kTotalUploadSlots

    // Localized text atlas (R8, include/boo/ui_text.hpp). Created only when a non-English catalog is loaded.
    ComPtr<ID3D12Resource> uiTextTex;
    std::array<ComPtr<ID3D12Resource>, kTotalUploadSlots> uiTextUploadBuf;
    std::unique_ptr<UiTextLayer> uiTextLayer;
    bool ensureUiTextResources(ID3D12Device* dev);
    void uploadUiText(ID3D12Device* dev, ID3D12GraphicsCommandList* cmd, uint32_t cbSlot, bool labels, bool caption);
    ComPtr<ID3D12Resource> abVerifyUav;

    // Dedicated GPU hardware timestamp query heap for exact Pass 1 RT/Raster execution ms (works in headless & swapchain!)
    ComPtr<ID3D12QueryHeap> rtTsHeap;
    ComPtr<ID3D12Resource> rtTsReadback;
    std::array<bool, kTotalUploadSlots> rtTsSlotValid{};
    uint64_t gpuTimestampFreq = 0;
    float lastRtSceneMs = 0.45f;

    // Live zero-SDK dynamic hardware orchestra (CUDA 65K vortex, Tensor MMA caustics, Media Foundation HW H.264 round-trip)
    LiveAcceleratorOrchestra liveAccel;
    bool accelCoreInited = false;
    bool accelCodecInited = false;

    // Hardware DXR 1.1 multi-mesh acceleration structures & 3-slot verification ring
    bool dxrHardwareCapable = false;
    bool dxrTier12 = false;
    bool dxrPipelineReady = false;
    bool dxrQuarantined = false;
    bool dxrVerified = false;
    uint64_t dxrStaticBytes = 0;

    std::array<MeshSlice, kNumBlasMeshes> meshSlices{};
    ComPtr<ID3D12Resource> blasVbUpload;
    ComPtr<ID3D12Resource> blasIbUpload;
    ComPtr<ID3D12Resource> blasScratch;
    std::array<ComPtr<ID3D12Resource>, kNumBlasMeshes> blasResults;
    ComPtr<ID3D12Resource> tlasInstanceUpload;
    ComPtr<ID3D12Resource> tlasScratch;
    ComPtr<ID3D12Resource> tlasResult;
    ComPtr<ID3D12Resource> zeroUploadBuf;
    std::array<ComPtr<ID3D12Resource>, kFrameSlots> dxrVerifyUav;
    std::array<ComPtr<ID3D12Resource>, kFrameSlots> dxrVerifyReadback;

    struct PendingVerifySlot {
        bool active = false;
        uint64_t generation = 0;
        uint32_t expectedSeq = 0;
    };
    std::array<PendingVerifySlot, kFrameSlots> slotPending{};
    uint64_t dxrGeneration = 1;
    uint32_t nextDxrSeq = 1;
    uint32_t lastVerifiedHits = 0;

    HardwareResult liveRtStatus{};
    HardwareResult liveCudaStatus{};
    HardwareResult liveTensorStatus{};
    HardwareResult liveCodecStatus{};
    std::array<LivePillarTelemetry, 4> livePillars{};

    bool externalNv12Active = false;
    bool externalTexUploaded = false;
    uint64_t externalNv12Seq = 0;
    uint64_t uploadedExternalSeq = 0;
    std::vector<uint8_t> externalNv12Buf;
    void convertExternalNv12ToRgba8(std::vector<uint8_t>& outRgba) const noexcept;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvAt(uint32_t idx) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(idx) * rtvDescSize;
        return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuAt(uint32_t idx) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE h = srvUavHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(idx) * srvUavDescSize;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuAt(uint32_t idx) const noexcept {
        D3D12_GPU_DESCRIPTOR_HANDLE h = srvUavHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(idx) * srvUavDescSize;
        return h;
    }

    bool allocateStaticPillarResources(D3D12Context& d3d);
    void ensureLiveAccelerators(const D3D12Context& d3d, bool needCuda, bool needTensor, bool needCodec);

    bool buildCanvasBundle(
        D3D12Context& d3d,
        uint32_t reqW,
        uint32_t reqH,
        uint64_t softVramLimitBytes);

    bool buildDxrResourcesAndWarmUp(D3D12Context& d3d, uint64_t softVramLimitBytes);
    bool ensurePresentPso(ID3D12Device* dev, DXGI_FORMAT swapchainFmt);
    void updateLiveRtStatus(bool disableRT, bool presetAllowsDxr) noexcept;
    void updateLiveAuxiliaryStatuses(
        bool isWarp,
        bool disableCUDA,
        bool presetAllowsCuda,
        bool disableTensor,
        bool presetAllowsTensor,
        bool disableCodec,
        bool codecViewActive) noexcept;
    void retireVerifySlot(uint32_t slot) noexcept;
    void readRtTimestampSlot(uint32_t slot) noexcept;
    GpuM1SceneConstants buildSceneConstants(const FrameContext& ctx, M1CameraState& outCam) const noexcept;

    void recordBloomAndCompositeToTarget(
        ID3D12GraphicsCommandList* cmd,
        const FrameContext& ctx,
        float revealOpen01,
        ID3D12Resource* targetCanvas,
        uint32_t targetRtvIndex,
        D3D12Context* timestampCtx);

    void recordScenePipelineToCanvas(
        D3D12Context& d3d,
        ID3D12GraphicsCommandList* cmd,
        const FrameContext& ctx,
        bool useDxr,
        bool useCuda,
        bool useTensor,
        bool skipSharedCudaUpdate,
        bool skipSharedTensorUpdate,
        ID3D12Resource* targetCanvas,
        uint32_t targetRtvIndex,
        uint32_t slot,
        bool isCanonicalPass,
        bool recordTimestamps);

    void recordNv12PackAndReadback(
        ID3D12GraphicsCommandList* cmd,
        uint32_t readbackSlot);

    void uploadCodecRgbaToGpu(
        ID3D12GraphicsCommandList* cmd,
        uint32_t uploadSlot,
        const std::vector<uint8_t>& rgbaBytes,
        bool markHwVerified = true);

    void recordSwapchainPresentPass(
        ID3D12Device* dev,
        ID3D12GraphicsCommandList* cmd,
        uint32_t targetWidth,
        uint32_t targetHeight,
        DXGI_FORMAT targetFormat,
        uint32_t targetBits,
        uint32_t cbSlot,
        const FrameContext& ctx,
        const HudTelemetrySnapshot& hud,
        AbCompareEngine abEngine,
        bool codecHijackActive);
};

M1TaipeiSliceEffect::M1TaipeiSliceEffect(
    D3D12Context& d3d,
    const QualityPresetSpec& initialSpec,
    bool disableRT,
    bool simulateInitFailure,
    bool disableCUDA,
    bool disableTensor,
    bool disableCodec,
    bool enableCodecHijack,
    bool trilogy,
    bool injectMeshFailure,
    ShowVisualMode visualMode)
    : d3d_(d3d),
      activeSpec_(initialSpec),
      disableRT_(disableRT),
      disableCUDA_(disableCUDA),
      disableTensor_(disableTensor),
      disableCodec_(disableCodec),
      codecHijackActive_(enableCodecHijack),
      simulateInitFailure_(simulateInitFailure),
      impl_(std::make_unique<Impl>()) {
    if (trilogy && visualMode == ShowVisualMode::M1Slice60s) {
        visualMode = ShowVisualMode::M2Trilogy180s;
    }
    impl_->visualMode = visualMode;
    impl_->activeSpecForParticles = initialSpec;
    impl_->trilogy = (visualMode != ShowVisualMode::M1Slice60s && visualMode != ShowVisualMode::LegacyM0);
    impl_->injectMeshFailure = injectMeshFailure;
}

ShowVisualMode M1TaipeiSliceEffect::visualMode() const noexcept {
    return impl_ ? impl_->visualMode : ShowVisualMode::LegacyM0;
}

M1TaipeiSliceEffect::~M1TaipeiSliceEffect() {
    shutdown();
}

bool M1TaipeiSliceEffect::Impl::ensurePresentPso(ID3D12Device* dev, DXGI_FORMAT swapchainFmt) {
    if (psoPresentHud && presentPsoFormat == swapchainFmt) {
        return true;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = presentRootSig.Get();
    pd.VS = {vsFullscreenBytes.data(), vsFullscreenBytes.size()};
    pd.PS = {psPresentHudBytes.data(), psPresentHudBytes.size()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = swapchainFmt;
    pd.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> built;
    if (FAILED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&built))) || !built) {
        return false;
    }
    psoPresentHud = std::move(built);
    presentPsoFormat = swapchainFmt;
    return true;
}

bool M1TaipeiSliceEffect::Impl::allocateStaticPillarResources(D3D12Context& d3d) {
    auto* dev = static_cast<ID3D12Device*>(d3d.deviceRaw());
    if (!dev) return false;

    if (auto* q = static_cast<ID3D12CommandQueue*>(d3d.commandQueueRaw())) {
        q->GetTimestampFrequency(&gpuTimestampFreq);
    }

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;

    // 1. Tensor Core 320x180 RGBA16F field texture + upload ring
    const D3D12_RESOURCE_DESC tensorDesc = makeTex2dDesc(
        LiveAcceleratorOrchestra::kTensorWidth,
        LiveAcceleratorOrchestra::kTensorHeight,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (FAILED(dev->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &tensorDesc,
            kTensorReadState, nullptr, IID_PPV_ARGS(&tensorFieldTex)))) {
        return false;
    }
    UINT numRows = 0;
    UINT64 rowBytes = 0;
    dev->GetCopyableFootprints(&tensorDesc, 0, 1, 0, &tensorFootprint, &numRows, &rowBytes, &tensorUploadBytes);
    const D3D12_RESOURCE_DESC tensorUpDesc = makeBufferDesc(tensorUploadBytes);

    // 2. Up-to-262,144-particle GPU StructuredBuffer + upload ring (65,536 M1/M2 default; 102,400..262,144 SoloCUDA/Act 4)
    constexpr uint64_t kParticleBufBytes =
        static_cast<uint64_t>(LiveAcceleratorOrchestra::kMaxCudaParticleCount) * sizeof(LiveParticleState);
    const D3D12_RESOURCE_DESC partGpuDesc = makeBufferDesc(kParticleBufBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC partUpDesc  = makeBufferDesc(kParticleBufBytes);
    if (FAILED(dev->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &partGpuDesc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&particleGpuBuf)))) {
        return false;
    }

    // 3. 1280x720 NV12 pack UAV + readback ring + 1280x720 RGBA8 decoded texture + upload ring
    constexpr uint64_t kNv12Bytes = 1280ULL * 720ULL * 3ULL / 2ULL; // 1,382,400 bytes
    const D3D12_RESOURCE_DESC nv12GpuDesc = makeBufferDesc(kNv12Bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC nv12RbDesc  = makeBufferDesc(kNv12Bytes);
    if (FAILED(dev->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &nv12GpuDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&nv12PackBuf)))) {
        return false;
    }

    const D3D12_RESOURCE_DESC codecTexDesc = makeTex2dDesc(
        LiveAcceleratorOrchestra::kCodecWidth,
        LiveAcceleratorOrchestra::kCodecHeight,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_NONE);
    if (FAILED(dev->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &codecTexDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&codecDecodedTex)))) {
        return false;
    }
    dev->GetCopyableFootprints(&codecTexDesc, 0, 1, 0, &codecFootprint, &numRows, &rowBytes, &codecUploadBytes);
    const D3D12_RESOURCE_DESC codecUpDesc = makeBufferDesc(codecUploadBytes);

    for (uint32_t s = 0; s < kTotalUploadSlots; ++s) {
        if (FAILED(dev->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &tensorUpDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tensorUploadBuf[s]))) ||
            FAILED(dev->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &partUpDesc,   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&particleUploadBuf[s]))) ||
            FAILED(dev->CreateCommittedResource(&rbHeap,     D3D12_HEAP_FLAG_NONE, &nv12RbDesc,   D3D12_RESOURCE_STATE_COPY_DEST,    nullptr, IID_PPV_ARGS(&nv12ReadbackBuf[s]))) ||
            FAILED(dev->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &codecUpDesc,  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&codecRgbaUploadBuf[s])))) {
            return false;
        }
    }

    const D3D12_RESOURCE_DESC presentCbDesc = makeBufferDesc(512ULL * kTotalUploadSlots);
    const D3D12_RESOURCE_DESC abVerDesc     = makeBufferDesc(256ULL, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC tsRbDesc      = makeBufferDesc(16ULL * kTotalUploadSlots);
    if (FAILED(dev->CreateCommittedResource(&uploadHeap,  D3D12_HEAP_FLAG_NONE, &presentCbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&presentCbUpload))) ||
        FAILED(dev->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &abVerDesc,     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&abVerifyUav))) ||
        FAILED(dev->CreateCommittedResource(&rbHeap,      D3D12_HEAP_FLAG_NONE, &tsRbDesc,      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rtTsReadback)))) {
        return false;
    }

    D3D12_QUERY_HEAP_DESC qhd{};
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = 2u * kTotalUploadSlots;
    if (FAILED(dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&rtTsHeap)))) {
        return false;
    }

    // Zero-fill codecRgbaUploadBuf[0] so initial copy into codecDecodedTex produces defined opaque black
    void* mapped = nullptr;
    if (SUCCEEDED(codecRgbaUploadBuf[0]->Map(0, nullptr, &mapped)) && mapped) {
        std::memset(mapped, 0, static_cast<size_t>(codecUploadBytes));
        codecRgbaUploadBuf[0]->Unmap(0, nullptr);
    }

    // Write static SRV/UAV descriptors [7], [10], [11], [12], [13]
    D3D12_SHADER_RESOURCE_VIEW_DESC srvTensor{};
    srvTensor.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvTensor.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvTensor.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvTensor.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(tensorFieldTex.Get(), &srvTensor, srvCpuAt(7u));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvCodec = srvTensor;
    srvCodec.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dev->CreateShaderResourceView(codecDecodedTex.Get(), &srvCodec, srvCpuAt(10u));
    dev->CreateShaderResourceView(codecDecodedTex.Get(), &srvCodec, srvCpuAt(16u));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavTensor{};
    uavTensor.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uavTensor.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(tensorFieldTex.Get(), nullptr, &uavTensor, srvCpuAt(11u));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavNv12{};
    uavNv12.Format = DXGI_FORMAT_R32_TYPELESS;
    uavNv12.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavNv12.Buffer.NumElements = static_cast<UINT>(kNv12Bytes / 4ULL);
    uavNv12.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    dev->CreateUnorderedAccessView(nv12PackBuf.Get(), nullptr, &uavNv12, srvCpuAt(12u));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavParticles{};
    uavParticles.Format = DXGI_FORMAT_UNKNOWN;
    uavParticles.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavParticles.Buffer.NumElements = LiveAcceleratorOrchestra::kMaxCudaParticleCount;
    uavParticles.Buffer.StructureByteStride = sizeof(LiveParticleState);
    dev->CreateUnorderedAccessView(particleGpuBuf.Get(), nullptr, &uavParticles, srvCpuAt(13u));

    return true;
}

void M1TaipeiSliceEffect::Impl::ensureLiveAccelerators(
    const D3D12Context& d3d,
    bool needCuda,
    bool needTensor,
    bool needCodec) {
    if (d3d.isWarp()) return;
    const bool wantCore  = needCuda || needTensor || accelCoreInited;
    const bool wantCodec = needCodec || accelCodecInited;
    if ((needCuda || needTensor) && !accelCoreInited) {
        liveAccel.initialize(d3d.adapterLuidLow(), d3d.adapterLuidHigh(), false, true, true, wantCodec);
        accelCoreInited = true;
        accelCodecInited = wantCodec;
    } else if (needCodec && !accelCodecInited) {
        liveAccel.initialize(d3d.adapterLuidLow(), d3d.adapterLuidHigh(), false, wantCore, wantCore, true);
        accelCoreInited = wantCore;
        accelCodecInited = true;
    }
}

bool M1TaipeiSliceEffect::Impl::buildCanvasBundle(
    D3D12Context& d3d,
    uint32_t reqW,
    uint32_t reqH,
    uint64_t softVramLimitBytes) {
    auto* dev = static_cast<ID3D12Device*>(d3d.deviceRaw());
    if (!dev) return false;

    const D3D12_RESOURCE_DESC hdrDesc = makeTex2dDesc(
        reqW, reqH, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC depthDesc = makeTex2dDesc(
        reqW, reqH, DXGI_FORMAT_R32_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    std::array<uint32_t, kNumBloomMips> mw{};
    std::array<uint32_t, kNumBloomMips> mh{};
    std::array<D3D12_RESOURCE_DESC, kNumBloomMips> mipDescs{};
    for (uint32_t m = 0; m < kNumBloomMips; ++m) {
        mw[m] = std::max<uint32_t>(1u, reqW >> (m + 1u));
        mh[m] = std::max<uint32_t>(1u, reqH >> (m + 1u));
        mipDescs[m] = makeTex2dDesc(mw[m], mh[m], DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    }

    const D3D12_RESOURCE_DESC cleanDesc = makeTex2dDesc(
        reqW, reqH, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    uint64_t newBundleBytes =
        singleResourceAllocBytes(dev, hdrDesc) +
        singleResourceAllocBytes(dev, depthDesc) +
        2ULL * singleResourceAllocBytes(dev, cleanDesc);
    for (uint32_t m = 0; m < kNumBloomMips; ++m) {
        newBundleBytes += singleResourceAllocBytes(dev, mipDescs[m]);
    }

    if (softVramLimitBytes > 0) {
        const uint64_t curVram = d3d.queryEstimatedAppVramBytes();
        const uint64_t baseVram = (curVram > currentCanvasBundleBytes) ? (curVram - currentCanvasBundleBytes) : 0ULL;
        const bool fitsBudget =
            (newBundleBytes <= softVramLimitBytes) &&
            (baseVram <= (softVramLimitBytes - newBundleBytes));
        if (!fitsBudget) {
            BOO_LOG_WARN("Render", "Canvas bundle (" + std::to_string(reqW) + "x" + std::to_string(reqH) +
                                   ") exceeds soft VRAM cap; shedding resolution.");
            return false;
        }
    }

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> newHdr;
    ComPtr<ID3D12Resource> newDepth;
    std::array<ComPtr<ID3D12Resource>, kNumBloomMips> newMips;
    ComPtr<ID3D12Resource> newClean;
    ComPtr<ID3D12Resource> newAbCompare;

    if (FAILED(dev->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &hdrDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&newHdr))) ||
        FAILED(dev->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&newDepth)))) {
        return false;
    }
    for (uint32_t m = 0; m < kNumBloomMips; ++m) {
        if (FAILED(dev->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &mipDescs[m],
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&newMips[m])))) {
            return false;
        }
    }
    if (FAILED(dev->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &cleanDesc,
            kCanvasReadState, nullptr, IID_PPV_ARGS(&newClean))) ||
        FAILED(dev->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &cleanDesc,
            kCanvasReadState, nullptr, IID_PPV_ARGS(&newAbCompare)))) {
        return false;
    }

    sceneHdr = std::move(newHdr);
    sceneDepth = std::move(newDepth);
    bloomMips = std::move(newMips);
    canvasClean = std::move(newClean);
    canvasAbCompare = std::move(newAbCompare);
    canvasW = reqW;
    canvasH = reqH;
    bloomW = mw;
    bloomH = mh;
    currentCanvasBundleBytes = newBundleBytes;

    // RTVs: [0]=sceneHdr, [1]=sceneDepth, [2..4]=bloomMips[0..2], [5]=canvasClean, [6]=canvasAbCompare
    dev->CreateRenderTargetView(sceneHdr.Get(), nullptr, rtvAt(0u));
    dev->CreateRenderTargetView(sceneDepth.Get(), nullptr, rtvAt(1u));
    for (uint32_t m = 0; m < kNumBloomMips; ++m) {
        dev->CreateRenderTargetView(bloomMips[m].Get(), nullptr, rtvAt(2u + m));
    }
    dev->CreateRenderTargetView(canvasClean.Get(), nullptr, rtvAt(5u));
    dev->CreateRenderTargetView(canvasAbCompare.Get(), nullptr, rtvAt(6u));

    // Dynamic SRV/UAVs: [0]=UAV sceneHdr, [1]=UAV sceneDepth, [2]=SRV sceneDepth, [3]=SRV sceneHdr,
    //                   [4..6]=SRV bloomMips[0..2], [8]=SRV canvasClean, [9]=SRV canvasAbCompare
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavHdr{};
    uavHdr.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uavHdr.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(sceneHdr.Get(), nullptr, &uavHdr, srvCpuAt(0u));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDepth{};
    uavDepth.Format = DXGI_FORMAT_R32_FLOAT;
    uavDepth.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(sceneDepth.Get(), nullptr, &uavDepth, srvCpuAt(1u));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDepth{};
    srvDepth.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDepth.Format = DXGI_FORMAT_R32_FLOAT;
    srvDepth.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDepth.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(sceneDepth.Get(), &srvDepth, srvCpuAt(2u));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvHdr = srvDepth;
    srvHdr.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dev->CreateShaderResourceView(sceneHdr.Get(), &srvHdr, srvCpuAt(3u));
    for (uint32_t m = 0; m < kNumBloomMips; ++m) {
        dev->CreateShaderResourceView(bloomMips[m].Get(), &srvHdr, srvCpuAt(4u + m));
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvClean = srvHdr;
    srvClean.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    dev->CreateShaderResourceView(canvasClean.Get(), &srvClean, srvCpuAt(8u));
    dev->CreateShaderResourceView(canvasAbCompare.Get(), &srvClean, srvCpuAt(9u));
    dev->CreateShaderResourceView(canvasClean.Get(), &srvClean, srvCpuAt(14u));
    dev->CreateShaderResourceView(canvasAbCompare.Get(), &srvClean, srvCpuAt(15u));

    return true;
}

bool M1TaipeiSliceEffect::Impl::buildDxrResourcesAndWarmUp(D3D12Context& d3d, uint64_t softVramLimitBytes) {
    dxrPipelineReady = false;
    dxrVerified = false;
    dxrHardwareCapable = false;

    if (d3d.isWarp()) {
        return false;
    }

    auto* dev = static_cast<ID3D12Device*>(d3d.deviceRaw());
    if (!dev) return false;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5))) ||
        opt5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
        return false;
    }
    dxrHardwareCapable = true;
    dxrTier12 = (static_cast<uint32_t>(opt5.RaytracingTier) >= 12u);

    ComPtr<ID3D12Device5> dev5;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dev5))) || !dev5) {
        return false;
    }

    // DXR compute root signature:
    // [0]=28 root constants (b0), [1..3]=Root SRVs (t0..t2), [4]=Verify UAV (u0),
    // [5]=1-SRV table (t3 = tensorFieldTex), [6]=2-UAV table (u1..u2 = sceneHdr, sceneDepth)
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 3; // register(t3)

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 2;
    uavRange.BaseShaderRegister = 1; // register(u1..u2)

    D3D12_ROOT_PARAMETER dxrParams[7]{};
    dxrParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    dxrParams[0].Constants.ShaderRegister = 0;
    dxrParams[0].Constants.Num32BitValues = sizeof(GpuM1SceneConstants) / 4;
    dxrParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    for (uint32_t reg = 0; reg < 3u; ++reg) {
        dxrParams[1u + reg].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        dxrParams[1u + reg].Descriptor.ShaderRegister = reg;
        dxrParams[1u + reg].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    dxrParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    dxrParams[4].Descriptor.ShaderRegister = 0;
    dxrParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    dxrParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    dxrParams[5].DescriptorTable.NumDescriptorRanges = 1;
    dxrParams[5].DescriptorTable.pDescriptorRanges = &srvRange;
    dxrParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    dxrParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    dxrParams[6].DescriptorTable.NumDescriptorRanges = 1;
    dxrParams[6].DescriptorTable.pDescriptorRanges = &uavRange;
    dxrParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 7;
    rsDesc.pParameters = dxrParams;
    ComPtr<ID3DBlob> sigBlob, errBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob)) || !sigBlob) {
        return false;
    }
    if (FAILED(dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&dxrRootSig)))) {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC cpso{};
    cpso.pRootSignature = dxrRootSig.Get();
    cpso.CS = {csDxrSceneBytes.data(), csDxrSceneBytes.size()};
    if (FAILED(dev->CreateComputePipelineState(&cpso, IID_PPV_ARGS(&psoDxrScene))) || !psoDxrScene) {
        return false;
    }

    std::vector<GpuM1Vertex> allVerts;
    std::vector<uint32_t> allIndices;
    buildProceduralM1Meshes(allVerts, allIndices, meshSlices);
    const uint32_t totalVertexCount = static_cast<uint32_t>(allVerts.size());

    std::array<D3D12_RAYTRACING_GEOMETRY_DESC, kNumBlasMeshes> geomDescs{};
    std::array<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS, kNumBlasMeshes> blasInputs{};
    std::array<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO, kNumBlasMeshes> blasPre{};
    uint64_t maxBlasScratchBytes = 256ULL;

    for (uint32_t m = 0; m < kNumBlasMeshes; ++m) {
        geomDescs[m].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geomDescs[m].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geomDescs[m].Triangles.VertexBuffer.StrideInBytes = sizeof(GpuM1Vertex);
        geomDescs[m].Triangles.VertexCount = totalVertexCount;
        geomDescs[m].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geomDescs[m].Triangles.IndexCount = meshSlices[m].indexCount;
        geomDescs[m].Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

        blasInputs[m].Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blasInputs[m].Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        blasInputs[m].NumDescs = 1;
        blasInputs[m].DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blasInputs[m].pGeometryDescs = &geomDescs[m];

        dev5->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs[m], &blasPre[m]);
        maxBlasScratchBytes = std::max<uint64_t>(maxBlasScratchBytes, blasPre[m].ScratchDataSizeInBytes);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasIn{};
    tlasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    tlasIn.NumDescs = kNumTlasInstances;
    tlasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPre{};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasIn, &tlasPre);

    const auto vbDesc = makeBufferDesc(allVerts.size() * sizeof(GpuM1Vertex));
    const auto ibDesc = makeBufferDesc(allIndices.size() * sizeof(uint32_t));
    const auto blasScratchDesc = makeBufferDesc(maxBlasScratchBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    std::array<D3D12_RESOURCE_DESC, kNumBlasMeshes> blasResDescs{};
    for (uint32_t m = 0; m < kNumBlasMeshes; ++m) {
        blasResDescs[m] = makeBufferDesc(blasPre[m].ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }
    const auto instUpDesc = makeBufferDesc(sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * kNumTlasInstances * kTotalUploadSlots);
    const auto tlasScratchDesc = makeBufferDesc(tlasPre.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto tlasResDesc = makeBufferDesc(tlasPre.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto zeroDesc = makeBufferDesc(256);
    const auto verUavDesc = makeBufferDesc(256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto verRbDesc = makeBufferDesc(256);

    dxrStaticBytes =
        singleResourceAllocBytes(dev, vbDesc) +
        singleResourceAllocBytes(dev, ibDesc) +
        singleResourceAllocBytes(dev, blasScratchDesc) +
        singleResourceAllocBytes(dev, instUpDesc) +
        singleResourceAllocBytes(dev, tlasScratchDesc) +
        singleResourceAllocBytes(dev, tlasResDesc) +
        singleResourceAllocBytes(dev, zeroDesc) +
        3ULL * singleResourceAllocBytes(dev, verUavDesc) +
        3ULL * singleResourceAllocBytes(dev, verRbDesc);
    for (uint32_t m = 0; m < kNumBlasMeshes; ++m) {
        dxrStaticBytes += singleResourceAllocBytes(dev, blasResDescs[m]);
    }

    if (softVramLimitBytes > 0) {
        const uint64_t curVram = d3d.queryEstimatedAppVramBytes();
        if (dxrStaticBytes > softVramLimitBytes || curVram > (softVramLimitBytes - dxrStaticBytes)) {
            BOO_LOG_WARN("Render", "DXR acceleration buffers exceed soft VRAM limit; substituting raster.");
            return false;
        }
    }

    auto allocBuf = [&](const D3D12_RESOURCE_DESC& rd, D3D12_HEAP_TYPE ht, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = ht;
        return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&out))) && out;
    };

    if (!allocBuf(vbDesc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, blasVbUpload) ||
        !allocBuf(ibDesc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, blasIbUpload) ||
        !allocBuf(blasScratchDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, blasScratch) ||
        !allocBuf(instUpDesc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, tlasInstanceUpload) ||
        !allocBuf(tlasScratchDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, tlasScratch) ||
        !allocBuf(tlasResDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, tlasResult) ||
        !allocBuf(zeroDesc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, zeroUploadBuf)) {
        return false;
    }

    for (uint32_t m = 0; m < kNumBlasMeshes; ++m) {
        if (!allocBuf(blasResDescs[m], D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, blasResults[m])) {
            return false;
        }
    }

    for (uint32_t s = 0; s < kFrameSlots; ++s) {
        if (!allocBuf(verUavDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, dxrVerifyUav[s]) ||
            !allocBuf(verRbDesc, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, dxrVerifyReadback[s])) {
            return false;
        }
    }

    void* ptr = nullptr;
    blasVbUpload->Map(0, nullptr, &ptr);
    std::memcpy(ptr, allVerts.data(), allVerts.size() * sizeof(GpuM1Vertex));
    blasVbUpload->Unmap(0, nullptr);

    blasIbUpload->Map(0, nullptr, &ptr);
    std::memcpy(ptr, allIndices.data(), allIndices.size() * sizeof(uint32_t));
    blasIbUpload->Unmap(0, nullptr);

    zeroUploadBuf->Map(0, nullptr, &ptr);
    std::memset(ptr, 0, 256);
    zeroUploadBuf->Unmap(0, nullptr);

    std::array<D3D12_GPU_VIRTUAL_ADDRESS, kNumBlasMeshes> blasVas{};
    for (uint32_t m = 0; m < kNumBlasMeshes; ++m) {
        geomDescs[m].Triangles.VertexBuffer.StartAddress = blasVbUpload->GetGPUVirtualAddress();
        geomDescs[m].Triangles.IndexBuffer =
            blasIbUpload->GetGPUVirtualAddress() + static_cast<uint64_t>(meshSlices[m].baseIndex) * sizeof(uint32_t);
        blasVas[m] = blasResults[m]->GetGPUVirtualAddress();
    }

    uint8_t* instMapped = nullptr;
    if (FAILED(tlasInstanceUpload->Map(0, nullptr, reinterpret_cast<void**>(&instMapped))) || !instMapped) {
        return false;
    }
    populateTaipei96Instances(
        reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(instMapped),
        blasVas,
        48.0f,
        1.0f);
    tlasInstanceUpload->Unmap(0, nullptr);

    bool recordedOk = false;
    const bool submitted = d3d.executeSingleCommandListAndWait([&](void* rawCmd) {
        auto* baseCmd = static_cast<ID3D12GraphicsCommandList*>(rawCmd);
        ComPtr<ID3D12GraphicsCommandList4> cmd4;
        if (FAILED(baseCmd->QueryInterface(IID_PPV_ARGS(&cmd4))) || !cmd4) return;

        for (uint32_t m = 0; m < kNumBlasMeshes; ++m) {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildBlas{};
            buildBlas.Inputs = blasInputs[m];
            buildBlas.ScratchAccelerationStructureData = blasScratch->GetGPUVirtualAddress();
            buildBlas.DestAccelerationStructureData = blasResults[m]->GetGPUVirtualAddress();
            cmd4->BuildRaytracingAccelerationStructure(&buildBlas, 0, nullptr);

            D3D12_RESOURCE_BARRIER blasBarriers[2]{};
            blasBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            blasBarriers[0].UAV.pResource = blasResults[m].Get();
            blasBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            blasBarriers[1].UAV.pResource = blasScratch.Get();
            cmd4->ResourceBarrier(2, blasBarriers);
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildTlas{};
        buildTlas.Inputs = tlasIn;
        buildTlas.Inputs.InstanceDescs = tlasInstanceUpload->GetGPUVirtualAddress();
        buildTlas.ScratchAccelerationStructureData = tlasScratch->GetGPUVirtualAddress();
        buildTlas.DestAccelerationStructureData = tlasResult->GetGPUVirtualAddress();
        cmd4->BuildRaytracingAccelerationStructure(&buildTlas, 0, nullptr);

        D3D12_RESOURCE_BARRIER uavB{};
        uavB.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavB.UAV.pResource = tlasResult.Get();
        cmd4->ResourceBarrier(1, &uavB);

        cmd4->CopyBufferRegion(dxrVerifyUav[0].Get(), 0, zeroUploadBuf.Get(), 0, 8);
        transitionResource(cmd4.Get(), dxrVerifyUav[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transitionResource(cmd4.Get(), sceneHdr.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transitionResource(cmd4.Get(), sceneDepth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const M1CameraState warmCam = evaluateM1Camera(48LL * 48000LL, 48000, 0.5f, false);
        GpuM1SceneConstants sc{};
        std::memcpy(sc.camEye, warmCam.eye, sizeof(sc.camEye));
        sc.fovY = warmCam.fovYRadians;
        sc.aspect = 16.0f / 9.0f;
        buildCameraBasis(warmCam, sc.camForward, sc.camRight, sc.camUp);
        sc.filmTime = 48.0f;
        sc.revealOpen01 = warmCam.revealOpen01;
        sc.canvasWidth = canvasW;
        sc.canvasHeight = canvasH;
        sc.expectSeq = 1u;
        sc.emissivePulse = warmCam.emissivePulse;
        sc.shotIndex = (warmCam.shotIndex & 0xFFu) | (meshSlices[4].baseIndex << 8u);
        for (uint32_t m = 0; m < 4u; ++m) {
            sc.meshBaseIndices[m] = meshSlices[m].baseIndex;
        }

        ID3D12DescriptorHeap* heaps[] = {srvUavHeap.Get()};
        cmd4->SetDescriptorHeaps(1, heaps);
        cmd4->SetComputeRootSignature(dxrRootSig.Get());
        cmd4->SetComputeRoot32BitConstants(0, sizeof(GpuM1SceneConstants) / 4, &sc, 0);
        cmd4->SetComputeRootShaderResourceView(1, tlasResult->GetGPUVirtualAddress());
        cmd4->SetComputeRootShaderResourceView(2, blasVbUpload->GetGPUVirtualAddress());
        cmd4->SetComputeRootShaderResourceView(3, blasIbUpload->GetGPUVirtualAddress());
        cmd4->SetComputeRootUnorderedAccessView(4, dxrVerifyUav[0]->GetGPUVirtualAddress());
        cmd4->SetComputeRootDescriptorTable(5, srvGpuAt(7u)); // t3 = tensorFieldTex
        cmd4->SetComputeRootDescriptorTable(6, srvGpuAt(0u)); // u1..u2 = sceneHdr, sceneDepth
        cmd4->SetPipelineState(psoDxrScene.Get());
        cmd4->Dispatch((canvasW + 7u) / 8u, (canvasH + 7u) / 8u, 1u);

        transitionResource(cmd4.Get(), sceneHdr.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transitionResource(cmd4.Get(), sceneDepth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transitionResource(cmd4.Get(), dxrVerifyUav[0].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd4->CopyBufferRegion(dxrVerifyReadback[0].Get(), 0, dxrVerifyUav[0].Get(), 0, 8);
        transitionResource(cmd4.Get(), dxrVerifyUav[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        recordedOk = true;
    });

    if (!submitted || !recordedOk) {
        return false;
    }

    uint32_t readWords[2]{0, 0};
    D3D12_RANGE rr{0, 8};
    void* rbPtr = nullptr;
    if (SUCCEEDED(dxrVerifyReadback[0]->Map(0, &rr, &rbPtr)) && rbPtr) {
        std::memcpy(readWords, rbPtr, sizeof(readWords));
        D3D12_RANGE wr{0, 0};
        dxrVerifyReadback[0]->Unmap(0, &wr);
    }

    if (readWords[0] != 1u || readWords[1] == 0u) {
        BOO_LOG_WARN("Render", "DXR warm-up dispatch did not produce verified ray hits; substituting raster.");
        return false;
    }

    dxrPipelineReady = true;
    dxrVerified = true;
    lastVerifiedHits = readWords[1];
    nextDxrSeq = 2u;
    BOO_LOG_INFO("Render", "DXR 1.1 112-instance Taipei 101 + Glass Orchid warm-up verified (" +
                           std::to_string(lastVerifiedHits) + " sampled ray hits).");
    return true;
}

void M1TaipeiSliceEffect::Impl::updateLiveRtStatus(bool disableRT, bool presetAllowsDxr) noexcept {
    if(trilogyMesh) { dxrVerified=trilogyMesh->hardwareVerified(); lastVerifiedHits=trilogyMesh->verifiedHits(); }
    HardwareResult r{};
    r.engine = EngineKind::RT;
    r.requestedBackend = dxrTier12 ? "D3D12_DXR_1_2_RayQuery" : "D3D12_DXR_1_1_RayQuery";
    r.capabilityDetected = dxrHardwareCapable;

    const bool activeDxr = dxrPipelineReady && !dxrQuarantined && !disableRT && presetAllowsDxr && dxrVerified;
    const float rtMs = std::max(0.05f, lastRtSceneMs);
    if (activeDxr) {
        r.status = BackendStatus::Accelerated;
        r.health = HealthState::ActiveAccelerated;
        r.activeBackend = dxrTier12 ? "D3D12_DXR_1_2_RayQuery" : "D3D12_DXR_1_1_RayQuery";
        r.successfullyExecutedTestWork = true;
        r.diagnosticCode = "DXR_LIVE_VERIFIED";
        char msg[224]{};
        std::snprintf(msg, sizeof(msg),
                      trilogy ? "Live M2 Blender triangle TLAS inline RayQuery verified (%u sampled hits)." : "Live 112-instance Taipei 101 + Glass Orchid TLAS + inline RayQuery verified (%u sampled hits).",
                      lastVerifiedHits);
        r.humanReadableReason = msg;

        const uint32_t approxRays = trilogyMesh ? trilogyMesh->queriedRays() : std::max<uint32_t>(canvasW * canvasH, lastVerifiedHits * 256u);
        livePillars[0].status = BackendStatus::Accelerated;
        livePillars[0].activeInScene = true;
        livePillars[0].hardwareVerified = true;
        livePillars[0].workloadItems = approxRays;
        livePillars[0].workloadSteps = trilogyMesh ? 3u : kNumTlasInstances;
        livePillars[0].executionMs = rtMs;
        livePillars[0].throughput = static_cast<float>(static_cast<double>(approxRays) / (static_cast<double>(rtMs) * 1000.0));
        livePillars[0].auxMetric = static_cast<float>(approxRays) * 1.0e-6f;
    } else {
        r.status = BackendStatus::Substituted;
        r.health = HealthState::ActiveSubstitute;
        r.activeBackend = "D3D12_Raster_Safe_M1";
        r.successfullyExecutedTestWork = false;
        if (disableRT) {
            r.diagnosticCode = "DISABLED_BY_FLAG";
            r.humanReadableReason = "DXR disabled via CLI/runtime toggle; rendering matching M1 SM 6.0 raster substitute.";
        } else if (!presetAllowsDxr) {
            r.diagnosticCode = "PRESET_RASTER_MODE";
            r.humanReadableReason = "Active preset uses M1 SM 6.0 raster substitute.";
        } else if (dxrQuarantined) {
            r.diagnosticCode = "DXR_RUNTIME_QUARANTINED";
            r.humanReadableReason = "DXR verification missed expected ray count; quarantined to M1 raster until scene cut.";
        } else {
            r.diagnosticCode = "DXR_UNAVAILABLE_RASTER_ACTIVE";
            r.humanReadableReason = "Hardware DXR unavailable (or WARP active); rendering M1 SM 6.0 raster substitute.";
        }

        const uint32_t pixCount = std::max<uint32_t>(1u, canvasW * canvasH);
        livePillars[0].status = BackendStatus::Substituted;
        livePillars[0].activeInScene = true;
        livePillars[0].hardwareVerified = false;
        livePillars[0].workloadItems = pixCount;
        livePillars[0].workloadSteps = 1u;
        livePillars[0].executionMs = rtMs;
        livePillars[0].throughput = static_cast<float>(static_cast<double>(pixCount) / (static_cast<double>(rtMs) * 1000.0));
        livePillars[0].auxMetric = static_cast<float>(pixCount) * 1.0e-6f;
    }
    liveRtStatus = std::move(r);
}

void M1TaipeiSliceEffect::Impl::updateLiveAuxiliaryStatuses(
    bool isWarp,
    bool disableCUDA,
    bool presetAllowsCuda,
    bool disableTensor,
    bool presetAllowsTensor,
    bool disableCodec,
    bool codecViewActive) noexcept {
    // Pillar 1: CUDA 65,536..262,144-particle 16-substep 3D curl-vortex simulation vs 8,192 HLSL linear substitute
    {
        HardwareResult c{};
        c.engine = EngineKind::CUDA;
        c.requestedBackend = (livePillars[1].workloadItems >= LiveAcceleratorOrchestra::kSoloCudaParticleCount)
                                 ? "CUDA_Driver_PTX_100K_Mutiny"
                                 : "CUDA_Driver_PTX_65K_Vortex";
        c.capabilityDetected = !isWarp && liveAccel.cudaHardwareAvailable();
        if (!isWarp && !disableCUDA && presetAllowsCuda && livePillars[1].hardwareVerified) {
            c.status = BackendStatus::Accelerated;
            c.health = HealthState::ActiveAccelerated;
            c.activeBackend = liveAccel.cudaBackendName();
            c.successfullyExecutedTestWork = true;
            c.diagnosticCode = "CUDA_LIVE_PARTICLES_VERIFIED";
            char msg[160]{};
            std::snprintf(msg, sizeof(msg),
                          "Live %u-particle 16-substep CUDA PTX 3D curl-vortex simulation verified on screen.",
                          livePillars[1].workloadItems);
            c.humanReadableReason = msg;
        } else {
            c.status = BackendStatus::Substituted;
            c.health = HealthState::ActiveSubstitute;
            c.activeBackend = "D3D12_HLSL_LinearParticles_8K";
            c.successfullyExecutedTestWork = false;
            c.diagnosticCode = disableCUDA ? "DISABLED_BY_FLAG" : (!presetAllowsCuda ? "PRESET_SUBSTITUTE" : "CUDA_SUBSTITUTE_ACTIVE");
            c.humanReadableReason = "Rendering 8,192-particle 2-step linear HLSL compute substitute.";
            if (!livePillars[1].hardwareVerified) {
                livePillars[1].status = BackendStatus::Substituted;
                livePillars[1].activeInScene = true;
                livePillars[1].workloadItems = LiveAcceleratorOrchestra::kSubParticleCount;
                livePillars[1].workloadSteps = LiveAcceleratorOrchestra::kSubSubsteps;
                if (livePillars[1].executionMs <= 0.0f) livePillars[1].executionMs = 0.08f;
                livePillars[1].throughput = (8192.0f * 2.0f) / (std::max(0.02f, livePillars[1].executionMs) * 1000.0f);
            }
        }
        liveCudaStatus = std::move(c);
    }

    // Pillar 2: Tensor Core FP16->FP32 mma.sync 64-layer SIREN caustic field vs scalar HLSL field substitute
    {
        HardwareResult t{};
        t.engine = EngineKind::Tensor;
        t.requestedBackend = "PTX_MMA_Sync_m16n8k16_SIREN64";
        t.capabilityDetected = !isWarp && liveAccel.tensorHardwareAvailable();
        if (!isWarp && !disableTensor && presetAllowsTensor && livePillars[2].hardwareVerified) {
            t.status = BackendStatus::Accelerated;
            t.health = HealthState::ActiveAccelerated;
            t.activeBackend = liveAccel.tensorBackendName();
            t.successfullyExecutedTestWork = true;
            t.diagnosticCode = "TENSOR_LIVE_MMA_VERIFIED";
            t.humanReadableReason = "Live 320x180 64-layer Tensor Core mma.sync.m16n8k16 neural caustic field verified on screen.";
        } else {
            t.status = BackendStatus::Substituted;
            t.health = HealthState::ActiveSubstitute;
            t.activeBackend = "D3D12_HLSL_ScalarField_Sub";
            t.successfullyExecutedTestWork = false;
            t.diagnosticCode = disableTensor ? "DISABLED_BY_FLAG" : (!presetAllowsTensor ? "PRESET_SUBSTITUTE" : "TENSOR_SUBSTITUTE_ACTIVE");
            t.humanReadableReason = "Rendering scalar HLSL baseline caustic field substitute.";
            if (!livePillars[2].hardwareVerified) {
                livePillars[2].status = BackendStatus::Substituted;
                livePillars[2].activeInScene = true;
                livePillars[2].workloadItems = LiveAcceleratorOrchestra::kTensorWidth * LiveAcceleratorOrchestra::kTensorHeight;
                livePillars[2].workloadSteps = 1u;
                if (livePillars[2].executionMs <= 0.0f) livePillars[2].executionMs = 0.05f;
                livePillars[2].throughput = 0.04f;
            }
        }
        liveTensorStatus = std::move(t);
    }

    // Pillar 3: Hardware Media Foundation H.264 Encode+Decode round-trip (`V` Signal Hijack or Codec A/B)
    {
        HardwareResult v{};
        v.engine = EngineKind::Codec;
        v.requestedBackend = "MF_Hardware_H264_RoundTrip_720p";
        v.capabilityDetected = !isWarp && liveAccel.codecHardwareAvailable();
        if (!codecViewActive) {
            v.status = v.capabilityDetected ? BackendStatus::Substituted : BackendStatus::Unavailable;
            v.health = v.capabilityDetected ? HealthState::ActiveSubstitute : HealthState::Unavailable;
            v.activeBackend = "Direct_10Bit_CanvasClean_Bypass";
            v.successfullyExecutedTestWork = false;
            v.diagnosticCode = "CODEC_BYPASS_CLEAN_ACTIVE";
            v.humanReadableReason = "Codec round-trip on standby (press V or cycle A/B to CODEC to inspect live H.264 encode+decode).";
            livePillars[3].status = v.status;
            livePillars[3].activeInScene = false;
            livePillars[3].hardwareVerified = false;
        } else if (!isWarp && !disableCodec && livePillars[3].hardwareVerified && codecHwTexValid) {
            v.status = BackendStatus::Accelerated;
            v.health = HealthState::ActiveAccelerated;
            v.activeBackend = liveAccel.codecBackendName();
            v.successfullyExecutedTestWork = true;
            v.diagnosticCode = "CODEC_LIVE_ROUNDTRIP_VERIFIED";
            v.humanReadableReason = "Live 1280x720 Hardware H.264 MFT encode -> bitstream -> H.264 decode round-trip verified on screen.";
        } else {
            v.status = BackendStatus::Substituted;
            v.health = HealthState::ActiveSubstitute;
            v.activeBackend = "D3D12_HLSL_BlockQuant_Substitute";
            v.successfullyExecutedTestWork = false;
            v.diagnosticCode = disableCodec ? "DISABLED_BY_FLAG" : "CODEC_SHADER_QUANT_SUBSTITUTE";
            v.humanReadableReason = "Displaying shader 8x8 macroblock quantization substitute.";
            livePillars[3].status = BackendStatus::Substituted;
            livePillars[3].activeInScene = true;
            livePillars[3].hardwareVerified = false;
            livePillars[3].workloadItems = LiveAcceleratorOrchestra::kCodecWidth * LiveAcceleratorOrchestra::kCodecHeight;
            livePillars[3].workloadSteps = 950u;
            livePillars[3].executionMs = 0.09f;
            livePillars[3].throughput = 60.0f;
            livePillars[3].qualityOrBitrate = 27.5f;
            livePillars[3].auxMetric = 0.95f;
        }
        liveCodecStatus = std::move(v);
    }
}

void M1TaipeiSliceEffect::Impl::retireVerifySlot(uint32_t slot) noexcept {
    if(trilogyMesh) { trilogyMesh->retire(slot); trilogyMesh->retire(kHeadlessSlot); return; }
    if (slot >= kFrameSlots) return;
    auto& pend = slotPending[slot];
    if (!pend.active) return;

    const bool matchingGen = (pend.generation == dxrGeneration);
    const uint32_t expSeq = pend.expectedSeq;
    pend.active = false;
    if (!matchingGen) return;

    uint32_t words[2]{0, 0};
    D3D12_RANGE rr{0, 8};
    void* mapped = nullptr;
    if (dxrVerifyReadback[slot] && SUCCEEDED(dxrVerifyReadback[slot]->Map(0, &rr, &mapped)) && mapped) {
        std::memcpy(words, mapped, sizeof(words));
        D3D12_RANGE wr{0, 0};
        dxrVerifyReadback[slot]->Unmap(0, &wr);
    }

    if (words[0] == expSeq && words[1] > 0u) {
        dxrVerified = true;
        lastVerifiedHits = words[1];
    } else {
        dxrVerified = false;
        dxrQuarantined = true;
        ++dxrGeneration;
        BOO_LOG_WARN("Render", "Live DXR verification slot missed expected ray hits; quarantining to M1 raster until next scene cut.");
    }
}

void M1TaipeiSliceEffect::Impl::readRtTimestampSlot(uint32_t slot) noexcept {
    if (slot >= kTotalUploadSlots || !rtTsSlotValid[slot] || !rtTsReadback || gpuTimestampFreq == 0) {
        return;
    }
    rtTsSlotValid[slot] = false;
    const size_t byteOff = static_cast<size_t>(slot) * 16u;
    D3D12_RANGE rr{byteOff, byteOff + 16u};
    const uint8_t* mapped = nullptr;
    if (SUCCEEDED(rtTsReadback->Map(0, &rr, reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped)))) && mapped) {
        uint64_t ticks[2]{0, 0};
        std::memcpy(ticks, mapped + byteOff, 16u);
        D3D12_RANGE wr{0, 0};
        rtTsReadback->Unmap(0, &wr);
        if (ticks[1] > ticks[0]) {
            const double ms = static_cast<double>(ticks[1] - ticks[0]) * 1000.0 / static_cast<double>(gpuTimestampFreq);
            if (ms > 0.01 && ms < 250.0) {
                lastRtSceneMs = static_cast<float>(ms);
            }
        }
    }
}

HardwareResult M1TaipeiSliceEffect::probeAndInitialize() {
    HardwareResult res{};
    res.engine = EngineKind::RT;
    res.requestedBackend = "D3D12_M1_TaipeiSlice";

    if (simulateInitFailure_) {
        res.status = BackendStatus::Faulted;
        res.health = HealthState::Faulted;
        res.diagnosticCode = "INJECTED_SHADER_FAILURE";
        res.humanReadableReason = "Simulated primary M1 shader failure (--inject-shader-fail).";
        return res;
    }

    auto* dev = static_cast<ID3D12Device*>(d3d_.deviceRaw());
    if (!dev) {
        res.status = BackendStatus::Unavailable;
        res.health = HealthState::Unavailable;
        res.diagnosticCode = "NO_D3D12_DEVICE";
        res.humanReadableReason = "D3D12 device is null.";
        return res;
    }

    if (!readShaderCso("assets/shaders/m0_test_vs.cso", impl_->vsFullscreenBytes) ||
        !readShaderCso("assets/shaders/m1_dxr_scene.cso", impl_->csDxrSceneBytes) ||
        !readShaderCso("assets/shaders/m1_raster_scene.cso", impl_->psRasterSceneBytes) ||
        !readShaderCso("assets/shaders/m1_bloom_down.cso", impl_->psBloomDownBytes) ||
        !readShaderCso("assets/shaders/m1_bloom_up.cso", impl_->psBloomUpBytes) ||
        !readShaderCso("assets/shaders/m1_composite.cso", impl_->psCompositeBytes) ||
        !readShaderCso("assets/shaders/m1_present_hud.cso", impl_->psPresentHudBytes) ||
        !readShaderCso("assets/shaders/m1_particles_sub_cs.cso", impl_->csParticlesSubBytes) ||
        !readShaderCso("assets/shaders/m1_particles_vs.cso", impl_->vsParticlesBytes) ||
        !readShaderCso("assets/shaders/m1_particles_ps.cso", impl_->psParticlesBytes) ||
        !readShaderCso("assets/shaders/m1_tensor_sub_cs.cso", impl_->csTensorSubBytes) ||
        !readShaderCso("assets/shaders/m1_nv12_pack_cs.cso", impl_->csNv12PackBytes)) {
        res.status = BackendStatus::Faulted;
        res.health = HealthState::Faulted;
        res.diagnosticCode = "M1_SHADER_CSO_MISSING";
        res.humanReadableReason = "One or more compiled M1 .cso shader binaries could not be loaded.";
        return res;
    }

    D3D12_STATIC_SAMPLER_DESC linClampPs{};
    linClampPs.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linClampPs.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linClampPs.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linClampPs.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linClampPs.MaxLOD = D3D12_FLOAT32_MAX;
    linClampPs.ShaderRegister = 0;
    linClampPs.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // 1. Graphics root signature (Raster Scene, Bloom Down/Up): [0]=28 root constants (b0), [1]=1-SRV table (t0)
    //    Composite gets its own 3-SRV root signature (1b) so Bloom Down/Up never bind an SRV range overlapping a bound RTV.
    auto buildPostRootSig = [&](uint32_t srvCount, ComPtr<ID3D12RootSignature>& outSig) -> bool {
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = srvCount;
        srvRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER gfxParams[2]{};
        gfxParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        gfxParams[0].Constants.ShaderRegister = 0;
        gfxParams[0].Constants.Num32BitValues = sizeof(GpuM1SceneConstants) / 4;
        gfxParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        gfxParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        gfxParams[1].DescriptorTable.NumDescriptorRanges = 1;
        gfxParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
        gfxParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = gfxParams;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &linClampPs;

        ComPtr<ID3DBlob> sigBlob, errBlob;
        return SUCCEEDED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob)) && sigBlob &&
               SUCCEEDED(dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&outSig))) && outSig;
    };
    if (!buildPostRootSig(1u, impl_->gfxRootSig) ||
        !buildPostRootSig(3u, impl_->compositeRootSig)) {
        res.status = BackendStatus::Faulted;
        res.health = HealthState::Faulted;
        res.diagnosticCode = "M1_GFX_ROOTSIG_FAILED";
        res.humanReadableReason = "Failed to create M1 graphics root signature.";
        return res;
    }

    // 2. Present root signature (288-byte Root CBV + 3-SRV table t0..t2 for CanvasClean, CanvasAbCompare, CodecDecoded)
    {
        D3D12_DESCRIPTOR_RANGE presentSrvRange{};
        presentSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        presentSrvRange.NumDescriptors = 4; // t3 = localized text atlas
        presentSrvRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER presParams[2]{};
        presParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        presParams[0].Descriptor.ShaderRegister = 0;
        presParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        presParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        presParams[1].DescriptorTable.NumDescriptorRanges = 1;
        presParams[1].DescriptorTable.pDescriptorRanges = &presentSrvRange;
        presParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = presParams;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &linClampPs;

        ComPtr<ID3DBlob> sigBlob, errBlob;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob)) || !sigBlob ||
            FAILED(dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&impl_->presentRootSig)))) {
            res.status = BackendStatus::Faulted;
            res.health = HealthState::Faulted;
            res.diagnosticCode = "M1_PRESENT_ROOTSIG_FAILED";
            res.humanReadableReason = "Failed to create M1 present root signature.";
            return res;
        }
    }

    // 3. 3D Particle graphics root signature: [0]=28 root constants (b0 ALL), [1]=Root SRV (t0 VS), [2]=1-SRV table (t0 PS)
    {
        D3D12_DESCRIPTOR_RANGE depthSrvRange{};
        depthSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        depthSrvRange.NumDescriptors = 1;
        depthSrvRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER partParams[3]{};
        partParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        partParams[0].Constants.ShaderRegister = 0;
        partParams[0].Constants.Num32BitValues = sizeof(GpuM1SceneConstants) / 4;
        partParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        partParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        partParams[1].Descriptor.ShaderRegister = 0;
        partParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        partParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        partParams[2].DescriptorTable.NumDescriptorRanges = 1;
        partParams[2].DescriptorTable.pDescriptorRanges = &depthSrvRange;
        partParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 3;
        rsDesc.pParameters = partParams;

        ComPtr<ID3DBlob> sigBlob, errBlob;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob)) || !sigBlob ||
            FAILED(dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&impl_->particlesGfxRootSig)))) {
            res.status = BackendStatus::Faulted;
            res.health = HealthState::Faulted;
            res.diagnosticCode = "M1_PARTICLES_ROOTSIG_FAILED";
            res.humanReadableReason = "Failed to create M1 3D particle root signature.";
            return res;
        }
    }

    // 4. Auxiliary compute root signature (Particles Sub CS, Tensor Sub CS, NV12 Pack CS): [0]=28 DWORDs, [1]=t0 SRV, [2]=u0 UAV
    {
        D3D12_DESCRIPTOR_RANGE csSrvRange{};
        csSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        csSrvRange.NumDescriptors = 1;
        csSrvRange.BaseShaderRegister = 0;

        D3D12_DESCRIPTOR_RANGE csUavRange{};
        csUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        csUavRange.NumDescriptors = 1;
        csUavRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER auxParams[3]{};
        auxParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        auxParams[0].Constants.ShaderRegister = 0;
        auxParams[0].Constants.Num32BitValues = sizeof(GpuM1SceneConstants) / 4;
        auxParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        auxParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        auxParams[1].DescriptorTable.NumDescriptorRanges = 1;
        auxParams[1].DescriptorTable.pDescriptorRanges = &csSrvRange;
        auxParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        auxParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        auxParams[2].DescriptorTable.NumDescriptorRanges = 1;
        auxParams[2].DescriptorTable.pDescriptorRanges = &csUavRange;
        auxParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC linClampAll = linClampPs;
        linClampAll.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 3;
        rsDesc.pParameters = auxParams;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &linClampAll;

        ComPtr<ID3DBlob> sigBlob, errBlob;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob)) || !sigBlob ||
            FAILED(dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&impl_->auxComputeRootSig)))) {
            res.status = BackendStatus::Faulted;
            res.health = HealthState::Faulted;
            res.diagnosticCode = "M1_AUX_CS_ROOTSIG_FAILED";
            res.humanReadableReason = "Failed to create M1 auxiliary compute root signature.";
            return res;
        }
    }

    auto makeGfxPso = [&](ID3D12RootSignature* rs,
                          const std::vector<uint8_t>& vsBytes,
                          const std::vector<uint8_t>& psBytes,
                          DXGI_FORMAT rtvFmt0,
                          DXGI_FORMAT rtvFmt1,
                          bool additive,
                          ComPtr<ID3D12PipelineState>& outPso) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs;
        pd.VS = {vsBytes.data(), vsBytes.size()};
        pd.PS = {psBytes.data(), psBytes.size()};
        const uint32_t numRts = (rtvFmt1 != DXGI_FORMAT_UNKNOWN) ? 2u : 1u;
        for (uint32_t i = 0; i < numRts; ++i) {
            auto& b = pd.BlendState.RenderTarget[i];
            b.BlendEnable = (additive && i == 0u) ? TRUE : FALSE;
            b.SrcBlend = D3D12_BLEND_ONE;
            b.DestBlend = D3D12_BLEND_ONE;
            b.BlendOp = D3D12_BLEND_OP_ADD;
            b.SrcBlendAlpha = D3D12_BLEND_ONE;
            b.DestBlendAlpha = D3D12_BLEND_ZERO;
            b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = numRts;
        pd.RTVFormats[0] = rtvFmt0;
        pd.RTVFormats[1] = rtvFmt1;
        pd.SampleDesc.Count = 1;
        return SUCCEEDED(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&outPso))) && outPso;
    };

    auto makeCsPso = [&](ID3D12RootSignature* rs, const std::vector<uint8_t>& csBytes, ComPtr<ID3D12PipelineState>& outPso) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cp{};
        cp.pRootSignature = rs;
        cp.CS = {csBytes.data(), csBytes.size()};
        return SUCCEEDED(dev->CreateComputePipelineState(&cp, IID_PPV_ARGS(&outPso))) && outPso;
    };

    if (!makeGfxPso(impl_->gfxRootSig.Get(), impl_->vsFullscreenBytes, impl_->psRasterSceneBytes,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, false, impl_->psoRasterScene) ||
        !makeGfxPso(impl_->gfxRootSig.Get(), impl_->vsFullscreenBytes, impl_->psBloomDownBytes,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, false, impl_->psoBloomDown) ||
        !makeGfxPso(impl_->gfxRootSig.Get(), impl_->vsFullscreenBytes, impl_->psBloomUpBytes,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, true, impl_->psoBloomUpAdditive) ||
        !makeGfxPso(impl_->compositeRootSig.Get(), impl_->vsFullscreenBytes, impl_->psCompositeBytes,
                    DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN, false, impl_->psoCompositeCanvas) ||
        !makeGfxPso(impl_->particlesGfxRootSig.Get(), impl_->vsParticlesBytes, impl_->psParticlesBytes,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN, true, impl_->psoParticles3D) ||
        !makeCsPso(impl_->auxComputeRootSig.Get(), impl_->csParticlesSubBytes, impl_->psoParticlesSubCs) ||
        !makeCsPso(impl_->auxComputeRootSig.Get(), impl_->csTensorSubBytes, impl_->psoTensorSubCs) ||
        !makeCsPso(impl_->auxComputeRootSig.Get(), impl_->csNv12PackBytes, impl_->psoNv12PackCs) ||
        !impl_->ensurePresentPso(dev, static_cast<DXGI_FORMAT>(d3d_.backBufferDxgiFormat()))) {
        res.status = BackendStatus::Faulted;
        res.health = HealthState::Faulted;
        res.diagnosticCode = "M1_GFX_PSO_FAILED";
        res.humanReadableReason = "Failed to create M1 graphics or compute pipeline states.";
        return res;
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHd{};
    rtvHd.NumDescriptors = 7;
    rtvHd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    D3D12_DESCRIPTOR_HEAP_DESC suHd{};
    suHd.NumDescriptors = 18;
    suHd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    suHd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&rtvHd, IID_PPV_ARGS(&impl_->rtvHeap))) ||
        FAILED(dev->CreateDescriptorHeap(&suHd, IID_PPV_ARGS(&impl_->srvUavHeap)))) {
        res.status = BackendStatus::Faulted;
        res.health = HealthState::Faulted;
        res.diagnosticCode = "M1_HEAP_CREATE_FAILED";
        res.humanReadableReason = "Failed to allocate M1 RTV/SRV/UAV descriptor heaps.";
        return res;
    }
    impl_->rtvDescSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    impl_->srvUavDescSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // t3 of the present pass is a null view until a translated catalog needs the text atlas.
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nullText{};
        nullText.Format = DXGI_FORMAT_R8_UNORM;
        nullText.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullText.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullText.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(nullptr, &nullText, impl_->srvCpuAt(17u));
    }

    if (!impl_->allocateStaticPillarResources(d3d_)) {
        res.status = BackendStatus::Faulted;
        res.health = HealthState::Faulted;
        res.diagnosticCode = "M1_PILLAR_ALLOC_FAILED";
        res.humanReadableReason = "Failed to allocate M1 4-pillar static GPU buffers and textures.";
        return res;
    }

    uint32_t targetW = std::clamp<uint32_t>(activeSpec_.renderWidth, 320u, 3840u);
    uint32_t targetH = std::clamp<uint32_t>(activeSpec_.renderHeight, 180u, 2160u);

    if (!impl_->buildCanvasBundle(d3d_, targetW, targetH, activeSpec_.softVramLimitBytes)) {
        activeSpec_.id = QualityPresetId::Safe;
        activeSpec_.renderWidth = 1280;
        activeSpec_.renderHeight = 720;
        activeSpec_.allowDxr = false;
        activeSpec_.allowCuda = false;
        activeSpec_.allowTensor = false;
        if (!impl_->buildCanvasBundle(d3d_, 1280u, 720u, 0ULL)) {
            res.status = BackendStatus::Faulted;
            res.health = HealthState::Faulted;
            res.diagnosticCode = "M1_CANVAS_ALLOC_FAILED";
            res.humanReadableReason = "Failed to allocate M1 offscreen canvas bundle even at Safe 1280x720.";
            return res;
        }
    }

    // Deterministically initialize default-heap Tensor field, Particle buffer, and Codec texture before any warm-up/render
    d3d_.executeSingleCommandListAndWait([&](void* rawCmd) {
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(rawCmd);
        ID3D12DescriptorHeap* heaps[] = {impl_->srvUavHeap.Get()};
        cmd->SetDescriptorHeaps(1, heaps);

        transitionResource(cmd, impl_->codecDecodedTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dstCodec{};
        dstCodec.pResource = impl_->codecDecodedTex.Get();
        dstCodec.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstCodec.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION srcCodec{};
        srcCodec.pResource = impl_->codecRgbaUploadBuf[0].Get();
        srcCodec.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcCodec.PlacedFootprint = impl_->codecFootprint;
        cmd->CopyTextureRegion(&dstCodec, 0, 0, 0, &srcCodec, nullptr);
        transitionResource(cmd, impl_->codecDecodedTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        transitionResource(cmd, impl_->tensorFieldTex.Get(), kTensorReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        GpuM1TensorSubConstants tsc{0.0f, 0.0f, 0u, 0u};
        cmd->SetComputeRootSignature(impl_->auxComputeRootSig.Get());
        cmd->SetPipelineState(impl_->psoTensorSubCs.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(GpuM1TensorSubConstants) / 4, &tsc, 0);
        cmd->SetComputeRootDescriptorTable(2, impl_->srvGpuAt(11u));
        cmd->Dispatch(40u, 23u, 1u);
        transitionResource(cmd, impl_->tensorFieldTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kTensorReadState);

        transitionResource(cmd, impl_->particleGpuBuf.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        GpuM1SceneConstants initSc{};
        cmd->SetPipelineState(impl_->psoParticlesSubCs.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(GpuM1SceneConstants) / 4, &initSc, 0);
        cmd->SetComputeRootDescriptorTable(2, impl_->srvGpuAt(13u));
        cmd->Dispatch(128u, 1u, 1u);
        transitionResource(cmd, impl_->particleGpuBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    });

    impl_->ensureLiveAccelerators(
        d3d_,
        !disableCUDA_ && activeSpec_.allowCuda,
        !disableTensor_ && activeSpec_.allowTensor,
        !disableCodec_ && (codecHijackActive_ || abCompareEngine_ == AbCompareEngine::Codec));

    // Warm up live CUDA & Tensor once at t=48s if enabled so initial queryLiveEngineStatus reflects verified live hardware
    impl_->activeSpecForParticles = activeSpec_;
    if (!d3d_.isWarp()) {
        if (!disableCUDA_ && activeSpec_.allowCuda && impl_->liveAccel.cudaHardwareAvailable()) {
            LivePillarTelemetry cudaTel{};
            uint32_t warmCount = LiveAcceleratorOrchestra::kCudaParticleCount;
            uint32_t warmMode = 0u;
            impl_->resolveCudaSimParams(0, 48000, activeSpec_, warmCount, warmMode);
            impl_->liveAccel.simulateCudaParticles(48LL * 48000LL, 48000, 0xB005A5A4u, Edition::Family, 1.0f, cudaTel, warmCount, warmMode);
            impl_->livePillars[1] = cudaTel;
        }
        if (!disableTensor_ && activeSpec_.allowTensor && impl_->liveAccel.tensorHardwareAvailable()) {
            LivePillarTelemetry tensorTel{};
            impl_->liveAccel.evaluateTensorCausticField(48LL * 48000LL, 48000, 0xB005A5A4u, Edition::Family, 1.0f, tensorTel);
            impl_->livePillars[2] = tensorTel;
        }
    }

    if (!disableRT_ && !d3d_.isWarp()) {
        impl_->buildDxrResourcesAndWarmUp(d3d_, activeSpec_.softVramLimitBytes);
        if (!activeSpec_.allowDxr) {
            impl_->dxrVerified = false;
        }
    }

    if (impl_->trilogy && !impl_->trilogyMesh) {
        impl_->trilogyMesh=std::make_unique<TrilogyMeshRenderer>(d3d_);
        std::string error;
        if(!impl_->trilogyMesh->initialize(impl_->injectMeshFailure,&error,impl_->visualMode,impl_->tensorFieldTex.Get())) throw std::runtime_error(error);
    }
    if(impl_->trilogyMesh && !impl_->trilogyMesh->resize(impl_->canvasW,impl_->canvasH)) throw std::runtime_error("M2 depth allocation failed");
    if (impl_->trilogyMesh && !disableRT_ && activeSpec_.allowDxr && !d3d_.isWarp()) {
        FrameContext warmCtx{};
        warmCtx.samplePosition = 720000;
        warmCtx.sampleRate = 48000;
        warmCtx.filmTimeSeconds = 15.0;
        M1CameraState warmCam{};
        (void)impl_->buildSceneConstants(warmCtx, warmCam);
        d3d_.executeSingleCommandListAndWait([&](void* rawCmd) {
            auto* cmd = static_cast<ID3D12GraphicsCommandList*>(rawCmd);
            ID3D12DescriptorHeap* heaps[] = {impl_->srvUavHeap.Get()};
            cmd->SetDescriptorHeaps(1, heaps);
            impl_->trilogyMesh->record(
                cmd, warmCtx, warmCam,
                impl_->sceneHdr.Get(), impl_->sceneDepth.Get(),
                impl_->srvGpuAt(7u).ptr, impl_->canvasW, impl_->canvasH,
                kHeadlessSlot, true, true);
            cmd->SetDescriptorHeaps(1, heaps);
        });
        impl_->trilogyMesh->retire(kHeadlessSlot);
    }
    impl_->updateLiveRtStatus(disableRT_, activeSpec_.allowDxr);
    impl_->updateLiveAuxiliaryStatuses(
        d3d_.isWarp(),
        disableCUDA_, activeSpec_.allowCuda,
        disableTensor_, activeSpec_.allowTensor,
        disableCodec_, codecHijackActive_);
    return impl_->liveRtStatus;
}

void M1TaipeiSliceEffect::preload(double /*atFilmTime*/) {}

void M1TaipeiSliceEffect::onSceneBoundary(uint32_t /*actIndex*/, double /*filmTimeSeconds*/) noexcept {
    if (!impl_) return;
    impl_->dxrQuarantined = false;
    impl_->slotPending = {};
    ++impl_->dxrGeneration;
}

void M1TaipeiSliceEffect::resetTo(double /*filmTime*/) {
    if (!impl_) return;
    impl_->dxrQuarantined = false;
    impl_->slotPending = {};
    impl_->nv12SlotReady = {false, false, false};
    ++impl_->dxrGeneration;
}

bool M1TaipeiSliceEffect::syncPresetBeforeFrame(const QualityPresetSpec& spec, bool allowImmediateResize) {
    if (!impl_ || !impl_->sceneHdr) return false;

    const uint32_t reqW = std::clamp<uint32_t>(spec.renderWidth, 320u, 3840u);
    const uint32_t reqH = std::clamp<uint32_t>(spec.renderHeight, 180u, 2160u);

    const bool allowDxrChanged = (activeSpec_.allowDxr != spec.allowDxr);
    activeSpec_ = spec;
    impl_->activeSpecForParticles = spec;

    if ((reqW != impl_->canvasW || reqH != impl_->canvasH) && allowImmediateResize) {
        d3d_.waitForGpu();
        if (!d3d_.checkDeviceHealthy()) return false;
        if (!impl_->buildCanvasBundle(d3d_, reqW, reqH, spec.softVramLimitBytes)) {
            activeSpec_.id = QualityPresetId::Safe;
            activeSpec_.renderWidth = 1280;
            activeSpec_.renderHeight = 720;
            activeSpec_.allowDxr = false;
            activeSpec_.allowCuda = false;
            activeSpec_.allowTensor = false;
            if (!impl_->buildCanvasBundle(d3d_, 1280u, 720u, 0ULL)) {
                return false;
            }
        }
    }

    impl_->ensureLiveAccelerators(
        d3d_,
        !disableCUDA_ && activeSpec_.allowCuda,
        !disableTensor_ && activeSpec_.allowTensor,
        !disableCodec_ && (codecHijackActive_ || abCompareEngine_ == AbCompareEngine::Codec));

    if (allowDxrChanged) {
        impl_->slotPending = {};
        ++impl_->dxrGeneration;
        if (!activeSpec_.allowDxr) {
            impl_->dxrVerified = false;
        } else if (!disableRT_ && !impl_->dxrPipelineReady && !d3d_.isWarp()) {
            impl_->buildDxrResourcesAndWarmUp(d3d_, activeSpec_.softVramLimitBytes);
        }
    }
    if (impl_->trilogy && !impl_->trilogyMesh) {
        impl_->trilogyMesh=std::make_unique<TrilogyMeshRenderer>(d3d_);
        std::string error;
        if(!impl_->trilogyMesh->initialize(impl_->injectMeshFailure,&error,impl_->visualMode,impl_->tensorFieldTex.Get())) throw std::runtime_error(error);
    }
    if(impl_->trilogyMesh && !impl_->trilogyMesh->resize(impl_->canvasW,impl_->canvasH)) throw std::runtime_error("M2 depth allocation failed");
    impl_->updateLiveRtStatus(disableRT_, activeSpec_.allowDxr);
    impl_->updateLiveAuxiliaryStatuses(
        d3d_.isWarp(),
        disableCUDA_, activeSpec_.allowCuda,
        disableTensor_, activeSpec_.allowTensor,
        disableCodec_, codecHijackActive_);
    return true;
}

void M1TaipeiSliceEffect::setDisableRt(bool disable) noexcept {
    if (disableRT_ == disable) return;
    disableRT_ = disable;
    if (!impl_) return;
    impl_->slotPending = {};
    ++impl_->dxrGeneration;
    if (disableRT_) {
        impl_->dxrVerified = false;
    } else if (!impl_->dxrPipelineReady && !d3d_.isWarp()) {
        impl_->buildDxrResourcesAndWarmUp(d3d_, activeSpec_.softVramLimitBytes);
    }
    impl_->updateLiveRtStatus(disableRT_, activeSpec_.allowDxr);
}

void M1TaipeiSliceEffect::setDisableCuda(bool disable) noexcept {
    if (disableCUDA_ == disable) return;
    disableCUDA_ = disable;
    if (!impl_) return;
    if (disableCUDA_) {
        impl_->livePillars[1].hardwareVerified = false;
    } else {
        impl_->ensureLiveAccelerators(d3d_, activeSpec_.allowCuda, false, false);
    }
    impl_->updateLiveAuxiliaryStatuses(
        d3d_.isWarp(),
        disableCUDA_, activeSpec_.allowCuda,
        disableTensor_, activeSpec_.allowTensor,
        disableCodec_, codecHijackActive_);
}

void M1TaipeiSliceEffect::setDisableTensor(bool disable) noexcept {
    if (disableTensor_ == disable) return;
    disableTensor_ = disable;
    if (!impl_) return;
    if (disableTensor_) {
        impl_->livePillars[2].hardwareVerified = false;
    } else {
        impl_->ensureLiveAccelerators(d3d_, false, activeSpec_.allowTensor, false);
    }
    impl_->updateLiveAuxiliaryStatuses(
        d3d_.isWarp(),
        disableCUDA_, activeSpec_.allowCuda,
        disableTensor_, activeSpec_.allowTensor,
        disableCodec_, codecHijackActive_);
}

void M1TaipeiSliceEffect::setDisableCodec(bool disable) noexcept {
    if (disableCodec_ == disable) return;
    disableCodec_ = disable;
    if (!impl_) return;
    if (disableCodec_) {
        impl_->livePillars[3].hardwareVerified = false;
    } else {
        impl_->ensureLiveAccelerators(d3d_, false, false, true);
    }
    impl_->updateLiveAuxiliaryStatuses(
        d3d_.isWarp(),
        disableCUDA_, activeSpec_.allowCuda,
        disableTensor_, activeSpec_.allowTensor,
        disableCodec_, codecHijackActive_);
}

void M1TaipeiSliceEffect::setCodecSignalHijackActive(bool active) noexcept {
    codecHijackActive_ = active;
    if (!impl_) return;
    if (!active) {
        impl_->nv12SlotReady = {false, false, false};
    } else if (!disableCodec_) {
        impl_->ensureLiveAccelerators(d3d_, false, false, true);
    }
    impl_->updateLiveAuxiliaryStatuses(
        d3d_.isWarp(),
        disableCUDA_, activeSpec_.allowCuda,
        disableTensor_, activeSpec_.allowTensor,
        disableCodec_, codecHijackActive_);
}

void M1TaipeiSliceEffect::setAbCompareEngine(AbCompareEngine engine) noexcept {
    abCompareEngine_ = engine;
    if (!impl_) return;
    if (engine == AbCompareEngine::Codec && !disableCodec_) {
        impl_->ensureLiveAccelerators(d3d_, false, false, true);
    }
}

LivePillarTelemetry M1TaipeiSliceEffect::livePillarTelemetry(size_t pillarIndex) const noexcept {
    if (!impl_ || pillarIndex >= impl_->livePillars.size()) {
        return LivePillarTelemetry{};
    }
    return impl_->livePillars[pillarIndex];
}

bool M1TaipeiSliceEffect::queryLiveEngineStatus(EngineKind kind, HardwareResult& outStatus) const noexcept {
    if (!impl_) return false;
    switch (kind) {
    case EngineKind::RT:     outStatus = impl_->liveRtStatus; return true;
    case EngineKind::CUDA:   outStatus = impl_->liveCudaStatus; return true;
    case EngineKind::Tensor: outStatus = impl_->liveTensorStatus; return true;
    case EngineKind::Codec:  outStatus = impl_->liveCodecStatus; return true;
    default:                 return false;
    }
}

void M1TaipeiSliceEffect::setExternalNv12Frame(const uint8_t* nv12Bytes, size_t byteCount, uint64_t frameSeq) noexcept {
    constexpr size_t kNv12Bytes = 1280u * 720u * 3u / 2u;
    if (!impl_ || !nv12Bytes || byteCount != kNv12Bytes) {
        clearExternalNv12Frame();
        return;
    }
    if (!impl_->externalNv12Active || impl_->externalNv12Seq != frameSeq || impl_->externalNv12Buf.size() != kNv12Bytes) {
        impl_->externalNv12Buf.assign(nv12Bytes, nv12Bytes + kNv12Bytes);
        impl_->externalNv12Seq = (frameSeq == 0ULL) ? (impl_->externalNv12Seq + 1ULL) : frameSeq;
    }
    impl_->externalNv12Active = true;
}

void M1TaipeiSliceEffect::clearExternalNv12Frame() noexcept {
    if (!impl_) return;
    impl_->externalNv12Active = false;
    impl_->externalTexUploaded = false;
    impl_->uploadedExternalSeq = 0ULL;
}

bool M1TaipeiSliceEffect::hasExternalMediaFrame() const noexcept {
    return impl_ && impl_->externalNv12Active;
}

void M1TaipeiSliceEffect::Impl::convertExternalNv12ToRgba8(std::vector<uint8_t>& outRgba) const noexcept {
    constexpr uint32_t W = 1280u;
    constexpr uint32_t H = 720u;
    constexpr size_t kNv12Bytes = static_cast<size_t>(W) * H * 3u / 2u;
    outRgba.resize(static_cast<size_t>(W) * H * 4u, 0u);
    if (externalNv12Buf.size() != kNv12Bytes) {
        return;
    }
    const uint8_t* yPlane  = externalNv12Buf.data();
    const uint8_t* uvPlane = externalNv12Buf.data() + static_cast<size_t>(W) * H;
    for (uint32_t y = 0; y < H; ++y) {
        const uint8_t* yRow  = yPlane  + static_cast<size_t>(y) * W;
        const uint8_t* uvRow = uvPlane + static_cast<size_t>(y / 2u) * W;
        uint8_t* dstRow = outRgba.data() + static_cast<size_t>(y) * W * 4u;
        for (uint32_t x = 0; x < W; ++x) {
            const int c = std::max(0, static_cast<int>(yRow[x]) - 16) * 298;
            const int d = static_cast<int>(uvRow[(x & ~1u) + 0u]) - 128;
            const int e = static_cast<int>(uvRow[(x & ~1u) + 1u]) - 128;
            const int r = (c + 459 * e + 128) >> 8;
            const int g = (c - 55 * d - 136 * e + 128) >> 8;
            const int b = (c + 541 * d + 128) >> 8;
            dstRow[x * 4u + 0u] = static_cast<uint8_t>(std::clamp(r, 0, 255));
            dstRow[x * 4u + 1u] = static_cast<uint8_t>(std::clamp(g, 0, 255));
            dstRow[x * 4u + 2u] = static_cast<uint8_t>(std::clamp(b, 0, 255));
            dstRow[x * 4u + 3u] = 255u;
        }
    }
}

void* M1TaipeiSliceEffect::canvasCleanResourceRaw() const noexcept {
    return (impl_ && impl_->canvasClean) ? impl_->canvasClean.Get() : nullptr;
}

uint32_t M1TaipeiSliceEffect::canvasWidth() const noexcept {
    return impl_ ? impl_->canvasW : 0u;
}

uint32_t M1TaipeiSliceEffect::canvasHeight() const noexcept {
    return impl_ ? impl_->canvasH : 0u;
}

uint32_t M1TaipeiSliceEffect::lastVerifiedRayHits() const noexcept {
    return impl_ ? impl_->lastVerifiedHits : 0u;
}

uint32_t M1TaipeiSliceEffect::activeDenoiseMode() const noexcept {
    return (impl_ && impl_->trilogyMesh) ? impl_->trilogyMesh->activeDenoiseMode() : 0u;
}

GpuM1SceneConstants M1TaipeiSliceEffect::Impl::buildSceneConstants(
    const FrameContext& ctx,
    M1CameraState& outCam) const noexcept {
    outCam = evaluateShowCamera(visualMode, ctx.samplePosition, ctx.sampleRate, ctx.moodIrisAngle, ctx.reducedFlash, ctx.motionScale);
    const float wCam = std::clamp(ctx.interactive.blendWeight, 0.0f, 1.0f);
    if (wCam > 1e-4f) {
        if (wCam >= 0.9999f) {
            std::memcpy(outCam.eye, ctx.interactive.eye, sizeof(outCam.eye));
            std::memcpy(outCam.target, ctx.interactive.target, sizeof(outCam.target));
            std::memcpy(outCam.up, ctx.interactive.up, sizeof(outCam.up));
            normalize3(outCam.up);
            outCam.fovYRadians = ctx.interactive.fovYRadians;
        } else {
            float filmFwd[3] = {
                outCam.target[0] - outCam.eye[0],
                outCam.target[1] - outCam.eye[1],
                outCam.target[2] - outCam.eye[2]};
            const float filmDist = std::max(0.25f, std::sqrt(
                filmFwd[0] * filmFwd[0] + filmFwd[1] * filmFwd[1] + filmFwd[2] * filmFwd[2]));
            normalize3(filmFwd);

            float expFwd[3] = {
                ctx.interactive.target[0] - ctx.interactive.eye[0],
                ctx.interactive.target[1] - ctx.interactive.eye[1],
                ctx.interactive.target[2] - ctx.interactive.eye[2]};
            const float expDist = std::max(0.25f, std::sqrt(
                expFwd[0] * expFwd[0] + expFwd[1] * expFwd[1] + expFwd[2] * expFwd[2]));
            normalize3(expFwd);

            float blendFwd[3] = {
                filmFwd[0] + (expFwd[0] - filmFwd[0]) * wCam,
                filmFwd[1] + (expFwd[1] - filmFwd[1]) * wCam,
                filmFwd[2] + (expFwd[2] - filmFwd[2]) * wCam};
            if (blendFwd[0] * blendFwd[0] + blendFwd[1] * blendFwd[1] + blendFwd[2] * blendFwd[2] < 1e-4f) {
                float side[3]{};
                cross3(outCam.up, filmFwd, side);
                normalize3(side);
                blendFwd[0] += side[0] * 0.25f;
                blendFwd[1] += side[1] * 0.25f;
                blendFwd[2] += side[2] * 0.25f;
            }
            normalize3(blendFwd);
            const float blendDist = filmDist + (expDist - filmDist) * wCam;
            for (int c = 0; c < 3; ++c) {
                outCam.eye[c] = outCam.eye[c] + (ctx.interactive.eye[c] - outCam.eye[c]) * wCam;
                outCam.target[c] = outCam.eye[c] + blendFwd[c] * blendDist;
                outCam.up[c] = outCam.up[c] + (ctx.interactive.up[c] - outCam.up[c]) * wCam;
            }
            normalize3(outCam.up);
            outCam.fovYRadians = outCam.fovYRadians + (ctx.interactive.fovYRadians - outCam.fovYRadians) * wCam;
        }
    }
    const float exactTimeSec = static_cast<float>(
        static_cast<double>(ctx.samplePosition) / static_cast<double>(std::max<int32_t>(1, ctx.sampleRate)));

    uint32_t flags = (ctx.edition == Edition::Midnight) ? 1u : 0u;
    if (visualMode == ShowVisualMode::M2Trilogy180s) {
        flags |= 2u | (std::min(2u, static_cast<uint32_t>(ctx.samplePosition / 2880000)) << 2u);
    } else if (visualMode == ShowVisualMode::SoloCUDA ||
               (visualMode == ShowVisualMode::FullFilm360s && exactTimeSec >= 195.0f && exactTimeSec < 245.0f)) {
        flags |= 16u;
    } else if (visualMode != ShowVisualMode::M1Slice60s && visualMode != ShowVisualMode::LegacyM0) {
        flags |= 32u;
    }

    GpuM1SceneConstants sc{};
    std::memcpy(sc.camEye, outCam.eye, sizeof(sc.camEye));
    sc.fovY = outCam.fovYRadians;
    sc.aspect = 16.0f / 9.0f;
    buildCameraBasis(outCam, sc.camForward, sc.camRight, sc.camUp);
    sc.filmTime = exactTimeSec;
    sc.revealOpen01 = outCam.revealOpen01;
    sc.canvasWidth = canvasW;
    sc.canvasHeight = canvasH;
    sc.sceneFlags = flags;
    sc.emissivePulse = outCam.emissivePulse;
    sc.moodIntensity = ctx.moodIntensity;
    sc.shotIndex = (outCam.shotIndex & 0xFFu) | (meshSlices[4].baseIndex << 8u);
    sc.deterministicSeed = static_cast<uint32_t>(ctx.randomSeed & 0xFFFFFFFFULL);
    for (uint32_t m = 0; m < 4u; ++m) {
        sc.meshBaseIndices[m] = meshSlices[m].baseIndex;
    }
    return sc;
}

void M1TaipeiSliceEffect::Impl::recordBloomAndCompositeToTarget(
    ID3D12GraphicsCommandList* cmd,
    const FrameContext& ctx,
    float revealOpen01,
    ID3D12Resource* targetCanvas,
    uint32_t targetRtvIndex,
    D3D12Context* timestampCtx) {
    cmd->SetGraphicsRootSignature(gfxRootSig.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Pass 2: 3-Mip Dual-Kawase HDR Bloom (disciplined threshold preserving crystal petal internal detail)
    cmd->SetPipelineState(psoBloomDown.Get());
    for (uint32_t m = 0; m < kNumBloomMips; ++m) {
        const uint32_t srcW = (m == 0) ? canvasW : bloomW[m - 1];
        const uint32_t srcH = (m == 0) ? canvasH : bloomH[m - 1];
        transitionResource(cmd, bloomMips[m].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvAt(2u + m);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(bloomW[m]), static_cast<float>(bloomH[m]), 0.0f, 1.0f};
        D3D12_RECT scis{0, 0, static_cast<LONG>(bloomW[m]), static_cast<LONG>(bloomH[m])};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &scis);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        GpuM1PostConstants pc{};
        pc.invSize[0] = 1.0f / static_cast<float>(srcW);
        pc.invSize[1] = 1.0f / static_cast<float>(srcH);
        pc.bloomThreshold = 1.35f;
        pc.scaleOrVignette = 1.0f;
        pc.isFirstDownsample = (m == 0) ? 1u : 0u;
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(GpuM1PostConstants) / 4, &pc, 0);
        cmd->SetGraphicsRootDescriptorTable(1, srvGpuAt(3u + m)); // [3]=sceneHdr, [4]=mip0, [5]=mip1
        cmd->DrawInstanced(3, 1, 0, 0);

        transitionResource(cmd, bloomMips[m].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    cmd->SetPipelineState(psoBloomUpAdditive.Get());
    for (int m = static_cast<int>(kNumBloomMips) - 2; m >= 0; --m) {
        const uint32_t dstIdx = static_cast<uint32_t>(m);
        const uint32_t srcIdx = dstIdx + 1u;
        transitionResource(cmd, bloomMips[dstIdx].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvAt(2u + dstIdx);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(bloomW[dstIdx]), static_cast<float>(bloomH[dstIdx]), 0.0f, 1.0f};
        D3D12_RECT scis{0, 0, static_cast<LONG>(bloomW[dstIdx]), static_cast<LONG>(bloomH[dstIdx])};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &scis);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        GpuM1PostConstants pc{};
        pc.invSize[0] = 1.0f / static_cast<float>(bloomW[srcIdx]);
        pc.invSize[1] = 1.0f / static_cast<float>(bloomH[srcIdx]);
        pc.scaleOrVignette = 1.12f;
        pc.bloomIntensity = 0.62f;
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(GpuM1PostConstants) / 4, &pc, 0);
        cmd->SetGraphicsRootDescriptorTable(1, srvGpuAt(4u + srcIdx)); // [6]=mip2, [5]=mip1
        cmd->DrawInstanced(3, 1, 0, 0);

        transitionResource(cmd, bloomMips[dstIdx].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    if (timestampCtx) {
        timestampCtx->writePassTimestamp(2);
    }

    // Pass 3: Clean 16:9 Canvas Composite (Chroma-Preserving Filmic + IEC sRGB + Sample-Deterministic IGN Dither)
    transitionResource(cmd, targetCanvas, kCanvasReadState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvDst = rtvAt(targetRtvIndex);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(canvasW), static_cast<float>(canvasH), 0.0f, 1.0f};
        D3D12_RECT scis{0, 0, static_cast<LONG>(canvasW), static_cast<LONG>(canvasH)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &scis);
        cmd->OMSetRenderTargets(1, &rtvDst, FALSE, nullptr);

        const uint64_t sampleBucket = static_cast<uint64_t>(std::max<int64_t>(0, ctx.samplePosition) / 800LL);
        const uint32_t curAov = std::min<uint32_t>(ctx.interactive.aovMode, 7u);
        GpuM1PostConstants pc{};
        pc.invSize[0] = 1.0f / static_cast<float>(canvasW);
        pc.invSize[1] = 1.0f / static_cast<float>(canvasH);
        pc.scaleOrVignette = (curAov != 0u) ? 0.0f : 0.24f;
        // Bits 0..7 = AOV mode; bit8 = autofocus bokeh DoF (M2 linear scene distance), bit9 = lens ghosts + halo.
        pc.isFirstDownsample = curAov | ((trilogy && curAov == 0u) ? 0x100u : 0u) | ((ctx.reducedFlash || curAov != 0u) ? 0u : 0x200u);
        pc.ditherSeed = static_cast<uint32_t>((sampleBucket ^ ctx.randomSeed) & 0xFFFFFFFFULL);
        pc.preExposure = 0.94f;
        pc.bloomIntensity = (curAov != 0u) ? 0.0f : (0.18f + 0.06f * revealOpen01);
        if (curAov == 0u &&
            (visualMode == ShowVisualMode::SoloNPU ||
             (visualMode == ShowVisualMode::FullFilm360s && ctx.filmTimeSeconds >= 150.0 && ctx.filmTimeSeconds < 195.0))) {
            pc.bloomIntensity *= 0.85f + 0.30f * std::clamp(ctx.moodBloomStrength, 0.f, 1.f);
        }

        // Lens breathes with the music: the beat envelope pumps bloom slightly and feeds the composite's
        // streak / chromatic-fringe gain. Zero in AOV modes, progressive accumulation, reduced flash and deterministic contexts.
        const bool allowAudioDrive = (curAov == 0u) && !ctx.interactive.accumulate;
        const float audioKick = (allowAudioDrive && !ctx.reducedFlash) ? std::clamp(ctx.audioReactive.beat, 0.0f, 1.0f) : 0.0f;
        pc.bloomThreshold = audioKick; // Composite reads this slot as gAudioKick (bloom passes use their own constants)
        pc.bloomIntensity *= 1.0f + 0.12f * audioKick + 0.06f * (allowAudioDrive ? std::clamp(ctx.audioReactive.low, 0.0f, 1.0f) : 0.0f);

        cmd->SetGraphicsRootSignature(compositeRootSig.Get());
        cmd->SetPipelineState(psoCompositeCanvas.Get());
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(GpuM1PostConstants) / 4, &pc, 0);
        cmd->SetGraphicsRootDescriptorTable(1, srvGpuAt(2u)); // [2]=sceneDistance (t0), [3]=sceneHdr (t1), [4]=bloomMip0 (t2)
        cmd->DrawInstanced(3, 1, 0, 0);
    }
    transitionResource(cmd, targetCanvas, D3D12_RESOURCE_STATE_RENDER_TARGET, kCanvasReadState);

    if (timestampCtx) {
        timestampCtx->writePassTimestamp(3);
    }
}

void M1TaipeiSliceEffect::Impl::recordScenePipelineToCanvas(
    D3D12Context& d3d,
    ID3D12GraphicsCommandList* cmd,
    const FrameContext& ctx,
    bool useDxr,
    bool useCuda,
    bool useTensor,
    bool skipSharedCudaUpdate,
    bool skipSharedTensorUpdate,
    ID3D12Resource* targetCanvas,
    uint32_t targetRtvIndex,
    uint32_t slot,
    bool isCanonicalPass,
    bool recordTimestamps) {
    if (isCanonicalPass) {
        if (slot < kFrameSlots) {
            retireVerifySlot(slot);
        }
        readRtTimestampSlot(slot);
    }

    M1CameraState cam{};
    GpuM1SceneConstants sc = buildSceneConstants(ctx, cam);

    ID3D12DescriptorHeap* heaps[] = {srvUavHeap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);

    // During stationary Explore NRD convergence (accumFrame > 0), samplePosition is paused:
    // reuse frame 0's GPU-resident Tensor caustic texture & CUDA particle buffer without CPU/CUDA sync stalls.
    if (isCanonicalPass && ctx.interactive.accumulate && ctx.interactive.accumFrame > 0u) {
        skipSharedTensorUpdate = true;
        skipSharedCudaUpdate = true;
    }

    // 1. Update 320x180 RGBA16F Tensor Core neural caustic / spectral field (or scalar HLSL substitute)
    if (!skipSharedTensorUpdate) {
        bool uploadedHwTensor = false;
        if (useTensor && !d3d.isWarp() && liveAccel.tensorHardwareAvailable()) {
            LivePillarTelemetry tTel{};
            const uint16_t* tPixels = liveAccel.evaluateTensorCausticField(
                ctx.samplePosition, ctx.sampleRate, sc.deterministicSeed, ctx.edition, cam.revealOpen01, tTel);
            if (tPixels != nullptr) {
                uint8_t* dstMapped = nullptr;
                D3D12_RANGE emptyRead{0, 0};
                if (SUCCEEDED(tensorUploadBuf[slot]->Map(0, &emptyRead, reinterpret_cast<void**>(&dstMapped))) && dstMapped) {
                    const size_t srcRowBytes = static_cast<size_t>(LiveAcceleratorOrchestra::kTensorWidth) * 8u;
                    const auto* srcBytes = reinterpret_cast<const uint8_t*>(tPixels);
                    for (uint32_t y = 0; y < LiveAcceleratorOrchestra::kTensorHeight; ++y) {
                        std::memcpy(
                            dstMapped + tensorFootprint.Offset + static_cast<size_t>(y) * tensorFootprint.Footprint.RowPitch,
                            srcBytes + static_cast<size_t>(y) * srcRowBytes,
                            srcRowBytes);
                    }
                    D3D12_RANGE written{0, static_cast<SIZE_T>(tensorUploadBytes)};
                    tensorUploadBuf[slot]->Unmap(0, &written);

                    transitionResource(cmd, tensorFieldTex.Get(), kTensorReadState, D3D12_RESOURCE_STATE_COPY_DEST);
                    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
                    dstLoc.pResource = tensorFieldTex.Get();
                    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dstLoc.SubresourceIndex = 0;

                    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
                    srcLoc.pResource = tensorUploadBuf[slot].Get();
                    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    srcLoc.PlacedFootprint = tensorFootprint;

                    cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
                    transitionResource(cmd, tensorFieldTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kTensorReadState);
                    uploadedHwTensor = true;
                    if (isCanonicalPass) {
                        livePillars[2] = tTel;
                    }
                }
            }
        }
        if (!uploadedHwTensor) {
            transitionResource(cmd, tensorFieldTex.Get(), kTensorReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            GpuM1TensorSubConstants tsc{
                sc.filmTime,
                cam.revealOpen01,
                (ctx.edition == Edition::Midnight) ? 1u : 0u,
                sc.deterministicSeed};
            cmd->SetComputeRootSignature(auxComputeRootSig.Get());
            cmd->SetPipelineState(psoTensorSubCs.Get());
            cmd->SetComputeRoot32BitConstants(0, sizeof(GpuM1TensorSubConstants) / 4, &tsc, 0);
            cmd->SetComputeRootDescriptorTable(2, srvGpuAt(11u));
            cmd->Dispatch(40u, 23u, 1u);
            transitionResource(cmd, tensorFieldTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kTensorReadState);
            if (isCanonicalPass) {
                livePillars[2] = LivePillarTelemetry{};
                livePillars[2].status = BackendStatus::Substituted;
                livePillars[2].activeInScene = true;
                livePillars[2].hardwareVerified = false;
                livePillars[2].workloadItems = LiveAcceleratorOrchestra::kTensorWidth * LiveAcceleratorOrchestra::kTensorHeight;
                livePillars[2].workloadSteps = 1u;
                livePillars[2].executionMs = 0.05f;
                livePillars[2].throughput = 0.04f;
            }
        }
    }

    // 2. Pass 1: Primary 112-instance DXR 1.1 RayQuery scene OR SM 6.0 SDF/Analytic Raster substitute
    if (isCanonicalPass && rtTsHeap) {
        cmd->EndQuery(rtTsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u);
    }

    if(trilogyMesh) {
        trilogyMesh->record(cmd,ctx,cam,sceneHdr.Get(),sceneDepth.Get(),srvGpuAt(7u).ptr,canvasW,canvasH,slot,useDxr,isCanonicalPass);
        ID3D12DescriptorHeap* m1Heaps[] = {srvUavHeap.Get()};
        cmd->SetDescriptorHeaps(1, m1Heaps);
    } else {
    const bool canAttemptDxr = dxrPipelineReady && !dxrQuarantined && !d3d.isWarp() && useDxr;
    ComPtr<ID3D12GraphicsCommandList4> cmd4;
    bool dispatchedDxr = false;
    if (canAttemptDxr && SUCCEEDED(cmd->QueryInterface(IID_PPV_ARGS(&cmd4))) && cmd4) {
        bool tlasReadyForPass = !isCanonicalPass;
        if (isCanonicalPass) {
            const size_t sliceOffset = static_cast<size_t>(slot) * kNumTlasInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
            uint8_t* mappedInst = nullptr;
            D3D12_RANGE emptyRead{0, 0};
            if (SUCCEEDED(tlasInstanceUpload->Map(0, &emptyRead, reinterpret_cast<void**>(&mappedInst))) && mappedInst) {
                std::array<D3D12_GPU_VIRTUAL_ADDRESS, kNumBlasMeshes> blasVas{};
                for (uint32_t m = 0; m < kNumBlasMeshes; ++m) {
                    blasVas[m] = blasResults[m]->GetGPUVirtualAddress();
                }
                populateTaipei96Instances(
                    reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(mappedInst + sliceOffset),
                    blasVas,
                    sc.filmTime,
                    cam.revealOpen01);
                D3D12_RANGE written{sliceOffset, sliceOffset + kNumTlasInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC)};
                tlasInstanceUpload->Unmap(0, &written);

                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildTlas{};
                buildTlas.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                buildTlas.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
                buildTlas.Inputs.NumDescs = kNumTlasInstances;
                buildTlas.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                buildTlas.Inputs.InstanceDescs = tlasInstanceUpload->GetGPUVirtualAddress() + sliceOffset;
                buildTlas.ScratchAccelerationStructureData = tlasScratch->GetGPUVirtualAddress();
                buildTlas.DestAccelerationStructureData = tlasResult->GetGPUVirtualAddress();

                D3D12_RESOURCE_BARRIER beforeBuild[2]{};
                beforeBuild[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                beforeBuild[0].UAV.pResource = tlasResult.Get();
                beforeBuild[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                beforeBuild[1].UAV.pResource = tlasScratch.Get();
                cmd4->ResourceBarrier(2, beforeBuild);
                cmd4->BuildRaytracingAccelerationStructure(&buildTlas, 0, nullptr);

                D3D12_RESOURCE_BARRIER tlasUav{};
                tlasUav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                tlasUav.UAV.pResource = tlasResult.Get();
                cmd4->ResourceBarrier(1, &tlasUav);
                tlasReadyForPass = true;
            }
        }

        if (tlasReadyForPass) {
            const uint32_t verSlot = (slot < kFrameSlots) ? slot : 0u;
            const uint32_t seq = isCanonicalPass ? (++nextDxrSeq) : 0u;
            sc.expectSeq = seq;

            if (isCanonicalPass) {
                cmd4->CopyBufferRegion(dxrVerifyUav[verSlot].Get(), 0, zeroUploadBuf.Get(), 0, 8);
                transitionResource(cmd4.Get(), dxrVerifyUav[verSlot].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            transitionResource(cmd4.Get(), sceneHdr.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transitionResource(cmd4.Get(), sceneDepth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            cmd4->SetComputeRootSignature(dxrRootSig.Get());
            cmd4->SetComputeRoot32BitConstants(0, sizeof(GpuM1SceneConstants) / 4, &sc, 0);
            cmd4->SetComputeRootShaderResourceView(1, tlasResult->GetGPUVirtualAddress());
            cmd4->SetComputeRootShaderResourceView(2, blasVbUpload->GetGPUVirtualAddress());
            cmd4->SetComputeRootShaderResourceView(3, blasIbUpload->GetGPUVirtualAddress());
            cmd4->SetComputeRootUnorderedAccessView(
                4, isCanonicalPass ? dxrVerifyUav[verSlot]->GetGPUVirtualAddress() : abVerifyUav->GetGPUVirtualAddress());
            cmd4->SetComputeRootDescriptorTable(5, srvGpuAt(7u)); // t3 = tensorFieldTex
            cmd4->SetComputeRootDescriptorTable(6, srvGpuAt(0u)); // u1..u2 = sceneHdr, sceneDepth
            cmd4->SetPipelineState(psoDxrScene.Get());
            cmd4->Dispatch((canvasW + 7u) / 8u, (canvasH + 7u) / 8u, 1u);

            transitionResource(cmd4.Get(), sceneHdr.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transitionResource(cmd4.Get(), sceneDepth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            if (isCanonicalPass) {
                transitionResource(cmd4.Get(), dxrVerifyUav[verSlot].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmd4->CopyBufferRegion(dxrVerifyReadback[verSlot].Get(), 0, dxrVerifyUav[verSlot].Get(), 0, 8);
                transitionResource(cmd4.Get(), dxrVerifyUav[verSlot].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                slotPending[verSlot] = PendingVerifySlot{true, dxrGeneration, seq};
            }
            dispatchedDxr = true;
        }
    }

    if (!dispatchedDxr) {
        if (isCanonicalPass) {
            ++dxrGeneration;
            dxrVerified = false;
        }

        transitionResource(cmd, sceneHdr.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transitionResource(cmd, sceneDepth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = rtvAt(0u);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(canvasW), static_cast<float>(canvasH), 0.0f, 1.0f};
        D3D12_RECT scis{0, 0, static_cast<LONG>(canvasW), static_cast<LONG>(canvasH)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &scis);
        cmd->OMSetRenderTargets(2, &rtv0, TRUE, nullptr);
        cmd->SetGraphicsRootSignature(gfxRootSig.Get());
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(GpuM1SceneConstants) / 4, &sc, 0);
        cmd->SetGraphicsRootDescriptorTable(1, srvGpuAt(7u)); // t0 = tensorFieldTex
        cmd->SetPipelineState(psoRasterScene.Get());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
        transitionResource(cmd, sceneHdr.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transitionResource(cmd, sceneDepth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    } // legacy M1 primary scene
    if (isCanonicalPass && rtTsHeap && rtTsReadback) {
        cmd->EndQuery(rtTsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2u + 1u);
        cmd->ResolveQueryData(
            rtTsHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            slot * 2u, 2u, rtTsReadback.Get(), static_cast<UINT64>(slot) * 16ULL);
        rtTsSlotValid[slot] = true;
    }

    // 3. Pass 1.5: Live 3D Particle Simulation (65.5K..262.1K x 16-substep CUDA PTX vs 8.2K x 2-step HLSL) + Soft-Depth HDR Shading
    uint32_t reqCudaParticles = LiveAcceleratorOrchestra::kCudaParticleCount;
    uint32_t reqCudaMode = 0u;
    resolveCudaSimParams(ctx.samplePosition, ctx.sampleRate, activeSpecForParticles, reqCudaParticles, reqCudaMode);

    uint32_t activeParticleCount = LiveAcceleratorOrchestra::kSubParticleCount;
    if (!skipSharedCudaUpdate) {
        bool uploadedHwParticles = false;
        if (useCuda && !d3d.isWarp() && liveAccel.cudaHardwareAvailable()) {
            LivePillarTelemetry cTel{};
            const LiveParticleState* pStates = liveAccel.simulateCudaParticles(
                ctx.samplePosition, ctx.sampleRate, sc.deterministicSeed, ctx.edition, cam.revealOpen01, cTel,
                reqCudaParticles, reqCudaMode);
            if (pStates != nullptr) {
                void* dstMapped = nullptr;
                D3D12_RANGE emptyRead{0, 0};
                const size_t byteCount = static_cast<size_t>(reqCudaParticles) * sizeof(LiveParticleState);
                if (SUCCEEDED(particleUploadBuf[slot]->Map(0, &emptyRead, &dstMapped)) && dstMapped) {
                    std::memcpy(dstMapped, pStates, byteCount);
                    D3D12_RANGE written{0, byteCount};
                    particleUploadBuf[slot]->Unmap(0, &written);

                    transitionResource(cmd, particleGpuBuf.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                    cmd->CopyBufferRegion(particleGpuBuf.Get(), 0, particleUploadBuf[slot].Get(), 0, byteCount);
                    transitionResource(cmd, particleGpuBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                    activeParticleCount = reqCudaParticles;
                    uploadedHwParticles = true;
                    if (isCanonicalPass) {
                        livePillars[1] = cTel;
                    }
                }
            }
        }
        if (!uploadedHwParticles) {
            transitionResource(cmd, particleGpuBuf.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmd->SetComputeRootSignature(auxComputeRootSig.Get());
            cmd->SetPipelineState(psoParticlesSubCs.Get());
            cmd->SetComputeRoot32BitConstants(0, sizeof(GpuM1SceneConstants) / 4, &sc, 0);
            cmd->SetComputeRootDescriptorTable(2, srvGpuAt(13u));
            cmd->Dispatch(128u, 1u, 1u);
            transitionResource(cmd, particleGpuBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            activeParticleCount = LiveAcceleratorOrchestra::kSubParticleCount;
            if (isCanonicalPass) {
                livePillars[1] = LivePillarTelemetry{};
                livePillars[1].status = BackendStatus::Substituted;
                livePillars[1].activeInScene = true;
                livePillars[1].hardwareVerified = false;
                livePillars[1].workloadItems = LiveAcceleratorOrchestra::kSubParticleCount;
                livePillars[1].workloadSteps = LiveAcceleratorOrchestra::kSubSubsteps;
                livePillars[1].executionMs = 0.08f;
                livePillars[1].throughput = (8192.0f * 2.0f) / (0.08f * 1000.0f);
            }
        }
    } else {
        activeParticleCount = livePillars[1].hardwareVerified
                                  ? std::max(LiveAcceleratorOrchestra::kCudaParticleCount, livePillars[1].workloadItems)
                                  : LiveAcceleratorOrchestra::kSubParticleCount;
    }

    if (ctx.interactive.aovMode == 0u) {
        transitionResource(cmd, sceneHdr.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHdr = rtvAt(0u);
        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(canvasW), static_cast<float>(canvasH), 0.0f, 1.0f};
        D3D12_RECT scis{0, 0, static_cast<LONG>(canvasW), static_cast<LONG>(canvasH)};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &scis);
        cmd->OMSetRenderTargets(1, &rtvHdr, FALSE, nullptr);
        cmd->SetGraphicsRootSignature(particlesGfxRootSig.Get());
        cmd->SetPipelineState(psoParticles3D.Get());
        cmd->SetGraphicsRoot32BitConstants(0, sizeof(GpuM1SceneConstants) / 4, &sc, 0);
        cmd->SetGraphicsRootShaderResourceView(1, particleGpuBuf->GetGPUVirtualAddress());
        cmd->SetGraphicsRootDescriptorTable(2, srvGpuAt(2u)); // t0 = sceneDepth
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(6u, activeParticleCount, 0u, 0u);
        transitionResource(cmd, sceneHdr.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    if (recordTimestamps) {
        d3d.writePassTimestamp(1);
    }

    recordBloomAndCompositeToTarget(
        cmd,
        ctx,
        cam.revealOpen01,
        targetCanvas,
        targetRtvIndex,
        recordTimestamps ? &d3d : nullptr);
}

void M1TaipeiSliceEffect::Impl::recordNv12PackAndReadback(
    ID3D12GraphicsCommandList* cmd,
    uint32_t readbackSlot) {
    constexpr uint64_t kNv12Bytes = 1280ULL * 720ULL * 3ULL / 2ULL;
    ID3D12DescriptorHeap* heaps[] = {srvUavHeap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);

    cmd->SetComputeRootSignature(auxComputeRootSig.Get());
    cmd->SetPipelineState(psoNv12PackCs.Get());
    cmd->SetComputeRootDescriptorTable(1, srvGpuAt(8u));  // t0 = canvasClean
    cmd->SetComputeRootDescriptorTable(2, srvGpuAt(12u)); // u0 = nv12PackBuf
    cmd->Dispatch(40u, 45u, 1u);

    transitionResource(cmd, nv12PackBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(nv12ReadbackBuf[readbackSlot].Get(), 0, nv12PackBuf.Get(), 0, kNv12Bytes);
    transitionResource(cmd, nv12PackBuf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void M1TaipeiSliceEffect::Impl::uploadCodecRgbaToGpu(
    ID3D12GraphicsCommandList* cmd,
    uint32_t uploadSlot,
    const std::vector<uint8_t>& rgbaBytes,
    bool markHwVerified) {
    const size_t expected = static_cast<size_t>(LiveAcceleratorOrchestra::kCodecWidth) *
                            static_cast<size_t>(LiveAcceleratorOrchestra::kCodecHeight) * 4u;
    if (rgbaBytes.size() < expected) return;

    uint8_t* dstMapped = nullptr;
    D3D12_RANGE emptyRead{0, 0};
    if (FAILED(codecRgbaUploadBuf[uploadSlot]->Map(0, &emptyRead, reinterpret_cast<void**>(&dstMapped))) || !dstMapped) {
        return;
    }
    const size_t srcRowBytes = static_cast<size_t>(LiveAcceleratorOrchestra::kCodecWidth) * 4u;
    for (uint32_t y = 0; y < LiveAcceleratorOrchestra::kCodecHeight; ++y) {
        std::memcpy(
            dstMapped + codecFootprint.Offset + static_cast<size_t>(y) * codecFootprint.Footprint.RowPitch,
            rgbaBytes.data() + static_cast<size_t>(y) * srcRowBytes,
            srcRowBytes);
    }
    D3D12_RANGE written{0, static_cast<SIZE_T>(codecUploadBytes)};
    codecRgbaUploadBuf[uploadSlot]->Unmap(0, &written);

    transitionResource(cmd, codecDecodedTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = codecDecodedTex.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = codecRgbaUploadBuf[uploadSlot].Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = codecFootprint;

    cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    transitionResource(cmd, codecDecodedTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    codecHwTexValid = markHwVerified;
}

bool M1TaipeiSliceEffect::Impl::ensureUiTextResources(ID3D12Device* dev) {
    if (uiTextTex && uiTextLayer) return uiTextLayer->ready();
    if (!dev) return false;
    if (!uiTextLayer) uiTextLayer = std::make_unique<UiTextLayer>();
    if (!uiTextLayer->ready()) return false;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const D3D12_RESOURCE_DESC desc = makeTex2dDesc(
        UiTextAtlasLayout::kWidth, UiTextAtlasLayout::kHeight, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    if (FAILED(dev->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&uiTextTex)))) {
        uiTextTex.Reset();
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(uiTextTex.Get(), &srv, srvCpuAt(17u));
    return true;
}

// Copies the changed band(s) of the CPU atlas to the texture. Each frame slot has its own upload
// buffer, so a copy still queued on the GPU never has its source rewritten.
void M1TaipeiSliceEffect::Impl::uploadUiText(ID3D12Device* dev, ID3D12GraphicsCommandList* cmd, uint32_t cbSlot, bool labels, bool caption) {
    if (!uiTextTex || !uiTextLayer || cbSlot >= kTotalUploadSlots || (!labels && !caption)) return;
    constexpr uint64_t kPitch = UiTextAtlasLayout::kWidth; // 3072: a multiple of the 256-byte row and 512-byte placement alignments
    ComPtr<ID3D12Resource>& buf = uiTextUploadBuf[cbSlot];
    if (!buf) {
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        const D3D12_RESOURCE_DESC bufDesc = makeBufferDesc(kPitch * UiTextAtlasLayout::kHeight);
        if (FAILED(dev->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf)))) {
            buf.Reset();
            return;
        }
    }
    struct Band { uint32_t top; uint32_t rows; };
    Band bands[2]; uint32_t bandCount = 0;
    if (labels)  bands[bandCount++] = {0u, UiTextAtlasLayout::kCaptionTop};
    if (caption) bands[bandCount++] = {UiTextAtlasLayout::kCaptionTop, UiTextAtlasLayout::kCaptionMaxH};

    uint8_t* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    if (FAILED(buf->Map(0, &noRead, reinterpret_cast<void**>(&mapped))) || !mapped) return;
    for (uint32_t b = 0; b < bandCount; ++b) {
        std::memcpy(mapped + kPitch * bands[b].top, uiTextLayer->pixels() + kPitch * bands[b].top, kPitch * bands[b].rows);
    }
    buf->Unmap(0, nullptr);

    transitionResource(cmd, uiTextTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    for (uint32_t b = 0; b < bandCount; ++b) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = uiTextTex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = buf.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = kPitch * bands[b].top;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
        src.PlacedFootprint.Footprint.Width = UiTextAtlasLayout::kWidth;
        src.PlacedFootprint.Footprint.Height = bands[b].rows;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(kPitch);
        cmd->CopyTextureRegion(&dst, 0, bands[b].top, 0, &src, nullptr);
    }
    transitionResource(cmd, uiTextTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void M1TaipeiSliceEffect::Impl::recordSwapchainPresentPass(
    ID3D12Device* dev,
    ID3D12GraphicsCommandList* cmd,
    uint32_t targetWidth,
    uint32_t targetHeight,
    DXGI_FORMAT targetFormat,
    uint32_t targetBits,
    uint32_t cbSlot,
    const FrameContext& ctx,
    const HudTelemetrySnapshot& hud,
    AbCompareEngine abEngine,
    bool codecHijackActive) {
    if (!dev || !ensurePresentPso(dev, targetFormat)) {
        return;
    }

    const LetterboxViewport lb = computeLetterboxViewport16x9(targetWidth, targetHeight);
    const uint32_t abIdx = static_cast<uint32_t>(abEngine) & 3u;
    const bool codecSplitActive = ctx.showOverlay && ctx.hudMode == 2u && (abEngine == AbCompareEngine::Codec);
    const bool codecViewActive = codecHijackActive || codecSplitActive;

    bool abLeftHwVerified = false;
    switch (abEngine) {
    case AbCompareEngine::RT:     abLeftHwVerified = (liveRtStatus.status == BackendStatus::Accelerated); break;
    case AbCompareEngine::CUDA:   abLeftHwVerified = livePillars[1].hardwareVerified; break;
    case AbCompareEngine::Tensor: abLeftHwVerified = livePillars[2].hardwareVerified; break;
    case AbCompareEngine::Codec:  abLeftHwVerified = livePillars[3].hardwareVerified && codecHwTexValid; break;
    default: break;
    }

    GpuM1PresentConstants pc{};
    pc.swapchainAndCanvas[0] = static_cast<float>(targetWidth);
    pc.swapchainAndCanvas[1] = static_cast<float>(targetHeight);
    pc.swapchainAndCanvas[2] = static_cast<float>(canvasW);
    pc.swapchainAndCanvas[3] = static_cast<float>(canvasH);
    pc.letterboxRect[0] = lb.x;
    pc.letterboxRect[1] = lb.y;
    pc.letterboxRect[2] = lb.width;
    pc.letterboxRect[3] = lb.height;
    pc.filmTime = static_cast<float>(ctx.filmTimeSeconds);
    pc.duration = static_cast<float>(std::max(1.0, hud.durationSeconds));
    pc.actProgress = ctx.actProgress;
    pc.fps = hud.fps;
    pc.gpuMs = hud.gpuFrameMs;
    pc.p95Ms = hud.p95FrameMs;
    pc.vramUsedMb = hud.vramUsedMb;
    pc.vramBudgetMb = hud.vramBudgetMb;

    uint32_t flags = 0u;
    if (ctx.showOverlay)                  flags |= (1u << 0u);
    if (hud.audioClockActive)             flags |= (1u << 1u);
    if (hud.muted)                        flags |= (1u << 2u);
    if (hud.paused)                       flags |= (1u << 3u);
    if (hud.loopEnabled)                  flags |= (1u << 4u);
    if (ctx.edition == Edition::Midnight) flags |= (1u << 5u);
    if (hud.benchRunning)                 flags |= (1u << 6u);
    if (hud.benchHasResults)              flags |= (1u << 7u);
    flags |= ((ctx.hudMode & 3u) << 8u);
    flags |= (abIdx << 10u);
    if (codecHijackActive)                flags |= (1u << 12u);
    if (codecHwTexValid && livePillars[3].hardwareVerified) flags |= (1u << 13u);
    if (abLeftHwVerified)                 flags |= (1u << 14u);
    if (externalNv12Active && externalTexUploaded) flags |= (1u << 15u);
    pc.hudFlags = flags;

    std::copy(hud.recentGpuMs.begin(), hud.recentGpuMs.end(), pc.recentGpuMs);
    pc.volumeAndPadding[0] = hud.volumePercent;

    const std::array<BackendStatus, kEngineKindCount> liveEngineStatuses = {
        liveRtStatus.status,
        liveTensorStatus.status,
        hud.engineStatus[static_cast<size_t>(EngineKind::NPU)],
        liveCudaStatus.status,
        liveCodecStatus.status,
    };
    uint32_t packed = 0u;
    for (size_t i = 0; i < kEngineKindCount; ++i) {
        packed |= ((static_cast<uint32_t>(liveEngineStatuses[i]) & 0xFu) << (i * 4u));
    }
    pc.enginePacked = packed;
    pc.swapchainBits = targetBits;
    pc.benchProgress = hud.benchOverallProgress01;

    pc.audioBands[0] = hud.audioBands.rms;
    pc.audioBands[1] = hud.audioBands.low;
    pc.audioBands[2] = hud.audioBands.mid;
    pc.audioBands[3] = hud.audioBands.high;
    for (size_t i = 0; i < 4; ++i) {
        pc.benchPrimary[i] = hud.benchPrimaryValues[i];
        pc.benchFill[i]    = hud.benchBarFill01[i];
    }

    // Live 4-pillar attribution table (scaled cleanly to [0.0, 9999.9] for fixed-point HUD glyphs):
    //   p=0 (RT):     .z = MRays/MPix,  .x = GPU ms, .y = MRays/s
    //   p=1 (CUDA):   .z = K particles, .x = GPU ms, .y = MSteps/s
    //   p=2 (Tensor): .z = GFLOP/frame, .x = GPU ms, .y = TFLOPS
    //   p=3 (Codec):  .z = Mbps,        .x = RT ms,  .y = FPS,      .w = PSNR (dB)
    pc.livePillarMetrics[0][0] = livePillars[0].executionMs;
    pc.livePillarMetrics[0][1] = livePillars[0].throughput;
    pc.livePillarMetrics[0][2] = livePillars[0].auxMetric;
    pc.livePillarMetrics[0][3] = static_cast<float>(livePillars[0].workloadSteps);

    pc.livePillarMetrics[1][0] = livePillars[1].executionMs;
    pc.livePillarMetrics[1][1] = livePillars[1].throughput;
    pc.livePillarMetrics[1][2] = static_cast<float>(livePillars[1].workloadItems) * 1.0e-3f;
    pc.livePillarMetrics[1][3] = static_cast<float>(livePillars[1].workloadSteps);

    pc.livePillarMetrics[2][0] = livePillars[2].executionMs;
    pc.livePillarMetrics[2][1] = livePillars[2].throughput;
    pc.livePillarMetrics[2][2] = livePillars[2].hardwareVerified ? 7.55f : 0.06f;
    pc.livePillarMetrics[2][3] = static_cast<float>(livePillars[2].workloadSteps);

    pc.livePillarMetrics[3][0] = codecViewActive ? livePillars[3].executionMs : 0.0f;
    pc.livePillarMetrics[3][1] = codecViewActive ? livePillars[3].throughput : 0.0f;
    pc.livePillarMetrics[3][2] = codecViewActive ? livePillars[3].auxMetric : 0.0f;
    pc.livePillarMetrics[3][3] = codecViewActive ? livePillars[3].qualityOrBitrate : 0.0f;

    uint32_t modePacked = 0u;
    for (uint32_t p = 0; p < 4u; ++p) {
        uint32_t nib = 2u; // OFF / STANDBY
        if (p == 3u && !codecViewActive) {
            nib = 2u;
        } else if (livePillars[p].status == BackendStatus::Faulted) {
            nib = 3u;
        } else if (livePillars[p].hardwareVerified) {
            nib = 0u; // LIVE HW
        } else if (livePillars[p].activeInScene) {
            nib = 1u; // SUB
        }
        modePacked |= ((nib & 0xFu) << (p * 4u));
    }
    pc.livePillarMeta[0] = modePacked;

    const CaptionLine cap = evaluateFilmCaption(
        visualMode, ctx.samplePosition, ctx.sampleRate, ctx.edition, ctx.showCaptions);
    // Translated text: labels and the current caption are drawn into the text atlas; English skips this
    // entirely and keeps the shader glyph tables.
    const UiStrings& uiStrings = UiStrings::instance();
    bool captionLocalized = false;
    if (uiStrings.active() && ensureUiTextResources(dev)) {
        const bool capShown = cap.active && cap.alpha > 0.005f && !cap.text.empty();
        const std::wstring* capText = capShown ? uiStrings.findCaption(cap.text) : nullptr;
        const uint32_t uiScale = std::clamp<uint32_t>(targetHeight / 540u, 1u, UiTextAtlasLayout::kMaxUiScale);
        const float capBaseScale = std::clamp(lb.height / 285.0f, 1.0f, 6.0f) * (cap.isEndCard ? 0.90f : 1.0f);
        const UiTextLayer::Frame uiFrame = uiTextLayer->prepare(uiStrings, uiScale, capText, 7.6f * capBaseScale, lb.width * 0.86f);
        uploadUiText(dev, cmd, cbSlot, uiFrame.labelsDirty, uiFrame.captionDirty);
        pc.hudFlags |= (1u << 16u);
        if (capText && uiFrame.captionValid) {
            captionLocalized = true;
            pc.hudFlags |= (1u << 17u);
            pc.captionPacked[0] = uiFrame.captionWidth;
            pc.captionPacked[1] = uiFrame.captionHeight;
            pc.captionStyle[0] = cap.alpha;
            pc.captionStyle[1] = 1.0f;
            pc.captionStyle[2] = cap.isEndCard ? 1.0f : 0.0f;
        }
    }
    if (!captionLocalized && cap.active && cap.alpha > 0.005f && !cap.text.empty()) {
        const size_t nChars = std::min<size_t>(cap.text.size(), 64u);
        for (size_t i = 0; i < nChars; ++i) {
            const uint32_t b = static_cast<uint8_t>(cap.text[i]);
            pc.captionPacked[i >> 2u] |= (b << ((i & 3u) * 8u));
        }
        pc.captionStyle[0] = cap.alpha;
        pc.captionStyle[1] = static_cast<float>(nChars);
        pc.captionStyle[2] = cap.isEndCard ? 1.0f : 0.0f;
    }
    pc.captionStyle[3] = hud.liveCameraActive ? 1.0f : 0.0f;

    pc.interactiveState[0] = std::clamp(ctx.interactive.abSplitRatio, 0.05f, 0.95f);
    pc.interactiveState[1] = std::clamp(ctx.interactive.sunElevation01, 0.0f, 1.0f);
    pc.interactiveState[2] = std::clamp(ctx.interactive.sunAzimuth01, 0.0f, 1.0f);
    const uint32_t accumSpp = ctx.interactive.accumulate ? (ctx.interactive.accumFrame + 1u) : 1u;
    pc.interactiveState[3] = static_cast<float>(std::min<uint32_t>(accumSpp, 9999u));

    const uint32_t curAov = std::min<uint32_t>(ctx.interactive.aovMode, 7u);
    const uint32_t denMode = trilogyMesh ? (trilogyMesh->activeDenoiseMode() & 0x3u) : 0u;
    const uint32_t mrblCnt = std::min<uint32_t>(ctx.interactive.marbleCount, InteractiveView::kMaxMarbles);

    uint32_t interFlags = 0u;
    if (ctx.interactive.exploreActive)           interFlags |= (1u << 0u);
    if (ctx.interactive.sunOverride)             interFlags |= (1u << 1u);
    if (ctx.interactive.lampsOff)                interFlags |= (1u << 2u);
    if (trilogyMesh && trilogyMesh->nrdActive()) interFlags |= (1u << 3u);
    interFlags |= ((ctx.interactive.activePreset & 0xFu) << 4u);
    if (ctx.interactive.accumulate)              interFlags |= (1u << 8u);
    interFlags |= (curAov << 9u);
    interFlags |= (denMode << 12u);
    interFlags |= ((mrblCnt & 0xFFu) << 16u);
    interFlags |= ((ctx.interactive.soloEngine & 0x7u) << 24u);
    if (ctx.interactive.studioDrawerOpen)        interFlags |= (1u << 27u);
    if (ctx.interactive.customAudioActive)       interFlags |= (1u << 28u);
    if (ctx.interactive.audioMuted)              interFlags |= (1u << 29u);
    pc.interactiveMeta[0] = interFlags;
    pc.interactiveMeta[1] = static_cast<uint32_t>(hud.npuCompletedInferences & 0xFFFFu) |
                            (static_cast<uint32_t>(std::clamp(hud.npuInferMs * 10.0f, 0.0f, 9999.0f) + 0.5f) << 16u);
    const uint32_t trkSlot = std::min<uint32_t>(ctx.interactive.audioTrackSlot, 5u);
    const uint32_t toastA8 = static_cast<uint32_t>(std::clamp(ctx.interactive.audioToastAlpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    pc.interactiveMeta[2] = (trkSlot & 0xFFu) | ((toastA8 & 0xFFu) << 8u);

    const size_t cbOffset = static_cast<size_t>(cbSlot) * 512u;
    uint8_t* mappedCb = nullptr;
    D3D12_RANGE emptyRead{0, 0};
    if (SUCCEEDED(presentCbUpload->Map(0, &emptyRead, reinterpret_cast<void**>(&mappedCb))) && mappedCb) {
        std::memcpy(mappedCb + cbOffset, &pc, sizeof(pc));
        D3D12_RANGE written{cbOffset, cbOffset + sizeof(pc)};
        presentCbUpload->Unmap(0, &written);
    }

    ID3D12DescriptorHeap* heaps[] = {srvUavHeap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(targetWidth), static_cast<float>(targetHeight), 0.0f, 1.0f};
    D3D12_RECT scis{0, 0, static_cast<LONG>(targetWidth), static_cast<LONG>(targetHeight)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &scis);
    cmd->SetGraphicsRootSignature(presentRootSig.Get());
    cmd->SetPipelineState(psoPresentHud.Get());
    cmd->SetGraphicsRootConstantBufferView(0, presentCbUpload->GetGPUVirtualAddress() + cbOffset);
    cmd->SetGraphicsRootDescriptorTable(1, srvGpuAt(14u)); // t0=canvasClean, t1=canvasAbCompare, t2=codecDecodedTex, t3=uiTextTex
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}

void M1TaipeiSliceEffect::render(const FrameContext& ctx) {
    if (!impl_ || !impl_->canvasClean || !impl_->canvasAbCompare) {
        throw std::runtime_error("M1TaipeiSliceEffect::render called without initialized canvas");
    }
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(d3d_.commandListRaw());
    auto* dev = static_cast<ID3D12Device*>(d3d_.deviceRaw());
    if (!cmd || !dev) {
        throw std::runtime_error("D3D12 command list or device is null");
    }

    const uint32_t slot = d3d_.currentBackBufferIndex();
    const bool canonDxr    = !disableRT_ && activeSpec_.allowDxr;
    const bool canonCuda   = !disableCUDA_ && activeSpec_.allowCuda;
    const bool canonTensor = !disableTensor_ && activeSpec_.allowTensor;
    const bool abSplitActive = ctx.showOverlay && (ctx.hudMode == 2u);
    const bool effectiveCodecHijack = codecHijackActive_ || impl_->isCueSignalHijackAt(ctx.filmTimeSeconds);
    const bool codecViewActive = effectiveCodecHijack || (abSplitActive && abCompareEngine_ == AbCompareEngine::Codec);

    if (codecViewActive && !disableCodec_) {
        impl_->ensureLiveAccelerators(d3d_, false, false, true);
    }

    // 1. Render canonical CanvasClean once (timestamps 1..3 measure canonical live scene + particles + bloom + composite)
    impl_->recordScenePipelineToCanvas(
        d3d_, cmd, ctx,
        canonDxr, canonCuda, canonTensor,
        false, false,
        impl_->canvasClean.Get(), 5u,
        slot, true, true);

    impl_->updateLiveRtStatus(disableRT_, activeSpec_.allowDxr);

    // 2. Live Codec Round-Trip (`V` Signal Hijack or `A` Codec A/B):
    //    Uses opt-in external 1280x720 NV12 media/camera frame when active, otherwise CanvasClean NV12 readback.
    if (codecViewActive) {
        constexpr size_t kNv12Bytes = 1280u * 720u * 3u / 2u;
        if (impl_->externalNv12Active && impl_->externalNv12Buf.size() == kNv12Bytes) {
            if (!disableCodec_ && !d3d_.isWarp() && impl_->liveAccel.codecHardwareAvailable()) {
                LivePillarTelemetry vTel{};
                if (impl_->liveAccel.processCodecRoundTrip(
                        impl_->externalNv12Buf.data(), kNv12Bytes, false, impl_->codecStagingRgba, vTel)) {
                    impl_->livePillars[3] = vTel;
                    impl_->uploadCodecRgbaToGpu(cmd, slot, impl_->codecStagingRgba, true);
                    impl_->externalTexUploaded = true;
                    impl_->uploadedExternalSeq = impl_->externalNv12Seq;
                }
            } else {
                if (!impl_->externalTexUploaded || impl_->uploadedExternalSeq != impl_->externalNv12Seq || impl_->codecHwTexValid) {
                    impl_->convertExternalNv12ToRgba8(impl_->codecStagingRgba);
                    impl_->uploadCodecRgbaToGpu(cmd, slot, impl_->codecStagingRgba, false);
                    impl_->externalTexUploaded = true;
                    impl_->uploadedExternalSeq = impl_->externalNv12Seq;
                }
            }
        } else {
            impl_->externalTexUploaded = false;
            if (!disableCodec_ && !d3d_.isWarp() && impl_->liveAccel.codecHardwareAvailable()) {
                if (impl_->nv12SlotReady[slot]) {
                    D3D12_RANGE rr{0, kNv12Bytes};
                    const uint8_t* nv12Ptr = nullptr;
                    if (SUCCEEDED(impl_->nv12ReadbackBuf[slot]->Map(
                            0, &rr, reinterpret_cast<void**>(const_cast<uint8_t**>(&nv12Ptr)))) && nv12Ptr) {
                        LivePillarTelemetry vTel{};
                        const bool ok = impl_->liveAccel.processCodecRoundTrip(
                            nv12Ptr, kNv12Bytes, false, impl_->codecStagingRgba, vTel);
                        D3D12_RANGE wr{0, 0};
                        impl_->nv12ReadbackBuf[slot]->Unmap(0, &wr);
                        if (ok) {
                            impl_->livePillars[3] = vTel;
                            impl_->uploadCodecRgbaToGpu(cmd, slot, impl_->codecStagingRgba, true);
                        }
                    }
                }
                impl_->recordNv12PackAndReadback(cmd, slot);
                impl_->nv12SlotReady[slot] = true;
            }
        }
    }

    impl_->updateLiveAuxiliaryStatuses(
        d3d_.isWarp(),
        disableCUDA_, activeSpec_.allowCuda,
        disableTensor_, activeSpec_.allowTensor,
        disableCodec_, codecViewActive);

    // 3. Single-variable A/B Split-Screen comparison pass into isolated canvasAbCompare (never touches CanvasClean!)
    if (abSplitActive) {
        if (abCompareEngine_ == AbCompareEngine::RT && impl_->liveRtStatus.status == BackendStatus::Accelerated) {
            impl_->recordScenePipelineToCanvas(
                d3d_, cmd, ctx,
                false, canonCuda, canonTensor,
                true, true,
                impl_->canvasAbCompare.Get(), 6u,
                slot, false, false);
        } else if (abCompareEngine_ == AbCompareEngine::CUDA && impl_->livePillars[1].hardwareVerified) {
            impl_->recordScenePipelineToCanvas(
                d3d_, cmd, ctx,
                canonDxr, false, canonTensor,
                false, true,
                impl_->canvasAbCompare.Get(), 6u,
                slot, false, false);
        } else if (abCompareEngine_ == AbCompareEngine::Tensor && impl_->livePillars[2].hardwareVerified) {
            impl_->recordScenePipelineToCanvas(
                d3d_, cmd, ctx,
                canonDxr, canonCuda, false,
                true, false,
                impl_->canvasAbCompare.Get(), 6u,
                slot, false, false);
        }
    }

    // 4. Composite letterboxed swapchain output + Live Attribution HUD / 4-Pillar A-B Split
    d3d_.bindSwapchainRenderTarget();
    impl_->recordSwapchainPresentPass(
        dev, cmd,
        d3d_.width(), d3d_.height(),
        static_cast<DXGI_FORMAT>(d3d_.backBufferDxgiFormat()),
        d3d_.swapchainChannelBits(),
        slot, ctx, hud_, abCompareEngine_, effectiveCodecHijack);
}

bool M1TaipeiSliceEffect::renderHeadlessCanvasForTest(
    const FrameContext& ctx,
    std::vector<uint8_t>& outRgba,
    uint32_t& outWidth,
    uint32_t& outHeight,
    std::string* outError,
    std::vector<uint32_t>* outRgb10) {
    if (!impl_ || !impl_->canvasClean || !impl_->canvasAbCompare) {
        if (outError) *outError = "M1TaipeiSliceEffect canvas is not initialized";
        return false;
    }

    auto* dev = static_cast<ID3D12Device*>(d3d_.deviceRaw());
    if (!dev) {
        if (outError) *outError = "D3D12 device is null";
        return false;
    }

    const D3D12_RESOURCE_DESC cleanDesc = impl_->canvasClean->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 totalBytes = 0;
    dev->GetCopyableFootprints(&cleanDesc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC rbDesc = makeBufferDesc(totalBytes);
    ComPtr<ID3D12Resource> readbackTexBuf;
    ComPtr<ID3D12Resource> readbackAbBuf;
    if (FAILED(dev->CreateCommittedResource(
            &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackTexBuf))) || !readbackTexBuf) {
        if (outError) *outError = "Failed to allocate headless CanvasClean readback buffer";
        return false;
    }

    const bool canonDxr    = !disableRT_ && activeSpec_.allowDxr;
    const bool canonCuda   = !disableCUDA_ && activeSpec_.allowCuda;
    const bool canonTensor = !disableTensor_ && activeSpec_.allowTensor;
    const bool wantAbSplice = ctx.showOverlay && (ctx.hudMode == 2u) && (abCompareEngine_ != AbCompareEngine::Codec);
    if (wantAbSplice) {
        if (FAILED(dev->CreateCommittedResource(
                &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackAbBuf))) || !readbackAbBuf) {
            if (outError) *outError = "Failed to allocate headless CanvasAbCompare readback buffer";
            return false;
        }
    }

    impl_->slotPending = {};
    ++impl_->dxrGeneration;

    const bool execOk = d3d_.executeSingleCommandListAndWait([&](void* rawCmd) {
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(rawCmd);
        impl_->recordScenePipelineToCanvas(
            d3d_, cmd, ctx,
            canonDxr, canonCuda, canonTensor,
            false, false,
            impl_->canvasClean.Get(), 5u,
            kHeadlessSlot, true, false);

        if (wantAbSplice) {
            const bool abDxr    = (abCompareEngine_ == AbCompareEngine::RT)     ? false : canonDxr;
            const bool abCuda   = (abCompareEngine_ == AbCompareEngine::CUDA)   ? false : canonCuda;
            const bool abTensor = (abCompareEngine_ == AbCompareEngine::Tensor) ? false : canonTensor;
            impl_->recordScenePipelineToCanvas(
                d3d_, cmd, ctx,
                abDxr, abCuda, abTensor,
                abCuda == canonCuda, abTensor == canonTensor,
                impl_->canvasAbCompare.Get(), 6u,
                kHeadlessSlot, false, false);
        }

        transitionResource(cmd, impl_->canvasClean.Get(), kCanvasReadState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = impl_->canvasClean.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = readbackTexBuf.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLoc.PlacedFootprint = footprint;
        cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        transitionResource(cmd, impl_->canvasClean.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kCanvasReadState);

        if (wantAbSplice && readbackAbBuf) {
            transitionResource(cmd, impl_->canvasAbCompare.Get(), kCanvasReadState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION srcAb{};
            srcAb.pResource = impl_->canvasAbCompare.Get();
            srcAb.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcAb.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION dstAb{};
            dstAb.pResource = readbackAbBuf.Get();
            dstAb.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dstAb.PlacedFootprint = footprint;
            cmd->CopyTextureRegion(&dstAb, 0, 0, 0, &srcAb, nullptr);
            transitionResource(cmd, impl_->canvasAbCompare.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kCanvasReadState);
        }
    }, outError);

    if (!execOk) {
        return false;
    }

    impl_->retireVerifySlot(0u);
    impl_->readRtTimestampSlot(kHeadlessSlot);
    impl_->updateLiveRtStatus(disableRT_, activeSpec_.allowDxr);
    impl_->updateLiveAuxiliaryStatuses(
        d3d_.isWarp(),
        disableCUDA_, activeSpec_.allowCuda,
        disableTensor_, activeSpec_.allowTensor,
        disableCodec_, codecHijackActive_);

    outWidth = impl_->canvasW;
    outHeight = impl_->canvasH;
    if (!unpackRgb10ReadbackBuffer(readbackTexBuf.Get(), footprint, totalBytes, outWidth, outHeight, outRgba, outRgb10, outError)) {
        return false;
    }

    if (wantAbSplice && readbackAbBuf) {
        bool hwLeftVerified = false;
        if (abCompareEngine_ == AbCompareEngine::RT)          hwLeftVerified = (impl_->liveRtStatus.status == BackendStatus::Accelerated);
        else if (abCompareEngine_ == AbCompareEngine::CUDA)   hwLeftVerified = impl_->livePillars[1].hardwareVerified;
        else if (abCompareEngine_ == AbCompareEngine::Tensor) hwLeftVerified = impl_->livePillars[2].hardwareVerified;

        if (hwLeftVerified) {
            std::vector<uint8_t> abRgba;
            std::vector<uint32_t> abRgb10;
            if (!unpackRgb10ReadbackBuffer(
                    readbackAbBuf.Get(), footprint, totalBytes, outWidth, outHeight,
                    abRgba, outRgb10 ? &abRgb10 : nullptr, outError)) {
                return false;
            }
            const uint32_t splitX = outWidth / 2u;
            for (uint32_t y = 0; y < outHeight; ++y) {
                const size_t rowStart = (static_cast<size_t>(y) * outWidth + splitX) * 4u;
                const size_t copyBytes = static_cast<size_t>(outWidth - splitX) * 4u;
                std::memcpy(outRgba.data() + rowStart, abRgba.data() + rowStart, copyBytes);
                if (outRgb10 && abRgb10.size() == outRgb10->size()) {
                    const size_t pStart = static_cast<size_t>(y) * outWidth + splitX;
                    std::copy_n(abRgb10.data() + pStart, outWidth - splitX, outRgb10->data() + pStart);
                }
            }
        }
    }
    return true;
}

bool M1TaipeiSliceEffect::readSceneHdrFiniteForTest(uint64_t& nonFiniteComponents, std::string* error) {
    nonFiniteComponents = 0;
    if (!impl_ || !impl_->sceneHdr || !d3d_.deviceRaw()) {
        if (error) *error = "SceneHDR is not initialized";
        return false;
    }
    auto* dev = static_cast<ID3D12Device*>(d3d_.deviceRaw());
    const auto desc = impl_->sceneHdr->GetDesc();
    if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        if (error) *error = "SceneHDR finite gate requires FP16 RGBA";
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    const auto rd = makeBufferDesc(bytes);
    ComPtr<ID3D12Resource> readback;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) {
        if (error) *error = "Unable to allocate FP16 readback";
        return false;
    }
    if (!d3d_.executeSingleCommandListAndWait([&](void* raw) {
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(raw);
        transitionResource(cmd, impl_->sceneHdr.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = impl_->sceneHdr.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transitionResource(cmd, impl_->sceneHdr.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }, error)) return false;
    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    if (FAILED(readback->Map(0, &range, &mapped)) || !mapped) {
        if (error) *error = "Unable to map FP16 readback";
        return false;
    }
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped)
            + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch);
        for (UINT64 x = 0; x < desc.Width * 4; ++x) {
            // IEEE binary16 exponent all-ones is Infinity or NaN, independent of sign/mantissa.
            if ((row[x] & 0x7c00u) == 0x7c00u) ++nonFiniteComponents;
        }
    }
    D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    return true;
}

bool M1TaipeiSliceEffect::readLastRenderedCanvasForTest(
    std::vector<uint8_t>& outRgba,
    uint32_t& outWidth,
    uint32_t& outHeight,
    std::string* outError,
    std::vector<uint32_t>* outRgb10) {
    if (!impl_ || !impl_->canvasClean) {
        if (outError) *outError = "M1TaipeiSliceEffect canvas is not initialized";
        return false;
    }

    if (!d3d_.waitForGpu() || !d3d_.checkDeviceHealthy()) {
        if (outError) *outError = "Canvas readback aborted: graphics device unavailable";
        return false;
    }

    auto* dev = static_cast<ID3D12Device*>(d3d_.deviceRaw());
    if (!dev) {
        if (outError) *outError = "D3D12 device is null";
        return false;
    }

    const D3D12_RESOURCE_DESC cleanDesc = impl_->canvasClean->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 totalBytes = 0;
    dev->GetCopyableFootprints(&cleanDesc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC rbDesc = makeBufferDesc(totalBytes);
    ComPtr<ID3D12Resource> readbackTexBuf;
    if (FAILED(dev->CreateCommittedResource(
            &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackTexBuf))) || !readbackTexBuf) {
        if (outError) *outError = "Failed to allocate live CanvasClean readback buffer";
        return false;
    }

    const bool execOk = d3d_.executeSingleCommandListAndWait([&](void* rawCmd) {
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(rawCmd);
        transitionResource(cmd, impl_->canvasClean.Get(), kCanvasReadState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = impl_->canvasClean.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = readbackTexBuf.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLoc.PlacedFootprint = footprint;

        cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
        transitionResource(cmd, impl_->canvasClean.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kCanvasReadState);
    }, outError);

    if (!execOk) {
        return false;
    }

    outWidth = impl_->canvasW;
    outHeight = impl_->canvasH;
    return unpackRgb10ReadbackBuffer(
        readbackTexBuf.Get(), footprint, totalBytes, outWidth, outHeight, outRgba, outRgb10, outError);
}

bool M1TaipeiSliceEffect::renderHeadlessAbCompareForTest(
    const FrameContext& ctx,
    std::vector<uint8_t>& outCleanRgba,
    std::vector<uint8_t>& outAbCompareRgba,
    uint32_t& outWidth,
    uint32_t& outHeight,
    std::string* outError,
    std::vector<uint32_t>* outCleanRgb10,
    std::vector<uint32_t>* outAbCompareRgb10) {
    if (!impl_ || !impl_->canvasClean || !impl_->canvasAbCompare) {
        if (outError) *outError = "M1TaipeiSliceEffect canvas bundle is not initialized";
        return false;
    }

    auto* dev = static_cast<ID3D12Device*>(d3d_.deviceRaw());
    if (!dev) {
        if (outError) *outError = "D3D12 device is null";
        return false;
    }

    const D3D12_RESOURCE_DESC cleanDesc = impl_->canvasClean->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 totalBytes = 0;
    dev->GetCopyableFootprints(&cleanDesc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &totalBytes);

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC rbDesc = makeBufferDesc(totalBytes);
    ComPtr<ID3D12Resource> rbClean;
    ComPtr<ID3D12Resource> rbAb;
    if (FAILED(dev->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbClean))) ||
        FAILED(dev->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbAb)))) {
        if (outError) *outError = "Failed to allocate A/B readback buffers";
        return false;
    }

    const bool canonDxr    = !disableRT_ && activeSpec_.allowDxr;
    const bool canonCuda   = !disableCUDA_ && activeSpec_.allowCuda;
    const bool canonTensor = !disableTensor_ && activeSpec_.allowTensor;
    const bool abDxr       = (abCompareEngine_ == AbCompareEngine::RT)     ? false : canonDxr;
    const bool abCuda      = (abCompareEngine_ == AbCompareEngine::CUDA)   ? false : canonCuda;
    const bool abTensor    = (abCompareEngine_ == AbCompareEngine::Tensor) ? false : canonTensor;

    impl_->slotPending = {};
    ++impl_->dxrGeneration;

    const bool execOk = d3d_.executeSingleCommandListAndWait([&](void* rawCmd) {
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(rawCmd);
        impl_->recordScenePipelineToCanvas(
            d3d_, cmd, ctx,
            canonDxr, canonCuda, canonTensor,
            false, false,
            impl_->canvasClean.Get(), 5u,
            kHeadlessSlot, true, false);

        impl_->recordScenePipelineToCanvas(
            d3d_, cmd, ctx,
            abDxr, abCuda, abTensor,
            abCuda == canonCuda, abTensor == canonTensor,
            impl_->canvasAbCompare.Get(), 6u,
            kHeadlessSlot, false, false);

        transitionResource(cmd, impl_->canvasClean.Get(), kCanvasReadState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transitionResource(cmd, impl_->canvasAbCompare.Get(), kCanvasReadState, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION srcClean{};
        srcClean.pResource = impl_->canvasClean.Get();
        srcClean.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcClean.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dstClean{};
        dstClean.pResource = rbClean.Get();
        dstClean.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstClean.PlacedFootprint = footprint;
        cmd->CopyTextureRegion(&dstClean, 0, 0, 0, &srcClean, nullptr);

        D3D12_TEXTURE_COPY_LOCATION srcAb{};
        srcAb.pResource = impl_->canvasAbCompare.Get();
        srcAb.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcAb.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dstAb{};
        dstAb.pResource = rbAb.Get();
        dstAb.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstAb.PlacedFootprint = footprint;
        cmd->CopyTextureRegion(&dstAb, 0, 0, 0, &srcAb, nullptr);

        transitionResource(cmd, impl_->canvasClean.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kCanvasReadState);
        transitionResource(cmd, impl_->canvasAbCompare.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kCanvasReadState);
    }, outError);

    if (!execOk) {
        return false;
    }

    impl_->retireVerifySlot(0u);
    impl_->readRtTimestampSlot(kHeadlessSlot);
    impl_->updateLiveRtStatus(disableRT_, activeSpec_.allowDxr);
    impl_->updateLiveAuxiliaryStatuses(
        d3d_.isWarp(),
        disableCUDA_, activeSpec_.allowCuda,
        disableTensor_, activeSpec_.allowTensor,
        disableCodec_, codecHijackActive_);

    outWidth = impl_->canvasW;
    outHeight = impl_->canvasH;
    return unpackRgb10ReadbackBuffer(rbClean.Get(), footprint, totalBytes, outWidth, outHeight, outCleanRgba, outCleanRgb10, outError) &&
           unpackRgb10ReadbackBuffer(rbAb.Get(), footprint, totalBytes, outWidth, outHeight, outAbCompareRgba, outAbCompareRgb10, outError);
}

bool M1TaipeiSliceEffect::renderHeadlessPresentedFrameForTest(
    const FrameContext& ctx,
    const HudTelemetrySnapshot& hud,
    uint32_t presentWidth,
    uint32_t presentHeight,
    std::vector<uint8_t>& outPresentedRgba,
    std::string* outError) {
    if (!impl_ || !impl_->canvasClean || !impl_->canvasAbCompare) {
        if (outError) *outError = "M1TaipeiSliceEffect canvas bundle is not initialized";
        return false;
    }
    auto* dev = static_cast<ID3D12Device*>(d3d_.deviceRaw());
    if (!dev) {
        if (outError) *outError = "D3D12 device is null";
        return false;
    }

    presentWidth  = std::clamp<uint32_t>(presentWidth, 320u, 3840u);
    presentHeight = std::clamp<uint32_t>(presentHeight, 180u, 2160u);

    const bool canonDxr    = !disableRT_ && activeSpec_.allowDxr;
    const bool canonCuda   = !disableCUDA_ && activeSpec_.allowCuda;
    const bool canonTensor = !disableTensor_ && activeSpec_.allowTensor;
    const bool abSplitActive = ctx.showOverlay && (ctx.hudMode == 2u);
    const bool effectiveCodecHijack = codecHijackActive_ || impl_->isCueSignalHijackAt(ctx.filmTimeSeconds);
    const bool codecViewActive = effectiveCodecHijack || (abSplitActive && abCompareEngine_ == AbCompareEngine::Codec);

    if (codecViewActive && !disableCodec_) {
        impl_->ensureLiveAccelerators(d3d_, false, false, true);
    }

    impl_->slotPending = {};
    ++impl_->dxrGeneration;

    // Phase A: Render canonical CanvasClean (+ single-variable A/B or NV12 pack) and resolve exact GPU timestamps
    const bool phaseAOk = d3d_.executeSingleCommandListAndWait([&](void* rawCmd) {
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(rawCmd);
        impl_->recordScenePipelineToCanvas(
            d3d_, cmd, ctx,
            canonDxr, canonCuda, canonTensor,
            false, false,
            impl_->canvasClean.Get(), 5u,
            kHeadlessSlot, true, false);

        if (abSplitActive && abCompareEngine_ != AbCompareEngine::Codec) {
            const bool abDxr    = (abCompareEngine_ == AbCompareEngine::RT)     ? false : canonDxr;
            const bool abCuda   = (abCompareEngine_ == AbCompareEngine::CUDA)   ? false : canonCuda;
            const bool abTensor = (abCompareEngine_ == AbCompareEngine::Tensor) ? false : canonTensor;
            impl_->recordScenePipelineToCanvas(
                d3d_, cmd, ctx,
                abDxr, abCuda, abTensor,
                abCuda == canonCuda, abTensor == canonTensor,
                impl_->canvasAbCompare.Get(), 6u,
                kHeadlessSlot, false, false);
        }

        if (codecViewActive && !impl_->externalNv12Active && !disableCodec_ && !d3d_.isWarp() && impl_->liveAccel.codecHardwareAvailable()) {
            impl_->recordNv12PackAndReadback(cmd, kHeadlessSlot);
        }
    }, outError);

    if (!phaseAOk) {
        return false;
    }

    impl_->retireVerifySlot(0u);
    impl_->readRtTimestampSlot(kHeadlessSlot);
    impl_->updateLiveRtStatus(disableRT_, activeSpec_.allowDxr);

    // Synchronous exact-frame Hardware H.264 encode+decode round-trip (or external SHADER SIGNAL upload) when Codec view is active!
    bool needCodecUpload = false;
    bool uploadMarksHwVerified = false;
    if (codecViewActive) {
        constexpr size_t kNv12Bytes = 1280u * 720u * 3u / 2u;
        if (impl_->externalNv12Active && impl_->externalNv12Buf.size() == kNv12Bytes) {
            if (!disableCodec_ && !d3d_.isWarp() && impl_->liveAccel.codecHardwareAvailable()) {
                LivePillarTelemetry vTel{};
                if (impl_->liveAccel.processCodecRoundTrip(
                        impl_->externalNv12Buf.data(), kNv12Bytes, true, impl_->codecStagingRgba, vTel)) {
                    impl_->livePillars[3] = vTel;
                    impl_->codecHwTexValid = true;
                    impl_->externalTexUploaded = true;
                    needCodecUpload = true;
                    uploadMarksHwVerified = true;
                }
            } else {
                impl_->convertExternalNv12ToRgba8(impl_->codecStagingRgba);
                impl_->codecHwTexValid = false;
                impl_->externalTexUploaded = true;
                needCodecUpload = true;
                uploadMarksHwVerified = false;
            }
        } else {
            impl_->externalTexUploaded = false;
            if (!disableCodec_ && !d3d_.isWarp() && impl_->liveAccel.codecHardwareAvailable()) {
                D3D12_RANGE rr{0, kNv12Bytes};
                const uint8_t* nv12Ptr = nullptr;
                if (SUCCEEDED(impl_->nv12ReadbackBuf[kHeadlessSlot]->Map(
                        0, &rr, reinterpret_cast<void**>(const_cast<uint8_t**>(&nv12Ptr)))) && nv12Ptr) {
                    LivePillarTelemetry vTel{};
                    const bool ok = impl_->liveAccel.processCodecRoundTrip(
                        nv12Ptr, kNv12Bytes, true, impl_->codecStagingRgba, vTel);
                    D3D12_RANGE wr{0, 0};
                    impl_->nv12ReadbackBuf[kHeadlessSlot]->Unmap(0, &wr);
                    if (ok) {
                        impl_->livePillars[3] = vTel;
                        impl_->codecHwTexValid = true;
                        needCodecUpload = true;
                        uploadMarksHwVerified = true;
                    }
                }
            }
        }
    }

    impl_->updateLiveAuxiliaryStatuses(
        d3d_.isWarp(),
        disableCUDA_, activeSpec_.allowCuda,
        disableTensor_, activeSpec_.allowTensor,
        disableCodec_, codecViewActive);

    // Phase B: Allocate offscreen RGBA8 presentation target + RTV, upload decoded H.264 if needed, execute Pass 4 HUD/A-B Present
    const D3D12_RESOURCE_DESC presDesc = makeTex2dDesc(
        presentWidth, presentHeight, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT presFootprint{};
    UINT numRows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    dev->GetCopyableFootprints(&presDesc, 0, 1, 0, &presFootprint, &numRows, &rowBytes, &totalBytes);

    D3D12_HEAP_PROPERTIES defaultHeap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_HEAP_PROPERTIES rbHeap{D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC rbDesc = makeBufferDesc(totalBytes);

    ComPtr<ID3D12Resource> presTex;
    ComPtr<ID3D12Resource> presRb;
    ComPtr<ID3D12DescriptorHeap> tempRtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC thd{};
    thd.NumDescriptors = 1;
    thd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

    if (FAILED(dev->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &presDesc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&presTex))) ||
        FAILED(dev->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&presRb))) ||
        FAILED(dev->CreateDescriptorHeap(&thd, IID_PPV_ARGS(&tempRtvHeap)))) {
        if (outError) *outError = "Failed to allocate headless presentation target";
        return false;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE presRtv = tempRtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(presTex.Get(), nullptr, presRtv);

    HudTelemetrySnapshot effHud = hud;
    if (effHud.gpuFrameMs <= 0.05f) {
        effHud.gpuFrameMs = impl_->livePillars[0].executionMs +
                            impl_->livePillars[1].executionMs +
                            impl_->livePillars[2].executionMs + 0.35f;
    }

    const bool phaseBOk = d3d_.executeSingleCommandListAndWait([&](void* rawCmd) {
        auto* cmd = static_cast<ID3D12GraphicsCommandList*>(rawCmd);
        if (needCodecUpload) {
            impl_->uploadCodecRgbaToGpu(cmd, kHeadlessSlot, impl_->codecStagingRgba, uploadMarksHwVerified);
        }
        cmd->OMSetRenderTargets(1, &presRtv, FALSE, nullptr);
        impl_->recordSwapchainPresentPass(
            dev, cmd,
            presentWidth, presentHeight,
            DXGI_FORMAT_R8G8B8A8_UNORM, 8u,
            kHeadlessSlot, ctx, effHud, abCompareEngine_, effectiveCodecHijack);

        transitionResource(cmd, presTex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = presTex.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = presRb.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dstLoc.PlacedFootprint = presFootprint;
        cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }, outError);

    if (!phaseBOk) {
        return false;
    }

    return unpackRgba8ReadbackBuffer(presRb.Get(), presFootprint, totalBytes, presentWidth, presentHeight, outPresentedRgba, outError);
}

bool M1TaipeiSliceEffect::captureClean(const FrameContext& ctx, const std::filesystem::path& directory, std::string* error) {
    try {
        FrameContext clean = ctx;
        clean.hudMode = 3;
        clean.showOverlay = false;
        clean.audioReactive = {};
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        if (!renderHeadlessCanvasForTest(clean, rgba, w, h, error)) return false;
        std::filesystem::create_directories(directory);
        const auto stem = directory / ("frame_" + std::to_string(ctx.samplePosition) + "_" + std::to_string(GetTickCount64()));
        const auto bmpTmpPath  = std::filesystem::path(stem.string() + ".bmp.tmp");
        const auto bmpFinalPath = std::filesystem::path(stem.string() + ".bmp");
        const auto jsonTmpPath = std::filesystem::path(stem.string() + ".json.tmp");
        const auto jsonFinalPath = std::filesystem::path(stem.string() + ".json");

        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + w * h * 4u;
        info.biSize = sizeof(info);
        info.biWidth = static_cast<LONG>(w);
        info.biHeight = -static_cast<LONG>(h);
        info.biPlanes = 1;
        info.biBitCount = 32;
        info.biCompression = BI_RGB;
        for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
        {
            std::ofstream bmp(bmpTmpPath, std::ios::binary | std::ios::trunc);
            bmp.write(reinterpret_cast<const char*>(&file), sizeof(file));
            bmp.write(reinterpret_cast<const char*>(&info), sizeof(info));
            bmp.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
            bmp.flush();
            if (!bmp.good()) {
                if (error) *error = "Capture BMP temp write failed.";
                return false;
            }
        }
        const auto timings = d3d_.lastPassTimings();
        {
            std::ofstream json(jsonTmpPath, std::ios::binary | std::ios::trunc);
            json << "{\n  \"samplePosition\": " << ctx.samplePosition
                 << ",\n  \"preset\": \"" << toString(activeSpec_.id)
                 << "\",\n  \"width\": " << w << ", \"height\": " << h
                 << ",\n  \"swapchainBits\": " << d3d_.swapchainChannelBits()
                 << ",\n  \"cleanBits\": 10, \"exportBits\": 8,"
                 << "\n  \"rtBackend\": \"" << impl_->liveRtStatus.activeBackend
                 << "\",\n  \"cudaBackend\": \"" << impl_->liveCudaStatus.activeBackend
                 << "\",\n  \"tensorBackend\": \"" << impl_->liveTensorStatus.activeBackend
                 << "\",\n  \"codecBackend\": \"" << impl_->liveCodecStatus.activeBackend
                 << "\",\n  \"livePillarsMs\": {\"rt\": " << impl_->livePillars[0].executionMs
                 << ", \"cuda\": " << impl_->livePillars[1].executionMs
                 << ", \"tensor\": " << impl_->livePillars[2].executionMs
                 << ", \"codec\": " << impl_->livePillars[3].executionMs << "},"
                 << "\n  \"gpuPassMs\": {\"scene\": " << timings.sceneMs
                 << ", \"bloom\": " << timings.bloomMs << ", \"composite\": " << timings.compositeMs
                 << ", \"present\": " << timings.presentPassMs << "},"
                 << "\n  \"timingSource\": \"lastCompletedPresentation\"\n}\n";
            json.flush();
            if (!json.good()) {
                if (error) *error = "Capture JSON temp write failed.";
                return false;
            }
        }
        std::filesystem::rename(bmpTmpPath, bmpFinalPath);
        std::filesystem::rename(jsonTmpPath, jsonFinalPath);
        BOO_LOG_INFO("Capture", bmpFinalPath.string());
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

void M1TaipeiSliceEffect::shutdown() noexcept {
    if (!impl_) return;
    d3d_.waitForGpu();
    impl_->liveAccel.shutdown();
    impl_.reset();
}

} // namespace boo
