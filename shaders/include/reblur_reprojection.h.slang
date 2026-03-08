#ifndef REBLUR_REPROJECTION_H
#define REBLUR_REPROJECTION_H

#include "reblur_common.h.slang"

// Bilinear history sampling with per-sample occlusion validation.
// Returns: sampled history value, sets footprintQuality to [0,1].
// currentMaterialIdNorm: normalized material ID of the current pixel (from normalRoughness.a).
// Material ID mismatch → disocclusion (prevents cross-object ghosting).
float4 SampleBilinearTexture(Texture2D<float4> tex, int2 bilinearOrigin, uint2 resolution, float4 bilinearWeights)
{
    int2 maxPixel = int2(resolution) - int2(1, 1);
    int2 p00 = clamp(bilinearOrigin, int2(0, 0), maxPixel);
    int2 p10 = clamp(bilinearOrigin + int2(1, 0), int2(0, 0), maxPixel);
    int2 p01 = clamp(bilinearOrigin + int2(0, 1), int2(0, 0), maxPixel);
    int2 p11 = clamp(bilinearOrigin + int2(1, 1), int2(0, 0), maxPixel);

    float4 result = tex[p00] * bilinearWeights.x;
    result += tex[p10] * bilinearWeights.y;
    result += tex[p01] * bilinearWeights.z;
    result += tex[p11] * bilinearWeights.w;
    return result;
}

float2 SampleBilinearTexture(Texture2D<float2> tex, int2 bilinearOrigin, uint2 resolution, float4 bilinearWeights)
{
    int2 maxPixel = int2(resolution) - int2(1, 1);
    int2 p00 = clamp(bilinearOrigin, int2(0, 0), maxPixel);
    int2 p10 = clamp(bilinearOrigin + int2(1, 0), int2(0, 0), maxPixel);
    int2 p01 = clamp(bilinearOrigin + int2(0, 1), int2(0, 0), maxPixel);
    int2 p11 = clamp(bilinearOrigin + int2(1, 1), int2(0, 0), maxPixel);

    float2 result = tex[p00] * bilinearWeights.x;
    result += tex[p10] * bilinearWeights.y;
    result += tex[p01] * bilinearWeights.z;
    result += tex[p11] * bilinearWeights.w;
    return result;
}

float4 BilinearHistorySample(
    Texture2D<float4> prevTexture,
    float2 prevUV,
    uint2 resolution,
    Texture2D<float> prevViewZ,
    Texture2D<float4> prevNormalRough,
    float expectedPrevZ,
    float3 currentNormal,
    float currentMaterialIdNorm,
    float disocclusionThreshold,
    float denoisingRange,
    out float footprintQuality,
    out bool isValid,
    out bool allSamplesValid,
    out int2 bilinearOrigin,
    out float4 bilinearWeights)
{
    float2 prevPixelF = prevUV * float2(resolution) - 0.5;
    int2 tl = int2(floor(prevPixelF));
    float2 f = prevPixelF - float2(tl);
    bilinearOrigin = tl;
    bilinearWeights = 0.0;

    // Bilinear weights
    float4 bw = float4(
        (1.0 - f.x) * (1.0 - f.y),  // tl
        f.x * (1.0 - f.y),            // tr
        (1.0 - f.x) * f.y,            // bl
        f.x * f.y                     // br
    );

    // Offsets for the 4 corners
    int2 offsets[4] = { int2(0,0), int2(1,0), int2(0,1), int2(1,1) };

    float4 occMask = float4(0, 0, 0, 0);

    [unroll]
    for (int i = 0; i < 4; i++)
    {
        int2 p = tl + offsets[i];

        // Bounds check
        if (p.x < 0 || p.y < 0 || p.x >= int(resolution.x) || p.y >= int(resolution.y))
            continue;

        float pz = prevViewZ[p];

        // Sky check
        if (pz > denoisingRange)
            continue;

        // Depth-based occlusion test
        float zDiff = abs(pz - expectedPrevZ);
        float zThresh = disocclusionThreshold * max(abs(expectedPrevZ), 1.0);
        if (zDiff > zThresh)
            continue;

        // Normal agreement test
        float4 pnr = prevNormalRough[p];
        float3 pn = DecodeNormalOct(pnr.xy);
        if (dot(pn, currentNormal) < 0.75)
            continue;

        // Material ID test: reject history from different surfaces.
        // Threshold accounts for RGBA16Unorm quantization (~1/65535).
        // Consecutive material IDs differ by 1/1023 ≈ 0.000978.
        if (abs(pnr.a - currentMaterialIdNorm) > 0.0005)
            continue;

        // This sample is valid
        occMask[i] = 1.0;
    }

    float4 customWeights = bw * occMask;
    float weightSum = dot(customWeights, float4(1, 1, 1, 1));
    isValid = weightSum > 1e-6;
    allSamplesValid = isValid && all(occMask > 0.5);

    if (isValid)
    {
        if (allSamplesValid)
        {
            bilinearWeights = customWeights / weightSum;
        }
        else
        {
            // A partial bilinear footprint means one or more taps landed on a
            // different surface. Renormalizing the remaining taps shifts the
            // effective sample position across the silhouette, which shows up
            // as a wrong-side halo on small camera nudges. Fall back to the
            // dominant valid tap instead; keep footprintQuality low so the
            // caller still treats this as a fragile reprojection.
            int dominantIndex = 0;
            float dominantWeight = customWeights.x;
            if (customWeights.y > dominantWeight)
            {
                dominantIndex = 1;
                dominantWeight = customWeights.y;
            }
            if (customWeights.z > dominantWeight)
            {
                dominantIndex = 2;
                dominantWeight = customWeights.z;
            }
            if (customWeights.w > dominantWeight)
            {
                dominantIndex = 3;
            }

            bilinearWeights[dominantIndex] = 1.0;
        }
    }

    // Footprint quality: fraction of valid bilinear area
    footprintQuality = sqrt(saturate(weightSum));

    return isValid ? SampleBilinearTexture(prevTexture, bilinearOrigin, resolution, bilinearWeights)
                   : float4(0, 0, 0, 0);
}

