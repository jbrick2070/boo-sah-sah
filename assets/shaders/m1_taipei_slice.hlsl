// BOO SAH SAH — The Glass Orchid Protocol
// Milestone M1 60-Second Taipei -> Enamel Monolith -> Glass Orchid Vertical Slice Shaders
// Compiled with Windows SDK DXC (-Ges -WX -O3):
//   - CSDxrTaipeiScene       (cs_6_5, -DBOO_M1_DXR_CS)
//   - PSRasterTaipeiScene    (ps_6_0, -DBOO_M1_RASTER_PS)
//   - PSBloomDownsample      (ps_6_0, -DBOO_M1_BLOOM_DOWN_PS)
//   - PSBloomUpsample        (ps_6_0, -DBOO_M1_BLOOM_UP_PS)
//   - PSCompositeCanvas      (ps_6_0, -DBOO_M1_COMPOSITE_PS)
//   - PSPresentLetterboxHud  (ps_6_0, -DBOO_M1_PRESENT_PS)

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

struct GpuM1Vertex
{
    float4 posAndU;  // xyz = object position, w = surface u in [0,1]
    float4 normAndV; // xyz = object smooth normal, w = surface v in [0,1]
};

struct GpuLiveParticle
{
    float4 posAndAge;  // xyz = world position, w = normalized particle age [0,1)
    float4 velAndKind; // xyz = world velocity, w = <0.5 Taipei rain-mist, >=0.5 sanctuary pollen crystal
};

// =============================================================================
// Shared Palette, Math & Taipei / Enamel Monolith / Botanical Glass Orchid Shading
// =============================================================================
static const float kPi    = 3.14159265359;
static const float kTwoPi = 6.28318530718;

