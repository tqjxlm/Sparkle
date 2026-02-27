[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float4 params0;
    float4 params1;
};

[[vk::binding(1, 0)]] Texture2D<uint> in_tiles;
[[vk::binding(2, 0)]] Texture2D<float4> in_normal_roughness;
[[vk::binding(3, 0)]] Texture2D<float> in_view_z;
[[vk::binding(4, 0)]] Texture2D<float4> in_motion_vectors;
[[vk::binding(5, 0)]] Texture2D<float4> in_diff;
[[vk::binding(6, 0)]] Texture2D<float4> in_spec;
[[vk::binding(7, 0)]] Texture2D<float> in_spec_hit_dist_for_tracking;
[[vk::binding(8, 0)]] Texture2D<float> prev_view_z;
[[vk::binding(9, 0)]] Texture2D<float4> prev_normal_roughness;
[[vk::binding(10, 0)]] Texture2D<float4> prev_internal_data;
[[vk::binding(11, 0)]] Texture2D<float4> prev_diff_history;
[[vk::binding(12, 0)]] Texture2D<float4> prev_spec_history;
[[vk::binding(13, 0)]] Texture2D<float4> prev_diff_fast_history;
[[vk::binding(14, 0)]] Texture2D<float4> prev_spec_fast_history;
[[vk::binding(15, 0)]] Texture2D<float> prev_spec_hit_dist_for_tracking;
[[vk::binding(16, 0)]] RWTexture2D<float4> out_data1;
[[vk::binding(17, 0)]] RWTexture2D<float4> out_data2;
[[vk::binding(18, 0)]] RWTexture2D<float4> out_internal_data;
[[vk::binding(19, 0)]] RWTexture2D<float4> out_diff_history;
[[vk::binding(20, 0)]] RWTexture2D<float4> out_spec_history;
[[vk::binding(21, 0)]] RWTexture2D<float4> out_diff_fast_history;
[[vk::binding(22, 0)]] RWTexture2D<float4> out_spec_fast_history;
[[vk::binding(23, 0)]] RWTexture2D<float> out_spec_hit_dist_for_tracking;

static const float Epsilon = 1e-6f;

bool IsFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool IsInside(int2 pixel)
{
    return pixel.x >= 0 && pixel.y >= 0 && pixel.x < int(resolution.x) && pixel.y < int(resolution.y);
}

bool IsValidViewZ(float view_z)
{
    float denoising_range = params0.x;
    return IsFinite(view_z) && view_z > 0.0f && view_z <= denoising_range;
}

float3 UnpackNormal(float4 packed_normal_roughness)
{
    float3 normal = packed_normal_roughness.xyz * 2.0f - 1.0f;
    float normal_length = length(normal);
    return normal_length > Epsilon ? normal / normal_length : float3(0.0f, 0.0f, 1.0f);
}

