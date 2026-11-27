#include "boo/live_accelerators.hpp"
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
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace boo {

using Microsoft::WRL::ComPtr;

namespace {

std::string wideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    out.pop_back();
    return out;
}

uint16_t floatToFp16Bits(float value) noexcept {
    uint32_t fbits = 0;
    std::memcpy(&fbits, &value, sizeof(uint32_t));
    const uint32_t sign = (fbits >> 16u) & 0x8000u;
    int32_t exp = static_cast<int32_t>((fbits >> 23u) & 0xFFu) - 127 + 15;
    uint32_t mant = fbits & 0x007FFFFFu;
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant = (mant | 0x00800000u) >> static_cast<uint32_t>(1 - exp);
        return static_cast<uint16_t>(sign | ((mant + 0x00001000u) >> 13u));
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10u) | ((mant + 0x00001000u) >> 13u));
}

float fp16BitsToFloat(uint16_t h) noexcept {
    const uint32_t sign = (static_cast<uint32_t>(h & 0x8000u)) << 16u;
    const uint32_t exp  = (h >> 10u) & 0x1Fu;
    const uint32_t mant = h & 0x03FFu;
    uint32_t outBits = 0;
    if (exp == 0u) {
        if (mant == 0u) {
            outBits = sign;
        } else {
            uint32_t m = mant;
            int32_t e = -14;
            while ((m & 0x0400u) == 0u) {
                m <<= 1u;
                --e;
            }
            m &= 0x03FFu;
            outBits = sign | (static_cast<uint32_t>(e + 127) << 23u) | (m << 13u);
        }
    } else if (exp == 31u) {
        outBits = sign | 0x7F800000u | (mant << 13u);
    } else {
        outBits = sign | ((exp + (127u - 15u)) << 23u) | (mant << 13u);
    }
    float res = 0.0f;
    std::memcpy(&res, &outBits, sizeof(float));
    return res;
}

uint32_t packHalf2Bits(float lo, float hi) noexcept {
    return static_cast<uint32_t>(floatToFp16Bits(lo)) |
           (static_cast<uint32_t>(floatToFp16Bits(hi)) << 16u);
}

// ============================================================================
// Dynamic CUDA Driver API Table (`nvcuda.dll` via LoadLibraryExW, zero SDK!)
// ============================================================================
struct DynamicCudaDriver {
    HMODULE lib = nullptr;
    int (*cuInit)(unsigned int) = nullptr;
    int (*cuDeviceGetCount)(int*) = nullptr;
    int (*cuDeviceGet)(int*, int) = nullptr;
    int (*cuDeviceGetName)(char*, int, int) = nullptr;
    int (*cuDeviceGetAttribute)(int*, int, int) = nullptr;
    int (*cuDeviceGetLuid)(char*, unsigned int*, int) = nullptr;
    int (*cuDevicePrimaryCtxRetain)(void**, int) = nullptr;
    int (*cuDevicePrimaryCtxRelease)(int) = nullptr;
    int (*cuCtxSetCurrent)(void*) = nullptr;
    int (*cuCtxSynchronize)() = nullptr;
    int (*cuStreamCreate)(void**, unsigned int) = nullptr;
    int (*cuStreamDestroy)(void*) = nullptr;
    int (*cuStreamSynchronize)(void*) = nullptr;
    int (*cuModuleLoadDataEx)(void**, const void*, unsigned int, void*, void**) = nullptr;
    int (*cuModuleUnload)(void*) = nullptr;
    int (*cuModuleGetFunction)(void**, void*, const char*) = nullptr;
    int (*cuMemAlloc)(uint64_t*, size_t) = nullptr;
    int (*cuMemFree)(uint64_t) = nullptr;
    int (*cuMemAllocHost)(void**, size_t) = nullptr;
    int (*cuMemFreeHost)(void*) = nullptr;
    int (*cuMemcpyHtoD)(uint64_t, const void*, size_t) = nullptr;
    int (*cuMemcpyDtoH)(void*, uint64_t, size_t) = nullptr;
    int (*cuMemcpyDtoHAsync)(void*, uint64_t, size_t, void*) = nullptr;
    int (*cuLaunchKernel)(
        void*,
        unsigned int, unsigned int, unsigned int,
        unsigned int, unsigned int, unsigned int,
        unsigned int, void*, void**, void**) = nullptr;
    int (*cuEventCreate)(void**, unsigned int) = nullptr;
    int (*cuEventDestroy)(void*) = nullptr;
    int (*cuEventRecord)(void*, void*) = nullptr;
    int (*cuEventSynchronize)(void*) = nullptr;
    int (*cuEventElapsedTime)(float*, void*, void*) = nullptr;

