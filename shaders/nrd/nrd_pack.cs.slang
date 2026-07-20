// NRD input packing: converts the path tracer's G-buffer into the five textures ReBLUR consumes
// (IN_MV, IN_NORMAL_ROUGHNESS, IN_VIEWZ, IN_DIFF/SPEC_RADIANCE_HITDIST). Demodulates the diffuse by
// albedo and the specular by the pre-integrated BRDF so the denoiser sees a texture-free signal; the
// nrd_resolve pass re-modulates.

#include "../include/nrd.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    // ReblurHitDistanceParameters {A, B, C}; must match the C++ ReblurSettings fed to NRD
    float hitDistA;
    float hitDistB;
    float hitDistC;
    float demodEps;
    float skyViewZ; // pushed beyond CommonSettings::denoisingRange so NRD skips sky pixels
}

[[vk::binding(1, 0)]] Texture2D<float4> gRadiance;         // rgb: full radiance, w: diffuse hit dist
[[vk::binding(2, 0)]] Texture2D<float4> gRadianceSpecular; // rgb: specular radiance, w: specular hit dist
[[vk::binding(3, 0)]] Texture2D<float4> gNormalDepth;      // xyz: world normal, w: view-space linear z
[[vk::binding(4, 0)]] Texture2D<float4> gAlbedoObj;        // rgb: base color
// gMotion.z encodes the hit state: 0 = miss, 1 = hit, 2 = primary-emissive hit (bypasses NRD, see below)
[[vk::binding(5, 0)]] Texture2D<float4> gMotion;     // xy: uv motion (prev - cur), z: hit state, w: metallic
[[vk::binding(6, 0)]] Texture2D<float4> gSpecAlbedo; // rgb: specular demod factor, w: roughness

[[vk::binding(7, 0)]] RWTexture2D<float4> outMv;
[[vk::binding(8, 0)]] RWTexture2D<float4> outNormalRoughness;
[[vk::binding(9, 0)]] RWTexture2D<float4> outViewZ;
[[vk::binding(10, 0)]] RWTexture2D<float4> outDiff;
[[vk::binding(11, 0)]] RWTexture2D<float4> outSpec;

