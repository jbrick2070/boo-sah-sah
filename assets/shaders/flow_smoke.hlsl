// NVIDIA Flow smoke, drawn by the film's own renderer (src/render/flow_smoke.cpp).
//
// Flow, the smoke and fire solver of the PhysX family, simulates on the GPU and hands its smoke
// density back as a sparse NanoVDB grid. This pass marches each camera ray through that grid,
// stops at whatever the scene's distance buffer says is in the way, lights the smoke from the key
// light with its own shadowing, and lays the result over SceneHDR.
//
// PNanoVDB.h is the NanoVDB reader that ships with the PhysX SDK; it is included from the SDK
// checkout at build time, not copied here.
#define PNANOVDB_HLSL
#include "PNanoVDB.h"

cbuffer SmokeConstants : register(b0)
{
    float3 gEye;        float gTanHalfFov;
    float3 gRight;      float gAspect;
    float3 gUp;         float gExtinction;   // per unit of smoke, per metre
    float3 gForward;    float gStepMetres;
    float3 gLightDir;   float gVoxelMetres;  // gLightDir points toward the light
    float3 gLightColor; uint  gFrame;
    float3 gAmbient;    uint  gWidth;
    float3 gBoxMin;     uint  gHeight;
    float3 gBoxMax;     float gShadowReach;  // metres of smoke looked through toward the light
};

Texture2D<float>        gDistance : register(t0); // metres from the eye to the scene
StructuredBuffer<uint>  gSmoke    : register(t1); // NanoVDB float grid from Flow
RWTexture2D<float4>     gHdr      : register(u0);

static const uint kGridType = PNANOVDB_GRID_TYPE_FLOAT;
static const uint kMaxSteps = 384u;

float smokeAt(inout pnanovdb_readaccessor_t acc, int3 ijk)
{
    return pnanovdb_read_float(gSmoke, pnanovdb_readaccessor_get_value_address(kGridType, gSmoke, acc, ijk));
}

// Smoke between voxel centres (index space; centres sit on whole numbers).
float smokeSmooth(inout pnanovdb_readaccessor_t acc, float3 p)
{
    const float3 base = floor(p);
    const float3 f = p - base;
    const int3 i = int3(base);
    const float c000 = smokeAt(acc, i);
    const float c100 = smokeAt(acc, i + int3(1, 0, 0));
    const float c010 = smokeAt(acc, i + int3(0, 1, 0));
    const float c110 = smokeAt(acc, i + int3(1, 1, 0));
    const float c001 = smokeAt(acc, i + int3(0, 0, 1));
    const float c101 = smokeAt(acc, i + int3(1, 0, 1));
    const float c011 = smokeAt(acc, i + int3(0, 1, 1));
    const float c111 = smokeAt(acc, i + int3(1, 1, 1));
    const float x00 = lerp(c000, c100, f.x), x10 = lerp(c010, c110, f.x);
    const float x01 = lerp(c001, c101, f.x), x11 = lerp(c011, c111, f.x);
    return max(lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z), 0.0);
}

// Interleaved gradient noise: a different start along every ray, every frame.
float rayOffset(uint2 pixel, uint frame)
{
    const float2 p = float2(pixel) + 5.588238 * float(frame & 63u);
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

[numthreads(8, 8, 1)]
void CSSmokeMarch(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    const float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    const float3 dir = normalize(gForward + gRight * (ndc.x * gAspect * gTanHalfFov) + gUp * (ndc.y * gTanHalfFov));

    // Where the ray is inside the grid's box and in front of the scene.
    const float3 safeDir = float3(abs(dir.x) < 1e-6 ? 1e-6 : dir.x, abs(dir.y) < 1e-6 ? 1e-6 : dir.y, abs(dir.z) < 1e-6 ? 1e-6 : dir.z);
    const float3 ta = (gBoxMin - gEye) / safeDir, tb = (gBoxMax - gEye) / safeDir;
    const float3 tLo = min(ta, tb), tHi = max(ta, tb);
    const float tEnter = max(max(tLo.x, tLo.y), max(tLo.z, 0.0));
    const float tLeave = min(min(tHi.x, tHi.y), min(tHi.z, gDistance[id.xy]));
    if (tEnter >= tLeave) return;

    pnanovdb_grid_handle_t grid;
    grid.address.byte_offset = 0u;
    const pnanovdb_root_handle_t root = pnanovdb_tree_get_root(gSmoke, pnanovdb_grid_get_tree(gSmoke, grid));
    pnanovdb_readaccessor_t acc;
    pnanovdb_readaccessor_init(acc, root);
    pnanovdb_readaccessor_t shadowAcc = acc;

    // The ray in index space: p(t) = origin + step * t, t in metres.
    const float3 origin = pnanovdb_grid_world_to_indexf(gSmoke, grid, gEye);
    const float3 perMetre = pnanovdb_grid_world_to_indexf(gSmoke, grid, gEye + dir) - origin;
    const float3 safeStep = float3(abs(perMetre.x) < 1e-6 ? 1e-6 : perMetre.x, abs(perMetre.y) < 1e-6 ? 1e-6 : perMetre.y,
                                   abs(perMetre.z) < 1e-6 ? 1e-6 : perMetre.z);
    const float3 lightStep = (pnanovdb_grid_world_to_indexf(gSmoke, grid, gEye + gLightDir) - origin); // one metre toward the light

    float t = tEnter + gStepMetres * rayOffset(id.xy, gFrame);
    float through = 1.0; // how much of the scene behind still shows
    float3 glow = 0.0;
    for (uint i = 0u; i < kMaxSteps && t < tLeave && through > 0.012; ++i)
    {
        const float3 p = origin + perMetre * t;
        const int3 cell = int3(floor(p));
        if (!pnanovdb_readaccessor_is_active(kGridType, gSmoke, acc, cell))
        {
            // Nothing here: leap to the far side of the empty node this point is in.
            const int dim = int(pnanovdb_readaccessor_get_dim(kGridType, gSmoke, acc, cell));
            const float3 lo = float3(cell & ~(dim - 1));
            const float3 wall = lo + float3(dim, dim, dim) * step(0.0, perMetre);
            const float3 toWall = (wall - p) / safeStep;
            t += max(min(toWall.x, min(toWall.y, toWall.z)), 0.0) + 0.25 * gStepMetres;
            continue;
        }
        const float smoke = smokeSmooth(acc, p);
        if (smoke > 0.002)
        {
            // How much smoke lies between here and the light.
            float towardLight = 0.0;
            float reach = 0.6;
            float covered = 0.0;
            [unroll] for (uint s = 0u; s < 4u; ++s)
            {
                const float mid = covered + 0.5 * reach;
                towardLight += max(smokeAt(shadowAcc, int3(floor(p + lightStep * mid + 0.5))), 0.0) * reach;
                covered += reach;
                reach *= 2.1;
            }
            const float lit = exp(-gExtinction * towardLight * (gShadowReach / max(covered, 1e-3)));
            const float absorbed = 1.0 - exp(-gExtinction * smoke * gStepMetres);
            glow += through * absorbed * (gLightColor * lit + gAmbient);
            through *= 1.0 - absorbed;
        }
        t += gStepMetres;
    }
    if (through > 0.999) return;
    const float4 scene = gHdr[id.xy];
    gHdr[id.xy] = float4(scene.rgb * through + glow, scene.a);
}
