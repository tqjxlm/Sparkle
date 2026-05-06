[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    uint frame_index;
    uint composite_mode; // 0 = normal, 1 = passthrough diffuse, 2 = diffuse, 3 = specular, 4 = albedo, 5 = passthrough specular
};

[[vk::binding(1, 0)]] Texture2D<float4> denoisedDiffuse;
[[vk::binding(2, 0)]] Texture2D<float4> denoisedSpecular;
[[vk::binding(3, 0)]] Texture2D<float4> albedoMetallic;
[[vk::binding(4, 0)]] RWTexture2D<float4> outputImage;
[[vk::binding(5, 0)]] Texture2D<float> viewZ;
[[vk::binding(6, 0)]] Texture2D<float2> internalData;
[[vk::binding(7, 0)]] Texture2D<float4> ptAccumulated;

static const float DenoisingRange = 1000.0;

// Number of accumulated samples at which composite fully transitions to the
// path tracer's accumulated result. Below this, the denoised output dominates
// (better spatial filtering at low sample counts). Above this, the PT result
// dominates (correct radiance-space convergence without demod/remod artifacts).
static const float PT_BLEND_RAMP = 256.0;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
        return;

    // Diagnostic passthrough: output denoisedDiffuse directly without albedo
    // modulation. Used for TA / TS diagnostics where the denoised output
    // contains debug data instead of radiance.
    if (composite_mode == 1)
    {
        outputImage[pixel] = denoisedDiffuse[pixel];
        return;
    }

    if (composite_mode == 5)
    {
        outputImage[pixel] = denoisedSpecular[pixel];
        return;
    }

    // Sky pixels already have correct color in outputImage from the split path tracer.
    // Skip them to preserve the vanilla accumulated sky color.
    float vz = viewZ[pixel];
    if (vz > DenoisingRange)
        return;

    // Read the path tracer's accumulated result from a separate buffer.
    // This avoids contamination: the split PT maintains its own running average
    // in ptAccumulated, independent of the composite's output to outputImage.
    float3 pt_result = ptAccumulated[pixel].rgb;

    float4 diff = denoisedDiffuse[pixel];
    float4 spec = denoisedSpecular[pixel];
    float4 albedo = albedoMetallic[pixel];

    if (composite_mode == 2)
    {
        outputImage[pixel] = float4(diff.rgb * albedo.rgb, 1.0);
        return;
    }

    if (composite_mode == 3)
    {
        outputImage[pixel] = float4(spec.rgb, 1.0);
        return;
    }

    if (composite_mode == 4)
    {
        outputImage[pixel] = float4(albedo.rgb, 1.0);
        return;
    }


    float3 denoised_color = diff.rgb * albedo.rgb + spec.rgb;

    // Blend denoised composite with path tracer accumulated result.
    //
    // At low SPP: the denoiser provides much better quality through spatial filtering,
    // so we favor the denoised output.
    //
    // At high SPP: the PT accumulated result converges correctly in radiance space
    // without demod/remod artifacts at object silhouette edges. It becomes the more
    // stable and accurate result.
    //
    // The internalData read prevents the compiler from optimizing away the binding.
    float2 packed = internalData[pixel];
    float min_accum = min(packed.x, packed.y);
    // Quadratic ramp: at low sample counts the raw PT is extremely noisy
    // (local_std ~ 1/sqrt(n)). A linear ramp lets 2% of 5-sample noise
    // through, which doubles the floor local_std. The quadratic curve
    // keeps the PT contribution negligible until enough samples exist:
    //   5 samples → 0.04% (invisible)
    //   64 samples → 6.25%
    //   256 samples → 100%
    float t = saturate(float(frame_index) / PT_BLEND_RAMP);
    float pt_weight = t * t;

    // Guard ensures internalData binding is not optimized away
    float3 color = (min_accum >= 0.0)
        ? lerp(denoised_color, pt_result, pt_weight)
        : denoised_color;

    outputImage[pixel] = float4(color, 1.0);
}