// Catmull-Rom (5-tap optimized) history sampling.
// Only call when all 4 bilinear samples are valid (no disocclusion in footprint).
float4 CatmullRomHistorySample(
    Texture2D<float4> prevTexture,
    SamplerState linearSampler,
    float2 prevUV,
    uint2 resolution)
{
    float2 texSize = float2(resolution);
    float2 samplePos = prevUV * texSize;
    float2 centerPos = floor(samplePos - 0.5) + 0.5;
    float2 f = saturate(samplePos - centerPos);

    // Catmull-Rom weights (sharpness = 0.5)
    static const float S = 0.5;
    float2 w0 = f * (f * (-S * f + 2.0 * S) - S);
    float2 w1 = f * (f * ((2.0 - S) * f - (3.0 - S))) + 1.0;
    float2 w2 = f * (f * (-(2.0 - S) * f + (3.0 - 2.0 * S)) + S);
    float2 w3 = f * (f * (S * f - S));

    float2 w12 = w1 + w2;
    float2 tc = w2 / w12;

    float2 invTexSize = 1.0 / texSize;

    // 5 bilinear fetches using hardware filtering
    float4 result = float4(0, 0, 0, 0);
    float totalWeight = 0.0;

    // Center 2x2
    float2 uv12 = (centerPos + tc) * invTexSize;
    float weight12 = w12.x * w12.y;
    result += prevTexture.SampleLevel(linearSampler, uv12, 0) * weight12;
    totalWeight += weight12;

    // Left column
    float2 uv0y = (centerPos + float2(-1.0, tc.y)) * invTexSize;
    float weight0y = w0.x * w12.y;
    result += prevTexture.SampleLevel(linearSampler, uv0y, 0) * weight0y;
    totalWeight += weight0y;

    // Right column
    float2 uv3y = (centerPos + float2(2.0, tc.y)) * invTexSize;
    float weight3y = w3.x * w12.y;
    result += prevTexture.SampleLevel(linearSampler, uv3y, 0) * weight3y;
    totalWeight += weight3y;

    // Top row
    float2 uvx0 = (centerPos + float2(tc.x, -1.0)) * invTexSize;
    float weightx0 = w12.x * w0.y;
    result += prevTexture.SampleLevel(linearSampler, uvx0, 0) * weightx0;
    totalWeight += weightx0;

    // Bottom row
    float2 uvx3 = (centerPos + float2(tc.x, 2.0)) * invTexSize;
    float weightx3 = w12.x * w3.y;
    result += prevTexture.SampleLevel(linearSampler, uvx3, 0) * weightx3;
    totalWeight += weightx3;

    if (totalWeight > 0.0)
        result /= totalWeight;

    return result;
}

#endif // REBLUR_REPROJECTION_H
