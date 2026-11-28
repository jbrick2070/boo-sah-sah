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
    uint accumFrame; uint interFlags; float2 jitterNdc; // bit1 = accumulation, bit2 = lamps off, bits15:16 = diffuse path budget
    float4 courierLamp; // world-space underside lamp origin and intensity
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
#endif
RWStructuredBuffer<uint> verified : register(u0);

Texture2D<float4>      tensorField     : register(t3);
Texture2DArray<float4> gAlbedoAlphaTex : register(t4);
Texture2DArray<float4> gNormalTex      : register(t5);
Texture2DArray<float4> gOrmtTex        : register(t6);
Texture2DArray<float4> gEnvHdrTex      : register(t7);

Texture2DArray<float4> roomFaces : register(t10);

#if defined(BOO_GLASS_REFRACTION)
Texture2D<float4> gSceneColorCopy : register(t13);
Texture2D<float> gSceneDepthCopy : register(t14);
#endif

SamplerState linearClamp     : register(s0);
SamplerState anisoWrap       : register(s1);
SamplerState equirectSampler : register(s2);
SamplerState linearWrap      : register(s3);

static const float kPi = 3.14159265;
static const float kTwoPi = 6.28318530;
static const float kSceneFar = 3200.0; // Includes Taipei's surveyed northern mountain range.

bool liteMode() { return (interFlags & (1u << 20u)) != 0u; }

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
    return ((showFlags & 65u) == 1u) && (time >= 274.5) && (time < 338.0);
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

// "Last Light" (include/boo/last_light.hpp): the full film's opening at dusk on the Taipei terrace.
// Bit 64 of showFlags is set by the full film only (solo loops excluded), during its first 45 s.
bool duskOpening() {
    return (showFlags & 65u) == 65u && act == 2u && time < 45.0 && !sunOverridden();
}

// The setting sun of the opening: low in the west-south-west (behind the banner as the camera first
// sees it), dropping below the horizon between 14 and 30 s.
float3 duskSunDir() {
    float e = radians(lerp(11.0, -5.0, smoothstep(14.0, 30.0, time)));
    return norm(float3(-0.92 * cos(e), sin(e), -0.38 * cos(e)));
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
    return (interFlags & (2u | 32u | (1u << 18u))) != 0u;
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
    // The opening's sun until it has set (its light is gone by 26 s), then the night key.
    if (duskOpening() && time < 26.0) {
        return duskSunDir();
    }
    // Over the open city the moon stands lower (30 degrees, from the south-south-east), so the towers'
    // faces take a raking light and keep their form at night. The terrace keeps its high key.
    if (act == 2u && ((showFlags & 64u) != 0u || showFlags == 0u)) {   // cityExterior(), defined below
        return norm(float3(0.36, 0.52, -0.78));
    }
    return norm(float3(-0.44, 0.74, -0.32));
}

// Two theatrical spots mounted over the orchid. The gel wheel, pan and dimmer use
// exactly the same transfer function for the neural and scripted stage directors.
bool sanctuarySpotsOn() {
    return (showFlags & 32u) != 0u && !lampsOff();
}

struct SanctuarySpot { float3 origin; float3 axis; float3 radiance; };

SanctuarySpot sanctuarySpot(float side) {
    SanctuarySpot spot;
    spot.origin = float3(side * 2.9, 8.0, 4.0);
    float travel = ((interFlags & (1u << 14u)) != 0u) ? 0.45f : 1.0f;
    float pan = (moodLight - 0.5) * 5.0;
    float gel = moodAccent * 10.0 + moodBloom * 2.5;
    float3 target = float3(side * 0.25 + sin(pan + side * 0.65) * 2.65 * travel,
                           4.3 + (moodBloom - 0.45) * 2.1 * travel, 5.55 + side * 0.35);
    spot.axis = norm(target - spot.origin);
    float wheel = 0.5 + 0.5 * travel * sin(gel + side * 1.4);
    float3 cool = side < 0.0 ? float3(0.10, 0.80, 1.00) : float3(0.35, 0.19, 0.85);
    float3 warm = side < 0.0 ? float3(0.96, 0.24, 0.38) : float3(1.00, 0.53, 0.18);
    spot.radiance = lerp(cool, warm, wheel) * (1.05 + 0.80 * (moodBloom - 0.5) * travel);
    return spot;
}

float3 sanctuaryBeam(SanctuarySpot spot, float3 ray, float surfaceDistance) {
    float3 fromSpot = eye - spot.origin;
    float dv = dot(ray, spot.axis);
    float t = clamp((dv * dot(fromSpot, spot.axis) - dot(fromSpot, ray)) /
                    max(1.0 - dv * dv, 0.02), 0.0, min(surfaceDistance, 16.0));
    float3 path = fromSpot + t * ray;
    float along = dot(path, spot.axis);
    float radius = 0.08 + 0.17 * max(along, 0.0);
    float3 radial = path - along * spot.axis;
    float core = exp2(-2.0 * dot(radial, radial) / (radius * radius));
    float reach = smoothstep(0.0, 0.5, along) * (1.0 - smoothstep(4.8, 7.0, along));
    return spot.radiance * core * reach * (0.36 + 0.48 * moodBloom);
}

float3 sanctuaryHaze(float3 ray, float distanceToSurface) {
    return sanctuaryBeam(sanctuarySpot(-1.0), ray, distanceToSurface) +
           sanctuaryBeam(sanctuarySpot(1.0), ray, distanceToSurface);
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
    if (duskOpening()) {
        // Low gold sunlight, reddening and fading as the sun meets the horizon; the night key rises after
        // it has set, so the light's direction never jumps while it is lit.
        if (time < 26.0) {
            float elevation = duskSunDir().y;
            float3 sunCol = lerp(float3(3.30, 1.70, 0.72), float3(3.60, 2.45, 1.30), smoothstep(0.02, 0.19, elevation));
            return sunCol * smoothstep(-0.03, 0.06, elevation);
        }
        return nightKey * smoothstep(26.0, 31.0, time);
    }
    return lerp(nightKey, float3(1.85, 1.32, 0.88), daylight);
}

