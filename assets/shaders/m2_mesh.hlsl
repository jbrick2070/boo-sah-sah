// BOO SAH SAH — M2 / M6 shared mesh shading.
// Same primary triangles for the SM 6.0 raster substitute and the SM 6.5 DXR 1.1 inline-RayQuery path.
//
// "Showcase" RT feature set (M2_RT only, sample-deterministic, no temporal history):
//   * Physically based GGX / Smith height-correlated specular with metal / dielectric F0 per material
//   * Ray-traced soft key-light shadows (stochastic cone sampling of the sun / moon disk, 90-unit reach)
//   * Ray-traced ambient occlusion (cosine-weighted hemisphere, distance-adaptive radius)
//   * Roughness-driven glossy reflections with grazing-angle anisotropic stretch (rain-street streaks)
//   * Reflection hits shaded with interpolated vertex normals, key light and (Showcase) a shadow ray
//   * Spectral glass: per-channel IOR dispersion rays + thin-film interference on petal rims
//   * Ray-marched volumetric light shafts: each march step fires a key-light visibility ray
//   * Procedural rain-impact ripples on puddles and open water
//   * Physically motivated sky: Rayleigh / Mie phase glow, moon disk with halo, anti-aliased star field
// All stochastic terms hash (pixel, frameSeed) where frameSeed = samplePosition / 800, so seeking to a
// sample reproduces the identical frame.
struct Vertex { float4 p; float4 n; float4 uv; float4 tangent; };
cbuffer Scene : register(b0) {
    float3 eye; float fov;
    float3 right; float aspect;
    float3 up; float time;
    float3 forward; float reveal;
    uint width; uint height; uint act; uint canonical;
    float daylight; float pulse; uint showFlags; float moodAccent;
    uint quality; uint frameSeed; float sunCone; float fxStrength;
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

static const float kPi = 3.14159265;
static const float kTwoPi = 6.2831853;

float3 norm(float3 v) { return v * rsqrt(max(dot(v, v), 1e-10)); }
float3 transformPoint(float3 p) { float4 q = float4(p, 1.0); return float3(dot(row0, q), dot(row1, q), dot(row2, q)); }
float3 transformNormal(float3 n) { return norm(float3(dot(row0.xyz, n), dot(row1.xyz, n), dot(row2.xyz, n))); }

float hash21(float2 p) {
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float valueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Interleaved gradient noise, decorrelated per stochastic dimension k and per 800-sample frame bucket.
float ign(float2 px, uint k) {
    float2 q = px + 5.588238 * (float)((frameSeed + k * 37u) & 1023u);
    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));
}
float2 rnd2(float2 px, uint k) {
    return float2(ign(px, k), ign(px.yx * 1.13 + float2(23.7, 11.3), k + 5u));
}

// Branchless orthonormal basis (Duff et al. 2017).
void makeBasis(float3 n, out float3 t, out float3 b) {
    float s = (n.z >= 0.0) ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}
float2 concentricDisk(float2 u) {
    float r = sqrt(u.x);
    float ph = kTwoPi * u.y;
    return float2(cos(ph), sin(ph)) * r;
}
float3 cosineHemisphere(float3 n, float2 u) {
    float3 t, b;
    makeBasis(n, t, b);
    float2 d = concentricDisk(u);
    return norm(t * d.x + b * d.y + n * sqrt(saturate(1.0 - dot(d, d))));
}
float3 coneSample(float3 axis, float halfAngle, float2 u) {
    float3 t, b;
    makeBasis(axis, t, b);
    float2 d = concentricDisk(u) * halfAngle;
    return norm(axis + t * d.x + b * d.y);
}

float henyeyGreenstein(float cosT, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * cosT, 1e-4), 1.5));
}

// Key light shared by primary shading, reflection hits and the volumetric march.
float3 keyDirection() {
    return norm((act == 1u) ? float3(-0.48, 0.22 + 0.28 * daylight, 0.72) : float3(-0.44, 0.74, -0.32));
}
float3 keyTintFor() {
    float3 nightKey = (act == 1u) ? float3(0.24, 0.36, 0.54) : float3(0.42, 0.64, 0.92);
    return lerp(nightKey, float3(1.42, 1.02, 0.68), daylight);
}

