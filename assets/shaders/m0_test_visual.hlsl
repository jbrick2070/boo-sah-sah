// BOO SAH SAH — The Glass Orchid Protocol
// Milestone M0 Procedural Test Visual + Spy-Tech Numerical Telemetry HUD Shader
// Compiled via Windows SDK DXC (SM 6.0) with embedded safe fallback entry point.

cbuffer M0Constants : register(b0)
{
    float gFilmTime;
    float gDuration;
    float gActProgress;
    float gAudioEnvelope;

    float gWidth;
    float gHeight;
    uint  gActIndex;
    uint  gTotalActs;

    float gFps;
    float gGpuMs;
    float gP95Ms;
    float gVramUsedMb;

    float gVramBudgetMb;
    float gBloomStrength;
    float gLightTension;
    float gSpectralShift;

    uint  gFlags;          // bit0:overlay, bit1:audioClk, bit2:muted, bit3:paused, bit4:loop, bit5:midnight, bit6:benchRun, bit7:benchDone
    uint  gPresetId;       // 0..3
    uint  gEnginePacked;   // 4-bit nibbles: RT, Tensor, NPU, CUDA, Codec
    float gBenchProgress;  // [0..1]

    float4 gBenchPrimary;  // x:RT MRays/s, y:CUDA MPart/s, z:Tensor TFLOPS, w:NVENC FPS
    float4 gBenchFallback; // HLSL substitute comparison scores
    float4 gBenchFill;     // Normalized [0..1] bar fill per engine
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOut VSMain(uint vertexId : SV_VertexID)
{
    // Fullscreen triangle without vertex buffer
    VSOut o;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.uv = uv;
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 acesTonemap(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 linearToSrgb(float3 lin)
{
    lin = max(lin, 0.0.xxx);
    float3 lo = lin * 12.92;
    float3 hi = 1.055 * pow(lin, (1.0 / 2.4).xxx) - 0.055;
    return lerp(lo, hi, step(0.0031308.xxx, lin));
}

// 4x6 procedural bitmap glyph lookup (0xR5R4R3R2R1R0, 4 bits per row, top=row 0 in screen space)
uint glyphBits(uint ch)
{
    switch (ch)
    {
        case 48: return 0x699996u; // 0
        case 49: return 0x262227u; // 1
        case 50: return 0x69124Fu; // 2
        case 51: return 0xE1611Eu; // 3
        case 52: return 0x99F111u; // 4
        case 53: return 0xF8E11Eu; // 5
        case 54: return 0x68E996u; // 6
        case 55: return 0xF12444u; // 7
        case 56: return 0x696996u; // 8
        case 57: return 0x699716u; // 9
        case 46: return 0x000004u; // .
        case 58: return 0x040040u; // :
        case 45: return 0x00F000u; // -
        case 47: return 0x112248u; // /
        case 65: return 0x699F99u; // A
        case 66: return 0xE9E99Eu; // B
        case 67: return 0x698896u; // C
        case 68: return 0xE9999Eu; // D
        case 69: return 0xF8E88Fu; // E
        case 70: return 0xF8E888u; // F
        case 71: return 0x698B96u; // G
        case 72: return 0x99F999u; // H
        case 73: return 0x722227u; // I
        case 75: return 0x9ACCA9u; // K
        case 76: return 0x88888Fu; // L
        case 77: return 0x9F9999u; // M
        case 78: return 0x9DB999u; // N
        case 79: return 0x699996u; // O
        case 80: return 0xE99E88u; // P
        case 81: return 0x6999A5u; // Q
        case 82: return 0xE99EA9u; // R
        case 83: return 0x78611Eu; // S
        case 84: return 0xF44444u; // T
        case 85: return 0x999996u; // U
        case 86: return 0x999962u; // V
        case 88: return 0x996699u; // X
        case 89: return 0x996222u; // Y
        default: return 0u;
    }
}

float sampleGlyph(float2 px, float2 origin, float scale, uint ch)
{
    float2 rel = (px - origin) / scale;
    if (rel.x < 0.0 || rel.x >= 4.0 || rel.y < 0.0 || rel.y >= 6.0)
        return 0.0;
    uint col = (uint)rel.x;
    uint row = 5u - (uint)rel.y;
    uint bits = glyphBits(ch);
    uint rowNib = (bits >> (row * 4u)) & 0xFu;
    return ((rowNib >> (3u - col)) & 1u) ? 1.0 : 0.0;
}

// Renders fixed-width positive float "DDDD.D" (6 chars)
float sampleFixedNumber6(float2 px, float2 origin, float scale, float val)
{
    val = clamp(val, 0.0, 9999.9);
    uint tenthsTotal = (uint)(val * 10.0 + 0.5);
    uint frac = tenthsTotal % 10u;
    uint whole = tenthsTotal / 10u;

    uint d3 = (whole / 1000u) % 10u;
    uint d2 = (whole / 100u)  % 10u;
    uint d1 = (whole / 10u)   % 10u;
    uint d0 = whole % 10u;

    float stepX = 5.0 * scale;
    float a = 0.0;
    if (whole >= 1000u) a = max(a, sampleGlyph(px, origin + float2(0.0 * stepX, 0.0), scale, 48u + d3));
    if (whole >= 100u)  a = max(a, sampleGlyph(px, origin + float2(1.0 * stepX, 0.0), scale, 48u + d2));
    if (whole >= 10u)   a = max(a, sampleGlyph(px, origin + float2(2.0 * stepX, 0.0), scale, 48u + d1));
    a = max(a, sampleGlyph(px, origin + float2(3.0 * stepX, 0.0), scale, 48u + d0));
    a = max(a, sampleGlyph(px, origin + float2(4.0 * stepX, 0.0), scale, 46u));
    a = max(a, sampleGlyph(px, origin + float2(5.0 * stepX, 0.0), scale, 48u + frac));
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
    // 0=Accelerated, 1=Substituted, 2=Degraded, 3=Unavailable, 4=Faulted
    if (code == 0u) return float3(0.08, 0.96, 0.62); // Bright Jade Emerald
    if (code == 1u) return float3(0.12, 0.72, 0.94); // Taipei Cyan
    if (code == 2u) return float3(1.00, 0.72, 0.12); // Amber Gold
    if (code == 4u) return float3(0.98, 0.20, 0.32); // Fault Crimson
    return float3(0.32, 0.36, 0.44);                 // Slate Standby
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

float4 PSMain(VSOut pin) : SV_Target
{
    const float2 res = max(float2(gWidth, gHeight), float2(1.0, 1.0));
    const float2 px  = pin.uv * res;
    const float aspect = res.x / res.y;
    float2 p = (pin.uv - 0.5) * float2(aspect, 1.0) * 2.0;

    // Palette per SPEC_v4 Section 2.1:
    // Midnight charcoal, jade/teal, orchid magenta, amber-gold, warm enamel-white
    const bool isMidnight = (gFlags & 32u) != 0u;
    const float3 colMidnight = isMidnight ? float3(0.015, 0.018, 0.038) : float3(0.020, 0.028, 0.045);
    const float3 colJade     = float3(0.02, 0.78, 0.66);
    const float3 colOrchid   = isMidnight ? float3(0.92, 0.16, 0.74) : float3(0.82, 0.22, 0.86);
    const float3 colAmber    = float3(1.00, 0.68, 0.14);
    const float3 colEnamel   = float3(0.96, 0.95, 0.91);

    // Subtle act-specific angular modulation
    const float t = gFilmTime;
    const float r = length(p);
    const float a = atan2(p.y, p.x);

    // Taipei night rain-grid & architectural enamel horizon
    float2 gridUv = p * (3.5 + 0.4 * sin(t * 0.15));
    float2 gridCell = abs(frac(gridUv + float2(0.0, t * 0.08)) - 0.5);
    float cityLines = exp(-38.0 * min(gridCell.x, gridCell.y)) * exp(-1.3 * r);

    // Glass Orchid 6-petal + 12-subpetal refractive interference crystal
    float petalCount = 6.0 + (float)(gActIndex % 3u) * 2.0;
    float orchidOuter = 0.52 + 0.18 * cos(petalCount * a - t * 0.55) + 0.07 * cos(2.0 * petalCount * a + t * 0.9);
    float orchidInner = 0.26 + 0.09 * sin(petalCount * a + t * 0.85) * (0.6 + 0.4 * gBloomStrength);

    float distOuter = abs(r - orchidOuter);
    float distInner = abs(r - orchidInner);

    float glowOuter = 0.018 / (distOuter + 0.016);
    float glowInner = 0.014 / (distInner + 0.014);

    // Audio-synchronized enamel crystal ripple rings
    float beatRing = exp(-28.0 * abs(r - (0.18 + 0.62 * frac(t * 0.5)))) * (0.35 + 0.95 * gAudioEnvelope);

    // Iridescent spectral mix driven by Scripted Director mood state
    float3 spectral = lerp(colJade, colOrchid, 0.5 + 0.5 * sin(3.0 * a + t * 0.7 + gSpectralShift * 3.14159));
    float3 hdr = colMidnight;
    hdr += colJade * cityLines * (0.28 + 0.35 * (1.0 - gLightTension));
    hdr += spectral * glowOuter * (0.45 + 0.55 * gBloomStrength);
    hdr += lerp(colOrchid, colEnamel, 0.45) * glowInner * 0.65;
    hdr += colAmber * beatRing;

    // Enamel prism core highlight
    float coreFacet = smoothstep(0.16, 0.0, r) * (0.7 + 0.3 * cos(12.0 * a - t * 1.4));
    hdr += colEnamel * coreFacet * 0.85;

    // Subtle cinematic vignette
    hdr *= saturate(1.22 - 0.36 * dot(p, p));

    float3 sdr = linearToSrgb(acesTonemap(hdr));

    // Optional Diagnostics & Creative Numerical Progress Bar HUD (Cleanly toggleable via Tab / F1)
    const bool showOverlay = (gFlags & 1u) != 0u;
    if (showOverlay && res.x >= 480.0 && res.y >= 300.0)
    {
        const float uiScale = clamp(floor(res.y / 420.0), 1.0, 3.0);

        // 1. Bottom Cinema Timeline & Hardware Orchestra Bar
        const float barH = 44.0 * uiScale;
        if (px.y >= res.y - barH)
        {
            sdr = lerp(sdr, float3(0.03, 0.045, 0.075), 0.84);
            if (abs(px.y - (res.y - barH)) < 1.5)
                sdr = lerp(sdr, colJade, 0.65);

            // Timecode MM:SS on bottom-left
            float tcMask = sampleTimecodeMMSS(px, float2(14.0 * uiScale, res.y - 34.0 * uiScale), 2.0 * uiScale, gFilmTime);
            sdr = lerp(sdr, colEnamel, tcMask);

            // 7-Act Full Film Progress Scrub Bar
            float2 tlMin  = float2(86.0 * uiScale, res.y - 31.0 * uiScale);
            float2 tlSize = float2(max(120.0, res.x * 0.36), 10.0 * uiScale);
            float filmFill = saturate(gFilmTime / max(gDuration, 1.0));
            float tlMask = segmentedBar(px, tlMin, tlSize, filmFill, 28u);
            float3 tlCol = lerp(colJade, colOrchid, filmFill);
            sdr = lerp(sdr, tlCol, tlMask * 0.92);

            // Act progress sub-bar right underneath
            float2 actMin  = float2(86.0 * uiScale, res.y - 16.0 * uiScale);
            float2 actSize = float2(max(120.0, res.x * 0.36), 5.0 * uiScale);
            float actMask = segmentedBar(px, actMin, actSize, gActProgress, 14u);
            sdr = lerp(sdr, colAmber, actMask * 0.88);

            // FPS & GPU ms numeric readouts
            float fpsLbl = sampleWord4(px, float2(tlMin.x + tlSize.x + 18.0 * uiScale, res.y - 34.0 * uiScale), 1.8 * uiScale, 70u, 80u, 83u, 58u); // "FPS:"
            float fpsVal = sampleFixedNumber6(px, float2(tlMin.x + tlSize.x + 56.0 * uiScale, res.y - 34.0 * uiScale), 1.8 * uiScale, gFps);
            sdr = lerp(sdr, colJade, fpsLbl);
            sdr = lerp(sdr, colEnamel, fpsVal);

            float msLbl = sampleWord4(px, float2(tlMin.x + tlSize.x + 18.0 * uiScale, res.y - 18.0 * uiScale), 1.6 * uiScale, 71u, 80u, 85u, 58u); // "GPU:"
            float msVal = sampleFixedNumber6(px, float2(tlMin.x + tlSize.x + 56.0 * uiScale, res.y - 18.0 * uiScale), 1.6 * uiScale, gGpuMs);
            sdr = lerp(sdr, colAmber, msLbl);
            sdr = lerp(sdr, colEnamel, msVal);

            // 5 Hardware Orchestra Engine Pills on bottom-right: RT, TNSR, NPU, CUDA, NVEC
            float rightBase = res.x - 245.0 * uiScale;
            if (rightBase > tlMin.x + tlSize.x + 125.0 * uiScale)
            {
                [unroll]
                for (uint e = 0u; e < 5u; ++e)
                {
                    uint stCode = (gEnginePacked >> (e * 4u)) & 0xFu;
                    float3 pillCol = badgeStatusColor(stCode);
                    float2 pMin = float2(rightBase + (float)e * 47.0 * uiScale, res.y - 34.0 * uiScale);
                    float2 pMax = pMin + float2(42.0 * uiScale, 22.0 * uiScale);
                    if (px.x >= pMin.x && px.x <= pMax.x && px.y >= pMin.y && px.y <= pMax.y)
                    {
                        sdr = lerp(sdr, pillCol * 0.25, 0.85);
                        bool border = (px.x - pMin.x < 1.5) || (pMax.x - px.x < 1.5) ||
                                      (px.y - pMin.y < 1.5) || (pMax.y - px.y < 1.5);
                        if (border) sdr = pillCol;
                    }
                    uint c0 = 82u, c1 = 84u, c2 = 32u, c3 = 32u;
                    if (e == 1u) { c0 = 84u; c1 = 78u; c2 = 83u; c3 = 82u; } // TNSR
                    if (e == 2u) { c0 = 78u; c1 = 80u; c2 = 85u; c3 = 32u; } // NPU
                    if (e == 3u) { c0 = 67u; c1 = 85u; c2 = 68u; c3 = 65u; } // CUDA
                    if (e == 4u) { c0 = 78u; c1 = 86u; c2 = 69u; c3 = 67u; } // NVEC
                    float txt = sampleWord4(px, pMin + float2(5.0 * uiScale, 6.0 * uiScale), 1.6 * uiScale, c0, c1, c2, c3);
                    sdr = lerp(sdr, pillCol, txt);
                }
            }
        }

        // 2. Top-Right Creative Numerical Benchmark Telemetry Card (RT / CUDA / Tensor / NVENC)
        const bool benchActiveOrDone = ((gFlags & 64u) != 0u) || ((gFlags & 128u) != 0u);
        if (benchActiveOrDone)
        {
            float2 cardMin = float2(res.x - 275.0 * uiScale, 16.0 * uiScale);
            float2 cardSize = float2(260.0 * uiScale, 118.0 * uiScale);
            if (px.x >= cardMin.x && px.x <= cardMin.x + cardSize.x &&
                px.y >= cardMin.y && px.y <= cardMin.y + cardSize.y)
            {
                sdr = lerp(sdr, float3(0.025, 0.035, 0.065), 0.88);
                bool cardBorder = (px.x - cardMin.x < 1.5) || (cardMin.x + cardSize.x - px.x < 1.5) ||
                                  (px.y - cardMin.y < 1.5) || (cardMin.y + cardSize.y - px.y < 1.5);
                if (cardBorder) sdr = lerp(colJade, colOrchid, 0.5);
            }

            // Overall sweep header progress bar
            float hdrFill = ((gFlags & 64u) != 0u) ? gBenchProgress : 1.0;
            float hdrBar = segmentedBar(px, cardMin + float2(10.0 * uiScale, 8.0 * uiScale), float2(240.0 * uiScale, 6.0 * uiScale), hdrFill, 24u);
            sdr = lerp(sdr, colAmber, hdrBar);

            float scores[4] = { gBenchPrimary.x, gBenchPrimary.y, gBenchPrimary.z, gBenchPrimary.w };
            float fills[4]  = { gBenchFill.x,    gBenchFill.y,    gBenchFill.z,    gBenchFill.w };
            float3 rowCols[4] = {
                float3(0.10, 0.94, 0.76), // RT: Jade
                float3(0.28, 0.78, 1.00), // CUDA: Electric Blue
                float3(0.92, 0.30, 0.90), // Tensor: Orchid Magenta
                float3(1.00, 0.74, 0.18)  // NVENC: Amber Gold
            };

            [unroll]
            for (uint b = 0u; b < 4u; ++b)
            {
                float rowY = cardMin.y + (22.0 + (float)b * 23.0) * uiScale;
                uint l0 = 82u, l1 = 84u, l2 = 32u, l3 = 32u;
                if (b == 1u) { l0 = 67u; l1 = 85u; l2 = 68u; l3 = 65u; } // CUDA
                if (b == 2u) { l0 = 84u; l1 = 78u; l2 = 83u; l3 = 82u; } // TNSR
                if (b == 3u) { l0 = 78u; l1 = 86u; l2 = 69u; l3 = 67u; } // NVEC

                float lbl = sampleWord4(px, float2(cardMin.x + 10.0 * uiScale, rowY), 1.7 * uiScale, l0, l1, l2, l3);
                sdr = lerp(sdr, rowCols[b], lbl);

                float barMask = segmentedBar(
                    px,
                    float2(cardMin.x + 52.0 * uiScale, rowY + 1.0 * uiScale),
                    float2(130.0 * uiScale, 9.0 * uiScale),
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

    return float4(sdr, 1.0);
}

// Section 8.2: Embedded precompiled safe fallback pixel shader (guaranteed non-blackscreen)
float4 PSFallbackMain(VSOut pin) : SV_Target
{
    const float2 res = max(float2(gWidth, gHeight), float2(1.0, 1.0));
    const float aspect = res.x / res.y;
    float2 p = (pin.uv - 0.5) * float2(aspect, 1.0) * 2.0;
    float r = length(p);
    float ring = smoothstep(0.06, 0.0, abs(r - (0.42 + 0.08 * sin(gFilmTime * 1.5))));
    float3 base = lerp(float3(0.03, 0.05, 0.08), float3(0.06, 0.55, 0.50), ring);
    base += float3(0.75, 0.45, 0.12) * smoothstep(0.18, 0.0, r);
    return float4(saturate(base), 1.0);
}
