#include "cubemap.h.slang"
#include "sampler.h.slang"
#include "screen.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    uint max_sample;
    uint time_seed;
    float roughness;
    float max_brightness;
    uint sample_batch;
}

[[vk::binding(1, 0)]] TextureCube env_map;
[[vk::binding(2, 0)]] SamplerState env_map_sampler;

[[vk::binding(3, 0)]] RWTexture2DArray<float4> out_cube_map;

[shader("compute")]
[numthreads(16, 16, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;
    uint face_id = DTid.z;

    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float4 prev_color = out_cube_map[DTid];

    float2 pixel_size = 1.f / (float2(resolution) - 1);
    float2 uv = float2(pixel.x, pixel.y) * pixel_size;

    float3 w_n = GetDirectionFromCubeMapUV(uv, face_id);

    // enter tangent space of w_n
    float3 local_w_n = float3(0, 0, 1);
    float3 u, v, w;
    GetLocalAxisFromNormal(w_n, u, v, w);

    // approximation: treat view direction as normal since we do not know at this stage
    float3 local_w_o = float3(0, 0, 1);

    float3 batch_radiance = 0.f;
    for (uint sample_index = 0; sample_index < sample_batch; sample_index++)
    {
        InitRandomBase(pixel.x, pixel.y, time_seed + sample_index);

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

            float pdf = DistributionGGX::Pdf(roughness, local_w_o, local_w_m);
            float sa_texel = 4.0 * Pi / (6.0 * resolution.x * resolution.y);
            float sa_sample = 1.0 / (max_sample * pdf + Eps);
            float mip_level = roughness < Eps ? 0.0 : 0.5 * log2(sa_sample / sa_texel);

            float3 env_sample = env_map.SampleLevel(env_map_sampler, w_i, mip_level).rgb;

            env_sample = ClampToLength(env_sample, max_brightness);

            total_radiance = env_sample;
        }

        batch_radiance += total_radiance;
    }

    out_cube_map[DTid] = prev_color + float4(batch_radiance / max_sample, float(sample_batch));
}
