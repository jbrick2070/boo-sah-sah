// Unified PBR + DXR 1.1 Inline-Raytracing Shader for BooSahSah
// Supports SM 6.0 raster (opaque + alpha-cutout) and SM 6.5 DXR 1.1 inline RayQuery
// with BC7 sRGB base-color/alpha, BC5 tangent-space normals, BC7 linear ORMT,
// Toksvig roughness mip-chains, RGBA16F GGX prefiltered IBL, cutout candidate rays,
// unified secondary-hit PBR evaluation, stratified soft shadows, and contact AO.

struct Vertex {
    float4 p;
    float4 n;
    float4 uv;
    float4 tangent;
};

cbuffer Scene : register(b0) {
    float3 eye;      float fov;
    float3 right;    float aspect;
    float3 up;       float time;
    float3 forward;  float reveal;
    uint width;      uint height;   uint act;       uint canonical;
    float daylight;  float pulse;   uint showFlags; float moodAccent;
    float moodBloom; float moodLight; float moodCut; float scenePadding;
    float3 sunOverrideDir; float sunOverrideScale;   // Explore-mode interactive sun (interFlags bit 0)
    uint accumFrame; uint interFlags; float2 jitterNdc; // bit1 = progressive accumulation, bit2 = lamps off
};

cbuffer Draw : register(b1) {
    float4 row0;
    float4 row1;
    float4 row2;
    uint firstIndex;
    uint material;
    uint part;
    uint unused;
};

StructuredBuffer<Vertex> vertices : register(t0);
StructuredBuffer<uint>   indices  : register(t1);
#if defined(M2_RT)
RaytracingAccelerationStructure scene : register(t2);
RWStructuredBuffer<uint> verified : register(u0);
#endif

Texture2D<float4>      tensorField     : register(t3);
Texture2DArray<float4> gAlbedoAlphaTex : register(t4);
Texture2DArray<float4> gNormalTex      : register(t5);
Texture2DArray<float4> gOrmtTex        : register(t6);
Texture2DArray<float4> gEnvHdrTex      : register(t7);

SamplerState linearClamp     : register(s0);
SamplerState anisoWrap       : register(s1);
SamplerState equirectSampler : register(s2);

static const float kPi = 3.14159265;
static const float kTwoPi = 6.28318530;

float3 norm(float3 v) {
    return v * rsqrt(max(dot(v, v), 1e-10));
}

float3 transformPoint(float3 p) {
    float4 q = float4(p, 1.0);
    return float3(dot(row0, q), dot(row1, q), dot(row2, q));
}

float3 transformNormal(float3 n) {
    return norm(float3(dot(row0.xyz, n), dot(row1.xyz, n), dot(row2.xyz, n)));
}