    bool load() noexcept {
        if (lib) return true;
        lib = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib) return false;
        auto sym = [&](auto& fnPtr, const char* name, const char* alt = nullptr) noexcept -> bool {
            void* p = reinterpret_cast<void*>(GetProcAddress(lib, name));
            if (!p && alt) p = reinterpret_cast<void*>(GetProcAddress(lib, alt));
            fnPtr = reinterpret_cast<std::decay_t<decltype(fnPtr)>>(p);
            return fnPtr != nullptr;
        };
        const bool ok =
            sym(cuInit, "cuInit") &&
            sym(cuDeviceGetCount, "cuDeviceGetCount") &&
            sym(cuDeviceGet, "cuDeviceGet") &&
            sym(cuDeviceGetName, "cuDeviceGetName") &&
            sym(cuDeviceGetAttribute, "cuDeviceGetAttribute") &&
            sym(cuDeviceGetLuid, "cuDeviceGetLuid") &&
            sym(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain") &&
            sym(cuDevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2", "cuDevicePrimaryCtxRelease") &&
            sym(cuCtxSetCurrent, "cuCtxSetCurrent") &&
            sym(cuCtxSynchronize, "cuCtxSynchronize") &&
            sym(cuStreamCreate, "cuStreamCreate") &&
            sym(cuStreamDestroy, "cuStreamDestroy_v2", "cuStreamDestroy") &&
            sym(cuStreamSynchronize, "cuStreamSynchronize") &&
            sym(cuModuleLoadDataEx, "cuModuleLoadDataEx") &&
            sym(cuModuleUnload, "cuModuleUnload") &&
            sym(cuModuleGetFunction, "cuModuleGetFunction") &&
            sym(cuMemAlloc, "cuMemAlloc_v2", "cuMemAlloc") &&
            sym(cuMemFree, "cuMemFree_v2", "cuMemFree") &&
            sym(cuMemcpyHtoD, "cuMemcpyHtoD_v2", "cuMemcpyHtoD") &&
            sym(cuMemcpyDtoH, "cuMemcpyDtoH_v2", "cuMemcpyDtoH") &&
            sym(cuLaunchKernel, "cuLaunchKernel") &&
            sym(cuEventCreate, "cuEventCreate") &&
            sym(cuEventDestroy, "cuEventDestroy_v2", "cuEventDestroy") &&
            sym(cuEventRecord, "cuEventRecord") &&
            sym(cuEventSynchronize, "cuEventSynchronize") &&
            sym(cuEventElapsedTime, "cuEventElapsedTime");

        // Optional pinned-memory & async-copy symbols (graceful fallback if absent)
        sym(cuMemAllocHost, "cuMemAllocHost_v2", "cuMemAllocHost");
        sym(cuMemFreeHost, "cuMemFreeHost");
        sym(cuMemcpyDtoHAsync, "cuMemcpyDtoHAsync_v2", "cuMemcpyDtoHAsync");
        if (!ok) unload();
        return ok;
    }

    void unload() noexcept {
        if (lib) {
            FreeLibrary(lib);
            lib = nullptr;
        }
    }

    ~DynamicCudaDriver() {
        unload();
    }

    // Strict LUID match to the active D3D12 hardware adapter so we never claim CUDA/Tensor on a wrong GPU.
    int findExactAdapterDevice(
        int32_t luidLow,
        int32_t luidHigh,
        std::string& outName,
        int& outMajor,
        int& outMinor,
        int& outSmCount) noexcept {
        if (!lib || cuInit(0) != 0) return -1;
        int count = 0;
        if (cuDeviceGetCount(&count) != 0 || count <= 0) return -1;
        for (int i = 0; i < count; ++i) {
            int d = -1;
            if (cuDeviceGet(&d, i) != 0) continue;
            LUID luid{};
            unsigned int nodeMask = 0;
            if (cuDeviceGetLuid(reinterpret_cast<char*>(&luid), &nodeMask, d) == 0) {
                if (luid.LowPart == static_cast<DWORD>(luidLow) && luid.HighPart == luidHigh) {
                    char nbuf[128]{};
                    cuDeviceGetName(nbuf, sizeof(nbuf), d);
                    outName = nbuf;
                    cuDeviceGetAttribute(&outSmCount, 16, d);
                    cuDeviceGetAttribute(&outMajor, 75, d);
                    cuDeviceGetAttribute(&outMinor, 76, d);
                    return d;
                }
            }
        }
        return -1;
    }
};

// ============================================================================
// PTX 1: Deterministic 65,536-Particle x 16-Substep 3D Curl-Vortex Simulation
// ============================================================================
static const char* kLiveCudaParticlesPtx = R"PTX(
.version 7.0
.target sm_75
.address_size 64

.visible .entry boo_live_cuda_particles(
    .param .u64 p_out,
    .param .u64 p_verify,
    .param .u32 p_count,
    .param .u32 p_seed,
    .param .f32 p_time,
    .param .f32 p_reveal,
    .param .u32 p_mode
)
{
    .reg .pred %p<8>;
    .reg .b32  %r<24>;
    .reg .b64  %rd<6>;
    .reg .f32  %f<48>;
    .reg .f32 %progress, %theta, %cosTheta, %sinTheta, %phi, %cosPhi, %sinPhi;
    .reg .f32 %diskRadius, %helixRadius, %helixY, %minor, %ringRadius, %ringY;
    .reg .f32 %weight, %weightRate, %deltaRadius, %deltaY, %radius, %radialRate;
    .reg .f32 %azimuthSpeed, %minorSpeed, %scratch;

    mov.u32 %r0, %ctaid.x;
    mov.u32 %r1, %ntid.x;
    mov.u32 %r2, %tid.x;
    mad.lo.u32 %r3, %r0, %r1, %r2;
    ld.param.u32 %r4, [p_count];
    setp.ge.u32 %p0, %r3, %r4;
    @%p0 bra DONE_PART;

    ld.param.u64 %rd0, [p_out];
    ld.param.u64 %rd1, [p_verify];
    ld.param.u32 %r5,  [p_seed];
    ld.param.f32 %f0,  [p_time];
    ld.param.f32 %f1,  [p_reveal];
    ld.param.u32 %r17, [p_mode];

    // PCG-style deterministic hashes h0..h3 in [0, 1)
    xor.b32 %r6, %r3, %r5;
    mul.lo.u32 %r6, %r6, 0x2C9277B5;
    add.u32 %r6, %r6, 0xAC564B05;
    shr.u32 %r7, %r6, 16;
    xor.b32 %r6, %r6, %r7;
    mul.lo.u32 %r6, %r6, 277803737;

    and.b32 %r8, %r6, 65535;
    cvt.rn.f32.u32 %f2, %r8;
    mul.f32 %f2, %f2, 0f37800000; // h0 in [0,1)

    shr.u32 %r9, %r6, 16;
    cvt.rn.f32.u32 %f3, %r9;
    mul.f32 %f3, %f3, 0f37800000; // h1 in [0,1)

    mul.lo.u32 %r10, %r6, 1664525;
    add.u32 %r10, %r10, 1013904223;
    and.b32 %r11, %r10, 65535;
    cvt.rn.f32.u32 %f4, %r11;
    mul.f32 %f4, %f4, 0f37800000; // h2 in [0,1)

    shr.u32 %r12, %r10, 16;
    cvt.rn.f32.u32 %f5, %r12;
    mul.f32 %f5, %f5, 0f37800000; // h3 in [0,1)

    // Mode 1 (Particle Mutiny M3 / Act 4): 100k-262k seeded double-helix <-> crown torus morph
    setp.ne.u32 %p5, %r17, 0;
    @%p5 bra INIT_MUTINY;

    // Fixed species per particle index for 100% temporal coherence:
    // 37.5% (3 of every 8) = golden/orchid sanctuary pollen & crystal motes; 62.5% = Taipei rain-mist filaments
    and.b32 %r13, %r3, 7;
    setp.lt.u32 %p1, %r13, 3;
    @%p1 bra INIT_POLLEN;

    // --- Species 0: Taipei Aerodynamic Rain-Mist Filament ---
    fma.rn.f32 %f6, %f0, 0f3E8F5C29, %f2; // phase = time * 0.28 + h0
    cvt.rzi.u32.f32 %r14, %f6;
    cvt.rn.f32.u32 %f7, %r14;
    sub.f32 %f8, %f6, %f7;                // age01 in [0, 1)

    // Boulevard & monolith corridor volume: x in [-5.2, +5.2], y falling [13.5 -> 0.05], z in [-13.5, +9.5]
    fma.rn.f32 %f10, %f3, 0f41266666, 0fC0A66666; // px0 = h1 * 10.4 - 5.2
    sub.f32 %f11, 0f3F800000, %f8;
    fma.rn.f32 %f11, %f11, 0f41500000, 0f3DF5C28F; // py0 = (1 - age) * 13.0 + 0.12
    fma.rn.f32 %f12, %f4, 0f41B80000, 0fC1580000; // pz0 = h2 * 23.0 - 13.5

    // Initial wind-sheared rain velocity
    fma.rn.f32 %f13, %f5, 0f3E800000, 0f3E99999A; // vx = 0.30 + 0.25 * h3
    mov.f32 %f14, 0fC0C66666;                     // vy = -6.20
    mov.f32 %f15, 0fBF4CCCCD;                     // vz = -0.80
    mov.f32 %f16, 0f3E4CCCCD;                     // kindAndGlow = 0.20 (rain species < 0.5)
    bra SUBSTEP_START;

INIT_POLLEN:
    // --- Species 1: Bioluminescent Sanctuary Pollen & Faceted Crystal Motes ---
    fma.rn.f32 %f6, %f0, 0f3DE147AE, %f2; // phase = time * 0.11 + h0
    cvt.rzi.u32.f32 %r14, %f6;
    cvt.rn.f32.u32 %f7, %r14;
    sub.f32 %f8, %f6, %f7;                // age01 in [0, 1)

    // Hollow toroidal sanctuary halo rising around Glass Orchid hub (0, 7.80, 5.50).
    fma.rn.f32 %f17, %f0, 0f3F0CCCCD, 0f00000000; // slow orbital spin
    fma.rn.f32 %f18, %f3, 0f40C90FDB, %f17;       // angle = h1 * 2pi + spin
    fma.rn.f32 %f19, %f4, 0f40200000, 0f4029999A; // radius = 2.65 + h2 * 2.50 (r in [2.65, 5.15])
    cos.approx.f32 %f20, %f18;
    sin.approx.f32 %f21, %f18;

    mul.f32 %f10, %f20, %f19;                     // px0 = cos(a) * rad
    // Blend vertical center from street/corridor (1.2..9.6) toward Orchid sanctuary (5.2..10.4) on reveal
    fma.rn.f32 %f22, %f5, 0f41066666, 0f3F99999A; // baseY = 1.2 + h3 * 8.4
    fma.rn.f32 %f23, %f5, 0f40A66666, 0f40A66666; // orchidY = 5.2 + h3 * 5.2
    sub.f32 %f24, %f23, %f22;
    fma.rn.f32 %f11, %f1, %f24, %f22;             // py0 = lerp(baseY, orchidY, reveal)

    // Blend Z center from boulevard (-9..+6) toward Glass Orchid hub Z=5.50 on reveal
    fma.rn.f32 %f25, %f2, 0f41700000, 0fC1100000; // baseZ = -9.0 + h0 * 15.0
    fma.rn.f32 %f26, %f21, %f19, 0f40B00000;      // orchidZ = 5.50 + sin(a) * rad
    sub.f32 %f27, %f26, %f25;
    fma.rn.f32 %f12, %f1, %f27, %f25;             // pz0 = lerp(baseZ, orchidZ, reveal)

    // Initial tangential helical updraft velocity
    neg.f32 %f13, %f21;
    mul.f32 %f13, %f13, 0f3FACCCCD;               // vx = -sin(a) * 1.35
    mov.f32 %f14, 0f3F266666;                     // vy = +0.65
    mul.f32 %f15, %f20, 0f3FACCCCD;               // vz = +cos(a) * 1.35
    fma.rn.f32 %f16, %f5, 0f3EE66666, 0f3F0CCCCD; // kindAndGlow in [0.55, 1.00] (pollen/crystal >= 0.5)
    bra SUBSTEP_START;

INIT_MUTINY:
    // A particle advances along one of two helices. One longitudinal parameter
    // drives both height and azimuth. Fade at the ends before wrapping to the base.
    fma.rn.f32 %progress, %f0, 0f3D23D70A, %f2;
    cvt.rmi.f32.f32 %f7, %progress;
    sub.f32 %f8, %progress, %f7;
    and.b32 %r18, %r3, 4;                         // independent of tracer/species bits
    setp.ne.u32 %p6, %r18, 0;
    selp.f32 %theta, 0f40490FDB, 0f00000000, %p6;
    fma.rn.f32 %theta, %f8, 0f41490FDB, %theta;
    fma.rn.f32 %theta, %f0, 0f3E75C28F, %theta;
    cos.approx.f32 %cosTheta, %theta;
    sin.approx.f32 %sinTheta, %theta;

    // Uniform disk around each centerline: h1 sets the transverse direction
    // and sqrt(h2) gives even area density across the 0.24-radius tube.
    mul.f32 %phi, %f3, 0f40C90FDB;
    cos.approx.f32 %cosPhi, %phi;
    sin.approx.f32 %sinPhi, %phi;
    sqrt.approx.f32 %diskRadius, %f4;
    mul.f32 %diskRadius, %diskRadius, 0f3E75C28F;
    fma.rn.f32 %helixRadius, %diskRadius, %cosPhi, 0f4039999A;
    fma.rn.f32 %helixY, %f8, 0f40C66666, 0f403CCCCD;
    fma.rn.f32 %helixY, %diskRadius, %sinPhi, %helixY;

    // Hold the major azimuth through the morph, so the streams never fold
    // through the axis. The torus has a thin 0.78..0.90 shell about radius 3.75.
    fma.rn.f32 %phi, %f0, 0f3F3851EC, %phi;
    cos.approx.f32 %cosPhi, %phi;
    sin.approx.f32 %sinPhi, %phi;
    fma.rn.f32 %minor, %f5, 0f3DF5C28F, 0f3F47AE14;
    fma.rn.f32 %ringRadius, %minor, %cosPhi, 0f40700000;
    fma.rn.f32 %ringY, %minor, %sinPhi, 0f40B1999A;

    mul.f32 %scratch, %f0, 0f3EB33333;
    cos.approx.f32 %weight, %scratch;
    sin.approx.f32 %weightRate, %scratch;
    fma.rn.f32 %weight, %weight, 0fBF000000, 0f3F000000;
    mul.f32 %weightRate, %weightRate, 0f3E333333;
    sub.f32 %deltaRadius, %ringRadius, %helixRadius;
    sub.f32 %deltaY, %ringY, %helixY;
    fma.rn.f32 %radius, %weight, %deltaRadius, %helixRadius;
    mul.f32 %f10, %radius, %cosTheta;
    fma.rn.f32 %f11, %weight, %deltaY, %helixY;
    fma.rn.f32 %f12, %radius, %sinTheta, 0f40B00000;

    // Differentiate the moving cylindrical coordinates before applying curl.
    // theta' = 4*pi*0.04 + 0.24, helixY' = 6.2*0.04, and phi' = 0.72.
    mul.f32 %azimuthSpeed, %radius, 0f3F3E1EA0;
    mul.f32 %minorSpeed, %minor, 0f3F3851EC;
    mul.f32 %minorSpeed, %minorSpeed, %weight;
    mul.f32 %radialRate, %weightRate, %deltaRadius;
    neg.f32 %scratch, %sinPhi;
    fma.rn.f32 %radialRate, %minorSpeed, %scratch, %radialRate;
    mul.f32 %f13, %azimuthSpeed, %sinTheta;
    neg.f32 %f13, %f13;
    fma.rn.f32 %f13, %radialRate, %cosTheta, %f13;
    sub.f32 %f14, 0f3F800000, %weight;
    mul.f32 %f14, %f14, 0f3E7DF3B6;
    fma.rn.f32 %f14, %weightRate, %deltaY, %f14;
    fma.rn.f32 %f14, %minorSpeed, %cosPhi, %f14;
    mul.f32 %f15, %azimuthSpeed, %cosTheta;
    fma.rn.f32 %f15, %radialRate, %sinTheta, %f15;

    and.b32 %r19, %r3, 3;
    setp.eq.u32 %p7, %r19, 0;
    fma.rn.f32 %f42, %f5, 0f3EE66666, 0f3F0CCCCD;
    selp.f32 %f16, 0f3EA3D70A, %f42, %p7;

SUBSTEP_START:
    // 16 substeps of divergence-free 3D trigonometric curl-vortex + sanctuary orchid helical attraction
    mov.u32 %r15, 0;
CURL_LOOP:
    setp.ge.u32 %p2, %r15, 16;
    @%p2 bra WRITE_PARTICLE;

    // Spatial-temporal curl coordinates
    fma.rn.f32 %f20, %f10, 0f3F400000, %f0; // sx = px * 0.75 + t
    fma.rn.f32 %f21, %f11, 0f3F400000, %f0; // sy = py * 0.75 + t
    fma.rn.f32 %f22, %f12, 0f3F400000, %f0; // sz = pz * 0.75 + t

    // Curl X = sin(sy) - cos(sz)
    sin.approx.f32 %f23, %f21;
    cos.approx.f32 %f24, %f22;
    sub.f32 %f25, %f23, %f24;

    // Curl Y = sin(sz) - cos(sx)
    sin.approx.f32 %f26, %f22;
    cos.approx.f32 %f27, %f20;
    sub.f32 %f28, %f26, %f27;

    // Curl Z = sin(sx) - cos(sy)
    sin.approx.f32 %f29, %f20;
    cos.approx.f32 %f30, %f21;
    sub.f32 %f31, %f29, %f30;

    // Sanctuary Glass Orchid (0, 7.80, 5.50) tangential swirl force
    sub.f32 %f32, %f10, 0f00000000; // dx = px
    sub.f32 %f33, %f12, 0f40B00000; // dz = pz - 5.50
    mul.f32 %f34, %f32, %f32;
    fma.rn.f32 %f34, %f33, %f33, %f34;
    add.f32 %f34, %f34, 0f3F800000; // r2 + 1.0
    rcp.approx.f32 %f35, %f34;      // 1 / (r2 + 1)
    neg.f32 %f36, %f33;
    mul.f32 %f36, %f36, %f35;       // swirlX = -dz / (r2 + 1)
    mul.f32 %f37, %f32, %f35;       // swirlZ = +dx / (r2 + 1)

    // Update velocity with damping + curl turbulence + sanctuary swirl
    mul.f32 %f13, %f13, 0f3F7C28F6;
    fma.rn.f32 %f13, %f25, 0f3E3851EC, %f13;
    fma.rn.f32 %f13, %f36, 0f3E800000, %f13;

    mul.f32 %f14, %f14, 0f3F7EB852;
    fma.rn.f32 %f14, %f28, 0f3E19999A, %f14;

    mul.f32 %f15, %f15, 0f3F7C28F6;
    fma.rn.f32 %f15, %f31, 0f3E3851EC, %f15;
    fma.rn.f32 %f15, %f37, 0f3E800000, %f15;

    // Integrate position: p += v * 0.016
    fma.rn.f32 %f10, %f13, 0f3C83126F, %f10;
    fma.rn.f32 %f11, %f14, 0f3C83126F, %f11;
    fma.rn.f32 %f12, %f15, 0f3C83126F, %f12;

    // Soft ground splash bounce at wet asphalt y = 0.04
    setp.lt.f32 %p3, %f11, 0f3D23D70A;
    @!%p3 bra NEXT_SUBSTEP;
    mov.f32 %f11, 0f3D23D70A;
    abs.f32 %f14, %f14;
    mul.f32 %f14, %f14, 0f3E99999A;

NEXT_SUBSTEP:
    add.u32 %r15, %r15, 1;
    bra CURL_LOOP;

WRITE_PARTICLE:
    mul.wide.u32 %rd2, %r3, 32;
    add.u64 %rd3, %rd0, %rd2;
    st.global.v4.f32 [%rd3],    {%f10, %f11, %f12, %f8};
    st.global.v4.f32 [%rd3+16], {%f13, %f14, %f15, %f16};

    // Thread 0 writes hardware execution sentinel + substeps verification
    setp.eq.u32 %p4, %r3, 0;
    @!%p4 bra DONE_PART;
    mov.u32 %r16, 0xC0DAC0DE;
    st.global.u32 [%rd1],    %r16;
    st.global.u32 [%rd1+4],  %r4;
    st.global.u32 [%rd1+8],  %r15;
    st.global.f32 [%rd1+12], %f10;

DONE_PART:
    ret;
}
)PTX";

// ============================================================================
// PTX 2A: Exact Startup Checksum Kernel for Tensor Core `mma.sync.m16n8k16`
// ============================================================================
static const char* kTensorChecksumPtx = R"PTX(
.version 7.0
.target sm_80
.address_size 64

.visible .entry boo_tensor_verify_256(
    .param .u64 p_out,
    .param .u32 p_a_packed,
    .param .u32 p_b_packed
)
{
    .reg .b32 %r<8>;
    .reg .b64 %rd<4>;
    .reg .f32 %f<4>;

    ld.param.u64 %rd0, [p_out];
    ld.param.u32 %r0,  [p_a_packed];
    ld.param.u32 %r4,  [p_b_packed];
    mov.b32 %r1, %r0;
    mov.b32 %r2, %r0;
    mov.b32 %r3, %r0;
    mov.b32 %r5, %r4;
    mov.f32 %f0, 0f00000000;
    mov.f32 %f1, 0f00000000;
    mov.f32 %f2, 0f00000000;
    mov.f32 %f3, 0f00000000;

    // 16 x (16 * 1.0 * 1.0) = 256.0f exact across all 32 warp lanes
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r0, %r1, %r2, %r3}, {%r4, %r5}, {%f0, %f1, %f2, %f3};

    mov.u32 %r6, %tid.x;
    mul.wide.u32 %rd1, %r6, 16;
    add.u64 %rd2, %rd0, %rd1;
    st.global.v4.f32 [%rd2], {%f0, %f1, %f2, %f3};
    ret;
}
)PTX";

