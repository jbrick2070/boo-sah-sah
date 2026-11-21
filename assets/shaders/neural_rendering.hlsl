// Neural rendering suite: three small neural networks that run on the GPU every frame.
//
//   Neural shading           16 -> 32 -> 32 -> 3 MLP, offline trained (tools/train_neural_weights.py) to
//                            reproduce filmReference(); applied per pixel to the HDR scene.
//   Neural texture decode    64 x 64 x 8 grid of 8-bit latents + 16 -> 32 -> 32 -> 3 decoder MLP that
//                            rebuilds a 512 x 512 texture; measured against the original every frame.
//   Neural radiance cache    multi-resolution feature grid + 9 -> 32 -> 32 -> 3 MLP trained online on
//                            16,384 live scene samples per frame (forward, backprop, Adam), then queried
//                            for every pixel at half resolution.
//
// Gradients are summed with 32-bit fixed-point atomics, so training is deterministic whatever order the
// GPU runs threads in. Every pass is plain SM 6.0 HLSL and runs on any D3D12 GPU.
//
// Built with -DBOO_NEURAL_LINALG=1 (cs_6_10, DXC preview), the inference matrix-vector products run on
// tensor cores through SM 6.10 LinAlg thread-scope matrices in FP16. Training stays FP32 on shader cores.

#define HID 32
#define SHADE_IN 16
#define MLP_PARAMS (HID * SHADE_IN + HID + HID * HID + HID + 3 * HID + 3) // 1699
#define NTC_LAT 64
#define NTC_C 8
#define NTC_TEX 512

#define NRC_LEVELS 4
#define NRC_FEAT 2
#define NRC_IN (NRC_LEVELS * NRC_FEAT + 1) // 8 grid features + distance
#define NRC_GRID_PARAMS 43520              // 2 * (16^2 + 32^2 + 64^2 + 128^2)
#define NRC_MLP_PARAMS (HID * NRC_IN + HID + HID * HID + HID + 3 * HID + 3) // 1475
#define NRC_PARAMS (NRC_GRID_PARAMS + NRC_MLP_PARAMS)
#define NRC_SAMPLES 16384
#define GRAD_SCALE 65536.0
#define STAT_SCALE 65536.0

cbuffer NeuralConstants : register(b0)
{
    uint gCanvasW;
    uint gCanvasH;
    uint gFrame;        // presentation frame, seeds the NRC sample pattern
    uint gFlags;        // bit0 shading, bits1..2 solo (0 none, 1 shading, 2 textures, 3 cache), bit3 stats
    uint gAdamStep;     // 1-based optimizer step for bias correction
    float gLearnRate;
    float gDistanceScale;
    uint gPad;
};

Texture2D<float>          gDepth     : register(t0);
StructuredBuffer<float>   gWeights   : register(t1); // [shade MLP][ntc MLP][latent scale 8][latent bias 8]
ByteAddressBuffer         gLatents   : register(t2); // NTC_LAT^2 cells x 8 bytes
ByteAddressBuffer         gReference : register(t3); // NTC_TEX^2 RGBA8

RWTexture2D<float4>       gHdr       : register(u0);
RWStructuredBuffer<float> gNrcParams : register(u1);
RWStructuredBuffer<float2> gNrcAdam  : register(u2);
RWStructuredBuffer<int>   gNrcGrad   : register(u3);
RWTexture2D<float4>       gNtcOut    : register(u4);
RWTexture2D<float4>       gNrcOut    : register(u5);
RWByteAddressBuffer       gStats     : register(u6);
// gStats (uint): 0 shading sq-error, 1 shading pixels, 2 ntc sq-error, 3 ntc texels, 4 nrc loss, 5 nrc samples

// FP16 weight matrices for the tensor-core path, row-major, each matrix 128-byte aligned:
//   gMlpF16: shading W1 32x16 @0, W2 32x32 @1024, W3 4x32 @3072 (row 3 zero); texture decoder same at +3328.
//   gNrcF16: cache W1 32x16 (inputs 9..15 zero) @0, W2 @1024, W3 @3072, rewritten from gNrcParams each frame.
ByteAddressBuffer         gMlpF16    : register(t4);
ByteAddressBuffer         gNrcF16    : register(t5);
RWByteAddressBuffer       gNrcF16Out : register(u7);
#define F16_NET_BYTES 3328u