// <sky-table>
// Generated by tools/gen_sky_table.py; do not edit by hand. A scattering sky for 10 sun elevations,
// each under its own exposure. The big tables are packed (9-bit mantissas, shared 5-bit exponent);
// index = (sunKnot * kSkyElevSteps + viewElevation) * kSkyAzimSteps + azimuth, with view elevation
// (0..90 degrees) on a square-root scale and azimuth from the sun (0..180) on a power scale.
static const uint kSkySunKnots = 10u;
static const uint kSkyElevSteps = 8u;
static const uint kSkyAzimSteps = 9u;
static const float kSkyAzimPower = 1.5;
static const float kSkyMieG = 0.78;
static const float kSkySunElev[10] = { -6.5, -4.5, -3.0, -1.5, 0.0, 2.0, 5.0, 11.0, 25.0, 60.0 };
// Sky irradiance / pi on an up-facing surface.
static const float3 kSkyAmbUp[10] = {
    float3(0.0582, 0.08392, 0.1746),
    float3(0.08222, 0.1077, 0.2147),
    float3(0.1125, 0.1319, 0.2317),
    float3(0.1332, 0.1534, 0.2352),
    float3(0.1414, 0.1715, 0.2389),
    float3(0.1311, 0.1864, 0.261),
    float3(0.1197, 0.1929, 0.299),
    float3(0.1174, 0.2017, 0.357),
    float3(0.1343, 0.2351, 0.461),
    float3(0.1613, 0.2842, 0.5878),
};
// Sky irradiance / pi on a wall facing the sun's bearing.
static const float3 kSkyAmbSunward[10] = {
    float3(0.1307, 0.1202, 0.1392),
    float3(0.0973, 0.09112, 0.1161),
    float3(0.09068, 0.08407, 0.102),
    float3(0.08948, 0.08193, 0.09058),
    float3(0.1026, 0.08931, 0.08682),
    float3(0.1039, 0.1034, 0.09632),
    float3(0.0989, 0.1151, 0.1202),
    float3(0.09427, 0.1245, 0.1568),
    float3(0.09062, 0.134, 0.204),
    float3(0.07912, 0.1278, 0.2234),
};
// Sky irradiance / pi on a wall facing away from the sun.
static const float3 kSkyAmbAway[10] = {
    float3(0.004371, 0.009079, 0.02563),
    float3(0.01233, 0.01698, 0.03758),
    float3(0.02692, 0.02876, 0.04969),
    float3(0.046, 0.04414, 0.05942),
    float3(0.06257, 0.06074, 0.06753),
    float3(0.06488, 0.07691, 0.0825),
    float3(0.06111, 0.08592, 0.1041),
    float3(0.0587, 0.09107, 0.1307),
    float3(0.06155, 0.09958, 0.1635),
    float3(0.06342, 0.1046, 0.1871),
};
// Radiance of a white surface facing the sun at the ground.
static const float3 kSkySunLight[10] = {
    float3(0.0, 0.0, 0.0),
    float3(0.0, 0.0, 0.0),
    float3(0.0, 0.0, 0.0),
    float3(0.0, 0.0, 0.0),
    float3(1.043, 0.1962, 0.0),
    float3(2.477, 0.9606, 0.01962),
    float3(2.989, 1.828, 0.5051),
    float3(3.23, 2.563, 1.4),
    float3(3.582, 3.236, 2.451),
    float3(3.784, 3.605, 3.145),
};
// Radiance of a white surface facing the sun at cloud height (6 km).
static const float3 kSkyCloudSun[10] = {
    float3(0.0, 0.0, 0.0),
    float3(0.0, 0.0, 0.0),
    float3(0.0, 0.0, 0.0),
    float3(3.346, 0.6813, 0.0),
    float3(5.186, 2.235, 0.1758),
    float3(4.723, 3.181, 1.224),
    float3(4.153, 3.453, 2.178),
    float3(3.814, 3.515, 2.82),
    float3(3.874, 3.74, 3.38),
    float3(3.934, 3.868, 3.681),
};
// Rayleigh single scattering (phase applied) plus multiple scattering.
static const uint kSkyRayleigh[720] = {
    // sun -6.5 degrees
    0x88149F62u, 0x88149D5Du, 0x88208F39u, 0x8070CD97u, 0x722555AAu, 0x6C0D6C93u, 0x67162EA8u, 0x665DDA84u, 0x661DBE7Au,
    0x88991D11u, 0x88991B0Fu, 0x88990D00u, 0x8139B797u, 0x79E18539u, 0x6CEDA4A2u, 0x6C09123Fu, 0x6711CA65u, 0x66BDAE5Eu,
    0x850A42F2u, 0x85023EF0u, 0x84BA18E0u, 0x7FBB4F61u, 0x7C05869Fu, 0x6EB1BE97u, 0x6C54CA24u, 0x678D5838u, 0x67314835u,
    0x84092C66u, 0x7FEE4EC7u, 0x7EFE02AEu, 0x7CF96278u, 0x75A5727Bu, 0x6E295268u, 0x67ED5A58u, 0x6664F02Du, 0x6618DE25u,
    0x76F5AC8Au, 0x76DDA688u, 0x763D7C79u, 0x74ED225Du, 0x6ED57675u, 0x6CBCEA46u, 0x673D465Du, 0x6600FA45u, 0x6594E03Cu,
    0x6EBD6E72u, 0x6EB16A71u, 0x6E515269u, 0x6D7D2059u, 0x6C7CE043u, 0x67555866u, 0x66652255u, 0x65D9044Du, 0x65A4FA4Bu,
    0x67B97872u, 0x67B17871u, 0x67896E6Eu, 0x67255666u, 0x6699345Cu, 0x661D1852u, 0x65B9024Cu, 0x658CFA4Au, 0x657CF649u,
    0x65B1024Cu, 0x65B1024Cu, 0x65B1024Cu, 0x65B1024Cu, 0x65B1024Cu, 0x65B1024Cu, 0x65B1024Cu, 0x65B1024Cu, 0x65B1024Cu,
    // sun -4.5 degrees
    0x8068E7BAu, 0x806CE3B2u, 0x8074CB7Bu, 0x79051DE1u, 0x7225377Au, 0x6C1972C4u, 0x675632BFu, 0x66ADE098u, 0x6671C68Cu,
    0x8101C7C1u, 0x8101C3BCu, 0x8105A59Eu, 0x81014D44u, 0x79A54D2Au, 0x6CFDED20u, 0x6C19144Du, 0x674DCE77u, 0x6701B46Eu,
    0x7F97A1BAu, 0x7F8797B6u, 0x7F1F5999u, 0x7DEEAB45u, 0x7772FF69u, 0x741D589Au, 0x6CE92C73u, 0x67A96854u, 0x67514E43u,
    0x7E9E06C7u, 0x7E89FEC4u, 0x7DF1CCB1u, 0x7CAD6087u, 0x7651C2ABu, 0x74311468u, 0x6E1D7C94u, 0x6CB51673u, 0x6C2CEE64u,
    0x7765E4AEu, 0x7751DEACu, 0x76D1BA9Fu, 0x75B16882u, 0x7451065Eu, 0x6EA9848Bu, 0x6DD1527Cu, 0x6D81467Eu, 0x6D5D4280u,
    0x7438FA58u, 0x7430F857u, 0x7400EC53u, 0x6F259E90u, 0x6E095476u, 0x6D392064u, 0x6CF51261u, 0x6D0D1E69u, 0x6D1D266Du,
    0x6D3D2063u, 0x6D392063u, 0x6D211A61u, 0x6CE90C5Cu, 0x6CA0FA56u, 0x6C60EA51u, 0x6C48E650u, 0x6C50EA52u, 0x6C58EE54u,
    0x6C14D84Au, 0x6C14D84Au, 0x6C14D84Au, 0x6C14D84Au, 0x6C14D84Au, 0x6C14D84Au, 0x6C14D84Au, 0x6C14D84Au, 0x6C14D84Au,
    // sun -3.0 degrees
    0x8078BB35u, 0x8078B92Fu, 0x807CA509u, 0x7904FB70u, 0x720D4174u, 0x6BD98B10u, 0x673A6AF6u, 0x66CA1EC5u, 0x669E02B7u,
    0x80E58777u, 0x80E58374u, 0x80E5695Au, 0x80E12114u, 0x79794F33u, 0x6CB665E6u, 0x6C15448Cu, 0x677A0E9Eu, 0x6739F293u,
    0x7DFB2F9Eu, 0x7DF3279Au, 0x7DA2F381u, 0x7CCE6B3Eu, 0x76BF1F9Fu, 0x7465CEF8u, 0x6E3A4550u, 0x6C8962DDu, 0x6C0114A6u,
    0x7D8DE6CDu, 0x7D7DE0CAu, 0x7D15BABAu, 0x7C356497u, 0x7631F8D7u, 0x7499669Cu, 0x74053C93u, 0x6F966B3Cu, 0x6F526345u,
    0x770204CEu, 0x76F1FECCu, 0x7689DEBFu, 0x759D94A2u, 0x7479387Du, 0x6F55F4CBu, 0x6F25F0D1u, 0x6FAA28F6u, 0x6FF2490Au,
    0x74512272u, 0x744D2071u, 0x7421146Cu, 0x6F79ECC1u, 0x6E79A2A3u, 0x6DC57091u, 0x6DD17897u, 0x6E4DA4AEu, 0x6E95BEBBu,
    0x6DA56489u, 0x6DA16289u, 0x6D895C86u, 0x6D554C80u, 0x6D0D3A79u, 0x6CD92C74u, 0x6CD92E76u, 0x6D053C7Du, 0x6D1D4481u,
    0x6C85146Bu, 0x6C85146Bu, 0x6C85146Bu, 0x6C85146Bu, 0x6C85146Bu, 0x6C85146Bu, 0x6C85146Bu, 0x6C85146Bu, 0x6C85146Bu,
    // sun -1.5 degrees
    0x78E959F5u, 0x78E953EDu, 0x78F137BBu, 0x78F8FB4Bu, 0x71F9638Eu, 0x6BE1F197u, 0x6762EF77u, 0x66FE8F04u, 0x66D270F3u,
    0x80D96F4Fu, 0x80D96B4Cu, 0x80D55134u, 0x799223F3u, 0x79616945u, 0x7261B17Fu, 0x6C2A05BAu, 0x67B69D69u, 0x678A64D4u,
    0x7D0AF988u, 0x7CFEF385u, 0x7CBAC36Du, 0x7C124D31u, 0x763347B8u, 0x74B25544u, 0x73E9FD36u, 0x735DDB55u, 0x7311C362u,
    0x7CB1D0C8u, 0x7CA9CCC6u, 0x7C61ACB9u, 0x777ACF36u, 0x75E220ECu, 0x74C9B0BEu, 0x74A9B4CCu, 0x7509FAFEu, 0x753A1F1Au,
    0x765E14D9u, 0x76520ED7u, 0x7601F2CBu, 0x7549AEAFu, 0x745D5A8Cu, 0x6F8E4CF0u, 0x6FC26F07u, 0x746D74A4u, 0x74BD98B7u,
    0x74253C7Eu, 0x74213A7Du, 0x6FF65CEFu, 0x6F4E22D8u, 0x6E75DABAu, 0x6DEDAEAAu, 0x6E19C4B6u, 0x6ECA08D7u, 0x6F322EEAu,
    0x6D9D929Du, 0x6D9D909Cu, 0x6D898A99u, 0x6D597C93u, 0x6D1D688Cu, 0x6CF55C88u, 0x6D01628Bu, 0x6D357694u, 0x6D55829Au,
    0x6C9D427Du, 0x6C9D427Du, 0x6C9D427Du, 0x6C9D427Du, 0x6C9D427Du, 0x6C9D427Du, 0x6C9D427Du, 0x6C9D427Du, 0x6C9D427Du,
    // sun 0.0 degrees
    0x8070DD16u, 0x8070DB13u, 0x78E597FBu, 0x78ED5199u, 0x78F0FB1Du, 0x71DD6F85u, 0x71CD2530u, 0x6B81FDEDu, 0x6B75EDBBu,
    0x80E58B44u, 0x80E18741u, 0x80DD6D2Cu, 0x799A63F5u, 0x796DBF6Du, 0x793D3F0Bu, 0x72260DF4u, 0x78F0E508u, 0x78E4D30Au,
    0x7CBEFD75u, 0x7CB6F771u, 0x7C7AC95Bu, 0x7BE65D26u, 0x763393BCu, 0x750AD163u, 0x74C2D782u, 0x74DF45EAu, 0x7A75C112u,
    0x7C65D4C0u, 0x7C59D0BEu, 0x7C1DB4B2u, 0x772AEB2Fu, 0x75CE48ECu, 0x74EDE6C5u, 0x751E0EDDu, 0x75F28F21u, 0x766AD948u,
    0x761E28D5u, 0x761224D3u, 0x75CA08C7u, 0x7525C6ADu, 0x7455748Du, 0x6FA686F4u, 0x740D6088u, 0x74C5B4AEu, 0x752DE4C4u,
    0x7411507Du, 0x740D4E7Du, 0x6FD684EFu, 0x6F3A4AD9u, 0x6E6E00BCu, 0x6DF1D4ABu, 0x6E31F0B9u, 0x6EFE42DBu, 0x6F7672F0u,
    0x6D99B29Eu, 0x6D95B29Eu, 0x6D81AA9Bu, 0x6D559A95u, 0x6D19868Du, 0x6CF57A88u, 0x6D05808Cu, 0x6D3D9695u, 0x6D61A49Bu,
    0x6C9D5C7Du, 0x6C9D5C7Du, 0x6C9D5C7Du, 0x6C9D5C7Du, 0x6C9D5C7Du, 0x6C9D5C7Du, 0x6C9D5C7Du, 0x6C9D5C7Du, 0x6C9D5C7Du,
    // sun 2.0 degrees
    0x80BD471Cu, 0x80BD4519u, 0x80B93106u, 0x796207B7u, 0x794D974Bu, 0x79394905u, 0x792D410Au, 0x79255F38u, 0x79256F50u,
    0x815DD72Du, 0x8159D32Au, 0x814DB516u, 0x7A56E5D3u, 0x7A063B62u, 0x79C5CB1Cu, 0x79ADD130u, 0x79A2157Du, 0x799E39A6u,
    0x7D53234Au, 0x7D471B47u, 0x7CFEEB32u, 0x7C627F03u, 0x772BE790u, 0x76273F4Bu, 0x76578976u, 0x7BA63AF7u, 0x7BEE7F1Au,
    0x7C99DEA8u, 0x7C91DAA6u, 0x7C55C09Cu, 0x77A3070Cu, 0x764E6AD3u, 0x75820EB2u, 0x75E64ACAu, 0x7726F10Cu, 0x77E35333u,
    0x766A3CBDu, 0x766238BCu, 0x761A1CB2u, 0x7575DA9Bu, 0x74A1887Eu, 0x7425586Eu, 0x746D787Au, 0x754DD69Cu, 0x75CE0EB0u,
    0x74516271u, 0x744D6070u, 0x7429526Cu, 0x6FAE68C4u, 0x6EDE1CA9u, 0x6E5DEE9Au, 0x6EA20AA4u, 0x6F7E60C2u, 0x74014A6Au,
    0x6DF9CA8Fu, 0x6DF5CA8Fu, 0x6DE1C28Cu, 0x6DB1B287u, 0x6D719C7Fu, 0x6D458C7Au, 0x6D51927Cu, 0x6D8DA884u, 0x6DB1B488u,
    0x6CE96C70u, 0x6CE96C70u, 0x6CE96C70u, 0x6CE96C70u, 0x6CE96C70u, 0x6CE96C70u, 0x6CE96C70u, 0x6CE96C70u, 0x6CE96C70u,
    // sun 5.0 degrees
    0x8195AB0Eu, 0x8191A70Bu, 0x7B071FF6u, 0x7AC2B5ACu, 0x7A6E2D4Eu, 0x7A35DB16u, 0x7A45FF33u, 0x7A8A7188u, 0x7AB2B3B8u,
    0x824E0B09u, 0x824A0707u, 0x7C5BD3EDu, 0x7BE74BA4u, 0x7B569F47u, 0x7AFA3910u, 0x7B166D32u, 0x7B870F92u, 0x7BCB69C8u,
    0x7E772D1Eu, 0x7E6B251Cu, 0x7E1AF70Bu, 0x7D668EE4u, 0x7C7E0AB2u, 0x77E37B2Cu, 0x7C41F4ACu, 0x7D2E88E5u, 0x7DBADF07u,
    0x7D35E094u, 0x7D2DDC93u, 0x7CEDC28Au, 0x7C5D8877u, 0x774676BDu, 0x766A1EA0u, 0x76F65CB5u, 0x7C498878u, 0x7CC5BA89u,
    0x773242AAu, 0x77263EA8u, 0x76D622A0u, 0x7625E28Cu, 0x753D9072u, 0x74AD5C62u, 0x74FD7C6Cu, 0x75F9D889u, 0x76920E9Au,
    0x74D96A67u, 0x74D16866u, 0x74AD5A62u, 0x74513C59u, 0x6FB6289Au, 0x6F19F28Au, 0x6F520891u, 0x741D2C54u, 0x7461445Cu,
    0x6EB5D884u, 0x6EB1D683u, 0x6E99CE81u, 0x6E61BC7Bu, 0x6E15A474u, 0x6DD9906Eu, 0x6DD9906Eu, 0x6E0DA273u, 0x6E31AE77u,
    0x6D6D7065u, 0x6D6D7065u, 0x6D6D7065u, 0x6D6D7065u, 0x6D6D7065u, 0x6D6D7065u, 0x6D6D7065u, 0x6D6D7065u, 0x6D6D7065u,
    // sun 11.0 degrees
    0x82FA0708u, 0x82F20306u, 0x7DA7D1EEu, 0x7D1759ACu, 0x7C5EBF59u, 0x7BFA692Au, 0x7C4AAF50u, 0x7D2765B5u, 0x7DA7CFEFu,
    0x83A63EF7u, 0x83A23AF5u, 0x837A1CE7u, 0x7E37AF8Fu, 0x7D46FF3Fu, 0x7CC29B11u, 0x7D2AEB37u, 0x7E43BF99u, 0x83721CE9u,
    0x8419A085u, 0x84119E84u, 0x7FC30EF9u, 0x7EE2A8D7u, 0x7DC628ABu, 0x7D21DC92u, 0x7D9A14A5u, 0x7EE6B0DAu, 0x7FAF0CF9u,
    0x7E35EC8Cu, 0x7E29E88Bu, 0x7DE1CE84u, 0x7D3D9472u, 0x7C65485Cu, 0x77BE349Cu, 0x7C293656u, 0x7D198A6Fu, 0x7DA5BC7Eu,
    0x7C452C53u, 0x7C3D2A52u, 0x7C111C4Eu, 0x7751F889u, 0x763DA271u, 0x75856A60u, 0x75C17C66u, 0x76C5CE7Du, 0x7762008Cu,
    0x75CD8067u, 0x75C97E67u, 0x75997063u, 0x752D505Au, 0x7499244Du, 0x74290244u, 0x742D0444u, 0x7495224Du, 0x74D53653u,
    0x740CFE43u, 0x7408FE43u, 0x6FF1F283u, 0x6FA9DC7Du, 0x6F3DBC75u, 0x6EDDA26Du, 0x6EB9966Au, 0x6ED19E6Cu, 0x6EEDA66Eu,
    0x6E618064u, 0x6E618064u, 0x6E618064u, 0x6E618064u, 0x6E618064u, 0x6E618064u, 0x6E618064u, 0x6E618064u, 0x6E618064u,
    // sun 25.0 degrees
    0x84AA711Bu, 0x84A66D19u, 0x847A510Cu, 0x841614EFu, 0x7F279194u, 0x7E9B3B6Cu, 0x7F238B92u, 0x843A2AF9u, 0x849E6715u,
    0x85468B00u, 0x853E86FEu, 0x850A6AF2u, 0x849228D7u, 0x7FFBA767u, 0x7F4F473Fu, 0x7FDF9961u, 0x84AE36DDu, 0x851E74F7u,
    0x853DC88Bu, 0x8535C48Au, 0x84FDB083u, 0x84818074u, 0x7FC284C0u, 0x7EFE36A8u, 0x7F6E62B6u, 0x84697671u, 0x84D1A07Eu,
    0x7FC62298u, 0x7FBA1E97u, 0x7F6A0690u, 0x7EB1CE7Fu, 0x7DBD8269u, 0x7D114C5Au, 0x7D35585Eu, 0x7E0D9A71u, 0x7E91C47Du,
    0x7D795C5Eu, 0x7D715A5Du, 0x7D3D4C59u, 0x7CC52A50u, 0x7C1CFC43u, 0x772DAC71u, 0x7711A66Fu, 0x77CDDA7Eu, 0x7C290044u,
    0x77C5D67Du, 0x77BDD47Cu, 0x7785C478u, 0x76F9A06Eu, 0x76356A5Fu, 0x757D3A52u, 0x7531264Du, 0x7559304Fu, 0x75813C52u,
    0x759D4656u, 0x759D4656u, 0x75854054u, 0x754D3250u, 0x74F91C4Au, 0x749D0444u, 0x745CF23Fu, 0x7440EC3Du, 0x743CEA3Du,
    0x7454F23Fu, 0x7454F23Fu, 0x7454F23Fu, 0x7454F23Fu, 0x7454F23Fu, 0x7454F23Fu, 0x7454F23Fu, 0x7454F23Fu, 0x7454F23Fu,
    // sun 60.0 degrees
    0x855A6D0Bu, 0x85566B0Au, 0x85466306u, 0x85224EFDu, 0x84EE34F1u, 0x84D626EBu, 0x84EE32F1u, 0x85325500u, 0x85566909u,
    0x85BE68E8u, 0x85BA66E7u, 0x85A65CE3u, 0x857A46DAu, 0x853E28CFu, 0x851A16C7u, 0x852E20CBu, 0x857242D8u, 0x859A54E0u,
    0x8559A47Eu, 0x8555A47Du, 0x85419C7Bu, 0x850D8A76u, 0x84C9726Eu, 0x84956068u, 0x84915E68u, 0x84C16E6Du, 0x84DD7870u,
    0x84010448u, 0x7FFE088Fu, 0x7FDDFE8Cu, 0x7F85E685u, 0x7F09C27Bu, 0x7E95A071u, 0x7E61906Du, 0x7E71966Fu, 0x7E899C71u,
    0x7E056861u, 0x7E016861u, 0x7DE5605Fu, 0x7DA14E5Au, 0x7D3D3452u, 0x7CD1184Bu, 0x7C850445u, 0x7C68FE43u, 0x7C68FC43u,
    0x7CC9144Au, 0x7CC51449u, 0x7CB10E48u, 0x7C850445u, 0x7C3CF240u, 0x77CDB874u, 0x7735926Au, 0x76DD7C64u, 0x76C17662u,
    0x77EDC077u, 0x77E9C077u, 0x77D9BC76u, 0x77A9B073u, 0x775D9E6Eu, 0x76FD8667u, 0x769D6E61u, 0x76555E5Cu, 0x763D565Au,
    0x76C17863u, 0x76C17863u, 0x76C17863u, 0x76C17863u, 0x76C17863u, 0x76C17863u, 0x76C17863u, 0x76C17863u, 0x76C17863u,
};
// Mie single scattering before its phase function.
static const uint kSkyMie[720] = {
    // sun -6.5 degrees
    0x200019A4u, 0x2000177Bu, 0x1800155Eu, 0x00000D4Du, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00001ED6u, 0x00001AC4u, 0x00000C65u, 0x0000020Cu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // sun -4.5 degrees
    0x48001F42u, 0x48001D31u, 0x40002398u, 0x300021CAu, 0x10000D73u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x3000331Du, 0x3000310Fu, 0x2800417Eu, 0x20002910u, 0x00002752u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x000030DBu, 0x00002ED4u, 0x000022A6u, 0x00000E4Bu, 0x0000020Fu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x0000020Bu, 0x0000020Bu, 0x00000209u, 0x00000205u, 0x00000002u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000001u, 0x00000001u, 0x00000001u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    // sun -3.0 degrees
    0x6000250Eu, 0x60002505u, 0x580035A5u, 0x50002D8Eu, 0x4000152Du, 0x080009B2u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x50003904u, 0x48006DFAu, 0x480059A2u, 0x40005DF6u, 0x38002D2Fu, 0x18001B19u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x30004B1Du, 0x30004918u, 0x28007FF0u, 0x28005974u, 0x2000537Bu, 0x100053DEu, 0x0000293Fu, 0x00000216u, 0x00000005u,
    0x180073C5u, 0x180071BFu, 0x1800699Du, 0x1800535Du, 0x18003902u, 0x10003711u, 0x08003525u, 0x00002B09u, 0x00001EC0u,
    0x10005566u, 0x10005564u, 0x10005157u, 0x10004936u, 0x080073F7u, 0x08004954u, 0x000067FEu, 0x00004778u, 0x00003B43u,
    0x08005B80u, 0x08005B7Eu, 0x08005774u, 0x0800515Bu, 0x0800452Du, 0x000067DDu, 0x0000579Bu, 0x00004B68u, 0x00004553u,
    0x00006FE5u, 0x00006FE4u, 0x00006DDDu, 0x000069CAu, 0x00005FA9u, 0x0000537Cu, 0x00004D64u, 0x00004951u, 0x00004749u,
    0x00004D5Bu, 0x00004D5Bu, 0x00004D5Bu, 0x00004D5Bu, 0x00004D5Bu, 0x00004D5Bu, 0x00004D5Bu, 0x00004D5Bu, 0x00004D5Bu,
    // sun -1.5 degrees
    0x70004F61u, 0x70004D5Cu, 0x70004336u, 0x680053ADu, 0x60003B8Cu, 0x50001716u, 0x080001A4u, 0x00000000u, 0x00000000u,
    0x6800591Fu, 0x6800571Cu, 0x68004F0Bu, 0x60007BBFu, 0x6000492Du, 0x58003523u, 0x400037DBu, 0x20001358u, 0x0000046Du,
    0x5800590Fu, 0x5800590Eu, 0x58005506u, 0x500097E3u, 0x500079A0u, 0x5000594Au, 0x48006FDCu, 0x48004750u, 0x48003B21u,
    0x48008BB2u, 0x48008BB2u, 0x480087ABu, 0x4800819Bu, 0x4800757Eu, 0x48006557u, 0x4800532Cu, 0x48004709u, 0x400081F7u,
    0x400097DBu, 0x400097DBu, 0x400095D7u, 0x400091CCu, 0x400089BAu, 0x40007FA1u, 0x40007386u, 0x4000696Fu, 0x40006566u,
    0x40006339u, 0x40006339u, 0x40006337u, 0x40006133u, 0x40005D2Cu, 0x40005923u, 0x40005518u, 0x40005110u, 0x4000510Cu,
    0x380095DDu, 0x380095DDu, 0x380095DCu, 0x380093D9u, 0x380091D4u, 0x38008FCDu, 0x38008BC5u, 0x380089BFu, 0x380087BCu,
    0x380083A7u, 0x380083A7u, 0x380083A7u, 0x380083A7u, 0x380083A7u, 0x380083A7u, 0x380083A7u, 0x380083A7u, 0x380083A7u,
    // sun 0.0 degrees
    0x80008F5Au, 0x80008D59u, 0x80008950u, 0x80007B3Au, 0x80006512u, 0x780091B9u, 0x7800654Du, 0x700097FFu, 0x700087C5u,
    0x78009F3Fu, 0x78009F3Fu, 0x78009B3Au, 0x7800932Fu, 0x7800831Bu, 0x7000DFFFu, 0x7000B7C4u, 0x70009B95u, 0x70009182u,
    0x6800E1A1u, 0x6800E1A1u, 0x6800DF9Eu, 0x6800DB99u, 0x6800D18Fu, 0x6800C580u, 0x6800B971u, 0x6800AF65u, 0x6800AB60u,
    0x6000D583u, 0x6000D583u, 0x6000D382u, 0x6000D180u, 0x6000CD7Bu, 0x6000C775u, 0x6000C16Eu, 0x6000BD68u, 0x6000BB66u,
    0x5800F9C4u, 0x5800F9C4u, 0x5800F7C4u, 0x5800F7C2u, 0x5800F3BFu, 0x5800F1BBu, 0x5800EDB7u, 0x5800E9B3u, 0x5800E7B2u,
    0x5800AB35u, 0x5800AB35u, 0x5800AB35u, 0x5800A935u, 0x5800A933u, 0x5800A732u, 0x5800A530u, 0x5800A52Fu, 0x5800A52Eu,
    0x50010BE6u, 0x50010BE6u, 0x50010BE5u, 0x500109E5u, 0x500109E4u, 0x500109E3u, 0x500107E2u, 0x500107E1u, 0x500105E0u,
    0x5000F3BAu, 0x5000F3BAu, 0x5000F3BAu, 0x5000F3BAu, 0x5000F3BAu, 0x5000F3BAu, 0x5000F3BAu, 0x5000F3BAu, 0x5000F3BAu,
    // sun 2.0 degrees
    0x8800BB08u, 0x8800B908u, 0x8800B706u, 0x800163FFu, 0x80014DE9u, 0x800131CAu, 0x800113A7u, 0x8000FB8Au, 0x8000F17Eu,
    0x782197E2u, 0x782195E1u, 0x782193DFu, 0x781D8BDAu, 0x78197FD1u, 0x78116DC3u, 0x780D59B4u, 0x780547A7u, 0x780541A2u,
    0x70413141u, 0x70413141u, 0x703D3141u, 0x703D2F3Fu, 0x70392B3Du, 0x7035253Au, 0x70311F37u, 0x70311B34u, 0x702D1933u,
    0x6849292Eu, 0x6849292Eu, 0x6849272Eu, 0x6849272Du, 0x6849252Cu, 0x6845232Bu, 0x68411F29u, 0x68411D28u, 0x68411D28u,
    0x605D5F63u, 0x605D5F63u, 0x605D5F63u, 0x605D5F62u, 0x605D5D62u, 0x60595D61u, 0x60595B60u, 0x6059595Fu, 0x6055595Fu,
    0x5885E5E7u, 0x5885E5E7u, 0x5885E5E7u, 0x5885E5E7u, 0x5881E3E6u, 0x5881E3E6u, 0x5881E1E5u, 0x5881E1E5u, 0x587DDFE4u,
    0x58697F80u, 0x58697F80u, 0x58697F7Fu, 0x58697F7Fu, 0x58697F7Fu, 0x58697D7Fu, 0x58697D7Fu, 0x58697D7Fu, 0x58697D7Eu,
    0x58615D5Fu, 0x58615D5Fu, 0x58615D5Fu, 0x58615D5Fu, 0x58615D5Fu, 0x58615D5Fu, 0x58615D5Fu, 0x58615D5Fu, 0x58615D5Fu,
    // sun 5.0 degrees
    0x885D2720u, 0x885D2720u, 0x885D251Fu, 0x8859211Du, 0x88591B18u, 0x88551312u, 0x884D090Bu, 0x884D0104u, 0x8848FD02u,
    0x809D3B00u, 0x809D3B00u, 0x809D3B00u, 0x793A6FFEu, 0x793269FAu, 0x792A5FF4u, 0x791E53EEu, 0x791649E9u, 0x791645E7u,
    0x7149DB57u, 0x7149DB57u, 0x7149D957u, 0x7149D956u, 0x7145D755u, 0x7141D354u, 0x713DD153u, 0x7139CF52u, 0x7139CD51u,
    0x6961CD43u, 0x6961CD43u, 0x6961CD43u, 0x6961CD43u, 0x695DCD43u, 0x695DCB42u, 0x6959C941u, 0x6959C941u, 0x6959C741u,
    0x61B6257Cu, 0x61B6257Cu, 0x61B6257Cu, 0x61B6257Cu, 0x61B2257Cu, 0x61B2237Cu, 0x61B2237Bu, 0x61AE237Bu, 0x61AE217Bu,
    0x61317D05u, 0x61317D05u, 0x61317D05u, 0x61317D05u, 0x61317B05u, 0x61317B05u, 0x61317B05u, 0x61317B05u, 0x61317B05u,
    0x59EA579Cu, 0x59EA579Cu, 0x59EA579Cu, 0x59EA579Cu, 0x59E6579Cu, 0x59E6579Cu, 0x59E6579Cu, 0x59E6579Bu, 0x59E6579Bu,
    0x59BE2579u, 0x59BE2579u, 0x59BE2579u, 0x59BE2579u, 0x59BE2579u, 0x59BE2579u, 0x59BE2579u, 0x59BE2579u, 0x59BE2579u,
    // sun 11.0 degrees
    0x8909832Au, 0x8909832Au, 0x89058129u, 0x89058129u, 0x89057F27u, 0x89017B25u, 0x88FD7723u, 0x88FD7521u, 0x88F97320u,
    0x81799305u, 0x81799305u, 0x81799305u, 0x81799105u, 0x81759104u, 0x81758F04u, 0x81718D03u, 0x816D8B02u, 0x816D8B02u,
    0x72CE5B5Eu, 0x72CE5B5Eu, 0x72CE595Eu, 0x72CE595Eu, 0x72CA595Du, 0x72CA575Du, 0x72C6575Du, 0x72C6575Cu, 0x72C6555Cu,
    0x6AF2494Au, 0x6AF2494Au, 0x6AF2494Au, 0x6AF2494Au, 0x6AF2494Au, 0x6AF2494Au, 0x6AF24949u, 0x6AEE4949u, 0x6AEE4749u,
    0x639EB985u, 0x639EB985u, 0x639EB985u, 0x639EB985u, 0x639EB984u, 0x639EB984u, 0x639EB984u, 0x639EB984u, 0x639AB984u,
    0x6289E30Bu, 0x6289E30Bu, 0x6289E30Bu, 0x6289E30Bu, 0x6289E30Bu, 0x6289E30Bu, 0x6289E30Bu, 0x6289E30Bu, 0x6289E30Bu,
    0x5C0AF9A5u, 0x5C06F9A5u, 0x5C06F9A5u, 0x5C06F9A5u, 0x5C06F9A5u, 0x5C06F9A5u, 0x5C06F9A5u, 0x5C06F9A5u, 0x5C06F9A5u,
    0x5BB2B981u, 0x5BB2B981u, 0x5BB2B981u, 0x5BB2B981u, 0x5BB2B981u, 0x5BB2B981u, 0x5BB2B981u, 0x5BB2B981u, 0x5BB2B981u,
    // sun 25.0 degrees
    0x89CDDD44u, 0x89CDDD44u, 0x89C9DD44u, 0x89C9DD44u, 0x89C9DD44u, 0x89C9DB43u, 0x89C9DB43u, 0x89C5D942u, 0x89C5D942u,
    0x8269E91Au, 0x8269E91Au, 0x8269E91Au, 0x8269E91Au, 0x8269E71Au, 0x8269E71Au, 0x8269E71Au, 0x8265E719u, 0x8265E719u,
    0x7472D579u, 0x7472D579u, 0x7472D579u, 0x7472D579u, 0x7472D579u, 0x7472D579u, 0x7472D379u, 0x7472D379u, 0x7472D379u,
    0x6CA2BF64u, 0x6CA2BF64u, 0x6CA2BF64u, 0x6CA2BF64u, 0x6CA2BF64u, 0x6CA2BF64u, 0x6CA2BF64u, 0x6CA2BF64u, 0x6CA2BF64u,
    0x65AB45A3u, 0x65AB45A3u, 0x65AB45A3u, 0x65AB45A3u, 0x65AB45A3u, 0x65AB45A3u, 0x65AB45A3u, 0x65AB45A3u, 0x65AB45A3u,
    0x63FA4320u, 0x63FA4320u, 0x63FA4320u, 0x63FA4320u, 0x63FA4320u, 0x63FA4320u, 0x63FA4320u, 0x63FA4320u, 0x63F64320u,
    0x5E4F91C6u, 0x5E4F91C6u, 0x5E4F91C6u, 0x5E4F91C6u, 0x5E4F91C6u, 0x5E4F91C6u, 0x5E4F91C6u, 0x5E4F91C6u, 0x5E4F91C6u,
    0x5DCB45A0u, 0x5DCB45A0u, 0x5DCB45A0u, 0x5DCB45A0u, 0x5DCB45A0u, 0x5DCB45A0u, 0x5DCB45A0u, 0x5DCB45A0u, 0x5DCB45A0u,
    // sun 60.0 degrees
    0x8A4E1154u, 0x8A4E1154u, 0x8A4E1154u, 0x8A4E1154u, 0x8A4E1154u, 0x8A4E1154u, 0x8A4E1154u, 0x8A4E1154u, 0x8A4E1154u,
    0x83061727u, 0x83061727u, 0x83061727u, 0x83061727u, 0x83061727u, 0x83061727u, 0x83061727u, 0x83061727u, 0x83061727u,
    0x757F178Au, 0x757F178Au, 0x757F178Au, 0x757F178Au, 0x757F178Au, 0x757F178Au, 0x757F178Au, 0x757F178Au, 0x757F178Au,
    0x6DB2FF74u, 0x6DB2FF74u, 0x6DB2FF74u, 0x6DB2FF74u, 0x6DB2FF74u, 0x6DB2FF74u, 0x6DB2FF74u, 0x6DB2FF74u, 0x6DB2FF74u,
    0x66F791B6u, 0x66F791B6u, 0x66F791B6u, 0x66F791B6u, 0x66F791B6u, 0x66F791B6u, 0x66F791B6u, 0x66F791B6u, 0x66F791B6u,
    0x64DE772Du, 0x64DE772Du, 0x64DE772Du, 0x64DE772Du, 0x64DE772Du, 0x64DE772Du, 0x64DE772Du, 0x64DE772Du, 0x64DE772Du,
    0x5FBBE5DAu, 0x5FBBE5DAu, 0x5FBBE5DAu, 0x5FBBE5DAu, 0x5FBBE5DAu, 0x5FBBE5DAu, 0x5FBBE5DAu, 0x5FBBE5DAu, 0x5FBBE5DAu,
    0x5F1791B2u, 0x5F1791B2u, 0x5F1791B2u, 0x5F1791B2u, 0x5F1791B2u, 0x5F1791B2u, 0x5F1791B2u, 0x5F1791B2u, 0x5F1791B2u,
};
// </sky-table>

// ---- The scattering sky: everything outdoors is lit from the table above ----
float3 skyUnpack(uint v) {
    return float3(v & 511u, (v >> 9u) & 511u, (v >> 18u) & 511u) * exp2(float(v >> 27u) - 24.0);
}

// Once the city's lights are on (the same ramp that switches its windows on).
float cityNightAmount() {
    return sunOverridden() ? 1.0 - smoothstep(-0.10, 0.30, norm(sunOverrideDir).y) : smoothstep(0.68, 0.08, daylight);
}

// The sun that lights the air. It is not the key light: at night the key is moonlight from high up,
// while this sun waits 6.5 degrees below the horizon on the bearing where it set, so the sky holds the
// blue hour and never goes black.
float3 skySunDir() {
    if (sunOverridden()) return norm(sunOverrideDir);
    float e = -6.5;
    if (duskOpening()) {
        // "Last Light": the visible sunset (duskSunDir), then on down into the blue hour.
        e = lerp(11.0, -5.0, smoothstep(14.0, 30.0, time)) - 1.5 * smoothstep(30.0, 44.0, time);
    } else if (daylight > 0.0) {
        e = lerp(-6.5, 30.0, pow(saturate(daylight), 1.6));
    }
    float r = radians(e);
    if (act == 1u) return norm(float3(-0.48 * cos(r), 0.865 * sin(r), 0.72 * cos(r)));
    return norm(float3(-0.92 * cos(r), sin(r), -0.38 * cos(r)));
}

// Where the sun's elevation falls between the table's sun elevations, and the white balance of the
// light it gives (grade): a golden-hour warmth while the sun is within a few degrees of the horizon,
// the way a film camera balanced for daylight sees it, with luminance unchanged. The glow round a low
// sun is warmer still (aureole): Taipei's humid air scatters the reddened sunlight forward, which is
// what turns the opening's reflecting pool to gold. Only that glow takes the aureole tint, so the sky
// overhead stays blue.
struct SkyKnot { uint lo; float f; float3 grade; float3 aureole; };
SkyKnot skyKnot(float3 sunDir) {
    float e = degrees(asin(clamp(sunDir.y, -1.0, 1.0)));
    SkyKnot k;
    k.lo = 0u;
    k.f = 0.0;
    float golden = smoothstep(-4.0, 1.0, e) * (1.0 - smoothstep(10.0, 35.0, e));
    k.grade = lerp(1.0.xxx, float3(1.13, 0.98, 0.80), golden);
    k.aureole = lerp(1.0.xxx, float3(1.22, 0.95, 0.66), golden);
    [unroll] for (uint i = 0u; i + 1u < kSkySunKnots; ++i) {
        if (e >= kSkySunElev[i]) {
            k.lo = i;
            k.f = saturate((e - kSkySunElev[i]) / (kSkySunElev[i + 1u] - kSkySunElev[i]));
        }
    }
    return k;
}
#define SKY_KNOT_LERP(table, k) lerp(table[(k).lo], table[(k).lo + 1u], (k).f)

float2 skyBearing(float3 v) {
    return v.xz * rsqrt(max(dot(v.xz, v.xz), 1e-8));
}