// ============================================================================
// PTX 2B: Live 320x180 64-Layer Hardware Tensor Core SIREN Caustic/Spectral MLP
//   - 3,600 warps (4x4 pixel tile per warp = 16 pixels/warp -> 320x180 = 57,600 texels)
//   - Each residual block executes 8 `mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32`
//     (16->32 expansion + SIREN sin activation + 32->16 residual projection)
//   - 64 layers * 8 mma.sync = 512 mma.sync/warp = 7.5497 GFLOP/frame
// ============================================================================
static const char* kLiveTensorCausticPtx = R"PTX(
.version 7.0
.target sm_80
.address_size 64

.visible .entry boo_live_tensor_caustics(
    .param .u64 p_rgba16f_out,
    .param .u64 p_weights,
    .param .u64 p_verify,
    .param .u32 p_layers,
    .param .f32 p_time,
    .param .f32 p_reveal,
    .param .f32 p_edition_shift
)
{
    .reg .pred %p<4>;
    .reg .b16  %h<16>;
    .reg .b32  %r<44>;
    .reg .b64  %rd<12>;
    .reg .f32  %f<40>;

    mov.u32 %r0, %ctaid.x;
    mov.u32 %r1, %ntid.x;
    mov.u32 %r2, %tid.x;
    mad.lo.u32 %r3, %r0, %r1, %r2; // global thread id
    shr.u32 %r4, %r3, 5;           // warpId = globalTid >> 5
    setp.ge.u32 %p0, %r4, 3600;
    @%p0 bra DONE_TENSOR;

    and.b32 %r5, %r2, 31;          // laneId in [0..31]
    shr.u32 %r6, %r5, 2;           // groupID in [0..7]
    and.b32 %r7, %r5, 3;           // subCol in [0..3]

    // 80x45 tiles of 4x4 pixels = 320x180
    div.u32 %r8, %r4, 80;          // tileY in [0..44]
    rem.u32 %r9, %r4, 80;          // tileX in [0..79]

    and.b32 %r10, %r6, 3;          // local dx in [0..3]
    shr.u32 %r11, %r6, 2;          // local dy0 in [0..1]
    shl.b32 %r12, %r9, 2;
    add.u32 %r12, %r12, %r10;      // px0 = tileX * 4 + dx
    shl.b32 %r13, %r8, 2;
    add.u32 %r13, %r13, %r11;      // py0 = tileY * 4 + dy0
    add.u32 %r14, %r13, 2;         // py1 = py0 + 2 (pixel groupID + 8!)

    ld.param.u64 %rd0, [p_rgba16f_out];
    ld.param.u64 %rd1, [p_weights];
    ld.param.u64 %rd2, [p_verify];
    ld.param.u32 %r15, [p_layers];
    ld.param.f32 %f20, [p_time];
    ld.param.f32 %f21, [p_reveal];
    ld.param.f32 %f22, [p_edition_shift];

    // Normalized [-1, +1] coordinates for pixel p0 (px0, py0) and pixel p1 (px0, py1)
    cvt.rn.f32.u32 %f23, %r12;
    fma.rn.f32 %f23, %f23, 0f3BCCCCCD, 0fBF800000; // u0 = px0 * (2/320=0.00625) - 1.0
    cvt.rn.f32.u32 %f24, %r13;
    fma.rn.f32 %f24, %f24, 0f3C360B61, 0fBF800000; // v0 = py0 * (2/180=0.0111111) - 1.0
    cvt.rn.f32.u32 %f25, %r14;
    fma.rn.f32 %f25, %f25, 0f3C360B61, 0fBF800000; // v1 = py1 * (2/180=0.0111111) - 1.0

    // Frequency harmonic per subCol (1, 2, 3, 4) * pi
    add.u32 %r16, %r7, 1;
    cvt.rn.f32.u32 %f26, %r16;
    mul.f32 %f26, %f26, 0f402DF854; // freq = (subCol + 1) * 2.71828f
    fma.rn.f32 %f27, %f20, 0f3F266666, %f22; // temporal phase = time * 0.65 + editionShift

    // Encode 16 Fourier input channels across the 4 subCol threads:
    // Channels 0..7 (A left half: %r20, %r21) & Channels 8..15 (A right half: %r22, %r23)
    fma.rn.f32 %f28, %f23, %f26, %f27;
    fma.rn.f32 %f29, %f24, %f26, %f21;
    fma.rn.f32 %f30, %f25, %f26, %f21;

    sin.approx.f32 %f0, %f28;
    cos.approx.f32 %f1, %f29;
    sin.approx.f32 %f2, %f28;
    cos.approx.f32 %f3, %f30;

    add.f32 %f31, %f28, %f29;
    sub.f32 %f32, %f28, %f29;
    add.f32 %f33, %f28, %f30;
    sub.f32 %f34, %f28, %f30;

    sin.approx.f32 %f4, %f31;
    cos.approx.f32 %f5, %f32;
    sin.approx.f32 %f6, %f33;
    cos.approx.f32 %f7, %f34;

    // Pack initial hidden state h[0..15] into FP16 matrix A registers {%r20, %r21, %r22, %r23}
    cvt.rn.f16.f32 %h0, %f0;
    cvt.rn.f16.f32 %h1, %f1;
    mov.b32 %r20, {%h0, %h1};
    cvt.rn.f16.f32 %h2, %f2;
    cvt.rn.f16.f32 %h3, %f3;
    mov.b32 %r21, {%h2, %h3};
    cvt.rn.f16.f32 %h4, %f4;
    cvt.rn.f16.f32 %h5, %f5;
    mov.b32 %r22, {%h4, %h5};
    cvt.rn.f16.f32 %h6, %f6;
    cvt.rn.f16.f32 %h7, %f7;
    mov.b32 %r23, {%h6, %h7};

    // Per-lane weight pointer base: each layer has 8 B-matrices * 32 lanes * 8 bytes = 2048 bytes/layer
    mul.wide.u32 %rd3, %r5, 8;
    add.u64 %rd4, %rd1, %rd3;

    mov.u32 %r17, 0;
LAYER_LOOP:
    setp.ge.u32 %p1, %r17, %r15;
    @%p1 bra WRITE_TEXELS;

    // --- Expansion 16 -> 32 via 4 Tensor Core mma.sync ops ---
    ld.global.ca.v2.u32 {%r24, %r25}, [%rd4];
    ld.global.ca.v2.u32 {%r26, %r27}, [%rd4+256];
    ld.global.ca.v2.u32 {%r28, %r29}, [%rd4+512];
    ld.global.ca.v2.u32 {%r30, %r31}, [%rd4+768];

    mov.f32 %f8,  0f00000000; mov.f32 %f9,  0f00000000; mov.f32 %f10, 0f00000000; mov.f32 %f11, 0f00000000;
    mov.f32 %f12, 0f00000000; mov.f32 %f13, 0f00000000; mov.f32 %f14, 0f00000000; mov.f32 %f15, 0f00000000;
    mov.f32 %f16, 0f00000000; mov.f32 %f17, 0f00000000; mov.f32 %f18, 0f00000000; mov.f32 %f19, 0f00000000;
    mov.f32 %f35, 0f00000000; mov.f32 %f36, 0f00000000; mov.f32 %f37, 0f00000000; mov.f32 %f38, 0f00000000;

    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f8,  %f9,  %f10, %f11}, {%r20, %r21, %r22, %r23}, {%r24, %r25}, {%f8,  %f9,  %f10, %f11};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f12, %f13, %f14, %f15}, {%r20, %r21, %r22, %r23}, {%r26, %r27}, {%f12, %f13, %f14, %f15};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f16, %f17, %f18, %f19}, {%r20, %r21, %r22, %r23}, {%r28, %r29}, {%f16, %f17, %f18, %f19};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f35, %f36, %f37, %f38}, {%r20, %r21, %r22, %r23}, {%r30, %r31}, {%f35, %f36, %f37, %f38};

    // SIREN periodic sine activation on all 32 expanded channels
    sin.approx.f32 %f8,  %f8;  sin.approx.f32 %f9,  %f9;  sin.approx.f32 %f10, %f10; sin.approx.f32 %f11, %f11;
    sin.approx.f32 %f12, %f12; sin.approx.f32 %f13, %f13; sin.approx.f32 %f14, %f14; sin.approx.f32 %f15, %f15;
    sin.approx.f32 %f16, %f16; sin.approx.f32 %f17, %f17; sin.approx.f32 %f18, %f18; sin.approx.f32 %f19, %f19;
    sin.approx.f32 %f35, %f35; sin.approx.f32 %f36, %f36; sin.approx.f32 %f37, %f37; sin.approx.f32 %f38, %f38;

    // Pack first 16 activated channels into {%r32..%r35} and second 16 into {%r36..%r39}
    cvt.rn.f16.f32 %h0, %f8;  cvt.rn.f16.f32 %h1, %f9;  mov.b32 %r32, {%h0, %h1};
    cvt.rn.f16.f32 %h2, %f10; cvt.rn.f16.f32 %h3, %f11; mov.b32 %r33, {%h2, %h3};
    cvt.rn.f16.f32 %h4, %f12; cvt.rn.f16.f32 %h5, %f13; mov.b32 %r34, {%h4, %h5};
    cvt.rn.f16.f32 %h6, %f14; cvt.rn.f16.f32 %h7, %f15; mov.b32 %r35, {%h6, %h7};

    cvt.rn.f16.f32 %h8,  %f16; cvt.rn.f16.f32 %h9,  %f17; mov.b32 %r36, {%h8,  %h9};
    cvt.rn.f16.f32 %h10, %f18; cvt.rn.f16.f32 %h11, %f19; mov.b32 %r37, {%h10, %h11};
    cvt.rn.f16.f32 %h12, %f35; cvt.rn.f16.f32 %h13, %f36; mov.b32 %r38, {%h12, %h13};
    cvt.rn.f16.f32 %h14, %f37; cvt.rn.f16.f32 %h15, %f38; mov.b32 %r39, {%h14, %h15};

    // --- Projection 32 -> 16 residual accumulate onto {%f0..%f7} via 4 Tensor Core mma.sync ops ---
    ld.global.ca.v2.u32 {%r24, %r25}, [%rd4+1024];
    ld.global.ca.v2.u32 {%r26, %r27}, [%rd4+1280];
    ld.global.ca.v2.u32 {%r28, %r29}, [%rd4+1536];
    ld.global.ca.v2.u32 {%r30, %r31}, [%rd4+1792];

    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r32, %r33, %r34, %r35}, {%r24, %r25}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f0, %f1, %f2, %f3}, {%r36, %r37, %r38, %r39}, {%r26, %r27}, {%f0, %f1, %f2, %f3};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f4, %f5, %f6, %f7}, {%r32, %r33, %r34, %r35}, {%r28, %r29}, {%f4, %f5, %f6, %f7};
    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%f4, %f5, %f6, %f7}, {%r36, %r37, %r38, %r39}, {%r30, %r31}, {%f4, %f5, %f6, %f7};

    // Repack updated residual state into FP16 matrix A registers for next layer
    cvt.rn.f16.f32 %h0, %f0; cvt.rn.f16.f32 %h1, %f1; mov.b32 %r20, {%h0, %h1};
    cvt.rn.f16.f32 %h2, %f2; cvt.rn.f16.f32 %h3, %f3; mov.b32 %r21, {%h2, %h3};
    cvt.rn.f16.f32 %h4, %f4; cvt.rn.f16.f32 %h5, %f5; mov.b32 %r22, {%h4, %h5};
    cvt.rn.f16.f32 %h6, %f6; cvt.rn.f16.f32 %h7, %f7; mov.b32 %r23, {%h6, %h7};

    add.u64 %rd4, %rd4, 2048;
    add.u32 %r17, %r17, 1;
    bra LAYER_LOOP;

