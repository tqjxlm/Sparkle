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
[[vk::binding(6, 0)]] RWTexture2D<float4> out_diff;
[[vk::binding(7, 0)]] RWTexture2D<float4> out_spec;
[[vk::binding(8, 0)]] RWTexture2D<float> out_spec_hit_dist_for_tracking;

static const float Epsilon = 1e-6f;
static const uint MaxPrePassRadius = 4u;

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
                            float center_roughness, float radius, bool use_roughness_weight)
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

    float roughness_weight = 1.0f;
    if (use_roughness_weight)
    {
        float roughness_delta = abs(neighbor_roughness - center_roughness);
        roughness_weight = exp(-roughness_delta * 12.0f);
    }

    return distance_weight * depth_weight * normal_weight * roughness_weight;
}

float4 FilterSignal(Texture2D<float4> source_signal, uint2 pixel, float center_view_z, float3 center_normal,
                    float center_roughness, float radius, bool use_roughness_weight)
{
    uint radius_i = min(uint(round(radius)), MaxPrePassRadius);
    float4 center_value = source_signal.Load(int3(pixel, 0));
    if (radius_i == 0u)
    {
        return center_value;
    }

    float4 weighted_sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float sum_weights = 0.0f;
    for (int offset_y = -int(MaxPrePassRadius); offset_y <= int(MaxPrePassRadius); offset_y++)
    {
        for (int offset_x = -int(MaxPrePassRadius); offset_x <= int(MaxPrePassRadius); offset_x++)
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

            float weight = ComputeNeighborWeight(pixel, uint2(neighbor), center_view_z, center_normal, center_roughness,
                                                 radius, use_roughness_weight);
            if (weight <= 0.0f)
            {
                continue;
            }

            float4 neighbor_value = source_signal.Load(int3(neighbor, 0));
            weighted_sum += neighbor_value * weight;
            sum_weights += weight;
        }
    }

    if (sum_weights <= Epsilon)
    {
        return center_value;
    }

    float4 result = weighted_sum / sum_weights;
    result.w = saturate(result.w);
    return result;
}

float FilterSpecHitDistForTracking(uint2 pixel, float center_view_z, float3 center_normal, float center_roughness,
                                   float radius)
{
    uint radius_i = min(uint(round(radius)), MaxPrePassRadius);
    float center_value = saturate(in_spec.Load(int3(pixel, 0)).w);
    if (radius_i == 0u)
    {
        return center_value;
    }

    float weighted_sum = 0.0f;
    float sum_weights = 0.0f;
    for (int offset_y = -int(MaxPrePassRadius); offset_y <= int(MaxPrePassRadius); offset_y++)
    {
        for (int offset_x = -int(MaxPrePassRadius); offset_x <= int(MaxPrePassRadius); offset_x++)
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

            float neighbor_value = in_spec.Load(int3(neighbor, 0)).w;
            if (!IsFinite(neighbor_value))
            {
                continue;
            }

            float weight = ComputeNeighborWeight(pixel, uint2(neighbor), center_view_z, center_normal, center_roughness,
                                                 radius, true);
            if (weight <= 0.0f)
            {
                continue;
            }

            weighted_sum += saturate(neighbor_value) * weight;
            sum_weights += weight;
        }
    }

    if (sum_weights <= Epsilon)
    {
        return center_value;
    }

    return saturate(weighted_sum / sum_weights);
}

[shader("compute")][numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float4 diff = in_diff.Load(int3(pixel, 0));
    float4 spec = in_spec.Load(int3(pixel, 0));

    uint is_sky_tile = in_tiles.Load(int3(pixel >> 4, 0));
    float center_view_z = in_view_z.Load(int3(pixel, 0));
    if (is_sky_tile != 0u || !IsValidViewZ(center_view_z))
    {
        out_diff[pixel] = diff;
        out_spec[pixel] = spec;
        out_spec_hit_dist_for_tracking[pixel] = saturate(spec.w);
        return;
    }

    float4 center_normal_roughness = in_normal_roughness.Load(int3(pixel, 0));
    float3 center_normal = UnpackNormal(center_normal_roughness);
    float center_roughness = saturate(center_normal_roughness.w);

    float diffuse_radius = max(params.y, 0.0f);
    float specular_radius = max(params.z, 0.0f);
    float tracking_radius = max(params.w, 0.0f);

    out_diff[pixel] =
        FilterSignal(in_diff, pixel, center_view_z, center_normal, center_roughness, diffuse_radius, false);
    out_spec[pixel] =
        FilterSignal(in_spec, pixel, center_view_z, center_normal, center_roughness, specular_radius, true);
    out_spec_hit_dist_for_tracking[pixel] =
        FilterSpecHitDistForTracking(pixel, center_view_z, center_normal, center_roughness, tracking_radius);
}
