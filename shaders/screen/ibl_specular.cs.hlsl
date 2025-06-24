#include "cubemap.h.hlsl"
#include "sampler.h.hlsl"
#include "screen.h.hlsl"

[[vk::binding(0, 0)]] cbuffer ubo : register(b0)
{
    uint2 resolution;
    uint max_sample;
    uint time_seed;
    float roughness;
    float max_brightness;
}

[[vk::binding(1, 0)]] TextureCube env_map : register(t1);
[[vk::binding(2, 0)]] SamplerState env_map_sampler : register(s2);

[[vk::binding(3, 0)]] RWTexture2DArray<float4> out_cube_map : register(u0);

[numthreads(16, 16, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    uint face_id = DTid.z;

    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float4 prev_color = out_cube_map[DTid];

    InitRandomBase(pixel.x, pixel.y, time_seed);

    float2 pixel_size = 1.f / (float2(resolution) - 1);
    float2 uv = float2(pixel.x, pixel.y) * pixel_size;

    float3 w_n = GetDirectionFromCubeMapUV(uv, face_id);

    // enter tangent space of w_n
    float3 local_w_n = float3(0, 0, 1);
    float3 u, v, w;
    GetLocalAxisFromNormal(w_n, u, v, w);

    // approximation: treat view direction as normal since we do not know at this stage
    float3 local_w_o = float3(0, 0, 1);

    float3 local_w_m = SampleMicroFacetNormal(local_w_o, roughness);
    float3 local_w_i = reflect(-local_w_o, local_w_m);

    float3 total_radiance;
    if (CosTheta(local_w_i) < Eps)
    {
        total_radiance = (float3)0;
    }
    else
    {
        // back to world space
        float3 w_i = normalize(TransformBasisToWorld(local_w_i, u, v, w));

        float pdf = DistributionVn::Pdf(roughness, local_w_o, local_w_m);
        float sa_texel = 4.0 * Pi / (6.0 * resolution.x * resolution.y);
        float sa_sample = 1.0 / (max_sample * pdf + Eps);
        float mip_level = roughness < Eps ? 0.0 : 0.5 * log2(sa_sample / sa_texel);

        float3 env_sample = env_map.SampleLevel(env_map_sampler, w_i, mip_level).rgb;

        env_sample = ClampToLength(env_sample, max_brightness);

        total_radiance = env_sample;
    }

    out_cube_map[DTid] = prev_color + float4(total_radiance / max_sample, 1.f);
}
