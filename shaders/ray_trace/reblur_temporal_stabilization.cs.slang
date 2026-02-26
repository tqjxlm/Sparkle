#include "reblur_common.h.slang"
#include "reblur_config.h.slang"
#include "reblur_data.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float stabilization_strength;
    float fast_history_sigma_scale;
    float antilag_sigma_scale;
    float antilag_sensitivity;
    float denoising_range;
    uint frame_index;
    uint max_stabilized_frame_num;
};

// Current denoised output (from PostBlur)
[[vk::binding(1, 0)]] Texture2D<float4> inDiffuse;
[[vk::binding(2, 0)]] Texture2D<float4> inSpecular;
[[vk::binding(3, 0)]] Texture2D<float> inViewZ;
[[vk::binding(4, 0)]] Texture2D<float2> inInternalData;

// Stabilized history (previous frame)
[[vk::binding(5, 0)]] Texture2D<float4> prevStabilizedDiff;
[[vk::binding(6, 0)]] Texture2D<float4> prevStabilizedSpec;

// Outputs
[[vk::binding(7, 0)]] RWTexture2D<float4> outDiffuse;
[[vk::binding(8, 0)]] RWTexture2D<float4> outSpecular;

// Accumulation speed writeback (antilag may reduce speeds)
[[vk::binding(9, 0)]] RWTexture2D<float2> outInternalData;

// Shared memory for neighborhood luminance computation (5x5 = BORDER=2)
#define BORDER 2
#define TILE_SIZE 16
#define BUFFER_X (TILE_SIZE + 2 * BORDER)
#define BUFFER_Y (TILE_SIZE + 2 * BORDER)

groupshared float s_DiffLuma[BUFFER_Y][BUFFER_X];
groupshared float s_SpecLuma[BUFFER_Y][BUFFER_X];

