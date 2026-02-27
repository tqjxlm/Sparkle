[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float4 params;
};

[[vk::binding(1, 0)]] Texture2D<uint> in_tiles;
[[vk::binding(2, 0)]] Texture2D<float4> in_normal_roughness;
[[vk::binding(3, 0)]] Texture2D<float> in_view_z;
[[vk::binding(4, 0)]] Texture2D<float4> in_diff;
[[vk::binding(5, 0)]] Texture2D<float4> in_spec;
[[vk::binding(6, 0)]] Texture2D<float4> in_data1;
[[vk::binding(7, 0)]] RWTexture2D<float> out_prev_view_z;
[[vk::binding(8, 0)]] RWTexture2D<float4> out_prev_normal_roughness;
[[vk::binding(9, 0)]] RWTexture2D<float4> out_diff;
[[vk::binding(10, 0)]] RWTexture2D<float4> out_spec;

static const float Epsilon = 1e-6f;
static const uint MaxBlurRadius = 8u;

bool IsFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool IsValidViewZ(float view_z)
{
    float denoising_range = params.x;
    return IsFinite(view_z) && view_z > 0.0f && view_z <= denoising_range;
}

float3 UnpackNormal(float4 packed_normal_roughness)
{
    float3 normal = packed_normal_roughness.xyz * 2.0f - 1.0f;
    float normal_length = length(normal);
    return normal_length > Epsilon ? normal / normal_length : float3(0.0f, 0.0f, 1.0f);
}

float ComputeNeighborWeight(uint2 center_pixel, uint2 neighbor_pixel, float center_view_z, float3 center_normal,
                            float center_roughness, float4 center_signal, float4 neighbor_signal, float radius,
                            bool use_roughness_weight)
{
    float neighbor_view_z = in_view_z.Load(int3(neighbor_pixel, 0));
    if (!IsValidViewZ(neighbor_view_z))
    {
        return 0.0f;
    }

    float4 neighbor_normal_roughness = in_normal_roughness.Load(int3(neighbor_pixel, 0));
    float3 neighbor_normal = UnpackNormal(neighbor_normal_roughness);
    float neighbor_roughness = saturate(neighbor_normal_roughness.w);

    float2 offset = float2(neighbor_pixel) - float2(center_pixel);
    float distance_squared = dot(offset, offset);
    float sigma = max(radius * 0.75f, 1.0f);
    float distance_weight = exp(-distance_squared / (2.0f * sigma * sigma));

    float relative_depth_delta = abs(neighbor_view_z - center_view_z) / max(center_view_z, Epsilon);
    float depth_weight = exp(-relative_depth_delta * 24.0f);

    float normal_weight = pow(saturate(dot(center_normal, neighbor_normal)), 16.0f);

    float hit_distance_delta = abs(saturate(center_signal.w) - saturate(neighbor_signal.w));
    float hit_distance_weight = exp(-hit_distance_delta * 14.0f);

    float roughness_weight = 1.0f;
    if (use_roughness_weight)
    {
        float roughness_delta = abs(neighbor_roughness - center_roughness);
        roughness_weight = exp(-roughness_delta * 8.0f);
    }

    return distance_weight * depth_weight * normal_weight * hit_distance_weight * roughness_weight;
}

float ComputeEffectiveRadius(float4 center_signal, float roughness, float history_factor, bool is_specular)
{
    float min_blur_radius = max(params.y, 0.0f);
    float max_blur_radius = max(params.z, min_blur_radius);

    float hit_distance_factor = sqrt(saturate(center_signal.w));
    float convergence_factor = 1.0f - saturate(history_factor);
    float roughness_factor = is_specular ? saturate(0.2f + 0.8f * roughness) : 1.0f;
    float blend = saturate(hit_distance_factor * roughness_factor * convergence_factor);

    return clamp(lerp(min_blur_radius, max_blur_radius, blend), min_blur_radius, max_blur_radius);
}

float4 FilterSignal(Texture2D<float4> source_signal, uint2 pixel, float center_view_z, float3 center_normal,
                    float center_roughness, float4 center_signal, float radius, bool use_roughness_weight)
{
    uint radius_i = min(uint(round(radius)), MaxBlurRadius);
    if (radius_i == 0u)
    {
        return center_signal;
    }

    float4 weighted_sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float sum_weights = 0.0f;
    for (int offset_y = -int(MaxBlurRadius); offset_y <= int(MaxBlurRadius); offset_y++)
    {
        for (int offset_x = -int(MaxBlurRadius); offset_x <= int(MaxBlurRadius); offset_x++)
        {
            if (abs(offset_x) > int(radius_i) || abs(offset_y) > int(radius_i))
            {
                continue;
            }

            int2 neighbor = int2(pixel) + int2(offset_x, offset_y);
            if (neighbor.x < 0 || neighbor.y < 0 || neighbor.x >= int(resolution.x) || neighbor.y >= int(resolution.y))
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

            float4 neighbor_signal = source_signal.Load(int3(neighbor, 0));
            float weight = ComputeNeighborWeight(pixel, uint2(neighbor), center_view_z, center_normal, center_roughness,
                                                 center_signal, neighbor_signal, radius, use_roughness_weight);
            if (weight <= 0.0f)
            {
                continue;
            }

            weighted_sum += neighbor_signal * weight;
            sum_weights += weight;
        }
    }

    if (sum_weights <= Epsilon)
    {
        return center_signal;
    }

    float4 filtered = weighted_sum / sum_weights;
    filtered.w = saturate(filtered.w);
    return filtered;
}

[shader("compute")][numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float center_view_z = in_view_z.Load(int3(pixel, 0));
    out_prev_view_z[pixel] = center_view_z;
    out_prev_normal_roughness[pixel] = in_normal_roughness.Load(int3(pixel, 0));

    float4 diff = in_diff.Load(int3(pixel, 0));
    float4 spec = in_spec.Load(int3(pixel, 0));

    uint is_sky_tile = in_tiles.Load(int3(pixel >> 4, 0));
    if (is_sky_tile != 0u || !IsValidViewZ(center_view_z))
    {
        out_diff[pixel] = diff;
        out_spec[pixel] = spec;
        return;
    }

    float4 center_normal_roughness = in_normal_roughness.Load(int3(pixel, 0));
    float3 center_normal = UnpackNormal(center_normal_roughness);
    float center_roughness = saturate(center_normal_roughness.w);
    float history_factor = saturate(in_data1.Load(int3(pixel, 0)).x);

    float diff_radius = ComputeEffectiveRadius(diff, center_roughness, history_factor, false);
    float spec_radius = ComputeEffectiveRadius(spec, center_roughness, history_factor, true);

    out_diff[pixel] =
        FilterSignal(in_diff, pixel, center_view_z, center_normal, center_roughness, diff, diff_radius, false);
    out_spec[pixel] =
        FilterSignal(in_spec, pixel, center_view_z, center_normal, center_roughness, spec, spec_radius, true);
}