static const uint kShadeBase = 0u;
static const uint kNtcBase = MLP_PARAMS;
static const uint kLatScale = 2u * MLP_PARAMS;
static const uint kLatBias = 2u * MLP_PARAMS + 8u;

float tmap(float x) { return x / (1.0 + x); }
float3 tmap3(float3 x) { return x / (1.0 + x); }
float lum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float distance01(float d) { return saturate(d / (d + gDistanceScale)); }

uint hash32(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// ---------------------------------------------------------------------------------------------
// Offline-trained MLP evaluation (shading and texture decoder share the 16 -> 32 -> 32 -> 3 shape).
// Layout per network: W1[32][16], b1[32], W2[32][32], b2[32], W3[3][32], b3[3].
#ifdef BOO_NEURAL_LINALG
#include <dx/linalg.h>
typedef dx::linalg::Matrix<dx::linalg::ComponentType::F16, HID, 16, dx::linalg::MatrixUse::A, dx::linalg::MatrixScope::Thread> MatHx16;
typedef dx::linalg::Matrix<dx::linalg::ComponentType::F16, HID, HID, dx::linalg::MatrixUse::A, dx::linalg::MatrixScope::Thread> MatHxH;
typedef dx::linalg::Matrix<dx::linalg::ComponentType::F16, 4, HID, dx::linalg::MatrixUse::A, dx::linalg::MatrixScope::Thread> Mat4xH;

// Three tensor-core layers. Biases come from the FP32 weights and are added at FP32 before each ReLU.
float3 tensorMlp(ByteAddressBuffer w, uint f16Base, vector<float16_t, 16> v, uint b1, uint b2, uint b3, bool nrcBias)
{
    const vector<float16_t, HID> z1 = dx::linalg::Multiply<float16_t>(MatHx16::Load<dx::linalg::MatrixLayout::RowMajor>(w, f16Base, 32), v);
    vector<float16_t, HID> h1;
    [unroll] for (uint o = 0; o < HID; ++o) h1[o] = (float16_t)max((float)z1[o] + (nrcBias ? gNrcParams[b1 + o] : gWeights[b1 + o]), 0.0);
    const vector<float16_t, HID> z2 = dx::linalg::Multiply<float16_t>(MatHxH::Load<dx::linalg::MatrixLayout::RowMajor>(w, f16Base + 1024u, 64), h1);
    vector<float16_t, HID> h2;
    [unroll] for (uint o2 = 0; o2 < HID; ++o2) h2[o2] = (float16_t)max((float)z2[o2] + (nrcBias ? gNrcParams[b2 + o2] : gWeights[b2 + o2]), 0.0);
    const vector<float16_t, 4> y = dx::linalg::Multiply<float16_t>(Mat4xH::Load<dx::linalg::MatrixLayout::RowMajor>(w, f16Base + 3072u, 64), h2);
    float3 r;
    [unroll] for (uint c = 0; c < 3; ++c) r[c] = (float)y[c] + (nrcBias ? gNrcParams[b3 + c] : gWeights[b3 + c]);
    return r;
}
#endif

float3 evalMlp16(uint base, float x[SHADE_IN])
{
#ifdef BOO_NEURAL_LINALG
    vector<float16_t, 16> v;
    [unroll] for (uint i0 = 0; i0 < SHADE_IN; ++i0) v[i0] = (float16_t)x[i0];
    const uint b1 = base + HID * SHADE_IN, b2 = b1 + HID + HID * HID, b3 = b2 + HID + 3 * HID;
    return tensorMlp(gMlpF16, base == kShadeBase ? 0u : F16_NET_BYTES, v, b1, b2, b3, false);
#else
    float h1[HID];
    [unroll] for (uint o = 0; o < HID; ++o) {
        float acc = gWeights[base + HID * SHADE_IN + o];
        [unroll] for (uint i = 0; i < SHADE_IN; ++i) acc += gWeights[base + o * SHADE_IN + i] * x[i];
        h1[o] = max(acc, 0.0);
    }
    const uint w2 = base + HID * SHADE_IN + HID;
    float h2[HID];
    [unroll] for (uint o2 = 0; o2 < HID; ++o2) {
        float acc = gWeights[w2 + HID * HID + o2];
        [unroll] for (uint i = 0; i < HID; ++i) acc += gWeights[w2 + o2 * HID + i] * h1[i];
        h2[o2] = max(acc, 0.0);
    }
    const uint w3 = w2 + HID * HID + HID;
    float3 y;
    [unroll] for (uint c = 0; c < 3; ++c) {
        float acc = gWeights[w3 + 3 * HID + c];
        [unroll] for (uint i = 0; i < HID; ++i) acc += gWeights[w3 + c * HID + i] * h2[i];
        y[c] = acc;
    }
    return y;
#endif
}

// Mirrors film_reference() in tools/train_neural_weights.py.
float3 filmReference(float3 rgb, float d01, float2 uv)
{
    float3 c = float3(dot(rgb, float3(0.92, 0.06, 0.02)),
                      dot(rgb, float3(0.05, 0.89, 0.06)),
                      dot(rgb, float3(0.02, 0.07, 0.91)));
    c += 0.020 * float3(1.00, 0.62, 0.80) * exp(-6.0 * lum(c));
    c = c / (1.0 + 0.04 * c);
    const float haze = 0.10 * d01 * d01;
    c += (lum(c) * float3(0.85, 0.95, 1.15) - c) * haze;
    const float2 q = uv - 0.5;
    return c * (1.0 - 0.22 * dot(q, q));
}

// Mirrors shade_features() in tools/train_neural_weights.py.
void shadeFeatures(float3 rgb, float d01, float2 uv, out float x[SHADE_IN])
{
    const float Y = lum(rgb);
    const float2 q = uv - 0.5;
    const float pi = 3.14159265;
    x[0] = tmap(rgb.r); x[1] = tmap(rgb.g); x[2] = tmap(rgb.b); x[3] = tmap(Y);
    x[4] = log2(Y + 1e-4) / 12.0; x[5] = d01; x[6] = d01 * d01; x[7] = uv.x; x[8] = uv.y;
    x[9] = dot(q, q);
    x[10] = sin(pi * uv.x); x[11] = cos(pi * uv.x); x[12] = sin(pi * uv.y); x[13] = cos(pi * uv.y);
    x[14] = sin(2.0 * pi * uv.x); x[15] = sin(2.0 * pi * uv.y);
}

// Neural shading: returns the graded colour in tone-mapped [0, 1) space.
float3 neuralShadeT(float3 rgb, float d01, float2 uv)
{
    float x[SHADE_IN];
    shadeFeatures(rgb, d01, uv, x);
    return clamp(tmap3(rgb) + evalMlp16(kShadeBase, x), 0.0, 0.999);
}

// ---------------------------------------------------------------------------------------------
// Neural texture decode
float latentChannel(uint cell, uint c)
{
    const uint word = gLatents.Load(cell * 8u + (c & 4u));
    const float q = float((word >> ((c & 3u) * 8u)) & 0xFFu);
    return q * gWeights[kLatScale + c] + gWeights[kLatBias + c];
}

float3 ntcDecode(float2 uv)
{
    const float2 f = uv * NTC_LAT - 0.5;
    const int2 b = int2(floor(f));
    const float2 t = f - float2(b);
    const int2 p0 = clamp(b, 0, NTC_LAT - 1);
    const int2 p1 = clamp(b + 1, 0, NTC_LAT - 1);
    const uint c00 = p0.y * NTC_LAT + p0.x, c10 = p0.y * NTC_LAT + p1.x;
    const uint c01 = p1.y * NTC_LAT + p0.x, c11 = p1.y * NTC_LAT + p1.x;
    float x[SHADE_IN];
    [unroll] for (uint c = 0; c < NTC_C; ++c) {
        x[c] = lerp(lerp(latentChannel(c00, c), latentChannel(c10, c), t.x),
                    lerp(latentChannel(c01, c), latentChannel(c11, c), t.x), t.y);
    }
    const float twoPi = 6.28318531;
    [unroll] for (uint k = 0; k < 2; ++k) {
        const float s = twoPi * (k + 1.0) * NTC_LAT;
        x[8 + k * 4 + 0] = sin(s * uv.x); x[8 + k * 4 + 1] = cos(s * uv.x);
        x[8 + k * 4 + 2] = sin(s * uv.y); x[8 + k * 4 + 3] = cos(s * uv.y);
    }
    return saturate(evalMlp16(kNtcBase, x));
}

float3 referenceTexel(int2 p)
{
    p = clamp(p, 0, NTC_TEX - 1);
    const uint v = gReference.Load((p.y * NTC_TEX + p.x) * 4u);
    return float3(v & 0xFFu, (v >> 8u) & 0xFFu, (v >> 16u) & 0xFFu) / 255.0;
}

groupshared float gsSum[64];

// One atomic per 64-thread group keeps the 32-bit fixed-point totals far from overflow.
void reduceStat(uint gi, float value, uint sumIndex, uint countIndex, uint count)
{
    gsSum[gi] = value;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 32u; s > 0u; s >>= 1u) {
        if (gi < s) gsSum[gi] += gsSum[gi + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0u) {
        uint ignored;
        gStats.InterlockedAdd(sumIndex * 4u, uint(min(gsSum[0], 60000.0) * STAT_SCALE + 0.5), ignored);
        gStats.InterlockedAdd(countIndex * 4u, count, ignored);
    }
}

[numthreads(64, 1, 1)]
void CSClearStats(uint id : SV_DispatchThreadID)
{
    if (id < 16u) gStats.Store(id * 4u, 0u);
}

[numthreads(8, 8, 1)]
void CSNtcDecode(uint2 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    const float2 uv = (float2(id) + 0.5) / NTC_TEX;
    const float3 rgb = ntcDecode(uv);
    gNtcOut[id] = float4(rgb, 1.0);
    const float3 stored = round(rgb * 255.0) / 255.0;
    const float3 e = stored - referenceTexel(int2(id));
    reduceStat(gi, dot(e, e) / 3.0, 2u, 3u, 64u);
}

// ---------------------------------------------------------------------------------------------
// Neural radiance cache
static const uint kLevelRes[NRC_LEVELS] = {16u, 32u, 64u, 128u};
static const uint kLevelOffset[NRC_LEVELS] = {0u, 512u, 2560u, 10752u};
static const uint kMlp = NRC_GRID_PARAMS;
static const uint kW1 = kMlp, kB1 = kMlp + HID * NRC_IN;
static const uint kW2 = kB1 + HID, kB2 = kW2 + HID * HID;
static const uint kW3 = kB2 + HID, kB3 = kW3 + 3 * HID;

struct GridTaps { uint idx[NRC_LEVELS][4]; float w[NRC_LEVELS][4]; };

GridTaps gridTaps(float2 uv)
{
    GridTaps g;
    [unroll] for (uint l = 0; l < NRC_LEVELS; ++l) {
        const float res = float(kLevelRes[l]);
        const float2 f = uv * res - 0.5;
        const int2 b = int2(floor(f));
        const float2 t = f - float2(b);
        const int2 p0 = clamp(b, 0, int(res) - 1);
        const int2 p1 = clamp(b + 1, 0, int(res) - 1);
        g.idx[l][0] = kLevelOffset[l] + (p0.y * kLevelRes[l] + p0.x) * NRC_FEAT;
        g.idx[l][1] = kLevelOffset[l] + (p0.y * kLevelRes[l] + p1.x) * NRC_FEAT;
        g.idx[l][2] = kLevelOffset[l] + (p1.y * kLevelRes[l] + p0.x) * NRC_FEAT;
        g.idx[l][3] = kLevelOffset[l] + (p1.y * kLevelRes[l] + p1.x) * NRC_FEAT;
        g.w[l][0] = (1.0 - t.x) * (1.0 - t.y); g.w[l][1] = t.x * (1.0 - t.y);
        g.w[l][2] = (1.0 - t.x) * t.y;         g.w[l][3] = t.x * t.y;
    }
    return g;
}

void nrcEncode(GridTaps g, float d01, out float x[NRC_IN])
{
    [unroll] for (uint l = 0; l < NRC_LEVELS; ++l) {
        [unroll] for (uint f = 0; f < NRC_FEAT; ++f) {
            float acc = 0.0;
            [unroll] for (uint k = 0; k < 4; ++k) acc += g.w[l][k] * gNrcParams[g.idx[l][k] + f];
            x[l * NRC_FEAT + f] = acc;
        }
    }
    x[NRC_IN - 1] = d01;
}

float3 nrcForward(float x[NRC_IN], out float z1[HID], out float h1[HID], out float z2[HID], out float h2[HID])
{
    [unroll] for (uint o = 0; o < HID; ++o) {
        float acc = gNrcParams[kB1 + o];
        [unroll] for (uint i = 0; i < NRC_IN; ++i) acc += gNrcParams[kW1 + o * NRC_IN + i] * x[i];
        z1[o] = acc; h1[o] = max(acc, 0.0);
    }
    [unroll] for (uint o2 = 0; o2 < HID; ++o2) {
        float acc = gNrcParams[kB2 + o2];
        [unroll] for (uint i = 0; i < HID; ++i) acc += gNrcParams[kW2 + o2 * HID + i] * h1[i];
        z2[o2] = acc; h2[o2] = max(acc, 0.0);
    }
    float3 y;
    [unroll] for (uint c = 0; c < 3; ++c) {
        float acc = gNrcParams[kB3 + c];
        [unroll] for (uint i = 0; i < HID; ++i) acc += gNrcParams[kW3 + c * HID + i] * h2[i];
        y[c] = acc;
    }
    return y;
}

// Radiance is learned in log space so highlights do not swamp the loss.
float3 nrcTarget(float3 hdr) { return log2(1.0 + clamp(hdr, 0.0, 255.0)); }
float3 nrcDecodeRadiance(float3 y) { return max(exp2(y) - 1.0, 0.0); }

uint paramSeed(uint i) { return hash32(i * 0x9E3779B9u + 0x6A09E667u); }

[numthreads(64, 1, 1)]
void CSNrcInit(uint id : SV_DispatchThreadID)
{
    if (id >= NRC_PARAMS) return;
    const float r = float(paramSeed(id) & 0xFFFFFFu) / 16777215.0 * 2.0 - 1.0;
    float value = 0.0;
    if (id < NRC_GRID_PARAMS) {
        value = r * 1e-4;
    } else if (id < kB1) {
        value = r * sqrt(6.0 / NRC_IN);
    } else if (id >= kW2 && id < kB2) {
        value = r * sqrt(6.0 / HID);
    } else if (id >= kW3 && id < kB3) {
        value = r * sqrt(6.0 / HID) * 0.1;
    }
    gNrcParams[id] = value;
    gNrcAdam[id] = float2(0.0, 0.0);
    gNrcGrad[id] = 0;
}

groupshared int gsGrad[NRC_MLP_PARAMS];
groupshared float gsLoss[128];

void addGrad(uint mlpIndex, float g)
{
    InterlockedAdd(gsGrad[mlpIndex], int(round(clamp(g, -0.5, 0.5) * GRAD_SCALE)));
}

[numthreads(128, 1, 1)]
void CSNrcTrain(uint id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    for (uint z = gi; z < NRC_MLP_PARAMS; z += 128u) gsGrad[z] = 0;
    GroupMemoryBarrierWithGroupSync();

    const uint h = hash32(id * 0x2545F491u ^ hash32(gFrame + 0x51u));
    const uint2 px = uint2(h % gCanvasW, hash32(h) % gCanvasH);
    const float2 uv = (float2(px) + 0.5) / float2(gCanvasW, gCanvasH);
    const float d01 = distance01(gDepth[px]);
    const float3 target = nrcTarget(gHdr[px].rgb);

    const GridTaps g = gridTaps(uv);
    float x[NRC_IN];
    nrcEncode(g, d01, x);
    float z1[HID], h1[HID], z2[HID], h2[HID];
    const float3 y = nrcForward(x, z1, h1, z2, h2);
    const float3 diff = y - target;
    const float3 dy = diff * (2.0 / 3.0);

    // Output layer
    float d2[HID];
    [unroll] for (uint i = 0; i < HID; ++i) d2[i] = 0.0;
    [unroll] for (uint c = 0; c < 3; ++c) {
        addGrad(kB3 - kMlp + c, dy[c]);
        [unroll] for (uint i2 = 0; i2 < HID; ++i2) {
            addGrad(kW3 - kMlp + c * HID + i2, dy[c] * h2[i2]);
            d2[i2] += dy[c] * gNrcParams[kW3 + c * HID + i2];
        }
    }
    // Hidden layer 2
    float d1[HID];
    [unroll] for (uint j = 0; j < HID; ++j) d1[j] = 0.0;
    [loop] for (uint o = 0; o < HID; ++o) {
        const float dz = z2[o] > 0.0 ? d2[o] : 0.0;
        if (dz != 0.0) {
            addGrad(kB2 - kMlp + o, dz);
            [unroll] for (uint i3 = 0; i3 < HID; ++i3) {
                addGrad(kW2 - kMlp + o * HID + i3, dz * h1[i3]);
                d1[i3] += dz * gNrcParams[kW2 + o * HID + i3];
            }
        }
    }
    // Hidden layer 1 and the gradient reaching the encoded grid features
    float dx[NRC_IN];
    [unroll] for (uint k = 0; k < NRC_IN; ++k) dx[k] = 0.0;
    [loop] for (uint o1 = 0; o1 < HID; ++o1) {
        const float dz = z1[o1] > 0.0 ? d1[o1] : 0.0;
        if (dz != 0.0) {
            addGrad(kB1 - kMlp + o1, dz);
            [unroll] for (uint i4 = 0; i4 < NRC_IN; ++i4) {
                addGrad(kW1 - kMlp + o1 * NRC_IN + i4, dz * x[i4]);
                dx[i4] += dz * gNrcParams[kW1 + o1 * NRC_IN + i4];
            }
        }
    }
    // Scatter into the feature grid: sparse, so straight to global memory.
    [unroll] for (uint l = 0; l < NRC_LEVELS; ++l) {
        [unroll] for (uint f = 0; f < NRC_FEAT; ++f) {
            const float gf = dx[l * NRC_FEAT + f];
            [unroll] for (uint t = 0; t < 4; ++t) {
                InterlockedAdd(gNrcGrad[g.idx[l][t] + f], int(round(clamp(gf * g.w[l][t], -0.5, 0.5) * GRAD_SCALE)));
            }
        }
    }

    gsLoss[gi] = dot(diff, diff) / 3.0;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 64u; s > 0u; s >>= 1u) {
        if (gi < s) gsLoss[gi] += gsLoss[gi + s];
        GroupMemoryBarrierWithGroupSync();
    }
    for (uint w = gi; w < NRC_MLP_PARAMS; w += 128u) {
        if (gsGrad[w] != 0) InterlockedAdd(gNrcGrad[kMlp + w], gsGrad[w]);
    }
    if (gi == 0u) {
        uint ignored;
        gStats.InterlockedAdd(16u, uint(min(gsLoss[0], 60000.0) * STAT_SCALE + 0.5), ignored);
        gStats.InterlockedAdd(20u, 128u, ignored);
    }
}