float Luma(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

[shader("compute")][numthreads(16, 16, 1)] void shader_main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float4 radiance_hd = gRadiance.Load(int3(pixel, 0));
    float4 spec_radiance_hd = gRadianceSpecular.Load(int3(pixel, 0));
    float4 normal_depth = gNormalDepth.Load(int3(pixel, 0));
    float4 motion_hit = gMotion.Load(int3(pixel, 0));
    float4 spec_albedo_rough = gSpecAlbedo.Load(int3(pixel, 0));
    bool is_hit = motion_hit.z > 0.5f;
    // primary-hit emissive pixels bypass NRD: the sky sentinel keeps them out of ReBLUR and out of
    // neighbors' kernels (their demodulated value is emissive/albedo, huge and texture-shaped);
    // nrd_resolve routes them to the accumulator, which equals the emission exactly.
    bool is_emissive = motion_hit.z > 1.5f;

    // the engine's view space is right-handed (projection w = -z_view): visible geometry has NEGATIVE
    // view-space z, and NRD reconstructs world positions from viewZ's sign — feeding +distance breaks its
    // reprojection (history rejected everywhere; found via the NRD validation overlay's FRAMES panels).
    float view_z = (is_hit && !is_emissive) ? -normal_depth.w : -skyViewZ;
    float roughness = spec_albedo_rough.w;

    // sky pixels carry normal (0,0,0); normalize(0) is NaN, and ReBLUR's temporal accumulation averages
    // neighbor normals without range-gating, so a NaN here corrupts adjacent geometry pixels.
    float3 normal = is_hit ? normalize(normal_depth.xyz) : float3(0.f, 0.f, 1.f);

    // metal vs dielectric buckets: demodulated diffuse (and spec) is discontinuous across that
    // boundary (metal diffuse ~0 vs dielectric ~illumination), and ReBLUR only stops blurring
    // across it when material IDs differ. IDs 0/3 hit the exact 2-bit-unorm endpoints (0.0/1.0),
    // so NRD's two internal decode paths agree bitwise on any driver (Adreno's mid-range unorm
    // decodes are 1 ulp apart, which the equality test reads as a different material).
    float material_id = motion_hit.w > 0.5f ? 3.f : 0.f;

    outMv[pixel] = float4(motion_hit.xy, 0.f, 0.f);
    outNormalRoughness[pixel] = NrdPackNormalRoughness(normal, roughness, material_id);
    outViewZ[pixel] = float4(view_z, 0.f, 0.f, 0.f);

    // sky keeps demodulation factor 1 so pack/resolve round-trips exactly.
    float3 albedo = gAlbedoObj.Load(int3(pixel, 0)).rgb;
    float3 demod_albedo = max(is_hit ? albedo : float3(1, 1, 1), float3(demodEps, demodEps, demodEps));
    float3 diff = is_emissive ? float3(0, 0, 0) : max(radiance_hd.rgb - spec_radiance_hd.rgb, 0.f) / demod_albedo;

    // Firefly suppression: demodulation re-amplifies an OutputLimit-clamped outlier on a low-albedo
    // silhouette rim; clamp the demodulated illumination to its local 3x3 luminance (mean + 4*std) so
    // the spike never enters ReBLUR's history.
    if (!is_emissive)
    {
        float l_sum = 0.f;
        float l_sum2 = 0.f;
        float l_n = 0.f;
        [unroll] for (int yy = -1; yy <= 1; yy++)
        {
            [unroll] for (int xx = -1; xx <= 1; xx++)
            {
                int2 q = clamp(int2(pixel) + int2(xx, yy), int2(0, 0), int2(resolution) - 1);
                float hit_z_q = gMotion.Load(int3(q, 0)).z;
                if (hit_z_q > 1.5f)
                {
                    continue;
                }
                bool hit_q = hit_z_q > 0.5f;
                float3 alb_q = max(hit_q ? gAlbedoObj.Load(int3(q, 0)).rgb : float3(1, 1, 1),
                                   float3(demodEps, demodEps, demodEps));
                float3 diff_q =
                    max(gRadiance.Load(int3(q, 0)).rgb - gRadianceSpecular.Load(int3(q, 0)).rgb, 0.f) / alb_q;
                float lq = Luma(diff_q);
                l_sum += lq;
                l_sum2 += lq * lq;
                l_n += 1.f;
            }
        }
        float l_mean = l_sum / l_n;
        float l_std = sqrt(max(0.f, l_sum2 / l_n - l_mean * l_mean));
        float l_max = l_mean + 4.f * l_std;
        float l_cur = Luma(diff);
        if (l_cur > l_max && l_cur > 1e-6f)
        {
            diff *= l_max / l_cur;
        }
    }

    float3 spec_demod = max(spec_albedo_rough.rgb, float3(demodEps, demodEps, demodEps));
    float3 spec = is_emissive ? float3(0, 0, 0) : spec_radiance_hd.rgb / spec_demod;

    float3 hit_dist_params = float3(hitDistA, hitDistB, hitDistC);
    float diff_norm_hd = saturate(radiance_hd.w / NrdHitDistanceNormalization(view_z, hit_dist_params, 1.f));
    float spec_norm_hd = saturate(spec_radiance_hd.w / NrdHitDistanceNormalization(view_z, hit_dist_params, roughness));

    // raw hit distance 0 = that lobe was not sampled this frame (probabilistic primary-lobe selection).
    // NRD requires EXACTLY 0 there (HitDistanceReconstructionMode fills from neighbors); any positive
    // floor reads as a valid hit AT the surface and collapses blur radius + virtual reprojection.
    outDiff[pixel] = NrdPackRadianceAndNormHitDist(diff, diff_norm_hd, radiance_hd.w > 0.f);
    outSpec[pixel] = NrdPackRadianceAndNormHitDist(spec, spec_norm_hd, spec_radiance_hd.w > 0.f);
}