// GGX / Trowbridge-Reitz NDF and Smith height-correlated visibility (Heitz 2014, Karis 2013).
float ggxD(float ndh, float a) {
    float a2 = a * a;
    float d = (ndh * a2 - ndh) * ndh + 1.0;
    return a2 / (kPi * d * d + 1e-7);
}
float smithV(float ndv, float ndl, float a) {
    float gv = ndl * (ndv * (1.0 - a) + a);
    float gl = ndv * (ndl * (1.0 - a) + a);
    return 0.5 / max(gv + gl, 1e-5);
}
float3 fresnelSchlick(float3 f0, float vdh) {
    float f = pow(1.0 - saturate(vdh), 5.0);
    return f0 + (1.0 - f0) * f;
}

// Thin-film interference (single MgF2-like layer, n = 1.38) sampled at R/G/B wavelengths.
float3 thinFilm(float cosTheta, float thicknessNm) {
    float nf = 1.38;
    float cosT = sqrt(saturate(1.0 - (1.0 - cosTheta * cosTheta) / (nf * nf)));
    float opd = 2.0 * nf * thicknessNm * cosT;
    return 0.5 + 0.5 * cos(kTwoPi * opd / float3(650.0, 532.0, 450.0));
}

float puddleMask(float2 xz) {
    float a = 0.5 + 0.5 * sin(xz.x * 0.43 + sin(xz.y * 0.31) * 1.4);
    float b = 0.5 + 0.5 * cos(xz.y * 0.67 - sin(xz.x * 0.52) * 1.2);
    return smoothstep(0.36, 0.78, a * 0.58 + b * 0.42);
}

// Two decorrelated grids of expanding rain-impact rings; returns the height-field gradient in xz.
float2 rainRippleGrad(float2 xz, float t) {
    float2 g = float2(0.0, 0.0);
    [unroll] for (uint layer = 0u; layer < 2u; ++layer) {
        float fl = (float)layer;
        float scale = (layer == 0u) ? 2.6 : 4.3;
        float2 p = xz * scale + fl * 17.31;
        float2 cell = floor(p);
        float2 f = frac(p) - 0.5;
        float h = hash21(cell + 3.7 * fl);
        float2 off = (float2(hash21(cell + 11.1), hash21(cell + 23.9)) - 0.5) * 0.5;
        float2 d = f - off;
        float r = length(d);
        float phase = frac(t * (0.55 + 0.60 * h) + h * 7.0);
        float x = (r - phase * 0.42) * 38.0;
        float fade = (1.0 - phase) * (1.0 - phase);
        float wave = sin(x * 2.2) * exp(-x * x) * fade;
        g += (d / max(r, 1e-3)) * wave * scale;
    }
    return g;
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
    // precise: the depth pre-pass PSO and the shading PSOs must produce bit-identical clip positions.
    precise float3 world = transformPoint(v.p.xyz);
    o.world = world;
    o.normal = transformNormal(v.n.xyz);
    o.uv = v.uv;
    o.mat = (uint)v.p.w;
    precise float3 d = world - eye;
    precise float z = dot(d, forward);
    precise float fy = 1.0 / tan(fov * 0.5);
    precise float4 clip = float4(dot(d, right) * fy / aspect, dot(d, up) * fy, z * 1000.0 / 999.9 - 0.1 * 1000.0 / 999.9, z);
    o.pos = clip;
    return o;
}

