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

static const float Epsilon = 1e-6f;

bool IsFinite(float value)
{
    return !isnan(value) && !isinf(value);
}

bool IsValidViewZ(float view_z)
{
    float denoising_range = params.x;
    return IsFinite(view_z) && view_z > 0.0f && view_z <= denoising_range;
}

bool IsValidNormHitDist(float norm_hit_distance)
{
    return IsFinite(norm_hit_distance) && norm_hit_distance > 0.0f && norm_hit_distance <= 1.0f;
}

float3 UnpackNormal(float4 packed_normal_roughness)
{
    float3 normal = packed_normal_roughness.xyz * 2.0f - 1.0f;
    float normal_length = length(normal);
    return normal_length > Epsilon ? normal / normal_length : float3(0.0f, 0.0f, 1.0f);
}

float ComputeNeighborWeight(uint2 center_pixel, uint2 neighbor_pixel, float center_view_z, float3 center_normal)
{
    float neighbor_view_z = in_view_z.Load(int3(neighbor_pixel, 0));
    if (!IsValidViewZ(neighbor_view_z))
    {
        return 0.0f;
    }

    float3 neighbor_normal = UnpackNormal(in_normal_roughness.Load(int3(neighbor_pixel, 0)));

    float2 offset = float2(neighbor_pixel) - float2(center_pixel);
    float distance_squared = dot(offset, offset);
    float distance_sigma = max(1.0f, params.z);
    float distance_weight = exp(-distance_squared / (2.0f * distance_sigma * distance_sigma));

    float relative_depth_delta = abs(neighbor_view_z - center_view_z) / max(center_view_z, Epsilon);
    float depth_weight = exp(-relative_depth_delta * 16.0f);

    float normal_weight = pow(saturate(dot(center_normal, neighbor_normal)), 8.0f);

    return distance_weight * depth_weight * normal_weight;
}

float ReconstructNormHitDist(Texture2D<float4> source_signal, uint2 pixel, float center_view_z, float3 center_normal,
                             uint radius)
{
    float center_value = source_signal.Load(int3(pixel, 0)).w;
    if (IsValidNormHitDist(center_value))
    {
        return center_value;
    }

    float weighted_sum = 0.0f;
    float sum_weights = 0.0f;
    for (int offset_y = -int(radius); offset_y <= int(radius); offset_y++)
    {
        for (int offset_x = -int(radius); offset_x <= int(radius); offset_x++)
        {
            if (offset_x == 0 && offset_y == 0)
            {
                continue;
            }

            int2 neighbor = int2(pixel) + int2(offset_x, offset_y);
            if (neighbor.x < 0 || neighbor.y < 0 || neighbor.x >= int(resolution.x) || neighbor.y >= int(resolution.y))
            {
                continue;
            }

            float neighbor_value = source_signal.Load(int3(neighbor, 0)).w;
            if (!IsValidNormHitDist(neighbor_value))
            {
                continue;
            }

            float weight = ComputeNeighborWeight(pixel, uint2(neighbor), center_view_z, center_normal);
            if (weight <= 0.0f)
            {
                continue;
            }

            weighted_sum += neighbor_value * weight;
            sum_weights += weight;
        }
    }

    if (sum_weights <= Epsilon)
    {
        return 0.0f;
    }

    return saturate(weighted_sum / sum_weights);
}

[shader("compute")][numthreads(16, 16, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float4 diff = in_diff.Load(int3(pixel, 0));
    float4 spec = in_spec.Load(int3(pixel, 0));

    uint mode = uint(round(params.y));
    if (mode == 0u)
    {
        out_diff[pixel] = diff;
        out_spec[pixel] = spec;
        return;
    }

    uint is_sky_tile = in_tiles.Load(int3(pixel >> 4, 0));
    float center_view_z = in_view_z.Load(int3(pixel, 0));
    if (is_sky_tile != 0u || !IsValidViewZ(center_view_z))
    {
        out_diff[pixel] = diff;
        out_spec[pixel] = spec;
        return;
    }

    uint radius = clamp(uint(round(params.z)), 1u, 2u);
    float3 center_normal = UnpackNormal(in_normal_roughness.Load(int3(pixel, 0)));

    float reconstructed_diff_hit_distance =
        ReconstructNormHitDist(in_diff, pixel, center_view_z, center_normal, radius);
    float reconstructed_spec_hit_distance =
        ReconstructNormHitDist(in_spec, pixel, center_view_z, center_normal, radius);

    out_diff[pixel] = float4(diff.xyz, reconstructed_diff_hit_distance);
    out_spec[pixel] = float4(spec.xyz, reconstructed_spec_hit_distance);
}
