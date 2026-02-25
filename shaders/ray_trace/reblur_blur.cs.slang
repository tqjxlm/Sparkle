#include "reblur_common.h.slang"
#include "reblur_config.h.slang"
#include "reblur_data.h.slang"
#include "poisson_samples.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float max_blur_radius;
    float min_blur_radius;
    float lobe_angle_fraction;
    float plane_dist_sensitivity;
    float min_hit_dist_weight;
    float denoising_range;
    float4 rotator; // cos, sin, -sin, cos
    uint frame_index;
    uint blur_pass_index; // 0=pre, 1=blur, 2=post
};

[[vk::binding(1, 0)]] Texture2D<float4> inDiffuse;
[[vk::binding(2, 0)]] Texture2D<float4> inSpecular;
[[vk::binding(3, 0)]] Texture2D<float4> inNormalRoughness;
[[vk::binding(4, 0)]] Texture2D<float> inViewZ;
[[vk::binding(5, 0)]] SamplerState pointSampler;
[[vk::binding(6, 0)]] RWTexture2D<float4> outDiffuse;
[[vk::binding(7, 0)]] RWTexture2D<float4> outSpecular;

float2 RotateVector(float4 rotator_val, float2 v)
{
    return float2(
        v.x * rotator_val.x - v.y * rotator_val.y,
        v.x * rotator_val.y + v.y * rotator_val.x
    );
}

float GetGeometryWeight(float3 center_normal, float center_viewZ, float3 sample_pos_vs, float sensitivity)
{
    float plane_dist = abs(dot(center_normal, sample_pos_vs));
    float relative_plane_dist = plane_dist / max(abs(center_viewZ), 1e-6);
    return exp(-relative_plane_dist * relative_plane_dist / max(sensitivity * sensitivity, 1e-6));
}

float GetNormalWeight(float3 center_normal, float3 sample_normal, float fraction)
{
    float cos_angle = saturate(dot(center_normal, sample_normal));
    float angle = acos(cos_angle);
    float threshold = fraction * 3.14159265;
    return smoothstep(threshold, 0.0, angle);
}

float GetHitDistWeight(float center_hit_dist, float sample_hit_dist, float min_weight)
{
    float diff = abs(center_hit_dist - sample_hit_dist);
    float max_val = max(center_hit_dist, sample_hit_dist);
    float relative_diff = diff / max(max_val, 1e-6);
    return lerp(min_weight, 1.0, exp(-relative_diff * relative_diff * 4.0));
}

