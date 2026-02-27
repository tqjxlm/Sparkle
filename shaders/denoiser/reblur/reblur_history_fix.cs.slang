[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float4 params0;
    float4 params1;
};

[[vk::binding(1, 0)]] Texture2D<uint> in_tiles;
[[vk::binding(2, 0)]] Texture2D<float4> in_normal_roughness;
[[vk::binding(3, 0)]] Texture2D<float4> in_data1;
[[vk::binding(4, 0)]] Texture2D<float> in_view_z;
[[vk::binding(5, 0)]] Texture2D<float4> in_diff;
[[vk::binding(6, 0)]] Texture2D<float4> in_spec;
[[vk::binding(7, 0)]] Texture2D<float4> in_diff_fast;
[[vk::binding(8, 0)]] Texture2D<float4> in_spec_fast;
[[vk::binding(9, 0)]] Texture2D<float> in_spec_hit_dist_for_tracking;
[[vk::binding(10, 0)]] RWTexture2D<float4> out_diff;
[[vk::binding(11, 0)]] RWTexture2D<float4> out_spec;
[[vk::binding(12, 0)]] RWTexture2D<float4> out_diff_fast;
[[vk::binding(13, 0)]] RWTexture2D<float4> out_spec_fast;

static const float Epsilon = 1e-6f;
static const int HistoryFixFilterRadius = 2;

bool IsFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool IsValidViewZ(float view_z)
{
    float denoising_range = params0.x;
    return IsFinite(view_z) && view_z > 0.0f && view_z <= denoising_range;
}

bool IsInside(int2 pixel)
{
    return pixel.x >= 0 && pixel.y >= 0 && pixel.x < int(resolution.x) && pixel.y < int(resolution.y);
}