[shader("compute")]
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    // Load neighborhood into shared memory (tile + 2-pixel border for 5x5)
    int2 tile_origin = int2(Gid.xy) * TILE_SIZE - BORDER;

    for (int i = int(GTid.y * TILE_SIZE + GTid.x); i < BUFFER_X * BUFFER_Y; i += TILE_SIZE * TILE_SIZE)
    {
        int2 local = int2(i % BUFFER_X, i / BUFFER_X);
        int2 global = tile_origin + local;
        global = clamp(global, int2(0, 0), int2(resolution) - int2(1, 1));

        float4 diff = inDiffuse[global];
        float4 spec = inSpecular[global];

        s_DiffLuma[local.y][local.x] = GetLuminance(diff.rgb);
        s_SpecLuma[local.y][local.x] = GetLuminance(spec.rgb);
    }

    GroupMemoryBarrierWithGroupSync();

    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
        return;

    float center_viewZ = inViewZ[pixel];

    // Read internal data for writeback
    float2 packed = inInternalData[pixel];
    float diff_accum_speed, spec_accum_speed;
    UnpackInternalData(packed, diff_accum_speed, spec_accum_speed);

    // Sky pixels: pass through
    if (center_viewZ > denoising_range)
    {
        outDiffuse[pixel] = inDiffuse[pixel];
        outSpecular[pixel] = inSpecular[pixel];
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }

    // Stabilization disabled
    if (max_stabilized_frame_num == 0)
    {
        outDiffuse[pixel] = inDiffuse[pixel];
        outSpecular[pixel] = inSpecular[pixel];
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }

    float4 current_diff = inDiffuse[pixel];
    float4 current_spec = inSpecular[pixel];

    // Compute local mean and variance from shared memory 5x5 neighborhood
    int2 smem_center = int2(GTid.xy) + BORDER;

    float diff_luma_sum = 0.0;
    float diff_luma_sq_sum = 0.0;
    float spec_luma_sum = 0.0;
    float spec_luma_sq_sum = 0.0;
    float sample_count = 0.0;

    for (int dy = -BORDER; dy <= BORDER; dy++)
    {
        for (int dx = -BORDER; dx <= BORDER; dx++)
        {
            int2 s = smem_center + int2(dx, dy);

            float dl = s_DiffLuma[s.y][s.x];
            float sl = s_SpecLuma[s.y][s.x];

            diff_luma_sum += dl;
            diff_luma_sq_sum += dl * dl;
            spec_luma_sum += sl;
            spec_luma_sq_sum += sl * sl;
            sample_count += 1.0;
        }
    }

    float inv_count = 1.0 / sample_count;
    float diff_mean = diff_luma_sum * inv_count;
    float diff_variance = max(0.0, diff_luma_sq_sum * inv_count - diff_mean * diff_mean);
    float diff_sigma = sqrt(diff_variance);

    float spec_mean = spec_luma_sum * inv_count;
    float spec_variance = max(0.0, spec_luma_sq_sum * inv_count - spec_mean * spec_mean);
    float spec_sigma = sqrt(spec_variance);

    // Read stabilized history (for static camera: previous pixel = current pixel)
    float4 stab_diff = prevStabilizedDiff[pixel];
    float4 stab_spec = prevStabilizedSpec[pixel];

    float stab_diff_luma = GetLuminance(stab_diff.rgb);
    float stab_spec_luma = GetLuminance(stab_spec.rgb);

    // Save incoming accumSpeed before antilag modifies it.
    // Used later for blend weight and sigma_scale: truly disoccluded edge pixels
    // (incoming accum<=2) get moderate blend and tighter sigma for faster adaptation.
    float diff_accum_incoming = diff_accum_speed;
    float spec_accum_incoming = spec_accum_speed;

    // Antilag detection: when history diverges from current neighborhood,
    // reduce accumulation speed to forget stale history faster.
    // Gate on accum_speed >= 8: before that, temporal average is too noisy
    // for reliable divergence detection (Monte Carlo noise dominates).
    // Also use sigma-based denominator instead of mean-based to avoid
    // false triggers in dark regions.
    float diff_antilag = 1.0;
    float spec_antilag = 1.0;
    {
        if (diff_accum_speed >= 8.0)
        {
            float diff_deviation = abs(stab_diff_luma - diff_mean);
            float diff_threshold = diff_sigma * antilag_sigma_scale;
            diff_antilag = saturate(1.0 - (diff_deviation - diff_threshold) /
                                           max(diff_threshold * antilag_sensitivity, 1e-6));
            diff_accum_speed = lerp(0.0, diff_accum_speed, diff_antilag);
        }

        if (spec_accum_speed >= 8.0)
        {
            float spec_deviation = abs(stab_spec_luma - spec_mean);
            float spec_threshold = spec_sigma * antilag_sigma_scale;
            spec_antilag = saturate(1.0 - (spec_deviation - spec_threshold) /
                                           max(spec_threshold * antilag_sensitivity, 1e-6));
            spec_accum_speed = lerp(0.0, spec_accum_speed, spec_antilag);
        }
    }

    // Adaptive sigma scale: tighter clamping as more history accumulates.
    float diff_non_linear = GetNonLinearAccumSpeed(diff_accum_speed);
    float spec_non_linear = GetNonLinearAccumSpeed(spec_accum_speed);
    float diff_sigma_scale = fast_history_sigma_scale * (1.0 + 3.0 * diff_non_linear);
    float spec_sigma_scale = fast_history_sigma_scale * (1.0 + 3.0 * spec_non_linear);

    // Clamp stabilized history to local color box
    float diff_lo = diff_mean - diff_sigma * diff_sigma_scale;
    float diff_hi = diff_mean + diff_sigma * diff_sigma_scale;
    float spec_lo = spec_mean - spec_sigma * spec_sigma_scale;
    float spec_hi = spec_mean + spec_sigma * spec_sigma_scale;

    // For disoccluded edge pixels (incoming accum<=2), SKIP clamping entirely.
    // At edges, the 5x5 neighborhood shifts dramatically when the center pixel
    // alternates surfaces due to sub-pixel jitter. The clamping box follows the
    // current surface, pulling the stabilized output toward it each frame.
    // This overrides the blend weight (0.9995) and causes the output to oscillate
    // with the clamping box instead of staying stable.
    // Without clamping, the per-frame change is only 0.0005 * (PostBlur - history),
    // giving sub-1/255 stability.
    float3 clamped_diff_color;
    float3 clamped_spec_color;

    if (diff_accum_incoming <= 2.0)
    {
        clamped_diff_color = stab_diff.rgb;
    }
    else
    {
        float clamped_diff_luma = clamp(stab_diff_luma, diff_lo, diff_hi);
        clamped_diff_color = stab_diff.rgb * (clamped_diff_luma / max(stab_diff_luma, 1e-6));
        if (stab_diff_luma < 1e-6)
            clamped_diff_color = current_diff.rgb;
    }

    if (spec_accum_incoming <= 2.0)
    {
        clamped_spec_color = stab_spec.rgb;
    }
    else
    {
        float clamped_spec_luma = clamp(stab_spec_luma, spec_lo, spec_hi);
        clamped_spec_color = stab_spec.rgb * (clamped_spec_luma / max(stab_spec_luma, 1e-6));
        if (stab_spec_luma < 1e-6)
            clamped_spec_color = current_spec.rgb;
    }

    // Stabilization blend weight: full strength from frame_index for all pixels.
    float diff_stab_frames = min(float(max_stabilized_frame_num), max(diff_accum_speed, float(frame_index)));
    float spec_stab_frames = min(float(max_stabilized_frame_num), max(spec_accum_speed, float(frame_index)));
    float diff_blend = diff_stab_frames / (diff_stab_frames + 1.0) * stabilization_strength;
    float spec_blend = spec_stab_frames / (spec_stab_frames + 1.0) * stabilization_strength;

    // Blend current with clamped history
    float3 final_diff = lerp(current_diff.rgb, clamped_diff_color, diff_blend);
    float3 final_spec = lerp(current_spec.rgb, clamped_spec_color, spec_blend);

    outDiffuse[pixel] = float4(final_diff, current_diff.a);
    outSpecular[pixel] = float4(final_spec, current_spec.a);

    // Write back modified accumulation speed (antilag may have reduced it)
    outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
}
