// Same primary triangles for SM 6.0 raster and DXR 1.1 inline-reflection paths.
struct Vertex { float4 p; float4 n; float4 uv; float4 tangent; };
cbuffer Scene : register(b0) {
    float3 eye; float fov;
    float3 right; float aspect;
    float3 up; float time;
    float3 forward; float reveal;
    uint width; uint height; uint act; uint canonical;
    float daylight; float pulse; uint showFlags; float moodAccent;
};
cbuffer Draw : register(b1) {
    float4 row0; float4 row1; float4 row2;
    uint firstIndex; uint material; uint part; uint unused;
};
StructuredBuffer<Vertex> vertices : register(t0);
StructuredBuffer<uint> indices : register(t1);
#if defined(M2_RT)
RaytracingAccelerationStructure scene : register(t2);
RWStructuredBuffer<uint> verified : register(u0);
#endif
Texture2D<float4> tensorField : register(t3);
SamplerState linearClamp : register(s0);

float3 norm(float3 v) { return v * rsqrt(max(dot(v, v), 1e-10)); }
float3 transformPoint(float3 p) { float4 q = float4(p, 1.0); return float3(dot(row0, q), dot(row1, q), dot(row2, q)); }
float3 transformNormal(float3 n) { return norm(float3(dot(row0.xyz, n), dot(row1.xyz, n), dot(row2.xyz, n))); }