WRITE_TEXELS:
    // Lanes with subCol == 0 hold {ch0, ch1, ch8, ch9} for pixel p0 (px0, py0) and pixel p1 (px0, py1)
    setp.eq.u32 %p2, %r7, 0;
    @!%p2 bra DONE_TENSOR;

    // Map raw SIREN outputs into vivid caustic filaments & spectral dispersion [0, 1]
    sin.approx.f32 %f8,  %f0; fma.rn.f32 %f8,  %f8,  0f3F000000, 0f3F000000;
    sin.approx.f32 %f9,  %f1; fma.rn.f32 %f9,  %f9,  0f3F000000, 0f3F000000;
    sin.approx.f32 %f10, %f4; fma.rn.f32 %f10, %f10, 0f3F000000, 0f3F000000;
    sin.approx.f32 %f11, %f5; fma.rn.f32 %f11, %f11, 0f3F000000, 0f3F000000;
    // Sharpen caustic ridge in alpha channel (a = a*a)
    mul.f32 %f11, %f11, %f11;

    cvt.rn.f16.f32 %h0, %f8;  cvt.rn.f16.f32 %h1, %f9;  mov.b32 %r40, {%h0, %h1};
    cvt.rn.f16.f32 %h2, %f10; cvt.rn.f16.f32 %h3, %f11; mov.b32 %r41, {%h2, %h3};

    mad.lo.u32 %r18, %r13, 320, %r12; // idx0 = py0 * 320 + px0
    mul.wide.u32 %rd5, %r18, 8;
    add.u64 %rd6, %rd0, %rd5;
    st.global.v2.u32 [%rd6], {%r40, %r41};

    // Pixel p1 = (px0, py1)
    sin.approx.f32 %f12, %f2; fma.rn.f32 %f12, %f12, 0f3F000000, 0f3F000000;
    sin.approx.f32 %f13, %f3; fma.rn.f32 %f13, %f13, 0f3F000000, 0f3F000000;
    sin.approx.f32 %f14, %f6; fma.rn.f32 %f14, %f14, 0f3F000000, 0f3F000000;
    sin.approx.f32 %f15, %f7; fma.rn.f32 %f15, %f15, 0f3F000000, 0f3F000000;
    mul.f32 %f15, %f15, %f15;

    cvt.rn.f16.f32 %h4, %f12; cvt.rn.f16.f32 %h5, %f13; mov.b32 %r42, {%h4, %h5};
    cvt.rn.f16.f32 %h6, %f14; cvt.rn.f16.f32 %h7, %f15; mov.b32 %r43, {%h6, %h7};

    mad.lo.u32 %r19, %r14, 320, %r12; // idx1 = py1 * 320 + px0
    mul.wide.u32 %rd7, %r19, 8;
    add.u64 %rd8, %rd0, %rd7;
    st.global.v2.u32 [%rd8], {%r42, %r43};

    // Thread 0 writes Tensor Core live execution verification sentinel
    setp.eq.u32 %p3, %r3, 0;
    @!%p3 bra DONE_TENSOR;
    mov.u32 %r40, 0x7E450A80;
    st.global.u32 [%rd2],    %r40;
    st.global.u32 [%rd2+4],  %r17;
    st.global.f32 [%rd2+8],  %f8;
    st.global.f32 [%rd2+12], %f11;

DONE_TENSOR:
    ret;
}
)PTX";

// Builds deterministic, orthogonal-like subtle residual SIREN weights so 64 layers produce
// rich multi-scale caustic interference ridges without exploding or vanishing gradients.
std::vector<uint32_t> buildDeterministicTensorWeights() {
    // 64 layers * 8 matrices/layer * 32 lanes * 2 uint32_t/lane = 32,768 uint32_t (128 KB)
    constexpr size_t kTotalWords = 64u * 8u * 32u * 2u;
    std::vector<uint32_t> words(kTotalWords, 0u);
    uint32_t state = 0x266203A5u;
    auto nextFloatSigned = [&]() noexcept -> float {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        return (static_cast<float>(state & 0xFFFFu) / 32768.0f) - 1.0f;
    };

    for (size_t layer = 0; layer < 64u; ++layer) {
        for (size_t mat = 0; mat < 8u; ++mat) {
            // Expansion matrices (0..3) use larger SIREN frequency weights (~0.42);
            // Residual projection matrices (4..7) use gentle residual step (~0.048) so 64 layers evolve smoothly!
            const float scale = (mat < 4u)
                                    ? ((layer == 0u) ? 0.85f : 0.44f)
                                    : 0.048f;
            for (size_t lane = 0; lane < 32u; ++lane) {
                const size_t base = ((layer * 8u + mat) * 32u + lane) * 2u;
                const float w0 = nextFloatSigned() * scale;
                const float w1 = nextFloatSigned() * scale;
                const float w2 = nextFloatSigned() * scale;
                const float w3 = nextFloatSigned() * scale;
                words[base + 0u] = packHalf2Bits(w0, w1);
                words[base + 1u] = packHalf2Bits(w2, w3);
            }
        }
    }
    return words;
}

// ============================================================================
// Live Hardware H.264 Encoder -> Decoder Round-Trip Pipeline (Media Foundation)
// ============================================================================
// ProcessOutput can return events and allocated samples even when reporting a
// stream change/failure. Release them on every exit, without releasing the caller's sample.
struct ScopedMftOutput {
    MFT_OUTPUT_DATA_BUFFER& output;
    IMFSample* callerSample;
    ~ScopedMftOutput() {
        if (output.pSample && output.pSample != callerSample) output.pSample->Release();
        if (output.pEvents) output.pEvents->Release();
    }
};

bool allocateMftOutputSample(DWORD byteCount, ComPtr<IMFSample>& sample) {
    ComPtr<IMFMediaBuffer> buffer;
    return SUCCEEDED(MFCreateSample(&sample)) && sample &&
           SUCCEEDED(MFCreateMemoryBuffer(byteCount, &buffer)) && buffer &&
           SUCCEEDED(sample->AddBuffer(buffer.Get()));
}

class LiveCodecSession {
public:
    LiveCodecSession() = default;
    ~LiveCodecSession() { shutdown(); }