// Atmosphere without celestial bodies: used for aerial-perspective fog so stars never print on buildings.
float3 skyBase(float3 d) {
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
    float cosSun = dot(d, sunDir);
    float sun = pow(saturate(cosSun), 180.0);
    float sunHalo = pow(saturate(cosSun), 10.0);
    // Rayleigh (3/16pi (1 + cos^2)) brightens the anti-solar and solar sky; Mie (HG g = 0.76) forms the aureole.
    float rayleigh = 0.0597 * (1.0 + cosSun * cosSun);
    float mie = henyeyGreenstein(cosSun, 0.76);
    float3 scatter = rayleigh * float3(0.10, 0.20, 0.42) * horizon + mie * float3(0.060, 0.038, 0.020) * (0.35 + 0.65 * horizon);
    float3 c = clearSky + (sun * float3(3.2, 1.9, 0.85) + sunHalo * float3(0.32, 0.16, 0.06) + scatter) * daylight;

    // Moonlit Mie halo around the act-2 key light at night.
    if (act == 2u) {
        float3 kd = keyDirection();
        float moonHalo = henyeyGreenstein(dot(d, kd), 0.86);
        float3 moonTint = ((showFlags & 2u) != 0u) ? float3(0.62, 0.42, 0.92) : float3(0.55, 0.66, 0.86);
        c += moonTint * moonHalo * 0.0042 * (1.0 - daylight);
    }

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

// Stars (cube-face hashed cells, Gaussian footprint matched to the pixel cone) and a textured moon disk.
float3 celestial(float3 d, float pixelAngle) {
    float night = saturate(1.0 - daylight * 1.6);
    if (night <= 0.0 || d.y <= 0.0) return float3(0.0, 0.0, 0.0);
    float3 c = float3(0.0, 0.0, 0.0);

    float3 a = abs(d);
    float2 f = d.xy;
    float face = 0.0;
    if (a.x >= a.y && a.x >= a.z) { f = d.yz / a.x; face = (d.x > 0.0) ? 0.0 : 1.0; }
    else if (a.y >= a.z)         { f = d.xz / a.y; face = (d.y > 0.0) ? 2.0 : 3.0; }
    else                         { f = d.xy / a.z; face = (d.z > 0.0) ? 4.0 : 5.0; }
    const float cells = 150.0;
    float2 g = f * cells;
    float2 cell = floor(g);
    float h = hash21(cell + face * 41.3);
    if (h > 0.972) {
        float2 off = (float2(hash21(cell + 7.7 + face), hash21(cell + 19.1 + face)) - 0.5) * 0.55;
        float2 dl = frac(g) - 0.5 - off;
        float sigma = max(pixelAngle * cells * 0.5, 0.06);
        float footprint = exp(-dot(dl, dl) / (2.0 * sigma * sigma)) * min(1.0, (0.06 * 0.06) / (sigma * sigma) * 4.0);
        float mag = pow(saturate((h - 0.972) / 0.028), 3.0);
        float twinkle = 0.72 + 0.28 * sin(time * (1.7 + 4.0 * h) + h * 113.0);
        float3 starTint = lerp(float3(0.70, 0.80, 1.00), float3(1.00, 0.82, 0.62), frac(h * 57.0));
        float altitude = smoothstep(0.06, 0.35, d.y);
        c += starTint * footprint * (0.25 + 2.6 * mag) * twinkle * altitude;
    }

    if (act == 2u) {
        float3 kd = keyDirection();
        float cosM = dot(d, kd);
        float ang = sqrt(max(2.0 * (1.0 - cosM), 0.0));
        const float moonR = 0.016;
        float edge = max(pixelAngle, 0.0008);
        float disk = smoothstep(moonR + edge, moonR - edge, ang);
        if (disk > 0.0) {
            float3 t, b;
            makeBasis(kd, t, b);
            float2 lp = float2(dot(d, t), dot(d, b)) / moonR;
            float maria = valueNoise(lp * 2.3 + 4.0) * 0.6 + valueNoise(lp * 5.1 - 2.0) * 0.4;
            float limb = sqrt(saturate(1.0 - dot(lp, lp)));
            float3 moonCol = ((showFlags & 2u) != 0u) ? float3(1.00, 0.86, 1.05) : float3(0.98, 0.95, 0.88);
            c += moonCol * disk * (0.55 + 0.45 * limb) * (0.70 + 0.30 * smoothstep(0.35, 0.65, maria)) * 3.4;
        }
    }
    return c * night;
}

float3 sky(float3 d) { return skyBase(d); }
float3 skyView(float3 d, float pixelAngle) { return skyBase(d) + celestial(d, pixelAngle); }

// Height-falling participating medium for the volumetric shafts.
float volumeDensity(float3 p) {
    float base = (act == 1u) ? 0.010 : ((act == 2u) ? 0.015 : 0.013);
    if ((showFlags & 2u) != 0u) base *= 1.25;
    return base * exp(-max(p.y, 0.0) * 0.085);
}
float3 volumeScatterTint() {
    float3 t = keyTintFor();
    if ((showFlags & 2u) != 0u) t = lerp(t, float3(0.70, 0.36, 0.92), 0.35);
    return t * lerp(2.1, 3.4, daylight) * fxStrength;
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
bool traceOccluded(float3 origin, float3 direction, float distance, inout uint rays) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.022;
    ray.TMax = distance;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01u, ray);
    while (q.Proceed()) {}
    rays += 1u;
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// Closest-hit secondary ray. Hits are shaded with the interpolated vertex normal, key light and
// hemispherical ambient; Showcase tier adds a key-light shadow ray from the hit point.
bool traceScene(float3 origin, float3 direction, float distance, uint mask, float coneSpread, bool hitShadow, inout uint rays, out float3 color) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.020;
    ray.TMax = distance;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, mask, ray);
    while (q.Proceed()) {}
    rays += 1u;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        uint offset = q.CommittedInstanceID() + q.CommittedPrimitiveIndex() * 3u;
        Vertex v0 = vertices[indices[offset]];
        Vertex v1 = vertices[indices[offset + 1u]];
        Vertex v2 = vertices[indices[offset + 2u]];
        float2 bc = q.CommittedTriangleBarycentrics();
        float3 nObj = v0.n.xyz * (1.0 - bc.x - bc.y) + v1.n.xyz * bc.x + v2.n.xyz * bc.y;
        float3x4 o2w = q.CommittedObjectToWorld3x4();
        float3 hn = norm(mul((float3x3)o2w, nObj));
        if (dot(hn, direction) > 0.0) hn = -hn;
        uint hm = (uint)v0.p.w;
        float hitT = q.CommittedRayT();
        float3 p = origin + direction * hitT;

        float3 kd = keyDirection();
        float ndl = saturate(dot(hn, kd));
        float vis = 1.0;
        if (hitShadow && ndl > 0.0) {
            vis = traceOccluded(p + hn * 0.03, kd, 90.0, rays) ? 0.0 : 1.0;
        }
        float hemi = 0.55 + 0.45 * hn.y;
        float3 hitCol = baseColor(hm) * keyTintFor() * (0.30 * hemi + 0.78 * ndl * vis) * (0.62 + 0.38 * daylight)
                      + emissive(hm, p, coneSpread * hitT);
        float hFog = 1.0 - exp(-max(hitT - 4.0, 0.0) * 0.006);
        color = lerp(hitCol, sky(direction), hFog * 0.72);
        return true;
    }
    color = skyView(direction, coneSpread);
    return false;
}