float3 safeNormalize(float3 v)
{
    return v * rsqrt(max(dot(v, v), 1e-12));
}

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float valueNoise2D(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + float2(1.0, 0.0));
    float c = hash12(i + float2(0.0, 1.0));
    float d = hash12(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float interleavedGradientNoise(float2 pixelPos, uint seed)
{
    float2 jittered = pixelPos + float2((float)(seed & 1023u) * 5.588238, (float)((seed >> 10u) & 1023u) * 3.14159);
    return frac(52.9829189 * frac(dot(jittered, float2(0.06711056, 0.00583715))));
}

float triangularDither(float2 pixelPos, uint seed)
{
    float r0 = interleavedGradientNoise(pixelPos, seed);
    float r1 = interleavedGradientNoise(pixelPos + float2(17.0, 31.0), seed ^ 0x9E3779B9u);
    return (r0 - r1);
}

float acesScalar(float x)
{
    x = max(x, 0.0);
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 acesNarkowicz(float3 x)
{
    x = max(x, 0.0.xxx);
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Luminance-guided chromaticity-preserving filmic tone curve:
// Preserves rich spectral jade, orchid-magenta, and molten-amber saturation in bright glass highlights
// while gently rolling off ultra-high specular peaks without hard channel clipping.
float3 tonemapChromaPreservingFilmic(float3 hdr)
{
    hdr = max(hdr, 0.0.xxx);
    float lum = dot(hdr, float3(0.2126, 0.7152, 0.0722));
    float lumTm = acesScalar(lum);
    float3 lumScaled = hdr * (lumTm / max(lum, 1e-5));
    float maxCh = max(lumScaled.r, max(lumScaled.g, lumScaled.b));
    if (maxCh > 1.0)
    {
        // Soft gamut compression toward tone-mapped luminance rather than bleaching to grey-white
        float compress = (maxCh - 1.0) / max(maxCh - lumTm, 1e-4);
        lumScaled = lerp(lumScaled, lumTm.xxx, saturate(compress * 0.65));
    }
    float3 perChannel = acesNarkowicz(hdr);
    return saturate(lerp(lumScaled, perChannel, 0.24 * smoothstep(0.35, 0.98, lumTm)));
}

float3 linearToSrgbExact(float3 lin)
{
    lin = max(lin, 0.0.xxx);
    float3 lo = lin * 12.92;
    float3 hi = 1.055 * pow(max(lin, 1e-6.xxx), (1.0 / 2.4).xxx) - 0.055;
    return lerp(lo, hi, step(0.0031308.xxx, lin));
}

float evalWetPuddleMask(float2 xz)
{
    float n0 = valueNoise2D(xz * 0.42 + float2(3.1, 7.4));
    float n1 = valueNoise2D(xz * 1.28 - float2(5.2, 1.9));
    // Boulevard drainage crown: slightly drier centerline, pooled rainwater along colonnade channels & sanctuary plaza
    float curbChannel = smoothstep(0.55, 2.5, abs(xz.x)) * (1.0 - 0.35 * smoothstep(4.2, 6.5, abs(xz.x)));
    float plazaPool = smoothstep(-2.0, 4.5, xz.y) * 0.28;
    float raw = n0 * 0.62 + n1 * 0.26 + curbChannel * 0.24 + plazaPool;
    return smoothstep(0.36, 0.76, raw);
}

float2 evalRainCellGrad(float2 p, float filmTime, float rateScale)
{
    float2 cell = floor(p);
    float2 f = frac(p) - 0.5;
    float h = hash12(cell);
    float phase = frac(filmTime * rateScale + h);
    float rad = length(f);
    float env = exp(-11.0 * rad) * (1.0 - phase) * smoothstep(0.0, 0.08, phase);
    float ring = cos((rad - phase * 0.42) * 44.0) * env;
    return (rad > 1e-4) ? (f / rad) * ring : float2(0.0, 0.0);
}

float3 evalWetRainRippleNormal(float3 worldPos, float filmTime)
{
    float puddle = evalWetPuddleMask(worldPos.xz);
    float puddleStrength = smoothstep(0.22, 0.82, puddle);

    float2 g0 = evalRainCellGrad(worldPos.xz * 2.15, filmTime, 0.78);
    float2 g1 = evalRainCellGrad(worldPos.xz * 3.35 + float2(11.3, 6.7), filmTime + 0.37, 0.94);

    // Subtle wind-driven capillary ripple across standing puddles + mineral asphalt grain on damp aggregate
    float windWave = cos(worldPos.x * 6.5 + worldPos.z * 9.0 - filmTime * 2.2) * 0.008 * puddleStrength;
    float grainX = (valueNoise2D(worldPos.xz * 26.0) - 0.5) * 0.038 * (1.0 - 0.78 * puddleStrength);
    float grainZ = (valueNoise2D(worldPos.xz * 26.0 + float2(19.1, 4.7)) - 0.5) * 0.038 * (1.0 - 0.78 * puddleStrength);

    float2 grad = (g0 * 0.075 + g1 * 0.048) * puddleStrength + float2(windWave + grainX, windWave * 0.7 + grainZ);
    return safeNormalize(float3(grad.x, 1.0, grad.y));
}

float3 evalTaipeiSkyAndBeams(float3 ro, float3 rd, float filmTime, float revealOpen01, float emissivePulse, bool isMidnight)
{
    const float3 colMidnight = isMidnight ? float3(0.004, 0.007, 0.016) : float3(0.006, 0.010, 0.022);
    const float3 colJade     = float3(0.03, 0.82, 0.70);
    const float3 colOrchid   = isMidnight ? float3(0.92, 0.16, 0.74) : float3(0.84, 0.22, 0.88);
    const float3 colAmber    = float3(1.00, 0.64, 0.16);

    // Keep central sanctuary backdrop deep midnight-obsidian during reveal so separated crystal petals pop with high contrast
    float centerBackdropMask = 1.0 - 0.72 * revealOpen01 * exp(-9.0 * rd.x * rd.x);
    float horizon = exp(-5.8 * abs(rd.y + 0.02));
    float3 sky = colMidnight * (1.15 - 0.45 * saturate(rd.y));
    sky += lerp(colJade, colOrchid, 0.5 + 0.5 * sin(rd.x * 3.2 + filmTime * 0.16)) * horizon * 0.20 * centerBackdropMask;

    // Distant Taipei skyline architectural window haze (flanking the monoliths, recessed behind the central vault)
    float colCoord = rd.x * 14.0;
    float towerHash = hash12(float2(floor(colCoord), 7.0));
    float towerTop = 0.05 + 0.17 * towerHash;
    if (rd.y > -0.04 && rd.y < towerTop)
    {
        float win = step(0.76, frac(rd.y * 85.0)) * step(0.60, frac(colCoord * 3.0));
        float3 neonPick = (towerHash > 0.66) ? colAmber : ((towerHash > 0.33) ? colJade : colOrchid);
        float flankMask = smoothstep(0.06, 0.22, abs(rd.x)) + (1.0 - revealOpen01) * 0.45;
        sky += neonPick * win * 0.14 * exp(-2.2 * abs(rd.x)) * saturate(flankMask);
    }

    // Restrained atmospheric sanctuary shaft around (0, 7.80, 5.50)
    float3 orchidCenter = float3(0.0, 7.80, 5.50);
    float3 toCenter = orchidCenter - ro;
    float proj = clamp(dot(toCenter, rd), 0.0, 28.0);
    float3 closest = ro + rd * proj;
    float3 delta = closest - orchidCenter;
    float radialSq = delta.x * delta.x * 1.65 + delta.y * delta.y * 0.42 + delta.z * delta.z * 0.95;
    float vaultGlow = (0.06 + 0.24 * revealOpen01) * (1.0 + emissivePulse * 1.4);
    sky += lerp(colJade, colOrchid, 0.42) * (0.11 / (1.10 + radialSq)) * vaultGlow;
    sky += colAmber * (0.030 / (0.75 + radialSq * 2.0)) * revealOpen01;

    // Fine rain streak shimmer in air during boulevard & crane shots
    float2 rainUv = float2(rd.x * 28.0 + rd.z * 5.0, rd.y * 10.5 + filmTime * 2.5);
    float rainLine = smoothstep(0.974, 1.0, sin(rainUv.x * 9.0) * 0.5 + 0.5) *
                     (0.5 + 0.5 * sin(rainUv.y * 14.0));
    sky += colJade * rainLine * 0.028 * (1.0 - 0.72 * revealOpen01);

    return sky;
}

float evalAnalyticGroundContactAo(float3 hitPos)
{
    // Ground contact shadow beneath colonnade pillars (x = +-4.5, z = -12..20 step 4) and monoliths (z = 5.5)
    float ao = 1.0;
    if (hitPos.y < 1.2)
    {
        float hFade = saturate(1.0 - hitPos.y / 1.2);
        float nearZ = clamp(round(hitPos.z / 4.0) * 4.0, -12.0, 20.0);
        float2 dPillar = abs(float2(abs(hitPos.x) - 4.5, hitPos.z - nearZ)) - float2(0.46, 0.46);
        float distP = length(max(dPillar, 0.0.xx));
        ao *= lerp(1.0, smoothstep(0.0, 0.85, distP) * 0.72 + 0.28, hFade);

        float2 dMono = abs(float2(abs(hitPos.x) - 2.9, hitPos.z - 5.5)) - float2(1.15, 2.2);
        float distM = length(max(dMono, 0.0.xx));
        ao *= lerp(1.0, smoothstep(0.0, 1.45, distM) * 0.65 + 0.35, hFade);
    }
    return ao;
}

float3 evalThinFilmInterference(float cosTheta, float thicknessMicrons)
{
    float3 wavelength = float3(0.645, 0.530, 0.460);
    float3 ior = 1.50 + 0.0055 / (wavelength * wavelength);
    float phaseScale = 4.0 * kPi * thicknessMicrons * max(cosTheta, 0.06);
    return 0.5 + 0.5 * cos(phaseScale * ior / wavelength + float3(0.0, 0.35, 0.70));
}

#if defined(BOO_M1_DXR_CS) || defined(BOO_M1_RASTER_PS)
#if defined(BOO_M1_DXR_CS)
Texture2D<float4> gTensorFieldTex : register(t3);
#else
Texture2D<float4> gTensorFieldTex : register(t0);
#endif

float4 sampleTensorFieldBilinear(float2 uv)
{
    float2 wrapped = frac(uv);
    float2 coord = wrapped * float2(320.0, 180.0) - 0.5;
    int2 base = int2(floor(coord));
    float2 f = coord - float2(base);

    int2 p00 = int2((base.x + 320) % 320, (base.y + 180) % 180);
    int2 p10 = int2((base.x + 321) % 320, (base.y + 180) % 180);
    int2 p01 = int2((base.x + 320) % 320, (base.y + 181) % 180);
    int2 p11 = int2((base.x + 321) % 320, (base.y + 181) % 180);

    float4 c00 = gTensorFieldTex.Load(int3(p00, 0));
    float4 c10 = gTensorFieldTex.Load(int3(p10, 0));
    float4 c01 = gTensorFieldTex.Load(int3(p01, 0));
    float4 c11 = gTensorFieldTex.Load(int3(p11, 0));
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}
#else
float4 sampleTensorFieldBilinear(float2 uv)
{
    return float4(0.035, 0.045, 0.055, 0.5 + 0.0 * uv.x);
}
#endif

float3 shadeSurfaceMaterial(
    uint matKind,
    uint subIdx,
    float2 surfUv,
    float3 hitPos,
    float3 hitNormal,
    float3 viewDir,
    float3 reflColor,
    float3 transColor,
    float ambientOcc,
    float filmTime,
    float revealOpen01,
    float emissivePulse,
    bool isMidnight)
{
    const float3 colJade   = float3(0.05, 0.88, 0.74);
    const float3 colOrchid = isMidnight ? float3(0.94, 0.18, 0.76) : float3(0.86, 0.24, 0.90);
    const float3 colRuby   = float3(0.95, 0.12, 0.42);
    const float3 colAmber  = float3(1.00, 0.64, 0.14);
    const float3 colIvory  = float3(0.84, 0.79, 0.71);
    const float3 colBrass  = float3(0.82, 0.63, 0.33);

    float ndotv = saturate(dot(hitNormal, viewDir));
    float fresnelDielectric = 0.042 + 0.958 * pow(max(1.0 - ndotv, 0.0), 4.5);

    // Core sanctuary point light at nectar heart (0, 7.78, 5.10)
    float3 corePos = float3(0.0, 7.78, 5.10);
    float3 toCore = corePos - hitPos;
    float coreDist = max(length(toCore), 0.22);
    float3 coreDir = toCore / coreDist;
    float coreNdotL = saturate(dot(hitNormal, coreDir));
    float3 coreLight = lerp(colAmber, colOrchid, 0.32) *
                       ((1.45 * (0.18 + 0.82 * revealOpen01)) / (0.65 + 0.22 * coreDist * coreDist));

    // Sculptural sanctuary top-key (cool jade) + grazing back-rim (orchid violet)
    float3 keyDir = safeNormalize(float3(-0.36, 0.82, -0.44));
    float3 rimDir = safeNormalize(float3(0.42, 0.34, 0.84));
    float keyNdotL = saturate(dot(hitNormal, keyDir));
    float rimNdotL = saturate(dot(hitNormal, rimDir));
    float3 keyLight = lerp(colJade, float3(0.75, 0.92, 1.0), 0.45) * (0.34 + 0.42 * revealOpen01);
    float3 rimLight = lerp(colOrchid, colJade, 0.38) * (0.24 + 0.48 * revealOpen01);

    // Boulevard colonnade neon strip illumination along x = +-3.92
    float neonWave = 0.5 + 0.5 * sin(hitPos.z * 0.58 - filmTime * 0.75);
    float3 neonCol = lerp(colJade, lerp(colAmber, colOrchid, 0.35), neonWave);
    float dxNeon = abs(abs(hitPos.x) - 3.92);
    float neonFalloff = 1.0 / (1.0 + 0.35 * dxNeon * dxNeon + 0.06 * hitPos.y * hitPos.y);

    float contactAo = saturate(ambientOcc * evalAnalyticGroundContactAo(hitPos));
    float3 lit = 0.0.xxx;

    if (matKind == 0u)
    {
        // Material 0: Wet Taipei asphalt roadbed with spatially varying puddle vs damp micro-roughness
        float puddle = evalWetPuddleMask(hitPos.xz);
        float aggregate = valueNoise2D(hitPos.xz * 18.0);
        float lane = smoothstep(0.055, 0.018, abs(abs(hitPos.x) - 1.82)) *
                     step(0.38, frac(hitPos.z * 0.22)) *
                     (0.55 + 0.45 * aggregate);

        float3 dryAsphalt = float3(0.022, 0.025, 0.034) * (0.80 + 0.40 * aggregate);
        float3 wetAsphalt = float3(0.006, 0.008, 0.013);
        float3 base = lerp(dryAsphalt, wetAsphalt, puddle);

        float3 lanePaint = float3(0.72, 0.44, 0.08) * lane * lerp(0.55, 0.28, puddle);
        float reflWeight = lerp(0.11, 0.84, puddle) * (0.28 + 0.72 * fresnelDielectric);

        float3 hvKey = safeNormalize(keyDir + viewDir);
        float specPow = lerp(14.0, 96.0, puddle);
        float spec = pow(max(dot(hitNormal, hvKey), 0.0), specPow) * lerp(0.08, 0.45, puddle);

        // Tensor Core 64-layer SIREN refractive caustic filaments focused strictly inside wet puddles
        float4 tensorRoad = sampleTensorFieldBilinear(hitPos.xz * float2(0.145, 0.105) + float2(0.5, 0.35));
        float3 roadDelta  = max(tensorRoad.rgb - float3(0.042, 0.052, 0.062), 0.0.xxx);
        float  roadWave   = saturate(max(roadDelta.r, max(roadDelta.g, roadDelta.b)) * 1.15);
        float  roadRidge  = pow(roadWave, 3.0);
        float3 roadCausticHue = lerp(colJade, lerp(colAmber, colOrchid, saturate(tensorRoad.b)), saturate(tensorRoad.r * 0.85));
        float3 roadCaustic = roadCausticHue * roadRidge * (puddle * puddle * 0.18) * (0.45 + 0.55 * revealOpen01) * contactAo;

        lit = (base + lanePaint) * (0.18 * contactAo + neonCol * neonFalloff * 0.42 + coreNdotL * coreLight * 0.35)
            + lanePaint * 0.16
            + neonCol * neonFalloff * lerp(0.04, 0.11, puddle) * contactAo
            + keyLight * spec * contactAo
            + roadCaustic
            + reflColor * reflWeight * lerp(0.65, 1.0, contactAo);
    }
    else if (matKind == 1u)
    {
        // Material 1: Polished obsidian black-enamel architecture (subIdx 0 = street pillars, subIdx 1 = twin monoliths/plinth)
        float3 base = float3(0.006, 0.008, 0.014);
        float3 inlay = 0.0.xxx;
        if (subIdx == 1u)
        {
            // Razor-sharp hairline architectural gold/jade reveal seams on the twin sanctuary monoliths & plinth
            float seamY = exp(-185.0 * abs(frac(hitPos.y * 0.25) - 0.5));
            float innerEdge = exp(-12.0 * abs(surfUv.x - 0.5));
            inlay += colAmber * seamY * 0.075 * (1.0 + emissivePulse);
            inlay += colJade * exp(-140.0 * abs(frac(hitPos.z * 0.45) - 0.5)) * 0.065 * innerEdge;

            // Tensor Core sanctuary water-and-crystal caustic curtains dancing subtly up inner obsidian monolith walls
            float4 tensorWall = sampleTensorFieldBilinear(float2(hitPos.z * 0.16 + 0.25, hitPos.y * 0.11));
            float3 wallDelta  = max(tensorWall.rgb - float3(0.042, 0.052, 0.062), 0.0.xxx);
            float  wallRidge  = pow(saturate(max(wallDelta.r, max(wallDelta.g, wallDelta.b)) * 1.15), 2.8);
            float  vaultFocus = exp(-0.22 * hitPos.x * hitPos.x);
            inlay += lerp(colJade, colAmber, saturate(tensorWall.r)) * wallRidge * (0.06 + 0.11 * revealOpen01) * vaultFocus;
        }
        else
        {
            // Crisp plinth band near street pillar foot (y ~ 0.35m)
            float plinth = exp(-68.0 * abs(hitPos.y - 0.36));
            inlay += colAmber * plinth * 0.14;
        }

        float3 hvKey = safeNormalize(keyDir + viewDir);
        float3 hvCore = safeNormalize(coreDir + viewDir);
        float spec = pow(max(dot(hitNormal, hvKey), 0.0), 96.0) * 0.32
                   + pow(max(dot(hitNormal, hvCore), 0.0), 72.0) * 0.42;
        float reflWeight = 0.14 + 0.78 * fresnelDielectric;

        lit = base * (0.18 * contactAo + keyNdotL * keyLight + coreNdotL * coreLight + neonCol * neonFalloff * 0.32)
            + (keyLight + coreLight * 0.7) * spec * contactAo
            + inlay
            + reflColor * reflWeight * lerp(0.55, 1.0, contactAo);
    }
    else if (matKind == 2u)
    {
        // Material 2: Framed Taipei vertical boulevard light-boxes
        float2 frameDist = abs(surfUv - 0.5) * 2.0;
        float isFrame = smoothstep(0.78, 0.90, max(frameDist.x, frameDist.y * 0.94));
        float glyphBand = step(0.28, frac(hitPos.y * 2.8 + 0.15)) * (0.65 + 0.35 * sin(hitPos.y * 9.0 + hitPos.z));
        float palettePick = frac(abs(hitPos.z) * 0.173 + (hitPos.x > 0.0 ? 0.37 : 0.0));
        float3 signHue = (palettePick < 0.38) ? colJade : ((palettePick < 0.72) ? colAmber : colOrchid);

        float3 panelGlow = signHue * (0.68 + 0.42 * glyphBand) * (1.0 + emissivePulse * 1.8);
        float3 frameCol = float3(0.018, 0.022, 0.030);
        lit = lerp(panelGlow, frameCol, isFrame)
            + reflColor * (0.12 + 0.45 * fresnelDielectric) * (0.35 + 0.65 * isFrame);
    }
    else if (matKind == 3u)
    {
        // Material 3: Sculpted Botanical Refractive Glass Orchid Petals, Sepals & Cupped Labellum
        // surfUv.x in [0,1] across blade width (0.5 = midrib), surfUv.y in [0,1] from claw (0) to apex (1)
        float edgeU = abs(surfUv.x * 2.0 - 1.0);
        float bladeV = saturate(surfUv.y);

        // Curved vascular venation radiating from midrib toward margins + crisp glass rim facet highlight
        float midrib = exp(-38.0 * edgeU * edgeU) * (1.0 - 0.42 * bladeV);
        float veinCoord = edgeU * 9.5 - bladeV * 3.2;
        float fineVein = pow(0.5 + 0.5 * cos(veinCoord * kTwoPi), 7.0) * (1.0 - 0.25 * edgeU) * smoothstep(0.05, 0.94, bladeV);
        float marginGlow = smoothstep(0.76, 0.99, edgeU);

        // Tensor Core high-order thin-film phase & internal caustic dispersion across crystal petal blades
        float4 tensorPetal = sampleTensorFieldBilinear(surfUv * float2(1.35, 1.65) + float2((float)subIdx * 0.13, 0.07));
        float ruffleThickness = 0.34 + 0.14 * midrib + 0.05 * fineVein + 0.08 * edgeU + (tensorPetal.a - 0.5) * 0.24;

        // Cauchy thin-film interference strictly weighting grazing Fresnel & specular facets
        float3 thinFilm = evalThinFilmInterference(ndotv, ruffleThickness);

        // Optical path through sculpted thin-shell petal (thicker at claw/midrib, crystal-thin at margin/tip)
        float shellThickness = (0.16 + 0.50 * (1.0 - 0.74 * edgeU * edgeU) * (1.0 - 0.56 * bladeV));
        float opticalDepth = shellThickness / max(ndotv, 0.24);

        float3 absorptionSigma;
        float3 veinTint;
        float3 internalScatter;

        if (subIdx == 5u)
        {
            // Sculpted 3-lobed Labellum (lip): deep crimson-ruby & golden-amber throat nectar calligraphy
            float throatMask = exp(-3.0 * bladeV);
            float nectarCallus = exp(-26.0 * pow(abs(edgeU - 0.20), 2.0)) * smoothstep(0.65, 0.04, bladeV);
            float spots = smoothstep(0.70, 0.94, sin(edgeU * 26.0) * cos(bladeV * 32.0)) * throatMask;

            absorptionSigma = lerp(float3(0.22, 1.55, 0.62), float3(0.85, 0.24, 0.58), smoothstep(0.18, 0.78, bladeV));
            veinTint = lerp(colRuby, colAmber, throatMask * 0.68);
            internalScatter = colAmber * (throatMask * 0.22 + nectarCallus * 0.36 + spots * 0.20)
                            + colRuby * ((1.0 - 0.5 * throatMask) * 0.22 * (midrib + fineVein * 0.8 + marginGlow * 0.45));
        }
        else if (subIdx == 0u || subIdx == 3u || subIdx == 4u || subIdx >= 12u)
        {
            // Posterior 3 Sepals (12h dorsal, 8h/4h lateral) & calyx lancets: translucent imperial celadon-jade crystal!
            absorptionSigma = float3(1.35, 0.16, 0.42);
            veinTint = lerp(colJade, float3(0.35, 0.96, 0.88), 0.35 + 0.45 * bladeV);
            internalScatter = veinTint * (0.14 * midrib + 0.085 * fineVein + 0.12 * marginGlow);
        }
        else
        {
            // Anterior Broad Butterfly Wing Petals (10h, 2h) & inner perianth: luminous orchid-amethyst -> icy cyan crystal
            absorptionSigma = lerp(float3(0.48, 0.32, 0.14), float3(0.22, 0.68, 0.16), smoothstep(0.15, 0.88, bladeV));
            veinTint = lerp(colJade, colOrchid, saturate(0.38 + 0.58 * bladeV));
            float clawWarmth = exp(-5.2 * bladeV);
            internalScatter = colAmber * (clawWarmth * 0.16)
                            + veinTint * (0.13 * midrib + 0.08 * fineVein)
                            + lerp(colOrchid, colJade, 0.45) * (0.15 * marginGlow);
        }

        float3 beerLambert = exp(-absorptionSigma * opticalDepth * 1.45);

        // Dielectric glass Fresnel (IOR = 1.52 -> F0 = 0.0426)
        float fresnelGlass = 0.043 + 0.957 * pow(max(1.0 - ndotv, 0.0), 4.0);
        float3 transmitted = transColor * beerLambert * (1.0 - fresnelGlass) * 0.90;

        // Sharp dual-lobe crystal specular + thin-film iridescent rim
        float3 hvKey  = safeNormalize(keyDir + viewDir);
        float3 hvRim  = safeNormalize(rimDir + viewDir);
        float3 hvCore = safeNormalize(coreDir + viewDir);
        float specKey  = pow(max(dot(hitNormal, hvKey), 0.0), 136.0);
        float specRim  = pow(max(dot(hitNormal, hvRim), 0.0), 76.0);
        float specCore = pow(max(dot(hitNormal, hvCore), 0.0), 84.0);

        // Back-lit translucent forward-scattering + luminous sculpted crystal edge rim
        float backTranslucency = saturate(dot(-hitNormal, coreDir)) * (0.30 + 0.70 * edgeU);
        float rimFresnel = pow(max(1.0 - ndotv, 0.0), 2.8);
        float3 subsurf = coreLight * beerLambert * backTranslucency * 0.34
                       + lerp(veinTint, thinFilm, 0.52) * (rimFresnel * 0.36 + marginGlow * 0.12) * (0.35 + 0.65 * revealOpen01);

        float3 surfaceRefl = reflColor * lerp(float3(1.0, 1.0, 1.0), thinFilm * 1.35, 0.62) * (0.14 + 0.86 * fresnelGlass);
        float3 specularGlint = keyLight * specKey * 0.90
                             + rimLight * specRim * thinFilm * 0.85
                             + coreLight * specCore * 0.55;
        float3 tensorDispersion = max(tensorPetal.rgb - float3(0.035, 0.045, 0.055), 0.0.xxx) *
                                  (0.22 + 0.26 * fresnelGlass) * (0.45 + 0.55 * revealOpen01);

        lit = transmitted
            + surfaceRefl
            + specularGlint
            + subsurf
            + tensorDispersion
            + internalScatter * (0.42 + 0.58 * revealOpen01) * (1.0 + emissivePulse * 1.3);
    }
    else if (matKind == 4u)
    {
        // Material 4: Jewel-sized molten amber-ruby pollinia / nectar core (deep saturated gold-crimson, zero whiteout)
        float3 coreGrad = lerp(float3(0.96, 0.20, 0.05), float3(1.00, 0.62, 0.12), pow(ndotv, 1.35));
        float facetSparkle = pow(max(dot(hitNormal, safeNormalize(keyDir + viewDir)), 0.0), 80.0);
        lit = coreGrad * (0.92 + 0.52 * revealOpen01 + emissivePulse * 1.6)
            + colRuby * pow(max(1.0 - ndotv, 0.0), 2.2) * 0.55
            + reflColor * (0.16 + 0.42 * fresnelDielectric)
            + float3(1.0, 0.82, 0.42) * facetSparkle * 0.42;
    }
    else if (matKind == 5u)
    {
        // Material 5: Warm Porcelain-Ivory Enamel (Gynostemium Column — distinctly opaque sculptural ceramic)
        float3 tipBlush = lerp(colIvory, float3(0.90, 0.52, 0.64), smoothstep(0.58, 1.0, surfUv.y) * 0.48);
        float wrapCore = saturate(0.32 + 0.68 * dot(hitNormal, coreDir));
        float wrapKey  = saturate(0.25 + 0.75 * dot(hitNormal, keyDir));
        float3 hvKey   = safeNormalize(keyDir + viewDir);
        float clearcoatSpec = pow(max(dot(hitNormal, hvKey), 0.0), 46.0);

        lit = tipBlush * (0.14 * contactAo + wrapKey * keyLight * 0.78 + wrapCore * coreLight * 0.62 + rimNdotL * rimLight * 0.32)
            + float3(1.0, 0.96, 0.90) * clearcoatSpec * 0.34 * contactAo
            + reflColor * (0.05 + 0.30 * fresnelDielectric) * contactAo;
    }
    else
    {
        // Material 6: Brushed Spy-Optical Brass / Dark Obsidian-Titanium (subIdx 0=pedicel/calyx, 1=vault louvers, 2=gold stamens)
        float3 metalF0 = colBrass;
        float specExp = 40.0;
        float3 opticalSlit = 0.0.xxx;

        if (subIdx == 1u)
        {
            // Articulated sanctuary vault louvers: sleek dark gunmetal-titanium with crisp jade-amber optical edge slit
            metalF0 = float3(0.085, 0.095, 0.115);
            specExp = 64.0;
            float edgeBevel = smoothstep(0.80, 0.97, abs(surfUv.y * 2.0 - 1.0));
            float centerWarmth = exp(-0.32 * hitPos.x * hitPos.x);
            opticalSlit = lerp(colJade, colAmber, 0.48) * edgeBevel * centerWarmth * (0.24 + 0.20 * revealOpen01);
        }
        else if (subIdx == 2u)
        {
            // Delicate 24k satin-gold stamen filaments
            metalF0 = float3(0.94, 0.74, 0.34);
            specExp = 52.0;
        }
        else if (subIdx == 3u)
        {
            // Taipei 101 660-Metric-Ton Polished 24k-Gold Tuned Mass Damper (TMD) Sphere
            metalF0 = float3(1.00, 0.78, 0.28);
            specExp = 96.0;
            float ringGroove = smoothstep(0.75, 0.96, abs(sin(surfUv.y * 28.0)));
            metalF0 *= (1.0 - 0.22 * ringGroove);
            float sphereShading = 0.28 + 0.72 * pow(ndotv, 1.35);
            opticalSlit = float3(1.00, 0.66, 0.16) * sphereShading * (0.34 + 0.26 * revealOpen01) * (1.0 + emissivePulse * 0.8);
        }
        else
        {
            // Botanical pedicel stem & pedestal collar: patinated bronze -> polished spy-brass
            metalF0 = lerp(float3(0.32, 0.25, 0.15), colBrass, smoothstep(0.1, 0.9, surfUv.y));
            specExp = 38.0;
        }

        float3 fresnelMetal = metalF0 + (1.0.xxx - metalF0) * pow(max(1.0 - ndotv, 0.0), 5.0);
        // Subtle brushed micro-groove modulation along circumference/length
        float brush = 0.88 + 0.12 * sin(surfUv.x * 120.0 + surfUv.y * 24.0);
        float3 hvKey  = safeNormalize(keyDir + viewDir);
        float3 hvCore = safeNormalize(coreDir + viewDir);
        float3 hvRim  = safeNormalize(rimDir + viewDir);
        float specK = pow(max(dot(hitNormal, hvKey), 0.0), specExp);
        float specC = pow(max(dot(hitNormal, hvCore), 0.0), specExp * 0.8);
        float specR = pow(max(dot(hitNormal, hvRim), 0.0), specExp * 0.7);

        lit = metalF0 * brush * (0.08 * contactAo + keyNdotL * keyLight * 0.38 + coreNdotL * coreLight * 0.42)
            + fresnelMetal * brush * (keyLight * specK * 0.95 + coreLight * specC * 0.85 + rimLight * specR * 0.55) * contactAo
            + reflColor * fresnelMetal * brush * 0.68 * lerp(0.55, 1.0, contactAo)
            + opticalSlit;
    }

    if (matKind == 7u)
    {
        // Material 7: Architecturally Accurate Taipei 101 Canted Bamboo Curtain-Wall Glass & Ruyi Bronze Trim
        float4 tensorTower = sampleTensorFieldBilinear(float2(hitPos.x * 0.18 + 0.5, hitPos.y * 0.065 - filmTime * 0.03));
        float floorBand = frac(surfUv.y * 8.0);
        float mullionH  = smoothstep(0.86, 0.96, floorBand) + smoothstep(0.10, 0.02, floorBand);
        float mullionV  = smoothstep(0.84, 0.96, frac(surfUv.x * 16.0));
        float tierSpandrel = smoothstep(0.90, 0.99, surfUv.y) + smoothstep(0.08, 0.01, surfUv.y);
        float cornerRuyi = smoothstep(0.36, 0.48, abs(surfUv.x - 0.5)) + tierSpandrel * 0.85;
        float frameMask = saturate(mullionH * 0.52 + mullionV * 0.42 + cornerRuyi * 0.78);

        // Deep nocturnal emerald-jade low-iron curtain glass (crisp silhouette + zero whiteout!)
        float3 glassTint = float3(0.008, 0.048, 0.046);
        float3 ruyiBronze = float3(0.46, 0.36, 0.18);
        float3 baseCol = lerp(glassTint, ruyiBronze * 0.22, frameMask);

        // Restrained warm Taipei night interior office windows (recessed observatory bay stays dark behind Golden TMD!)
        float winHash = hash12(float2(floor(surfUv.x * 16.0), floor(hitPos.y * 4.0)));
        float winLit  = step(0.72, winHash) * (1.0 - saturate(frameMask * 1.25));
        float3 winCol = lerp(colJade * 0.55, colAmber, step(0.86, winHash));
        float3 interior = winCol * winLit * ((subIdx == 1u) ? 0.06 : 0.12);
        if (subIdx == 1u)
        {
            // Dark emerald-bronze framing around 88F-91F Sanctuary Observatory so the 24k-Gold TMD Sphere pops!
            interior += colJade * 0.07 * tierSpandrel;
        }

        float3 hvKey = safeNormalize(keyDir + viewDir);
        float specGlass = pow(max(dot(hitNormal, hvKey), 0.0), 96.0);
        float3 ruyiCrest = lerp(colJade, colAmber, 0.55) * smoothstep(0.92, 0.99, surfUv.y) * 0.15;
        float3 towerDelta = max(tensorTower.rgb - float3(0.042, 0.052, 0.062), 0.0.xxx);
        float  towerRidge = pow(saturate(max(towerDelta.r, max(towerDelta.g, towerDelta.b)) * 1.15), 2.8);
        float3 tensorCaustic = colJade * towerRidge * 0.09;

        lit = baseCol * (0.42 + 0.58 * keyNdotL) * contactAo
            + interior
            + ruyiCrest
            + tensorCaustic
            + reflColor * lerp(float3(0.32, 0.88, 0.78), float3(0.95, 0.78, 0.45), frameMask * 0.55) * (0.14 + 0.68 * fresnelDielectric)
            + keyLight * specGlass * 0.42;
    }

    // Restrained Taipei night atmospheric perspective
    float radialDist = length(hitPos - float3(0.0, 3.0, -6.0));
    float fog = 1.0 - exp(-0.011 * max(radialDist - 6.0, 0.0));
    float3 fogCol = isMidnight ? float3(0.008, 0.013, 0.028) : float3(0.011, 0.018, 0.036);
    return lerp(lit, fogCol, saturate(fog * 0.48));
}

// =============================================================================
// PASS 1A: Hardware DXR 1.1 Inline RayQuery Compute Scene (SM 6.5)
// =============================================================================
#if defined(BOO_M1_DXR_CS)
cbuffer M1SceneCB : register(b0)
{
    float3 gCamEye;
    float  gFovY;
    float3 gCamRight;
    float  gAspect;
    float3 gCamUp;
    float  gFilmTime;
    float3 gCamForward;
    float  gRevealOpen01;
    uint   gCanvasWidth;
    uint   gCanvasHeight;
    uint   gExpectSeq;
    uint   gSceneFlags;     // bit0: midnight
    float  gEmissivePulse;
    float  gMoodIntensity;
    uint   gShotIndex;
    uint   gDeterministicSeed;
    uint4  gMeshBaseIndices;
};

RaytracingAccelerationStructure gSceneTlas    : register(t0);
StructuredBuffer<GpuM1Vertex>   gMeshVertices : register(t1);
StructuredBuffer<uint>          gMeshIndices  : register(t2);
RWStructuredBuffer<uint>        gVerifyOut    : register(u0);
RWTexture2D<float4>             gOutHdr       : register(u1);
RWTexture2D<float>              gOutDepth     : register(u2);

void unpackInstanceMetadata(uint packedId, out uint meshId, out uint matKind, out uint subIdx)
{
    meshId  = packedId & 7u;
    matKind = (packedId >> 3u) & 15u;
    subIdx  = (packedId >> 7u) & 255u;
}

uint lookupMeshBaseIndex(uint meshId)
{
    if (meshId == 0u) return gMeshBaseIndices.x;
    if (meshId == 1u) return gMeshBaseIndices.y;
    if (meshId == 2u) return gMeshBaseIndices.z;
    if (meshId == 3u) return gMeshBaseIndices.w;
    return (gShotIndex >> 8u);
}

void evalDxrHitAttributes(
    uint packedInstId,
    uint primIdx,
    float2 bary,
    float3 worldRayOrigin,
    float3 worldRayDir,
    float hitT,
    float3x4 objToWorld,
    out float3 outHitPos,
    out float3 outNormal,
    out float2 outUv,
    out uint outMatKind,
    out uint outSubIdx)
{
    outHitPos = worldRayOrigin + worldRayDir * hitT;
    uint meshId;
    unpackInstanceMetadata(packedInstId, meshId, outMatKind, outSubIdx);

    uint baseIdx = lookupMeshBaseIndex(meshId) + primIdx * 3u;
    uint i0 = gMeshIndices[baseIdx + 0u];
    uint i1 = gMeshIndices[baseIdx + 1u];
    uint i2 = gMeshIndices[baseIdx + 2u];

    GpuM1Vertex v0 = gMeshVertices[i0];
    GpuM1Vertex v1 = gMeshVertices[i1];
    GpuM1Vertex v2 = gMeshVertices[i2];

    float w0 = 1.0 - bary.x - bary.y;
    float3 objN = safeNormalize(v0.normAndV.xyz * w0 + v1.normAndV.xyz * bary.x + v2.normAndV.xyz * bary.y);
    outUv = float2(v0.posAndU.w, v0.normAndV.w) * w0
          + float2(v1.posAndU.w, v1.normAndV.w) * bary.x
          + float2(v2.posAndU.w, v2.normAndV.w) * bary.y;

    if (outMatKind == 0u)
    {
        outNormal = evalWetRainRippleNormal(outHitPos, gFilmTime);
        return;
    }

    // Exact 3x3 cofactor normal transform supporting full 3D rotation + non-uniform botanical scaling
    float3 c0 = float3(objToWorld._m00, objToWorld._m10, objToWorld._m20);
    float3 c1 = float3(objToWorld._m01, objToWorld._m11, objToWorld._m21);
    float3 c2 = float3(objToWorld._m02, objToWorld._m12, objToWorld._m22);
    float3 cof0 = cross(c1, c2);
    float3 cof1 = cross(c2, c0);
    float3 cof2 = cross(c0, c1);
    float detSign = (dot(c0, cof0) >= 0.0) ? 1.0 : -1.0;

    float3 worldN = safeNormalize((cof0 * objN.x + cof1 * objN.y + cof2 * objN.z) * detSign);
    if (dot(worldN, -worldRayDir) < 0.0)
    {
        worldN = -worldN;
    }
    outNormal = worldN;
}

float3 refractAirToGlass(float3 incidentDir, float3 surfNormal, float eta)
{
    float cosi = clamp(dot(-incidentDir, surfNormal), 0.0, 1.0);
    float k = max(1.0 - eta * eta * (1.0 - cosi * cosi), 0.0);
    return safeNormalize(incidentDir * eta + surfNormal * (eta * cosi - sqrt(k)));
}

float3 traceSecondaryRadiance(
    float3 rayOrigin,
    float3 rayDir,
    float tMax,
    bool isMidnight,
    inout uint ioVerifiedHits)
{
    RayDesc secRay;
    secRay.Origin    = rayOrigin;
    secRay.Direction = rayDir;
    secRay.TMin      = 0.018;
    secRay.TMax      = tMax;

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE> rqSec;
    rqSec.TraceRayInline(gSceneTlas, RAY_FLAG_NONE, 0xFFu, secRay);
    while (rqSec.Proceed())
    {
    }

    float3 envCol = evalTaipeiSkyAndBeams(rayOrigin, rayDir, gFilmTime, gRevealOpen01, gEmissivePulse, isMidnight);
    if (rqSec.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return envCol;
    }

    ioVerifiedHits += 1u;
    float3 sPos;
    float3 sNorm;
    float2 sUv;
    uint sMat;
    uint sSub;
    evalDxrHitAttributes(
        rqSec.CommittedInstanceID(),
        rqSec.CommittedPrimitiveIndex(),
        rqSec.CommittedTriangleBarycentrics(),
        rayOrigin,
        rayDir,
        rqSec.CommittedRayT(),
        rqSec.CommittedObjectToWorld3x4(),
        sPos,
        sNorm,
        sUv,
        sMat,
        sSub);

    float3 sReflEnv = evalTaipeiSkyAndBeams(sPos, reflect(rayDir, sNorm), gFilmTime, gRevealOpen01, gEmissivePulse, isMidnight);
    float3 sTransEnv = evalTaipeiSkyAndBeams(sPos, rayDir, gFilmTime, gRevealOpen01, gEmissivePulse, isMidnight);
    return shadeSurfaceMaterial(
        sMat,
        sSub,
        sUv,
        sPos,
        sNorm,
        -rayDir,
        sReflEnv,
        sTransEnv,
        1.0,
        gFilmTime,
        gRevealOpen01,
        gEmissivePulse,
        isMidnight);
}

[numthreads(8, 8, 1)]
void CSDxrTaipeiScene(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x == 0u && dtid.y == 0u)
    {
        gVerifyOut[0] = gExpectSeq;
    }

    if (dtid.x >= gCanvasWidth || dtid.y >= gCanvasHeight)
        return;

    const bool isMidnight = (gSceneFlags & 1u) != 0u;
    float2 uv = (float2(dtid.xy) + 0.5) / float2((float)gCanvasWidth, (float)gCanvasHeight);
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float tanHalfFov = tan(gFovY * 0.5);
    float3 rd = safeNormalize(gCamForward + gCamRight * (ndc.x * gAspect * tanHalfFov) + gCamUp * (ndc.y * tanHalfFov));

    RayDesc primaryRay;
    primaryRay.Origin    = gCamEye;
    primaryRay.Direction = rd;
    primaryRay.TMin      = 0.02;
    primaryRay.TMax      = 72.0;

    RayQuery<RAY_FLAG_CULL_NON_OPAQUE> rqPrimary;
    rqPrimary.TraceRayInline(gSceneTlas, RAY_FLAG_NONE, 0xFFu, primaryRay);
    while (rqPrimary.Proceed())
    {
    }

    uint verifiedHits = 0u;
    float3 hdrColor = 0.0.xxx;
    float primaryRayT = 1000.0;

    if (rqPrimary.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        verifiedHits += 1u;
        primaryRayT = rqPrimary.CommittedRayT();
        float3 hitPos;
        float3 hitNormal;
        float2 surfUv;
        uint matKind;
        uint subIdx;
        evalDxrHitAttributes(
            rqPrimary.CommittedInstanceID(),
            rqPrimary.CommittedPrimitiveIndex(),
            rqPrimary.CommittedTriangleBarycentrics(),
            gCamEye,
            rd,
            rqPrimary.CommittedRayT(),
            rqPrimary.CommittedObjectToWorld3x4(),
            hitPos,
            hitNormal,
            surfUv,
            matKind,
            subIdx);

        // Roughness-aware reflection vector on damp asphalt; crisp mirror/crystal reflection elsewhere
        float3 reflNormal = hitNormal;
        if (matKind == 0u)
        {
            float puddle = evalWetPuddleMask(hitPos.xz);
            float roughSpread = (1.0 - puddle) * 0.075;
            float2 h2 = float2(hash12(hitPos.xz * 83.0), hash12(hitPos.xz * 83.0 + 19.7)) - 0.5;
            reflNormal = safeNormalize(hitNormal + float3(h2.x * roughSpread, 0.0, h2.y * roughSpread));
        }

        float3 reflDir = reflect(rd, reflNormal);
        if (matKind == 0u && reflDir.y < 0.005)
        {
            reflDir = safeNormalize(float3(reflDir.x, 0.005, reflDir.z));
        }

        float3 reflColor = traceSecondaryRadiance(
            hitPos + hitNormal * 0.022,
            reflDir,
            62.0,
            isMidnight,
            verifiedHits);

        float3 transColor = 0.0.xxx;
        float ambientOcc = 1.0;

        if (matKind == 3u)
        {
            // Refractive transmission ray through thin-shell botanical crystal petal (IOR = 1.52)
            // Steps cleanly past the back shell wall (~2-4 cm) so overlapping petals, ivory column,
            // golden stamens, amber core, and Taipei 101 are genuinely refracted through the glass!
            float3 refrDir = refractAirToGlass(rd, hitNormal, 1.0 / 1.52);
            transColor = traceSecondaryRadiance(
                hitPos + refrDir * 0.085,
                refrDir,
                56.0,
                isMidnight,
                verifiedHits);
        }
        else if (matKind != 4u)
        {
            // Fast hardware inline contact-AO / crevice occlusion ray
            float3 aoDir = safeNormalize(hitNormal + float3(0.0, 0.90, -0.15));
            RayDesc aoRay;
            aoRay.Origin    = hitPos + hitNormal * 0.025;
            aoRay.Direction = aoDir;
            aoRay.TMin      = 0.02;
            aoRay.TMax      = 2.35;

            RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rqAo;
            rqAo.TraceRayInline(gSceneTlas, RAY_FLAG_NONE, 0xFFu, aoRay);
            while (rqAo.Proceed())
            {
            }
            if (rqAo.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            {
                ambientOcc = 0.44 + 0.56 * saturate(rqAo.CommittedRayT() / 2.35);
            }
        }

        hdrColor = shadeSurfaceMaterial(
            matKind,
            subIdx,
            surfUv,
            hitPos,
            hitNormal,
            -rd,
            reflColor,
            transColor,
            ambientOcc,
            gFilmTime,
            gRevealOpen01,
            gEmissivePulse,
            isMidnight);
    }
    else
    {
        hdrColor = evalTaipeiSkyAndBeams(gCamEye, rd, gFilmTime, gRevealOpen01, gEmissivePulse, isMidnight);
    }

    // Subsampled atomic hit verification counter so GPU overhead is near-zero while proving live ray hits
    if (((dtid.x & 15u) == 0u) && ((dtid.y & 15u) == 0u) && verifiedHits > 0u)
    {
        InterlockedAdd(gVerifyOut[1], verifiedHits);
    }

    gOutHdr[dtid.xy] = float4(max(hdrColor, 0.0.xxx), 1.0);
    gOutDepth[dtid.xy] = primaryRayT;
}
#endif

// =============================================================================
// PASS 1B: Matching SM 6.0 Raster / Analytic Substitute Taipei Scene (Non-DXR)
// =============================================================================
#if defined(BOO_M1_RASTER_PS)
cbuffer M1SceneCB : register(b0)
{
    float3 gCamEye;
    float  gFovY;
    float3 gCamRight;
    float  gAspect;
    float3 gCamUp;
    float  gFilmTime;
    float3 gCamForward;
    float  gRevealOpen01;
    uint   gCanvasWidth;
    uint   gCanvasHeight;
    uint   gExpectSeq;
    uint   gSceneFlags;
    float  gEmissivePulse;
    float  gMoodIntensity;
    uint   gShotIndex;
    uint   gDeterministicSeed;
    uint4  gMeshBaseIndices;
};

struct PSRasterOut
{
    float4 hdrColor   : SV_Target0;
    float  linearRayT : SV_Target1;
};

struct SdfHit
{
    float d;
    uint  matKind;
    uint  subIdx;
    float2 uv;
};

float sdRoundBox(float3 p, float3 b, float r)
{
    float3 q = abs(p) - (b - r.xxx);
    return length(max(q, 0.0.xxx)) + min(max(q.x, max(q.y, q.z)), 0.0) - r;
}

float sdCapsuleSeg(float3 p, float3 a, float3 b, float r0, float r1, out float outV)
{
    float3 pa = p - a;
    float3 ba = b - a;
    outV = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-6));
    float rad = lerp(r0, r1, outV);
    return length(pa - ba * outV) - rad;
}

// Sculpted curved botanical petal blade SDF in local frame:
// local +Y = claw (0) -> apex (len), local X = blade width, local +Z = adaxial (front) face normal
float sdBotanicalPetal(float3 localP, float len, float widthScale, bool isLabellum, out float2 outUv)
{
    float v = clamp(localP.y / max(len, 1e-3), 0.0, 1.0);
    float sinPiV = sin(v * kPi);
    float halfW;
    float zCurve;

    if (isLabellum)
    {
        float throat = 0.24 + 0.34 * sinPiV;
        float flare  = 0.48 * smoothstep(0.38, 0.80, v) * (1.0 - 0.55 * smoothstep(0.86, 1.0, v));
        halfW = (throat + flare) * widthScale * len * 0.42;
        float uNormPre = clamp(localP.x / max(halfW, 1e-3), -1.0, 1.0);
        // Upcurved lateral throat lobes + forward flared lip
        float throatCup = (1.0 - smoothstep(0.45, 0.78, v)) * (uNormPre * uNormPre) * 0.26 * len;
        float lipDrop   = -0.18 * smoothstep(0.50, 1.0, v) * len;
        zCurve = 0.08 * sinPiV * len + throatCup + lipDrop;
    }
    else
    {
        float clawPinch = smoothstep(0.0, 0.24, v);
        float env = pow(max(sin(kPi * pow(max(v, 1e-4), 0.92)), 0.0), 0.88);
        float tipTaper = 1.0 - 0.94 * smoothstep(0.86, 1.0, v);
        halfW = (0.022 + 0.295 * env * (0.28 + 0.72 * clawPinch)) * tipTaper * widthScale * len;
        float uNormPre = clamp(localP.x / max(halfW, 1e-3), -1.0, 1.0);
        zCurve = (0.13 * sin(v * kPi * 0.92) - 0.10 * v * v * v + 0.085 * uNormPre * uNormPre * sinPiV) * len;
    }

    float uSigned = clamp(localP.x / max(halfW, 1e-3), -1.0, 1.0);
    outUv = float2(uSigned * 0.5 + 0.5, v);

    float dx = max(abs(localP.x) - halfW, 0.0);
    float dy = (localP.y < 0.0) ? -localP.y : max(localP.y - len, 0.0);
    float dz = abs(localP.z - zCurve);
    float halfThick = (0.024 * (1.0 - 0.55 * v) + 0.007) * (1.0 - 0.55 * uSigned * uSigned) * len;
    return (sqrt(dx * dx + dy * dy + dz * dz) - halfThick) * 0.80;
}

// Transforms point rel to flower hub into petal local space:
// rollAngle rotates in XY around -Z flower forward; openAngle tilts radial +Y away from -Z toward XY plane.
float3 toPetalFrame(float3 rel, float rollRad, float openRad)
{
    float cr = cos(rollRad);
    float sr = sin(rollRad);
    // Radial direction in XY plane: (cos(roll), sin(roll), 0); lateral tangent: (-sin(roll), cos(roll), 0)
    float3 latAxis = float3(-sr, cr, 0.0);
    float3 radXY   = float3(cr, sr, 0.0);
    float3 fwdAxis = float3(0.0, 0.0, -1.0); // Toward camera

    float co = cos(openRad);
    float so = sin(openRad);
    float3 petalY = fwdAxis * co + radXY * so;   // Claw -> Tip
    float3 petalZ = fwdAxis * so - radXY * co;   // Adaxial front normal facing camera (-Z) when openRad -> 75 deg
    return float3(dot(rel, latAxis), dot(rel, petalY), dot(rel, petalZ));
}

SdfHit evalElevatedSdf(float3 p)
{
    SdfHit best;
    best.d = 1e5;
    best.matKind = 1u;
    best.subIdx = 0u;
    best.uv = float2(0.5, 0.5);

    // 1. Boulevard colonnade pillars & framed Taipei signage (evaluated unconditionally so rays never overstep!)
    {
        float cellZ = clamp(round(p.z / 4.0) * 4.0, -12.0, 20.0);
        float3 colP = float3(abs(p.x) - 4.5, p.y - 3.2, p.z - cellZ);
        float dPillar = sdRoundBox(colP, float3(0.48, 3.2, 0.48), 0.035);
        if (dPillar < best.d)
        {
            best.d = dPillar;
            best.matKind = 1u;
            best.subIdx = 0u;
            best.uv = saturate(colP.zy / float2(0.96, 6.4) + 0.5);
        }

        float3 signP = float3(abs(p.x) - 3.92, p.y - 2.6, p.z - cellZ);
        float dSign = sdRoundBox(signP, float3(0.08, 1.35, 0.28), 0.015);
        if (dSign < best.d)
        {
            best.d = dSign;
            best.matKind = 2u;
            best.subIdx = 0u;
            best.uv = saturate(float2(signP.z / 0.56 + 0.5, signP.y / 2.70 + 0.5));
        }
    }

    // 2. Twin colossal chamfered black-enamel monoliths parting via gRevealOpen01
    float monolithSpread = 2.35 + gRevealOpen01 * 1.45;
    float3 monoP = float3(abs(p.x) - monolithSpread, p.y - 7.5, p.z - 5.5);
    float dMono = sdRoundBox(monoP, float3(1.15, 7.5, 2.2), 0.06);
    if (dMono < best.d)
    {
        best.d = dMono;
        best.matKind = 1u;
        best.subIdx = 1u;
        best.uv = saturate(float2((monoP.x + 1.15) / 2.30, (monoP.y + 7.5) / 15.0));
    }

    // 3. Sleek dark-titanium vault louvers (retracting cleanly inside monolith flanks before t=43s!)
    float smoothOpen = gRevealOpen01 * gRevealOpen01 * (3.0 - 2.0 * gRevealOpen01);
    float louverRetract = smoothstep(0.0, 0.72, gRevealOpen01);
    float louverX = 0.62 + louverRetract * 2.95;
    {
        float louverRow = clamp(round((p.y - 5.0) / 1.1) * 1.1 + 5.0, 5.0, 10.5);
        float3 lvP = float3(abs(p.x) - louverX, p.y - louverRow, p.z - 3.8);
        float dLouver = sdRoundBox(lvP, float3(0.64, 0.13, 0.20), 0.018);
        if (dLouver < best.d)
        {
            best.d = dLouver;
            best.matKind = 6u;
            best.subIdx = 1u;
            best.uv = saturate(float2((lvP.x + 0.64) / 1.28, (lvP.y + 0.13) / 0.26));
        }
    }

    // 4. Grounded Sanctuary Altar Plinth + Brass Collar + S-Curved Botanical Pedicel Stem
    {
        float3 plinthP = p - float3(0.0, 1.05, 6.28);
        float dPlinth = sdRoundBox(plinthP, float3(0.56, 1.05, 0.56), 0.045);
        if (dPlinth < best.d)
        {
            best.d = dPlinth;
            best.matKind = 1u;
            best.subIdx = 1u;
            best.uv = saturate(float2(plinthP.x / 1.12 + 0.5, plinthP.y / 2.10 + 0.5));
        }

        float3 collarP = p - float3(0.0, 2.16, 6.28);
        float dCollar = sdRoundBox(collarP, float3(0.38, 0.09, 0.38), 0.025);
        if (dCollar < best.d)
        {
            best.d = dCollar;
            best.matKind = 6u;
            best.subIdx = 0u;
            best.uv = float2(0.5, 0.2);
        }

        float vS0;
        float dS0 = sdCapsuleSeg(p, float3(0.0, 2.18, 6.28), float3(-0.12, 4.25, 6.18), 0.095, 0.080, vS0);
        if (dS0 < best.d) { best.d = dS0; best.matKind = 6u; best.subIdx = 0u; best.uv = float2(0.5, vS0 * 0.33); }

        float vS1;
        float dS1 = sdCapsuleSeg(p, float3(-0.12, 4.25, 6.18), float3(0.08, 6.18, 5.92), 0.080, 0.072, vS1);
        if (dS1 < best.d) { best.d = dS1; best.matKind = 6u; best.subIdx = 0u; best.uv = float2(0.5, 0.33 + vS1 * 0.33); }

        float vS2;
        float dS2 = sdCapsuleSeg(p, float3(0.08, 6.18, 5.92), float3(0.0, 7.68, 5.58), 0.072, 0.135, vS2);
        if (dS2 < best.d) { best.d = dS2; best.matKind = 6u; best.subIdx = 0u; best.uv = float2(0.5, 0.66 + vS2 * 0.34); }
    }

    // 5. Separated 5-Petal + 3-Lobed Labellum Zygomorphic Taiwan Glass Orchid at (0, 7.80, 5.50)
    float3 hub = float3(0.0, 7.80, 5.50);
    float3 rel = p - hub;
    float orcBound = length(rel);
    if (orcBound < 2.75)
    {
        float breath = 0.025 * sin(gFilmTime * 0.85);
        float unfurl = smoothOpen;

        // Central Porcelain-Ivory Gynostemium Column (matKind = 5)
        float vCol;
        float dCol = sdCapsuleSeg(rel, float3(0.0, 0.02, 0.05), float3(0.0, 0.11, -0.56), 0.125, 0.092, vCol);
        if (dCol < best.d) { best.d = dCol; best.matKind = 5u; best.subIdx = 0u; best.uv = float2(0.5, vCol); }

        // Molten Amber-Ruby Pollinia Nectar Core (matKind = 4)
        float dCore = length((rel - float3(0.0, -0.04, -0.40)) * float3(1.15, 1.30, 0.95)) - 0.125;
        if (dCore < best.d) { best.d = dCore; best.matKind = 4u; best.subIdx = 0u; best.uv = float2(0.5, 0.5); }

        float3 symRel = float3(abs(rel.x), rel.y, rel.z);
        float2 pUv;

        // Posterior Whorl: Tall Emerald-Jade Dorsal Sepal (12h) & Swept Lateral Sepals (7:30 & 4:30)
        float openDorsal = lerp(0.24, 1.34, unfurl) + breath;
        float dDorsal = sdBotanicalPetal(toPetalFrame(rel - float3(0.0, 0.04, 0.14), 1.5707963, openDorsal), lerp(1.05, 2.02, unfurl), 0.92, false, pUv);
        if (dDorsal < best.d) { best.d = dDorsal; best.matKind = 3u; best.subIdx = 0u; best.uv = pUv; }

        float openLower = lerp(0.25, 1.36, unfurl) + breath * 0.7;
        float dLower = sdBotanicalPetal(toPetalFrame(symRel - float3(0.04, -0.04, 0.14), -0.907571, openLower), lerp(1.00, 1.92, unfurl), 0.86, false, pUv);
        if (dLower < best.d) { best.d = dLower; best.matKind = 3u; best.subIdx = 3u; best.uv = pUv; }

        // Anterior Whorl: Broad Orchid-Amethyst Butterfly Wing Petals (9:45 & 2:15 — crisp open notches above & below!)
        float openWing = lerp(0.26, 1.16, unfurl) - breath;
        float dWing = sdBotanicalPetal(toPetalFrame(symRel - float3(0.05, 0.02, -0.06), 0.279253, openWing), lerp(1.10, 2.05, unfurl), 1.28, false, pUv);
        if (dWing < best.d) { best.d = dWing; best.matKind = 3u; best.subIdx = 1u; best.uv = pUv; }

        // Central 3-Lobed Cupped Labellum Lip (6h, crimson-ruby & nectar-gold throat)
        float openLip = lerp(0.20, 0.92, unfurl);
        float dLip = sdBotanicalPetal(toPetalFrame(rel - float3(0.0, -0.07, -0.14), -1.5707963, openLip), lerp(0.86, 1.48, unfurl), 1.10, true, pUv);
        if (dLip < best.d) { best.d = dLip; best.matKind = 3u; best.subIdx = 5u; best.uv = pUv; }

        // Compact Inner Throat Cristae Petals (nestled tightly around column without blocking outer petal clefts)
        float openInner = lerp(0.18, 0.72, unfurl);
        float dInner0 = sdBotanicalPetal(toPetalFrame(symRel - float3(0.0, 0.0, -0.06), 0.942478, openInner), lerp(0.52, 0.82, unfurl), 0.74, false, pUv);
        if (dInner0 < best.d) { best.d = dInner0; best.matKind = 3u; best.subIdx = 6u; best.uv = pUv; }

        // Delicate 24k Gold Stamen Filaments radiating from throat
        float vStam;
        float3 stamEnd = float3(0.26 * unfurl + 0.05, -0.10 * unfurl, -0.58 * unfurl - 0.18);
        float dStam = sdCapsuleSeg(symRel, float3(0.04, -0.04, -0.14), stamEnd, 0.022, 0.014, vStam);
        if (dStam < best.d) { best.d = dStam; best.matKind = 6u; best.subIdx = 2u; best.uv = float2(0.5, vStam); }
    }
    else
    {
        best.d = min(best.d, orcBound - 2.45);
    }

    // 6. Architecturally Accurate Taipei 101 at (0, 0..28.8, 37.5):
    //    Inward-sloped notched podium + 8 outward-canted 7-deg bamboo modules + 660-ton 24k-Gold TMD + 508m spire!
    float3 tpRel = p - float3(0.0, 14.5, 37.5);
    float dTpBound = sdRoundBox(tpRel, float3(3.10, 15.0, 3.10), 0.05);
    if (dTpBound < 0.85)
    {
        float2 xz = float2(p.x, p.z - 37.5);
        float2 axz = abs(xz);

        // Podium Base (1F-25F, y = 0.0..5.2): inward-sloped truncated pyramid with notched corners
        if (p.y < 5.6)
        {
            float vPod = saturate(p.y / 5.2);
            float wPod = lerp(2.48, 1.84, vPod);
            float notchP = wPod * 0.22;
            float dSqP = length(max(axz - wPod.xx, 0.0.xx)) + min(max(axz.x - wPod, axz.y - wPod), 0.0);
            float dCornerCutP = min(axz.x - (wPod - notchP), axz.y - (wPod - notchP));
            float d2dP = max(dSqP, dCornerCutP);
            float dyP = max(-p.y, p.y - 5.2);
            float dPod = (length(max(float2(d2dP, dyP), 0.0.xx)) + min(max(d2dP, dyP), 0.0)) * 0.85;
            if (dPod < best.d)
            {
                best.d = dPod;
                best.matKind = 7u;
                best.subIdx = 0u;
                best.uv = float2(saturate(xz.x / max(2.0 * wPod, 1e-3) + 0.5), vPod);
            }
        }

        // 8 Stacked 7-deg Outward-Canted Double-Notched Ruyi / Bamboo Modules (26F-89F, y = 5.2..21.2)
        if (p.y >= 4.8 && p.y <= 21.6)
        {
            float tierIdx = clamp(floor((p.y - 5.2) / 2.0), 0.0, 7.0);
            float yBase = 5.2 + tierIdx * 2.0;
            float vTier = saturate((p.y - yBase) / 2.0);
            float tierTaper = 1.0 - 0.018 * tierIdx;
            float wTier = lerp(1.68, 1.98, vTier) * tierTaper;
            float notchT = wTier * 0.24;
            float dSqT = length(max(axz - wTier.xx, 0.0.xx)) + min(max(axz.x - wTier, axz.y - wTier), 0.0);
            float dCornerCutT = min(axz.x - (wTier - notchT), axz.y - (wTier - notchT));
            float d2dT = max(dSqT, dCornerCutT);
            // South-facing observatory sanctuary window recess on top module (tier 7) revealing the Golden TMD!
            if (tierIdx > 6.5)
            {
                float dWindowCut = max(0.95 - abs(xz.x), (-wTier + 0.65) - xz.y);
                d2dT = max(d2dT, dWindowCut);
            }
            float dyT = max(yBase - p.y, p.y - (yBase + 2.0));
            float dTier = (length(max(float2(d2dT, dyT), 0.0.xx)) + min(max(d2dT, dyT), 0.0)) * 0.82;
            if (dTier < best.d)
            {
                best.d = dTier;
                best.matKind = 7u;
                best.subIdx = (tierIdx > 6.5) ? 1u : 0u;
                best.uv = float2(saturate(xz.x / max(2.0 * wTier, 1e-3) + 0.5), vTier);
            }
        }

        // Suspended 660-Metric-Ton 24k-Gold Tuned Mass Damper (TMD) Sphere inside 87F-92F Sanctuary
        float3 tmdRel = p - float3(0.0, 20.25, 36.15);
        float dTmd = length(tmdRel) - 0.58;
        if (dTmd < best.d)
        {
            best.d = dTmd;
            best.matKind = 6u;
            best.subIdx = 3u;
            best.uv = float2(0.5, saturate(tmdRel.y / 1.16 + 0.5));
        }

        // Observatory Crown + Stepped Spire + 508m Pinnacle Mast (y = 21.2..28.8)
        if (p.y > 20.8)
        {
            float dCrown = sdRoundBox(p - float3(0.0, 22.0, 37.5), float3(1.12, 0.82, 1.12), 0.04);
            if (dCrown < best.d)
            {
                best.d = dCrown;
                best.matKind = 7u;
                best.subIdx = 1u;
                best.uv = float2(saturate(xz.x / 2.24 + 0.5), saturate((p.y - 21.18) / 1.64));
            }
            float dSpireBase = sdRoundBox(p - float3(0.0, 23.65, 37.5), float3(0.56, 0.85, 0.56), 0.03);
            if (dSpireBase < best.d)
            {
                best.d = dSpireBase;
                best.matKind = 6u;
                best.subIdx = 0u;
                best.uv = float2(0.5, saturate((p.y - 22.8) / 1.7));
            }
            float vMast;
            float dMast = sdCapsuleSeg(p, float3(0.0, 24.4, 37.5), float3(0.0, 28.6, 37.5), 0.16, 0.045, vMast);
            if (dMast < best.d)
            {
                best.d = dMast;
                best.matKind = (vMast > 0.88) ? 4u : 6u;
                best.subIdx = 2u;
                best.uv = float2(0.5, vMast);
            }
        }
    }
    else
    {
        best.d = min(best.d, dTpBound);
    }

    return best;
}

float3 evalElevatedSdfNormal(float3 p)
{
    const float eps = 0.0045;
    float2 e = float2(1.0, -1.0) * eps;
    return safeNormalize(
        e.xyy * evalElevatedSdf(p + e.xyy).d +
        e.yyx * evalElevatedSdf(p + e.yyx).d +
        e.yxy * evalElevatedSdf(p + e.yxy).d +
        e.xxx * evalElevatedSdf(p + e.xxx).d);
}

// Honest analytic planar wet-road reflection substitute (zero BVH rays)
float3 evalAnalyticRoadReflection(float3 hitPos, float3 reflDir, bool isMidnight)
{
    float3 env = evalTaipeiSkyAndBeams(hitPos, reflDir, gFilmTime, gRevealOpen01, gEmissivePulse, isMidnight);

    // Analytic silhouette occlusion & reflection of twin monoliths + colonnade neon light-boxes
    if (reflDir.y > 0.002)
    {
        // Intersect plane x = +-3.92 for reflected boulevard signage glow
        float signTargetX = (reflDir.x >= 0.0) ? 3.92 : -3.92;
        float dx = signTargetX - hitPos.x;
        if (dx * reflDir.x > 0.0)
        {
            float tSign = dx / reflDir.x;
            float3 sp = hitPos + reflDir * tSign;
            if (tSign > 0.2 && tSign < 36.0 && sp.z > -13.0 && sp.z < 21.0 && sp.y > 1.1 && sp.y < 4.1)
            {
                float cellZ = clamp(round(sp.z / 4.0) * 4.0, -12.0, 20.0);
                if (abs(sp.z - cellZ) < 0.34)
                {
                    float palettePick = frac(abs(cellZ) * 0.173 + (signTargetX > 0.0 ? 0.37 : 0.0));
                    float3 colJade   = float3(0.05, 0.88, 0.74);
                    float3 colOrchid = isMidnight ? float3(0.94, 0.18, 0.76) : float3(0.86, 0.24, 0.90);
                    float3 colAmber  = float3(1.00, 0.64, 0.14);
                    float3 signHue = (palettePick < 0.38) ? colJade : ((palettePick < 0.72) ? colAmber : colOrchid);
                    env += signHue * 0.65 * exp(-0.04 * tSign);
                }
            }
        }

        // Intersect plane z = 3.3 for twin monolith dark reflection silhouette + z = 36.0 for Taipei 101 reflection
        if (reflDir.z > 0.01)
        {
            float tMono = (3.3 - hitPos.z) / reflDir.z;
            float3 mp = hitPos + reflDir * tMono;
            float monoSpread = 2.35 + gRevealOpen01 * 1.45;
            if (tMono > 0.2 && mp.y > 0.0 && mp.y < 15.0 && abs(abs(mp.x) - monoSpread) < 1.15)
            {
                env *= 0.18;
            }
            else
            {
                float tTp = (36.0 - hitPos.z) / reflDir.z;
                float3 tp = hitPos + reflDir * tTp;
                if (tTp > 2.0 && tp.y > 0.0 && tp.y < 28.5 && abs(tp.x) < (tp.y < 21.5 ? 2.0 : 0.45))
                {
                    env = lerp(env, float3(0.03, 0.22, 0.20), 0.65);
                    if (length(tp.xy - float2(0.0, 20.25)) < 0.75)
                    {
                        env += float3(1.0, 0.72, 0.22) * 0.55;
                    }
                }
            }
        }
    }
    return env;
}

PSRasterOut PSRasterTaipeiScene(VSOut pin)
{
    const bool isMidnight = (gSceneFlags & 1u) != 0u;
    float2 ndc = pin.uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float tanHalfFov = tan(gFovY * 0.5);
    float3 rd = safeNormalize(gCamForward + gCamRight * (ndc.x * gAspect * tanHalfFov) + gCamUp * (ndc.y * tanHalfFov));

    // Exact analytical y = 0 wet asphalt intersection — completely eliminates grazing sphere-march holes!
    float tRoad = (rd.y < -1e-4 && gCamEye.y > 0.0) ? (-gCamEye.y / rd.y) : 1e6;
    if (tRoad < 0.02 || tRoad > 70.0)
    {
        tRoad = 1e6;
    }

    float maxMarchT = min(tRoad, 68.0);
    float t = 0.04;
    bool hitElevated = false;
    SdfHit hitInfo;
    hitInfo.d = 1e5;
    hitInfo.matKind = 0u;
    hitInfo.subIdx = 0u;
    hitInfo.uv = float2(0.5, 0.5);

    [loop]
    for (uint stepIdx = 0u; stepIdx < 92u; ++stepIdx)
    {
        if (t >= maxMarchT)
            break;
        float3 p = gCamEye + rd * t;
        SdfHit cur = evalElevatedSdf(p);
        if (cur.d < 0.0045)
        {
            hitElevated = true;
            hitInfo = cur;
            break;
        }
        t += max(cur.d * 0.86, 0.006);
    }

    float3 hdrColor = 0.0.xxx;
    float outRayT = 1000.0;
    if (hitElevated || tRoad < 65.0)
    {
        float finalT = hitElevated ? t : tRoad;
        outRayT = finalT;
        uint matKind = hitElevated ? hitInfo.matKind : 0u;
        uint subIdx  = hitElevated ? hitInfo.subIdx  : 0u;
        float2 surfUv = hitElevated ? hitInfo.uv     : float2(0.5, 0.5);
        float3 hitPos = gCamEye + rd * finalT;

        float3 hitNormal = (matKind == 0u) ? evalWetRainRippleNormal(hitPos, gFilmTime) : evalElevatedSdfNormal(hitPos);
        if (dot(hitNormal, -rd) < 0.0)
        {
            hitNormal = -hitNormal;
        }

        float3 reflDir = reflect(rd, hitNormal);
        if (matKind == 0u && reflDir.y < 0.005)
        {
            reflDir = safeNormalize(float3(reflDir.x, 0.005, reflDir.z));
        }

        float3 analyticRefl = (matKind == 0u)
            ? evalAnalyticRoadReflection(hitPos, reflDir, isMidnight)
            : evalTaipeiSkyAndBeams(hitPos + hitNormal * 0.04, reflDir, gFilmTime, gRevealOpen01, gEmissivePulse, isMidnight);

        float3 analyticTrans = 0.0.xxx;
        float ao = 1.0;
        if (matKind == 3u)
        {
            // Analytic glass refraction substitute: sample sanctuary backdrop + inner amber core glow behind petal
            float3 refrDir = safeNormalize(lerp(rd, -hitNormal, 0.24));
            analyticTrans = evalTaipeiSkyAndBeams(hitPos + refrDir * 0.25, refrDir, gFilmTime, gRevealOpen01, gEmissivePulse, isMidnight);
            float3 coreDelta = float3(0.0, 7.78, 5.10) - hitPos;
            float coreAlign = pow(saturate(dot(refrDir, safeNormalize(coreDelta))), 6.0);
            analyticTrans += float3(1.0, 0.58, 0.16) * coreAlign * (0.65 * gRevealOpen01);
        }
        else if (matKind != 4u)
        {
            // Fast 2-tap SDF ambient occlusion
            float d1 = evalElevatedSdf(hitPos + hitNormal * 0.18).d;
            float d2 = evalElevatedSdf(hitPos + hitNormal * 0.55).d;
            float occ = saturate((0.18 - d1) * 2.2) + saturate((0.55 - d2) * 0.85);
            ao = saturate(1.0 - 0.52 * occ);
        }

        hdrColor = shadeSurfaceMaterial(
            matKind,
            subIdx,
            surfUv,
            hitPos,
            hitNormal,
            -rd,
            analyticRefl,
            analyticTrans,
            ao,
            gFilmTime,
            gRevealOpen01,
            gEmissivePulse,
            isMidnight);
    }
    else
    {
        hdrColor = evalTaipeiSkyAndBeams(gCamEye, rd, gFilmTime, gRevealOpen01, gEmissivePulse, isMidnight);
    }

    PSRasterOut pout;
    pout.hdrColor   = float4(max(hdrColor, 0.0.xxx), 1.0);
    pout.linearRayT = outRayT;
    return pout;
}
#endif

// =============================================================================
// PASS 2A: Dual-Kawase HDR Bloom Downsample + Firefly Clamp (SM 6.0)
// =============================================================================
#if defined(BOO_M1_BLOOM_DOWN_PS)
cbuffer M1PostCB : register(b0)
{
    float2 gSrcTexelSize;
    float  gBloomThreshold;
    float  gBloomRadiusScale;
    uint   gIsFirstDownsample;
    uint   gDitherSeed;
    float  gPreExposure;
    float  gBloomIntensity;
};

Texture2D<float4> gSrcTex : register(t0);
SamplerState      gLinearClamp : register(s0);

float3 softThresholdAndClamp(float3 c, float threshold)
{
    c = min(max(c, 0.0.xxx), 8.0.xxx); // Firefly clamp <= 8.0 HDR units
    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - threshold + 0.40, 0.0, 0.80);
    soft = (soft * soft) / (4.0 * 0.40 + 1e-5);
    float contrib = max(soft, br - threshold) / max(br, 1e-4);
    return c * saturate(contrib);
}

