#include "reblur_data.h.slang"
#include "reblur_reprojection.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    float disocclusion_threshold;
    float denoising_range;
    uint reset_history;
};

[[vk::binding(1, 0)]] Texture2D<float4> inAlbedoMetallic;
[[vk::binding(2, 0)]] Texture2D<float4> inNormalRoughness;
[[vk::binding(3, 0)]] Texture2D<float> inViewZ;
[[vk::binding(4, 0)]] Texture2D<float2> inMotionVectors;
[[vk::binding(5, 0)]] Texture2D<float2> inInternalData;
[[vk::binding(6, 0)]] Texture2D<float4> prevAlbedoMetallic;
[[vk::binding(7, 0)]] Texture2D<float> prevViewZ;
[[vk::binding(8, 0)]] Texture2D<float4> prevNormalRoughness;
[[vk::binding(9, 0)]] SamplerState linearSampler;
[[vk::binding(10, 0)]] RWTexture2D<float4> outAlbedoMetallic;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
        return;

    float4 current = inAlbedoMetallic[pixel];
    float center_viewZ = inViewZ[pixel];

    if (center_viewZ > denoising_range || reset_history != 0)
    {
        outAlbedoMetallic[pixel] = current;
        return;
    }

    float4 nr = inNormalRoughness[pixel];
    float3 center_normal = DecodeNormalOct(nr.xy);
    float current_material_id = nr.a;

    float2 motionVec = inMotionVectors[pixel];
    float2 currentUV = (float2(pixel) + 0.5) / float2(resolution);
    float2 prevUV = currentUV + motionVec;
    bool inScreen = all(prevUV >= 0.0) && all(prevUV <= 1.0);

    float footprintQuality = 0.0;
    bool historyValid = false;
    float4 history = current;

    if (inScreen)
    {
        int2 prevPixelCenter = int2(prevUV * float2(resolution));
        prevPixelCenter = clamp(prevPixelCenter, int2(0, 0), int2(resolution) - int2(1, 1));

        float expectedPrevZ = prevViewZ[prevPixelCenter];
        history = BilinearHistorySample(
            prevAlbedoMetallic, prevUV, resolution,
            prevViewZ, prevNormalRoughness, expectedPrevZ,
            center_normal, current_material_id,
            disocclusion_threshold, denoising_range,
            footprintQuality, historyValid);

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
                    if (abs(npnr.a - current_material_id) > 0.0005)
                        catrom_safe = false;
                }
            }

            if (catrom_safe)
                history = CatmullRomHistorySample(prevAlbedoMetallic, linearSampler, prevUV, resolution);
        }
    }

    if (!inScreen || !historyValid)
    {
        outAlbedoMetallic[pixel] = current;
        return;
    }

    float diff_accum_speed = 0.0;
    float spec_accum_speed = 0.0;
    UnpackInternalData(inInternalData[pixel], diff_accum_speed, spec_accum_speed);
    float accum = min(diff_accum_speed, spec_accum_speed);

    float history_weight = accum / (accum + 1.0);
    history_weight *= footprintQuality;

    outAlbedoMetallic[pixel] = lerp(current, history, saturate(history_weight));
}