// Sky radiance toward d: Rayleigh and multiple scattering from the table, plus Mie scattering with its
// phase function applied here so the glow round the sun stays sharp. Below the horizon it holds the
// horizon's value.
float3 skyScatter(float3 d, float3 sunDir, SkyKnot k) {
    float ue = sqrt(asin(saturate(d.y)) * (2.0 / kPi)) * float(kSkyElevSteps - 1u);
    float azim = acos(clamp(dot(skyBearing(d), skyBearing(sunDir)), -1.0, 1.0)) / kPi;
    float ua = pow(azim, 1.0 / kSkyAzimPower) * float(kSkyAzimSteps - 1u);
    uint i0 = min(uint(ue), kSkyElevSteps - 2u);
    uint j0 = min(uint(ua), kSkyAzimSteps - 2u);
    float fe = ue - float(i0);
    float fa = ua - float(j0);
    float cosGamma = dot(d, sunDir);
    float g = kSkyMieG;
    float miePhase = (3.0 / (8.0 * kPi)) * ((1.0 - g * g) * (1.0 + cosGamma * cosGamma))
                   / ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * cosGamma, 1.5));
    float3 atKnot[2];
    [unroll] for (uint s = 0u; s < 2u; ++s) {
        uint a = ((k.lo + s) * kSkyElevSteps + i0) * kSkyAzimSteps + j0;
        uint b = a + kSkyAzimSteps;
        float3 rayleigh = lerp(lerp(skyUnpack(kSkyRayleigh[a]), skyUnpack(kSkyRayleigh[a + 1u]), fa),
                               lerp(skyUnpack(kSkyRayleigh[b]), skyUnpack(kSkyRayleigh[b + 1u]), fa), fe);
        float3 mie = lerp(lerp(skyUnpack(kSkyMie[a]), skyUnpack(kSkyMie[a + 1u]), fa),
                          lerp(skyUnpack(kSkyMie[b]), skyUnpack(kSkyMie[b + 1u]), fa), fe);
        atKnot[s] = rayleigh + mie * (miePhase * k.aureole);
    }
    return lerp(atKnot[0], atKnot[1], k.f) * k.grade;
}

// Irradiance / pi that the sky, and the ground it lights, put on a surface facing n.
float3 skyAmbient(float3 n, float3 sunDir, SkyKnot k) {
    float3 up = SKY_KNOT_LERP(kSkyAmbUp, k);
    // n.xz is deliberately not normalised: the bearing matters less as the surface turns to face up or down.
    float facing = 0.5 + 0.5 * dot(n.xz, skyBearing(sunDir));
    float3 wall = lerp(SKY_KNOT_LERP(kSkyAmbAway, k), SKY_KNOT_LERP(kSkyAmbSunward, k), facing);
    float3 fromSky = n.y >= 0.0 ? lerp(wall, up, n.y) : wall * (1.0 + n.y);
    return (fromSky + up * (0.22 * (0.5 - 0.5 * n.y))) * k.grade;
}

// Light scattered toward the eye by the city's own haze along d: air lit by the whole sky, sunlight
// scattered forward while the sun is up, and at night the glow of the city's lamps in the low air.
// The haze is not the colour of the sky behind it: after sunset the low air is in the Earth's shadow,
// so the skyline stands dark against the afterglow.
float3 skyHaze(float3 d, float3 sunDir, SkyKnot k) {
    float facing = 0.5 + 0.5 * dot(skyBearing(d), skyBearing(sunDir));
    float3 air = 0.45 * SKY_KNOT_LERP(kSkyAmbUp, k)
               + 0.90 * lerp(SKY_KNOT_LERP(kSkyAmbAway, k), SKY_KNOT_LERP(kSkyAmbSunward, k), facing);
    float forward = pow(saturate(0.5 + 0.5 * dot(d, sunDir)), 6.0);
    // Near the ground the haze is lit mostly by the low sun itself, so it takes the sunlight's warmth.
    air += SKY_KNOT_LERP(kSkySunLight, k) * (0.05 + 0.45 * forward * k.aureole);
    // At night the low air sits in the Earth's shadow and is lit only by the deep blue overhead: dark
    // and blue, so the lit windows of far blocks still read through it rather than greying out.
    float night = cityNightAmount();
    air = lerp(air, 0.50 * SKY_KNOT_LERP(kSkyAmbUp, k), night);
    float3 lamps = ((showFlags & 2u) != 0u) ? float3(0.050, 0.023, 0.043) : float3(0.052, 0.032, 0.016);
    return air * k.grade + lamps * night;
}

// The cloudless sky in any direction. Below the horizon it fades into the haze over the ground, so the
// edge of the modelled city is lost in the same air that veils its far blocks.
float3 skyClear(float3 d, float3 sunDir, SkyKnot k) {
    float3 c = skyScatter(d, sunDir, k);
    if (d.y < 0.0) c = lerp(c, skyHaze(d, sunDir, k), smoothstep(0.0, -0.10, d.y));
    return c;
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
    // Outdoors the sky lights the scene: a sharp lookup sees the sky itself, a rough one its irradiance.
    // The baked night environment adds the city's own light once its lamps are on.
    float3 sunDir = skySunDir();
    SkyKnot knot = skyKnot(sunDir);
    // At night the sky's fill is eased so walls stay darker than the lit windows in them.
    float night = cityNightAmount();
    float3 s0 = skyAmbient(dn, sunDir, knot) * (1.0 - 0.25 * night);
    [branch] if (rough < 0.70) {
        s0 = lerp(skyClear(dn, sunDir, knot), s0, smoothstep(0.10, 0.70, rough));
    }
    s0 += gEnvHdrTex.SampleLevel(equirectSampler, float3(uv, 0.0), mip).rgb * (0.55 * night);
    if ((showFlags & 2u) != 0u) {
        s0 = lerp(s0, s0.bgr * float3(1.15, 0.75, 1.25), 0.35);
    }
    return s0;
}

bool cityExterior() {
    return (showFlags & 64u) != 0u || (showFlags == 0u && act != 0u);
}

// Integrate the denser lower layer between the viewer and the surface. This keeps
// the waterfront streets hazier than the tower crowns on the same block.
float cityHazeWeight(float distance, float startHeight, float endHeight) {
    float3 heights = float3(startHeight, (startHeight + endHeight) * 0.5, endHeight);
    float3 layer = exp2(-max(heights, 0.0) / 16.0);
    float lowAir = dot(layer, float3(1.0, 4.0, 1.0) / 6.0);
    float density = lerp(0.0025, 0.0068, lowAir) * (1.0 - 0.33 * daylight);
    return 0.90 * (1.0 - exp(-max(distance - 8.0, 0.0) * density));
}

// Value and analytic slope from the same four lattice corners. The slope
// lets the sun catch individual lobes without additional lighting samples.
float3 cloudNoise(float2 p) {
    int2 cell = int2(floor(p));
    float2 f = frac(p);
    float2 w = f * f * (3.0 - 2.0 * f);
    uint seed = asuint(cell.x) * 0x8da6b343u + asuint(cell.y) * 0xd8163841u;
    uint4 h = seed + uint4(0u, 0x8da6b343u, 0xd8163841u, 0x65bceb84u);
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    float4 n = float4(h >> 8u) * (1.0 / 16777216.0);
    float2 rows = lerp(n.xz, n.yw, w.x);
    float2 slope = 6.0 * f * (1.0 - f) * float2(lerp(n.y - n.x, n.w - n.z, w.y), rows.y - rows.x);
    return float3(lerp(rows.x, rows.y, w.y), slope);
}

float2 cloudLayerPoint(float3 origin, float3 d, float altitude) {
    // A gently curved deck has a finite sightline even along the horizon.
    // Both wind and camera motion translate this plane in world coordinates.
    float heightAboveEye = max(altitude - origin.y, 1.0);
    float travel = 2.0 * heightAboveEye / (sqrt(d.y * d.y + heightAboveEye / 4800.0) + d.y);
    return origin.xz + travel * d.xz;
}

float2 cloudWind() {
    return (interFlags & (1u << 14u)) != 0u ? float2(0.0, 0.0) : time * float2(0.70, 0.24);
}

static const float2x2 kCloudTurn = float2x2(0.8, -0.6, 0.6, 0.8);
float2 cloudBankDomain(float3 origin, float3 direction) {
    return (cloudLayerPoint(origin, direction, 145.0) - cloudWind()) * 0.013;
}
float2 cloudLobeDomain(float2 p) { return mul(kCloudTurn, p) * 2.14 + float2(13.4, 8.2); }

bool cityWeatherLighting() { return showFlags == 0u || cityExterior(); }

float cloudKeyTransmission(float3 p, float3 light) {
    if (!cityWeatherLighting() || light.y <= 0.008 || p.y >= 145.0) return 1.0;
    float2 domain = cloudBankDomain(p, light);
    // The projected shadow uses the two broad lobes. Integrate out the
    // small detail so the penumbra travels smoothly across blocks.
    float density = 0.54 * cloudNoise(domain).x + 0.31 * cloudNoise(cloudLobeDomain(domain)).x + 0.075;
    float opacity = smoothstep(0.46, 0.68, density) * smoothstep(0.008, 0.095, light.y);
    return 1.0 - lerp(0.27, 0.44, daylight) * opacity;
}

// What lights a cloud: its thin high wisps, its shaded body, the edge that faces the sun, and how far
// the city's lamps are on beneath it.
struct CloudLight { float3 wisp; float3 base; float3 edge; float night; };

float3 cityCloudsLit(float3 skyColor, float3 origin, float3 d, float3 sunDir, CloudLight lit, float panoramaWeight) {
    float visible = smoothstep(0.008, 0.095, d.y) * (1.0 - panoramaWeight);
    if (visible < 0.002) return skyColor;

    float2 p = cloudBankDomain(origin, d);
    float3 broad = cloudNoise(p);
    float2 midP = cloudLobeDomain(p);
    float3 lobe = cloudNoise(midP);
    if (liteMode()) {
        float density = 0.64f * broad.x + 0.36f * lobe.x;
        float cover = smoothstep(0.465f, 0.64f, density);
        float2 light = normalize(sunDir.xz + float2(0.0001f, 0.0f));
        float slope = dot(broad.yz, light) * 0.54f + dot(lobe.yz, mul(kCloudTurn, light)) * 0.66f;
        float rim = saturate(0.45f - slope * 0.78f - (density - 0.55f) * 1.1f);
        return lerp(skyColor, lerp(lit.base, lit.edge, rim), visible * cover * 0.84f);
    }
    // Curl the smaller tufts along the large lobes. Thin, sheared remnants
    // reach past the bank's edge and break up its outline against clear sky.
    float2 curl = float2(lobe.y + broad.x, broad.z - lobe.x);
    float3 tuft = cloudNoise(p * 9.5 + 1.12 * curl + float2(25.2, 2.4));
    float2 windBasis = float2(0.946, 0.324);
    float2 crossWind = float2(-windBasis.y, windBasis.x);
    float2 wispyP = float2(dot(p, windBasis) * 14.0, dot(p, crossWind) * 37.0);
    float3 filament = cloudNoise(wispyP + 2.8 * curl + float2(tuft.x, 2.7 * tuft.x));
    float coarse = dot(float3(broad.x, lobe.x, tuft.x), float3(0.54, 0.31, 0.15));
    float fringe = 1.0 - smoothstep(0.035, 0.17, abs(coarse - 0.52));
    float density = coarse + fringe * 0.125 * (filament.x - 0.5);
    float cover = smoothstep(0.465, 0.64, density);

    // A higher, wind-sheared layer moves more slowly across the camera.
    float2 high = mul(kCloudTurn, (cloudLayerPoint(origin, d, 335.0) - 1.38 * cloudWind()) * 0.004);
    float highEnvelope = cloudNoise(high * 1.25).x;
    float streak = cloudNoise(float2(high.x * 1.9, high.y * 11.0) + float2(1.4, 4.7) * highEnvelope).x;
    float wisp = smoothstep(0.51, 0.82, 0.69 * streak + 0.31 * highEnvelope);
    skyColor += lit.wisp * (visible * wisp * 0.55);

    float2 light = normalize(sunDir.xz + float2(0.0001, 0.0));
    float slope = 0.54 * dot(broad.yz, light) + 0.66 * dot(lobe.yz, mul(kCloudTurn, light));
    float smallSlope = dot(tuft.yz, light) + fringe * 0.42 * filament.z * dot(crossWind, light);
    float rim = saturate(0.45 - slope * 0.78 - smallSlope * 0.27 - (density - 0.55) * 1.1);
    // Keep the existing neural sky input as a subtle tint across the cloud body.
    float neural = dot(tensorField.SampleLevel(linearClamp, dirToEquirectUv(d), 0).rgb, float3(0.2126, 0.7152, 0.0722));
    float3 cloud = lerp(lit.base, lit.edge, rim) * (0.96 + 0.08 * saturate(neural));
    float2 warmDistrict = act == 1u ? float2(-0.62, 0.78) : float2(0.52, 0.85);
    float uplight = (0.20 + 0.80 * saturate(dot(d.xz, warmDistrict))) * lit.night;
    cloud += float3(0.052, 0.019, 0.005) * uplight * (1.0 - 0.74 * d.y) * (0.35 + 0.65 * cover);
    return lerp(skyColor, cloud, visible * cover * 0.84);
}

// The dawn finale's hand-graded clouds.
float3 cityClouds(float3 skyColor, float3 origin, float3 d, float3 sunDir, float day, float panoramaWeight) {
    CloudLight lit;
    lit.wisp = lerp(float3(0.030, 0.043, 0.065), float3(0.31, 0.31, 0.33), day);
    lit.base = lerp(float3(0.024, 0.034, 0.051), float3(0.32, 0.38, 0.48), day);
    lit.edge = lerp(float3(0.119, 0.132, 0.153), float3(1.01, 0.86, 0.69), day);
    lit.night = 1.0 - day;
    return cityCloudsLit(skyColor, origin, d, sunDir, lit, panoramaWeight);
}

float3 skyImpl(float3 d, float3 origin, bool includeCelestialAndClouds) {
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
        return cityClouds(clearDawn + float3(1.0, 0.76, 0.52) * (sunDisc + sunGlow),
                          origin, dn, sDir, dawnT, hdriWinMask);
    }

    // The scattering sky (tools/gen_sky_table.py): the air lit by the sun wherever it is, down to the
    // blue hour that the night holds.
    float3 sunDir = skySunDir();
    SkyKnot knot = skyKnot(sunDir);
    float3 c = skyClear(dn, sunDir, knot);
    float cityNight = cityNightAmount();

    // Light pollution: the lit city glows in the low air, most over its busiest districts.
    float urbanGlow = pow(saturate(1.0 - max(dn.y + 0.04, 0.0) * 3.8), 3.0);
    float3 glow = float3(0.032, 0.024, 0.016) * urbanGlow;
    if (cityExterior()) {
        // Fixed world bearings keep the glow behind each district as the aircraft banks.
        float2 warmBearing = act == 1u ? float2(-0.62, 0.78) : float2(0.52, 0.85);
        float2 coolBearing = act == 1u ? float2(0.64, 0.77) : float2(-0.72, 0.69);
        float warm = smoothstep(0.52, 0.99, dot(dn.xz, warmBearing));
        float cool = smoothstep(0.57, 0.99, dot(dn.xz, coolBearing));
        float elevation = dn.y - 0.018;
        float lowHaze = exp2(-95.0 * elevation * elevation);
        glow += lowHaze * (warm * float3(0.136, 0.056, 0.024) + cool * float3(0.012, 0.068, 0.084));
    }
    c += glow * cityNight;
    if ((showFlags & 2u) != 0u) {
        // The Midnight edition's violet night.
        c = lerp(c, c.bgr * float3(1.15, 0.75, 1.25), 0.35 * cityNight);
    }

    if (includeCelestialAndClouds) {
        // The disc, a little larger than life. Its glow is the Mie term of the sky itself.
        float disc = smoothstep(0.99985, 0.99996, dot(dn, sunDir)) * smoothstep(-0.012, 0.004, dn.y);
        c += SKY_KNOT_LERP(kSkySunLight, knot) * knot.grade * (12.0 * disc);
        // Clouds take their light from the same sky: the body from the sky overhead, the sunward edge
        // from sunlight at cloud height, which outlasts the sunset below and reddens as it goes.
        float3 ambUp = SKY_KNOT_LERP(kSkyAmbUp, knot) * knot.grade;
        float3 cloudSun = SKY_KNOT_LERP(kSkyCloudSun, knot) * knot.grade;
        CloudLight lit;
        lit.wisp = 0.45 * ambUp + 0.07 * cloudSun;
        lit.base = 0.85 * ambUp;
        lit.edge = lit.base + 0.20 * cloudSun;
        lit.night = cityNight;
        c = cityCloudsLit(c, origin, dn, sunDir, lit, 0.0);
    }

    if (showFlags == 0u) {
        float mist = 1.0 - smoothstep(0.0, 2.0, min(abs(time - 60.0), abs(time - 120.0)));
        c = lerp(c, float3(0.28, 0.33, 0.36), mist);
    }
    return c;
}

float3 sky(float3 d) {
    return skyImpl(d, eye, true);
}

float3 skyAerialFog(float3 d) {
    if (isDawnFinale()) return skyImpl(d, eye, false);
    float3 sunDir = skySunDir();
    float3 haze = skyHaze(norm(d), sunDir, skyKnot(sunDir));
    if ((showFlags & 2u) != 0u) haze = lerp(haze, haze.bgr * float3(1.15, 0.75, 1.25), 0.35 * cityNightAmount());
    return haze;
}

uint effectsDetail() { return (interFlags >> 12u) & 3u; }

// A mirrored sample domain keeps a non-periodic neural texture continuous at a tile edge.
float2 mirrorField(float2 uv) { return 1.0 - abs(frac(uv * 0.5) * 2.0 - 1.0); }

// Fold a planar motif into progressively smaller branches. Fade a branch before it is subpixel.
float velvetFiligree(float2 p, float footprint, float3 neuralColor) {
    float pattern = 0.0;
    float weight = 1.0;
    uint folds = effectsDetail() * 2u + 2u;
    [loop] for (uint k = 0u; k < folds; ++k) {
        p = abs(p) - float2(0.55, 0.31);
        p = float2(0.8 * p.x - 0.6 * p.y, 0.6 * p.x + 0.8 * p.y) * 1.72;
        footprint *= 1.72;
        float lineDist = abs(length(p) - (0.22 + 0.15 * neuralColor.g));
        float coverage = 1.0 - smoothstep(0.024, 0.045 + footprint, lineDist);
        pattern = max(pattern, coverage * weight * (1.0 - smoothstep(0.06, 0.20, footprint)));
        weight *= 0.82;
    }
    return pattern;
}

// Shared with the depth-of-field output so the brass is focused at the face.
float stelaBrassWeight(float2 uv, float coverage) {
    float across = abs(2.0f * uv.y - 1.0f);
    float end = min(uv.x, 1.0f - uv.x);
    float fan = abs(frac(5.0f * end - 0.36f * across + 0.50f) - 0.50f);
    float aa = clamp(coverage * 5.4f, 0.002f, 0.22f);
    float etched = (1.0f - smoothstep(0.011f, 0.018f + aa, fan))
                 * (1.0f - smoothstep(0.13f, 0.29f, end)) * (1.0f - smoothstep(0.045f, 0.11f, coverage));
    float edge = smoothstep(0.78f, 0.86f, across);
    return max(etched, edge * 0.78f);
}

// The stelae and enamel columns have longitudinal, object-locked UVs. Retain
// the same finish when the surface is seen in a mirror or through the flower.
float3 cathedralFinish(uint material, uint part, float2 uv, float coverage,
                       inout float3 base, inout float rough, inout float metallic) {
    if (material == 12u && part >= 200u && part <= 207u) {
        float brass = stelaBrassWeight(uv, coverage);
        base = lerp(base, float3(0.51, 0.25, 0.082), brass);
        rough = lerp(0.048, 0.32, brass);
        metallic = lerp(metallic, 0.86, brass);
        return brass * float3(0.13, 0.061, 0.015);
    }
    if (material == 20u && part >= 210u && part <= 215u) {
        float height = saturate(uv.x);
        float rib = 0.5 + 0.5 * cos(kTwoPi * (uv.y * 6.0 + 0.33 * height));
        float root = (1.0 - smoothstep(0.05, 0.66, height)) * (0.55 + 0.38 * rib);
        base *= lerp(1.0.xxx, float3(0.32, 0.62, 0.55), root);
        rough = lerp(rough, 0.28, root);
        return 0.0.xxx;
    }
    return 0.0.xxx;
}

// Periodic lozenges follow the warp and weft of the exported ribbon. The
// neural field sets the height of the threads and the strength of the gold weft.
float3 velvetThreads(float2 uv, float2 uvWidth, float3 field) {
    float2 loom = float2(uv.x * 56.0, uv.y * 3.0);
    float spread = dot(uvWidth, float2(56.0, 3.0));
    float2 distance = abs(frac(float2(loom.x + loom.y, loom.x - loom.y) + 0.5) - 0.5);
    float2 thread = 1.0 - smoothstep(0.045.xx, (0.085 + min(spread, 0.2)).xx, distance);
    float visibility = 1.0 - smoothstep(0.12, 0.45, spread);
    return visibility * (thread.x * float3(0.62, 0.27, 0.082) * (0.32 + 0.55 * field.r)
                       + thread.y * float3(0.048, 0.31, 0.33) * (0.42 + 0.58 * field.b));
}

bool isCurtainWall(uint m) { return m == 5u || m == 13u || m == 14u; }

bool isDistrictGlazing(uint m, uint partId) {
    return isCurtainWall(m) && (act == 1u || (act == 2u && ((partId >= 2u && partId <= 4u) || partId == 61u || partId == 62u)));
}

// The opening's ignition starts gently, swells through the blue hour, and finishes by 32 s.
float windowNightProgress() {
    return smoothstep(23.0f, 32.0f, time);
}

// A single optional ballast strike. Full night is exactly neutral; reduced flash disables it.
float windowIgnition(float nightProgress, float roomOrder, float flickHash, out float strikeTint) {
    strikeTint = 0.0f;
    if (nightProgress <= 0.0f) return 0.0f;
    if (nightProgress >= 1.0f) return 1.0f;
    bool reducedFlash = (interFlags & (1u << 14u)) != 0u;
    float age = nightProgress - lerp(0.01f, 0.91f, saturate(roomOrder));
    if (age <= 0.0f) return 0.0f;
    float ramp = reducedFlash ? 0.068f : 0.034f;
    if (!reducedFlash && flickHash < 0.08f) {
        float strikePulse = smoothstep(0.0f, 0.011f, age) * (1.0f - smoothstep(0.011f, 0.030f, age));
        float catchRamp = smoothstep(0.020f, 0.056f, age);
        strikeTint = strikePulse * 0.65f + catchRamp * (1.0f - catchRamp) * 0.60f;
        return saturate(max(catchRamp, 0.78f * strikePulse));
    }
    return smoothstep(0.0f, ramp, age);
}

// Use the actual room grouping and floor coordinates for every district LOD.
// The same height rule is also used by the non-district glazing below.
float ignitionOrder(float orderHash, float storeyCentre) {
    return lerp(orderHash, saturate((storeyCentre + 0.4f) / 16.0f), 0.15f);
}

