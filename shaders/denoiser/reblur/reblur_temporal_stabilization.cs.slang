[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float4 params0;
    float4 params1;
};

[[vk::binding(1, 0)]] Texture2D<uint> in_tiles;
[[vk::binding(2, 0)]] Texture2D<float4> in_normal_roughness;
[[vk::binding(3, 0)]] Texture2D<float> in_view_z;
[[vk::binding(4, 0)]] Texture2D<float4> in_data1;
[[vk::binding(5, 0)]] Texture2D<float4> in_data2;
[[vk::binding(6, 0)]] Texture2D<float4> in_diff_history;
[[vk::binding(7, 0)]] Texture2D<float4> in_spec_history;
[[vk::binding(8, 0)]] Texture2D<float> in_spec_hit_dist_for_tracking;
[[vk::binding(9, 0)]] Texture2D<float> prev_diff_stabilized_luma;
[[vk::binding(10, 0)]] Texture2D<float> prev_spec_stabilized_luma;
[[vk::binding(11, 0)]] RWTexture2D<float4> out_internal_data;
[[vk::binding(12, 0)]] RWTexture2D<float> out_diff_stabilized_luma;
[[vk::binding(13, 0)]] RWTexture2D<float> out_spec_stabilized_luma;
[[vk::binding(14, 0)]] RWTexture2D<float4> out_denoised_output;

static const float Epsilon = 1e-6f;

bool IsFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool IsValidViewZ(float view_z)
{
    float denoising_range = params0.x;
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

float GetLuma(float3 rgb)
{
    return dot(max(rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
}

float4 ChangeSignalLuma(float4 signal, float target_luma)
{
    float3 color = max(signal.xyz, 0.0f);
    float source_luma = GetLuma(color);
    if (source_luma > Epsilon)
    {
        color *= target_luma / source_luma;
    }
    else
    {
        color = float3(target_luma, target_luma, target_luma);
    }

    signal.xyz = max(color, 0.0f);
    signal.w = saturate(signal.w);
    return signal;
}

[shader("compute")][numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float4 diff_history_value = in_diff_history.Load(int3(pixel, 0));
    float4 spec_history_value = in_spec_history.Load(int3(pixel, 0));
    float3 diff_linear = YCoCgToLinear(diff_history_value.xyz);
    float3 spec_linear = YCoCgToLinear(spec_history_value.xyz);
    float diff_luma = GetLuma(diff_linear);
    float spec_luma = GetLuma(spec_linear);

    float4 internal_data = out_internal_data[pixel];
    internal_data.z = 0.0f;
    internal_data.w = 0.0f;

    uint is_sky_tile = in_tiles.Load(int3(pixel >> 4, 0));
    float view_z = in_view_z.Load(int3(pixel, 0));
    if (is_sky_tile != 0u || !IsValidViewZ(view_z))
    {
        out_internal_data[pixel] = internal_data;
        out_diff_stabilized_luma[pixel] = max(diff_luma, 0.0f);
        out_spec_stabilized_luma[pixel] = max(spec_luma, 0.0f);
        out_denoised_output[pixel] = float4(max(diff_linear + spec_linear, 0.0f), 1.0f);
        return;
    }

    float stabilization_strength = saturate(params0.y);
    float max_stabilized_frame_num = max(params0.z, 1.0f);
    bool history_available = params0.w > 0.5f;
    bool enable_mv_patch = params1.x > 0.5f;
    float mv_patch_spec_threshold = saturate(params1.y);
    float mv_patch_strength = max(params1.z, 0.0f);

    float4 data1 = in_data1.Load(int3(pixel, 0));
    float4 data2 = in_data2.Load(int3(pixel, 0));
    bool valid_reprojection = history_available && data1.y > 0.5f && data2.w < 0.5f;
    float depth_delta_norm = saturate(data2.z);

    float2 current_uv = (float2(pixel) + 0.5f) / float2(resolution);
    float2 prev_uv = data2.xy;
    float mv_patch_scale = 0.0f;

    float roughness = saturate(in_normal_roughness.Load(int3(pixel, 0)).w);
    float tracking = saturate(in_spec_hit_dist_for_tracking.Load(int3(pixel, 0)));
    float total_luma = max(diff_luma + spec_luma, Epsilon);
    float spec_dominance = saturate(spec_luma / total_luma);

    if (valid_reprojection && enable_mv_patch)
    {
        float spec_factor =
            saturate((spec_dominance - mv_patch_spec_threshold) / max(1.0f - mv_patch_spec_threshold, Epsilon));
        float glossy_factor = saturate(1.0f - roughness);
        float tracking_factor = sqrt(tracking);
        mv_patch_scale =
            saturate(spec_factor * glossy_factor * tracking_factor * saturate(depth_delta_norm * mv_patch_strength));
        prev_uv = lerp(prev_uv, current_uv, mv_patch_scale);
    }

    int2 prev_pixel = int2(round(prev_uv * float2(resolution) - 0.5f));
    bool prev_inside =
        prev_pixel.x >= 0 && prev_pixel.y >= 0 && prev_pixel.x < int(resolution.x) && prev_pixel.y < int(resolution.y);

    float prev_diff_luma = diff_luma;
    float prev_spec_luma = spec_luma;
    if (valid_reprojection && prev_inside)
    {
        prev_diff_luma = max(prev_diff_stabilized_luma.Load(int3(prev_pixel, 0)), 0.0f);
        prev_spec_luma = max(prev_spec_stabilized_luma.Load(int3(prev_pixel, 0)), 0.0f);
    }

    float stabilized_history_length = saturate(data1.z) * (max_stabilized_frame_num - 1.0f) + 1.0f;
    float base_blend = stabilization_strength * (1.0f - 1.0f / max(stabilized_history_length, 1.0f));
    float motion_guard = 1.0f - saturate(depth_delta_norm * 6.0f);
    float blend = valid_reprojection && prev_inside ? base_blend * motion_guard : 0.0f;
    blend = saturate(blend * (1.0f - 0.85f * mv_patch_scale));

    float stabilized_diff_luma = lerp(diff_luma, prev_diff_luma, blend);
    float stabilized_spec_luma = lerp(spec_luma, prev_spec_luma, blend);

    float4 stabilized_diff_signal = ChangeSignalLuma(float4(diff_linear, diff_history_value.w), stabilized_diff_luma);
    float4 stabilized_spec_signal = ChangeSignalLuma(float4(spec_linear, spec_history_value.w), stabilized_spec_luma);
    float3 denoised = max(stabilized_diff_signal.xyz + stabilized_spec_signal.xyz, 0.0f);

    internal_data.z = blend;
    internal_data.w = mv_patch_scale;
    out_internal_data[pixel] = internal_data;
    out_diff_stabilized_luma[pixel] = max(stabilized_diff_luma, 0.0f);
    out_spec_stabilized_luma[pixel] = max(stabilized_spec_luma, 0.0f);
    out_denoised_output[pixel] = float4(denoised, 1.0f);
}