float4 PSBloomDownsample(VSOut pin) : SV_Target
{
    float2 d = gSrcTexelSize * gBloomRadiusScale;
    float3 c0 = gSrcTex.SampleLevel(gLinearClamp, pin.uv, 0.0).rgb;
    float3 c1 = gSrcTex.SampleLevel(gLinearClamp, pin.uv + float2(-d.x, -d.y), 0.0).rgb;
    float3 c2 = gSrcTex.SampleLevel(gLinearClamp, pin.uv + float2( d.x, -d.y), 0.0).rgb;
    float3 c3 = gSrcTex.SampleLevel(gLinearClamp, pin.uv + float2(-d.x,  d.y), 0.0).rgb;
    float3 c4 = gSrcTex.SampleLevel(gLinearClamp, pin.uv + float2( d.x,  d.y), 0.0).rgb;

    if (gIsFirstDownsample != 0u)
    {
        c0 = softThresholdAndClamp(c0, gBloomThreshold);
        c1 = softThresholdAndClamp(c1, gBloomThreshold);
        c2 = softThresholdAndClamp(c2, gBloomThreshold);
        c3 = softThresholdAndClamp(c3, gBloomThreshold);
        c4 = softThresholdAndClamp(c4, gBloomThreshold);
    }

    float3 filtered = c0 * 0.50 + (c1 + c2 + c3 + c4) * 0.125;
    return float4(filtered, 1.0);
}
#endif

// =============================================================================
// PASS 2B: Dual-Kawase 9-Tap Tent HDR Bloom Upsample (Additive Blend)
// =============================================================================
#if defined(BOO_M1_BLOOM_UP_PS)
cbuffer M1PostCB : register(b0)
{
    float2 gSrcTexelSize;
    float  gBloomThreshold;
    float  gBloomRadiusScale;
    uint   gIsFirstDownsample;
    uint   gDitherSeed;
    float  gPreExposure;
    float  gBloomIntensity;
};

Texture2D<float4> gSrcTex : register(t0);
SamplerState      gLinearClamp : register(s0);

float4 PSBloomUpsample(VSOut pin) : SV_Target
{
    float2 d = gSrcTexelSize * gBloomRadiusScale;
    float3 s = 0.0.xxx;
    s += gSrcTex.SampleLevel(gLinearClamp, pin.uv + float2(-d.x, -d.y), 0.0).rgb * 1.0;
    s += gSrcTex.SampleLevel(gLinearClamp, pin.uv + float2( 0.0, -d.y), 0.0).rgb * 2.0;
    s += gSrcTex.SampleLevel(gLinearClamp, pin.uv + float2( d.x, -d.y), 0.0).rgb * 1.0;
    s += gSrcTex.SampleLevel(gLinearClamp, pin.uv + float2(-d.x,  0.0), 0.0).rgb * 2.0;
    s += gSrcTex.SampleLevel(gLinearClamp, pin.uv,                      0.0).rgb * 4.0;
    s += gSrcTex.SampleLevel(gLinearClamp, pin.uv + float2( d.x,  0.0), 0.0).rgb * 2.0;
    s += gSrcTex.SampleLevel(gLinearClamp, pin.uv + float2(-d.x,  d.y), 0.0).rgb * 1.0;
    s += gSrcTex.SampleLevel(gLinearClamp, pin.uv + float2( 0.0,  d.y), 0.0).rgb * 2.0;
    s += gSrcTex.SampleLevel(gLinearClamp, pin.uv + float2( d.x,  d.y), 0.0).rgb * 1.0;
    return float4((s / 16.0) * gBloomIntensity, 1.0);
}
#endif

// =============================================================================
// PASS 3: Clean 16:9 Canvas Composite — Cinematic Lens Model
//   gIsFirstDownsample bits 0..7 = AOV / debug mode (0 = film), bits 8..9 = lens flags:
//   bit8 = autofocus bokeh DoF (scene distance valid), bit9 = lens ghosts + halo (off under reduced flash).
//   Film mode adds: 24-tap golden-angle foreground-aware bokeh DoF, lateral chromatic aberration,
//   four-octave anamorphic streaks, pseudo lens ghosts + chromatic halo, halation and film grain.
//   The live beat envelope (gAudioKick) gently pumps bloom, streaks and chromatic fringing.
//   Every stochastic term is keyed on (pixel, gDitherSeed) so seeks reproduce identical frames.
// =============================================================================
#if defined(BOO_M1_COMPOSITE_PS)
cbuffer M1PostCB : register(b0)
{
    float2 gCanvasInvSize;
    float  gAudioKick;      // live beat envelope [0,1] (0 in AOV, reduced-flash and deterministic contexts)
    float  gVignetteStrength;
    uint   gIsFirstDownsample;
    uint   gDitherSeed;
    float  gPreExposure;
    float  gBloomIntensity;
};

Texture2D<float>  gSceneDistTex : register(t0);
Texture2D<float4> gSceneHdrTex  : register(t1);
Texture2D<float4> gBloomMip0    : register(t2);
SamplerState      gLinearClamp  : register(s0);

int2 canvasSizePx()
{
    return int2(round(1.0 / gCanvasInvSize));
}

float loadDistance(int2 p, int2 size)
{
    return gSceneDistTex.Load(int3(clamp(p, int2(0, 0), size - 1), 0));
}

float3 loadHdr(int2 p, int2 size)
{
    return min(gSceneHdrTex.Load(int3(clamp(p, int2(0, 0), size - 1), 0)).rgb, 64.0.xxx);
}

float sampleInvDistanceBilinear(float2 uv, int2 size)
{
    float2 fp = uv * (float2)size - 0.5;
    int2 ip = int2(floor(fp));
    float2 f = fp - (float2)ip;
    float i00 = 1.0 / max(loadDistance(ip, size), 0.05);
    float i10 = 1.0 / max(loadDistance(ip + int2(1, 0), size), 0.05);
    float i01 = 1.0 / max(loadDistance(ip + int2(0, 1), size), 0.05);
    float i11 = 1.0 / max(loadDistance(ip + int2(1, 1), size), 0.05);
    return lerp(lerp(i00, i10, f.x), lerp(i01, i11, f.x), f.y);
}

// Weighted harmonic mean of thirteen sub-pixel bilinear probes: biases focus smoothly toward the nearest
// central subject without single-pixel silhouette step jumps as stems or petal edges cross a probe.
float autofocusDistance(int2 size)
{
    static const float2 kOffsets[13] = {
        float2( 0.000,  0.000),
        float2( 0.038,  0.000), float2(-0.038,  0.000), float2( 0.000,  0.038), float2( 0.000, -0.038),
        float2( 0.042,  0.042), float2(-0.042,  0.042), float2( 0.042, -0.042), float2(-0.042, -0.042),
        float2( 0.085,  0.000), float2(-0.085,  0.000), float2( 0.000,  0.085), float2( 0.000, -0.085)
    };
    static const float kWeights[13] = {
        1.50,
        1.10, 1.10, 1.10, 1.10,
        0.85, 0.85, 0.85, 0.85,
        0.65, 0.65, 0.65, 0.65
    };
    float invSum = 0.0;
    float wSum = 0.0;
    [unroll] for (int k = 0; k < 13; ++k)
    {
        float2 uv = float2(0.5, 0.53) + kOffsets[k] * float2(0.5625, 1.0);
        invSum += sampleInvDistanceBilinear(uv, size) * kWeights[k];
        wSum += kWeights[k];
    }
    return wSum / invSum;
}

// Thin-lens circle of confusion in canvas pixels: |1 - f/d| / f, scaled to a 1440p reference frame.
float circleOfConfusionPx(float d, float focus, float scale)
{
    return min(abs(1.0 - focus / max(d, 0.05)) * 34.0 / max(focus, 0.6), 14.0) * scale;
}

float3 bokehDepthOfField(float2 pxPos, int2 size, float3 centerColor, float centerD, float focus, float scale)
{
    float centerCoc = circleOfConfusionPx(centerD, focus, scale);
    float radius = 14.0 * scale;
    float rot = interleavedGradientNoise(pxPos, 0x5BD1E995u) * kTwoPi;
    float3 acc = centerColor;
    float wsum = 1.0;
    [unroll] for (int k = 0; k < 24; ++k)
    {
        float r = sqrt(((float)k + 0.5) / 24.0) * radius;
        float th = (float)k * 2.39996323 + rot;
        int2 tp = int2(pxPos + float2(cos(th), sin(th)) * r);
        float td = loadDistance(tp, size);
        float3 tc = loadHdr(tp, size);
        float tcoc = circleOfConfusionPx(td, focus, scale);
        // Background taps may not smear across a sharper foreground (no halo of the backdrop over the subject).
        float effCoc = (td > centerD) ? min(tcoc, centerCoc) : tcoc;
        float w = saturate(effCoc - r + 1.0);
        float lum = dot(tc, float3(0.2126, 0.7152, 0.0722));
        w *= 1.0 + saturate((lum - 1.0) * 0.6); // bright points bloom into crisp bokeh discs
        acc += tc * w;
        wsum += w;
    }
    return acc / wsum;
}