float3 facadeIgnition(uint m, uint buildingSeed, float3 p, float3 n, float3 footprint) {
    bool front = abs(n.z) >= abs(n.x);
    bool hotel = m == 13u;
    bool campus = m == 5u;
    bool apartments = act == 2u;
    float bayWidth = apartments ? (hotel ? 0.68f : 0.52f) : (hotel ? 0.78f : (campus ? 1.15f : 0.62f));
    float floorHeight = apartments ? 0.49f : (hotel ? 0.57f : (campus ? 0.82f : 0.67f));
    float roomBays = hotel ? 1.0f : (apartments ? 2.0f : (campus ? 4.0f : 6.0f));
    float roomWidth = bayWidth * roomBays;
    float face = front ? (n.z >= 0.0f ? 0.0f : 1.0f) : (n.x >= 0.0f ? 2.0f : 3.0f);
    float2 cell = floor(float2((front ? p.x : p.z) / roomWidth, (p.y + 0.31f) / floorHeight));
    float seed = float(buildingSeed & 65535u);
    float orderHash = hash21(cell + float2(seed * 0.137f + face * 19.1f, seed * 0.071f + 41.3f));
    float flickHash = hash21(cell.yx + float2(seed * 0.053f + face * 31.7f, seed * 0.113f + 73.1f));
    float storeyCentre = (cell.y + 0.5f) * floorHeight - 0.31f;
    float progress = windowNightProgress();
    float strike = 0.0f;
    float switched = windowIgnition(progress, ignitionOrder(orderHash, storeyCentre), flickHash, strike);
    float2 width = float2((front ? footprint.x : footprint.z) / roomWidth, footprint.y / floorHeight);
    float resolved = 1.0f - smoothstep(0.20f, 0.60f, max(width.x, width.y));
    float average = saturate((progress - 0.01f) / 0.90f);
    float night = lerp(average, switched, resolved);
    return night * lerp(float3(1.0f, 1.0f, 1.0f), float3(0.76f, 0.96f, 1.16f), strike * resolved);
}

// City street furniture (module part 63) and rooftop sets (64), src/render/city_modules.cpp. A tree or
// a lamp is one mesh in one material, told apart by height in its own frame (0 at the ground, 1 at its
// top), so each costs one instance. Trees: bark below 0.30 of the height, leaves varied tree by tree.
// Lamps: a dark steel pole below the lit luminaire. Rooftop shrubs vary their leaves the same way.
// Returns false where the surface does not glow (a lamp's pole).
bool streetFurniture(uint partId, uint m, float localY, uint seed, inout float3 base, inout float4 orm) {
    if (partId < 63u || partId > 65u) return true;
    float h = hash21(float2(float(seed & 4095u) * 0.137, 7.31));
    if (partId == 65u && m == 38u) {
        // A grove's trunks (part 65 keeps them in their own material).
        base = float3(0.15, 0.115, 0.085) * (0.80 + 0.40 * h);
        orm = float4(1.0, 0.86, 0.0, 0.0);
        return true;
    }
    if (m == 10u) {
        if (partId == 63u && localY < 0.30) {
            base = float3(0.15, 0.115, 0.085) * (0.80 + 0.40 * h);
            orm = float4(1.0, 0.86, 0.0, 0.0);
        } else {
            // Subtropical evergreens: camphor, banyan and Taiwan ash in deep greens, a few yellow-green.
            base *= lerp(float3(0.66, 0.84, 0.60), float3(1.12, 1.04, 0.70), h * h);
            if (partId == 63u) base *= 0.70 + 0.40 * saturate((localY - 0.30) / 0.70);
        }
        return true;
    }
    if (partId == 63u && m == 16u && localY < 0.90) {
        base = float3(0.085, 0.09, 0.10);
        orm = float4(1.0, 0.40, 0.80, 0.0);
        return false;
    }
    return true;
}

// Unit-room slabs in tangent space. Face slices are back, left, right, floor and ceiling.
float2 roomCoordinates(InstanceData data, float3 position, float3 T, float3 B, float2 authoredUv) {
    if ((data.flags & 0xffffu) == 62u) return authoredUv;
    float3 offset = position - float3(data.world[0].w, data.world[1].w, data.world[2].w);
    float meters = (act == 2u ? taipeiKit.w : 1.0) / 0.08;
    return float2(dot(offset, T), dot(offset, B)) * meters;
}

// Distance to a limb in the plane of a shop window, in metres.
float limbDistance(float2 p, float2 a, float2 b) {
    float2 segment = b - a;
    return length(p - a - segment * saturate(dot(p - a, segment) / dot(segment, segment)));
}

float shopperMask(float2 window, float3 ray, float wallDistance, float2 filterWidth, uint h) {
    // Stop the sightline at a plane inside the room. A side wall can hide the walker.
    float depth = (0.18 + 0.14 * float((h >> 18u) & 7u) / 7.0) / ray.z;
    if (depth >= wallDistance) return 0.0;
    float phase = float(h & 1023u) / 1024.0;
    float speed = lerp(0.75, 1.18, phase);
    float t = time * (((interFlags & (1u << 14u)) != 0u) ? 0.38f : 1.0f);
    float walkX = frac(phase + t * speed / 5.25) * 5.25 - 1.1;
    if ((h & 1024u) != 0u) walkX = 3.0 - walkX;
    float2 p = (window + ray.xy * depth) * float2(3.0, 3.2) - float2(walkX, 0.23);
    if (abs(p.x) > 0.55 || p.y < -0.08 || p.y > 1.87) return 0.0;
    float stride = sin((t * speed / 1.4 + phase) * kTwoPi);
    p.y -= 0.035 * stride * stride;
    float head = length((p - float2(0.0, 1.54)) * float2(1.0, 0.82)) - 0.12;
    float coat = limbDistance(p, float2(0.0, 0.78), float2(0.0, 1.29)) - 0.145;
    float leg = limbDistance(float2(abs(p.x), p.y), float2(0.04, 0.83),
                             float2(0.045 + 0.27 * abs(stride), 0.03)) - 0.061;
    float arm = limbDistance(p, float2(0.0, 1.28), float2(-0.22 * stride, 0.77)) - 0.062;
    float edge = clamp(3.2 * max(filterWidth.x, filterWidth.y), 0.015, 0.13);
    return 1.0 - smoothstep(-edge, edge, min(min(head, coat), min(arm, leg)));
}