    bool initialize(int32_t luidLow, int32_t luidHigh, std::string& outBackendName) {
        shutdown();

        std::string nvencApiTag;
        if (HMODULE hNv = LoadLibraryExW(L"nvEncodeAPI64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            using GetMaxVerFn = int(WINAPI*)(uint32_t*);
            if (auto pVer = reinterpret_cast<GetMaxVerFn>(GetProcAddress(hNv, "NvEncodeAPIGetMaxSupportedVersion"))) {
                uint32_t v = 0;
                if (pVer(&v) == 0) {
                    nvencApiTag = " [NVENC " + std::to_string(v >> 4u) + "." + std::to_string(v & 0xFu) + "]";
                }
            }
            FreeLibrary(hNv);
        }

        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        didCoInit_ = (hrCo == S_OK || hrCo == S_FALSE);
        comThread_ = GetCurrentThreadId();
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
            if (didCoInit_) { CoUninitialize(); didCoInit_ = false; }
            return false;
        }
        mfStarted_ = true;

        // 1. Activate adapter-matched Hardware H.264 Encoder MFT
        MFT_REGISTER_TYPE_INFO encOutInfo{MFMediaType_Video, MFVideoFormat_H264};
        ComPtr<IMFAttributes> enumAttrs;
        MFCreateAttributes(&enumAttrs, 1);
        LUID adapterLuid{static_cast<DWORD>(luidLow), luidHigh};
        if (enumAttrs) {
            enumAttrs->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&adapterLuid), sizeof(LUID));
        }

        IMFActivate** ppEncAct = nullptr;
        UINT32 encCount = 0;
        const UINT32 encFlags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
        HRESULT hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, encFlags, nullptr, &encOutInfo, enumAttrs.Get(), &ppEncAct, &encCount);
        enumAttrs.Reset();
        if (FAILED(hr) || encCount == 0 || !ppEncAct) {
            shutdown();
            return false;
        }

        std::string friendly = "Hardware H.264 Encoder MFT";
        wchar_t* wName = nullptr;
        UINT32 wLen = 0;
        if (SUCCEEDED(ppEncAct[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wName, &wLen)) && wName) {
            friendly = wideToUtf8(wName);
            CoTaskMemFree(wName);
        }
        outBackendName = friendly + nvencApiTag + " -> H.264 Decode";

        hr = ppEncAct[0]->ActivateObject(IID_PPV_ARGS(&encoder_));
        for (UINT32 i = 0; i < encCount; ++i) ppEncAct[i]->Release();
        CoTaskMemFree(ppEncAct);
        if (FAILED(hr) || !encoder_) {
            shutdown();
            return false;
        }

        {
            ComPtr<IMFAttributes> attrs;
            if (SUCCEEDED(encoder_->GetAttributes(&attrs)) && attrs) {
                attrs->GetUINT32(MF_TRANSFORM_ASYNC, &encIsAsync_);
                if (encIsAsync_) {
                    attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                }
            }
        }
        {
            ComPtr<ICodecAPI> codecApi;
            if (SUCCEEDED(encoder_.As(&codecApi)) && codecApi) {
                VARIANT vTrue{};
                vTrue.vt = VT_BOOL;
                vTrue.boolVal = VARIANT_TRUE;
                codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &vTrue);

                VARIANT vZeroB{};
                vZeroB.vt = VT_UI4;
                vZeroB.ulVal = 0;
                codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &vZeroB);

                VARIANT vGop{};
                vGop.vt = VT_UI4;
                vGop.ulVal = 30;
                codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &vGop);
            }
        }

        constexpr uint32_t kW = LiveAcceleratorOrchestra::kCodecWidth;
        constexpr uint32_t kH = LiveAcceleratorOrchestra::kCodecHeight;
        constexpr uint32_t kFps = 60u;
        constexpr uint32_t kBitrate = LiveAcceleratorOrchestra::kCodecTargetBitrate;

        ComPtr<IMFMediaType> encOutType;
        MFCreateMediaType(&encOutType);
        encOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        encOutType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        encOutType->SetUINT32(MF_MT_AVG_BITRATE, kBitrate);
        encOutType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        encOutType->SetUINT32(MF_MT_MPEG2_PROFILE, 66u); // Baseline/Constrained low-latency zero-reorder profile
        MFSetAttributeSize(encOutType.Get(), MF_MT_FRAME_SIZE, kW, kH);
        MFSetAttributeRatio(encOutType.Get(), MF_MT_FRAME_RATE, kFps, 1);
        MFSetAttributeRatio(encOutType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (FAILED(encoder_->SetOutputType(0, encOutType.Get(), 0))) {
            // Retry without explicit profile constraint if driver prefers Main/High
            encOutType->DeleteItem(MF_MT_MPEG2_PROFILE);
            if (FAILED(encoder_->SetOutputType(0, encOutType.Get(), 0))) {
                encOutType.Reset();
                shutdown();
                return false;
            }
        }
        encOutType.Reset();

        ComPtr<IMFMediaType> chosenEncIn;
        for (DWORD idx = 0; idx < 32; ++idx) {
            ComPtr<IMFMediaType> cand;
            if (FAILED(encoder_->GetInputAvailableType(0, idx, &cand))) break;
            GUID sub{};
            if (SUCCEEDED(cand->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
                MFSetAttributeSize(cand.Get(), MF_MT_FRAME_SIZE, kW, kH);
                MFSetAttributeRatio(cand.Get(), MF_MT_FRAME_RATE, kFps, 1);
                cand->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                if (SUCCEEDED(encoder_->SetInputType(0, cand.Get(), 0))) {
                    chosenEncIn = cand;
                    break;
                }
            }
        }
        if (!chosenEncIn) {
            MFCreateMediaType(&chosenEncIn);
            chosenEncIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            chosenEncIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            chosenEncIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(chosenEncIn.Get(), MF_MT_FRAME_SIZE, kW, kH);
            MFSetAttributeRatio(chosenEncIn.Get(), MF_MT_FRAME_RATE, kFps, 1);
            if (FAILED(encoder_->SetInputType(0, chosenEncIn.Get(), 0))) {
                chosenEncIn.Reset();
                shutdown();
                return false;
            }
        }
        chosenEncIn.Reset();

        MFT_OUTPUT_STREAM_INFO encStreamInfo{};
        encoder_->GetOutputStreamInfo(0, &encStreamInfo);
        encAllocatesOut_ =
            (encStreamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        encOutCbSize_ = std::max<DWORD>(262144u, encStreamInfo.cbSize);

        if (encIsAsync_) {
            encoder_.As(&encEvGen_);
            if (!encEvGen_) {
                shutdown();
                return false;
            }
        }

        // 2. Activate inbox synchronous H.264 Video Decoder MFT for real bitstream reconstruction
        MFT_REGISTER_TYPE_INFO decInInfo{MFMediaType_Video, MFVideoFormat_H264};
        MFT_REGISTER_TYPE_INFO decOutInfo{MFMediaType_Video, MFVideoFormat_NV12};
        IMFActivate** ppDecAct = nullptr;
        UINT32 decCount = 0;
        hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_DECODER,
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
            &decInInfo,
            &decOutInfo,
            &ppDecAct,
            &decCount);
        if (FAILED(hr) || decCount == 0 || !ppDecAct) {
            shutdown();
            return false;
        }
        hr = ppDecAct[0]->ActivateObject(IID_PPV_ARGS(&decoder_));
        for (UINT32 i = 0; i < decCount; ++i) ppDecAct[i]->Release();
        CoTaskMemFree(ppDecAct);
        if (FAILED(hr) || !decoder_) {
            shutdown();
            return false;
        }

        // Enable low-latency immediate frame output on the H.264 decoder
        {
            ComPtr<IMFAttributes> decAttrs;
            if (SUCCEEDED(decoder_->GetAttributes(&decAttrs)) && decAttrs) {
                decAttrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
            }
            ComPtr<ICodecAPI> decCodecApi;
            if (SUCCEEDED(decoder_.As(&decCodecApi)) && decCodecApi) {
                VARIANT vTrue{};
                vTrue.vt = VT_BOOL;
                vTrue.boolVal = VARIANT_TRUE;
                decCodecApi->SetValue(&CODECAPI_AVLowLatencyMode, &vTrue);
            }
        }

        ComPtr<IMFMediaType> decInType;
        MFCreateMediaType(&decInType);
        decInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        decInType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        decInType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(decInType.Get(), MF_MT_FRAME_SIZE, kW, kH);
        MFSetAttributeRatio(decInType.Get(), MF_MT_FRAME_RATE, kFps, 1);
        MFSetAttributeRatio(decInType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

        // Copy SPS/PPS sequence header if hardware encoder already populated it on current output type
        ComPtr<IMFMediaType> curEncOut;
        if (SUCCEEDED(encoder_->GetOutputCurrentType(0, &curEncOut)) && curEncOut) {
            UINT8 seqHdr[512]{};
            UINT32 seqLen = 0;
            if (SUCCEEDED(curEncOut->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHdr, sizeof(seqHdr), &seqLen)) && seqLen > 0) {
                decInType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHdr, seqLen);
                cachedSeqHeader_.assign(seqHdr, seqHdr + seqLen);
            }
        }
        curEncOut.Reset();

        if (FAILED(decoder_->SetInputType(0, decInType.Get(), 0))) {
            decInType.Reset();
            shutdown();
            return false;
        }
        decInType.Reset();

        if (!configureDecoderNv12Output()) {
            shutdown();
            return false;
        }

        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        ready_ = true;
        return true;
    }

    void shutdown() noexcept {
        ready_ = false;
        decReusableSample_.Reset();
        decReusableBuffer_.Reset();
        encEvGen_.Reset();
        if (decoder_) {
            decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            ComPtr<IMFShutdown> sh;
            if (SUCCEEDED(decoder_.As(&sh)) && sh) sh->Shutdown();
            sh.Reset();
            decoder_.Reset();
        }
        if (encoder_) {
            encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            ComPtr<IMFShutdown> sh;
            if (SUCCEEDED(encoder_.As(&sh)) && sh) sh->Shutdown();
            sh.Reset();
            encoder_.Reset();
        }
        if (mfStarted_) {
            MFShutdown();
            mfStarted_ = false;
        }
        if (didCoInit_) {
            // Only the thread that initialised COM may undo it; start-up runs this on a loader thread.
            if (comThread_ == GetCurrentThreadId()) CoUninitialize();
            didCoInit_ = false;
        }
    }

    bool isReady() const noexcept { return ready_; }

    bool encodeAndDecodeOneFrame(
        const uint8_t* nv12In,
        size_t nv12Len,
        std::vector<uint8_t>& outRgba8,
        LivePillarTelemetry& outTel) {
        outRgba8.clear();
        constexpr uint32_t kW = LiveAcceleratorOrchestra::kCodecWidth;
        constexpr uint32_t kH = LiveAcceleratorOrchestra::kCodecHeight;
        constexpr size_t kExpectedNv12 = (static_cast<size_t>(kW) * kH * 3u) / 2u;
        if (!ready_ || !nv12In || nv12Len < kExpectedNv12) return false;

        const auto t0 = std::chrono::steady_clock::now();

        auto feedOneEncoderInput = [&]() -> bool {
            // The asynchronous MFT may retain an input sample beyond four feeds.
            // Give each accepted sample its own buffer, released by COM when consumed.
            ComPtr<IMFMediaBuffer> inputBuffer;
            if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(kExpectedNv12), &inputBuffer))) return false;
            IMFMediaBuffer* ringBuf = inputBuffer.Get();
            BYTE* dst = nullptr;
            if (FAILED(ringBuf->Lock(&dst, nullptr, nullptr)) || !dst) return false;
            std::memcpy(dst, nv12In, kExpectedNv12);
            ringBuf->Unlock();
            ringBuf->SetCurrentLength(static_cast<DWORD>(kExpectedNv12));

            ComPtr<IMFSample> samp;
            if (FAILED(MFCreateSample(&samp)) || !samp) return false;
            samp->AddBuffer(ringBuf);
            const LONGLONG dur100ns = 10000000LL / 60LL;
            samp->SetSampleTime(static_cast<LONGLONG>(frameIndex_) * dur100ns);
            samp->SetSampleDuration(dur100ns);
            ++frameIndex_;
            return SUCCEEDED(encoder_->ProcessInput(0, samp.Get(), 0));
        };

        std::vector<uint8_t> bitstream;
        bitstream.reserve(32768);

        auto harvestEncoderSample = [&](IMFSample* outSamp) -> bool {
            if (!outSamp) return false;
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(outSamp->ConvertToContiguousBuffer(&buf)) || !buf) return false;
            BYTE* p = nullptr;
            DWORD len = 0;
            if (FAILED(buf->Lock(&p, nullptr, &len))) return false;
            if (!p || len <= 4u) { buf->Unlock(); return false; }

            const bool hasStartCode =
                (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) ||
                (p[0] == 0 && p[1] == 0 && p[2] == 1);

            if (!sentSeqHeaderToDec_ && !cachedSeqHeader_.empty()) {
                bitstream.insert(bitstream.end(), cachedSeqHeader_.begin(), cachedSeqHeader_.end());
                sentSeqHeaderToDec_ = true;
            }
            bitstream.insert(bitstream.end(), p, p + len);
            buf->Unlock();
            return hasStartCode;
        };

        bool annexBVerified = false;
        if (encIsAsync_) {
            bool fedCurrent = false;
            uint32_t extraWarmupFeeds = 0;
            const auto deadline = t0 + std::chrono::milliseconds(80);

            if (pendingNeedInputCount_ > 0) {
                --pendingNeedInputCount_;
                fedCurrent = feedOneEncoderInput();
            }

            while (std::chrono::steady_clock::now() < deadline) {
                ComPtr<IMFMediaEvent> ev;
                const HRESULT hrEv = encEvGen_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
                if (hrEv == MF_E_NO_EVENTS_AVAILABLE) {
                    if (!bitstream.empty()) break;
                    Sleep(0);
                    continue;
                }
                if (FAILED(hrEv) || !ev) break;

                MediaEventType met = MEUnknown;
                ev->GetType(&met);
                if (met == METransformNeedInput) {
                    if (!fedCurrent) {
                        fedCurrent = feedOneEncoderInput();
                    } else if (bitstream.empty() && decodedTotalFrames_ == 0 && extraWarmupFeeds < 3u) {
                        // Fill initial hardware encoder lookahead on frame 0 without blocking
                        ++extraWarmupFeeds;
                        feedOneEncoderInput();
                    } else {
                        ++pendingNeedInputCount_;
                    }
                } else if (met == METransformHaveOutput) {
                    MFT_OUTPUT_DATA_BUFFER odb{};
                    ComPtr<IMFSample> callerSamp;
                    if (!encAllocatesOut_) {
                        if (!allocateMftOutputSample(encOutCbSize_, callerSamp)) return false;
                        odb.pSample = callerSamp.Get();
                    }
                    const ScopedMftOutput releaseOutput{odb, odb.pSample};
                    DWORD status = 0;
                    const HRESULT hrOut = encoder_->ProcessOutput(0, 1, &odb, &status);
                    if (hrOut == MF_E_TRANSFORM_STREAM_CHANGE) {
                        ComPtr<IMFMediaType> newOut;
                        if (SUCCEEDED(encoder_->GetOutputAvailableType(0, 0, &newOut)) && newOut) {
                            encoder_->SetOutputType(0, newOut.Get(), 0);
                            UINT8 seqHdr[512]{};
                            UINT32 seqLen = 0;
                            if (SUCCEEDED(newOut->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHdr, sizeof(seqHdr), &seqLen)) && seqLen > 0) {
                                cachedSeqHeader_.assign(seqHdr, seqHdr + seqLen);
                            }
                        }
                    } else if (SUCCEEDED(hrOut)) {
                        if (harvestEncoderSample(odb.pSample)) {
                            annexBVerified = true;
                        }
                        if (fedCurrent && !bitstream.empty()) {
                            break;
                        }
                    }
                }
            }
        } else {
            feedOneEncoderInput();
            for (int tries = 0; tries < 4 && bitstream.empty(); ++tries) {
                MFT_OUTPUT_DATA_BUFFER odb{};
                ComPtr<IMFSample> callerSamp;
                if (!encAllocatesOut_) {
                    if (!allocateMftOutputSample(encOutCbSize_, callerSamp)) return false;
                    odb.pSample = callerSamp.Get();
                }
                const ScopedMftOutput releaseOutput{odb, odb.pSample};
                DWORD status = 0;
                const HRESULT hrOut = encoder_->ProcessOutput(0, 1, &odb, &status);
                if (hrOut == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                    if (decodedTotalFrames_ == 0) feedOneEncoderInput();
                    else break;
                } else if (SUCCEEDED(hrOut)) {
                    if (harvestEncoderSample(odb.pSample)) annexBVerified = true;
                } else {
                    break;
                }
            }
        }

        if (bitstream.empty() || !annexBVerified) {
            return false;
        }

        // Feed compressed H.264 Annex-B bitstream into inbox H.264 Decoder MFT
        auto feedDecoderPacket = [&](const std::vector<uint8_t>& pkt) -> bool {
            ComPtr<IMFMediaBuffer> cbuf;
            if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(pkt.size()), &cbuf)) || !cbuf) return false;
            BYTE* dst = nullptr;
            if (FAILED(cbuf->Lock(&dst, nullptr, nullptr)) || !dst) return false;
            std::memcpy(dst, pkt.data(), pkt.size());
            cbuf->Unlock();
            cbuf->SetCurrentLength(static_cast<DWORD>(pkt.size()));

            ComPtr<IMFSample> csamp;
            if (FAILED(MFCreateSample(&csamp)) || !csamp) return false;
            csamp->AddBuffer(cbuf.Get());
            const LONGLONG dur100ns = 10000000LL / 60LL;
            csamp->SetSampleTime(static_cast<LONGLONG>(decPacketIndex_++) * dur100ns);
            csamp->SetSampleDuration(dur100ns);
            return SUCCEEDED(decoder_->ProcessInput(0, csamp.Get(), 0));
        };

        if (!feedDecoderPacket(bitstream)) {
            return false;
        }

        bool decodedFrameReady = false;
        double psnrDb = 0.0;

        auto pullDecoderOutput = [&]() -> bool {
            for (int iter = 0; iter < 6; ++iter) {
                MFT_OUTPUT_DATA_BUFFER odb{};
                if (!decAllocatesOut_) {
                    if (!ensureDecoderSampleAllocated()) return false;
                    odb.pSample = decReusableSample_.Get();
                }
                const ScopedMftOutput releaseOutput{odb, odb.pSample};
                DWORD status = 0;
                const HRESULT hrDec = decoder_->ProcessOutput(0, 1, &odb, &status);
                if (hrDec == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                    return decodedFrameReady;
                }
                if (hrDec == MF_E_TRANSFORM_STREAM_CHANGE) {
                    if (!configureDecoderNv12Output()) return false;
                    continue;
                }
                if (FAILED(hrDec)) {
                    return decodedFrameReady;
                }

                if (odb.pSample) {
                    psnrDb = convertDecodedNv12SampleToRgbaAndPsnr(odb.pSample, nv12In, outRgba8);
                    decodedFrameReady = !outRgba8.empty();
                }
            }
            return decodedFrameReady;
        };

        pullDecoderOutput();

        // On frame 0 cold start, some H.264 decoders hold 1 reference picture before emitting output 0;
        // feed one more encoded P-frame so even synchronous single-frame capture gets an immediate decoded image!
        if (!decodedFrameReady && decodedTotalFrames_ == 0) {
            for (int warm = 0; warm < 3 && !decodedFrameReady; ++warm) {
                bitstream.clear();
                if (encIsAsync_) {
                    feedOneEncoderInput();
                    const auto wDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
                    while (std::chrono::steady_clock::now() < wDeadline && bitstream.empty()) {
                        ComPtr<IMFMediaEvent> ev;
                        if (FAILED(encEvGen_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev)) || !ev) {
                            Sleep(0);
                            continue;
                        }
                        MediaEventType met = MEUnknown;
                        ev->GetType(&met);
                        if (met == METransformNeedInput) {
                            ++pendingNeedInputCount_;
                        } else if (met == METransformHaveOutput) {
                            MFT_OUTPUT_DATA_BUFFER odb{};
                            ComPtr<IMFSample> callerSamp;
                            if (!encAllocatesOut_) {
                                if (!allocateMftOutputSample(encOutCbSize_, callerSamp)) return false;
                                odb.pSample = callerSamp.Get();
                            }
                            const ScopedMftOutput releaseOutput{odb, odb.pSample};
                            DWORD st = 0;
                            if (SUCCEEDED(encoder_->ProcessOutput(0, 1, &odb, &st))) {
                                harvestEncoderSample(odb.pSample);
                            }
                        }
                    }
                } else {
                    feedOneEncoderInput();
                    MFT_OUTPUT_DATA_BUFFER odb{};
                    ComPtr<IMFSample> callerSamp;
                    if (!encAllocatesOut_) {
                        if (!allocateMftOutputSample(encOutCbSize_, callerSamp)) return false;
                        odb.pSample = callerSamp.Get();
                    }
                    const ScopedMftOutput releaseOutput{odb, odb.pSample};
                    DWORD st = 0;
                    if (SUCCEEDED(encoder_->ProcessOutput(0, 1, &odb, &st))) {
                        harvestEncoderSample(odb.pSample);
                    }
                }
                if (!bitstream.empty() && feedDecoderPacket(bitstream)) {
                    pullDecoderOutput();
                }
            }
        }

        if (!decodedFrameReady) {
            return false;
        }

        ++decodedTotalFrames_;
        const auto t1 = std::chrono::steady_clock::now();
        const float elapsedMs = static_cast<float>(std::chrono::duration<double, std::milli>(t1 - t0).count());
        const float instantMbps = (static_cast<float>(bitstream.size()) * 8.0f * 60.0f) / 1e6f;
        smoothMbps_ = (decodedTotalFrames_ == 1u) ? instantMbps : (smoothMbps_ * 0.85f + instantMbps * 0.15f);
        smoothMs_   = (decodedTotalFrames_ == 1u) ? elapsedMs   : (smoothMs_ * 0.80f + elapsedMs * 0.20f);

        outTel.status = BackendStatus::Accelerated;
        outTel.activeInScene = true;
        outTel.hardwareVerified = true;
        outTel.workloadItems = kW * kH;
        outTel.workloadSteps = static_cast<uint32_t>(std::max<size_t>(1u, (bitstream.size() + 512u) / 1024u)); // Frame KB
        outTel.executionMs = std::max(0.05f, smoothMs_);
        outTel.throughput = 1000.0f / std::max(0.1f, smoothMs_); // Round-trip FPS capacity
        outTel.qualityOrBitrate = static_cast<float>(psnrDb);
        outTel.auxMetric = smoothMbps_;
        return true;
    }