float hash21(float2 p) {
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float interleavedGradientNoise(float2 pix) {
    return frac(52.9829189 * frac(0.06711056 * floor(pix.x) + 0.00583715 * floor(pix.y)));
}

float puddleMask(float2 xz) {
    float a = 0.5 + 0.5 * sin(xz.x * 0.43 + sin(xz.y * 0.31) * 1.4);
    float b = 0.5 + 0.5 * cos(xz.y * 0.67 - sin(xz.x * 0.52) * 1.2);
    return smoothstep(0.36, 0.78, a * 0.58 + b * 0.42);
}

bool isDawnFinale() {
    return ((showFlags & 1u) != 0u) && (time >= 338.5);
}

bool isGrandTutti() {
    return ((showFlags & 1u) != 0u) && (time >= 274.5) && (time < 338.0);
}

bool isInsideDawnBathroom(float3 p) {
    return isDawnFinale() &&
           (p.x > -2.56 && p.x < 1.56) &&
           (p.y > 2.58 && p.y < 5.95) &&
           (p.z > 2.12 && p.z < 6.62);
}

bool sunOverridden() {
    return (interFlags & 1u) != 0u;
}

bool accumulating() {
    return (interFlags & 2u) != 0u;
}

bool lampsOff() {
    return (interFlags & 4u) != 0u;
}

bool exploreMode() {
    return (interFlags & 8u) != 0u;
}

// Scripted dawn ramp over film time, or (in Explore mode) driven by the interactive sun elevation.
float timeRamp(float a, float b) {
    if (sunOverridden()) {
        return smoothstep(-0.10, 0.30, norm(sunOverrideDir).y);
    }
    return smoothstep(a, b, time);
}

float3 sceneSunDir() {
    if (sunOverridden()) {
        return norm(sunOverrideDir);
    }
    if (isDawnFinale()) {
        // Golden-peach Taipei dawn sun shining from the southeast (+X, +Y, -Z) through the X=+1.65 bathroom window
        return norm(float3(0.80, 0.34, -0.49));
    }
    if (act == 1u) {
        return norm(float3(-0.48, 0.22 + 0.28 * daylight, 0.72));
    }
    return norm(float3(-0.44, 0.74, -0.32));
}

float3 sceneSunRadiance() {
    if (sunOverridden()) {
        // Physically motivated time-of-day: warm low sun -> neutral high sun -> cool moonlight below the horizon
        float3 d = norm(sunOverrideDir);
        float above = smoothstep(-0.04, 0.10, d.y);
        float3 lowSun  = float3(3.35, 2.05, 1.12);
        float3 highSun = float3(3.30, 3.05, 2.80);
        float3 moon    = float3(0.10, 0.14, 0.24);
        float3 sunCol = lerp(lowSun, highSun, smoothstep(0.08, 0.60, saturate(d.y)));
        return (sunCol * above + moon * (1.0 - above)) * sunOverrideScale;
    }
    if (isDawnFinale()) {
        float dawnRamp = timeRamp(336.0, 346.0);
        return lerp(float3(0.55, 0.62, 0.82), float3(3.35, 2.32, 1.52), dawnRamp);
    }
    float3 nightKey = (act == 1u) ? float3(0.30, 0.44, 0.66) : float3(0.52, 0.78, 1.08);
    return lerp(nightKey, float3(1.85, 1.32, 0.88), daylight);
}

float2 dirToEquirectUv(float3 d) {
    float3 dn = norm(d);
    return float2(atan2(dn.x, dn.z) / kTwoPi + 0.5, acos(clamp(dn.y, -1.0, 1.0)) / kPi);
}

float3 sampleIblEnv(float3 d, float rough, bool insideBathroom) {
    float2 uv = dirToEquirectUv(d);
    float mip = clamp(rough * 8.0, 0.0, 8.0);
    if (insideBathroom) {
        float3 dawnEnv = gEnvHdrTex.SampleLevel(equirectSampler, float3(uv, 2.0), mip).rgb;
        if (sunOverridden()) {
            float3 nightEnv = gEnvHdrTex.SampleLevel(equirectSampler, float3(uv, 0.0), mip).rgb * 0.24;
            return lerp(nightEnv, dawnEnv, timeRamp(336.0, 345.0));
        }
        return dawnEnv;
    }
    if (isDawnFinale()) {
        float3 nightEnv = gEnvHdrTex.SampleLevel(equirectSampler, float3(uv, 0.0), mip).rgb;
        float3 dawnEnv  = gEnvHdrTex.SampleLevel(equirectSampler, float3(uv, 2.0), mip).rgb;
        return lerp(nightEnv, dawnEnv, timeRamp(336.0, 345.0));
    }
    if (act == 1u) {
        float3 nightEnv = gEnvHdrTex.SampleLevel(equirectSampler, float3(uv, 0.0), mip).rgb;
        float3 harbEnv  = gEnvHdrTex.SampleLevel(equirectSampler, float3(uv, 1.0), mip).rgb;
        return lerp(nightEnv, harbEnv, saturate(daylight * 1.15));
    }
    float3 s0 = gEnvHdrTex.SampleLevel(equirectSampler, float3(uv, 0.0), mip).rgb;
    if ((showFlags & 2u) != 0u) {
        s0 = lerp(s0, s0.bgr * float3(1.15, 0.75, 1.25), 0.35);
    }
    return s0;
}

float3 sky(float3 d) {
    float3 dn = norm(d);
    if (isDawnFinale()) {
        float dawnT = timeRamp(336.0, 346.0);
        float3 sDir = sceneSunDir();
        float horiz = pow(saturate(1.0 - max(dn.y, 0.0)), 3.2);
        float sunDot = saturate(dot(dn, sDir));
        float3 zenithCol = lerp(float3(0.025, 0.048, 0.110), float3(0.20, 0.34, 0.58), dawnT);
        float3 horizWarm = lerp(float3(0.08, 0.13, 0.22), float3(0.98, 0.62, 0.38), dawnT);
        float3 horizCool = lerp(float3(0.05, 0.09, 0.16), float3(0.48, 0.46, 0.62), dawnT);
        float azim = saturate(dot(normalize(float2(dn.x, dn.z) + 1e-5), normalize(float2(sDir.x, sDir.z))));
        float3 clearDawn = lerp(zenithCol, lerp(horizCool, horizWarm, pow(azim, 1.4)), horiz);
        float3 basinMist = lerp(float3(0.05, 0.08, 0.14), float3(0.56, 0.48, 0.54), dawnT);
        clearDawn = lerp(clearDawn, basinMist, saturate(-dn.y * 5.0));
        float sunDisc = pow(sunDot, 260.0) * 5.2 * dawnT;
        float sunGlow = pow(sunDot, 12.0) * 0.52 * dawnT;
        float2 tuv = dirToEquirectUv(dn);
        float3 neural = tensorField.SampleLevel(linearClamp, tuv, 0.0).rgb;
        float nLum = dot(neural, float3(0.2126, 0.7152, 0.0722));
        float cloudBand = smoothstep(0.02, 0.28, dn.y) * (1.0 - smoothstep(0.36, 0.82, dn.y));
        return clearDawn
             + float3(1.0, 0.74, 0.48) * (sunDisc + sunGlow)
             + float3(0.24, 0.16, 0.12) * (nLum * cloudBand * 0.45 * dawnT);
    }

    float horizon = pow(saturate(1.0 - abs(dn.y)), 3.6);
    float urbanGlow = pow(saturate(1.0 - max(dn.y + 0.04, 0.0) * 3.8), 3.0);

    float3 nightZenith = (act == 2u) ? float3(0.004, 0.009, 0.022) : float3(0.006, 0.013, 0.030);
    float3 nightHorizon = (act == 2u) ? float3(0.028, 0.052, 0.078) : float3(0.038, 0.082, 0.128);
    if ((showFlags & 2u) != 0u) {
        nightZenith = float3(0.010, 0.004, 0.024);
        nightHorizon = float3(0.058, 0.022, 0.072);
    }
    float3 night = lerp(nightZenith, nightHorizon, horizon) + float3(0.016, 0.012, 0.008) * urbanGlow;

    float twilight = 4.0 * daylight * (1.0 - daylight);
    float3 dusk = lerp(float3(0.035, 0.068, 0.165), float3(0.68, 0.28, 0.22), horizon);
    float3 day = lerp(float3(0.12, 0.29, 0.52), float3(0.92, 0.54, 0.24), horizon);
    float3 clearSky = lerp(lerp(night, day, daylight), dusk, twilight * 0.55);

    float3 sunDir = sunOverridden() ? sceneSunDir() : norm(float3(-0.48, 0.16 + 0.22 * daylight, 0.72));
    float sun = pow(saturate(dot(dn, sunDir)), 180.0);
    float sunHalo = pow(saturate(dot(dn, sunDir)), 10.0);
    float3 c = clearSky + (sun * float3(3.2, 1.9, 0.85) + sunHalo * float3(0.32, 0.16, 0.06)) * daylight;

    float2 uv = dirToEquirectUv(dn);
    float3 neural = tensorField.SampleLevel(linearClamp, uv, 0.0).rgb;
    float nLum = dot(neural, float3(0.2126, 0.7152, 0.0722));
    float highCloud = smoothstep(0.04, 0.34, dn.y) * (1.0 - smoothstep(0.45, 0.88, dn.y));
    c += lerp(float3(0.018, 0.034, 0.052), float3(0.18, 0.12, 0.07), daylight) * nLum * (0.28 + 0.55 * daylight) * highCloud;

    if (showFlags == 0u) {
        float mist = 1.0 - smoothstep(0.0, 2.0, min(abs(time - 60.0), abs(time - 120.0)));
        c = lerp(c, float3(0.28, 0.33, 0.36), mist);
    }
    return c;
}

float3 emissive(uint m, float3 p, float filterW) {
    float nightFactor = smoothstep(0.68, 0.08, daylight);
    if (isDawnFinale()) {
        // Warm subtle architectural halo/sconce glow in bathroom even at dawn; exterior city lights fade gently
        if (m == 15u && isInsideDawnBathroom(p)) {
            return lampsOff() ? (float3)0.0 : float3(0.92, 0.68, 0.42) * 0.52;
        }
        nightFactor *= 0.22;
    }
    if (m == 6u) {
        return float3(0.05, 0.86, 0.66) * (0.62 + 0.18 * pulse) * (0.08 + 0.92 * nightFactor);
    }
    if (m == 15u) {
        return float3(0.56, 0.36, 0.16) * (0.08 + 0.92 * nightFactor) * (1.0 + 0.08 * pulse);
    }
    if (m == 16u) {
        return float3(1.25, 0.64, 0.18) * (0.04 + 0.96 * nightFactor);
    }
    if (m == 5u || m == 13u || m == 14u) {
        float floorH = (m == 5u) ? 0.34 : 0.31;
        float bayW   = (m == 5u) ? 0.25 : 0.28;
        float fy = frac(p.y / floorH);
        float flr = floor(p.y / floorH);
        float perim = p.x * 1.07 + p.z * 1.13;
        float fx = frac(perim / bayW);
        float bay = floor(perim / bayW);
        float bldgId = floor(p.x * 0.12) * 17.0 + floor(p.z * 0.12) * 31.0;

        float mechFloor = (m == 5u) ? step(0.13, frac(flr / 8.0)) : step(0.09, frac((flr + bldgId) / 12.0));
        float rRoom  = hash21(float2(flr * 1.37 + bldgId, bay * 2.19));
        float rFloor = hash21(float2(flr * 0.73, bldgId + 5.1));
        float occThreshold = (m == 13u) ? 0.48 : ((m == 5u) ? 0.52 : 0.56);
        float litSharp = step(occThreshold, rRoom * 0.72 + rFloor * 0.28) * mechFloor;
        float ceilGrad = 0.55 + 0.45 * fy;
        float dimVar = 0.42 + 0.58 * hash21(float2(bay * 3.1, flr * 1.7));

        float softEdge = clamp(filterW * 0.35, 0.0, 0.22);
        float paneSoft = smoothstep(0.16 - softEdge, 0.24 + softEdge, fy) * (1.0 - smoothstep(0.78 - softEdge, 0.86 + softEdge, fy))
                       * smoothstep(0.12 - softEdge, 0.20 + softEdge, fx) * (1.0 - smoothstep(0.80 - softEdge, 0.88 + softEdge, fx));
        float lodBlend = smoothstep(0.35, 1.25, filterW);
        float winFactor = lerp(paneSoft * litSharp * ceilGrad * dimVar, 0.22 * (0.55 + 0.45 * rFloor) * mechFloor, lodBlend);

        float3 warmTint = float3(0.96, 0.68, 0.36);
        float3 ivoryTint = float3(0.86, 0.88, 0.80);
        float3 coolTint = float3(0.46, 0.74, 0.88);
        float3 winCol = (m == 13u) ? lerp(warmTint, ivoryTint, rRoom * 0.45)
                      : ((m == 5u) ? lerp(ivoryTint, float3(0.42, 0.84, 0.74), 0.42 + 0.30 * rRoom)
                                   : lerp(coolTint, ivoryTint, rRoom * 0.55));
        float intensity = (m == 5u) ? 0.70 : 0.58;
        return winCol * (winFactor * intensity * nightFactor);
    }
    if (m == 21u) {
        float2 tuv = frac(p.xz * 0.095 + float2(time * 0.028, -time * 0.018));
        float3 nSample = tensorField.SampleLevel(linearClamp, tuv, 0.0).rgb;
        float3 accent = ((showFlags & 2u) != 0u) ? float3(1.15, 0.42, 1.05) : float3(0.35, 1.15, 0.96);
        return nSample * accent * (0.42 + 0.12 * pulse);
    }
    if (m == 22u) {
        float3 floraTint = lerp(float3(0.08, 0.88, 0.64), float3(0.96, 0.62, 0.20), clamp(moodAccent, 0.0, 1.0));
        if ((showFlags & 2u) != 0u) floraTint = lerp(floraTint, float3(0.88, 0.24, 0.74), 0.45);
        return floraTint * (0.045 + 0.05 * moodBloom + 0.025 * pulse) * (0.25 + 0.75 * nightFactor);
    }
    return (float3)0.0;
}

float3 evalGgxDirect(float3 n, float3 v, float3 l, float3 base, float rough, float metallic, float clearcoat) {
    float nl = saturate(dot(n, l));
    if (nl <= 1e-4) return (float3)0.0;
    float nv = max(dot(n, v), 1e-3);
    float3 h = norm(v + l);
    float nh = saturate(dot(n, h));
    float vh = saturate(dot(v, h));

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), base, metallic);
    float3 F = f0 + (1.0 - f0) * pow(1.0 - vh, 5.0);

    float alpha = max(rough * rough, 0.0012);
    float a2 = alpha * alpha;
    float denom = nh * nh * (a2 - 1.0) + 1.0;
    float D = a2 / (kPi * denom * denom);

    float k = (rough + 1.0) * (rough + 1.0) * 0.125;
    float G = (nv / (nv * (1.0 - k) + k)) * (nl / (nl * (1.0 - k) + k));
    float3 spec = min(D * G * F / max(4.0 * nv * nl, 0.004), 14.0.xxx);

    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 diff = kd * base / kPi;

    float3 coatSpec = (float3)0.0;
    if (clearcoat > 0.02) {
        float cAlpha = 0.0036;
        float ca2 = cAlpha * cAlpha;
        float cDen = nh * nh * (ca2 - 1.0) + 1.0;
        float cD = ca2 / (kPi * cDen * cDen);
        float cF = 0.04 + 0.96 * pow(1.0 - vh, 5.0);
        float cK = 0.14;
        float cG = (nv / (nv * (1.0 - cK) + cK)) * (nl / (nl * (1.0 - cK) + cK));
        coatSpec = min(cD * cG * cF / max(4.0 * nv * nl, 0.006), 12.0) * clearcoat * float3(1.0, 1.0, 1.0);
        diff *= (1.0 - 0.35 * clearcoat * cF);
    }

    return (diff + spec + coatSpec) * nl;
}

float2 envBrdfApprox(float rough, float nv) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * nv)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

float clearcoatForMaterial(uint m, float aux, float wetPuddle) {
    if (m == 1u)  return 0.85 * wetPuddle;
    if (m == 11u || m == 20u || m == 25u || m == 27u || m == 28u || m == 37u) return aux;
    if (m == 31u) return 0.32 * aux;
    return 0.0;
}

float translucencyForMaterial(uint m, float aux) {
    if (m == 10u || m == 20u || m == 22u || m == 29u || m == 30u || m == 33u || m == 36u) return aux;
    return 0.0;
}

void buildOrthonormalBasis(float3 n, out float3 b1, out float3 b2) {
    float3 upRef = (abs(n.y) < 0.94) ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    b1 = norm(cross(upRef, n));
    b2 = cross(n, b1);
}

#if defined(M2_RT)
bool traceShadow(float3 origin, float3 direction, float distance) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.016;
    ray.TMax = distance;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01u, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            uint cOff = q.CandidateInstanceID() + q.CandidatePrimitiveIndex() * 3u;
            Vertex cv0 = vertices[indices[cOff]];
            Vertex cv1 = vertices[indices[cOff + 1u]];
            Vertex cv2 = vertices[indices[cOff + 2u]];
            float2 cb = q.CandidateTriangleBarycentrics();
            float2 cuv = cv0.uv.xy + cb.x * (cv1.uv.xy - cv0.uv.xy) + cb.y * (cv2.uv.xy - cv0.uv.xy);
            uint cm = min((uint)cv0.p.w, 41u);
            float ca = gAlbedoAlphaTex.SampleLevel(anisoWrap, float3(cuv, (float)cm), 1.0).a;
            if (ca >= 0.44) {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float traceAmbientRay(float3 origin, float3 direction, float maxDist) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.008;
    ray.TMax = maxDist;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_FORCE_OPAQUE, 0x01u, ray);
    q.Proceed();
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        return saturate(1.0 - q.CommittedRayT() / maxDist);
    }
    return 0.0;
}

