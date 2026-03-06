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
    float framerate_scale;
    uint debug_output;
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

// Motion vectors for reprojection
[[vk::binding(10, 0)]] Texture2D<float2> inMotionVectors;

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

    // Sky pixels: pass through (reset stabilized counter)
    if (center_viewZ > denoising_range)
    {
        float4 d = inDiffuse[pixel];
        outDiffuse[pixel] = float4(d.rgb, 0.0);
        outSpecular[pixel] = inSpecular[pixel];
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }

    // Stabilization disabled (reset stabilized counter)
    if (max_stabilized_frame_num == 0)
    {
        float4 d = inDiffuse[pixel];
        outDiffuse[pixel] = float4(d.rgb, 0.0);
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

    // Reproject stabilized history via motion vectors
    float2 motionVec = inMotionVectors[pixel];
    float2 currentUV = (float2(pixel) + 0.5) / float2(resolution);
    float2 prevUV = currentUV + motionVec;

    float4 stab_diff, stab_spec;
    // Per-pixel stabilized frame counter stored in .a of the stabilized textures.
    // Tracks how many consecutive frames this pixel has been stabilized.
    // Capped at max_stabilized_frame_num (default 63, matching TA).
    // Higher values cause energy loss due to float16 quantization error
    // amplified by the EMA's high temporal correlation.
    float prev_stab_count = 0.0;
    if (all(prevUV >= 0.0) && all(prevUV <= 1.0))
    {
        int2 prevPixel = int2(prevUV * float2(resolution));
        prevPixel = clamp(prevPixel, int2(0, 0), int2(resolution) - int2(1, 1));
        stab_diff = prevStabilizedDiff[prevPixel];
        stab_spec = prevStabilizedSpec[prevPixel];
        // Use diffuse .a for the shared stabilized frame counter
        prev_stab_count = stab_diff.a;
    }
    else
    {
        stab_diff = current_diff;
        stab_spec = current_spec;
        prev_stab_count = 0.0;
    }

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
            float s = diff_sigma * antilag_sigma_scale;
            float magic = antilag_sensitivity * framerate_scale * framerate_scale;
            float clamped = clamp(stab_diff_luma, diff_mean - s, diff_mean + s);
            float d = abs(stab_diff_luma - clamped) / (max(stab_diff_luma, clamped) + 1e-6);
            diff_antilag = 1.0 / (1.0 + d * diff_accum_speed / max(magic, 1e-6));
            diff_accum_speed = lerp(0.0, diff_accum_speed, diff_antilag);
        }

        if (spec_accum_speed >= 8.0)
        {
            float s = spec_sigma * antilag_sigma_scale;
            float magic = antilag_sensitivity * framerate_scale * framerate_scale;
            float clamped = clamp(stab_spec_luma, spec_mean - s, spec_mean + s);
            float d = abs(stab_spec_luma - clamped) / (max(stab_spec_luma, clamped) + 1e-6);
            spec_antilag = 1.0 / (1.0 + d * spec_accum_speed / max(magic, 1e-6));
            spec_accum_speed = lerp(0.0, spec_accum_speed, spec_antilag);
        }
    }

    // Adaptive sigma scale: tighter clamping as more history accumulates.
    float diff_non_linear = GetNonLinearAccumSpeed(diff_accum_speed);
    float spec_non_linear = GetNonLinearAccumSpeed(spec_accum_speed);
    float diff_sigma_scale = fast_history_sigma_scale * (1.0 + 3.0 * framerate_scale * diff_non_linear);
    float spec_sigma_scale = fast_history_sigma_scale * (1.0 + 3.0 * framerate_scale * spec_non_linear);

    // Clamp stabilized history to local color box.
    // At convergence sigma → 0, collapsing the range to [mean, mean] and
    // clamping every pixel to the neighborhood average (8.5% energy loss).
    // A minimum sigma floor proportional to the mean prevents this collapse
    // while still allowing tight clamping during noisy early frames.
    float diff_sigma_eff = max(diff_sigma, diff_mean * REBLUR_TS_MIN_SIGMA_FRACTION);
    float spec_sigma_eff = max(spec_sigma, spec_mean * REBLUR_TS_MIN_SIGMA_FRACTION);
    float diff_lo = diff_mean - diff_sigma_eff * diff_sigma_scale;
    float diff_hi = diff_mean + diff_sigma_eff * diff_sigma_scale;
    float spec_lo = spec_mean - spec_sigma_eff * spec_sigma_scale;
    float spec_hi = spec_mean + spec_sigma_eff * spec_sigma_scale;

    // For freshly disoccluded pixels (incoming accum<=1), the stabilized history
    // at the reprojected position belongs to a different surface (the object that
    // was there before). Blending it in causes ghosting.
    // Skip stabilization entirely: output 100% PostBlur.
    // For edge pixels (accum<=2), skip clamping but still blend weakly.
    float3 clamped_diff_color;
    float3 clamped_spec_color;

    bool diff_disoccluded = diff_accum_incoming <= 1.0;
    bool spec_disoccluded = spec_accum_incoming <= 1.0;

    if (diff_disoccluded)
    {
        // Freshly disoccluded: stabilized history is from wrong surface
        clamped_diff_color = current_diff.rgb;
    }
    else if (diff_accum_incoming <= 2.0)
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

    if (spec_disoccluded)
    {
        clamped_spec_color = current_spec.rgb;
    }
    else if (spec_accum_incoming <= 2.0)
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

    // Per-pixel stabilized frame counter: increments each frame, resets on
    // disocclusion, caps at max_stabilized_frame_num.
    // Antilag reduces the counter proportionally.
    float min_accum = min(diff_accum_incoming, spec_accum_incoming);
    float stab_count;
    if (min_accum <= 1.0)
    {
        // Disoccluded: reset stabilization counter
        stab_count = min_accum;
    }
    else
    {
        // Increment previous counter, but don't exceed max or drop below
        // current TA accum_speed (TA disocclusion detection is authoritative).
        float min_antilag = min(diff_antilag, spec_antilag);
        stab_count = prev_stab_count * min_antilag + 1.0;
        stab_count = clamp(stab_count, min_accum, float(max_stabilized_frame_num));
    }

    float diff_stab_frames = diff_disoccluded ? 0.0 : stab_count;
    float spec_stab_frames = spec_disoccluded ? 0.0 : stab_count;
    float diff_blend = diff_stab_frames / (diff_stab_frames + 1.0) * stabilization_strength;
    float spec_blend = spec_stab_frames / (spec_stab_frames + 1.0) * stabilization_strength;

    // Blend current with clamped history
    float3 final_diff = lerp(current_diff.rgb, clamped_diff_color, diff_blend);
    float3 final_spec = lerp(current_spec.rgb, clamped_spec_color, spec_blend);

    // Store stabilized frame counter in diffuse .a for next frame's readback
    outDiffuse[pixel] = float4(final_diff, stab_count);
    outSpecular[pixel] = float4(final_spec, current_spec.a);

    // Write back modified accumulation speed (antilag may have reduced it)
    outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
}
