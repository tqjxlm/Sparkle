#include "screen.h.slang"

[[vk::binding(0, 1)]] Texture2D screenTexture;
[[vk::binding(1, 1)]] SamplerState screenTextureSampler;

[shader("fragment")]
float4 main(VSInterpolant input) : SV_TARGET
{
    float3 color = screenTexture.Sample(screenTextureSampler, input.uv).xyz;

    return float4(color, 1.0f);
}
