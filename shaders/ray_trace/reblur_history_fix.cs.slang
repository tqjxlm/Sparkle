#include "reblur_common.h.slang"
#include "reblur_config.h.slang"
#include "reblur_data.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float history_fix_frame_num;
    float history_fix_stride;
    float plane_dist_sensitivity;
    float lobe_angle_fraction;
    float denoising_range;
};

[[vk::binding(1, 0)]] Texture2D<float4> inDiffuse;
[[vk::binding(2, 0)]] Texture2D<float4> inSpecular;
[[vk::binding(3, 0)]] Texture2D<float4> inNormalRoughness;
[[vk::binding(4, 0)]] Texture2D<float> inViewZ;
[[vk::binding(5, 0)]] Texture2D<float2> inInternalData;
[[vk::binding(6, 0)]] RWTexture2D<float4> outDiffuse;
[[vk::binding(7, 0)]] RWTexture2D<float4> outSpecular;

float GetBilateralWeight(float3 center_normal, float center_viewZ, float3 sample_normal, float sample_viewZ,
                         float sensitivity, float lobe_fraction)
{
    // Normal weight
    float cos_angle = saturate(dot(center_normal, sample_normal));
    float angle = acos(cos_angle);
    float normal_threshold = lobe_fraction * 3.14159265;
    float w_normal = smoothstep(normal_threshold, 0.0, angle);

    // Depth weight
    float w_depth = exp(-abs(center_viewZ - sample_viewZ) / max(abs(center_viewZ) * sensitivity, 1e-6));

    return w_normal * w_depth;
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

    // Read accumulation speed
    float2 packed = inInternalData[pixel];
    float accum_speed, accum_speed_fast;
    UnpackInternalData(packed, accum_speed, accum_speed_fast);

    // Only apply history fix to recently disoccluded pixels
    if (accum_speed >= history_fix_frame_num)
    {
        outDiffuse[pixel] = inDiffuse[pixel];
        outSpecular[pixel] = inSpecular[pixel];
        return;
    }

    // Wide-stride bilateral reconstruction
    float stride = history_fix_stride * (1.0 - accum_speed / history_fix_frame_num);

    float4 center_diff = inDiffuse[pixel];
    float4 center_spec = inSpecular[pixel];
    float4 nr = inNormalRoughness[pixel];
    float3 center_normal = DecodeNormalOct(nr.xy);

    float3 diff_sum = center_diff.rgb;
    float diff_hit_sum = center_diff.a;
    float diff_weight_sum = 1.0;

    float3 spec_sum = center_spec.rgb;
    float spec_hit_sum = center_spec.a;
    float spec_weight_sum = 1.0;

    // 5x5 bilateral kernel with stride
    int radius = REBLUR_HISTORY_FIX_FILTER_RADIUS;
    for (int dy = -radius; dy <= radius; dy++)
    {
        for (int dx = -radius; dx <= radius; dx++)
        {
            if (dx == 0 && dy == 0)
                continue;

            int2 sample_pixel = int2(pixel) + int2(int(round(float(dx) * stride)), int(round(float(dy) * stride)));
            sample_pixel = clamp(sample_pixel, int2(0, 0), int2(resolution) - int2(1, 1));

            float sample_viewZ = inViewZ[sample_pixel];
            if (sample_viewZ > denoising_range)
                continue;

            float4 sample_nr = inNormalRoughness[sample_pixel];
            float3 sample_normal = DecodeNormalOct(sample_nr.xy);

            float w = GetBilateralWeight(center_normal, center_viewZ, sample_normal, sample_viewZ,
                                         plane_dist_sensitivity, lobe_angle_fraction);

            // Gaussian falloff based on grid distance
            float dist_sq = float(dx * dx + dy * dy) / float((2 * radius + 1) * (2 * radius + 1));
            w *= exp(-dist_sq * 2.0);

            float4 sample_diff = inDiffuse[sample_pixel];
            float4 sample_spec = inSpecular[sample_pixel];

            diff_sum += sample_diff.rgb * w;
            diff_hit_sum += sample_diff.a * w;
            diff_weight_sum += w;

            spec_sum += sample_spec.rgb * w;
            spec_hit_sum += sample_spec.a * w;
            spec_weight_sum += w;
        }
    }

    float inv_diff_w = 1.0 / max(diff_weight_sum, 1e-6);
    float inv_spec_w = 1.0 / max(spec_weight_sum, 1e-6);

    outDiffuse[pixel] = float4(diff_sum * inv_diff_w, diff_hit_sum * inv_diff_w);
    outSpecular[pixel] = float4(spec_sum * inv_spec_w, spec_hit_sum * inv_spec_w);
}