// Ray-marched single scattering: every step fires a key-light visibility ray through the BVH, so
// towers, the bridge truss and orchid petals carve real light shafts out of the haze.
float3 rtVolumetric(float3 dir, float maxT, float2 px, inout uint rays) {
    uint steps = (quality == 2u) ? 4u : ((quality == 3u) ? 2u : 3u);
    float marchT = min(maxT, 90.0);
    float dt = marchT / (float)steps;
    float j = ign(px, 41u);
    float3 kd = keyDirection();
    float acc = 0.0;
    float tau = 0.0;
    [loop] for (uint s = 0u; s < steps; ++s) {
        float t = ((float)s + j) * dt;
        float3 p = eye + dir * t;
        float dens = volumeDensity(p);
        float lit = traceOccluded(p, kd, 80.0, rays) ? 0.0 : 1.0;
        acc += lit * dens * exp(-tau) * dt;
        tau += dens * dt;
    }
    float phase = lerp(0.0796, henyeyGreenstein(dot(dir, kd), 0.62), 0.82);
    return acc * phase * volumeScatterTint();
}
#endif

struct Targets { float4 color : SV_Target0; float distance : SV_Target1; };

Targets PSMain(VOut i, bool front : SV_IsFrontFace) {
    float3 n = norm(front ? i.normal : -i.normal);
    float3 v = norm(eye - i.world);
    float dist = length(i.world - eye);
    float2 px = i.pos.xy;
    bool isGlass = (i.mat == 3u || i.mat == 4u || i.mat == 18u);
    float pixelAngle = fov / max((float)height, 1.0);

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
        // Rain-impact rings on upward-facing wet surfaces, faded out before they can alias.
        float ringAmt = ((i.mat == 7u) ? 0.010 : 0.016 * wetPuddle) * saturate(1.0 - dist / 55.0) * saturate(n.y * 4.0 - 2.8) * fxStrength;
        if (ringAmt > 0.0) {
            float2 rg = rainRippleGrad(i.world.xz, time);
            n = norm(n - float3(rg.x, 0.0, rg.y) * ringAmt);
        }
    }

    float ndotv = saturate(dot(n, v));
    float fres = 0.035 + 0.965 * pow(1.0 - ndotv, 4.4);
    float3 keyDir = keyDirection();
    float3 rimDir = norm(float3(0.58, 0.28, 0.76));
    float3 upSanctuaryDir = norm(float3(0.08, -0.86, -0.48));

    float rough = clamp(i.uv.w, 0.04, 0.95);
    if (i.mat == 1u)  rough = lerp(0.48, 0.06, wetPuddle);
    if (i.mat == 12u) rough = 0.09;
    if (i.mat == 10u || i.mat == 19u) rough = 0.92;
    if (i.mat == 7u)  rough = min(rough, 0.10);

    // Restrained refraction bend keeps background silhouettes optically coherent rather than shattered
    float3 refrDir = norm(lerp(-v, refract(-v, n, 1.0 / 1.49), 0.42));
    float3 mirrorDir = reflect(-v, n);

    float ao = 1.0;
    float shadow = 1.0;
    float3 reflected = skyView(mirrorDir, pixelAngle);
    float3 transmitted = skyView(refrDir, pixelAngle);
    float3 inscatter = float3(0.0, 0.0, 0.0);

