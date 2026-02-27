[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float2 params;
};

[[vk::binding(1, 0)]] Texture2D<float> in_view_z;
[[vk::binding(2, 0)]] Texture2D<float4> in_diff_noisy;
[[vk::binding(3, 0)]] Texture2D<float4> in_spec_noisy;
[[vk::binding(4, 0)]] RWTexture2D<float4> out_denoised_output;

bool IsFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool IsValidViewZ(float view_z)
{
    float denoising_range = params.x;
    return IsFinite(view_z) && view_z > 0.0f && view_z <= denoising_range;
}

float3 YCoCgToLinear(float3 ycocg)
{
    float t = ycocg.x - ycocg.z;
    float r = t + ycocg.y;
    float g = ycocg.x + ycocg.z;
    float b = t - ycocg.y;
    return max(float3(r, g, b), 0.0f);
}

[shader("compute")][numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float split_screen = saturate(params.y);
    float2 pixel_uv = (float2(pixel) + 0.5f) / float2(resolution);
    if (pixel_uv.x > split_screen)
    {
        return;
    }

    float view_z = in_view_z.Load(int3(pixel, 0));
    float3 noisy_rgb = 0.0f;
    if (IsValidViewZ(view_z))
    {
        float3 diff = YCoCgToLinear(in_diff_noisy.Load(int3(pixel, 0)).xyz);
        float3 spec = YCoCgToLinear(in_spec_noisy.Load(int3(pixel, 0)).xyz);
        noisy_rgb = max(diff + spec, 0.0f);
    }

    out_denoised_output[pixel] = float4(noisy_rgb, 1.0f);
}
