#include "reblur_common.h.slang"
#include "reblur_config.h.slang"
#include "reblur_data.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    float4 view_row0;
    float4 view_row1;
    float4 view_row2;
    uint2 resolution;
    float history_fix_frame_num;
    float history_fix_stride;
    float plane_dist_sensitivity;
    float lobe_angle_fraction;
    float denoising_range;
    float min_rect_dim_mul_unproject;
    float roughness_fraction;
    float unproject_x;
    float unproject_y;
    uint enable_anti_firefly;
};

[[vk::binding(1, 0)]] Texture2D<float4> inDiffuse;
[[vk::binding(2, 0)]] Texture2D<float4> inSpecular;
[[vk::binding(3, 0)]] Texture2D<float4> inNormalRoughness;
[[vk::binding(4, 0)]] Texture2D<float> inViewZ;
[[vk::binding(5, 0)]] Texture2D<float2> inInternalData;
[[vk::binding(6, 0)]] RWTexture2D<float4> outDiffuse;
[[vk::binding(7, 0)]] RWTexture2D<float4> outSpecular;

// Transform world normal to view space
float3 WorldToViewNormal(float3 world_normal)
{
    return float3(dot(view_row0.xyz, world_normal),
                  dot(view_row1.xyz, world_normal),
                  dot(view_row2.xyz, world_normal));
}

