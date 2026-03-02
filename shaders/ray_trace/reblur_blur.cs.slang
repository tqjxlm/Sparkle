#include "poisson_samples.h.slang"
#include "reblur_common.h.slang"
#include "reblur_config.h.slang"
#include "reblur_data.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    float4 rotator; // cos, sin, -sin, cos
    float4 hit_dist_params;
    float4 view_row0;
    float4 view_row1;
    float4 view_row2;
    uint2 resolution;
    float max_blur_radius;
    float min_blur_radius;
    float lobe_angle_fraction;
    float plane_dist_sensitivity;
    float min_hit_dist_weight;
    float denoising_range;
    float diffuse_prepass_blur_radius;
    float specular_prepass_blur_radius;
    float min_rect_dim_mul_unproject;
    float roughness_fraction;
    float unproject_x;
    float unproject_y;
    uint frame_index;
    uint blur_pass_index; // 0=pre, 1=blur, 2=post
    uint has_temporal_data;
};

[[vk::binding(1, 0)]] Texture2D<float4> inDiffuse;
[[vk::binding(2, 0)]] Texture2D<float4> inSpecular;
[[vk::binding(3, 0)]] Texture2D<float4> inNormalRoughness;
[[vk::binding(4, 0)]] Texture2D<float> inViewZ;
[[vk::binding(5, 0)]] RWTexture2D<float4> outDiffuse;
[[vk::binding(6, 0)]] RWTexture2D<float4> outSpecular;
[[vk::binding(7, 0)]] Texture2D<float2> inInternalData;

float2 RotateVector(float4 rotator_val, float2 v)
{
    return float2(v.x * rotator_val.x - v.y * rotator_val.y, v.x * rotator_val.y + v.y * rotator_val.x);
}

// Transform world normal to view space using view matrix rows
float3 WorldToViewNormal(float3 world_normal)
{
    return float3(dot(view_row0.xyz, world_normal),
                  dot(view_row1.xyz, world_normal),
                  dot(view_row2.xyz, world_normal));
}

// Reconstruct view-space position from pixel coordinates and viewZ
float3 GetViewSpacePosition(uint2 px, float view_z)
{
    float2 uv = (float2(px) + 0.5) / float2(resolution);
    float2 ndc = uv * 2.0 - 1.0;
    return float3(ndc.x * unproject_x * view_z, ndc.y * unproject_y * view_z, view_z);
}

// Plane-distance geometry weight (NRD approach)
// Measures distance from sample's view-space position to center's tangent plane
float GetPlaneDistanceWeight(float3 center_Nv, float3 center_Xv, float3 sample_Xv,
                             float sensitivity, float frustum_size)
{
    float plane_dist = abs(dot(center_Nv, sample_Xv - center_Xv));
    float threshold = sensitivity * frustum_size;
    return smoothstep(1.0, 0.0, plane_dist / max(threshold, 1e-6));
}

// Normal weight using specular lobe half-angle for specular, fixed fraction for diffuse
float GetNormalWeight(float3 center_normal, float3 sample_normal, float fraction)
{
    float cos_angle = saturate(dot(center_normal, sample_normal));
    float angle = acos(cos_angle);
    float threshold = fraction * 3.14159265;
    return smoothstep(threshold, 0.0, angle);
}