private:
    bool configureDecoderNv12Output() {
        for (DWORD idx = 0; idx < 32; ++idx) {
            ComPtr<IMFMediaType> cand;
            if (FAILED(decoder_->GetOutputAvailableType(0, idx, &cand))) break;
            GUID sub{};
            if (SUCCEEDED(cand->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
                if (SUCCEEDED(decoder_->SetOutputType(0, cand.Get(), 0))) {
                    UINT32 aw = LiveAcceleratorOrchestra::kCodecWidth;
                    UINT32 ah = LiveAcceleratorOrchestra::kCodecHeight;
                    MFGetAttributeSize(cand.Get(), MF_MT_FRAME_SIZE, &aw, &ah);
                    if (aw > 4096u || ah > 4096u) return false;
                    decAlignedWidth_  = std::max<uint32_t>(LiveAcceleratorOrchestra::kCodecWidth, aw);
                    decAlignedHeight_ = std::max<uint32_t>(LiveAcceleratorOrchestra::kCodecHeight, ah);

                    INT32 strideSigned = static_cast<INT32>(decAlignedWidth_);
                    UINT32 strideRaw = 0;
                    if (SUCCEEDED(cand->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideRaw)) && strideRaw != 0) {
                        strideSigned = static_cast<INT32>(strideRaw);
                    }
                    decDefaultStride_ = (strideSigned > 0) ? static_cast<uint32_t>(strideSigned) : decAlignedWidth_;
                    if (decDefaultStride_ < LiveAcceleratorOrchestra::kCodecWidth || decDefaultStride_ > 16384u) return false;

                    MFT_OUTPUT_STREAM_INFO osi{};
                    decoder_->GetOutputStreamInfo(0, &osi);
                    decAllocatesOut_ =
                        (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                    decOutCbSize_ = std::max<DWORD>(
                        osi.cbSize,
                        static_cast<DWORD>((decDefaultStride_ * decAlignedHeight_ * 3u) / 2u));
                    decReusableSample_.Reset();
                    decReusableBuffer_.Reset();
                    return true;
                }
            }
        }
        return false;
    }

    bool ensureDecoderSampleAllocated() {
        if (decReusableSample_ && decReusableBuffer_) {
            decReusableBuffer_->SetCurrentLength(0);
            return true;
        }
        if (FAILED(MFCreateMemoryBuffer(decOutCbSize_, &decReusableBuffer_)) ||
            FAILED(MFCreateSample(&decReusableSample_))) return false;
        if (decReusableSample_ && decReusableBuffer_) {
            return SUCCEEDED(decReusableSample_->AddBuffer(decReusableBuffer_.Get()));
        }
        return false;
    }

    double convertDecodedNv12SampleToRgbaAndPsnr(
        IMFSample* decSample,
        const uint8_t* refNv12,
        std::vector<uint8_t>& outRgba8) const {
        constexpr uint32_t kW = LiveAcceleratorOrchestra::kCodecWidth;
        constexpr uint32_t kH = LiveAcceleratorOrchestra::kCodecHeight;

        ComPtr<IMFMediaBuffer> buf;
        outRgba8.clear();
        if (FAILED(decSample->ConvertToContiguousBuffer(&buf)) || !buf) return 0.0;

        BYTE* scan0 = nullptr;
        LONG pitch = static_cast<LONG>(decDefaultStride_);
        ComPtr<IMF2DBuffer> buf2d;
        bool locked2d = false;
        if (SUCCEEDED(buf.As(&buf2d)) && buf2d && SUCCEEDED(buf2d->Lock2D(&scan0, &pitch))) {
            locked2d = true;
            if (!scan0 || pitch < static_cast<LONG>(kW) || pitch > 16384) {
                buf2d->Unlock2D();
                return 0.0;
            }
        } else {
            DWORD curLen = 0;
            if (FAILED(buf->Lock(&scan0, nullptr, &curLen))) return 0.0;
            pitch = static_cast<LONG>(decDefaultStride_);
            const size_t required = static_cast<size_t>(pitch) * (decAlignedHeight_ + kH / 2u);
            if (!scan0 || curLen < required) {
                buf->Unlock();
                return 0.0;
            }
        }

        outRgba8.resize(static_cast<size_t>(kW) * kH * 4u);
        const uint8_t* yPlane  = scan0;
        const uint8_t* uvPlane = scan0 + static_cast<size_t>(pitch) * decAlignedHeight_;
        const uint8_t* refYPlane = refNv12;

        uint64_t sqErrY = 0;
        for (uint32_t y = 0; y < kH; ++y) {
            const uint8_t* yRow   = yPlane + static_cast<size_t>(y) * static_cast<size_t>(pitch);
            const uint8_t* uvRow  = uvPlane + static_cast<size_t>(y >> 1u) * static_cast<size_t>(pitch);
            const uint8_t* refRow = refYPlane + static_cast<size_t>(y) * kW;
            uint8_t* dstRow       = outRgba8.data() + static_cast<size_t>(y) * kW * 4u;

            for (uint32_t x = 0; x < kW; ++x) {
                const int yv = static_cast<int>(yRow[x]);
                const int diffY = yv - static_cast<int>(refRow[x]);
                sqErrY += static_cast<uint64_t>(diffY * diffY);

                const uint32_t uvX = x & ~1u;
                const int u = static_cast<int>(uvRow[uvX + 0u]) - 128;
                const int v = static_cast<int>(uvRow[uvX + 1u]) - 128;

                // BT.709 limited-range NV12 -> full-range sRGB 8-bit integer decode
                const int c = std::max(0, yv - 16) * 298;
                const int r = std::clamp((c + 459 * v + 128) >> 8, 0, 255);
                const int g = std::clamp((c - 55 * u - 136 * v + 128) >> 8, 0, 255);
                const int b = std::clamp((c + 541 * u + 128) >> 8, 0, 255);

                dstRow[x * 4u + 0u] = static_cast<uint8_t>(r);
                dstRow[x * 4u + 1u] = static_cast<uint8_t>(g);
                dstRow[x * 4u + 2u] = static_cast<uint8_t>(b);
                dstRow[x * 4u + 3u] = 255u;
            }
        }

        if (locked2d) {
            buf2d->Unlock2D();
        } else {
            buf->Unlock();
        }

        const double mse = static_cast<double>(sqErrY) / static_cast<double>(kW * kH);
        if (mse <= 1e-6) return 60.0;
        return std::clamp(10.0 * std::log10((255.0 * 255.0) / mse), 10.0, 60.0);
    }

    bool didCoInit_ = false;
    DWORD comThread_ = 0;
    bool mfStarted_ = false;
    bool ready_ = false;
    UINT32 encIsAsync_ = 0;
    bool encAllocatesOut_ = false;
    DWORD encOutCbSize_ = 262144;
    uint32_t pendingNeedInputCount_ = 0;
    uint64_t frameIndex_ = 0;
    uint64_t decPacketIndex_ = 0;
    uint64_t decodedTotalFrames_ = 0;
    bool sentSeqHeaderToDec_ = false;
    std::vector<uint8_t> cachedSeqHeader_;

    bool decAllocatesOut_ = false;
    DWORD decOutCbSize_ = 0;
    uint32_t decAlignedWidth_ = LiveAcceleratorOrchestra::kCodecWidth;
    uint32_t decAlignedHeight_ = LiveAcceleratorOrchestra::kCodecHeight;
    uint32_t decDefaultStride_ = LiveAcceleratorOrchestra::kCodecWidth;

    float smoothMbps_ = 0.95f;
    float smoothMs_ = 2.5f;

    ComPtr<IMFTransform> encoder_;
    ComPtr<IMFMediaEventGenerator> encEvGen_;
    ComPtr<IMFTransform> decoder_;
    ComPtr<IMFSample> decReusableSample_;
    ComPtr<IMFMediaBuffer> decReusableBuffer_;
};

} // namespace

// ============================================================================
// LiveAcceleratorOrchestra::Impl
// ============================================================================
struct LiveAcceleratorOrchestra::Impl {
    DynamicCudaDriver cuda;
    int cudaDev = -1;
    void* cudaCtx = nullptr;
    void* cudaStream = nullptr;
    void* evParticleStart = nullptr;
    void* evParticleEnd = nullptr;
    void* evTensorStart = nullptr;
    void* evTensorEnd = nullptr;

    // Live CUDA Particle PTX module & buffers
    bool cudaReady = false;
    std::string cudaName = "HLSL Compute Substitute (8,192 Linear)";
    void* particleMod = nullptr;
    void* particleFn = nullptr;
    uint64_t dParticles = 0;
    uint64_t dParticleVerify = 0;
    LiveParticleState* hParticlesPinned = nullptr;
    std::vector<LiveParticleState> hParticlesFallbackHost;
    uint32_t* hParticleVerifyPinned = nullptr;
    uint32_t hParticleVerifyFallback[4]{};
    float smoothCudaMs = 0.15f;
    bool hasCudaSample = false;

    // Live Tensor Core SIREN Caustic PTX module & buffers
    bool tensorReady = false;
    std::string tensorName = "HLSL Scalar Field Substitute (320x180)";
    void* tensorMod = nullptr;
    void* tensorFn = nullptr;
    uint64_t dTensorOut = 0;
    uint64_t dTensorWeights = 0;
    uint64_t dTensorVerify = 0;
    uint16_t* hTensorPinned = nullptr;
    std::vector<uint16_t> hTensorFallbackHost;
    uint32_t* hTensorVerifyPinned = nullptr;
    uint32_t hTensorVerifyFallback[4]{};
    float smoothTensorMs = 0.18f;
    bool hasTensorSample = false;

    // Live Hardware Codec worker + synchronous access
    bool codecAllowed = false;
    int32_t luidLow = 0;
    int32_t luidHigh = 0;
    std::atomic<bool> codecReady{false};
    std::atomic<bool> codecInitAttempted{false};
    std::string codecName = "HLSL Block Quantization Substitute (8x8)";

    std::mutex codecMutex;
    std::condition_variable codecCv;
    std::thread codecWorker;
    bool codecStop = false;
    bool codecWorkerRunning = false;
    std::vector<uint8_t> pendingNv12;
    uint64_t pendingNv12Seq = 0;
    uint64_t processedNv12Seq = 0;
    uint64_t decodedNv12Seq = 0;
    std::vector<uint8_t> latestDecodedRgba;
    LivePillarTelemetry latestCodecTelemetry{};
    std::unique_ptr<LiveCodecSession> syncOrWorkerCodec;

    bool codecInitDoneFlag = false;

    void ensureCodecStartedLocked() {
        if (codecInitAttempted.load(std::memory_order_relaxed)) return;
        codecInitAttempted.store(true, std::memory_order_release);
        if (!codecAllowed) return;

        codecStop = false;
        codecInitDoneFlag = false;
        codecWorkerRunning = true;
        codecWorker = std::thread([this]() {
          try {
            LiveCodecSession session;
            std::string bname;
            const bool ok = session.initialize(luidLow, luidHigh, bname);
            {
                std::lock_guard<std::mutex> lk(codecMutex);
                if (ok) {
                    codecName = bname;
                    codecReady.store(true, std::memory_order_release);
                }
                codecInitDoneFlag = true;
            }
            codecCv.notify_all();
            if (!ok) {
                return;
            }

            std::vector<uint8_t> localNv12;
            std::vector<uint8_t> localRgba;
            while (true) {
                uint64_t targetSeq = 0;
                {
                    std::unique_lock<std::mutex> lk(codecMutex);
                    codecCv.wait(lk, [&]() { return codecStop || pendingNv12Seq > processedNv12Seq; });
                    if (codecStop) break;
                    targetSeq = pendingNv12Seq;
                    localNv12 = pendingNv12;
                }

                LivePillarTelemetry tel{};
                const bool decOk = session.encodeAndDecodeOneFrame(localNv12.data(), localNv12.size(), localRgba, tel);
                {
                    std::lock_guard<std::mutex> lk(codecMutex);
                    if (decOk) {
                        latestDecodedRgba = localRgba;
                        latestCodecTelemetry = tel;
                        decodedNv12Seq = targetSeq;
                    }
                    processedNv12Seq = targetSeq;
                }
                codecCv.notify_all();
            }
            session.shutdown();
          } catch (const std::exception& e) {
            BOO_LOG_WARN("Codec", std::string("Codec worker failed; selecting shader substitute: ") + e.what());
            std::lock_guard<std::mutex> lk(codecMutex);
            codecReady.store(false, std::memory_order_release);
            codecInitDoneFlag = true;
            codecStop = true;
            latestDecodedRgba.clear();
          } catch (...) {
            BOO_LOG_WARN("Codec", "Codec worker failed; selecting shader substitute.");
            std::lock_guard<std::mutex> lk(codecMutex);
            codecReady.store(false, std::memory_order_release);
            codecInitDoneFlag = true;
            codecStop = true;
            latestDecodedRgba.clear();
          }
          codecCv.notify_all();
        });
    }
};

