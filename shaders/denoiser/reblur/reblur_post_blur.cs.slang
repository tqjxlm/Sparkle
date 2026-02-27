[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float2 params;
};

[[vk::binding(1, 0)]] Texture2D<uint> in_tiles;
[[vk::binding(2, 0)]] Texture2D<float4> in_normal_roughness;
[[vk::binding(3, 0)]] Texture2D<float> in_view_z;
[[vk::binding(4, 0)]] Texture2D<float4> in_diff;
[[vk::binding(5, 0)]] Texture2D<float4> in_spec;
[[vk::binding(6, 0)]] RWTexture2D<float4> out_prev_normal_roughness;
[[vk::binding(7, 0)]] RWTexture2D<float4> out_diff_history;
[[vk::binding(8, 0)]] RWTexture2D<float4> out_spec_history;
[[vk::binding(9, 0)]] RWTexture2D<float4> out_denoised_output;

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

    float4 diff = in_diff.Load(int3(pixel, 0));
    float4 spec = in_spec.Load(int3(pixel, 0));
    float4 normal_roughness = in_normal_roughness.Load(int3(pixel, 0));
    float view_z = in_view_z.Load(int3(pixel, 0));
    uint is_sky_tile = in_tiles.Load(int3(pixel >> 4, 0));

    out_prev_normal_roughness[pixel] = normal_roughness;
    out_diff_history[pixel] = diff;
    out_spec_history[pixel] = spec;

    float3 denoised = YCoCgToLinear(diff.xyz) + YCoCgToLinear(spec.xyz);
    if (is_sky_tile != 0u || !IsValidViewZ(view_z))
    {
        denoised = max(denoised, 0.0f);
    }
    out_denoised_output[pixel] = float4(denoised, 1.0f);
}