// Specular normal weight derived from the actual lobe shape
float GetSpecNormalWeight(float3 center_normal, float3 sample_normal, float roughness,
                          float fraction, float non_linear_accum_speed)
{
    float lobe_half_angle = atan(GetSpecularLobeTanHalfAngle(roughness, 0.75));
    float threshold = max(lobe_half_angle * fraction, 0.01);
    // Widen threshold when history is short (non_linear_accum_speed is large)
    threshold = lerp(threshold, 3.14159265 * fraction, non_linear_accum_speed);
    float cos_angle = saturate(dot(center_normal, sample_normal));
    float angle = acos(cos_angle);
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

[shader("compute")][numthreads(16, 16, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
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
    uint center_instance_id = uint(round(nr.a * 1023.0));

    // Hit distance is in alpha channel (normalized)
    float center_diff_hit_norm = center_diff.a;
    float center_spec_hit_norm = center_spec.a;

    // Compute frustum size at this depth
    float frustum_size = GetFrustumSize(min_rect_dim_mul_unproject, center_viewZ);

    // Denormalize hit distance to world space, then compute frustum-relative factor
    HitDistanceParams hdp;
    hdp.A = hit_dist_params.x;
    hdp.B = hit_dist_params.y;
    hdp.C = hit_dist_params.z;
    hdp.D = hit_dist_params.w;

    float diff_hit_world = DenormalizeHitDistance(center_diff_hit_norm, center_viewZ, 1.0, hdp);
    float spec_hit_world = DenormalizeHitDistance(center_spec_hit_norm, center_viewZ, center_roughness, hdp);

    float diff_hit_factor = GetHitDistFactor(diff_hit_world, frustum_size);
    float spec_hit_factor = GetHitDistFactor(spec_hit_world, frustum_size);

    // Read separate accumulation speeds per channel
    float diff_accum_speed = 0;
    float spec_accum_speed = 0;
    if (has_temporal_data != 0)
    {
        float2 packed = inInternalData[pixel];
        UnpackInternalData(packed, diff_accum_speed, spec_accum_speed);
    }
    float diff_non_linear = GetNonLinearAccumSpeed(diff_accum_speed);
    float spec_non_linear = GetNonLinearAccumSpeed(spec_accum_speed);

    // GetSpecMagicCurve modulates specular behavior based on roughness
    float smc = GetSpecMagicCurve(center_roughness);

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

    // PrePass uses separate diffuse/specular radii; Blur and PostBlur use shared max_blur_radius
    float diff_max_radius = blur_pass_index == 0 ? diffuse_prepass_blur_radius : max_blur_radius;
    float spec_max_radius = blur_pass_index == 0 ? specular_prepass_blur_radius : max_blur_radius;

    // Radius formula: sqrt(hitFactor * nonLinearAccumSpeed) * maxRadius * radiusScale
    float diff_radius = diff_max_radius * sqrt(diff_hit_factor * diff_non_linear) * radius_scale;
    float spec_radius = spec_max_radius * sqrt(spec_hit_factor * spec_non_linear) * radius_scale;

    // Apply GetSpecMagicCurve to specular radius (mirror-like → near zero, rough → full)
    spec_radius *= smc;

    // Clamp radii
    diff_radius = clamp(diff_radius, min_blur_radius, diff_max_radius * radius_scale);
    spec_radius = clamp(spec_radius, min_blur_radius * smc, spec_max_radius * radius_scale);

    // Lobe trimming for PreBlur: cap specular radius to specular lobe extent
    if (blur_pass_index == 0 && center_roughness < 1.0)
    {
        float lobe_tan_half = GetSpecularLobeTanHalfAngle(center_roughness, 0.3);
        float lobe_radius = spec_hit_world * lobe_tan_half;
        // Convert world-space lobe radius to pixel radius
        float lobe_pixel_radius = lobe_radius / max(frustum_size / min(float(resolution.x), float(resolution.y)), 1e-6);
        spec_radius = min(spec_radius, max(lobe_pixel_radius, min_blur_radius * smc));
    }

    // Compute view-space position and normal for plane distance weight
    float3 center_Xv = GetViewSpacePosition(pixel, center_viewZ);
    float3 center_Nv = WorldToViewNormal(center_normal);

    // Blend Poisson rotation from per-frame (golden angle) to per-pixel (deterministic)
    // as accumulation progresses. At high accumSpeed, per-pixel rotation eliminates
    // the frame-to-frame instability caused by rotating the spatial kernel.
    float min_accum = min(diff_accum_speed, spec_accum_speed);
    float stability_weight = saturate(min_accum / 16.0); // 0 at accum=0, 1 at accum>=16
    float pixel_angle = frac(sin(dot(float2(pixel), float2(12.9898, 78.233))) * 43758.5453) * 6.28318;
    float4 pixel_rotator = float4(cos(pixel_angle), sin(pixel_angle), -sin(pixel_angle), cos(pixel_angle));
    // Blend rotators via angle interpolation
    float blended_cos = lerp(rotator.x, pixel_rotator.x, stability_weight);
    float blended_sin = lerp(rotator.y, pixel_rotator.y, stability_weight);
    float inv_len = 1.0 / max(sqrt(blended_cos * blended_cos + blended_sin * blended_sin), 1e-6);
    float4 effective_rotator = float4(blended_cos * inv_len, blended_sin * inv_len,
                                      -blended_sin * inv_len, blended_cos * inv_len);

    // Accumulate weighted samples
    float3 diff_sum = center_diff.rgb;
    float diff_hit_sum = center_diff_hit_norm;
    float diff_weight_sum = 1.0;

    float3 spec_sum = center_spec.rgb;
    float spec_hit_sum = center_spec_hit_norm;
    float spec_weight_sum = 1.0;

    float adjusted_lobe_fraction = lobe_angle_fraction * fraction_scale;

    for (int i = 0; i < REBLUR_POISSON_SAMPLE_NUM; i++)
    {
        float2 offset = RotateVector(effective_rotator, g_Special8[i]);

        // Diffuse sample
        {
            float2 sample_uv_offset = offset * diff_radius;
            int2 sample_pixel = int2(pixel) + int2(round(sample_uv_offset));
            sample_pixel = clamp(sample_pixel, int2(0, 0), int2(resolution) - int2(1, 1));

            float sample_viewZ = inViewZ[sample_pixel];
            if (sample_viewZ <= denoising_range)
            {
                float4 sample_nr = inNormalRoughness[sample_pixel];
                float3 sample_normal = DecodeNormalOct(sample_nr.xy);

                // Instance ID edge-stopping: prevent cross-object blur
                uint sample_instance_id = uint(round(sample_nr.a * 1023.0));
                if (sample_instance_id == center_instance_id)
                {
                    float4 sample_diff = inDiffuse[sample_pixel];

                    // Plane-distance geometry weight
                    float3 sample_Xv = GetViewSpacePosition(uint2(sample_pixel), sample_viewZ);
                    float w_geom = GetPlaneDistanceWeight(center_Nv, center_Xv, sample_Xv,
                                                           plane_dist_sensitivity, frustum_size);

                    // Normal weight
                    float w_normal = GetNormalWeight(center_normal, sample_normal, adjusted_lobe_fraction);

                    // Hit distance weight
                    float w_hit = GetHitDistWeight(center_diff_hit_norm, sample_diff.a, min_hit_dist_weight);

                    // Gaussian spatial weight
                    float w_gauss = GetGaussianWeight(dot(offset, offset));

                    float w = w_normal * w_geom * w_hit * w_gauss;

                    diff_sum += sample_diff.rgb * w;
                    diff_hit_sum += sample_diff.a * w;
                    diff_weight_sum += w;
                }
            }
        }

        // Specular sample
        {
            float2 sample_uv_offset = offset * spec_radius;
            int2 sample_pixel = int2(pixel) + int2(round(sample_uv_offset));
            sample_pixel = clamp(sample_pixel, int2(0, 0), int2(resolution) - int2(1, 1));

            float sample_viewZ = inViewZ[sample_pixel];
            if (sample_viewZ <= denoising_range)
            {
                float4 sample_nr = inNormalRoughness[sample_pixel];
                float3 sample_normal = DecodeNormalOct(sample_nr.xy);
                float sample_roughness = sample_nr.z;

                // Instance ID edge-stopping: prevent cross-object blur
                uint sample_instance_id_spec = uint(round(sample_nr.a * 1023.0));
                if (sample_instance_id_spec == center_instance_id)
                {
                    float4 sample_spec = inSpecular[sample_pixel];

                    // Plane-distance geometry weight
                    float3 sample_Xv = GetViewSpacePosition(uint2(sample_pixel), sample_viewZ);
                    float w_geom = GetPlaneDistanceWeight(center_Nv, center_Xv, sample_Xv,
                                                           plane_dist_sensitivity, frustum_size);

                    // Specular normal weight using lobe half-angle
                    float w_normal = GetSpecNormalWeight(center_normal, sample_normal, center_roughness,
                                                          adjusted_lobe_fraction, spec_non_linear);

                    // Hit distance weight modulated by specular magic curve
                    float spec_min_hit_weight = min_hit_dist_weight * fraction_scale * smc;
                    float w_hit = GetHitDistWeight(center_spec_hit_norm, sample_spec.a, spec_min_hit_weight);

                    // Roughness weight for specular: prevents blur leaking across roughness boundaries
                    float w_roughness = GetRoughnessWeight(center_roughness, sample_roughness, roughness_fraction);

                    // Gaussian spatial weight
                    float w_gauss = GetGaussianWeight(dot(offset, offset));

                    float w = w_normal * w_geom * w_hit * w_roughness * w_gauss;

                    spec_sum += sample_spec.rgb * w;
                    spec_hit_sum += sample_spec.a * w;
                    spec_weight_sum += w;
                }
            }
        }
    }

    float inv_diff_weight = 1.0 / max(diff_weight_sum, 1e-6);
    float inv_spec_weight = 1.0 / max(spec_weight_sum, 1e-6);

    outDiffuse[pixel] = float4(diff_sum * inv_diff_weight, diff_hit_sum * inv_diff_weight);
    outSpecular[pixel] = float4(spec_sum * inv_spec_weight, spec_hit_sum * inv_spec_weight);
}