// Reconstruct view-space position
float3 GetViewSpacePosition(uint2 px, float view_z)
{
    float2 uv = (float2(px) + 0.5) / float2(resolution);
    float2 ndc = uv * 2.0 - 1.0;
    return float3(ndc.x * unproject_x * view_z, ndc.y * unproject_y * view_z, view_z);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
        return;

    float center_viewZ = inViewZ[pixel];

    // Sky pixels: pass through
    if (center_viewZ > denoising_range)
    {
        outDiffuse[pixel] = inDiffuse[pixel];
        outSpecular[pixel] = inSpecular[pixel];
        return;
    }

    // Read separate accumulation speeds
    float2 packed = inInternalData[pixel];
    float diff_accum_speed, spec_accum_speed;
    UnpackInternalData(packed, diff_accum_speed, spec_accum_speed);

    float4 center_diff = inDiffuse[pixel];
    float4 center_spec = inSpecular[pixel];

    // Bilateral reconstruction: only for recently disoccluded pixels (first N frames)
    float min_accum = min(diff_accum_speed, spec_accum_speed);
    bool needs_bilateral = min_accum < history_fix_frame_num;

    float4 result_diff = center_diff;
    float4 result_spec = center_spec;

    if (needs_bilateral)
    {
        float4 nr = inNormalRoughness[pixel];
        float3 center_normal = DecodeNormalOct(nr.xy);
        float center_roughness = nr.z;
        uint center_instance_id = uint(round(nr.a * 1023.0));

        float frustum_size = GetFrustumSize(min_rect_dim_mul_unproject, center_viewZ);
        float3 center_Xv = GetViewSpacePosition(pixel, center_viewZ);
        float3 center_Nv = WorldToViewNormal(center_normal);

        float3 diff_sum = center_diff.rgb;
        float diff_hit_sum = center_diff.a;
        float diff_weight_sum = 1.0;

        float3 spec_sum = center_spec.rgb;
        float spec_hit_sum = center_spec.a;
        float spec_weight_sum = 1.0;

        float stride = history_fix_stride * (1.0 - min_accum / history_fix_frame_num);

        int radius = REBLUR_HISTORY_FIX_FILTER_RADIUS;
        for (int dy = -radius; dy <= radius; dy++)
        {
            for (int dx = -radius; dx <= radius; dx++)
            {
                if (dx == 0 && dy == 0)
                    continue;

                if (abs(dx) + abs(dy) == radius * 2)
                    continue;

                int2 sample_pixel = int2(pixel) + int2(int(round(float(dx) * stride)), int(round(float(dy) * stride)));
                sample_pixel = clamp(sample_pixel, int2(0, 0), int2(resolution) - int2(1, 1));

                float sample_viewZ = inViewZ[sample_pixel];
                if (sample_viewZ > denoising_range)
                    continue;

                float4 sample_nr = inNormalRoughness[sample_pixel];
                float3 sample_normal = DecodeNormalOct(sample_nr.xy);
                float sample_roughness = sample_nr.z;

                // Instance ID edge-stopping: prevent cross-object reconstruction.
                // Without this, disoccluded floor pixels pull object radiance from
                // nearby converged object pixels via the (1+accum) weight preference.
                uint sample_instance_id = uint(round(sample_nr.a * 1023.0));
                if (sample_instance_id != center_instance_id)
                    continue;

                float cos_angle = saturate(dot(center_normal, sample_normal));
                float angle = acos(cos_angle);
                float normal_threshold = lobe_angle_fraction * 3.14159265;
                float w_normal = smoothstep(normal_threshold, 0.0, angle);

                float3 sample_Xv = GetViewSpacePosition(uint2(sample_pixel), sample_viewZ);
                float w_geom = smoothstep(1.0, 0.0, abs(dot(center_Nv, sample_Xv - center_Xv)) /
                                                       max(plane_dist_sensitivity * frustum_size, 1e-6));

                float dist_sq = float(dx * dx + dy * dy) / float((2 * radius + 1) * (2 * radius + 1));
                float w_gauss = exp(-dist_sq * 2.0);

                float2 sample_packed = inInternalData[sample_pixel];
                float sample_diff_accum, sample_spec_accum;
                UnpackInternalData(sample_packed, sample_diff_accum, sample_spec_accum);

                float w_diff = w_normal * w_geom * w_gauss * (1.0 + sample_diff_accum);

                float4 sample_diff = inDiffuse[sample_pixel];
                diff_sum += sample_diff.rgb * w_diff;
                diff_hit_sum += sample_diff.a * w_diff;
                diff_weight_sum += w_diff;

                float w_roughness = GetRoughnessWeight(center_roughness, sample_roughness, roughness_fraction);
                float w_spec = w_normal * w_geom * w_gauss * w_roughness * (1.0 + sample_spec_accum);

                float4 sample_spec = inSpecular[sample_pixel];
                spec_sum += sample_spec.rgb * w_spec;
                spec_hit_sum += sample_spec.a * w_spec;
                spec_weight_sum += w_spec;
            }
        }

        float inv_diff_w = 1.0 / max(diff_weight_sum, 1e-6);
        float inv_spec_w = 1.0 / max(spec_weight_sum, 1e-6);

        result_diff = float4(diff_sum * inv_diff_w, diff_hit_sum * inv_diff_w);
        result_spec = float4(spec_sum * inv_spec_w, spec_hit_sum * inv_spec_w);
    }

    // Anti-firefly clamping: spatial neighborhood mean ± 2σ clamp.
    // Only apply once accumulation is stable (accum >= 16).
    // At low accum speeds (after disocclusion/camera motion), spatial statistics
    // are noisy and the right-skewed PT distribution causes asymmetric energy loss
    // — bright outliers get clamped but dark pixels don't gain energy.
    if (enable_anti_firefly != 0 && min_accum >= 16.0)
    {
        float diff_M1 = 0, diff_M2 = 0;
        float spec_M1 = 0, spec_M2 = 0;
        float af_count = 0;

        int af_radius = REBLUR_ANTI_FIREFLY_FILTER_RADIUS;
        for (int dy = -af_radius; dy <= af_radius; dy++)
        {
            for (int dx = -af_radius; dx <= af_radius; dx++)
            {
                // Skip center 3x3
                if (abs(dx) <= 1 && abs(dy) <= 1)
                    continue;

                int2 sp = int2(pixel) + int2(dx, dy);
                sp = clamp(sp, int2(0), int2(resolution) - int2(1, 1));

                float sp_viewZ = inViewZ[sp];
                if (sp_viewZ > denoising_range)
                    continue;

                float dl = GetLuminance(inDiffuse[sp].rgb);
                float sl = GetLuminance(inSpecular[sp].rgb);

                diff_M1 += dl;
                diff_M2 += dl * dl;
                spec_M1 += sl;
                spec_M2 += sl * sl;
                af_count += 1.0;
            }
        }

        if (af_count > 0)
        {
            diff_M1 /= af_count;
            diff_M2 /= af_count;
            spec_M1 /= af_count;
            spec_M2 /= af_count;

            float diff_sigma = sqrt(max(0.0, diff_M2 - diff_M1 * diff_M1)) * REBLUR_ANTI_FIREFLY_SIGMA_SCALE;
            float spec_sigma = sqrt(max(0.0, spec_M2 - spec_M1 * spec_M1)) * REBLUR_ANTI_FIREFLY_SIGMA_SCALE;

            float result_diff_luma = GetLuminance(result_diff.rgb);
            float clamped_diff_luma = clamp(result_diff_luma, diff_M1 - diff_sigma, diff_M1 + diff_sigma);
            if (result_diff_luma > 1e-6)
                result_diff.rgb = ChangeLuma(result_diff.rgb, max(clamped_diff_luma, 0.0));

            float result_spec_luma = GetLuminance(result_spec.rgb);
            float clamped_spec_luma = clamp(result_spec_luma, spec_M1 - spec_sigma, spec_M1 + spec_sigma);
            if (result_spec_luma > 1e-6)
                result_spec.rgb = ChangeLuma(result_spec.rgb, max(clamped_spec_luma, 0.0));
        }
    }

    outDiffuse[pixel] = result_diff;
    outSpecular[pixel] = result_spec;
}
