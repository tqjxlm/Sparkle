#include "screen.h.slang"

#include "sampler.h.slang"
#include "tangent_space.h.slang"

[[vk::binding(0, 0)]] cbuffer ubo
{
    uint2 resolution;
    uint time_seed;
    uint max_sample;
}

[[vk::binding(1, 0)]] RWTexture2D<float4> out_texture;

[shader("compute")]
[numthreads(16, 16, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
    uint2 pixel = DTid.xy;

    if (pixel.x >= resolution.x || pixel.y >= resolution.y)
    {
        return;
    }

    float4 prev_color = out_texture[pixel];

    InitRandomBase(pixel.x, pixel.y, time_seed);

    float2 pixel_size = 1.f / (float2(resolution) - 1);
    float2 uv = float2(pixel.x, pixel.y) * pixel_size;

    // enter tangent space of w_n
    // w_n: surface normal
    // w_m: micro-surface normal
    // w_o: outward (view) direction
    // w_i: inward (light) direction
    float3 local_w_n = float3(0, 0, 1.0);

    float cos_o = uv.x;
    float roughness = uv.y;

    // recover a pseudo w_o from cos_o.
    // note that the x component is here to make a unit direction, it does not affect the convolution.
    float3 local_w_o = float3(sqrt(1.0 - cos_o * cos_o), 0, cos_o);

    float3 local_w_m = SampleMicroFacetNormal(local_w_o, roughness);
    float3 local_w_i = reflect(-local_w_o, local_w_m);
    float i_dot_m = SaturateDot(local_w_m, local_w_i);

    float cos_i = CosTheta(local_w_i);

    float2 output;
    if (cos_i < Eps)
    {
        output = float2(0, 0);
    }
    else
    {
        float cos_m = AbsCosTheta(local_w_m);

        float geometry = SmithGGX(cos_o, cos_i, roughness);
        float normalizer = GeometrySchlickGGX(i_dot_m, roughness) + Eps;
        float brdf_term = geometry / normalizer;
        float fresnel_term = pow(1.0 - i_dot_m, 5.0);
        output = float2((1.0 - fresnel_term) * brdf_term, fresnel_term * brdf_term);
    }

    out_texture[pixel] = prev_color + float4(output / max_sample, 0, 1.f);
}