[numthreads(64, 1, 1)]
void CSNrcUpdate(uint id : SV_DispatchThreadID)
{
    if (id >= NRC_PARAMS) return;
    const float g = float(gNrcGrad[id]) / (GRAD_SCALE * NRC_SAMPLES);
    gNrcGrad[id] = 0;
    const float lr = gLearnRate * (id < NRC_GRID_PARAMS ? 1.0 : 0.25);
    float2 mv = gNrcAdam[id];
    mv.x = 0.9 * mv.x + 0.1 * g;
    mv.y = 0.99 * mv.y + 0.01 * g * g;
    gNrcAdam[id] = mv;
    const float t = float(gAdamStep);
    const float mh = mv.x / (1.0 - pow(0.9, t));
    const float vh = mv.y / (1.0 - pow(0.99, t));
    gNrcParams[id] -= lr * mh / (sqrt(vh) + 1e-10);
}

[numthreads(8, 8, 1)]
void CSNrcInfer(uint2 id : SV_DispatchThreadID)
{
    const uint w = (gCanvasW + 1u) / 2u, hgt = (gCanvasH + 1u) / 2u;
    if (id.x >= w || id.y >= hgt) return;
    const uint2 full = min(id * 2u, uint2(gCanvasW - 1u, gCanvasH - 1u));
    const float2 uv = (float2(id) + 0.5) / float2(w, hgt);
    float x[NRC_IN];
    nrcEncode(gridTaps(uv), distance01(gDepth[full]), x);
#ifdef BOO_NEURAL_LINALG
    vector<float16_t, 16> v = (vector<float16_t, 16>)0;
    [unroll] for (uint i = 0; i < NRC_IN; ++i) v[i] = (float16_t)x[i];
    gNrcOut[id] = float4(nrcDecodeRadiance(tensorMlp(gNrcF16, 0u, v, kB1, kB2, kB3, true)), 1.0);
#else
    float z1[HID], h1[HID], z2[HID], h2[HID];
    gNrcOut[id] = float4(nrcDecodeRadiance(nrcForward(x, z1, h1, z2, h2)), 1.0);
#endif
}