float4 PSCompositeCanvas(VSOut pin) : SV_Target
{
    uint aovMode = gIsFirstDownsample & 0xFFu;
    uint lensFlags = (gIsFirstDownsample >> 8u) & 3u;
    float2 pxPos = floor(pin.position.xy);
    float3 hdrCenter = gSceneHdrTex.SampleLevel(gLinearClamp, pin.uv, 0.0).rgb;
    float3 sdr;
    if (aovMode != 0u)
    {
        if (aovMode == 1u)
        {
            sdr = linearToSrgbExact(saturate(hdrCenter));
        }
        else if (aovMode == 2u || aovMode == 3u || aovMode >= 7u)
        {
            sdr = saturate(hdrCenter);
        }
        else
        {
            sdr = linearToSrgbExact(tonemapChromaPreservingFilmic(max(hdrCenter, 0.0.xxx) * gPreExposure));
        }
    }
    else
    {
        int2 size = canvasSizePx();
        float scale = (float)size.y / 1440.0;
        float3 hdr = hdrCenter;
        if ((lensFlags & 1u) != 0u)
        {
            float focus = autofocusDistance(size);
            float centerD = loadDistance(int2(pxPos), size);
            hdr = bokehDepthOfField(pxPos, size, min(hdrCenter, 64.0.xxx), centerD, focus, scale);
        }

        float2 centered = (pin.uv - 0.5) * float2(1.7777778, 1.0);
        float r2 = dot(centered, centered);

        // Lateral chromatic aberration: red and blue image planes magnify slightly differently toward the corners.
        float2 caShift = (pin.uv - 0.5) * (0.0022 * r2 * (1.0 + 0.45 * gAudioKick));
        float caR = gSceneHdrTex.SampleLevel(gLinearClamp, pin.uv + caShift, 0.0).r;
        float caB = gSceneHdrTex.SampleLevel(gLinearClamp, pin.uv - caShift, 0.0).b;
        hdr += float3(caR - hdrCenter.r, 0.0, caB - hdrCenter.b);

        float vignette = saturate(1.14 - gVignetteStrength * r2);
        float3 bloom = gBloomMip0.SampleLevel(gLinearClamp, pin.uv, 0.0).rgb;

        // Wide anamorphic streak: four symmetric horizontal tap pairs with falling weights and a cool cast.
        float3 streak = 0.0.xxx;
        [unroll] for (int s = 0; s < 4; ++s)
        {
            float offPx = 12.0 * pow(2.2, (float)s) * scale;
            float wS = 1.0 / (1.0 + (float)s * 1.6);
            float2 o = float2(gCanvasInvSize.x * offPx, 0.0);
            streak += (gBloomMip0.SampleLevel(gLinearClamp, pin.uv + o, 0.0).rgb +
                       gBloomMip0.SampleLevel(gLinearClamp, pin.uv - o, 0.0).rgb) * wS;
        }
        float3 flare = streak * float3(0.42, 0.70, 1.0) * (1.0 + 0.40 * gAudioKick);

        // Pseudo lens ghosts mirrored through the optical centre, plus a thin chromatic halo ring.
        // Gated on compact high-intensity highlights so broad daylight skies do not print bands onto water or shadows.
        if ((lensFlags & 2u) != 0u)
        {
            float2 ghostVec = (0.5 - pin.uv) * 0.46;
            static const float3 kGhostTint[4] = {
                float3(0.90, 0.55, 1.00), float3(0.45, 0.95, 0.85), float3(1.00, 0.80, 0.45), float3(0.55, 0.70, 1.00)
            };
            [unroll] for (int g = 1; g <= 4; ++g)
            {
                float2 guv = pin.uv + ghostVec * (float)g;
                float2 edge = smoothstep(0.0, 0.08, guv) * smoothstep(0.0, 0.08, 1.0 - guv);
                float inBounds = edge.x * edge.y;
                float wG = pow(1.0 - saturate(length(0.5 - guv) / 0.70710678), 8.0) * inBounds;
                float3 gCol = gBloomMip0.SampleLevel(gLinearClamp, saturate(guv), 0.0).rgb;
                float gKnee = saturate((dot(gCol, float3(0.2126, 0.7152, 0.0722)) - 1.15) * 0.65);
                flare += gCol * kGhostTint[g - 1] * (wG * gKnee * 0.24);
            }
            float r = sqrt(r2);
            float wH = pow(1.0 - saturate(abs(r - 0.36) * 9.0), 3.0);
            float2 huv = 1.0 - pin.uv;
            float2 hDisp = (0.5 - huv) * 0.035;
            float3 hCenter = gBloomMip0.SampleLevel(gLinearClamp, huv, 0.0).rgb;
            float hKnee = saturate((dot(hCenter, float3(0.2126, 0.7152, 0.0722)) - 1.15) * 0.65);
            flare += float3(gBloomMip0.SampleLevel(gLinearClamp, saturate(huv + hDisp), 0.0).r,
                            hCenter.g,
                            gBloomMip0.SampleLevel(gLinearClamp, saturate(huv - hDisp), 0.0).b) * (wH * hKnee * 0.16);
        }

        // Film halation: highlights scatter a warm red fringe back through the emulsion base.
        float3 halation = bloom * float3(0.050, 0.012, 0.002);

        float3 combined = (hdr + bloom * gBloomIntensity + flare * 0.012 + halation) * gPreExposure * vignette;
        sdr = linearToSrgbExact(tonemapChromaPreservingFilmic(combined));

        // Luminance-weighted film grain (strongest in the mid-tones, absent in pure black and white).
        float lumS = dot(sdr, float3(0.2126, 0.7152, 0.0722));
        float grain = triangularDither(pxPos * 0.5 + 7.0, gDitherSeed ^ 0x2545F491u);
        sdr += grain * 0.014 * lumS * (1.0 - lumS) * 4.0;
    }

    // Sample-deterministic 10-bit Triangular-PDF IGN dither (zero dependency on presentationFrame!)
    float dither = triangularDither(pxPos, gDitherSeed) / 1023.0;
    sdr = saturate(sdr + dither.xxx);

    return float4(sdr, 1.0);
}
#endif

// =============================================================================
// PASS 4: Swapchain 16:9 Letterbox Catmull-Rom Blit + Multi-Engine A/B + Attribution HUD
// =============================================================================
#if defined(BOO_M1_PRESENT_PS)
cbuffer M1PresentCB : register(b0)
{
    float4 gSwapchainAndCanvas; // xy = swapchain (W,H), zw = canvas (W,H)
    float4 gLetterboxRect;      // xy = origin (x,y), zw = size (w,h) in swapchain pixels
    float  gFilmTime;
    float  gDuration;
    float  gActProgress;
    float  gFps;

    float  gGpuMs;
    float  gP95Ms;
    float  gVramUsedMb;
    float  gVramBudgetMb;

    // bit0:overlay, bit1:audioClk, bit2:muted, bit3:paused, bit4:loop, bit5:midnight,
    // bit6:benchRun, bit7:benchDone, bits8..9:hudMode, bits10..11:abEngine (0=RT,1=CUDA,2=Tensor,3=Codec),
    // bit12:codecViewActive, bit13:codecHwTexValid, bit14:abLeftHwVerified
    uint   gHudFlags;
    uint   gEnginePacked;       // 4-bit nibbles: RT, Tensor, NPU, CUDA, Codec
    uint   gSwapchainBits;      // 10 or 8
    float  gBenchProgress;

    float4 gAudioBands;         // x:rms, y:low, z:mid, w:high
    float4 gBenchPrimary;       // Standalone offscreen `B` sweep: RT MRays/s, CUDA MPart/s, Tensor TFLOPS, NVENC FPS
    float4 gBenchFill;          // Normalized [0..1] standalone benchmark bar fill
    float4 gRecentGpuMs[4];
    float4 gVolumeAndPadding;   // x: volumePercent (<0 hidden)

    // Live-scene per-pillar attribution metrics (0=RT, 1=CUDA, 2=Tensor, 3=Codec):
    //   .x = live execution ms, .y = live throughput, .z = workload scale, .w = aux quality (PSNR dB)
    float4 gLivePillarMetrics[4];
    // .x = packed 4-bit live pillar mode nibbles: 0=LIVE_HW, 1=SUB, 2=OFF, 3=FAULT
    uint4  gLivePillarMeta;

    // Pass-4 HUD-only lower-third captions, end card & LIVE CAMERA indicator (never in CanvasClean!)
    uint4  gCaptionPacked[4];   // Up to 64 ASCII characters (4 bytes per uint)
    float4 gCaptionStyle;       // .x = alpha, .y = charCount, .z = isEndCard, .w = liveCameraActive

    float4 gInteractiveState;   // .x = abSplitRatio, .y = sunElev01, .z = sunAzim01, .w = accumSpp
    uint4  gInteractiveMeta;    // .x: bit0=explore, bit1=sunOvr, bit2=lampsOff, bit3=nrd, bits4..7=preset, bit8=accum
};

Texture2D<float4> gCanvasCleanTex     : register(t0);
Texture2D<float4> gCanvasAbCompareTex : register(t1);
Texture2D<float4> gCodecDecodedTex    : register(t2);
SamplerState      gLinearClamp        : register(s0);

// 5-tap Catmull-Rom bicubic upscale/downsample filter for crisp 1080p->4K or 720p->1080p scaling
float3 sampleTexCatmullRom(Texture2D<float4> tex, float2 uv, float2 texSize)
{
    float2 samplePos = uv * texSize;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;

    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    float2 w12 = w1 + w2;
    float2 offset12 = w2 / max(w12, 1e-5.xx);

    float2 invTex = 1.0 / max(texSize, float2(1.0, 1.0));
    float2 tc0  = (texPos1 - 1.0) * invTex;
    float2 tc3  = (texPos1 + 2.0) * invTex;
    float2 tc12 = (texPos1 + offset12) * invTex;

    float3 result = 0.0.xxx;
    result += tex.SampleLevel(gLinearClamp, float2(tc12.x, tc0.y),  0.0).rgb * (w12.x * w0.y);
    result += tex.SampleLevel(gLinearClamp, float2(tc0.x,  tc12.y), 0.0).rgb * (w0.x  * w12.y);
    result += tex.SampleLevel(gLinearClamp, float2(tc12.x, tc12.y), 0.0).rgb * (w12.x * w12.y);
    result += tex.SampleLevel(gLinearClamp, float2(tc3.x,  tc12.y), 0.0).rgb * (w3.x  * w12.y);
    result += tex.SampleLevel(gLinearClamp, float2(tc12.x, tc3.y),  0.0).rgb * (w12.x * w3.y);

    float normWeight = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
    return saturate(result / max(normWeight, 1e-5));
}

// Honest shader block-quantization substitute when Codec view is active but hardware Codec is disabled/unavailable
float3 sampleShaderCodecSubstitute(float2 uv, bool useExternalTex)
{
    float2 macroGrid = float2(160.0, 90.0); // 8x8 blocks at 1280x720
    float2 chromaGrid = float2(320.0, 180.0);
    float2 blockUv = (floor(uv * macroGrid) + 0.5) / macroGrid;
    float2 chromaUv = (floor(uv * chromaGrid) + 0.5) / chromaGrid;

    float3 fine = useExternalTex
        ? gCodecDecodedTex.SampleLevel(gLinearClamp, uv, 0.0).rgb
        : gCanvasCleanTex.SampleLevel(gLinearClamp, uv, 0.0).rgb;
    float3 blk  = useExternalTex
        ? gCodecDecodedTex.SampleLevel(gLinearClamp, blockUv, 0.0).rgb
        : gCanvasCleanTex.SampleLevel(gLinearClamp, blockUv, 0.0).rgb;
    float3 chr  = useExternalTex
        ? gCodecDecodedTex.SampleLevel(gLinearClamp, chromaUv, 0.0).rgb
        : gCanvasCleanTex.SampleLevel(gLinearClamp, chromaUv, 0.0).rgb;

    float yFine = dot(fine, float3(0.2126, 0.7152, 0.0722));
    float yBlk  = dot(blk,  float3(0.2126, 0.7152, 0.0722));
    float yQuant = floor(lerp(yFine, yBlk, 0.58) * 28.0 + 0.5) / 28.0;

    float yChr = max(dot(chr, float3(0.2126, 0.7152, 0.0722)), 1e-3);
    float3 quantRgb = saturate(chr * (yQuant / yChr));
    if (useExternalTex)
    {
        float3 carrier = gCanvasCleanTex.SampleLevel(gLinearClamp, uv, 0.0).rgb;
        float scan = 0.94 + 0.06 * sin(uv.y * 720.0 * 3.14159265 + gFilmTime * 18.0);
        quantRgb = saturate(lerp(quantRgb * scan, carrier, 0.18));
    }
    return quantRgb;
}

// BEGIN FIRST-PARTY VECTOR TYPE (generated by tools/gen_title_glyphs.py)
static const float4 kTypeStrokes[283] = {
    float4(0.500,5.500,0.500,2.000),
    float4(0.500,2.000,2.000,0.500),
    float4(2.000,0.500,3.500,2.000),
    float4(3.500,2.000,3.500,5.500),
    float4(0.500,3.200,3.500,3.200),
    float4(0.500,5.500,0.500,0.500),
    float4(0.500,0.500,2.700,0.500),
    float4(2.700,0.500,3.500,1.200),
    float4(3.500,1.200,3.500,2.200),
    float4(3.500,2.200,2.700,3.000),
    float4(2.700,3.000,0.500,3.000),
    float4(2.700,3.000,3.500,3.700),
    float4(3.500,3.700,3.500,4.800),
    float4(3.500,4.800,2.700,5.500),
    float4(2.700,5.500,0.500,5.500),
    float4(3.500,1.000,2.800,0.500),
    float4(2.800,0.500,1.200,0.500),
    float4(1.200,0.500,0.500,1.200),
    float4(0.500,1.200,0.500,4.800),
    float4(0.500,4.800,1.200,5.500),
    float4(1.200,5.500,2.800,5.500),
    float4(2.800,5.500,3.500,5.000),
    float4(0.500,5.500,0.500,0.500),
    float4(0.500,0.500,2.600,0.500),
    float4(2.600,0.500,3.500,1.500),
    float4(3.500,1.500,3.500,4.500),
    float4(3.500,4.500,2.600,5.500),
    float4(2.600,5.500,0.500,5.500),
    float4(3.500,0.500,0.500,0.500),
    float4(0.500,0.500,0.500,5.500),
    float4(0.500,5.500,3.500,5.500),
    float4(0.500,3.000,2.900,3.000),
    float4(3.500,0.500,0.500,0.500),
    float4(0.500,0.500,0.500,5.500),
    float4(0.500,3.000,2.900,3.000),
    float4(3.500,1.000,2.800,0.500),
    float4(2.800,0.500,1.200,0.500),
    float4(1.200,0.500,0.500,1.200),
    float4(0.500,1.200,0.500,4.800),
    float4(0.500,4.800,1.200,5.500),
    float4(1.200,5.500,3.500,5.500),
    float4(3.500,5.500,3.500,3.000),
    float4(3.500,3.000,2.300,3.000),
    float4(0.500,0.500,0.500,5.500),
    float4(3.500,0.500,3.500,5.500),
    float4(0.500,3.000,3.500,3.000),
    float4(0.800,0.500,3.200,0.500),
    float4(2.000,0.500,2.000,5.500),
    float4(0.800,5.500,3.200,5.500),
    float4(1.600,0.500,3.500,0.500),
    float4(3.500,0.500,3.500,4.600),
    float4(3.500,4.600,2.600,5.500),
    float4(2.600,5.500,1.300,5.500),
    float4(1.300,5.500,0.500,4.600),
    float4(0.500,0.500,0.500,5.500),
    float4(3.500,0.500,0.500,3.000),
    float4(0.500,3.000,3.500,5.500),
    float4(0.500,0.500,0.500,5.500),
    float4(0.500,5.500,3.500,5.500),
    float4(0.500,5.500,0.500,0.500),
    float4(0.500,0.500,2.000,2.800),
    float4(2.000,2.800,3.500,0.500),
    float4(3.500,0.500,3.500,5.500),
    float4(0.500,5.500,0.500,0.500),
    float4(0.500,0.500,3.500,5.500),
    float4(3.500,5.500,3.500,0.500),
    float4(1.200,0.500,2.800,0.500),
    float4(2.800,0.500,3.500,1.200),
    float4(3.500,1.200,3.500,4.800),
    float4(3.500,4.800,2.800,5.500),
    float4(2.800,5.500,1.200,5.500),
    float4(1.200,5.500,0.500,4.800),
    float4(0.500,4.800,0.500,1.200),
    float4(0.500,1.200,1.200,0.500),
    float4(0.500,5.500,0.500,0.500),
    float4(0.500,0.500,2.800,0.500),
    float4(2.800,0.500,3.500,1.200),
    float4(3.500,1.200,3.500,2.300),
    float4(3.500,2.300,2.800,3.000),
    float4(2.800,3.000,0.500,3.000),
    float4(1.200,0.500,2.800,0.500),
    float4(2.800,0.500,3.500,1.200),
    float4(3.500,1.200,3.500,4.800),
    float4(3.500,4.800,2.800,5.500),
    float4(2.800,5.500,1.200,5.500),
    float4(1.200,5.500,0.500,4.800),
    float4(0.500,4.800,0.500,1.200),
    float4(0.500,1.200,1.200,0.500),
    float4(2.400,4.100,3.700,5.600),
    float4(0.500,5.500,0.500,0.500),
    float4(0.500,0.500,2.800,0.500),
    float4(2.800,0.500,3.500,1.200),
    float4(3.500,1.200,3.500,2.300),
    float4(3.500,2.300,2.800,3.000),
    float4(2.800,3.000,0.500,3.000),
    float4(2.200,3.000,3.500,5.500),
    float4(3.500,1.000,2.800,0.500),
    float4(2.800,0.500,1.200,0.500),
    float4(1.200,0.500,0.500,1.200),
    float4(0.500,1.200,0.500,2.200),
    float4(0.500,2.200,1.200,3.000),
    float4(1.200,3.000,2.800,3.000),
    float4(2.800,3.000,3.500,3.800),
    float4(3.500,3.800,3.500,4.800),
    float4(3.500,4.800,2.800,5.500),
    float4(2.800,5.500,1.200,5.500),
    float4(1.200,5.500,0.500,5.000),
    float4(0.500,0.500,3.500,0.500),
    float4(2.000,0.500,2.000,5.500),
    float4(0.500,0.500,0.500,4.800),
    float4(0.500,4.800,1.200,5.500),
    float4(1.200,5.500,2.800,5.500),
    float4(2.800,5.500,3.500,4.800),
    float4(3.500,4.800,3.500,0.500),
    float4(0.500,0.500,1.100,3.600),
    float4(1.100,3.600,2.000,5.500),
    float4(2.000,5.500,2.900,3.600),
    float4(2.900,3.600,3.500,0.500),
    float4(0.500,0.500,0.800,5.500),
    float4(0.800,5.500,2.000,3.000),
    float4(2.000,3.000,3.200,5.500),
    float4(3.200,5.500,3.500,0.500),
    float4(0.500,0.500,3.500,5.500),
    float4(3.500,0.500,0.500,5.500),
    float4(0.500,0.500,2.000,3.000),
    float4(2.000,3.000,3.500,0.500),
    float4(2.000,3.000,2.000,5.500),
    float4(0.500,0.500,3.500,0.500),
    float4(3.500,0.500,0.500,5.500),
    float4(0.500,5.500,3.500,5.500),
    float4(1.200,0.500,2.800,0.500),
    float4(2.800,0.500,3.500,1.200),
    float4(3.500,1.200,3.500,4.800),
    float4(3.500,4.800,2.800,5.500),
    float4(2.800,5.500,1.200,5.500),
    float4(1.200,5.500,0.500,4.800),
    float4(0.500,4.800,0.500,1.200),
    float4(0.500,1.200,1.200,0.500),
    float4(1.000,1.400,2.000,0.500),
    float4(2.000,0.500,2.000,5.500),
    float4(0.800,5.500,3.200,5.500),
    float4(0.500,1.200,1.200,0.500),
    float4(1.200,0.500,2.800,0.500),
    float4(2.800,0.500,3.500,1.200),
    float4(3.500,1.200,3.500,2.200),
    float4(3.500,2.200,0.500,5.500),
    float4(0.500,5.500,3.500,5.500),
    float4(0.500,0.500,3.500,0.500),
    float4(3.500,0.500,2.000,2.800),
    float4(2.000,2.800,3.500,3.500),
    float4(3.500,3.500,3.500,4.800),
    float4(3.500,4.800,2.800,5.500),
    float4(2.800,5.500,1.200,5.500),
    float4(1.200,5.500,0.500,5.000),
    float4(2.900,5.500,2.900,0.500),
    float4(2.900,0.500,0.500,3.800),
    float4(0.500,3.800,3.700,3.800),
    float4(3.500,0.500,0.500,0.500),
    float4(0.500,0.500,0.500,2.800),
    float4(0.500,2.800,2.800,2.800),
    float4(2.800,2.800,3.500,3.500),
    float4(3.500,3.500,3.500,4.800),
    float4(3.500,4.800,2.800,5.500),
    float4(2.800,5.500,1.000,5.500),
    float4(1.000,5.500,0.500,5.000),
    float4(3.200,0.500,1.400,0.500),
    float4(1.400,0.500,0.500,1.500),
    float4(0.500,1.500,0.500,4.800),
    float4(0.500,4.800,1.200,5.500),
    float4(1.200,5.500,2.800,5.500),
    float4(2.800,5.500,3.500,4.800),
    float4(3.500,4.800,3.500,3.500),
    float4(3.500,3.500,2.800,2.800),
    float4(2.800,2.800,0.500,2.800),
    float4(0.500,0.500,3.500,0.500),
    float4(3.500,0.500,1.300,5.500),
    float4(1.200,0.500,2.800,0.500),
    float4(2.800,0.500,3.500,1.200),
    float4(3.500,1.200,3.500,2.200),
    float4(3.500,2.200,2.800,3.000),
    float4(2.800,3.000,1.200,3.000),
    float4(1.200,3.000,0.500,2.200),
    float4(0.500,2.200,0.500,1.200),
    float4(0.500,1.200,1.200,0.500),
    float4(1.200,3.000,0.500,3.700),
    float4(0.500,3.700,0.500,4.800),
    float4(0.500,4.800,1.200,5.500),
    float4(1.200,5.500,2.800,5.500),
    float4(2.800,5.500,3.500,4.800),
    float4(3.500,4.800,3.500,3.700),
    float4(3.500,3.700,2.800,3.000),
    float4(3.500,3.000,1.200,3.000),
    float4(1.200,3.000,0.500,2.300),
    float4(0.500,2.300,0.500,1.200),
    float4(0.500,1.200,1.200,0.500),
    float4(1.200,0.500,2.800,0.500),
    float4(2.800,0.500,3.500,1.200),
    float4(3.500,1.200,3.500,4.500),
    float4(3.500,4.500,2.600,5.500),
    float4(2.600,5.500,0.800,5.500),
    float4(1.900,5.300,2.100,5.300),
    float4(1.900,1.700,2.100,1.700),
    float4(1.900,4.800,2.100,4.800),
    float4(2.200,4.900,1.600,5.700),
    float4(2.000,0.400,1.700,1.400),
    float4(2.000,0.500,2.000,3.800),
    float4(1.900,5.300,2.100,5.300),
    float4(0.500,1.200,1.200,0.500),
    float4(1.200,0.500,2.800,0.500),
    float4(2.800,0.500,3.500,1.200),
    float4(3.500,1.200,3.500,2.200),
    float4(3.500,2.200,2.000,3.300),
    float4(2.000,3.300,2.000,3.800),
    float4(1.900,5.300,2.100,5.300),
    float4(0.700,3.000,3.300,3.000),
    float4(0.500,5.500,3.500,5.500),
    float4(0.600,3.000,3.400,3.000),
    float4(2.000,1.200,2.000,4.800),
    float4(0.500,5.500,3.500,0.500),
    float4(0.500,5.500,3.500,0.500),
    float4(0.800,0.800,1.200,1.400),
    float4(2.800,4.600,3.200,5.200),
    float4(2.800,0.500,1.500,1.500),
    float4(1.500,1.500,1.200,3.000),
    float4(1.200,3.000,1.500,4.500),
    float4(1.500,4.500,2.800,5.500),
    float4(1.200,0.500,2.500,1.500),
    float4(2.500,1.500,2.800,3.000),
    float4(2.800,3.000,2.500,4.500),
    float4(2.500,4.500,1.200,5.500),
    float4(2.800,0.500,1.200,0.500),
    float4(1.200,0.500,1.200,5.500),
    float4(1.200,5.500,2.800,5.500),
    float4(1.200,0.500,2.800,0.500),
    float4(2.800,0.500,2.800,5.500),
    float4(2.800,5.500,1.200,5.500),
    float4(1.000,5.000,9.000,5.000),
    float4(2.000,2.500,8.000,2.500),
    float4(1.000,7.500,9.000,7.500),
    float4(1.500,2.000,8.500,2.000),
    float4(2.300,5.000,7.700,5.000),
    float4(1.000,8.000,9.000,8.000),
    float4(1.000,1.500,9.000,1.500),
    float4(9.000,1.500,9.000,8.500),
    float4(9.000,8.500,1.000,8.500),
    float4(1.000,8.500,1.000,1.500),
    float4(4.000,1.500,4.000,4.500),
    float4(4.000,4.500,2.500,6.000),
    float4(6.000,1.500,6.000,6.000),
    float4(6.000,6.000,8.000,6.000),
    float4(1.500,1.500,8.500,1.500),
    float4(4.400,1.500,3.200,8.500),
    float4(2.000,4.500,7.300,4.500),
    float4(7.300,4.500,7.300,8.500),
    float4(1.000,8.500,9.000,8.500),
    float4(4.300,1.000,5.300,2.500),
    float4(1.000,3.500,9.000,3.500),
    float4(3.500,5.200,1.700,8.800),
    float4(6.500,5.200,8.300,8.800),
    float4(1.000,4.200,9.000,3.000),
    float4(5.000,0.800,5.000,7.500),
    float4(5.000,7.500,6.000,8.700),
    float4(6.000,8.700,8.500,8.700),
    float4(8.500,8.700,9.000,7.400),
    float4(5.000,1.000,2.000,4.000),
    float4(2.000,4.000,8.000,4.000),
    float4(6.500,2.400,8.500,4.600),
    float4(2.400,6.000,7.700,6.000),
    float4(7.700,6.000,7.700,9.000),
    float4(7.700,9.000,2.400,9.000),
    float4(2.400,9.000,2.400,6.000),
    float4(4.000,1.000,4.000,8.800),
    float4(1.000,3.600,4.000,3.600),
    float4(1.000,8.000,4.000,6.800),
    float4(6.400,1.000,6.400,8.100),
    float4(6.400,8.100,7.000,9.000),
    float4(7.000,9.000,9.000,9.000),
    float4(9.000,9.000,9.300,7.400),
    float4(6.400,4.800,9.000,2.800),
    float4(3.300,0.700,0.700,3.000),
    float4(0.700,3.000,3.300,5.300),
    float4(0.700,0.700,3.300,3.000),
    float4(3.300,3.000,0.700,5.300)
};
uint2 glyphStrokes(uint ch) {
    if(ch>=97u && ch<=122u) ch-=32u;
    switch(ch) {
        case 65u: return uint2(0u,5u); // A
        case 66u: return uint2(5u,10u); // B
        case 67u: return uint2(15u,7u); // C
        case 68u: return uint2(22u,6u); // D
        case 69u: return uint2(28u,4u); // E
        case 70u: return uint2(32u,3u); // F
        case 71u: return uint2(35u,8u); // G
        case 72u: return uint2(43u,3u); // H
        case 73u: return uint2(46u,3u); // I
        case 74u: return uint2(49u,5u); // J
        case 75u: return uint2(54u,3u); // K
        case 76u: return uint2(57u,2u); // L
        case 77u: return uint2(59u,4u); // M
        case 78u: return uint2(63u,3u); // N
        case 79u: return uint2(66u,8u); // O
        case 80u: return uint2(74u,6u); // P
        case 81u: return uint2(80u,9u); // Q
        case 82u: return uint2(89u,7u); // R
        case 83u: return uint2(96u,11u); // S
        case 84u: return uint2(107u,2u); // T
        case 85u: return uint2(109u,5u); // U
        case 86u: return uint2(114u,4u); // V
        case 87u: return uint2(118u,4u); // W
        case 88u: return uint2(122u,2u); // X
        case 89u: return uint2(124u,3u); // Y
        case 90u: return uint2(127u,3u); // Z
        case 48u: return uint2(130u,8u); // 0
        case 49u: return uint2(138u,3u); // 1
        case 50u: return uint2(141u,6u); // 2
        case 51u: return uint2(147u,7u); // 3
        case 52u: return uint2(154u,3u); // 4
        case 53u: return uint2(157u,8u); // 5
        case 54u: return uint2(165u,9u); // 6
        case 55u: return uint2(174u,2u); // 7
        case 56u: return uint2(176u,15u); // 8
        case 57u: return uint2(191u,9u); // 9
        case 46u: return uint2(200u,1u); // .
        case 58u: return uint2(201u,2u); // :
        case 44u: return uint2(203u,1u); // ,
        case 39u: return uint2(204u,1u); // '
        case 33u: return uint2(205u,2u); // !
        case 63u: return uint2(207u,7u); // ?
        case 45u: return uint2(214u,1u); // -
        case 95u: return uint2(215u,1u); // _
        case 43u: return uint2(216u,2u); // +
        case 47u: return uint2(218u,1u); // /
        case 37u: return uint2(219u,3u); // %
        case 40u: return uint2(222u,4u); // (
        case 41u: return uint2(226u,4u); // )
        case 91u: return uint2(230u,3u); // [
        case 93u: return uint2(233u,3u); // ]
        case 60u: return uint2(279u,2u); // <
        case 62u: return uint2(281u,2u); // >
        case 19968u: return uint2(236u,1u); // 一
        case 20108u: return uint2(237u,2u); // 二
        case 19977u: return uint2(239u,3u); // 三
        case 22235u: return uint2(242u,8u); // 四
        case 20116u: return uint2(250u,5u); // 五
        case 20845u: return uint2(255u,4u); // 六
        case 19971u: return uint2(259u,5u); // 七
        case 21488u: return uint2(264u,7u); // 台
        case 21271u: return uint2(271u,8u); // 北
        default: return uint2(0u,0u);
    }
}
float typeLineDistance(float2 p,float2 a,float2 b) {
    float2 ab=b-a;
    return length(p-a-ab*saturate(dot(p-a,ab)/max(dot(ab,ab),1e-5)));
}
float sampleGlyph(float2 px,float2 origin,float scale,uint ch) {
    float2 p=(px-origin)/max(scale,.01);
    float2 extent=(ch>255u)?float2(10,10):float2(4,6);
    if(any(p<-.6)||any(p>extent+.6)) return 0;
    uint2 span=glyphStrokes(ch);
    float d=100.0;
    [loop] for(uint i=0u;i<span.y;++i) {
        float4 stroke=kTypeStrokes[span.x+i];
        d=min(d,typeLineDistance(p,stroke.xy,stroke.zw));
    }
    float radius=(ch>255u)?.25:.22;
    float aa=.65/max(scale,.01);
    return 1.0-smoothstep(radius-aa,radius+aa,d);
}
// END FIRST-PARTY VECTOR TYPE

