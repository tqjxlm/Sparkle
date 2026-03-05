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
    uint debug_output;
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
        int2 prevPixelCenter = int2(prevUV * float2(resolution));
        prevPixelCenter = clamp(prevPixelCenter, int2(0, 0), int2(resolution) - int2(1, 1));

        float expectedPrevZ = prevViewZ[prevPixelCenter];
        float currentMaterialIdNorm = nr.a;

        hist_diff = BilinearHistorySample(
            prevDiffHistory, prevUV, resolution,
            prevViewZ, prevNormalRoughness, expectedPrevZ,
            center_normal, currentMaterialIdNorm,
            disocclusion_threshold, denoising_range,
            footprintQuality, historyValid);

        // Sample specular with same occlusion pattern
        float specFootprint;
        bool specValid;
        hist_spec = BilinearHistorySample(
            prevSpecHistory, prevUV, resolution,
            prevViewZ, prevNormalRoughness, expectedPrevZ,
            center_normal, currentMaterialIdNorm,
            disocclusion_threshold, denoising_range,
            specFootprint, specValid);

        historyValid = historyValid && specValid;

        // Catmull-Rom upgrade: only when ALL bilinear taps are valid AND the 3x3
        // neighborhood around the reprojected center has the same instance_id.
        // CatmullRom's 4x4 footprint can reach across object boundaries, pulling
        // in wrong-surface history and causing ghosting near edges.
        if (historyValid && footprintQuality > 0.99)
        {
            bool catrom_safe = true;
            for (int dy = -1; dy <= 1 && catrom_safe; dy++)
            {
                for (int dx = -1; dx <= 1 && catrom_safe; dx++)
                {
                    int2 np = prevPixelCenter + int2(dx, dy);
                    np = clamp(np, int2(0, 0), int2(resolution) - int2(1, 1));
                    float4 npnr = prevNormalRoughness[np];
                    if (abs(npnr.a - currentMaterialIdNorm) > 0.0005)
                        catrom_safe = false;
                }
            }
            if (catrom_safe)
            {
                hist_diff = CatmullRomHistorySample(prevDiffHistory, linearSampler, prevUV, resolution);
                hist_spec = CatmullRomHistorySample(prevSpecHistory, linearSampler, prevUV, resolution);
            }
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
        // Use current frame as history stand-in to avoid energy loss.
        // When all bilinear taps are rejected, hist_diff/spec = float4(0).
        // Without this, lerp(0, current, 0.5) = 0.5 * current → dark ghosts.
        // With this, lerp(current, current, 0.5) = current → full energy.
        // accum_speed = 1 is preserved for edge stability tracking.
        hist_diff = current_diff;
        hist_spec = current_spec;
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

    // Specular parallax: view-dependent highlights move differently than surfaces.
    // Surface motion vectors don't capture the "virtual motion" of specular reflections.
    // For smooth surfaces with camera motion, reduce specular history trust.
    if (!disoccluded)
    {
        float roughness = nr.z;
        float2 mv_pixels = motionVec * float2(resolution);
        float mv_mag = length(mv_pixels);

        // Threshold > 1px to ignore sub-pixel jitter (TAA jitter causes ~0.7px MVs)
        if (mv_mag > 1.5)
        {
            // Sensitivity: 1.0 for mirrors (roughness=0), 0 at roughness >= 0.5
            // At roughness >= 0.5 specular lobe is broad enough for surface MVs
            float sensitivity = saturate(1.0 - roughness * 2.0);

            // Reduce accumulation speed toward 1.0 (fast re-convergence)
            float parallax_reduction = saturate(mv_mag * sensitivity / 5.0);
            spec_accum_speed = lerp(spec_accum_speed, 1.0, parallax_reduction);
            spec_accum_speed = max(spec_accum_speed, 1.0);
        }
    }

    // Separate blend weights per channel
    float diff_weight = 1.0 / (1.0 + diff_accum_speed);
    float spec_weight = 1.0 / (1.0 + spec_accum_speed);

    // Firefly suppression is NOT applied here in temporal accumulation.
    // Reason: the TA output feeds back as history (via CopyHistoryData), so any
    // per-sample clamping compounds through the exponential moving average,
    // causing systematic energy loss (measured: 23% at 2048 spp with 4x clamp).
    // Instead, firefly suppression is handled by HistoryFix (neighborhood mean±2σ
    // clamp), which operates on the spatial domain and is NOT in the history
    // feedback loop (CopyHistoryData copies TA output before HistoryFix).

    // Linear accumulation: lerp(history, current, weight)
    float4 acc_diff = lerp(hist_diff, current_diff, diff_weight);
    float4 acc_spec = lerp(hist_spec, current_spec, spec_weight);

    // Diagnostic output modes (triggered by debug_output UBO flag)
    if (debug_output == 1)
    {
        // Disocclusion map:
        //   R = disoccluded (1=yes), G = footprintQuality, B = inScreen
        outDiffuse[pixel] = float4(disoccluded ? 1.0 : 0.0, footprintQuality, inScreen ? 1.0 : 0.0, 1.0);
        outSpecular[pixel] = float4(0, 0, 0, 0);
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }
    if (debug_output == 2)
    {
        // Motion vector visualization:
        //   RG = motionVec * resolution (pixel displacement), B = magnitude
        float2 mv_pixels = motionVec * float2(resolution);
        outDiffuse[pixel] = float4(mv_pixels.x * 0.01 + 0.5, mv_pixels.y * 0.01 + 0.5,
                                   length(mv_pixels) * 0.01, 1.0);
        outSpecular[pixel] = float4(0, 0, 0, 0);
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }
    if (debug_output == 3)
    {
        // Depth test diagnostic:
        //   R = depth ratio (expected vs actual at prevPixelCenter), G = normal dot, B = prev accum speed / 63
        float depth_ratio = 0.0;
        float normal_agreement = 0.0;
        if (inScreen)
        {
            int2 prevPC = int2(prevUV * float2(resolution));
            prevPC = clamp(prevPC, int2(0, 0), int2(resolution) - int2(1, 1));
            float epz = prevViewZ[prevPC];
            // Also sample one of the bilinear corners
            float2 prevPixelF = prevUV * float2(resolution) - 0.5;
            int2 tl = int2(floor(prevPixelF));
            tl = clamp(tl, int2(0, 0), int2(resolution) - int2(1, 1));
            float cornerZ = prevViewZ[tl];
            if (abs(epz) > 1e-6)
                depth_ratio = abs(cornerZ - epz) / (disocclusion_threshold * max(abs(epz), 1.0));
            float4 pnr = prevNormalRoughness[prevPC];
            float3 pn = DecodeNormalOct(pnr.xy);
            normal_agreement = dot(pn, center_normal) * 0.5 + 0.5;
        }
        outDiffuse[pixel] = float4(saturate(depth_ratio), normal_agreement, prev_diff_accum_speed / 63.0, 1.0);
        outSpecular[pixel] = float4(0, 0, 0, 0);
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }
    if (debug_output == 4)
    {
        // Raw history diagnostic: output reprojected history and accum speeds.
        // Composite with diagnostic passthrough will show diff*albedo+spec.
        // If luminance matches converged before-screenshot, history is intact.
        // R,G,B = raw reprojected history (hist_diff), not blended.
        outDiffuse[pixel] = hist_diff;
        outSpecular[pixel] = hist_spec;
        outInternalData[pixel] = PackInternalData(prev_diff_accum_speed, prev_spec_accum_speed);
        return;
    }
    if (debug_output == 5)
    {
        // Material ID diagnostic:
        //   R = current material ID (normalized), G = prev material ID at reprojected position,
        //   B = material ID mismatch (1 = mismatch/disoccluded, 0 = match)
        float prevMatId = 0.0;
        bool matMismatch = false;
        if (inScreen)
        {
            int2 prevPC = int2(prevUV * float2(resolution));
            prevPC = clamp(prevPC, int2(0, 0), int2(resolution) - int2(1, 1));
            float4 pnr = prevNormalRoughness[prevPC];
            prevMatId = pnr.a;
            matMismatch = abs(pnr.a - nr.a) > 0.0005;
        }
        outDiffuse[pixel] = float4(nr.a, prevMatId, matMismatch ? 1.0 : 0.0, 1.0);
        outSpecular[pixel] = float4(0, 0, 0, 0);
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }
    // Write outputs
    outDiffuse[pixel] = acc_diff;
    outSpecular[pixel] = acc_spec;
    outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
}