// Packs the freshly trained cache weights into the FP16 row-major matrices the tensor path loads.
// One thread per 32-bit word (two halves); 1,664 words.
float nrcF16Source(uint h)
{
    if (h < 512u) { const uint r = h / 16u, c = h % 16u; return c < NRC_IN ? gNrcParams[kW1 + r * NRC_IN + c] : 0.0; }
    if (h < 1536u) return gNrcParams[kW2 + (h - 512u)];
    const uint k = h - 1536u;
    return k < 96u ? gNrcParams[kW3 + k] : 0.0;
}

[numthreads(64, 1, 1)]
void CSNrcPackF16(uint id : SV_DispatchThreadID)
{
    if (id >= F16_NET_BYTES / 4u) return;
    gNrcF16Out.Store(id * 4u, f32tof16(nrcF16Source(id * 2u)) | (f32tof16(nrcF16Source(id * 2u + 1u)) << 16u));
}

// ---------------------------------------------------------------------------------------------
// Composite: neural shading and the three solo views. Writes the HDR scene in place.
float4 bilinearUav(RWTexture2D<float4> tex, float2 uv, uint2 size)
{
    const float2 f = uv * float2(size) - 0.5;
    const int2 b = int2(floor(f));
    const float2 t = f - float2(b);
    const int2 hi = int2(size) - 1;
    const float4 a = tex[clamp(b, 0, hi)], c = tex[clamp(b + int2(1, 0), 0, hi)];
    const float4 d = tex[clamp(b + int2(0, 1), 0, hi)], e = tex[clamp(b + int2(1, 1), 0, hi)];
    return lerp(lerp(a, c, t.x), lerp(d, e, t.x), t.y);
}