float sampleFixedNumber6(float2 px, float2 origin, float scale, float val)
{
    float stepX = 5.0 * scale;
    float a = 0.0;
    if (val >= 10000.0)
    {
        // Auto-format large throughput scores (e.g. 73,635 MSteps/s -> "73.6K") without clamping to 9999.9!
        float kVal = clamp(val * 0.001, 10.0, 999.9);
        uint tenthsK = (uint)(kVal * 10.0 + 0.5);
        uint fracK = tenthsK % 10u;
        uint wholeK = tenthsK / 10u;
        uint k2 = (wholeK / 100u) % 10u;
        uint k1 = (wholeK / 10u)  % 10u;
        uint k0 = wholeK % 10u;
        if (wholeK >= 100u) a = max(a, sampleGlyph(px, origin + float2(0.0 * stepX, 0.0), scale, 48u + k2));
        if (wholeK >= 10u)  a = max(a, sampleGlyph(px, origin + float2(1.0 * stepX, 0.0), scale, 48u + k1));
        a = max(a, sampleGlyph(px, origin + float2(2.0 * stepX, 0.0), scale, 48u + k0));
        a = max(a, sampleGlyph(px, origin + float2(3.0 * stepX, 0.0), scale, 46u));
        a = max(a, sampleGlyph(px, origin + float2(4.0 * stepX, 0.0), scale, 48u + fracK));
        a = max(a, sampleGlyph(px, origin + float2(5.0 * stepX, 0.0), scale, 75u)); // 'K'
        return a;
    }

    val = clamp(val, 0.0, 9999.9);
    uint tenthsTotal = (uint)(val * 10.0 + 0.5);
    uint fracDig = tenthsTotal % 10u;
    uint whole = tenthsTotal / 10u;

    uint d3 = (whole / 1000u) % 10u;
    uint d2 = (whole / 100u)  % 10u;
    uint d1 = (whole / 10u)   % 10u;
    uint d0 = whole % 10u;

    if (whole >= 1000u) a = max(a, sampleGlyph(px, origin + float2(0.0 * stepX, 0.0), scale, 48u + d3));
    if (whole >= 100u)  a = max(a, sampleGlyph(px, origin + float2(1.0 * stepX, 0.0), scale, 48u + d2));
    if (whole >= 10u)   a = max(a, sampleGlyph(px, origin + float2(2.0 * stepX, 0.0), scale, 48u + d1));
    a = max(a, sampleGlyph(px, origin + float2(3.0 * stepX, 0.0), scale, 48u + d0));
    a = max(a, sampleGlyph(px, origin + float2(4.0 * stepX, 0.0), scale, 46u));
    a = max(a, sampleGlyph(px, origin + float2(5.0 * stepX, 0.0), scale, 48u + fracDig));
    return a;
}

float sampleFixedMs6(float2 px, float2 origin, float scale, float val)
{
    val = clamp(val, 0.0, 999.99);
    uint hundTotal = (uint)(val * 100.0 + 0.5);
    uint f1 = (hundTotal / 10u) % 10u;
    uint f0 = hundTotal % 10u;
    uint whole = hundTotal / 100u;

    uint d2 = (whole / 100u) % 10u;
    uint d1 = (whole / 10u)  % 10u;
    uint d0 = whole % 10u;

    float stepX = 5.0 * scale;
    float a = 0.0;
    if (whole >= 100u) a = max(a, sampleGlyph(px, origin + float2(0.0 * stepX, 0.0), scale, 48u + d2));
    if (whole >= 10u)  a = max(a, sampleGlyph(px, origin + float2(1.0 * stepX, 0.0), scale, 48u + d1));
    a = max(a, sampleGlyph(px, origin + float2(2.0 * stepX, 0.0), scale, 48u + d0));
    a = max(a, sampleGlyph(px, origin + float2(3.0 * stepX, 0.0), scale, 46u));
    a = max(a, sampleGlyph(px, origin + float2(4.0 * stepX, 0.0), scale, 48u + f1));
    a = max(a, sampleGlyph(px, origin + float2(5.0 * stepX, 0.0), scale, 48u + f0));
    return a;
}

float sampleUint4(float2 px, float2 origin, float scale, uint val)
{
    val = min(val, 9999u);
    float stepX = 5.0 * scale;
    float a = 0.0;
    uint d3 = (val / 1000u) % 10u;
    uint d2 = (val / 100u)  % 10u;
    uint d1 = (val / 10u)   % 10u;
    uint d0 = val % 10u;
    if (val >= 1000u) a = max(a, sampleGlyph(px, origin + float2(0.0 * stepX, 0.0), scale, 48u + d3));
    if (val >= 100u)  a = max(a, sampleGlyph(px, origin + float2(1.0 * stepX, 0.0), scale, 48u + d2));
    if (val >= 10u)   a = max(a, sampleGlyph(px, origin + float2(2.0 * stepX, 0.0), scale, 48u + d1));
    a = max(a, sampleGlyph(px, origin + float2(3.0 * stepX, 0.0), scale, 48u + d0));
    return a;
}

float sampleWord4(float2 px, float2 origin, float scale, uint c0, uint c1, uint c2, uint c3)
{
    float stepX = 5.0 * scale;
    float a = 0.0;
    a = max(a, sampleGlyph(px, origin + float2(0.0 * stepX, 0.0), scale, c0));
    a = max(a, sampleGlyph(px, origin + float2(1.0 * stepX, 0.0), scale, c1));
    a = max(a, sampleGlyph(px, origin + float2(2.0 * stepX, 0.0), scale, c2));
    a = max(a, sampleGlyph(px, origin + float2(3.0 * stepX, 0.0), scale, c3));
    return a;
}

float sampleWord8(
    float2 px, float2 origin, float scale,
    uint c0, uint c1, uint c2, uint c3, uint c4, uint c5, uint c6, uint c7)
{
    float stepX = 5.0 * scale;
    return max(
        sampleWord4(px, origin, scale, c0, c1, c2, c3),
        sampleWord4(px, origin + float2(4.0 * stepX, 0.0), scale, c4, c5, c6, c7));
}

uint fetchPackedCaptionByte(uint idx)
{
    uint vecIdx  = idx >> 4u;
    uint compIdx = (idx >> 2u) & 3u;
    uint4 v = (vecIdx == 0u) ? gCaptionPacked[0]
            : (vecIdx == 1u) ? gCaptionPacked[1]
            : (vecIdx == 2u) ? gCaptionPacked[2]
                             : gCaptionPacked[3];
    uint word = (compIdx == 0u) ? v.x
              : (compIdx == 1u) ? v.y
              : (compIdx == 2u) ? v.z
                                : v.w;
    return (word >> ((idx & 3u) * 8u)) & 0xFFu;
}

float samplePackedCaption(float2 px, float2 origin, float scale, uint charCount)
{
    float stepX = 5.0 * scale;
    float2 rel = px - origin;
    if (rel.x < 0.0 || rel.y < 0.0 || rel.y >= 6.0 * scale)
        return 0.0;
    uint idx = (uint)(rel.x / stepX);
    if (idx >= charCount || idx >= 64u)
        return 0.0;
    uint ch = fetchPackedCaptionByte(idx);
    if (ch <= 32u)
        return 0.0;
    return sampleGlyph(px, origin + float2((float)idx * stepX, 0.0), scale, ch);
}

float sampleTimecodeMMSS(float2 px, float2 origin, float scale, float seconds)
{
    uint totalSec = (uint)max(0.0, seconds);
    uint mm = (totalSec / 60u) % 100u;
    uint ss = totalSec % 60u;
    float stepX = 5.0 * scale;
    float a = 0.0;
    a = max(a, sampleGlyph(px, origin + float2(0.0 * stepX, 0.0), scale, 48u + (mm / 10u)));
    a = max(a, sampleGlyph(px, origin + float2(1.0 * stepX, 0.0), scale, 48u + (mm % 10u)));
    a = max(a, sampleGlyph(px, origin + float2(2.0 * stepX, 0.0), scale, 58u));
    a = max(a, sampleGlyph(px, origin + float2(3.0 * stepX, 0.0), scale, 48u + (ss / 10u)));
    a = max(a, sampleGlyph(px, origin + float2(4.0 * stepX, 0.0), scale, 48u + (ss % 10u)));
    return a;
}

float3 badgeStatusColor(uint code)
{
    if (code == 0u) return float3(0.08, 0.96, 0.62); // Accelerated: Bright Jade Emerald
    if (code == 1u) return float3(0.12, 0.72, 0.94); // Substituted: Taipei Cyan
    if (code == 2u) return float3(1.00, 0.72, 0.12); // Degraded: Amber Gold
    if (code == 4u) return float3(0.98, 0.20, 0.32); // Faulted: Crimson
    return float3(0.32, 0.36, 0.44);                 // Unavailable: Slate Standby
}

float3 livePillarModeColor(uint modeNib)
{
    if (modeNib == 0u) return float3(0.08, 0.96, 0.62); // LIVE HW
    if (modeNib == 1u) return float3(0.14, 0.74, 0.96); // SUB
    if (modeNib == 3u) return float3(0.98, 0.22, 0.34); // FAULT
    return float3(0.42, 0.46, 0.56);                    // OFF / STANDBY
}

float segmentedBar(float2 px, float2 rectMin, float2 rectSize, float fill01, uint segments)
{
    float2 rel = (px - rectMin) / rectSize;
    if (rel.x < 0.0 || rel.x > 1.0 || rel.y < 0.0 || rel.y > 1.0)
        return 0.0;
    float segCoord = rel.x * (float)segments;
    float segFrac  = frac(segCoord);
    float gapMask  = step(0.14, segFrac) * step(segFrac, 0.92);
    return (rel.x <= saturate(fill01)) ? gapMask : (0.12 * gapMask);
}

float verticalBar(float2 px, float2 rectMin, float2 rectSize, float fill01)
{
    float2 rel = (px - rectMin) / rectSize;
    if (rel.x < 0.0 || rel.x > 1.0 || rel.y < 0.0 || rel.y > 1.0)
        return 0.0;
    float fromBottom = 1.0 - rel.y;
    return (fromBottom <= saturate(fill01)) ? 1.0 : 0.15;
}