float3 UnpackNormal(float4 packed_normal_roughness)
{
    float3 normal = packed_normal_roughness.xyz * 2.0f - 1.0f;
    float normal_length = length(normal);
    return normal_length > Epsilon ? normal / normal_length : float3(0.0f, 0.0f, 1.0f);
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

float ComputeHistoryFixStride(float history_length, float history_fix_frame_num, float base_stride, float roughness,
                              float normalized_hit_distance)
{
    float early_history_factor = saturate((history_fix_frame_num - history_length) / max(history_fix_frame_num, 1.0f));
    float roughness_factor = lerp(1.0f, 0.45f, saturate(roughness));
    float hit_distance_factor = 0.35f + 0.65f * sqrt(saturate(normalized_hit_distance));
    float stride = 1.0f + base_stride * early_history_factor * roughness_factor * hit_distance_factor;
    return clamp(round(stride), 1.0f, 4.0f);
}

float ComputeNeighborWeight(uint2 center_pixel, uint2 neighbor_pixel, float center_view_z, float3 center_normal,
                            float3 neighbor_normal, float stride_i)
{
    float2 offset = float2(neighbor_pixel) - float2(center_pixel);
    float distance_squared = dot(offset, offset);
    float sigma = max(stride_i * 1.25f, 1.0f);
    float distance_weight = exp(-distance_squared / (2.0f * sigma * sigma));

    float neighbor_view_z = in_view_z.Load(int3(neighbor_pixel, 0));
    float relative_depth_delta = abs(neighbor_view_z - center_view_z) / max(center_view_z, Epsilon);
    float depth_weight = exp(-relative_depth_delta * 20.0f);

    float normal_weight = pow(saturate(dot(center_normal, neighbor_normal)), 8.0f);
    return distance_weight * depth_weight * normal_weight;
}

float4 ReconstructHistory(Texture2D<float4> signal, uint2 pixel, float center_view_z, float3 center_normal, float stride_i)
{
    float4 center_signal = signal.Load(int3(pixel, 0));
    float4 weighted_sum = center_signal;
    float sum_weights = 1.0f;

    int stride_i_int = int(stride_i);
    for (int offset_y = -HistoryFixFilterRadius; offset_y <= HistoryFixFilterRadius; offset_y++)
    {
        for (int offset_x = -HistoryFixFilterRadius; offset_x <= HistoryFixFilterRadius; offset_x++)
        {
            if (offset_x == 0 && offset_y == 0)
            {
                continue;
            }

            if (abs(offset_x) + abs(offset_y) == HistoryFixFilterRadius * 2)
            {
                continue;
            }

            int2 neighbor = int2(pixel) + int2(offset_x * stride_i_int, offset_y * stride_i_int);
            if (!IsInside(neighbor))
            {
                continue;
            }

            uint is_sky_tile = in_tiles.Load(int3(uint2(neighbor) >> 4, 0));
            if (is_sky_tile != 0u)
            {
                continue;
            }

            float neighbor_view_z = in_view_z.Load(int3(neighbor, 0));
            if (!IsValidViewZ(neighbor_view_z))
            {
                continue;
            }

            float3 neighbor_normal = UnpackNormal(in_normal_roughness.Load(int3(neighbor, 0)));
            float weight = ComputeNeighborWeight(pixel, uint2(neighbor), center_view_z, center_normal, neighbor_normal, stride_i);
            if (weight <= 0.0f)
            {
                continue;
            }

            weighted_sum += signal.Load(int3(neighbor, 0)) * weight;
            sum_weights += weight;
        }
    }

    float4 filtered = weighted_sum / max(sum_weights, Epsilon);
    filtered.w = saturate(filtered.w);
    return filtered;
}

void ComputeFastStats(Texture2D<float4> fast_signal, uint2 pixel, out float mean_luma, out float sigma_luma,
                      out float ring_mean_luma, out float ring_sigma_luma)
{
    float center_luma = GetLuma(fast_signal.Load(int3(pixel, 0)).xyz);
    float m1 = center_luma;
    float m2 = center_luma * center_luma;
    float weight_count = 1.0f;

    float ring_m1 = 0.0f;
    float ring_m2 = 0.0f;
    float ring_weight_count = 0.0f;

    for (int offset_y = -HistoryFixFilterRadius; offset_y <= HistoryFixFilterRadius; offset_y++)
    {
        for (int offset_x = -HistoryFixFilterRadius; offset_x <= HistoryFixFilterRadius; offset_x++)
        {
            if (offset_x == 0 && offset_y == 0)
            {
                continue;
            }

            int2 neighbor = int2(pixel) + int2(offset_x, offset_y);
            if (!IsInside(neighbor))
            {
                continue;
            }

            uint is_sky_tile = in_tiles.Load(int3(uint2(neighbor) >> 4, 0));
            float neighbor_view_z = in_view_z.Load(int3(neighbor, 0));
            if (is_sky_tile != 0u || !IsValidViewZ(neighbor_view_z))
            {
                continue;
            }

            float neighbor_luma = GetLuma(fast_signal.Load(int3(neighbor, 0)).xyz);
            m1 += neighbor_luma;
            m2 += neighbor_luma * neighbor_luma;
            weight_count += 1.0f;

            if (abs(offset_x) <= 1 && abs(offset_y) <= 1)
            {
                continue;
            }

            ring_m1 += neighbor_luma;
            ring_m2 += neighbor_luma * neighbor_luma;
            ring_weight_count += 1.0f;
        }
    }

    mean_luma = m1 / max(weight_count, 1.0f);
    sigma_luma = sqrt(max(m2 / max(weight_count, 1.0f) - mean_luma * mean_luma, 0.0f));

    if (ring_weight_count > 0.0f)
    {
        ring_mean_luma = ring_m1 / ring_weight_count;
        ring_sigma_luma = sqrt(max(ring_m2 / ring_weight_count - ring_mean_luma * ring_mean_luma, 0.0f));
    }
    else
    {
        ring_mean_luma = mean_luma;
        ring_sigma_luma = sigma_luma;
    }
}

[shader("compute")][numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float4 current_diff = in_diff.Load(int3(pixel, 0));
    float4 current_spec = in_spec.Load(int3(pixel, 0));
    float4 prev_fast_diff = in_diff_fast.Load(int3(pixel, 0));
    float4 prev_fast_spec = in_spec_fast.Load(int3(pixel, 0));

    uint is_sky_tile = in_tiles.Load(int3(pixel >> 4, 0));
    float center_view_z = in_view_z.Load(int3(pixel, 0));
    if (is_sky_tile != 0u || !IsValidViewZ(center_view_z))
    {
        out_diff[pixel] = current_diff;
        out_spec[pixel] = current_spec;
        out_diff_fast[pixel] = prev_fast_diff;
        out_spec_fast[pixel] = prev_fast_spec;
        return;
    }

    float4 normal_roughness = in_normal_roughness.Load(int3(pixel, 0));
    float3 center_normal = UnpackNormal(normal_roughness);
    float roughness = saturate(normal_roughness.w);

    float max_history_frames = max(params1.y, 1.0f);
    float history_fix_frame_num = max(params0.y, 1.0f);
    float history_length = saturate(in_data1.Load(int3(pixel, 0)).z) * max_history_frames;
    float base_stride = max(params0.z, 0.0f);
    float sigma_scale = max(params0.w, 0.25f);
    bool enable_anti_firefly = params1.x > 0.5f;

    float spec_hit_tracking = saturate(in_spec_hit_dist_for_tracking.Load(int3(pixel, 0)));
    float diff_stride = ComputeHistoryFixStride(history_length, history_fix_frame_num, base_stride, 0.0f, current_diff.w);
    float spec_stride =
        ComputeHistoryFixStride(history_length, history_fix_frame_num, base_stride, roughness, spec_hit_tracking);

    float4 reconstructed_diff = ReconstructHistory(in_diff, pixel, center_view_z, center_normal, diff_stride);
    float4 reconstructed_spec = ReconstructHistory(in_spec, pixel, center_view_z, center_normal, spec_stride);

    float diff_mean_luma;
    float diff_sigma_luma;
    float diff_ring_mean_luma;
    float diff_ring_sigma_luma;
    ComputeFastStats(in_diff_fast, pixel, diff_mean_luma, diff_sigma_luma, diff_ring_mean_luma, diff_ring_sigma_luma);

    float spec_mean_luma;
    float spec_sigma_luma;
    float spec_ring_mean_luma;
    float spec_ring_sigma_luma;
    ComputeFastStats(in_spec_fast, pixel, spec_mean_luma, spec_sigma_luma, spec_ring_mean_luma, spec_ring_sigma_luma);

    float reconstructed_diff_luma = GetLuma(reconstructed_diff.xyz);
    float reconstructed_spec_luma = GetLuma(reconstructed_spec.xyz);
    if (history_length <= 1.05f)
    {
        diff_mean_luma = reconstructed_diff_luma;
        spec_mean_luma = reconstructed_spec_luma;
    }

    if (enable_anti_firefly)
    {
        float diff_anti_sigma = max(diff_ring_sigma_luma * 0.85f, 0.01f);
        reconstructed_diff_luma = clamp(reconstructed_diff_luma, diff_ring_mean_luma - diff_anti_sigma,
                                        diff_ring_mean_luma + diff_anti_sigma);

        float spec_anti_sigma = max(spec_ring_sigma_luma * 0.85f, 0.01f);
        reconstructed_spec_luma = clamp(reconstructed_spec_luma, spec_ring_mean_luma - spec_anti_sigma,
                                        spec_ring_mean_luma + spec_anti_sigma);
    }

    float diff_fast_sigma = max(diff_sigma_luma * sigma_scale, 0.01f);
    float spec_fast_sigma = max(spec_sigma_luma * sigma_scale, 0.01f);
    float diff_clamped_luma =
        clamp(reconstructed_diff_luma, diff_mean_luma - diff_fast_sigma, diff_mean_luma + diff_fast_sigma);
    float spec_clamped_luma =
        clamp(reconstructed_spec_luma, spec_mean_luma - spec_fast_sigma, spec_mean_luma + spec_fast_sigma);

    float history_release = history_length / (history_fix_frame_num + history_length + Epsilon);
    float final_diff_luma = lerp(diff_clamped_luma, reconstructed_diff_luma, saturate(history_release));
    float final_spec_luma = lerp(spec_clamped_luma, reconstructed_spec_luma, saturate(history_release));

    float fast_history_mix = saturate(history_length / history_fix_frame_num);
    float updated_fast_diff_luma = lerp(final_diff_luma, GetLuma(prev_fast_diff.xyz), fast_history_mix);
    float updated_fast_spec_luma = lerp(final_spec_luma, GetLuma(prev_fast_spec.xyz), fast_history_mix);

    float4 final_diff = ChangeSignalLuma(reconstructed_diff, final_diff_luma);
    float4 final_spec = ChangeSignalLuma(reconstructed_spec, final_spec_luma);
    float4 final_fast_diff = ChangeSignalLuma(final_diff, updated_fast_diff_luma);
    float4 final_fast_spec = ChangeSignalLuma(final_spec, updated_fast_spec_luma);

    out_diff[pixel] = final_diff;
    out_spec[pixel] = final_spec;
    out_diff_fast[pixel] = final_fast_diff;
    out_spec_fast[pixel] = final_fast_spec;
}