float3 srgbToLinear(float3 c) { return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045)); }

// Error palette in the film's colours: midnight navy -> velvet violet -> orchid pink -> warm pearl.
float3 heat(float v)
{
    v = saturate(v);
    const float3 c0 = float3(0.010, 0.016, 0.045), c1 = float3(0.20, 0.07, 0.42);
    const float3 c2 = float3(0.95, 0.24, 0.52), c3 = float3(1.00, 0.93, 0.82);
    return v < 0.33 ? lerp(c0, c1, v / 0.33) : (v < 0.66 ? lerp(c1, c2, (v - 0.33) / 0.33) : lerp(c2, c3, (v - 0.66) / 0.34));
}

[numthreads(8, 8, 1)]
void CSNeuralComposite(uint2 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    const bool inside = id.x < gCanvasW && id.y < gCanvasH;
    const uint2 p = min(id, uint2(gCanvasW - 1u, gCanvasH - 1u));
    const float2 res = float2(gCanvasW, gCanvasH);
    const float2 uv = (float2(p) + 0.5) / res;
    const uint solo = (gFlags >> 1u) & 3u;
    const bool shading = (gFlags & 1u) != 0u || solo == 1u;
    const float4 src = gHdr[p];
    const float3 rgb = max(src.rgb, 0.0);
    float3 outRgb = rgb;
    float err = 0.0;

    if (shading) {
        const float d01 = distance01(gDepth[p]);
        const float3 predT = neuralShadeT(rgb, d01, uv);
        const float3 e = predT - tmap3(max(filmReference(rgb, d01, uv), 0.0));
        err = dot(e, e) / 3.0;
        const float3 graded = predT / (1.0 - predT);
        outRgb = (solo == 1u && uv.x < 0.5) ? rgb : graded;
    }

    const float px = 1.0 / res.y;
    if (solo == 1u && abs(uv.x - 0.5) < 1.5 * px * res.y / res.x) outRgb = float3(2.0, 2.4, 2.2);

    if (solo == 2u) {
        // Three panels: original texture | neural decode | error x4.
        const float side = min(res.y * 0.72, res.x * 0.30);
        const float gap = (res.x - 3.0 * side) / 4.0;
        const float top = (res.y - side) * 0.5;
        // Backdrop: a quiet midnight-teal gradient with a faint vignette so the three panels float.
        const float2 cq = uv - 0.5;
        outRgb = lerp(float3(0.006, 0.020, 0.030), float3(0.020, 0.010, 0.040), uv.y) * (1.0 - 0.9 * dot(cq, cq));
        [unroll] for (uint panel = 0; panel < 3; ++panel) {
            const float x0 = gap + panel * (side + gap);
            const float2 q = (float2(p) + 0.5 - float2(x0, top)) / side;
            const float2 qa = abs(q - 0.5);
            if (max(qa.x, qa.y) <= 0.5) {
                const float2 tpx = q * NTC_TEX;
                const float3 ref = referenceTexel(int2(tpx));
                const float3 dec = gNtcOut[min(uint2(tpx), uint2(NTC_TEX - 1u, NTC_TEX - 1u))].rgb;
                const float3 shown = panel == 0u ? ref : (panel == 1u ? dec : heat(length(dec - ref) * 4.0));
                outRgb = srgbToLinear(shown) * 1.15;
            } else if (max(qa.x, qa.y) <= 0.5 + 3.0 * px * res.y / side) {
                // Frames: jade for the neural decode, pearl for the original, orchid for the error map.
                outRgb = panel == 1u ? float3(0.25, 1.10, 0.75) : (panel == 0u ? float3(0.80, 0.82, 0.86) : float3(1.10, 0.35, 0.70));
            } else if (max(qa.x, qa.y) <= 0.5 + 14.0 * px * res.y / side) {
                // Soft glow around each frame.
                const float d = (max(qa.x, qa.y) - 0.5) * side / (14.0 * px * res.y);
                const float3 glow = panel == 1u ? float3(0.05, 0.30, 0.20) : (panel == 0u ? float3(0.12, 0.13, 0.15) : float3(0.30, 0.06, 0.18));
                outRgb += glow * (1.0 - d) * (1.0 - d);
            }
        }
    }

    if (solo == 3u) {
        const uint2 half = uint2((gCanvasW + 1u) / 2u, (gCanvasH + 1u) / 2u);
        const float3 cache = bilinearUav(gNrcOut, uv, half).rgb;
        outRgb = uv.x < 0.5 ? rgb : cache;
        if (abs(uv.x - 0.5) < 1.5 * px * res.y / res.x) outRgb = float3(2.4, 1.6, 0.6);
    }

    if (inside) gHdr[p] = float4(outRgb, src.a);
    if ((gFlags & 8u) != 0u && shading) reduceStat(gi, inside ? err : 0.0, 0u, 1u, 0u);
    if ((gFlags & 8u) != 0u && shading && gi == 0u) {
        // Pixel count is added separately so partial edge groups count only their inside pixels.
        const uint2 g0 = id - uint2(gi % 8u, gi / 8u);
        const uint2 cnt = min(uint2(8u, 8u), uint2(gCanvasW, gCanvasH) - min(g0, uint2(gCanvasW, gCanvasH)));
        uint ignored;
        gStats.InterlockedAdd(4u, cnt.x * cnt.y, ignored);
    }
}
