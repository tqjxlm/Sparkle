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
    float4 view_to_view_prev_row0;
    float4 view_to_view_prev_row1;
    float4 view_to_view_prev_row2;
    float4 view_to_view_prev_row3;
    float4 view_to_world_row0;
    float4 view_to_world_row1;
    float4 view_to_world_row2;
    float4 view_to_world_row3;
    float4 camera_pos;
    float4 prev_camera_pos;
    float unproject_x;
    float unproject_y;
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

float ComputeSpecularSurfaceHistoryConfidence(float motion_pixels, float center_roughness,
                                              float current_hit_norm, float history_hit_norm,
                                              float prev_spec_accum_speed)
{
    float spec_magic = saturate(GetSpecMagicCurve(center_roughness));
    float spec_sharpness = 1.0 - spec_magic;
    float hit_norm = max(current_hit_norm, history_hit_norm);

    // Surface motion underestimates reflection motion. Approximate the missing
    // virtual-motion path by amplifying tiny camera motion on far, sharp lobes.
    float virtual_motion_pixels =
        motion_pixels * (1.0 + REBLUR_TA_SPEC_VIRTUAL_MOTION_SCALE * hit_norm * spec_sharpness);
    virtual_motion_pixels *= 1.0 + saturate(prev_spec_accum_speed / 4.0);

    // Sharp, long-hit specular should tolerate much less motion than rough,
    // near-surface reflections. Normalized hit distance is already scene-scale
    // aware, so use it directly as the parallax proxy here.
    float allowable_motion_pixels =
        lerp(REBLUR_TA_SPEC_MIN_VIRTUAL_LOBE_PX, REBLUR_TA_SPEC_MAX_VIRTUAL_LOBE_PX, spec_magic);
    allowable_motion_pixels /= 1.0 + hit_norm;

    float motion_ratio = virtual_motion_pixels / max(allowable_motion_pixels, 1e-4);
    float motion_confidence = 1.0 - smoothstep(0.1, 1.0, motion_ratio);

    float hit_dist_delta = abs(current_hit_norm - history_hit_norm);
    float hit_dist_confidence =
        1.0 / (1.0 + hit_dist_delta * REBLUR_TA_SPEC_HITDIST_CONFIDENCE_SCALE * (1.0 + 4.0 * spec_sharpness));

    return saturate(motion_confidence * hit_dist_confidence);
}

float3 GetViewSpacePosition(uint2 pixel, float view_z)
{
    float2 uv = (float2(pixel) + 0.5) / float2(resolution);
    float2 ndc = uv * 2.0 - 1.0;
    return float3(ndc.x * unproject_x * view_z, ndc.y * unproject_y * view_z, view_z);
}

