#include "screen.h.slang"

[[vk::binding(0, 1)]] cbuffer ubo
{
    float2 pixel_size;
}

[[vk::binding(1, 1)]] Texture2D inputTexture;
[[vk::binding(2, 1)]] SamplerState inputTextureSampler;

[shader("fragment")]
float4 main(VSInterpolant input) : SV_Target
{
    const int KernelSize = 3;
    float3 color = float3(0, 0, 0);
    float2 top_left = input.uv - pixel_size * KernelSize * 0.5;

    for (int i = 0; i < KernelSize; i++)
    {
        for (int j = 0; j < KernelSize; j++)
        {
            float2 uv = top_left + pixel_size * float2(i, j);
            float3 sub_sample = inputTexture.Sample(inputTextureSampler, uv).xyz;
            color += sub_sample;
        }
    }

    color = color / (KernelSize * KernelSize);
    return float4(color, 1.0f);
}