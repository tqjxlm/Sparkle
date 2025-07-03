#include "cubemap.h.slang"
#include "sampler.h.slang"
#include "screen.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    uint max_sample;
    uint time_seed;
    float max_brightness;
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

    InitRandomBase(pixel.x, pixel.y, time_seed);

    float2 pixel_size = 1.f / (float2(resolution) - 1);
    float2 uv = float2(pixel.x, pixel.y) * pixel_size;

    float3 w_n = GetDirectionFromCubeMapUV(uv, face_id);

    // enter tangent space of w_n
    float3 local_w_n = float3(0, 0, 1);
    float3 u, v, w;
    GetLocalAxisFromNormal(w_n, u, v, w);

    float3 local_w_i = SampleCosineWeightedHemisphere();

    // back to world space
    float3 w_i = normalize(TransformBasisToWorld(local_w_i, u, v, w));

    float3 env_sample = env_map.SampleLevel(env_map_sampler, w_i, 0).rgb;

    env_sample = ClampToLength(env_sample, max_brightness);
    float3 total_radiance = env_sample;

    // the sampling method we use allow us to skip cos_w_i and InvPi here
    // float cos_w_i = AbsCosTheta(local_w_i);
    // total_radiance *= cos_w_i * InvPi;

    out_cube_map[DTid] = prev_color + float4(total_radiance / max_sample, 1.f);
}