float hash21(float2 p) {
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float puddleMask(float2 xz) {
    float a = 0.5 + 0.5 * sin(xz.x * 0.43 + sin(xz.y * 0.31) * 1.4);
    float b = 0.5 + 0.5 * cos(xz.y * 0.67 - sin(xz.x * 0.52) * 1.2);
    return smoothstep(0.36, 0.78, a * 0.58 + b * 0.42);
}

struct VOut {
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 uv : TEXCOORD2;
    nointerpolation uint mat : TEXCOORD3;
};

VOut VSMain(uint id : SV_VertexID) {
    Vertex v = vertices[indices[firstIndex + id]];
    VOut o;
    o.world = transformPoint(v.p.xyz);
    o.normal = transformNormal(v.n.xyz);
    o.uv = v.uv;
    o.mat = (uint)v.p.w;
    float3 d = o.world - eye;
    float z = dot(d, forward);
    float fy = 1.0 / tan(fov * 0.5);
    o.pos = float4(dot(d, right) * fy / aspect, dot(d, up) * fy, z * 1000.0 / 999.9 - 0.1 * 1000.0 / 999.9, z);
    return o;
}

float3 sky(float3 d) {
    float horizon = pow(saturate(1.0 - abs(d.y)), 3.6);
    float urbanGlow = pow(saturate(1.0 - max(d.y + 0.04, 0.0) * 3.8), 3.0);

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

    float3 sunDir = norm(float3(-0.48, 0.16 + 0.22 * daylight, 0.72));
    float sun = pow(saturate(dot(d, sunDir)), 180.0);
    float sunHalo = pow(saturate(dot(d, sunDir)), 10.0);
    float3 c = clearSky + (sun * float3(3.2, 1.9, 0.85) + sunHalo * float3(0.32, 0.16, 0.06)) * daylight;

    // Verified Tensor field drives subtle high-altitude cloud irradiance without horizon banding.
    float2 uv = float2(atan2(d.x, d.z) / 6.2831853 + 0.5, acos(clamp(d.y, -1.0, 1.0)) / 3.14159265);
    float3 neural = tensorField.SampleLevel(linearClamp, uv, 0.0).rgb;
    float nLum = dot(neural, float3(0.2126, 0.7152, 0.0722));
    float highCloud = smoothstep(0.04, 0.34, d.y) * (1.0 - smoothstep(0.45, 0.88, d.y));
    c += lerp(float3(0.018, 0.034, 0.052), float3(0.18, 0.12, 0.07), daylight) * nLum * (0.28 + 0.55 * daylight) * highCloud;

    if (showFlags == 0u) {
        float mist = 1.0 - smoothstep(0.0, 2.0, min(abs(time - 60.0), abs(time - 120.0)));
        c = lerp(c, float3(0.28, 0.33, 0.36), mist);
    }
    return c;
}

float3 baseColor(uint m) {
    if (m == 0u)  return float3(0.034, 0.044, 0.056); // Structural steel / dark bronze-graphite
    if (m == 1u)  return float3(0.018, 0.023, 0.030); // Wet asphalt / basalt slate deck
    if (m == 2u)  return float3(0.165, 0.148, 0.128); // Architectural stone / concrete / sleeper
    if (m == 3u)  return float3(0.76, 0.85, 0.86);    // Low-iron blown crystal glass (broad petals)
    if (m == 4u)  return float3(0.58, 0.80, 0.75);    // Pale celadon crystal glass (sepals, column, buds)
    if (m == 5u)  return float3(0.020, 0.088, 0.108); // Taipei 101 jade-blue curtain wall
    if (m == 6u)  return float3(0.06, 0.74, 0.56);    // Cyan/jade architectural accent
    if (m == 7u)  return float3(0.009, 0.034, 0.050); // River / basin / reflecting pool water
    if (m == 8u)  return float3(0.86, 0.58, 0.16);    // 24k gold / brushed brass (TMD, callus, medallions)
    if (m == 9u)  return float3(0.42, 0.048, 0.18);   // Orchid labellum crimson-plum / Pearl sphere
    if (m == 10u) return float3(0.018, 0.068, 0.034); // Matte dark wet botanical foliage
    if (m == 11u) return float3(0.80, 0.82, 0.85);    // Porcelain anther cap / aerospace white-titanium
    if (m == 12u) return float3(0.11, 0.13, 0.15);    // Polished steel / obsidian mirror stela
    if (m == 13u) return float3(0.048, 0.042, 0.040); // Warm mixed-use / hotel facade
    if (m == 14u) return float3(0.026, 0.046, 0.068); // Cool high-rise glass facade
    if (m == 15u) return float3(0.76, 0.54, 0.28);    // Warm architectural interior / soffit
    if (m == 16u) return float3(0.95, 0.55, 0.16);    // Warm streetlamp / approach / beacon
    if (m == 17u) return float3(0.42, 0.48, 0.54);    // Rotor motion disc
    if (m == 18u) return float3(0.022, 0.045, 0.072); // Tinted cockpit canopy glass
    if (m == 19u) return float3(0.013, 0.022, 0.030); // Distant Taipei Basin misty mountain ridge
    if (m == 20u) return float3(0.84, 0.81, 0.75);    // Sculpted ivory hydroxyapatite tooth-crown enamel
    if (m == 21u) return float3(0.08, 0.22, 0.26);    // Neural Velvet 64-layer Tensor Core SIREN ribbon
    if (m == 22u) return float3(0.06, 0.38, 0.26);    // Symbiotic bioluminescent oral micro-flora
    return float3(0.70, 0.74, 0.76);
}

float3 emissive(uint m, float3 p, float filterW) {
    // Windows and urban lights stay off in late-afternoon sun (daylight ~ 0.9) and ignite across dusk into night
    float nightFactor = smoothstep(0.68, 0.08, daylight);
    if (m == 6u) {
        return float3(0.05, 0.86, 0.66) * (0.62 + 0.18 * pulse) * (0.08 + 0.92 * nightFactor);
    }
    if (m == 15u) {
        return float3(0.56, 0.36, 0.16) * (0.04 + 0.96 * nightFactor) * (1.0 + 0.08 * pulse);
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

        // Soften window mullion edges smoothly along curved secondary ray cones while preserving warm glowing rooms
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
        return nSample * accent * (1.35 + 0.35 * pulse);
    }
    if (m == 22u) {
        float3 floraTint = lerp(float3(0.08, 0.88, 0.64), float3(0.96, 0.62, 0.20), clamp(moodAccent, 0.0, 1.0));
        if ((showFlags & 2u) != 0u) floraTint = lerp(floraTint, float3(0.88, 0.24, 0.74), 0.45);
        return floraTint * (0.36 + 0.22 * pulse) * (0.25 + 0.75 * nightFactor);
    }
    return (float3)0.0;
}

#if defined(M2_RT)
bool traceScene(float3 origin, float3 direction, float distance, uint mask, float coneSpread, out float3 color) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.020;
    ray.TMax = distance;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, mask, ray);
    while (q.Proceed()) {}
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        uint offset = q.CommittedInstanceID() + q.CommittedPrimitiveIndex() * 3u;
        Vertex v = vertices[indices[offset]];
        uint hm = (uint)v.p.w;
        float hitT = q.CommittedRayT();
        float3 p = origin + direction * hitT;
        float3 hitCol = baseColor(hm) * (0.26 + 0.74 * daylight) + emissive(hm, p, coneSpread * hitT);
        float hFog = 1.0 - exp(-max(hitT - 4.0, 0.0) * 0.006);
        color = lerp(hitCol, sky(direction), hFog * 0.72);
        return true;
    }
    color = sky(direction);
    return false;
}