void traceBathroomGiRay(float3 origin, float3 direction, out float3 giRad, out float occWeight) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.006;
    ray.TMax = 3.85;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_FORCE_OPAQUE, 0x01u, ray);
    q.Proceed();

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        float hitT = q.CommittedRayT();
        float3 hitP = origin + direction * hitT;
        uint offset = q.CommittedInstanceID() + q.CommittedPrimitiveIndex() * 3u;
        Vertex v0 = vertices[indices[offset]];
        Vertex v1 = vertices[indices[offset + 1u]];
        Vertex v2 = vertices[indices[offset + 2u]];
        uint hm = min((uint)v0.p.w, 41u);
        float2 bary = q.CommittedTriangleBarycentrics();
        float2 hitUv = v0.uv.xy + bary.x * (v1.uv.xy - v0.uv.xy) + bary.y * (v2.uv.xy - v0.uv.xy);
        float3 hitAlb = gAlbedoAlphaTex.SampleLevel(anisoWrap, float3(hitUv, (float)hm), 4.5).rgb;

        // Rays that pass out through the corner window opening onto the terrace or skyline gather outdoor portal light
        if (!isInsideDawnBathroom(hitP)) {
            giRad = hitAlb * float3(0.92, 0.84, 0.76) * 0.72 + sky(direction) * 0.28;
            occWeight = 0.0;
            return;
        }

        float wMicro = 1.0 - smoothstep(0.004, 0.14, hitT);
        float wMeso  = 1.0 - smoothstep(0.020, 0.92, hitT);
        occWeight = 0.68 * wMicro + 0.32 * wMeso;

        float3 hitObjN = v0.n.xyz + bary.x * (v1.n.xyz - v0.n.xyz) + bary.y * (v2.n.xyz - v0.n.xyz);
        float3 hitWN = norm(mul(hitObjN, (float3x3)q.CommittedWorldToObject3x4()));
        if (dot(hitWN, direction) > 0.0) hitWN = -hitWN;

        float dawnRamp = timeRamp(336.0, 346.0);
        float3 sDir = sceneSunDir();
        float3 sRad = sceneSunRadiance();
        float sunNdotL = saturate(dot(hitWN, sDir));
        // Analytical window-aperture projection estimates sunlit floor/vanity/wall patch for warm 2nd-bounce color bleeding
        float tWin = (1.62 - hitP.x) / max(sDir.x, 0.08);
        float3 winHit = hitP + sDir * max(tWin, 0.0);
        float inWinPatch = (tWin > 0.0)
            ? (smoothstep(2.90, 3.25, winHit.y) * (1.0 - smoothstep(5.45, 5.75, winHit.y)) *
               smoothstep(2.75, 3.20, winHit.z) * (1.0 - smoothstep(6.35, 6.60, winHit.z)))
            : 0.0;
        float winProx = smoothstep(-1.25, 1.40, hitP.x) * 0.56 + smoothstep(2.8, 6.48, hitP.z) * 0.44;
        float3 skyBounce = lerp(float3(0.16, 0.16, 0.17), float3(0.92, 0.80, 0.68), winProx * dawnRamp);
        float3 sunBounce = sRad * (sunNdotL * inWinPatch * 0.42);
        float3 pracBounce = lampsOff() ? (float3)0.0 : (float3(0.42, 0.28, 0.15) / (1.0 + 0.65 * dot(hitP - float3(0.08, 4.44, 2.32), hitP - float3(0.08, 4.44, 2.32))));
        giRad = hitAlb * (skyBounce + sunBounce + pracBounce) * smoothstep(0.004, 0.080, hitT);
        if (hm == 15u && !lampsOff()) {
            giRad += float3(0.95, 0.66, 0.36) * 0.78;
        }
        return;
    }

    // Ray escaped cleanly through the penthouse corner window portal into the Taipei dawn sky!
    giRad = sky(direction) * 0.80;
    occWeight = 0.0;
}