#if defined(M2_RT)
    uint rays = 0u;
    bool reflHit = false;
    bool secondHit = false;
    bool showcase = (quality == 2u);

    // Glossy reflection: one GGX-width cone sample around the mirror direction. Wet asphalt, water and
    // curtain walls stretch the lobe along the plane of incidence, reproducing rain-street light streaks.
    float3 reflDir = mirrorDir;
    if (!isGlass && rough > 0.07) {
        float2 u = concentricDisk(rnd2(px, 3u));
        float3 axisT = n - mirrorDir * dot(n, mirrorDir);
        axisT = (dot(axisT, axisT) > 1e-6) ? norm(axisT) : float3(0.0, 1.0, 0.0);
        float3 axisB = norm(cross(mirrorDir, axisT));
        bool streaky = (i.mat == 1u || i.mat == 7u || i.mat == 5u || i.mat == 14u);
        float stretch = streaky ? lerp(1.0, 3.2, 1.0 - ndotv) : 1.0;
        float spread = rough * rough * 1.15;
        reflDir = norm(mirrorDir + (axisT * u.x * stretch + axisB * u.y / sqrt(stretch)) * spread);
        if (dot(reflDir, n) < 0.02) reflDir = norm(reflDir + n * (0.02 - dot(reflDir, n)));
    }

    if (isGlass) {
        reflHit = traceScene(i.world + n * 0.03, mirrorDir, 180.0, 0x01u, 0.012, false, rays, reflected);
        if (i.mat != 18u && quality != 3u) {
            // Spectral dispersion: separate R / G / B refraction rays (crown-glass-like IOR spread).
            float3 dirR = norm(lerp(-v, refract(-v, n, 1.0 / 1.455), 0.42));
            float3 dirB = norm(lerp(-v, refract(-v, n, 1.0 / 1.535), 0.42));
            float3 tR, tG, tB;
            bool hR = traceScene(i.world - n * 0.03, dirR,    180.0, 0x01u, 0.014, false, rays, tR);
            bool hG = traceScene(i.world - n * 0.03, refrDir, 180.0, 0x01u, 0.014, false, rays, tG);
            bool hB = traceScene(i.world - n * 0.03, dirB,    180.0, 0x01u, 0.014, false, rays, tB);
            transmitted = float3(tR.r, tG.g, tB.b);
            secondHit = hR || hG || hB;
        } else {
            secondHit = traceScene(i.world - n * 0.03, refrDir, 180.0, 0x01u, 0.014, false, rays, transmitted);
        }
    } else {
        reflHit = traceScene(i.world + n * 0.024, reflDir, 180.0, 0x03u, 0.005 + rough * 0.02, showcase, rays, reflected);

        // Soft shadow: cone-sample the key-light disk. Showcase takes a second, rotated sample.
        float ndlRaw = dot(n, keyDir);
        if (ndlRaw > 0.0) {
            uint shadowSpp = showcase ? 2u : 1u;
            float lit = 0.0;
            [loop] for (uint s = 0u; s < shadowSpp; ++s) {
                float3 sd = coneSample(keyDir, sunCone, rnd2(px, 11u + s * 2u));
                lit += traceOccluded(i.world + n * 0.024, sd, 90.0, rays) ? 0.0 : 1.0;
            }
            shadow = lit / (float)shadowSpp;
        } else {
            shadow = 0.0;
        }
        secondHit = shadow < 1.0;

        // Ray-traced ambient occlusion, radius adapted to viewing distance (city blocks vs petals).
        float aoRadius = min(1.4, 0.22 + dist * 0.035);
        float3 aoDir = cosineHemisphere(n, rnd2(px, 17u));
        float aoVis = traceOccluded(i.world + n * 0.02, aoDir, aoRadius, rays) ? 0.0 : 1.0;
        ao = lerp(1.0, aoVis, 0.55);
    }

    inscatter = rtVolumetric(-v, dist, px, rays);

    uint waveQueries = WaveActiveSum(rays);
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

    // Metal / dielectric response per material.
    float metal = 0.0;
    float3 f0 = float3(0.04, 0.04, 0.04);
    if (i.mat == 8u)  { metal = 1.0;  f0 = float3(1.00, 0.71, 0.29); }
    if (i.mat == 0u)  { metal = 0.55; f0 = float3(0.30, 0.31, 0.33); }
    if (i.mat == 12u) { metal = 0.50; f0 = float3(0.45, 0.47, 0.50); }
    if (i.mat == 11u) { metal = 0.25; f0 = float3(0.52, 0.54, 0.56); }
    if (i.mat == 7u)  { f0 = float3(0.02, 0.02, 0.02); }

    float3 neural = tensorField.SampleLevel(linearClamp, frac(i.world.xz * 0.035), 0.0).rgb;
    float ndl = saturate(dot(n, keyDir));
    float hemi = 0.62 + 0.38 * n.y;
    float rimLight = pow(saturate(dot(n, rimDir)), 2.2) * 0.24;

    float3 keyTint = keyTintFor();
    float3 rimTint = lerp(float3(0.32, 0.48, 0.68), float3(0.85, 0.66, 0.48), daylight);
    float3 diffuseAlbedo = base * (1.0 - 0.8 * metal);
    float3 color = diffuseAlbedo * (keyTint * (0.22 * hemi * ao + 0.78 * ndl * shadow) + rimLight * rimTint * ao)
                 + emissive(i.mat, i.world, dist * 0.0022);

    // Subtle upward vertiport pad bounce illumination on hovering eVTOL airframe & wings at night
    if (act == 1u && i.world.y > 3.72 && i.world.y < 7.2) {
        float padDist = length((i.world - float3(0.0, 3.70, -2.0)) * float3(0.22, 0.38, 0.22));
        float padBounce = exp(-padDist * padDist * 2.2) * (0.35 + 0.65 * saturate(-n.y)) * (1.0 - daylight);
        color += base * float3(0.26, 0.68, 0.62) * padBounce;
    }

    // GGX key-light specular. Alpha is floored by the light's angular size to keep highlights stable.
    {
        float a = max(rough * rough, 0.0025 + sunCone);
        float3 hv = norm(v + keyDir);
        float ndh = saturate(dot(n, hv));
        float vdh = saturate(dot(v, hv));
        float3 F = fresnelSchlick(f0, vdh);
        float specMask = (i.mat == 10u || i.mat == 19u) ? 0.15 : 1.0;
        float3 specular = ggxD(ndh, a) * smithV(max(ndotv, 1e-3), ndl, a) * F * ndl;
        color += specular * keyTint * (kPi * 0.78) * shadow * specMask;
    }

    float reflWeight = 0.12;
    if (i.mat == 7u)  reflWeight = 0.78;
    else if (i.mat == 12u) reflWeight = 0.68;
    else if (i.mat == 1u)  reflWeight = lerp(0.04, 0.62, wetPuddle);
    else if (i.mat == 5u || i.mat == 14u || i.mat == 13u) reflWeight = ((i.mat == 13u) ? 0.18 : 0.34) * (0.30 + 0.70 * cwPane);
    else if (i.mat == 8u)  reflWeight = 0.56;
    else if (i.mat == 10u || i.mat == 19u) reflWeight = 0.015;

    float3 reflTint = (metal > 0.0) ? lerp(float3(1.0, 1.0, 1.0), f0 * 1.15, metal) : float3(1.0, 1.0, 1.0);
    // Specular occlusion: crevices that occlude the hemisphere also dim their glossy reflections.
    float specOcc = lerp(1.0, ao, 0.6);
    color += reflected * reflTint * reflWeight * (0.28 + 0.72 * fres) * specOcc;

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

            // Thin-film iridescence: film thickness varies along the blade and across the midrib.
            float filmNm = 280.0 + 220.0 * uBlade + 120.0 * abs(vBlade) + 40.0 * sin(time * 0.35 + uBlade * 6.0);
            float3 film = lerp(float3(1.0, 1.0, 1.0), thinFilm(ndotv, filmNm) * 1.25, saturate(0.10 + 0.55 * edgeRim));

            float3 glassTrans = transmitted * beer + bodyScatter + nectarGlow;
            float reflMix = clamp(0.16 + 0.78 * fres, 0.0, 0.93);
            color = lerp(glassTrans, reflected * float3(1.05, 1.08, 1.12) * film, reflMix)
                  + rimSheen * film
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
        // Wrapped-diffuse subsurface approximation: light bleeds past the terminator through the crystal lip.
        float wrapSss = saturate((dot(n, keyDir) + 0.45) / 1.45);
        color = lipTint * (0.20 + 0.68 * (0.22 + 0.78 * lerp(wrapSss, ndl * shadow, 0.5)))
              + transmitted * float3(0.32, 0.04, 0.14) * 0.35
              + reflected * (0.36 * fres)
              + float3(0.68, 0.09, 0.28) * (rimRuby * 0.52)
              + float3(0.26, 0.14, 0.03) * throat
              + (specLip * shadow + specLip2 * 0.45) * float3(1.15, 0.95, 1.02) * 0.65;
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
        float backLit = pow(saturate(dot(-v, keyDir)), 3.0) * 0.35;
        color = color * 0.78 + enamelWarm * (0.45 + 0.55 * sss + backLit) * lerp(0.75, 1.0, ao) + reflected * (0.28 * fres);
    }

    if (i.mat == 21u) {
        // Neural Velvet 64-layer Tensor Core SIREN silk-glass canopy ribbon
        float2 ruv = frac(float2(i.uv.x * 2.0 + time * 0.04, i.uv.y + i.world.z * 0.06));
        float3 siren = tensorField.SampleLevel(linearClamp, ruv, 0.0).rgb;
        float3 sirenB = tensorField.SampleLevel(linearClamp, frac(ruv.yx * 1.37 - time * 0.03), 0.0).rgb;
        float velvetRim = pow(1.0 - ndotv, 2.4);
        float3 tone = ((showFlags & 2u) != 0u) ? float3(1.22, 0.52, 1.15) : float3(0.52, 1.18, 1.05);
        // Velvet sheen (Charlie / inverted-Gaussian lobe) on top of the emissive SIREN field.
        float sheen = pow(saturate(1.0 - ndotv), 4.0) * 0.6 + 0.25 * pow(saturate(dot(n, norm(v + keyDir))), 6.0);
        color = (siren * 1.45 + sirenB * 0.65) * tone * (0.55 + 0.85 * velvetRim)
              + reflected * (0.34 * fres)
              + float3(0.95, 0.74, 0.38) * (velvetRim * 0.18)
              + keyTint * sheen * 0.22 * shadow;
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
    color += inscatter;

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
    // Drawn last at the far plane: only pixels still at the cleared depth (1.0) run the sky shader.
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
    return o;
}
Targets PSSky(SkyOut i) {
    float2 p = i.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float3 d = norm(forward + right * p.x * aspect * tan(fov * 0.5) + up * p.y * tan(fov * 0.5));
    float3 c = skyView(d, fov / max((float)height, 1.0));
#if defined(M2_RT)
    // Shafts continue seamlessly across silhouettes into open sky.
    uint rays = 0u;
    c += rtVolumetric(d, 90.0, i.pos.xy, rays);
    uint waveQueries = WaveActiveSum(rays);
    if (canonical != 0u && WaveIsFirstLane()) InterlockedAdd(verified[1], waveQueries);
#endif
    Targets o;
    o.color = float4(c, 1.0);
    o.distance = 1000.0;
    return o;
}