bool traceShadow(float3 origin, float3 direction, float distance) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.022;
    ray.TMax = distance;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01u, ray);
    while (q.Proceed()) {}
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}
#endif

struct Targets { float4 color : SV_Target0; float distance : SV_Target1; };

Targets PSMain(VOut i, bool front : SV_IsFrontFace) {
    float3 n = norm(front ? i.normal : -i.normal);
    float3 v = norm(eye - i.world);
    float dist = length(i.world - eye);
    bool isGlass = (i.mat == 3u || i.mat == 4u || i.mat == 18u);

    float wetPuddle = (i.mat == 1u) ? puddleMask(i.world.xz) : 0.0;
    if (i.mat == 7u || (i.mat == 1u && wetPuddle > 0.15)) {
        float baseRip = (i.mat == 7u) ? 0.011 : (0.006 * wetPuddle);
        float ripScale = baseRip / (1.0 + dist * 0.022);
        // Multi-frequency cross-chop avoids artificial sinusoidal sawtooth artifacts on reflected vertical tower edges
        float wx = sin(i.world.z * 5.7 + i.world.x * 2.3 + time * 2.4) * 0.55
                 + sin(i.world.z * 12.3 - i.world.x * 6.1 - time * 1.9) * 0.30
                 + cos(i.world.x * 8.9 + i.world.z * 3.4 + time * 1.3) * 0.15;
        float wz = cos(i.world.x * 6.4 - i.world.z * 2.8 - time * 2.1) * 0.55
                 + cos(i.world.z * 9.8 + i.world.x * 5.2 + time * 1.7) * 0.30
                 + sin(i.world.x * 13.7 + time * 2.7) * 0.15;
        n = norm(n + float3(wx * ripScale, 0.0, wz * ripScale));
    }

    float ndotv = saturate(dot(n, v));
    float fres = 0.035 + 0.965 * pow(1.0 - ndotv, 4.4);
    float3 keyDir = norm((act == 1u) ? float3(-0.48, 0.22 + 0.28 * daylight, 0.72) : float3(-0.44, 0.74, -0.32));
    float3 rimDir = norm(float3(0.58, 0.28, 0.76));
    float3 upSanctuaryDir = norm(float3(0.08, -0.86, -0.48));

    // Restrained refraction bend keeps background silhouettes optically coherent rather than shattered
    float3 refrDir = norm(lerp(-v, refract(-v, n, 1.0 / 1.49), 0.42));

    float ao = 1.0;
    float3 reflected = sky(reflect(-v, n));
    float3 transmitted = sky(refrDir);

#if defined(M2_RT)
    bool reflHit = false;
    bool secondHit = false;
    if (isGlass) {
        reflHit   = traceScene(i.world + n * 0.03, reflect(-v, n), 180.0, 0x01u, 0.012, reflected);
        secondHit = traceScene(i.world - n * 0.03, refrDir,        180.0, 0x01u, 0.014, transmitted);
    } else {
        reflHit   = traceScene(i.world + n * 0.024, reflect(-v, n), 180.0, 0x03u, 0.005, reflected);
        secondHit = traceShadow(i.world + n * 0.024, keyDir, 3.0);
        ao = secondHit ? 0.58 : 1.0;
    }
    uint waveQueries = WaveActiveSum(2u);
    if (canonical != 0u && WaveIsFirstLane()) InterlockedAdd(verified[1], waveQueries);
    if (canonical != 0u && (((uint)i.pos.x & 31u) == 0u) && (((uint)i.pos.y & 31u) == 0u)) {
        InterlockedAdd(verified[0], (reflHit ? 1u : 0u) + (secondHit ? 1u : 0u));
    }
#endif

    float3 base = baseColor(i.mat);
    float cwPane = 1.0;
    if (i.mat == 5u || i.mat == 13u || i.mat == 14u) {
        // Curtain-wall floor spandrels & vertical mullions articulate skyscrapers in daylight as well as night
        float floorH = (i.mat == 5u) ? 0.34 : 0.38;
        float bayW   = (i.mat == 5u) ? 0.25 : 0.32;
        float fy = frac(i.world.y / floorH);
        float fx = frac((i.world.x * 1.07 + i.world.z * 1.13) / bayW);
        cwPane = smoothstep(0.12, 0.20, fy) * (1.0 - smoothstep(0.82, 0.90, fy))
               * smoothstep(0.08, 0.15, fx) * (1.0 - smoothstep(0.85, 0.92, fx));
        float panelVar = 0.86 + 0.28 * hash21(floor(float2((i.world.x + i.world.z) / bayW, i.world.y / floorH)));
        base = lerp(base * 0.42, base * panelVar, cwPane);
    }
    float3 neural = tensorField.SampleLevel(linearClamp, frac(i.world.xz * 0.035), 0.0).rgb;
    float diffuse = 0.22 + 0.78 * saturate(dot(n, keyDir));
    float rimLight = pow(saturate(dot(n, rimDir)), 2.2) * 0.24;

    float3 nightKey = (act == 1u) ? float3(0.24, 0.36, 0.54) : float3(0.42, 0.64, 0.92);
    float3 keyTint = lerp(nightKey, float3(1.42, 1.02, 0.68), daylight);
    float3 rimTint = lerp(float3(0.32, 0.48, 0.68), float3(0.85, 0.66, 0.48), daylight);
    float3 color = base * (diffuse * keyTint + rimLight * rimTint) * ao + emissive(i.mat, i.world, dist * 0.0022);

    // Subtle upward vertiport pad bounce illumination on hovering eVTOL airframe & wings at night
    if (act == 1u && i.world.y > 3.72 && i.world.y < 7.2) {
        float padDist = length((i.world - float3(0.0, 3.70, -2.0)) * float3(0.22, 0.38, 0.22));
        float padBounce = exp(-padDist * padDist * 2.2) * (0.35 + 0.65 * saturate(-n.y)) * (1.0 - daylight);
        color += base * float3(0.26, 0.68, 0.62) * padBounce;
    }

    float rough = clamp(i.uv.w, 0.04, 0.95);
    if (i.mat == 1u)  rough = lerp(0.48, 0.06, wetPuddle);
    if (i.mat == 12u) rough = 0.09;
    if (i.mat == 10u || i.mat == 19u) rough = 0.92;

    float spec = pow(saturate(dot(n, norm(v + keyDir))), lerp(150.0, 8.0, rough));
    float specMask = (i.mat == 10u || i.mat == 19u) ? 0.06 : (1.0 - 0.55 * rough);
    float3 specTint = (i.mat == 8u) ? float3(1.65, 1.18, 0.46) : lerp(float3(0.78, 0.90, 1.0), float3(1.28, 0.96, 0.66), daylight);
    color += spec * specTint * ao * specMask;

    float reflWeight = 0.12;
    if (i.mat == 7u)  reflWeight = 0.78;
    else if (i.mat == 12u) reflWeight = 0.68;
    else if (i.mat == 1u)  reflWeight = lerp(0.04, 0.62, wetPuddle);
    else if (i.mat == 5u || i.mat == 14u || i.mat == 13u) reflWeight = ((i.mat == 13u) ? 0.18 : 0.34) * (0.30 + 0.70 * cwPane);
    else if (i.mat == 8u)  reflWeight = 0.56;
    else if (i.mat == 10u || i.mat == 19u) reflWeight = 0.015;

    float3 reflTint = (i.mat == 8u) ? float3(1.0, 0.74, 0.30) : float3(1.0, 1.0, 1.0);
    color += reflected * reflTint * reflWeight * (0.28 + 0.72 * fres);

    // Metallic 24k Gold (TMD sphere, callus crest, ruyi medallions, brass collar)
    if (i.mat == 8u) {
        float upCatch = 0.38 + 0.62 * saturate(-n.y * 0.45 + 0.55);
        float rimGold = pow(1.0 - ndotv, 2.2);
        // Dedicated architectural spotlighting on Taipei 101's 88F-91F 5.5 m Tuned Mass Damper sphere
        float tmdSpot = (act == 2u && i.world.y > 30.0 && i.world.y < 36.0) ? 1.55 : 1.0;
        color = color * tmdSpot + (base * float3(0.58, 0.40, 0.14) * upCatch + float3(0.72, 0.48, 0.16) * rimGold * 0.50) * tmdSpot;
    }

    // Blown Botanical Crystal Glass: broad low-iron petals (3), celadon keeled sepals & buds (4), smoked canopy (18)
    if (isGlass) {
        float3 sideCrystalDir = norm(float3(0.78, 0.42, -0.46));
        float specKey  = pow(saturate(dot(n, norm(v + keyDir))), 180.0);
        float specSide = pow(saturate(dot(n, norm(v + sideCrystalDir))), 42.0);
        float specRim  = pow(saturate(dot(n, norm(v + rimDir))), 65.0);
        float specUp   = pow(saturate(dot(n, norm(v - upSanctuaryDir))), 48.0);

        if (i.mat == 18u) {
            // Smoked aerospace polycarbonate cockpit canopy: dark interior tint + crisp sun/sky specular
            float3 canopyTrans = transmitted * float3(0.14, 0.17, 0.22) + float3(0.012, 0.016, 0.022);
            float canopyReflMix = clamp(0.08 + 0.46 * fres, 0.0, 0.62);
            color = lerp(canopyTrans, reflected * 0.88, canopyReflMix)
                  + specKey  * float3(1.35, 1.32, 1.28)
                  + specSide * float3(0.10, 0.13, 0.16)
                  + specRim  * float3(0.22, 0.28, 0.35);
        } else {
            float uBlade = saturate(i.uv.x);
            float vBlade = clamp(i.uv.y, -1.0, 1.0);
            float thick = clamp(i.uv.z, 0.012, 0.10);
            float opticalPath = thick / max(ndotv, 0.24);

            // Water-clear low-iron crystal for broad petals vs subtle cool-celadon depth in thicker keeled sepals
            float3 absorbRate = (i.mat == 4u) ? float3(3.2, 1.05, 1.35) : float3(1.15, 0.55, 0.60);
            float3 beer = exp(-absorbRate * opticalPath);

            // Curvature & thickness cues: midrib ridge, rolled luminous rim, and warm floral-heart internal glow
            float edgeRim = pow(1.0 - ndotv, 2.5);
            float crispMargin = pow(saturate(abs(vBlade)), 5.0) * smoothstep(0.10, 0.30, uBlade);
            float midribDepth = exp(-14.0 * vBlade * vBlade) * (1.0 - 0.45 * uBlade);
            float heartWarmth = (act == 2u) ? (exp(-uBlade * 5.6) * (1.0 - 0.45 * abs(vBlade))) : 0.0;

            float3 bodyScatter = (i.mat == 4u)
                ? float3(0.016, 0.036, 0.034) * (0.40 + 1.10 * midribDepth)
                : float3(0.018, 0.024, 0.028) * (0.30 + 0.90 * midribDepth);
            float3 rimSheen = ((i.mat == 4u) ? float3(0.28, 0.42, 0.40) : float3(0.38, 0.44, 0.48)) * (edgeRim + 0.45 * crispMargin);
            float3 nectarGlow = float3(0.32, 0.19, 0.07) * heartWarmth;

            float3 glassTrans = transmitted * beer + bodyScatter + nectarGlow;
            float reflMix = clamp(0.16 + 0.78 * fres, 0.0, 0.93);
            color = lerp(glassTrans, reflected * float3(1.05, 1.08, 1.12), reflMix)
                  + rimSheen
                  + specKey  * float3(1.55, 1.58, 1.62)
                  + specSide * float3(0.20, 0.25, 0.30)
                  + specRim  * float3(0.45, 0.58, 0.70)
                  + specUp   * float3(0.48, 0.30, 0.13) * (act == 2u ? 1.0 : 0.0)
                  + neural * (0.015 * edgeRim);
        }
    }

    // Orchid Trilobed Labellum (9): deep hand-blown ruby-burgundy Liuli crystal with golden-nectar throat
    if (i.mat == 9u && act == 2u) {
        float uLip = saturate(i.uv.x);
        float vLip = clamp(i.uv.y, -1.0, 1.0);
        float throat = exp(-uLip * 3.6) * exp(-2.4 * vLip * vLip);
        float stria = 0.5 + 0.5 * cos(vLip * 22.0);
        float throatVeins = exp(-uLip * 2.4) * stria * (1.0 - throat) * 0.35;
        float rimRuby = pow(1.0 - ndotv, 2.2);
        float specLip = pow(saturate(dot(n, norm(v + keyDir))), 135.0);
        float3 sideCrystalDir = norm(float3(0.78, 0.42, -0.46));
        float specLip2 = pow(saturate(dot(n, norm(v + sideCrystalDir))), 55.0);
        float3 rubyCore = float3(0.20, 0.014, 0.082);
        float3 nectarAmber = float3(0.78, 0.44, 0.09);
        float3 lipTint = lerp(rubyCore, nectarAmber, saturate(throat * 0.85 + throatVeins));
        color = lipTint * (0.20 + 0.68 * diffuse)
              + transmitted * float3(0.32, 0.04, 0.14) * 0.35
              + reflected * (0.36 * fres)
              + float3(0.68, 0.09, 0.28) * (rimRuby * 0.52)
              + float3(0.26, 0.14, 0.03) * throat
              + (specLip + specLip2 * 0.45) * float3(1.15, 0.95, 1.02) * 0.65;
    }

    if (i.mat == 7u) {
        float wave = 0.5 + 0.5 * sin(length(i.world.xz) * 2.4 - time * 1.8);
        float energy = dot(neural, float3(0.2126, 0.7152, 0.0722));
        float caustBoost = (((showFlags >> 2u) & 7u) == 2u) ? 0.62 : 0.14;
        float3 caustCol = lerp(float3(0.020, 0.11, 0.14), float3(0.22, 0.15, 0.07), daylight);
        if (((showFlags >> 2u) & 7u) == 2u) {
            caustCol = ((showFlags & 2u) != 0u) ? float3(0.36, 0.10, 0.38) : float3(0.06, 0.34, 0.32);
        }
        color += (caustCol * energy + neural * (caustBoost * 0.85)) * (0.25 + 0.75 * wave) * ((((showFlags >> 2u) & 7u) == 2u) ? 1.0 : 0.14);
    }

    if (i.mat == 20u) {
        // Translucent pearlescent hydroxyapatite enamel: warm subsurface scatter + crisp ceramic rim
        float sss = pow(1.0 - ndotv, 2.0);
        float3 enamelWarm = lerp(float3(0.22, 0.19, 0.15), float3(0.62, 0.56, 0.46), daylight);
        color = color * 0.78 + enamelWarm * (0.45 + 0.55 * sss) + reflected * (0.28 * fres);
    }

    if (i.mat == 21u) {
        // Neural Velvet 64-layer Tensor Core SIREN silk-glass canopy ribbon
        float2 ruv = frac(float2(i.uv.x * 2.0 + time * 0.04, i.uv.y + i.world.z * 0.06));
        float3 siren = tensorField.SampleLevel(linearClamp, ruv, 0.0).rgb;
        float3 sirenB = tensorField.SampleLevel(linearClamp, frac(ruv.yx * 1.37 - time * 0.03), 0.0).rgb;
        float velvetRim = pow(1.0 - ndotv, 2.4);
        float3 tone = ((showFlags & 2u) != 0u) ? float3(1.22, 0.52, 1.15) : float3(0.52, 1.18, 1.05);
        color = (siren * 1.45 + sirenB * 0.65) * tone * (0.55 + 0.85 * velvetRim)
              + reflected * (0.34 * fres)
              + float3(0.95, 0.74, 0.38) * (velvetRim * 0.18);
    }

    if (i.mat == 22u) {
        // Symbiotic bioluminescent micro-flora garden domes
        float domeGlow = pow(1.0 - ndotv, 1.8);
        float3 bioHue = lerp(float3(0.10, 0.92, 0.68), float3(0.98, 0.68, 0.24), clamp(moodAccent, 0.0, 1.0));
        if ((showFlags & 2u) != 0u) bioHue = lerp(bioHue, float3(0.92, 0.26, 0.78), 0.45);
        color += bioHue * (0.32 + 0.48 * domeGlow) + neural * 0.12;
    }

    if (i.mat == 17u) {
        // Subtle silver-graphite rotor tip-path shimmer
        color = lerp(reflected, float3(0.62, 0.68, 0.74), 0.35) * (0.28 + 0.45 * daylight) + float3(0.04, 0.07, 0.10);
    }

    // Layered atmospheric depth: humid low-altitude basin haze + aerial perspective
    float heightAtten = exp(-max(i.world.y, 0.0) * 0.055);
    float fogDensity = (act == 1u) ? lerp(0.0048, 0.0024, daylight) : ((act == 2u) ? (0.0038 + 0.0045 * heightAtten) : 0.0055);
    float fog = 1.0 - exp(-max(dist - 4.5, 0.0) * fogDensity);
    color = lerp(color, sky(norm(i.world - eye)), fog * 0.76);

    if (showFlags == 0u) {
        float mist = 1.0 - smoothstep(0.0, 2.0, min(abs(time - 60.0), abs(time - 120.0)));
        color = lerp(color, float3(0.28, 0.33, 0.36), mist);
    }

    Targets o;
    o.color = float4(max(color, 0.0.xxx), 1.0);
    o.distance = dist;
    return o;
}

struct SkyOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
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