bool traceSceneEx(float3 origin, float3 direction, float distance, uint mask, float coneSpread, out float3 color, out float outHitT) {
    outHitT = distance;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.014;
    ray.TMax = distance;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    uint dynFlags = (coneSpread >= 0.045) ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_NONE;
    q.TraceRayInline(scene, dynFlags, mask, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            uint cOff = q.CandidateInstanceID() + q.CandidatePrimitiveIndex() * 3u;
            Vertex cv0 = vertices[indices[cOff]];
            Vertex cv1 = vertices[indices[cOff + 1u]];
            Vertex cv2 = vertices[indices[cOff + 2u]];
            float2 cb = q.CandidateTriangleBarycentrics();
            float2 cuv = cv0.uv.xy + cb.x * (cv1.uv.xy - cv0.uv.xy) + cb.y * (cv2.uv.xy - cv0.uv.xy);
            uint cm = min((uint)cv0.p.w, 41u);
            float ca = gAlbedoAlphaTex.SampleLevel(anisoWrap, float3(cuv, (float)cm), 1.0).a;
            if (ca >= 0.44) {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        uint offset = q.CommittedInstanceID() + q.CommittedPrimitiveIndex() * 3u;
        Vertex v0 = vertices[indices[offset]];
        Vertex v1 = vertices[indices[offset + 1u]];
        Vertex v2 = vertices[indices[offset + 2u]];
        uint hm = min((uint)v0.p.w, 41u);
        float2 bary = q.CommittedTriangleBarycentrics();

        float3 objN = v0.n.xyz + bary.x * (v1.n.xyz - v0.n.xyz) + bary.y * (v2.n.xyz - v0.n.xyz);
        float3 objT = v0.tangent.xyz + bary.x * (v1.tangent.xyz - v0.tangent.xyz) + bary.y * (v2.tangent.xyz - v0.tangent.xyz);
        float2 hitUv = v0.uv.xy + bary.x * (v1.uv.xy - v0.uv.xy) + bary.y * (v2.uv.xy - v0.uv.xy);

        float3x3 w2o = (float3x3)q.CommittedWorldToObject3x4();
        float3x3 o2w = (float3x3)q.CommittedObjectToWorld3x4();
        float3 hitNGeom = norm(mul(objN, w2o));
        bool hitFront = dot(hitNGeom, direction) < 0.0;
        if (!hitFront) hitNGeom = -hitNGeom;

        float hitT = q.CommittedRayT();
        outHitT = hitT;
        float3 p = origin + direction * hitT;
        bool inBath = isInsideDawnBathroom(p);

        float mipLod = clamp(log2(max(coneSpread * hitT * 512.0, 1.0)), 0.0, 8.0);
        float3 hCoord = float3(hitUv, (float)hm);
        float3 hBase  = gAlbedoAlphaTex.SampleLevel(anisoWrap, hCoord, mipLod).rgb;
        float2 hNorXY = gNormalTex.SampleLevel(anisoWrap, hCoord, mipLod).xy * 2.0 - 1.0;
        float4 hOrm   = gOrmtTex.SampleLevel(anisoWrap, hCoord, mipLod);

        float3 wT = mul(o2w, objT);
        wT = wT - hitNGeom * dot(hitNGeom, wT);
        float wt2 = dot(wT, wT);
        float3 T = (wt2 > 1e-8)
            ? (wT * rsqrt(wt2))
            : norm(abs(hitNGeom.y) < 0.99 ? cross(float3(0.0, 1.0, 0.0), hitNGeom) : cross(float3(1.0, 0.0, 0.0), hitNGeom));
        float bSign = (hitFront ? 1.0 : -1.0) * ((v0.tangent.w >= 0.0) ? 1.0 : -1.0);
        float3 B = cross(hitNGeom, T) * bSign;

        float hNormScale = (hm == 34u || hm == 40u) ? 0.04 : saturate(1.0 - mipLod * 0.12);
        hNorXY *= hNormScale;
        float hNz = sqrt(saturate(1.0 - dot(hNorXY, hNorXY)));
        float3 hitN = norm(T * hNorXY.x + B * hNorXY.y + hitNGeom * hNz);

        float wetP = (hm == 1u) ? puddleMask(p.xz) : 0.0;
        float hRough = clamp(hOrm.y, 0.03, 0.98);
        if (hm == 1u) hRough = lerp(hRough, 0.08, wetP);
        float hMetal = saturate(hOrm.z);
        float hCoat  = clearcoatForMaterial(hm, hOrm.w, wetP);
        float hTrans = translucencyForMaterial(hm, hOrm.w);

        if (hm == 5u || hm == 13u || hm == 14u) {
            float floorH = (hm == 5u) ? 0.34 : 0.38;
            float bayW   = (hm == 5u) ? 0.25 : 0.32;
            float fy = frac(p.y / floorH);
            float fx = frac((p.x * 1.07 + p.z * 1.13) / bayW);
            float cw = smoothstep(0.12, 0.20, fy) * (1.0 - smoothstep(0.82, 0.90, fy))
                     * smoothstep(0.08, 0.15, fx) * (1.0 - smoothstep(0.85, 0.92, fx));
            hBase = lerp(hBase * 0.45, hBase, cw);
        }

        float3 V2 = -direction;
        float nv2 = max(dot(hitN, V2), 1e-3);
        float3 sunDir = sceneSunDir();
        float3 sunRad = sceneSunRadiance();

        // Sharp mirrors/glass/water evaluate secondary sun shadow so reflected rooms display real window sunbeams!
        float sunVis2 = 1.0;
        float secShadowConeMax = (accumulating() && accumFrame >= 2u) ? 0.0035 : 0.055;
        if (coneSpread < secShadowConeMax && (dot(hitNGeom, sunDir) > 0.0 || hTrans > 0.2)) {
            float3 shOrig = p + hitNGeom * 0.018 + ((hm == 29u || hm == 36u) ? sunDir * 0.020 : 0.0.xxx);
            sunVis2 = traceShadow(shOrig, sunDir, inBath ? 16.0 : 22.0) ? 0.0 : 1.0;
        }

        float3 direct2 = evalGgxDirect(hitN, V2, sunDir, hBase, hRough, hMetal, hCoat) * sunRad * sunVis2;
        if (hTrans > 0.05) {
            float back2 = saturate(dot(-hitN, sunDir) * 0.65 + 0.35);
            direct2 += hBase * sunRad * (back2 * hTrans * (0.30 + 0.70 * sunVis2) * 0.48);
        }

        float3 f0_2 = lerp(float3(0.04, 0.04, 0.04), hBase, hMetal);
        float2 brdf2 = envBrdfApprox(hRough, nv2);
        float3 diffEnv2 = sampleIblEnv(hitN, 0.92, inBath);
        float3 specEnv2 = sampleIblEnv(reflect(-V2, hitN), hRough, inBath);

        if (inBath) {
            // Balanced window portal irradiance + warm opal sconce so reflected wainscot slats & shower tiles have crisp depth
            float3 toWin = float3(1.48, 4.25, 4.90) - p;
            float winDist = length(toWin);
            float3 winL = toWin / max(winDist, 0.25);
            float winDay = sunOverridden() ? (0.03 + 0.97 * timeRamp(336.0, 345.0)) : 1.0;
            float winFall = winDay / (1.0 + 0.18 * winDist * winDist);
            diffEnv2 = diffEnv2 * 0.34 + float3(0.58, 0.56, 0.54) * (0.28 + 0.72 * saturate(dot(hitN, winL))) * winFall;
            if (!lampsOff()) {
                float sconceBoost = sunOverridden() ? lerp(2.6, 1.0, timeRamp(336.0, 345.0)) : 1.0;
                float3 toSconce = float3(0.08, 4.44, 2.32) - p;
                float dSc = length(toSconce);
                diffEnv2 += float3(0.95, 0.64, 0.34) * sconceBoost * saturate(dot(hitN, toSconce / max(dSc, 0.15)) * 0.6 + 0.4) / (1.0 + 2.4 * dSc * dSc);
                float3 toVanityLamp = float3(-0.18, 5.16, 5.88) - p;
                float dVl = length(toVanityLamp);
                if (sunOverridden()) {
                    diffEnv2 += float3(0.88, 0.58, 0.30) * (sconceBoost - 0.85) * saturate(dot(hitN, toVanityLamp / max(dVl, 0.15))) / (1.0 + 1.6 * dVl * dVl);
                }
            }
        }

        float3 kd2 = (1.0 - f0_2) * (1.0 - hMetal);
        float3 ibl2 = (kd2 * hBase * diffEnv2 + specEnv2 * (f0_2 * brdf2.x + brdf2.y)) * hOrm.x;

        if (hm == 3u || hm == 4u || hm == 18u || hm == 35u || hm == 40u) {
            float fres2 = 0.04 + 0.96 * pow(1.0 - nv2, 5.0);
            float3 trans2 = sampleIblEnv(direction, 0.12, inBath) * hBase;
            color = lerp(trans2, specEnv2, clamp(fres2, 0.06, 0.88)) + direct2;
            return true;
        }

        float3 hitCol = direct2 + ibl2 + emissive(hm, p, coneSpread * hitT);
        if (!inBath) {
            float hFog = 1.0 - exp(-max(hitT - 4.5, 0.0) * (isDawnFinale() ? 0.0028 : 0.0055));
            hitCol = lerp(hitCol, sky(direction), hFog * 0.72);
        }
        color = hitCol;
        return true;
    }

    color = isInsideDawnBathroom(origin) ? (sky(direction) * 0.85) : sky(direction);
    return false;
}

bool traceScene(float3 origin, float3 direction, float distance, uint mask, float coneSpread, out float3 color) {
    float dummyT = distance;
    return traceSceneEx(origin, direction, distance, mask, coneSpread, color, dummyT);
}

bool traceMirrorScene(float3 origin, float3 direction, out float3 color, out uint extraQueries) {
    extraQueries = 1u;
    float opqT = 180.0;
    bool hitOpq = traceSceneEx(origin, direction, 180.0, 0x01u, 0.0015, color, opqT);

    // Check thin-glass sheets (shower partition, gallery frame glass, window panes) in front of the opaque hit!
    RayDesc gRay;
    gRay.Origin = origin;
    gRay.Direction = direction;
    gRay.TMin = 0.014;
    gRay.TMax = max(opqT - 0.006, 0.015);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> gq;
    gq.TraceRayInline(scene, RAY_FLAG_NONE, 0x02u, gRay);
    while (gq.Proceed()) {}
    ++extraQueries;

    if (gq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        uint gOff = gq.CommittedInstanceID() + gq.CommittedPrimitiveIndex() * 3u;
        Vertex gv0 = vertices[indices[gOff]];
        Vertex gv1 = vertices[indices[gOff + 1u]];
        Vertex gv2 = vertices[indices[gOff + 2u]];
        uint gm = min((uint)gv0.p.w, 41u);
        float2 gb = gq.CommittedTriangleBarycentrics();
        float3 gObjN = gv0.n.xyz + gb.x * (gv1.n.xyz - gv0.n.xyz) + gb.y * (gv2.n.xyz - gv0.n.xyz);
        float3x3 gw2o = (float3x3)gq.CommittedWorldToObject3x4();
        float3 gN = norm(mul(gObjN, gw2o));
        if (dot(gN, direction) > 0.0) gN = -gN;

        float gNv = saturate(dot(gN, -direction));
        float gFres = clamp((0.04 + 0.96 * pow(1.0 - gNv, 5.0)) * 1.25, 0.055, 0.68);
        float3 gTint = (gm == 35u) ? float3(0.72, 0.32, 0.08)
                     : ((gm == 4u) ? float3(0.84, 0.96, 0.90) : float3(0.955, 0.982, 0.972));
        bool mirBath = isInsideDawnBathroom(origin);
        float3 gSpec = sampleIblEnv(reflect(direction, gN), 0.035, mirBath) * 0.68;
        float3 sDir = sceneSunDir();
        gSpec += pow(saturate(dot(gN, norm(-direction + sDir))), 180.0) * sceneSunRadiance() * 0.65;
        color = lerp(color * gTint, gSpec, gFres);
        return true;
    }
    return hitOpq;
}
#endif


// =============================================================================
// Borrowed NVIDIA RTXDI Reservoir Resampling (ReSTIR DI — Weighted RIS)
// Evaluates dynamic practical emitters with 1 unbiased DXR visibility shadow ray.
// =============================================================================
struct RTXDI_DIReservoir {
    uint  lightIndex;
    uint  valid;
    float weightSum;
    float targetPdf;
    float M;
};

RTXDI_DIReservoir RTXDI_EmptyDIReservoir() {
    RTXDI_DIReservoir r;
    r.lightIndex = 0u;
    r.valid = 0u;
    r.weightSum = 0.0;
    r.targetPdf = 0.0;
    r.M = 0.0;
    return r;
}

void RTXDI_StreamLight(inout RTXDI_DIReservoir r, uint candidateIdx, float candidateTargetPdf, float rnd01) {
    r.M += 1.0;
    r.weightSum += candidateTargetPdf;
    if (candidateTargetPdf > 0.0 && (rnd01 * r.weightSum < candidateTargetPdf)) {
        r.lightIndex = candidateIdx;
        r.targetPdf = candidateTargetPdf;
        r.valid = 1u;
    }
}

void getRtxdiEmitter(uint idx, float3 worldP, bool inBath, out float3 lightPos, out float3 lightIntensity, out float lightRadius) {
    if (inBath) {
        float lampGate = lampsOff() ? 0.0 : (sunOverridden() ? lerp(3.40, 1.0, timeRamp(336.0, 345.0)) : 1.0);
        if (idx == 0u) { // Recessed warm 2700K architectural ceiling vanity downlight above porcelain basin
            lightPos = float3(-0.18, 5.18, 5.82);
            lightIntensity = float3(1.55, 1.02, 0.56) * lampGate;
            lightRadius = 0.065;
        } else if (idx == 1u) { // Backlit vanity mirror perimeter halo (2900K)
            lightPos = float3(clamp(worldP.x, -0.54, 0.18), clamp(worldP.y, 3.85, 4.92), 6.48);
            lightIntensity = float3(0.72, 0.50, 0.28) * lampGate;
            lightRadius = 0.048;
        } else if (idx == 2u) { // Opal cylindrical wall sconce (2700K) & warm under-cabinet wash
            lightPos = (worldP.z < 4.2) ? float3(0.08, 4.44, 2.32) : float3(clamp(worldP.x, -0.82, 0.44), 3.04, 5.84);
            lightIntensity = float3(1.05, 0.70, 0.38) * lampGate;
            lightRadius = 0.050;
        } else { // Keepsake Glass Orchid bioluminescent labellum core in celadon vase
            lightPos = float3(0.402, 3.80, 6.235);
            lightIntensity = float3(0.28, 0.88, 0.72) * (0.68 + 0.32 * pulse) * lampGate;
            lightRadius = 0.032;
        }
    } else {
        float lampGate = lampsOff() ? 0.0 : 1.0;
        if (idx == 0u) { // Hero Glass Orchid bioluminescent core
            lightPos = float3(0.0, 4.55, 5.20);
            lightIntensity = float3(0.45, 1.35, 1.05) * (0.45 + 0.55 * reveal) * lampGate;
            lightRadius = 0.12;
        } else if (idx == 1u) { // Sanctuary warm left brass specimen lamp
            lightPos = float3(-2.65, 5.85, 3.80);
            lightIntensity = float3(1.85, 1.28, 0.74) * lampGate;
            lightRadius = 0.10;
        } else if (idx == 2u) { // Sanctuary warm right brass specimen lamp
            lightPos = float3(2.65, 5.85, 3.80);
            lightIntensity = float3(1.85, 1.28, 0.74) * lampGate;
            lightRadius = 0.10;
        } else { // Enamel Cathedral / Plaza warm uplight
            lightPos = float3(0.0, 2.95, 6.10);
            lightIntensity = float3(1.40, 1.02, 0.66) * lampGate;
            lightRadius = 0.12;
        }
    }
}

struct VOut {
    float4 pos         : SV_Position;
    float3 world       : TEXCOORD0;
    float3 normal      : TEXCOORD1;
    float4 uv          : TEXCOORD2;
    nointerpolation uint mat : TEXCOORD3;
    float4 tangentSign : TEXCOORD4;
};

VOut VSMain(uint id : SV_VertexID) {
    Vertex v = vertices[indices[firstIndex + id]];
    VOut o;
    o.world = transformPoint(v.p.xyz);
    o.normal = transformNormal(v.n.xyz);
    o.uv = v.uv;
    o.mat = (uint)v.p.w;
    o.tangentSign = float4(transformNormal(v.tangent.xyz), v.tangent.w);
    float3 d = o.world - eye;
    float z = dot(d, forward);
    float fy = 1.0 / tan(fov * 0.5);
    o.pos = float4(dot(d, right) * fy / aspect, dot(d, up) * fy, z * 1000.0 / 999.9 - 0.1 * 1000.0 / 999.9, z);
    o.pos.xy += jitterNdc * o.pos.w;
    return o;
}

struct Targets {
    float4 color   : SV_Target0;
    float distance : SV_Target1;
};

#if defined(BOO_DEPTH_CUTOUT_PS)
void PSDepthCutout(VOut i) {
    uint matId = min(i.mat, 41u);
    float alpha = gAlbedoAlphaTex.Sample(anisoWrap, float3(i.uv.xy, (float)matId)).a;
    if (alpha < 0.44) {
        discard;
    }
}
#else
[earlydepthstencil]
Targets PSMain(VOut i, bool front : SV_IsFrontFace) {
    uint matId = min(i.mat, 41u);
    float3 texCoord = float3(i.uv.xy, (float)matId);

    // Unconditional anisotropic texture-array fetches (strictly uniform before any cutout discard)
    float4 albSample = gAlbedoAlphaTex.Sample(anisoWrap, texCoord);
    float2 norSample = gNormalTex.Sample(anisoWrap, texCoord).xy;
    float4 ormSample = gOrmtTex.Sample(anisoWrap, texCoord);

#if defined(BOO_ALPHA_CUTOUT)
    if (albSample.a < 0.44) {
        discard;
    }
#endif

    float3 nGeom = norm(front ? i.normal : -i.normal);
    float3 v = norm(eye - i.world);
    float dist = length(i.world - eye);
    bool inBath = isInsideDawnBathroom(i.world);

    // Orthonormal MikkTSpace TBN reconstruction
    float3 tProj = i.tangentSign.xyz - nGeom * dot(nGeom, i.tangentSign.xyz);
    float tLen2 = dot(tProj, tProj);
    float3 T = (tLen2 > 1e-8)
        ? (tProj * rsqrt(tLen2))
        : norm(abs(nGeom.y) < 0.99 ? cross(float3(0.0, 1.0, 0.0), nGeom) : cross(float3(1.0, 0.0, 0.0), nGeom));
    float bSign = (front ? 1.0 : -1.0) * ((i.tangentSign.w >= 0.0) ? 1.0 : -1.0);
    float3 B = cross(nGeom, T) * bSign;

    float2 nxy = norSample * 2.0 - 1.0;
    float normFade = 1.0 / (1.0 + dist * 0.016);
    if (matId == 34u || matId == 40u) normFade *= 0.03; // Optical silvered mirror & architectural float glass
    nxy *= normFade;
    float nz = sqrt(saturate(1.0 - dot(nxy, nxy)));
    float3 n = norm(T * nxy.x + B * nxy.y + nGeom * nz);

    float wetPuddle = (matId == 1u) ? puddleMask(i.world.xz) : 0.0;
    float3 base = albSample.rgb;
    float texAO = ormSample.x;
    float rough = clamp(ormSample.y, 0.020, 0.98);
    float metallic = saturate(ormSample.z);
    float auxVal = ormSample.w;

    if (matId == 1u) {
        rough = lerp(rough, 0.065, wetPuddle);
        base *= lerp(1.0, 0.58, wetPuddle);
    }
    if (matId == 7u || (matId == 1u && wetPuddle > 0.15)) {
        float baseRip = (matId == 7u) ? 0.010 : (0.005 * wetPuddle);
        float ripScale = baseRip / (1.0 + dist * 0.022);
        float wx = sin(i.world.z * 5.7 + i.world.x * 2.3 + time * 2.4) * 0.55
                 + sin(i.world.z * 12.3 - i.world.x * 6.1 - time * 1.9) * 0.30
                 + cos(i.world.x * 8.9 + i.world.z * 3.4 + time * 1.3) * 0.15;
        float wz = cos(i.world.x * 6.4 - i.world.z * 2.8 - time * 2.1) * 0.55
                 + cos(i.world.z * 9.8 + i.world.x * 5.2 + time * 1.7) * 0.30
                 + sin(i.world.x * 13.7 + time * 2.7) * 0.15;
        n = norm(n + float3(wx * ripScale, 0.0, wz * ripScale));
    }

    float cwPane = 1.0;
    if (matId == 5u || matId == 13u || matId == 14u) {
        float floorH = (matId == 5u) ? 0.34 : 0.38;
        float bayW   = (matId == 5u) ? 0.25 : 0.32;
        float fy = frac(i.world.y / floorH);
        float fx = frac((i.world.x * 1.07 + i.world.z * 1.13) / bayW);
        cwPane = smoothstep(0.12, 0.20, fy) * (1.0 - smoothstep(0.82, 0.90, fy))
               * smoothstep(0.08, 0.15, fx) * (1.0 - smoothstep(0.85, 0.92, fx));
        float panelVar = 0.88 + 0.24 * hash21(floor(float2((i.world.x + i.world.z) / bayW, i.world.y / floorH)));
        base = lerp(base * 0.42, base * panelVar, cwPane);
        rough = lerp(0.38, rough, cwPane);
    }

    bool isGlass = (matId == 3u || matId == 4u || matId == 18u || matId == 35u || matId == 40u);
    float clearcoat = clearcoatForMaterial(matId, auxVal, wetPuddle);
    float translucency = translucencyForMaterial(matId, auxVal);

    float ndotv = saturate(dot(n, v));
    float fres = 0.04 + 0.96 * pow(1.0 - ndotv, 5.0);
    float3 sunDir = sceneSunDir();
    float3 sunRad = sceneSunRadiance();
    float3 rimDir = isDawnFinale() ? norm(float3(-0.62, 0.42, -0.66)) : norm(float3(0.58, 0.28, 0.76));
    float3 upSanctuaryDir = norm(float3(0.08, -0.86, -0.48));

    bool isWaterDroplet = (inBath && matId == 3u && part >= 230u);
    float ior = isWaterDroplet ? 1.333 : ((matId == 35u) ? 1.52 : 1.495);
    float refrBend = (matId == 40u) ? 0.0 : (isWaterDroplet ? 0.36 : 0.42);
    float3 refrDir = norm(lerp(-v, refract(-v, n, 1.0 / ior), refrBend));

    float sunVis = 1.0;
    float rtContactAO = 1.0;
    float3 reflected = sampleIblEnv(reflect(-v, n), rough, inBath);
    float3 transmitted = (inBath && matId != 40u) ? sampleIblEnv(refrDir, 0.06, true) : sky(refrDir);
    bool hasTracedReflection = false;
    bool hasTracedBathGi = false;
    float3 tracedBathGi = (float3)0.0;
    float3 restirDirect = (float3)0.0;

#if defined(M2_RT)
    bool reflHit = false;
    bool refrHit = false;
    uint shadowHits = 0u;
    uint aoHits = 0u;
    uint queries = 0u;

    // Explore-mode progressive accumulation decorrelates every stochastic estimator per frame
    float ign = accumulating()
        ? frac(interleavedGradientNoise(i.pos.xy + float2(5.588238, 3.143218) * (float)(accumFrame & 255u)) + 0.6180340 * (float)accumFrame)
        : interleavedGradientNoise(i.pos.xy);
    float accumJit = accumulating() ? hash21(i.pos.xy + (float)(accumFrame & 1023u) * 17.31) : 0.5;
    float rotPhi = ign * kTwoPi;

    if (isGlass) {
        uint reflMask = (matId == 40u || isWaterDroplet) ? 0x01u : 0x03u;
        reflHit = traceScene(i.world + nGeom * 0.018, reflect(-v, n), 180.0, reflMask, 0.004 + rough * rough * 0.14, reflected);
        refrHit = traceScene(i.world - nGeom * 0.018, refrDir,        180.0, 0x01u,    (matId == 40u) ? 0.002 : 0.010, transmitted);
        // Close-up thick dielectric internal reflection + spectral dispersion sparkle (Glass Orchid, vase, amber bottle, droplets)
        if (matId != 40u && dist < 8.0) {
            float3 tirDir = reflect(refrDir, -n);
            if (accumulating() || isWaterDroplet || matId == 35u || (inBath && matId == 3u)) {
                float3 internalCol = transmitted;
                if (traceScene(i.world + nGeom * 0.012, tirDir, 24.0, 0x01u, 0.018, internalCol)) {
                    transmitted = lerp(transmitted, internalCol * float3(1.06, 1.00, 0.94), 0.24 * (1.0 - ndotv));
                }
                ++queries;
            }
        }
        hasTracedReflection = reflHit;
        queries += 2u;
    } else {
        bool wantRayRefl =
            (matId == 34u) || // Silvered flat mirror (always crisp ray-traced reflection + thin-glass shower transmission!)
            (matId == 7u)  || // Water
            (matId == 12u) || // Obsidian mirror stela
            (matId == 27u && dist < 12.0) || // Glazed porcelain basin
            (matId == 23u && dist < 12.0) || // Brushed champagne nickel faucet/ribs
            (matId == 8u  && dist < 28.0) || // 24k gold / brass
            (matId == 38u && dist < 10.0) || // Anodized dark bronze mullions/frame
            (matId == 28u && dist < 9.0)  || // Glazed ceramic backsplash tiles
            (matId == 31u && dist < 9.0)  || // Honed marble countertop
            (matId == 25u && dist < 10.0) || // Celadon stoneware vessels
            (matId == 20u && dist < 14.0) || // Hydrated hydroxyapatite enamel
            (matId == 1u  && wetPuddle > 0.38) ||
            (rough < 0.26 && fres > 0.24 && dist < 22.0);

        if (wantRayRefl) {
            if (matId == 34u || (matId == 12u && dist < 24.0)) {
                uint mQueries = 0u;
                reflHit = traceMirrorScene(i.world + nGeom * 0.016, reflect(-v, n), reflected, mQueries);
                hasTracedReflection = reflHit;
                queries += mQueries;
            } else {
                float spread = 0.004 + rough * rough * 0.22;
                float3 rDir0 = reflect(-v, n);
                if (accumulating() && rough > 0.025) {
                    float3 jB1, jB2;
                    buildOrthonormalBasis(rDir0, jB1, jB2);
                    float ggxR = rough * rough * sqrt(accumJit / max(1.0 - accumJit * 0.85, 0.05)) * 0.55;
                    float3 jDir = norm(rDir0 + (jB1 * cos(rotPhi) + jB2 * sin(rotPhi)) * min(ggxR, 0.45));
                    if (dot(jDir, nGeom) > 0.03) rDir0 = jDir;
                }
                reflHit = traceScene(i.world + nGeom * 0.016, rDir0, 180.0, 0x03u, spread, reflected);
                hasTracedReflection = reflHit;
                ++queries;

                // 2-ray satin GGX lobe filter for close brushed nickel, bronze & honed Calacatta marble in bathroom
                // (During NRD temporal accumulation after frame 1, rDir0 is already GGX-jittered every frame!)
                if (inBath && dist < 4.5 && !(accumulating() && accumFrame >= 2u) && (matId == 23u || matId == 31u || matId == 38u)) {
                    float3 rB1, rB2;
                    buildOrthonormalBasis(rDir0, rB1, rB2);
                    float lobeR = clamp(rough * 0.32, 0.015, 0.095);
                    float3 rDir1 = norm(rDir0 + (rB1 * cos(rotPhi) + rB2 * sin(rotPhi)) * lobeR);
                    if (dot(rDir1, nGeom) > 0.02) {
                        float3 refl2 = reflected;
                        bool hit2 = traceScene(i.world + nGeom * 0.016, rDir1, 60.0, 0x03u, spread * 1.25, refl2);
                        if (hit2 || reflHit) {
                            reflected = 0.5 * (reflected + refl2);
                            hasTracedReflection = true;
                        }
                        ++queries;
                    }
                }
            }
        }

        // Stratified deterministic Vogel-disk soft area sun shadows with 2-probe penumbra early-out
        bool canReceiveSun = (matId != 21u && matId != 17u) && ((dot(nGeom, sunDir) > -0.06) || (translucency > 0.18));
        if (canReceiveSun && dist < 85.0) {
            float3 sT1, sT2;
            buildOrthonormalBasis(sunDir, sT1, sT2);
            uint maxShadowSamples = isDawnFinale()
                ? 6u
                : (isGrandTutti() ? ((dist < 6.0) ? 2u : 1u) : ((dist < 9.5) ? 4u : ((dist < 22.0) ? 2u : 1u)));
            float sunRadius = isDawnFinale() ? 0.021 : 0.015;
            float maxShadowDist = isDawnFinale() ? 18.0 : 28.0;
            float3 shOrigin = i.world + nGeom * 0.015 + ((matId == 29u || matId == 36u) ? (sunDir * 0.022) : 0.0.xxx);

            if (accumulating() && accumFrame >= 2u) {
                // Unbiased 1-SPP stochastic disk sun shadow per temporal NRD accumulation frame
                float r = sqrt(accumJit) * sunRadius;
                float3 jitDir = norm(sunDir + (sT1 * cos(rotPhi) + sT2 * sin(rotPhi)) * r);
                bool occ = traceShadow(shOrigin, jitDir, maxShadowDist);
                if (occ) ++shadowHits;
                sunVis = occ ? 0.0 : 1.0;
                ++queries;
            } else if (maxShadowSamples == 1u) {
                bool occ = traceShadow(shOrigin, sunDir, maxShadowDist);
                if (occ) ++shadowHits;
                sunVis = occ ? 0.0 : 1.0;
                ++queries;
            } else {
                float rProbe = 0.72 * sunRadius;
                float th0 = accumulating() ? rotPhi : rotPhi * 0.30;
                bool occ0 = traceShadow(shOrigin, norm(sunDir + (sT1 * cos(th0) + sT2 * sin(th0)) * rProbe), maxShadowDist);
                bool occ1 = traceShadow(shOrigin, norm(sunDir - (sT1 * cos(th0) + sT2 * sin(th0)) * rProbe), maxShadowDist);
                if (occ0) ++shadowHits;
                if (occ1) ++shadowHits;
                queries += 2u;

                bool needPenumbraRefine = (occ0 != occ1) || (!exploreMode() && dist < 1.35) || (matId == 29u || matId == 36u);
                if (!needPenumbraRefine) {
                    sunVis = occ0 ? 0.0 : 1.0;
                } else {
                    float visAcc = (occ0 ? 0.0 : 1.0) + (occ1 ? 0.0 : 1.0);
                    [loop]
                    for (uint s = 2u; s < maxShadowSamples; ++s) {
                        float r = sqrt(((float)s + (accumulating() ? accumJit : 0.5)) / (float)maxShadowSamples) * sunRadius;
                        float theta = (float)s * 2.3999632 + (accumulating() ? rotPhi : rotPhi * 0.30);
                        float3 jitDir = norm(sunDir + (sT1 * cos(theta) + sT2 * sin(theta)) * r);
                        bool occ = traceShadow(shOrigin, jitDir, maxShadowDist);
                        if (occ) ++shadowHits;
                        visAcc += occ ? 0.0 : 1.0;
                        ++queries;
                    }
                    sunVis = visAcc / (float)maxShadowSamples;
                }
            }
        }

        // Bathroom: Cosine-Weighted DXR Hemisphere GI + Multi-Scale Contact AO (4mm crevices -> 3.85m window portal!)
        if (inBath && matId != 34u) {
            float3 gB1, gB2;
            buildOrthonormalBasis(n, gB1, gB2);
            float3 giAcc = (float3)0.0;
            float occAcc = 0.0;
            float3 giOrigin = i.world + nGeom * 0.010;
            uint giSamples = (accumulating() && accumFrame >= 2u) ? 2u : 4u;
            [loop]
            for (uint g = 0u; g < giSamples; ++g) {
                float uRad = ((float)g + accumJit) / (float)giSamples;
                float sinTheta = sqrt(uRad * 0.86);
                float cosTheta = sqrt(max(1.0 - sinTheta * sinTheta, 0.04));
                float phi = (float)g * 2.3999632 + (accumulating() ? rotPhi : rotPhi * 0.18);
                float3 giDir = norm(n * cosTheta + (gB1 * cos(phi) + gB2 * sin(phi)) * sinTheta);
                // Prevent normal-mapped bumps from shooting hemisphere rays below the geometric surface
                if (dot(giDir, nGeom) < 0.06) {
                    giDir = norm(giDir + nGeom * (0.08 - dot(giDir, nGeom)));
                }
                float3 sampleGi;
                float sampleOcc;
                traceBathroomGiRay(giOrigin, giDir, sampleGi, sampleOcc);
                if (sampleOcc > 0.05) ++aoHits;
                giAcc += sampleGi;
                occAcc += sampleOcc;
                ++queries;
            }
            float invGi = 1.0 / (float)giSamples;
            tracedBathGi = giAcc * invGi;
            rtContactAO = clamp(1.0 - 0.88 * (occAcc * invGi), 0.06, 1.0);
            hasTracedBathGi = true;
        } else if (dist < (isGrandTutti() ? 8.5 : 13.5) && matId != 34u && matId != 21u && matId != 17u) {
            // Exterior / Sanctuary / Cathedral: distance-adaptive 3-ray close / 1-ray mid Contact AO
            float3 aB1, aB2;
            buildOrthonormalBasis(nGeom, aB1, aB2);
            uint aoCount = isGrandTutti() ? 1u : ((dist < 7.2 || accumulating()) ? 3u : 1u);
            float occSum = 0.0;
            [loop]
            for (uint a = 0u; a < aoCount; ++a) {
                float ang = (float)a * (kTwoPi / 3.0) + (accumulating() ? rotPhi : rotPhi * 0.28);
                float3 aoDir = norm(nGeom * 0.72 + (aB1 * cos(ang) + aB2 * sin(ang)) * 0.694);
                float wOcc = traceAmbientRay(i.world + nGeom * 0.010, aoDir, 0.65);
                if (wOcc > 0.05) ++aoHits;
                occSum += wOcc;
                ++queries;
            }
            rtContactAO = 1.0 - 0.62 * (occSum / (float)aoCount);
        }

        // RTXDI ReSTIR Many-Light Reservoir Sampling (4 physical practical emitters -> 1 unbiased DXR shadow ray)
        if ((inBath || (exploreMode() && dist < 10.5)) && matId != 34u && matId != 21u && matId != 17u && !lampsOff()) {
            RTXDI_DIReservoir res = RTXDI_EmptyDIReservoir();
            [unroll]
            for (uint li = 0u; li < 4u; ++li) {
                float3 lPos, lInt;
                float lRad;
                getRtxdiEmitter(li, i.world, inBath, lPos, lInt, lRad);
                float3 toL = lPos - i.world;
                float d2 = dot(toL, toL);
                float dL = sqrt(max(d2, 1e-4));
                if (dL < (inBath ? 4.8 : 9.5)) {
                    float3 Ldir = toL / dL;
                    float atten = 1.0 / (1.0 + (inBath ? 1.85 : 0.45) * d2);
                    float3 contrib = evalGgxDirect(n, v, Ldir, base, max(rough, 0.14), metallic, clearcoat * 0.5) * lInt * atten;
                    float pHat = dot(contrib, float3(0.2126, 0.7152, 0.0722));
                    float uStream = frac(ign + (float)li * 0.3819660 + accumJit * 0.6180340);
                    RTXDI_StreamLight(res, li, pHat, uStream);
                }
            }
            if (res.valid != 0u && res.targetPdf > 1e-5) {
                float3 winPos, winInt;
                float winRad;
                getRtxdiEmitter(res.lightIndex, i.world, inBath, winPos, winInt, winRad);
                float3 lB1, lB2;
                float3 rawVec = winPos - i.world;
                float rawDist = length(rawVec);
                float3 lDir = rawVec / max(rawDist, 1e-3);
                buildOrthonormalBasis(lDir, lB1, lB2);
                float3 jitterPos = winPos + (lB1 * cos(rotPhi) + lB2 * sin(rotPhi)) * (winRad * sqrt(accumJit));
                float3 jVec = jitterPos - i.world;
                float jDist = length(jVec);
                float3 jDir = jVec / max(jDist, 1e-3);
                bool lOcc = (jDist > 0.08) ? traceShadow(i.world + nGeom * 0.014, jDir, max(jDist - 0.05, 0.02)) : false;
                ++queries;
                if (!lOcc) {
                    float atten = 1.0 / (1.0 + (inBath ? 1.85 : 0.45) * jDist * jDist);
                    float3 fullEval = evalGgxDirect(n, v, jDir, base, max(rough, 0.14), metallic, clearcoat * 0.5) * winInt * atten;
                    float risW = min(res.weightSum / max(res.targetPdf, 1e-5), 4.0);
                    restirDirect = fullEval * risW * (inBath ? 0.52 : 0.38);
                }
            }
        }
    }

    uint waveQueries = WaveActiveSum(queries);
    if (canonical != 0u && WaveIsFirstLane()) {
        InterlockedAdd(verified[1], waveQueries);
    }
    if (canonical != 0u && (((uint)i.pos.x & 31u) == 0u) && (((uint)i.pos.y & 31u) == 0u)) {
        uint anyHitCount = (reflHit ? 1u : 0u) + (refrHit ? 1u : 0u) + shadowHits + aoHits;
        // Guarantee hardware execution proof even on unoccluded sky-facing tiles where rays cleanly miss
        InterlockedAdd(verified[0], max(anyHitCount, (queries > 0u) ? 1u : 0u));
    }
#endif

    float combinedAO = texAO * rtContactAO;
    float3 neural = tensorField.SampleLevel(linearClamp, frac(i.world.xz * 0.035), 0.0).rgb;

    // 1. Direct Sun / Key GGX + Clearcoat Lighting
    float3 directLight = evalGgxDirect(n, v, sunDir, base, rough, metallic, clearcoat) * sunRad * sunVis + restirDirect;

    // 2. Subsurface / Botanical Leaf Translucency (backlit Calathea veins, Fern fronds, Succulent, Enamel, Towel)
    if (translucency > 0.02) {
        float backPhase = pow(saturate(dot(-v, sunDir) * 0.55 + saturate(dot(-n, sunDir)) * 0.65), 1.5);
        float wrapPhase = saturate(dot(n, sunDir) * 0.45 + 0.55);
        float transVis = (matId == 29u || matId == 36u) ? (0.28 + 0.72 * sunVis) : (0.18 + 0.82 * sunVis);
        float3 sssTint = (matId == 29u || matId == 36u || matId == 10u || matId == 30u)
            ? base * float3(1.22, 1.30, 0.68)
            : base * float3(1.08, 0.96, 0.84);
        directLight += sssTint * sunRad * translucency * (0.55 * backPhase + 0.22 * wrapPhase) * transVis;
    }

    // 3. Motivated Dawn Bathroom Window Area Specular/Fill + Warm Architectural Halo
    if (inBath) {
        // Window area-light direction gives realistic broad specular & subtle directional sheen occluded by rtContactAO
        float3 winPoint = float3(1.48, clamp(i.world.y + 0.25, 3.10, 5.45), clamp(i.world.z + 0.15, 3.00, 6.45));
        float3 toWin = winPoint - i.world;
        float winDist = length(toWin);
        float3 winL = toWin / max(winDist, 0.22);
        float winAtten = 1.0 / (1.0 + 0.22 * winDist * winDist);
        float3 winSkyIrr = float3(0.68, 0.76, 0.90) * timeRamp(336.0, 345.0) * winAtten * rtContactAO;
        directLight += evalGgxDirect(n, v, winL, base, max(rough, 0.14), metallic, clearcoat * 0.5) * winSkyIrr * (hasTracedBathGi ? 0.55 : 1.15);

        // Subtle warm 2900K backlit mirror halo at (-0.18, 4.39, 6.53)
        float3 toMirrorHalo = float3(clamp(i.world.x, -0.54, 0.18), clamp(i.world.y, 3.83, 4.95), 6.53) - i.world;
        float dmh = length(toMirrorHalo);
        if (dmh < 2.1) {
            float3 mhL = toMirrorHalo / max(dmh, 0.12);
            float mhFall = 1.0 / (1.0 + 4.2 * dmh * dmh);
            directLight += evalGgxDirect(n, v, mhL, base, max(rough, 0.22), metallic, 0.0) * float3(0.48, 0.34, 0.20) * mhFall * combinedAO * (lampsOff() ? 0.0 : 1.0);
        }
    } else {
        // Outdoor / Sanctuary / Cathedral secondary rim fill
        float3 rimTint = lerp(float3(0.26, 0.40, 0.58), float3(0.72, 0.56, 0.42), daylight);
        directLight += evalGgxDirect(n, v, rimDir, base, max(rough, 0.22), metallic, 0.0) * rimTint * 0.42 * combinedAO;
    }

    // 4. Image-Based Lighting + Ray-Traced Bathroom Hemisphere GI + Ray-Traced Specular Blend
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), base, metallic);
    if (matId == 34u) {
        // Optical silver mirror with subtle low-iron soda-lime glass green edge tint
        f0 = lerp(float3(0.95, 0.955, 0.95), float3(0.80, 0.89, 0.86), pow(1.0 - ndotv, 3.2));
    }
    float2 envBrdf = envBrdfApprox(rough, ndotv);
    float3 diffIrr = sampleIblEnv(n, 0.94, inBath);
    if (inBath) {
        if (hasTracedBathGi) {
            // 82% true 6-ray DXR hemisphere GI + 18% smooth prefiltered ambient stabilizer
            diffIrr = tracedBathGi * 0.84 + diffIrr * 0.16 * 0.36;
        } else {
            float sunRoomGlow = timeRamp(336.5, 346.0);
            float3 warmBounce = float3(0.18, 0.15, 0.12) * (0.55 + 0.45 * saturate(-n.y * 0.4 + 0.6));
            diffIrr = diffIrr * 0.42 + warmBounce * sunRoomGlow;
        }
    }

    float3 iblSpecEnv = sampleIblEnv(reflect(-v, n), rough, inBath);
    if (inBath) iblSpecEnv *= (0.35 + 0.65 * rtContactAO);
    float rayConfidence = hasTracedReflection ? saturate(1.0 - rough * 1.15) : 0.0;
    if (matId == 34u || matId == 7u || matId == 12u) {
        rayConfidence = hasTracedReflection ? saturate(1.0 - rough * 0.55) : 0.0;
    }
    float3 specRadiance = lerp(iblSpecEnv, reflected, rayConfidence);

    float3 kd = (1.0 - f0) * (1.0 - metallic);
    float diffOcclusion = (inBath && hasTracedBathGi) ? (texAO * (0.28 + 0.72 * rtContactAO)) : combinedAO;
    float specOcclusion = saturate(pow(combinedAO + ndotv, exp2(-16.0 * rough - 1.0)) - 1.0 + combinedAO);
    if (matId == 34u) specOcclusion = 1.0;

    float3 ambientLight = kd * base * diffIrr * diffOcclusion
                        + specRadiance * (f0 * envBrdf.x + envBrdf.y) * specOcclusion;

    if (clearcoat > 0.02) {
        float2 coatBrdf = envBrdfApprox(0.06, ndotv);
        float3 coatRefl = hasTracedReflection ? lerp(reflected, iblSpecEnv, 0.15) : sampleIblEnv(reflect(-v, n), 0.06, inBath);
        ambientLight += coatRefl * (0.04 * coatBrdf.x + coatBrdf.y) * clearcoat * specOcclusion * 0.65;
    }

    float3 color = directLight + ambientLight + emissive(matId, i.world, dist * 0.0022);

    // Subtle upward vertiport pad bounce illumination on hovering eVTOL airframe & wings at night
    if (act == 1u && i.world.y > 3.72 && i.world.y < 7.2) {
        float padDist = length((i.world - float3(0.0, 3.70, -2.0)) * float3(0.22, 0.38, 0.22));
        float padBounce = exp(-padDist * padDist * 2.2) * (0.35 + 0.65 * saturate(-n.y)) * (1.0 - daylight);
        color += base * float3(0.26, 0.68, 0.62) * padBounce;
    }

    // Dedicated architectural spotlighting on Taipei 101's 88F-91F 5.5 m Tuned Mass Damper sphere
    if (matId == 8u && act == 2u && i.world.y > 30.0 && i.world.y < 36.0) {
        float upCatch = 0.38 + 0.62 * saturate(-n.y * 0.45 + 0.55);
        color = color * 1.45 + base * float3(0.45, 0.32, 0.12) * upCatch;
    }

    // 5. Thin-Walled Dielectric Glass: Crystal Petals (3, 4), Smoked Canopy (18), Amber Bottle (35), Float Glass (40), Droplets
    if (isGlass) {
        float3 sideCrystalDir = norm(float3(0.78, 0.42, -0.46));
        float specKey  = pow(saturate(dot(n, norm(v + sunDir))), 220.0) * sunVis;
        float specSide = pow(saturate(dot(n, norm(v + sideCrystalDir))), 46.0);
        float specRim  = pow(saturate(dot(n, norm(v + rimDir))), 68.0);
        float specUp   = pow(saturate(dot(n, norm(v - upSanctuaryDir))), 48.0);

        if (matId == 18u) {
            float3 canopyTrans = transmitted * float3(0.14, 0.17, 0.22) + float3(0.012, 0.016, 0.022);
            float canopyReflMix = clamp(0.08 + 0.48 * fres, 0.0, 0.64);
            color = lerp(canopyTrans, specRadiance * 0.90, canopyReflMix)
                  + specKey  * sunRad * 0.85
                  + specSide * float3(0.10, 0.13, 0.16)
                  + specRim  * float3(0.22, 0.28, 0.35);
        } else if (matId == 35u) {
            // Warm hand-blown Amber Apothecary Soap Dispenser Glass
            float thickAmb = clamp(i.uv.z, 0.04, 0.22) / max(ndotv, 0.22);
            float3 beerAmb = exp(-float3(0.55, 2.85, 7.40) * thickAmb * 4.2);
            float3 ambTrans = transmitted * beerAmb + base * float3(0.14, 0.045, 0.008) * (1.0 - beerAmb);
            float ambReflMix = clamp(0.055 + 0.82 * fres, 0.0, 0.88);
            color = lerp(ambTrans, specRadiance, ambReflMix)
                  + specKey  * sunRad * 1.15
                  + specSide * float3(0.14, 0.09, 0.04);
        } else if (matId == 40u) {
            // Optically flat AR-coated low-iron architectural float glass (penthouse window, shower screen, balustrade)
            float thickPane = clamp(i.uv.z, 0.008, 0.04) / max(ndotv, 0.28);
            float3 beerLowIron = exp(-float3(0.28, 0.10, 0.14) * thickPane * 1.8);
            float paneReflMix = clamp(0.018 + 0.54 * fres, 0.018, 0.60);
            color = lerp(transmitted * beerLowIron, specRadiance * 0.88, paneReflMix)
                  + specKey * sunRad * 0.95;
        } else if (isWaterDroplet) {
            // 3D pure water droplets beaded inside the glazed porcelain basin & poised at faucet aerator tip
            float dropReflMix = clamp(0.03 + 0.88 * fres, 0.03, 0.86);
            color = lerp(transmitted * float3(0.98, 0.99, 1.0), specRadiance, dropReflMix)
                  + specKey  * sunRad * 1.45
                  + specSide * float3(0.18, 0.20, 0.24);
        } else {
            // Blown Botanical Liuli Crystal Glass Orchid (broad petals 3, celadon sepals & bud vase 4)
            float uBlade = saturate(i.uv.x);
            // Compatible with both [0..1]^2 UVs (2*uv.y - 1) and legacy [-1..1] parametric UVs
            float vBlade = (i.uv.y < 0.0) ? clamp(i.uv.y, -1.0, 1.0) : clamp(i.uv.y * 2.0 - 1.0, -1.0, 1.0);
            float thick = clamp(i.uv.z, 0.012, 0.10);
            float opticalPath = thick / max(ndotv, 0.24);

            float3 absorbRate = (matId == 4u) ? float3(3.0, 0.95, 1.25) : float3(1.05, 0.50, 0.55);
            float3 beer = exp(-absorbRate * opticalPath);

            float edgeRim = pow(1.0 - ndotv, 2.5);
            float crispMargin = pow(saturate(abs(vBlade)), 5.0) * smoothstep(0.10, 0.30, uBlade);
            float midribDepth = exp(-14.0 * vBlade * vBlade) * (1.0 - 0.45 * uBlade);
            float heartWarmth = (act == 2u && !inBath) ? (exp(-uBlade * 5.6) * (1.0 - 0.45 * abs(vBlade))) : 0.0;

            float3 bodyScatter = (matId == 4u)
                ? float3(0.018, 0.040, 0.036) * (0.40 + 1.10 * midribDepth)
                : float3(0.018, 0.024, 0.028) * (0.30 + 0.90 * midribDepth);
            float3 rimSheen = ((matId == 4u) ? float3(0.26, 0.40, 0.38) : float3(0.36, 0.42, 0.46)) * (edgeRim + 0.45 * crispMargin) * (inBath ? 0.55 : 1.0);
            float3 nectarGlow = float3(0.32, 0.19, 0.07) * heartWarmth;

            float3 redSky  = inBath ? sampleIblEnv(refract(-v, n, 1.0 / 1.486), 0.08, true) : sky(norm(refract(-v, n, 1.0 / 1.486)));
            float3 blueSky = inBath ? sampleIblEnv(refract(-v, n, 1.0 / 1.504), 0.08, true) : sky(norm(refract(-v, n, 1.0 / 1.504)));
            float3 dispersion = float3(redSky.r - transmitted.r, 0.0, blueSky.b - transmitted.b) * (0.06 * edgeRim);
            float3 glassTrans = max(transmitted + dispersion, 0.0) * beer + bodyScatter * (1.0 - beer) + nectarGlow;
            float reflMix = clamp(0.14 + 0.80 * fres, 0.0, 0.93);
            color = lerp(glassTrans, specRadiance * float3(1.04, 1.06, 1.10), reflMix)
                  + rimSheen
                  + specKey  * sunRad * 0.95
                  + specSide * float3(0.18, 0.22, 0.28) * (inBath ? 0.4 : 1.0)
                  + specRim  * float3(0.38, 0.48, 0.58) * (inBath ? 0.35 : 1.0)
                  + specUp   * float3(0.45, 0.28, 0.12) * ((act == 2u && !inBath) ? 1.0 : 0.0)
                  + neural * (0.012 * edgeRim);
        }
    }

    // Orchid Trilobed Labellum (9): hand-blown ruby-burgundy Liuli crystal with golden-nectar throat
    if (matId == 9u && act == 2u) {
        float uLip = saturate(i.uv.x);
        float vLip = (i.uv.y < 0.0) ? clamp(i.uv.y, -1.0, 1.0) : clamp(i.uv.y * 2.0 - 1.0, -1.0, 1.0);
        float throat = exp(-uLip * 3.6) * exp(-2.4 * vLip * vLip);
        float stria = 0.5 + 0.5 * cos(vLip * 22.0);
        float throatVeins = exp(-uLip * 2.4) * stria * (1.0 - throat) * 0.35;
        float rimRuby = pow(1.0 - ndotv, 2.2);
        float specLip = pow(saturate(dot(n, norm(v + sunDir))), 145.0) * sunVis;
        float3 rubyCore = base * 0.65 + float3(0.14, 0.010, 0.058);
        float3 nectarAmber = float3(0.78, 0.44, 0.09);
        float3 lipTint = lerp(rubyCore, nectarAmber, saturate(throat * 0.85 + throatVeins));
        float diffLip = 0.24 + 0.76 * saturate(dot(n, sunDir)) * sunVis;
        color = lipTint * diffLip * (inBath ? 1.35 : 1.0)
              + specRadiance * (0.38 * fres)
              + float3(0.56, 0.08, 0.24) * (rimRuby * (inBath ? 0.28 : 0.48))
              + float3(0.24, 0.13, 0.03) * throat * (inBath ? 0.5 : 1.0)
              + specLip * sunRad * 0.55;
    }

    // Sanctuary / Harbour Water Caustic Modulation
    if (matId == 7u && !inBath) {
        float wave = 0.5 + 0.5 * sin(length(i.world.xz) * 2.4 - time * 1.8);
        float energy = dot(neural, float3(0.2126, 0.7152, 0.0722));
        float caustBoost = (((showFlags >> 2u) & 7u) == 2u) ? 0.62 : 0.14;
        float3 caustCol = lerp(float3(0.020, 0.11, 0.14), float3(0.22, 0.15, 0.07), daylight);
        if (((showFlags >> 2u) & 7u) == 2u) {
            caustCol = ((showFlags & 2u) != 0u) ? float3(0.36, 0.10, 0.38) : float3(0.06, 0.34, 0.32);
        }
        color += (caustCol * energy + neural * (caustBoost * 0.85)) * (0.25 + 0.75 * wave) * ((((showFlags >> 2u) & 7u) == 2u) ? 1.0 : 0.14);
    }

    // Neural Velvet 64-layer Tensor Core SIREN silk-glass canopy ribbon
    if (matId == 21u) {
        float2 ruv = frac(float2(i.uv.x * 2.0 + time * 0.04, i.uv.y + i.world.z * 0.06));
        float3 siren  = tensorField.SampleLevel(linearClamp, ruv, 0.0).rgb;
        float3 sirenB = tensorField.SampleLevel(linearClamp, frac(ruv.yx * 1.37 - time * 0.03), 0.0).rgb;
        float velvetRim = pow(1.0 - ndotv, 2.4);
        float3 field = siren * 0.70 + sirenB * 0.30;
        float fieldLuma = dot(field, float3(0.2126, 0.7152, 0.0722));
        float3 tone = ((showFlags & 2u) != 0u) ? float3(0.86, 0.42, 0.66) : float3(0.34, 0.72, 0.66);
        float3 silk = lerp(fieldLuma.xxx * tone, field * tone, 0.24);
        color = silk * 0.68 * (0.55 + 0.55 * velvetRim)
              + specRadiance * (0.34 * fres)
              + float3(0.95, 0.74, 0.38) * (velvetRim * 0.18);
    }

    // Symbiotic bioluminescent micro-flora garden domes
    if (matId == 22u) {
        float domeGlow = pow(1.0 - ndotv, 1.8);
        float3 bioHue = lerp(float3(0.10, 0.92, 0.68), float3(0.98, 0.68, 0.24), clamp(moodAccent, 0.0, 1.0));
        if ((showFlags & 2u) != 0u) bioHue = lerp(bioHue, float3(0.92, 0.26, 0.78), 0.45);
        color += bioHue * (0.04 + 0.12 * domeGlow) * (0.75 + 0.30 * moodBloom + 0.15 * moodLight + 0.08 * moodCut) + neural * 0.025;
    }

    if (matId == 17u) {
        color = lerp(specRadiance, float3(0.62, 0.68, 0.74), 0.35) * (0.28 + 0.45 * daylight) + float3(0.04, 0.07, 0.10);
    }

    // Layered atmospheric depth for exterior vistas ONLY (never fog indoor architectural rooms!)
    if (!inBath) {
        float heightAtten = exp(-max(i.world.y, 0.0) * 0.055);
        float fogDensity = isDawnFinale()
            ? (0.0022 + 0.0028 * heightAtten)
            : ((act == 1u) ? lerp(0.0048, 0.0024, daylight) : ((act == 2u) ? (0.0038 + 0.0045 * heightAtten) : 0.0055));
        float fogStart = isDawnFinale() ? 14.0 : 4.5;
        float fog = 1.0 - exp(-max(dist - fogStart, 0.0) * fogDensity);
        color = lerp(color, sky(norm(i.world - eye)), fog * 0.74);
    }

    if (showFlags == 0u) {
        float mist = 1.0 - smoothstep(0.0, 2.0, min(abs(time - 60.0), abs(time - 120.0)));
        color = lerp(color, float3(0.28, 0.33, 0.36), mist);
    }

    Targets o;
    o.color = float4(max(color, 0.0.xxx), 1.0);
    o.distance = dist;
    return o;
}
#endif

struct SkyOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

SkyOut VSSky(uint id : SV_VertexID) {
    SkyOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.99999, 1.0);
    return o;
}

Targets PSSky(SkyOut i) {
    float2 p = i.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float3 d = norm(forward + right * p.x * aspect * tan(fov * 0.5) + up * p.y * tan(fov * 0.5));
    Targets o;
    o.color = float4(sky(d), 1.0);
    o.distance = 1000.0;
    return o;
}


#if defined(BOO_ACCUM)
// Explore-mode progressive accumulation: full-screen blend of the current HDR frame into an
// RGBA32F history (blend factor = 1/(n+1)) and resolve back into the HDR scene target.
Texture2D<float4> accumSrc : register(t0, space1);

float4 VSAccum(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 PSAccumBlend(float4 pos : SV_Position) : SV_Target0 {
    float4 c = accumSrc.Load(int3(pos.xy, 0));
    // Luminance firefly clamp keeps rare specular outliers from sparkling in the converged image
    float lum = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
    c.rgb *= (lum > 48.0) ? (48.0 / lum) : 1.0;
    return c;
}

float4 PSAccumResolve(float4 pos : SV_Position) : SV_Target0 {
    return accumSrc.Load(int3(pos.xy, 0));
}
#endif
