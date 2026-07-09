// NRD resolve / debug pass.
//
// mode == 0 (None): re-modulate ReBLUR's denoised diffuse/specular (YCoCg + normHitDist) by the same
// factors nrd_pack demodulated with, composite, and fall back to raw radiance for sky (NRD does not
// write pixels beyond the denoising range). Other modes visualize an NRD input/output channel for
// headless inspection. `mode` must match NrdDebugMode in NrdConfig.h.

#include "../include/nrd.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    uint mode;
    float demodEps;
    float viewZScale;   // debug display: view_z / viewZScale
    float motionScale;  // debug display: uv motion * motionScale
    // Convergence handoff: ReBLUR's capped history has a permanent noise floor (it re-blends fresh 1-spp
    // noise every frame), so a static view would shimmer forever. Cross-fade to the progressive
    // accumulator as it converges; under motion the accumulator resets, so the weight is 0 there.
    float handoffWeight;
    // output-side temporal EMA (0 = off): displayed change per frame is capped at (1-beta) of any input
    // change, so isolated pops (ReBLUR churn, fresh accumulator samples) become invisible micro-ramps on a
    // static view. Keyed to the accumulator's sample count on the CPU side: resets to 0 under motion.
    float stabilizationBeta;
}

[[vk::binding(1, 0)]] Texture2D<float4> denoisedDiff; // OUT_DIFF_RADIANCE_HITDIST (YCoCg, w: normHitDist)
[[vk::binding(2, 0)]] Texture2D<float4> denoisedSpec; // OUT_SPEC_RADIANCE_HITDIST
[[vk::binding(3, 0)]] Texture2D<float4> inMv;
[[vk::binding(4, 0)]] Texture2D<float4> inNormalRoughness;
[[vk::binding(5, 0)]] Texture2D<float4> inViewZ;
[[vk::binding(6, 0)]] Texture2D<float4> inDiff;
[[vk::binding(7, 0)]] Texture2D<float4> inSpec;
[[vk::binding(8, 0)]] Texture2D<float4> gRadiance;   // raw per-frame radiance (sky fallback)
[[vk::binding(9, 0)]] Texture2D<float4> gAlbedoObj;
[[vk::binding(10, 0)]] Texture2D<float4> gMotion;    // z: is-hit
[[vk::binding(11, 0)]] Texture2D<float4> gSpecAlbedo;
[[vk::binding(12, 0)]] RWTexture2D<float4> outputImage;
[[vk::binding(14, 0)]] Texture2D<float4> validation; // NRD OUT_VALIDATION overlay (.w: transparency)
[[vk::binding(15, 0)]] Texture2D<float4> sceneAccum;  // progressive accumulator (converged reference)
[[vk::binding(16, 0)]] RWTexture2D<float4> outputHistory; // previous stabilized output (read-modify-write)

[shader("compute")]
[numthreads(16, 16, 1)] void shader_main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float3 color;
    switch (mode)
    {
    case 1: // ViewZ (grayscale; sky saturates white)
        color = ((float3)1) * saturate(abs(inViewZ.Load(int3(pixel, 0)).x) / viewZScale);
        break;
    case 2: // NormalRoughness (N remapped to color)
        color = NrdUnpackNormalRoughness(inNormalRoughness.Load(int3(pixel, 0))).xyz * 0.5f + 0.5f;
        break;
    case 3: // Roughness (grayscale)
        color = ((float3)1) * NrdUnpackNormalRoughness(inNormalRoughness.Load(int3(pixel, 0))).w;
        break;
    case 4: // MotionVector (|mv| amplified; static camera => black)
        color = float3(abs(inMv.Load(int3(pixel, 0)).xy) * motionScale, 0.f);
        break;
    case 5: // Diffuse radiance input (YCoCg decoded for display)
        color = NrdYCoCgToLinear(inDiff.Load(int3(pixel, 0)).rgb);
        break;
    case 6: // Diffuse normalized hit distance (grayscale in [0,1])
        color = ((float3)1) * inDiff.Load(int3(pixel, 0)).w;
        break;
    case 7: // Specular radiance input
        color = NrdYCoCgToLinear(inSpec.Load(int3(pixel, 0)).rgb);
        break;
    case 8: // Specular normalized hit distance
        color = ((float3)1) * inSpec.Load(int3(pixel, 0)).w;
        break;
    case 9: // Denoised diffuse (demodulated space)
        color = NrdYCoCgToLinear(denoisedDiff.Load(int3(pixel, 0)).rgb);
        break;
    case 10: // Denoised specular (demodulated space)
        color = NrdYCoCgToLinear(denoisedSpec.Load(int3(pixel, 0)).rgb);
        break;
    case 11: // NRD validation overlay, composited over the raw radiance by its own transparency
    {
        float4 v = validation.Load(int3(pixel, 0));
        color = lerp(gRadiance.Load(int3(pixel, 0)).rgb, v.rgb, v.a);
        break;
    }
    default: { // None: re-modulate + composite
        bool is_hit = gMotion.Load(int3(pixel, 0)).z > 0.5f;
        if (!is_hit)
        {
            // accumulator, NOT per-frame radiance: silhouette pixels whose AA samples straddle
            // geometry/sky never enter NRD (is_hit=0) — per-frame radiance leaves them flickering at
            // 1-spp noise forever (the "fireflies on edges" horizon line); the accumulator converges.
            color = sceneAccum.Load(int3(pixel, 0)).rgb;
            break;
        }
        float3 albedo = max(gAlbedoObj.Load(int3(pixel, 0)).rgb, float3(demodEps, demodEps, demodEps));
        float3 spec_demod = max(gSpecAlbedo.Load(int3(pixel, 0)).rgb, float3(demodEps, demodEps, demodEps));
        float3 diff = NrdYCoCgToLinear(denoisedDiff.Load(int3(pixel, 0)).rgb);
        float3 spec = NrdYCoCgToLinear(denoisedSpec.Load(int3(pixel, 0)).rgb);
        color = diff * albedo + spec * spec_demod;
        break;
    }
    }

    if (mode == 0)
    {
        // at weight 1 the accumulator must come through bit-exact: lerp(a, b, 1) still propagates
        // NaN/Inf from a, so select instead of blending
        float3 accum = sceneAccum.Load(int3(pixel, 0)).rgb;
        color = handoffWeight >= 1.f ? accum : lerp(color, accum, handoffWeight);
        // outputHistory is never cleared; on reset frames (beta == 0) it may hold NaN garbage, which
        // lerp(a, b, 0) still propagates — skip the read entirely so the write below (re)initializes it.
        if (stabilizationBeta > 0.f)
        {
            color = lerp(color, outputHistory[pixel].rgb, stabilizationBeta);
        }
        outputHistory[pixel] = float4(color, 1.f);
    }

    outputImage[pixel] = float4(color, 1.f);
}