float3 GetWorldSpacePosition(float3 view_pos)
{
    float4 p = float4(
        dot(view_to_world_row0, float4(view_pos, 1.0)),
        dot(view_to_world_row1, float4(view_pos, 1.0)),
        dot(view_to_world_row2, float4(view_pos, 1.0)),
        dot(view_to_world_row3, float4(view_pos, 1.0)));
    return p.xyz / max(abs(p.w), 1e-6);
}

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
    float center_roughness = nr.z;
    float currentMaterialIdNorm = nr.a;

    bool boundary_band = false;
    static const int boundary_radius = 2;
    [unroll]
    for (int dy = -boundary_radius; dy <= boundary_radius && !boundary_band; dy++)
    {
        [unroll]
        for (int dx = -boundary_radius; dx <= boundary_radius && !boundary_band; dx++)
        {
            int2 np = clamp(int2(pixel) + int2(dx, dy), int2(0, 0), int2(resolution) - int2(1, 1));
            float4 npnr = inNormalRoughness[uint2(np)];
            boundary_band = abs(npnr.a - currentMaterialIdNorm) > 0.0005;
        }
    }

    // Reprojection via motion vectors
    float2 motionVec = inMotionVectors[pixel];
    float motion_pixels = length(motionVec * float2(resolution));
    float2 currentUV = (float2(pixel) + 0.5) / float2(resolution);
    float2 prevUV = currentUV + motionVec;

    // First frame or reset: output current data with accumSpeed = 0
    if (reset_history != 0)
    {
        outDiffuse[pixel] = current_diff;
        outSpecular[pixel] = current_spec;
        outInternalData[pixel] = PackInternalData(0.0, 0.0);
        return;
    }

    // Check if reprojected position is in screen
    bool inScreen = all(prevUV >= 0.0) && all(prevUV <= 1.0);

    // Read previous frame data with occlusion-aware bilinear sampling
    float footprintQuality = 0.0;
    bool historyValid = false;
    bool allSamplesValid = false;
    float4 hist_diff = float4(0, 0, 0, 0);
    float4 hist_spec = float4(0, 0, 0, 0);
    float prev_diff_accum_speed = 0.0;
    float prev_spec_accum_speed = 0.0;
    bool centerFallbackUsed = false;

    if (inScreen)
    {
        int2 prevPixelCenter = int2(prevUV * float2(resolution));
        prevPixelCenter = clamp(prevPixelCenter, int2(0, 0), int2(resolution) - int2(1, 1));
        float expectedPrevZ = prevViewZ[prevPixelCenter];
        int2 historyOrigin = int2(0, 0);
        float4 historyWeights = 0.0;

        hist_diff = BilinearHistorySample(
            prevDiffHistory, prevUV, resolution,
            prevViewZ, prevNormalRoughness, expectedPrevZ,
            center_normal, currentMaterialIdNorm,
            disocclusion_threshold, denoising_range,
            footprintQuality, historyValid, allSamplesValid, historyOrigin, historyWeights);

        // Catmull-Rom upgrade: only when ALL bilinear taps are valid AND the 3x3
        // neighborhood around the reprojected center has the same instance_id.
        // CatmullRom's 4x4 footprint can reach across object boundaries, pulling
        // in wrong-surface history and causing ghosting near edges.
        if (historyValid)
        {
            hist_spec = SampleBilinearTexture(prevSpecHistory, historyOrigin, resolution, historyWeights);

            float2 prev_packed = SampleBilinearTexture(prevInternalData, historyOrigin, resolution, historyWeights);
            UnpackInternalData(prev_packed, prev_diff_accum_speed, prev_spec_accum_speed);

            if (!allSamplesValid)
            {
                float4 centerPrevNR = prevNormalRoughness[prevPixelCenter];
                float centerPrevZ = prevViewZ[prevPixelCenter];
                bool centerSampleValid = centerPrevZ <= denoising_range;
                if (centerSampleValid)
                {
                    float3 centerPrevNormal = DecodeNormalOct(centerPrevNR.xy);
                    centerSampleValid =
                        dot(centerPrevNormal, center_normal) >= 0.75 &&
                        abs(centerPrevNR.a - currentMaterialIdNorm) <= 0.0005;
                }

                if (centerSampleValid)
                {
                    hist_diff = prevDiffHistory[prevPixelCenter];
                    hist_spec = prevSpecHistory[prevPixelCenter];
                    UnpackInternalData(prevInternalData[prevPixelCenter],
                                       prev_diff_accum_speed, prev_spec_accum_speed);
                    // Same-surface center fallback avoids the shifted bilinear
                    // footprint entirely, so keep TA's history-quality path
                    // consistent with FinalHistory / stabilized albedo.
                    footprintQuality = 1.0;
                    centerFallbackUsed = true;
                }
            }

            if (allSamplesValid)
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
        }
    }

    // Disocclusion: either out-of-screen or no valid history samples
    bool disoccluded = !inScreen || !historyValid;
    float spec_history_quality = footprintQuality;

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

        if (motion_pixels > REBLUR_TA_MOTION_RESET_THRESHOLD_PX && !allSamplesValid)
        {
            float partial_motion_cap = centerFallbackUsed
                ? REBLUR_TA_CENTER_FALLBACK_MOTION_MAX_ACCUM
                : REBLUR_TA_MOTION_MAX_ACCUM;
            diff_accum_speed = min(diff_accum_speed, partial_motion_cap);
            spec_accum_speed = min(spec_accum_speed, partial_motion_cap);
        }

        if (motion_pixels > REBLUR_TA_MOTION_RESET_THRESHOLD_PX && boundary_band && !allSamplesValid)
        {
            float boundary_motion_cap = centerFallbackUsed
                ? REBLUR_TA_CENTER_FALLBACK_MOTION_MAX_ACCUM
                : REBLUR_TA_BOUNDARY_MOTION_MAX_ACCUM;
            diff_accum_speed = min(diff_accum_speed, boundary_motion_cap);
            spec_accum_speed = min(spec_accum_speed, boundary_motion_cap);
        }

        if (motion_pixels > 0.0)
        {
            float spec_surface_history_confidence = ComputeSpecularSurfaceHistoryConfidence(
                motion_pixels, center_roughness, current_spec.a, hist_spec.a, prev_spec_accum_speed);

            // Keep the old surface-motion path for larger motion, but let the
            // parallax proxy drive confidence continuously below the 1 px gate.
            if (motion_pixels > REBLUR_TA_MOTION_RESET_THRESHOLD_PX)
            {
                float spec_motion_sensitivity =
                    lerp(1.0, REBLUR_TA_SPEC_MOTION_MIN_SENSITIVITY, saturate(center_roughness));
                float motion_gate_confidence =
                    1.0 / (1.0 + motion_pixels * spec_motion_sensitivity * REBLUR_TA_SPEC_MOTION_CONFIDENCE_SCALE);
                spec_surface_history_confidence = min(spec_surface_history_confidence, motion_gate_confidence);
            }

            spec_history_quality = min(spec_history_quality, spec_surface_history_confidence);

            float spec_max_frames = lerp(REBLUR_TA_SPEC_MOTION_MIN_MAX_ACCUM,
                                         REBLUR_TA_SPEC_MOTION_MAX_ACCUM,
                                         sqrt(spec_history_quality));
            spec_accum_speed = min(spec_accum_speed, spec_max_frames);
        }
    }

    // Surface-motion reprojection is the only specular path currently implemented.
    // The old "specular parallax" heuristic aggressively reset smooth-surface
    // history on any camera nudge, which made the blue sphere lose history even
    // when the user-visible motion was tiny. Leave the history intact here until
    // a real VMB path is implemented.

    // Separate blend weights per channel
    float diff_weight = 1.0 / (1.0 + diff_accum_speed);
    float spec_weight = 1.0 / (1.0 + spec_accum_speed);
    spec_weight = max(spec_weight, 1.0 - spec_history_quality);

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
        // Raw specular history diagnostic for spec-only passthrough.
        outDiffuse[pixel] = hist_diff;
        outSpecular[pixel] = hist_spec;
        outInternalData[pixel] = PackInternalData(prev_diff_accum_speed, prev_spec_accum_speed);
        return;
    }
    if (debug_output == 6)
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
    if (debug_output == 7)
    {
        float inv_max_accum = 1.0 / max(max_accumulated_frame_num, 1.0);
        // Diffuse RGB = current accum, previous accum, footprint quality.
        // Specular RGB = current accum, previous accum, full-footprint-valid flag.
        outDiffuse[pixel] = float4(
            saturate(diff_accum_speed * inv_max_accum),
            saturate(prev_diff_accum_speed * inv_max_accum),
            footprintQuality,
            1.0);
        outSpecular[pixel] = float4(
            saturate(spec_accum_speed * inv_max_accum),
            saturate(prev_spec_accum_speed * inv_max_accum),
            allSamplesValid ? 1.0 : 0.0,
            1.0);
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }
    if (debug_output == 8)
    {
        float inv_max_accum = 1.0 / max(max_accumulated_frame_num, 1.0);
        outDiffuse[pixel] = float4(
            saturate(diff_accum_speed * inv_max_accum),
            saturate(prev_diff_accum_speed * inv_max_accum),
            footprintQuality,
            1.0);
        outSpecular[pixel] = float4(
            saturate(spec_accum_speed * inv_max_accum),
            saturate(prev_spec_accum_speed * inv_max_accum),
            spec_history_quality,
            1.0);
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }
    if (debug_output == 9)
    {
        float inv_max_accum = 1.0 / max(max_accumulated_frame_num, 1.0);
        outDiffuse[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        outSpecular[pixel] = float4(
            saturate(spec_accum_speed * inv_max_accum),
            spec_history_quality,
            allSamplesValid ? 1.0 : 0.0,
            1.0);
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }
    if (debug_output == 10)
    {
        outDiffuse[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        outSpecular[pixel] = float4(
            saturate((1.0 - footprintQuality) * 16.0),
            saturate((1.0 - spec_history_quality) * 16.0),
            allSamplesValid ? 1.0 : 0.0,
            1.0);
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }
    if (debug_output == 11)
    {
        outDiffuse[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        outSpecular[pixel] = float4(
            center_roughness,
            current_spec.a,
            saturate(GetSpecMagicCurve(center_roughness)),
            1.0);
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }
    if (debug_output == 12)
    {
        // Fine-scale motion visualization for subpixel gate diagnostics.
        float2 mv_pixels = motionVec * float2(resolution);
        outDiffuse[pixel] = float4(
            mv_pixels.x * 0.1 + 0.5,
            mv_pixels.y * 0.1 + 0.5,
            length(mv_pixels) * 0.1,
            1.0);
        outSpecular[pixel] = float4(0, 0, 0, 0);
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }
    if (debug_output == 13)
    {
        float2 plane_weights = 0.0;
        float plane_weight_sum = 0.0;

        float3 center_view_pos = GetViewSpacePosition(pixel, center_viewZ);
        float3 center_world_pos = GetWorldSpacePosition(center_view_pos);
        float4 prev_view_pos_h = float4(
            dot(view_to_view_prev_row0, float4(center_view_pos, 1.0)),
            dot(view_to_view_prev_row1, float4(center_view_pos, 1.0)),
            dot(view_to_view_prev_row2, float4(center_view_pos, 1.0)),
            dot(view_to_view_prev_row3, float4(center_view_pos, 1.0)));
        float expected_prev_view_z = prev_view_pos_h.z / max(abs(prev_view_pos_h.w), 1e-6);
        float plane_thresh = disocclusion_threshold * max(abs(expected_prev_view_z), 1.0);
        float3 V = normalize(camera_pos.xyz - center_world_pos);
        float3 Vprev = normalize(prev_camera_pos.xyz - center_world_pos);
        float NoV = abs(dot(center_normal, V));
        float NoVprev = abs(dot(center_normal, Vprev));
        float sizeQuality = (NoVprev + 1e-3) / (NoV + 1e-3);
        sizeQuality *= sizeQuality;
        sizeQuality = lerp(0.1, 1.0, saturate(sizeQuality));

        if (inScreen)
        {
            float2 prevPixelF = prevUV * float2(resolution) - 0.5;
            int2 tl = int2(floor(prevPixelF));
            float2 f = prevPixelF - float2(tl);
            float4 bw = float4(
                (1.0 - f.x) * (1.0 - f.y),
                f.x * (1.0 - f.y),
                (1.0 - f.x) * f.y,
                f.x * f.y);
            int2 offsets[4] = { int2(0,0), int2(1,0), int2(0,1), int2(1,1) };

            for (int i = 0; i < 4; i++)
            {
                int2 p = tl + offsets[i];
                if (p.x < 0 || p.y < 0 || p.x >= int(resolution.x) || p.y >= int(resolution.y))
                    continue;

                float pz = prevViewZ[p];
                if (pz > denoising_range)
                    continue;

                float4 pnr = prevNormalRoughness[p];
                float3 pn = DecodeNormalOct(pnr.xy);
                if (dot(pn, center_normal) < 0.75)
                    continue;
                if (abs(pnr.a - currentMaterialIdNorm) > 0.0005)
                    continue;

                float plane_ratio = abs(pz - expected_prev_view_z) / max(plane_thresh, 1e-6);
                plane_weights.x += bw[i] * saturate(plane_ratio);
                plane_weights.y += bw[i] * float(plane_ratio <= 1.0);
                plane_weight_sum += bw[i];
            }
        }

        float weighted_plane_ratio = plane_weight_sum > 0.0 ? plane_weights.x / plane_weight_sum : 0.0;
        float plane_footprint_quality = plane_weight_sum > 0.0 ? sqrt(saturate(plane_weights.y / plane_weight_sum)) : 0.0;
        outDiffuse[pixel] = float4(
            saturate(weighted_plane_ratio),
            saturate((1.0 - plane_footprint_quality) * 16.0),
            saturate((1.0 - sizeQuality) * 16.0),
            1.0);
        outSpecular[pixel] = float4(
            saturate(abs(expected_prev_view_z - prevViewZ[clamp(int2(prevUV * float2(resolution)), int2(0, 0), int2(resolution) - int2(1, 1))]) / max(plane_thresh, 1e-6)),
            saturate((1.0 - footprintQuality) * 16.0),
            0.0,
            1.0);
        outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
        return;
    }
    // Write outputs
    outDiffuse[pixel] = acc_diff;
    outSpecular[pixel] = acc_spec;
    outInternalData[pixel] = PackInternalData(diff_accum_speed, spec_accum_speed);
}