float3 roomEmission(float3 broad, uint seed, float2 meters, float2 meterWidth,
                    float3 toEye, float3 N, float3 T, float3 B, float distanceMeters, bool streetFront) {
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
    float wallDistance = min(back, min(wall.x, wall.y));
    float3 hit = float3(window, 0) + wallDistance * ray;
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
    float occupancy = ((h >> 8) & 7u) < 3u ? 0.13f : 1.0f;
    if (streetFront && occupancy == 1.0 && ((h >> 12u) & 3u) == 0u) {
        room *= 1.0 - 0.89 * shopperMask(window, ray, wallDistance, filterWidth, h);
    }
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

// Four-metre lightboxes recessed beneath the Xinyi arcades. The stationary
// lettering is backlit above a moving band of enamel-colored light.
float3 shopLightbox(float2 uv, float footprint, uint seed) {
    float x = 4.4 - uv.x;
    float night = 1.0 - smoothstep(0.10, 0.72, daylight);
    bool tea = (seed & 1u) != 0u;
    float3 color = tea ? float3(0.045, 0.80, 0.54) : float3(1.25, 0.32, 0.13);
    float phase = float((seed >> 4u) & 255u) / 256.0;
    float speed = ((interFlags & (1u << 14u)) != 0u) ? 0.045f : 0.16f;
    float sweep = 0.5 + 0.5 * cos(kTwoPi * (x / 3.8 - time * speed - phase));
    float band = 1.0 - smoothstep(0.018, 0.032 + footprint, abs(uv.y - 0.065));
    float3 emitted = color * (0.10 + band * lerp(0.15, 1.1, sweep));
    float2 grid = float2(x - (tea ? 1.37f : 1.13f), 0.51 - uv.y) / float2(0.14, 0.075);
    if (all(grid >= 0.0) && grid.x < (tea ? 11.0f : 15.0f) && grid.y < 5.0) {
        uint2 cell = (uint2)grid;
        uint letter = cell.x / 4u;
        // The low three bits are the top row, leftmost column in bit zero.
        uint glyph = tea ? (letter == 0u ? 0x2497u : (letter == 1u ? 0x72cfu : 0x5beau)) // TEA
                         : (letter == 0u ? 0x724fu : (letter == 1u ? 0x5beau : (letter == 2u ? 0x12cfu : 0x72cfu))); // CAFE
        uint column = cell.x % 4u;
        float detail = 1.0 - smoothstep(0.035, 0.13, footprint);
        if (column < 3u && ((glyph >> (cell.y * 3u + column)) & 1u) != 0u) {
            float2 edge = min(frac(grid), 1.0 - frac(grid)) * float2(0.14, 0.075);
            float ink = smoothstep(-footprint, 0.009 + footprint, min(edge.x, edge.y));
            emitted += lerp(color, float3(1.15, 0.96, 0.78), 0.7) * (0.72 + 0.28 * sweep) * ink * detail;
        }
    }
    return emitted * lerp(0.36, 1.0, night);
}

// At street-block distances, occupied wings and floor plates survive the pixel filter
// longer than individual rooms. Use the same averaged elevation in a reflection cone.
float3 shadeBlockWindows(uint m, uint buildingSeed, float3 p, float3 n, float3 footprint,
                         out float3 base, out float rough, out float metallic) {
    bool housing = m == 13u;
    bool jade = m == 5u;
    bool xAxis = abs(n.z) >= abs(n.x);
    float width = housing ? 0.78 : (jade ? 0.98f : 0.69f);
    float floorHeight = housing ? 0.51f : 0.65f;
    float u = (xAxis ? p.x : p.z) / width + float(buildingSeed & 31u);
    float v = p.y / floorHeight;
    float du = (xAxis ? footprint.x : footprint.z) / width;
    float dv = footprint.y / floorHeight;
    float suiteWidth = housing ? 3.0 : (jade ? 4.0f : 6.0f);
    float stackHeight = housing ? 5.0 : (jade ? 8.0f : 6.0f);
    float column = u / suiteWidth;
    float level = (v + float((buildingSeed >> 5u) & 7u)) / stackHeight;

    float pier = facadeStripe(column, housing ? 0.29 : (jade ? 0.16f : 0.075f), du / suiteWidth);
    float floorPlate = facadeStripe(v, housing ? 0.39f : 0.24f, dv);
    float bay = 1.0 - facadeStripe(u, housing ? 0.20f : 0.12f, du);
    float service = facadeStripe(level, housing ? 0.14f : 0.20f, dv / stackHeight);
    float glass = (1.0 - pier) * (1.0 - floorPlate) * bay * (1.0 - service);
    float3 cladding = housing ? float3(0.31, 0.22, 0.14) :
                     (jade ? float3(0.10, 0.20, 0.19) : float3(0.12, 0.17, 0.23));
    float3 glazing = housing ? float3(0.035, 0.049, 0.048) :
                    (jade ? float3(0.016, 0.070, 0.069) : float3(0.021, 0.049, 0.094));
    base = lerp(cladding, glazing, glass);
    rough = lerp(housing ? 0.55f : 0.37f, 0.19, glass);
    metallic = lerp(housing ? 0.15f : 0.49f, 0.16, glass);

    // Tenant leases span a few bays and one to three storeys. Stagger the suites
    // in adjacent wings so the occupied regions don't become continuous bands.
    float leaseWidth = housing ? 1.9 : (jade ? 3.7f : 3.1f);
    float leaseHeight = housing ? 1.7 : (jade ? 3.2f : 2.3f);
    float leaseColumn = floor(u / leaseWidth);
    float seed = float((buildingSeed >> 12u) & 255u);
    float leaseFloor = floor(v / leaseHeight + frac(leaseColumn * 0.618 + seed * 0.27));
    float tenant = hash21(float2(leaseColumn + seed, leaseFloor + m));
    float groupsResolved = 1.0 - smoothstep(0.26, 0.95, max(du / leaseWidth, dv / leaseHeight));
    float typicalOccupancy = (housing ? 0.52f : 0.38f) + 0.21 * (frac(seed * 0.71) - 0.5);
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
        accent = facadeStripe(column - 0.21, housing ? 0.11f : 0.047f, du / suiteWidth)
               * (1.0 - floorPlate * (housing ? 0.83f : 0.30f));
    } else {
        float lobbyWing = facadeStripe(column - 0.51, 0.49, du / suiteWidth);
        accent = facadeStripe(level - 0.17, 0.12, dv / stackHeight) * lobbyWing;
    }
    float3 gallery = housing ? float3(0.51, 0.24, 0.095) :
                    (jade ? float3(0.13, 0.48, 0.43) : float3(0.52, 0.31, 0.14));
    float band = audioBandForBuilding(float(buildingSeed & 255u));
    if (duskOpening() && time > 18.0f && time < 32.0f) {
        float oldNight = 1.0f - smoothstep(0.08f, 0.68f, daylight);
        float3 ignition = facadeIgnition(m, buildingSeed, p, n, footprint);
        // The old curve only lends a faint 10 % floor; each room's own ignition supplies the rest, so the
        // city visibly switches on window by window instead of all at once.
        float floorNight = min(oldNight, 0.1f);
        return (rooms + gallery * accent) * (floorNight + (1.0f - floorNight) * ignition) * (1.0f + 0.20f * band);
    }
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
    float blockWeight = cityExterior() ? smoothstep(0.012, 0.046, max(faceWidth, footprint.y)) : 0.0;
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
    float bayWidth = apartments ? (hotel ? 0.68f : 0.52f) : (hotel ? 0.78 : (campus ? 1.15f : 0.62f));
    float floorHeight = apartments ? 0.49 : (hotel ? 0.57 : (campus ? 0.82f : 0.67f));
    bool frontFacing = abs(geomNormal.z) >= abs(geomNormal.x);
    float u = (frontFacing ? p.x : p.z) / bayWidth;
    float v = (p.y + 0.31) / floorHeight;
    float du = (frontFacing ? footprint.x : footprint.z) / bayWidth;
    float dv = footprint.y / floorHeight;

    float bayGroup = apartments ? 2.0 : (hotel ? 3.0 : (campus ? 4.0f : 6.0f));
    float floorGroup = apartments ? 6.0 : (hotel ? 7.0 : (campus ? 5.0f : 10.0f));
    float seed = (float)m + (frontFacing ? 0.0f : 17.0f);
    float suite = floor(u / bayGroup);
    float storey = floor(v);
    float suiteId = hash21(float2(suite + seed, storey));
    float wingId = hash21(float2(suite + seed, floor(v / (hotel ? 2.0f : 3.0f))));
    float elevation = saturate((p.y + 0.4) / 16.0);
    // Neighboring blocks pick up different amounts of warm and cool glazing.
    float blockTint = 0.5 + 0.5 * sin(0.079 * p.x + 0.112 * p.z + seed);

    float fin = facadeStripe(u / bayGroup, hotel ? 0.089f : 0.065f, du / bayGroup);
    float mullion = facadeStripe(u, hotel ? 0.16f : 0.10f, du);
    float spandrel = facadeStripe(v, hotel ? 0.36f : 0.25f, dv);
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
    float warm = apartments ? 0.78 : (hotel ? 0.78 : (campus ? 0.20f : 0.32f));
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
    if (effectsDetail() > 0u && suiteResolved > 0.0) {
        float roomBays = hotel ? 1.0 : (apartments ? 2.0 : bayGroup);
        float2 aperture = float2(frac(u / roomBays), frac(v));
        float viewU = frontFacing ? toViewer.x : toViewer.z;
        float depth = (hotel ? 0.54 : (apartments ? 0.62f : 0.84f)) / max(abs(dot(geomNormal, toViewer)), 0.28);
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
    if (effectsDetail() == 2u) {
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
    if (duskOpening() && time > 18.0f && time < 32.0f) {
        float3 ignition = facadeIgnition(m, buildingSeed, p, geomNormal, footprint);
        float floorNight = min(night, 0.1f);
        float3 lit = (light + trim * gallery * run * (campus ? 0.85f : 0.48f)) * (floorNight + (1.0f - floorNight) * ignition) * wall;
        return lerp(lit, blockLight, blockWeight);
    }
    return lerp((light + trim * gallery * run * (campus ? 0.85f : 0.48f)) * night * wall, blockLight, blockWeight);
}

bool isAvenue(uint m, uint partId) {
    return (act == 1u || act == 2u) && m == 1u && (partId == 7u || partId == 8u);
}

// Sky subjects reuse the scenery's opaque slots. Override those samples in
// both hit paths so a mirrored sign or reflected wing has the same finish.
bool citySkyFinish(uint part, uint material, inout float3 base, inout float4 ormt) {
    if (part >= 184u && part <= 186u && (material == 25u || material == 27u || material == 18u)) {
        // Navy enamel belt, ivory airframe paint and smoked cabin glazing.
        base = material == 27u ? float3(0.82, 0.80, 0.74)
             : material == 25u ? float3(0.017, 0.050, 0.105) : float3(0.06, 0.11, 0.14);
        ormt = float4(1.0, material == 18u ? 0.09f : 0.29f, material == 18u ? 0.0f : 0.06f, 0.0);
        return true;
    }
    if (part >= 86u && part <= 88u) {
        // The transport has pearl paint, the survey jet has a copper airframe.
        bool transport = part == 86u;
        base = transport ? float3(.60, .72, .72) : float3(.66, .29, .075);
        if (material == 11u) base *= .63;
        if (material == 12u) base = float3(.038, .12, .17);
        if (part == 88u) base = float3(.54, .40, .21);
        ormt = float4(1.0, material == 12u ? .19 : .37, transport ? .28 : .45, 0.0);
        return true;
    }
    if (part == 191u && material == 33u) {
        // "Last Light"'s banner: vermilion habotai silk, a soft sheen, no metal.
        base = float3(0.62, 0.07, 0.035);
        ormt = float4(1.0, 0.45, 0.0, 0.0);
        return true;
    }
    if (part == 172u) {
        if (material == 33u) base = float3(0.31, 0.16, 0.28);       // tinted envelope fabric
        else if (material == 24u) base = float3(0.05, 0.30, 0.28); // fins and gondola
        else if (material == 26u) base = float3(0.54, 0.27, 0.10); // raised seams and brass frames
        else if (material == 11u) base = float3(0.016, 0.025, 0.047);
        else base = float3(0.85, 0.73, 0.47);
        ormt = float4(1.0, material == 26u ? 0.47f : 0.72f, material == 26u ? 0.56f : 0.0f, 0.0);
        return true;
    }
    if (part >= 192u && part <= 196u) {
        // Night heron: pale underside, slate plumage, and black crown and bill.
        if (material == 33u) base = float3(0.60f, 0.59f, 0.54f);
        else if (material == 37u) base = float3(0.035f, 0.038f, 0.045f);
        else base = float3(0.20f, 0.22f, 0.26f);
        ormt = float4(1.0f, 0.82f, 0.0f, 0.0f);
        return true;
    }
    if (part < 160u || part > 169u) return false;
    if (part < 165u) {
        // Warm umber coverts and darker, spread outer flight feathers.
        base = material == 37u ? float3(0.048, 0.040, 0.037) :
               part == 160u ? float3(0.145, 0.095, 0.058) : float3(0.105, 0.075, 0.053);
    } else {
        base = material == 37u ? float3(0.028, 0.032, 0.026) :
               material == 26u ? float3(0.63, 0.31, 0.045) : float3(0.78, 0.77, 0.72);
    }
    ormt = float4(1.0, 0.83, 0.0, 0.0);
    return true;
}

// Thin white wing vanes scatter some of the open sky through to their undersides.
float3 egretFeatherScatter(uint part, uint material, float3 normal) {
    if (part < 165u || part > 169u || material != 33u) return 0.0.xxx;
    float underside = saturate(0.5 - 0.5 * normal.y);
    float sky = lerp(0.19, 0.30, daylight);
    return float3(0.98, 0.95, 0.86) * sky * lerp(0.20, 1.0, underside);
}

float3 policeAirframeFill(uint part, uint material, float3 normal, float3 toEye) {
    if (part != 184u || (material != 27u && material != 25u)) return 0.0.xxx;
    float upward = saturate(0.5 + 0.5 * normal.y);
    float rim = pow(1.0 - saturate(dot(normal, toEye)), 3.0);
    // Cool open sky wraps the paint, with a warmer diffuse return under the
    // hull from the waterfront. Keep the navy belt dark at every angle.
    float3 skyFill = float3(0.15, 0.19, 0.24) * (0.68 + 0.50 * upward);
    float3 cityReturn = float3(0.115, 0.075, 0.044) * (1.0 - upward);
    float3 edge = float3(0.055, 0.072, 0.08) * rim;
    return material == 27u ? skyFill + cityReturn + edge
                           : skyFill * float3(0.08, 0.17, 0.31) + 0.39 * edge;
}

float3 policeGlazing(float3 cabin, float3 reflected, float3 p, float3 n, float3 toEye) {
    float3 ray = reflect(-toEye, n);
    float rim = pow(1.0 - saturate(dot(n, toEye)), 3.0);
    float overcast = smoothstep(-0.10, 0.35, ray.y);
    float3 skyline = reflected * (0.12 + 0.20 * rim);
    float3 skyReflection = skyImpl(ray, p, true) * (0.38 + 0.43 * rim);
    return cabin * float3(0.10, 0.16, 0.20) + lerp(skyline, skyReflection, overcast)
         + overcast * float3(0.035, 0.059, 0.072);
}

float3 blimpEmission(uint material) {
    if (lampsOff()) return 0.0.xxx;
    if (material == 15u) return float3(1.65, 1.17, 0.61);
    return material == 33u ? float3(0.055, 0.025, 0.049) : 0.0.xxx;
}

// Soft sky fill, edge reflections and graphic strips shared by both hit paths.
float3 relayLight(uint part, uint material, float3 normal, float3 toEye) {
    float grazing = 1.0 - saturate(dot(normal, toEye));
    float rim = grazing * grazing;
    float lamps = lampsOff() ? 0.0f : 1.0f;
    if (part == 88u) {
        float3 braid = float3(.39, .27, .14) * (.62 + .38 * rim);
        return braid + float3(1.25, .81, .34) * (material == 15u ? lamps : 0.0);
    }
    bool transport = part == 86u;
    float3 key = transport ? float3(.14, .22, .27) : float3(.27, .12, .052);
    float3 edge = transport ? float3(.17, .38, .46) : float3(.47, .22, .070);
    float3 fill = key * (.67 + .33 * saturate(.5 + .5 * normal.y)) + edge * rim;
    if (material == 12u) return .35 * fill;
    if (material == 15u) return fill + lamps * (transport ? float3(.16, .92, 1.30) : float3(1.40, .57, .12));
    if (material == 6u) return fill + lamps * float3(.09, .86, .43);
    if (material == 16u) return fill + lamps * float3(1.10, .07, .024);
    return fill;
}

// The painted fairings and wings share a waterline in aircraft space. Integrate
// the narrow bronze stripe across the pixel or ray footprint so it recedes cleanly.
void aircraftPaint(uint partId, float localY, float footprint,
                   inout float3 base, inout float rough, inout float metallic) {
    float centre = partId == 100u ? 0.072f : 0.36f;
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

float2 avenueCoordinates(float2 uv) {
    // The mesh packer stores Direct3D V = 1 - the authored Blender lateral coordinate.
    return float2(uv.x, 1.0 - uv.y) * 2.8;
}

// The avenue mesh carries signed distances along and across the road in its asphalt UVs.
float stripCoverage(float offset, float halfWidth, float footprint) {
    float w = max(footprint, 0.008);
    return saturate((min(offset + 0.5 * w, halfWidth) - max(offset - 0.5 * w, -halfWidth)) / w);
}

float3 avenueLighting(float2 road, float2 footprint, float streetCenter, uint partId, float viewAlong, inout float3 base) {
    // A surveyed Xinyi side street can be only 0.32 scene units wide.
    // Work in vehicle-length units so both lanes stay on the ribbon through bends.
    float carScale = act == 2u ? 0.36f : 1.0f;
    road /= carScale;
    footprint /= carScale;
    float dash = facadeStripe(road.x / 2.8, 0.34, footprint.x / 2.8);
    float centerPaint = stripCoverage(road.y, 0.035, footprint.y) * dash;
    base = lerp(base, float3(0.40, 0.31, 0.18), 0.65 * centerPaint);

    float night = sunOverridden() ? 1.0 - timeRamp(0.0, 1.0) : 1.0 - smoothstep(0.08, 0.72, daylight);
    if (night < 0.001) return 0.0.xxx;
    // On curved mapped roads the transverse world coordinate changes along the
    // same lane. The exported UV already contains continuous arc length.
    float streetId = act == 2u ? float(partId) : floor(streetCenter * 2.0 + 0.5);
    bool quiet = (interFlags & (1u << 14u)) != 0u;
    float driveTime = time * (quiet ? 0.22f : 1.0f);
    float3 light = 0.0.xxx;
    [unroll] for (uint lane = 0u; lane < 2u; ++lane) {
        float sign = (lane == 0u) ? -1.0f : 1.0f;
        float across = road.y - sign * (act == 2u ? 0.39f : 0.71f);
        float seed = hash21(float2(streetId, (float)lane + (float)act * 13.0));
        float spacing = 5.8 + 4.8 * seed;
        float phase = (road.x * sign - driveTime * (1.5 + 0.84 * seed) / carScale) / spacing + seed;
        float dw = footprint.x / spacing;
        float carId = hash21(float2(floor(phase + .5), streetId + lane * 3.17));
        float resolved = 1.0 - smoothstep(.10, .45, dw);
        phase += (carId - .5) * .35 * resolved;
        float lengthScale = 1.0 + .65 * smoothstep(.76, .93, carId) * resolved;
        float wheels = stripCoverage(across, 0.23, footprint.y);
        float body = wheels * facadeStripe(phase, lengthScale / spacing, dw);
        float3 roof = lerp(float3(.10,.16,.19),float3(.28,.19,.11),carId * resolved);
        base = lerp(base, roof, .90 * body);

        float pair = stripCoverage(across - 0.16, 0.055, footprint.y)
                   + stripCoverage(across + 0.16, 0.055, footprint.y);
        float nose = .46 * lengthScale;
        float head = facadeStripe(phase - nose / spacing, 0.18 / spacing, dw) * pair;
        float tail = facadeStripe(phase + nose / spacing, 0.16 / spacing, dw) * pair;
        float beam = facadeStripe(phase - (nose + .67) / spacing, 1.34 / spacing, dw)
                   * stripCoverage(across, 0.24, footprint.y);
        float rearSpill = facadeStripe(phase + (nose + .29) / spacing, .58 / spacing, dw) * wheels;
        float facing = saturate(0.50 + 0.65 * sign * viewAlong);
        float3 headlamp = lerp(float3(1.20,.74,.35),float3(.76,1.00,1.17),seed);
        float3 taillamp = float3(1.15,.075,.024);
        light += headlamp * ((1.35 + 2.10 * facing) * head + .21 * beam)
               + taillamp * ((2.9 - 1.35 * facing) * tail + .17 * rearSpill);
    }
    return light * night * (quiet ? 0.61f : 1.0f);
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
        float floorH = (m == 5u) ? 0.34f : 0.31f;
        float bayW   = (m == 5u) ? 0.25f : 0.28f;
        float fy = frac(p.y / floorH);
        float flr = floor(p.y / floorH);
        float perim = p.x * 1.07 + p.z * 1.13;
        float fx = frac(perim / bayW);
        float bay = floor(perim / bayW);
        float bldgId = floor(p.x * 0.12) * 17.0 + floor(p.z * 0.12) * 31.0;

        float mechFloor = (m == 5u) ? step(0.13, frac(flr / 8.0)) : step(0.09, frac((flr + bldgId) / 12.0));
        float rRoom  = hash21(float2(flr * 1.37 + bldgId, bay * 2.19));
        float rFloor = hash21(float2(flr * 0.73, bldgId + 5.1));
        float occThreshold = (m == 13u) ? 0.48 : ((m == 5u) ? 0.52f : 0.56f);
        float litSharp = step(occThreshold, rRoom * 0.72 + rFloor * 0.28) * mechFloor;
        float ceilGrad = 0.55 + 0.45 * fy;
        float dimVar = 0.42 + 0.58 * hash21(float2(bay * 3.1, flr * 1.7));

        float softEdge = clamp(filterW * 0.35, 0.0, 0.22);
        float paneSoft = smoothstep(0.16 - softEdge, 0.24 + softEdge, fy) * (1.0 - smoothstep(0.78 - softEdge, 0.86 + softEdge, fy))
                       * smoothstep(0.12 - softEdge, 0.20 + softEdge, fx) * (1.0 - smoothstep(0.80 - softEdge, 0.88 + softEdge, fx));
        float lodBlend = smoothstep(0.35, 1.25, filterW);
        float winFactor = lerp(paneSoft * litSharp * ceilGrad * dimVar, 0.22 * (0.55 + 0.45 * rFloor) * mechFloor, lodBlend);
        if (effectsDetail() > 0u && filterW < 0.5) {
            // Recessed partitions shift behind the glazing as the camera travels past.
            float3 toEye = norm(eye - p);
            float rearX = fx + (toEye.x * 0.47 + toEye.z * 0.51) * (0.12 + 0.13 * rRoom);
            float partition = smoothstep(0.035, 0.11 + softEdge, abs(rearX - (0.32 + 0.34 * rFloor)));
            float ceiling = smoothstep(0.55, 0.68, fy) * (1.0 - smoothstep(0.73, 0.83, fy));
            float design = 0.60 + 0.24 * partition + 0.56 * ceiling;
            if (effectsDetail() > 1u) {
                float blind = smoothstep(0.16, 0.38 + softEdge, abs(frac(fy * 3.0 + rFloor) - 0.5));
                design *= lerp(1.0, 0.68 + 0.32 * blind, smoothstep(0.46, 0.8, rRoom));
            }
            winFactor *= lerp(design, 1.0, smoothstep(0.15, 0.5, filterW));
        }

        float3 warmTint = float3(0.96, 0.68, 0.36);
        float3 ivoryTint = float3(0.86, 0.88, 0.80);
        float3 coolTint = float3(0.46, 0.74, 0.88);
        float3 winCol = (m == 13u) ? lerp(warmTint, ivoryTint, rRoom * 0.45)
                      : ((m == 5u) ? lerp(ivoryTint, float3(0.42, 0.84, 0.74), 0.42 + 0.30 * rRoom)
                                   : lerp(coolTint, ivoryTint, rRoom * 0.55));
        float intensity = (m == 5u) ? 0.70f : 0.58f;
        // Skyline as a spectrum analyser: each tower's lit rooms swell with its band, and the level
        // climbs the facade like a VU meter (lower storeys respond first).
        float band = audioBandForBuilding(bldgId);
        float meter = smoothstep(0.0, 0.20, band - frac(flr / 40.0) * 0.92);
        float audioGain = 1.0 + (0.14 * band + 0.38 * meter) * (1.0 - lodBlend * 0.5);
        if (duskOpening() && time > 18.0f && time < 32.0f) {
            float baseNight = windowNightProgress();
            float roomHash = hash21(float2(flr * 2.13f + bldgId * 0.41f, bay * 1.79f + 13.7f));
            float flickHash = hash21(float2(bay * 3.47f + bldgId, flr * 1.91f + 29.3f));
            float roomOrder = ignitionOrder(roomHash, (flr + 0.5f) * floorH);
            float roomStrike = 0.0f;
            float roomSwitch = windowIgnition(baseNight, roomOrder, flickHash, roomStrike);
            float ignLod = smoothstep(0.20f, 0.60f, filterW);
            float winNight = lerp(roomSwitch, saturate((baseNight - 0.01f) / 0.90f), ignLod);
            float3 strikeCol = lerp(float3(1.0f, 1.0f, 1.0f), float3(0.76f, 0.96f, 1.16f), roomStrike * (1.0f - ignLod));
            float floorNight = min(nightFactor, 0.1f);
            return winCol * (winFactor * intensity * audioGain) * (floorNight + (1.0f - floorNight) * strikeCol * winNight);
        }
        return winCol * (winFactor * intensity * nightFactor * audioGain);
    }
    if (m == 21u) {
        float2 tuv = mirrorField(p.xz * 0.095 + float2(time * 0.028, -time * 0.018));
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
bool cutoutCoversRay(uint instanceId, uint primitive, float2 bary) {
    uint offset = triangleOffset(instanceId, primitive);
    float2 uv0 = vertices[indices[offset]].uv.xy;
    float2 uv1 = vertices[indices[offset + 1u]].uv.xy;
    float2 uv2 = vertices[indices[offset + 2u]].uv.xy;
    float2 uv = uv0 + bary.x * (uv1 - uv0) + bary.y * (uv2 - uv0);
    uint material = min(sceneInstances[instanceId].materialSlice, 41u);
    return gAlbedoAlphaTex.SampleLevel(anisoWrap, float3(uv, (float)material), 1.0).a >= 0.44;
}

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
            if (cutoutCoversRay(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics())) {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float pathRandom(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (float)((word ^ (word >> 22u)) >> 8u) * (1.0 / 16777216.0);
}

float3 sampleDiffuseDirection(float3 normal, inout uint state) {
    float radius2 = pathRandom(state);
    float phi = kTwoPi * pathRandom(state);
    float3 tangent, bitangent;
    buildOrthonormalBasis(normal, tangent, bitangent);
    return normal * sqrt(1.0 - radius2) + (tangent * cos(phi) + bitangent * sin(phi)) * sqrt(radius2);
}

// One cosine-weighted path per receiver. At the second surface, sample both the
// directional key and an independent diffuse continuation toward the environment.
// The Lambert cosine/PDF factors cancel for both sampled hemisphere directions.
float3 traceDiffusePath(float3 origin, float3 normal, uint state, out float firstHitDistance, inout uint queries) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = sampleDiffuseDirection(normal, state);
    ray.TMin = 0.008;
    ray.TMax = kSceneFar;
    firstHitDistance = ray.TMax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01u, ray);
    ++queries;
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
            if (cutoutCoversRay(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
                q.CommitNonOpaqueTriangleHit();
        }
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return sampleIblEnv(ray.Direction, 0.18, false);

    firstHitDistance = q.CommittedRayT();
    float3 p = origin + ray.Direction * firstHitDistance;
    uint instance = q.CommittedInstanceID();
    uint offset = triangleOffset(instance, q.CommittedPrimitiveIndex());
    Vertex a = vertices[indices[offset]], b = vertices[indices[offset + 1u]], c = vertices[indices[offset + 2u]];
    float2 bary = q.CommittedTriangleBarycentrics();
    float2 uv = a.uv.xy + bary.x * (b.uv.xy - a.uv.xy) + bary.y * (c.uv.xy - a.uv.xy);
    uint material = min(sceneInstances[instance].materialSlice, 41u);
    float3 texCoord = float3(uv, (float)material);
    float mip = clamp(log2(1.0 + firstHitDistance * 1.5), 1.0, 7.0);
    float3 reflectance = saturate(gAlbedoAlphaTex.SampleLevel(anisoWrap, texCoord, mip).rgb);
    float metal = saturate(gOrmtTex.SampleLevel(anisoWrap, texCoord, mip).z);
    reflectance *= 0.96 * (1.0 - metal);
    float3 radiance = emissive(material, p, firstHitDistance * 0.15);
    if (max(reflectance.x, max(reflectance.y, reflectance.z)) < 0.005) return radiance;

    float3 objectNormal = cross(b.p.xyz - a.p.xyz, c.p.xyz - a.p.xyz);
    if (dot(objectNormal, objectNormal) < 1e-10)
        objectNormal = a.n.xyz + bary.x * (b.n.xyz - a.n.xyz) + bary.y * (c.n.xyz - a.n.xyz);
    float3 hitNormal = norm(mul(norm(objectNormal), (float3x3)q.CommittedWorldToObject3x4()));
    if (dot(hitNormal, ray.Direction) > 0.0) hitNormal = -hitNormal;
    float3 nextOrigin = p + 0.012 * hitNormal;
    float3 key = sceneSunDir();
    float cosine = saturate(dot(hitNormal, key));
    if (cosine > 0.0) {
        ++queries;
        if (!traceShadow(nextOrigin, key, kSceneFar))
            radiance += reflectance * sceneSunRadiance() * (cosine / kPi) * cloudKeyTransmission(p, key);
    }
    float3 continuation = sampleDiffuseDirection(hitNormal, state);
    ++queries;
    if (!traceShadow(nextOrigin, continuation, kSceneFar))
        radiance += reflectance * sampleIblEnv(continuation, 0.18, false);
    return radiance;
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
        float vanityBlock = (hitP.x > -0.95 && hitP.x < 0.56 && hitP.y < 3.50 && hitP.z > 5.70) ? 0.14f : 1.0f;
        float3 skyBounce = float3(0.010, 0.010, 0.012)
                         + float3(0.36, 0.33, 0.30) * (faceWin * roomPortalVis * vanityBlock * dawnRamp / (1.0 + 0.28 * dWinH * dWinH));
        float3 sunBounce = sRad * (sunNdotL * inWinPatch * vanityBlock * 0.24);
        float3 toSc = float3(0.08, 4.44, 2.32) - hitP;
        float3 pracBounce = lampsOff() ? (float3)0.0 : (float3(0.20, 0.12, 0.05) / (0.85 + 2.40 * dot(toSc, toSc)));
        giRad = hitAlb * (skyBounce + sunBounce + pracBounce) * smoothstep(0.004, 0.080, hitT);
        if (hm == 15u && !lampsOff()) {
            float emScale = (hitP.z < 3.2) ? 0.85 : ((hitP.y < 3.3) ? 0.12f : 0.24f);
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

bool traceSceneEx(float3 origin, float3 direction, float distance, uint mask, float coneSpread, float coneTravel, bool traceSunShadow, out float3 color, out float outHitT,
                  out uint outHitMat, out float3 outHitPos, out float3 outHitGeomNormal, out float3 outHitNormal,
                  out float3 outMirrorIbl, out float3 outMirrorWeight) {
    outHitT = distance;
    outHitMat = 0u;
    outHitPos = 0.0.xxx;
    outHitGeomNormal = 0.0.xxx;
    outHitNormal = 0.0.xxx;
    outMirrorIbl = 0.0.xxx;
    outMirrorWeight = 0.0.xxx;
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

        float mipLod = clamp(log2(max(coneSpread * (coneTravel + hitT) * 512.0, 1.0)), 0.0, 8.0);
        float3 hCoord = float3(hitUv, (float)hm);
        float3 hBase  = gAlbedoAlphaTex.SampleLevel(anisoWrap, hCoord, mipLod).rgb;
        float2 hNorXY = gNormalTex.SampleLevel(anisoWrap, hCoord, mipLod).xy * 2.0 - 1.0;
        float4 hOrm   = gOrmtTex.SampleLevel(anisoWrap, hCoord, mipLod);
        uint hitPart = (sceneInstances[q.CommittedInstanceID()].flags & 0xffffu);
        float hitLocalY = v0.p.y + bary.x * (v1.p.y - v0.p.y) + bary.y * (v2.p.y - v0.p.y);
        bool hitGlows = streetFurniture(hitPart, hm, hitLocalY, sceneInstances[q.CommittedInstanceID()].buildingSeed, hBase, hOrm);
        if (citySkyFinish(hitPart, hm, hBase, hOrm)) hNorXY = float2(0.0, 0.0);

        float3 wT = mul(o2w, objT);
        wT = wT - hitNGeom * dot(hitNGeom, wT);
        float wt2 = dot(wT, wT);
        float3 T = (wt2 > 1e-8)
            ? (wT * rsqrt(wt2))
            : norm(abs(hitNGeom.y) < 0.99 ? cross(float3(0.0, 1.0, 0.0), hitNGeom) : cross(float3(1.0, 0.0, 0.0), hitNGeom));
        float bSign = (hitFront ? 1.0f : -1.0f) * ((v0.tangent.w >= 0.0) ? 1.0f : -1.0f);
        float3 B = cross(hitNGeom, T) * bSign;

        bool districtFacade = isDistrictGlazing(hm, hitPart);
        float hNormScale = (hm == 34u || hm == 40u || districtFacade) ? 0.04 : saturate(1.0 - mipLod * 0.12);
        hNorXY *= hNormScale;
        float hNz = sqrt(saturate(1.0 - dot(hNorXY, hNorXY)));
        float3 hitN = norm(T * hNorXY.x + B * hNorXY.y + hitNGeom * hNz);
        outHitMat = hm;
        outHitPos = p;
        outHitGeomNormal = hitNGeom;
        outHitNormal = hitN;

        float wetP = (hm == 1u) ? puddleMask(p.xz) : 0.0;
        float hRough = clamp(hOrm.y, 0.03, 0.98);
        if (hm == 1u) hRough = lerp(hRough, 0.08, wetP);
        float hMetal = saturate(hOrm.z);
        float hitUvWidth = coneSpread * (coneTravel + hitT) / max(0.2, abs(dot(hitNGeom, direction)));
        float3 stageTrim = cathedralFinish(hm, hitPart, hitUv, hitUvWidth, hBase, hRough, hMetal);
        if (act == 1u && hm == 11u && hitPart >= 100u && hitPart <= 115u) {
            float bodyY = v0.p.y + bary.x * (v1.p.y - v0.p.y) + bary.y * (v2.p.y - v0.p.y);
            aircraftPaint(hitPart, bodyY, coneSpread * (coneTravel + hitT) * length(w2o[1]), hBase, hRough, hMetal);
        }
        float hCoat  = clearcoatForMaterial(hm, hOrm.w, wetP);
        float hTrans = translucencyForMaterial(hm, hOrm.w);

        float3 facadeLight = 0.0.xxx;
        float3 streetLight = 0.0.xxx;
        if (districtFacade) {
            float hitWidth = coneSpread * (coneTravel + hitT) / max(abs(dot(hitNGeom, direction)), 0.18);
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
                                           -direction, hitNGeom, T, B, length(p-eye)*metersPerUnit, hitPart == 62u);
            }
        } else if (isCurtainWall(hm)) {
            float floorH = (hm == 5u) ? 0.34f : 0.38f;
            float bayW   = (hm == 5u) ? 0.25f : 0.32f;
            float fy = frac(p.y / floorH);
            float fx = frac((p.x * 1.07 + p.z * 1.13) / bayW);
            float cw = smoothstep(0.12, 0.20, fy) * (1.0 - smoothstep(0.82, 0.90, fy))
                     * smoothstep(0.08, 0.15, fx) * (1.0 - smoothstep(0.85, 0.92, fx));
            hBase = lerp(hBase * 0.45, hBase, cw);
        }
        if (isAvenue(hm, hitPart) && objN.y > 0.90) {
            float3 objPos = v0.p.xyz + bary.x * (v1.p.xyz - v0.p.xyz) + bary.y * (v2.p.xyz - v0.p.xyz);
            float2 road = avenueCoordinates(hitUv);
            float crossRoad = (hitPart == 7u ? objPos.z : objPos.x) * (act == 2u ? 0.08f : 1.0f);
            float viewAlong = (hitPart == 7u) ? -direction.x : -direction.z;
            float rayWidth = coneSpread * (coneTravel + hitT);
            if (act == 2u) rayWidth *= 0.08 * length(w2o[0]);
            float2 roadWidth = rayWidth * (1.0 + abs(direction.xz) / max(abs(direction.y), 0.06));
            if (hitPart == 8u) roadWidth = roadWidth.yx;
            streetLight = avenueLighting(road, roadWidth, crossRoad - road.y, hitPart, viewAlong, hBase);
        }

        float3 V2 = -direction;
        float nv2 = max(dot(hitN, V2), 1e-3);
        float3 sunDir = sceneSunDir();
        float3 sunRad = sceneSunRadiance() * cloudKeyTransmission(p, sunDir);

        // Sharp mirrors/glass/water evaluate secondary sun shadow so reflected rooms display real window sunbeams!
        float sunVis2 = 1.0;
        float secShadowConeMax = (accumulating() && accumFrame >= 2u) ? 0.0035f : 0.065f;
        if ((traceSunShadow || coneSpread < secShadowConeMax) && (dot(hitNGeom, sunDir) > 0.0 || hTrans > 0.2)) {
            float3 shOrig = p + hitNGeom * 0.018 + ((hm == 29u || hm == 36u) ? sunDir * 0.020 : 0.0.xxx);
            sunVis2 = traceShadow(shOrig, sunDir, inBath ? 16.0f : 22.0f) ? 0.0f : 1.0f;
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
            float underVan2 = (p.x > -0.95 && p.x < 0.56 && p.y < 3.50 && p.z > 5.68) ? 0.14f : 1.0f;
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
        ibl2 += egretFeatherScatter(hitPart, hm, hitNGeom);
        ibl2 += policeAirframeFill(hitPart, hm, hitNGeom, V2);

        if (hm == 3u || hm == 4u || hm == 18u || hm == 35u || hm == 40u) {
            float fres2 = 0.04 + 0.96 * pow(1.0 - nv2, 5.0);
            float3 beer2 = (hm == 35u) ? float3(0.62, 0.22, 0.04) : ((hm == 4u) ? float3(0.68, 0.92, 0.82) : hBase);
            float3 trans2 = sampleIblEnv(direction, 0.10, inBath) * beer2;
            color = lerp(trans2, specEnv2, clamp(fres2, 0.045, 0.88)) + direct2;
            if (hitPart == 184u && hm == 18u)
                color = policeGlazing(trans2, specEnv2, p, hitN, V2) + direct2;
            return true;
        }

        float3 hitEmission = districtFacade ? facadeLight : emissive(hm, p, coneSpread * (coneTravel + hitT));
        if (hitPart >= 86u && hitPart <= 88u) hitEmission = relayLight(hitPart, hm, hitNGeom, -direction);
        if (hitPart == 172u) hitEmission = blimpEmission(hm);
        if (!hitGlows) hitEmission = 0.0.xxx;
        if (hitPart == 62u && hm == 15u && abs(hitNGeom.y) < 0.55)
            hitEmission = shopLightbox(hitUv, hitUvWidth * taipeiKit.w / 0.08, sceneInstances[q.CommittedInstanceID()].buildingSeed);
        float3 hitCol = direct2 + ibl2 + stageTrim + streetLight + hitEmission;
        float fogTransmission = 1.0f;
        if (cityExterior()) {
            float haze = cityHazeWeight(hitT, origin.y, p.y);
            fogTransmission = 1.0f - haze;
            hitCol = lerp(hitCol, skyAerialFog(direction), haze);
        } else if (!inBath) {
            float hAlt = exp(-max(p.y - 2.0, 0.0) * 0.14);
            float hFog = 1.0 - exp(-max(hitT - (isDawnFinale() ? 95.0f : 4.5f), 0.0) * (isDawnFinale() ? (0.0006 + 0.0020 * hAlt) : 0.0055));
            float fog = hFog * (isDawnFinale() ? 0.36f : 0.72f);
            fogTransmission = 1.0f - fog;
            hitCol = lerp(hitCol, sky(direction), fog);
        }
        if (hm == 12u) {
            // Replace only the stela's IBL specular term when tracing a further bounce.
            float3 specWeight = (f0_2 * brdf2.x + brdf2.y) * hOrm.x;
            outMirrorIbl = specEnv2 * specWeight * fogTransmission;
            outMirrorWeight = saturate(specWeight) * fogTransmission;
        }
        color = hitCol;
        return true;
    }

    color = skyImpl(direction, origin, true) * (isInsideDawnBathroom(origin) ? 0.92f : 1.0f);
    return false;
}

bool traceSceneEx(float3 origin, float3 direction, float distance, uint mask, float coneSpread, out float3 color, out float outHitT) {
    uint hitMat;
    float3 hitPos, hitGeomNormal, hitNormal, mirrorIbl, mirrorWeight;
    return traceSceneEx(origin, direction, distance, mask, coneSpread, 0.0f, false, color, outHitT,
                        hitMat, hitPos, hitGeomNormal, hitNormal, mirrorIbl, mirrorWeight);
}

bool traceScene(float3 origin, float3 direction, float distance, uint mask, float coneSpread, out float3 color) {
    float dummyT = distance;
    return traceSceneEx(origin, direction, distance, mask, coneSpread, color, dummyT);
}

bool traceMirrorScene(float3 origin, float3 direction, bool stela, out float3 color, out uint extraQueries, out float firstHitDistance) {
    extraQueries = 0u;
    firstHitDistance = 65504.0f;
    uint maxBounces = effectsDetail() >= 2u ? 3u : (effectsDetail() == 1u ? 2u : 1u);
    float3 throughput = 1.0.xxx;
    float3 accumulated = 0.0.xxx;
    float3 rayOrigin = origin;
    float3 rayDir = direction;
    float traveled = 0.0f;
    [loop]
    for (uint bounce = 0u; bounce < maxBounces; ++bounce) {
        float opqT = 180.0f;
        uint hitMat;
        float3 hitPos, hitGeomNormal, hitNormal, mirrorIbl, mirrorWeight, segmentColor;
        bool hitOpq = traceSceneEx(rayOrigin, rayDir, 180.0f, 0x01u,
                                   0.0015f + (float)bounce * 0.002f, traveled, true,
                                   segmentColor, opqT, hitMat, hitPos, hitGeomNormal, hitNormal, mirrorIbl, mirrorWeight);
        ++extraQueries;
        if (bounce == 0u && hitOpq) firstHitDistance = opqT;

        // Glass is an affine radiance layer in front of this segment's opaque hit.
        RayDesc gRay;
        gRay.Origin = rayOrigin;
        gRay.Direction = rayDir;
        gRay.TMin = 0.014f;
        gRay.TMax = max(opqT - 0.006f, 0.015f);
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> gq;
        gq.TraceRayInline(scene, RAY_FLAG_NONE, 0x02u, gRay);
        while (gq.Proceed()) {}
        ++extraQueries;
        if (gq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            if (bounce == 0u) firstHitDistance = gq.CommittedRayT();
            uint gOff = triangleOffset(gq.CommittedInstanceID(), gq.CommittedPrimitiveIndex());
            Vertex gv0 = vertices[indices[gOff]];
            Vertex gv1 = vertices[indices[gOff + 1u]];
            Vertex gv2 = vertices[indices[gOff + 2u]];
            uint gm = min(sceneInstances[gq.CommittedInstanceID()].materialSlice, 41u);
            float2 gb = gq.CommittedTriangleBarycentrics();
            float3 gObjN = gv0.n.xyz + gb.x * (gv1.n.xyz - gv0.n.xyz) + gb.y * (gv2.n.xyz - gv0.n.xyz);
            float3x3 gw2o = (float3x3)gq.CommittedWorldToObject3x4();
            float3 gN = norm(mul(gObjN, gw2o));
            if (dot(gN, rayDir) > 0.0f) gN = -gN;

            float gNv = saturate(dot(gN, -rayDir));
            float gFres = clamp((0.04f + 0.96f * pow(1.0f - gNv, 5.0f)) * 1.15f, 0.035f, 0.58f);
            float3 gTint = (gm == 35u) ? float3(0.68f, 0.26f, 0.05f)
                         : ((gm == 4u) ? float3(0.76f, 0.94f, 0.86f) : float3(0.965f, 0.988f, 0.980f));
            bool mirBath = isInsideDawnBathroom(rayOrigin);
            float3 gSpec = sampleIblEnv(reflect(rayDir, gN), 0.035f, mirBath) * 0.45f;
            float3 sDir = sceneSunDir();
            gSpec += pow(saturate(dot(gN, norm(-rayDir + sDir))), 180.0f) * sceneSunRadiance() * 0.65f;
            accumulated += throughput * gSpec * gFres;
            throughput *= gTint * (1.0f - gFres);
        }

        if (!stela || !hitOpq || hitMat != 12u || bounce + 1u >= maxBounces) {
            accumulated += throughput * segmentColor;
            break;
        }
        // Replace the hit stela's IBL specular with the next segment.
        accumulated += throughput * (segmentColor - mirrorIbl);
        throughput *= mirrorWeight * 0.88f;
        if (max(throughput.r, max(throughput.g, throughput.b)) < 0.015f) break;
        float3 nextDir = reflect(rayDir, hitNormal);
        if (dot(nextDir, hitGeomNormal) <= 0.0f) nextDir = reflect(rayDir, hitGeomNormal);
        rayOrigin = hitPos + hitGeomNormal * 0.016f;
        rayDir = nextDir;
        traveled += opqT;
    }
    color = accumulated;
    return firstHitDistance < 65504.0f;
}

// Rough, IBL-shaded receivers still need the geometry seen along their
// reflected view direction. Stop at the closest covered surface, including glass.
float rrReflectionDistance(float3 origin, float3 direction) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.016;
    ray.TMax = kSceneFar;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0x03u, ray);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            cutoutCoversRay(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics()))
            query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? query.CommittedRayT() : 65504.0;
}
#endif


// =============================================================================
// Borrowed NVIDIA RTXDI Reservoir Resampling (ReSTIR DI â€” Weighted RIS)
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
        float lampGate = lampsOff() ? 0.0f : 1.0f;
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
        float scale = (layer == 0u) ? 2.6f : 4.3f;
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
    float base = (act == 1u) ? 0.010 : ((act == 2u) ? 0.015f : 0.013f);
    if (isDawnFinale()) base *= 0.35;
    if ((showFlags & 2u) != 0u) base *= 1.25;
    return base * exp(-max(p.y, 0.0) * 0.085);
}

