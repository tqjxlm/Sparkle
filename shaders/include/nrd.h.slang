#ifndef NRD_COMMON_H_
#define NRD_COMMON_H_

// Exact replicas of the NRD v4.17.3 front/back-end encoding math (thirdparty/NRD/Shaders/NRD.hlsli).
// Must stay in sync with the pinned NRD submodule: the packed values are decoded by NRD's own shaders.
// Normal encoding is RGBA16_SNORM (NRD_NORMAL_ENCODING=4 in thirdparty/CMakeLists.txt), i.e. raw
// float4(N, linear_roughness) — nothing to encode here.

float3 NrdLinearToYCoCg(float3 color)
{
    float y = dot(color, float3(0.25, 0.5, 0.25));
    float co = dot(color, float3(0.5, 0.0, -0.5));
    float cg = dot(color, float3(-0.25, 0.5, -0.25));
    return float3(y, co, cg);
}

float3 NrdYCoCgToLinear(float3 color)
{
    float t = color.x - color.z;
    float3 r;
    r.y = color.x + color.z;
    r.x = t + color.y;
    r.z = t - color.y;
    return max(r, 0.0);
}

float NrdSpecMagicCurve(float roughness)
{
    float f = 1.0 - exp2(-200.0 * roughness * roughness);
    f *= pow(saturate(roughness), 0.5);
    return f;
}

// hit_dist_params = ReblurHitDistanceParameters {A, B, C}; roughness = 1 for the diffuse signal.
float NrdHitDistanceNormalization(float view_z, float3 hit_dist_params, float roughness)
{
    float smc = NrdSpecMagicCurve(roughness);
    return (hit_dist_params.x + abs(view_z) * hit_dist_params.y) * lerp(hit_dist_params.z, 1.0, smc);
}

// REBLUR_FrontEnd_PackRadianceAndNormHitDist (sanitize inlined). hit_dist_valid=false writes hitT EXACTLY 0
// = NRD's "lobe skipped this frame" protocol (probabilistic lobe selection; requires
// HitDistanceReconstructionMode != OFF). The 1e-6 floor on valid samples keeps "lobe present".
float4 NrdPackRadianceAndNormHitDist(float3 radiance, float norm_hit_dist, bool hit_dist_valid)
{
    radiance = clamp(radiance, 0.0, 65504.0);
    float hit_dist = hit_dist_valid ? max(saturate(norm_hit_dist), 1e-6) : 0.0;
    return float4(NrdLinearToYCoCg(radiance), hit_dist);
}

#endif
