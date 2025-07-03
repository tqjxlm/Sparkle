#include "screen.h.slang"

[[vk::binding(0, 1)]] cbuffer ubo
{
    float exposure;
}

[[vk::binding(1, 1)]] Texture2D screenTexture;
[[vk::binding(2, 1)]] SamplerState screenTextureSampler;

float3 ACESFilm(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    x *= exposure;
    return clamp((x * (x * a + b)) / (x * (x * c + d) + e), 0.0f, 1.0f);
}

[shader("fragment")]
float4 main(VSInterpolant input) : SV_Target
{
    float3 color = screenTexture.Sample(screenTextureSampler, input.uv).xyz;

    color = ACESFilm(color);

    return float4(color, 1.0f);
}