#if defined(M2_RT)
// Directed single scattering in the city exteriors. The harbor also has a wide
// skylight component, integrated continuously between the lens and each surface.
float3 rtLightShafts(float3 dir, float maxT, float jitter, float surfaceVisibility, inout uint queries) {
    bool harbor = (act == 1u);
    uint steps = harbor ? 4u : 3u;
    float marchT = min(maxT, 90.0);
    float3 sd = sceneSunDir();
    // At night these two sets are lit by practicals in shallow stage haze.
    // Integrate that local layer and reuse key-light visibility at the surface.
    // The two colored stage fixtures have their own finite beam integrals.
    if (!sunOverridden() && (sanctuarySpotsOn() || isGrandTutti())) {
        float reach = min(marchT, 16.0);
        float density = shaftMediumDensity(eye + dir * (0.5 * reach));
        float scattered = 1.0 - exp(-density * reach);
        return sceneSunRadiance() * scattered * lerp(0.035, 0.105, surfaceVisibility);
    }
    float acc = 0.0;
    float tau = 0.0;
    [loop] for (uint s = 0u; s < steps; ++s) {
        float lo = (float)s / (float)steps;
        float hi = (float)(s + 1u) / (float)steps;
        float nearT = harbor ? (90.0 * lo * lo) : (marchT * lo);
        float farT = harbor ? min(marchT, 90.0 * hi * hi) : (marchT * hi);
        if (farT <= nearT) break;
        float dt = farT - nearT;
        float t = nearT + (harbor ? 0.5 : jitter) * dt;
        float3 p = eye + dir * t;
        float dens = shaftMediumDensity(p);
        if (harbor) {
            // Broad harbor skylight has no single shadow direction. Integrate
            // its extinction instead of projecting a few binary tower shadows
            // into sharply outlined slices across the flight path.
            acc += exp(-tau) * (1.0 - exp(-dens * dt)) * 0.66;
        } else if (dens > 1e-5) {
            float lit = traceShadow(p, sd, 80.0) ? 0.0f : 1.0f;
            ++queries;
            acc += lit * dens * exp(-tau) * dt;
        }
        tau += dens * dt;
    }
    float phase = lerp(0.0796, henyeyGreensteinPhase(dot(dir, sd), 0.62), 0.82);
    // A cloud bank is much broader than this short haze march. Sample the
    // moving sky shadow at its midpoint; buildings still occlude each DXR step.
    float3 tint = sceneSunRadiance() * cloudKeyTransmission(eye + dir * (0.5 * marchT), sd);
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
    o.pos = float4(dot(d, right) * fy / aspect, dot(d, up) * fy, (z - 0.1) * (kSceneFar / (kSceneFar - 0.1)), z);
    o.pos.xy += jitterNdc * o.pos.w;
    return o;
}

// Only the temporal upscaling prepass consumes previous transforms. The main
// raster/DXR and NRD shaders retain their normal camera and world-motion inputs.
cbuffer MotionCamera : register(b2) { float4 prevClipRows[3]; };
struct MotionTransform { float4 rows[3]; };
StructuredBuffer<MotionTransform> previousInstances : register(t11);
// Deforming meshes (the PhysX cloth canopy) move vertices inside the vertex buffer itself. Element 0
// holds the moving range (first vertex, count); elements 1.. hold last frame's local positions.
StructuredBuffer<float4> previousDeformed : register(t12);

float3 previousClipXYW(float4 world) {
    return float3(dot(world, prevClipRows[0]), dot(world, prevClipRows[1]), dot(world, prevClipRows[2]));
}

float2 pixelMotion(float2 currentNdc, float3 previousClip) {
    if (previousClip.z <= 0.001) return 0.0.xx;
    float2 pixelScale = float2((float)width, -(float)height) * 0.5;
    return clamp((previousClip.xy / previousClip.z - currentNdc) * pixelScale, -32768.0.xx, 32768.0.xx);
}

struct MotionVertex {
    float4 pos : SV_Position;
    float3 prevClip : TEXCOORD0;
    float2 uv : TEXCOORD1;
    nointerpolation uint mat : TEXCOORD2;
};

MotionVertex VSSceneMotion(uint id : SV_VertexID, uint instanceId : SV_InstanceID) {
    VOut current = VSMain(id, instanceId);
    MotionVertex o;
    o.pos = current.pos;
    o.uv = current.uv.xy;
    o.mat = current.mat;
    uint instance = instanceBase + instanceId;
    uint vertexIndex = indices[meshFirstIndices[sceneInstances[instance].meshId] + id];
    float4 local = float4(vertices[vertexIndex].p.xyz, 1.0);
    uint4 deformed = asuint(previousDeformed[0]);
    if (vertexIndex - deformed.x < deformed.y) local.xyz = previousDeformed[1u + vertexIndex - deformed.x].xyz;
    MotionTransform prev = previousInstances[instance];
    float3 world = float3(dot(prev.rows[0], local), dot(prev.rows[1], local), dot(prev.rows[2], local));
    o.prevClip = previousClipXYW(float4(world, 1.0));
    return o;
}

float2 PSSceneMotion(MotionVertex i) : SV_Target0 {
#if defined(BOO_ALPHA_CUTOUT)
    if (gAlbedoAlphaTex.Sample(anisoWrap, float3(i.uv, (float)min(i.mat,41u))).a < 0.44) discard;
#endif
    float2 ndc = i.pos.xy / float2(width, height) * float2(2.0, -2.0) + float2(-1.0, 1.0) - jitterNdc;
    return pixelMotion(ndc, i.prevClip);
}

// Match the camera-to-surface distance written by PSMain for flat panes.
[earlydepthstencil]
float PSPaneDistance(VOut i) : SV_Target0 {
    return length(i.world - eye);
}

struct Targets {
    float4 color   : SV_Target0;
#if defined(BOO_GLASS_PANE)
    float4 transmittance : SV_Target1;
#else
    float distance : SV_Target1;
    float dofDistance : SV_Target7;
#endif
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
    float alpha = liteMode() ? gAlbedoAlphaTex.Sample(linearWrap, float3(i.uv.xy, (float)matId)).a
                             : gAlbedoAlphaTex.Sample(anisoWrap, float3(i.uv.xy, (float)matId)).a;
    if (alpha < 0.44) {
        discard;
    }
}
#else
#if defined(BOO_GLASS_REFRACTION)
// The copied distance is linear camera-to-surface distance, like dist below.
// Check all four texels used by the bilinear color lookup so foreground
// silhouettes cannot bleed into an otherwise valid background sample.
bool glassSceneSample(float2 uv, float glassDist, float bias, out float3 color) {
    color = 0.0f.xxx;
    if (any(uv < 0.0f) || any(uv >= 1.0f)) return false;
    float4 depths = gSceneDepthCopy.GatherRed(linearClamp, uv);
    if (any(depths < glassDist - bias)) return false;
    color = gSceneColorCopy.SampleLevel(linearClamp, uv, 0.0f).rgb;
    return true;
}

// Returns false only when neither refracted nor unshifted scene pixels are usable.
// The caller then evaluates the environment fallback, avoiding sky work on a hit.
bool glassSceneRefraction(VOut i, float dist, float ndotv, uint matId,
                          float refrBend, bool isCanopy, bool isThinPetal,
                          float3 refrDir, out float3 result,
                          out float3 dispersionRed, out float3 dispersionBlue) {
    int2 dims = int2(max(width, 1u), max(height, 1u));
    float2 baseUv = i.pos.xy / float2(dims);
    float bias = max(0.015f, dist * 0.0025f);
    float3 baseColor;
    bool baseValid = glassSceneSample(baseUv, dist, bias, baseColor);
    result = baseColor;
    dispersionRed = baseColor;
    dispersionBlue = baseColor;
    if (refrBend <= 0.0001f) return baseValid;

    float thick = clamp(i.uv.z, (matId == 35u) ? 0.028f : 0.008f, 0.12f) / max(ndotv, 0.28f);
    float travelScale = isThinPetal ? 2.5f : (isCanopy ? 1.2f : 1.0f);
    float3 d = i.world + refrDir * (thick * travelScale) - eye;
    float z = dot(d, forward);
    if (z <= 0.12f) return baseValid;
    float fy = 1.0f / tan(fov * 0.5f);
    float2 ndc = float2(dot(d, right) * fy / aspect, dot(d, up) * fy) / z + jitterNdc;
    float2 delta = float2(0.5f + 0.5f * ndc.x, 0.5f - 0.5f * ndc.y) - baseUv;
    float deltaLength = length(delta * float2(aspect, 1.0f));
    if (deltaLength > 0.10f) delta *= 0.10f / deltaLength;
    float2 refrUv = baseUv + delta;
    float3 refrColor;
    bool refrValid = glassSceneSample(refrUv, dist, bias, refrColor);
    if (!refrValid) return baseValid;
    float2 edge = min(refrUv, 1.0f - refrUv);
    float fade = smoothstep(0.0f, 0.035f, min(edge.x, edge.y));
    result = baseValid ? lerp(baseColor, refrColor, fade) : refrColor;
    dispersionRed = result;
    dispersionBlue = result;
    if (!liteMode() && effectsDetail() > 0u && !isCanopy && (matId == 3u || matId == 4u)) {
        float3 sampleColor;
        if (glassSceneSample(baseUv + delta * 0.88f, dist, bias, sampleColor))
            dispersionRed = baseValid ? lerp(baseColor, sampleColor, fade) : sampleColor;
        if (glassSceneSample(baseUv + delta * 1.14f, dist, bias, sampleColor))
            dispersionBlue = baseValid ? lerp(baseColor, sampleColor, fade) : sampleColor;
    }
    return true;
}
#endif