namespace {
bool loadCudaModuleWithLog(
    DynamicCudaDriver& cuda,
    void** outMod,
    const char* ptxSrc,
    const char* label) {
    char errLog[2048]{};
    // CU_JIT_ERROR_LOG_BUFFER = 5, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES = 6
    unsigned int opts[2] = {5u, 6u};
    void* optVals[2] = {errLog, reinterpret_cast<void*>(static_cast<uintptr_t>(sizeof(errLog) - 1u))};
    const int rc = cuda.cuModuleLoadDataEx(outMod, ptxSrc, 2u, opts, optVals);
    if (rc != 0) {
        BOO_LOG_WARN("CUDA", std::string("PTX JIT failed for ") + label + " (rc=" + std::to_string(rc) + "): " + errLog);
        return false;
    }
    return true;
}
} // namespace

LiveAcceleratorOrchestra::LiveAcceleratorOrchestra()
    : impl_(std::make_unique<Impl>()) {}

LiveAcceleratorOrchestra::~LiveAcceleratorOrchestra() {
    shutdown();
}

void LiveAcceleratorOrchestra::initialize(
    int32_t adapterLuidLow,
    int32_t adapterLuidHigh,
    bool isWarpAdapter,
    bool allowCuda,
    bool allowTensor,
    bool allowCodec) {
    shutdown();
    if (isWarpAdapter) {
        // Hard invariant: never claim hardware acceleration on WARP software adapter!
        return;
    }

    impl_->luidLow = adapterLuidLow;
    impl_->luidHigh = adapterLuidHigh;
    impl_->codecAllowed = allowCodec;

    if ((allowCuda || allowTensor) && impl_->cuda.load()) {
        std::string gpuName;
        int ccMaj = 0, ccMin = 0, smCount = 0;
        const int dev = impl_->cuda.findExactAdapterDevice(
            adapterLuidLow, adapterLuidHigh, gpuName, ccMaj, ccMin, smCount);
        if (dev >= 0 && impl_->cuda.cuDevicePrimaryCtxRetain(&impl_->cudaCtx, dev) == 0) {
            impl_->cudaDev = dev;
            if (impl_->cuda.cuCtxSetCurrent(impl_->cudaCtx) != 0) {
                shutdown();
                impl_->codecAllowed = allowCodec;
                return;
            }
            // CU_STREAM_NON_BLOCKING = 0x1
            if (impl_->cuda.cuStreamCreate(&impl_->cudaStream, 1u) != 0 ||
                impl_->cuda.cuEventCreate(&impl_->evParticleStart, 0u) != 0 ||
                impl_->cuda.cuEventCreate(&impl_->evParticleEnd, 0u) != 0 ||
                impl_->cuda.cuEventCreate(&impl_->evTensorStart, 0u) != 0 ||
                impl_->cuda.cuEventCreate(&impl_->evTensorEnd, 0u) != 0) {
                shutdown();
                impl_->codecAllowed = allowCodec;
                return;
            }

            // 1. Live CUDA 65,536..262,144-Particle PTX Kernel
            if (allowCuda && ccMaj >= 7 &&
                loadCudaModuleWithLog(impl_->cuda, &impl_->particleMod, kLiveCudaParticlesPtx, "boo_live_cuda_particles") &&
                impl_->cuda.cuModuleGetFunction(&impl_->particleFn, impl_->particleMod, "boo_live_cuda_particles") == 0) {
                const size_t partBytes = sizeof(LiveParticleState) * kMaxCudaParticleCount;
                if (impl_->cuda.cuMemAlloc(&impl_->dParticles, partBytes) == 0 &&
                    impl_->cuda.cuMemAlloc(&impl_->dParticleVerify, 64u) == 0) {
                    if (impl_->cuda.cuMemAllocHost) {
                        void* pHost = nullptr;
                        if (impl_->cuda.cuMemAllocHost(&pHost, partBytes) == 0) {
                            impl_->hParticlesPinned = static_cast<LiveParticleState*>(pHost);
                        }
                        void* pVer = nullptr;
                        if (impl_->cuda.cuMemAllocHost(&pVer, 64u) == 0) {
                            impl_->hParticleVerifyPinned = static_cast<uint32_t*>(pVer);
                        }
                    }
                    if (!impl_->hParticlesPinned) {
                        impl_->hParticlesFallbackHost.resize(kMaxCudaParticleCount);
                    }
                    impl_->cudaName = "CUDA Driver PTX (" + gpuName + " SM " +
                                      std::to_string(ccMaj) + "." + std::to_string(ccMin) + ")";
                    impl_->cudaReady = true;
                }
            }

            // 2. Live Tensor Core 320x180 64-Layer SIREN Caustic PTX Kernel (Requires SM 8.0+)
            if (allowTensor && ccMaj >= 8) {
                void* chkMod = nullptr;
                void* chkFn = nullptr;
                bool checksumPassed = false;
                if (loadCudaModuleWithLog(impl_->cuda, &chkMod, kTensorChecksumPtx, "boo_tensor_verify_256") &&
                    impl_->cuda.cuModuleGetFunction(&chkFn, chkMod, "boo_tensor_verify_256") == 0) {
                    uint64_t dChk = 0;
                    if (impl_->cuda.cuMemAlloc(&dChk, 32u * 4u * sizeof(float)) == 0) {
                        uint32_t oneHalf2 = 0x3C003C00u;
                        void* cargs[] = {&dChk, &oneHalf2, &oneHalf2};
                        if (impl_->cuda.cuLaunchKernel(chkFn, 1u, 1u, 1u, 32u, 1u, 1u, 0u, impl_->cudaStream, cargs, nullptr) == 0 &&
                            impl_->cuda.cuStreamSynchronize(impl_->cudaStream) == 0) {
                            float hostChk[128]{};
                            if (impl_->cuda.cuMemcpyDtoH(hostChk, dChk, sizeof(hostChk)) == 0) {
                                checksumPassed = true;
                                for (float v : hostChk) {
                                    if (!std::isfinite(v) || std::fabs(v - 256.0f) > 1e-3f) {
                                        checksumPassed = false;
                                        break;
                                    }
                                }
                            }
                        }
                        impl_->cuda.cuMemFree(dChk);
                    }
                }
                if (chkMod) impl_->cuda.cuModuleUnload(chkMod);

                if (checksumPassed &&
                    loadCudaModuleWithLog(impl_->cuda, &impl_->tensorMod, kLiveTensorCausticPtx, "boo_live_tensor_caustics") &&
                    impl_->cuda.cuModuleGetFunction(&impl_->tensorFn, impl_->tensorMod, "boo_live_tensor_caustics") == 0) {
                    const size_t outBytes = static_cast<size_t>(kTensorWidth) * kTensorHeight * sizeof(uint16_t) * 4u;
                    const auto weights = buildDeterministicTensorWeights();
                    const size_t weightBytes = weights.size() * sizeof(uint32_t);

                    if (impl_->cuda.cuMemAlloc(&impl_->dTensorOut, outBytes) == 0 &&
                        impl_->cuda.cuMemAlloc(&impl_->dTensorWeights, weightBytes) == 0 &&
                        impl_->cuda.cuMemAlloc(&impl_->dTensorVerify, 64u) == 0 &&
                        impl_->cuda.cuMemcpyHtoD(impl_->dTensorWeights, weights.data(), weightBytes) == 0) {
                        if (impl_->cuda.cuMemAllocHost) {
                            void* pHost = nullptr;
                            if (impl_->cuda.cuMemAllocHost(&pHost, outBytes) == 0) {
                                impl_->hTensorPinned = static_cast<uint16_t*>(pHost);
                            }
                            void* pVer = nullptr;
                            if (impl_->cuda.cuMemAllocHost(&pVer, 64u) == 0) {
                                impl_->hTensorVerifyPinned = static_cast<uint32_t*>(pVer);
                            }
                        }
                        if (!impl_->hTensorPinned) {
                            impl_->hTensorFallbackHost.resize(static_cast<size_t>(kTensorWidth) * kTensorHeight * 4u);
                        }
                        impl_->tensorName = "PTX mma.sync.m16n8k16 (" + gpuName + " SM " +
                                            std::to_string(ccMaj) + "." + std::to_string(ccMin) + ")";
                        impl_->tensorReady = true;
                    }
                }
            }
        }
    }
}

void LiveAcceleratorOrchestra::shutdown() noexcept {
    if (!impl_) return;

    // 1. Shut down Codec worker cleanly
    {
        std::lock_guard<std::mutex> lk(impl_->codecMutex);
        impl_->codecStop = true;
    }
    impl_->codecCv.notify_all();
    if (impl_->codecWorker.joinable()) {
        impl_->codecWorker.join();
    }
    impl_->codecWorkerRunning = false;
    impl_->codecStop = false;
    impl_->codecInitDoneFlag = false;
    impl_->codecReady.store(false, std::memory_order_release);
    impl_->codecInitAttempted.store(false, std::memory_order_release);
    impl_->pendingNv12.clear();
    impl_->latestDecodedRgba.clear();
    impl_->pendingNv12Seq = 0;
    impl_->processedNv12Seq = 0;
    impl_->decodedNv12Seq = 0;
    impl_->codecAllowed = false;
    impl_->latestCodecTelemetry = {};

    // 2. Shut down CUDA & Tensor resources cleanly
    if (impl_->cudaCtx && impl_->cuda.lib) {
        impl_->cuda.cuCtxSetCurrent(impl_->cudaCtx);
        if (impl_->cudaStream) {
            impl_->cuda.cuStreamSynchronize(impl_->cudaStream);
        }
        if (impl_->hParticlesPinned && impl_->cuda.cuMemFreeHost) {
            impl_->cuda.cuMemFreeHost(impl_->hParticlesPinned);
            impl_->hParticlesPinned = nullptr;
        }
        if (impl_->hParticleVerifyPinned && impl_->cuda.cuMemFreeHost) {
            impl_->cuda.cuMemFreeHost(impl_->hParticleVerifyPinned);
            impl_->hParticleVerifyPinned = nullptr;
        }
        if (impl_->hTensorPinned && impl_->cuda.cuMemFreeHost) {
            impl_->cuda.cuMemFreeHost(impl_->hTensorPinned);
            impl_->hTensorPinned = nullptr;
        }
        if (impl_->hTensorVerifyPinned && impl_->cuda.cuMemFreeHost) {
            impl_->cuda.cuMemFreeHost(impl_->hTensorVerifyPinned);
            impl_->hTensorVerifyPinned = nullptr;
        }
        if (impl_->dParticles) { impl_->cuda.cuMemFree(impl_->dParticles); impl_->dParticles = 0; }
        if (impl_->dParticleVerify) { impl_->cuda.cuMemFree(impl_->dParticleVerify); impl_->dParticleVerify = 0; }
        if (impl_->dTensorOut) { impl_->cuda.cuMemFree(impl_->dTensorOut); impl_->dTensorOut = 0; }
        if (impl_->dTensorWeights) { impl_->cuda.cuMemFree(impl_->dTensorWeights); impl_->dTensorWeights = 0; }
        if (impl_->dTensorVerify) { impl_->cuda.cuMemFree(impl_->dTensorVerify); impl_->dTensorVerify = 0; }
        if (impl_->particleMod) { impl_->cuda.cuModuleUnload(impl_->particleMod); impl_->particleMod = nullptr; }
        if (impl_->tensorMod) { impl_->cuda.cuModuleUnload(impl_->tensorMod); impl_->tensorMod = nullptr; }
        if (impl_->evParticleStart) { impl_->cuda.cuEventDestroy(impl_->evParticleStart); impl_->evParticleStart = nullptr; }
        if (impl_->evParticleEnd) { impl_->cuda.cuEventDestroy(impl_->evParticleEnd); impl_->evParticleEnd = nullptr; }
        if (impl_->evTensorStart) { impl_->cuda.cuEventDestroy(impl_->evTensorStart); impl_->evTensorStart = nullptr; }
        if (impl_->evTensorEnd) { impl_->cuda.cuEventDestroy(impl_->evTensorEnd); impl_->evTensorEnd = nullptr; }
        if (impl_->cudaStream) { impl_->cuda.cuStreamDestroy(impl_->cudaStream); impl_->cudaStream = nullptr; }
        if (impl_->cudaDev >= 0) {
            impl_->cuda.cuDevicePrimaryCtxRelease(impl_->cudaDev);
            impl_->cudaDev = -1;
        }
        impl_->cudaCtx = nullptr;
    }
    impl_->cudaReady = false;
    impl_->tensorReady = false;
    impl_->hasCudaSample = false;
    impl_->hasTensorSample = false;
    impl_->particleFn = nullptr;
    impl_->tensorFn = nullptr;
    impl_->cudaName = "HLSL Compute Substitute (8,192 Linear)";
    impl_->tensorName = "HLSL Scalar Field Substitute (320x180)";
    impl_->codecName = "HLSL Block Quantization Substitute (8x8)";
}

bool LiveAcceleratorOrchestra::cudaHardwareAvailable() const noexcept {
    return impl_ && impl_->cudaReady;
}

bool LiveAcceleratorOrchestra::tensorHardwareAvailable() const noexcept {
    return impl_ && impl_->tensorReady;
}

