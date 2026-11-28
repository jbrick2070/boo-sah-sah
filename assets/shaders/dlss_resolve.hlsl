Texture2D<float4> sceneColor : register(t0);
Texture2D<float> sceneDistance : register(t1);
Texture2D<float> sceneDofDistance : register(t2);
RWTexture2D<float4> resolvedColor : register(u0);
RWTexture2D<float> resolvedDistance : register(u1);
RWTexture2D<float> resolvedDofDistance : register(u2);
SamplerState linearClamp : register(s0);
cbuffer ResolveFlags : register(b0) { uint spatialColor; uint resolveDofDistance; }

// Particles and lens effects use full-resolution world distance after Super Resolution.
// A failed NGX evaluate can still display the current frame using the spatial sampler.
[numthreads(8, 8, 1)]
void CSResolve(uint3 id : SV_DispatchThreadID) {
    uint2 outputSize, inputSize;
    resolvedDistance.GetDimensions(outputSize.x, outputSize.y);
    sceneDistance.GetDimensions(inputSize.x, inputSize.y);
    if (any(id.xy >= outputSize)) return;
    float2 uv = (id.xy + 0.5) / float2(outputSize);
    resolvedDistance[id.xy] = sceneDistance.Load(int3(min(uint2(uv * inputSize), inputSize - 1), 0));
    if (resolveDofDistance != 0)
        resolvedDofDistance[id.xy] = sceneDofDistance.Load(int3(min(uint2(uv * inputSize), inputSize - 1), 0));
    if (spatialColor != 0) resolvedColor[id.xy] = sceneColor.SampleLevel(linearClamp, uv, 0);
}