float4 PSPresentLetterboxHud(VSOut pin) : SV_Target
{
    const float2 res = max(gSwapchainAndCanvas.xy, float2(1.0, 1.0));
    const float2 canvasRes = max(gSwapchainAndCanvas.zw, float2(1.0, 1.0));
    const float2 px = pin.uv * res;

    const bool showOverlay        = (gHudFlags & 1u) != 0u;
    const uint hudMode            = (gHudFlags >> 8u) & 3u;
    const uint abEngine           = (gHudFlags >> 10u) & 3u;
    const bool codecViewActive    = (gHudFlags & (1u << 12u)) != 0u;
    const bool codecHwTexValid    = (gHudFlags & (1u << 13u)) != 0u;
    const bool abLeftHwVerified   = (gHudFlags & (1u << 14u)) != 0u;
    const bool externalMediaValid = (gHudFlags & (1u << 15u)) != 0u;

    float3 sdr = 0.0.xxx;
    float2 lbMin = gLetterboxRect.xy;
    float2 lbSize = max(gLetterboxRect.zw, float2(1.0, 1.0));
    if (px.x >= lbMin.x && px.x < (lbMin.x + lbSize.x) &&
        px.y >= lbMin.y && px.y < (lbMin.y + lbSize.y))
    {
        float2 canvasUv = saturate((px - lbMin) / lbSize);
        const float splitRatio = clamp(gInteractiveState.x, 0.05, 0.95);
        if (showOverlay && hudMode == 2u)
        {
            if (abEngine == 3u)
            {
                // Codec A/B: Left = Live Hardware H.264 Encoded+Decoded (or Shader Quant Sub), Right = Pristine 10-bit CanvasClean
                if (canvasUv.x < splitRatio)
                {
                    if (codecHwTexValid)
                    {
                        float3 hwRgb = gCodecDecodedTex.SampleLevel(gLinearClamp, canvasUv, 0.0).rgb;
                        if (externalMediaValid)
                        {
                            float3 carrier = gCanvasCleanTex.SampleLevel(gLinearClamp, canvasUv, 0.0).rgb;
                            hwRgb = saturate(lerp(hwRgb, carrier, 0.18));
                        }
                        sdr = hwRgb;
                    }
                    else
                    {
                        sdr = sampleShaderCodecSubstitute(canvasUv, externalMediaValid);
                    }
                }
                else
                {
                    sdr = sampleTexCatmullRom(gCanvasCleanTex, canvasUv, canvasRes);
                }
            }
            else if (abLeftHwVerified && canvasUv.x >= splitRatio)
            {
                // RT / CUDA / Tensor A/B: Left = Canonical Live HW CanvasClean, Right = Isolated Substitute CanvasAbCompare
                sdr = sampleTexCatmullRom(gCanvasAbCompareTex, canvasUv, canvasRes);
            }
            else
            {
                sdr = sampleTexCatmullRom(gCanvasCleanTex, canvasUv, canvasRes);
            }
        }
        else if (codecViewActive)
        {
            if (codecHwTexValid)
            {
                float3 hwRgb = gCodecDecodedTex.SampleLevel(gLinearClamp, canvasUv, 0.0).rgb;
                if (externalMediaValid)
                {
                    float3 carrier = gCanvasCleanTex.SampleLevel(gLinearClamp, canvasUv, 0.0).rgb;
                    hwRgb = saturate(lerp(hwRgb, carrier, 0.18));
                }
                sdr = hwRgb;
            }
            else
            {
                sdr = sampleShaderCodecSubstitute(canvasUv, externalMediaValid);
            }
        }
        else
        {
            sdr = sampleTexCatmullRom(gCanvasCleanTex, canvasUv, canvasRes);
        }
    }

    const float3 colJade   = float3(0.04, 0.88, 0.74);
    const float3 colOrchid = float3(0.88, 0.24, 0.90);
    const float3 colAmber  = float3(1.00, 0.68, 0.16);
    const float3 colCyan   = float3(0.14, 0.74, 0.96);
    const float3 colEnamel = float3(0.96, 0.95, 0.91);

    const uint curAovMode   = (gInteractiveMeta.x >> 9u) & 7u;
    const uint curDenMode   = (gInteractiveMeta.x >> 12u) & 3u;
    const uint curMarbleCnt = (gInteractiveMeta.x >> 16u) & 0xFFu;
    const uint curSolo      = (gInteractiveMeta.x >> 24u) & 7u;
    const bool studioPinned = (gInteractiveMeta.x & (1u << 27u)) != 0u;
    const bool studioActive = studioPinned || ((gInteractiveMeta.x & 7u) != 0u) || (curAovMode != 0u) || (curMarbleCnt != 0u) || (curSolo != 0u);

    if (showOverlay && (hudMode == 0u || hudMode == 2u) && res.x >= 480.0 && res.y >= 300.0)
    {
        const float uiScale = clamp(floor(res.y / 540.0), 1.0, 3.0);
        const float barH = 44.0 * uiScale;
        if (px.y >= res.y - barH)
        {
            sdr = lerp(sdr, float3(0.020, 0.032, 0.058), 0.90);
            if (abs(px.y - (res.y - barH)) < 1.5)
                sdr = lerp(sdr, colJade, 0.65);

            // Timecode MM:SS
            float tcMask = sampleTimecodeMMSS(px, float2(14.0 * uiScale, res.y - 34.0 * uiScale), 2.0 * uiScale, gFilmTime);
            sdr = lerp(sdr, colEnamel, tcMask);

            // 10B / 8B swapchain bit-depth + Audio Clock lock badge ("ACLK" vs "QPC ")
            uint b0 = (gSwapchainBits >= 10u) ? 49u : 56u;
            uint b1 = (gSwapchainBits >= 10u) ? 48u : 66u;
            uint b2 = (gSwapchainBits >= 10u) ? 66u : 32u;
            float bitTxt = sampleWord4(px, float2(14.0 * uiScale, res.y - 17.0 * uiScale), 1.5 * uiScale, b0, b1, b2, 32u);
            sdr = lerp(sdr, (gSwapchainBits >= 10u) ? colJade : colAmber, bitTxt);

            bool aclk = (gHudFlags & 2u) != 0u;
            float clkTxt = sampleWord4(
                px,
                float2(44.0 * uiScale, res.y - 17.0 * uiScale),
                1.5 * uiScale,
                aclk ? 65u : 81u, aclk ? 67u : 80u, aclk ? 76u : 67u, aclk ? 75u : 32u);
            sdr = lerp(sdr, aclk ? colJade : colCyan, clkTxt);

            // Clickable / Draggable Timeline Scrub Bar + Act Sub-Bar
            float2 tlMin  = float2(82.0 * uiScale, res.y - 31.0 * uiScale);
            float2 tlSize = float2(max(90.0, res.x * 0.20), 10.0 * uiScale);
            float filmFill = saturate(gFilmTime / max(gDuration, 1.0));
            float tlMask = segmentedBar(px, tlMin, tlSize, filmFill, 24u);
            sdr = lerp(sdr, lerp(colJade, colOrchid, filmFill), tlMask * 0.92);
            float tlThumbX = tlMin.x + filmFill * tlSize.x;
            if (abs(px.x - tlThumbX) <= 2.2 * uiScale && px.y >= tlMin.y - 2.5 * uiScale && px.y <= tlMin.y + tlSize.y + 2.5 * uiScale)
                sdr = colEnamel;

            float2 actMin  = float2(82.0 * uiScale, res.y - 16.0 * uiScale);
            float2 actSize = float2(max(90.0, res.x * 0.20), 5.0 * uiScale);
            float actMask = segmentedBar(px, actMin, actSize, gActProgress, 12u);
            sdr = lerp(sdr, colAmber, actMask * 0.88);

            // 3-Band Live Audio Crossover Meters (LOW / MID / HIGH)
            float eqBaseX = tlMin.x + tlSize.x + 12.0 * uiScale;
            float bands[3] = { gAudioBands.y, gAudioBands.z, gAudioBands.w };
            float3 eqCols[3] = { colAmber, colJade, colOrchid };
            [unroll]
            for (uint bandIdx = 0u; bandIdx < 3u; ++bandIdx)
            {
                float2 bMin = float2(eqBaseX + (float)bandIdx * 9.0 * uiScale, res.y - 34.0 * uiScale);
                float2 bSz  = float2(6.0 * uiScale, 24.0 * uiScale);
                float vm = verticalBar(px, bMin, bSz, bands[bandIdx]);
                sdr = lerp(sdr, eqCols[bandIdx], vm * 0.90);
            }

            // FPS & Total Frame GPU ms readouts
            float statX = eqBaseX + 32.0 * uiScale;
            float fpsLbl = sampleWord4(px, float2(statX, res.y - 34.0 * uiScale), 1.8 * uiScale, 70u, 80u, 83u, 58u);
            float fpsVal = sampleFixedNumber6(px, float2(statX + 38.0 * uiScale, res.y - 34.0 * uiScale), 1.8 * uiScale, gFps);
            sdr = lerp(sdr, colJade, fpsLbl);
            sdr = lerp(sdr, colEnamel, fpsVal);

            float msLbl = sampleWord4(px, float2(statX, res.y - 18.0 * uiScale), 1.6 * uiScale, 71u, 80u, 85u, 58u);
            float msVal = sampleFixedNumber6(px, float2(statX + 38.0 * uiScale, res.y - 18.0 * uiScale), 1.6 * uiScale, gGpuMs);
            sdr = lerp(sdr, colAmber, msLbl);
            sdr = lerp(sdr, colEnamel, msVal);

            // 5 Clickable Hardware Engine SOLO / ISOLATE Pills on bottom-right ([RT] [TNSR] [NPU] [CUDA] [CODC])
            float rightBase = res.x - 245.0 * uiScale;
            if (rightBase > statX + 90.0 * uiScale)
            {
                [unroll]
                for (uint e = 0u; e < 5u; ++e)
                {
                    uint stCode = (gEnginePacked >> (e * 4u)) & 0xFu;
                    bool isSolo = (curSolo == (e + 1u));
                    bool mutedByOtherSolo = (curSolo != 0u) && !isSolo;
                    float3 pillCol = isSolo ? colJade : (mutedByOtherSolo ? float3(0.32, 0.36, 0.44) : badgeStatusColor(stCode));
                    float2 pMin = float2(rightBase + (float)e * 47.0 * uiScale, res.y - 34.0 * uiScale);
                    float2 pMax = pMin + float2(42.0 * uiScale, 22.0 * uiScale);
                    if (px.x >= pMin.x && px.x <= pMax.x && px.y >= pMin.y && px.y <= pMax.y)
                    {
                        sdr = lerp(sdr, pillCol * (isSolo ? 0.42 : 0.22), 0.88);
                        float bThick = isSolo ? (2.2 * uiScale) : 1.5;
                        bool border = (px.x - pMin.x < bThick) || (pMax.x - px.x < bThick) ||
                                      (px.y - pMin.y < bThick) || (pMax.y - px.y < bThick);
                        if (border) sdr = isSolo ? colEnamel : pillCol;
                    }
                    uint c0 = 82u, c1 = 84u, c2 = 32u, c3 = 32u;
                    if (e == 1u) { c0 = 84u; c1 = 78u; c2 = 83u; c3 = 82u; } // TNSR
                    if (e == 2u) { c0 = 78u; c1 = 80u; c2 = 85u; c3 = 32u; } // NPU
                    if (e == 3u) { c0 = 67u; c1 = 85u; c2 = 68u; c3 = 65u; } // CUDA
                    if (e == 4u) { c0 = 67u; c1 = 79u; c2 = 68u; c3 = 67u; } // CODC
                    float txt = sampleWord4(px, pMin + float2(5.0 * uiScale, 6.0 * uiScale), 1.6 * uiScale, c0, c1, c2, c3);
                    sdr = lerp(sdr, isSolo ? colEnamel : pillCol, txt);
                }

                // Persistent Bottom Scene & Playback Buttons: [< PRV] [PLAY/PAUS] [NXT >] [F:FILM/STUDIO]
                if (rightBase - 272.0 * uiScale > statX + 82.0 * uiScale)
                {
                    float pTop = res.y - 34.0 * uiScale;
                    float pBot = res.y - 12.0 * uiScale;

                    // [< PRV]
                    float2 prvMin = float2(rightBase - 272.0 * uiScale, pTop);
                    float2 prvMax = float2(rightBase - 220.0 * uiScale, pBot);
                    if (px.x >= prvMin.x && px.x <= prvMax.x && px.y >= pTop && px.y <= pBot)
                    {
                        sdr = lerp(sdr, colCyan * 0.20, 0.86);
                        if ((px.x - prvMin.x < 1.4) || (prvMax.x - px.x < 1.4) || (px.y - pTop < 1.4) || (pBot - px.y < 1.4))
                            sdr = colCyan;
                    }
                    float tPrv = max(
                        sampleGlyph(px, prvMin + float2(5.0 * uiScale, 6.0 * uiScale), 1.5 * uiScale, 60u),
                        sampleWord4(px, prvMin + float2(15.0 * uiScale, 6.0 * uiScale), 1.5 * uiScale, 80u, 82u, 86u, 32u));
                    sdr = lerp(sdr, colEnamel, tPrv);

                    // [PLAY] / [PAUS]
                    bool isPaused = (gHudFlags & 8u) != 0u;
                    float3 plyCol = isPaused ? colJade : colAmber;
                    float2 plyMin = float2(rightBase - 214.0 * uiScale, pTop);
                    float2 plyMax = float2(rightBase - 162.0 * uiScale, pBot);
                    if (px.x >= plyMin.x && px.x <= plyMax.x && px.y >= pTop && px.y <= pBot)
                    {
                        sdr = lerp(sdr, plyCol * 0.24, 0.86);
                        if ((px.x - plyMin.x < 1.4) || (plyMax.x - px.x < 1.4) || (px.y - pTop < 1.4) || (pBot - px.y < 1.4))
                            sdr = plyCol;
                    }
                    float tPly = isPaused
                        ? sampleWord4(px, plyMin + float2(8.0 * uiScale, 6.0 * uiScale), 1.5 * uiScale, 80u, 76u, 65u, 89u)
                        : sampleWord4(px, plyMin + float2(8.0 * uiScale, 6.0 * uiScale), 1.5 * uiScale, 80u, 65u, 85u, 83u);
                    sdr = lerp(sdr, colEnamel, tPly);

                    // [NXT >] (prominent Next Scene button!)
                    float2 nxtMin = float2(rightBase - 156.0 * uiScale, pTop);
                    float2 nxtMax = float2(rightBase - 92.0 * uiScale, pBot);
                    if (px.x >= nxtMin.x && px.x <= nxtMax.x && px.y >= pTop && px.y <= pBot)
                    {
                        sdr = lerp(sdr, colJade * 0.28, 0.88);
                        if ((px.x - nxtMin.x < 1.5) || (nxtMax.x - px.x < 1.5) || (px.y - pTop < 1.5) || (pBot - px.y < 1.5))
                            sdr = colJade;
                    }
                    float tNxt = max(
                        sampleWord4(px, nxtMin + float2(7.0 * uiScale, 6.0 * uiScale), 1.55 * uiScale, 78u, 69u, 88u, 84u),
                        sampleGlyph(px, nxtMin + float2(48.0 * uiScale, 6.0 * uiScale), 1.55 * uiScale, 62u));
                    sdr = lerp(sdr, colEnamel, tNxt);
                }

                // Clickable Interactive Explore Mode Pill ("F:FILM" / "F:FREE")
                float2 expMin = float2(rightBase - 86.0 * uiScale, res.y - 34.0 * uiScale);
                float2 expMax = expMin + float2(78.0 * uiScale, 22.0 * uiScale);
                if (expMin.x > statX + 90.0 * uiScale)
                {
                    bool expActive = (gInteractiveMeta.x & 1u) != 0u;
                    float3 expCol = expActive ? colOrchid : colJade;
                    if (px.x >= expMin.x && px.x <= expMax.x && px.y >= expMin.y && px.y <= expMax.y)
                    {
                        sdr = lerp(sdr, expCol * (expActive ? 0.34 : 0.20), 0.88);
                        if ((px.x - expMin.x < 1.5) || (expMax.x - px.x < 1.5) ||
                            (px.y - expMin.y < 1.5) || (expMax.y - px.y < 1.5))
                        {
                            sdr = expCol;
                        }
                    }
                    float expTxt = expActive
                        ? sampleWord8(px, expMin + float2(9.0 * uiScale, 6.0 * uiScale), 1.5 * uiScale, 70u, 58u, 70u, 73u, 76u, 77u, 32u, 32u)
                        : sampleWord8(px, expMin + float2(9.0 * uiScale, 6.0 * uiScale), 1.5 * uiScale, 70u, 58u, 70u, 82u, 69u, 69u, 32u, 32u);
                    sdr = lerp(sdr, expActive ? colEnamel : expCol, expTxt);
                }
            }
        }
    }

    if (showOverlay && hudMode == 0u && res.x >= 480.0 && res.y >= 300.0)
    {
        const float uiScale = clamp(floor(res.y / 540.0), 1.0, 3.0);

        // Top-Left LIVE SCENE WORKLOADS Attribution Card (strictly live on-screen execution — NEVER standalone bench!)
        float liveTopY = (gVolumeAndPadding.x >= 0.0) ? (44.0 * uiScale) : (14.0 * uiScale);
        float2 liveMin  = float2(14.0 * uiScale, liveTopY);
        float2 liveSize = float2(306.0 * uiScale, 112.0 * uiScale);
        if (px.x >= liveMin.x && px.x <= liveMin.x + liveSize.x &&
            px.y >= liveMin.y && px.y <= liveMin.y + liveSize.y)
        {
            sdr = lerp(sdr, float3(0.016, 0.026, 0.048), 0.86);
            bool bdr = (px.x - liveMin.x < 1.5) || (liveMin.x + liveSize.x - px.x < 1.5) ||
                       (px.y - liveMin.y < 1.5) || (liveMin.y + liveSize.y - px.y < 1.5);
            if (bdr) sdr = lerp(colJade, colCyan, 0.35);

            // Header: "LIVE GPU" + column headers "SIZE"  "MS"  "RATE"
            float hdrTxt = sampleWord8(
                px, liveMin + float2(8.0 * uiScale, 7.0 * uiScale), 1.55 * uiScale,
                76u, 73u, 86u, 69u, 32u, 71u, 80u, 85u);
            sdr = lerp(sdr, colJade, hdrTxt);

            float colSzTxt = sampleWord4(px, liveMin + float2(128.0 * uiScale, 7.0 * uiScale), 1.4 * uiScale, 83u, 73u, 90u, 69u);
            float colMsTxt = sampleWord4(px, liveMin + float2(188.0 * uiScale, 7.0 * uiScale), 1.4 * uiScale, 32u, 77u, 83u, 32u);
            float colRtTxt = sampleWord4(px, liveMin + float2(246.0 * uiScale, 7.0 * uiScale), 1.4 * uiScale, 82u, 65u, 84u, 69u);
            sdr = lerp(sdr, colAmber, max(colSzTxt, max(colMsTxt, colRtTxt)));

            [unroll]
            for (uint p = 0u; p < 4u; ++p)
            {
                float rowY = liveMin.y + (23.0 + (float)p * 21.5) * uiScale;
                uint modeNib = (gLivePillarMeta.x >> (p * 4u)) & 0xFu;
                float3 mCol = livePillarModeColor(modeNib);
                float4 pm = gLivePillarMetrics[p];

                // Pillar name + runtime toggle key: "RT X", "CUDA", "TNSR", "CODC"
                uint n0 = 82u, n1 = 84u, n2 = 91u, n3 = 88u; // RT[X
                if (p == 1u) { n0 = 67u; n1 = 85u; n2 = 68u; n3 = 65u; } // CUDA
                if (p == 2u) { n0 = 84u; n1 = 78u; n2 = 83u; n3 = 82u; } // TNSR
                if (p == 3u) { n0 = 67u; n1 = 79u; n2 = 68u; n3 = 67u; } // CODC
                float nameMask = sampleWord4(px, float2(liveMin.x + 8.0 * uiScale, rowY), 1.55 * uiScale, n0, n1, n2, n3);
                sdr = lerp(sdr, colEnamel, nameMask);

                // Attribution pill: LIVE / SUB / OFF / FLT
                float2 pillMin = float2(liveMin.x + 54.0 * uiScale, rowY - 2.0 * uiScale);
                float2 pillMax = pillMin + float2(42.0 * uiScale, 13.0 * uiScale);
                if (px.x >= pillMin.x && px.x <= pillMax.x && px.y >= pillMin.y && px.y <= pillMax.y)
                {
                    sdr = lerp(sdr, mCol * 0.24, 0.85);
                    if ((px.x - pillMin.x < 1.2) || (pillMax.x - px.x < 1.2) ||
                        (px.y - pillMin.y < 1.2) || (pillMax.y - px.y < 1.2))
                    {
                        sdr = mCol;
                    }
                }
                uint s0 = 76u, s1 = 73u, s2 = 86u, s3 = 69u; // LIVE
                if (modeNib == 1u) { s0 = 83u; s1 = 85u; s2 = 66u; s3 = 32u; } // SUB
                if (modeNib == 2u) { s0 = 79u; s1 = 70u; s2 = 70u; s3 = 32u; } // OFF
                if (modeNib == 3u) { s0 = 70u; s1 = 76u; s2 = 84u; s3 = 32u; } // FLT
                float pillTxt = sampleWord4(px, pillMin + float2(4.0 * uiScale, 2.0 * uiScale), 1.45 * uiScale, s0, s1, s2, s3);
                sdr = lerp(sdr, mCol, pillTxt);

                // Workload scale, live ms, and live rate/quality
                float szMask = sampleFixedNumber6(px, float2(liveMin.x + 104.0 * uiScale, rowY), 1.5 * uiScale, pm.z);
                float msMask = sampleFixedMs6(px, float2(liveMin.x + 168.0 * uiScale, rowY), 1.5 * uiScale, pm.x);
                float rtMask = sampleFixedNumber6(px, float2(liveMin.x + 234.0 * uiScale, rowY), 1.5 * uiScale, pm.y);
                sdr = lerp(sdr, colEnamel, max(szMask, rtMask));
                sdr = lerp(sdr, (modeNib == 0u) ? colJade : colCyan, msMask);
            }
        }

        // Top-Right Standalone Offscreen Benchmark Card (`B` sweep — explicitly labeled "BENCH [B]")
        const bool benchActiveOrDone = ((gHudFlags & 64u) != 0u) || ((gHudFlags & 128u) != 0u);
        if (benchActiveOrDone)
        {
            float2 cardMin = float2(res.x - 278.0 * uiScale, 14.0 * uiScale);
            float2 cardSize = float2(264.0 * uiScale, 126.0 * uiScale);
            if (px.x >= cardMin.x && px.x <= cardMin.x + cardSize.x &&
                px.y >= cardMin.y && px.y <= cardMin.y + cardSize.y)
            {
                sdr = lerp(sdr, float3(0.025, 0.032, 0.062), 0.88);
                bool cardBorder = (px.x - cardMin.x < 1.5) || (cardMin.x + cardSize.x - px.x < 1.5) ||
                                  (px.y - cardMin.y < 1.5) || (cardMin.y + cardSize.y - px.y < 1.5);
                if (cardBorder) sdr = lerp(colAmber, colOrchid, 0.5);
            }

            float bTitle = max(
                sampleWord8(
                    px, cardMin + float2(10.0 * uiScale, 6.0 * uiScale), 1.5 * uiScale,
                    66u, 69u, 78u, 67u, 72u, 32u, 91u, 66u),
                sampleGlyph(px, cardMin + float2(70.0 * uiScale, 6.0 * uiScale), 1.5 * uiScale, 93u));
            sdr = lerp(sdr, colAmber, bTitle);

            float hdrFill = ((gHudFlags & 64u) != 0u) ? gBenchProgress : 1.0;
            float hdrBar = segmentedBar(px, cardMin + float2(96.0 * uiScale, 8.0 * uiScale), float2(156.0 * uiScale, 6.0 * uiScale), hdrFill, 20u);
            sdr = lerp(sdr, colAmber, hdrBar);

            float scores[4] = { gBenchPrimary.x, gBenchPrimary.y, gBenchPrimary.z, gBenchPrimary.w };
            float fills[4]  = { gBenchFill.x,    gBenchFill.y,    gBenchFill.z,    gBenchFill.w };
            float3 rowCols[4] = {
                float3(0.10, 0.94, 0.76),
                float3(0.28, 0.78, 1.00),
                float3(0.92, 0.30, 0.90),
                float3(1.00, 0.74, 0.18)
            };

            [unroll]
            for (uint b = 0u; b < 4u; ++b)
            {
                float rowY = cardMin.y + (26.0 + (float)b * 23.0) * uiScale;
                uint l0 = 82u, l1 = 84u, l2 = 32u, l3 = 32u;
                if (b == 1u) { l0 = 67u; l1 = 85u; l2 = 68u; l3 = 65u; }
                if (b == 2u) { l0 = 84u; l1 = 78u; l2 = 83u; l3 = 82u; }
                if (b == 3u) { l0 = 67u; l1 = 79u; l2 = 68u; l3 = 67u; }

                float lbl = sampleWord4(px, float2(cardMin.x + 10.0 * uiScale, rowY), 1.7 * uiScale, l0, l1, l2, l3);
                sdr = lerp(sdr, rowCols[b], lbl);

                float barMask = segmentedBar(
                    px,
                    float2(cardMin.x + 54.0 * uiScale, rowY + 1.0 * uiScale),
                    float2(126.0 * uiScale, 9.0 * uiScale),
                    fills[b],
                    16u);
                sdr = lerp(sdr, rowCols[b], barMask * 0.92);

                float numMask = sampleFixedNumber6(
                    px,
                    float2(cardMin.x + 190.0 * uiScale, rowY),
                    1.7 * uiScale,
                    scores[b]);
                sdr = lerp(sdr, colEnamel, numMask);
            }
        }
    }

    if (showOverlay && hudMode == 1u && px.x < 330.0 && px.y > res.y - 42.0) {
        sdr = lerp(sdr, float3(0.015, 0.025, 0.04), 0.85);
        sdr = lerp(sdr, colEnamel, sampleTimecodeMMSS(px, float2(12, res.y - 30), 2.0, gFilmTime));
        [unroll] for (uint e = 0u; e < 5u; ++e) {
            if (abs(px.x - (84.0 + (float)e * 17.0)) < 5.0 && abs(px.y - (res.y - 23.0)) < 5.0)
                sdr = badgeStatusColor((gEnginePacked >> (e * 4u)) & 15u);
        }
        sdr = lerp(sdr, colAmber, sampleFixedNumber6(px, float2(190, res.y - 30), 2.0, gGpuMs));
    }
    if (showOverlay && hudMode == 1u) {
        [unroll] for (uint i = 0u; i < 16u; ++i) {
            float ms = gRecentGpuMs[i / 4u][i % 4u];
            float bar = verticalBar(px, float2(255.0 + (float)i * 4.0, res.y - 34.0), float2(3, 24), saturate(ms / 16.667));
            sdr = lerp(sdr, ms > 16.667 ? colAmber : colJade, bar);
        }
    }
    if (showOverlay && gVolumeAndPadding.x >= 0.0 && px.y < 38.0 && px.x < 190.0) {
        sdr *= 0.25;
        sdr = lerp(sdr, colEnamel, sampleWord4(px, float2(12, 12), 2, 86u, 79u, 76u, 58u));
        sdr = lerp(sdr, colJade, sampleFixedNumber6(px, float2(62, 12), 2, gVolumeAndPadding.x));
    }
    if (showOverlay && hudMode == 2u) {
        const float uiScale = clamp(floor(res.y / 540.0), 1.0, 3.0);
        const float splitRatio = clamp(gInteractiveState.x, 0.05, 0.95);
        float splitX = lbMin.x + lbSize.x * splitRatio;
        if (abs(px.x - splitX) < 1.35 && px.y >= lbMin.y && px.y < lbMin.y + lbSize.y)
            sdr = colEnamel;

        // Draggable RTX Split Curtain Center Grab Handle
        float2 handleCenter = float2(splitX, lbMin.y + lbSize.y * 0.5);
        float2 handleHalf   = float2(6.5 * uiScale, 20.0 * uiScale);
        if (abs(px.x - handleCenter.x) <= handleHalf.x && abs(px.y - handleCenter.y) <= handleHalf.y)
        {
            sdr = lerp(sdr, float3(0.018, 0.028, 0.052), 0.92);
            bool hb = (handleHalf.x - abs(px.x - handleCenter.x) < 1.4) ||
                      (handleHalf.y - abs(px.y - handleCenter.y) < 1.4);
            if (hb) sdr = colJade;
            else if (abs(abs(px.x - handleCenter.x) - 2.2 * uiScale) < 0.7 && abs(px.y - handleCenter.y) < 11.0 * uiScale)
                sdr = colEnamel;
        }

        float4 pm = gLivePillarMetrics[abEngine];
        float2 bannerSz    = float2(262.0 * uiScale, 28.0 * uiScale);
        float2 leftBoxMin  = float2(lbMin.x + 14.0 * uiScale, lbMin.y + 14.0 * uiScale);
        float  rightMaxX   = max(lbMin.x + 14.0 * uiScale, lbMin.x + lbSize.x - bannerSz.x - 14.0 * uiScale);
        float2 rightBoxMin = float2(clamp(max(splitX + 14.0 * uiScale, leftBoxMin.x + bannerSz.x + 12.0 * uiScale), lbMin.x + 14.0 * uiScale, rightMaxX), lbMin.y + 14.0 * uiScale);

        if (px.x >= leftBoxMin.x && px.x <= leftBoxMin.x + bannerSz.x &&
            px.y >= leftBoxMin.y && px.y <= leftBoxMin.y + bannerSz.y)
        {
            sdr = lerp(sdr, float3(0.015, 0.025, 0.045), 0.86);
            if ((px.x - leftBoxMin.x < 1.5) || (leftBoxMin.x + bannerSz.x - px.x < 1.5) ||
                (px.y - leftBoxMin.y < 1.5) || (leftBoxMin.y + bannerSz.y - px.y < 1.5))
                sdr = abLeftHwVerified ? colJade : colAmber;
        }
        if (px.x >= rightBoxMin.x && px.x <= rightBoxMin.x + bannerSz.x &&
            px.y >= rightBoxMin.y && px.y <= rightBoxMin.y + bannerSz.y)
        {
            sdr = lerp(sdr, float3(0.015, 0.025, 0.045), 0.86);
            if ((px.x - rightBoxMin.x < 1.5) || (rightBoxMin.x + bannerSz.x - px.x < 1.5) ||
                (px.y - rightBoxMin.y < 1.5) || (rightBoxMin.y + bannerSz.y - px.y < 1.5))
                sdr = colOrchid;
        }

        // Left banner label: "LIVE DXR" / "CUDA 65K" / "TNSR MMA" / "NVEC H264" + live ms + throughput/PSNR
        uint lc0 = 76u, lc1 = 73u, lc2 = 86u, lc3 = 69u, lc4 = 32u, lc5 = 68u, lc6 = 88u, lc7 = 82u;
        uint rc0 = 83u, rc1 = 85u, rc2 = 66u, rc3 = 32u, rc4 = 82u, rc5 = 65u, rc6 = 83u, rc7 = 84u;
        if (abEngine == 0u && !abLeftHwVerified) {
            lc0 = 83u; lc1 = 85u; lc2 = 66u; lc3 = 32u; lc4 = 82u; lc5 = 65u; lc6 = 83u; lc7 = 84u;
        }
        if (abEngine == 1u) {
            lc0 = 67u; lc1 = 85u; lc2 = 68u; lc3 = 65u; lc4 = 32u; lc5 = 54u; lc6 = 53u; lc7 = 75u; // CUDA 65K
            rc0 = 83u; rc1 = 85u; rc2 = 66u; rc3 = 32u; rc4 = 32u; rc5 = 56u; rc6 = 75u; rc7 = 32u; // SUB  8K
        }
        if (abEngine == 2u) {
            lc0 = 84u; lc1 = 78u; lc2 = 83u; lc3 = 82u; lc4 = 32u; lc5 = 77u; lc6 = 77u; lc7 = 65u; // TNSR MMA
            rc0 = 83u; rc1 = 85u; rc2 = 66u; rc3 = 32u; rc4 = 72u; rc5 = 76u; rc6 = 83u; rc7 = 76u; // SUB HLSL
        }
        if (abEngine == 3u) {
            lc0 = 67u; lc1 = 79u; lc2 = 68u; lc3 = 67u; lc4 = 32u; lc5 = 49u; lc6 = 77u; lc7 = 66u; // CODC 1MB
            rc0 = 67u; rc1 = 76u; rc2 = 69u; rc3 = 65u; rc4 = 78u; rc5 = 49u; rc6 = 48u; rc7 = 66u; // CLEAN10B
        }

        float lTxt = sampleWord8(px, leftBoxMin + float2(8.0 * uiScale, 8.0 * uiScale), 1.8 * uiScale, lc0, lc1, lc2, lc3, lc4, lc5, lc6, lc7);
        float lMs  = sampleFixedMs6(px, leftBoxMin + float2(96.0 * uiScale, 8.0 * uiScale), 1.8 * uiScale, pm.x);
        float lVal = sampleFixedNumber6(px, leftBoxMin + float2(168.0 * uiScale, 8.0 * uiScale), 1.8 * uiScale, (abEngine == 3u) ? pm.w : pm.y);
        sdr = lerp(sdr, abLeftHwVerified ? colJade : colAmber, lTxt);
        sdr = lerp(sdr, colEnamel, max(lMs, lVal));

        float rTxt = sampleWord8(px, rightBoxMin + float2(8.0 * uiScale, 8.0 * uiScale), 1.8 * uiScale, rc0, rc1, rc2, rc3, rc4, rc5, rc6, rc7);
        sdr = lerp(sdr, colOrchid, rTxt);
    }

    // Persistent Bottom Interactive NVIDIA RTX Studio Dock (Pass 4 only; anchored right above Bottom Bar even during WIPE!)
    const bool studioDockShown = showOverlay && (hudMode != 3u) && studioActive && (res.x >= 640.0) && (res.y >= 300.0);
    if (studioDockShown)
    {
        const float u = clamp(floor(res.y / 540.0), 1.0, 3.0);
        const float2 barSize = float2(536.0 * u, 54.0 * u);
        const float barMinX  = 0.5 * (res.x - barSize.x);
        const float barMinY  = (hudMode == 1u) ? (res.y - 60.0 * u) : (res.y - 104.0 * u);
        const float2 barMin  = float2(barMinX, barMinY);
        const uint   trkSlot = gInteractiveMeta.z & 0xFFu;
        const float  trkToastAlpha = saturate((float)((gInteractiveMeta.z >> 8u) & 0xFFu) / 255.0);

        if (px.x >= barMin.x && px.x <= barMin.x + barSize.x &&
            px.y >= barMin.y && px.y <= barMin.y + barSize.y)
        {
            const bool expOn    = (gInteractiveMeta.x & 1u) != 0u;
            const bool sunOvr   = (gInteractiveMeta.x & 2u) != 0u;
            const bool lampsOff = (gInteractiveMeta.x & 4u) != 0u;
            const bool nrdOn    = (gInteractiveMeta.x & 8u) != 0u;
            const uint presetId = (gInteractiveMeta.x >> 4u) & 0xFu;
            const bool isAccum   = (gInteractiveMeta.x & 256u) != 0u;
            const bool customMp3 = (gInteractiveMeta.x & (1u << 28u)) != 0u;
            const bool audMuted  = (gInteractiveMeta.x & (1u << 29u)) != 0u;
            const bool isReblur  = (curDenMode == 1u);
            const uint sppCount = (uint)(clamp(gInteractiveState.w, 1.0, 9999.0) + 0.5);

            sdr = lerp(sdr, float3(0.015, 0.024, 0.046), 0.91);
            bool outerBdr = (px.x - barMin.x < 1.5) || (barMin.x + barSize.x - px.x < 1.5) ||
                            (px.y - barMin.y < 1.5) || (barMin.y + barSize.y - px.y < 1.5);
            if (outerBdr) sdr = lerp(colJade, colOrchid, 0.35);

            // Row 1: [FREE/FILM] [RBLR/NRD + SPP] [LAMP] [WIPE] [AOV] [MRBL] [<] [1..6] [>] [SCENE NAME]
            const float r1MinY = barMin.y + 5.0 * u;
            const float r1MaxY = barMin.y + 23.0 * u;

            // 1. FREE / FILM camera mode pill (rx = 6..56)
            float2 bExpMin = float2(barMin.x + 6.0 * u, r1MinY);
            float2 bExpMax = float2(barMin.x + 56.0 * u, r1MaxY);
            float3 expC = expOn ? colJade : colAmber;
            if (px.x >= bExpMin.x && px.x <= bExpMax.x && px.y >= bExpMin.y && px.y <= bExpMax.y)
            {
                sdr = lerp(sdr, expC * 0.24, 0.85);
                if ((px.x - bExpMin.x < 1.2) || (bExpMax.x - px.x < 1.2) || (px.y - bExpMin.y < 1.2) || (bExpMax.y - px.y < 1.2))
                    sdr = expC;
            }
            float tExp = expOn
                ? sampleWord4(px, barMin + float2(16.0 * u, 9.5 * u), 1.35 * u, 70u, 82u, 69u, 69u) // FREE
                : sampleWord4(px, barMin + float2(16.0 * u, 9.5 * u), 1.35 * u, 70u, 73u, 76u, 77u); // FILM
            sdr = lerp(sdr, expC, tExp);

            // 2. REBLUR / NRD / ACC denoiser + live SPP convergence readout (rx = 60..152)
            float2 bSppMin = float2(barMin.x + 60.0 * u, r1MinY);
            float2 bSppMax = float2(barMin.x + 152.0 * u, r1MaxY);
            float3 nrdC = isReblur ? colCyan : (nrdOn ? colJade : colCyan);
            bool denActive = isAccum || isReblur;
            if (px.x >= bSppMin.x && px.x <= bSppMax.x && px.y >= bSppMin.y && px.y <= bSppMax.y)
            {
                sdr = lerp(sdr, nrdC * (denActive ? 0.24 : 0.10), 0.85);
                if ((px.x - bSppMin.x < 1.2) || (bSppMax.x - px.x < 1.2) || (px.y - bSppMin.y < 1.2) || (bSppMax.y - px.y < 1.2))
                    sdr = nrdC * (denActive ? 1.0 : 0.65);
            }
            float tNrd = isReblur
                ? sampleWord4(px, barMin + float2(63.0 * u, 9.5 * u), 1.35 * u, 82u, 66u, 76u, 82u) // RBLR
                : (nrdOn
                    ? sampleWord4(px, barMin + float2(64.0 * u, 9.5 * u), 1.35 * u, 78u, 82u, 68u, 32u)  // NRD
                    : sampleWord4(px, barMin + float2(64.0 * u, 9.5 * u), 1.35 * u, 65u, 67u, 67u, 32u)); // ACC
            float tSppNum = sampleUint4(px, barMin + float2(91.0 * u, 9.5 * u), 1.35 * u, sppCount);
            float tSppLbl = sampleWord4(px, barMin + float2(121.0 * u, 9.5 * u), 1.35 * u, 83u, 80u, 80u, 32u);
            sdr = lerp(sdr, nrdC, tNrd);
            sdr = lerp(sdr, colEnamel, tSppNum);
            sdr = lerp(sdr, isAccum ? colAmber : (isReblur ? colJade : float3(0.58, 0.64, 0.72)), tSppLbl);

            // 3. Practical LAMP [J] toggle pill (rx = 156..196)
            float2 bLmpMin = float2(barMin.x + 156.0 * u, r1MinY);
            float2 bLmpMax = float2(barMin.x + 196.0 * u, r1MaxY);
            float3 lmpC = (!lampsOff) ? colAmber : float3(0.42, 0.46, 0.56);
            if (px.x >= bLmpMin.x && px.x <= bLmpMax.x && px.y >= bLmpMin.y && px.y <= bLmpMax.y)
            {
                sdr = lerp(sdr, lmpC * ((!lampsOff) ? 0.24 : 0.12), 0.85);
                if ((px.x - bLmpMin.x < 1.2) || (bLmpMax.x - px.x < 1.2) || (px.y - bLmpMin.y < 1.2) || (bLmpMax.y - px.y < 1.2))
                    sdr = lmpC;
            }
            float tLmp = sampleWord4(px, barMin + float2(162.0 * u, 9.5 * u), 1.35 * u, 76u, 65u, 77u, 80u);
            sdr = lerp(sdr, lmpC, tLmp);

            // 4. RTX Split WIPE [H] toggle pill (rx = 200..240 — persistent in exact same spot!)
            float2 bWipMin = float2(barMin.x + 200.0 * u, r1MinY);
            float2 bWipMax = float2(barMin.x + 240.0 * u, r1MaxY);
            bool wipeOn = (hudMode == 2u);
            float3 wipC = wipeOn ? colOrchid : colCyan;
            if (px.x >= bWipMin.x && px.x <= bWipMax.x && px.y >= bWipMin.y && px.y <= bWipMax.y)
            {
                sdr = lerp(sdr, wipC * (wipeOn ? 0.36 : 0.14), 0.85);
                if ((px.x - bWipMin.x < 1.2) || (bWipMax.x - px.x < 1.2) || (px.y - bWipMin.y < 1.2) || (bWipMax.y - px.y < 1.2))
                    sdr = wipC;
            }
            float tWip = sampleWord4(px, barMin + float2(206.0 * u, 9.5 * u), 1.35 * u, 87u, 73u, 80u, 69u);
            sdr = lerp(sdr, wipeOn ? colEnamel : wipC, tWip);

            // 5. Buffer / AOV Inspection [I / F9] pill (rx = 244..284)
            float2 bAovMin = float2(barMin.x + 244.0 * u, r1MinY);
            float2 bAovMax = float2(barMin.x + 284.0 * u, r1MaxY);
            bool aovOn = (curAovMode != 0u);
            float3 aovC = aovOn ? colAmber : float3(0.56, 0.66, 0.78);
            if (px.x >= bAovMin.x && px.x <= bAovMax.x && px.y >= bAovMin.y && px.y <= bAovMax.y)
            {
                sdr = lerp(sdr, aovC * (aovOn ? 0.32 : 0.14), 0.85);
                if ((px.x - bAovMin.x < 1.2) || (bAovMax.x - px.x < 1.2) || (px.y - bAovMin.y < 1.2) || (bAovMax.y - px.y < 1.2))
                    sdr = aovC;
            }
            float tAov = sampleWord4(px, barMin + float2(250.0 * u, 9.5 * u), 1.35 * u, 65u, 79u, 86u, aovOn ? (48u + curAovMode) : 32u);
            sdr = lerp(sdr, aovOn ? colEnamel : aovC, tAov);

            // 6. Marbles RTX Physics Cascade [Y / F10 / M] pill (rx = 288..330)
            float2 bMrbMin = float2(barMin.x + 288.0 * u, r1MinY);
            float2 bMrbMax = float2(barMin.x + 330.0 * u, r1MaxY);
            bool mrbOn = (curMarbleCnt != 0u);
            float3 mrbC = mrbOn ? colOrchid : colJade;
            if (px.x >= bMrbMin.x && px.x <= bMrbMax.x && px.y >= bMrbMin.y && px.y <= bMrbMax.y)
            {
                sdr = lerp(sdr, mrbC * (mrbOn ? 0.32 : 0.16), 0.85);
                if ((px.x - bMrbMin.x < 1.2) || (bMrbMax.x - px.x < 1.2) || (px.y - bMrbMin.y < 1.2) || (bMrbMax.y - px.y < 1.2))
                    sdr = mrbC;
            }
            float tMrb = sampleWord4(px, barMin + float2(295.0 * u, 9.5 * u), 1.35 * u, 77u, 82u, 66u, 76u);
            sdr = lerp(sdr, mrbOn ? colEnamel : mrbC, tMrb);

            // 7. Prominent Scene Switcher: [<] (336..354) + [1..6] (357..444) + [>] (447..465) + Scene Name (470..530)
            float2 bScnPrvMin = float2(barMin.x + 336.0 * u, r1MinY);
            float2 bScnPrvMax = float2(barMin.x + 354.0 * u, r1MaxY);
            if (px.x >= bScnPrvMin.x && px.x <= bScnPrvMax.x && px.y >= r1MinY && px.y <= r1MaxY)
            {
                sdr = lerp(sdr, colCyan * 0.22, 0.85);
                if ((px.x - bScnPrvMin.x < 1.1) || (bScnPrvMax.x - px.x < 1.1) || (px.y - r1MinY < 1.1) || (r1MaxY - px.y < 1.1))
                    sdr = colCyan;
            }
            sdr = lerp(sdr, colEnamel, sampleGlyph(px, bScnPrvMin + float2(6.0 * u, 4.8 * u), 1.45 * u, 60u));

            [unroll]
            for (uint k = 0u; k < 6u; ++k)
            {
                float2 cMin = float2(barMin.x + (357.0 + (float)k * 14.5) * u, r1MinY);
                float2 cMax = cMin + float2(12.5 * u, 18.0 * u);
                bool sel = (presetId == (k + 1u));
                float3 cCol = sel ? colJade : float3(0.50, 0.58, 0.68);
                if (px.x >= cMin.x && px.x <= cMax.x && px.y >= cMin.y && px.y <= cMax.y)
                {
                    sdr = lerp(sdr, cCol * (sel ? 0.34 : 0.12), 0.85);
                    if ((px.x - cMin.x < 1.1) || (cMax.x - px.x < 1.1) || (px.y - cMin.y < 1.1) || (cMax.y - px.y < 1.1))
                        sdr = cCol;
                }
                float dMask = sampleGlyph(px, cMin + float2(3.4 * u, 4.8 * u), 1.45 * u, 49u + k);
                sdr = lerp(sdr, sel ? colEnamel : cCol, dMask);
            }

            float2 bScnNxtMin = float2(barMin.x + 447.0 * u, r1MinY);
            float2 bScnNxtMax = float2(barMin.x + 465.0 * u, r1MaxY);
            if (px.x >= bScnNxtMin.x && px.x <= bScnNxtMax.x && px.y >= r1MinY && px.y <= r1MaxY)
            {
                sdr = lerp(sdr, colJade * 0.26, 0.85);
                if ((px.x - bScnNxtMin.x < 1.1) || (bScnNxtMax.x - px.x < 1.1) || (px.y - r1MinY < 1.1) || (r1MaxY - px.y < 1.1))
                    sdr = colJade;
            }
            sdr = lerp(sdr, colEnamel, sampleGlyph(px, bScnNxtMin + float2(6.0 * u, 4.8 * u), 1.45 * u, 62u));

            // Active Scene Name Readout ("BATHRM", "FAUCET", "LEAVES", "ORCHID", "ENAMEL", "SANCTU", or "ACT 1..7")
            uint sn0=65u, sn1=67u, sn2=84u, sn3=32u, sn4=49u, sn5=32u;
            if (!expOn) {
                uint curActNum = (gFilmTime < 45.0) ? 1u : (gFilmTime < 100.0) ? 2u : (gFilmTime < 150.0) ? 3u :
                                 (gFilmTime < 195.0) ? 4u : (gFilmTime < 245.0) ? 5u : (gFilmTime < 275.0) ? 6u : 7u;
                sn4 = 48u + curActNum;
            } else if (presetId == 1u) { sn0=66u; sn1=65u; sn2=84u; sn3=72u; sn4=82u; sn5=77u; } // BATHRM
            else if (presetId == 2u)   { sn0=70u; sn1=65u; sn2=85u; sn3=67u; sn4=69u; sn5=84u; } // FAUCET
            else if (presetId == 3u)   { sn0=76u; sn1=69u; sn2=65u; sn3=86u; sn4=69u; sn5=83u; } // LEAVES
            else if (presetId == 4u)   { sn0=79u; sn1=82u; sn2=67u; sn3=72u; sn4=73u; sn5=68u; } // ORCHID
            else if (presetId == 5u)   { sn0=69u; sn1=78u; sn2=65u; sn3=77u; sn4=69u; sn5=76u; } // ENAMEL
            else if (presetId == 6u)   { sn0=83u; sn1=65u; sn2=78u; sn3=67u; sn4=84u; sn5=85u; } // SANCTU
            else                       { sn0=67u; sn1=85u; sn2=83u; sn3=84u; sn4=79u; sn5=77u; } // CUSTOM
            float tScnName = sampleWord8(px, barMin + float2(470.0 * u, 9.5 * u), 1.35 * u, sn0, sn1, sn2, sn3, sn4, sn5, 32u, 32u);
            sdr = lerp(sdr, expOn ? colJade : colAmber, tScnName);

            // Row 2: Draggable [SUN] (34..152) + [AZM] (186..304) + [MP3] (312..350) + [MUSC/MUTE] (354..396) + [RST] (400..438) + [NEXT SCN >] (442..530)
            float tSunLbl = sampleWord4(px, barMin + float2(8.0 * u, 34.0 * u), 1.35 * u, 83u, 85u, 78u, 32u);
            sdr = lerp(sdr, sunOvr ? colAmber : float3(0.68, 0.72, 0.80), tSunLbl);

            float2 sunMin  = barMin + float2(34.0 * u, 34.0 * u);
            float2 sunSize = float2(118.0 * u, 8.0 * u);
            float  sunVal  = saturate(gInteractiveState.y);
            float  sunBar  = segmentedBar(px, sunMin, sunSize, sunVal, 16u);
            float3 sunTint = lerp(float3(0.26, 0.42, 0.88), lerp(colAmber, colEnamel, smoothstep(0.45, 1.0, sunVal)), smoothstep(0.0, 0.55, sunVal));
            sdr = lerp(sdr, sunTint, sunBar * 0.92);
            float sunThumbX = sunMin.x + sunVal * sunSize.x;
            if (abs(px.x - sunThumbX) <= 2.4 * u && px.y >= sunMin.y - 2.5 * u && px.y <= sunMin.y + sunSize.y + 2.5 * u)
                sdr = colEnamel;

            float tAzmLbl = sampleWord4(px, barMin + float2(160.0 * u, 34.0 * u), 1.35 * u, 65u, 90u, 77u, 32u);
            sdr = lerp(sdr, sunOvr ? colCyan : float3(0.68, 0.72, 0.80), tAzmLbl);

            float2 azmMin  = barMin + float2(186.0 * u, 34.0 * u);
            float2 azmSize = float2(118.0 * u, 8.0 * u);
            float  azmVal  = saturate(gInteractiveState.z);
            float  azmBar  = segmentedBar(px, azmMin, azmSize, azmVal, 16u);
            float3 azmTint = colCyan;
            sdr = lerp(sdr, azmTint, azmBar * 0.90);
            float azmThumbX = azmMin.x + azmVal * azmSize.x;
            if (abs(px.x - azmThumbX) <= 2.4 * u && px.y >= azmMin.y - 2.5 * u && px.y <= azmMin.y + azmSize.y + 2.5 * u)
                sdr = colEnamel;

            // [TRK1..4 / SYNT / MP3+] Built-In Soundtrack Selector (rx = 310..350; LMB/Z cycles tracks, Shift+Click imports MP3)
            float2 bMp3Min = barMin + float2(310.0 * u, 29.5 * u);
            float2 bMp3Max = bMp3Min + float2(40.0 * u, 17.5 * u);
            bool isSongTrack = (trkSlot >= 1u && trkSlot <= 5u) || customMp3;
            float3 mp3C = (trkSlot == 5u || (customMp3 && trkSlot == 0u)) ? colOrchid : (isSongTrack ? colJade : colCyan);
            if (px.x >= bMp3Min.x && px.x <= bMp3Max.x && px.y >= bMp3Min.y && px.y <= bMp3Max.y)
            {
                sdr = lerp(sdr, mp3C * (isSongTrack ? 0.30 : 0.16), 0.86);
                if ((px.x - bMp3Min.x < 1.1) || (bMp3Max.x - px.x < 1.1) || (px.y - bMp3Min.y < 1.1) || (bMp3Max.y - px.y < 1.1))
                    sdr = mp3C;
            }
            uint tc0 = 83u, tc1 = 89u, tc2 = 78u, tc3 = 84u; // SYNT
            if (trkSlot >= 1u && trkSlot <= 4u) { tc0 = 84u; tc1 = 82u; tc2 = 75u; tc3 = 48u + trkSlot; } // TRK1..TRK4
            else if (trkSlot == 5u || customMp3) { tc0 = 77u; tc1 = 80u; tc2 = 51u; tc3 = 43u; }          // MP3+
            float tMp3 = sampleWord4(px, bMp3Min + float2(7.0 * u, 4.8 * u), 1.35 * u, tc0, tc1, tc2, tc3);
            sdr = lerp(sdr, isSongTrack ? colEnamel : mp3C, tMp3);

            // [MUSC] / [MUTE] Live Music Toggle Pill (rx = 354..396)
            float2 bMusMin = barMin + float2(354.0 * u, 29.5 * u);
            float2 bMusMax = bMusMin + float2(42.0 * u, 17.5 * u);
            float3 musC = audMuted ? colAmber : colJade;
            if (px.x >= bMusMin.x && px.x <= bMusMax.x && px.y >= bMusMin.y && px.y <= bMusMax.y)
            {
                sdr = lerp(sdr, musC * (audMuted ? 0.28 : 0.22), 0.86);
                if ((px.x - bMusMin.x < 1.1) || (bMusMax.x - px.x < 1.1) || (px.y - bMusMin.y < 1.1) || (bMusMax.y - px.y < 1.1))
                    sdr = musC;
            }
            float tMus = audMuted
                ? sampleWord4(px, bMusMin + float2(5.5 * u, 4.8 * u), 1.35 * u, 77u, 85u, 84u, 69u)  // MUTE
                : sampleWord4(px, bMusMin + float2(5.5 * u, 4.8 * u), 1.35 * u, 77u, 85u, 83u, 67u); // MUSC
            sdr = lerp(sdr, audMuted ? colEnamel : musC, tMus);

            // [RST] Lighting & Studio Reset Pill (rx = 400..438)
            float2 bRstMin = barMin + float2(400.0 * u, 29.5 * u);
            float2 bRstMax = bRstMin + float2(38.0 * u, 17.5 * u);
            bool anyOvr = sunOvr || lampsOff || (curAovMode != 0u) || (curMarbleCnt != 0u) || (curSolo != 0u);
            float3 rstC = anyOvr ? colAmber : float3(0.42, 0.46, 0.56);
            if (px.x >= bRstMin.x && px.x <= bRstMax.x && px.y >= bRstMin.y && px.y <= bRstMax.y)
            {
                sdr = lerp(sdr, rstC * (anyOvr ? 0.24 : 0.10), 0.85);
                if ((px.x - bRstMin.x < 1.1) || (bRstMax.x - px.x < 1.1) || (px.y - bRstMin.y < 1.1) || (bRstMax.y - px.y < 1.1))
                    sdr = rstC;
            }
            float tRst = sampleWord4(px, bRstMin + float2(7.5 * u, 4.8 * u), 1.35 * u, 82u, 83u, 84u, 32u);
            sdr = lerp(sdr, rstC, tRst);

            // Prominent Row-2 [NEXT SCN >] One-Click Scene Button (rx = 442..530)
            float2 bNxtScnMin = barMin + float2(442.0 * u, 29.5 * u);
            float2 bNxtScnMax = bNxtScnMin + float2(88.0 * u, 17.5 * u);
            if (px.x >= bNxtScnMin.x && px.x <= bNxtScnMax.x && px.y >= bNxtScnMin.y && px.y <= bNxtScnMax.y)
            {
                sdr = lerp(sdr, colJade * 0.28, 0.88);
                if ((px.x - bNxtScnMin.x < 1.2) || (bNxtScnMax.x - px.x < 1.2) || (px.y - bNxtScnMin.y < 1.2) || (bNxtScnMax.y - px.y < 1.2))
                    sdr = colJade;
            }
            float tNxtScn = max(
                sampleWord8(px, bNxtScnMin + float2(6.0 * u, 4.8 * u), 1.35 * u, 78u, 69u, 88u, 84u, 32u, 83u, 67u, 78u),
                sampleGlyph(px, bNxtScnMin + float2(75.0 * u, 4.8 * u), 1.35 * u, 62u));
            sdr = lerp(sdr, colEnamel, tNxtScn);
        }

        // Pass-4 AOV Buffer Inspection Pill Banner (placed cleanly right above the Bottom Studio Dock!)
        if (curAovMode != 0u)
        {
            float2 aovBoxSz  = float2(174.0 * u, 20.0 * u);
            float2 aovBoxMin = float2(barMin.x + 0.5 * (barSize.x - aovBoxSz.x), max(12.0 * u, barMin.y - 25.0 * u));
            if (px.x >= aovBoxMin.x && px.x <= aovBoxMin.x + aovBoxSz.x &&
                px.y >= aovBoxMin.y && px.y <= aovBoxMin.y + aovBoxSz.y)
            {
                sdr = lerp(sdr, float3(0.012, 0.020, 0.038), 0.90);
                if ((px.x - aovBoxMin.x < 1.3) || (aovBoxMin.x + aovBoxSz.x - px.x < 1.3) ||
                    (px.y - aovBoxMin.y < 1.3) || (aovBoxMin.y + aovBoxSz.y - px.y < 1.3))
                    sdr = colAmber;
                uint a0=65u, a1=79u, a2=86u, a3=58u;
                uint b0=65u, b1=76u, b2=66u, b3=69u, b4=68u, b5=79u, b6=32u, b7=32u; // ALBEDO
                if (curAovMode == 2u) { b0=78u; b1=79u; b2=82u; b3=77u; b4=65u; b5=76u; b6=32u; b7=32u; } // NORMAL
                if (curAovMode == 3u) { b0=82u; b1=71u; b2=72u; b3=47u; b4=77u; b5=84u; b6=76u; b7=32u; } // RGH/MTL
                if (curAovMode == 4u) { b0=68u; b1=73u; b2=82u; b3=69u; b4=67u; b5=84u; b6=32u; b7=32u; } // DIRECT
                if (curAovMode == 5u) { b0=73u; b1=78u; b2=68u; b3=73u; b4=82u; b5=32u; b6=71u; b7=73u; } // INDIR GI
                if (curAovMode == 6u) { b0=82u; b1=69u; b2=70u; b3=76u; b4=47u; b5=82u; b6=70u; b7=82u; } // REFL/RFR
                if (curAovMode == 7u) { b0=82u; b1=65u; b2=89u; b3=32u; b4=72u; b5=69u; b6=65u; b7=84u; } // RAY HEAT
                float tLbl = sampleWord4(px, aovBoxMin + float2(10.0 * u, 5.5 * u), 1.45 * u, a0, a1, a2, a3);
                float tMod = sampleWord8(px, aovBoxMin + float2(48.0 * u, 5.5 * u), 1.45 * u, b0, b1, b2, b3, b4, b5, b6, b7);
                sdr = lerp(sdr, colAmber, tLbl);
                sdr = lerp(sdr, colEnamel, tMod);
            }
        }

        // Pass-4 Now-Playing Soundtrack Toast Banner (left-aligned above Studio Dock; fades out smoothly)
        if (trkToastAlpha > 0.01)
        {
            float2 ostBoxSz  = float2(176.0 * u, 20.0 * u);
            float2 ostBoxMin = float2(barMin.x, max(12.0 * u, barMin.y - 25.0 * u));
            if (px.x >= ostBoxMin.x && px.x <= ostBoxMin.x + ostBoxSz.x &&
                px.y >= ostBoxMin.y && px.y <= ostBoxMin.y + ostBoxSz.y)
            {
                float3 bdrC = (trkSlot == 5u) ? colOrchid : ((trkSlot >= 1u && trkSlot <= 4u) ? colJade : colCyan);
                sdr = lerp(sdr, float3(0.010, 0.022, 0.040), 0.90 * trkToastAlpha);
                if ((px.x - ostBoxMin.x < 1.3) || (ostBoxMin.x + ostBoxSz.x - px.x < 1.3) ||
                    (px.y - ostBoxMin.y < 1.3) || (ostBoxMin.y + ostBoxSz.y - px.y < 1.3))
                    sdr = lerp(sdr, bdrC, trkToastAlpha);

                uint w0a=80u, w0b=82u, w0c=79u, w0d=67u, w0e=69u, w0f=68u, w0g=85u, w0h=82u; // PROCEDUR
                uint w1a=65u, w1b=76u, w1c=32u, w1d=82u, w1e=84u, w1f=88u, w1g=32u, w1h=83u; // AL RTX S
                uint w2a=89u, w2b=78u, w2c=84u, w2d=72u, w2e=32u, w2f=32u, w2g=32u, w2h=32u; // YNTH
                if (trkSlot == 1u) {
                    w0a=49u; w0b=47u; w0c=52u; w0d=32u; w0e=78u; w0f=69u; w0g=79u; w0h=78u; // 1/4 NEON
                    w1a=32u; w1b=79u; w1c=82u; w1d=67u; w1e=72u; w1f=73u; w1g=68u; w1h=32u; //  ORCHID
                    w2a=83u; w2b=80u; w2c=89u; w2d=32u; w2e=74u; w2f=65u; w2g=90u; w2h=90u; // SPY JAZZ
                } else if (trkSlot == 2u) {
                    w0a=50u; w0b=47u; w0c=52u; w0d=32u; w0e=65u; w0f=67u; w0g=73u; w0h=68u; // 2/4 ACID
                    w1a=32u; w1b=76u; w1c=79u; w1d=84u; w1e=85u; w1f=83u; w1g=32u; w1h=84u; //  LOTUS T
                    w2a=69u; w2b=67u; w2c=72u; w2d=78u; w2e=79u; w2f=32u; w2g=32u; w2h=32u; // ECHNO
                } else if (trkSlot == 3u) {
                    w0a=51u; w0b=47u; w0c=52u; w0d=32u; w0e=86u; w0f=69u; w0g=76u; w0h=86u; // 3/4 VELV
                    w1a=69u; w1b=84u; w1c=32u; w1d=67u; w1e=73u; w1f=80u; w1g=72u; w1h=69u; // ET CIPHE
                    w2a=82u; w2b=32u; w2c=70u; w2d=85u; w2e=78u; w2f=75u; w2g=32u; w2h=32u; // R FUNK
                } else if (trkSlot == 4u) {
                    w0a=52u; w0b=47u; w0c=52u; w0d=32u; w0e=71u; w0f=76u; w0g=65u; w0h=83u; // 4/4 GLAS
                    w1a=83u; w1b=32u; w1c=68u; w1d=82u; w1e=65u; w1f=71u; w1g=79u; w1h=78u; // S DRAGON
                    w2a=32u; w2b=69u; w2c=76u; w2d=69u; w2e=67u; w2f=84u; w2g=82u; w2h=79u; //  ELECTRO
                } else if (trkSlot == 5u) {
                    w0a=67u; w0b=85u; w0c=83u; w0d=84u; w0e=79u; w0f=77u; w0g=32u; w0h=69u; // CUSTOM E
                    w1a=88u; w1b=84u; w1c=69u; w1d=82u; w1e=78u; w1f=65u; w1g=76u; w1h=32u; // XTERNAL
                    w2a=77u; w2b=80u; w2c=51u; w2d=43u; w2e=32u; w2f=32u; w2g=32u; w2h=32u; // MP3+
                }

                float ostSc = 1.32 * u;
                float ostM0 = sampleWord8(px, ostBoxMin + float2(8.5 * u, 6.0 * u), ostSc, w0a, w0b, w0c, w0d, w0e, w0f, w0g, w0h);
                float ostM1 = sampleWord8(px, ostBoxMin + float2(8.5 * u + 40.0 * ostSc, 6.0 * u), ostSc, w1a, w1b, w1c, w1d, w1e, w1f, w1g, w1h);
                float ostM2 = sampleWord8(px, ostBoxMin + float2(8.5 * u + 80.0 * ostSc, 6.0 * u), ostSc, w2a, w2b, w2c, w2d, w2e, w2f, w2g, w2h);
                sdr = lerp(sdr, colEnamel, max(ostM0, max(ostM1, ostM2)) * trkToastAlpha);
            }
        }
    }

    // Live Hardware Engine Isolation / SOLO Banner (clickable "[ALL]" return to Full Orchestra!)
    if (showOverlay && (hudMode != 3u) && (curSolo != 0u) && res.x >= 480.0 && res.y >= 300.0)
    {
        const float u = clamp(floor(res.y / 540.0), 1.0, 3.0);
        float2 soloSz  = float2(230.0 * u, 20.0 * u);
        float  soloTop = studioDockShown ? ((hudMode == 1u ? (res.y - 60.0 * u) : (res.y - 104.0 * u)) - 25.0 * u) : (res.y - 70.0 * u);
        float2 soloMin = float2(max(14.0 * u, res.x - 245.0 * u), max(12.0 * u, soloTop));
        if (px.x >= soloMin.x && px.x <= soloMin.x + soloSz.x &&
            px.y >= soloMin.y && px.y <= soloMin.y + soloSz.y)
        {
            sdr = lerp(sdr, float3(0.012, 0.028, 0.042), 0.92);
            if ((px.x - soloMin.x < 1.5) || (soloMin.x + soloSz.x - px.x < 1.5) ||
                (px.y - soloMin.y < 1.5) || (soloMin.y + soloSz.y - px.y < 1.5))
                sdr = colJade;

            uint s0=82u, s1=84u, s2=32u, s3=68u, s4=88u, s5=82u, s6=32u, s7=32u; // RT DXR
            if (curSolo == 2u) { s0=84u; s1=78u; s2=83u; s3=82u; s4=32u; s5=77u; s6=77u; s7=65u; } // TNSR MMA
            if (curSolo == 3u) { s0=78u; s1=80u; s2=85u; s3=32u; s4=53u; s5=72u; s6=90u; s7=32u; } // NPU 5HZ
            if (curSolo == 4u) { s0=67u; s1=85u; s2=68u; s3=65u; s4=32u; s5=54u; s6=53u; s7=75u; } // CUDA 65K
            if (curSolo == 5u) { s0=67u; s1=79u; s2=68u; s3=67u; s4=32u; s5=49u; s6=77u; s7=66u; } // CODC 1MB

            float tSolo = sampleWord8(px, soloMin + float2(8.0 * u, 5.5 * u), 1.4 * u, 83u, 79u, 76u, 79u, 58u, 32u, 32u, 32u);
            float tEng  = sampleWord8(px, soloMin + float2(52.0 * u, 5.5 * u), 1.4 * u, s0, s1, s2, s3, s4, s5, s6, s7);
            float tAll  = sampleWord8(px, soloMin + float2(174.0 * u, 5.5 * u), 1.4 * u, 91u, 65u, 76u, 76u, 93u, 32u, 32u, 32u);
            sdr = lerp(sdr, colAmber, tSolo);
            sdr = lerp(sdr, colJade, tEng);
            sdr = lerp(sdr, colEnamel, tAll);
        }
    }

    // Authored vector title and Traditional Chinese place motif (台北, Taipei).
    // Pass 4 only: neither title nor caption enters the canonical clean texture.
    if(hudMode!=2u && gCaptionStyle.z>.5 && gCaptionStyle.x>.01 && lbSize.x>=400.0)
    {
        float titleAlpha=saturate(gCaptionStyle.x);
        float2 titleOrigin=lbMin+lbSize*float2(.075,.30);
        float titleScale=clamp(lbSize.y/115.0,2.0,18.0);
        float leftScrim=1.0-smoothstep(.30,.62,(px.x-lbMin.x)/max(lbSize.x,1.0));
        if (showOverlay && hudMode == 0u) {
            // Full telemetry owns the upper-left; the optional benchmark card
            // owns the upper-right. Place the complete title below that card,
            // and localize its scrim so neither panel's text gets dimmed.
            float uiScale=clamp(floor(res.y/540.0),1.0,3.0);
            titleScale=min(titleScale*.78,lbSize.x*.40/55.0);
            float motifTop=max(lbMin.y+lbSize.y*.35,148.0*uiScale);
            titleOrigin=float2(lbMin.x+lbSize.x*.94-55.0*titleScale,motifTop+22.0*titleScale);
            float2 plateMin=titleOrigin-float2(2,24)*titleScale;
            float2 plateMax=titleOrigin+float2(57,19)*titleScale;
            float feather=max(titleScale*2.0,1.0);
            float2 fadeIn=smoothstep(plateMin,plateMin+feather,px);
            float2 fadeOut=1.0-smoothstep(plateMax-feather,plateMax,px);
            leftScrim=fadeIn.x*fadeIn.y*fadeOut.x*fadeOut.y;
        }
        sdr=lerp(sdr,float3(.012,.024,.026),leftScrim*.68*titleAlpha);
        float title=sampleWord8(px,titleOrigin,titleScale,66,79,79,32,83,65,72,32);
        title=max(title,sampleWord4(px,titleOrigin+float2(40*titleScale,0),titleScale,83,65,72,32));
        float2 motifOrigin=titleOrigin-float2(0,22*titleScale);
        float motifScale=titleScale*.52;
        float motif=max(sampleGlyph(px,motifOrigin,motifScale,21488u),
                        sampleGlyph(px,motifOrigin+float2(12*motifScale,0),motifScale,21271u));
        float roman=sampleWord8(px,motifOrigin+float2(0,13*motifScale),titleScale*.28,84,65,73,80,69,73,32,32);
        float subScale=titleScale*.32;
        float2 subOrigin=titleOrigin+float2(0,10*titleScale);
        float subtitle=sampleWord8(px,subOrigin,subScale,84,72,69,32,71,76,65,83);
        subtitle=max(subtitle,sampleWord8(px,subOrigin+float2(40*subScale,0),subScale,83,32,79,82,67,72,73,68));
        subtitle=max(subtitle,sampleWord8(px,subOrigin+float2(80*subScale,0),subScale,32,80,82,79,84,79,67,79));
        subtitle=max(subtitle,sampleGlyph(px,subOrigin+float2(120*subScale,0),subScale,76));
        sdr=lerp(sdr,float3(.93,.88,.75),title*titleAlpha);
        sdr=lerp(sdr,float3(.72,.82,.74),max(subtitle,roman)*titleAlpha);
        sdr=lerp(sdr,colAmber,motif*titleAlpha*.85);
    }

    // A limited first-party Traditional Chinese numeral set paired with Roman
    // act numbering, shown with narrative captions only in the full film.
    if (hudMode != 2u && gDuration > 359.0 && gFilmTime < 351.0 && gCaptionStyle.x > .01)
    {
        uint act = (gFilmTime < 45) ? 0u : (gFilmTime < 100) ? 1u : (gFilmTime < 150) ? 2u :
                   (gFilmTime < 195) ? 3u : (gFilmTime < 245) ? 4u : (gFilmTime < 275) ? 5u : 6u;
        static const uint numeral[7] = {19968u,20108u,19977u,22235u,20116u,20845u,19971u};
        float sc = max(1.0,lbSize.y/500.0);
        float2 at = lbMin + lbSize*float2(.075,.105);
        if (showOverlay && hudMode == 0u) {
            float uiScale=clamp(floor(res.y/540.0),1.0,3.0);
            at=float2(lbMin.x+lbSize.x*.94-60.0*sc,max(lbMin.y+lbSize.y*.35,148.0*uiScale));
        }
        float glyph = sampleGlyph(px,at,sc*1.4,numeral[act]);
        float2 romanAt = at + float2(21*sc,3*sc);
        float roman = sampleWord4(px,romanAt,sc,65u,67u,84u,32u);
        uint first = (act >= 4u) ? 86u : 73u;
        uint second = (act == 1u || act == 2u || act == 5u || act == 6u) ? 73u : (act == 3u ? 86u : 32u);
        uint third = (act == 2u || act == 6u) ? 73u : 32u;
        roman = max(roman,sampleWord4(px,romanAt+float2(20*sc,0),sc,first,second,third,32u));
        sdr = lerp(sdr,colAmber,max(glyph,roman)*saturate(gCaptionStyle.x)*.82);
    }

    // Contrast-safe Pass-4 Subtitle / Non-Medical End-Card Plate (strictly excluded from CanvasClean & Spout2!)
    if (hudMode != 2u && gCaptionStyle.x > 0.01 && gCaptionStyle.y > 0.5 && res.x >= 400.0 && res.y >= 240.0)
    {
        float capAlpha  = saturate(gCaptionStyle.x);
        uint  charCount = min((uint)(gCaptionStyle.y + 0.5), 64u);
        bool  isEndCard = gCaptionStyle.z > 0.5;
        bool  isMidEd   = (gHudFlags & 32u) != 0u;

        float baseScale = clamp(lbSize.y / 285.0, 1.0, 6.0) * (isEndCard ? .90 : 1.0);
        float fitScale  = (lbSize.x * 0.86) / max(1.0, (float)charCount * 5.0);
        float capScale  = max(1.0, min(baseScale, fitScale));

        float textW = max(4.0, ((float)charCount * 5.0 - 1.0) * capScale);
        float textH = 6.0 * capScale;
        float padX  = 12.0 * capScale;
        float padY  = 6.0 * capScale;
        float2 boxSz = float2(textW + 2.0 * padX, textH + 2.0 * padY);

        float hudUiScale = clamp(floor(res.y / 540.0), 1.0, 3.0);
        float hudReserve = (showOverlay && (hudMode == 0u || hudMode == 2u))
            ? (studioDockShown ? (112.0 * hudUiScale) : (52.0 * hudUiScale))
            : (studioDockShown ? (68.0 * hudUiScale) : (16.0 * capScale));
        float targetY    = isEndCard
            ? (lbMin.y + lbSize.y * 0.78 - 0.5 * boxSz.y)
            : (lbMin.y + lbSize.y - boxSz.y - 18.0 * capScale);
        float boxMinY = min(targetY, res.y - hudReserve - boxSz.y);
        float2 boxMin = float2(lbMin.x + 0.5 * (lbSize.x - boxSz.x), max(lbMin.y + 8.0, boxMinY));

        if (px.x >= boxMin.x && px.x <= boxMin.x + boxSz.x &&
            px.y >= boxMin.y && px.y <= boxMin.y + boxSz.y)
        {
            sdr = lerp(sdr, float3(0.008, 0.014, 0.028), 0.88 * capAlpha);
            bool bdr = (px.x - boxMin.x < 1.3) || (boxMin.x + boxSz.x - px.x < 1.3) ||
                       (px.y - boxMin.y < 1.3) || (boxMin.y + boxSz.y - px.y < 1.3);
            if (bdr)
            {
                float3 bdrCol = isEndCard ? colAmber : (isMidEd ? colOrchid : colJade);
                sdr = lerp(sdr, bdrCol, 0.78 * capAlpha);
            }
            float gMask = samplePackedCaption(px, boxMin + float2(padX, padY), capScale, charCount);
            float3 txtCol = isEndCard ? lerp(colEnamel, colAmber, 0.28) : colEnamel;
            sdr = lerp(sdr, txtCol, gMask * capAlpha);
        }
    }

    // Opt-in Webcam Privacy Indicator ("LIVE CAMERA" — SPEC_v4 Section 2.4)
    if (gCaptionStyle.w > 0.5 && res.x >= 320.0 && res.y >= 200.0)
    {
        float camScale = clamp(floor(res.y / 540.0), 1.0, 3.0);
        float2 pillSz  = float2(128.0 * camScale, 20.0 * camScale);
        float2 pillMin = float2(0.5 * (res.x - pillSz.x), 12.0 * camScale);
        if (px.x >= pillMin.x && px.x <= pillMin.x + pillSz.x &&
            px.y >= pillMin.y && px.y <= pillMin.y + pillSz.y)
        {
            float3 recRed = float3(0.96, 0.18, 0.24);
            sdr = lerp(sdr, float3(0.06, 0.01, 0.02), 0.90);
            if ((px.x - pillMin.x < 1.4) || (pillMin.x + pillSz.x - px.x < 1.4) ||
                (px.y - pillMin.y < 1.4) || (pillMin.y + pillSz.y - px.y < 1.4))
            {
                sdr = recRed;
            }
            float2 dotC = pillMin + float2(11.0 * camScale, 10.0 * camScale);
            if (length(px - dotC) <= 4.0 * camScale)
                sdr = recRed;

            // "LIVE CAMERA" (11 chars)
            float tScale = 1.55 * camScale;
            float2 tOrg  = pillMin + float2(20.0 * camScale, 5.5 * camScale);
            float camTxt = max(
                sampleWord8(px, tOrg, tScale, 76u, 73u, 86u, 69u, 32u, 67u, 65u, 77u),
                sampleWord4(px, tOrg + float2(8.0 * 5.0 * tScale, 0.0), tScale, 69u, 82u, 65u, 32u));
            sdr = lerp(sdr, colEnamel, camTxt);
        }
    }

    float maxQuant = (gSwapchainBits >= 10u) ? 1023.0 : 255.0;
    float scDither = triangularDither(floor(px), 0x13572468u) / maxQuant;
    sdr = saturate(sdr + scDither.xxx);
    return float4(sdr, 1.0);
}
#endif

