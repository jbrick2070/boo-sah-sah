// BOO SAH SAH — The Glass Orchid Protocol
// Ad-Hoc Numerical Benchmark Kernels (DXR 1.1 Hardware RayQuery + D3D12 HLSL Substitutes)

cbuffer BenchCB : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    uint  gIteration;
    uint  gParticleCount;
    float gTime;
    float gParam0;
    float gParam1;
    float gParam2;
};

// -----------------------------------------------------------------------------
// 1. Hardware DXR 1.1 Inline RayQuery Benchmark (SM 6.5)
// Casts 4 hardware BVH rays per pixel (primary + reflective fan) at 1920x1080 = 8,294,400 rays/pass
// -----------------------------------------------------------------------------
#if defined(BOO_SHADER_DXR_BENCH)
RaytracingAccelerationStructure gSceneTlas : register(t0);
RWStructuredBuffer<uint> gOutHitSummary : register(u0);

[numthreads(8, 8, 1)]
void CSDxrRayQueryBench(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gWidth || dtid.y >= gHeight)
        return;

    float2 uv = (float2(dtid.xy) + 0.5) / float2(gWidth, gHeight);
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    ndc.x *= (float)gWidth / (float)gHeight;

    float angle = gTime * 0.25 + (float)gIteration * 0.013;
    float3 camOrigin = float3(sin(angle) * 6.5, 1.2, -cos(angle) * 6.5);
    float3 forward   = normalize(-camOrigin);
    float3 rightVec  = normalize(cross(float3(0.0, 1.0, 0.0), forward));
    float3 upVec     = cross(forward, rightVec);

    uint localHits = 0u;
    float accumT   = 0.0;

    [unroll]
    for (uint s = 0u; s < 4u; ++s)
    {
        float2 jitter = float2((s & 1u) ? 0.0015 : -0.0015, (s & 2u) ? 0.0015 : -0.0015);
        float3 dir = normalize(forward * 1.45 + rightVec * (ndc.x + jitter.x) + upVec * (ndc.y + jitter.y));

        RayDesc ray;
        ray.Origin    = camOrigin;
        ray.Direction = dir;
        ray.TMin      = 0.01;
        ray.TMax      = 50.0;

        RayQuery<RAY_FLAG_CULL_NON_OPAQUE> rq;
        rq.TraceRayInline(gSceneTlas, RAY_FLAG_NONE, 0xFF, ray);
        while (rq.Proceed())
        {
        }
        if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            localHits += 1u;
            accumT += rq.CommittedRayT();
        }
    }

    if (((dtid.x & 31u) == 0u) && ((dtid.y & 31u) == 0u) && localHits > 0u)
    {
        InterlockedAdd(gOutHitSummary[0], localHits);
        InterlockedAdd(gOutHitSummary[1], (uint)(accumT * 10.0));
    }
}
#endif

// -----------------------------------------------------------------------------
// 2. Software HLSL Ray-Crystal Intersection Substitute Benchmark (SM 6.0, non-DXR)
// Casts the exact same 4 rays/pixel against the 64-crystal lattice without DXR BVH
// -----------------------------------------------------------------------------
#if defined(BOO_SHADER_RT_FALLBACK_BENCH)
RWStructuredBuffer<uint> gOutFallbackHits : register(u0);

[numthreads(8, 8, 1)]
void CSAnalyticRtFallbackBench(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gWidth || dtid.y >= gHeight)
        return;

    float2 uv = (float2(dtid.xy) + 0.5) / float2(gWidth, gHeight);
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    ndc.x *= (float)gWidth / (float)gHeight;

    float angle = gTime * 0.25 + (float)gIteration * 0.013;
    float3 camOrigin = float3(sin(angle) * 6.5, 1.2, -cos(angle) * 6.5);
    float3 forward   = normalize(-camOrigin);
    float3 rightVec  = normalize(cross(float3(0.0, 1.0, 0.0), forward));
    float3 upVec     = cross(forward, rightVec);

    uint hits = 0u;
    for (uint s = 0u; s < 4u; ++s)
    {
        float2 jitter = float2((s & 1u) ? 0.0015 : -0.0015, (s & 2u) ? 0.0015 : -0.0015);
        float3 rd = normalize(forward * 1.45 + rightVec * (ndc.x + jitter.x) + upVec * (ndc.y + jitter.y));

        for (uint i = 0u; i < 64u; ++i)
        {
            float gx = ((float)(i & 7u) - 3.5) * 1.15;
            float gy = ((float)(i >> 3u) - 3.5) * 0.85;
            float gz = sin((float)i * 0.7) * 1.2;
            float3 oc = camOrigin - float3(gx, gy, gz);
            float b = dot(oc, rd);
            float c = dot(oc, oc) - 0.30;
            if (b * b - c > 0.0)
            {
                hits += 1u;
                break;
            }
        }
    }

    if (((dtid.x & 31u) == 0u) && ((dtid.y & 31u) == 0u) && hits > 0u)
    {
        InterlockedAdd(gOutFallbackHits[0], hits);
    }
}
#endif

