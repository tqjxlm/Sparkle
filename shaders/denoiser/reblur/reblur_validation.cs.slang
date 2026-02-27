[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float4 params;
};

[[vk::binding(1, 0)]] Texture2D<uint> in_tiles;
[[vk::binding(2, 0)]] Texture2D<float> in_view_z;
[[vk::binding(3, 0)]] Texture2D<float4> in_data1;
[[vk::binding(4, 0)]] Texture2D<float4> in_data2;
[[vk::binding(5, 0)]] Texture2D<float4> in_diff;
[[vk::binding(6, 0)]] Texture2D<float4> in_spec;
[[vk::binding(7, 0)]] RWTexture2D<float4> out_denoised_output;

bool IsFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool IsValidViewZ(float view_z)
{
    float denoising_range = params.x;
    return IsFinite(view_z) && view_z > 0.0f && view_z <= denoising_range;
}

bool IsValidNormHitDistance(float value)
{
    return IsFinite(value) && value >= 0.0f && value <= 1.0f;
}

[shader("compute")][numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    uint is_sky_tile = in_tiles.Load(int3(pixel >> 4, 0));
    float view_z = in_view_z.Load(int3(pixel, 0));
    if (is_sky_tile != 0u || !IsValidViewZ(view_z))
    {
        out_denoised_output[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float4 data1 = in_data1.Load(int3(pixel, 0));
    float4 data2 = in_data2.Load(int3(pixel, 0));
    float4 diff = in_diff.Load(int3(pixel, 0));
    float4 spec = in_spec.Load(int3(pixel, 0));

    float history_length_norm = saturate(data1.z);
    bool reprojection_invalid = data1.y < 0.5f && history_length_norm > params.w;
    bool disoccluded = data2.w > params.y;
    float depth_delta = saturate(data2.z);
    bool large_depth_delta = depth_delta > params.z;
    bool invalid_hit_distance = !IsValidNormHitDistance(diff.w) || !IsValidNormHitDistance(spec.w);

    float3 debug_color = 0.0f;
    if (disoccluded)
    {
        debug_color.r += 1.0f;
    }
    if (large_depth_delta)
    {
        debug_color.r += 0.5f * depth_delta;
    }
    if (reprojection_invalid)
    {
        debug_color.g += 1.0f;
    }
    if (invalid_hit_distance)
    {
        debug_color.b = 1.0f;
    }
    if (!disoccluded && !reprojection_invalid && !invalid_hit_distance && !large_depth_delta)
    {
        debug_color.g = 0.15f * (1.0f - history_length_norm);
    }

    out_denoised_output[pixel] = float4(saturate(debug_color), 1.0f);
}
