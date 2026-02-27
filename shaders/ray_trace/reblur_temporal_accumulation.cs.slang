#include "reblur_common.h.slang"
#include "reblur_config.h.slang"
#include "reblur_data.h.slang"
#include "reblur_reprojection.h.slang"

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

// Motion vectors (from path tracer)
[[vk::binding(13, 0)]] Texture2D<float2> inMotionVectors;
[[vk::binding(14, 0)]] SamplerState linearSampler;

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

    // Reprojection via motion vectors
    float2 motionVec = inMotionVectors[pixel];
    float2 currentUV = (float2(pixel) + 0.5) / float2(resolution);
    float2 prevUV = currentUV + motionVec;

    // Check if reprojected position is in screen
    bool inScreen = all(prevUV >= 0.0) && all(prevUV <= 1.0);

    // Read previous frame data with occlusion-aware bilinear sampling
    float footprintQuality = 0.0;
    bool historyValid = false;
    float4 hist_diff = float4(0, 0, 0, 0);
    float4 hist_spec = float4(0, 0, 0, 0);
    float prev_diff_accum_speed = 0.0;
    float prev_spec_accum_speed = 0.0;

    if (inScreen)
    {
        // Use center of bilinear footprint for expected previous-frame depth
        int2 prevPixelCenter = int2(prevUV * float2(resolution));
        prevPixelCenter = clamp(prevPixelCenter, int2(0, 0), int2(resolution) - int2(1, 1));
        float expectedPrevZ = prevViewZ[prevPixelCenter];

        hist_diff = BilinearHistorySample(
            prevDiffHistory, prevUV, resolution,
            prevViewZ, prevNormalRoughness, expectedPrevZ,
            center_normal, disocclusion_threshold, denoising_range,
            footprintQuality, historyValid);

        // Sample specular with same occlusion pattern
        float specFootprint;
        bool specValid;
        hist_spec = BilinearHistorySample(
            prevSpecHistory, prevUV, resolution,
            prevViewZ, prevNormalRoughness, expectedPrevZ,
            center_normal, disocclusion_threshold, denoising_range,
            specFootprint, specValid);

        historyValid = historyValid && specValid;

        // Catmull-Rom upgrade when all bilinear samples are valid
        if (historyValid && footprintQuality > 0.99)
        {
            hist_diff = CatmullRomHistorySample(prevDiffHistory, linearSampler, prevUV, resolution);
            hist_spec = CatmullRomHistorySample(prevSpecHistory, linearSampler, prevUV, resolution);
        }

        // Read previous accumulation speed from center of footprint
        float2 prev_packed = prevInternalData[prevPixelCenter];
        UnpackInternalData(prev_packed, prev_diff_accum_speed, prev_spec_accum_speed);
    }

    // Disocclusion: either out-of-screen or no valid history samples
    bool disoccluded = !inScreen || !historyValid;

    // Compute new accumulation speeds - separate per channel
    float diff_accum_speed, spec_accum_speed;
    if (disoccluded)
    {
        // Fixed minimum accumSpeed of 1 for disoccluded pixels (weight = 0.5).
        // Prevents full-range jumps at object edges where sub-pixel jitter
        // causes the primary ray to alternate between surfaces every frame.
        diff_accum_speed = 1.0;
        spec_accum_speed = 1.0;
    }
    else
    {
        diff_accum_speed = min(prev_diff_accum_speed + 1.0, max_accumulated_frame_num);
        spec_accum_speed = min(prev_spec_accum_speed + 1.0, max_accumulated_frame_num);

        // Modulate by footprint quality: lower quality → slower accumulation
        diff_accum_speed *= footprintQuality;
        spec_accum_speed *= footprintQuality;
        diff_accum_speed = max(diff_accum_speed, 1.0);
        spec_accum_speed = max(spec_accum_speed, 1.0);
    }

    // Separate blend weights per channel
    float diff_weight = 1.0 / (1.0 + diff_accum_speed);
    float spec_weight = 1.0 / (1.0 + spec_accum_speed);

    // Firefly suppression: clamp incoming sample luminance relative to history.
    // At high accumSpeed, history represents a stable average. Extreme 1-spp outliers
    // (specular caustics reaching 40-60x the average) cause large per-frame jumps
    // that propagate through Blur/PostBlur/Stabilization.
    //
    // Evidence (Session 4):
    //   - 38x threshold has zero measurable effect because PrePass dilutes outliers to ~5-10x
    //   - Disoccluded pixels (accumSpeed=0) have weight=1.0 and are completely unprotected
    //   - Nuclear diagnostic confirmed code path IS executing
    //   - Need much lower threshold AND coverage of disoccluded pixels
    //
    // Strategy:
    //   - accumSpeed >= 4: history is stable, use tight relative clamp (4x history)
    //   - accumSpeed 1-3: history is noisy, use moderate relative clamp (8x history)
    //   - accumSpeed 0 (disoccluded): no valid history, use fixed absolute maximum
    //   - All branches have absolute minimum floor to prevent clamping near-zero to zero
    if (enable_firefly_suppression != 0)
    {
        float hist_diff_luma = GetLuminance(hist_diff.rgb);
        float curr_diff_luma = GetLuminance(current_diff.rgb);
        float hist_spec_luma = GetLuminance(hist_spec.rgb);
        float curr_spec_luma = GetLuminance(current_spec.rgb);

        // Diffuse channel
        if (diff_accum_speed >= 4.0)
        {
            // Stable history: tight clamp at 4x
            float max_diff_luma = max(hist_diff_luma * 4.0, REBLUR_FIREFLY_SUPPRESSOR_MIN_RELATIVE_SCALE);
            if (curr_diff_luma > max_diff_luma && curr_diff_luma > 0.0)
                current_diff.rgb *= max_diff_luma / curr_diff_luma;
        }
        else if (diff_accum_speed >= 1.0)
        {
            // Early accumulation: moderate clamp at 8x
            float max_diff_luma = max(hist_diff_luma * 8.0, REBLUR_FIREFLY_SUPPRESSOR_MIN_RELATIVE_SCALE);
            if (curr_diff_luma > max_diff_luma && curr_diff_luma > 0.0)
                current_diff.rgb *= max_diff_luma / curr_diff_luma;
        }
        else
        {
            // Disoccluded: no valid history, fixed absolute maximum.
            // At 2048+ spp, well-converged pixel luminance rarely exceeds 2.0 in demodulated space.
            float max_diff_luma = REBLUR_FIREFLY_SUPPRESSOR_MIN_RELATIVE_SCALE;
            if (curr_diff_luma > max_diff_luma && curr_diff_luma > 0.0)
                current_diff.rgb *= max_diff_luma / curr_diff_luma;
        }

        // Specular channel
        if (spec_accum_speed >= 4.0)
        {
            float max_spec_luma = max(hist_spec_luma * 4.0, REBLUR_FIREFLY_SUPPRESSOR_MIN_RELATIVE_SCALE);
            if (curr_spec_luma > max_spec_luma && curr_spec_luma > 0.0)
                current_spec.rgb *= max_spec_luma / curr_spec_luma;
        }
        else if (spec_accum_speed >= 1.0)
        {
            float max_spec_luma = max(hist_spec_luma * 8.0, REBLUR_FIREFLY_SUPPRESSOR_MIN_RELATIVE_SCALE);
            if (curr_spec_luma > max_spec_luma && curr_spec_luma > 0.0)
                current_spec.rgb *= max_spec_luma / curr_spec_luma;
        }
        else
        {
            float max_spec_luma = REBLUR_FIREFLY_SUPPRESSOR_MIN_RELATIVE_SCALE;
            if (curr_spec_luma > max_spec_luma && curr_spec_luma > 0.0)
                current_spec.rgb *= max_spec_luma / curr_spec_luma;
        }
    }

    // Linear accumulation: lerp(history, current, weight)
    float4 acc_diff = lerp(hist_diff, current_diff, diff_weight);
    float4 acc_spec = lerp(hist_spec, current_spec, spec_weight);

    // Write outputs
    outDiffuse[pixel] = acc_diff;
    outSpecular[pixel] = acc_spec;
    outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
}