// =============================================================================
// LIVE PILLAR 2 SUBSTITUTE: 8,192-Particle 2-Step Linear Rain & Mote Compute (SM 6.0)
// =============================================================================
#if defined(BOO_M1_PARTICLES_SUB_CS)
cbuffer M1SceneCB : register(b0)
{
    float3 gCamEye;
    float  gFovY;
    float3 gCamRight;
    float  gAspect;
    float3 gCamUp;
    float  gFilmTime;
    float3 gCamForward;
    float  gRevealOpen01;
    uint   gCanvasWidth;
    uint   gCanvasHeight;
    uint   gExpectSeq;
    uint   gSceneFlags;
    float  gEmissivePulse;
    float  gMoodIntensity;
    uint   gShotIndex;
    uint   gDeterministicSeed;
    uint4  gMeshBaseIndices;
};

RWStructuredBuffer<GpuLiveParticle> gOutParticles : register(u0);

uint pcgHashSub(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float rand01Sub(uint s)
{
    return (float)(s & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

[numthreads(64, 1, 1)]
void CSLiveParticlesSub(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    if (idx >= 8192u)
        return;

    uint h0 = pcgHashSub(idx ^ gDeterministicSeed ^ 0x51A7E201u);
    uint h1 = pcgHashSub(h0 + 0x9E3779B9u);
    uint h2 = pcgHashSub(h1 + 0x85EBCA6Bu);
    uint h3 = pcgHashSub(h2 + 0xC2B2AE35u);

    float r0 = rand01Sub(h0);
    float r1 = rand01Sub(h1);
    float r2 = rand01Sub(h2);
    float r3 = rand01Sub(h3);

    bool isMutinySub = (gSceneFlags & 16u) != 0u;
    bool isSanctuaryMote = isMutinySub ? ((idx & 3u) != 0u) : ((idx & 3u) == 0u);
    GpuLiveParticle outP;

    if (isMutinySub)
    {
        // Honest 8,192-particle 2-step linear sanctuary ring substitute when CUDA is disabled/unavailable in SoloCUDA / Act 4
        float age = frac(r3 + gFilmTime * 0.22);
        float ang = r0 * kTwoPi + gFilmTime * 0.48;
        float rad = 2.25 + r1 * 2.55;
        float px = cos(ang) * rad;
        float py = 3.10 + r2 * 4.60 + age * 0.65;
        float pz = 5.50 + sin(ang) * rad;
        outP.posAndAge  = float4(px, py, pz, age);
        outP.velAndKind = float4(-sin(ang) * 0.85, 0.45, cos(ang) * 0.85, isSanctuaryMote ? 0.82 : 0.28);
    }
    else if (!isSanctuaryMote)
    {
        // Honest 2-step linear raindrop fall (no 16-substep 3D curl-vortex turbulence)
        float age = frac(r3 + gFilmTime * 0.42);
        float px = (r0 * 2.0 - 1.0) * 5.0 + 0.18 * age;
        float py = 12.8 - age * 12.6;
        float pz = r2 * 22.0 - 13.0;
        outP.posAndAge  = float4(px, max(py, 0.05), pz, age);
        outP.velAndKind = float4(0.25, -7.2, 0.0, 0.18);
    }
    else
    {
        // Sparse linear upward drift in outer halo around sanctuary plinth/orchid (no helical 16-substep curl orbit)
        float age = frac(r3 + gFilmTime * 0.18);
        float ang = r0 * kTwoPi + gFilmTime * 0.35;
        float rad = 2.65 + r1 * (2.40 + 0.55 * gRevealOpen01);
        float px = cos(ang) * rad;
        float py = 4.8 + r2 * 4.8 + age * 1.1;
        float pz = 5.50 + sin(ang) * rad * 0.85;
        outP.posAndAge  = float4(px, py, pz, age);
        outP.velAndKind = float4(-sin(ang) * 0.35, 0.55, cos(ang) * 0.35, 0.72 + 0.25 * gRevealOpen01);
    }

    gOutParticles[idx] = outP;
}
#endif

// =============================================================================
// LIVE PILLAR 2 3D SHADING: Velocity-Aligned Rain Filaments & Faceted Crystal Motes
// =============================================================================
#if defined(BOO_M1_PARTICLES_VS) || defined(BOO_M1_PARTICLES_PS)
cbuffer M1SceneCB : register(b0)
{
    float3 gCamEye;
    float  gFovY;
    float3 gCamRight;
    float  gAspect;
    float3 gCamUp;
    float  gFilmTime;
    float3 gCamForward;
    float  gRevealOpen01;
    uint   gCanvasWidth;
    uint   gCanvasHeight;
    uint   gExpectSeq;
    uint   gSceneFlags;
    float  gEmissivePulse;
    float  gMoodIntensity;
    uint   gShotIndex;
    uint   gDeterministicSeed;
    uint4  gMeshBaseIndices;
};

struct VSParticleOut
{
    float4 position     : SV_Position;
    float2 quadUv       : TEXCOORD0;
    float4 colorAndKind : TEXCOORD1;
    float  rayDist      : TEXCOORD2;
    float  energyScale  : TEXCOORD3;
};
#endif

#if defined(BOO_M1_PARTICLES_VS)
StructuredBuffer<GpuLiveParticle> gInParticles : register(t0);

VSParticleOut VSLiveParticles(uint vertId : SV_VertexID, uint instId : SV_InstanceID)
{
    VSParticleOut vout;
    vout.position     = float4(0.0, 0.0, 0.0, 0.0);
    vout.quadUv       = float2(0.0, 0.0);
    vout.colorAndKind = float4(0.0, 0.0, 0.0, 0.0);
    vout.rayDist      = 0.0;
    vout.energyScale  = 0.0;

    GpuLiveParticle p = gInParticles[instId];
    float3 center = p.posAndAge.xyz;
    float age01   = p.posAndAge.w;
    float3 vel    = p.velAndKind.xyz;
    float kind    = p.velAndKind.w;

    bool isMote = (kind >= 0.5);
    bool isM2 = (gSceneFlags & 2u) != 0u;
    bool isMutiny = (gSceneFlags & 16u) != 0u;
    bool isShow3D = (gSceneFlags & 32u) != 0u;
    // Keep the full simulation workload, but draw a stable sparse subset so
    // the velocity field frames the hero instead of veiling its silhouette.
    if (isMutiny && (instId % 5u) != 0u) return vout;
    if(isM2) {
        uint act=(gSceneFlags>>2u)&3u;
        if(act==0u) {
            // Restrained directional river-crossing rain: sparse enough to keep bridge rivets, rails & skyline sharp
            if(isMote || (instId%3u)!=0u) return vout;
            center.z+=gCamEye.z+3.2;
            center.x*=1.35;
        }
        if(act==1u) {
            // Rotor downwash spray localized beneath the Hero Vertiport Pad (0, 3.65, -2.0) during VTOL hover/descent
            if(gFilmTime < 88.0 || (instId%4u)!=0u) return vout;
            float hoverGate = smoothstep(88.0, 102.0, gFilmTime);
            float a = gFilmTime * 3.4 + instId * 0.031;
            float r = (1.4 + frac(instId * 0.618) * 3.6) * (0.55 + 0.45 * age01);
            center = float3(sin(a) * r, 3.85 - age01 * 2.2 * hoverGate, -2.0 + cos(a) * r) + center * 0.04;
            vel = float3(cos(a) * 3.8, -1.6, -sin(a) * 3.8) + vel * 0.10;
        }
        if(act==2u) {
            if(!isMote && (instId%6u)!=0u) return vout;
            if(isMote) {
                if((instId%6u)!=0u) return vout;
                // Gentle sanctuary mist motes drifting broadly across the reflecting pool & plinth (never clumping behind the stem)
                center = float3(0.0, 3.45, 5.35) + (center - float3(0.0, 7.5, 5.5)) * float3(0.56, 0.14, 0.42);
            }
        }
    }
    else if (isShow3D)
    {
        if (gFilmTime >= 338.0)
        {
            // Enclosed Dawn Bathroom Finale: ZERO indoor rain; ultra-subtle warm dust motes in window sunbeams
            if (!isMote || (instId % 20u) != 0u) return vout;
            float h1 = frac((float)instId * 0.6180339);
            float h2 = frac((float)instId * 0.3819660);
            float h3 = frac((float)instId * 0.7548776);
            float driftT = gFilmTime * 0.08 + h1 * 6.2831853;
            center = float3(
                lerp(-0.32, 1.42, h1) + 0.06 * sin(driftT),
                lerp(3.62, 5.02, h2) + 0.05 * cos(driftT * 0.85 + h3 * 4.0),
                lerp(4.55, 6.28, h3) + 0.05 * sin(driftT * 0.72));
        }
        else if (gFilmTime >= 245.0 && gFilmTime < 275.0)
        {
            // Codec Hijack eVTOL Hovering Downwash Spray
            if ((instId % 5u) != 0u) return vout;
            float a = gFilmTime * 3.4 + instId * 0.031;
            float r = (1.4 + frac(instId * 0.618) * 3.6) * (0.55 + 0.45 * age01);
            center = float3(sin(a) * r, 3.85 - age01 * 2.1, -2.0 + cos(a) * r) + center * 0.04;
            vel = float3(cos(a) * 3.8, -1.6, -sin(a) * 3.8) + vel * 0.10;
        }
        else
        {
            if (!isMote && (instId % 6u) != 0u) return vout;
            if (isMote)
            {
                if ((instId % 6u) != 0u) return vout;
                center = float3(0.0, 3.45, 5.35) + (center - float3(0.0, 7.5, 5.5)) * float3(0.56, 0.14, 0.42);
            }
        }
    }
    float3 relCenter = center - gCamEye;
    float zCenter = dot(relCenter, gCamForward);
    if (zCenter <= 0.18 || zCenter >= 52.0)
        return vout;

    static const float2 kCorners[6] = {
        float2(-1.0, -1.0), float2( 1.0, -1.0), float2(-1.0,  1.0),
        float2(-1.0,  1.0), float2( 1.0, -1.0), float2( 1.0,  1.0)
    };
    float2 corner = kCorners[vertId % 6u];

    float moteGate = smoothstep(0.12, 0.72, gRevealOpen01);
    if (isMote && moteGate <= 0.002)
        return vout;

    bool isMidnight = (gSceneFlags & 1u) != 0u;
    float tanHalfFov = tan(gFovY * 0.5);
    float pixelWorldSize = zCenter * tanHalfFov * (2.0 / max((float)gCanvasHeight, 1.0));

    float3 viewDir = safeNormalize(relCenter);
    float3 axisLong;
    float3 axisWide;
    float halfLen;
    float halfWid;
    float energy = isM2
        ? (.32 * smoothstep(0, 2, min(abs(gFilmTime - 60), abs(gFilmTime - 120))))
        : (isMutiny ? 0.36 : (isShow3D ? 0.34 : 1.0));

    if (!isMote)
    {
        float3 velProj = vel - viewDir * dot(vel, viewDir);
        float velMag = length(velProj);
        axisLong = (velMag > 1e-3) ? (velProj / velMag) : gCamUp;
        axisWide = safeNormalize(cross(viewDir, axisLong));

        float desiredWid = (isM2 || isShow3D) ? 0.0042 : (isMutiny ? 0.0038 : 0.0055);
        halfWid = max(desiredWid, pixelWorldSize * 0.75);
        energy *= saturate(desiredWid / max(halfWid, 1e-4));
        halfLen = clamp(length(vel) * ((isM2 || isShow3D) ? 0.010 : 0.013), halfWid * 3.0, (isM2 || isShow3D) ? 0.14 : (isMutiny ? 0.11 : 0.19));
    }
    else
    {
        float spin = (float)(instId & 255u) * 0.17 + gFilmTime * 0.65;
        float cs = cos(spin);
        float sn = sin(spin);
        axisWide = gCamRight * cs + gCamUp * sn;
        axisLong = -gCamRight * sn + gCamUp * cs;

        bool isDawnMote = isShow3D && (gFilmTime >= 338.0);
        float radScale = isDawnMote ? 0.18 : ((isM2 || isShow3D) ? 0.48 : (isMutiny ? 0.54 : 1.0));
        float desiredRad = (0.0085 + 0.0070 * frac((float)instId * 0.371)) * radScale;
        halfWid = max(desiredRad, pixelWorldSize * ((isM2 || isShow3D || isMutiny) ? 0.78 : 1.05));
        halfLen = halfWid;
        energy *= saturate(desiredRad / max(halfWid, 1e-4)) * ((isDawnMote ? 0.16 : ((isM2 || isShow3D) ? 0.42 : (isMutiny ? 0.52 : 0.55))) * moteGate);
    }

    // Smooth lifecycle fade at birth & death so looping/respawning particles never pop
    float lifeFade = smoothstep(0.0, 0.12, age01) * smoothstep(1.0, 0.82, age01);
    float3 worldCorner = center + axisWide * (corner.x * halfWid) + axisLong * (corner.y * halfLen);
    float3 rel = worldCorner - gCamEye;
    float zCam = max(dot(rel, gCamForward), 0.05);
    float xCam = dot(rel, gCamRight);
    float yCam = dot(rel, gCamUp);

    float ndcX = xCam / (zCam * gAspect * tanHalfFov);
    float ndcY = yCam / (zCam * tanHalfFov);

    float3 colJade   = float3(0.06, 0.90, 0.76);
    float3 colOrchid = isMidnight ? float3(0.95, 0.20, 0.78) : float3(0.88, 0.26, 0.92);
    float3 colAmber  = float3(1.00, 0.66, 0.18);

    float3 tint;
    if (!isMote)
    {
        if (isM2 || isShow3D)
        {
            tint = float3(0.20, 0.28, 0.36) * (0.20 + 0.12 * kind);
        }
        else if (isMutiny)
        {
            tint = lerp(colJade, colAmber, 0.35 + 0.35 * sin(center.y * 0.8 + gFilmTime)) * 0.38;
        }
        else
        {
            // Rain filaments catch Taipei colonnade neon (x = +-3.92) and sanctuary vault glow
            float neonPhase = 0.5 + 0.5 * sin(center.z * 0.55 - gFilmTime * 0.75);
            float3 neonTint = lerp(colJade, lerp(colAmber, colOrchid, 0.4), neonPhase);
            float nearSign = 1.0 / (1.0 + 0.25 * pow(abs(abs(center.x) - 3.92), 2.0));
            tint = lerp(float3(0.16, 0.40, 0.58), neonTint, saturate(0.32 + 0.68 * nearSign)) * (0.16 + 0.14 * kind);
        }
    }
    else
    {
        // Faceted sanctuary pollen & crystal motes
        float hPick = frac((float)instId * 0.6180339);
        float3 baseHue = (isM2 || isShow3D)
            ? ((hPick < 0.48) ? float3(0.92, 0.68, 0.28) : ((hPick < 0.82) ? float3(0.55, 0.84, 0.78) : float3(0.85, 0.78, 0.82)))
            : (isMutiny
                ? ((hPick < 0.52) ? colJade : ((hPick < 0.82) ? colAmber : colOrchid))
                : ((hPick < 0.36) ? colAmber : ((hPick < 0.70) ? colJade : colOrchid)));
        tint = baseHue * (0.62 + 0.75 * gRevealOpen01) * (1.0 + gEmissivePulse * 0.45);
    }

    vout.position     = float4(ndcX * zCam, ndcY * zCam, 0.5 * zCam, zCam);
    vout.quadUv       = corner;
    vout.colorAndKind = float4(tint, isMote ? 1.0 : 0.0);
    vout.rayDist      = length(relCenter);
    vout.energyScale  = energy * lifeFade;
    return vout;
}
#endif

#if defined(BOO_M1_PARTICLES_PS)
Texture2D<float> gSceneDepthTex : register(t0);

float4 PSLiveParticles(VSParticleOut pin) : SV_Target
{
    int2 pix = clamp(int2(pin.position.xy), int2(0, 0), int2((int)gCanvasWidth - 1, (int)gCanvasHeight - 1));
    float sceneRayT = gSceneDepthTex.Load(int3(pix, 0));
    float softOcc = saturate((sceneRayT - pin.rayDist + 0.06) * 4.0);
    if (softOcc <= 0.001)
        discard;

    float2 q = pin.quadUv;
    float3 hdr = 0.0.xxx;

    if (pin.colorAndKind.w < 0.5)
    {
        // Cylindrical refractive rain filament profile with crisp specular spine
        float radial = abs(q.x);
        float axial  = abs(q.y);
        if (radial > 1.0 || axial > 1.0)
            discard;
        float coreSpine = exp(-7.5 * radial * radial) * (1.0 - axial * axial);
        float edgeGlint = exp(-22.0 * pow(abs(radial - 0.52), 2.0)) * (1.0 - axial);
        hdr = pin.colorAndKind.rgb * (coreSpine * 0.85 + edgeGlint * 0.45);
    }
    else
    {
        // Mathematically shaded 3D octahedral micro-crystalote (`|x| + |y| <= 1` diamond silhouette + 3D facet normal!)
        float l1 = abs(q.x) + abs(q.y);
        if (l1 > 1.0)
            discard;
        float nz = max(1.0 - l1, 0.0);
        float3 facetN = safeNormalize(float3(q.x * 1.35, q.y * 1.35, nz + 0.25));
        float3 lightDir = safeNormalize(float3(-0.35, 0.75, 0.56));
        float facetSpec = pow(saturate(dot(facetN, lightDir)), 18.0);
        float3 thinFilm = evalThinFilmInterference(saturate(facetN.z), 0.38 + 0.14 * q.x);
        float jewelCore = exp(-4.8 * dot(q, q));
        hdr = pin.colorAndKind.rgb * lerp(float3(1.0, 1.0, 1.0), thinFilm * 1.3, 0.55) * (jewelCore * 0.90 + (1.0 - l1) * 0.35)
            + float3(1.0, 0.92, 0.78) * facetSpec * 0.55;
    }

    return float4(hdr * (pin.energyScale * softOcc), 0.0);
}
#endif

// =============================================================================
// LIVE PILLAR 3 SUBSTITUTE: Scalar HLSL Baseline Field (Zero Tensor Core MMA Ops)
// =============================================================================
#if defined(BOO_M1_TENSOR_SUB_CS)
cbuffer M1TensorSubCB : register(b0)
{
    float gFilmTime;
    float gRevealOpen01;
    uint  gMidnight;
    uint  gSeed;
};

RWTexture2D<float4> gOutTensorTex : register(u0);

[numthreads(8, 8, 1)]
void CSLiveTensorSub(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= 320u || dtid.y >= 180u)
        return;

    // Honest flat 1-harmonic scalar substitute (no 64-layer Tensor Core mma.sync SIREN caustic lattice)
    float2 uv = (float2(dtid.xy) + 0.5) / float2(320.0, 180.0);
    float gentle = 0.5 + 0.5 * sin(uv.x * kTwoPi + gFilmTime * 0.25) * cos(uv.y * kTwoPi);
    float3 base = float3(0.035, 0.045, 0.055) + float3(0.004, 0.008, 0.007) * gentle * (0.5 + 0.5 * gRevealOpen01);
    gOutTensorTex[dtid.xy] = float4(base, 0.50);
}
#endif

// =============================================================================
// LIVE PILLAR 4 INPUT PACKER: CanvasClean -> 1280x720 BT.709 Studio-Swing NV12
// =============================================================================
#if defined(BOO_M1_NV12_PACK_CS)
Texture2D<float4>   gCanvasCleanSrc : register(t0);
SamplerState        gLinearClamp    : register(s0);
RWByteAddressBuffer gOutNv12Words   : register(u0);

uint rgbToBt709Y(float3 rgb)
{
    float y = 16.0 + 219.0 * saturate(0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b);
    return clamp((uint)(y + 0.5), 16u, 235u);
}

uint2 rgbToBt709Uv(float3 rgb)
{
    float cb = 128.0 + 224.0 * (-0.114572 * rgb.r - 0.385428 * rgb.g + 0.500000 * rgb.b);
    float cr = 128.0 + 224.0 * ( 0.500000 * rgb.r - 0.454153 * rgb.g - 0.045847 * rgb.b);
    return uint2(clamp((uint)(cb + 0.5), 16u, 240u), clamp((uint)(cr + 0.5), 16u, 240u));
}

// Dispatched with (40, 45, 1) groups of (8, 8, 1) = 320 x 360 threads.
// Each thread packs a 4x2 pixel tile into 3 aligned 32-bit NV12 words (zero atomics!).
[numthreads(8, 8, 1)]
void CSPackCanvasToNv12(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= 320u || dtid.y >= 360u)
        return;

    uint baseX = dtid.x * 4u;
    uint row0  = dtid.y * 2u;
    uint row1  = row0 + 1u;
    const float2 invRes = float2(1.0 / 1280.0, 1.0 / 720.0);

    float3 r00 = gCanvasCleanSrc.SampleLevel(gLinearClamp, (float2((float)(baseX + 0u), (float)row0) + 0.5) * invRes, 0.0).rgb;
    float3 r01 = gCanvasCleanSrc.SampleLevel(gLinearClamp, (float2((float)(baseX + 1u), (float)row0) + 0.5) * invRes, 0.0).rgb;
    float3 r02 = gCanvasCleanSrc.SampleLevel(gLinearClamp, (float2((float)(baseX + 2u), (float)row0) + 0.5) * invRes, 0.0).rgb;
    float3 r03 = gCanvasCleanSrc.SampleLevel(gLinearClamp, (float2((float)(baseX + 3u), (float)row0) + 0.5) * invRes, 0.0).rgb;

    float3 r10 = gCanvasCleanSrc.SampleLevel(gLinearClamp, (float2((float)(baseX + 0u), (float)row1) + 0.5) * invRes, 0.0).rgb;
    float3 r11 = gCanvasCleanSrc.SampleLevel(gLinearClamp, (float2((float)(baseX + 1u), (float)row1) + 0.5) * invRes, 0.0).rgb;
    float3 r12 = gCanvasCleanSrc.SampleLevel(gLinearClamp, (float2((float)(baseX + 2u), (float)row1) + 0.5) * invRes, 0.0).rgb;
    float3 r13 = gCanvasCleanSrc.SampleLevel(gLinearClamp, (float2((float)(baseX + 3u), (float)row1) + 0.5) * invRes, 0.0).rgb;

    uint yWord0 = rgbToBt709Y(r00)
                | (rgbToBt709Y(r01) << 8u)
                | (rgbToBt709Y(r02) << 16u)
                | (rgbToBt709Y(r03) << 24u);
    uint yWord1 = rgbToBt709Y(r10)
                | (rgbToBt709Y(r11) << 8u)
                | (rgbToBt709Y(r12) << 16u)
                | (rgbToBt709Y(r13) << 24u);

    uint2 uv0 = rgbToBt709Uv((r00 + r01 + r10 + r11) * 0.25);
    uint2 uv1 = rgbToBt709Uv((r02 + r03 + r12 + r13) * 0.25);
    uint uvWord = uv0.x | (uv0.y << 8u) | (uv1.x << 16u) | (uv1.y << 24u);

    gOutNv12Words.Store(row0 * 1280u + baseX, yWord0);
    gOutNv12Words.Store(row1 * 1280u + baseX, yWord1);
    gOutNv12Words.Store(921600u + dtid.y * 1280u + baseX, uvWord);
}
#endif

// =============================================================================
// MILESTONE M8 SPOUT2 CLEAN FEED: Pass-3 CanvasClean (10-bit) -> Shared BGRA8
// Strictly isolated from Pass-4 HUD, captions, A/B splits, and letterbox bars.
// =============================================================================
#if defined(BOO_M8_SPOUT_BLIT_PS)
Texture2D<float4> gCanvasCleanSrc : register(t0);
SamplerState      gLinearClamp    : register(s0);

float4 PSSpoutCleanBlit(VSOut pin) : SV_Target
{
    float3 cleanRgb = gCanvasCleanSrc.SampleLevel(gLinearClamp, pin.uv, 0.0).rgb;
    float d = triangularDither(floor(pin.position.xy), 0x5A0172u) / 255.0;
    return float4(saturate(cleanRgb + d.xxx), 1.0);
}
#endif