[shader("compute")][numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float4 current_diff = in_diff.Load(int3(pixel, 0));
    float4 current_spec = in_spec.Load(int3(pixel, 0));
    float current_tracking = saturate(in_spec_hit_dist_for_tracking.Load(int3(pixel, 0)));

    uint is_sky_tile = in_tiles.Load(int3(pixel >> 4, 0));
    float current_view_z = in_view_z.Load(int3(pixel, 0));
    if (is_sky_tile != 0u || !IsValidViewZ(current_view_z))
    {
        out_data1[pixel] = float4(0.0f, 0.0f, 0.0f, current_tracking);
        out_data2[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        out_internal_data[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        out_diff_history[pixel] = current_diff;
        out_spec_history[pixel] = current_spec;
        out_diff_fast_history[pixel] = current_diff;
        out_spec_fast_history[pixel] = current_spec;
        out_spec_hit_dist_for_tracking[pixel] = current_tracking;
        return;
    }

    float history_available = params1.x;
    float max_history_frames = max(params0.y, 1.0f);
    float max_fast_history_frames = max(params1.y, 1.0f);
    float disocclusion_threshold = max(params0.z, 0.0f);
    float normal_threshold = saturate(params0.w);

    float3 current_normal = UnpackNormal(in_normal_roughness.Load(int3(pixel, 0)));
    bool valid_reprojection = false;
    float depth_delta_norm = 1.0f;
    uint2 prev_pixel = pixel;

    if (history_available > 0.5f)
    {
        float2 motion_vector = in_motion_vectors.Load(int3(pixel, 0)).xy;
        int2 reprojected_pixel = int2(round(float2(pixel) + motion_vector));
        if (IsInside(reprojected_pixel))
        {
            prev_pixel = uint2(reprojected_pixel);
            float prev_depth = prev_view_z.Load(int3(prev_pixel, 0));
            if (IsValidViewZ(prev_depth))
            {
                float3 prev_normal = UnpackNormal(prev_normal_roughness.Load(int3(prev_pixel, 0)));
                float normal_cos = saturate(dot(current_normal, prev_normal));
                depth_delta_norm = abs(prev_depth - current_view_z) / max(current_view_z, Epsilon);
                valid_reprojection = depth_delta_norm <= disocclusion_threshold && normal_cos >= normal_threshold;
            }
        }
    }

    float history_length = 1.0f;
    float fast_history_length = 1.0f;
    float4 prev_diff_value = current_diff;
    float4 prev_spec_value = current_spec;
    float4 prev_diff_fast_value = current_diff;
    float4 prev_spec_fast_value = current_spec;
    float prev_tracking = current_tracking;
    if (valid_reprojection)
    {
        float4 prev_internal = prev_internal_data.Load(int3(prev_pixel, 0));
        history_length = clamp(prev_internal.x + 1.0f, 1.0f, max_history_frames);
        fast_history_length = clamp(prev_internal.y + 1.0f, 1.0f, max_fast_history_frames);
        prev_diff_value = prev_diff_history.Load(int3(prev_pixel, 0));
        prev_spec_value = prev_spec_history.Load(int3(prev_pixel, 0));
        prev_diff_fast_value = prev_diff_fast_history.Load(int3(prev_pixel, 0));
        prev_spec_fast_value = prev_spec_fast_history.Load(int3(prev_pixel, 0));
        prev_tracking = saturate(prev_spec_hit_dist_for_tracking.Load(int3(prev_pixel, 0)));
    }

    float history_alpha = valid_reprojection ? (1.0f / max(history_length, 1.0f)) : 1.0f;
    float fast_history_alpha = valid_reprojection ? (1.0f / max(fast_history_length, 1.0f)) : 1.0f;

    float4 accumulated_diff = lerp(prev_diff_value, current_diff, history_alpha);
    float4 accumulated_spec = lerp(prev_spec_value, current_spec, history_alpha);
    float4 accumulated_diff_fast = lerp(prev_diff_fast_value, current_diff, fast_history_alpha);
    float4 accumulated_spec_fast = lerp(prev_spec_fast_value, current_spec, fast_history_alpha);
    float accumulated_tracking = saturate(lerp(prev_tracking, current_tracking, fast_history_alpha));

    accumulated_diff.w = saturate(accumulated_diff.w);
    accumulated_spec.w = saturate(accumulated_spec.w);
    accumulated_diff_fast.w = saturate(accumulated_diff_fast.w);
    accumulated_spec_fast.w = saturate(accumulated_spec_fast.w);

    float history_denom = max(max_history_frames - 1.0f, 1.0f);
    float history_factor = saturate((history_length - 1.0f) / history_denom);
    float2 prev_uv = valid_reprojection ? (float2(prev_pixel) + 0.5f) / float2(resolution) : float2(0.0f, 0.0f);

    out_data1[pixel] = float4(history_factor, valid_reprojection ? 1.0f : 0.0f, history_length / max_history_frames,
                              accumulated_tracking);
    out_data2[pixel] = float4(prev_uv, saturate(depth_delta_norm), valid_reprojection ? 0.0f : 1.0f);
    out_internal_data[pixel] = float4(history_length, fast_history_length, valid_reprojection ? 1.0f : 0.0f, 0.0f);
    out_diff_history[pixel] = accumulated_diff;
    out_spec_history[pixel] = accumulated_spec;
    out_diff_fast_history[pixel] = accumulated_diff_fast;
    out_spec_fast_history[pixel] = accumulated_spec_fast;
    out_spec_hit_dist_for_tracking[pixel] = accumulated_tracking;
}
