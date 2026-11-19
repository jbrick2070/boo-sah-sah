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
    float moodBloom; float moodLight; float moodCut; uint audioPacked; // RGBA8: low, mid, high, beat
    float3 sunOverrideDir; float sunOverrideScale;   // Explore-mode interactive sun (interFlags bit 0)
    uint accumFrame; uint interFlags; float2 jitterNdc; // bit1 = progressive accumulation, bit2 = lamps off
};

cbuffer Draw : register(b1) {
    float4 taipeiKit; // Kit translation and normalized district-space scale.
    uint instanceBase;
};

struct InstanceData {
    float4 world[3];
    float4 worldToObject[3];
    uint meshId;
    uint materialSlice;
    uint buildingSeed;
    uint flags;
};
StructuredBuffer<InstanceData> sceneInstances : register(t8);
StructuredBuffer<uint> meshFirstIndices : register(t9);

uint triangleOffset(uint instanceId, uint primitive) {
    return meshFirstIndices[sceneInstances[instanceId].meshId] + 3u * primitive;
}
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

Texture2DArray<float4> roomFaces : register(t10);

SamplerState linearClamp     : register(s0);
SamplerState anisoWrap       : register(s1);
SamplerState equirectSampler : register(s2);

static const float kPi = 3.14159265;
static const float kTwoPi = 6.28318530;

float3 norm(float3 v) {
    return v * rsqrt(max(dot(v, v), 1e-10));
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

bool splitDenoiseOutputs() {
    return (interFlags & 16u) != 0u;
}

bool reblurMotion() {
    return (interFlags & 32u) != 0u;
}

bool temporalSampling() {
    return (interFlags & (2u | 32u)) != 0u;
}

uint packHalf2(float2 v) {
    uint2 h = f32tof16(clamp(v, -65504.0.xx, 65504.0.xx));
    return (h.x & 0xFFFFu) | ((h.y & 0xFFFFu) << 16u);
}

float2 unpackHalf2(uint u) {
    return float2(f16tof32(u & 0xFFFFu), f16tof32(u >> 16u));
}

void unpackModViewZ(uint4 m, out float3 diffMod, out float3 specMod, out float viewZ) {
    float2 xy0 = unpackHalf2(m.x);
    float2 xy1 = unpackHalf2(m.y);
    float2 xy2 = unpackHalf2(m.z);
    diffMod = float3(xy0.x, xy0.y, xy1.x);
    specMod = float3(xy1.y, xy2.x, xy2.y);
    viewZ = asfloat(m.w);
}

uint aovMode() {
    return (interFlags >> 8u) & 0xFu;
}

float3 unpackDrawMotion(uint part, uint unused) {
    if ((part & 0xffffu) < 900u || (part & 0xffffu) >= 920u) return 0.0.xxx;
    uint hx = (part >> 16u) & 0xFFFFu;
    uint hy = unused & 0xFFFFu;
    uint hz = (unused >> 16u) & 0xFFFFu;
    return float3(f16tof32(hx), f16tof32(hy), f16tof32(hz));
}

// Scripted dawn ramp over film time, or (in Explore mode) driven by the interactive sun elevation.
// Live audio drive (smoothed, auto-levelled, flash-safe; all zero in deterministic captures and AOVs).
float audioLow()  { return (float)(audioPacked & 0xFFu) / 255.0; }
float audioMid()  { return (float)((audioPacked >> 8u) & 0xFFu) / 255.0; }
float audioHigh() { return (float)((audioPacked >> 16u) & 0xFFu) / 255.0; }
float audioBeat() { return (float)(audioPacked >> 24u) / 255.0; }
// Skyline spectrum: every building listens to one band (bass / mids / treble), chosen by its hash.
float audioBandForBuilding(float bldgId) {
    float pick = hash21(float2(bldgId * 0.37, 91.7));
    return (pick < 0.34) ? audioLow() : ((pick < 0.67) ? audioMid() : audioHigh());
}

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
        float3 lowSun  = float3(4.35, 3.12, 1.98);
        float3 highSun = float3(4.20, 3.88, 3.55);
        float3 moon    = float3(0.10, 0.14, 0.24);
        float3 sunCol = lerp(lowSun, highSun, smoothstep(0.08, 0.60, saturate(d.y)));
        return (sunCol * above + moon * (1.0 - above)) * sunOverrideScale;
    }
    if (isDawnFinale()) {
        float dawnRamp = timeRamp(336.0, 346.0);
        return lerp(float3(0.55, 0.62, 0.82), float3(4.68, 3.36, 2.04), dawnRamp);
    }
    float3 nightKey = (act == 1u) ? float3(0.30, 0.44, 0.66) : float3(0.52, 0.78, 1.08);
    return lerp(nightKey, float3(1.85, 1.32, 0.88), daylight);
}

float2 dirToEquirectUv(float3 d) {
    float3 dn = norm(d);
    return float2(atan2(dn.x, dn.z) / kTwoPi + 0.5, acos(clamp(dn.y, -1.0, 1.0)) / kPi);
}