bool LiveAcceleratorOrchestra::codecHardwareAvailable() const noexcept {
    if (!impl_ || !impl_->codecAllowed) return false;
    {
        std::unique_lock<std::mutex> lk(impl_->codecMutex);
        impl_->ensureCodecStartedLocked();
        impl_->codecCv.wait_for(lk, std::chrono::milliseconds(1500), [&]() { return impl_->codecInitDoneFlag; });
    }
    return impl_->codecReady.load(std::memory_order_acquire);
}

const std::string& LiveAcceleratorOrchestra::cudaBackendName() const noexcept {
    return impl_->cudaName;
}

const std::string& LiveAcceleratorOrchestra::tensorBackendName() const noexcept {
    return impl_->tensorName;
}

const std::string& LiveAcceleratorOrchestra::codecBackendName() const noexcept {
    return impl_->codecName;
}

const LiveParticleState* LiveAcceleratorOrchestra::simulateCudaParticles(
    int64_t samplePosition,
    int32_t sampleRate,
    uint32_t deterministicSeed,
    Edition edition,
    float orchidReveal01,
    LivePillarTelemetry& outTelemetry,
    uint32_t requestedParticleCount,
    uint32_t simMode) noexcept {
    outTelemetry = {};
    if (!impl_ || !impl_->cudaReady || !impl_->cudaCtx) {
        outTelemetry.status = BackendStatus::Substituted;
        return nullptr;
    }

    if (impl_->cuda.cuCtxSetCurrent(impl_->cudaCtx) != 0) {
        outTelemetry.status = BackendStatus::Faulted;
        return nullptr;
    }

    const uint32_t actualCount = std::clamp<uint32_t>(
        (requestedParticleCount == 0u) ? kCudaParticleCount : requestedParticleCount,
        1024u,
        kMaxCudaParticleCount);
    const double sr = static_cast<double>(std::max<int32_t>(1, sampleRate));
    const float exactTime = static_cast<float>(std::fmod(std::max(0.0, static_cast<double>(samplePosition) / sr), 360.0));
    uint32_t countArg = actualCount;
    uint32_t seedArg = deterministicSeed ^ ((edition == Edition::Midnight) ? 0x9E3779B9u : 0x243F6A88u);
    float timeArg = exactTime;
    float revealArg = std::clamp(orchidReveal01, 0.0f, 1.0f);
    uint32_t modeArg = simMode;

    LiveParticleState* dstParticles = impl_->hParticlesPinned
                                          ? impl_->hParticlesPinned
                                          : impl_->hParticlesFallbackHost.data();
    uint32_t* dstVerify = impl_->hParticleVerifyPinned
                              ? impl_->hParticleVerifyPinned
                              : impl_->hParticleVerifyFallback;
    dstVerify[0] = 0u;
    dstVerify[1] = 0u;
    dstVerify[2] = 0u;

    void* args[] = {
        &impl_->dParticles,
        &impl_->dParticleVerify,
        &countArg,
        &seedArg,
        &timeArg,
        &revealArg,
        &modeArg
    };
    const unsigned int blockDim = 256u;
    const unsigned int gridDim  = (actualCount + blockDim - 1u) / blockDim;
    const size_t copyBytes = sizeof(LiveParticleState) * static_cast<size_t>(actualCount);

    impl_->cuda.cuEventRecord(impl_->evParticleStart, impl_->cudaStream);
    const int launchRc = impl_->cuda.cuLaunchKernel(
        impl_->particleFn, gridDim, 1u, 1u, blockDim, 1u, 1u, 0u, impl_->cudaStream, args, nullptr);
    impl_->cuda.cuEventRecord(impl_->evParticleEnd, impl_->cudaStream);

    if (launchRc != 0) {
        outTelemetry.status = BackendStatus::Faulted;
        return nullptr;
    }

    int copyRc = 0;
    if (impl_->cuda.cuMemcpyDtoHAsync) {
        copyRc = impl_->cuda.cuMemcpyDtoHAsync(dstParticles, impl_->dParticles, copyBytes, impl_->cudaStream);
        copyRc |= impl_->cuda.cuMemcpyDtoHAsync(dstVerify, impl_->dParticleVerify, 16u, impl_->cudaStream);
        copyRc |= impl_->cuda.cuStreamSynchronize(impl_->cudaStream);
    } else {
        copyRc = impl_->cuda.cuStreamSynchronize(impl_->cudaStream);
        copyRc |= impl_->cuda.cuMemcpyDtoH(dstParticles, impl_->dParticles, copyBytes);
        copyRc |= impl_->cuda.cuMemcpyDtoH(dstVerify, impl_->dParticleVerify, 16u);
    }

    if (copyRc != 0 ||
        dstVerify[0] != 0xC0DAC0DEu ||
        dstVerify[1] != actualCount ||
        dstVerify[2] != kCudaSubsteps ||
        !std::isfinite(dstParticles[0].px) ||
        !std::isfinite(dstParticles[actualCount - 1u].py)) {
        outTelemetry.status = BackendStatus::Faulted;
        return nullptr;
    }

    float kernelMs = 0.0f;
    impl_->cuda.cuEventElapsedTime(&kernelMs, impl_->evParticleStart, impl_->evParticleEnd);
    const float clampedMs = std::max(0.005f, kernelMs);
    impl_->smoothCudaMs = impl_->hasCudaSample ? (impl_->smoothCudaMs * 0.85f + clampedMs * 0.15f) : clampedMs;
    impl_->hasCudaSample = true;

    // Throughput in Million Particle-Substeps / sec
    const float millionStepsPerFrame =
        (static_cast<float>(actualCount) * static_cast<float>(kCudaSubsteps)) / 1.0e6f;

    outTelemetry.status = BackendStatus::Accelerated;
    outTelemetry.activeInScene = true;
    outTelemetry.hardwareVerified = true;
    outTelemetry.workloadItems = actualCount;
    outTelemetry.workloadSteps = kCudaSubsteps;
    outTelemetry.executionMs = impl_->smoothCudaMs;
    outTelemetry.throughput = millionStepsPerFrame / (impl_->smoothCudaMs * 0.001f); // MSteps/s
    return dstParticles;
}

const uint16_t* LiveAcceleratorOrchestra::evaluateTensorCausticField(
    int64_t samplePosition,
    int32_t sampleRate,
    uint32_t deterministicSeed,
    Edition edition,
    float orchidReveal01,
    LivePillarTelemetry& outTelemetry) noexcept {
    outTelemetry = {};
    if (!impl_ || !impl_->tensorReady || !impl_->cudaCtx) {
        outTelemetry.status = BackendStatus::Substituted;
        return nullptr;
    }

    if (impl_->cuda.cuCtxSetCurrent(impl_->cudaCtx) != 0) {
        outTelemetry.status = BackendStatus::Faulted;
        return nullptr;
    }

    const double sr = static_cast<double>(std::max<int32_t>(1, sampleRate));
    const float exactTime = static_cast<float>(std::fmod(std::max(0.0, static_cast<double>(samplePosition) / sr), 360.0));
    uint32_t layersArg = kTensorLayers;
    float timeArg = exactTime;
    float revealArg = std::clamp(orchidReveal01, 0.0f, 1.0f);
    float editionShiftArg = ((edition == Edition::Midnight) ? 1.5707963f : 0.0f) +
                            static_cast<float>(deterministicSeed & 0xFFu) * 0.0125f;

    uint16_t* dstTex = impl_->hTensorPinned ? impl_->hTensorPinned : impl_->hTensorFallbackHost.data();
    uint32_t* dstVerify = impl_->hTensorVerifyPinned ? impl_->hTensorVerifyPinned : impl_->hTensorVerifyFallback;
    dstVerify[0] = 0u;
    dstVerify[1] = 0u;

    void* args[] = {
        &impl_->dTensorOut,
        &impl_->dTensorWeights,
        &impl_->dTensorVerify,
        &layersArg,
        &timeArg,
        &revealArg,
        &editionShiftArg
    };
    // 3,600 warps = 115,200 threads -> 450 blocks of 256 threads
    constexpr unsigned int kBlockThreads = 256u;
    constexpr unsigned int kGridBlocks   = 450u;
    const size_t copyBytes = static_cast<size_t>(kTensorWidth) * kTensorHeight * sizeof(uint16_t) * 4u;

    impl_->cuda.cuEventRecord(impl_->evTensorStart, impl_->cudaStream);
    const int launchRc = impl_->cuda.cuLaunchKernel(
        impl_->tensorFn, kGridBlocks, 1u, 1u, kBlockThreads, 1u, 1u, 0u, impl_->cudaStream, args, nullptr);
    impl_->cuda.cuEventRecord(impl_->evTensorEnd, impl_->cudaStream);

    if (launchRc != 0) {
        outTelemetry.status = BackendStatus::Faulted;
        return nullptr;
    }

    int copyRc = 0;
    if (impl_->cuda.cuMemcpyDtoHAsync) {
        copyRc = impl_->cuda.cuMemcpyDtoHAsync(dstTex, impl_->dTensorOut, copyBytes, impl_->cudaStream);
        copyRc |= impl_->cuda.cuMemcpyDtoHAsync(dstVerify, impl_->dTensorVerify, 16u, impl_->cudaStream);
        copyRc |= impl_->cuda.cuStreamSynchronize(impl_->cudaStream);
    } else {
        copyRc = impl_->cuda.cuStreamSynchronize(impl_->cudaStream);
        copyRc |= impl_->cuda.cuMemcpyDtoH(dstTex, impl_->dTensorOut, copyBytes);
        copyRc |= impl_->cuda.cuMemcpyDtoH(dstVerify, impl_->dTensorVerify, 16u);
    }

    const float sampleTex0 = fp16BitsToFloat(dstTex[0]);
    if (copyRc != 0 ||
        dstVerify[0] != 0x7E450A80u ||
        dstVerify[1] != kTensorLayers ||
        !std::isfinite(sampleTex0)) {
        outTelemetry.status = BackendStatus::Faulted;
        return nullptr;
    }

    float kernelMs = 0.0f;
    impl_->cuda.cuEventElapsedTime(&kernelMs, impl_->evTensorStart, impl_->evTensorEnd);
    const float clampedMs = std::max(0.010f, kernelMs);
    impl_->smoothTensorMs = impl_->hasTensorSample ? (impl_->smoothTensorMs * 0.85f + clampedMs * 0.15f) : clampedMs;
    impl_->hasTensorSample = true;

    // Exact Tensor Core FLOPs/frame: 3,600 warps * 512 mma.sync * (2 * 16 * 8 * 16) = 7,549,747,200 FLOPs
    constexpr double kGflopsPerFrame = (3600.0 * 512.0 * 4096.0) / 1.0e9; // 7.5497 GFLOP
    const float tflops = static_cast<float>((kGflopsPerFrame / (static_cast<double>(impl_->smoothTensorMs) * 0.001)) / 1000.0);

    outTelemetry.status = BackendStatus::Accelerated;
    outTelemetry.activeInScene = true;
    outTelemetry.hardwareVerified = true;
    outTelemetry.workloadItems = kTensorWidth * kTensorHeight;
    outTelemetry.workloadSteps = kTensorMmaPerWarp;
    outTelemetry.executionMs = impl_->smoothTensorMs;
    outTelemetry.throughput = tflops; // TFLOPS
    outTelemetry.auxMetric = static_cast<float>(kGflopsPerFrame);
    return dstTex;
}

bool LiveAcceleratorOrchestra::processCodecRoundTrip(
    const uint8_t* nv12FrameBytes,
    size_t nv12ByteCount,
    bool waitForExactFrame,
    std::vector<uint8_t>& outRgba8,
    LivePillarTelemetry& outTelemetry) {
    outTelemetry = {};
    outRgba8.clear();
    constexpr size_t kExpectedNv12 = (static_cast<size_t>(kCodecWidth) * kCodecHeight * 3u) / 2u;
    if (!impl_ || !impl_->codecAllowed || !nv12FrameBytes || nv12ByteCount < kExpectedNv12) {
        outTelemetry.status = BackendStatus::Substituted;
        return false;
    }

    uint64_t submittedSeq = 0;
    {
        std::unique_lock<std::mutex> lk(impl_->codecMutex);
        impl_->ensureCodecStartedLocked();
        if (waitForExactFrame) {
            impl_->codecCv.wait_for(lk, std::chrono::milliseconds(1500), [&]() { return impl_->codecInitDoneFlag; });
        }
        if (!impl_->codecReady.load(std::memory_order_acquire)) {
            outTelemetry.status = BackendStatus::Substituted;
            return false;
        }

        // Submit latest NV12 frame into single-slot mailbox (never builds latency queue)
        if (waitForExactFrame || impl_->pendingNv12Seq == impl_->processedNv12Seq) {
            impl_->pendingNv12.assign(nv12FrameBytes, nv12FrameBytes + kExpectedNv12);
            submittedSeq = ++impl_->pendingNv12Seq;
            impl_->codecCv.notify_all();
        } else {
            submittedSeq = impl_->pendingNv12Seq;
        }

        if (waitForExactFrame) {
            impl_->codecCv.wait_for(lk, std::chrono::milliseconds(400), [&]() {
                return impl_->processedNv12Seq >= submittedSeq || impl_->codecStop;
            });
        }

        if (impl_->latestDecodedRgba.empty() || (waitForExactFrame && impl_->decodedNv12Seq != submittedSeq)) {
            outTelemetry.status = BackendStatus::Faulted;
            return false;
        }

        outRgba8 = impl_->latestDecodedRgba;
        outTelemetry = impl_->latestCodecTelemetry;
    }
    return true;
}

} // namespace boo
