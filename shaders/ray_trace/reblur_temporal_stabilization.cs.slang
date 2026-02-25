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

// Shared memory for neighborhood luminance computation
#define BORDER 1
#define TILE_SIZE 16
#define BUFFER_X (TILE_SIZE + 2 * BORDER)
#define BUFFER_Y (TILE_SIZE + 2 * BORDER)

groupshared float s_DiffLuma[BUFFER_Y][BUFFER_X];
groupshared float s_SpecLuma[BUFFER_Y][BUFFER_X];

[shader("compute")]
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    // Load neighborhood into shared memory (tile + 1-pixel border)
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

    // Sky pixels: pass through
    if (center_viewZ > denoising_range)
    {
        outDiffuse[pixel] = inDiffuse[pixel];
        outSpecular[pixel] = inSpecular[pixel];
        return;
    }

    // Stabilization disabled
    if (max_stabilized_frame_num == 0)
    {
        outDiffuse[pixel] = inDiffuse[pixel];
        outSpecular[pixel] = inSpecular[pixel];
        return;
    }

    float4 current_diff = inDiffuse[pixel];
    float4 current_spec = inSpecular[pixel];

    // Compute local mean and variance from shared memory neighborhood
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

    // Clamp stabilized history to local color box
    float stab_diff_luma = GetLuminance(stab_diff.rgb);
    float stab_spec_luma = GetLuminance(stab_spec.rgb);

    float diff_lo = diff_mean - diff_sigma * fast_history_sigma_scale;
    float diff_hi = diff_mean + diff_sigma * fast_history_sigma_scale;
    float spec_lo = spec_mean - spec_sigma * fast_history_sigma_scale;
    float spec_hi = spec_mean + spec_sigma * fast_history_sigma_scale;

    // Clamp history color based on luminance box
    float clamped_diff_luma = clamp(stab_diff_luma, diff_lo, diff_hi);
    float clamped_spec_luma = clamp(stab_spec_luma, spec_lo, spec_hi);

    // Rescale the history color to match clamped luminance
    float3 clamped_diff_color = stab_diff.rgb * (clamped_diff_luma / max(stab_diff_luma, 1e-6));
    float3 clamped_spec_color = stab_spec.rgb * (clamped_spec_luma / max(stab_spec_luma, 1e-6));

    // Guard against zero-luminance history (first frame or sky->geometry)
    if (stab_diff_luma < 1e-6)
        clamped_diff_color = current_diff.rgb;
    if (stab_spec_luma < 1e-6)
        clamped_spec_color = current_spec.rgb;

    // Read accumulation speed from internal data to modulate blend
    float2 packed = inInternalData[pixel];
    float accum_speed, accum_speed_fast;
    UnpackInternalData(packed, accum_speed, accum_speed_fast);

    // Stabilization blend weight: stronger with more history
    float stab_frames = min(float(max_stabilized_frame_num), accum_speed);
    float blend = stab_frames / (stab_frames + 1.0);
    blend *= stabilization_strength;

    // Blend current with clamped history
    float3 final_diff = lerp(current_diff.rgb, clamped_diff_color, blend);
    float3 final_spec = lerp(current_spec.rgb, clamped_spec_color, blend);

    outDiffuse[pixel] = float4(final_diff, current_diff.a);
    outSpecular[pixel] = float4(final_spec, current_spec.a);
}
