#include "reblur_common.h.slang"
#include "reblur_config.h.slang"
#include "reblur_data.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float max_accumulated_frame_num;
    float disocclusion_threshold;
    float denoising_range;
    uint frame_index;
    uint reset_history;
    uint enable_firefly_suppression;
};

// Current frame inputs (from PrePass)
[[vk::binding(1, 0)]] Texture2D<float4> inDiffuse;
[[vk::binding(2, 0)]] Texture2D<float4> inSpecular;
[[vk::binding(3, 0)]] Texture2D<float4> inNormalRoughness;
[[vk::binding(4, 0)]] Texture2D<float> inViewZ;

// Previous frame history
[[vk::binding(5, 0)]] Texture2D<float4> prevDiffHistory;
[[vk::binding(6, 0)]] Texture2D<float4> prevSpecHistory;
[[vk::binding(7, 0)]] Texture2D<float> prevViewZ;
[[vk::binding(8, 0)]] Texture2D<float4> prevNormalRoughness;
[[vk::binding(9, 0)]] Texture2D<float2> prevInternalData;

// Outputs
[[vk::binding(10, 0)]] RWTexture2D<float4> outDiffuse;
[[vk::binding(11, 0)]] RWTexture2D<float4> outSpecular;
[[vk::binding(12, 0)]] RWTexture2D<float2> outInternalData;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
        return;

    float center_viewZ = inViewZ[pixel];

    // Sky pixels: pass through with zero accumulation
    if (center_viewZ > denoising_range)
    {
        outDiffuse[pixel] = inDiffuse[pixel];
        outSpecular[pixel] = inSpecular[pixel];
        outInternalData[pixel] = float2(0.0, 0.0);
        return;
    }

    float4 current_diff = inDiffuse[pixel];
    float4 current_spec = inSpecular[pixel];
    float4 nr = inNormalRoughness[pixel];
    float3 center_normal = DecodeNormalOct(nr.xy);

    // First frame or reset: output current data with accumSpeed = 0
    if (reset_history != 0)
    {
        outDiffuse[pixel] = current_diff;
        outSpecular[pixel] = current_spec;
        outInternalData[pixel] = PackInternalData(0.0, 0.0);
        return;
    }

    // For static camera: previous pixel = current pixel (no motion vector reprojection)
    // TODO: Add bilinear history sampling with occlusion-aware weights (Issue #5)
    uint2 prev_pixel = pixel;

    // Read previous frame data
    float prev_z = prevViewZ[prev_pixel];
    float4 prev_nr = prevNormalRoughness[prev_pixel];
    float3 prev_normal = DecodeNormalOct(prev_nr.xy);
    float2 prev_packed = prevInternalData[prev_pixel];

    float prev_diff_accum_speed, prev_spec_accum_speed;
    UnpackInternalData(prev_packed, prev_diff_accum_speed, prev_spec_accum_speed);

    // Disocclusion test
    bool disoccluded = false;

    // Depth-based disocclusion: relative depth difference
    float z_diff = abs(center_viewZ - prev_z);
    float z_threshold = disocclusion_threshold * max(abs(center_viewZ), 1.0);
    disoccluded = disoccluded || (z_diff > z_threshold);

    // Normal-based disocclusion: cosine similarity
    float normal_agreement = dot(center_normal, prev_normal);
    disoccluded = disoccluded || (normal_agreement < 0.5);

    // Previous pixel was sky
    disoccluded = disoccluded || (prev_z > denoising_range);

    // Compute new accumulation speeds - separate per channel
    float diff_accum_speed, spec_accum_speed;
    if (disoccluded)
    {
        diff_accum_speed = 0.0;
        spec_accum_speed = 0.0;
    }
    else
    {
        diff_accum_speed = min(prev_diff_accum_speed + 1.0, max_accumulated_frame_num);
        spec_accum_speed = min(prev_spec_accum_speed + 1.0, max_accumulated_frame_num);
    }

    // Separate blend weights per channel
    float diff_weight = 1.0 / (1.0 + diff_accum_speed);
    float spec_weight = 1.0 / (1.0 + spec_accum_speed);

    // Read history
    float4 hist_diff = prevDiffHistory[prev_pixel];
    float4 hist_spec = prevSpecHistory[prev_pixel];

    // Linear accumulation: lerp(history, current, weight)
    float4 acc_diff = lerp(hist_diff, current_diff, diff_weight);
    float4 acc_spec = lerp(hist_spec, current_spec, spec_weight);

    // Write outputs
    outDiffuse[pixel] = acc_diff;
    outSpecular[pixel] = acc_spec;
    outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
}