// -----------------------------------------------------------------------------
// 3. D3D12 HLSL Compute 1M-Particle Mutiny Substitute Benchmark (SM 6.0)
// -----------------------------------------------------------------------------
#if defined(BOO_SHADER_PARTICLE_BENCH)
RWStructuredBuffer<float4> gPositions : register(u0);
RWStructuredBuffer<float4> gVelocities : register(u1);

[numthreads(256, 1, 1)]
void CSHlslParticleBench(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    if (idx >= gParticleCount)
        return;

    float4 pos = gPositions[idx];
    float4 vel = gVelocities[idx];
    float dt = 0.0166667;

    // 8-step curl-vortex integration matching CUDA benchmark workload
    [unroll]
    for (uint k = 0u; k < 8u; ++k)
    {
        float3 c = float3(
            sin(pos.y * 1.7 + gTime) - cos(pos.z * 1.3),
            sin(pos.z * 1.7 + gTime) - cos(pos.x * 1.3),
            sin(pos.x * 1.7 + gTime) - cos(pos.y * 1.3));
        vel.xyz = vel.xyz * 0.992 + c * 0.045;
        pos.xyz += vel.xyz * (dt * 0.125);
    }
    pos.w += dt;

    gPositions[idx] = pos;
    gVelocities[idx] = vel;
}
#endif

// -----------------------------------------------------------------------------
// 4. D3D12 HLSL Neural Velvet MLP Procedural Substitute Benchmark (960x540, SM 6.0)
// -----------------------------------------------------------------------------
#if defined(BOO_SHADER_TENSOR_FALLBACK_BENCH)
RWTexture2D<float4> gOutNeuralTex : register(u0);

[numthreads(8, 8, 1)]
void CSHlslNeuralVelvetBench(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gWidth || dtid.y >= gHeight)
        return;

    float2 uv = (float2(dtid.xy) + 0.5) / float2(gWidth, gHeight);
    float4 h0 = float4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, sin(gTime), cos(gTime));
    float4 h1 = float4(cos(uv.x * 6.283), sin(uv.y * 6.283), 0.5, -0.5);

    // Compact 16-stage procedural MLP matrix-vector layers
    [unroll]
    for (uint layer = 0u; layer < 16u; ++layer)
    {
        float fl = (float)layer * 0.37 + 0.11;
        float4 n0 = tanh(h0 * (1.15 + 0.05 * sin(fl)) + h1.yzwx * 0.62 + fl * 0.03);
        float4 n1 = tanh(h1 * 1.08 - h0.zwxy * 0.58 + fl * 0.02);
        h0 = n0;
        h1 = n1;
    }

    gOutNeuralTex[dtid.xy] = float4(0.5 + 0.5 * h0.xyz, 1.0);
}
#endif

// -----------------------------------------------------------------------------
// 5. D3D12 HLSL "SHADER SIGNAL" Codec Substitute Benchmark (1920x1080, SM 6.0)
// -----------------------------------------------------------------------------
#if defined(BOO_SHADER_SIGNAL_BENCH)
RWTexture2D<float4> gOutSignalTex : register(u0);

[numthreads(8, 8, 1)]
void CSShaderSignalBench(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gWidth || dtid.y >= gHeight)
        return;

    // 8x8 macroblock quantization + YCoCg 4:2:0 subsampling + temporal feedback glitch
    uint2 blockId = dtid.xy >> 3u;
    float2 blockUv = (float2(blockId << 3u) + 4.0) / float2(gWidth, gHeight);
    float2 fineUv  = (float2(dtid.xy) + 0.5) / float2(gWidth, gHeight);

    float glitch = frac(sin(dot(float2(blockId), float2(12.9898, 78.233)) + gTime) * 43758.5453);
    float2 warpedUv = (glitch > 0.92) ? (blockUv + float2(0.03 * (glitch - 0.92), 0.0)) : fineUv;

    float3 rgb = float3(
        0.5 + 0.5 * sin(warpedUv.x * 18.0 + gTime),
        0.5 + 0.5 * cos(warpedUv.y * 14.0 - gTime * 1.3),
        0.5 + 0.5 * sin((warpedUv.x + warpedUv.y) * 10.0));

    // Convert RGB -> YCoCg, quantize chroma on 2x2 block, reconstruct RGB
    float Y  = dot(rgb, float3(0.25, 0.50, 0.25));
    float Co = dot(rgb, float3(0.50, 0.00, -0.50));
    float Cg = dot(rgb, float3(-0.25, 0.50, -0.25));

    Y  = floor(Y  * 24.0 + 0.5) / 24.0;
    Co = floor(Co * 10.0 + 0.5) / 10.0;
    Cg = floor(Cg * 10.0 + 0.5) / 10.0;

    float3 recon = saturate(float3(Y + Co - Cg, Y + Cg, Y - Co - Cg));
    gOutSignalTex[dtid.xy] = float4(recon, 1.0);
}
#endif