float3 sampleIblEnv(float3 d, float rough, bool insideBathroom) {
    float3 dn = norm(d);
    float2 uv = dirToEquirectUv(dn);
    float mip = clamp(rough * 8.0, 0.0, 8.0);
    if (insideBathroom) {
        float3 dawnEnv = gEnvHdrTex.SampleLevel(equirectSampler, float3(uv, 2.0), mip).rgb;
        // Ground non-window indoor hemisphere (-X) so shadowed room walls & metals retain deep Cycles contrast
        float winHemi = smoothstep(-0.08, 0.56, dn.x) * smoothstep(-0.22, 0.24, dn.y);
        dawnEnv *= lerp(0.11, 1.15, winHemi);
        if (sunOverridden()) {
            float3 nightEnv = gEnvHdrTex.SampleLevel(equirectSampler, float3(uv, 0.0), mip).rgb * 0.14;
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

float3 skyImpl(float3 d, bool includeCelestialAndClouds) {
    float3 dn = norm(d);
    if (isDawnFinale()) {
        float dawnT = timeRamp(336.0, 346.0);
        float3 sDir = sceneSunDir();
        float horiz = pow(saturate(1.0 - max(dn.y, 0.0)), 2.35);
        float sunDot = saturate(dot(dn, sDir));
        float3 zenithCol = lerp(float3(0.025, 0.048, 0.110), float3(0.54, 0.66, 0.86), dawnT);
        float3 horizWarm = lerp(float3(0.08, 0.13, 0.22), float3(2.15, 1.68, 1.24), dawnT);
        float3 horizCool = lerp(float3(0.05, 0.09, 0.16), float3(1.28, 1.22, 1.26), dawnT);
        float azim = saturate(dot(normalize(float2(dn.x, dn.z) + 1e-5), normalize(float2(sDir.x, sDir.z))));
        float3 clearDawn = lerp(zenithCol, lerp(horizCool, horizWarm, pow(azim, 1.20)), horiz);
        float3 basinMist = lerp(float3(0.05, 0.08, 0.14), float3(1.26, 1.14, 1.04), dawnT);
        clearDawn = lerp(clearDawn, basinMist, saturate(-dn.y * 5.0));
        float sunGlow = pow(sunDot, 12.0) * 0.62 * dawnT;
        if (!includeCelestialAndClouds) {
            return clearDawn + float3(1.0, 0.76, 0.52) * sunGlow;
        }
        // Blend authentic CC0 Qwantani Dawn PureSky HDRI across the east/southeast window vista for Cycles parity
        float2 tuv = dirToEquirectUv(dn);
        float hdriWinMask = smoothstep(0.14, 0.52, dn.x) * (1.0 - smoothstep(0.16, 0.48, dn.y)) * smoothstep(-0.14, -0.01, dn.y) * dawnT;
        if (hdriWinMask > 0.001) {
            float3 hdriSky = gEnvHdrTex.SampleLevel(equirectSampler, float3(tuv, 2.0), 0.0).rgb * 1.88;
            clearDawn = lerp(clearDawn, hdriSky, hdriWinMask * 0.78);
        }
        float sunDisc = pow(sunDot, 260.0) * 6.0 * dawnT;
        float3 neural = tensorField.SampleLevel(linearClamp, tuv, 0.0).rgb;
        float nLum = dot(neural, float3(0.2126, 0.7152, 0.0722));
        float cloudBand = smoothstep(0.02, 0.28, dn.y) * (1.0 - smoothstep(0.36, 0.82, dn.y));
        return clearDawn
             + float3(1.0, 0.76, 0.52) * (sunDisc + sunGlow)
             + float3(0.26, 0.18, 0.14) * (nLum * cloudBand * 0.35 * dawnT);
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
    float sun = includeCelestialAndClouds ? pow(saturate(dot(dn, sunDir)), 180.0) : 0.0;
    float sunHalo = pow(saturate(dot(dn, sunDir)), 10.0);
    float3 c = clearSky + (sun * float3(3.2, 1.9, 0.85) + sunHalo * float3(0.32, 0.16, 0.06)) * daylight;

    if (includeCelestialAndClouds) {
        float2 uv = dirToEquirectUv(dn);
        float3 neural = tensorField.SampleLevel(linearClamp, uv, 0.0).rgb;
        float nLum = dot(neural, float3(0.2126, 0.7152, 0.0722));
        float highCloud = smoothstep(0.04, 0.34, dn.y) * (1.0 - smoothstep(0.45, 0.88, dn.y));
        c += lerp(float3(0.018, 0.034, 0.052), float3(0.18, 0.12, 0.07), daylight) * nLum * (0.28 + 0.55 * daylight) * highCloud;
    }

    if (showFlags == 0u) {
        float mist = 1.0 - smoothstep(0.0, 2.0, min(abs(time - 60.0), abs(time - 120.0)));
        c = lerp(c, float3(0.28, 0.33, 0.36), mist);
    }
    return c;
}

float3 sky(float3 d) {
    return skyImpl(d, true);
}

float3 skyAerialFog(float3 d) {
    return skyImpl(d, false);
}

bool cityExterior() {
    return (showFlags & 64u) != 0u || (showFlags == 0u && act != 0u);
}

bool isCurtainWall(uint m) { return m == 5u || m == 13u || m == 14u; }

bool isDistrictGlazing(uint m, uint partId) {
    return cityExterior() && isCurtainWall(m) && (act == 1u || (act == 2u && ((partId >= 2u && partId <= 4u) || partId == 61u || partId == 62u)));
}

// Unit-room slabs in tangent space. Face slices are back, left, right, floor and ceiling.
float2 roomCoordinates(InstanceData data, float3 position, float3 T, float3 B, float2 authoredUv) {
    if ((data.flags & 0xffffu) == 62u) return authoredUv;
    float3 offset = position - float3(data.world[0].w, data.world[1].w, data.world[2].w);
    float meters = (act == 2u ? taipeiKit.w : 1.0) / 0.08;
    return float2(dot(offset, T), dot(offset, B)) * meters;
}

float3 roomEmission(float3 broad, uint seed, float2 meters, float2 meterWidth,
                    float3 toEye, float3 N, float3 T, float3 B, float distanceMeters) {
    float2 grid = meters / float2(3.0, 3.2);
    float2 filterWidth = meterWidth / float2(3.0, 3.2);
    float facing = saturate(dot(toEye, N));
    float blend = (1.0 - smoothstep(160.0, 250.0, distanceMeters)) * smoothstep(0.05, 0.18, facing)
                * (1.0 - smoothstep(0.13, 0.34, max(filterWidth.x, filterWidth.y)));
    if (blend < 0.001) return broad;
    float2 window = frac(grid);
    float2 edges = min(window, 1.0 - window);
    float2 pane = smoothstep(float2(0.09,0.11), float2(0.12,0.15) + filterWidth, edges);
    int2 cell = (int2)floor(grid);
    uint h = asuint(cell.x) * 0x7feb352du ^ asuint(cell.y) * 0x846ca68bu ^ seed;
    h ^= h >> 15; h *= 0x9e3779b9u; h ^= h >> 16;
    float3 ray = float3(-dot(toEye,T)/3.0, -dot(toEye,B)/3.2, facing/4.0);
    float2 wall = (step(0.0, ray.xy) - window) /
                  (lerp(-1.0.xx, 1.0.xx, step(0.0,ray.xy)) * max(abs(ray.xy), 1e-5));
    float back = 1.0 / max(ray.z, 1e-5);
    float3 hit = float3(window, 0) + min(back, min(wall.x, wall.y)) * ray;
    uint face = 0u;
    float2 uv = float2(hit.x, 1.0 - hit.y);
    if (wall.x < back && wall.x < wall.y) {
        face = ray.x < 0 ? 1u : 2u;
        uv = float2(hit.z, 1.0 - hit.y);
    } else if (wall.y < back) {
        face = ray.y < 0 ? 3u : 4u;
        uv = float2(hit.x, hit.z);
    }
    float mip = log2(max(1.0, 64.0 * max(filterWidth.x, filterWidth.y) / max(facing, 0.15)));
    float3 room = roomFaces.SampleLevel(linearClamp, float3(saturate(uv), (h % 6u)*5u + face), mip).rgb;
    float occupancy = ((h >> 8) & 7u) < 3u ? 0.13 : 1.0;
    float trim = (1.0 - pane.x*pane.y);
    float3 behindGlass = room * occupancy * (0.75 + 0.25*audioBandForBuilding(float(seed & 255u)));
    return lerp(broad, behindGlass * (1.0 - trim) + 0.08*broad*trim, blend);
}

// Exact coverage of a repeated stripe, including its average when several stripes fit in one pixel.
float facadeStripe(float t, float fraction, float footprint) {
    float w = max(footprint, 0.012);
    float2 ends = t + 0.5 * fraction + float2(-0.5, 0.5) * w;
    float2 integrals = floor(ends) * fraction + min(frac(ends), fraction);
    return saturate((integrals.y - integrals.x) / w);
}

// At street-block distances, occupied wings and floor plates survive the pixel filter
// longer than individual rooms. Use the same averaged elevation in a reflection cone.
float3 shadeBlockWindows(uint m, uint buildingSeed, float3 p, float3 n, float3 footprint,
                         out float3 base, out float rough, out float metallic) {
    bool housing = m == 13u;
    bool jade = m == 5u;
    bool xAxis = abs(n.z) >= abs(n.x);
    float width = housing ? 0.78 : (jade ? 0.98 : 0.69);
    float floorHeight = housing ? 0.51 : 0.65;
    float u = (xAxis ? p.x : p.z) / width + float(buildingSeed & 31u);
    float v = p.y / floorHeight;
    float du = (xAxis ? footprint.x : footprint.z) / width;
    float dv = footprint.y / floorHeight;
    float suiteWidth = housing ? 3.0 : (jade ? 4.0 : 6.0);
    float stackHeight = housing ? 5.0 : (jade ? 8.0 : 6.0);
    float column = u / suiteWidth;
    float level = (v + float((buildingSeed >> 5u) & 7u)) / stackHeight;

    float pier = facadeStripe(column, housing ? 0.29 : (jade ? 0.16 : 0.075), du / suiteWidth);
    float floorPlate = facadeStripe(v, housing ? 0.39 : 0.24, dv);
    float bay = 1.0 - facadeStripe(u, housing ? 0.20 : 0.12, du);
    float service = facadeStripe(level, housing ? 0.14 : 0.20, dv / stackHeight);
    float glass = (1.0 - pier) * (1.0 - floorPlate) * bay * (1.0 - service);
    float3 cladding = housing ? float3(0.31, 0.22, 0.14) :
                     (jade ? float3(0.10, 0.20, 0.19) : float3(0.12, 0.17, 0.23));
    float3 glazing = housing ? float3(0.035, 0.049, 0.048) :
                    (jade ? float3(0.016, 0.070, 0.069) : float3(0.021, 0.049, 0.094));
    base = lerp(cladding, glazing, glass);
    rough = lerp(housing ? 0.55 : 0.37, 0.19, glass);
    metallic = lerp(housing ? 0.15 : 0.49, 0.16, glass);

    // Tenant leases span a few bays and one to three storeys. Stagger the suites
    // in adjacent wings so the occupied regions don't become continuous bands.
    float leaseWidth = housing ? 1.9 : (jade ? 3.7 : 3.1);
    float leaseHeight = housing ? 1.7 : (jade ? 3.2 : 2.3);
    float leaseColumn = floor(u / leaseWidth);
    float seed = float((buildingSeed >> 12u) & 255u);
    float leaseFloor = floor(v / leaseHeight + frac(leaseColumn * 0.618 + seed * 0.27));
    float tenant = hash21(float2(leaseColumn + seed, leaseFloor + m));
    float groupsResolved = 1.0 - smoothstep(0.26, 0.95, max(du / leaseWidth, dv / leaseHeight));
    float typicalOccupancy = (housing ? 0.52 : 0.38) + 0.21 * (frac(seed * 0.71) - 0.5);
    float occupancy = lerp(typicalOccupancy, smoothstep(0.91, 1.09, tenant + typicalOccupancy), groupsResolved);
    float3 interior = housing ? float3(0.74, 0.32, 0.12) :
                     (jade ? float3(0.13, 0.48, 0.40) : float3(0.30, 0.43, 0.63));
    float warmth = saturate(2.3 * frac(tenant * 8.7) - 1.35) * groupsResolved;
    interior = lerp(interior, housing ? float3(0.58, 0.40, 0.22) : float3(0.55, 0.45, 0.33), warmth * 0.58);
    float3 rooms = interior * ((0.024 + 0.78 * occupancy) * glass);
    // Recessed apartment stairwells, vertical facade fins, and double-height
    // office sky lobbies give each family a distinct rhythm across the block.
    float accent;
    if (housing || jade) {
        accent = facadeStripe(column - 0.21, housing ? 0.11 : 0.047, du / suiteWidth)
               * (1.0 - floorPlate * (housing ? 0.83 : 0.30));
    } else {
        float lobbyWing = facadeStripe(column - 0.51, 0.49, du / suiteWidth);
        accent = facadeStripe(level - 0.17, 0.12, dv / stackHeight) * lobbyWing;
    }
    float3 gallery = housing ? float3(0.51, 0.24, 0.095) :
                    (jade ? float3(0.13, 0.48, 0.43) : float3(0.52, 0.31, 0.14));
    float band = audioBandForBuilding(float(buildingSeed & 255u));
    return (rooms + gallery * accent) * ((1.0 - smoothstep(0.08, 0.68, daylight)) * (1.0 + 0.20 * band));
}

// Near glass keeps motion parallax. Crossfade to the compact elevation as panes shrink.
float3 shadeDistrictFacade(uint m, uint buildingSeed, float3 p, float3 geomNormal, float3 toViewer, float3 footprint,
                           inout float3 base, inout float rough, inout float metallic) {
    float wall = 1.0 - smoothstep(0.32, 0.82, abs(geomNormal.y));
    if (wall < 0.001) {
        base = lerp(float3(0.035, 0.053, 0.066), base, 0.12);
        rough = 0.46;
        metallic = 0.26;
        return 0.0.xxx;
    }

    float3 blockBase = 0.0.xxx, blockLight = 0.0.xxx;
    float blockRough = 0.0, blockMetallic = 0.0;
    float faceWidth = abs(geomNormal.z) >= abs(geomNormal.x) ? footprint.x : footprint.z;
    float blockWeight = smoothstep(0.012, 0.046, max(faceWidth, footprint.y));
    if (blockWeight > 0.0) {
        blockLight = shadeBlockWindows(m, buildingSeed, p, geomNormal, footprint, blockBase, blockRough, blockMetallic) * wall;
        if (blockWeight >= 1.0) {
            base = blockBase;
            rough = blockRough;
            metallic = blockMetallic;
            return blockLight;
        }
    }
    bool hotel = m == 13u;
    bool campus = m == 5u;
    bool apartments = act == 2u;
    float bayWidth = apartments ? (hotel ? 0.68 : 0.52) : (hotel ? 0.78 : (campus ? 1.15 : 0.62));
    float floorHeight = apartments ? 0.49 : (hotel ? 0.57 : (campus ? 0.82 : 0.67));
    bool frontFacing = abs(geomNormal.z) >= abs(geomNormal.x);
    float u = (frontFacing ? p.x : p.z) / bayWidth;
    float v = (p.y + 0.31) / floorHeight;
    float du = (frontFacing ? footprint.x : footprint.z) / bayWidth;
    float dv = footprint.y / floorHeight;

    float bayGroup = apartments ? 2.0 : (hotel ? 3.0 : (campus ? 4.0 : 6.0));
    float floorGroup = apartments ? 6.0 : (hotel ? 7.0 : (campus ? 5.0 : 10.0));
    float seed = (float)m + (frontFacing ? 0.0 : 17.0);
    float suite = floor(u / bayGroup);
    float storey = floor(v);
    float suiteId = hash21(float2(suite + seed, storey));
    float wingId = hash21(float2(suite + seed, floor(v / (hotel ? 2.0 : 3.0))));
    float elevation = saturate((p.y + 0.4) / 16.0);
    // Neighboring blocks pick up different amounts of warm and cool glazing.
    float blockTint = 0.5 + 0.5 * sin(0.079 * p.x + 0.112 * p.z + seed);

    float fin = facadeStripe(u / bayGroup, hotel ? 0.089 : 0.065, du / bayGroup);
    float mullion = facadeStripe(u, hotel ? 0.16 : 0.10, du);
    float spandrel = facadeStripe(v, hotel ? 0.36 : 0.25, dv);
    float service = facadeStripe((v - 0.58) / floorGroup, 1.02 / floorGroup, dv / floorGroup);
    float frame = max(fin, max(mullion, spandrel));
    float glass = (1.0 - frame) * (1.0 - service);

    float3 glazing = hotel ? float3(0.082, 0.077, 0.075)
                           : (campus ? float3(0.023, 0.072, 0.080) : float3(0.036, 0.068, 0.105));
    float3 ribMetal = hotel ? float3(0.20, 0.15, 0.10) : float3(0.109, 0.155, 0.17);
    if (apartments) ribMetal = float3(0.156, 0.145, 0.127);
    glazing *= lerp(float3(0.84, 0.97, 1.13), float3(1.16, 1.04, 0.90), blockTint);
    float3 facing = lerp(glazing * (0.52 + 0.48 * elevation), ribMetal, max(fin, 0.55 * frame));
    facing = lerp(facing, float3(0.018, 0.031, 0.041), service * 0.87);
    float louver = facadeStripe(v * 5.0, 0.29, dv * 5.0);
    facing += ribMetal * 0.24 * louver * service * (1.0 - fin);
    base = lerp(base, facing, 0.94 * wall);
    rough = lerp(0.44, 0.16, glass);
    metallic = lerp(0.52, 0.15, glass);

    // A suite spans multiple panes. Adjacent floors share tenants and colour temperature.
    float occupancy = smoothstep(0.38, 0.76, 0.62 * wingId + 0.38 * suiteId);
    float suiteResolved = 1.0 - smoothstep(0.34, 1.32, max(dv, du / bayGroup));
    occupancy = lerp(0.32, occupancy, suiteResolved);
    float3 cool = campus ? float3(0.32, 0.71, 0.66) : float3(0.43, 0.64, 0.78);
    float warm = apartments ? 0.78 : (hotel ? 0.78 : (campus ? 0.20 : 0.32));
    warm = saturate(warm + (wingId - 0.5) * 0.65 * suiteResolved);
    float3 room = lerp(cool, float3(0.96, 0.58, 0.29), warm);
    float ceiling = facadeStripe(v - 0.72, 0.16, dv);
    float night = 1.0 - smoothstep(0.08, 0.68, daylight);
    if (isDawnFinale()) night *= 0.22;
    float band = audioBandForBuilding(seed + suite);
    float3 light = room * glass * occupancy * (0.16 + 0.21 * ceiling) * (1.0 + 0.24 * band);
    // Project the viewing ray from the pane onto the wall behind it. Surrounding reveals
    // occlude that wall as the camera passes. Fade the entire room into its average
    // once the floor or suite becomes too small to resolve, including in reflections.
    if (suiteResolved > 0.0) {
        float roomBays = hotel ? 1.0 : (apartments ? 2.0 : bayGroup);
        float2 aperture = float2(frac(u / roomBays), frac(v));
        float viewU = frontFacing ? toViewer.x : toViewer.z;
        float depth = (hotel ? 0.54 : (apartments ? 0.62 : 0.84)) / max(abs(dot(geomNormal, toViewer)), 0.28);
        float2 back = aperture - float2(viewU * floorHeight / (roomBays * bayWidth), toViewer.y) * depth;
        float2 aa = clamp(float2(du / roomBays, dv), 0.015, 0.48);
        float2 visible = smoothstep(-aa, aa, back) * (1.0 - smoothstep(1.0 - aa, 1.0 + aa, back));
        float backWall = visible.x * visible.y;
        float soffit = smoothstep(1.0 - aa.y, 1.0 + aa.y, back.y);
        // Hotel rooms have curtains; offices have ceiling fixtures over desk-height partitions.
        float curtain = 1.0 - smoothstep(0.13 - aa.x, 0.13 + aa.x, min(back.x, 1.0 - back.x));
        float panel = (1.0 - smoothstep(0.40 - aa.y, 0.40 + aa.y, back.y)) *
                      smoothstep(0.20 - aa.x, 0.20 + aa.x, back.x);
        float fitting = facadeStripe(back.y - 0.83, 0.09, dv) * backWall;
        float decor = hotel ? curtain : panel;
        float roomLight = lerp(0.42, 1.18 - decor * 0.51, backWall) + 0.30 * soffit + 0.48 * fitting;
        float resolvable = suiteResolved * (1.0 - smoothstep(0.25, 0.90, max(du / roomBays, dv)));
        light *= lerp(1.0, roomLight, resolvable);
        base *= 1.0 - glass * resolvable * (0.22 * (1.0 - backWall) + 0.15 * decor * backWall);
    }
    if (dv < 0.18) {
        // Individual tenants leave their blinds at different heights. Resolve slats only
        // when the pixel footprint can distinguish them, including in ray-traced reflections.
        float resolved = 1.0 - smoothstep(0.045, 0.18, dv);
        float lowered = smoothstep(0.20 + 0.56 * suiteId, 0.29 + 0.56 * suiteId, frac(v));
        float slats = facadeStripe(v * 6.0 + wingId, 0.42, dv * 6.0);
        light *= 1.0 - 0.45 * lowered * slats * resolved * suiteResolved;
    }

    // Narrow inlaid lights follow the service galleries; breaks align with groups of facade fins.
    float gallery = facadeStripe((v - 1.06) / floorGroup, 0.12 / floorGroup, dv / floorGroup);
    float run = facadeStripe((u - bayGroup) / (3.0 * bayGroup), 0.49, du / (3.0 * bayGroup));
    float3 trim = hotel ? float3(0.47, 0.21, 0.09) : float3(0.07, 0.39, 0.45);
    if (apartments) {
        // Small, sheltered stair landings recur in a single bay instead of long corporate light strips.
        run = facadeStripe((u - 0.5) / 6.0, 0.11, du / 6.0);
        trim = float3(0.42, 0.23, 0.095);
        light *= 0.78;
    }
    base = lerp(base, blockBase, blockWeight);
    rough = lerp(rough, blockRough, blockWeight);
    metallic = lerp(metallic, blockMetallic, blockWeight);
    return lerp((light + trim * gallery * run * (campus ? 0.85 : 0.48)) * night * wall, blockLight, blockWeight);
}

// The painted fairings and wings share a waterline in aircraft space. Integrate
// the narrow bronze stripe across the pixel or ray footprint so it recedes cleanly.
void aircraftPaint(uint partId, float localY, float footprint,
                   inout float3 base, inout float rough, inout float metallic) {
    float centre = partId == 100u ? 0.072 : 0.36;
    float w = max(footprint, 0.006);
    float aboveBelly = saturate((localY - centre + 0.018) / w + 0.5);
    float aboveStripe = saturate((localY - centre - 0.018) / w + 0.5);
    float stripe = aboveBelly - aboveStripe;
    float3 paint = float3(0.017, 0.080, 0.096) * (1.0 - aboveBelly)
                 + float3(0.62, 0.30, 0.092) * stripe
                 + float3(0.73, 0.76, 0.72) * aboveStripe;
    base = paint * (0.88 + 0.12 * base);
    rough = 0.27 - 0.075 * stripe;
    metallic = 0.22 + 0.46 * stripe;
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
        return float3(0.05, 0.86, 0.66) * (0.62 + 0.18 * pulse + 0.26 * audioHigh()) * (0.08 + 0.92 * nightFactor);
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
        // Skyline as a spectrum analyser: each tower's lit rooms swell with its band, and the level
        // climbs the facade like a VU meter (lower storeys respond first).
        float band = audioBandForBuilding(bldgId);
        float meter = smoothstep(0.0, 0.20, band - frac(flr / 40.0) * 0.92);
        float audioGain = 1.0 + (0.14 * band + 0.38 * meter) * (1.0 - lodBlend * 0.5);
        return winCol * (winFactor * intensity * nightFactor * audioGain);
    }
    if (m == 21u) {
        float2 tuv = frac(p.xz * 0.095 + float2(time * 0.028, -time * 0.018));
        float3 nSample = tensorField.SampleLevel(linearClamp, tuv, 0.0).rgb;
        float3 accent = ((showFlags & 2u) != 0u) ? float3(1.15, 0.42, 1.05) : float3(0.35, 1.15, 0.96);
        return nSample * accent * (0.42 + 0.12 * pulse + 0.26 * audioMid());
    }
    if (m == 22u) {
        float3 floraTint = lerp(float3(0.08, 0.88, 0.64), float3(0.96, 0.62, 0.20), clamp(moodAccent, 0.0, 1.0));
        if ((showFlags & 2u) != 0u) floraTint = lerp(floraTint, float3(0.88, 0.24, 0.74), 0.45);
        return floraTint * (0.045 + 0.05 * moodBloom + 0.025 * pulse + 0.042 * audioBeat()) * (0.25 + 0.75 * nightFactor);
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
            uint cOff = triangleOffset(q.CandidateInstanceID(), q.CandidatePrimitiveIndex());
            Vertex cv0 = vertices[indices[cOff]];
            Vertex cv1 = vertices[indices[cOff + 1u]];
            Vertex cv2 = vertices[indices[cOff + 2u]];
            float2 cb = q.CandidateTriangleBarycentrics();
            float2 cuv = cv0.uv.xy + cb.x * (cv1.uv.xy - cv0.uv.xy) + cb.y * (cv2.uv.xy - cv0.uv.xy);
            uint cm = min(sceneInstances[q.CandidateInstanceID()].materialSlice, 41u);
            float ca = gAlbedoAlphaTex.SampleLevel(anisoWrap, float3(cuv, (float)cm), 1.0).a;
            if (ca >= 0.44) {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float3 traceShadowTransmittance(float3 origin, float3 direction, float distance, bool evalGlassCaustics, out bool hitOpaque) {
    hitOpaque = traceShadow(origin, direction, distance);
    if (hitOpaque) {
        return (float3)0.0;
    }
    if (!evalGlassCaustics) {
        return (float3)1.0;
    }

    RayDesc gRay;
    gRay.Origin = origin;
    gRay.Direction = direction;
    gRay.TMin = 0.012;
    gRay.TMax = min(distance, 2.8);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> gq;
    float3 trans = (float3)1.0;
    uint hitGlasses = 0u;
    gq.TraceRayInline(scene, RAY_FLAG_NONE, 0x02u, gRay);
    while (gq.Proceed()) {
        if (gq.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            gq.CommitNonOpaqueTriangleHit();
        }
    }
    if (gq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        uint gOff = triangleOffset(gq.CommittedInstanceID(), gq.CommittedPrimitiveIndex());
        Vertex gv0 = vertices[indices[gOff]];
        Vertex gv1 = vertices[indices[gOff + 1u]];
        Vertex gv2 = vertices[indices[gOff + 2u]];
        uint gm = min(sceneInstances[gq.CommittedInstanceID()].materialSlice, 41u);
        if (gm != 40u) {
            float2 gb = gq.CommittedTriangleBarycentrics();
            float3 gObjN = gv0.n.xyz + gb.x * (gv1.n.xyz - gv0.n.xyz) + gb.y * (gv2.n.xyz - gv0.n.xyz);
            float3 gN = norm(mul(gObjN, (float3x3)gq.CommittedWorldToObject3x4()));
            float cosInc = saturate(abs(dot(gN, direction)));
            // Grazing glass walls bend light outward -> dark dielectric rim shadow; central lens axis focuses warm caustic!
            float fresLoss = (1.0 - (0.04 + 0.96 * pow(1.0 - cosInc, 5.0))) * smoothstep(0.12, 0.56, cosInc);
            float lensFocus = 0.30 + 1.85 * pow(saturate((cosInc - 0.62) / 0.38), 2.4);
            float3 beerTint = (gm == 35u) ? float3(0.96, 0.42, 0.08)
                            : ((gm == 4u) ? float3(0.56, 0.92, 0.78)
                            : ((gm == 18u) ? float3(0.24, 0.30, 0.38) : float3(0.95, 0.99, 1.02)));
            trans *= beerTint * clamp(fresLoss * lensFocus, 0.06, 1.85);
            ++hitGlasses;
        }
    }
    return (hitGlasses > 0u) ? trans : (float3)1.0;
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

void traceBathroomGiRay(float3 origin, float3 direction, out float3 giRad, out float occWeight, out float outHitT) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.006;
    ray.TMax = 8.50;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_FORCE_OPAQUE, 0x01u, ray);
    q.Proceed();

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        float hitT = q.CommittedRayT();
        outHitT = hitT;
        float3 hitP = origin + direction * hitT;
        uint offset = triangleOffset(q.CommittedInstanceID(), q.CommittedPrimitiveIndex());
        Vertex v0 = vertices[indices[offset]];
        Vertex v1 = vertices[indices[offset + 1u]];
        Vertex v2 = vertices[indices[offset + 2u]];
        uint hm = min(sceneInstances[q.CommittedInstanceID()].materialSlice, 41u);
        float2 bary = q.CommittedTriangleBarycentrics();
        float2 hitUv = v0.uv.xy + bary.x * (v1.uv.xy - v0.uv.xy) + bary.y * (v2.uv.xy - v0.uv.xy);
        float3 hitAlb = gAlbedoAlphaTex.SampleLevel(anisoWrap, float3(hitUv, (float)hm), 4.5).rgb;

        // Rays that pass out through the corner window opening onto the terrace or skyline gather outdoor portal light
        if (!isInsideDawnBathroom(hitP)) {
            giRad = hitAlb * float3(0.54, 0.48, 0.42) + sky(direction) * 0.24;
            occWeight = 0.0;
            return;
        }

        float wMicro = 1.0 - smoothstep(0.004, 0.18, hitT);
        float wMeso  = 1.0 - smoothstep(0.025, 1.35, hitT);
        occWeight = 0.64 * wMicro + 0.36 * wMeso;

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
        // Physically Lambert-normalized directional window bounce (preserves Cycles' deep west-wall & basin chiaroscuro!)
        float3 toWinH = float3(1.58, clamp(hitP.y, 3.12, 5.35), clamp(hitP.z, 3.12, 6.35)) - hitP;
        float dWinH = length(toWinH);
        float3 lWinH = toWinH / max(dWinH, 0.25);
        float faceWin = pow(saturate(dot(hitWN, lWinH)), 1.25);
        float roomPortalVis = smoothstep(-2.35, 1.35, hitP.x) * smoothstep(2.25, 3.85, hitP.z);
        float vanityBlock = (hitP.x > -0.95 && hitP.x < 0.56 && hitP.y < 3.50 && hitP.z > 5.70) ? 0.14 : 1.0;
        float3 skyBounce = float3(0.010, 0.010, 0.012)
                         + float3(0.36, 0.33, 0.30) * (faceWin * roomPortalVis * vanityBlock * dawnRamp / (1.0 + 0.28 * dWinH * dWinH));
        float3 sunBounce = sRad * (sunNdotL * inWinPatch * vanityBlock * 0.24);
        float3 toSc = float3(0.08, 4.44, 2.32) - hitP;
        float3 pracBounce = lampsOff() ? (float3)0.0 : (float3(0.20, 0.12, 0.05) / (0.85 + 2.40 * dot(toSc, toSc)));
        giRad = hitAlb * (skyBounce + sunBounce + pracBounce) * smoothstep(0.004, 0.080, hitT);
        if (hm == 15u && !lampsOff()) {
            float emScale = (hitP.z < 3.2) ? 0.85 : ((hitP.y < 3.3) ? 0.12 : 0.24);
            giRad += float3(0.95, 0.66, 0.36) * emScale;
        }
        return;
    }

    // Ray escaped through the east/southeast penthouse window portal into the Taipei dawn sky!
    outHitT = 8.50;
    float eastPortal = smoothstep(-0.02, 0.28, direction.x);
    giRad = sky(direction) * (0.48 * eastPortal);
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
            uint cOff = triangleOffset(q.CandidateInstanceID(), q.CandidatePrimitiveIndex());
            Vertex cv0 = vertices[indices[cOff]];
            Vertex cv1 = vertices[indices[cOff + 1u]];
            Vertex cv2 = vertices[indices[cOff + 2u]];
            float2 cb = q.CandidateTriangleBarycentrics();
            float2 cuv = cv0.uv.xy + cb.x * (cv1.uv.xy - cv0.uv.xy) + cb.y * (cv2.uv.xy - cv0.uv.xy);
            uint cm = min(sceneInstances[q.CandidateInstanceID()].materialSlice, 41u);
            float ca = gAlbedoAlphaTex.SampleLevel(anisoWrap, float3(cuv, (float)cm), 1.0).a;
            if (ca >= 0.44) {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        uint offset = triangleOffset(q.CommittedInstanceID(), q.CommittedPrimitiveIndex());
        Vertex v0 = vertices[indices[offset]];
        Vertex v1 = vertices[indices[offset + 1u]];
        Vertex v2 = vertices[indices[offset + 2u]];
        uint hm = min(sceneInstances[q.CommittedInstanceID()].materialSlice, 41u);
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

        uint hitPart = sceneInstances[q.CommittedInstanceID()].flags & 0xffffu;
        bool districtFacade = isDistrictGlazing(hm, hitPart);
        float hNormScale = (hm == 34u || hm == 40u || districtFacade) ? 0.04 : saturate(1.0 - mipLod * 0.12);
        hNorXY *= hNormScale;
        float hNz = sqrt(saturate(1.0 - dot(hNorXY, hNorXY)));
        float3 hitN = norm(T * hNorXY.x + B * hNorXY.y + hitNGeom * hNz);

        float wetP = (hm == 1u) ? puddleMask(p.xz) : 0.0;
        float hRough = clamp(hOrm.y, 0.03, 0.98);
        if (hm == 1u) hRough = lerp(hRough, 0.08, wetP);
        float hMetal = saturate(hOrm.z);
        if (act == 1u && hm == 11u && hitPart >= 100u && hitPart <= 115u) {
            float bodyY = v0.p.y + bary.x * (v1.p.y - v0.p.y) + bary.y * (v2.p.y - v0.p.y);
            aircraftPaint(hitPart, bodyY, coneSpread * hitT * length(w2o[1]), hBase, hRough, hMetal);
        }
        float hCoat  = clearcoatForMaterial(hm, hOrm.w, wetP);
        float hTrans = translucencyForMaterial(hm, hOrm.w);

        float3 facadeLight = 0.0.xxx;
        if (districtFacade) {
            float hitWidth = coneSpread * hitT / max(abs(dot(hitNGeom, direction)), 0.18);
            float3 facadePos = p;
            if (act == 2u) {
                facadePos = (p - taipeiKit.xyz) * taipeiKit.w;
                hitWidth *= taipeiKit.w;
            }
            facadeLight = shadeDistrictFacade(hm, sceneInstances[q.CommittedInstanceID()].buildingSeed,
                                             facadePos, hitNGeom, -direction, hitWidth.xxx, hBase, hRough, hMetal);
            InstanceData roomInstance = sceneInstances[q.CommittedInstanceID()];
            if ((roomInstance.flags & (1u << 24)) && hitPart >= 60u && hitPart <= 62u && abs(hitNGeom.y) < 0.55) {
                float metersPerUnit = (act == 2u ? taipeiKit.w : 1.0) / 0.08;
                float2 meters = roomCoordinates(roomInstance, p, T, B, hitUv);
                facadeLight = roomEmission(facadeLight, roomInstance.buildingSeed, meters, (hitWidth/0.08).xx,
                                           -direction, hitNGeom, T, B, length(p-eye)*metersPerUnit);
            }
        } else if (isCurtainWall(hm)) {
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
        float secShadowConeMax = (accumulating() && accumFrame >= 2u) ? 0.0035 : 0.065;
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
            // Physically grounded directional window-aperture irradiance + localized 2700K opal sconce for Cycles mirror parity
            float3 toWin = float3(1.52, clamp(p.y, 3.15, 5.30), clamp(p.z, 3.15, 6.25)) - p;
            float winDist = length(toWin);
            float3 winL = toWin / max(winDist, 0.25);
            float winDay = sunOverridden() ? (0.02 + 0.98 * timeRamp(336.0, 345.0)) : 1.0;
            float zPortalVis = smoothstep(2.15, 4.55, p.z) * (0.16 + 0.84 * smoothstep(-2.25, 1.25, p.x));
            float underVan2 = (p.x > -0.95 && p.x < 0.56 && p.y < 3.50 && p.z > 5.68) ? 0.14 : 1.0;
            float winFall = winDay * zPortalVis * underVan2 / (1.0 + 0.42 * winDist * winDist);
            diffEnv2 = diffEnv2 * 0.035 + float3(0.42, 0.38, 0.35) * pow(saturate(dot(hitN, winL)), 1.25) * winFall;
            specEnv2 *= (0.05 + 0.80 * zPortalVis * underVan2 * saturate(dot(reflect(-V2, hitN), winL) * 0.55 + 0.45));
            if (!lampsOff()) {
                float sconceBoost = sunOverridden() ? lerp(2.8, 1.0, timeRamp(336.0, 345.0)) : 1.0;
                float3 toSconce = float3(0.08, 4.44, 2.32) - p;
                float dSc = length(toSconce);
                float3 scL = toSconce / max(dSc, 0.10);
                diffEnv2 += float3(0.46, 0.27, 0.12) * sconceBoost * saturate(dot(hitN, scL) * 0.80 + 0.20) / (0.18 + 7.2 * dSc * dSc);
                if (sunOverridden()) {
                    float3 toVanityLamp = float3(-0.18, 5.16, 5.88) - p;
                    float dVl = length(toVanityLamp);
                    diffEnv2 += float3(0.88, 0.58, 0.30) * max(sconceBoost - 1.0, 0.0) * saturate(dot(hitN, toVanityLamp / max(dVl, 0.15))) / (1.0 + 1.6 * dVl * dVl);
                }
            }
        }

        float3 kd2 = (1.0 - f0_2) * (1.0 - hMetal);
        float3 ibl2 = (kd2 * hBase * diffEnv2 + specEnv2 * (f0_2 * brdf2.x + brdf2.y)) * hOrm.x;

        if (hm == 3u || hm == 4u || hm == 18u || hm == 35u || hm == 40u) {
            float fres2 = 0.04 + 0.96 * pow(1.0 - nv2, 5.0);
            float3 beer2 = (hm == 35u) ? float3(0.62, 0.22, 0.04) : ((hm == 4u) ? float3(0.68, 0.92, 0.82) : hBase);
            float3 trans2 = sampleIblEnv(direction, 0.10, inBath) * beer2;
            color = lerp(trans2, specEnv2, clamp(fres2, 0.045, 0.88)) + direct2;
            return true;
        }

        float3 hitCol = direct2 + ibl2 + (districtFacade ? facadeLight : emissive(hm, p, coneSpread * hitT));
        if (!inBath) {
            float hAlt = exp(-max(p.y - 2.0, 0.0) * 0.14);
            float hFog = 1.0 - exp(-max(hitT - (isDawnFinale() ? 95.0 : 4.5), 0.0) * (isDawnFinale() ? (0.0006 + 0.0020 * hAlt) : 0.0055));
            hitCol = lerp(hitCol, sky(direction), hFog * (isDawnFinale() ? 0.36 : 0.72));
        }
        color = hitCol;
        return true;
    }

    color = isInsideDawnBathroom(origin) ? (sky(direction) * 0.92) : sky(direction);
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
        uint gOff = triangleOffset(gq.CommittedInstanceID(), gq.CommittedPrimitiveIndex());
        Vertex gv0 = vertices[indices[gOff]];
        Vertex gv1 = vertices[indices[gOff + 1u]];
        Vertex gv2 = vertices[indices[gOff + 2u]];
        uint gm = min(sceneInstances[gq.CommittedInstanceID()].materialSlice, 41u);
        float2 gb = gq.CommittedTriangleBarycentrics();
        float3 gObjN = gv0.n.xyz + gb.x * (gv1.n.xyz - gv0.n.xyz) + gb.y * (gv2.n.xyz - gv0.n.xyz);
        float3x3 gw2o = (float3x3)gq.CommittedWorldToObject3x4();
        float3 gN = norm(mul(gObjN, gw2o));
        if (dot(gN, direction) > 0.0) gN = -gN;

        float gNv = saturate(dot(gN, -direction));
        float gFres = clamp((0.04 + 0.96 * pow(1.0 - gNv, 5.0)) * 1.15, 0.035, 0.58);
        float3 gTint = (gm == 35u) ? float3(0.68, 0.26, 0.05)
                     : ((gm == 4u) ? float3(0.76, 0.94, 0.86) : float3(0.965, 0.988, 0.980));
        bool mirBath = isInsideDawnBathroom(origin);
        float3 gSpec = sampleIblEnv(reflect(direction, gN), 0.035, mirBath) * 0.45;
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
        if (lampsOff()) {
            lightPos = float3(0.0, 5.0, 5.0);
            lightIntensity = (float3)0.0;
            lightRadius = 0.04;
            return;
        }
        // In daylight dawn (Cycles parity), practical ceiling/halo lights stay gently dimmed so window sunlight & deep
        // architectural shadows dominate; when interactive sun drops toward dusk/night, practicals ramp up warmly!
        float dayT = sunOverridden() ? timeRamp(336.0, 345.0) : 1.0;
        float vanityNightScale = lerp(3.10, 0.08, dayT);
        float sconceScale = lerp(3.20, 1.05, dayT);
        if (idx == 0u) { // Recessed warm 2700K architectural ceiling vanity downlight (night/dusk fill)
            lightPos = float3(-0.18, 5.18, 5.82);
            lightIntensity = float3(1.55, 1.02, 0.56) * vanityNightScale;
            lightRadius = 0.065;
        } else if (idx == 1u) { // Backlit 8mm vanity mirror perimeter halo (2900K)
            lightPos = float3(clamp(worldP.x, -0.54, 0.18), clamp(worldP.y, 3.85, 4.92), 6.50);
            lightIntensity = float3(0.72, 0.50, 0.28) * lerp(2.60, 0.22, dayT);
            lightRadius = 0.042;
        } else if (idx == 2u) { // Opal cylindrical wall sconce (2700K) on rear oak/tile wall
            lightPos = float3(0.08, 4.44, 2.32);
            lightIntensity = float3(1.25, 0.80, 0.42) * sconceScale;
            lightRadius = 0.055;
        } else { // Keepsake Glass Orchid bioluminescent labellum core in celadon vase
            lightPos = float3(0.402, 3.80, 6.235);
            lightIntensity = float3(0.24, 0.78, 0.64) * (0.68 + 0.32 * pulse) * lerp(2.20, 0.45, dayT);
            lightRadius = 0.028;
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

// ---------------------------------------------------------------------------------------------
// RTX showcase additions: rain-impact ripples, thin-film interference, ray-marched light shafts.
// ---------------------------------------------------------------------------------------------
// Two decorrelated grids of expanding rain-impact rings; returns the height-field gradient in xz,
// band-limited by the projected pixel footprint pxWorld so shallow grazing angles do not speckle.
float2 rainRippleGrad(float2 xz, float t, float pxWorld) {
    float2 g = float2(0.0, 0.0);
    [unroll] for (uint layer = 0u; layer < 2u; ++layer) {
        float fl = (float)layer;
        float scale = (layer == 0u) ? 2.6 : 4.3;
        float pxCell = pxWorld * scale;
        float bandFade = saturate(1.0 - pxCell * 3.2);
        float invWidth = min(20.0, 0.48 / max(pxCell, 1e-4));
        float2 p = xz * scale + fl * 17.31;
        float2 cell = floor(p);
        float2 f = frac(p) - 0.5;
        float h = hash21(cell + 3.7 * fl);
        float2 off = (float2(hash21(cell + 11.1), hash21(cell + 23.9)) - 0.5) * 0.5;
        float2 d = f - off;
        float r = length(d);
        float phase = frac(t * (0.55 + 0.60 * h) + h * 7.0);
        float x = (r - phase * 0.42) * invWidth;
        float fade = (1.0 - phase) * (1.0 - phase) * bandFade;
        float wave = sin(x * 2.0) * exp(-x * x) * fade;
        g += (d / max(r, 1e-3)) * wave * scale;
    }
    return g;
}

// Thin-film interference (single n = 1.38 layer) sampled at R/G/B wavelengths.
float3 thinFilmInterference(float cosTheta, float thicknessNm) {
    float nf = 1.38;
    float cosT = sqrt(saturate(1.0 - (1.0 - cosTheta * cosTheta) / (nf * nf)));
    float opd = 2.0 * nf * thicknessNm * cosT;
    return 0.5 + 0.5 * cos(6.2831853 * opd / float3(650.0, 532.0, 450.0));
}

float henyeyGreensteinPhase(float cosT, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (12.5663706 * pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-4), 1.5));
}

// Height-falling exterior haze. Indoor rooms stay clear, matching the aerial-fog rule.
float shaftMediumDensity(float3 p) {
    if (isInsideDawnBathroom(p)) return 0.0;
    float base = (act == 1u) ? 0.010 : ((act == 2u) ? 0.015 : 0.013);
    if (isDawnFinale()) base *= 0.35;
    if ((showFlags & 2u) != 0u) base *= 1.25;
    return base * exp(-max(p.y, 0.0) * 0.085);
}

#if defined(M2_RT)
// Ray-marched single scattering: each march step fires a sun / moon visibility ray through the BVH,
// so towers, the bridge truss and orchid petals carve real light shafts out of the haze.
float3 rtLightShafts(float3 dir, float maxT, float jitter, inout uint queries) {
    const uint kSteps = 3u;
    float marchT = min(maxT, 90.0);
    float dt = marchT / (float)kSteps;
    float3 sd = sceneSunDir();
    float acc = 0.0;
    float tau = 0.0;
    [loop] for (uint s = 0u; s < kSteps; ++s) {
        float t = ((float)s + jitter) * dt;
        float3 p = eye + dir * t;
        float dens = shaftMediumDensity(p);
        if (dens > 1e-5) {
            float lit = traceShadow(p, sd, 80.0) ? 0.0 : 1.0;
            ++queries;
            acc += lit * dens * exp(-tau) * dt;
        }
        tau += dens * dt;
    }
    float phase = lerp(0.0796, henyeyGreensteinPhase(dot(dir, sd), 0.62), 0.82);
    float3 tint = sceneSunRadiance();
    if ((showFlags & 2u) != 0u) tint = lerp(tint, float3(0.62, 0.32, 0.84), 0.35);
    return acc * phase * tint * lerp(1.7, 2.6, daylight) * (0.90 + 0.22 * audioLow());
}
#endif

struct VOut {
    float4 pos         : SV_Position;
    float3 world       : TEXCOORD0;
    float3 normal      : TEXCOORD1;
    float4 uv          : TEXCOORD2;
    nointerpolation uint mat : TEXCOORD3;
    float4 tangentSign : TEXCOORD4;
    nointerpolation uint flags : TEXCOORD5;
    nointerpolation uint seed : TEXCOORD6;
    float localY : TEXCOORD7;
    float2 roomMeters : TEXCOORD8;
};

VOut VSMain(uint id : SV_VertexID, uint instanceId : SV_InstanceID) {
    InstanceData data = sceneInstances[instanceBase + instanceId];
    Vertex v = vertices[indices[meshFirstIndices[data.meshId] + id]];
    float4 p = float4(v.p.xyz,1.0);
    float3x3 worldBasis = float3x3(data.world[0].xyz,data.world[1].xyz,data.world[2].xyz);
    float3x3 inverseLinear = float3x3(data.worldToObject[0].xyz,data.worldToObject[1].xyz,data.worldToObject[2].xyz);
    VOut o;
    o.world = float3(dot(data.world[0],p),dot(data.world[1],p),dot(data.world[2],p));
    o.normal = norm(mul(v.n.xyz,inverseLinear));
    o.uv = v.uv;
    o.mat = data.materialSlice;
    o.flags = data.flags;
    o.seed = data.buildingSeed;
    o.localY = v.p.y;
    o.tangentSign = float4(norm(mul(worldBasis,v.tangent.xyz)), v.tangent.w);
    o.roomMeters = 0.0.xx;
    if ((data.flags & (1u << 24)) && (o.mat == 5u || o.mat == 13u || o.mat == 14u)) {
        o.roomMeters = roomCoordinates(data, o.world, o.tangentSign.xyz,
            cross(o.normal, o.tangentSign.xyz) * o.tangentSign.w, o.uv.xy);
    }
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
#if defined(M2_RT)
    float4 normRough  : SV_Target2;
    float4 diffRadHit : SV_Target3;
    float4 specRadHit : SV_Target4;
    uint4  modViewZ   : SV_Target5;
    float4 worldMv    : SV_Target6;
#endif
};

float3 evalAovHeatmap(float t) {
    t = saturate(t);
    float3 c0 = float3(0.05, 0.07, 0.24);
    float3 c1 = float3(0.06, 0.56, 0.78);
    float3 c2 = float3(0.22, 0.84, 0.38);
    float3 c3 = float3(0.98, 0.78, 0.14);
    float3 c4 = float3(0.98, 0.18, 0.08);
    if (t < 0.25) return lerp(c0, c1, t * 4.0);
    if (t < 0.50) return lerp(c1, c2, (t - 0.25) * 4.0);
    if (t < 0.75) return lerp(c2, c3, (t - 0.50) * 4.0);
    return lerp(c3, c4, (t - 0.75) * 4.0);
}

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
    uint partId = i.flags & 0xffffu;
    bool aircraft = act == 1u && partId >= 100u && partId <= 125u;
    bool districtFacade = isDistrictGlazing(matId, partId);
    float3 worldFootprint = fwidth(i.world);
    float2 roomWidth = fwidth(i.roomMeters);
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
    float viewZ = max(dot(i.world - eye, forward), 0.05);
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
    if (aircraft && (matId == 11u || matId == 18u)) normFade *= 0.18;
    if (districtFacade) normFade *= 0.04;
    nxy *= normFade;
    float nz = sqrt(saturate(1.0 - dot(nxy, nxy)));
    float3 n = norm(T * nxy.x + B * nxy.y + nGeom * nz);

    float wetPuddle = (matId == 1u) ? puddleMask(i.world.xz) : 0.0;
    float3 base = albSample.rgb;
    float texAO = ormSample.x;
    float rough = clamp(ormSample.y, 0.020, 0.98);
    float metallic = saturate(ormSample.z);
    float auxVal = ormSample.w;
    if (aircraft && matId == 11u && partId <= 115u) {
        aircraftPaint(partId, i.localY, fwidth(i.localY), base, rough, metallic);
    }

    // Ground indoor PVD brushed champagne nickel (mat 23) & botanical Calathea/Fern chlorophyll (mat 29, 36) to Cycles
    if (inBath && matId == 23u) {
        base *= 0.88;
        rough = clamp(rough * 0.86, 0.075, 0.27);
    }
    if (inBath && (matId == 29u || matId == 36u)) {
        base = pow(max(base, 1e-4.xxx), 1.28.xxx) * float3(0.82, 0.88, 0.62);
        rough = clamp(rough * 0.84, 0.22, 0.56);
    }

    if (matId == 1u) {
        rough = lerp(rough, 0.065, wetPuddle);
        base *= lerp(1.0, 0.58, wetPuddle);
    }
    if (matId == 7u || (matId == 1u && wetPuddle > 0.15)) {
        float baseRip = ((matId == 7u) ? 0.010 : (0.005 * wetPuddle)) * (1.0 + 0.28 * audioLow());
        float ripScale = baseRip / (1.0 + dist * 0.022);
        float wx = sin(i.world.z * 5.7 + i.world.x * 2.3 + time * 2.4) * 0.55
                 + sin(i.world.z * 12.3 - i.world.x * 6.1 - time * 1.9) * 0.30
                 + cos(i.world.x * 8.9 + i.world.z * 3.4 + time * 1.3) * 0.15;
        float wz = cos(i.world.x * 6.4 - i.world.z * 2.8 - time * 2.1) * 0.55
                 + cos(i.world.z * 9.8 + i.world.x * 5.2 + time * 1.7) * 0.30
                 + sin(i.world.x * 13.7 + time * 2.7) * 0.15;
        n = norm(n + float3(wx * ripScale, 0.0, wz * ripScale));
        // Rain-impact rings on upward-facing wet exterior surfaces, faded out before they can alias.
        float ringAmt = ((matId == 7u) ? 0.010 : 0.016 * wetPuddle) * saturate(1.0 - dist / 55.0) * saturate(n.y * 4.0 - 2.8) * (1.0 + 0.55 * audioLow());
        if (ringAmt > 0.0 && !inBath) {
            // Projected pixel footprint on the surface, scaled by the real canvas height and field of view.
            // 3.4888 = 1440 / (900 * tan(0.43)): identical to the hand-tuned value at the 1440p, 0.86 rad
            // reference, while 4K now keeps ripple detail it can resolve and 720p filters a little harder.
            float pxWorld = dist * tan(fov * 0.5) * 3.4888 / ((float)max(height, 1u) * max(abs(nGeom.y) * saturate(dot(nGeom, v)), 0.06));
            float2 rg = rainRippleGrad(i.world.xz, time, pxWorld);
            n = norm(n - float3(rg.x, 0.0, rg.y) * ringAmt);
        }
    }

    float cwPane = 1.0;
    float3 facadeLight = 0.0.xxx;
    if (districtFacade) {
        float3 facadePos = i.world;
        float3 facadeFootprint = worldFootprint;
        if (act == 2u) {
            // The Taipei kit translates and rescales during the penthouse transition. Keep rooms on their walls.
            float districtScale = taipeiKit.w;
            facadePos = (facadePos - taipeiKit.xyz) * districtScale;
            facadeFootprint *= districtScale;
        }
        facadeLight = shadeDistrictFacade(matId, i.seed, facadePos, nGeom, v, facadeFootprint, base, rough, metallic);
        if ((i.flags & (1u << 24)) && partId >= 60u && partId <= 62u && abs(nGeom.y) < 0.55) {
            float metersPerUnit = (act == 2u ? taipeiKit.w : 1.0) / 0.08;
            facadeLight = roomEmission(facadeLight, i.seed, i.roomMeters, roomWidth, v, nGeom, T, B, dist * metersPerUnit);
        }
    } else if (isCurtainWall(matId)) {
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

    bool isWaterDroplet = (inBath && matId == 3u && partId >= 230u && partId < 900u);
    bool isDynamicMarble = (partId >= 900u && partId < 920u);
    bool isThinCrystalPetal = (inBath && matId == 3u && !isWaterDroplet && !isDynamicMarble);
    float ior = isWaterDroplet ? 1.333 : ((matId == 35u) ? 1.52 : 1.50);
    float refrBend = (matId == 40u) ? 0.0 : (isThinCrystalPetal ? 0.18 : (isWaterDroplet ? 0.36 : 0.65));
    float3 refrDir = norm(lerp(-v, refract(-v, n, 1.0 / ior), refrBend));

    float3 sunTrans = 1.0.xxx;
    float rtContactAO = 1.0;
    float3 reflected = sampleIblEnv(reflect(-v, n), rough, inBath);
    float3 transmitted = (inBath && matId != 40u) ? sampleIblEnv(refrDir, 0.06, true) : sky(refrDir);
    bool hasTracedReflection = false;
    bool hasTracedBathGi = false;
    float3 tracedBathGi = (float3)0.0;
    float3 restirDirect = (float3)0.0;
    float glassThickness3D = 0.0;
    bool glassTir = false;
    float diffHitDist = 2.0;
    float specHitDist = 16.0;
    uint rayQueryCount = 0u;
    float3 shaftInscatter = (float3)0.0;

#if defined(M2_RT)
    bool reflHit = false;
    bool refrHit = false;
    uint shadowHits = 0u;
    uint aoHits = 0u;
    uint queries = 0u;

    // Temporal decorrelation for both NRD::REFERENCE (stationary) and NRD::REBLUR_DIFFUSE_SPECULAR (motion)
    bool tempSamp = temporalSampling();
    float ign = tempSamp
        ? frac(interleavedGradientNoise(i.pos.xy + float2(5.588238, 3.143218) * (float)(accumFrame & 255u)) + 0.6180340 * (float)accumFrame)
        : interleavedGradientNoise(i.pos.xy);
    float accumJit = tempSamp ? hash21(i.pos.xy + (float)(accumFrame & 1023u) * 17.31) : 0.5;
    float rotPhi = ign * kTwoPi;

    if (isGlass) {
        uint reflMask = (matId == 40u || isWaterDroplet) ? 0x01u : 0x03u;
        float gReflT = 180.0;
        reflHit = traceSceneEx(i.world + nGeom * 0.018, reflect(-v, n), 180.0, reflMask, 0.004 + rough * rough * 0.14, reflected, gReflT);
        specHitDist = reflHit ? gReflT : 24.0;

        if (matId == 40u || isThinCrystalPetal) {
            float3 thinDir = (matId == 40u) ? -v : refrDir;
            refrHit = traceScene(i.world - nGeom * 0.018, thinDir, 180.0, 0x01u, 0.003, transmitted);
            glassThickness3D = isThinCrystalPetal ? 0.012 : 0.008;
            queries += 2u;
        } else {
            // Solid 2-Interface Snell Refraction + 3D Internal Thickness & Total Internal Reflection
            float3 refrDir1 = refract(-v, n, 1.0 / ior);
            if (dot(refrDir1, refrDir1) < 1e-5) {
                refrDir1 = -v;
            } else {
                refrDir1 = norm(refrDir1);
            }

            RayDesc tRay;
            tRay.Origin = i.world + refrDir1 * 0.0035 - nGeom * 0.0025;
            tRay.Direction = refrDir1;
            tRay.TMin = 0.002;
            tRay.TMax = 0.32;
            RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> tq;
            tq.TraceRayInline(scene, RAY_FLAG_FORCE_OPAQUE, 0x02u, tRay);
            while (tq.Proceed()) {}
            ++queries;

            float3 exitOrigin = i.world - nGeom * 0.018;
            float3 exitDir = refrDir1;
            if (tq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                float tBack = tq.CommittedRayT();
                glassThickness3D = clamp(tBack, 0.008, 0.24);
                uint tOff = triangleOffset(tq.CommittedInstanceID(), tq.CommittedPrimitiveIndex());
                Vertex tv0 = vertices[indices[tOff]];
                Vertex tv1 = vertices[indices[tOff + 1u]];
                Vertex tv2 = vertices[indices[tOff + 2u]];
                float2 tbary = tq.CommittedTriangleBarycentrics();
                float3 objNBack = tv0.n.xyz + tbary.x * (tv1.n.xyz - tv0.n.xyz) + tbary.y * (tv2.n.xyz - tv0.n.xyz);
                float3x3 tw2o = (float3x3)tq.CommittedWorldToObject3x4();
                float3 nBack = norm(mul(objNBack, tw2o));
                float3 nExit = (dot(nBack, refrDir1) >= 0.0) ? nBack : -nBack;
                float3 refrDir2 = refract(refrDir1, -nExit, ior);
                if (dot(refrDir2, refrDir2) < 1e-5) {
                    glassTir = true;
                    exitDir = norm(reflect(refrDir1, -nExit));
                    exitOrigin = tRay.Origin + refrDir1 * tBack - nExit * 0.008;
                } else {
                    exitDir = norm(refrDir2);
                    exitOrigin = tRay.Origin + refrDir1 * tBack + nExit * 0.012;
                }
            }

            refrHit = traceScene(exitOrigin, exitDir, 180.0, 0x01u, 0.008, transmitted);
            queries += 2u;

            if (!glassTir && dist < 8.0 && (tempSamp || isWaterDroplet || isDynamicMarble || matId == 35u || (inBath && (matId == 3u || matId == 4u)))) {
                float3 tirDir = reflect(refrDir1, -n);
                float3 internalCol = transmitted;
                if (traceScene(i.world + nGeom * 0.012, tirDir, 24.0, 0x01u, 0.018, internalCol)) {
                    transmitted = lerp(transmitted, internalCol * float3(1.06, 1.00, 0.94), 0.22 * (1.0 - ndotv));
                }
                ++queries;
            }
        }
        hasTracedReflection = reflHit;
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
                specHitDist = 4.0;
                queries += mQueries;
            } else {
                float spread = 0.004 + rough * rough * 0.22;
                float3 rDir0 = reflect(-v, n);
                if (tempSamp && rough > 0.025) {
                    float3 jB1, jB2;
                    buildOrthonormalBasis(rDir0, jB1, jB2);
                    float lobeScale = reblurMotion() ? 0.36 : 0.55;
                    float ggxR = rough * rough * sqrt(accumJit / max(1.0 - accumJit * 0.85, 0.05)) * lobeScale;
                    float3 jDir = norm(rDir0 + (jB1 * cos(rotPhi) + jB2 * sin(rotPhi)) * min(ggxR, 0.42));
                    if (dot(jDir, nGeom) > 0.03) rDir0 = jDir;
                }
                float rHitT0 = 180.0;
                reflHit = traceSceneEx(i.world + nGeom * 0.016, rDir0, 180.0, 0x03u, spread, reflected, rHitT0);
                specHitDist = reflHit ? rHitT0 : 32.0;
                hasTracedReflection = reflHit;
                ++queries;

                // 2-ray satin GGX lobe filter for close brushed nickel, bronze & honed Calacatta marble in bathroom
                if (inBath && dist < 4.5 && !(accumulating() && accumFrame >= 2u) && (matId == 23u || matId == 31u || matId == 38u)) {
                    float3 rB1, rB2;
                    buildOrthonormalBasis(rDir0, rB1, rB2);
                    float lobeR = clamp(rough * 0.30, 0.014, 0.088);
                    float3 rDir1 = norm(rDir0 + (rB1 * cos(rotPhi) + rB2 * sin(rotPhi)) * lobeR);
                    if (dot(rDir1, nGeom) > 0.02) {
                        float3 refl2 = reflected;
                        float rHitT1 = 60.0;
                        bool hit2 = traceSceneEx(i.world + nGeom * 0.016, rDir1, 60.0, 0x03u, spread * 1.25, refl2, rHitT1);
                        if (hit2 || reflHit) {
                            reflected = 0.5 * (reflected + refl2);
                            if (hit2) specHitDist = min(specHitDist, rHitT1);
                            hasTracedReflection = true;
                        }
                        ++queries;
                    }
                }
            }
        }

        // Stratified Vogel-disk soft sun shadows + curved-glass refractive Beer-Lambert caustics
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
            bool evalCaustics = inBath || (act == 2u && dist < 8.5);
            uint qPerShadow = evalCaustics ? 2u : 1u;

            if (accumulating() && accumFrame >= 2u) {
                float r = sqrt(accumJit) * sunRadius;
                float3 jitDir = norm(sunDir + (sT1 * cos(rotPhi) + sT2 * sin(rotPhi)) * r);
                bool occ = false;
                sunTrans = traceShadowTransmittance(shOrigin, jitDir, maxShadowDist, evalCaustics, occ);
                if (occ || dot(sunTrans, float3(0.333, 0.334, 0.333)) < 0.85) ++shadowHits;
                queries += qPerShadow;
            } else if (maxShadowSamples == 1u) {
                bool occ = false;
                sunTrans = traceShadowTransmittance(shOrigin, sunDir, maxShadowDist, evalCaustics, occ);
                if (occ || dot(sunTrans, float3(0.333, 0.334, 0.333)) < 0.85) ++shadowHits;
                queries += qPerShadow;
            } else {
                float rProbe = 0.72 * sunRadius;
                float th0 = tempSamp ? rotPhi : rotPhi * 0.18;
                bool occ0 = false;
                bool occ1 = false;
                float3 tr0 = traceShadowTransmittance(shOrigin, norm(sunDir + (sT1 * cos(th0) + sT2 * sin(th0)) * rProbe), maxShadowDist, evalCaustics, occ0);
                float3 tr1 = traceShadowTransmittance(shOrigin, norm(sunDir - (sT1 * cos(th0) + sT2 * sin(th0)) * rProbe), maxShadowDist, evalCaustics, occ1);
                bool glassMod0 = (dot(tr0, float3(0.333, 0.334, 0.333)) < 0.92);
                bool glassMod1 = (dot(tr1, float3(0.333, 0.334, 0.333)) < 0.92);
                if (occ0 || glassMod0) ++shadowHits;
                if (occ1 || glassMod1) ++shadowHits;
                queries += 2u * qPerShadow;

                bool anyCaustic = (glassMod0 != glassMod1) || any(tr0 > 1.02.xxx) || any(tr1 > 1.02.xxx);
                bool needPenumbraRefine = (occ0 != occ1) || anyCaustic || (!exploreMode() && dist < 1.35) || (matId == 29u || matId == 36u);
                if (!needPenumbraRefine) {
                    sunTrans = 0.5 * (tr0 + tr1);
                } else {
                    float3 trAcc = tr0 + tr1;
                    [loop]
                    for (uint s = 2u; s < maxShadowSamples; ++s) {
                        float r = sqrt(((float)s + (tempSamp ? accumJit : 0.5)) / (float)maxShadowSamples) * sunRadius;
                        float theta = (float)s * 2.3999632 + (tempSamp ? rotPhi : rotPhi * 0.18);
                        float3 jitDir = norm(sunDir + (sT1 * cos(theta) + sT2 * sin(theta)) * r);
                        bool occS = false;
                        float3 trS = traceShadowTransmittance(shOrigin, jitDir, maxShadowDist, evalCaustics, occS);
                        if (occS || dot(trS, float3(0.333, 0.334, 0.333)) < 0.92) ++shadowHits;
                        trAcc += trS;
                        queries += qPerShadow;
                    }
                    sunTrans = trAcc / (float)maxShadowSamples;
                }
            }
        }

        // Bathroom: Cosine-Weighted DXR Hemisphere GI + Multi-Scale Contact AO (4mm crevices -> 3.85m window portal!)
        if (inBath && matId != 34u) {
            float3 gB1, gB2;
            buildOrthonormalBasis(n, gB1, gB2);
            float3 giAcc = (float3)0.0;
            float occAcc = 0.0;
            float hitTAcc = 0.0;
            float3 giOrigin = i.world + nGeom * 0.010;
            uint giSamples = (accumulating() && accumFrame >= 2u) ? 2u : 4u;
            [loop]
            for (uint g = 0u; g < giSamples; ++g) {
                float uRad = ((float)g + accumJit) / (float)giSamples;
                float sinTheta = sqrt(uRad * 0.86);
                float cosTheta = sqrt(max(1.0 - sinTheta * sinTheta, 0.04));
                float phi = (float)g * 2.3999632 + (tempSamp ? rotPhi : rotPhi * 0.25);
                float3 giDir = norm(n * cosTheta + (gB1 * cos(phi) + gB2 * sin(phi)) * sinTheta);
                if (dot(giDir, nGeom) < 0.06) {
                    giDir = norm(giDir + nGeom * (0.08 - dot(giDir, nGeom)));
                }
                float3 sampleGi;
                float sampleOcc;
                float sampleHitT;
                traceBathroomGiRay(giOrigin, giDir, sampleGi, sampleOcc, sampleHitT);
                if (sampleOcc > 0.05) ++aoHits;
                giAcc += sampleGi;
                occAcc += sampleOcc;
                hitTAcc += sampleHitT;
                ++queries;
            }
            float invGi = 1.0 / (float)giSamples;
            tracedBathGi = giAcc * invGi;
            rtContactAO = clamp(1.0 - 0.90 * (occAcc * invGi), 0.05, 1.0);
            diffHitDist = clamp(hitTAcc * invGi, 0.02, 4.0);
            hasTracedBathGi = true;
        } else if (dist < (isGrandTutti() ? 8.5 : 13.5) && matId != 34u && matId != 21u && matId != 17u) {
            // Exterior / Sanctuary / Cathedral: distance-adaptive 3-ray close / 1-ray mid Contact AO
            float3 aB1, aB2;
            buildOrthonormalBasis(nGeom, aB1, aB2);
            uint aoCount = isGrandTutti() ? 1u : ((dist < 7.2 || tempSamp) ? 3u : 1u);
            float occSum = 0.0;
            [loop]
            for (uint a = 0u; a < aoCount; ++a) {
                float ang = (float)a * (kTwoPi / 3.0) + (tempSamp ? rotPhi : rotPhi * 0.28);
                float3 aoDir = norm(nGeom * 0.72 + (aB1 * cos(ang) + aB2 * sin(ang)) * 0.694);
                float wOcc = traceAmbientRay(i.world + nGeom * 0.010, aoDir, 0.65);
                if (wOcc > 0.05) ++aoHits;
                occSum += wOcc;
                ++queries;
            }
            rtContactAO = 1.0 - 0.62 * (occSum / (float)aoCount);
            diffHitDist = clamp(rtContactAO * 2.4, 0.08, 2.4);
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
                    restirDirect = fullEval * risW * (inBath ? 0.48 : 0.38);
                }
            }
        }
    }

    if (!inBath) {
        shaftInscatter = rtLightShafts(-v, dist, frac(ign + 0.3710), queries);
    }

    rayQueryCount = queries;
    uint waveQueries = WaveActiveSum(queries);
    if (canonical != 0u && WaveIsFirstLane()) {
        InterlockedAdd(verified[1], waveQueries);
    }
    if (canonical != 0u && (((uint)i.pos.x & 31u) == 0u) && (((uint)i.pos.y & 31u) == 0u)) {
        uint anyHitCount = (reflHit ? 1u : 0u) + (refrHit ? 1u : 0u) + shadowHits + aoHits;
        InterlockedAdd(verified[0], max(anyHitCount, (queries > 0u) ? 1u : 0u));
    }