float GetGaussianWeight(float distance_sq)
{
    return exp(-0.5 * distance_sq);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
        return;

    float center_viewZ = inViewZ[pixel];

    // Skip sky pixels
    if (center_viewZ > denoising_range)
    {
        outDiffuse[pixel] = inDiffuse[pixel];
        outSpecular[pixel] = inSpecular[pixel];
        return;
    }

    float4 center_diff = inDiffuse[pixel];
    float4 center_spec = inSpecular[pixel];
    float4 nr = inNormalRoughness[pixel];
    float3 center_normal = DecodeNormalOct(nr.xy);
    float center_roughness = nr.z;

    // Hit distance is in alpha channel (normalized)
    float center_diff_hit = center_diff.a;
    float center_spec_hit = center_spec.a;

    // Compute blur radius
    // Without temporal accumulation, nonLinearAccumSpeed = 1.0
    float non_linear_accum_speed = 1.0;
    float diff_hit_factor = saturate(center_diff_hit);
    float spec_hit_factor = saturate(center_spec_hit);

    float fraction_scale;
    float radius_scale;
    if (blur_pass_index == 0)
    {
        fraction_scale = REBLUR_PRE_BLUR_FRACTION_SCALE;
        radius_scale = 1.0;
    }
    else if (blur_pass_index == 2)
    {
        fraction_scale = REBLUR_POST_BLUR_FRACTION_SCALE;
        radius_scale = REBLUR_POST_BLUR_RADIUS_SCALE;
    }
    else
    {
        fraction_scale = REBLUR_BLUR_FRACTION_SCALE;
        radius_scale = 1.0;
    }

    float diff_radius = max_blur_radius * sqrt(diff_hit_factor * non_linear_accum_speed) * radius_scale;
    float spec_radius = max_blur_radius * sqrt(spec_hit_factor * non_linear_accum_speed) * radius_scale;

    diff_radius = clamp(diff_radius, min_blur_radius, max_blur_radius * radius_scale);
    spec_radius = clamp(spec_radius, min_blur_radius, max_blur_radius * radius_scale);

    // Accumulate weighted samples
    float3 diff_sum = center_diff.rgb;
    float diff_hit_sum = center_diff_hit;
    float diff_weight_sum = 1.0;

    float3 spec_sum = center_spec.rgb;
    float spec_hit_sum = center_spec_hit;
    float spec_weight_sum = 1.0;

    float adjusted_lobe_fraction = lobe_angle_fraction * fraction_scale;

    for (int i = 0; i < REBLUR_POISSON_SAMPLE_NUM; i++)
    {
        float2 offset = RotateVector(rotator, g_Special8[i]);

        // Diffuse sample
        {
            float2 sample_uv_offset = offset * diff_radius;
            int2 sample_pixel = int2(pixel) + int2(round(sample_uv_offset));
            sample_pixel = clamp(sample_pixel, int2(0, 0), int2(resolution) - int2(1, 1));

            float sample_viewZ = inViewZ[sample_pixel];
            if (sample_viewZ > denoising_range)
                continue;

            float4 sample_nr = inNormalRoughness[sample_pixel];
            float3 sample_normal = DecodeNormalOct(sample_nr.xy);

            float4 sample_diff = inDiffuse[sample_pixel];

            // Compute bilateral weights
            float w_normal = GetNormalWeight(center_normal, sample_normal, adjusted_lobe_fraction);
            float w_geom = exp(-abs(center_viewZ - sample_viewZ) / max(abs(center_viewZ) * plane_dist_sensitivity, 1e-6));
            float w_hit = GetHitDistWeight(center_diff_hit, sample_diff.a, min_hit_dist_weight);
            float w_gauss = GetGaussianWeight(dot(offset, offset));

            float w = w_normal * w_geom * w_hit * w_gauss;

            diff_sum += sample_diff.rgb * w;
            diff_hit_sum += sample_diff.a * w;
            diff_weight_sum += w;
        }

        // Specular sample (tighter radius for low roughness)
        {
            float spec_radius_adj = spec_radius * lerp(0.3, 1.0, center_roughness);
            float2 sample_uv_offset = offset * spec_radius_adj;
            int2 sample_pixel = int2(pixel) + int2(round(sample_uv_offset));
            sample_pixel = clamp(sample_pixel, int2(0, 0), int2(resolution) - int2(1, 1));

            float sample_viewZ = inViewZ[sample_pixel];
            if (sample_viewZ > denoising_range)
                continue;

            float4 sample_nr = inNormalRoughness[sample_pixel];
            float3 sample_normal = DecodeNormalOct(sample_nr.xy);

            float4 sample_spec = inSpecular[sample_pixel];

            float w_normal = GetNormalWeight(center_normal, sample_normal, adjusted_lobe_fraction);
            float w_geom = exp(-abs(center_viewZ - sample_viewZ) / max(abs(center_viewZ) * plane_dist_sensitivity, 1e-6));
            float w_hit = GetHitDistWeight(center_spec_hit, sample_spec.a, min_hit_dist_weight);
            float w_gauss = GetGaussianWeight(dot(offset, offset));

            float w = w_normal * w_geom * w_hit * w_gauss;

            spec_sum += sample_spec.rgb * w;
            spec_hit_sum += sample_spec.a * w;
            spec_weight_sum += w;
        }
    }

    float inv_diff_weight = 1.0 / max(diff_weight_sum, 1e-6);
    float inv_spec_weight = 1.0 / max(spec_weight_sum, 1e-6);

    outDiffuse[pixel] = float4(diff_sum * inv_diff_weight, diff_hit_sum * inv_diff_weight);
    outSpecular[pixel] = float4(spec_sum * inv_spec_weight, spec_hit_sum * inv_spec_weight);
}
