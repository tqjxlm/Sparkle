// Convergence handoff for the MetalFX provider: cross-fade the scaler output into the progressive
// accumulator so the display equals the accumulator when accumulation completes, with no visible
// step (the scaler stays slightly soft even on converged input). Nearest accumulator sampling
// matches the tone-mapping upsample exactly, so the post-handoff switch to the accumulator is
// seamless at any render scale.

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 outputResolution;
    uint2 inputResolution;
    float handoffWeight;
    float3 padding;
}

[[vk::binding(1, 0)]] Texture2D<float4> scalerOutput;
[[vk::binding(2, 0)]] Texture2D<float4> sceneAccum;

[[vk::binding(3, 0)]] RWTexture2D<float4> outColor;

[shader("compute")][numthreads(16, 16, 1)] void shader_main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= outputResolution.x || pixel.y >= outputResolution.y)
    {
        return;
    }

    float3 denoised = scalerOutput.Load(int3(pixel, 0)).rgb;
    uint2 accum_pixel = uint2((float2(pixel) + 0.5f) * float2(inputResolution) / float2(outputResolution));
    float3 accumulated = sceneAccum.Load(int3(accum_pixel, 0)).rgb;

    outColor[pixel] = float4(lerp(denoised, accumulated, handoffWeight), 1.f);
}