#endif

    float sunVisScalar = saturate(dot(sunTrans, float3(0.333333, 0.333334, 0.333333)));
    float combinedAO = texAO * rtContactAO;
    float3 neural = tensorField.SampleLevel(linearClamp, frac(i.world.xz * 0.035), 0.0).rgb;

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), base, metallic);
    if (matId == 34u) {
        f0 = lerp(float3(0.95, 0.955, 0.95), float3(0.80, 0.89, 0.86), pow(1.0 - ndotv, 3.2));
    }

    // 1. Direct Sun / Key GGX + Clearcoat + Curved-Glass Caustics (restirDirect routed to denoised path!)
    float3 diffConservBase = (translucency > 0.02) ? (base * (1.0 - 0.24 * translucency)) : base;
    float3 directSunOnly = evalGgxDirect(n, v, sunDir, diffConservBase, rough, metallic, clearcoat) * sunRad * sunTrans;

    // 2. Subsurface / Botanical Leaf Translucency (backlit Calathea veins, Fern fronds, Succulent, Enamel, Towel)
    if (translucency > 0.02) {
        float backNdotL = saturate(dot(-n, sunDir));
        float backPhase = pow(saturate(dot(-v, sunDir) * 0.45 + backNdotL * 0.75), 1.65);
        float wrapPhase = (matId == 29u || matId == 36u) ? (backNdotL * 0.35) : saturate(dot(n, sunDir) * 0.45 + 0.55);
        float3 transVis = (matId == 29u || matId == 36u) ? (0.14.xxx + 0.86 * sunTrans) : (0.12.xxx + 0.88 * sunTrans);
        float3 sssTint = (matId == 29u || matId == 36u || matId == 10u || matId == 30u)
            ? base * float3(1.26, 1.18, 0.48)
            : base * float3(1.08, 0.96, 0.84);
        directSunOnly += sssTint * sunRad * translucency * (0.48 * backPhase + 0.16 * wrapPhase) * transVis;
    }

    // 3. Motivated Dawn Bathroom Window Area Specular Sheen + Warm Architectural Halo
    if (inBath) {
        float vanityOverhangMask = (i.world.x > -0.95 && i.world.x < 0.56 && i.world.y < 3.53 && i.world.z > 5.72) ? 0.14 : 1.0;
        float3 winPoint = float3(1.48, clamp(i.world.y + 0.25, 3.10, 5.45), clamp(i.world.z + 0.15, 3.00, 6.45));
        float3 toWin = winPoint - i.world;
        float winDist = length(toWin);
        float3 winL = toWin / max(winDist, 0.22);
        float winAtten = 1.0 / (1.0 + 0.24 * winDist * winDist);
        float dayT = timeRamp(336.0, 345.0);
        float3 winSkyIrr = float3(0.68, 0.76, 0.90) * dayT * winAtten * rtContactAO * vanityOverhangMask;
        if (hasTracedBathGi) {
            // Specular-only analytic window area sheen (diffuse window GI is already traced with full DXR occlusion!)
            directSunOnly += evalGgxDirect(n, v, winL, f0, max(rough, 0.14), 1.0, clearcoat * 0.5) * winSkyIrr * 0.60;
        } else {
            directSunOnly += evalGgxDirect(n, v, winL, base, max(rough, 0.14), metallic, clearcoat * 0.5) * winSkyIrr * 0.85;
        }

        // Subtle warm 2900K backlit mirror halo at (-0.18, 4.39, 6.53): gentle at bright dawn, glowing at dusk/night
        float3 toMirrorHalo = float3(clamp(i.world.x, -0.54, 0.18), clamp(i.world.y, 3.83, 4.95), 6.53) - i.world;
        float dmh = length(toMirrorHalo);
        if (dmh < 2.1 && !lampsOff()) {
            float3 mhL = toMirrorHalo / max(dmh, 0.12);
            float mhFall = lerp(1.35, 0.18, dayT) / (1.0 + 4.4 * dmh * dmh);
            directSunOnly += evalGgxDirect(n, v, mhL, base, max(rough, 0.22), metallic, 0.0) * float3(0.46, 0.32, 0.18) * mhFall * combinedAO;
        }
    } else {
        float3 rimTint = lerp(float3(0.26, 0.40, 0.58), float3(0.72, 0.56, 0.42), daylight);
        directSunOnly += evalGgxDirect(n, v, rimDir, base, max(rough, 0.22), metallic, 0.0) * rimTint * 0.42 * combinedAO;
    }

    // 4. Image-Based Lighting + Ray-Traced Bathroom Hemisphere GI + Ray-Traced Specular Blend
    float2 envBrdf = envBrdfApprox(rough, ndotv);
    float3 diffIrr = sampleIblEnv(n, 0.94, inBath);
    if (inBath) {
        if (hasTracedBathGi) {
            diffIrr = tracedBathGi * 0.90 + diffIrr * 0.10 * 0.25;
        } else {
            float sunRoomGlow = timeRamp(336.5, 346.0);
            float3 warmBounce = float3(0.14, 0.11, 0.09) * (0.45 + 0.55 * saturate(-n.y * 0.4 + 0.6));
            diffIrr = diffIrr * 0.30 + warmBounce * sunRoomGlow;
        }
    }

    float3 iblSpecEnv = sampleIblEnv(reflect(-v, n), rough, inBath);
    if (inBath) iblSpecEnv *= (0.18 + 0.82 * rtContactAO);
    float rayConfidence = hasTracedReflection ? saturate(1.0 - max(rough - 0.62, 0.0) * 2.5) : 0.0;
    if (matId == 34u || matId == 7u || matId == 12u) {
        rayConfidence = hasTracedReflection ? saturate(1.0 - rough * 0.55) : 0.0;
    }
    float3 specRadiance = lerp(iblSpecEnv, reflected, rayConfidence);

    float3 kd = (1.0 - f0) * (1.0 - metallic);
    float diffOcclusion = (inBath && hasTracedBathGi) ? (texAO * (0.16 + 0.84 * rtContactAO)) : combinedAO;
    float specOcclusion = saturate(pow(combinedAO + ndotv, exp2(-16.0 * rough - 1.0)) - 1.0 + combinedAO);
    if (matId == 34u) specOcclusion = 1.0;

    float3 diffIndirectTerm = kd * base * diffIrr * diffOcclusion;
    float2 coatBrdf = envBrdfApprox(0.06, ndotv);
    float3 coatWeight = (clearcoat > 0.02) ? ((0.04 * coatBrdf.x + coatBrdf.y) * clearcoat * 0.65).xxx : 0.0.xxx;
    float3 coatRefl = hasTracedReflection ? lerp(reflected, iblSpecEnv, 0.12) : sampleIblEnv(reflect(-v, n), 0.06, inBath);
    float3 specIndirectTerm = (specRadiance * (f0 * envBrdf.x + envBrdf.y) + coatRefl * coatWeight) * specOcclusion;

    float3 emissiveTerm = districtFacade ? facadeLight : emissive(matId, i.world, dist * 0.0022);
    float3 nonIndirectColor = directSunOnly + emissiveTerm;
    float3 indirectColor = diffIndirectTerm + specIndirectTerm + restirDirect;

    // Subtle upward vertiport pad bounce illumination on hovering eVTOL airframe & wings at night
    if (act == 1u && i.world.y > 3.72 && i.world.y < 7.2) {
        float padDist = length((i.world - float3(0.0, 3.70, -2.0)) * float3(0.22, 0.38, 0.22));
        float padBounce = exp(-padDist * padDist * 2.2) * (0.35 + 0.65 * saturate(-n.y)) * (1.0 - daylight);
        nonIndirectColor += base * float3(0.26, 0.68, 0.62) * padBounce;
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
        nonIndirectColor += (caustCol * energy + neural * (caustBoost * 0.85)) * (0.25 + 0.75 * wave) * ((((showFlags >> 2u) & 7u) == 2u) ? 1.0 : 0.14);
    }

    // Symbiotic bioluminescent micro-flora garden domes
    if (matId == 22u) {
        float domeGlow = pow(1.0 - ndotv, 1.8);
        float3 bioHue = lerp(float3(0.10, 0.92, 0.68), float3(0.98, 0.68, 0.24), clamp(moodAccent, 0.0, 1.0));
        if ((showFlags & 2u) != 0u) bioHue = lerp(bioHue, float3(0.92, 0.26, 0.78), 0.45);
        nonIndirectColor += bioHue * (0.04 + 0.12 * domeGlow) * (0.75 + 0.30 * moodBloom + 0.15 * moodLight + 0.08 * moodCut) + neural * 0.025;
    }

    float transScale = 1.0;
    // Dedicated architectural spotlighting on Taipei 101's 88F-91F 5.5 m Tuned Mass Damper sphere
    if (matId == 8u && act == 2u && i.world.y > 30.0 && i.world.y < 36.0) {
        float upCatch = 0.38 + 0.62 * saturate(-n.y * 0.45 + 0.55);
        float3 tmdLift = base * float3(0.45, 0.32, 0.12) * upCatch;
        nonIndirectColor = nonIndirectColor * 1.45 + tmdLift;
        indirectColor *= 1.45;
        transScale *= 1.45;
    }

    float3 color = nonIndirectColor + indirectColor;

    // 5. Solid 2-Interface & Thin-Walled Dielectric Glass: Crystal (3), Celadon (4), Smoked Canopy (18), Amber Bottle (35), Float Glass (40), Droplets & Marbles
    if (isGlass) {
        float3 sideCrystalDir = norm(float3(0.78, 0.42, -0.46));
        float specKey  = pow(saturate(dot(n, norm(v + sunDir))), 220.0) * sunVisScalar;
        float specSide = pow(saturate(dot(n, norm(v + sideCrystalDir))), 46.0);
        float specRim  = pow(saturate(dot(n, norm(v + rimDir))), 68.0);
        float specUp   = pow(saturate(dot(n, norm(v - upSanctuaryDir))), 48.0);
        float tirBoost = glassTir ? 1.25 : 1.0;

        if (matId == 18u) {
            float3 canopyTrans = transmitted * float3(0.14, 0.17, 0.22) + float3(0.010, 0.014, 0.020);
            float canopyReflMix = clamp(0.06 + 0.50 * fres, 0.06, 0.66);
            color = lerp(canopyTrans, specRadiance * 0.92, canopyReflMix)
                  + specKey  * sunRad * 0.85
                  + specSide * float3(0.10, 0.13, 0.16)
                  + specRim  * float3(0.22, 0.28, 0.35);
        } else if (matId == 35u) {
            // Rich Hand-Blown Cognac-Amber Apothecary Glass (measured 3D Beer-Lambert optical path, zero milky floor!)
            float optPathAmb = max(glassThickness3D, clamp(i.uv.z, 0.028, 0.12) / max(ndotv, 0.24));
            float3 beerAmb = exp(-float3(5.8, 34.0, 96.0) * optPathAmb);
            float3 ambTrans = transmitted * beerAmb * tirBoost;
            float ambReflMix = clamp(0.042 + 0.86 * fres, 0.042, 0.90);
            color = lerp(ambTrans, specRadiance, ambReflMix)
                  + specKey  * sunRad * 1.20
                  + specSide * float3(0.10, 0.06, 0.025);
        } else if (matId == 40u) {
            float thickPane = clamp(i.uv.z, 0.008, 0.04) / max(ndotv, 0.28);
            float3 beerLowIron = exp(-float3(0.28, 0.10, 0.14) * thickPane * 1.8);
            float paneReflMix = clamp(0.018 + 0.54 * fres, 0.018, 0.60);
            color = lerp(transmitted * beerLowIron, specRadiance * 0.88, paneReflMix)
                  + specKey * sunRad * 0.95;
        } else if (isWaterDroplet) {
            float dropReflMix = clamp(0.026 + 0.88 * fres, 0.026, 0.88);
            color = lerp(transmitted * float3(0.98, 0.99, 1.0) * tirBoost, specRadiance, dropReflMix)
                  + specKey  * sunRad * 1.45
                  + specSide * float3(0.14, 0.17, 0.21);
        } else {
            // Blown Botanical Liuli Crystal Glass Orchid (3), Celadon Keepsake Vase & Sepals (4), and Crystal/Jade Marbles
            float uBlade = saturate(i.uv.x);
            float vBlade = (i.uv.y < 0.0) ? clamp(i.uv.y, -1.0, 1.0) : clamp(i.uv.y * 2.0 - 1.0, -1.0, 1.0);
            float fallbackThick = clamp(i.uv.z, (matId == 4u) ? 0.020 : 0.014, 0.10) / max(ndotv, 0.24);
            float opticalPath = max(glassThickness3D, fallbackThick);

            float3 absorbRate = (matId == 4u)
                ? (inBath ? float3(52.0, 7.8, 25.0) : float3(14.5, 4.2, 6.8))
                : float3(2.8, 1.35, 1.45);
            float3 beer = exp(-absorbRate * opticalPath);

            float edgeRim = pow(1.0 - ndotv, 2.5);
            float crispMargin = pow(saturate(abs(vBlade)), 5.0) * smoothstep(0.10, 0.30, uBlade);
            float midribDepth = exp(-14.0 * vBlade * vBlade) * (1.0 - 0.45 * uBlade);
            float heartWarmth = (act == 2u && !inBath && !isDynamicMarble) ? (exp(-uBlade * 5.6) * (1.0 - 0.45 * abs(vBlade))) : 0.0;

            float3 bodyScatter = isDynamicMarble
                ? ((matId == 4u) ? float3(0.008, 0.038, 0.024) : 0.0.xxx)
                : ((matId == 4u)
                    ? float3(0.012, 0.048, 0.032) * (0.55 + 0.70 * midribDepth) * (inBath ? 0.85 : 1.0)
                    : float3(0.010, 0.015, 0.018) * (0.25 + 0.75 * midribDepth) * (inBath ? 0.30 : 1.0));
            float3 rimSheen = ((matId == 4u) ? float3(0.18, 0.30, 0.28) : float3(0.26, 0.32, 0.36))
                            * (edgeRim + 0.40 * crispMargin) * (inBath ? 0.32 : 1.0);
            float3 nectarGlow = float3(0.32, 0.19, 0.07) * heartWarmth * (1.0 + 0.65 * audioBeat());

            float3 redSky  = inBath ? sampleIblEnv(refract(-v, n, 1.0 / 1.488), 0.08, true) : sky(norm(refract(-v, n, 1.0 / 1.488)));
            float3 blueSky = inBath ? sampleIblEnv(refract(-v, n, 1.0 / 1.512), 0.08, true) : sky(norm(refract(-v, n, 1.0 / 1.512)));
            float3 dispersion = float3(redSky.r - transmitted.r, 0.0, blueSky.b - transmitted.b) * (0.07 * edgeRim);
            float3 glassTrans = max(transmitted * tirBoost + dispersion, 0.0.xxx) * beer + bodyScatter * (1.0 - beer) + nectarGlow;
            float reflMix = isThinCrystalPetal
                ? clamp(0.032 + 0.48 * fres, 0.032, 0.56)
                : clamp(0.044 + 0.85 * fres, 0.044, 0.92);
            // Thin-film iridescence on exterior orchid petals: thickness varies along the blade and midrib.
            float3 film = float3(1.0, 1.0, 1.0);
            if (!inBath && !isDynamicMarble && !isWaterDroplet) {
                float filmNm = 280.0 + 220.0 * uBlade + 120.0 * abs(vBlade) + 40.0 * sin(time * 0.35 + uBlade * 6.0) + 75.0 * audioHigh();
                film = lerp(float3(1.0, 1.0, 1.0), thinFilmInterference(ndotv, filmNm) * 1.25, saturate(0.10 + 0.55 * edgeRim));
            }
            color = lerp(glassTrans, specRadiance * float3(1.02, 1.04, 1.06) * film, reflMix)
                  + rimSheen * film
                  + specKey  * sunRad * 1.05
                  + specSide * float3(0.18, 0.22, 0.28) * (inBath ? 0.28 : 1.0)
                  + specRim  * float3(0.38, 0.48, 0.58) * (inBath ? 0.25 : 1.0)
                  + specUp   * float3(0.45, 0.28, 0.12) * ((act == 2u && !inBath && !isDynamicMarble) ? 1.0 : 0.0)
                  + neural * (isDynamicMarble ? 0.0 : (0.010 * edgeRim));
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
        float specLip = pow(saturate(dot(n, norm(v + sunDir))), 145.0) * sunVisScalar;
        float3 rubyCore = base * 0.65 + float3(0.14, 0.010, 0.058);
        float3 nectarAmber = float3(0.78, 0.44, 0.09);
        float3 lipTint = lerp(rubyCore, nectarAmber, saturate(throat * 0.85 + throatVeins));
        float diffLip = 0.20 + 0.80 * saturate(dot(n, sunDir)) * sunVisScalar;
        color = lipTint * diffLip * (inBath ? 1.25 : 1.0)
              + specRadiance * (0.38 * fres)
              + float3(0.56, 0.08, 0.24) * (rimRuby * (inBath ? 0.24 : 0.48))
              + float3(0.24, 0.13, 0.03) * throat * (inBath ? 0.45 : 1.0)
              + specLip * sunRad * 0.55;
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

    if (matId == 17u) {
        color = lerp(specRadiance, float3(0.62, 0.68, 0.74), 0.35) * (0.28 + 0.45 * daylight) + float3(0.04, 0.07, 0.10);
    }

    // Layered atmospheric depth for exterior vistas ONLY (never fog indoor architectural rooms, and keep upper Taipei 101 crisp at dawn!)
    if (!inBath) {
        float heightAtten = isDawnFinale()
            ? exp(-max(i.world.y - 2.0, 0.0) * 0.14)
            : exp(-max(i.world.y, 0.0) * 0.055);
        float fogDensity = isDawnFinale()
            ? (0.00055 + 0.0021 * heightAtten)
            : ((act == 1u) ? lerp(0.0048, 0.0024, daylight) : ((act == 2u) ? (0.0038 + 0.0045 * heightAtten) : 0.0055));
        float fogStart = isDawnFinale() ? 95.0 : 4.5;
        float fog = 1.0 - exp(-max(dist - fogStart, 0.0) * fogDensity);
        float fogW = fog * (isDawnFinale() ? 0.36 : 0.74);
        float3 fogSky = skyAerialFog(norm(i.world - eye));
        color = lerp(color, fogSky, fogW);
        nonIndirectColor = lerp(nonIndirectColor, fogSky, fogW);
        transScale *= (1.0 - fogW);
        color += shaftInscatter;
        nonIndirectColor += shaftInscatter;
    }

    if (showFlags == 0u) {
        float mist = 1.0 - smoothstep(0.0, 2.0, min(abs(time - 60.0), abs(time - 120.0)));
        float3 mistCol = float3(0.28, 0.33, 0.36);
        color = lerp(color, mistCol, mist);
        nonIndirectColor = lerp(nonIndirectColor, mistCol, mist);
        transScale *= (1.0 - mist);
    }

    uint curAov = aovMode();
    if (curAov != 0u) {
        if (curAov == 1u) {
            color = base;
        } else if (curAov == 2u) {
            color = n * 0.5 + 0.5;
        } else if (curAov == 3u) {
            color = float3(rough, metallic, combinedAO);
        } else if (curAov == 4u) {
            color = directSunOnly + restirDirect;
        } else if (curAov == 5u) {
            color = diffIndirectTerm;
        } else if (curAov == 6u) {
            color = isGlass ? color : specIndirectTerm;
        } else {
            color = evalAovHeatmap((float)rayQueryCount / 18.0);
        }
    }

    Targets o;
    o.distance = dist;

#if defined(M2_RT)
    bool isDenoisable = !isGlass &&
                        (matId != 34u) &&
                        (matId != 12u) &&
                        !(matId == 9u && act == 2u) &&
                        (matId != 21u) &&
                        (matId != 17u) &&
                        (viewZ < 480.0);

    if (splitDenoiseOutputs() && curAov == 0u && isDenoisable) {
        float3 diffWeight = kd * base * texAO;
        float3 diffModBase = max(diffWeight, float3(0.02, 0.02, 0.02));
        float3 rawDiffIrr = (diffIndirectTerm + restirDirect * (1.0 - metallic)) / diffModBase;

        float3 specWeight = (f0 * envBrdf.x + envBrdf.y) + coatWeight;
        float3 specModBase = max(specWeight, float3(0.02, 0.02, 0.02));
        float3 rawSpecRad = (specIndirectTerm + restirDirect * metallic) / specModBase;

        float3 outDiffMod = diffModBase * transScale;
        float3 outSpecMod = specModBase * transScale;

        o.color      = float4(max(nonIndirectColor, 0.0.xxx), 1.0);
        o.normRough  = float4(n, rough);
        o.diffRadHit = float4(clamp(rawDiffIrr, 0.0.xxx, 65504.0.xxx), diffHitDist);
        o.specRadHit = float4(clamp(rawSpecRad, 0.0.xxx, 65504.0.xxx), specHitDist);
        o.modViewZ   = uint4(packHalf2(outDiffMod.rg), packHalf2(float2(outDiffMod.b, outSpecMod.r)), packHalf2(outSpecMod.gb), asuint(viewZ));
        o.worldMv    = float4(unpackDrawMotion(i.flags, i.seed), 0.0);
    } else {
        o.color      = float4(max(color, 0.0.xxx), 1.0);
        o.normRough  = float4(n, rough);
        o.diffRadHit = float4(0.0, 0.0, 0.0, 0.0);
        o.specRadHit = float4(0.0, 0.0, 0.0, 0.0);
        o.modViewZ   = uint4(0u, 0u, 0u, asuint(600000.0));
        o.worldMv    = float4(unpackDrawMotion(i.flags, i.seed), 0.0);
    }
#else
    o.color = float4(max(color, 0.0.xxx), 1.0);
#endif
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
    // Far plane (1.0): drawn after the opaque passes, the sky only fills pixels still at the cleared depth.
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
    return o;
}

Targets PSSky(SkyOut i) {
    float2 p = i.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float3 d = norm(forward + right * p.x * aspect * tan(fov * 0.5) + up * p.y * tan(fov * 0.5));
    uint curAov = aovMode();
    float3 skyCol = sky(d);
    if (curAov != 0u) {
        if (curAov == 2u) skyCol = (-d) * 0.5 + 0.5;
        else if (curAov == 3u) skyCol = float3(1.0, 0.0, 1.0);
        else if (curAov == 7u) skyCol = evalAovHeatmap(0.0);
        else if (curAov != 6u) skyCol = 0.0.xxx;
    }
#if defined(M2_RT)
    if (curAov == 0u) {
        // Shafts continue seamlessly across silhouettes into open sky (same jitter scheme as PSMain).
        bool tempSampSky = temporalSampling();
        float ignSky = tempSampSky
            ? frac(interleavedGradientNoise(i.pos.xy + float2(5.588238, 3.143218) * (float)(accumFrame & 255u)) + 0.6180340 * (float)accumFrame)
            : interleavedGradientNoise(i.pos.xy);
        uint skyQueries = 0u;
        skyCol += rtLightShafts(d, 90.0, frac(ignSky + 0.3710), skyQueries);
        uint waveSkyQueries = WaveActiveSum(skyQueries);
        if (canonical != 0u && WaveIsFirstLane()) {
            InterlockedAdd(verified[1], waveSkyQueries);
        }
    }
#endif
    Targets o;
    o.color = float4(skyCol, 1.0);
    o.distance = 1000.0;
#if defined(M2_RT)
    o.normRough  = float4(0.0, 1.0, 0.0, 1.0);
    o.diffRadHit = float4(0.0, 0.0, 0.0, 0.0);
    o.specRadHit = float4(0.0, 0.0, 0.0, 0.0);
    o.modViewZ   = uint4(0u, 0u, 0u, asuint(600000.0));
    o.worldMv    = float4(0.0, 0.0, 0.0, 0.0);
#endif
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


#if defined(BOO_DENOISE_RESOLVE) || defined(BOO_NRD_PACK_CS) || defined(BOO_NRD_UNPACK)
float3 nrdEncodeNormalRoughness101010(float3 n, float roughness) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    float3 r;
    r.y = n.y * 0.5 + 0.5;
    r.x = n.x * 0.5 + r.y;
    r.y -= n.x * 0.5;
    roughness = max(roughness, 1.5 / 512.0);
    float s = (n.z < 0.0) ? -roughness : roughness;
    r.z = s * 0.5 + 0.5;
    return saturate(r);
}

float3 nrdLinearToYCoCg(float3 c) {
    return float3(
        dot(c, float3( 0.25, 0.50,  0.25)),
        dot(c, float3( 0.50, 0.00, -0.50)),
        dot(c, float3(-0.25, 0.50, -0.25)));
}

float3 nrdYCoCgToLinear(float3 ycc) {
    float t = ycc.x - ycc.z;
    float3 r;
    r.y = ycc.x + ycc.z;
    r.x = t + ycc.y;
    r.z = t - ycc.y;
    return max(r, 0.0.xxx);
}

float reblurGetNormHitDist(float hitDist, float viewZ, float3 hitDistParams, float roughness) {
    float smc = (1.0 - exp2(-200.0 * roughness * roughness)) * sqrt(saturate(roughness));
    float normScale = (hitDistParams.x + abs(viewZ) * hitDistParams.y) * lerp(hitDistParams.z, 1.0, smc);
    return saturate(hitDist / max(normScale, 1e-4));
}
#endif


#if defined(BOO_DENOISE_RESOLVE)
// Stateless, 100% deterministic normal/depth/roughness edge-stopping cross-bilateral GI & specular
// demodulated resolve pass. Eliminates 1-SPP IGN hemisphere noise while keeping albedo & micro-normals razor sharp!
cbuffer DenoiseResolveCB : register(b0, space1) {
    uint2 gResTexSize;
    float gResSpatialScale;
    uint  gResFlags;
};

Texture2D<float4> gResNormRough  : register(t0, space1);
Texture2D<float4> gResDiffRadHit : register(t1, space1);
Texture2D<float4> gResSpecRadHit : register(t2, space1);
Texture2D<uint4>  gResModViewZ   : register(t3, space1);

float4 PSDeterministicResolve(float4 pos : SV_Position) : SV_Target0 {
    int2 p = int2(pos.xy);
    float3 diffMod;
    float3 specMod;
    float centerZ;
    unpackModViewZ(gResModViewZ.Load(int3(p, 0)), diffMod, specMod, centerZ);
    if (centerZ > 500.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    float4 nr0 = gResNormRough.Load(int3(p, 0));
    float4 d0  = gResDiffRadHit.Load(int3(p, 0));
    float4 s0  = gResSpecRadHit.Load(int3(p, 0));
    if (gResSpatialScale <= 0.01) {
        return float4(max(d0.rgb * diffMod + s0.rgb * specMod, 0.0.xxx), 0.0);
    }

    float3 n0 = norm(nr0.xyz);
    float rough0 = clamp(nr0.w, 0.02, 0.99);
    float diffRadPx = gResSpatialScale * clamp(2.4 + d0.w * 4.4, 2.4, 10.5);
    float specRadPx = (rough0 < 0.065)
        ? 0.0
        : (gResSpatialScale * clamp(rough0 * 14.0 * saturate(0.25 + s0.w * 0.65), 1.2, 5.5));

    float dLum0 = dot(d0.rgb, float3(0.2126, 0.7152, 0.0722));
    float sLum0 = dot(s0.rgb, float3(0.2126, 0.7152, 0.0722));
    float dClampMax = max(dLum0 * 5.0 + 2.5, 3.0);
    float sClampMax = max(sLum0 * 6.0 + 4.0, 6.0);

    float3 diffAcc = d0.rgb;
    float  diffW   = 1.0;
    float3 specAcc = s0.rgb;
    float  specW   = 1.0;

    int2 maxCoord = int2(gResTexSize) - int2(1, 1);
    float zScale = 1.0 / max(0.018 * centerZ, 0.008);
    float specNormExp = lerp(128.0, 24.0, rough0);

    // Deterministic per-pixel rotation from integer coordinates (breaks regular grid banding with zero temporal state)
    float baseAng = interleavedGradientNoise(pos.xy) * kTwoPi;

    [unroll]
    for (uint k = 0u; k < 16u; ++k) {
        float rk = sqrt(((float)k + 0.5) / 16.0);
        float th = (float)k * 2.3999632 + baseAng;
        float2 unitOff = float2(cos(th), sin(th)) * rk;

        int2 qd = clamp(p + int2(round(unitOff * diffRadPx)), int2(0, 0), maxCoord);
        uint4 md = gResModViewZ.Load(int3(qd, 0));
        float zd = asfloat(md.w);
        if (zd < 500.0) {
            float4 nrdTap = gResNormRough.Load(int3(qd, 0));
            float3 nd = norm(nrdTap.xyz);
            float dz = (zd - centerZ) * zScale;
            float wZ = exp(-dz * dz * 3.5);
            float wN = pow(saturate(dot(n0, nd)), 28.0);
            float wK = exp(-rk * rk * 1.35) * wZ * wN;
            if (wK > 1e-4) {
                float3 dTap = gResDiffRadHit.Load(int3(qd, 0)).rgb;
                float dLum = dot(dTap, float3(0.2126, 0.7152, 0.0722));
                if (dLum > dClampMax) dTap *= (dClampMax / dLum);
                diffAcc += dTap * wK;
                diffW += wK;
            }
        }

        if (specRadPx > 0.4) {
            int2 qs = clamp(p + int2(round(unitOff * specRadPx)), int2(0, 0), maxCoord);
            uint4 ms = gResModViewZ.Load(int3(qs, 0));
            float zs = asfloat(ms.w);
            if (zs < 500.0) {
                float4 nrsTap = gResNormRough.Load(int3(qs, 0));
                float3 ns = norm(nrsTap.xyz);
                float dzs = (zs - centerZ) * zScale;
                float wZs = exp(-dzs * dzs * 4.5);
                float wNs = pow(saturate(dot(n0, ns)), specNormExp);
                float wRs = exp(-abs(rough0 - nrsTap.w) * 18.0);
                float wKs = exp(-rk * rk * 1.65) * wZs * wNs * wRs;
                if (wKs > 1e-4) {
                    float3 sTap = gResSpecRadHit.Load(int3(qs, 0)).rgb;
                    float sLum = dot(sTap, float3(0.2126, 0.7152, 0.0722));
                    if (sLum > sClampMax) sTap *= (sClampMax / sLum);
                    specAcc += sTap * wKs;
                    specW += wKs;
                }
            }
        }
    }

    float3 resolved = (diffAcc / diffW) * diffMod + (specAcc / specW) * specMod;
    return float4(max(resolved, 0.0.xxx), 0.0);
}
#endif


#if defined(BOO_NRD_PACK_CS)
// Packs G-buffer world normals, linear roughness, linear viewZ, and demodulated diffuse/specular
// radiance + normalized hit distances into NVIDIA NRD v4.18 REBLUR_DIFFUSE_SPECULAR front-end textures.
cbuffer NrdPackCB : register(b0, space1) {
    uint2 gPackTexSize;
    uint2 gPackPad;
};

Texture2D<float4> gPackNormRough  : register(t0, space1);
Texture2D<float4> gPackDiffRadHit : register(t1, space1);
Texture2D<float4> gPackSpecRadHit : register(t2, space1);
Texture2D<uint4>  gPackModViewZ   : register(t3, space1);

RWTexture2D<unorm float4> uNrdNormRough : register(u0, space1);
RWTexture2D<float>        uNrdViewZ     : register(u1, space1);
RWTexture2D<float4>       uNrdDiffIn    : register(u2, space1);
RWTexture2D<float4>       uNrdSpecIn    : register(u3, space1);

[numthreads(8, 8, 1)]
void CSPackNrdReblur(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= gPackTexSize.x || dtid.y >= gPackTexSize.y) {
        return;
    }

    uint2 px = dtid.xy;
    uint4 m = gPackModViewZ[px];
    float viewZ = asfloat(m.w);
    float4 nr = gPackNormRough[px];
    float3 N = norm(nr.xyz);
    float rough = clamp(nr.w, 0.02, 0.99);

    uNrdNormRough[px] = float4(nrdEncodeNormalRoughness101010(N, rough), 0.0);
    uNrdViewZ[px] = viewZ;

    if (viewZ > 500.0) {
        uNrdDiffIn[px] = float4(0.0, 0.0, 0.0, 0.0);
        uNrdSpecIn[px] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    float4 d = gPackDiffRadHit[px];
    float4 s = gPackSpecRadHit[px];
    float3 hitDistParams = float3(3.0, 0.1, 20.0);
    float normDiffHit = reblurGetNormHitDist(max(d.w, 0.0), viewZ, hitDistParams, 1.0);
    float normSpecHit = reblurGetNormHitDist(max(s.w, 0.0), viewZ, hitDistParams, rough);

    uNrdDiffIn[px] = float4(nrdLinearToYCoCg(clamp(d.rgb, 0.0.xxx, 65504.0.xxx)), normDiffHit);
    uNrdSpecIn[px] = float4(nrdLinearToYCoCg(clamp(s.rgb, 0.0.xxx, 65504.0.xxx)), normSpecHit);
}
#endif


#if defined(BOO_NRD_UNPACK)
// Unpacks NRD v4.18 REBLUR_DIFFUSE_SPECULAR YCoCg outputs and re-modulates with full-res material factors.
cbuffer DenoiseUnpackCB : register(b0, space1) {
    uint2 gUnpackTexSize;
    float gUnpackPad0;
    uint  gUnpackPad1;
};

Texture2D<float4> gNrdOutDiff     : register(t0, space1);
Texture2D<float4> gNrdOutSpec     : register(t1, space1);
Texture2D<float4> gUnpackNormRough: register(t2, space1);
Texture2D<uint4>  gUnpackModViewZ : register(t3, space1);

float4 PSUnpackNrdReblur(float4 pos : SV_Position) : SV_Target0 {
    int2 p = int2(pos.xy);
    float3 diffMod;
    float3 specMod;
    float viewZ;
    unpackModViewZ(gUnpackModViewZ.Load(int3(p, 0)), diffMod, specMod, viewZ);
    if (viewZ > 500.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    float4 dOut = gNrdOutDiff.Load(int3(p, 0));
    float4 sOut = gNrdOutSpec.Load(int3(p, 0));
    float3 diffLin = nrdYCoCgToLinear(dOut.xyz);
    float3 specLin = nrdYCoCgToLinear(sOut.xyz);
    return float4(max(diffLin * diffMod + specLin * specMod, 0.0.xxx), 0.0);
}
#endif