[earlydepthstencil]
Targets PSMain(VOut i, bool front : SV_IsFrontFace) {
    uint matId = min(i.mat, 41u);
    uint partId = i.flags & 0xffffu;
    bool aircraft = (act == 1u && partId >= 100u && partId <= 125u);
    bool districtFacade = isDistrictGlazing(matId, partId);
    float3 worldFootprint = fwidth(i.world);
    float2 roomWidth = fwidth(i.roomMeters);
    float2 roadFootprint = 2.8 * fwidth(i.uv.xy);
    float3 texCoord = float3(i.uv.xy, (float)matId);

    // Unconditional anisotropic texture-array fetches (strictly uniform before any cutout discard)
    float4 albSample = liteMode() ? gAlbedoAlphaTex.Sample(linearWrap, texCoord) : gAlbedoAlphaTex.Sample(anisoWrap, texCoord);
    float2 norSample = liteMode() ? gNormalTex.Sample(linearWrap, texCoord).xy : gNormalTex.Sample(anisoWrap, texCoord).xy;
    float4 ormSample = liteMode() ? gOrmtTex.Sample(linearWrap, texCoord) : gOrmtTex.Sample(anisoWrap, texCoord);

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
    float bSign = (front ? 1.0f : -1.0f) * ((i.tangentSign.w >= 0.0) ? 1.0f : -1.0f);
    float3 B = cross(nGeom, T) * bSign;

    float3 base = albSample.rgb;
    bool furnitureGlows = streetFurniture(partId, matId, i.localY, i.seed, base, ormSample);
    // A reflecting pool is deep, dark-bottomed water: at dusk its body would otherwise be lit by the low sun
    // and the warm sky and swamp the few per cent the surface reflects, which is all a pool can reflect
    // seen from above. In "Last Light" the water body is dark, so the sky, the tower and the kites carry it.
    if (matId == 7u && duskOpening()) base *= 0.12;
    float2 nxy = norSample * 2.0 - 1.0;
    if (citySkyFinish(partId, matId, base, ormSample)) nxy = float2(0.0, 0.0);
    float normFade = 1.0 / (1.0 + dist * 0.016);
    if (matId == 34u || matId == 40u) normFade *= 0.03; // Optical silvered mirror & architectural float glass
    if (aircraft && (matId == 11u || matId == 18u)) normFade *= 0.18;
    if (districtFacade) normFade *= 0.04;
    nxy *= normFade;
    float nz = sqrt(saturate(1.0 - dot(nxy, nxy)));
    float3 n = norm(T * nxy.x + B * nxy.y + nGeom * nz);

    float wetPuddle = (matId == 1u) ? puddleMask(i.world.xz) : 0.0;
    float texAO = ormSample.x;
    float rough = clamp(ormSample.y, 0.020, 0.98);
    float metallic = saturate(ormSample.z);
    float auxVal = ormSample.w;
    float2 uvWidth = fwidth(i.uv.xy);
    float3 stageTrim = cathedralFinish(matId, partId, i.uv.xy, max(uvWidth.x, uvWidth.y), base, rough, metallic);
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
    float3 streetLight = 0.0.xxx;
    if (isAvenue(matId, partId) && i.normal.y > 0.90) {
        float2 road = avenueCoordinates(i.uv.xy);
        float crossRoad = (partId == 7u) ? i.world.z : i.world.x;
        if (act == 2u) crossRoad = (crossRoad - ((partId == 7u) ? taipeiKit.z : taipeiKit.x)) * taipeiKit.w;
        float viewAlong = (partId == 7u) ? v.x : v.z;
        streetLight = avenueLighting(road, roadFootprint, crossRoad - road.y, partId, viewAlong, base);
    }
    if (matId == 7u || (matId == 1u && wetPuddle > 0.15)) {
        // "Last Light" opens on the sky reflected in the pool: the evening air is calmer than the night's
        // rain, so the water is near-still and the tower and the kites read in it.
        float calm = (duskOpening() && matId == 7u) ? 0.22f : 1.0f;
        float baseRip = ((matId == 7u) ? 0.010 : (0.005 * wetPuddle)) * (1.0 + 0.28 * audioLow()) * calm;
        float ripScale = baseRip / (1.0 + dist * 0.022);
        float wx = sin(i.world.z * 5.7 + i.world.x * 2.3 + time * 2.4) * 0.55
                 + sin(i.world.z * 12.3 - i.world.x * 6.1 - time * 1.9) * 0.30
                 + cos(i.world.x * 8.9 + i.world.z * 3.4 + time * 1.3) * 0.15;
        float wz = cos(i.world.x * 6.4 - i.world.z * 2.8 - time * 2.1) * 0.55
                 + cos(i.world.z * 9.8 + i.world.x * 5.2 + time * 1.7) * 0.30
                 + sin(i.world.x * 13.7 + time * 2.7) * 0.15;
        n = norm(n + float3(wx * ripScale, 0.0, wz * ripScale));
        // Rain-impact rings on upward-facing wet exterior surfaces, faded out before they can alias.
        float ringAmt = ((matId == 7u) ? 0.010f : 0.016f * wetPuddle) * saturate(1.0 - dist / 55.0) * saturate(n.y * 4.0 - 2.8) * (1.0 + 0.55 * audioLow()) * calm;
        if (ringAmt > 0.0 && !inBath && !liteMode()) {
            // Projected pixel footprint on the surface, scaled by the real canvas height and field of view.
            // 3.4888 = 1440 / (900 * tan(0.43)): identical to the hand-tuned value at the 1440p, 0.86 rad
            // reference, while 4K now keeps ripple detail it can resolve and 720p filters a little harder.
            float pxWorld = dist * tan(fov * 0.5) * 3.4888 / ((float)max(height, 1u) * max(abs(nGeom.y) * saturate(dot(nGeom, v)), 0.06));
            float2 rg = rainRippleGrad(i.world.xz, time, pxWorld);
            n = norm(n - float3(rg.x, 0.0, rg.y) * ringAmt);
        }
    }

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
            facadeLight = roomEmission(facadeLight, i.seed, i.roomMeters, roomWidth, v, nGeom, T, B, dist * metersPerUnit, partId == 62u);
        }
    } else if (isCurtainWall(matId)) {
        float floorH = (matId == 5u) ? 0.34f : 0.38f;
        float bayW   = (matId == 5u) ? 0.25f : 0.32f;
        float fy = frac(i.world.y / floorH);
        float fx = frac((i.world.x * 1.07 + i.world.z * 1.13) / bayW);
        float cwPane = smoothstep(0.12, 0.20, fy) * (1.0 - smoothstep(0.82, 0.90, fy))
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
    float3 sunRad = sceneSunRadiance() * (liteMode() ? 1.0f : cloudKeyTransmission(i.world, sunDir));
    float3 rimDir = isDawnFinale() ? norm(float3(-0.62, 0.42, -0.66)) : norm(float3(0.58, 0.28, 0.76));
    float3 upSanctuaryDir = norm(float3(0.08, -0.86, -0.48));

    bool isWaterDroplet = (inBath && matId == 3u && partId >= 230u && partId < 900u);
    bool isDynamicMarble = (partId >= 900u && partId < 920u);
    bool isThinCrystalPetal = (inBath && matId == 3u && !isWaterDroplet && !isDynamicMarble);
    float ior = isWaterDroplet ? 1.333 : ((matId == 35u) ? 1.52f : 1.50f);
    bool isCanopy = (aircraft || partId == 184u) && matId == 18u;
    float refrBend = isCanopy ? 0.06 : ((matId == 40u) ? 0.0 : (isThinCrystalPetal ? 0.18 : (isWaterDroplet ? 0.36f : 0.65f)));
    float3 refrDir = norm(lerp(-v, refract(-v, n, 1.0 / ior), refrBend));

    float3 sunTrans = 1.0.xxx;
    float rtContactAO = 1.0;
    float3 reflected = sampleIblEnv(reflect(-v, n), rough, inBath);
    float3 transmitted = 0.0f.xxx;
#if defined(BOO_GLASS_REFRACTION)
    float3 ssrRed = 0.0f.xxx;
    float3 ssrBlue = 0.0f.xxx;
    bool sceneHit = isGlass && glassSceneRefraction(i, dist, ndotv, matId, refrBend,
        isCanopy, isThinCrystalPetal, refrDir, transmitted, ssrRed, ssrBlue);
    [branch] if (!sceneHit && (!liteMode() || isGlass)) {
        if (matId == 18u)
            transmitted = float3(0.043f, 0.070f, 0.085f) * (0.65f + 0.35f * daylight);
        else
            transmitted = (inBath && matId != 40u) ? sampleIblEnv(refrDir, 0.06f, true) : skyImpl(refrDir, i.world, !liteMode());
        ssrRed = transmitted;
        ssrBlue = transmitted;
    }
#elif !defined(BOO_GLASS_PANE)
    if (!liteMode() || isGlass)
        transmitted = (inBath && matId != 40u) ? sampleIblEnv(refrDir, 0.06f, true) : skyImpl(refrDir, i.world, !liteMode());
#endif
    bool hasTracedReflection = false;
    bool hasTracedBathGi = false;
    float3 tracedBathGi = (float3)0.0;
    float3 restirDirect = (float3)0.0;
    float glassThickness3D = 0.0;
    bool glassTir = false;
    float diffHitDist = 2.0;
    float specHitDist = 16.0;
    float rrHitDistance = 65504.0;
    uint rayQueryCount = 0u;
    float3 shaftInscatter = (float3)0.0;

#if defined(M2_RT)
    bool reflHit = false;
    bool refrHit = false;
    float paneHitT = 180.0f;
    uint shadowHits = 0u;
    uint aoHits = 0u;
    uint queries = 0u;

    // Temporal decorrelation for both NRD::REFERENCE (stationary) and NRD::REBLUR_DIFFUSE_SPECULAR (motion)
    bool tempSamp = temporalSampling();
    bool pathTemporal = tempSamp || (interFlags & (1u << 17u)) != 0u;
    float pathWeight = 0.0;
    float3 pathIrradiance = 0.0.xxx;
    uint pathLevel = (interFlags >> 15u) & 3u;
#if !defined(BOO_ALPHA_CUTOUT)
    if (pathLevel != 0u && aovMode() == 0u && !isGlass && !isDynamicMarble &&
        matId != 7u && matId != 12u && matId != 17u && matId != 21u && matId != 34u &&
        !(matId == 9u && act == 2u) && metallic < 0.98) {
        // Keep distant silhouettes stable and retain part of the authored fill
        // when temporal history is unavailable. All three levels target the same lighting.
        pathWeight = (1.0 - smoothstep(150.0, 230.0, dist)) * (pathTemporal ? 1.0f : 0.55f);
    }
#endif
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
        rrHitDistance = reflHit ? length(nGeom * 0.018 + reflect(-v,n) * gReflT) : 65504.0;

        if (matId == 40u || isThinCrystalPetal || isCanopy) {
            float3 thinDir = (matId == 40u) ? -v : refrDir;
            refrHit = traceSceneEx(i.world - nGeom * 0.018f, thinDir, 180.0f, 0x01u, 0.003f, transmitted, paneHitT);
            glassThickness3D = isThinCrystalPetal ? 0.012f : 0.008f;
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
        hasTracedReflection = reflHit || ((cityWeatherLighting() || (duskOpening() && matId == 7u)) && reflect(-v, n).y > 0.008);
    } else {
        bool wantRayRefl =
            (matId == 34u) || // Silvered flat mirror (always crisp ray-traced reflection + thin-glass shower transmission!)
            (matId == 7u)  || // Water
            (matId == 12u) || // Obsidian mirror stela
            (aircraft && (matId == 11u || matId == 37u) && dist < 32.0) ||
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
                reflHit = traceMirrorScene(i.world + nGeom * 0.016, reflect(-v, n), matId == 12u, reflected, mQueries, rrHitDistance);
                if (reflHit) rrHitDistance = length(nGeom * 0.016 + reflect(-v,n) * rrHitDistance);
                hasTracedReflection = reflHit || ((cityWeatherLighting() || (duskOpening() && matId == 7u)) && reflect(-v, n).y > 0.008);
                specHitDist = 4.0;
                queries += mQueries;
            } else {
                float spread = 0.004 + rough * rough * 0.22;
                float3 rDir0 = reflect(-v, n);
                if (tempSamp && rough > 0.025) {
                    float3 jB1, jB2;
                    buildOrthonormalBasis(rDir0, jB1, jB2);
                    float lobeScale = reblurMotion() ? 0.36f : 0.55f;
                    float ggxR = rough * rough * sqrt(accumJit / max(1.0 - accumJit * 0.85, 0.05)) * lobeScale;
                    float3 jDir = norm(rDir0 + (jB1 * cos(rotPhi) + jB2 * sin(rotPhi)) * min(ggxR, 0.42));
                    if (dot(jDir, nGeom) > 0.03) rDir0 = jDir;
                }
                float rHitT0 = 180.0;
                reflHit = traceSceneEx(i.world + nGeom * 0.016, rDir0, 180.0, 0x03u, spread, reflected, rHitT0);
                specHitDist = reflHit ? rHitT0 : 32.0;
                rrHitDistance = reflHit ? length(nGeom * 0.016 + rDir0 * rHitT0) : 65504.0;
                // A ray that escapes returns the real sky (traceSceneEx); in "Last Light" the pool shows that
                // dusk sky, clouds and sun, not the environment map.
                hasTracedReflection = reflHit || ((cityWeatherLighting() || (duskOpening() && matId == 7u)) && rDir0.y > 0.008);
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

        if (!wantRayRefl && (interFlags & (1u << 18u)) != 0u) {
            rrHitDistance = rrReflectionDistance(i.world, reflect(-v, n));
            ++queries;
        }

        // Stratified Vogel-disk soft sun shadows + curved-glass refractive Beer-Lambert caustics
        bool canReceiveSun = (matId != 21u && matId != 17u) && ((dot(nGeom, sunDir) > -0.06) || (translucency > 0.18));
        if (canReceiveSun && dist < 85.0) {
            float3 sT1, sT2;
            buildOrthonormalBasis(sunDir, sT1, sT2);
            uint maxShadowSamples = isDawnFinale()
                ? 6u
                : (isGrandTutti() ? ((dist < 6.0) ? 2u : 1u) : ((dist < 9.5) ? 4u : ((dist < 22.0) ? 2u : 1u)));
            float sunRadius = isDawnFinale() ? 0.021f : 0.015f;
            float maxShadowDist = isDawnFinale() ? 18.0f : 28.0f;
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
        if (pathWeight > 0.0) {
            uint seed = (uint)i.pos.x + (uint)i.pos.y * width;
            if (pathTemporal) seed ^= accumFrame * 1597334677u;
            uint pathCount = 1u << (pathLevel - 1u);
            float distanceSum = 0.0;
            [loop] for (uint sampleIndex = 0u; sampleIndex < pathCount; ++sampleIndex) {
                float pathDistance;
                pathIrradiance += traceDiffusePath(i.world + nGeom * 0.010, nGeom,
                    seed ^ (sampleIndex * 277803737u), pathDistance, queries);
                distanceSum += min(pathDistance, 480.0);
            }
            // The worker reads this marker after the fence for this specific draw.
            if ((interFlags & (1u << 19u)) != 0u && pathCount == 4u &&
                (((uint)i.pos.x | (uint)i.pos.y) & 15u) == 0u) {
                InterlockedOr(verified[2], 1u);
            }
            pathIrradiance /= (float)pathCount;
            diffHitDist = max(distanceSum / (float)pathCount, 0.02);
        } else if (inBath && matId != 34u) {
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
        } else if (dist < (isGrandTutti() ? 8.5f : 13.5f) && matId != 34u && matId != 21u && matId != 17u) {
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
                if (dL < (inBath ? 4.8f : 9.5f)) {
                    float3 Ldir = toL / dL;
                    float atten = 1.0 / (1.0 + (inBath ? 1.85f : 0.45f) * d2);
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
                    float atten = 1.0 / (1.0 + (inBath ? 1.85f : 0.45f) * jDist * jDist);
                    float3 fullEval = evalGgxDirect(n, v, jDir, base, max(rough, 0.14), metallic, clearcoat * 0.5) * winInt * atten;
                    float risW = min(res.weightSum / max(res.targetPdf, 1e-5), 4.0);
                    restirDirect = fullEval * risW * (inBath ? 0.48f : 0.38f);
                }
            }
        }
    }

    if (!inBath) {
        shaftInscatter = rtLightShafts(-v, dist, frac(ign + 0.3710), dot(sunTrans, 1.0.xxx / 3.0), queries);
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
    float3 stageIrradiance = 0.0.xxx;
    if (sanctuarySpotsOn()) {
        [unroll] for (uint fixture = 0; fixture < 2; ++fixture) {
            SanctuarySpot spot = sanctuarySpot(fixture == 0u ? -1.0f : 1.0f);
            float3 toward = spot.origin - i.world;
            float radiusSq = max(dot(toward, toward), 0.04);
            float3 lightDirection = toward * rsqrt(radiusSq);
            float cone = smoothstep(0.89, 0.985, dot(-lightDirection, spot.axis));
            // A cut-metal gobo projects five overlapping petals from each spot.
            // Its coordinates follow the real director-steered projection axis.
            float3 beamRight = norm(cross(spot.axis, float3(0.0, 0.0, 1.0)));
            float3 beamUp = cross(beamRight, spot.axis);
            float2 projected = float2(dot(lightDirection, beamRight), dot(lightDirection, beamUp));
            float radius = max(length(projected), 0.001);
            float c = projected.x / radius;
            float c2 = c * c;
            float petal = c * (16.0 * c2 * c2 - 20.0 * c2 + 5.0);
            float boundary = 0.19 + 0.077 * petal;
            float cut = 1.0 - smoothstep(boundary - 0.025, boundary + 0.025, radius);
            cone *= 0.28 + 0.72 * cut;
            float3 incident = spot.radiance * (48.0 * cone / (1.0 + radiusSq));
            directSunOnly += evalGgxDirect(n, v, lightDirection, diffConservBase, rough, metallic, clearcoat) * incident;
            stageIrradiance += incident * (0.28 + 0.72 * abs(dot(nGeom, lightDirection)));
        }
    }

    // The courier casts a warm pool straight down as it banks past the planter.
    if (courierLamp.w > 0.0 && !lampsOff()) {
        float3 toLamp = courierLamp.xyz - i.world;
        float height = toLamp.y;
        float beamWidth = 0.22 + 0.38 * height;
        float radialSq = dot(toLamp.xz, toLamp.xz);
        if (height > 0.0 && height < 9.0 && radialSq < beamWidth * beamWidth) {
            float mask = 1.0 - smoothstep(0.20, 1.0, radialSq / (beamWidth * beamWidth));
            float distanceSq = max(dot(toLamp, toLamp), 0.04);
            float3 L = toLamp * rsqrt(distanceSq);
            float3 incident = float3(1.0, 0.52, 0.18) *
                (26.0 * courierLamp.w * mask * (1.0 - smoothstep(7.0, 9.0, height)) / (1.0 + distanceSq));
            directSunOnly += evalGgxDirect(n, v, L, diffConservBase, rough, metallic, clearcoat) * incident;
            stageIrradiance += incident * (0.25 + 0.75 * abs(dot(nGeom, L)));
        }
    }

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
        float vanityOverhangMask = (i.world.x > -0.95 && i.world.x < 0.56 && i.world.y < 3.53 && i.world.z > 5.72) ? 0.14f : 1.0f;
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
    // The terminal lounge has an 18.4 m strip facing the harbor. Find the
    // point on its long axis closest to the reflection ray, so the highlight
    // rolls continuously over the painted chine as the fuselage banks.
    float3 terminalHighlight = 0.0.xxx;
    if (aircraft && !lampsOff()) {
        float3 mirrorDir = reflect(-v, n);
        float2 stripOffset = float2(2.75, -5.95) - i.world.yz;
        float rayT = max(dot(stripOffset, mirrorDir.yz) / max(dot(mirrorDir.yz, mirrorDir.yz), 0.001), 0.0);
        float stripX = clamp(i.world.x + rayT * mirrorDir.x, -12.2, 6.2);
        float3 toStrip = float3(stripX, 2.75, -5.95) - i.world;
        float d2 = max(dot(toStrip, toStrip), 0.01);
        float3 l = toStrip * rsqrt(d2);
        float falloff = (1.0 - daylight) / (1.0 + 0.014 * d2);
        float3 warm = float3(2.6, 1.52, 0.68) * falloff;
        directSunOnly += evalGgxDirect(n, v, l, base, max(rough, 0.20), metallic, 0.0) * warm * combinedAO;
        terminalHighlight = warm * (0.32 * pow(saturate(dot(mirrorDir, l)), 96.0));
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

#if defined(M2_RT)
    if (pathWeight > 0.0) diffIrr = lerp(diffIrr, min(pathIrradiance, 12.0.xxx), pathWeight);
#endif

    float3 iblSpecEnv = sampleIblEnv(reflect(-v, n), rough, inBath);
#if !defined(M2_RT)
    // A glossy outdoor surface can reflect the weather over its horizon.
    // DXR selects between the actual hit and the sky using the reflected ray.
    float3 weatherRay = reflect(-v, n);
    float skyWeight = smoothstep(0.008, 0.17, weatherRay.y) * (1.0 - smoothstep(0.20, 0.42, rough));
    if (cityWeatherLighting() && skyWeight > 0.01) {
        iblSpecEnv = lerp(iblSpecEnv, skyImpl(weatherRay, i.world, !liteMode()), skyWeight);
    }
#endif
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
    diffIndirectTerm += egretFeatherScatter(partId, matId, nGeom);
    diffIndirectTerm += policeAirframeFill(partId, matId, nGeom, v);
    float2 coatBrdf = envBrdfApprox(0.06, ndotv);
    float3 coatWeight = (clearcoat > 0.02) ? ((0.04 * coatBrdf.x + coatBrdf.y) * clearcoat * 0.65).xxx : 0.0.xxx;
    float3 coatRefl = hasTracedReflection ? lerp(reflected, iblSpecEnv, 0.12) : sampleIblEnv(reflect(-v, n), 0.06, inBath);
    float3 specIndirectTerm = (specRadiance * (f0 * envBrdf.x + envBrdf.y) + coatRefl * coatWeight) * specOcclusion;

    float3 emissiveTerm = stageTrim + streetLight + (districtFacade ? facadeLight : emissive(matId, i.world, dist * 0.0022));
    if (!furnitureGlows) emissiveTerm = 0.0.xxx;
    if (partId >= 86u && partId <= 88u) emissiveTerm = relayLight(partId, matId, nGeom, v);
    if (partId == 172u) emissiveTerm = blimpEmission(matId);
    if (partId == 62u && matId == 15u && abs(nGeom.y) < 0.55)
        emissiveTerm = shopLightbox(i.uv.xy, max(uvWidth.x, uvWidth.y), i.seed);
    if (aircraft && partId >= 110u && partId <= 115u && (matId == 6u || matId == 16u)) {
        // Navigation lamps are continuous and remain identifiable in a still.
        emissiveTerm = ((partId & 1u) == 0u ? float3(2.4, 0.08, 0.025) : float3(0.045, 1.9, 0.72))
                     * (lampsOff() ? 0.0 : lerp(1.0, 0.28, daylight));
    }
    float3 nonIndirectColor = directSunOnly + emissiveTerm;
    if (partId == 191u) {
        // Thin silk lets the low sun through: lit from behind, the banner glows.
        float3 L = sceneSunDir();
        nonIndirectColor += sceneSunRadiance() * base * 0.55 * saturate(dot(-n, L)) * smoothstep(-0.02, 0.06, L.y);
    }
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
        float caustBoost = (((showFlags >> 2u) & 7u) == 2u) ? 0.62f : 0.14f;
        float3 caustCol = lerp(float3(0.020, 0.11, 0.14), float3(0.22, 0.15, 0.07), daylight);
        if (((showFlags >> 2u) & 7u) == 2u) {
            caustCol = ((showFlags & 2u) != 0u) ? float3(0.36, 0.10, 0.38) : float3(0.06, 0.34, 0.32);
        }
        if ((interFlags & (1u << 19u)) != 0u && energy > 0.0f &&
            (((uint)i.pos.x | (uint)i.pos.y) & 7u) == 0u) InterlockedOr(verified[2], 2u);
        nonIndirectColor += (caustCol * energy + neural * (caustBoost * 0.85)) * (0.25 + 0.75 * wave) * ((((showFlags >> 2u) & 7u) == 2u) ? 1.0f : 0.14f);
    }

    // Symbiotic bioluminescent micro-flora garden domes
    if (matId == 22u) {
        float domeGlow = pow(1.0 - ndotv, 1.8);
        float3 bioHue = lerp(float3(0.10, 0.92, 0.68), float3(0.98, 0.68, 0.24), clamp(moodAccent, 0.0, 1.0));
        if ((showFlags & 2u) != 0u) bioHue = lerp(bioHue, float3(0.92, 0.26, 0.78), 0.45);
        nonIndirectColor += bioHue * (0.04 + 0.12 * domeGlow) * (0.75 + 0.30 * moodBloom + 0.15 * moodLight + 0.08 * moodCut) + neural * 0.025;
        // The stage director's lighting output moves a luminous band across each garden dome.
        float latitude = 0.5 + 0.5 * nGeom.y;
        float band = 1.0 - smoothstep(0.03, 0.15, abs(latitude - lerp(0.20, 0.85, moodLight)));
        nonIndirectColor += bioHue * band * (0.08 + 0.22 * moodBloom) * (0.6 + 0.4 * domeGlow);
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
#if defined(BOO_GLASS_PANE)
    float3 paneTransmittance = 0.0f.xxx;
#endif

    // 5. Solid 2-Interface & Thin-Walled Dielectric Glass: Crystal (3), Celadon (4), Smoked Canopy (18), Amber Bottle (35), Float Glass (40), Droplets & Marbles
    if (isGlass) {
        float3 sideCrystalDir = norm(float3(0.78, 0.42, -0.46));
        float specKey  = pow(saturate(dot(n, norm(v + sunDir))), 220.0) * sunVisScalar;
        float specSide = pow(saturate(dot(n, norm(v + sideCrystalDir))), 46.0);
        float specRim  = pow(saturate(dot(n, norm(v + rimDir))), 68.0);
        float specUp   = pow(saturate(dot(n, norm(v - upSanctuaryDir))), 48.0);
        float tirBoost = glassTir ? 1.25f : 1.0f;

        if (matId == 18u) {
            float3 cabin = transmitted;
#if !defined(M2_RT) && !defined(BOO_GLASS_REFRACTION)
            // Raster has no refraction hit behind an opaque draw. Keep the
            // smoked interior dark, preserving the reflected skyline at grazing.
            cabin = float3(0.043, 0.070, 0.085) * (0.65 + 0.35 * daylight);
#endif
            float3 canopyTrans = cabin * float3(0.48, 0.65, 0.72);
            float canopyReflMix = 0.10 + 0.86 * fres;
            color = lerp(canopyTrans, specRadiance, canopyReflMix) + terminalHighlight
                  + specKey  * sunRad * 0.85
                  + specSide * float3(0.10, 0.13, 0.16)
                  + specRim  * float3(0.22, 0.28, 0.35);
            if (partId == 184u)
                color = policeGlazing(cabin, specRadiance, i.world, n, v) + specKey * sunRad * 0.85;
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
#if defined(BOO_GLASS_PANE)
            // The blend lets the real scene through (destination times paneTransmittance), so the pane
            // adds only its reflection and highlight. Adding the environment's guess at what is behind
            // the glass as well laid a blurred copy of the sky over the sharp skyline.
            paneTransmittance = beerLowIron * (1.0f - paneReflMix);
            color = specRadiance * 0.88f * paneReflMix + specKey * sunRad * 0.95f;
#else
            color = lerp(transmitted * beerLowIron, specRadiance * 0.88, paneReflMix)
                  + specKey * sunRad * 0.95;
#endif
        } else if (isWaterDroplet) {
            float dropReflMix = clamp(0.026 + 0.88 * fres, 0.026, 0.88);
            color = lerp(transmitted * float3(0.98, 0.99, 1.0) * tirBoost, specRadiance, dropReflMix)
                  + specKey  * sunRad * 1.45
                  + specSide * float3(0.14, 0.17, 0.21);
        } else {
            // Blown Botanical Liuli Crystal Glass Orchid (3), Celadon Keepsake Vase & Sepals (4), and Crystal/Jade Marbles
            float uBlade = saturate(i.uv.x);
            float vBlade = (i.uv.y < 0.0) ? clamp(i.uv.y, -1.0, 1.0) : clamp(i.uv.y * 2.0 - 1.0, -1.0, 1.0);
            float fallbackThick = clamp(i.uv.z, (matId == 4u) ? 0.020f : 0.014f, 0.10) / max(ndotv, 0.24);
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
                    ? float3(0.012, 0.048, 0.032) * (0.55 + 0.70 * midribDepth) * (inBath ? 0.85f : 1.0f)
                    : float3(0.010, 0.015, 0.018) * (0.25 + 0.75 * midribDepth) * (inBath ? 0.30f : 1.0f));
            float3 rimSheen = ((matId == 4u) ? float3(0.18, 0.30, 0.28) : float3(0.26, 0.32, 0.36))
                            * (edgeRim + 0.40 * crispMargin) * (inBath ? 0.32f : 1.0f);
            float3 nectarGlow = float3(0.32, 0.19, 0.07) * heartWarmth * (1.0 + 0.65 * audioBeat());

            float3 dispersion = 0.0f.xxx;
            if (!liteMode()) {
#if defined(BOO_GLASS_REFRACTION)
                float3 redSky = ssrRed;
                float3 blueSky = ssrBlue;
#else
                float3 redSky  = inBath ? sampleIblEnv(refract(-v, n, 1.0f / 1.488f), 0.08f, true) : sky(norm(refract(-v, n, 1.0f / 1.488f)));
                float3 blueSky = inBath ? sampleIblEnv(refract(-v, n, 1.0f / 1.512f), 0.08f, true) : sky(norm(refract(-v, n, 1.0f / 1.512f)));
#endif
                dispersion = float3(redSky.r - transmitted.r, 0.0, blueSky.b - transmitted.b) * (0.07 * edgeRim);
            }
            float3 glassTrans = max(transmitted * tirBoost + dispersion, 0.0.xxx) * beer + bodyScatter * (1.0 - beer) + nectarGlow;
            if (matId == 3u && partId >= 10u && partId <= 16u) {
                glassTrans += stageIrradiance * (0.014 + 0.085 * midribDepth + 0.045 * crispMargin);
            }
            if (isGrandTutti() && matId == 3u && partId >= 10u && partId <= 16u) {
                // A mineral front climbs the orchid before the tabletop reveal.
                // The clear margins still refract the set throughout the change.
                float growth = smoothstep(276.0, 328.0, time) * (1.0 - smoothstep(333.0, 337.5, time));
                float bladeDepth = uBlade + 0.38 * abs(vBlade);
                float mineral = growth * (1.0 - smoothstep(growth - 0.16, growth + 0.18, bladeDepth));
                float ribs = 0.5 + 0.5 * cos(kTwoPi * (7.0 * uBlade - 1.7 * vBlade * vBlade));
                ribs = lerp(ribs, 0.5, smoothstep(0.025, 0.08, uvWidth.x));
                float3 nacre = lerp(float3(0.09, 0.34, 0.30), float3(0.87, 0.65, 0.40),
                                     saturate(0.20 + 0.55 * ribs + 0.22 * n.y));
                glassTrans = lerp(glassTrans, nacre * (0.45 + 0.42 * saturate(dot(n, sunDir))), mineral * 0.76);
            }
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
                  + specSide * float3(0.18, 0.22, 0.28) * (inBath ? 0.28f : 1.0f)
                  + specRim  * float3(0.38, 0.48, 0.58) * (inBath ? 0.25f : 1.0f)
                  + specUp   * float3(0.45, 0.28, 0.12) * ((act == 2u && !inBath && !isDynamicMarble) ? 1.0f : 0.0f)
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
        color = lipTint * diffLip * (inBath ? 1.25f : 1.0f)
              + specRadiance * (0.38 * fres)
              + float3(0.56, 0.08, 0.24) * (rimRuby * (inBath ? 0.24f : 0.48f))
              + float3(0.24, 0.13, 0.03) * throat * (inBath ? 0.45f : 1.0f)
              + specLip * sunRad * 0.55;
    }

    // Neural Velvet 64-layer Tensor Core SIREN silk-glass canopy ribbon
    if (matId == 21u) {
        float2 ribbon = float2(i.uv.x * 2.0, i.uv.y + i.world.z * 0.06);
        float2 ruv = mirrorField(ribbon + float2(time * 0.04, 0.0));
        float3 siren  = tensorField.SampleLevel(linearClamp, ruv, 0.0).rgb;
        float3 sirenB = tensorField.SampleLevel(linearClamp, mirrorField(ruv.yx * 1.37 - time * 0.03), 0.0).rgb;
        float velvetRim = pow(1.0 - ndotv, 2.4);
        float3 field = siren * 0.70 + sirenB * 0.30;
        float fieldLuma = dot(field, float3(0.2126, 0.7152, 0.0722));
        float3 tone = ((showFlags & 2u) != 0u) ? float3(0.86, 0.42, 0.66) : float3(0.34, 0.72, 0.66);
        if ((interFlags & (1u << 19u)) != 0u && fieldLuma > 0.0f &&
            (((uint)i.pos.x | (uint)i.pos.y) & 7u) == 0u) InterlockedOr(verified[2], 2u);
        float3 silk = lerp(fieldLuma.xxx * tone, field * tone, 0.24);
        float2 motif = i.world.xy * 0.34 + i.world.z * float2(0.08, 0.13);
        float scale = ((interFlags & (1u << 14u)) != 0u) ? 0.0f : 0.16f;
        float2 tiled = mirrorField(motif + scale * float2(sin(time * 0.13), cos(time * 0.09))) * 2.0 - 1.0;
        float filigree = velvetFiligree(tiled, max(length(ddx(motif)), length(ddy(motif))) * 2.0, field);
        silk += (0.12 + 0.32 * saturate(fieldLuma)) * filigree * lerp(tone, float3(1.0, 0.64, 0.30), velvetRim);
        silk += velvetThreads(i.uv.xy, uvWidth, field);
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
        float fogStart = isDawnFinale() ? 95.0f : 4.5f;
        float fog = 1.0 - exp(-max(dist - fogStart, 0.0) * fogDensity);
        float fogW = cityExterior() ? cityHazeWeight(dist, eye.y, i.world.y)
                                    : fog * (isDawnFinale() ? 0.36f : 0.74f);
        float3 fogSky = skyAerialFog(norm(i.world - eye));
        color = lerp(color, fogSky, fogW);
        nonIndirectColor = lerp(nonIndirectColor, fogSky, fogW);
        transScale *= (1.0 - fogW);
        color += shaftInscatter;
        nonIndirectColor += shaftInscatter;
    }

    if (sanctuarySpotsOn()) {
        float3 localHaze = sanctuaryHaze(norm(i.world - eye), dist);
        color += localHaze;
        nonIndirectColor += localHaze;
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
#if defined(BOO_GLASS_PANE)
    // Debug AOVs replace the underlying image, except the glass-color AOV.
    if (curAov != 0u && curAov != 6u) paneTransmittance = 0.0f.xxx;
    o.transmittance = float4(paneTransmittance * transScale, 1.0f);
#else
    o.distance = dist;
    o.dofDistance = dist;
#if defined(M2_RT)
    // A traced miss is sky even when the reflected color uses an IBL fallback.
    if (matId == 12u || matId == 34u) {
        float virtualDistance = reflHit ? dist + rrHitDistance : kSceneFar;
        float brass = (matId == 12u && partId >= 200u && partId <= 207u)
            ? stelaBrassWeight(i.uv.xy, max(uvWidth.x, uvWidth.y)) : 0.0f;
        o.dofDistance = lerp(virtualDistance, dist, brass);
    } else if (matId == 40u) {
        // The flat pane traces straight through; the eye-to-hit distance is physical.
        o.dofDistance = refrHit ? length(i.world - nGeom * 0.018f - v * paneHitT - eye) : kSceneFar;
    }
#elif defined(BOO_GLASS_REFRACTION)
    if (matId == 40u) {
        float behind = gSceneDepthCopy.Load(int3(int2(i.pos.xy), 0));
        o.dofDistance = behind >= dist ? behind : dist;
    }
#endif
#endif

#if defined(M2_RT)
    bool isDenoisable = !isGlass &&
                        (matId != 34u) &&
                        (matId != 12u) &&
                        !(matId == 9u && act == 2u) &&
                        (matId != 21u) &&
                        (matId != 17u) &&
                        (viewZ < 480.0);

    if ((interFlags & (1u << 18u)) != 0u) {
        // The RR input is the complete noisy radiance; no NRD modulation or spatial resolve.
        // The linear guides contain reflectance only (no received irradiance, AO, or exposure).
        o.color      = float4(max(color, 0.0.xxx), 1.0);
        o.normRough  = float4(n, rough);
        o.diffRadHit = float4(isGlass ? 0.0.xxx : saturate(kd * base), 1.0);
        o.specRadHit = float4(saturate(f0 * envBrdf.x + envBrdf.y + coatWeight), 1.0);
        o.modViewZ   = uint4(0u, 0u, 0u, asuint(viewZ));
        o.worldMv    = float4(rrHitDistance, 0, 0, 0); // R16_FLOAT target in the RR pipeline.
    } else if (splitDenoiseOutputs() && curAov == 0u && isDenoisable) {
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

// The orbital insert is a miniature lit in station coordinates. The orthographic
// view keeps the arrays, pressure hulls and visiting capsule in the same focus.
struct OrbitalHit {
    float depth;
    float3 normal;
    float3 albedo;
    float gloss;
};

void orbitalBox(inout OrbitalHit hit, float3 ro, float3 rd, float3 center,
                float3 halfSize, float3 albedo, bool solar) {
    float3 a = (center - halfSize - ro) / rd;
    float3 b = (center + halfSize - ro) / rd;
    float3 nearSlab = min(a, b), farSlab = max(a, b);
    float t = max(max(nearSlab.x, nearSlab.y), nearSlab.z);
    if (t <= 0.0 || t >= hit.depth || t > min(min(farSlab.x, farSlab.y), farSlab.z)) return;
    float3 n = t == nearSlab.x ? float3(-sign(rd.x),0,0) :
               t == nearSlab.y ? float3(0,-sign(rd.y),0) : float3(0,0,-sign(rd.z));
    if (solar && n.y > 0.5) {
        float2 uv = (ro + t * rd - center).xz / halfSize.xz;
        float2 cellUV = (uv * 0.5 + 0.5) * float2(4.0, 11.0);
        float2 cells = abs(frac(cellUV) - 0.5);
        float trace = smoothstep(0.405, 0.495, max(cells.x, cells.y));
        float frame = smoothstep(0.955, 0.99, max(abs(uv.x), abs(uv.y)));
        albedo *= 0.72 + 0.36 * hash21(floor(cellUV)+center.xz*23.0);
        albedo = lerp(albedo, float3(0.29, 0.34, 0.47), trace * 0.43);
        albedo = lerp(albedo, float3(0.58, 0.35, 0.15), frame);
    }
    hit.depth = t; hit.normal = n; hit.albedo = albedo; hit.gloss = solar ? 0.85f : 0.55f;
}

void orbitalModule(inout OrbitalHit hit, float3 ro, float3 rd, float3 a, float3 b,
                   float r, float3 albedo, bool bands) {
    float3 axis = b-a, rel = ro-a;
    float len2 = dot(axis, axis), ad = dot(axis, rd), ar = dot(axis, rel);
    float dr = dot(rd, rel);
    float k2 = len2-ad*ad, k1 = len2*dr-ar*ad;
    float h = k1*k1-k2*(len2*(dot(rel,rel)-r*r)-ar*ar);
    if (h < 0.0) return;
    float t = (-k1-sqrt(h))/max(k2, 0.00001);
    float u = (ar+t*ad)/len2;
    if (u < 0.0 || u > 1.0) {
        float3 cap = u < 0.0 ? rel : ro-b;
        float cd = dot(rd,cap);
        h = cd*cd-dot(cap,cap)+r*r;
        if (h < 0.0) return;
        t = -cd-sqrt(h);
        u = saturate((ar+t*ad)/len2);
    }
    if (t <= 0.0 || t >= hit.depth) return;
    hit.depth = t;
    hit.normal = norm(rel+t*rd-u*axis);
    if (bands) {
        // Blanketed pressure barrels have overlapping sheet panels, two dark
        // docking collars, framed observation ports and an access hatch.
        float3 across = norm(cross(float3(0,1,0),axis));
        float az = atan2(dot(hit.normal,across),hit.normal.y);
        float2 plating = float2(az*(6.0/kTwoPi)+0.5,u*5.0);
        float2 edges = abs(frac(plating)-0.5);
        float seam = smoothstep(0.435,0.497,max(edges.x,edges.y));
        albedo *= (0.84+0.16*hash21(floor(plating)+a.xz))*(1.0-0.35*seam);
        float collar = min(abs(u-0.07),abs(u-0.92));
        albedo = lerp(float3(0.18,0.21,0.23),albedo,smoothstep(0.022,0.044,collar));
        float2 windowUV = float2((u-0.35)/0.26,(az-0.68)/0.30);
        float port = length(float2((frac(windowUV.x+0.5)-0.5)*3.95,windowUV.y));
        if (u>0.19 && u<0.82) {
            albedo = lerp(float3(0.38,0.42,0.42),albedo,smoothstep(0.78,1.06,port));
            albedo = lerp(float3(0.012,0.061,0.092),albedo,smoothstep(0.55,0.80,port));
        }
        float hatch = length(float2((u-0.59)/0.13,(az+0.29)/0.53));
        albedo = lerp(albedo,float3(0.26,0.29,0.29),1.0-smoothstep(0.08,0.22,abs(hatch-0.82)));
    }
    hit.albedo = albedo; hit.gloss = 0.75;
}

float orbitalNoise(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f*f*(3.0-2.0*f);
    return lerp(lerp(hash21(i),hash21(i+float2(1,0)),f.x),
                lerp(hash21(i+float2(0,1)),hash21(i+1.0),f.x),f.y);
}

float orbitalFbm(float2 p) {
    float v = 0.0, w = 0.53;
    [unroll] for (int octave = 0; octave < 5; ++octave) {
        if (liteMode() && octave >= 2) break;
        v += w*orbitalNoise(p);
        p = float2(1.63*p.x+1.19*p.y,-1.19*p.x+1.63*p.y)+float2(7.34,4.47);
        w *= 0.48;
    }
    return v;
}

float3 orbitalEarth(float2 p) {
    // Shallow horizon seen from above the weather. Surface coordinates compress
    // toward the limb, so the weather bands follow the sphere's curvature.
    const float radius = 3.45;
    float2 disc = (p-float2(-0.96,-3.39))/radius;
    float limb = (length(disc)-1.0)*radius;
    float sunward = saturate(0.68-0.27*p.x);
    float3 air = lerp(float3(0.025,0.13,0.35),float3(0.18,0.66,1.12),sunward);
    float3 col = float3(0.0015,0.0033,0.009);
    if (limb > 0.0) {
        float2 tile = p*82.0, id = floor(tile);
        float seed = hash21(id+21.7);
        float2 spot = 0.22+0.56*float2(hash21(id+31.5),hash21(id+8.2));
        float dist = length(frac(tile)-spot) / 82.0;
        float px = 2.0/max(float(height),1.0);
        float star = exp2(-1.9*dist*dist/max(px*px,0.0000017));
        col += step(0.9955,seed)*star*(0.10+0.47*hash21(id+72.9))*
               lerp(float3(0.63,0.78,1.0),float3(1.0,0.86,0.69),hash21(id+5.7));
    } else {
        float3 n = float3(disc,sqrt(saturate(1.0-dot(disc,disc))));
        // A local tangent chart avoids a longitude seam converging at the
        // visible pole. Swirling weather crosses the coasts independently.
        float2 map = float2(dot(n,float3(0.93,-0.18,0.32)),
                            dot(n,float3(-0.37,-0.44,0.82)));
        float2 coast = map*13.0+float2(12.6,3.1);
        float land = orbitalFbm(coast);
        float shore = smoothstep(0.45,0.478,land);
        float arid = orbitalFbm(coast*0.54+21.0);
        float3 earth = lerp(float3(0.072,0.125,0.085),float3(0.31,0.21,0.115),smoothstep(0.38,0.68,arid));
        float3 ocean = lerp(float3(0.013,0.065,0.135),float3(0.024,0.15,0.175),smoothstep(0.37,0.485,land));
        float3 surface = lerp(ocean,earth,shore);
        float2 w = map*26.0;
        float2 wind = float2(orbitalNoise(w*0.24+4.0),orbitalNoise(w*0.24+13.2))-0.5;
        w += wind*2.6;
        float2 storm = w-float2(7.8,3.6);
        float curl = 1.7*exp2(-0.065*dot(storm,storm));
        float cs = cos(curl), sn = sin(curl);
        w = float2(7.8,3.6)+float2(cs*storm.x-sn*storm.y,sn*storm.x+cs*storm.y);
        float frontDistance = w.y+0.34*w.x-5.2+2.5*wind.x;
        float front = exp2(-0.31*frontDistance*frontDistance);
        float airMass = orbitalFbm(w*0.40+15.0);
        float convection = orbitalFbm(w*2.75);
        float tufts = orbitalFbm(w*9.2+8.7);
        float vapor = 0.38*airMass+0.45*convection+0.17*tufts+0.12*front;
        float coverage = smoothstep(0.477,0.632,vapor);
        float3 cloud = lerp(float3(0.39,0.51,0.60),float3(0.91,0.92,0.90),saturate(1.5*tufts-0.20));
        surface = lerp(surface,cloud,coverage);
        float daylightFactor = saturate(dot(n,norm(float3(-0.72,0.52,-0.21))));
        col = surface*(0.15+0.94*daylightFactor);
        float grazing = pow(1.0-n.z,5.0);
        col = lerp(col,air*0.65,0.52*grazing);
    }
    float atmosphere = (0.63*exp2(-190.0*abs(limb))+0.12*exp2(-49.0*abs(limb)))*
                       (1.0-smoothstep(0.037,0.082,abs(limb)));
    col += air*atmosphere;
    return col;
}

float orbitalShadow(float3 origin, float3 sun, float3 center, float3 radii) {
    float3 q = (origin-center)/radii, d = sun/radii;
    float t = -dot(q,d)/dot(d,d);
    float3 gap = q+t*d;
    return smoothstep(0.04,0.13,t)*(1.0-smoothstep(0.83,1.0,dot(gap,gap)));
}

float3 orbitalView(float2 p) {
    float motion = (interFlags & (1u<<14u)) != 0u ? 0.0f : 1.0f;
    float drift = clamp(time-298.0,-3.0,3.0)*motion;
    float3 background = orbitalEarth(p+float2(drift*0.008,0.0));
    p -= float2(0.11+drift*0.030,0.14+drift*0.007);
    if (abs(p.x)>0.9 || abs(p.y)>0.62) return background;

    float3 view = norm(float3(0.42+drift*0.009,0.64,1.0));
    float3 side = norm(cross(float3(0,1,0),view));
    float3 vertical = cross(view,side);
    float2 uv = float2(0.977*p.x+0.213*p.y,-0.213*p.x+0.977*p.y);
    float3 ro = view*12.0 + (side*uv.x + vertical*uv.y)*5.15;
    float3 rd = -view;
    OrbitalHit hit;
    hit.depth = 100.0; hit.normal = 0.0.xxx; hit.albedo = 0.0.xxx; hit.gloss = 0.0;
    float3 nickel = float3(0.58,0.63,0.66), ceramic = float3(0.75,0.74,0.66);
    float3 brass = float3(0.57,0.32,0.115), silicon = float3(0.048,0.092,0.29);
    // The double chord and alternating tubular braces leave real openings
    // between the blanket hinges. Only the bay crossed by this ray can hit.
    orbitalBox(hit,ro,rd,float3(0,-0.088,0.065),float3(3.21,0.019,0.02),nickel,false);
    orbitalBox(hit,ro,rd,float3(0, 0.088,0.065),float3(3.21,0.019,0.02),nickel,false);
    orbitalBox(hit,ro,rd,float3(0,0,-0.076),float3(3.21,0.018,0.022),brass,false);
    float crossedX = ro.x+(0.065-ro.z)*rd.x/rd.z;
    int trussBay = (int)floor((crossedX+3.2)/0.4);
    if (trussBay>=0 && trussBay<16) {
        float x = -3.2+float(trussBay)*0.4;
        float y = ((trussBay & 1)==0) ? 0.088f : -0.088f;
        orbitalModule(hit,ro,rd,float3(x,y,0.065),float3(x+0.4,-y,0.065),0.022,nickel,false);
        orbitalModule(hit,ro,rd,float3(x,0,-0.076),float3(x+0.4,-y,0.065),0.018,brass,false);
    }
    [unroll] for (int end = -1; end <= 1; end += 2) {
        [unroll] for (int bay = 0; bay < 2; ++bay) {
            float x = float(end)*(1.55+float(bay)*1.03);
            orbitalBox(hit,ro,rd,float3(x,-0.025,0),float3(0.027,0.035,1.98),nickel,false);
            orbitalBox(hit,ro,rd,float3(x,0.022,0),float3(0.095,0.105,0.12),brass,false);
            [unroll] for (int wing = -1; wing <= 1; wing += 2) {
                orbitalBox(hit,ro,rd,float3(x,0.04,wing*1.13),float3(0.46,0.025,0.98),silicon,true);
            }
        }
    }
    orbitalModule(hit,ro,rd,float3(0,0,-1.28),float3(0,0,0.3),0.31,ceramic,true);
    orbitalModule(hit,ro,rd,float3(0,0,0.43),float3(0,0,1.15),0.29,nickel,true);
    orbitalModule(hit,ro,rd,float3(-0.8,0,0.13),float3(0.8,0,0.13),0.22,ceramic,true);
    orbitalModule(hit,ro,rd,float3(0,0,1.39),float3(0,0,1.47),0.18,float3(0.18,0.18,0.17),false);
    // The capsule closes the last gap, then holds against the fixed collar
    // before the cut returns to Taipei. Reduced motion shows it berthed.
    float gap = (1.0-smoothstep(-2.75,1.65,drift))*motion;
    float3 capsuleRay = ro-float3(-0.18*gap,0.10*gap,1.34)*gap;
    orbitalModule(hit,capsuleRay,rd,float3(0,0,1.52),float3(0,0,1.78),0.20,brass,true);
    orbitalModule(hit,capsuleRay,rd,float3(0,0,1.84),float3(0,0,2.03),0.158,nickel,false);
    orbitalModule(hit,capsuleRay,rd,float3(-0.065,0.123,1.91),float3(0.065,0.123,1.94),0.048,float3(0.023,0.060,0.08),false);
    orbitalBox(hit,capsuleRay,rd,float3(0,0.02,1.67),float3(0.73,0.025,0.11),silicon,true);
    // Electronics housing and the exposed avionics mast of the rear module.
    orbitalBox(hit,ro,rd,float3(-0.23,0.17,-0.71),float3(0.13,0.13,0.26),nickel,false);
    orbitalModule(hit,ro,rd,float3(-0.27,0.24,-0.91),float3(-0.43,0.70,-0.98),0.021,nickel,false);
    orbitalBox(hit,ro,rd,float3(-0.43,0.71,-0.98),float3(0.15,0.026,0.085),ceramic,false);
    if (hit.depth >= 100.0) return background;
    float3 sun = norm(float3(-0.83,0.45,-0.33));
    float3 world = ro+rd*hit.depth;
    float3 origin = world+hit.normal*0.015;
    float shadow = max(orbitalShadow(origin,sun,float3(0,0,-0.42),float3(0.30,0.30,1.12)),
                       orbitalShadow(origin,sun,float3(0,0,0.79),float3(0.29,0.29,0.56)));
    shadow = max(shadow,orbitalShadow(origin,sun,float3(0,0,0.13),float3(0.96,0.21,0.22)));
    float joint = exp2(-9.0*abs(world.z)-10.0*abs(world.y));
    float visibility = 1.0-0.87*shadow;
    float diffuse = saturate(dot(hit.normal,sun));
    float spec = pow(saturate(dot(hit.normal,norm(view+sun))),23.0)*hit.gloss;
    return hit.albedo*(float3(0.095,0.15,0.22)*(1.0-0.42*joint)
                     +float3(1.52,1.24,0.94)*diffuse*visibility)
           + spec*visibility*float3(0.75,0.58,0.42);
}

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

float2 PSSkyMotion(SkyOut i) : SV_Target0 {
    if ((showFlags & 128u) != 0u) return 0.0.xx; // The orbital insert is authored in screen space.
    float2 p = i.uv * float2(2.0,-2.0) + float2(-1.0,1.0) - jitterNdc;
    float3 direction = forward + (right * p.x * aspect + up * p.y) * tan(0.5 * fov);
    return pixelMotion(p, previousClipXYW(float4(direction,0.0)));
}

#ifdef M2_RT
// Only visible sky pixels need to trace rays or increment the work counters.
[earlydepthstencil]
#endif
Targets PSSky(SkyOut i) {
    float2 p = i.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    if ((interFlags & (1u << 17u)) != 0u) p -= jitterNdc;
    float3 d = norm(forward + right * p.x * aspect * tan(fov * 0.5) + up * p.y * tan(fov * 0.5));
    uint curAov = aovMode();
    bool orbital = (showFlags & 128u) != 0u;
    float3 skyCol;
    [branch] if (orbital) skyCol = orbitalView(float2(p.x*aspect,p.y));
    else skyCol = sky(d);
    if (!orbital && sanctuarySpotsOn() && curAov == 0u)
        skyCol += sanctuaryHaze(d, 16.0);
    if (curAov != 0u) {
        if (curAov == 2u) skyCol = (-d) * 0.5 + 0.5;
        else if (curAov == 3u) skyCol = float3(1.0, 0.0, 1.0);
        else if (curAov == 7u) skyCol = evalAovHeatmap(0.0);
        else if (curAov != 6u) skyCol = 0.0.xxx;
    }
#if defined(M2_RT)
    if (curAov == 0u && !orbital) {
        // Shafts continue seamlessly across silhouettes into open sky (same jitter scheme as PSMain).
        bool tempSampSky = temporalSampling();
        float ignSky = tempSampSky
            ? frac(interleavedGradientNoise(i.pos.xy + float2(5.588238, 3.143218) * (float)(accumFrame & 255u)) + 0.6180340 * (float)accumFrame)
            : interleavedGradientNoise(i.pos.xy);
        uint skyQueries = 0u;
        skyCol += rtLightShafts(d, 90.0, frac(ignSky + 0.3710), 1.0, skyQueries);
        uint waveSkyQueries = WaveActiveSum(skyQueries);
        if (canonical != 0u && WaveIsFirstLane()) {
            InterlockedAdd(verified[1], waveSkyQueries);
        }
    }
#endif
    Targets o;
    o.color = float4(skyCol, 1.0);
#if defined(BOO_GLASS_PANE)
    o.transmittance = 0.0f.xxxx;
#else
    o.distance = kSceneFar;
    o.dofDistance = kSceneFar;
#endif
#if defined(M2_RT)
    o.normRough  = float4(0.0, 1.0, 0.0, 1.0);
    o.diffRadHit = float4(0.0, 0.0, 0.0, 0.0);
    o.specRadHit = float4(0.0, 0.0, 0.0, 0.0);
    o.modViewZ   = uint4(0u, 0u, 0u, asuint(600000.0));
    o.worldMv    = float4(0.0, 0.0, 0.0, 0.0);
    if ((interFlags & (1u << 18u)) != 0u) {
        o.normRough = 0.0.xxxx;
        o.diffRadHit = float4(0.5,0.5,0.5,1);
        o.worldMv.x = 65504.0;
    }
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
